#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_audio_dec.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "player.h"
#include "playlist.h"
#include "wrapper.h"
#include "codec_mp3.h"
#include "codec_aac.h"
#include "audio_i2s.h"
#include "eq.h"
#include "decoder/esp_audio_dec_reg.h"
#include "config.h"
#include "icy.h"

static const char *TAG = "PLAYER";
#define HTTP_RECEIVE_BUFFER_SIZE (8192)
#define PLAYER_TASK_STACK_SIZE (4096)
static const char* PCRADIO_HOST_PRIMARY = "stream.pcradio.ru";
#define PCRADIO_NUM_ALT_SERVERS 6
#define PCRADIO_RETRY_DELAY_MS 5000
#define PLAYER_STREAM_READ_TIMEOUT_MS 1000
#define PLAYER_STOP_TIMEOUT_MS 25000
#define ICY_METADATA_HEADER "Icy-MetaData: 1"
#define ICY_METAINT_RESPONSE_HEADER "icy-metaint"
#define ICY_MAX_META_SIZE 4080
int sample_rate = 44100;
int bits_per_sample = 16;
int channels = 2;


typedef struct {
    TaskHandle_t task_handle;
    SemaphoreHandle_t stop_sem;
    SemaphoreHandle_t stopped_sem;
    char *original_playlist_url;
    char *current_playback_url;
    char *current_opt;
    bool is_pcradio_stream;
    char *pcradio_url_path;
    int pcradio_port;
    int pcradio_current_server_idx;
    esp_audio_dec_handle_t decoder_handle;
    esp_http_client_handle_t http_client_handle;
    bool is_playing;
    wrapper_audio_type_t detected_audio_type;
   icy_state_t icy;
    uint8_t *http_recv_buffer;
} player_status_t;

static player_status_t s_player_status = {0};

static bool s_codec_buffers_initialized = false;

static void player_task(void *pvParameters);

static esp_err_t player_connect_and_setup_decoder(wrapper_audio_type_t *detected_type_out);

static bool player_take_stop_signal(void) {
    if (s_player_status.stop_sem != NULL &&
        xSemaphoreTake(s_player_status.stop_sem, 0) == pdTRUE) {
        s_player_status.is_playing = false;
        return true;
    }
    return false;
}

