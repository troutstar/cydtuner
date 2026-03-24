#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t xpt2046_init(void);
bool xpt2046_read(int *x, int *y, int *pressure);
