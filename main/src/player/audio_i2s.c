#include "audio_i2s.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_ae_eq.h"
#include "alc.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "volume.h"
#include "config.h"

// Отключаем неиспользуемые функции для экономии IRAM
#define ENABLE_AUDIO_FILTERING 1
#define ENABLE_DITHERING 1
#define ENABLE_EXPANDER_GATE 1
#define ENABLE_HIGH_FREQ_NOISE_REDUCTION 1

// Параметры фильтрации для улучшения качества звука
#define FILTER_STRENGTH 0.1f                                      // Сила фильтра (0.0-1.0), чем выше, тем более сглаженный звук

// Настройки деквантизации (dithering)
#define DITHER_TYPE_TRIANGULAR 0                                  // Тип деквантизации (1 - треугольная, 0 - равномерная)
#define DITHER_AMPLITUDE 0.5f                                     // Амплитуда шума деквантизации (в единицах LSB)

// Настройки экспандера/гейта (оптимизировано для 128 кбит/с)
#define EXPANDER_THRESHOLD 1000.0f                                // Порог экспандера: ниже для активности на шумах 128kbps
#define EXPANDER_RATIO 1.5f                                       // Соотношение экспандера: мягкое для естественности
#define GATE_THRESHOLD 400.0f                                     // Порог гейта: ниже для более полного заглушения тишины
#define GATE_FLOOR_ATTENUATION 0.05f                              // Остаточный уровень сигнала при закрытом гейте (0-1)
#define ATTACK_TIME_MS 2.0f                                       // Оптимизировано: более быстрая атака
#define RELEASE_TIME_MS 150.0f                                    // Оптимизировано: более плавное закрытие
#define HOLD_TIME_MS 50.0f                                        // Время удержания гейта открытым после падения сигнала ниже порога

// Настройки для шумоподавления высоких частот
#define HIGH_FREQ_NOISE_THRESHOLD 300.0f                          // Порог для шумоподавления высоких частот
#define HIGH_FREQ_NOISE_REDUCTION_RATIO 0.4f                      // Коэффициент подавления для высоких частот 0.8f

// Оптимизируем DMA буферы для экономии DIRAM
#define DMA_BUF_COUNT   28
#define DMA_BUF_LEN     1024

// Уменьшаем размер буферов для экономии DIRAM
#define NOISE_FILTER_BUFFER_SIZE 128

// Максимальный размер буфера для использования stack вместо heap
#define MAX_STACK_BUFFER_SIZE 512

// Буферы в PSRAM (если доступна) или динамические
static int16_t *s_noise_filter_left_buffer = NULL;
static int16_t *s_noise_filter_right_buffer = NULL;
static size_t s_noise_filter_index = 0;

static const char *TAG = "I2S";

#define I2S_NUM         I2S_NUM_0

static bool s_i2s_initialized = false;
static i2s_chan_handle_t i2s_handle = NULL;
static bool s_unmute_on_first_write = false; // Флаг для отложенного включения звука
static TimerHandle_t s_unmute_timer = NULL;  // Таймер для отложенного включения звука

// Статические переменные для EQ
static esp_ae_eq_handle_t s_eq_handle = NULL;
static esp_ae_eq_cfg_t *s_current_eq_cfg = NULL;
static SemaphoreHandle_t s_eq_mutex = NULL; // Мьютекс для защиты доступа к EQ

// Статические переменные для ALC
static bool s_alc_enabled = false;
static alc_config_t *s_current_alc_cfg = NULL;
static SemaphoreHandle_t s_alc_mutex = NULL; // Мьютекс для защиты доступа к ALC

// Статические переменные для хранения текущей конфигурации I2S и уровня громкости
static float s_current_volume;                                    // Текущая логарифмическая громкость (0.0-1.0)
static uint8_t s_current_volume_percent = 20;                     // Громкость в процентах для сохранения
static uint32_t s_current_sample_rate = 0;
static uint8_t s_current_bits_per_sample = 0;
static uint8_t s_current_i2s_channels = 0; // Количество каналов, с которым инициализирован I2S

// Буферы для хранения предыдущих значений при фильтрации
static int16_t s_prev_left_sample = 0;
static int16_t s_prev_right_sample = 0;

