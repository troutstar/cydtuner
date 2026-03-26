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

/* ---- Ground truth table (from gen_tuning_sim.py) ------------------------- */
/* Each slot: 2.5s pluck at fixed Hz + 0.6s silence = 3.1s.
 * Leading silence: 1.0s before first pluck.
 * 17 strings × 5 plucks = 85 slots total.
 * Pluck sequence per string: -50, -20, +12, -5, 0 cents. */
#define GT_LEADING_SILENCE  1.0f
#define GT_PLUCK_DUR        2.5f
#define GT_SILENCE_DUR      0.6f
#define GT_SLOT_DUR         (GT_PLUCK_DUR + GT_SILENCE_DUR)
#define GT_LOCK_OFFSET      0.3f   /* ignore first 300ms transient after pluck */
#define GT_N_SLOTS          85

static const float s_gt_table[GT_N_SLOTS] = {
    /* E1  */ 40.027f, 40.727f, 41.487f, 41.081f, 41.200f,
    /* A1  */ 53.434f, 54.368f, 55.383f, 54.841f, 55.000f,
    /* B1  */ 59.982f, 61.031f, 62.169f, 61.562f, 61.740f,
    /* D2  */ 71.330f, 72.577f, 73.931f, 73.208f, 73.420f,
    /* E2  */ 80.064f, 81.463f, 82.983f, 82.172f, 82.410f,
    /* F#2 */ 89.867f, 91.438f, 93.143f, 92.233f, 92.500f,
    /* G2  */ 95.210f, 96.874f, 98.682f, 97.717f, 98.000f,
    /* A2  */106.869f,108.737f,110.765f,109.683f,110.000f,
    /* B2  */119.955f,122.052f,124.329f,123.114f,123.470f,
    /* D3  */142.650f,145.144f,147.851f,146.407f,146.830f,
    /* E3  */160.118f,162.917f,165.956f,164.335f,164.810f,
    /* F#3 */179.733f,182.875f,186.287f,184.466f,185.000f,
    /* G3  */190.420f,193.749f,197.363f,195.435f,196.000f,
    /* G#3 */201.739f,205.265f,209.094f,207.051f,207.650f,
    /* B3  */239.910f,244.104f,248.658f,246.228f,246.940f,
    /* C#4 */269.289f,273.996f,279.108f,276.381f,277.180f,
    /* E4  */320.246f,325.844f,331.923f,328.679f,329.630f,
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
    if (pos_sec < GT_LEADING_SILENCE) return 0.0f;
    float t    = pos_sec - GT_LEADING_SILENCE;
    int   slot = (int)(t / GT_SLOT_DUR);
    if (slot >= GT_N_SLOTS) return 0.0f;
    float offset = t - (float)slot * GT_SLOT_DUR;
    if (offset >= GT_PLUCK_DUR) return 0.0f;      /* silence between plucks */
    if (offset < GT_LOCK_OFFSET) return 0.0f;     /* ignore pluck transient */
    return s_gt_table[slot];
}

/* ---- WS notify queue handle ---------------------------------------------- */

void *test_harness_get_ws_notify_q(void) { return (void *)s_ws_notify_q; }
