# ESP32 Strobe Tuner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a working ESP-IDF project for an ESP32 stroboscopic tuner — from bare environment to a strobe display that appears stationary when a 440 Hz tone is played.

**Architecture:** Hardware-first layered build: ESP-IDF install → project scaffold → display driver → touch driver → audio abstraction → YIN pitch detection → FreeRTOS task wiring with strobe rendering. Each layer is flashed and validated before the next begins.

**Tech Stack:** ESP-IDF v5.4, C, FreeRTOS, SPI (ILI9341V + XPT2046), SPIFFS, YIN algorithm

---

## File Map

| File | Responsibility |
|------|---------------|
| `main/main.c` | app_main, FreeRTOS task creation, queue + semaphore init |
| `main/audio.c` / `audio.h` | WAV parsing, SPIFFS streaming, audio_read interface |
| `main/pitch.c` / `pitch.h` | YIN algorithm, pitch_init, pitch_detect |
| `main/display.c` / `display.h` | Strobe rendering, segment drawing, phase accumulation |
| `main/touch.c` / `touch.h` | XPT2046 read wrapper (standalone, not wired to tasks) |
| `main/CMakeLists.txt` | Register all main sources + spiffs dependency |
| `components/ili9341/ili9341.c` / `ili9341.h` | ILI9341V SPI driver: init, fill_rect, draw_pixel |
| `components/ili9341/CMakeLists.txt` | Register component |
| `components/xpt2046/xpt2046.c` / `xpt2046.h` | XPT2046 SPI driver: init, read |
| `components/xpt2046/CMakeLists.txt` | Register component |
| `CMakeLists.txt` | Top-level, cmake_minimum_required, include(esp-idf) |
| `partitions.csv` | Custom partition table with SPIFFS partition |
| `test_audio/gen_440hz.py` | Python script to generate 440 Hz test WAV |
| `test_audio/440hz_sine.wav` | Generated test WAV (440 Hz, 44100 Hz, 16-bit, mono) |

---

## Task 0: Install ESP-IDF v5.4

**Files:** none (environment setup)

- [ ] **Step 1: Clone ESP-IDF v5.4 with submodules**

```bash
git clone --recursive https://github.com/espressif/esp-idf.git --branch v5.4 C:/esp/esp-idf
```

Expected: ~1–2 GB clone. Takes several minutes. Final line: `Submodule path '...': checked out '...'`

- [ ] **Step 2: Run the installer**

From CMD or PowerShell (not Git Bash — install.bat requires Windows shell):
```
C:\esp\esp-idf\install.bat esp32
```

Expected: Downloads toolchain, Python packages. Ends with: `All done! You can now run:`

- [ ] **Step 3: Verify environment activates in Git Bash**

```bash
source C:/esp/esp-idf/export.sh
idf.py --version
```

Expected: `ESP-IDF v5.4`

- [ ] **Step 4: Verify target chip is recognized**

```bash
idf.py --list-targets | grep esp32
```

Expected: `esp32` in output (along with other variants)

---

## Task 1: Project Scaffold + Clean Build

**Files:** All files in the file map above (stubs). Git initialized.

- [ ] **Step 1: Initialize git repo**

```bash
cd Y:/Claudetron
git init
```

- [ ] **Step 2: Create directory structure**

```bash
mkdir -p main components/ili9341 components/xpt2046 test_audio docs/superpowers/plans docs/superpowers/specs
```

- [ ] **Step 3: Write top-level CMakeLists.txt**

Create `Y:/Claudetron/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(strobe_tuner)
```

- [ ] **Step 4: Write main/CMakeLists.txt**

Create `Y:/Claudetron/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.c" "audio.c" "pitch.c" "display.c" "touch.c"
    INCLUDE_DIRS "."
    REQUIRES ili9341 xpt2046 esp_spiffs nvs_flash
)
```

- [ ] **Step 5: Write components/ili9341/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "ili9341.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)
```

- [ ] **Step 6: Write components/xpt2046/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "xpt2046.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)
```

- [ ] **Step 7: Write partitions.csv**

Create `Y:/Claudetron/partitions.csv`:
```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x6000
phy_init, data, phy,     0xf000,  0x1000
factory,  app,  factory, 0x10000, 1500K
spiffs,   data, spiffs,  ,        500K
```

- [ ] **Step 8: Write stub header files**

Create `Y:/Claudetron/components/ili9341/ili9341.h`:
```c
#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t ili9341_init(void);
esp_err_t ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
esp_err_t ili9341_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
```

Create `Y:/Claudetron/components/xpt2046/xpt2046.h`:
```c
#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t xpt2046_init(void);
bool xpt2046_read(int *x, int *y, int *pressure);
```

Create `Y:/Claudetron/main/audio.h`:
```c
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

typedef enum {
    AUDIO_SOURCE_WAV_FILE,
    AUDIO_SOURCE_I2S,
} audio_source_t;

esp_err_t audio_init(audio_source_t source);
int audio_read(int16_t *buf, size_t len);
uint32_t audio_get_sample_rate(void);
```

Create `Y:/Claudetron/main/pitch.h`:
```c
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t pitch_init(size_t buf_len);
float pitch_detect(const int16_t *buf, size_t len, float sample_rate);
```

Create `Y:/Claudetron/main/display.h`:
```c
#pragma once
#include "esp_err.h"

esp_err_t display_init(void);
void display_render_strobe(float detected_hz);
```

