# Tuner Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add green in-tune ring color, a horizontal cents deviation bar, and replace the YIN pitch detector with MPM for better accuracy.

**Architecture:** Four files change. `pitch.c` gets the new MPM algorithm plus two new helper functions. `display.c` consumes those helpers for dynamic ring color and a new cents bar. `main.c` gets enhanced serial logging. All changes layer cleanly — helpers first, algorithm second, display third, logging last.

**Tech Stack:** ESP-IDF v5.4, FreeRTOS, C, ILI9341V SPI display (240×320 RGB565), ESP32 hardware FPU

---

## File Map

| File | What changes |
|---|---|
| `main/pitch.h` | Add `pitch_hz_to_nearest_hz()` and `pitch_hz_to_cents()` declarations |
| `main/pitch.c` | Add helpers; add `s_window`/`s_wbuf` buffers; replace YIN with MPM in `pitch_detect()` |
| `main/display.c` | Add `#include "pitch.h"`; dynamic phase reference and ring color; add `render_bar()`; remove `#define F_TARGET` |
| `main/main.c` | Enhanced `ESP_LOGI` in `pitch_task` |

**Spec:** `docs/superpowers/specs/2026-03-24-tuner-improvements-design.md`

---

## Task 1: Add pitch helpers to pitch.h and pitch.c

**Files:**
- Modify: `main/pitch.h`
- Modify: `main/pitch.c`

These two functions are used by `display.c` and `main.c`. Add them first so later tasks can build on them.

- [ ] **Step 1: Add declarations to pitch.h**

Open `main/pitch.h`. After the existing declarations, add:

```c
float pitch_hz_to_nearest_hz(float hz);
float pitch_hz_to_cents(float hz);
```

Full file after edit:
```c
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t pitch_init(size_t buf_len);
float pitch_detect(const int16_t *buf, size_t len, float sample_rate);
void pitch_hz_to_note(float hz, char *buf, size_t len);
float pitch_hz_to_nearest_hz(float hz);
float pitch_hz_to_cents(float hz);
```

- [ ] **Step 2: Add implementations to pitch.c**

In `main/pitch.c`, add the two functions immediately after `pitch_hz_to_note()` (after line 21, before the static buffer declarations):

```c
float pitch_hz_to_nearest_hz(float hz) {
    if (hz < 20.0f) return 440.0f;
    int midi = (int)roundf(69.0f + 12.0f * log2f(hz / 440.0f));
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}

float pitch_hz_to_cents(float hz) {
    if (hz < 20.0f) return 0.0f;
    return 1200.0f * log2f(hz / pitch_hz_to_nearest_hz(hz));
}
```

Note: `powf` requires `<math.h>` which is already included.

- [ ] **Step 3: Build to verify**

```
idf.py build
```
Expected: compiles with no errors.

- [ ] **Step 4: Commit**

```bash
git add main/pitch.h main/pitch.c
git commit -m "feat: add pitch_hz_to_nearest_hz and pitch_hz_to_cents helpers"
```

---

## Task 2: Add MPM buffers to pitch_init()

**Files:**
- Modify: `main/pitch.c`

Add `s_window` (precomputed Hann coefficients) and `s_wbuf` (working buffer for windowed samples). No changes to `pitch_detect()` yet — just allocation and initialization.

- [ ] **Step 1: Add static buffer declarations**

In `main/pitch.c`, the current static declarations are:
```c
static float *s_diff = NULL;
static float *s_cmnd = NULL;
static size_t s_half  = 0;
```

Add two more lines immediately after:
```c
static float *s_window = NULL;
static float *s_wbuf   = NULL;
```

- [ ] **Step 2: Update pitch_init() to allocate and precompute**

Replace the entire `pitch_init()` function body:

```c
esp_err_t pitch_init(size_t buf_len) {
    s_half   = buf_len / 2;
    s_diff   = malloc(s_half * sizeof(float));
    s_cmnd   = malloc(s_half * sizeof(float));
    s_window = malloc(s_half * sizeof(float));
    s_wbuf   = malloc(s_half * sizeof(float));
    if (!s_diff || !s_cmnd || !s_window || !s_wbuf) {
        free(s_diff);   free(s_cmnd);
        free(s_window); free(s_wbuf);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s_half; i++)
        s_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (float)(s_half - 1)));
    ESP_LOGI(TAG, "init OK, half=%u", (unsigned)s_half);
    return ESP_OK;
}
```

