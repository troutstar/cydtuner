# ESP32 Strobe Tuner

A virtual stroboscopic tuner running on the 2.8-inch ESP32 CYD module (E32R28T). Built with ESP-IDF. Detects pitch in real time and drives a strobe pattern on the ILI9341V display — the pattern is stationary when the note is in tune, rotates clockwise when sharp, and counter-clockwise when flat.

---

## Hardware

**Module:** ESP32-2432S028 ("Cheap Yellow Display") — ESP32-D0WD-V3, 240x320 ILI9341V display, XPT2046 resistive touch, onboard SD card slot, RGB LED, USB-C power.

### Pin Assignments

#### LCD (ILI9341V, SPI2)
| Signal | Pin |
|--------|-----|
| CS | IO15 |
| DC | IO2 |
| CLK | IO14 |
| MOSI | IO13 |
| MISO | IO12 |
| RST | EN (shared with ESP32 reset) |
| Backlight | IO21 (high = on) |

#### Touch (XPT2046, separate SPI bus)
| Signal | Pin |
|--------|-----|
| CLK | IO25 |
| MOSI | IO32 |
| MISO | IO39 |
| CS | IO33 |
| IRQ | IO36 |

#### SD Card (SPI, shared with SPI peripheral header)
| Signal | Pin |
|--------|-----|
| CS | IO5 |
| CLK | IO18 |
| MOSI | IO23 |
| MISO | IO19 |

#### Other
| Function | Pin | Notes |
|----------|-----|-------|
| Audio enable | IO4 | Active low |
| Audio DAC out | IO26 | |
| RGB LED Red | IO22 | Common anode, low = on |
| RGB LED Green | IO16 | Common anode, low = on |
| RGB LED Blue | IO17 | Common anode, low = on |
| Battery ADC | IO34 | Input only |
| BOOT button | IO0 | |

IO35 and IO39 are input-only — never configure as output.

---

## Architecture

Three FreeRTOS tasks communicate via queues:

```
audio_task  →[sample queue]→  pitch_task  →[freq queue]→  display_task
```

- **audio_task** — reads sample buffers from WAV file (dev) or I2S (production)
- **pitch_task** — runs NSDF pitch detection, outputs Hz via queue; sends 0.0 on silence
- **display_task** — consumes detected Hz, updates strobe phase, renders frame to LCD

Tasks are fully decoupled. The audio source is swappable via `audio_source_t` without changes to pitch or display logic.

---

## Display Rendering

- **Strip-based rendering:** the ring is rendered in horizontal strips (8 rows each) to keep DMA allocation small (~3 KB vs ~80 KB for a full-frame buffer)
- **SPI clock:** 40 MHz
- **Strobe pattern:** 36 equal segments, filled/empty alternating. `atan2f` maps each pixel to a segment based on accumulated phase
- **Phase accumulation:** driven by `(detected_hz - ref_hz) / ref_hz`, clamped per frame to `π/N_SEG` (half a segment width) to prevent the wagon-wheel effect
- **Hysteresis reference:** the note label and rotation reference (`s_ref_hz`) snap to a new semitone only when deviation exceeds 65 cents — prevents flickering at the ±50 cent mathematical boundary
- **Silence handling:** when `detected_hz == 0`, the ring and bar are cleared and `s_ref_hz` is reset
- **In-tune indicator:** segments turn green within ±5 cents of the reference

---

## Pitch Detection

NSDF algorithm (Normalised Square Difference Function) operating on `int16_t` sample buffers. Designed for the guitar/bass frequency range (40 Hz–1200 Hz). Target accuracy: ±0.5 cents.

---

## Project Structure

```
/main
  main.c          — app_main, task creation
  audio.c/h       — audio source abstraction (WAV file or I2S)
  pitch.c/h       — NSDF pitch detection
  display.c/h     — strobe rendering, strip blitter
  touch.c/h       — XPT2046 driver
/components
  ili9341/        — ILI9341V SPI display driver
  xpt2046/        — XPT2046 touch driver
/test_harness/    — separate firmware for the validation device (see test_harness/README.md)
/tools/
  gen_tuning_sim.py   — generates tuning_sim.wav: discrete plucks for display validation
  claudesweeps.py     — earlier continuous-sweep generator (reference only)
/test_audio/      — reference WAV files
```

---

## Build and Flash

Requires ESP-IDF 5.5. From the project root:

```bash
idf.py build
idf.py -p COMx flash monitor
```

Audio source during development: copy `tuning_sim.wav` (or any WAV) to the SD card root as `sweep.wav`.

---

## Test Harness

A second CYD module runs separate firmware (`test_harness/`) that connects to WiFi and exposes HTTP endpoints for algorithm validation. See [test_harness/README.md](test_harness/README.md) for full details.

The test harness was built because validating pitch detection accuracy from serial logs alone is too slow — it provides live streaming of every detection frame, algorithm internals (full NSDF curve), ground truth comparison, and runtime parameter tuning, all accessible from any machine on the network.