Create `Y:/Claudetron/main/touch.h`:
```c
#pragma once
#include <stdbool.h>

esp_err_t touch_init(void);
bool touch_read(int *x, int *y);
```

- [ ] **Step 9: Write stub source files**

Create `Y:/Claudetron/components/ili9341/ili9341.c`:
```c
#include "ili9341.h"
esp_err_t ili9341_init(void) { return ESP_OK; }
esp_err_t ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) { return ESP_OK; }
esp_err_t ili9341_draw_pixel(uint16_t x, uint16_t y, uint16_t color) { return ESP_OK; }
```

Create `Y:/Claudetron/components/xpt2046/xpt2046.c`:
```c
#include "xpt2046.h"
esp_err_t xpt2046_init(void) { return ESP_OK; }
bool xpt2046_read(int *x, int *y, int *pressure) { return false; }
```

Create `Y:/Claudetron/main/audio.c`:
```c
#include "audio.h"
esp_err_t audio_init(audio_source_t source) { return ESP_OK; }
int audio_read(int16_t *buf, size_t len) { return 0; }
uint32_t audio_get_sample_rate(void) { return 44100; }
```

Create `Y:/Claudetron/main/pitch.c`:
```c
#include "pitch.h"
esp_err_t pitch_init(size_t buf_len) { return ESP_OK; }
float pitch_detect(const int16_t *buf, size_t len, float sample_rate) { return 0.0f; }
```

Create `Y:/Claudetron/main/display.c`:
```c
#include "display.h"
esp_err_t display_init(void) { return ESP_OK; }
void display_render_strobe(float detected_hz) {}
```

Create `Y:/Claudetron/main/touch.c`:
```c
#include "touch.h"
#include "xpt2046.h"
esp_err_t touch_init(void) { return xpt2046_init(); }
bool touch_read(int *x, int *y) { int p; return xpt2046_read(x, y, &p); }
```

Create `Y:/Claudetron/main/main.c`:
```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "main";

static void audio_task(void *arg) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(10));
}
static void pitch_task(void *arg) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(10));
}
static void display_task(void *arg) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(33));
}

void app_main(void) {
    ESP_LOGI(TAG, "Strobe tuner starting");
    xTaskCreate(audio_task,   "audio",   4096 * 4, NULL, 5, NULL);
    xTaskCreate(pitch_task,   "pitch",   4096 * 4, NULL, 4, NULL);
    xTaskCreate(display_task, "display", 4096 * 4, NULL, 3, NULL);
}
```

- [ ] **Step 10: Set target and configure partition table**

```bash
cd Y:/Claudetron
source C:/esp/esp-idf/export.sh
idf.py set-target esp32
```

Then enable custom partition table:
```bash
idf.py menuconfig
```

Navigate to: `Partition Table` → `Partition Table` → select `Custom partition table CSV` → set filename to `partitions.csv`. Save and exit.

- [ ] **Step 11: Build and verify clean**

```bash
idf.py build
```

Expected: `Project build complete. To flash, run: idf.py flash`
Zero errors, zero warnings about undefined symbols.

- [ ] **Step 12: Commit scaffold**

```bash
git add -A
git commit -m "feat: initial project scaffold with stub implementations"
```

---

## Task 2: ILI9341V Display Driver

**Files:**
- Implement: `components/ili9341/ili9341.c`
- Modify: `main/main.c` (add display init + test fill to app_main)

- [ ] **Step 1: Implement ili9341.c with full SPI init and init sequence**

Overwrite `Y:/Claudetron/components/ili9341/ili9341.c`:

