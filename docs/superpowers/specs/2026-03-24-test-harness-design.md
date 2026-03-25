# Test Harness Design

**Goal:** Add a WiFi-connected test harness to the cydtuner project that gives deep real-time access to pitch algorithm internals, tunable parameters, and accuracy measurement against known ground truth — all via a browser dashboard and HTTP API.

**Scope:** New subdirectory `test_harness/` within the cydtuner repo. Separate ESP-IDF build target sharing components with the main project. Targets a second ESP32 device on the bench.

---

## Project Structure

```
/test_harness
  main/
    main.c              — app_main, task creation (WiFi + audio + pitch + httpd)
    wifi.c / wifi.h     — station mode init, mDNS
    httpd.c / httpd.h   — HTTP server, REST endpoints, WebSocket handler
    test_harness.c / test_harness.h — shared state: latest frame, ring buffer, params, ground truth
  CMakeLists.txt        — references ../components/ and ../main/ for shared code
  sdkconfig             — separate sdkconfig from main project (tracked in git, private repo)
```

Shared source files compiled directly into the test harness build (not symlinked):
- `../../main/pitch.c` — pitch detection (modified for instrumentation)
- `../../main/audio.c` — WAV/SD audio source (modified to expose position)
- `../../main/display.c`, `../../main/touch.c` — kept for hardware parity

Shared components via `EXTRA_COMPONENT_DIRS`:
- `../components/ili9341/`
- `../components/xpt2046/`

**Constraint:** All modifications to `pitch.c` and `audio.c` must remain backward-compatible with the main tuner build (same file, compiled by both projects).

---

## CMakeLists.txt

`test_harness/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_harness)
```

`test_harness/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.c" "wifi.c" "httpd.c" "test_harness.c"
         "../../main/pitch.c" "../../main/audio.c"
         "../../main/display.c" "../../main/touch.c"
    INCLUDE_DIRS "." "../../main"
)

# Preserve the sdmmc_init_spi_crc linker wrap from audio.c
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=sdmmc_init_spi_crc")
```

The linker wrap is required because `audio.c` defines `__wrap_sdmmc_init_spi_crc` to suppress CMD59 on older SD cards. Without it the wrap is silently ignored and SD card CRC errors can occur.

---

## Required sdkconfig Options

The following non-default options must be set in `test_harness/sdkconfig`:

```
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n        # station only
CONFIG_MDNS_ENABLED=y
CONFIG_HTTPD_WS_SUPPORT=y              # CRITICAL — WebSocket silently non-functional without this
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10       # pitch_detect_full() is heavier than pitch_detect()
```

`CONFIG_HTTPD_WS_SUPPORT=y` is the most critical — without it `esp_http_server` compiles and links but WebSocket upgrade requests fall through as plain HTTP with no error.

---

## WiFi + mDNS

- Station mode: SSID `riotscanner`, passphrase hardcoded in `wifi.c`
- Block in `wifi_init()` until IP assigned (use `EventGroupHandle_t` on `WIFI_CONNECTED_BIT`)
- mDNS hostname: `cydtuner-test` → reachable at `http://cydtuner-test.local`
- Start HTTP server only after IP is assigned
- Required components: `esp_wifi`, `mdns`

---

## Pitch Frame Instrumentation

### `pitch_frame_t`

Defined in `test_harness.h`. Every `pitch_detect_full()` call fills one frame:

```c
typedef struct {
    uint64_t timestamp_us;      // esp_timer_get_time()
    float    detected_hz;       // raw algorithm output (0.0 = no detection)
    float    smooth_hz;         // after EMA smoothing
    char     note[4];           // nearest note name string
    float    cents;             // deviation from nearest note (cents)
    float    ground_truth_hz;   // expected Hz from WAV schedule (0.0 during silence)
    float    cents_error;       // cents(detected_hz vs ground_truth_hz); 0.0 if gt==0
    float    nsdf_peak_val;     // NSDF value at detected peak (confidence 0–1)
    float    nsdf_global_max;   // global max of NSDF array
    float    threshold_used;    // threshold_coeff * nsdf_global_max
    size_t   tau_detected;      // integer lag index of selected peak
    float    tau_interpolated;  // sub-sample lag after parabolic interpolation
    uint16_t nsdf_len;          // = s_half (set by pitch_init); always <= 2048
    float    nsdf[2048];        // full NSDF curve; valid indices [0, nsdf_len)
} pitch_frame_t;
```