- [ ] **Step 3: Build to verify**

```
idf.py build
```
Expected: compiles with no errors.

- [ ] **Step 4: Commit**

```bash
git add main/pitch.c
git commit -m "feat: add MPM window and working buffers to pitch_init"
```

---

## Task 3: Replace YIN algorithm with MPM in pitch_detect()

**Files:**
- Modify: `main/pitch.c`

Replace the entire body of `pitch_detect()`. The function signature is unchanged. The old YIN Steps 2–5 are replaced by: Hann windowing → NSDF → key maximum selection. Step 6 (parabolic interpolation) is kept verbatim.

- [ ] **Step 1: Replace pitch_detect() body**

Replace everything inside `pitch_detect()` with:

```c
float pitch_detect(const int16_t *buf, size_t len, float sample_rate) {
    size_t half = (len / 2 < s_half) ? len / 2 : s_half;

    /* Pre-apply Hann window into working buffer */
    float *wx = s_wbuf;
    for (size_t i = 0; i < half; i++)
        wx[i] = (float)buf[i] * s_window[i];

    /* NSDF: s_diff[] = m[] (autocorrelation), s_cmnd[] = nsdf[] */
    for (size_t tau = 0; tau < half; tau++) {
        float m_val = 0.0f, r0s = 0.0f, r0e = 0.0f;
        for (size_t j = 0; j < half - tau; j++) {
            m_val += wx[j] * wx[j + tau];
            r0s   += wx[j] * wx[j];
        }
        for (size_t j = tau; j < half; j++) {
            r0e += wx[j] * wx[j];
        }
        float denom = r0s + r0e;
        s_diff[tau] = m_val;
        s_cmnd[tau] = (denom > 0.0f) ? 2.0f * m_val / denom : 0.0f;
    }

    size_t tau_min = (size_t)(sample_rate / 1200.0f) + 1;
    size_t tau_max = (size_t)(sample_rate / 40.0f);
    if (tau_max >= half) tau_max = half - 1;

    /* Global max for key maximum threshold */
    float global_max = 0.0f;
    for (size_t tau = 0; tau < half; tau++)
        if (s_cmnd[tau] > global_max) global_max = s_cmnd[tau];
    float threshold = 0.8f * global_max;

    /* Key maximum selection: first local max above threshold in guitar range */
    for (size_t tau = tau_min; tau <= tau_max; tau++) {
        float right = (tau + 1 < half) ? s_cmnd[tau + 1] : 0.0f;
        if (s_cmnd[tau] > s_cmnd[tau - 1] &&   /* tau_min >= 1, so [tau-1] is safe */
            s_cmnd[tau] >= right &&
            s_cmnd[tau] > threshold) {
            /* Step 6: parabolic interpolation */
            float bt = (float)tau;
            if (tau > 0 && tau < half - 1) {
                float s0 = s_cmnd[tau-1], s1 = s_cmnd[tau], s2 = s_cmnd[tau+1];
                float denom2 = 2.0f * (2.0f * s1 - s2 - s0);
                if (fabsf(denom2) > FLT_EPSILON) bt = (float)tau + (s2 - s0) / denom2;
            }
            return sample_rate / bt;
        }
    }
    return 0.0f;
}
```

- [ ] **Step 2: Build to verify**

```
idf.py build
```
Expected: compiles with no errors or warnings.

- [ ] **Step 3: Flash and verify pitch accuracy**

```
idf.py -p /dev/ttyUSB0 flash monitor
```

Play `test_audio/guitar_sweep.wav` (via the audio task). Watch serial output during each 3-second hold. Expected readings:

| String | Target Hz | Acceptable range |
|---|---|---|
| E2 | 82.41 | 82.29–82.53 (±2¢) |
| A2 | 110.00 | 109.87–110.13 |
| D3 | 146.83 | 146.66–147.00 |
| G3 | 196.00 | 195.77–196.23 |
| B3 | 246.94 | 246.65–247.23 |
| E4 | 329.63 | 329.25–330.01 |

