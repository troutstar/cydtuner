# Test Harness Plan 2 — Algorithm Instrumentation, Pipeline, and Endpoints

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Instrument the pitch algorithm for deep introspection, wire up the audio→pitch data pipeline, and expose all REST and WebSocket endpoints — ending with live pitch frame data accessible via HTTP from any machine on the network.

**Architecture:** `pitch_detect_full()` runs the existing NSDF algorithm and fills a `pitch_frame_t` with every internal value (full NSDF curve, confidence, tau, ground truth accuracy). `test_harness.c` owns shared state: latest frame, 200-entry compact ring buffer, tunable params, and WebSocket notify queue. A dedicated `ws_sender_task` drives WebSocket broadcasts at ~10Hz via `httpd_queue_work()`. Display and touch tasks are not included — the LCD on the test device stays dark; the web interface is the display.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS, NSDF pitch detection, cJSON, esp_http_server WebSocket

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `test_harness/main/CMakeLists.txt` | Modify | Add audio.c + pitch.c to SRCS; uncomment linker wrap + PITCH_TEST_HARNESS; add driver/sdmmc/fatfs REQUIRES |
| `test_harness/main/test_harness.h` | Modify | Add `pitch_frame_t`, `compact_frame_t`, full API |
| `test_harness/main/test_harness.c` | Modify | Frame storage, history ring buffer, ground truth, WS notify queue |
| `main/pitch.h` | Modify | Add `pitch_detect_full()` declaration under `PITCH_TEST_HARNESS` guard |
| `main/pitch.c` | Modify | Implement `pitch_detect_full()` under guard |
| `main/audio.h` | Modify | Add `audio_get_position_sec()` declaration under guard |
| `main/audio.c` | Modify | Add `s_position_bytes` tracking; implement `audio_get_position_sec()` under guard |
| `test_harness/main/main.c` | Modify | Add audio_task, pitch_task, ws_sender_task creation |
| `test_harness/main/httpd.c` | Modify | Add `/snapshot`, `/stats`, `/history`, `/ws` + ws_sender_task + ws_broadcast_cb |
| `test_harness/main/httpd.h` | Modify | Expose `httpd_get_server()` for ws_sender_task registration |

---

### Task 1: Update build system

**Files:**
- Modify: `test_harness/main/CMakeLists.txt`

- [ ] **Step 1: Update CMakeLists.txt**

Replace the SRCS line and uncomment the guarded blocks:

```cmake
idf_component_register(
    SRCS "main.c" "wifi.c" "httpd.c" "test_harness.c"
         "../../main/pitch.c" "../../main/audio.c"
    INCLUDE_DIRS "." "../../main"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        mdns
        nvs_flash
        esp_http_server
        json
        esp_timer
        driver
        sdmmc
        fatfs
)

target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=sdmmc_init_spi_crc")

target_compile_definitions(${COMPONENT_LIB} PRIVATE PITCH_TEST_HARNESS=1)
```

`driver` is needed for audio.c GPIO/SPI. `sdmmc` for sdspi_host and sdmmc_cmd. `fatfs`
for esp_vfs_fat. display.c and touch.c are not included — display stays off on the test
device; the web interface replaces it.

- [ ] **Step 2: Build to confirm new SRCS compile**

```bash
idf.py build
```

