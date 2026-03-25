#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "audio.h"
#include "pitch.h"
#include "wifi.h"
#include "httpd.h"
#include "test_harness.h"
#include <math.h>

static const char *TAG = "main";

#define AUDIO_BUF_SAMPLES 4096

static int16_t           s_buf_pool[2][AUDIO_BUF_SAMPLES];
static QueueHandle_t     s_sample_q;
static SemaphoreHandle_t s_buf_sem;

static void audio_task(void *arg)
{
    int idx = 0;
    for (;;) {
        xSemaphoreTake(s_buf_sem, portMAX_DELAY);
        int16_t *buf = s_buf_pool[idx];
        int got = audio_read(buf, AUDIO_BUF_SAMPLES);
        if (got <= 0) { xSemaphoreGive(s_buf_sem); vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (xQueueSend(s_sample_q, &buf, portMAX_DELAY) == pdTRUE) idx = 1 - idx;
        else xSemaphoreGive(s_buf_sem);
    }
}

static void pitch_task(void *arg)
{
    float sr = (float)audio_get_sample_rate();
    static pitch_frame_t s_frame;  /* static — 8.2KB, must not be on stack */

    for (;;) {
        int16_t *buf = NULL;
        if (xQueueReceive(s_sample_q, &buf, portMAX_DELAY) != pdTRUE) continue;

        tuner_params_t params;
        test_harness_get_params(&params);

        float hz = pitch_detect_full(buf, AUDIO_BUF_SAMPLES, sr, &params, &s_frame);

        /* Ground truth from WAV position (~93ms lag: one buffer behind audio_task) */
        float pos = audio_get_position_sec();
        s_frame.ground_truth_hz = test_harness_ground_truth(pos);
        s_frame.cents_error = (s_frame.ground_truth_hz > 0.0f && hz > 0.0f)
            ? 1200.0f * log2f(hz / s_frame.ground_truth_hz)
            : 0.0f;

        vTaskDelay(1);  /* 1 tick lets IDLE reset WDT */
        xSemaphoreGive(s_buf_sem);

        test_harness_post_frame(&s_frame);
    }
}

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
    ESP_ERROR_CHECK(audio_init(AUDIO_SOURCE_WAV_FILE));
    ESP_ERROR_CHECK(pitch_init(AUDIO_BUF_SAMPLES));
    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(httpd_start_server());

    s_sample_q = xQueueCreate(1, sizeof(int16_t *));
    s_buf_sem  = xSemaphoreCreateCounting(2, 2);

    /* All large arrays in both tasks are static/heap — actual stack use is <1KB each */
    xTaskCreatePinnedToCore(audio_task, "audio", 4096*1, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(pitch_task, "pitch", 4096*2, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "ready at http://cydtuner-test.local");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
