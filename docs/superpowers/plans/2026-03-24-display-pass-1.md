# Display Pass 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Increase strobe petal density to 36 segments at 22% fill and eliminate screen tearing by consolidating per-frame SPI writes into two single-transaction blits.

**Architecture:** All changes are in `main/display.c`. The ring and bar regions each get a DMA-capable scratch buffer allocated at init. Each frame: memset → render into buffer → single `ili9341_draw_bitmap` call per region. The per-row scanline loop and 4-call `render_bar` are replaced. No other files change.

**Tech Stack:** ESP-IDF, C, ILI9341V SPI display, FreeRTOS

---

> **Verification note:** This is embedded firmware. There is no unit test runner. Each task is verified by `idf.py build` (compilation check) and `idf.py flash monitor` (visual check on hardware). The user runs build/flash themselves — plan steps state the commands but do not execute them.

---

## File Map

- Modify: `main/display.c` — all changes in this file only
- No changes: `components/ili9341/`, `main/display.h`, any other file

---

### Task 1: Petal Density Constants

**Files:**
- Modify: `main/display.c:12,18`

- [ ] **Step 1: Change N_SEG from 12 to 36**

In `main/display.c`, line 12:

```c
// Before:
#define N_SEG       12

// After:
#define N_SEG       36
```

- [ ] **Step 2: Change FILL_RATIO from 0.5f to 0.22f**

In `main/display.c`, line 18:

```c
// Before:
#define FILL_RATIO  0.5f      /* fraction of each segment arc that is lit */

// After:
#define FILL_RATIO  0.22f     /* fraction of each segment arc that is lit */
```

- [ ] **Step 3: Build to verify no compilation errors**

```bash
idf.py build
```

Expected: `Build successful` with no errors.

- [ ] **Step 4: Flash and verify visually**

```bash
idf.py flash monitor
```

Expected: ring shows thinner, more numerous petals. Count should be visibly higher than before. Green when in tune, white when out of tune.

- [ ] **Step 5: Commit**

```bash
git add main/display.c
git commit -m "feat: increase strobe petal density to 36 segments at 22% fill"
```

---

### Task 2: Scratch Buffer Setup + Ring Render Refactor

This task removes the per-row scanline blit and replaces it with a single full-ring blit. All pixel writes go into `s_ring_buf` instead of `s_row_buf`.

**Files:**
- Modify: `main/display.c` — includes, TAG, constants, buffer declarations, `display_init()`, `display_render_strobe()`

- [ ] **Step 1: Add missing includes and TAG**

At the top of `main/display.c`, add two includes and a TAG after the existing includes:

```c
// Add after existing #include lines:
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "display";
```

Full include block should look like:
```c
#include "display.h"
#include "ili9341.h"
#include "pitch.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "display";
```

- [ ] **Step 2: Add ring/bar rect constants**

After the existing `#define COL_BG` line (line 17), add:

```c
/* Bounding boxes for partial-region blits.
 * RING: X0 = CX - R_OUTER, Y0 = CY - R_OUTER, W/H = R_OUTER * 2 + 1
 * The +1 is required: the render loop is inclusive (y <= CY + R_OUTER),
 * so local row/col 200 is a valid write — needs a 201-element dimension.
 * Must be updated if CX, CY, or R_OUTER change.
 * BAR constants derived from render_bar() geometry. */
#define RING_X0  20
#define RING_Y0  60
#define RING_W   201    /* R_OUTER * 2 + 1 */
#define RING_H   201    /* R_OUTER * 2 + 1 */
#define BAR_X0   20
#define BAR_Y0   274
#define BAR_W    200
#define BAR_H    19
```

- [ ] **Step 3: Replace s_row_buf with s_ring_buf / s_bar_buf pointers**

Find this block (around line 45):
```c
static float    s_phase  = 0.0f;
static int64_t  s_last_t = 0;
static uint16_t s_row_buf[LCD_W];
```

Replace with:
```c
static float     s_phase    = 0.0f;
static int64_t   s_last_t   = 0;
static uint16_t *s_ring_buf = NULL;
static uint16_t *s_bar_buf  = NULL;
```

- [ ] **Step 4: Update display_init() to allocate scratch buffers**

Replace the current `display_init()`:
```c
esp_err_t display_init(void) {
    ili9341_fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    s_last_t = esp_timer_get_time();
    return ESP_OK;
}
```

With:
```c
esp_err_t display_init(void) {
    s_ring_buf = heap_caps_malloc(RING_W * RING_H * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_ring_buf) { ESP_LOGE(TAG, "s_ring_buf alloc failed"); abort(); }

    s_bar_buf = heap_caps_malloc(BAR_W * BAR_H * sizeof(uint16_t),
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bar_buf) { ESP_LOGE(TAG, "s_bar_buf alloc failed"); abort(); }

    ili9341_fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    s_last_t = esp_timer_get_time();
    return ESP_OK;
}
```

- [ ] **Step 5: Confirm SPI max_transfer_sz is sufficient**

Open `components/ili9341/ili9341.c`. Find the `spi_bus_initialize` call. Confirm:

