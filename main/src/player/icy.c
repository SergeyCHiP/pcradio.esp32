#include "icy.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG_ICY = "ICY";
#define ICY_READ_TIMEOUT_MS 15000

static int icy_read_with_stop(esp_http_client_handle_t client, char *buffer, int length,
                              SemaphoreHandle_t stop_sem) {
    TickType_t last_data_time = xTaskGetTickCount();

    while (true) {
        if (stop_sem && xSemaphoreTake(stop_sem, 0) == pdTRUE) {
            return -ESP_ERR_INVALID_STATE;
        }

        int read_len = esp_http_client_read(client, buffer, length);
        if (read_len != -ESP_ERR_HTTP_EAGAIN) {
            return read_len;
        }

        if ((xTaskGetTickCount() - last_data_time) > pdMS_TO_TICKS(ICY_READ_TIMEOUT_MS)) {
            return -ESP_ERR_TIMEOUT;
        }
    }
}

void icy_state_init(icy_state_t *state) {
    if (!state) return;
    state->metaint_interval = 0;
    state->bytes_until_meta = 0;
    state->icy_name = NULL;
    state->icy_genre = NULL;
    state->icy_url = NULL;
    state->meta_buffer = NULL;
    state->buffer_capacity = 0;
}

void icy_state_cleanup(icy_state_t *state) {
    if (!state) return;
    if (state->meta_buffer) heap_caps_free(state->meta_buffer);
    state->meta_buffer = NULL;
    state->buffer_capacity = 0;
    free(state->icy_name);
    state->icy_name = NULL;
    free(state->icy_genre);
    state->icy_genre = NULL;
    free(state->icy_url);
    state->icy_url = NULL;
    state->metaint_interval = 0;
    state->bytes_until_meta = 0;
}

void icy_state_reset(icy_state_t *state) {
    if (!state) return;
    state->bytes_until_meta = state->metaint_interval;
    free(state->icy_name); state->icy_name = NULL;
    free(state->icy_genre); state->icy_genre = NULL;
    free(state->icy_url); state->icy_url = NULL;
}

void icy_handle_header(icy_state_t *state, const char *key, const char *value) {
    if (!state || !key || !value) return;
    if (strcasecmp(key, ICY_METAINT_RESPONSE_HEADER) == 0) {
        state->metaint_interval = atoi(value);
        state->bytes_until_meta = state->metaint_interval;
        ESP_LOGI(TAG_ICY, "ICY: metaint set to %d", state->metaint_interval);
    } else if (strcasecmp(key, "icy-name") == 0) {
        free(state->icy_name);
        state->icy_name = strdup(value);
    } else if (strcasecmp(key, "icy-genre") == 0) {
        free(state->icy_genre);
        state->icy_genre = strdup(value);
    } else if (strcasecmp(key, "icy-url") == 0) {
        free(state->icy_url);
        state->icy_url = strdup(value);
    }
}

void icy_allocate_buffer(icy_state_t *state) {
    if (!state || state->meta_buffer) return;
    state->buffer_capacity = ICY_MAX_META_SIZE + 1;
    state->meta_buffer = heap_caps_malloc(state->buffer_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (state->meta_buffer) {
        ESP_LOGI(TAG_ICY, "ICY: allocated meta buffer of %d bytes", state->buffer_capacity);
    } else {
        ESP_LOGE(TAG_ICY, "ICY: failed to allocate meta buffer");
        state->metaint_interval = 0;
    }
}

esp_err_t icy_process_metadata(icy_state_t *state, esp_http_client_handle_t client, SemaphoreHandle_t stop_sem, bool *stream_end) {
    if (!state || !client) return ESP_FAIL;
    if (state->metaint_interval <= 0) return ESP_OK;
    if (!state->meta_buffer) return ESP_FAIL;
    uint8_t len_byte = 0;
    int r = icy_read_with_stop(client, (char*)&len_byte, 1, stop_sem);
    if (r == -ESP_ERR_INVALID_STATE) { *stream_end = true; return ESP_ERR_INVALID_STATE; }
    if (r <= 0) { *stream_end = true; return (r < 0) ? ESP_FAIL : ESP_OK; }
    int meta_len = len_byte * 16;
    if (meta_len > state->buffer_capacity - 1) meta_len = state->buffer_capacity - 1;
    int total = 0;
    while (total < meta_len) {
        r = icy_read_with_stop(client, (char *)state->meta_buffer + total, meta_len - total, stop_sem);
        if (r == -ESP_ERR_INVALID_STATE) { *stream_end = true; return ESP_ERR_INVALID_STATE; }
        if (r <= 0) { *stream_end = true; break; }
        total += r;
    }
    state->meta_buffer[total] = '\0';
    char *p = strstr((char *)state->meta_buffer, "StreamTitle='");
    if (p) {
        p += strlen("StreamTitle='");
        char *end = strchr(p, '\'');
        if (end) *end = '\0';
        free(state->icy_name);
        state->icy_name = strdup(p);
        ESP_LOGI(TAG_ICY, "ICY: StreamTitle='%s'", state->icy_name);
    }
    state->bytes_until_meta = state->metaint_interval;
    return ESP_OK;
}

const char* icy_get_name(const icy_state_t *state) {
    return state && state->icy_name ? state->icy_name : "";
}