`nsdf_len` is set to `s_half` (the value computed at `pitch_init()` time). With `buf_len=4096`, `s_half=2048`. `nsdf_len` must be checked by consumers — do not assume 2048.

**Memory:** `pitch_frame_t` is ~8.2KB. It must NOT be stack-allocated inside `pitch_task` (task stack is 16KB; the struct alone would exceed half). Declare it `static` in `pitch_task` or allocate with `heap_caps_malloc(MALLOC_CAP_INTERNAL)`.

### `pitch_detect_full()`

New function added to `pitch.c` (inside `#ifdef PITCH_TEST_HARNESS` guard so main tuner build is unaffected):

```c
#ifdef PITCH_TEST_HARNESS
float pitch_detect_full(const int16_t *buf, size_t len, float sample_rate,
                        const tuner_params_t *params, pitch_frame_t *frame);
#endif
```

- Runs the NSDF algorithm using `params->{threshold_coeff, pitch_min_hz, pitch_max_hz}` instead of compile-time constants
- Copies `s_cmnd[0..s_half-1]` into `frame->nsdf[]` and sets `frame->nsdf_len = s_half`
- Fills all other frame fields
- Returns detected Hz
- Existing `pitch_detect()` is unchanged

`PITCH_TEST_HARNESS` is defined in `test_harness/main/CMakeLists.txt` via:
```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE PITCH_TEST_HARNESS=1)
```

### `tuner_params_t`

Defined in `test_harness.h`:

```c
typedef struct {
    float threshold_coeff;  // default 0.8
    float pitch_min_hz;     // default 40.0
    float pitch_max_hz;     // default 1200.0
    float smooth_alpha;     // EMA: 0.0=raw, ~0.9=heavy; default 0.0
} tuner_params_t;
```

Stored in `test_harness.c`, protected by `s_params_mutex`. Written by httpd (`POST /params`), read by pitch_task.

---

## Audio Position for Ground Truth

`audio_task` owns the WAV file and is the sole caller of `audio_read()`. `pitch_task` receives a filled buffer via `sample_q` and never touches the file. Calling `ftell()` on `s_wav_file` from `pitch_task` would be a cross-task FATFS access — unsafe.

Fix: `audio_read()` updates a module-level position variable after each read. `audio_get_position_sec()` reads that variable — no file access, no mutex needed (32-bit read/write is atomic on ESP32 Xtensa).

Changes to `audio.c` (inside `#ifdef PITCH_TEST_HARNESS` guard):

```c
static volatile uint32_t s_position_bytes = 0;  // updated by audio_read()

// In audio_read(), after fread succeeds, add:
//   s_position_bytes = (uint32_t)ftell(s_wav_file) - s_data_start;

#ifdef PITCH_TEST_HARNESS
float audio_get_position_sec(void) {
    if (!s_wav_file || s_sample_rate == 0) return 0.0f;
    uint32_t bytes_per_sec = s_sample_rate * s_channels * 2;
    return (float)s_position_bytes / (float)bytes_per_sec;
}
#endif
```

**Lag:** The position reflects where `audio_task` has read to, which is one buffer ahead of the buffer `pitch_task` is currently processing (~93ms at 44100Hz / 4096 samples). For a 12s note this is negligible — ground truth lookup will still return the correct note throughout.

---

## Ground Truth Table

Hardcoded in `test_harness.c` from the `claudesweeps.py` schedule:

