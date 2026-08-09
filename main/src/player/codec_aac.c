#include "codec_aac.h"
#include "esp_log.h"
#include "esp_aac_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_heap_caps.h"
#include "audio_i2s.h"
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

static const char *TAG = "AAC";

typedef struct {
    uint8_t* buffer;
    uint32_t size;
    uint32_t capacity;
} codec_aac_internal_buffer_t;

static codec_aac_internal_buffer_t s_codec_aac_raw_buffer = {0};
static uint8_t *s_codec_aac_pcm_buffer = NULL;

#define CODEC_AAC_INTERNAL_PCM_BUFFER_SIZE (4096)

esp_err_t codec_aac_init_buffers(uint32_t raw_buffer_capacity) {
    ESP_LOGI(TAG, "Checking available memory before AAC buffer allocation...");
    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t largest_psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Available memory: heap=%zu, PSRAM=%zu", free_heap, free_psram);
    ESP_LOGI(TAG, "Largest contiguous blocks: heap=%zu, PSRAM=%zu", largest_free_block, largest_psram_block);

    if (raw_buffer_capacity > 16384) {
        ESP_LOGW(TAG, "Requested raw buffer size %" PRIu32 " is too large, limiting to 16KB", raw_buffer_capacity);
        raw_buffer_capacity = 16384;
    }

    if (s_codec_aac_raw_buffer.buffer) {
        heap_caps_free(s_codec_aac_raw_buffer.buffer);
        s_codec_aac_raw_buffer.buffer = NULL;
    }

    s_codec_aac_raw_buffer.buffer = heap_caps_malloc(raw_buffer_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_codec_aac_raw_buffer.buffer) {
        ESP_LOGW(TAG, "Failed to allocate AAC raw buffer in PSRAM, trying regular heap");
        s_codec_aac_raw_buffer.buffer = malloc(raw_buffer_capacity);
        if (!s_codec_aac_raw_buffer.buffer) {
            ESP_LOGE(TAG, "Failed to allocate AAC raw data buffer (%" PRIu32 " bytes)", raw_buffer_capacity);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "AAC raw data buffer allocated in regular heap (%" PRIu32 " bytes)", raw_buffer_capacity);
    } else {
        ESP_LOGI(TAG, "AAC raw data buffer allocated in PSRAM (%" PRIu32 " bytes)", raw_buffer_capacity);
    }

    s_codec_aac_raw_buffer.capacity = raw_buffer_capacity;
    s_codec_aac_raw_buffer.size = 0;

    if (s_codec_aac_pcm_buffer) {
        heap_caps_free(s_codec_aac_pcm_buffer);
        s_codec_aac_pcm_buffer = NULL;
    }

    s_codec_aac_pcm_buffer = heap_caps_malloc(CODEC_AAC_INTERNAL_PCM_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_codec_aac_pcm_buffer) {
        ESP_LOGW(TAG, "Failed to allocate AAC PCM buffer in PSRAM, trying regular heap");
        s_codec_aac_pcm_buffer = malloc(CODEC_AAC_INTERNAL_PCM_BUFFER_SIZE);
        if (!s_codec_aac_pcm_buffer) {
            ESP_LOGE(TAG, "Failed to allocate AAC PCM buffer (%d bytes)", CODEC_AAC_INTERNAL_PCM_BUFFER_SIZE);
            if (s_codec_aac_raw_buffer.buffer) {
                heap_caps_free(s_codec_aac_raw_buffer.buffer);
                s_codec_aac_raw_buffer.buffer = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "AAC PCM buffer allocated in regular heap (%d bytes)", CODEC_AAC_INTERNAL_PCM_BUFFER_SIZE);
    } else {
        ESP_LOGI(TAG, "AAC PCM buffer allocated in PSRAM (%d bytes)", CODEC_AAC_INTERNAL_PCM_BUFFER_SIZE);
    }

    free_heap = esp_get_free_heap_size();
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Memory after AAC buffer allocation: heap=%zu, PSRAM=%zu", free_heap, free_psram);

    return ESP_OK;
}

void codec_aac_deinit_buffers() {
    ESP_LOGI(TAG, "Deinitializing AAC buffers...");

    size_t free_heap_before = esp_get_free_heap_size();
    size_t free_psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (s_codec_aac_raw_buffer.buffer) {
        heap_caps_free(s_codec_aac_raw_buffer.buffer);
        s_codec_aac_raw_buffer.buffer = NULL;
        ESP_LOGI(TAG, "AAC raw data buffer freed");
    }
    s_codec_aac_raw_buffer.capacity = 0;
    s_codec_aac_raw_buffer.size = 0;

    if (s_codec_aac_pcm_buffer) {
        heap_caps_free(s_codec_aac_pcm_buffer);
        s_codec_aac_pcm_buffer = NULL;
        ESP_LOGI(TAG, "AAC PCM buffer freed");
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    size_t free_heap_after = esp_get_free_heap_size();
    size_t free_psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Memory freed by buffer cleanup: heap=%zd, PSRAM=%zd",
             free_heap_after - free_heap_before,
             free_psram_after - free_psram_before);

    ESP_LOGI(TAG, "AAC buffers deinitialization completed");
}

esp_err_t codec_aac_add_data(const uint8_t* data, uint32_t len) {
    if (!data || len == 0) {
        ESP_LOGW(TAG, "Invalid data parameters: data=%p, len=%" PRIu32, data, len);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_codec_aac_raw_buffer.buffer) {
        ESP_LOGE(TAG, "AAC raw buffer not initialized.");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_codec_aac_raw_buffer.size + len > s_codec_aac_raw_buffer.capacity) {
        ESP_LOGE(TAG, "AAC raw buffer overflow. Current size: %" PRIu32 ", trying to add: %" PRIu32 ", capacity: %" PRIu32,
                 s_codec_aac_raw_buffer.size, len, s_codec_aac_raw_buffer.capacity);
        return ESP_ERR_NO_MEM;
    }

    memcpy(s_codec_aac_raw_buffer.buffer + s_codec_aac_raw_buffer.size, data, len);
    s_codec_aac_raw_buffer.size += len;
    ESP_LOGD(TAG, "Added %" PRIu32 " bytes to AAC raw buffer. New size: %" PRIu32, len, s_codec_aac_raw_buffer.size);
    return ESP_OK;
}

static void remove_from_internal_aac_buffer(uint32_t len) {
    if (len == 0 || !s_codec_aac_raw_buffer.buffer) return;

    if (len > s_codec_aac_raw_buffer.size) {
        ESP_LOGW(TAG, "Trying to remove %" PRIu32 " bytes from AAC raw buffer, but it only has %" PRIu32 ". Clearing buffer.", len, s_codec_aac_raw_buffer.size);
        s_codec_aac_raw_buffer.size = 0;
        return;
    }
    if (len == s_codec_aac_raw_buffer.size) {
        s_codec_aac_raw_buffer.size = 0;
        return;
    }
    memmove(s_codec_aac_raw_buffer.buffer, s_codec_aac_raw_buffer.buffer + len, s_codec_aac_raw_buffer.size - len);
    s_codec_aac_raw_buffer.size -= len;
}

esp_err_t codec_aac_process_data(esp_audio_dec_handle_t dec_handle, const uint8_t* data, uint32_t len) {
    if (!s_codec_aac_raw_buffer.buffer || !s_codec_aac_pcm_buffer) {
        ESP_LOGE(TAG, "AAC raw or PCM buffer not initialized for processing.");
        return ESP_ERR_INVALID_STATE;
    }

    if (codec_aac_add_data(data, len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add data to AAC internal buffer. Buffer size: %" PRIu32 ", capacity: %" PRIu32 ".",
                 s_codec_aac_raw_buffer.size, s_codec_aac_raw_buffer.capacity);
        return ESP_FAIL;
    }

    const uint32_t MIN_ADTS_HEADER_SIZE = 7;

    while (s_codec_aac_raw_buffer.size >= MIN_ADTS_HEADER_SIZE) {
        uint32_t offset = 0;
        bool found_sync = false;

        for (uint32_t i = 0; i <= s_codec_aac_raw_buffer.size - 2; i++) {
            if (s_codec_aac_raw_buffer.buffer[i] == 0xFF && (s_codec_aac_raw_buffer.buffer[i+1] & 0xF0) == 0xF0) {
                if ((s_codec_aac_raw_buffer.buffer[i+1] & 0x06) == 0x00) {
                    found_sync = true;
                    offset = i;
                    break;
                }
            }
        }

        if (!found_sync) {
            ESP_LOGD(TAG, "No ADTS sync found in %" PRIu32 " bytes of AAC data. Waiting for more.", s_codec_aac_raw_buffer.size);
            if (s_codec_aac_raw_buffer.size > s_codec_aac_raw_buffer.capacity * 0.9) {
                 ESP_LOGW(TAG, "AAC buffer nearly full (%" PRIu32 "/%" PRIu32 ") with no ADTS sync. Discarding oldest half.", s_codec_aac_raw_buffer.size, s_codec_aac_raw_buffer.capacity);
                 remove_from_internal_aac_buffer(s_codec_aac_raw_buffer.size / 2);
            }
            break;
        }

        if (offset > 0) {
            ESP_LOGD(TAG, "Skipping %" PRIu32 " bytes before ADTS frame in AAC buffer.", offset);
            remove_from_internal_aac_buffer(offset);
        }

        if (s_codec_aac_raw_buffer.size < MIN_ADTS_HEADER_SIZE) {
            ESP_LOGD(TAG, "Not enough data for ADTS header after skipping offset. Buffer size: %" PRIu32, s_codec_aac_raw_buffer.size);
            break;
        }

        uint32_t frame_size = ((uint32_t)(s_codec_aac_raw_buffer.buffer[3] & 0x03) << 11) |
                              ((uint32_t)s_codec_aac_raw_buffer.buffer[4] << 3) |
                              ((uint32_t)(s_codec_aac_raw_buffer.buffer[5] & 0xE0) >> 5);

        if (frame_size == 0 || frame_size < MIN_ADTS_HEADER_SIZE) {
            ESP_LOGW(TAG, "Invalid ADTS frame size: %" PRIu32 ". Skipping sync bytes (2) and retrying.", frame_size);
            remove_from_internal_aac_buffer(2);
            continue;
        }

        if (frame_size > s_codec_aac_raw_buffer.capacity) {
            ESP_LOGE(TAG, "ADTS frame size %" PRIu32 " exceeds buffer capacity %" PRIu32 ". This is a critical error. Clearing buffer.", frame_size, s_codec_aac_raw_buffer.capacity);
            s_codec_aac_raw_buffer.size = 0;
            return ESP_FAIL;
        }

        if (frame_size > s_codec_aac_raw_buffer.size) {
            ESP_LOGD(TAG, "Incomplete AAC frame: need %" PRIu32 ", have %" PRIu32 ". Waiting for more data.", frame_size, s_codec_aac_raw_buffer.size);
            break;
        }

        ESP_LOGD(TAG, "Found AAC ADTS frame, reported size: %" PRIu32, frame_size);

        esp_audio_dec_in_raw_t in_frame;
        esp_audio_dec_out_frame_t out_frame;

        in_frame.buffer = s_codec_aac_raw_buffer.buffer;
        in_frame.len = frame_size;

        out_frame.buffer = s_codec_aac_pcm_buffer;
        out_frame.len = CODEC_AAC_INTERNAL_PCM_BUFFER_SIZE;

        static int frame_decode_log_count = 0;
        ESP_LOGD(TAG, "Processing AAC frame #%d (size %" PRIu32 ")", frame_decode_log_count, frame_size);
        frame_decode_log_count++;

        esp_audio_err_t dec_err = esp_audio_dec_process(dec_handle, &in_frame, &out_frame);

        if (dec_err == ESP_AUDIO_ERR_OK) {
            if (out_frame.decoded_size > 0) {
                size_t bytes_written_to_i2s = 0;
                esp_err_t i2s_err = audio_i2s_write(out_frame.buffer, out_frame.decoded_size, &bytes_written_to_i2s,
                                                    pdMS_TO_TICKS(AUDIO_I2S_WRITE_TIMEOUT_MS));
                if (i2s_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to write %" PRIu32 " AAC PCM bytes to I2S: %s. Written: %zu",
                             out_frame.decoded_size, esp_err_to_name(i2s_err), bytes_written_to_i2s);
                }
                ESP_LOGD(TAG, "AAC frame #%d decoded: %" PRIu32 " PCM bytes, sent %zu to I2S",
                         frame_decode_log_count, out_frame.decoded_size, bytes_written_to_i2s);
            } else {
                 ESP_LOGD(TAG, "AAC frame #%d decoded, but 0 PCM bytes output.", frame_decode_log_count);
            }
        } else if (dec_err == ESP_AUDIO_ERR_FAIL) {
            ESP_LOGW(TAG, "AAC decoder failed for frame #%d (ESP_AUDIO_ERR_FAIL). Skipping frame.", frame_decode_log_count);
            memset(s_codec_aac_pcm_buffer, 0, CODEC_AAC_INTERNAL_PCM_BUFFER_SIZE);
        } else if (dec_err == ESP_AUDIO_ERR_MEM_LACK) {
             ESP_LOGW(TAG, "AAC decoder reported ESP_AUDIO_ERR_MEM_LACK for frame #%d, though raw frame size was %" PRIu32 ". This might indicate a stream issue or insufficient output buffer.", frame_decode_log_count, frame_size);
        } else {
            ESP_LOGW(TAG, "AAC decoder error for frame #%d: %s (code %d)", frame_decode_log_count, esp_err_to_name(dec_err), dec_err);
        }
        remove_from_internal_aac_buffer(frame_size);
    }
    return ESP_OK;
}