Expected: Clean build. audio.c and pitch.c compile (no `pitch_detect_full` yet — that's added in Task 2 and referenced from main.c/test_harness.c in Task 3).

If `fatfs` is not found: try `esp_fatfs` as the component name (IDF 5.x variant). If `driver` fails: it may have been split into `esp_driver_gpio` + `esp_driver_spi_master` + `esp_driver_sdspi` in IDF 5.4 — update REQUIRES directly with the correct split-component names and rebuild (do not use `idf.py add-dependency`, which is only for managed components, not built-in IDF components).

- [ ] **Step 3: Commit**

```bash
git add test_harness/main/CMakeLists.txt
git commit -m "build: test_harness add audio.c + pitch.c, enable PITCH_TEST_HARNESS"
```

---

### Task 2: Define types + instrument pitch.c + audio.c

**Files:**
- Modify: `test_harness/main/test_harness.h`
- Modify: `main/pitch.h`
- Modify: `main/pitch.c`
- Modify: `main/audio.h`
- Modify: `main/audio.c`

- [ ] **Step 1: Expand test_harness.h with pitch_frame_t, compact_frame_t, and full API**

Replace `test_harness/main/test_harness.h`:

```c
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
void *test_harness_get_ws_notify_q(void);  /* returns QueueHandle_t, void* to avoid FreeRTOS include in header */
```

- [ ] **Step 2: Update pitch.h — add pitch_detect_full() declaration under guard**

Append to `main/pitch.h` before the final blank line:

```c
#ifdef PITCH_TEST_HARNESS
#include "test_harness.h"
float pitch_detect_full(const int16_t *buf, size_t len, float sample_rate,
                        const tuner_params_t *params, pitch_frame_t *frame);
#endif
```

- [ ] **Step 3: Update audio.h — add audio_get_position_sec() declaration under guard**

Append to `main/audio.h` before the final blank line:

```c
#ifdef PITCH_TEST_HARNESS
float audio_get_position_sec(void);
#endif
```

- [ ] **Step 4: Implement pitch_detect_full() in pitch.c**

Append to `main/pitch.c`:

```c
#ifdef PITCH_TEST_HARNESS
float pitch_detect_full(const int16_t *buf, size_t len, float sample_rate,
                        const tuner_params_t *params, pitch_frame_t *frame)
{
    static float s_smooth_hz = 0.0f;

    size_t half = (len / 2 < s_half) ? len / 2 : s_half;

    float *wx = s_wbuf;
    for (size_t i = 0; i < half; i++)
        wx[i] = (float)buf[i];

    for (size_t tau = 0; tau < half; tau++) {
        float m_val = 0.0f, r0s = 0.0f, r0e = 0.0f;
        for (size_t j = 0; j < half - tau; j++) {
            m_val += wx[j] * wx[j + tau];
            r0s   += wx[j] * wx[j];
        }
        for (size_t j = tau; j < half; j++)
            r0e += wx[j] * wx[j];
        float denom = r0s + r0e;
        s_diff[tau] = m_val;
        s_cmnd[tau] = (denom > 0.0f) ? 2.0f * m_val / denom : 0.0f;
    }

    size_t tau_min = (size_t)(sample_rate / params->pitch_max_hz) + 1;
    size_t tau_max = (size_t)(sample_rate / params->pitch_min_hz);
    if (tau_max >= half) tau_max = half - 1;

    float global_max = 0.0f;
    for (size_t tau = 0; tau < half; tau++)
        if (s_cmnd[tau] > global_max) global_max = s_cmnd[tau];
    float threshold = params->threshold_coeff * global_max;

    float detected_hz = 0.0f;
    size_t tau_det = 0;
    float  tau_interp = 0.0f;
    float  peak_val = 0.0f;

    for (size_t tau = tau_min; tau <= tau_max; tau++) {
        float right = (tau + 1 < half) ? s_cmnd[tau + 1] : 0.0f;
        if (s_cmnd[tau] > s_cmnd[tau - 1] &&
            s_cmnd[tau] >= right &&
            s_cmnd[tau] > threshold) {
            tau_det  = tau;
            peak_val = s_cmnd[tau];
            float bt = (float)tau;
            if (tau > 0 && tau < half - 1) {
                float s0 = s_cmnd[tau-1], s1 = s_cmnd[tau], s2 = s_cmnd[tau+1];
                float d2 = 2.0f * (2.0f * s1 - s2 - s0);
                if (fabsf(d2) > FLT_EPSILON) bt = (float)tau + (s2 - s0) / d2;
            }
            tau_interp  = bt;
            detected_hz = sample_rate / bt;
            break;
        }
    }

    /* EMA smoothing: alpha=0 → raw, alpha→1 → heavy smoothing */
    if (detected_hz > 0.0f) {
        s_smooth_hz = (s_smooth_hz == 0.0f)
            ? detected_hz
            : params->smooth_alpha * s_smooth_hz + (1.0f - params->smooth_alpha) * detected_hz;
    } else {
        s_smooth_hz = 0.0f;
    }

    /* Fill frame */
    frame->timestamp_us    = (uint64_t)esp_timer_get_time();
    frame->detected_hz     = detected_hz;
    frame->smooth_hz       = s_smooth_hz;
    frame->cents           = pitch_hz_to_cents(detected_hz > 0.0f ? detected_hz : 1.0f);
    frame->nsdf_peak_val   = peak_val;
    frame->nsdf_global_max = global_max;
    frame->threshold_used  = threshold;
    frame->tau_detected    = tau_det;
    frame->tau_interpolated= tau_interp;
    frame->nsdf_len        = (uint16_t)half;

    pitch_hz_to_note(detected_hz > 0.0f ? detected_hz : s_smooth_hz,
                     frame->note, sizeof(frame->note));

    frame->nsdf_len = (uint16_t)half;
    for (size_t i = 0; i < half; i++) frame->nsdf[i] = s_cmnd[i];

    return detected_hz;
}
#endif /* PITCH_TEST_HARNESS */
```

- [ ] **Step 5: Instrument audio.c — add position tracking + audio_get_position_sec()**

In `main/audio.c`, add at module level (after the existing static variables):

```c
#ifdef PITCH_TEST_HARNESS
static volatile uint32_t s_position_bytes = 0;
#endif
```

Replace `audio_read()` to update position after both mono and stereo paths:

```c
int audio_read(int16_t *buf, size_t len) {
    if (!s_wav_file) return -1;
    if ((uint32_t)ftell(s_wav_file) >= s_data_end)
        fseek(s_wav_file, s_data_start, SEEK_SET);

    int got;
    if (s_channels == 1) {
        got = (int)fread(buf, sizeof(int16_t), len, s_wav_file);
    } else {
        size_t n = fread(s_stereo_buf, sizeof(int16_t), len * 2, s_wav_file) / 2;
        for (size_t i = 0; i < n; i++)
            buf[i] = (int16_t)(((int32_t)s_stereo_buf[i*2] + s_stereo_buf[i*2+1]) / 2);
        got = (int)n;
    }

#ifdef PITCH_TEST_HARNESS
    if (got > 0 && s_wav_file)
        s_position_bytes = (uint32_t)ftell(s_wav_file) - s_data_start;
#endif

    return got;
}
```

Append `audio_get_position_sec()` to `main/audio.c`:

```c
#ifdef PITCH_TEST_HARNESS
float audio_get_position_sec(void) {
    if (!s_wav_file || s_sample_rate == 0) return 0.0f;
    uint32_t bytes_per_sec = s_sample_rate * s_channels * 2;
    return (float)s_position_bytes / (float)bytes_per_sec;
}
#endif
```

- [ ] **Step 6: Build**

```bash
idf.py build
```

Expected: Compiles cleanly. `pitch_detect_full` and `audio_get_position_sec` are defined; callers added in Task 3.

- [ ] **Step 7: Commit**

```bash
git add main/pitch.h main/pitch.c main/audio.h main/audio.c test_harness/main/test_harness.h
git commit -m "feat: pitch_detect_full() + audio_get_position_sec() under PITCH_TEST_HARNESS guard"
```

---

### Task 3: Shared state + audio/pitch pipeline wired

**Files:**
- Modify: `test_harness/main/test_harness.c`
- Modify: `test_harness/main/main.c`

- [ ] **Step 1: Implement test_harness.c — full shared state**

Replace `test_harness/main/test_harness.c`:

```c
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
static int               s_history_head = 0;
static int               s_history_count = 0;
static SemaphoreHandle_t s_history_mutex;

/* ---- WebSocket notify queue ---------------------------------------------- */
static QueueHandle_t     s_ws_notify_q;

/* ---- Ground truth table (from claudesweeps.py) --------------------------- */
#define GT_NOTE_DUR_SEC    12.0f
#define GT_SILENCE_DUR_SEC  0.5f
#define GT_SLOT_SEC        (GT_NOTE_DUR_SEC + GT_SILENCE_DUR_SEC)
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
    s_params_mutex   = xSemaphoreCreateMutex();
    s_frame_mutex    = xSemaphoreCreateMutex();
    s_history_mutex  = xSemaphoreCreateMutex();
    s_ws_notify_q    = xQueueCreate(4, sizeof(uint8_t));

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
    compact_frame_t cf = {
        .timestamp_us    = frame->timestamp_us,
        .detected_hz     = frame->detected_hz,
        .smooth_hz       = frame->smooth_hz,
        .cents           = frame->cents,
        .ground_truth_hz = frame->ground_truth_hz,
        .cents_error     = frame->cents_error,
        .nsdf_peak_val   = frame->nsdf_peak_val,
        .nsdf_global_max = frame->nsdf_global_max,
        .threshold_used  = frame->threshold_used,
        .tau_detected    = frame->tau_detected,
        .tau_interpolated= frame->tau_interpolated,
        .nsdf_len        = frame->nsdf_len,
    };
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
    /* Copy scalar fields only */
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
    /* Return in chronological order from oldest to newest */
    int start = (s_history_head - n + HISTORY_LEN) % HISTORY_LEN;
    for (int i = 0; i < n; i++)
        out[i] = s_history[(start + i) % HISTORY_LEN];
    xSemaphoreGive(s_history_mutex);
    return n;
}

/* ---- Ground truth -------------------------------------------------------- */

float test_harness_ground_truth(float pos_sec)
{
    if (pos_sec < 0.0f) return 0.0f;
    int slot = (int)(pos_sec / GT_SLOT_SEC);
    if (slot >= GT_N_NOTES) return 0.0f;
    float offset = pos_sec - (float)slot * GT_SLOT_SEC;
    if (offset >= GT_NOTE_DUR_SEC) return 0.0f;  /* silence gap */
    return s_gt_table[slot].hz;
}

/* ---- WS notify queue handle ---------------------------------------------- */

void *test_harness_get_ws_notify_q(void) { return (void *)s_ws_notify_q; }
```

- [ ] **Step 2: Replace main.c — add audio_task + pitch_task**

```c
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

static int16_t          s_buf_pool[2][AUDIO_BUF_SAMPLES];
static QueueHandle_t    s_sample_q;
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

        /* Ground truth from WAV position */
        float pos = audio_get_position_sec();
        s_frame.ground_truth_hz = test_harness_ground_truth(pos);
        s_frame.cents_error = (s_frame.ground_truth_hz > 0.0f && hz > 0.0f)
            ? 1200.0f * log2f(hz / s_frame.ground_truth_hz)
            : 0.0f;

        vTaskDelay(1);  /* let IDLE reset WDT */
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

    /* pitch_task stack: 24KB (spec §Task Architecture) to hold static frame + algorithm memory */
    xTaskCreatePinnedToCore(audio_task, "audio", 4096*4,  NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(pitch_task, "pitch", 4096*6,  NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "ready at http://cydtuner-test.local");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

- [ ] **Step 3: Build**

```bash
idf.py build
```

Expected: Clean build.

- [ ] **Step 4: Flash + verify pitch task is running**

```bash
idf.py -p COM5 flash monitor
```

Expected serial output (after WiFi connects):
```
I (xxx) wifi: IP: 192.168.86.163
I (xxx) httpd: HTTP server started
I (xxx) main: ready at http://cydtuner-test.local
```

Then pitch detections should appear (no log for them in this version — that's fine; the HTTP endpoints verify it in Task 4).

If SD card fails to mount: check that `sweep.wav` is on the card and the SD pins match CLAUDE.md.

- [ ] **Step 5: Commit**

```bash
git add test_harness/main/test_harness.c test_harness/main/main.c
git commit -m "feat: test_harness audio+pitch pipeline wired, ground truth tracking"
```

---

### Task 4: HTTP endpoints + WebSocket + end-to-end verification

**Files:**
- Modify: `test_harness/main/httpd.h`
- Modify: `test_harness/main/httpd.c`

- [ ] **Step 1: Update httpd.h**

```c
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Start the HTTP server and register all URI handlers.
 * Must be called after wifi_init() and test_harness_init().
 *
 * Registers:
 *   GET  /params    — current tuner_params_t as JSON
 *   POST /params    — update params (partial JSON accepted)
 *   GET  /snapshot  — full pitch_frame_t as chunked JSON (includes NSDF array)
 *   GET  /stats     — rolling stats over last 200 readings
 *   GET  /history   — last N compact frames (?n=50, max 200)
 *   GET  /ws        — WebSocket (compact frames pushed at ~10Hz)
 *
 * Also starts ws_sender_task which drives WebSocket broadcasts.
 */
esp_err_t httpd_start_server(void);

/** Returns the server handle (used by ws_sender_task internally). */
httpd_handle_t httpd_get_server(void);
```

- [ ] **Step 2: Replace httpd.c with full implementation**

```c
#include "httpd.h"
#include "test_harness.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "httpd";

static httpd_handle_t s_server = NULL;

/* ---- WebSocket client list ----------------------------------------------- */
#define MAX_WS_CLIENTS 4
static int              s_ws_fds[MAX_WS_CLIENTS];
static int              s_ws_n = 0;
static SemaphoreHandle_t s_ws_mutex;

static void ws_add_client(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    if (s_ws_n < MAX_WS_CLIENTS) s_ws_fds[s_ws_n++] = fd;
    xSemaphoreGive(s_ws_mutex);
}

/* ---- Compact frame JSON helper ------------------------------------------- */
static int compact_to_json(const compact_frame_t *f, char *buf, size_t sz)
{
    return snprintf(buf, sz,
        "{\"ts_us\":%llu,\"detected_hz\":%.4f,\"smooth_hz\":%.4f,"
        "\"note\":\"%s\",\"cents\":%.3f,\"ground_truth_hz\":%.4f,"
        "\"cents_error\":%.3f,\"nsdf_peak_val\":%.4f,"
        "\"nsdf_global_max\":%.4f,\"threshold_used\":%.4f,"
        "\"tau_detected\":%u,\"tau_interpolated\":%.4f,\"nsdf_len\":%u}",
        (unsigned long long)f->timestamp_us,
        (double)f->detected_hz, (double)f->smooth_hz,
        f->note,
        (double)f->cents, (double)f->ground_truth_hz,
        (double)f->cents_error, (double)f->nsdf_peak_val,
        (double)f->nsdf_global_max, (double)f->threshold_used,
        (unsigned)f->tau_detected, (double)f->tau_interpolated,
        (unsigned)f->nsdf_len);
}

/* ---- WebSocket broadcast (called from httpd task via httpd_queue_work) ---- */
static void ws_broadcast_cb(void *arg)
{
    compact_frame_t f;
    test_harness_get_latest_compact(&f);

    char buf[512];
    int len = compact_to_json(&f, buf, sizeof(buf));
    if (len <= 0 || len >= (int)sizeof(buf)) return;

    httpd_ws_frame_t pkt = {
        .final = true, .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = (size_t)len,
    };

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < s_ws_n; ) {
        esp_err_t ret = httpd_ws_send_frame_async(s_server, s_ws_fds[i], &pkt);
        if (ret != ESP_OK) {
            /* Stale fd — remove by swapping with last */
            s_ws_fds[i] = s_ws_fds[--s_ws_n];
        } else {
            i++;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

/* ---- ws_sender_task ------------------------------------------------------- */
static void ws_sender_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)test_harness_get_ws_notify_q();
    int64_t last_send_us = 0;
    uint8_t sig;

    for (;;) {
        /* Block until pitch_task signals a new frame (100ms timeout) */
        if (xQueueReceive(q, &sig, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        /* Drain any additional pending signals (don't send more than ~10Hz) */
        while (xQueueReceive(q, &sig, 0) == pdTRUE) {}

        int64_t now = esp_timer_get_time();
        if (now - last_send_us < 100000) continue;  /* < 100ms since last send */
        last_send_us = now;

        if (s_server && s_ws_n > 0)
            httpd_queue_work(s_server, ws_broadcast_cb, NULL);
    }
}

/* ---- GET /params --------------------------------------------------------- */
static esp_err_t params_get_handler(httpd_req_t *req)
{
    tuner_params_t p;
    test_harness_get_params(&p);
    char buf[160];
    int len = snprintf(buf, sizeof(buf),
        "{\"threshold_coeff\":%.4f,\"pitch_min_hz\":%.1f,"
        "\"pitch_max_hz\":%.1f,\"smooth_alpha\":%.4f}\n",
        (double)p.threshold_coeff, (double)p.pitch_min_hz,
        (double)p.pitch_max_hz,    (double)p.smooth_alpha);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ---- POST /params -------------------------------------------------------- */
static esp_err_t params_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    body[received] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    tuner_params_t p;
    test_harness_get_params(&p);
    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "threshold_coeff")) && cJSON_IsNumber(item)) p.threshold_coeff = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "pitch_min_hz"))    && cJSON_IsNumber(item)) p.pitch_min_hz    = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "pitch_max_hz"))    && cJSON_IsNumber(item)) p.pitch_max_hz    = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "smooth_alpha"))    && cJSON_IsNumber(item)) p.smooth_alpha    = (float)item->valuedouble;
    cJSON_Delete(root);
    test_harness_set_params(&p);
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- GET /snapshot (chunked — full NSDF array ~20KB) -------------------- */
static esp_err_t snapshot_get_handler(httpd_req_t *req)
{
    /* Static to avoid 8.2KB stack hit; safe — httpd is single-threaded for handlers */
    static pitch_frame_t s_snap;
    test_harness_get_latest_frame(&s_snap);

    httpd_resp_set_type(req, "application/json");

    /* Chunk 1: all scalar fields */
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "{\"ts_us\":%llu,\"detected_hz\":%.4f,\"smooth_hz\":%.4f,"
        "\"note\":\"%s\",\"cents\":%.3f,\"ground_truth_hz\":%.4f,"
        "\"cents_error\":%.3f,\"nsdf_peak_val\":%.4f,"
        "\"nsdf_global_max\":%.4f,\"threshold_used\":%.4f,"
        "\"tau_detected\":%u,\"tau_interpolated\":%.4f,\"nsdf_len\":%u,"
        "\"nsdf\":[",
        (unsigned long long)s_snap.timestamp_us,
        (double)s_snap.detected_hz, (double)s_snap.smooth_hz,
        s_snap.note,
        (double)s_snap.cents, (double)s_snap.ground_truth_hz,
        (double)s_snap.cents_error, (double)s_snap.nsdf_peak_val,
        (double)s_snap.nsdf_global_max, (double)s_snap.threshold_used,
        (unsigned)s_snap.tau_detected, (double)s_snap.tau_interpolated,
        (unsigned)s_snap.nsdf_len);
    httpd_resp_send_chunk(req, hdr, hlen);

    /* Chunk 2+: nsdf[] values in ~512-byte slices */
    char chunk[256];
    int clen = 0;
    for (uint16_t i = 0; i < s_snap.nsdf_len; i++) {
        clen += snprintf(chunk + clen, sizeof(chunk) - clen - 2,
            "%.5f", (double)s_snap.nsdf[i]);
        if (i < s_snap.nsdf_len - 1) chunk[clen++] = ',';
        if (clen > 220 || i == s_snap.nsdf_len - 1) {
            httpd_resp_send_chunk(req, chunk, clen);
            clen = 0;
        }
    }

    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- GET /stats ---------------------------------------------------------- */
