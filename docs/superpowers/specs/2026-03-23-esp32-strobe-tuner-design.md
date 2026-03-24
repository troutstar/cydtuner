# ESP32 Strobe Tuner — Setup & Implementation Design

**Date:** 2026-03-23
**Approach:** Hardware-first, layer by layer (Option A)

---

## 1. Environment Setup

ESP-IDF v5.4 installed manually to `C:\esp\esp-idf` via git clone with `--recursive`. Toolchain and Python dependencies installed via `install.bat`. Environment activated per-session via `export.sh` for Git Bash sessions, setting `IDF_PATH` and `PATH` so `idf.py` is available.

After install, project target set to `esp32` via `idf.py set-target esp32` in `Y:\Claudetron`.

---

## 2. Project Scaffold

Full directory structure created per CLAUDE.md. All source files start with complete header interfaces and minimal stub bodies that compile clean. `main.c` creates three FreeRTOS tasks (audio, pitch, display) as stubs. Goal: `idf.py build` succeeds with zero errors before any real implementation.

```
Y:\Claudetron\
  main/
    main.c
    audio.c / audio.h
    pitch.c / pitch.h
    display.c / display.h
    touch.c / touch.h
    CMakeLists.txt
  components/
    ili9341/
      ili9341.c / ili9341.h
      CMakeLists.txt
    xpt2046/
      xpt2046.c / xpt2046.h
      CMakeLists.txt
  partitions.csv       — custom partition table (app + SPIFFS)
  CMakeLists.txt
  sdkconfig
  test_audio/
```

### Partition Table

