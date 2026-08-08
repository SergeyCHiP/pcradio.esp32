#include "codec_mp3.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_heap_caps.h"
#include "audio_i2s.h"
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

static const char *TAG = "MP3";

typedef struct {
    uint8_t* buffer;
    uint32_t size;
    uint32_t capacity;
} codec_mp3_internal_buffer_t;

static codec_mp3_internal_buffer_t s_codec_mp3_raw_buffer = {0};
static uint8_t *s_codec_mp3_pcm_buffer = NULL;

#define MP3_BITRATE_TABLE_MPEG1_L3 {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}
#define MP3_SAMPLERATE_TABLE_MPEG1 {44100, 48000, 32000, 0}
#define MP3_SAMPLERATE_TABLE_MPEG2 {22050, 24000, 16000, 0}

esp_err_t codec_mp3_init_buffers(void) {
    if (s_codec_mp3_raw_buffer.buffer) {
        heap_caps_free(s_codec_mp3_raw_buffer.buffer);
        s_codec_mp3_raw_buffer.buffer = NULL;
    }
    s_codec_mp3_raw_buffer.buffer = heap_caps_malloc(CODEC_MP3_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_codec_mp3_raw_buffer.buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 raw data buffer (%d bytes)", CODEC_MP3_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }
    s_codec_mp3_raw_buffer.capacity = CODEC_MP3_BUFFER_SIZE;
    s_codec_mp3_raw_buffer.size = 0;
    ESP_LOGI(TAG, "MP3 raw data buffer initialized with capacity %d bytes in PSRAM", CODEC_MP3_BUFFER_SIZE);

    if (s_codec_mp3_pcm_buffer) {
        heap_caps_free(s_codec_mp3_pcm_buffer);
        s_codec_mp3_pcm_buffer = NULL;
    }
    s_codec_mp3_pcm_buffer = heap_caps_malloc(CODEC_MP3_PCM_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_codec_mp3_pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 PCM buffer (%d bytes)", CODEC_MP3_PCM_BUFFER_SIZE);
        heap_caps_free(s_codec_mp3_raw_buffer.buffer);
        s_codec_mp3_raw_buffer.buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "MP3 PCM buffer initialized with capacity %d bytes in PSRAM", CODEC_MP3_PCM_BUFFER_SIZE);
    return ESP_OK;
}

void codec_mp3_deinit_buffers(void) {
    if (s_codec_mp3_raw_buffer.buffer) {
        heap_caps_free(s_codec_mp3_raw_buffer.buffer);
        s_codec_mp3_raw_buffer.buffer = NULL;
    }
    s_codec_mp3_raw_buffer.capacity = 0;
    s_codec_mp3_raw_buffer.size = 0;
    ESP_LOGI(TAG, "MP3 raw data buffer deinitialized");

    if (s_codec_mp3_pcm_buffer) {
        heap_caps_free(s_codec_mp3_pcm_buffer);
        s_codec_mp3_pcm_buffer = NULL;
    }
    ESP_LOGI(TAG, "MP3 PCM buffer deinitialized");
}

static bool add_to_internal_mp3_buffer(const uint8_t* data, uint32_t len) {
    if (!s_codec_mp3_raw_buffer.buffer) {
        ESP_LOGE(TAG, "MP3 raw buffer not initialized.");
        return false;
    }
    if (s_codec_mp3_raw_buffer.size + len > s_codec_mp3_raw_buffer.capacity) {
        ESP_LOGE(TAG, "MP3 raw buffer overflow. Size: %" PRIu32 ", Add: %" PRIu32 ", Cap: %" PRIu32,
                 s_codec_mp3_raw_buffer.size, len, s_codec_mp3_raw_buffer.capacity);
        return false;
    }
    memcpy(s_codec_mp3_raw_buffer.buffer + s_codec_mp3_raw_buffer.size, data, len);
    s_codec_mp3_raw_buffer.size += len;
    return true;
}

static void remove_from_internal_mp3_buffer(uint32_t len) {
    if (len == 0 || !s_codec_mp3_raw_buffer.buffer) return;
    if (len >= s_codec_mp3_raw_buffer.size) {
        s_codec_mp3_raw_buffer.size = 0;
        return;
    }
    memmove(s_codec_mp3_raw_buffer.buffer, s_codec_mp3_raw_buffer.buffer + len, s_codec_mp3_raw_buffer.size - len);
    s_codec_mp3_raw_buffer.size -= len;
}

static int get_mp3_frame_size_internal(const uint8_t* header) {
    if (header[0] != 0xFF || (header[1] & 0xE0) != 0xE0) {
        return -1;
    }
    int mpeg_version = (header[1] & 0x18) >> 3;
    int layer = (header[1] & 0x06) >> 1;
    if (layer != 1) {
        return -1;
    }
    int bitrate_index = (header[2] & 0xF0) >> 4;
    int samplerate_index = (header[2] & 0x0C) >> 2;
    int padding = (header[2] & 0x02) >> 1;

    const int bitrate_table_mpeg1_l3[] = MP3_BITRATE_TABLE_MPEG1_L3;
    const int samplerate_table_mpeg1[] = MP3_SAMPLERATE_TABLE_MPEG1;
    const int samplerate_table_mpeg2[] = MP3_SAMPLERATE_TABLE_MPEG2;
    int bitrate = 0;
    int samplerate = 0;

    if (mpeg_version == 3) {
        bitrate = bitrate_table_mpeg1_l3[bitrate_index];
        samplerate = samplerate_table_mpeg1[samplerate_index];
    } else if (mpeg_version == 2 || mpeg_version == 0) {
        bitrate = bitrate_table_mpeg1_l3[bitrate_index];
        samplerate = samplerate_table_mpeg2[samplerate_index];
    } else {
        return -1;
    }

    if (bitrate == 0 || samplerate == 0 || bitrate_index == 15) {
        return -1;
    }
    int samples_per_frame = (mpeg_version == 3) ? 1152 : 576;
    int frame_size = (samples_per_frame / 8 * bitrate * 1000 / samplerate) + padding;
    return frame_size;
}

static bool find_valid_mp3_frame_internal(const uint8_t* buffer, uint32_t len, uint32_t* offset, uint32_t* frame_size_out) {
    if (len < 4) return false;

    for (uint32_t i = 0; i <= len - 4; i++) {
        if (buffer[i] == 0xFF && (buffer[i+1] & 0xE0) == 0xE0) {
            int current_frame_size = get_mp3_frame_size_internal(buffer + i);
            if (current_frame_size > 0 && i + current_frame_size <= len) {
                *offset = i;
                *frame_size_out = current_frame_size;
                static int log_count_frame_found = 0;
                if (log_count_frame_found < 5 || (log_count_frame_found % 100 == 0) ) {
                    ESP_LOGD(TAG, "Found potential MP3 frame at offset %" PRIu32 " with size %d", i, current_frame_size);
                    log_count_frame_found++;
                }
                return true;
            }
        }
    }
    return false;
}

esp_err_t codec_mp3_process_data(esp_audio_dec_handle_t dec_handle, const uint8_t* data, uint32_t len) {
    if (!s_codec_mp3_raw_buffer.buffer || !s_codec_mp3_pcm_buffer) {
        ESP_LOGE(TAG, "MP3 raw or PCM buffer not initialized.");
        return ESP_ERR_INVALID_STATE;
    }        if (!add_to_internal_mp3_buffer(data, len)) {
        ESP_LOGE(TAG, "Failed to add data to MP3 internal buffer. Size: %" PRIu32, s_codec_mp3_raw_buffer.size);
        if (s_codec_mp3_raw_buffer.size == s_codec_mp3_raw_buffer.capacity) {
            ESP_LOGW(TAG, "MP3 raw buffer is full. Clearing to attempt recovery.");
            s_codec_mp3_raw_buffer.size = 0;
        }
        return ESP_FAIL;
    }

    uint32_t offset = 0;
    uint32_t frame_size = 0;

    while (find_valid_mp3_frame_internal(s_codec_mp3_raw_buffer.buffer, s_codec_mp3_raw_buffer.size, &offset, &frame_size)) {
        if (offset > 0) {
            ESP_LOGD(TAG, "Skipping %" PRIu32 " bytes before MP3 frame.", offset);
            remove_from_internal_mp3_buffer(offset);
        }

        if (s_codec_mp3_raw_buffer.size < frame_size) {
            ESP_LOGD(TAG, "Not enough data for a complete MP3 frame (need %" PRIu32 ", have %" PRIu32 "), waiting for more.", frame_size, s_codec_mp3_raw_buffer.size);
            break;
        }

        esp_audio_dec_in_raw_t in_frame_data;
        esp_audio_dec_out_frame_t out_frame_data;

        in_frame_data.buffer = s_codec_mp3_raw_buffer.buffer;
        in_frame_data.len = frame_size;

        out_frame_data.buffer = s_codec_mp3_pcm_buffer;
        out_frame_data.len = CODEC_MP3_PCM_BUFFER_SIZE;

        static int frame_decode_log_count_mp3 = 0;
        ESP_LOGD(TAG, "Processing MP3 frame #%d (size %" PRIu32 ")", frame_decode_log_count_mp3, frame_size);
        frame_decode_log_count_mp3++;

        esp_audio_err_t dec_err = esp_audio_dec_process(dec_handle, &in_frame_data, &out_frame_data);

        if (dec_err == ESP_AUDIO_ERR_OK) {
            if (out_frame_data.decoded_size > 0) {
                size_t bytes_written_to_i2s = 0;
                esp_err_t i2s_err = audio_i2s_write(out_frame_data.buffer, out_frame_data.decoded_size, &bytes_written_to_i2s,
                                                    pdMS_TO_TICKS(AUDIO_I2S_WRITE_TIMEOUT_MS));
                if (i2s_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to write %" PRIu32 " MP3 PCM bytes to I2S: %s. Written: %zu",
                             out_frame_data.decoded_size, esp_err_to_name(i2s_err), bytes_written_to_i2s);
                }
                 ESP_LOGD(TAG, "MP3 frame #%d decoded: %" PRIu32 " PCM bytes, sent %zu to I2S",
                         frame_decode_log_count_mp3, out_frame_data.decoded_size, bytes_written_to_i2s);
            } else {
                ESP_LOGD(TAG, "MP3 frame #%d decoded, but 0 PCM bytes output.", frame_decode_log_count_mp3);
            }
        } else if (dec_err == ESP_AUDIO_ERR_FAIL) {
            ESP_LOGW(TAG, "MP3 decoder failed for frame #%d (ESP_AUDIO_ERR_FAIL). Skipping frame.", frame_decode_log_count_mp3);
            memset(s_codec_mp3_pcm_buffer, 0, CODEC_MP3_PCM_BUFFER_SIZE);
        } else if (dec_err == ESP_AUDIO_ERR_MEM_LACK) {
            ESP_LOGW(TAG, "MP3 decoder reported ESP_AUDIO_ERR_MEM_LACK for frame #%d, raw frame size %" PRIu32 ". Stream issue or insufficient output buffer?", frame_decode_log_count_mp3, frame_size);
        } else {
            ESP_LOGW(TAG, "MP3 decoder error for frame #%d: %s (code %d)", frame_decode_log_count_mp3, esp_err_to_name(dec_err), dec_err);
        }
        remove_from_internal_mp3_buffer(frame_size);
    }

    if (s_codec_mp3_raw_buffer.size > CODEC_MP3_BUFFER_SIZE / 2 && !find_valid_mp3_frame_internal(s_codec_mp3_raw_buffer.buffer, s_codec_mp3_raw_buffer.size, &offset, &frame_size)) {
        ESP_LOGW(TAG, "MP3 buffer has %" PRIu32 " bytes but no valid frames found. Clearing buffer to prevent stall.", s_codec_mp3_raw_buffer.size);
        s_codec_mp3_raw_buffer.size = 0;
    }
    return ESP_OK;
}