static esp_err_t stats_get_handler(httpd_req_t *req)
{
    static compact_frame_t hist[HISTORY_LEN];
    int n = test_harness_get_history(hist, HISTORY_LEN);
    if (n == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"n\":0}");
    }

    double sum_hz = 0, sum_hz2 = 0, min_hz = 1e9, max_hz = 0;
    double sum_ce = 0, sum_ce2 = 0, min_ce = 1e9, max_ce = -1e9;
    double sum_pk = 0, sum_pk2 = 0;
    int n_hz = 0, n_gt = 0;

    for (int i = 0; i < n; i++) {
        if (hist[i].detected_hz <= 0.0f) continue;
        double hz = hist[i].detected_hz;
        sum_hz += hz; sum_hz2 += hz * hz;
        if (hz < min_hz) min_hz = hz;
        if (hz > max_hz) max_hz = hz;
        double pk = hist[i].nsdf_peak_val;
        sum_pk += pk; sum_pk2 += pk * pk;
        n_hz++;
        if (hist[i].ground_truth_hz > 0.0f) {
            double ce = hist[i].cents_error;
            sum_ce += ce; sum_ce2 += ce * ce;
            if (ce < min_ce) min_ce = ce;
            if (ce > max_ce) max_ce = ce;
            n_gt++;
        }
    }

    if (n_hz == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"n\":0,\"n_with_gt\":0}");
    }
    double mean_hz  = sum_hz  / n_hz;
    double std_hz   = sqrtf((float)(sum_hz2  / n_hz - mean_hz  * mean_hz));
    double mean_pk  = sum_pk  / n_hz;
    double std_pk   = sqrtf((float)(sum_pk2  / n_hz - mean_pk  * mean_pk));
    double mean_ce  = n_gt > 0 ? sum_ce / n_gt : 0;
    double std_ce   = n_gt > 0 ? sqrtf((float)(sum_ce2 / n_gt - mean_ce * mean_ce)) : 0;

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"n\":%d,\"n_detected\":%d,\"n_with_gt\":%d,"
        "\"detected_hz\":{\"mean\":%.4f,\"std\":%.4f,\"min\":%.4f,\"max\":%.4f},"
        "\"cents_error\":{\"mean\":%.3f,\"std\":%.3f,\"min\":%.3f,\"max\":%.3f},"
        "\"nsdf_peak_val\":{\"mean\":%.4f,\"std\":%.4f}}",
        n, n_hz, n_gt,
        mean_hz, std_hz, min_hz, max_hz,
        mean_ce, std_ce, n_gt > 0 ? min_ce : 0, n_gt > 0 ? max_ce : 0,
        mean_pk, std_pk);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ---- GET /history -------------------------------------------------------- */