```c
#include "ili9341.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ili9341";

/* Pin assignments */
#define LCD_PIN_CS    15
#define LCD_PIN_DC     2
#define LCD_PIN_CLK   14
#define LCD_PIN_MOSI  13
#define LCD_PIN_MISO  12
#define LCD_PIN_BL    21

#define LCD_WIDTH   240
#define LCD_HEIGHT  320

#define ILI9341_CMD_CASET  0x2A
#define ILI9341_CMD_RASET  0x2B
#define ILI9341_CMD_RAMWR  0x2C
#define ILI9341_CMD_MADCTL 0x36
#define ILI9341_CMD_COLMOD 0x3A
#define ILI9341_CMD_SLPOUT 0x11
#define ILI9341_CMD_DISPON 0x29

static spi_device_handle_t s_spi = NULL;

/* Send a command byte (DC low) */
static void lcd_cmd(uint8_t cmd) {
    gpio_set_level(LCD_PIN_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi, &t);
}

/* Send data bytes (DC high) */
static void lcd_data(const uint8_t *data, size_t len) {
    if (len == 0) return;
    gpio_set_level(LCD_PIN_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data_byte(uint8_t b) { lcd_data(&b, 1); }

esp_err_t ili9341_init(void) {
    /* Configure DC pin */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(LCD_PIN_BL, 0);  /* backlight off during init */

    /* Init SPI bus (SPI2 / HSPI) */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = LCD_PIN_MOSI,
        .miso_io_num   = LCD_PIN_MISO,
        .sclk_io_num   = LCD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 26 * 1000 * 1000,  /* 26 MHz; drop to 10 MHz if corruption */
        .mode           = 0,
        .spics_io_num   = LCD_PIN_CS,
        .queue_size     = 7,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi), TAG, "SPI device add failed");

    /* ILI9341V init sequence */
    lcd_cmd(0xCF); lcd_data((uint8_t[]){0x00, 0x83, 0x30}, 3);
    lcd_cmd(0xED); lcd_data((uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);
    lcd_cmd(0xE8); lcd_data((uint8_t[]){0x85, 0x01, 0x79}, 3);
    lcd_cmd(0xCB); lcd_data((uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);
    lcd_cmd(0xF7); lcd_data((uint8_t[]){0x20}, 1);
    lcd_cmd(0xEA); lcd_data((uint8_t[]){0x00, 0x00}, 2);
    lcd_cmd(0xC0); lcd_data_byte(0x26);          /* Power control: VRH */
    lcd_cmd(0xC1); lcd_data_byte(0x11);          /* Power control: SAP, BT */
    lcd_cmd(0xC5); lcd_data((uint8_t[]){0x35, 0x3E}, 2);
    lcd_cmd(0xC7); lcd_data_byte(0xBE);
    lcd_cmd(0x36); lcd_data_byte(0x48);          /* MADCTL: portrait, BGR */
    lcd_cmd(0x3A); lcd_data_byte(0x55);          /* COLMOD: 16-bit RGB565 */
    lcd_cmd(0xB1); lcd_data((uint8_t[]){0x00, 0x1B}, 2);  /* Frame rate */
    lcd_cmd(0xF2); lcd_data_byte(0x08);          /* 3G disable */
    lcd_cmd(0x26); lcd_data_byte(0x01);          /* Gamma set */
    lcd_cmd(0xE0); lcd_data((uint8_t[]){
        0x1F,0x1A,0x18,0x0A,0x0F,0x06,0x45,0x87,
        0x32,0x0A,0x07,0x02,0x07,0x05,0x00}, 15);
    lcd_cmd(0xE1); lcd_data((uint8_t[]){
        0x00,0x25,0x27,0x05,0x10,0x09,0x3A,0x78,
        0x4D,0x05,0x18,0x0D,0x38,0x3A,0x1F}, 15);
    lcd_cmd(0xB7); lcd_data_byte(0x07);
    lcd_cmd(0xB6); lcd_data((uint8_t[]){0x0A, 0x82, 0x27, 0x00}, 4);
    lcd_cmd(ILI9341_CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(ILI9341_CMD_DISPON);

    /* Enable backlight */
    gpio_set_level(LCD_PIN_BL, 1);

    ESP_LOGI(TAG, "ILI9341 init OK");
    return ESP_OK;
}

esp_err_t ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (w == 0 || h == 0) return ESP_OK;

    /* Set column range */
    lcd_cmd(ILI9341_CMD_CASET);
    lcd_data((uint8_t[]){x >> 8, x & 0xFF, (x+w-1) >> 8, (x+w-1) & 0xFF}, 4);

    /* Set row range */
    lcd_cmd(ILI9341_CMD_RASET);
    lcd_data((uint8_t[]){y >> 8, y & 0xFF, (y+h-1) >> 8, (y+h-1) & 0xFF}, 4);

    /* Stream pixel data */
    lcd_cmd(ILI9341_CMD_RAMWR);
    gpio_set_level(LCD_PIN_DC, 1);

    /* Build a row buffer and send row by row (avoids huge single allocation) */
    static uint8_t row_buf[LCD_WIDTH * 2];
    uint8_t hi = color >> 8, lo = color & 0xFF;
    for (uint16_t i = 0; i < w; i++) { row_buf[i*2] = hi; row_buf[i*2+1] = lo; }

    for (uint16_t row = 0; row < h; row++) {
        spi_transaction_t t = {
            .length    = w * 16,
            .tx_buffer = row_buf,
        };
        spi_device_polling_transmit(s_spi, &t);
    }
    return ESP_OK;
}

esp_err_t ili9341_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    return ili9341_fill_rect(x, y, 1, 1, color);
}
```

- [ ] **Step 2: Modify main.c to test display init + red fill**

In `main/main.c`, update `app_main`:
```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ili9341.h"

static const char *TAG = "main";

static void audio_task(void *arg)   { for (;;) vTaskDelay(pdMS_TO_TICKS(10)); }
static void pitch_task(void *arg)   { for (;;) vTaskDelay(pdMS_TO_TICKS(10)); }
static void display_task(void *arg) { for (;;) vTaskDelay(pdMS_TO_TICKS(33)); }

void app_main(void) {
    ESP_LOGI(TAG, "Strobe tuner starting");

    ESP_ERROR_CHECK(ili9341_init());
    /* Validation: fill screen red (RGB565: 0xF800) */
    ESP_ERROR_CHECK(ili9341_fill_rect(0, 0, 240, 320, 0xF800));
    ESP_LOGI(TAG, "Display fill red: OK");

    xTaskCreate(audio_task,   "audio",   4096 * 4, NULL, 5, NULL);
    xTaskCreate(pitch_task,   "pitch",   4096 * 4, NULL, 4, NULL);
    xTaskCreate(display_task, "display", 4096 * 4, NULL, 3, NULL);
}
```

- [ ] **Step 3: Build**

```bash
idf.py build
```

Expected: Build succeeds. No errors.

- [ ] **Step 4: Full erase + flash (first time only — required for partition table change)**