```c
.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,  // = 153,600
```

153,600 ≥ 80,802 (RING_W × RING_H × 2) — no change needed. If the value is smaller than 80,802, update it to `RING_W * RING_H * sizeof(uint16_t)`. An undersized `max_transfer_sz` silently truncates the DMA transfer with no error.

- [ ] **Step 6: Refactor the render loop in display_render_strobe()**

The current render loop (lines ~97–140) has two parts to change:
1. Add `memset` before the loop
2. Change the inner pixel write from `s_row_buf[x - x0] = col` to `s_ring_buf[offset] = col`
3. Remove the per-row `ili9341_draw_bitmap` call inside the loop
4. Add a single `ili9341_draw_bitmap` call after the loop

Replace everything from the `int y0 = CY - R_OUTER` line through the closing `}` of the for loop (inclusive), i.e.:

```c
    int y0 = CY - R_OUTER, y1 = CY + R_OUTER;
    if (y0 < 0) y0 = 0;
    if (y1 >= LCD_H) y1 = LCD_H - 1;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)(y - CY);
        float xspan2 = r_outer2 - dy * dy;
        if (xspan2 < 0.0f) continue;
        int xi = (int)sqrtf(xspan2);

        int x0 = CX - xi, x1 = CX + xi;
        if (x0 < 0) x0 = 0;
        if (x1 >= LCD_W) x1 = LCD_W - 1;
        int w = x1 - x0 + 1;

        for (int x = x0; x <= x1; x++) {
            float dx = (float)(x - CX);
            float d2  = dx * dx + dy * dy;
            uint16_t col;
            if (d2 > r_outer2) {
                col = COL_BG;
            } else if (d2 < r_inner2) {
                /* Inner circle: render note glyph or background */
                if (nlen && x >= txt_x0 && x < txt_x1 &&
                            y >= txt_y0 && y < txt_y1) {
                    int ci   = (x - txt_x0) / (GLYPH_W * NOTE_SCALE);
                    int gcol = ((x - txt_x0) % (GLYPH_W * NOTE_SCALE)) / NOTE_SCALE;
                    int grow = (y - txt_y0) / NOTE_SCALE;
                    const uint8_t *bmp = (ci < nlen) ? glyphs[ci] : NULL;
                    col = (bmp && (bmp[grow] & (0x80 >> gcol))) ? COL_SEG : COL_BG;
                } else {
                    col = COL_BG;
                }
            } else {
                float angle = atan2f(dy, dx);
                float rel   = fmodf(angle - s_phase, seg_span);
                if (rel < 0.0f) rel += seg_span;
                col = (rel < lit_span) ? col_seg : COL_BG;
            }
            s_row_buf[x - x0] = col;
        }

        ili9341_draw_bitmap((uint16_t)x0, (uint16_t)y, (uint16_t)w, 1, s_row_buf);
    }
```

With:

```c
    memset(s_ring_buf, 0, RING_W * RING_H * sizeof(uint16_t));

    int y0 = CY - R_OUTER, y1 = CY + R_OUTER;
    if (y0 < 0) y0 = 0;
    if (y1 >= LCD_H) y1 = LCD_H - 1;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)(y - CY);
        float xspan2 = r_outer2 - dy * dy;
        if (xspan2 < 0.0f) continue;
        int xi = (int)sqrtf(xspan2);

        int x0 = CX - xi, x1 = CX + xi;
        if (x0 < 0) x0 = 0;
        if (x1 >= LCD_W) x1 = LCD_W - 1;

        for (int x = x0; x <= x1; x++) {
            float dx = (float)(x - CX);
            float d2  = dx * dx + dy * dy;
            uint16_t col;
            if (d2 > r_outer2) {
                col = COL_BG;
            } else if (d2 < r_inner2) {
                /* Inner circle: render note glyph or background */
                if (nlen && x >= txt_x0 && x < txt_x1 &&
                            y >= txt_y0 && y < txt_y1) {
                    int ci   = (x - txt_x0) / (GLYPH_W * NOTE_SCALE);
                    int gcol = ((x - txt_x0) % (GLYPH_W * NOTE_SCALE)) / NOTE_SCALE;
                    int grow = (y - txt_y0) / NOTE_SCALE;
                    const uint8_t *bmp = (ci < nlen) ? glyphs[ci] : NULL;
                    col = (bmp && (bmp[grow] & (0x80 >> gcol))) ? COL_SEG : COL_BG;
                } else {
                    col = COL_BG;
                }
            } else {
                float angle = atan2f(dy, dx);
                float rel   = fmodf(angle - s_phase, seg_span);
                if (rel < 0.0f) rel += seg_span;
                col = (rel < lit_span) ? col_seg : COL_BG;
            }
            s_ring_buf[(y - RING_Y0) * RING_W + (x - RING_X0)] = col;
        }
    }

    ili9341_draw_bitmap(RING_X0, RING_Y0, RING_W, RING_H, s_ring_buf);
```

