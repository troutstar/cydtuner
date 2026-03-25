#pragma once
#include "esp_err.h"

typedef struct {
    float threshold_coeff;
    float pitch_min_hz;
    float pitch_max_hz;
    float smooth_alpha;
} tuner_params_t;

esp_err_t test_harness_init(void);
void      test_harness_get_params(tuner_params_t *out);
void      test_harness_set_params(const tuner_params_t *in);
