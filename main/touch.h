#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t touch_init(void);
bool touch_read(int *x, int *y);