// Состояние экспандера/гейта
static float s_expander_envelope_left = 0.0f;                     // Огибающая сигнала левого канала
static float s_expander_envelope_right = 0.0f;                    // Огибающая сигнала правого канала
static bool s_expander_gate_enabled = ENABLE_EXPANDER_GATE;       // Активация экспандера/гейта
static float s_expander_threshold = EXPANDER_THRESHOLD;           // Текущий порог экспандера
static float s_expander_ratio = EXPANDER_RATIO;                   // Текущее соотношение экспандера
static float s_gate_threshold = GATE_THRESHOLD;                   // Текущий порог гейта
static float s_gate_floor_attenuation = GATE_FLOOR_ATTENUATION;   // Затухание закрытого гейта
static uint32_t s_left_hold_counter = 0;                          // Счетчик удержания для левого канала
static uint32_t s_right_hold_counter = 0;                         // Счетчик удержания для правого канала

static void unmute_timer_callback(TimerHandle_t xTimer) {
    if (g_app_config && g_app_config->is_loaded && !g_app_config->player.mute) {
        ESP_LOGI(TAG, "Unmute timer expired, unmuting audio as per configuration.");
        gpio_set_level(I2S_MUTE_PIN, 0);
    } else {
        ESP_LOGI(TAG, "Unmute timer expired, audio remains muted as per configuration.");
    }
}