static esp_err_t history_get_handler(httpd_req_t *req)
{
    /* Parse ?n=N query param, default 50, max 200 */
    char qbuf[16] = {0};
    int max_n = 50;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char nval[8] = {0};
        if (httpd_query_key_value(qbuf, "n", nval, sizeof(nval)) == ESP_OK)
            max_n = atoi(nval);
    }
    if (max_n < 1) max_n = 1;
    if (max_n > HISTORY_LEN) max_n = HISTORY_LEN;

    static compact_frame_t hist[HISTORY_LEN];
    int n = test_harness_get_history(hist, max_n);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    char entry[512];
    for (int i = 0; i < n; i++) {
        compact_to_json(&hist[i], entry, sizeof(entry));
        httpd_resp_sendstr_chunk(req, entry);
        if (i < n - 1) httpd_resp_sendstr_chunk(req, ",");
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- GET /ws (WebSocket upgrade) ---------------------------------------- */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — register client fd */
        int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        ESP_LOGI(TAG, "WS client connected fd=%d (total=%d)", fd, s_ws_n);
        return ESP_OK;
    }

    /* Incoming frame from client */
    httpd_ws_frame_t pkt = {.type = HTTPD_WS_TYPE_TEXT};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK || pkt.len == 0) return ret;

    uint8_t *payload = malloc(pkt.len + 1);
    if (!payload) return ESP_ERR_NO_MEM;
    pkt.payload = payload;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    payload[pkt.len] = '\0';

    /* Respond to {"cmd":"snapshot"} with a single full-frame push */
    if (ret == ESP_OK && strstr((char *)payload, "snapshot"))
        httpd_queue_work(s_server, ws_broadcast_cb, NULL);

    free(payload);
    return ret;
}