```bash
idf.py erase-flash
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected on serial monitor:
```
I (xxx) main: Strobe tuner starting
I (xxx) ili9341: ILI9341 init OK
I (xxx) main: Display fill red: OK
```

Expected on hardware: **screen lights up solid red.**

If screen shows corruption but no SPI errors: change clock from `26 * 1000 * 1000` to `10 * 1000 * 1000` in `ili9341.c` and reflash.

- [ ] **Step 5: Commit**

```bash
git add components/ili9341/ili9341.c main/main.c
git commit -m "feat: ILI9341V display driver — fills screen, validated on hardware"
```

---

## Task 3: XPT2046 Touch Driver

**Files:**
- Implement: `components/xpt2046/xpt2046.c`
- Modify: `main/main.c` (add touch poll loop to validate)

- [ ] **Step 1: Implement xpt2046.c**

Overwrite `Y:/Claudetron/components/xpt2046/xpt2046.c`:

```c
#include "xpt2046.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "xpt2046";

#define TOUCH_PIN_CS    33
#define TOUCH_PIN_CLK   25
#define TOUCH_PIN_MOSI  32
#define TOUCH_PIN_MISO  39   /* input-only pin — correct for MISO */
#define TOUCH_PIN_IRQ   36   /* input-only pin — correct for IRQ input */

#define XPT2046_CMD_X   0xD0  /* measure X position */
#define XPT2046_CMD_Y   0x90  /* measure Y position */
#define XPT2046_CMD_Z1  0xB0  /* measure Z1 (pressure) */

static spi_device_handle_t s_spi = NULL;

static uint16_t xpt2046_send(uint8_t cmd) {
    uint8_t tx[3] = {cmd, 0, 0};
    uint8_t rx[3] = {0};
    spi_transaction_t t = {
        .length    = 24,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(s_spi, &t);
    return ((rx[1] << 8) | rx[2]) >> 3;  /* 12-bit result */
}

esp_err_t xpt2046_init(void) {
    /* IRQ pin: input only (IO36 is input-only) */
    gpio_config_t irq_conf = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&irq_conf);

    spi_bus_config_t buscfg = {
        .mosi_io_num   = TOUCH_PIN_MOSI,
        .miso_io_num   = TOUCH_PIN_MISO,
        .sclk_io_num   = TOUCH_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_DISABLED), TAG, "Touch SPI bus init failed");

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000,  /* 2 MHz — XPT2046 maximum */
        .mode           = 0,
        .spics_io_num   = TOUCH_PIN_CS,
        .queue_size     = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI3_HOST, &devcfg, &s_spi), TAG, "Touch SPI device add failed");

    ESP_LOGI(TAG, "XPT2046 init OK");
    return ESP_OK;
}

