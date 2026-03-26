#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "audio.h"
#include "pitch.h"
#include "display.h"
#include "ili9341.h"
#include "wifi.h"
#include "httpd.h"
#include "test_harness.h"
#include <math.h>
#include <string.h>

#define LED_GREEN_PIN  16   /* common anode — low = on */

static const char *TAG = "main";

#define AUDIO_BUF_SAMPLES 4096

static int16_t           s_buf_pool[1][AUDIO_BUF_SAMPLES];  /* single buffer — WAV source, stall on contention is OK */
static QueueHandle_t     s_sample_q;
static SemaphoreHandle_t s_buf_sem;
static TaskHandle_t      s_audio_h, s_pitch_h, s_display_h;

/* Called by httpd /diag handler */
void main_get_diag(int *q_waiting, int *sem_count,
                   uint32_t *audio_hwm, uint32_t *pitch_hwm, uint32_t *display_hwm)
{
    *q_waiting   = s_sample_q ? (int)uxQueueMessagesWaiting(s_sample_q) : -1;
    *sem_count   = s_buf_sem  ? (int)uxSemaphoreGetCount(s_buf_sem)     : -1;
    *audio_hwm   = s_audio_h   ? uxTaskGetStackHighWaterMark(s_audio_h)   : 0;
    *pitch_hwm   = s_pitch_h   ? uxTaskGetStackHighWaterMark(s_pitch_h)   : 0;
    *display_hwm = s_display_h ? uxTaskGetStackHighWaterMark(s_display_h) : 0;
}

static void audio_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_buf_sem, portMAX_DELAY);
        int16_t *buf = s_buf_pool[0];
        int got = audio_read(buf, AUDIO_BUF_SAMPLES);
        if (got <= 0) { xSemaphoreGive(s_buf_sem); vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (xQueueSend(s_sample_q, &buf, portMAX_DELAY) != pdTRUE) xSemaphoreGive(s_buf_sem);
    }
}

static void display_task(void *arg)
{
    float last_hz = 440.0f;
    char  last_note[4] = "-";
    for (;;) {
        compact_frame_t f;
        test_harness_get_latest_compact(&f);
        if (f.detected_hz > 0.0f) {
            last_hz = f.detected_hz;
            memcpy(last_note, f.note, sizeof(last_note));
        }
        display_render_strobe(last_hz, last_note);
        vTaskDelay(1);  /* yield CPU; SPI transfer already throttles frame rate */
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

        /* Ground truth: synth Hz (direct) or WAV position lookup */
        float gt_hz = audio_synth_get_hz();
        if (gt_hz <= 0.0f) {
            float pos = audio_get_position_sec();
            gt_hz = test_harness_ground_truth(pos);
        }
        s_frame.ground_truth_hz = gt_hz;
        s_frame.cents_error = (gt_hz > 0.0f && hz > 0.0f)
            ? 1200.0f * log2f(hz / gt_hz)
            : 0.0f;

        vTaskDelay(1);  /* 1 tick lets IDLE reset WDT */
        xSemaphoreGive(s_buf_sem);

        test_harness_post_frame(&s_frame);
    }
}

#define HEAP_LOG(tag) ESP_LOGI(TAG, "heap after %-18s: %6lu free, %6lu DMA", \
    (tag), (unsigned long)esp_get_free_heap_size(), \
    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))

void app_main(void)
{
    ESP_LOGI(TAG, "test harness starting");
    HEAP_LOG("boot");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    HEAP_LOG("nvs_flash_init");

    gpio_reset_pin(LED_GREEN_PIN);
    gpio_set_direction(LED_GREEN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GREEN_PIN, 0);   /* on */

    ESP_ERROR_CHECK(ili9341_init());
    HEAP_LOG("ili9341_init");
    ESP_ERROR_CHECK(display_init());
    HEAP_LOG("display_init");
    ESP_ERROR_CHECK(test_harness_init());
    HEAP_LOG("test_harness_init");
    ESP_ERROR_CHECK(audio_init(AUDIO_SOURCE_WAV_FILE));
    HEAP_LOG("audio_init");
    ESP_ERROR_CHECK(pitch_init(AUDIO_BUF_SAMPLES));
    HEAP_LOG("pitch_init");
    ESP_ERROR_CHECK(wifi_init());
    HEAP_LOG("wifi_init");
    ESP_ERROR_CHECK(httpd_start_server());
    HEAP_LOG("httpd_start_server");

    s_sample_q = xQueueCreate(1, sizeof(int16_t *));
    s_buf_sem  = xSemaphoreCreateCounting(1, 1);

    /* All large arrays in both tasks are static/heap — actual stack use is <1KB each */
    xTaskCreatePinnedToCore(audio_task,   "audio",   4096*1, NULL, 5, &s_audio_h,   0);
    HEAP_LOG("audio_task");
    xTaskCreatePinnedToCore(pitch_task,   "pitch",   4096*1, NULL, 4, &s_pitch_h,   1);
    HEAP_LOG("pitch_task");
    xTaskCreatePinnedToCore(display_task, "display", 4096*1, NULL, 3, &s_display_h, 0);
    HEAP_LOG("display_task");

    ESP_LOGI(TAG, "ready at http://cydtuner-test.local");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
