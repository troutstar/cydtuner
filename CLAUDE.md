# Project: ESP32 Strobe Tuner

Embedded firmware for a virtual stroboscopic tuner running on the 2.8inch ESP32-32E display module (E32R28T). Built with ESP-IDF. Target is real-time pitch detection driving a strobe pattern on the ILI9341V display.

## Stack

- **Framework:** ESP-IDF (not Arduino)
- **Language:** C
- **Target chip:** ESP32-D0WD-V3 (ESP32-WROOM-32E module)
- **Display:** ILI9341V, 240x320, 4-line SPI
- **Touch:** XPT2046 resistive, SPI
- **Audio input:** INMP441 I2S MEMS microphone (active), WM8782S I2S ADC (planned)
- **Audio input (dev/test):** WAV reference files from SD card or embedded flash

## Hardware Pin Assignments

### LCD (SPI)
| Signal | Pin |
|---|---|
| CS | IO15 |
| DC | IO2 |
| CLK | IO14 |
| MOSI | IO13 |
| MISO | IO12 |
| RST | EN (shared with ESP32 reset) |
| Backlight | IO21 (high = on) |

### Touch XPT2046 (SPI)
| Signal | Pin |
|---|---|
| CLK | IO25 |
| MOSI | IO32 |
| MISO | IO39 |
| CS | IO33 |
| IRQ | IO36 |

### SD Card (SPI, shared bus with SPI peripheral)
| Signal | Pin |
|---|---|
| CS | IO5 |
| CLK | IO18 |
| MOSI | IO23 |
| MISO | IO19 |

### INMP441 MEMS Mic (I2S0, master)
| Signal | Pin |
|---|---|
| BCK | IO27 |
| WS | IO26 |
| SD (data in) | IO35 (input only) |
| L/R | GND (left channel) |

### Other
| Function | Pin |
|---|---|
| Audio enable | IO4 (low = enable) |
| RGB LED Red | IO22 (low = on) |
| RGB LED Green | IO16 (low = on) |
| RGB LED Blue | IO17 (low = on) |
| Battery ADC | IO34 (input only) |
| BOOT button | IO0 |
| IO35 | input only — used as I2S data in |

## Architecture

Three FreeRTOS tasks communicating via queues:

1. **audio_task** — reads sample buffer (from WAV file during dev, I2S in production)
2. **pitch_task** — runs pitch detection algorithm on sample buffer, outputs detected frequency
3. **display_task** — consumes detected frequency, calculates strobe phase, renders frame to LCD via SPI

Keep tasks decoupled. Audio source is swappable — the pitch task must not depend on source type.

## Project Structure

```
/main
  main.c              — app_main, task creation
  audio.c / audio.h   — audio source abstraction (file or I2S)
  pitch.c / pitch.h   — pitch detection
  display.c / display.h — strobe rendering, LCD driver calls
  touch.c / touch.h   — XPT2046 driver and input handling
/components
  ili9341/            — ILI9341V SPI display driver
  xpt2046/            — XPT2046 touch driver
/test_audio/          — reference WAV files for dev/test
CMakeLists.txt
sdkconfig
CLAUDE.md
```

## Build & Flash Commands

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor
idf.py -p /dev/ttyUSB0 flash monitor
idf.py menuconfig
```

## Key Constraints

- Display SPI and touch SPI are on separate buses — do not merge them
- SD card shares SPI bus with the SPI peripheral header (IO18/19/23) — manage CS lines correctly
- IO35 and IO39 are input-only ADC pins — never configure as output
- EN pin is shared between LCD reset and ESP32 reset — LCD reset follows system reset
- RGB LEDs are common anode — low = on, high = off
- Audio enable (IO4) is active low
- Working voltage is 5V via USB-C; ESP32 operates at 3.3V internally

## Audio Source Abstraction

The `audio.c` module must expose a single interface regardless of source:

```c
esp_err_t audio_init(audio_source_t source);
int audio_read(int16_t *buf, size_t len);
```

`audio_source_t` values: `AUDIO_SOURCE_WAV_FILE`, `AUDIO_SOURCE_I2S`

Pitch task calls `audio_read()` only. Source switching requires no changes to pitch or display tasks.

## Pitch Detection

Use YIN algorithm as baseline. Must operate on int16_t sample buffers. Output is frequency in Hz as float. Design for guitar/bass frequency range (40Hz–1200Hz) with accuracy target of ±0.5 cents.

## Strobe Rendering

Strobe pattern is a set of segments rendered on the LCD. Rotation speed of pattern is driven by detected pitch. At exact pitch match, pattern appears stationary. Sharp/flat deviation causes apparent rotation in either direction. Render target: 240x320, RGB565. Update rate target: as fast as SPI throughput allows, minimum 30fps.

## Notes

- This is a solo dev project
- Update this file when architecture or pin usage changes
- Do not add changelog entries — document current state only