```c
#define GT_NOTE_DUR_SEC    12.0f
#define GT_SILENCE_DUR_SEC  0.5f
#define GT_SLOT_SEC        (GT_NOTE_DUR_SEC + GT_SILENCE_DUR_SEC)
#define GT_N_NOTES         17

static const struct { float hz; const char *label; } s_gt_table[GT_N_NOTES] = {
    {  41.20f, "E1"  }, {  55.00f, "A1"  }, {  61.74f, "B1"  },
    {  73.42f, "D2"  }, {  82.41f, "E2"  }, {  92.50f, "F#2" },
    {  98.00f, "G2"  }, { 110.00f, "A2"  }, { 123.47f, "B2"  },
    { 146.83f, "D3"  }, { 164.81f, "E3"  }, { 185.00f, "F#3" },
    { 196.00f, "G3"  }, { 207.65f, "G#3" }, { 246.94f, "B3"  },
    { 277.18f, "C#4" }, { 329.63f, "E4"  },
};
```

`test_harness_ground_truth(float pos_sec)`:
- Computes `slot = (int)(pos_sec / GT_SLOT_SEC)`
- Within-slot offset = `pos_sec - slot * GT_SLOT_SEC`
- If offset < `GT_NOTE_DUR_SEC` and slot < `GT_N_NOTES`: returns `s_gt_table[slot].hz`
- Otherwise returns `0.0f` (silence gap or past end → excluded from accuracy stats)

---

## Shared State (`test_harness.c`)

```c
// Latest full frame — written by pitch_task, read by httpd REST + WebSocket notifier
static pitch_frame_t     s_latest_frame;   // static (not stack), ~8.2KB
static SemaphoreHandle_t s_frame_mutex;

// Compact ring buffer — written by pitch_task, read by httpd /stats and /history
#define HISTORY_LEN 200
static compact_frame_t   s_history[HISTORY_LEN];
static int               s_history_head;
static SemaphoreHandle_t s_history_mutex;

// Tunable params
static tuner_params_t    s_params;
static SemaphoreHandle_t s_params_mutex;

// WebSocket notification: pitch_task signals a sender task via queue
static QueueHandle_t     s_ws_notify_q;
```

`compact_frame_t` = `pitch_frame_t` without `nsdf[]` (~80 bytes vs ~8.2KB).

---

## WebSocket Push Architecture

`esp_http_server` is not thread-safe for sending from external task contexts. The correct pattern uses a dedicated sender task and `httpd_queue_work()`.

**Design:**
- `pitch_task` calls `test_harness_post_frame()` after each detection
- `test_harness_post_frame()` updates `s_latest_frame` (mutex), appends to ring buffer, then sends a signal to `s_ws_notify_q`
- A dedicated `ws_sender_task` blocks on `s_ws_notify_q`, then calls `httpd_queue_work(server, ws_broadcast_cb, NULL)` to schedule the broadcast within the httpd task context
- `ws_broadcast_cb` serializes `compact_frame_t` to JSON and calls `httpd_ws_send_frame_async()` for each connected WebSocket client fd
- `ws_broadcast_cb` must check the return value per fd: on error (`ESP_ERR_HTTPD_INVALID_REQ` or similar), remove the stale fd from the client list. Without this, the client list grows unbounded on repeated connect/disconnect cycles.

Rate limiting: `ws_sender_task` drains `s_ws_notify_q` but caps sends at ~10Hz using `esp_timer_get_time()`.

---

## HTTP API

Server started after WiFi IP assigned. All handlers registered on a single `httpd_handle_t`.

### REST Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/` | Dashboard HTML (embedded C string) |
| `GET` | `/snapshot` | Full `pitch_frame_t` as chunked JSON (includes NSDF array) |
| `GET` | `/params` | Current `tuner_params_t` as JSON |
| `POST` | `/params` | Update params; partial JSON accepted |
| `GET` | `/stats` | Rolling stats over last 200 readings |
| `GET` | `/history` | Last N compact frames; `?n=50` (default 50, max 200) |

### `/snapshot` — chunked response

The full snapshot JSON is ~20KB (2048 NSDF floats × ~9 bytes each). `esp_http_server`'s default send buffer is 4KB. Use `httpd_resp_send_chunk()`:

1. Send header fields as first chunk (all non-nsdf fields)
2. Stream `nsdf[]` array in chunks of ~512 floats at a time
3. Send closing `]}` chunk
4. Call `httpd_resp_send_chunk(req, NULL, 0)` to terminate

