#pragma once
#include "esp_err.h"

esp_err_t display_init(void);
void display_render_strobe(float detected_hz, const char *note);
