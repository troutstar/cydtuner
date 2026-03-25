#include "test_harness.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char        *TAG = "test_harness";
static tuner_params_t     s_params;
static SemaphoreHandle_t  s_params_mutex;

esp_err_t test_harness_init(void)
{
    s_params = (tuner_params_t){
        .threshold_coeff = 0.8f,
        .pitch_min_hz    = 40.0f,
        .pitch_max_hz    = 1200.0f,
        .smooth_alpha    = 0.0f,
    };
    s_params_mutex = xSemaphoreCreateMutex();
    if (!s_params_mutex) {
        ESP_LOGE(TAG, "params mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void test_harness_get_params(tuner_params_t *out)
{
    xSemaphoreTake(s_params_mutex, portMAX_DELAY);
    *out = s_params;
    xSemaphoreGive(s_params_mutex);
}

void test_harness_set_params(const tuner_params_t *in)
{
    xSemaphoreTake(s_params_mutex, portMAX_DELAY);
    s_params = *in;
    xSemaphoreGive(s_params_mutex);
}
