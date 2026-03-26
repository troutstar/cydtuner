#include "test_harness.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char *TAG = "test_harness";

/* ---- Params -------------------------------------------------------------- */
static tuner_params_t    s_params;
static SemaphoreHandle_t s_params_mutex;

/* ---- Latest full frame --------------------------------------------------- */
static pitch_frame_t     s_latest_frame;
static SemaphoreHandle_t s_frame_mutex;

/* ---- Compact ring buffer ------------------------------------------------- */
static compact_frame_t   s_history[HISTORY_LEN];
static int               s_history_head  = 0;
static int               s_history_count = 0;
static SemaphoreHandle_t s_history_mutex;

/* ---- WebSocket notify queue ---------------------------------------------- */
static QueueHandle_t     s_ws_notify_q;

/* ---- Ground truth table (from claudesweeps.py) --------------------------- */
#define GT_NOTE_DUR_SEC    12.0f
#define GT_SILENCE_DUR_SEC  1.5f
#define GT_SLOT_SEC        (GT_NOTE_DUR_SEC + GT_SILENCE_DUR_SEC)
#define GT_LOCK_OFFSET_SEC  9.0f   /* sweep locked at target only for last 3 s */
#define GT_N_NOTES         17

static const struct { float hz; } s_gt_table[GT_N_NOTES] = {
    {  41.20f }, {  55.00f }, {  61.74f },
    {  73.42f }, {  82.41f }, {  92.50f },
    {  98.00f }, { 110.00f }, { 123.47f },
    { 146.83f }, { 164.81f }, { 185.00f },
    { 196.00f }, { 207.65f }, { 246.94f },
    { 277.18f }, { 329.63f },
};

/* ---- Init ---------------------------------------------------------------- */

esp_err_t test_harness_init(void)
{
    s_params = (tuner_params_t){
        .threshold_coeff = 0.8f,
        .pitch_min_hz    = 40.0f,
        .pitch_max_hz    = 1200.0f,
        .smooth_alpha    = 0.0f,
    };
    s_params_mutex  = xSemaphoreCreateMutex();
    s_frame_mutex   = xSemaphoreCreateMutex();
    s_history_mutex = xSemaphoreCreateMutex();
    s_ws_notify_q   = xQueueCreate(4, sizeof(uint8_t));

    if (!s_params_mutex || !s_frame_mutex || !s_history_mutex || !s_ws_notify_q) {
        ESP_LOGE(TAG, "init alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(&s_latest_frame, 0, sizeof(s_latest_frame));
    return ESP_OK;
}

/* ---- Params -------------------------------------------------------------- */

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

/* ---- Frame post ---------------------------------------------------------- */

void test_harness_post_frame(const pitch_frame_t *frame)
{
    /* Update latest frame */
    xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
    s_latest_frame = *frame;
    xSemaphoreGive(s_frame_mutex);

    /* Append compact copy to ring buffer */
    compact_frame_t cf;
    cf.timestamp_us     = frame->timestamp_us;
    cf.detected_hz      = frame->detected_hz;
    cf.smooth_hz        = frame->smooth_hz;
    cf.cents            = frame->cents;
    cf.ground_truth_hz  = frame->ground_truth_hz;
    cf.cents_error      = frame->cents_error;
    cf.nsdf_peak_val    = frame->nsdf_peak_val;
    cf.nsdf_global_max  = frame->nsdf_global_max;
    cf.threshold_used   = frame->threshold_used;
    cf.tau_detected     = frame->tau_detected;
    cf.tau_interpolated = frame->tau_interpolated;
    cf.nsdf_len         = frame->nsdf_len;
    memcpy(cf.note, frame->note, sizeof(cf.note));

    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    s_history[s_history_head] = cf;
    s_history_head = (s_history_head + 1) % HISTORY_LEN;
    if (s_history_count < HISTORY_LEN) s_history_count++;
    xSemaphoreGive(s_history_mutex);

    /* Signal ws_sender_task (non-blocking; drop if queue full) */
    uint8_t sig = 1;
    xQueueSend(s_ws_notify_q, &sig, 0);
}

void test_harness_get_latest_frame(pitch_frame_t *out)
{
    xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
    *out = s_latest_frame;
    xSemaphoreGive(s_frame_mutex);
}

void test_harness_get_latest_compact(compact_frame_t *out)
{
    xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
    out->timestamp_us    = s_latest_frame.timestamp_us;
    out->detected_hz     = s_latest_frame.detected_hz;
    out->smooth_hz       = s_latest_frame.smooth_hz;
    out->cents           = s_latest_frame.cents;
    out->ground_truth_hz = s_latest_frame.ground_truth_hz;
    out->cents_error     = s_latest_frame.cents_error;
    out->nsdf_peak_val   = s_latest_frame.nsdf_peak_val;
    out->nsdf_global_max = s_latest_frame.nsdf_global_max;
    out->threshold_used  = s_latest_frame.threshold_used;
    out->tau_detected    = s_latest_frame.tau_detected;
    out->tau_interpolated= s_latest_frame.tau_interpolated;
    out->nsdf_len        = s_latest_frame.nsdf_len;
    memcpy(out->note, s_latest_frame.note, sizeof(out->note));
    xSemaphoreGive(s_frame_mutex);
}

/* ---- History ------------------------------------------------------------- */

int test_harness_get_history(compact_frame_t *out, int max_n)
{
    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    int n = s_history_count < max_n ? s_history_count : max_n;
    int start = (s_history_head - n + HISTORY_LEN) % HISTORY_LEN;
    for (int i = 0; i < n; i++)
        out[i] = s_history[(start + i) % HISTORY_LEN];
    xSemaphoreGive(s_history_mutex);
    return n;
}

void test_harness_history_clear(void)
{
    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    s_history_head  = 0;
    s_history_count = 0;
    xSemaphoreGive(s_history_mutex);
}

/* ---- Ground truth -------------------------------------------------------- */

float test_harness_ground_truth(float pos_sec)
{
    if (pos_sec < 0.0f) return 0.0f;
    int slot = (int)(pos_sec / GT_SLOT_SEC);
    if (slot >= GT_N_NOTES) return 0.0f;
    float offset = pos_sec - (float)slot * GT_SLOT_SEC;
    if (offset >= GT_NOTE_DUR_SEC) return 0.0f;  /* silence gap */
    if (offset < GT_LOCK_OFFSET_SEC) return 0.0f; /* approach/hunt phase */
    return s_gt_table[slot].hz;
}

/* ---- WS notify queue handle ---------------------------------------------- */

void *test_harness_get_ws_notify_q(void) { return (void *)s_ws_notify_q; }
