#pragma once
#include "esp_err.h"

/* NVS-backed tuner calibration.
 * Currently stores: A4 reference frequency (Hz).
 * Must call calib_init() before using other functions.
 */

esp_err_t calib_init(void);
float     calib_get_a4(void);
void      calib_set_a4(float hz);   /* clamps to 400–480 Hz, saves to NVS */