### WebSocket — `/ws`

- Registered as a WebSocket URI handler (`is_websocket = true`)
- Server pushes compact frame JSON at ~10Hz via `ws_sender_task` + `httpd_queue_work()`
- Client sends `{"cmd":"snapshot"}` → server responds with one full frame JSON (chunked) on the same socket

### JSON formats

Compact frame (WebSocket push, `/history`):
```json
{
  "ts_us": 12345678,
  "detected_hz": 440.12,
  "smooth_hz": 440.09,
  "note": "A",
  "cents": 0.47,
  "ground_truth_hz": 440.00,
  "cents_error": 0.47,
  "nsdf_peak_val": 0.94,
  "nsdf_global_max": 0.97,
  "threshold_used": 0.776,
  "tau_detected": 100,
  "tau_interpolated": 100.23,
  "nsdf_len": 2048
}
```

Full snapshot (`/snapshot`, WebSocket `snapshot` command) adds:
```json
  "nsdf": [0.0, 0.12, ...]
```

`/params`:
```json
{ "threshold_coeff": 0.8, "pitch_min_hz": 40.0, "pitch_max_hz": 1200.0, "smooth_alpha": 0.0 }
```

`/stats`:
```json
{
  "n": 200,
  "detected_hz": { "mean": 440.1, "std": 0.3, "min": 439.5, "max": 440.8 },
  "cents_error":  { "mean": 0.5,  "std": 1.2, "min": -3.1,  "max": 4.0  },
  "nsdf_peak_val":{ "mean": 0.93, "std": 0.02 }
}
```

---

## Web Dashboard

Single HTML file embedded as a C string literal in `httpd.c`. Three panels:

**Live panel:**
- Hz / note / cents readout (updated from WebSocket compact frames at ~10Hz)
- NSDF curve on `<canvas>` (fetched via WebSocket `snapshot` command at 1–2Hz)
- Threshold line overlaid on NSDF chart
- Peak confidence gauge

**Accuracy panel:**
- Rolling mean cents error and std dev (polled from `/stats` every 2s)
- History chart: detected Hz vs ground truth over last 200 readings
- Ground truth indicator: current note name from sweep schedule

**Params panel:**
- Sliders: `threshold_coeff` (0.1–1.0), `smooth_alpha` (0.0–0.99)
- Number inputs: `pitch_min_hz`, `pitch_max_hz`
- `POST /params` on change, debounced 200ms

JavaScript: `fetch()` for REST, `WebSocket` for live data, Chart.js from CDN, no build step.

---

## Data Flow

```
audio_task (core 0)
  audio_read() → updates s_position_bytes → sends buf ptr to sample_q

sample_q → pitch_task (core 1)
              pitch_detect_full(buf) + audio_get_position_sec() [reads s_position_bytes]
                      │
              test_harness_post_frame(pitch_frame_t*)
              ┌───────┴──────────────────────┐
         freq_q            s_latest_frame + s_history + s_ws_notify_q
              │                                     │
        display_task                      ws_sender_task (core 0)
                                          httpd_queue_work()
                                                │
                                    httpd internal tasks
                                    /snapshot /stats /history /params /ws
```

---

## Task Architecture

- `audio_task` (core 0, pri 5) — unchanged from main tuner
- `pitch_task` (core 1, pri 4) — calls `pitch_detect_full()`, posts to test_harness
- `display_task` (core 0, pri 3) — unchanged; disable via `#define ENABLE_DISPLAY 0` if no LCD
- `ws_sender_task` (core 0, pri 2) — drains `s_ws_notify_q`, triggers httpd broadcasts
- `esp_http_server` internal tasks — managed by IDF

Pitch task stack: increase to `4096*6` (24KB) to accommodate the static `pitch_frame_t` local plus algorithm working memory.

---

## GitHub

`test_harness/` directory committed to the `cydtuner` repo alongside the main firmware. `test_harness/sdkconfig` tracked (WiFi credentials in a private repo is acceptable). Build artifacts (`test_harness/build/`) excluded by existing `.gitignore` `build/` rule.
