#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t pitch_init(size_t buf_len);
float pitch_detect(const int16_t *buf, size_t len, float sample_rate);
void pitch_hz_to_note(float hz, char *buf, size_t len);
float pitch_hz_to_nearest_hz(float hz);
float pitch_hz_to_cents(float hz);