/* ---- httpd_start_server -------------------------------------------------- */
esp_err_t httpd_start_server(void)
{
    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex) return ESP_ERR_NO_MEM;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err)); return err; }

    httpd_uri_t uris[] = {
        { .uri = "/params",   .method = HTTP_GET,  .handler = params_get_handler  },
        { .uri = "/params",   .method = HTTP_POST, .handler = params_post_handler },
        { .uri = "/snapshot", .method = HTTP_GET,  .handler = snapshot_get_handler },
        { .uri = "/stats",    .method = HTTP_GET,  .handler = stats_get_handler  },
        { .uri = "/history",  .method = HTTP_GET,  .handler = history_get_handler },
        { .uri = "/ws",       .method = HTTP_GET,  .handler = ws_handler,
          .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_server, &uris[i]);

    xTaskCreatePinnedToCore(ws_sender_task, "ws_send", 4096*2, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "HTTP server started (/params /snapshot /stats /history /ws)");
    return ESP_OK;
}

httpd_handle_t httpd_get_server(void) { return s_server; }
```

- [ ] **Step 3: Build**

```bash
idf.py build
```

Expected: Clean build.

- [ ] **Step 4: Flash + verify all endpoints**

```bash
idf.py -p COM5 flash monitor
```

Verify in order:

```bash
# Params still works
curl http://192.168.86.163/params

