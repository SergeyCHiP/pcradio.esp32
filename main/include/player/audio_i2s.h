#ifndef AUDIO_I2S_H
#define AUDIO_I2S_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

#define AUDIO_I2S_WRITE_TIMEOUT_MS 1000
#include "esp_ae_eq.h"
#include "alc.h"

// Пины I2S для CJMCU-1334
// Wroover
// #define I2S_BCK_PIN     26
// #define I2S_LRCK_PIN    25
// #define I2S_DATA_PIN    22
// #define I2S_MUTE_PIN    21

// Esp32s3 N16R8
#define I2S_BCK_PIN     12   // I2S_CLK (BCK)
#define I2S_LRCK_PIN    11   // I2S_WS (LRCK)
#define I2S_DATA_PIN    10   // I2S_DOUT
#define I2S_MUTE_PIN    9    // mute

esp_err_t audio_i2s_init(uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels);
esp_err_t audio_i2s_set_volume(uint8_t volume);
esp_err_t audio_i2s_write(const void *data, size_t size, size_t *bytes_written, TickType_t wait_time);
esp_err_t audio_i2s_set_expander_gate(bool enable, float expander_threshold,float expander_ratio, float gate_threshold);
esp_err_t audio_i2s_set_gate_advanced(float floor_attenuation, float attack_ms, float release_ms, float hold_ms);
esp_err_t audio_i2s_set_eq_config(esp_ae_eq_cfg_t *eq_cfg);
esp_err_t audio_i2s_set_alc_config(alc_config_t *alc_cfg);
esp_err_t audio_i2s_enable_alc(bool enable);
esp_err_t audio_i2s_set_alc_gain_db(int channel_index, int gain_db);
esp_err_t audio_i2s_set_mute(bool mute);
esp_err_t audio_i2s_deinit(void);
uint32_t audio_i2s_get_sample_rate(void);
uint8_t audio_i2s_get_bits_per_sample(void);
uint8_t audio_i2s_get_channels(void);
#endif
