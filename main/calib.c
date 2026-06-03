#include "calib.h"
#include "pitch.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "calib";

#define NVS_NS      "tuner"
#define NVS_KEY     "a4_hz"
#define A4_DEFAULT  440.0f
#define A4_MIN      400.0f
#define A4_MAX      480.0f

static float s_a4_hz = A4_DEFAULT;

esp_err_t calib_init(void)
{
    nvs_flash_init();   /* required for NVS partition; ignore errors */
    s_a4_hz = A4_DEFAULT;
    pitch_set_a4(s_a4_hz);
    ESP_LOGI(TAG, "A4 = %.1f Hz (hardcoded)", (double)s_a4_hz);
    return ESP_OK;
}

float calib_get_a4(void) { return s_a4_hz; }

void calib_set_a4(float hz)
{
    if (hz < A4_MIN || hz > A4_MAX) return;
    s_a4_hz = hz;
    pitch_set_a4(hz);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint32_t raw;
        memcpy(&raw, &hz, 4);
        nvs_set_u32(h, NVS_KEY, raw);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "A4 saved: %.1f Hz", (double)hz);
    }
}