# Stats (may be zeros initially, wait a few seconds for frames to accumulate)
curl http://192.168.86.163/stats

# History (last 5 frames)
curl "http://192.168.86.163/history?n=5"

# Snapshot (large — piped to file to avoid terminal flood)
curl http://192.168.86.163/snapshot > snapshot.json
```

Expected `/stats` after ~5 seconds (WAV playing):
```json
{"n":10,"n_with_gt":10,"detected_hz":{"mean":...},"cents_error":{"mean":...},...}
```

Expected `/snapshot`: large JSON ending in `...,"nsdf":[0.00000,0.12345,...]}` with 2048 floats.

- [ ] **Step 5: Commit + push**

```bash
git add test_harness/main/httpd.h test_harness/main/httpd.c
git commit -m "feat: test_harness /snapshot /stats /history /ws endpoints + WebSocket push"
git push
```

---

## End State

After Plan 2, the test device:
- Runs the full audio→pitch pipeline with `pitch_detect_full()`
- Tracks ground truth from the WAV sweep schedule
- Exposes `GET /snapshot` — full NSDF + all algorithm internals
- Exposes `GET /stats` — rolling mean/std/min/max for hz, cents_error, confidence
- Exposes `GET /history?n=N` — last N compact frames
- Pushes compact frames via WebSocket at ~10Hz
- All params tunable at runtime via `POST /params`

Plan 3 adds the web dashboard (HTML/JS served from `GET /`).
