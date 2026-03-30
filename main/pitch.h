#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t pitch_init(size_t buf_len);
float pitch_detect(const int16_t *buf, size_t len, float sample_rate);
void  pitch_hz_to_note(float hz, char *buf, size_t len);
float pitch_hz_to_nearest_hz(float hz);
float pitch_hz_to_cents(float hz);
void  pitch_set_a4(float hz);
float pitch_get_a4(void);

#ifdef PITCH_TEST_HARNESS
#include "test_harness.h"
float pitch_detect_full(const int16_t *buf, size_t len, float sample_rate,
                        const tuner_params_t *params, pitch_frame_t *frame);
#endif