A custom `partitions.csv` is required for SPIFFS (WAV file storage). `sdkconfig` must set `CONFIG_PARTITION_TABLE_CUSTOM=y` pointing to `partitions.csv`. The `esp_spiffs` component must be listed as a dependency in `main/CMakeLists.txt`.

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x6000
phy_init, data, phy,     0xf000,  0x1000
factory,  app,  factory, 0x10000, 1500K
spiffs,   data, spiffs,  ,        500K
```

The SPIFFS offset is intentionally blank — ESP-IDF calculates it automatically from the preceding entry.

---

## 3. Implementation Layers

### Layer 1 — ILI9341V Display Driver

**Location:** `components/ili9341/`

Full SPI init on dedicated bus. **SPI clock: 26 MHz** (ESP32 APB clock 80 MHz / 3 ≈ 26.67 MHz — the ILI9341V datasheet specifies 10 MHz as its rated maximum but these controllers routinely operate reliably at 26–40 MHz on ESP32. Higher clock directly improves frame rate. If display corruption or SPI errors appear during validation, drop to 10 MHz to isolate signal integrity, then step back up).

| Signal    | Pin  |
|-----------|------|
| CS        | IO15 |
| DC        | IO2  |
| CLK       | IO14 |
| MOSI      | IO13 |
| MISO      | IO12 |
| Backlight | IO21 (high = on) |

Sends ILI9341 init command sequence. Public API:

```c
esp_err_t ili9341_init(void);
esp_err_t ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
esp_err_t ili9341_draw_pixel(uint16_t x, uint16_t y, uint16_t color);  // debug only, not used in rendering
```

**Validation:** Log SPI transaction completion and confirm no ESP-IDF SPI errors on serial monitor first. Then fill screen solid red and confirm visually. If display does not light up: check serial for SPI errors, verify CS/DC polarity, verify clock speed before assuming hardware fault.

---

### Layer 2 — XPT2046 Touch Driver

**Location:** `components/xpt2046/`

Second independent SPI bus. **SPI clock: 2 MHz** (XPT2046 maximum).

| Signal | Pin  |
|--------|------|
| CS     | IO33 |
| CLK    | IO25 |
| MOSI   | IO32 |
| MISO   | IO39 |
| IRQ    | IO36 |

Note: IO39 is input-only — used as MISO, which is correct.

Public API:

```c
esp_err_t xpt2046_init(void);
bool xpt2046_read(int *x, int *y, int *pressure);
```

`xpt2046_read()` returns `true` if a touch event is active, `false` if not touched. It polls the IRQ pin (IO36) internally — does not block. If IRQ is not asserted, returns `false` immediately without performing an SPI transaction. Coordinates are only valid when return value is `true`.

**Touch in task architecture:** Touch input is deferred — not wired into any FreeRTOS task in this design. The driver is implemented and validated standalone in Layer 2. `f_target` is a compile-time constant in this version (see Layer 5). Touch-based note selection is a future feature.

**Validation:** Log raw touch coordinates to serial monitor when screen is pressed; confirm `false` return when not touching.

---

### Layer 3 — Audio Abstraction

**Location:** `main/audio.c`

Implements the CLAUDE.md interface plus one extension:

```c
esp_err_t audio_init(audio_source_t source);
int audio_read(int16_t *buf, size_t len);
uint32_t audio_get_sample_rate(void);  // extension to CLAUDE.md interface
```

`audio_source_t`: `AUDIO_SOURCE_WAV_FILE`, `AUDIO_SOURCE_I2S`

`audio_get_sample_rate()` is an intentional addition beyond the two functions specified in CLAUDE.md. It is needed so the pitch task can pass the correct sample rate to `pitch_detect()` without hardcoding it. Future I2S implementation will also set this via `audio_init()`.

`audio_init()` opens the WAV file from SPIFFS, parses the WAV header entirely (format chunk, sample rate, bit depth, channel count), validates compatibility (16-bit PCM, mono or stereo), and stores the sample rate internally. `audio_read()` only streams sample data — no header logic.

`audio_read()` fills `buf` with up to `len` mono `int16_t` samples. If the source WAV is stereo, downmixing (average of L+R) is done using a static internal buffer of `2 × AUDIO_BUF_SAMPLES` `int16_t` values (16 KB, allocated once at module scope — not stack, not heap). Returns samples read, 0 at end of file, negative on error.

**Buffer size commitment:** 4096 samples. At 44.1 kHz: ~93 ms per buffer, covers the 40 Hz lower bound. Peak heap cost per audio buffer: 8 KB.

**Validation:** Log sample rate, bit depth, and first 10 sample values after `audio_init()`. Confirm non-zero values and expected sample rate.

---

### Layer 4 — YIN Pitch Detection

**Location:** `main/pitch.c`

YIN algorithm on `int16_t` buffers. Output: `float` Hz.

- Frequency range: 40–1200 Hz
- Accuracy target: ±0.5 cents
- Buffer size: fixed at 4096 samples

Public API:

```c
esp_err_t pitch_init(size_t buf_len);
float pitch_detect(const int16_t *buf, size_t len, float sample_rate);
```

`pitch_init(buf_len)` allocates two internal float working arrays of size `buf_len / 2` each (difference function and cumulative mean normalized difference). Called with `buf_len = 4096`. `pitch_detect()` must be called with the same `len` that was passed to `pitch_init()` — undefined behavior otherwise. YIN threshold: 0.15.

`pitch_detect()` returns detected Hz, or `0.0f` if no pitch found (silence or confidence below threshold).

**Validation:** Embed a 440 Hz A4 sine WAV in SPIFFS. Feed through `pitch_detect()` and log result. Confirm within ±0.5 cents (439.87–440.13 Hz).

---

### Layer 5 — FreeRTOS Task Wiring + Strobe Rendering

**Location:** `main/main.c`, `main/display.c`

Three tasks communicating via FreeRTOS queues:

```
audio_task → [sample_queue] → pitch_task → [freq_queue] → display_task
```

**Buffer pool and ownership:**

Two static audio buffers of 4096 `int16_t` each (16 KB total). A FreeRTOS counting semaphore (`buf_sem`, initial count = 2) guards availability. Protocol:

1. `audio_task` takes `buf_sem` (waits until a buffer is free), fills it, posts its pointer to `sample_queue` (depth 1)
2. `pitch_task` receives pointer from `sample_queue`, processes it, gives `buf_sem` back — making the buffer available for reuse
3. If `sample_queue` is full when audio task tries to post (pitch task is behind), audio task drops the current buffer and gives `buf_sem` back immediately

This prevents any race between audio writing and pitch reading.

**Task stack sizes and priorities:**

Stack depth values are in words (4 bytes each on Xtensa LX6) — pass directly to `xTaskCreate()`. The stereo downmix buffer in `audio_read()` is a static module-level allocation, so stack requirements are standard across all tasks.

| Task          | Stack (words) | Priority |
|---------------|---------------|----------|
| audio_task    | 4096          | 5        |
| pitch_task    | 4096          | 4        |
| display_task  | 4096          | 3        |

**`freq_queue` and 0.0f handling:**

`freq_queue` depth 1, element type `float`. `display_task` maintains `last_freq` (initialized to 440.0f). On receive: if value > 0.0f, update `last_freq`. If value == 0.0f (silence/no pitch), keep `last_freq` unchanged — do not freeze or stop rendering. This prevents a silent input from corrupting the strobe phase calculation.

**Strobe rendering:**

N = 12 arc segments arranged in a circle. `f_target = 440.0f` Hz (compile-time constant for this version).

Phase update per frame:

```
dt = time since last frame in seconds  (measured via esp_timer_get_time())
φ += 2π × (f_detected - f_target) / f_target × k × dt
φ = fmod(φ, 2π)
```

- `k` = 1.0f (visual rotation speed constant, tunable)
- First frame: `dt` = 0.033f (assume 30 fps)
- `f_detected` = `last_freq` from `freq_queue` logic above

Each segment `i` drawn at angular position `(2π × i / N) + φ` using `ili9341_fill_rect()` to approximate arc segments.

At pitch match: `f_detected == f_target` → `φ` unchanged → pattern stationary.
Sharp: `φ` increases (clockwise rotation). Flat: `φ` decreases (counter-clockwise).

**Validation:** With 440 Hz WAV, pattern stationary. Set `f_target = 441.0f` at compile time, confirm slow rotation.

---

## 4. Pin Constraint Summary

- Display SPI clock: 26 MHz (fallback 10 MHz if corruption). Touch SPI clock: 2 MHz. Buses are separate.
- IO35 and IO39 are **input-only** — never configure as output (IO39 = XPT2046 MISO, correct)
- RGB LEDs are **active low** (common anode) — not used in this design
- Audio enable IO4 is **active low** — not used in this design (no I2S yet)
- EN pin shared with LCD RST — LCD reset follows system reset

---

## 5. Mandatory Procedures

**Prerequisites before flashing:**
- Confirm ESP-IDF v5.4 is installed and `idf.py` is on PATH
- Confirm `partitions.csv` exists in the project root before any flash operation

**Partition table risk — Low to Moderate:**
Introducing a custom `partitions.csv` requires a full flash erase before the first flash with the new layout: `idf.py erase-flash`. This wipes all existing data on the chip. Always run `idf.py build` first as a dry-run to confirm the partition table compiles without error before erasing.

**Build-before-flash discipline:** For every layer, run `idf.py build` and confirm zero errors before running `idf.py flash`. Do not skip the build step.

---

## 6. Success Criteria

1. `idf.py build` passes clean after scaffold
2. SPI transactions logged clean, screen fills red after Layer 1
3. Touch coordinates print to serial, `false` returned when not touching after Layer 2
4. Audio buffer populated with correct sample rate and non-zero PCM data after Layer 3
5. 440 Hz detected within ±0.5 cents after Layer 4
6. Strobe pattern visible and stationary on 440 Hz input, rotating on pitch deviation after Layer 5