esp_err_t codec_init_mp3_decoder(void) {
    ESP_LOGI(TAG, "Registering MP3 decoder with the system.");
    esp_audio_err_t ret = esp_mp3_dec_register();
    if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
        ESP_LOGE(TAG, "Failed to register MP3 decoder: %d (%s)", ret, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MP3 decoder registered successfully or was already registered.");
    return ESP_OK;
}

esp_err_t codec_open_mp3_decoder(esp_audio_dec_handle_t *handle_out) {
    if (handle_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Opening MP3 decoder instance.");
    esp_audio_dec_cfg_t dec_cfg = {0};
    dec_cfg.type = ESP_AUDIO_TYPE_MP3;

    esp_err_t ret = esp_audio_dec_open(&dec_cfg, handle_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %s (code %d)", esp_err_to_name(ret), ret);
        *handle_out = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "MP3 decoder opened successfully, handle: %p", *handle_out);
    return ESP_OK;
}

esp_err_t codec_mp3_unregister_decoder(esp_audio_dec_handle_t *handle) {
    if (handle && *handle) {
        esp_audio_dec_close(*handle);
        ESP_LOGI(TAG, "Attempted to close MP3 decoder handle %p.", *handle);
        *handle = NULL;
    }
    ESP_LOGI(TAG, "Unregistering MP3 decoder from the system.");
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
    ESP_LOGI(TAG, "MP3 decoder unregistration attempted.");
    return ESP_OK;
}

esp_err_t codec_deinit_mp3_decoder(void) {
    ESP_LOGI(TAG, "Unregistering MP3 decoder from the system.");
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
    ESP_LOGI(TAG, "MP3 decoder unregistration attempted.");
    return ESP_OK;
}