static bool s_aac_decoder_registered = false;

esp_err_t codec_init_aac_decoder(void) {
    ESP_LOGI(TAG, "Registering AAC decoder with the system.");
    esp_audio_err_t ret = esp_aac_dec_register();
    if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_ALREADY_EXIST) {
        ESP_LOGE(TAG, "Failed to register AAC decoder: %d (%s)", ret, esp_err_to_name(ret));
        s_aac_decoder_registered = false;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AAC decoder registered successfully or was already registered.");
    s_aac_decoder_registered = true;
    return ESP_OK;
}

esp_err_t codec_open_aac_decoder(esp_audio_dec_handle_t *handle_out) {
    if (handle_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t free_heap = esp_get_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Memory before opening AAC decoder: heap=%zu, PSRAM=%zu, largest_block=%zu",
             free_heap, free_psram, largest_free_block);

    if (free_heap < 32768 || largest_free_block < 16384) {
        ESP_LOGW(TAG, "Low heap memory (heap=%zu, largest_block=%zu), attempting memory defragmentation",
                 free_heap, largest_free_block);

        for (int i = 0; i < 20; i++) {
            void* temp = malloc(1024);
            if (temp) {
                free(temp);
            }
            vTaskDelay(5 / portTICK_PERIOD_MS);
        }

        free_heap = esp_get_free_heap_size();
        largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "After defragmentation: heap=%zu, largest_block=%zu", free_heap, largest_free_block);
    }

    ESP_LOGI(TAG, "Opening AAC decoder instance (expecting ADTS headers).");

    esp_err_t ret;
    esp_audio_dec_handle_t decoder_handle = NULL;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "AAC decoder open attempt #%d after cleanup", attempt + 1);

            vTaskDelay(pdMS_TO_TICKS(50));

            heap_caps_malloc_extmem_enable(1024);

            free_heap = esp_get_free_heap_size();
            largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            ESP_LOGI(TAG, "Before retry: heap=%zu, largest_block=%zu", free_heap, largest_free_block);
        }

        esp_audio_dec_cfg_t general_dec_cfg = {0};
        general_dec_cfg.type = ESP_AUDIO_TYPE_AAC;

        esp_aac_dec_cfg_t aac_specific_cfg = {0};
        aac_specific_cfg.no_adts_header = false;
        aac_specific_cfg.aac_plus_enable = false;

        general_dec_cfg.cfg = &aac_specific_cfg;
        general_dec_cfg.cfg_sz = sizeof(esp_aac_dec_cfg_t);

        ESP_LOGI(TAG, "AAC decoder config: no_adts_header=%d, aac_plus_enable=%d",
                 (int)aac_specific_cfg.no_adts_header,
                 (int)aac_specific_cfg.aac_plus_enable);

        ret = esp_audio_dec_open(&general_dec_cfg, &decoder_handle);
        if (ret == ESP_OK) {
            break;
        } else {
            ESP_LOGW(TAG, "AAC decoder open attempt #%d failed: %s (code %d)",
                     attempt + 1, esp_err_to_name(ret), ret);
            if (decoder_handle) {
                esp_audio_dec_close(decoder_handle);
                decoder_handle = NULL;
            }
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open AAC decoder after all attempts: %s (code %d)", esp_err_to_name(ret), ret);
        free_heap = esp_get_free_heap_size();
        free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "Memory after failed AAC decoder open: heap=%zu, PSRAM=%zu", free_heap, free_psram);
        *handle_out = NULL;
        return ret;
    }

    *handle_out = decoder_handle;

    free_heap = esp_get_free_heap_size();
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "AAC decoder opened successfully, handle: %p", *handle_out);
    ESP_LOGI(TAG, "Memory after AAC decoder open: heap=%zu, PSRAM=%zu", free_heap, free_psram);
    return ESP_OK;
}