static bool player_wait_or_stop(uint32_t wait_ms) {
    if (s_player_status.stop_sem != NULL &&
        xSemaphoreTake(s_player_status.stop_sem, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
        s_player_status.is_playing = false;
        return true;
    }
    return false;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
           icy_handle_header(&s_player_status.icy, evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}


static int parse_url_alloc(const char *url, char **host_out, int *port_out, char **path_out) {
    *host_out = NULL;
    *path_out = NULL;
    if (url == NULL || strncmp(url, "http://", 7) != 0) return -1;

    const char *host_start = url + 7;
    const char *path_start_ptr = strchr(host_start, '/');
    const char *port_colon_ptr = strchr(host_start, ':');

    if (!path_start_ptr) return -1;

    const char *host_end_ptr;
    if (port_colon_ptr && port_colon_ptr < path_start_ptr) {
        host_end_ptr = port_colon_ptr;
        *port_out = atoi(port_colon_ptr + 1);
        if (*port_out == 0) return -1;
    } else {
        host_end_ptr = path_start_ptr;
        *port_out = 80;
    }

    size_t host_len = host_end_ptr - host_start;
    *host_out = malloc(host_len + 1);
    if (!*host_out) return -1;
    strncpy(*host_out, host_start, host_len);
    (*host_out)[host_len] = '\0';

    size_t path_len = strlen(path_start_ptr);
    *path_out = malloc(path_len + 1);
    if (!*path_out) {
        free(*host_out);
        *host_out = NULL;
        return -1;
    }
    strcpy(*path_out, path_start_ptr);
    (*path_out)[path_len] = '\0';

    return 0;
}


static esp_err_t player_connect_and_setup_decoder(wrapper_audio_type_t *detected_type_out) {
    ESP_LOGI(TAG, "Attempting to connect and setup decoder...");
    *detected_type_out = WRAPPER_TYPE_NONE;

    char temp_url_buffer[256];
    const char *url_to_use = NULL;

    if (s_player_status.is_pcradio_stream) {
        snprintf(temp_url_buffer, sizeof(temp_url_buffer), "http://stream%02d.pcradio.ru%s",
                 s_player_status.pcradio_current_server_idx + 1,
                 s_player_status.pcradio_url_path);
        url_to_use = temp_url_buffer;
        ESP_LOGI(TAG, "PCRadio: Attempting connection to: %s (server index %d)", url_to_use, s_player_status.pcradio_current_server_idx);
    } else {
        url_to_use = s_player_status.original_playlist_url;
        ESP_LOGI(TAG, "Non-PCRadio: Attempting connection to: %s", url_to_use);
    }

    if (!url_to_use) {
        ESP_LOGE(TAG, "URL to use is NULL. Cannot connect.");
        return ESP_FAIL;
    }

    free(s_player_status.current_playback_url);
    s_player_status.current_playback_url = strdup(url_to_use);
    if (!s_player_status.current_playback_url) {
        ESP_LOGE(TAG, "Failed to duplicate current_playback_url string.");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_config = {
        .url = s_player_status.current_playback_url,
        .event_handler = _http_event_handler,
        .user_agent = "PCRadio",
        .timeout_ms = 20000,
        .buffer_size = HTTP_RECEIVE_BUFFER_SIZE,
        .buffer_size_tx = 512,
        .disable_auto_redirect = false,
        .is_async = false,
        .use_global_ca_store = true,
    };

    s_player_status.http_client_handle = esp_http_client_init(&http_config);
    if (s_player_status.http_client_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client.");
        return ESP_FAIL;
    }

    esp_http_client_set_header(s_player_status.http_client_handle, "Icy-MetaData", "1");

    ESP_LOGI(TAG, "Opening HTTP connection to %s...", s_player_status.current_playback_url);
    esp_err_t err = esp_http_client_open(s_player_status.http_client_handle, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s. HTTP error: %s (%d)",
                 esp_err_to_name(err),
                 esp_err_to_name(esp_http_client_get_errno(s_player_status.http_client_handle)),
                 esp_http_client_get_errno(s_player_status.http_client_handle));
        esp_http_client_cleanup(s_player_status.http_client_handle);
        s_player_status.http_client_handle = NULL;
        return err;
    }
    if (player_take_stop_signal()) {
        ESP_LOGI(TAG, "Setup aborted: playback stopped after HTTP open");
        esp_http_client_close(s_player_status.http_client_handle);
        esp_http_client_cleanup(s_player_status.http_client_handle);
        s_player_status.http_client_handle = NULL;
        return ESP_FAIL;
    }

    int64_t content_length = esp_http_client_fetch_headers(s_player_status.http_client_handle);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch HTTP headers: %s. HTTP error: %s (%d)",
                 esp_err_to_name((esp_err_t)content_length),
                 esp_err_to_name(esp_http_client_get_errno(s_player_status.http_client_handle)),
                 esp_http_client_get_errno(s_player_status.http_client_handle));
        if (s_player_status.http_client_handle) {
            esp_http_client_close(s_player_status.http_client_handle);
            esp_http_client_cleanup(s_player_status.http_client_handle);
            s_player_status.http_client_handle = NULL;
        }
        return ESP_FAIL;
    }

    if (player_take_stop_signal()) {
        ESP_LOGI(TAG, "Setup aborted: playback stopped after fetching HTTP headers");
        esp_http_client_close(s_player_status.http_client_handle);
        esp_http_client_cleanup(s_player_status.http_client_handle);
        s_player_status.http_client_handle = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    int http_status = esp_http_client_get_status_code(s_player_status.http_client_handle);
    ESP_LOGI(TAG, "HTTP connection opened and headers fetched. Status = %d, content_length = %lld",
             http_status,
             esp_http_client_get_content_length(s_player_status.http_client_handle));

    if (http_status != 200) {
        ESP_LOGE(TAG, "HTTP connection failed with status code: %d", http_status);
        if (s_player_status.http_client_handle) {
            esp_http_client_close(s_player_status.http_client_handle);
            esp_http_client_cleanup(s_player_status.http_client_handle);
            s_player_status.http_client_handle = NULL;
        }
        return (http_status == 401 || http_status == 403) ? ESP_FAIL : ESP_ERR_HTTP_BASE;
    }

    char *content_type_value = NULL;
    esp_err_t get_header_err = esp_http_client_get_header(s_player_status.http_client_handle, "Content-Type", &content_type_value);

    if (get_header_err == ESP_OK && content_type_value != NULL) {
        ESP_LOGI(TAG, "Received Content-Type: %s", content_type_value);
        if (strstr(content_type_value, "audio/aac") || strstr(content_type_value, "audio/aacp")) {
            *detected_type_out = WRAPPER_TYPE_AAC;
            ESP_LOGI(TAG, "Audio type detected from Content-Type: AAC");
        } else if (strstr(content_type_value, "audio/mpeg") || strstr(content_type_value, "audio/mp3")) {
            *detected_type_out = WRAPPER_TYPE_MP3;
            ESP_LOGI(TAG, "Audio type detected from Content-Type: MP3");
        } else {
            ESP_LOGI(TAG, "Content-Type is '%s', not recognized as AAC or MP3 directly. Will try to detect from data.", content_type_value);
        }
        free(content_type_value);
        content_type_value = NULL;
    } else {
        if (get_header_err != ESP_OK) {
            ESP_LOGI(TAG, "Failed to get Content-Type header: %s. Will detect audio type from data.", esp_err_to_name(get_header_err));
        } else if (content_type_value == NULL && get_header_err == ESP_OK) {
             ESP_LOGI(TAG, "Content-Type header not found or is empty (get_header_err == ESP_OK, value is NULL). Will detect audio type from data.");
        } else {
             ESP_LOGW(TAG, "Unexpected state after esp_http_client_get_header: err=%s, value_ptr=%p. Will detect from data.", esp_err_to_name(get_header_err), content_type_value);
        }
         if(content_type_value) {
            free(content_type_value);
            content_type_value = NULL;
        }
    }

    esp_err_t timeout_err = esp_http_client_set_timeout_ms(
        s_player_status.http_client_handle, PLAYER_STREAM_READ_TIMEOUT_MS);
    if (timeout_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set HTTP stream read timeout: %s", esp_err_to_name(timeout_err));
        esp_http_client_close(s_player_status.http_client_handle);
        esp_http_client_cleanup(s_player_status.http_client_handle);
        s_player_status.http_client_handle = NULL;
        return timeout_err;
    }

    ESP_LOGI(TAG, "HTTP client setup successful for URL: %s", s_player_status.current_playback_url);
    ESP_LOGI(TAG, "Free internal RAM: %u bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}


esp_err_t player_init(void) {
    ESP_LOGI(TAG, "Initializing player...");
    bool stop_sem_created = false;
    if (s_player_status.stop_sem == NULL) {
        s_player_status.stop_sem = xSemaphoreCreateBinary();
        if (s_player_status.stop_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create stop semaphore");
            return ESP_FAIL;
        }
        stop_sem_created = true;
    }
    if (s_player_status.stopped_sem == NULL) {
        s_player_status.stopped_sem = xSemaphoreCreateBinary();
        if (s_player_status.stopped_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create player completion semaphore");
            if (stop_sem_created) {
                vSemaphoreDelete(s_player_status.stop_sem);
                s_player_status.stop_sem = NULL;
            }
            return ESP_FAIL;
        }
    }

    if (audio_i2s_init(sample_rate, bits_per_sample, channels) == ESP_OK) {

        audio_i2s_set_expander_gate(true, 1500.0f, 1.5f, 600.0f);
        audio_i2s_set_gate_advanced(0.05f, 2.0f, 150.0f, 50.0f);

        ESP_LOGI("main", "Configuring ALC.");
        alc_config_t *alc_cfg = (alc_config_t *)malloc(sizeof(alc_config_t));
        if (alc_cfg != NULL) {
            alc_cfg->sample_rate = sample_rate;
            alc_cfg->channels = channels;
            alc_cfg->bits_per_sample = bits_per_sample;
            esp_err_t alc_set_ret = audio_i2s_set_alc_config(alc_cfg);
            if (alc_set_ret != ESP_OK) {
                free(alc_cfg);
            }
        }

        ESP_LOGI(TAG, "Applying EQ preset %d from config", g_app_config->player.eq_preset);
        esp_ae_eq_cfg_t *eq_cfg = eq_config_para(sample_rate, channels, bits_per_sample, g_app_config->player.eq_preset);
        if (eq_cfg != NULL) {
            esp_err_t eq_set_ret = audio_i2s_set_eq_config(eq_cfg);
            if (eq_set_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to apply EQ preset %d: %s", g_app_config->player.eq_preset, esp_err_to_name(eq_set_ret));
            } else {
                ESP_LOGI(TAG, "EQ preset %d applied successfully", g_app_config->player.eq_preset);
            }
        } else {
            ESP_LOGE(TAG, "Failed to create EQ config for preset %d", g_app_config->player.eq_preset);
        }
    }

    ESP_LOGI(TAG, "Checking I2S output state");
    ESP_LOGI(TAG, "Player initialized.");
    return ESP_OK;
}

static void cleanup_current_stream_resources() {
    if (s_player_status.decoder_handle) {
        esp_audio_dec_close(s_player_status.decoder_handle);
        s_player_status.decoder_handle = NULL;
    }

    if (s_player_status.http_client_handle) {
        esp_http_client_close(s_player_status.http_client_handle);
        esp_http_client_cleanup(s_player_status.http_client_handle);
        s_player_status.http_client_handle = NULL;
    }
    free(s_player_status.current_playback_url); s_player_status.current_playback_url = NULL;
    free(s_player_status.original_playlist_url); s_player_status.original_playlist_url = NULL;
    free(s_player_status.current_opt); s_player_status.current_opt = NULL;
    free(s_player_status.pcradio_url_path); s_player_status.pcradio_url_path = NULL;
    if (s_player_status.http_recv_buffer) {
        heap_caps_free(s_player_status.http_recv_buffer);
        s_player_status.http_recv_buffer = NULL;
    }
   icy_state_cleanup(&s_player_status.icy);

    s_player_status.detected_audio_type = WRAPPER_TYPE_NONE;
}

esp_err_t player_play_channel(int channel_number) {
    if (channel_number < 0) {
        ESP_LOGI(TAG, "No channel selected, stopping playback.");
        return player_stop();
    }
    ESP_LOGI(TAG, "Muting audio for channel switch.");
    audio_i2s_set_mute(true);

    if (!s_codec_buffers_initialized) {
        codec_init_mp3_decoder();
        codec_init_aac_decoder();
        codec_mp3_init_buffers();
        codec_aac_init_buffers(CODEC_AAC_BUFFER_SIZE);
        s_codec_buffers_initialized = true;
    }
    if (s_player_status.task_handle != NULL) {
        ESP_LOGW(TAG, "Player is already playing. Stopping current stream first.");
        esp_err_t stop_err = player_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot switch channel because player did not stop: %s", esp_err_to_name(stop_err));
            return stop_err;
        }
    }

    playlist_channel_data_t ch_data;
    esp_err_t ret = playlist_get_channel_data(channel_number, &ch_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get channel %d data: %s", channel_number, esp_err_to_name(ret));
        return ret;
    }

    if (!ch_data.url) {
        ESP_LOGE(TAG, "Channel %d has no URL.", channel_number);
        playlist_free_channel_data(&ch_data);
        return ESP_ERR_INVALID_ARG;
    }

    cleanup_current_stream_resources();
   icy_state_init(&s_player_status.icy);

    s_player_status.original_playlist_url = strdup(ch_data.url);
    if (ch_data.opt) {
        s_player_status.current_opt = strdup(ch_data.opt);
    } else {
        s_player_status.current_opt = NULL;
    }

    if (!s_player_status.original_playlist_url) {
        ESP_LOGE(TAG, "Failed to duplicate original playlist URL string.");
        playlist_free_channel_data(&ch_data);
        cleanup_current_stream_resources();
        return ESP_ERR_NO_MEM;
    }

    char *host = NULL;
    char *path = NULL;
    int port = 0;

    if (parse_url_alloc(s_player_status.original_playlist_url, &host, &port, &path) == 0) {
        if (strcmp(host, PCRADIO_HOST_PRIMARY) == 0) {
            s_player_status.is_pcradio_stream = true;
            s_player_status.pcradio_url_path = path;
            s_player_status.pcradio_port = port;
            s_player_status.pcradio_current_server_idx = 0;
            ESP_LOGI(TAG, "PCRadio stream detected. Path: %s, Port: %d", s_player_status.pcradio_url_path, s_player_status.pcradio_port);
            path = NULL;
        } else {
            s_player_status.is_pcradio_stream = false;
        }
        free(host);
        free(path);
    } else {
        ESP_LOGW(TAG, "Failed to parse original URL: %s. Assuming non-pcradio stream.", s_player_status.original_playlist_url);
        s_player_status.is_pcradio_stream = false;
    }

    playlist_free_channel_data(&ch_data);

    xSemaphoreTake(s_player_status.stop_sem, 0);
    xSemaphoreTake(s_player_status.stopped_sem, 0);

    s_player_status.is_playing = true;

    BaseType_t task_created = xTaskCreatePinnedToCore(
      player_task,
      "player_task",
      PLAYER_TASK_STACK_SIZE,
      NULL,
      20,
      &s_player_status.task_handle,
      1);

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create player task");
        s_player_status.is_playing = false;
        cleanup_current_stream_resources();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Player task created for original URL: %s", s_player_status.original_playlist_url);
    return ESP_OK;
}

esp_err_t player_stop(void) {
    ESP_LOGI(TAG, "Stopping player...");
    if (s_player_status.task_handle == NULL) {
        ESP_LOGI(TAG, "Player task is not running.");
        return ESP_OK;
    }

    if (s_player_status.stop_sem == NULL || s_player_status.stopped_sem == NULL) {
        ESP_LOGE(TAG, "Player synchronization primitives are not initialized.");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreGive(s_player_status.stop_sem);
    if (xSemaphoreTake(s_player_status.stopped_sem,
                       pdMS_TO_TICKS(PLAYER_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for player task to stop after %d ms.",
                 PLAYER_STOP_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Player task stopped cooperatively.");
    return ESP_OK;
}


esp_err_t player_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing player...");
    if (s_player_status.task_handle != NULL) {
        esp_err_t stop_err = player_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG, "Player deinit aborted because task did not stop: %s", esp_err_to_name(stop_err));
            return stop_err;
        }
    }
    cleanup_current_stream_resources();

    if (s_player_status.stop_sem != NULL) {
        vSemaphoreDelete(s_player_status.stop_sem);
        s_player_status.stop_sem = NULL;
    }
    if (s_player_status.stopped_sem != NULL) {
        vSemaphoreDelete(s_player_status.stopped_sem);
        s_player_status.stopped_sem = NULL;
    }

    audio_i2s_deinit();

    ESP_LOGI(TAG, "Player deinitialized.");
    return ESP_OK;
}

const char* player_get_icy_name(void) {
    return icy_get_name(&s_player_status.icy);
}

static void player_task(void *pvParameters) {
    ESP_LOGI(TAG, "Player task started.");

    s_player_status.http_recv_buffer = heap_caps_malloc(HTTP_RECEIVE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_player_status.http_recv_buffer) {
        ESP_LOGE(TAG, "Failed to allocate HTTP receive buffer.");
        cleanup_current_stream_resources();
        s_player_status.is_playing = false;
        s_player_status.task_handle = NULL;
        xSemaphoreGive(s_player_status.stopped_sem);
        ESP_LOGI(TAG, "Player task deleting self due to allocation failure.");
        vTaskDelete(NULL);
        return;
    }
    icy_allocate_buffer(&s_player_status.icy);

    while (s_player_status.is_playing) {
        if (player_take_stop_signal()) {
            ESP_LOGI(TAG, "Stop signal received at the beginning of a stream attempt.");
            break;
        }

        esp_err_t connect_err = ESP_FAIL;
        esp_err_t process_data_status = ESP_OK;
        bool current_attempt_failed = false;
        bool first_data_chunk = true;
        bool info_logged = false;
        bool is_quality_switch_reconnect = false;

        s_player_status.detected_audio_type = WRAPPER_TYPE_NONE;

        connect_err = player_connect_and_setup_decoder(&s_player_status.detected_audio_type);

        if (player_take_stop_signal()) {
            ESP_LOGI(TAG, "Stop signal received after connection attempt.");
        }

        if (connect_err != ESP_OK) {
            ESP_LOGE(TAG, "Connection and setup failed: %s", esp_err_to_name(connect_err));
            current_attempt_failed = true;
        } else {
            ESP_LOGI(TAG, "Connection successful. Initial detected audio type (from URL/header): %d", (int)s_player_status.detected_audio_type);
        }

        if (!current_attempt_failed && s_player_status.is_playing) {
            int current_bytes_read = 0;
            bool stream_lost_or_ended = false;
            TickType_t last_data_time = xTaskGetTickCount();

            while (s_player_status.is_playing && !stream_lost_or_ended) {
                if (player_take_stop_signal()) {
                    ESP_LOGI(TAG, "Stop signal received during data processing loop.");
                    process_data_status = ESP_OK;
                    stream_lost_or_ended = true;
                    break;
                }

                if (s_player_status.icy.metaint_interval > 0 && s_player_status.icy.bytes_until_meta <= 0) {
                    bool meta_end = false;
                    esp_err_t icy_err = icy_process_metadata(&s_player_status.icy, s_player_status.http_client_handle, s_player_status.stop_sem, &meta_end);
                    if (icy_err == ESP_ERR_INVALID_STATE) {
                        ESP_LOGI(TAG, "Stop signal received while reading ICY metadata.");
                        s_player_status.is_playing = false;
                    }
                    if (icy_err != ESP_OK || meta_end) {
                        process_data_status = ESP_FAIL;
                        stream_lost_or_ended = true;
                        break;
                    }
                }

                int bytes_to_read_this_round = HTTP_RECEIVE_BUFFER_SIZE;
                if (s_player_status.icy.metaint_interval > 0 &&
                    s_player_status.icy.bytes_until_meta > 0 &&
                    s_player_status.icy.bytes_until_meta < bytes_to_read_this_round) {
                    bytes_to_read_this_round = s_player_status.icy.bytes_until_meta;
                }

                if (bytes_to_read_this_round > 0) {
                    current_bytes_read = esp_http_client_read(s_player_status.http_client_handle, (char*)s_player_status.http_recv_buffer, bytes_to_read_this_round);
                } else {
                    current_bytes_read = 0;
                }

                if (current_bytes_read == -ESP_ERR_HTTP_EAGAIN) {
                    current_bytes_read = 0;
                } else if (current_bytes_read < 0) {
                    ESP_LOGE(TAG, "HTTP client read error: %s. Status: %d, Content Length: %lld",
                             esp_err_to_name(esp_http_client_get_errno(s_player_status.http_client_handle)),
                             esp_http_client_get_status_code(s_player_status.http_client_handle),
                             esp_http_client_get_content_length(s_player_status.http_client_handle));
                    process_data_status = ESP_FAIL;
                    stream_lost_or_ended = true;
                    break;
                }

                if (current_bytes_read == 0) {
                    if (esp_http_client_is_complete_data_received(s_player_status.http_client_handle)) {
                        ESP_LOGI(TAG, "HTTP stream finished (all data received).");
                        process_data_status = ESP_OK;
                        stream_lost_or_ended = true;
                        break;
                    } else {
                        if ((xTaskGetTickCount() - last_data_time) > pdMS_TO_TICKS(15000)) {
                            ESP_LOGE(TAG, "No data received for 15 seconds, assuming stream timeout. HTTP errno: %s (%d)",
                                     esp_err_to_name(esp_http_client_get_errno(s_player_status.http_client_handle)),
                                     esp_http_client_get_errno(s_player_status.http_client_handle));
                            process_data_status = ESP_ERR_TIMEOUT;
                            stream_lost_or_ended = true;
                            break;
                        } else {
                            ESP_LOGD(TAG, "HTTP client read 0 bytes. Waiting for more data. HTTP errno: %s (%d). Data complete: no.",
                                     esp_err_to_name(esp_http_client_get_errno(s_player_status.http_client_handle)),
                                     esp_http_client_get_errno(s_player_status.http_client_handle));
                            vTaskDelay(pdMS_TO_TICKS(200));
                        }
                    }
                } else if (current_bytes_read > 0) {
                    ESP_LOGD(TAG, "HTTP client read %d bytes.", current_bytes_read);
                    last_data_time = xTaskGetTickCount();
                    if (s_player_status.icy.metaint_interval > 0) {
                        s_player_status.icy.bytes_until_meta -= current_bytes_read;
                    }

                    if (first_data_chunk && current_bytes_read > 0) {
                        wrapper_audio_type_t type_from_data = wrapper_detect_audio_type_from_data(s_player_status.http_recv_buffer, current_bytes_read);
                        ESP_LOGI(TAG, "Detected audio type from data (first chunk): %d. URL-hinted type was: %d", (int)type_from_data, (int)s_player_status.detected_audio_type);

                        if (type_from_data != WRAPPER_TYPE_NONE) {
                            if (s_player_status.detected_audio_type != WRAPPER_TYPE_NONE && type_from_data != s_player_status.detected_audio_type) {
                                ESP_LOGW(TAG, "Data-detected type (%d) overrides URL-hinted/header type (%d).", (int)type_from_data, (int)s_player_status.detected_audio_type);
                            }
                            s_player_status.detected_audio_type = type_from_data;
                        } else if (s_player_status.detected_audio_type == WRAPPER_TYPE_NONE) {
                             ESP_LOGE(TAG, "Critical: Failed to determine audio type from both URL/header and data. Cannot open decoder.");
                             process_data_status = ESP_FAIL; stream_lost_or_ended = true; break;
                        }

                        if (s_player_status.detected_audio_type != WRAPPER_TYPE_NONE) {
                            if (s_player_status.detected_audio_type == WRAPPER_TYPE_AAC) {
                                codec_deinit_aac_decoder();
                                if (codec_aac_init_buffers(CODEC_AAC_BUFFER_SIZE) != ESP_OK) {
                                    ESP_LOGE(TAG, "Failed to init AAC buffers. Stopping.");
                                    process_data_status = ESP_ERR_NO_MEM; stream_lost_or_ended = true; break;
                                }
                                if (codec_init_aac_decoder() != ESP_OK) {
                                     ESP_LOGE(TAG, "Failed to register AAC decoder. Stopping.");
                                     process_data_status = ESP_FAIL; stream_lost_or_ended = true; break;
                                }
                            } else if (s_player_status.detected_audio_type == WRAPPER_TYPE_MP3) {
                                codec_deinit_mp3_decoder();
                                if (codec_mp3_init_buffers() != ESP_OK) {
                                    ESP_LOGE(TAG, "Failed to init MP3 buffers. Stopping.");
                                    process_data_status = ESP_ERR_NO_MEM; stream_lost_or_ended = true; break;
                                }
                                 if (codec_init_mp3_decoder() != ESP_OK) {
                                    ESP_LOGE(TAG, "Failed to register MP3 decoder. Stopping.");
                                    process_data_status = ESP_FAIL; stream_lost_or_ended = true; break;
                                }
                            }

                            if (s_player_status.decoder_handle) {
                                ESP_LOGW(TAG, "Decoder handle was not NULL before opening. Closing previous.");
                                if (s_player_status.detected_audio_type == WRAPPER_TYPE_AAC) codec_aac_unregister_decoder(); else if (s_player_status.detected_audio_type == WRAPPER_TYPE_MP3) codec_mp3_unregister_decoder(&s_player_status.decoder_handle);
                                esp_audio_dec_close(s_player_status.decoder_handle);
                                s_player_status.decoder_handle = NULL;
                            }

                            esp_err_t open_err = wrapper_open_detected_decoder(s_player_status.detected_audio_type, &s_player_status.decoder_handle);
                            if (open_err != ESP_OK || s_player_status.decoder_handle == NULL) {
                                ESP_LOGE(TAG, "Failed to open detected decoder type %d: %s", (int)s_player_status.detected_audio_type, esp_err_to_name(open_err));
                                if (s_player_status.detected_audio_type == WRAPPER_TYPE_AAC) { codec_aac_deinit_buffers(); codec_aac_unregister_decoder(); }
                                else if (s_player_status.detected_audio_type == WRAPPER_TYPE_MP3) { codec_mp3_deinit_buffers(); codec_mp3_unregister_decoder(&s_player_status.decoder_handle); }
                                s_player_status.detected_audio_type = WRAPPER_TYPE_NONE;
                                process_data_status = open_err; stream_lost_or_ended = true; break;
                            }
                            ESP_LOGI(TAG, "Decoder for type %d opened successfully. Handle: %p", (int)s_player_status.detected_audio_type, s_player_status.decoder_handle);
                            first_data_chunk = false;
                        } else {
                             ESP_LOGE(TAG, "Audio type remains NONE after first chunk. Cannot proceed.");
                             process_data_status = ESP_FAIL; stream_lost_or_ended = true; break;
                        }
                    }

                    if (s_player_status.decoder_handle && s_player_status.detected_audio_type != WRAPPER_TYPE_NONE && current_bytes_read > 0) {
                        esp_err_t codec_process_err = ESP_FAIL;
                        if (s_player_status.detected_audio_type == WRAPPER_TYPE_AAC) {
                            codec_process_err = codec_aac_process_data(s_player_status.decoder_handle, s_player_status.http_recv_buffer, current_bytes_read);
                        } else if (s_player_status.detected_audio_type == WRAPPER_TYPE_MP3) {
                            codec_process_err = codec_mp3_process_data(s_player_status.decoder_handle, s_player_status.http_recv_buffer, current_bytes_read);
                        }

                        if (codec_process_err != ESP_OK) {
                            ESP_LOGE(TAG, "Error processing audio data with codec type %d: %s", (int)s_player_status.detected_audio_type, esp_err_to_name(codec_process_err));
                            process_data_status = codec_process_err;
                            stream_lost_or_ended = true;
                            break;
                        }
                        if (!info_logged) {
                            esp_audio_dec_info_t info;
                            if (esp_audio_dec_get_info(s_player_status.decoder_handle, &info) == ESP_AUDIO_ERR_OK) {
                                ESP_LOGI(TAG, "Stream info: sample_rate=%u Hz, bits_per_sample=%d, channels=%d, bitrate=%u, frame_size=%u",
                                        (unsigned int)info.sample_rate, info.bits_per_sample, info.channel,
                                        (unsigned int)info.bitrate, (unsigned int)info.frame_size);

                                bool is_high_bitrate = (s_player_status.detected_audio_type == WRAPPER_TYPE_MP3) &&
                                                       ((info.bitrate >= 250 && info.bitrate < 1000) || (info.bitrate >= 250000));

                                if (s_player_status.is_pcradio_stream && is_high_bitrate) {
                                    char *path = s_player_status.pcradio_url_path;
                                    char *hi_suffix = strstr(path, "-hi");

                                    if (hi_suffix != NULL && hi_suffix[3] == '\0') {
                                        ESP_LOGI(TAG, "High bitrate MP3 stream (>=250kbps) detected. Switching to 'med' quality.");

                                        size_t base_len = hi_suffix - path;
                                        char *new_path = malloc(base_len + 5);
                                        if (new_path) {
                                            memcpy(new_path, path, base_len);
                                            strcpy(new_path + base_len, "-med");

                                            free(s_player_status.pcradio_url_path);
                                            s_player_status.pcradio_url_path = new_path;

                                            stream_lost_or_ended = true;
                                            process_data_status = ESP_OK;
                                            is_quality_switch_reconnect = true;
                                            ESP_LOGI(TAG, "Restarting stream with new path: %s", s_player_status.pcradio_url_path);
                                        } else {
                                            ESP_LOGE(TAG, "Failed to allocate memory for new path. Continuing with high bitrate stream.");
                                        }
                                    }
                                }

                                if (!stream_lost_or_ended) {
                                    if (audio_i2s_init(info.sample_rate, info.bits_per_sample, info.channel) != ESP_OK) {
                                        ESP_LOGE(TAG, "Failed to re-initialize I2S with new stream parameters.");
                                        process_data_status = ESP_FAIL;
                                        stream_lost_or_ended = true;
                                    }
                                    eq_config_para(info.sample_rate, info.bits_per_sample, info.channel, g_app_config->player.eq_preset);
                                }
                            } else {
                                ESP_LOGW(TAG, "Failed to get stream info from decoder after first decode");
                            }
                            info_logged = true;
                        }
                    } else if (!s_player_status.decoder_handle && !first_data_chunk && current_bytes_read > 0) {
                        ESP_LOGE(TAG, "Decoder not open, but not first chunk. Should not happen. Stopping.");
                        process_data_status = ESP_FAIL;
                        stream_lost_or_ended = true;
                        break;
                    }
                }
                if (stream_lost_or_ended) break;
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }

        if (s_player_status.decoder_handle) {
            ESP_LOGI(TAG, "Closing previous decoder handle %p", s_player_status.decoder_handle);
            esp_audio_dec_close(s_player_status.decoder_handle);
            s_player_status.decoder_handle = NULL;
        }
        codec_mp3_deinit_buffers();
        codec_aac_deinit_buffers();
        s_player_status.detected_audio_type = WRAPPER_TYPE_NONE;


        if (s_player_status.http_client_handle) {
            ESP_LOGI(TAG, "Cleaning up HTTP client for URL: %s", s_player_status.current_playback_url ? s_player_status.current_playback_url : "N/A");
            ESP_LOGI(TAG, "Free internal RAM: %u bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            esp_http_client_close(s_player_status.http_client_handle);
            esp_http_client_cleanup(s_player_status.http_client_handle);
            s_player_status.http_client_handle = NULL;
        }
        free(s_player_status.current_playback_url);
        s_player_status.current_playback_url = NULL;


        if (!s_player_status.is_playing) {
            ESP_LOGI(TAG, "Playback stopped by user signal. Exiting player_task loop.");
            break;
        }

        if (is_quality_switch_reconnect) {
            ESP_LOGI(TAG, "Reconnecting due to stream quality switch...");
            is_quality_switch_reconnect = false;
            continue;
        }

        if (connect_err != ESP_OK) {
            current_attempt_failed = true;
             ESP_LOGW(TAG, "Stream attempt failed: Connection error (%s).", esp_err_to_name(connect_err));
        } else if (process_data_status != ESP_OK) {
            current_attempt_failed = true;
            ESP_LOGW(TAG, "Stream attempt failed: Data processing error (%s).", esp_err_to_name(process_data_status));
        }

        if (current_attempt_failed) {
            if (s_player_status.is_pcradio_stream) {
                s_player_status.pcradio_current_server_idx = (s_player_status.pcradio_current_server_idx + 1) % PCRADIO_NUM_ALT_SERVERS;
                ESP_LOGI(TAG, "PCRadio: Switching to server index %d after failure. Delaying for %d ms.", s_player_status.pcradio_current_server_idx, PCRADIO_RETRY_DELAY_MS);
                player_wait_or_stop(PCRADIO_RETRY_DELAY_MS);
            } else {
                ESP_LOGI(TAG, "Non-PCRadio stream failed. Stopping playback.");
                s_player_status.is_playing = false;
            }
        } else {
            ESP_LOGI(TAG, "Stream ended naturally (EOF).");
            if (s_player_status.is_pcradio_stream) {
                s_player_status.pcradio_current_server_idx = (s_player_status.pcradio_current_server_idx + 1) % PCRADIO_NUM_ALT_SERVERS;
                ESP_LOGI(TAG, "PCRadio: Switching to server index %d after stream end (EOF). Delaying for %d ms.", s_player_status.pcradio_current_server_idx, PCRADIO_RETRY_DELAY_MS);
                player_wait_or_stop(PCRADIO_RETRY_DELAY_MS);
            } else {
                ESP_LOGI(TAG, "Stream ended. Stopping playback.");
                s_player_status.is_playing = false;
            }
        }
         if (!s_player_status.is_playing) {
            break;
        }
    }

    ESP_LOGI(TAG, "Player task finishing and cleaning up all resources...");
    cleanup_current_stream_resources();
    s_player_status.is_playing = false;
    s_player_status.task_handle = NULL;
    xSemaphoreGive(s_player_status.stopped_sem);
    ESP_LOGI(TAG, "Player task deleting self.");
    vTaskDelete(NULL);
}
