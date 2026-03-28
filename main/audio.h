#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

typedef enum {
    AUDIO_SOURCE_WAV_FILE,
    AUDIO_SOURCE_I2S,
#ifdef PITCH_TEST_HARNESS
    AUDIO_SOURCE_SYNTH,
#endif
} audio_source_t;

esp_err_t audio_init(audio_source_t source);
int audio_read(int16_t *buf, size_t len);
uint32_t audio_get_sample_rate(void);

#ifdef PITCH_TEST_HARNESS
float audio_get_position_sec(void);
void           audio_synth_set_hz(float hz);
float          audio_synth_get_hz(void);
void           audio_set_source(audio_source_t src);
audio_source_t audio_get_source(void);
#endif
