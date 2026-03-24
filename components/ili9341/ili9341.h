#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t ili9341_init(void);
esp_err_t ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
esp_err_t ili9341_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
esp_err_t ili9341_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels);
