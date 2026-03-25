#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "wifi.h"
#include "httpd.h"
#include "test_harness.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "test harness starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(test_harness_init());
    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(httpd_start_server());

    ESP_LOGI(TAG, "ready at http://cydtuner-test.local");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
