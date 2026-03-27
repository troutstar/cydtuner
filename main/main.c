#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "ili9341.h"
#include "audio.h"
#include "pitch.h"
#include "display.h"
#include "touch.h"

static const char *TAG = "main";

#define AUDIO_BUF_SAMPLES 4096

static int16_t s_buf_pool[2][AUDIO_BUF_SAMPLES];
static QueueHandle_t    s_sample_q;
static QueueHandle_t    s_freq_q;
static SemaphoreHandle_t s_buf_sem;

static void audio_task(void *arg) {
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

static void pitch_task(void *arg) {
    float sr = (float)audio_get_sample_rate();
    for (;;) {
        int16_t *buf = NULL;
        if (xQueueReceive(s_sample_q, &buf, portMAX_DELAY) == pdTRUE) {
            float hz = pitch_detect(buf, AUDIO_BUF_SAMPLES, sr);
            vTaskDelay(1); /* 1 tick (1ms @ 1000Hz) lets IDLE1 run and reset WDT */
            xSemaphoreGive(s_buf_sem);
            char note_log[4];
            pitch_hz_to_note(hz, note_log, sizeof(note_log));
            float cents_log = pitch_hz_to_cents(hz);
            ESP_LOGI("pitch", "%.2f Hz  %s  %+.0f cents", hz, note_log, (double)cents_log);
            xQueueOverwrite(s_freq_q, &hz);
        }
    }
}

static void display_task(void *arg) {
    float last = 440.0f;
    char note[4] = "-";
    for (;;) {
        float hz;
        if (xQueueReceive(s_freq_q, &hz, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (hz > 0.0f) {
                last = hz;
                pitch_hz_to_note(hz, note, sizeof(note));
            } else {
                last = 0.0f;
            }
        }
        display_render_strobe(last, note);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "strobe tuner starting");
    ESP_ERROR_CHECK(ili9341_init());
    ESP_ERROR_CHECK(audio_init(AUDIO_SOURCE_WAV_FILE));
    ESP_ERROR_CHECK(pitch_init(AUDIO_BUF_SAMPLES));
    ESP_ERROR_CHECK(display_init());

    s_sample_q = xQueueCreate(1, sizeof(int16_t *));
    s_freq_q   = xQueueCreate(1, sizeof(float));
    s_buf_sem  = xSemaphoreCreateCounting(2, 2);

    xTaskCreatePinnedToCore(audio_task,   "audio",   4096*4, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(pitch_task,   "pitch",   4096*4, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(display_task, "display", 4096*4, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "tasks running");
}