Serial log at this point still shows `%.2f Hz` (no note/cents yet — that's Task 7). If readings are outside range, do not proceed; investigate MPM tuning before continuing.

- [ ] **Step 4: Commit**

```bash
git add main/pitch.c
git commit -m "feat: replace YIN with MPM pitch detection (NSDF + key maximum + Hann window)"
```

---

## Task 4: Update display.c — dynamic phase reference and ring color

**Files:**
- Modify: `main/display.c`

Three changes in one task: (1) add pitch.h include, (2) replace hardcoded F_TARGET with dynamic nearest_hz, (3) make ring color depend on cents.

- [ ] **Step 1: Add #include and remove F_TARGET**

At the top of `main/display.c`, add `#include "pitch.h"` after the existing includes:

```c
#include "display.h"
#include "ili9341.h"
#include "esp_timer.h"
#include "pitch.h"
#include <math.h>
#include <string.h>
```

Remove this line from the `#define` block:
```c
#define F_TARGET    440.0f
```

Leave `COL_SEG 0xFFFF` in place — it is still used for note glyph text color.

- [ ] **Step 2: Update display_render_strobe() — phase and ring color**

At the top of `display_render_strobe()`, the current code is:

```c
    s_phase += 2.0f * (float)M_PI * (detected_hz - F_TARGET) / F_TARGET * K_SPEED * dt;
    s_phase = fmodf(s_phase, 2.0f * (float)M_PI);
```

Replace with:

```c
    float nearest_hz = pitch_hz_to_nearest_hz(detected_hz);
    float cents      = pitch_hz_to_cents(detected_hz);

    s_phase += 2.0f * (float)M_PI * (detected_hz - nearest_hz) / nearest_hz * K_SPEED * dt;
    s_phase = fmodf(s_phase, 2.0f * (float)M_PI);

    uint16_t col_seg = (fabsf(cents) <= 5.0f) ? 0x07E0u : 0xFFFFu;
```

Then find the single line inside the ring scan that uses `COL_SEG` for the segment color:

```c
                col = (rel < lit_span) ? COL_SEG : COL_BG;
```

Replace with:

```c
                col = (rel < lit_span) ? col_seg : COL_BG;
```

Do NOT change the other `COL_SEG` usage (in the glyph rendering branch) — that one stays white.

- [ ] **Step 3: Build to verify**

```
idf.py build
```
Expected: compiles with no errors.

- [ ] **Step 4: Commit**

```bash
git add main/display.c
git commit -m "feat: dynamic F_TARGET and green/white ring color based on cents deviation"
```

---

## Task 5: Add cents deviation bar to display.c

**Files:**
- Modify: `main/display.c`

Add `render_bar()` and call it at the end of `display_render_strobe()`.

- [ ] **Step 1: Add render_bar() before display_render_strobe()**

Insert this static function in `main/display.c` immediately before `display_render_strobe()`:

```c
static void render_bar(float cents) {
    int fill_w = (int)((fabsf(cents) / 50.0f) * 100.0f);
    if (fill_w > 100) fill_w = 100;
    int fill_x = (cents < 0.0f) ? (120 - fill_w) : 120;
    uint16_t fill_col = (fabsf(cents) <= 5.0f) ? 0x07E0u : 0xFD20u;

    ili9341_fill_rect(20,  274, 200, 19, 0x0000);               /* clear */
    ili9341_fill_rect(20,  280, 200,  8, 0x1082);               /* track */
    if (fill_w > 0)
        ili9341_fill_rect((uint16_t)fill_x, 280,
                          (uint16_t)fill_w, 8, fill_col);       /* fill */
    ili9341_fill_rect(119, 276,   3, 16, 0xFFFF);               /* tick, last */
}
```

- [ ] **Step 2: Call render_bar() at the end of display_render_strobe()**

`display_render_strobe()` currently ends after the row scan loop (after the `ili9341_draw_bitmap` call closes). Add one line before the closing brace of the function:

```c
    render_bar(cents);
```

Note: `cents` is already declared earlier in the function (added in Task 4 Step 2).

- [ ] **Step 3: Build to verify**

```
idf.py build
```
Expected: compiles with no errors.

- [ ] **Step 4: Flash and verify display**

```
idf.py -p /dev/ttyUSB0 flash monitor
```

With the sweep WAV playing, observe the display:

- During each 3s hold: ring should be green when the note is close to target pitch, white when off
- Bar below ring: amber fill drifts left/right as pitch varies; snaps to no-fill + green ring when exactly in tune
- Center tick always visible
- No flicker or tearing

- [ ] **Step 5: Commit**

```bash
git add main/display.c
git commit -m "feat: add horizontal cents deviation bar to display"
```

---

## Task 6: Enhanced logging in pitch_task

**Files:**
- Modify: `main/main.c`

Update `pitch_task` to log note name and cents alongside Hz.

- [ ] **Step 1: Update pitch_task logging**

In `main/main.c`, find `pitch_task`. The current log line is:

```c
            ESP_LOGI("pitch", "%.2f Hz", hz);
```

Replace the block from `xSemaphoreGive` to `xQueueOverwrite` with:

```c
            vTaskDelay(1); /* 1 tick (10ms @ 100Hz) lets IDLE1 run and reset WDT */
            xSemaphoreGive(s_buf_sem);
            char note_log[4];
            pitch_hz_to_note(hz, note_log, sizeof(note_log));
            float cents_log = pitch_hz_to_cents(hz);
            ESP_LOGI("pitch", "%.2f Hz  %s  %+.0f cents", hz, note_log, (double)cents_log);
            if (hz > 0.0f) last = hz;
            xQueueOverwrite(s_freq_q, &last);
```

The full updated `pitch_task` for reference:

```c
static void pitch_task(void *arg) {
    float sr = (float)audio_get_sample_rate();
    float last = 440.0f;
    for (;;) {
        int16_t *buf = NULL;
        if (xQueueReceive(s_sample_q, &buf, portMAX_DELAY) == pdTRUE) {
            float hz = pitch_detect(buf, AUDIO_BUF_SAMPLES, sr);
            vTaskDelay(1); /* 1 tick (10ms @ 100Hz) lets IDLE1 run and reset WDT */
            xSemaphoreGive(s_buf_sem);
            char note_log[4];
            pitch_hz_to_note(hz, note_log, sizeof(note_log));
            float cents_log = pitch_hz_to_cents(hz);
            ESP_LOGI("pitch", "%.2f Hz  %s  %+.0f cents", hz, note_log, (double)cents_log);
            if (hz > 0.0f) last = hz;
            xQueueOverwrite(s_freq_q, &last);
        }
    }
}
```

- [ ] **Step 2: Build to verify**

```
idf.py build
```
Expected: compiles with no errors.

- [ ] **Step 3: Flash and verify logging**

```
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected serial output during sweep WAV playback:
```
I (pitch): 329.63 Hz  E  +2 cents
I (pitch): 82.41 Hz   E  -1 cents
I (pitch): 110.00 Hz  A  +0 cents
```

Verify:
- Note letter is correct for each string
- Cents sign is correct (+ for sharp, - for flat)
- Values are stable (not jumping between octaves) during the 3s holds

- [ ] **Step 4: Commit**

```bash
git add main/main.c
git commit -m "feat: enhanced pitch logging with note name and cents deviation"
```

---

## Task 7: Integration and stability test

**Files:** none (verification only)

- [ ] **Step 1: Full acceptance test with sweep WAV**

Flash the device and run `idf.py monitor`. Play `test_audio/guitar_sweep.wav` (looping). Verify each acceptance criterion:

| AC | What to check |
|---|---|
| 1 | Ring turns green during in-tune portions of each 3s hold |
| 2 | Ring is white when pitch is off |
| 3 | Bar fills left when flat, right when sharp; empty at exact pitch |
| 4 | Bar color matches ring transitions at same moment |
| 5 | Center tick visible at all times including max bar fill |
| 6 | Serial log shows `hz / note / ±cents` every line |
| 7 | Each string detected within ±2¢ (see table in Task 3 Step 3) |
| 8 | No WDT messages in serial output |

- [ ] **Step 2: Overnight stability run**

Leave device running with sweep WAV looping. Return after 8+ hours. Check serial log for:
- Uninterrupted pitch log lines (no gaps longer than a few seconds)
- No `E (...)` ESP_ERROR lines
- No abort/panic messages
- Display still updating (visually confirm)

- [ ] **Step 3: Done**

If all checks pass, all tasks are complete.