static esp_err_t ensure_mutexes_created(void) {
    if (s_eq_mutex == NULL) {
        s_eq_mutex = xSemaphoreCreateMutex();
        if (s_eq_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create EQ mutex");
            return ESP_FAIL;
        }
    }
    if (s_alc_mutex == NULL) {
        s_alc_mutex = xSemaphoreCreateMutex();
        if (s_alc_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create ALC mutex");
            if (s_eq_mutex) {
                vSemaphoreDelete(s_eq_mutex);
                s_eq_mutex = NULL;
            }
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static void apply_stored_audio_settings(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    ESP_LOGI(TAG, "Re-applying stored audio settings...");

    if (s_eq_mutex && xSemaphoreTake(s_eq_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_current_eq_cfg) {
            ESP_LOGI(TAG, "Re-initializing EQ with new stream parameters.");
            if (s_eq_handle) {
                esp_ae_eq_close(s_eq_handle);
                s_eq_handle = NULL;
            }
            s_current_eq_cfg->sample_rate = sample_rate;
            s_current_eq_cfg->channel = channels;
            s_current_eq_cfg->bits_per_sample = bits_per_sample;

            esp_err_t ret = esp_ae_eq_open(s_current_eq_cfg, &s_eq_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to re-open EQ instance: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "EQ re-initialized successfully.");
            }
        }
        xSemaphoreGive(s_eq_mutex);
    }

    if (s_alc_mutex && xSemaphoreTake(s_alc_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_current_alc_cfg) {
            ESP_LOGI(TAG, "Re-initializing ALC with new stream parameters.");
            alc_deinit();

            s_current_alc_cfg->sample_rate = sample_rate;
            s_current_alc_cfg->channels = channels;
            s_current_alc_cfg->bits_per_sample = bits_per_sample;

            esp_err_t ret = alc_init(s_current_alc_cfg);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to re-initialize ALC instance: %s", esp_err_to_name(ret));
                s_alc_enabled = false;
            } else {
                ESP_LOGI(TAG, "ALC re-initialized successfully. Enabled state: %d", s_alc_enabled);
            }
        }
        xSemaphoreGive(s_alc_mutex);
    }

    audio_i2s_set_volume(s_current_volume_percent);
}


esp_err_t audio_i2s_init(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    if (s_i2s_initialized) {
        ESP_LOGI(TAG, "Re-initializing I2S driver...");
        audio_i2s_deinit();
    }

    if (ensure_mutexes_created() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return ESP_FAIL;
    }

    if (s_unmute_timer == NULL) {
        s_unmute_timer = xTimerCreate("unmute_timer",
                                      pdMS_TO_TICKS(2000),
                                      pdFALSE,
                                      (void *)0,
                                      unmute_timer_callback);
        if (s_unmute_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create unmute timer");
        }
    }

    size_t free_heap = esp_get_free_heap_size();
    size_t free_iram = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    ESP_LOGI(TAG, "Free memory before I2S init: heap=%zu, IRAM=%zu", free_heap, free_iram);

    if (free_iram < 8192) {
        ESP_LOGE(TAG, "Insufficient IRAM memory: %zu bytes (need at least 8192)", free_iram);
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = DMA_BUF_COUNT,
        .dma_frame_num = DMA_BUF_LEN,
        .auto_clear = true,
    };

    esp_err_t ret = i2s_new_channel(&chan_cfg, &i2s_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_XTAL,
            .mclk_multiple = I2S_MCLK_MULTIPLE_384,
        },
        .slot_cfg = {
            .data_bit_width = bits_per_sample,
            .slot_bit_width = bits_per_sample,
            .slot_mode = channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = bits_per_sample,
            .ws_pol = false,
            .bit_shift = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_LRCK_PIN,
            .dout = I2S_DATA_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(i2s_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S in standard mode: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_handle);
        i2s_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(i2s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_handle);
        return ret;
    }

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << I2S_MUTE_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0
    };

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config MUTE pin: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_handle);
        i2s_handle = NULL;
        return ret;
    }

    ret = gpio_set_level(I2S_MUTE_PIN, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set MUTE pin to mute: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_handle);
        i2s_handle = NULL;
        return ret;
    }

    s_unmute_on_first_write = true;
    ESP_LOGI(TAG, "I2S initialized, audio is muted. Will unmute on first data write.");

    s_current_sample_rate = sample_rate;
    s_current_bits_per_sample = bits_per_sample;
    s_current_i2s_channels = channels;

    s_i2s_initialized = true;
    ESP_LOGI(TAG, "I2S initialized: sample_rate=%" PRIu32 ", bits_per_sample=%u, channels=%u",
             sample_rate, bits_per_sample, channels);

    s_prev_left_sample = 0;
    s_prev_right_sample = 0;

    if (s_noise_filter_left_buffer) {
        heap_caps_free(s_noise_filter_left_buffer);
        s_noise_filter_left_buffer = NULL;
    }
    if (s_noise_filter_right_buffer) {
        heap_caps_free(s_noise_filter_right_buffer);
        s_noise_filter_right_buffer = NULL;
    }

    #if ENABLE_HIGH_FREQ_NOISE_REDUCTION
    s_noise_filter_left_buffer = heap_caps_malloc(NOISE_FILTER_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_noise_filter_left_buffer) {
        s_noise_filter_left_buffer = malloc(NOISE_FILTER_BUFFER_SIZE * sizeof(int16_t));
        if (!s_noise_filter_left_buffer) {
            ESP_LOGE(TAG, "Failed to allocate noise filter left buffer");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "Noise filter left buffer allocated in DIRAM (PSRAM not available)");
    } else {
        ESP_LOGI(TAG, "Noise filter left buffer allocated in PSRAM");
    }

    s_noise_filter_right_buffer = heap_caps_malloc(NOISE_FILTER_BUFFER_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_noise_filter_right_buffer) {
        s_noise_filter_right_buffer = malloc(NOISE_FILTER_BUFFER_SIZE * sizeof(int16_t));
        if (!s_noise_filter_right_buffer) {
            ESP_LOGE(TAG, "Failed to allocate noise filter right buffer");
            free(s_noise_filter_left_buffer);
            s_noise_filter_left_buffer = NULL;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "Noise filter right buffer allocated in DIRAM (PSRAM not available)");
    } else {
        ESP_LOGI(TAG, "Noise filter right buffer allocated in PSRAM");
    }

    memset(s_noise_filter_left_buffer, 0, NOISE_FILTER_BUFFER_SIZE * sizeof(int16_t));
    memset(s_noise_filter_right_buffer, 0, NOISE_FILTER_BUFFER_SIZE * sizeof(int16_t));
    s_noise_filter_index = 0;
    #else
    ESP_LOGI(TAG, "Noise filter buffers not allocated (feature disabled)");
    #endif

    s_expander_envelope_left = 0.0f;
    s_expander_envelope_right = 0.0f;
    s_left_hold_counter = 0;
    s_right_hold_counter = 0;

    apply_stored_audio_settings(sample_rate, bits_per_sample, channels);

    free_heap = esp_get_free_heap_size();
    free_iram = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    ESP_LOGI(TAG, "Free memory after I2S init: heap=%zu, IRAM=%zu", free_heap, free_iram);

    return ESP_OK;
}

