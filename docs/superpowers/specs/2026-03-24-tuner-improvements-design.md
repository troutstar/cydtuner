# ESP32 Strobe Tuner Improvements — Design Spec
**Date:** 2026-03-24

## Overview

Three improvements to the strobe tuner firmware: green in-tune color feedback, horizontal cents deviation bar, and a switch from YIN to the McLeod Pitch Method (MPM) for better accuracy. A prerequisite architectural fix (dynamic F_TARGET) threads through all three.

**Existing code not changed by this spec:**
- `pitch_hz_to_note(float hz, char *buf, size_t len)` — defined in `pitch.c`/`pitch.h`
- `s_half` — set in `pitch_init(buf_len)` as `buf_len/2`; equals 2048 with `AUDIO_BUF_SAMPLES=4096`
- In `pitch_detect(buf, len, sample_rate)`, `half = (len/2 < s_half) ? len/2 : s_half`. In this codebase `len` is always `AUDIO_BUF_SAMPLES=4096` and `s_half=2048`, so `half == s_half == 2048` always. The short-buffer path never executes in normal operation.

---

## 1. Dynamic F_TARGET (Prerequisite)

**Existing formula:**
```c
s_phase += 2.0f * (float)M_PI * (detected_hz - F_TARGET) / F_TARGET * K_SPEED * dt;
// F_TARGET = 440.0f (hardcoded #define)
```

**New formula** — structurally identical, `F_TARGET` replaced with `nearest_hz`:
```c
float nearest_hz = pitch_hz_to_nearest_hz(detected_hz);
s_phase += 2.0f * (float)M_PI * (detected_hz - nearest_hz) / nearest_hz * K_SPEED * dt;
```

For A4=440 Hz, `nearest_hz=440` and result is bit-for-bit identical. For all other notes the ring is now stationary when the note is exactly in tune. Remove `#define F_TARGET 440.0f`.

`display.c` must add `#include "pitch.h"` to call the helpers below. This is the only new cross-module include.

In `display_render_strobe()`, call both helpers at the top of the function:
```c
float nearest_hz = pitch_hz_to_nearest_hz(detected_hz);
float cents      = pitch_hz_to_cents(detected_hz);
```

No change to `display_render_strobe(float detected_hz, const char *note)` signature.

---

## 2. Green In-Tune Color Feedback

| State | Threshold | Ring color | RGB565 |
|---|---|---|---|
| In tune | \|cents\| ≤ 5 | Green | `0x07E0` |
| Out of tune | \|cents\| > 5 | White | `0xFFFF` |

Bar uses amber `0xFD20` out-of-tune (section 3) — intentionally different from ring white.

```c
uint16_t col_seg = (fabsf(cents) <= 5.0f) ? 0x07E0u : 0xFFFFu;
```
Replace the `COL_SEG` constant in the ring segment check with `col_seg`. No other ring renderer changes.

---

## 3. Horizontal Cents Deviation Bar

Layout (240×320, top-left origin). All `ili9341_fill_rect(x, y, w, h, color)` calls:

| Step | x | y | w | h | color | condition |
|---|---|---|---|---|---|---|
| 1. Clear | 20 | 274 | 200 | 19 | `0x0000` | always |
| 2. Track | 20 | 280 | 200 | 8 | `0x1082` | always |
| 3. Fill | see below | 280 | fill_w | 8 | green/amber | fill_w > 0 |
| 4. Tick | 119 | 276 | 3 | 16 | `0xFFFF` | always, drawn last |

Fill x and color:
- `fill_w = (int)((fabsf(cents) / 50.0f) * 100.0f)`, capped at 100
- Flat (cents < 0): fill x = `120 - fill_w`
- Sharp (cents > 0): fill x = `120`
- Zero: skip step 3
- Color: `0x07E0` if `fabsf(cents) ≤ 5.0f`, else `0xFD20`

The flat fill ends at pixel x=119; the sharp fill starts at pixel x=120. This 1-pixel asymmetry is intentional — both directions may overlap the tick (x=119–121), but step 4 always redraws the tick last.

---

## 4. MPM Pitch Detection

NSDF derivation: `d(tau) = r0_start + r0_end - 2*m(tau)`, so `nsdf(tau) = 2*m(tau) / (r0_start + r0_end)`. Autocorrelation form used throughout.

### 4a. Hann Window

New static buffer `s_window`, allocated in `pitch_init()`, size `s_half`:
```c
for (size_t i = 0; i < s_half; i++)
    s_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (float)(s_half - 1)));
```

At the top of `pitch_detect()`, pre-apply the window into a local float buffer `wx[half]` (declared on the stack or as a static buffer — see 4e):
```c
for (size_t i = 0; i < half; i++)
    wx[i] = (float)buf[i] * s_window[i];
```

All NSDF computations use `wx[]` only. This eliminates any ambiguity about which window coefficient applies to shifted samples.

### 4b. NSDF (replaces YIN Steps 2 and 3)

