#pragma once
#include "esp_err.h"
#include <stdint.h>

/* ---- Tunable algorithm parameters ---------------------------------------- */

typedef struct {
    float threshold_coeff;  /* NSDF peak threshold = coeff * global_max; default 0.8 */
    float pitch_min_hz;     /* lower search bound; default 40.0 */
    float pitch_max_hz;     /* upper search bound; default 1200.0 */
    float smooth_alpha;     /* EMA: 0.0 = raw output, ~0.9 = heavy smoothing; default 0.0 */
} tuner_params_t;

/* ---- Full instrumented frame (~8.2KB — do NOT stack-allocate) ------------ */

typedef struct {
    uint64_t timestamp_us;      /* esp_timer_get_time() at detection */
    float    detected_hz;       /* raw algorithm output; 0.0 = no detection */
    float    smooth_hz;         /* after EMA smoothing */
    char     note[4];           /* nearest note name, e.g. "A", "C#" */
    float    cents;             /* deviation from nearest note (cents) */
    float    ground_truth_hz;   /* expected Hz from sweep schedule; 0.0 = silence/past end */
    float    cents_error;       /* cents(detected vs ground_truth); 0.0 if gt == 0 */
    float    nsdf_peak_val;     /* NSDF value at selected peak (confidence 0–1) */
    float    nsdf_global_max;   /* global max of s_cmnd[] array */
    float    threshold_used;    /* threshold_coeff * nsdf_global_max */
    size_t   tau_detected;      /* integer lag index of selected peak */
    float    tau_interpolated;  /* sub-sample lag after parabolic interpolation */
    uint16_t nsdf_len;          /* number of valid entries in nsdf[]; always <= 2048 */
    float    nsdf[2048];        /* full NSDF curve; valid indices [0, nsdf_len) */
} pitch_frame_t;

/* ---- Compact frame (no NSDF array, ~80 bytes) — used for ring buffer + WS  */

typedef struct {
    uint64_t timestamp_us;
    float    detected_hz;
    float    smooth_hz;
    char     note[4];
    float    cents;
    float    ground_truth_hz;
    float    cents_error;
    float    nsdf_peak_val;
    float    nsdf_global_max;
    float    threshold_used;
    size_t   tau_detected;
    float    tau_interpolated;
    uint16_t nsdf_len;
} compact_frame_t;

/* ---- Public API ----------------------------------------------------------- */

esp_err_t test_harness_init(void);

/* Params — written by HTTP POST /params, read by pitch_task */
void test_harness_get_params(tuner_params_t *out);
void test_harness_set_params(const tuner_params_t *in);

/* Frame — written by pitch_task, read by httpd handlers */
void test_harness_post_frame(const pitch_frame_t *frame);
void test_harness_get_latest_frame(pitch_frame_t *out);
void test_harness_get_latest_compact(compact_frame_t *out);

/* History — ring buffer of last 200 compact frames */
#define HISTORY_LEN 200
int  test_harness_get_history(compact_frame_t *out, int max_n);

/* Ground truth — returns expected Hz for given WAV position, 0.0 = silence/unknown */
float test_harness_ground_truth(float pos_sec);

/* WebSocket notification queue — ws_sender_task drains this */
void *test_harness_get_ws_notify_q(void);  /* returns QueueHandle_t as void* to avoid FreeRTOS include in header */
