#pragma once
#include "esp_err.h"

esp_err_t display_init(void);
void display_render_strobe(float detected_hz, const char *note);
void display_set_a4(float hz);   /* update A4 reference shown in header strip */