Reuse `s_diff[]` → m[], `s_cmnd[]` → nsdf[]. Use `wx[]` from 4a.

Exact C loop structure (naive O(N²); running-sum optimization is future work):

```c
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
```

At tau=0: both inner loops run j=0..half-1, m_val = r0s = r0e = Σwx²; denom = 2Σwx²; nsdf[0] = 1.0f.

### 4c. Key Maximum Selection

```c
// tau_min/tau_max: guitar range 40 Hz – 1200 Hz
// The +1 in tau_min guarantees tau_min >= 1, making s_cmnd[tau-1] always safe.
size_t tau_min = (size_t)(sample_rate / 1200.0f) + 1;  // e.g. 38 at 44100 Hz
size_t tau_max = (size_t)(sample_rate / 40.0f);         // e.g. 1102 at 44100 Hz
if (tau_max >= half) tau_max = half - 1;

float global_max = 0.0f;
for (size_t tau = 0; tau < half; tau++)
    if (s_cmnd[tau] > global_max) global_max = s_cmnd[tau];
float threshold = 0.8f * global_max;

for (size_t tau = tau_min; tau <= tau_max; tau++) {
    float right = (tau + 1 < half) ? s_cmnd[tau + 1] : 0.0f;
    if (s_cmnd[tau] > s_cmnd[tau - 1] &&   // safe: tau >= tau_min >= 1
        s_cmnd[tau] >= right &&
        s_cmnd[tau] > threshold) {
        // parabolic interpolation (section 4d), return sample_rate / bt
    }
}
return 0.0f;
```

Plateau: `>= right` qualifies the first of a run of equal values (lowest tau wins).

### 4d. Parabolic Interpolation

Unchanged from current YIN — same code, applied at the selected tau.

### 4e. Memory

New allocations in `pitch_init()`:
- `s_window = malloc(s_half * sizeof(float))` — precomputed Hann coefficients
- `s_wbuf   = malloc(s_half * sizeof(float))` — working buffer for `wx[]` in `pitch_detect()`

Total static heap: 4 × s_half floats (s_diff + s_cmnd + s_window + s_wbuf = 32 KB at s_half=2048). All four freed on any allocation failure in `pitch_init()`.

### 4f. Fallback

No key maximum found → return `0.0f`.

---

## 5. Enhanced Serial Logging

In `pitch_task` (`main.c`), after `pitch_detect()` returns `hz`:
```c
char note_log[4];
pitch_hz_to_note(hz, note_log, sizeof(note_log));
float cents_log = pitch_hz_to_cents(hz);
ESP_LOGI("pitch", "%.2f Hz  %s  %+.0f cents", hz, note_log, (double)cents_log);
```

`note_log` is local to `pitch_task`, independent of `display_task`'s note buffer.

---

## 6. Helpers: `pitch_hz_to_nearest_hz()` and `pitch_hz_to_cents()`

```c
// pitch.h — add to existing declarations
float pitch_hz_to_nearest_hz(float hz);
float pitch_hz_to_cents(float hz);

// pitch.c
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

---

## Files Changed

| File | Change |
|---|---|
| `main/pitch.c` | Replace YIN with MPM; add `s_window`, `s_wbuf`; add `pitch_hz_to_nearest_hz()`, `pitch_hz_to_cents()` |
| `main/pitch.h` | Add `pitch_hz_to_nearest_hz()`, `pitch_hz_to_cents()` declarations |
| `main/display.c` | Add `#include "pitch.h"`; use `pitch_hz_to_nearest_hz()` for phase, `pitch_hz_to_cents()` for color/bar; dynamic `col_seg`; add `render_bar()`; remove `#define F_TARGET` |
| `main/display.h` | No changes |
| `main/main.c` | Add `pitch_hz_to_note()` + `pitch_hz_to_cents()` calls in `pitch_task` for logging |
| `main/CMakeLists.txt` | No changes |

---

## Acceptance Criteria

1. Ring turns green (`0x07E0`) within ±5 cents of nearest note (sharp or flat)
2. Ring returns white (`0xFFFF`) beyond ±5 cents in either direction
3. Bar fills left (flat) / right (sharp) from center; pins at ±50 cents; no fill at 0 cents
4. Bar fill green ≤5¢, amber >5¢; same ±5¢ threshold as ring
5. Center tick always visible including at ±50 cents fill
6. Serial log: `hz / note / signed-cents` every detection
7. Test: `test_audio/guitar_sweep.wav` (16-bit mono 44100 Hz, from `gen_sweep.py`: 3s hold + 2s glide per string, E2→A2→D3→G3→B3→E4). During each 3s hold, serial log reports detected hz within ±2 cents of: E2=82.41, A2=110.00, D3=146.83, G3=196.00, B3=246.94, E4=329.63 Hz
8. No WDT trips; `pitch_task` yields `vTaskDelay(1)` after `pitch_detect`
9. Stability: 8+ hours on looping sweep WAV — no crashes, no abort messages, display visibly updating at run end