Key changes:
- `memset` added before the loop (fills ring region black)
- `int w = x1 - x0 + 1;` line removed (no longer needed)
- `s_row_buf[x - x0] = col` → `s_ring_buf[(y - RING_Y0) * RING_W + (x - RING_X0)] = col`
- Per-row `ili9341_draw_bitmap(x0, y, w, 1, s_row_buf)` removed from inside the loop
- Single `ili9341_draw_bitmap(RING_X0, RING_Y0, RING_W, RING_H, s_ring_buf)` added after the loop

- [ ] **Step 7: Build to verify**

```bash
idf.py build
```

Expected: `Build successful`, no errors, no warnings about `s_row_buf` (it is removed).

- [ ] **Step 8: Flash and verify ring renders correctly**

```bash
idf.py flash monitor
```

Expected: ring renders identically to after Task 1. No regression. Tearing should be visibly reduced on the ring region.

- [ ] **Step 9: Commit**

```bash
git add main/display.c
git commit -m "perf: consolidate ring render into single SPI blit to reduce tearing"
```

---

### Task 3: Bar Render Refactor

Replace the 4 `ili9341_fill_rect` calls in `render_bar()` with pixel writes into `s_bar_buf`, then a single `ili9341_draw_bitmap`.

**Color note:** `ili9341_fill_rect` sends colors big-endian (MSB first). `ili9341_draw_bitmap` sends raw `uint16_t` values little-endian (LSB first on ESP32). Colors that are not byte-symmetric must be byte-swapped. Mapping:
- `0x0000` (black) → `0x0000` (symmetric)
- `0xFFFF` (white) → `0xFFFF` (symmetric)
- `0x1082` (groove gray, used in fill_rect) → `0x8210` in draw_bitmap buffer
- `0x07E0` (green, used in fill_rect) → `0xE007` in draw_bitmap buffer
- `0xFD20` (orange, used in fill_rect) → `0x20FD` in draw_bitmap buffer

**Files:**
- Modify: `main/display.c` — `render_bar()` only

- [ ] **Step 1: Replace render_bar() body**

Replace the current `render_bar()` function:

```c
static void render_bar(float cents) {
    int fill_w = (int)((fabsf(cents) / 50.0f) * 100.0f);
    if (fill_w > 100) fill_w = 100;
    int fill_x = (cents < 0.0f) ? (120 - fill_w) : 120;
    uint16_t fill_col = (fabsf(cents) <= 5.0f) ? 0x07E0u : 0xFD20u;

    ili9341_fill_rect(20,  274, 200, 19, 0x0000);
    ili9341_fill_rect(20,  280, 200,  8, 0x1082);
    if (fill_w > 0)
        ili9341_fill_rect((uint16_t)fill_x, 280, (uint16_t)fill_w, 8, fill_col);
    ili9341_fill_rect(119, 276,   3, 16, 0xFFFF);
}
```

With:

```c
static void render_bar(float cents) {
    int fill_w = (int)((fabsf(cents) / 50.0f) * 100.0f);
    if (fill_w > 100) fill_w = 100;
    int fill_x = (cents < 0.0f) ? (120 - fill_w) : 120;
    /* Colors byte-swapped vs fill_rect: draw_bitmap sends uint16_t little-endian */
    uint16_t fill_col = (fabsf(cents) <= 5.0f) ? 0xE007u : 0x20FDu;

    memset(s_bar_buf, 0, BAR_W * BAR_H * sizeof(uint16_t));

    /* Groove: screen y=280-287 → local rows 6-13, all columns */
    for (int row = 6; row <= 13; row++)
        for (int col = 0; col < BAR_W; col++)
            s_bar_buf[row * BAR_W + col] = 0x8210u;

    /* Meter fill over groove */
    if (fill_w > 0) {
        int lx0 = fill_x - BAR_X0;
        int lx1 = lx0 + fill_w;
        if (lx0 < 0) lx0 = 0;
        if (lx1 > BAR_W) lx1 = BAR_W;
        for (int row = 6; row <= 13; row++)
            for (int col = lx0; col < lx1; col++)
                s_bar_buf[row * BAR_W + col] = fill_col;
    }

    /* Center tick: screen y=276-291 → local rows 2-17, screen x=119-121 → local cols 99-101 */
    for (int row = 2; row <= 17; row++)
        for (int col = 99; col <= 101; col++)
            s_bar_buf[row * BAR_W + col] = 0xFFFFu;

    ili9341_draw_bitmap(BAR_X0, BAR_Y0, BAR_W, BAR_H, s_bar_buf);
}
```

- [ ] **Step 2: Build to verify**

```bash
idf.py build
```

Expected: `Build successful`, no errors.

- [ ] **Step 3: Flash and verify bar renders correctly**

```bash
idf.py flash monitor
```

Expected:
- Dark groove bar visible at the bottom of the screen
- Meter fill moves correctly as pitch deviates (orange when off, green when within 5 cents)
- White center tick visible
- No fill_rect calls remain for the bar region — bar tearing should be reduced

- [ ] **Step 4: Commit**

```bash
git add main/display.c
git commit -m "perf: consolidate bar render into single SPI blit to reduce tearing"
```