esp_err_t audio_i2s_set_volume(uint8_t volume_percent) {
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    s_current_volume_percent = volume_percent;

    s_current_volume = powerCurveVolumeControl(volume_percent);

    ESP_LOGD(TAG, "Volume percent set to %d, resulting logarithmic volume: %.3f", volume_percent, s_current_volume);
    return ESP_OK;
}

esp_err_t audio_i2s_set_expander_gate(bool enable, float expander_threshold, float expander_ratio, float gate_threshold) {
    if (expander_ratio < 1.0f || expander_threshold < 0.0f || gate_threshold < 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    s_expander_gate_enabled = enable;
    s_expander_threshold = expander_threshold;
    s_expander_ratio = expander_ratio;
    s_gate_threshold = gate_threshold;

    ESP_LOGI(TAG, "Expander/Gate: %s, threshold=%.1f, ratio=%.1f, gate=%.1f",
             enable ? "enabled" : "disabled", expander_threshold, expander_ratio, gate_threshold);

    return ESP_OK;
}

esp_err_t audio_i2s_set_gate_advanced(float floor_attenuation, float attack_ms, float release_ms, float hold_ms) {
    if (floor_attenuation < 0.0f || floor_attenuation > 1.0f ||
        attack_ms < 0.1f || release_ms < 0.1f || hold_ms < 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gate_floor_attenuation = floor_attenuation;

    ESP_LOGI(TAG, "Gate advanced: floor=%.3f, attack=%.1fms, release=%.1fms, hold=%.1fms",
             floor_attenuation, attack_ms, release_ms, hold_ms);

    return ESP_OK;
}

esp_err_t audio_i2s_set_eq_config(esp_ae_eq_cfg_t *new_config) {
    if (s_eq_mutex == NULL) {
        ESP_LOGE(TAG, "EQ mutex not initialized in set_eq_config");
        if (new_config) {
            if (new_config->para) free(new_config->para);
            free(new_config);
        }
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_eq_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take EQ mutex in set_eq_config");
        if (new_config) {
            if (new_config->para) free(new_config->para);
            free(new_config);
        }
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Setting new EQ configuration.");

    if (s_eq_handle) {
        ESP_LOGI(TAG, "Closing existing EQ instance.");
        esp_ae_eq_close(s_eq_handle);
        s_eq_handle = NULL;
    }

    if (s_current_eq_cfg) {
        ESP_LOGI(TAG, "Freeing previous EQ configuration.");
        if (s_current_eq_cfg->para) {
            free(s_current_eq_cfg->para);
        }
        free(s_current_eq_cfg);
        s_current_eq_cfg = NULL;
    }

    if (new_config == NULL) {
        ESP_LOGI(TAG, "EQ disabled (new configuration is NULL).");
        xSemaphoreGive(s_eq_mutex);
        return ESP_OK;
    }

    s_current_eq_cfg = new_config;
    ESP_LOGI(TAG, "Stored new EQ configuration: SR=%d, Ch=%d, Bits=%d, Filters=%d",
             (int)s_current_eq_cfg->sample_rate,
             (int)s_current_eq_cfg->channel,
             (int)s_current_eq_cfg->bits_per_sample,
             (int)s_current_eq_cfg->filter_num);


    esp_err_t ret = esp_ae_eq_open(s_current_eq_cfg, &s_eq_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open EQ instance: %s", esp_err_to_name(ret));
        if (s_current_eq_cfg->para) {
            free(s_current_eq_cfg->para);
        }
        free(s_current_eq_cfg);
        s_current_eq_cfg = NULL;
        s_eq_handle = NULL;
        xSemaphoreGive(s_eq_mutex);
        return ret;
    }

    ESP_LOGI(TAG, "EQ instance opened successfully.");

    uint32_t nyquist_freq = s_current_eq_cfg->sample_rate / 2;
    for (int i = 0; i < s_current_eq_cfg->filter_num; i++) {
        if (s_current_eq_cfg->para[i].fc >= nyquist_freq) {
            ESP_LOGW(TAG, "Disabling EQ filter %d (fc=%d Hz) as it exceeds Nyquist frequency (%d Hz)",
                     i, (int)s_current_eq_cfg->para[i].fc, (int)nyquist_freq);
            esp_err_t disable_ret = esp_ae_eq_disable_filter(s_eq_handle, i);
            if (disable_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to disable EQ filter %d: %s", i, esp_err_to_name(disable_ret));
            }
        }
    }

    xSemaphoreGive(s_eq_mutex);
    return ESP_OK;
}

esp_err_t audio_i2s_set_alc_config(alc_config_t *new_alc_cfg) {
    if (s_alc_mutex == NULL) {
        ESP_LOGE(TAG, "ALC mutex not initialized in set_alc_config");
        if (new_alc_cfg) {
            free(new_alc_cfg);
        }
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_alc_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take ALC mutex in set_alc_config");
        if (new_alc_cfg) {
            free(new_alc_cfg);
        }
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Setting new ALC configuration.");

    if (s_alc_enabled && s_current_alc_cfg) {
        ESP_LOGI(TAG, "Deinitializing existing ALC instance.");
        alc_deinit();
        s_alc_enabled = false;
    }

    if (s_current_alc_cfg) {
        ESP_LOGI(TAG, "Freeing previous ALC configuration.");
        free(s_current_alc_cfg);
        s_current_alc_cfg = NULL;
    }

    if (new_alc_cfg == NULL) {
        ESP_LOGI(TAG, "ALC disabled (new configuration is NULL).");
        s_alc_enabled = false;
        xSemaphoreGive(s_alc_mutex);
        return ESP_OK;
    }

    s_current_alc_cfg = new_alc_cfg;
    ESP_LOGI(TAG, "Stored new ALC configuration: SR=%d, Ch=%d, Bits=%d",
             s_current_alc_cfg->sample_rate,
             s_current_alc_cfg->channels,
             s_current_alc_cfg->bits_per_sample);

    esp_err_t ret = alc_init(s_current_alc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ALC instance: %s", esp_err_to_name(ret));
        free(s_current_alc_cfg);
        s_current_alc_cfg = NULL;
        s_alc_enabled = false;
        xSemaphoreGive(s_alc_mutex);
        return ret;
    }

    s_alc_enabled = true;
    ESP_LOGI(TAG, "ALC instance initialized successfully. ALC is now enabled.");
    xSemaphoreGive(s_alc_mutex);
    return ESP_OK;
}

esp_err_t audio_i2s_enable_alc(bool enable) {
    if (s_alc_mutex == NULL) {
        ESP_LOGE(TAG, "ALC mutex not initialized in enable_alc");
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_alc_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take ALC mutex in enable_alc");
        return ESP_ERR_TIMEOUT;
    }

    if (enable && s_current_alc_cfg == NULL) {
        ESP_LOGE(TAG, "Cannot enable ALC: not configured. Call audio_i2s_set_alc_config() first.");
        xSemaphoreGive(s_alc_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (enable && s_current_alc_cfg && !s_alc_enabled) {
        esp_err_t ret = alc_init(s_current_alc_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to re-initialize ALC on enable: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_alc_mutex);
            return ret;
        }
        s_alc_enabled = true;
        ESP_LOGI(TAG, "ALC re-initialized and enabled.");
    } else if (!enable && s_alc_enabled) {
        alc_deinit();
        s_alc_enabled = false;
        ESP_LOGI(TAG, "ALC processing disabled and deinitialized (config kept).");
    } else {
         ESP_LOGI(TAG, "ALC already %s.", s_alc_enabled ? "enabled" : "disabled or not configured");
    }


    xSemaphoreGive(s_alc_mutex);
    return ESP_OK;
}

esp_err_t audio_i2s_set_alc_gain_db(int channel_index, int gain_db) {
    if (s_alc_mutex == NULL) {
        ESP_LOGE(TAG, "ALC mutex not initialized in set_alc_gain_db");
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_alc_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take ALC mutex in set_alc_gain_db");
        return ESP_ERR_TIMEOUT;
    }

    if (!s_alc_enabled || !s_current_alc_cfg) {
        ESP_LOGE(TAG, "Cannot set ALC gain: ALC not enabled or configured.");
        xSemaphoreGive(s_alc_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = alc_set_gain_db(channel_index, gain_db);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set ALC gain: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "ALC gain set for channel %d to %d dB", channel_index, gain_db);
    }

    xSemaphoreGive(s_alc_mutex);
    return ret;
}


esp_err_t audio_i2s_write(const void *data, size_t size, size_t *bytes_written, TickType_t wait_time) {
    if (s_unmute_on_first_write) {
        s_unmute_on_first_write = false;
        ESP_LOGI(TAG, "First write detected, starting/resetting 2-second unmute timer.");
        if (s_unmute_timer != NULL) {
            if (xTimerReset(s_unmute_timer, 0) != pdPASS) {
                ESP_LOGE(TAG, "Failed to start/reset unmute timer. Audio will remain muted.");
            }
        } else {
            ESP_LOGE(TAG, "Unmute timer not created. Audio will remain muted.");
        }
    }

    if (!s_i2s_initialized || !i2s_handle) {
        ESP_LOGE(TAG, "I2S not initialized");
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }

    if (size == 0) {
        if (bytes_written) *bytes_written = 0;
        return ESP_OK;
    }

    esp_err_t ret;
    const void *data_to_write = data;
    void *heap_buffer = NULL;
    bool using_heap_buffer = false;
    uint8_t stack_buffer[MAX_STACK_BUFFER_SIZE];

    bool alc_conditions_met = s_alc_enabled && s_current_alc_cfg &&
                              s_current_bits_per_sample == s_current_alc_cfg->bits_per_sample &&
                              s_current_i2s_channels == s_current_alc_cfg->channels;

    bool volume_conditions_met = s_current_bits_per_sample == 16 &&
                                 (s_current_volume < 0.99f || s_current_volume > 1.01f || ENABLE_AUDIO_FILTERING);

    bool eq_conditions_met = s_eq_handle && s_current_eq_cfg &&
                             s_current_bits_per_sample == s_current_eq_cfg->bits_per_sample &&
                             s_current_i2s_channels == s_current_eq_cfg->channel;

    if (alc_conditions_met || volume_conditions_met || eq_conditions_met) {
        if (size <= MAX_STACK_BUFFER_SIZE) {
            memcpy(stack_buffer, data, size);
            data_to_write = stack_buffer;
            using_heap_buffer = false;

            if (alc_conditions_met) {
                if (s_alc_mutex && xSemaphoreTake(s_alc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    size_t num_frames_alc = size / (s_current_alc_cfg->bits_per_sample / 8) / s_current_alc_cfg->channels;
                    if (num_frames_alc > 0) {
                        esp_err_t alc_ret = alc_process(stack_buffer, stack_buffer, num_frames_alc);
                        if (alc_ret != ESP_OK) {
                            ESP_LOGW(TAG, "ALC process error: %s", esp_err_to_name(alc_ret));
                        }
                    }
                    xSemaphoreGive(s_alc_mutex);
                } else if (s_alc_mutex) {
                    ESP_LOGD(TAG, "ALC processing skipped: could not take ALC mutex in time.");
                }
            }

            if (volume_conditions_met) {
                int16_t *samples = (int16_t *)stack_buffer;
                size_t num_samples_vol = size / sizeof(int16_t);

                for (size_t i = 0; i < num_samples_vol; i++) {
                    float current_sample_val = (float)samples[i] * s_current_volume;
                    if (ENABLE_AUDIO_FILTERING) {
                        if (s_current_i2s_channels == 2) {
                            if (i % 2 == 0) {
                                current_sample_val = (1.0f - FILTER_STRENGTH) * current_sample_val +
                                                     FILTER_STRENGTH * (float)s_prev_left_sample;
                                s_prev_left_sample = (int16_t)current_sample_val;
                            } else {
                                current_sample_val = (1.0f - FILTER_STRENGTH) * current_sample_val +
                                                     FILTER_STRENGTH * (float)s_prev_right_sample;
                                s_prev_right_sample = (int16_t)current_sample_val;
                            }
                        } else {
                             current_sample_val = (1.0f - FILTER_STRENGTH) * current_sample_val +
                                                 FILTER_STRENGTH * (float)s_prev_left_sample;
                             s_prev_left_sample = (int16_t)current_sample_val;
                        }
                    }
                    if (current_sample_val > 32767.0f) current_sample_val = 32767.0f;
                    if (current_sample_val < -32768.0f) current_sample_val = -32768.0f;
                    samples[i] = (int16_t)current_sample_val;
                }
            } else if (s_current_bits_per_sample != 16 && (s_current_volume < 0.99f || s_current_volume > 1.01f)) {
                 ESP_LOGW(TAG, "Volume scaling not applied: unsupported bit depth %d (only 16-bit supported for volume/filter)",
                         s_current_bits_per_sample);
            }

            if (eq_conditions_met) {
                if (s_eq_mutex && xSemaphoreTake(s_eq_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    size_t num_frames_eq = size / (s_current_eq_cfg->bits_per_sample / 8) / s_current_eq_cfg->channel;
                    if (num_frames_eq > 0) {
                        esp_err_t eq_ret = esp_ae_eq_process(s_eq_handle, num_frames_eq, stack_buffer, stack_buffer);
                        if (eq_ret != ESP_OK) {
                            ESP_LOGW(TAG, "EQ process error: %s", esp_err_to_name(eq_ret));
                        }
                    }
                    xSemaphoreGive(s_eq_mutex);
                } else if (s_eq_mutex) {
                    ESP_LOGD(TAG, "EQ processing skipped: could not take EQ mutex in time.");
                }
            }

        } else {
            heap_buffer = malloc(size);
            if (!heap_buffer) {
                ESP_LOGE(TAG, "Failed to allocate temporary buffer for audio processing");
                if (bytes_written) *bytes_written = 0;
                return i2s_channel_write(i2s_handle, data, size, bytes_written, wait_time);
            }
            memcpy(heap_buffer, data, size);
            data_to_write = heap_buffer;
            using_heap_buffer = true;

            if (alc_conditions_met) {
                if (s_alc_mutex && xSemaphoreTake(s_alc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    size_t num_frames_alc = size / (s_current_alc_cfg->bits_per_sample / 8) / s_current_alc_cfg->channels;
                    if (num_frames_alc > 0) {
                        esp_err_t alc_ret = alc_process(heap_buffer, heap_buffer, num_frames_alc);
                        if (alc_ret != ESP_OK) {
                            ESP_LOGW(TAG, "ALC process error: %s", esp_err_to_name(alc_ret));
                        }
                    }
                    xSemaphoreGive(s_alc_mutex);
                } else if (s_alc_mutex) {
                    ESP_LOGD(TAG, "ALC processing skipped: could not take ALC mutex in time.");
                }
            }

            if (volume_conditions_met) {
                int16_t *samples = (int16_t *)heap_buffer;
                size_t num_samples_vol = size / sizeof(int16_t);

                for (size_t i = 0; i < num_samples_vol; i++) {
                    float current_sample_val = (float)samples[i] * s_current_volume;
                    if (ENABLE_AUDIO_FILTERING) {
                        if (s_current_i2s_channels == 2) {
                            if (i % 2 == 0) {
                                current_sample_val = (1.0f - FILTER_STRENGTH) * current_sample_val +
                                                     FILTER_STRENGTH * (float)s_prev_left_sample;
                                s_prev_left_sample = (int16_t)current_sample_val;
                            } else {
                                current_sample_val = (1.0f - FILTER_STRENGTH) * current_sample_val +
                                                     FILTER_STRENGTH * (float)s_prev_right_sample;
                                s_prev_right_sample = (int16_t)current_sample_val;
                            }
                        } else {
                             current_sample_val = (1.0f - FILTER_STRENGTH) * current_sample_val +
                                                 FILTER_STRENGTH * (float)s_prev_left_sample;
                             s_prev_left_sample = (int16_t)current_sample_val;
                        }
                    }
                    if (current_sample_val > 32767.0f) current_sample_val = 32767.0f;
                    if (current_sample_val < -32768.0f) current_sample_val = -32768.0f;
                    samples[i] = (int16_t)current_sample_val;
                }
            } else if (s_current_bits_per_sample != 16 && (s_current_volume < 0.99f || s_current_volume > 1.01f)) {
                 ESP_LOGW(TAG, "Volume scaling not applied: unsupported bit depth %d (only 16-bit supported for volume/filter)",
                         s_current_bits_per_sample);
            }

            if (eq_conditions_met) {
                if (s_eq_mutex && xSemaphoreTake(s_eq_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    size_t num_frames_eq = size / (s_current_eq_cfg->bits_per_sample / 8) / s_current_eq_cfg->channel;
                    if (num_frames_eq > 0) {
                        esp_err_t eq_ret = esp_ae_eq_process(s_eq_handle, num_frames_eq, heap_buffer, heap_buffer);
                        if (eq_ret != ESP_OK) {
                            ESP_LOGW(TAG, "EQ process error: %s", esp_err_to_name(eq_ret));
                        }
                    }
                    xSemaphoreGive(s_eq_mutex);
                } else if (s_eq_mutex) {
                    ESP_LOGD(TAG, "EQ processing skipped: could not take EQ mutex in time.");
                }
            }
        }
    }

    ret = i2s_channel_write(i2s_handle, data_to_write, size, bytes_written, wait_time);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S write error: %s (written %zu of %zu bytes)",
                 esp_err_to_name(ret), bytes_written ? *bytes_written : 0, size);
    }

    if (using_heap_buffer && heap_buffer) {
        free(heap_buffer);
    }

    return ret;
}

esp_err_t audio_i2s_set_mute(bool mute) {
    if (!s_i2s_initialized) {
        ESP_LOGE(TAG, "I2S not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_unmute_timer != NULL && xTimerIsTimerActive(s_unmute_timer)) {
        xTimerStop(s_unmute_timer, 0);
        ESP_LOGI(TAG, "Unmute timer stopped due to manual mute/unmute control.");
    }

    if (mute) {
        s_unmute_on_first_write = false;
    }

    esp_err_t ret = gpio_set_level(I2S_MUTE_PIN, mute ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set MUTE pin level: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Audio %s", mute ? "muted" : "unmuted");
    return ESP_OK;
}

esp_err_t audio_i2s_deinit(void) {
    ESP_LOGI(TAG, "De-initializing I2S driver...");
    if (!s_i2s_initialized) {
        ESP_LOGW(TAG, "I2S already de-initialized.");
        return ESP_OK;
    }

    if (s_unmute_timer != NULL) {
        xTimerStop(s_unmute_timer, portMAX_DELAY);
        xTimerDelete(s_unmute_timer, portMAX_DELAY);
        s_unmute_timer = NULL;
        ESP_LOGI(TAG, "Unmute timer stopped and deleted.");
    }

    gpio_set_level(I2S_MUTE_PIN, 1);
    ESP_LOGI(TAG, "Audio muted for re-initialization.");

    s_unmute_on_first_write = false;

    if (i2s_handle) {
        i2s_channel_disable(i2s_handle);
        i2s_del_channel(i2s_handle);
        i2s_handle = NULL;
    }

    if (s_eq_handle) {
        esp_ae_eq_close(s_eq_handle);
        s_eq_handle = NULL;
    }
    alc_deinit();

    if (s_noise_filter_left_buffer) {
        heap_caps_free(s_noise_filter_left_buffer);
        s_noise_filter_left_buffer = NULL;
        ESP_LOGI(TAG, "Noise filter left buffer freed");
    }
    if (s_noise_filter_right_buffer) {
        heap_caps_free(s_noise_filter_right_buffer);
        s_noise_filter_right_buffer = NULL;
        ESP_LOGI(TAG, "Noise filter right buffer freed");
    }

    s_i2s_initialized = false;
    ESP_LOGI(TAG, "I2S deinitialized.");
    return ESP_OK;
}

uint32_t audio_i2s_get_sample_rate(void) {
    return s_current_sample_rate;
}

uint8_t audio_i2s_get_bits_per_sample(void) {
    return s_current_bits_per_sample;
}

uint8_t audio_i2s_get_channels(void) {
    return s_current_i2s_channels;
}
