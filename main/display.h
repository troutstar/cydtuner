#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t display_init(void);
void      display_render_strobe(float detected_hz, const char *note);
void      display_set_scope(const int16_t *buf, int len);
void      display_next_mode(void);