esp_err_t codec_aac_get_stream_info(esp_audio_dec_handle_t handle, int *out_sample_rate, int *out_channels, int *out_bits_per_sample) {
    if (handle == NULL || out_sample_rate == NULL || out_channels == NULL || out_bits_per_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_audio_dec_info_t dec_info;
    esp_err_t ret = esp_audio_dec_get_info(handle, &dec_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get stream info from AAC decoder: %s", esp_err_to_name(ret));
        return ret;
    }
    *out_sample_rate = dec_info.sample_rate;
    *out_channels = dec_info.channel;
    *out_bits_per_sample = dec_info.bits_per_sample;

    ESP_LOGI(TAG, "AAC Stream info from decoder: SR=%d, Channels=%d, BPS=%d",
             *out_sample_rate, *out_channels, *out_bits_per_sample);
    return ESP_OK;
}

esp_err_t codec_aac_close_decoder(esp_audio_dec_handle_t handle) {
    if (handle == NULL) {
        ESP_LOGW(TAG, "AAC decoder handle is NULL, nothing to close");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Closing AAC decoder handle: %p", handle);

    ESP_LOGI(TAG, "Attempting to close AAC decoder...");
    esp_audio_dec_close(handle);
    ESP_LOGI(TAG, "AAC decoder closed successfully");

    return ESP_OK;
}

esp_err_t codec_aac_unregister_decoder(void) {
    if (!s_aac_decoder_registered) {
        ESP_LOGI(TAG, "AAC decoder was not registered, skipping unregister");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unregistering AAC decoder from the system.");

    size_t free_heap_before = esp_get_free_heap_size();
    size_t free_psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Memory before AAC decoder unregister: heap=%zu, PSRAM=%zu", free_heap_before, free_psram_before);

    ESP_LOGI(TAG, "Attempting to unregister AAC decoder...");
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_AAC);
    ESP_LOGI(TAG, "AAC decoder unregistered successfully");

    s_aac_decoder_registered = false;

    vTaskDelay(pdMS_TO_TICKS(50));

    size_t free_heap_after = esp_get_free_heap_size();
    size_t free_psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Memory after AAC decoder unregister: heap=%zu, PSRAM=%zu", free_heap_after, free_psram_after);
    ESP_LOGI(TAG, "Memory freed: heap=%zd, PSRAM=%zd",
             free_heap_after - free_heap_before,
             free_psram_after - free_psram_before);

    if (free_heap_after < free_heap_before) {
        ESP_LOGW(TAG, "Memory leak detected after AAC unregister: heap decreased by %zd bytes",
                 free_heap_before - free_heap_after);
    }

    return ESP_OK;
}

esp_err_t codec_deinit_aac_decoder(void) {
    ESP_LOGI(TAG, "Deinitializing AAC decoder...");

    size_t free_heap_before = esp_get_free_heap_size();
    size_t free_psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Memory before AAC deinit: heap=%zu, PSRAM=%zu", free_heap_before, free_psram_before);

    codec_aac_deinit_buffers();

    esp_err_t ret = codec_aac_unregister_decoder();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to unregister AAC decoder during deinit: %s", esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    size_t free_heap_after = esp_get_free_heap_size();
    size_t free_psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Memory after complete AAC deinit: heap=%zu, PSRAM=%zu", free_heap_after, free_psram_after);
    ESP_LOGI(TAG, "Total memory freed: heap=%zd, PSRAM=%zd",
             free_heap_after - free_heap_before,
             free_psram_after - free_psram_before);

    ESP_LOGI(TAG, "AAC decoder deinitialization completed");
    return ESP_OK;
}