bool xpt2046_read(int *x, int *y, int *pressure) {
    /* Fast IRQ check — IO36 is pulled high when no touch */
    if (gpio_get_level(TOUCH_PIN_IRQ) != 0) {
        return false;
    }
    *x        = xpt2046_send(XPT2046_CMD_X);
    *y        = xpt2046_send(XPT2046_CMD_Y);
    *pressure = xpt2046_send(XPT2046_CMD_Z1);
    return true;
}
```

- [ ] **Step 2: Update main.c to validate touch**

Add to `app_main` after the existing display lines (before task creation):
```c
#include "touch.h"

    /* Validate touch driver */
    ESP_ERROR_CHECK(touch_init());
    ESP_LOGI(TAG, "Touch init OK. Touch the screen to see coordinates...");
    for (int i = 0; i < 200; i++) {
        int x, y;
        if (touch_read(&x, &y)) {
            ESP_LOGI(TAG, "Touch: x=%d y=%d", x, y);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "Touch validation done");
```

- [ ] **Step 3: Build**

```bash
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Flash and validate**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected on serial: `Touch init OK. Touch the screen to see coordinates...`
Touch the screen. Expected: `Touch: x=NNNN y=NNNN` lines appear. Values in range 0–4095.
When not touching: no output. After 10s: `Touch validation done`.

- [ ] **Step 5: Remove touch validation loop from main.c**

Remove the touch validation for-loop (keep `touch_init()` call — it will stay). The final `app_main` after cleanup:
```c
void app_main(void) {
    ESP_LOGI(TAG, "Strobe tuner starting");
    ESP_ERROR_CHECK(ili9341_init());
    ESP_ERROR_CHECK(ili9341_fill_rect(0, 0, 240, 320, 0xF800));
    ESP_LOGI(TAG, "Display init OK");
    ESP_ERROR_CHECK(touch_init());
    ESP_LOGI(TAG, "Touch init OK");
    xTaskCreate(audio_task,   "audio",   4096 * 4, NULL, 5, NULL);
    xTaskCreate(pitch_task,   "pitch",   4096 * 4, NULL, 4, NULL);
    xTaskCreate(display_task, "display", 4096 * 4, NULL, 3, NULL);
}
```

- [ ] **Step 6: Commit**

```bash
git add components/xpt2046/xpt2046.c main/main.c main/touch.c
git commit -m "feat: XPT2046 touch driver — IRQ poll, validated on hardware"
```

---

## Task 4: Audio Abstraction (WAV / SPIFFS)

**Files:**
- Create: `test_audio/gen_440hz.py`
- Create: `test_audio/440hz_sine.wav` (generated)
- Implement: `main/audio.c`
- Modify: `main/main.c` (add audio validation)

- [ ] **Step 1: Generate test WAV file**

Create `Y:/Claudetron/test_audio/gen_440hz.py`:
```python
import struct, math, wave, os

sample_rate = 44100
duration_s  = 3
frequency   = 440.0
amplitude   = 16000

out_path = os.path.join(os.path.dirname(__file__), "440hz_sine.wav")
with wave.open(out_path, "w") as wf:
    wf.setnchannels(1)
    wf.setsampwidth(2)
    wf.setframerate(sample_rate)
    for i in range(sample_rate * duration_s):
        s = int(amplitude * math.sin(2.0 * math.pi * frequency * i / sample_rate))
        wf.writeframes(struct.pack("<h", s))
print(f"Generated: {out_path}")
```

Run it (ESP-IDF Python env is available):
```bash
python test_audio/gen_440hz.py
```

Expected: `Generated: .../test_audio/440hz_sine.wav`

- [ ] **Step 2: Implement audio.c**

Overwrite `Y:/Claudetron/main/audio.c`:

```c
#include "audio.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "audio";

#define AUDIO_BUF_SAMPLES 4096
#define WAV_PATH "/spiffs/440hz_sine.wav"

static uint32_t  s_sample_rate = 0;
static uint16_t  s_channels    = 0;
static FILE     *s_wav_file    = NULL;
static uint32_t  s_data_end    = 0;   /* byte offset of end of PCM data */

/* Static downmix buffer for stereo → mono conversion */
static int16_t s_stereo_buf[AUDIO_BUF_SAMPLES * 2];

/* Read a little-endian uint16 from file */
static uint16_t read_u16(FILE *f) {
    uint8_t b[2]; fread(b, 1, 2, f);
    return (uint16_t)(b[0] | (b[1] << 8));
}

/* Read a little-endian uint32 from file */
static uint32_t read_u32(FILE *f) {
    uint8_t b[4]; fread(b, 1, 4, f);
    return (uint32_t)(b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24));
}

static esp_err_t mount_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t audio_init(audio_source_t source) {
    if (source != AUDIO_SOURCE_WAV_FILE) {
        ESP_LOGE(TAG, "Only AUDIO_SOURCE_WAV_FILE supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(mount_spiffs(), TAG, "SPIFFS mount failed");

    s_wav_file = fopen(WAV_PATH, "rb");
    if (!s_wav_file) {
        ESP_LOGE(TAG, "Cannot open %s", WAV_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    /* Parse RIFF header */
    char tag[5] = {0};
    fread(tag, 1, 4, s_wav_file);
    if (strncmp(tag, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "Not a RIFF file");
        return ESP_ERR_INVALID_ARG;
    }
    read_u32(s_wav_file);  /* file size — ignore */
    fread(tag, 1, 4, s_wav_file);
    if (strncmp(tag, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a WAVE file");
        return ESP_ERR_INVALID_ARG;
    }

    /* Scan for fmt and data chunks */
    bool found_fmt = false, found_data = false;
    uint16_t bits_per_sample = 0;
    while (!found_data) {
        if (fread(tag, 1, 4, s_wav_file) < 4) break;
        uint32_t chunk_size = read_u32(s_wav_file);
        if (strncmp(tag, "fmt ", 4) == 0) {
            uint16_t audio_format = read_u16(s_wav_file);
            if (audio_format != 1) {
                ESP_LOGE(TAG, "Only PCM WAV supported (format=%d)", audio_format);
                return ESP_ERR_INVALID_ARG;
            }
            s_channels    = read_u16(s_wav_file);
            s_sample_rate = read_u32(s_wav_file);
            read_u32(s_wav_file);  /* byte rate */
            read_u16(s_wav_file);  /* block align */
            bits_per_sample = read_u16(s_wav_file);
            if (bits_per_sample != 16) {
                ESP_LOGE(TAG, "Only 16-bit WAV supported");
                return ESP_ERR_INVALID_ARG;
            }
            /* Skip any extra fmt bytes */
            if (chunk_size > 16) fseek(s_wav_file, chunk_size - 16, SEEK_CUR);
            found_fmt = true;
        } else if (strncmp(tag, "data", 4) == 0) {
            /* PCM data starts here */
            s_data_end = (uint32_t)ftell(s_wav_file) + chunk_size;
            found_data = true;
        } else {
            fseek(s_wav_file, chunk_size, SEEK_CUR);  /* skip unknown chunk */
        }
    }

    if (!found_fmt || !found_data) {
        ESP_LOGE(TAG, "Malformed WAV: fmt=%d data=%d", found_fmt, found_data);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "WAV: %u Hz, %u ch, %u-bit", s_sample_rate, s_channels, bits_per_sample);
    return ESP_OK;
}

int audio_read(int16_t *buf, size_t len) {
    if (!s_wav_file) return -1;
    if ((uint32_t)ftell(s_wav_file) >= s_data_end) return 0;  /* EOF */

    if (s_channels == 1) {
        /* Mono: read directly */
        size_t samples = fread(buf, sizeof(int16_t), len, s_wav_file);
        return (int)samples;
    } else {
        /* Stereo: read interleaved, downmix to mono */
        size_t frames = len;  /* mono output frames = stereo input frames */
        size_t read = fread(s_stereo_buf, sizeof(int16_t), frames * 2, s_wav_file);
        size_t got = read / 2;
        for (size_t i = 0; i < got; i++) {
            buf[i] = (int16_t)(((int32_t)s_stereo_buf[i*2] + s_stereo_buf[i*2+1]) / 2);
        }
        return (int)got;
    }
}

uint32_t audio_get_sample_rate(void) {
    return s_sample_rate;
}
```

- [ ] **Step 3: Flash the SPIFFS image**

First, build the project so the partition table binary is generated:
```bash
idf.py build
```

Generate the SPIFFS image from the test_audio directory using `spiffsgen.py` (size must match the partition — 500K = 512000 bytes, but use the standard 512K token):
```bash
python C:/esp/esp-idf/components/spiffs/spiffsgen.py 512K test_audio spiffs_image.bin
```

Find the SPIFFS partition offset from the compiled partition table binary (not the CSV):
```bash
python C:/esp/esp-idf/components/partition_table/parttool.py \
  --partition-table-file build/partition_table/partition-table.bin \
  get_partition_info --partition-name spiffs
```

This should report offset `0x187000` (factory at 0x10000 + 1500K = 0x10000 + 0x177000 = 0x187000). If it differs, use the reported value.

Flash the SPIFFS image:
```bash
python C:/esp/esp-idf/components/esptool_py/esptool/esptool.py \
  --chip esp32 -p /dev/ttyUSB0 write_flash 0x187000 spiffs_image.bin
```

- [ ] **Step 4: Update main.c to validate audio**

Add audio validation to `app_main` (after touch_init, before task creation):
```c
#include "audio.h"

    /* Validate audio */
    ESP_ERROR_CHECK(audio_init(AUDIO_SOURCE_WAV_FILE));
    ESP_LOGI(TAG, "Audio init OK — sample rate: %lu Hz", audio_get_sample_rate());
    int16_t test_buf[16];
    int got = audio_read(test_buf, 16);
    ESP_LOGI(TAG, "audio_read got %d samples: [%d, %d, %d, %d, ...]",
             got, test_buf[0], test_buf[1], test_buf[2], test_buf[3]);
```

- [ ] **Step 5: Build and flash**

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected on serial:
```
I (xxx) audio: WAV: 44100 Hz, 1 ch, 16-bit
I (xxx) main: Audio init OK — sample rate: 44100 Hz
I (xxx) main: audio_read got 16 samples: [-12, 45, 101, 156, ...]
```

Non-zero sample values confirm WAV streaming is working.

- [ ] **Step 6: Remove audio validation from main.c**

Remove the test_buf lines. Keep `audio_init()` call.

- [ ] **Step 7: Commit**

```bash
git add main/audio.c main/audio.h main/main.c test_audio/gen_440hz.py
git commit -m "feat: audio abstraction — WAV/SPIFFS streaming, validated on hardware"
```

---

## Task 5: YIN Pitch Detection

**Files:**
- Implement: `main/pitch.c`
- Modify: `main/main.c` (add pitch validation loop)

- [ ] **Step 1: Implement pitch.c with YIN algorithm**

Overwrite `Y:/Claudetron/main/pitch.c`:

```c
#include "pitch.h"
#include "esp_log.h"
#include <stdlib.h>
#include <math.h>
#include <float.h>

static const char *TAG = "pitch";

#define YIN_THRESHOLD 0.15f

static float *s_diff = NULL;   /* difference function, len/2 floats */
static float *s_cmnd = NULL;   /* cumulative mean normalized diff, len/2 floats */
static size_t s_half  = 0;

esp_err_t pitch_init(size_t buf_len) {
    s_half = buf_len / 2;
    s_diff = (float *)malloc(s_half * sizeof(float));
    s_cmnd = (float *)malloc(s_half * sizeof(float));
    if (!s_diff || !s_cmnd) {
        free(s_diff); free(s_cmnd);
        ESP_LOGE(TAG, "pitch_init: malloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "pitch_init OK — half=%u, heap used=%u", (unsigned)s_half,
             (unsigned)(s_half * 2 * sizeof(float)));
    return ESP_OK;
}

float pitch_detect(const int16_t *buf, size_t len, float sample_rate) {
    size_t half = len / 2;
    if (half > s_half) half = s_half;  /* clamp to allocated size */

    /* Step 1: Difference function d(τ) = Σ(x[j] - x[j+τ])² */
    for (size_t tau = 0; tau < half; tau++) {
        double sum = 0.0;
        for (size_t j = 0; j < half; j++) {
            double delta = (double)(buf[j] - buf[j + tau]);
            sum += delta * delta;
        }
        s_diff[tau] = (float)sum;
    }

    /* Step 2: Cumulative mean normalized difference */
    s_cmnd[0] = 1.0f;
    double running_sum = 0.0;
    for (size_t tau = 1; tau < half; tau++) {
        running_sum += s_diff[tau];
        s_cmnd[tau] = (running_sum > 0.0)
            ? (float)(s_diff[tau] * tau / running_sum)
            : 1.0f;
    }

    /* Step 3: Find first τ below threshold in valid range */
    size_t tau_min = (size_t)(sample_rate / 1200.0f) + 1;  /* max 1200 Hz */
    size_t tau_max = (size_t)(sample_rate / 40.0f);         /* min 40 Hz */
    if (tau_max >= half) tau_max = half - 1;

    for (size_t tau = tau_min; tau <= tau_max; tau++) {
        if (s_cmnd[tau] < YIN_THRESHOLD) {
            /* Step 4: Parabolic interpolation for sub-sample accuracy */
            float better_tau = (float)tau;
            if (tau > 0 && tau < half - 1) {
                float s0 = s_cmnd[tau - 1];
                float s1 = s_cmnd[tau];
                float s2 = s_cmnd[tau + 1];
                float denom = 2.0f * (2.0f * s1 - s2 - s0);
                if (fabsf(denom) > FLT_EPSILON) {
                    better_tau = (float)tau + (s2 - s0) / denom;
                }
            }
            return sample_rate / better_tau;
        }
    }

    return 0.0f;  /* no pitch found */
}
```

- [ ] **Step 2: Update main.c to validate pitch detection**

Add pitch validation after audio_init (before task creation). This loops audio → pitch detection and logs the result:
```c
#include "pitch.h"

    /* Validate pitch detection */
    ESP_ERROR_CHECK(pitch_init(4096));
    {
        static int16_t pitch_test_buf[4096];
        float sr = (float)audio_get_sample_rate();
        float detected = 0.0f;
        int passes = 0;
        /* Read a few buffers until we get a stable pitch reading */
        for (int attempt = 0; attempt < 20 && detected == 0.0f; attempt++) {
            int n = audio_read(pitch_test_buf, 4096);
            if (n < 4096) break;
            detected = pitch_detect(pitch_test_buf, 4096, sr);
        }
        ESP_LOGI(TAG, "Pitch detection: %.2f Hz (expected ~440.00)", detected);
        /* ±1 Hz tolerance for log — spec requires ±0.5 cents ≈ ±0.13 Hz at 440 */
        if (detected > 439.0f && detected < 441.0f) {
            ESP_LOGI(TAG, "Pitch detection PASS");
        } else {
            ESP_LOGW(TAG, "Pitch detection out of range — check WAV and algorithm");
        }
    }
```

- [ ] **Step 3: Build**

```bash
idf.py build
```

Expected: clean build.

- [ ] **Step 4: Flash and validate**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected:
```
I (xxx) pitch: pitch_init OK
I (xxx) main: Pitch detection: 440.01 Hz (expected ~440.00)
I (xxx) main: Pitch detection PASS
```

If `detected == 0.0f`: the WAV may have ended before 20 buffers. Re-generate a longer WAV (increase `duration_s` in gen_440hz.py) and re-flash SPIFFS. Also confirm `audio_init` was called first — the file pointer must be at the data start.

- [ ] **Step 5: Remove pitch validation from main.c**

Remove the static pitch_test_buf block. Keep `pitch_init(4096)` call.

- [ ] **Step 6: Commit**

```bash
git add main/pitch.c main/main.c
git commit -m "feat: YIN pitch detection — 440 Hz validated within 1 Hz on hardware"
```

---

## Task 6: FreeRTOS Task Wiring + Strobe Rendering

**Files:**
- Implement: `main/display.c`
- Implement: `main/main.c` (final — queues, semaphore, wired tasks)

- [ ] **Step 1: Implement display.c with strobe rendering**

Overwrite `Y:/Claudetron/main/display.c`:

```c
#include "display.h"
#include "ili9341.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

#define LCD_W        240
#define LCD_H        320
#define N_SEGMENTS   12
#define CX           (LCD_W / 2)
#define CY           (LCD_H / 2)
#define R_INNER      60
#define R_OUTER      100
#define SEG_WIDTH    6      /* pixel width of each segment line */
#define F_TARGET     440.0f
#define K_SPEED      1.0f
#define COLOR_SEG    0xFFFF /* white */
#define COLOR_BG     0x0000 /* black */

static float s_phase     = 0.0f;
static int64_t s_last_t  = 0;

esp_err_t display_init(void) {
    /* Clear screen to black */
    ili9341_fill_rect(0, 0, LCD_W, LCD_H, COLOR_BG);
    s_last_t = esp_timer_get_time();
    return ESP_OK;
}

/* Draw or erase a thick radial line segment at given angle */
static void draw_segment(float angle, uint16_t color) {
    float cos_a = cosf(angle);
    float sin_a = sinf(angle);
    /* Step along radius from inner to outer */
    for (int r = R_INNER; r <= R_OUTER; r += 1) {
        int px = CX + (int)(r * cos_a);
        int py = CY + (int)(r * sin_a);
        /* Draw a SEG_WIDTH × SEG_WIDTH block at (px, py) */
        int x0 = px - SEG_WIDTH/2;
        int y0 = py - SEG_WIDTH/2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x0 + SEG_WIDTH > LCD_W) x0 = LCD_W - SEG_WIDTH;
        if (y0 + SEG_WIDTH > LCD_H) y0 = LCD_H - SEG_WIDTH;
        ili9341_fill_rect(x0, y0, SEG_WIDTH, SEG_WIDTH, color);
    }
}

void display_render_strobe(float detected_hz) {
    /* Compute dt */
    int64_t now = esp_timer_get_time();
    float dt = (s_last_t == 0) ? 0.033f : (float)(now - s_last_t) / 1e6f;
    s_last_t = now;
    if (dt > 0.1f) dt = 0.1f;  /* clamp for first frame or scheduler hiccup */

    /* Erase old segments */
    for (int i = 0; i < N_SEGMENTS; i++) {
        float angle = (2.0f * M_PI * i / N_SEGMENTS) + s_phase;
        draw_segment(angle, COLOR_BG);
    }

    /* Update phase */
    s_phase += 2.0f * M_PI * (detected_hz - F_TARGET) / F_TARGET * K_SPEED * dt;
    s_phase = fmodf(s_phase, 2.0f * M_PI);

    /* Draw new segments */
    for (int i = 0; i < N_SEGMENTS; i++) {
        float angle = (2.0f * M_PI * i / N_SEGMENTS) + s_phase;
        draw_segment(angle, COLOR_SEG);
    }
}
```

- [ ] **Step 2: Implement final main.c with wired tasks**

Overwrite `Y:/Claudetron/main/main.c`:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "ili9341.h"
#include "audio.h"
#include "pitch.h"
#include "display.h"
#include "touch.h"

static const char *TAG = "main";

#define AUDIO_BUF_SAMPLES 4096

/* Double-buffer pool: 2 × 4096 int16_t = 16 KB */
static int16_t s_buf_pool[2][AUDIO_BUF_SAMPLES];

static QueueHandle_t s_sample_queue;  /* int16_t * pointers */
static QueueHandle_t s_freq_queue;    /* float */
static SemaphoreHandle_t s_buf_sem;   /* counting: 2 = both buffers free */

static void audio_task(void *arg) {
    int buf_idx = 0;
    for (;;) {
        /* Wait for a free buffer */
        xSemaphoreTake(s_buf_sem, portMAX_DELAY);

        int16_t *buf = s_buf_pool[buf_idx];
        int got = audio_read(buf, AUDIO_BUF_SAMPLES);

        if (got <= 0) {
            /* EOF or error — give semaphore back and idle */
            xSemaphoreGive(s_buf_sem);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Try to post to pitch task */
        if (xQueueSend(s_sample_queue, &buf, 0) != pdTRUE) {
            /* Pitch task is behind — drop buffer */
            xSemaphoreGive(s_buf_sem);
        } else {
            buf_idx = 1 - buf_idx;  /* switch to other buffer */
        }
    }
}

static void pitch_task(void *arg) {
    float sample_rate = (float)audio_get_sample_rate();
    float last_hz = F_TARGET;  /* defined in display.c — use 440.0f directly */
    for (;;) {
        int16_t *buf = NULL;
        if (xQueueReceive(s_sample_queue, &buf, portMAX_DELAY) == pdTRUE) {
            float hz = pitch_detect(buf, AUDIO_BUF_SAMPLES, sample_rate);
            /* Release buffer back to pool */
            xSemaphoreGive(s_buf_sem);
            /* Only post non-zero hz */
            if (hz > 0.0f) last_hz = hz;
            xQueueOverwrite(s_freq_queue, &last_hz);
        }
    }
}

static void display_task(void *arg) {
    float last_freq = 440.0f;
    for (;;) {
        float hz;
        if (xQueueReceive(s_freq_queue, &hz, 0) == pdTRUE) {
            if (hz > 0.0f) last_freq = hz;
        }
        display_render_strobe(last_freq);
        /* No explicit delay — SPI transactions provide natural pacing */
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Strobe tuner starting");

    /* Hardware init */
    ESP_ERROR_CHECK(ili9341_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(audio_init(AUDIO_SOURCE_WAV_FILE));
    ESP_ERROR_CHECK(pitch_init(AUDIO_BUF_SAMPLES));
    ESP_ERROR_CHECK(display_init());

    ESP_LOGI(TAG, "All hardware init OK — sample rate: %lu Hz", audio_get_sample_rate());

    /* Create queues and semaphore */
    s_sample_queue = xQueueCreate(1, sizeof(int16_t *));
    s_freq_queue   = xQueueCreate(1, sizeof(float));
    s_buf_sem      = xSemaphoreCreateCounting(2, 2);

    /* Create tasks */
    xTaskCreate(audio_task,   "audio",   4096 * 4, NULL, 5, NULL);
    xTaskCreate(pitch_task,   "pitch",   4096 * 4, NULL, 4, NULL);
    xTaskCreate(display_task, "display", 4096 * 4, NULL, 3, NULL);

    ESP_LOGI(TAG, "Tasks started");
}
```

Note: `pitch_task` references `F_TARGET` from display.c. Instead, hardcode `440.0f` directly:
```c
/* In pitch_task: */
float last_hz = 440.0f;
```

Also note `xQueueOverwrite` requires a queue of depth 1 — correct here.

- [ ] **Step 3: Add math library to main/CMakeLists.txt**

The display.c uses `cosf`, `sinf`, `fmodf`. Add to `main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.c" "audio.c" "pitch.c" "display.c" "touch.c"
    INCLUDE_DIRS "."
    REQUIRES ili9341 xpt2046 esp_spiffs nvs_flash
)
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
```

- [ ] **Step 4: Build**

```bash
idf.py build
```

Expected: clean build.

- [ ] **Step 5: Flash and validate**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected on serial:
```
I (xxx) main: Strobe tuner starting
I (xxx) ili9341: ILI9341 init OK
I (xxx) audio: WAV: 44100 Hz, 1 ch, 16-bit
I (xxx) pitch: pitch_init OK
I (xxx) main: All hardware init OK — sample rate: 44100 Hz
I (xxx) main: Tasks started
```

Expected on hardware: **black screen with 12 white radial segments arranged in a circle. Segments should appear stationary** (440 Hz WAV playing). Phase accumulation is zero when detected frequency matches F_TARGET.

- [ ] **Step 6: Validate rotation**

To confirm rotation direction: temporarily change `F_TARGET` to `441.0f` in `display.c`, rebuild, flash. Pattern should rotate slowly in one direction. Change to `439.0f` — should rotate the opposite direction. Change back to `440.0f`.

- [ ] **Step 7: Commit**

```bash
git add main/display.c main/main.c main/CMakeLists.txt
git commit -m "feat: FreeRTOS task wiring + strobe rendering — full tuner pipeline complete"
```

---

## Done

All six success criteria from the spec should now be met:

1. `idf.py build` passes clean — Task 1
2. Screen fills red, SPI logged clean — Task 2
3. Touch coordinates on serial, false when not touching — Task 3
4. Audio buffer with correct sample rate and non-zero PCM — Task 4
5. 440 Hz detected within ±0.5 cents — Task 5
6. Strobe pattern visible, stationary at 440 Hz, rotates on deviation — Task 6
