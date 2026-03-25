# Display Pass 1 Design

**Goal:** Increase strobe petal density for a sharper visual and fix screen tearing via dirty-rect partial updates.

**Scope:** Two changes to `main/display.c` — petal constants and per-frame rendering strategy.

---

## Petal Density

Change the strobe ring from 12 segments at 50% fill to 36 segments at 22% fill (option C from visual comparison).

- `N_SEG`: 12 → 36
- Fill ratio: 50% → 22% (update wherever the segment arc width is calculated)

No logic changes required. The segment rendering loop already parameterizes on these values.

---

## Tearing Fix — Dirty-Rect Rendering

### Root cause

The current code calls `ili9341_draw_bitmap()` once per scanline — up to ~200 calls per frame, each with its own CASET/RASET/RAMWR command sequence. The fragmented, per-row transaction pattern is what drives tearing: the display's scan catches up between command sequences. `render_bar()` adds 4–5 more fragmented `ili9341_fill_rect()` transactions per frame.

### Solution

Consolidate all per-frame pixel writes into two single-transaction blits: one for the ring region, one for the bar region. Set the ILI9341 address window once per region and write all pixels in a single RAMWR sequence.

### Concrete geometry

From `display.c`: `CX=120, CY=160, R_OUTER=100`.

Ring constants (`RING_X0 = CX - R_OUTER`, `RING_Y0 = CY - R_OUTER`):
```c
#define RING_X0   20    // CX - R_OUTER
#define RING_Y0   60    // CY - R_OUTER
#define RING_W   200    // R_OUTER * 2
#define RING_H   200    // R_OUTER * 2
```

Bar constants (derived from `render_bar()` geometry):
```c
#define BAR_X0   20
#define BAR_Y0  274
#define BAR_W   200
#define BAR_H    19
```

These are hard-coded to current geometry. If `CX`, `CY`, or `R_OUTER` change, update ring constants using the formulas above. If `render_bar()` geometry changes, re-derive bar constants from that function.

### Memory — scratch buffers

Declare as pointers; allocate at display init with `heap_caps_malloc`. Abort-on-failure is the correct policy — there is no meaningful way to run without a framebuffer. Log before aborting so the crash is diagnosable:

```c
static uint16_t *ring_buf;
static uint16_t *bar_buf;

// In display_init():
ring_buf = heap_caps_malloc(RING_W * RING_H * sizeof(uint16_t),
                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
if (!ring_buf) { ESP_LOGE(TAG, "ring_buf alloc failed"); abort(); }

bar_buf = heap_caps_malloc(BAR_W * BAR_H * sizeof(uint16_t),
                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
if (!bar_buf) { ESP_LOGE(TAG, "bar_buf alloc failed"); abort(); }
```

Sizes: ring = 80,000 bytes (~78KB), bar = 7,600 bytes (~7.4KB), total = 87,600 bytes (~86KB). No PSRAM on this board; both allocations are from internal DRAM DMA pool. ESP32 internal DRAM is 320KB; firmware heap at runtime is ~200KB available — allocation should succeed.

### SPI DMA transfer size

ESP-IDF SPI bus `max_transfer_sz` defaults to 4096 bytes and must be explicitly raised to accommodate the ring buffer. Check the ILI9341 component's `spi_bus_initialize()` call and confirm (or set):

```c
.max_transfer_sz = RING_W * RING_H * sizeof(uint16_t)  // 80000 bytes
```

If the ili9341 component calculates this at init, verify it is >= 80000. This is a silent runtime failure if too small.

### Byte order

`ring_buf` and `bar_buf` store RGB565 values in the byte order that `ili9341_draw_bitmap()` expects (MSB-first for ILI9341). The existing per-pixel writes in the current scanline loop already produce values in this order — use the same write pattern when filling the scratch buffers.

### Per-frame render flow

**Startup (once):**
- Fill full 240×320 screen black

**Each frame:**

1. `memset(ring_buf, 0, RING_W * RING_H * sizeof(uint16_t))`
   - Use explicit size, not `sizeof(ring_buf)` (pointer, not array)
2. Render ring segments into `ring_buf`. Map screen coordinate `(x, y)` to buffer offset `(y - RING_Y0) * RING_W + (x - RING_X0)`. All existing segment pixel coordinates must use this offset.
3. Render note glyph into `ring_buf` at the same offset-adjusted coordinates. Note glyph lives inside the inner circle, which is within the ring bounding box — no separate blit needed. Re-render every frame (no dirty tracking).
4. Set ILI9341 address window to ring rect, write `ring_buf` in a single transaction.

5. `memset(bar_buf, 0, BAR_W * BAR_H * sizeof(uint16_t))`
6. Render bar into `bar_buf` using offset `(y - BAR_Y0) * BAR_W + (x - BAR_X0)`.
7. Set ILI9341 address window to bar rect, write `bar_buf` in a single transaction.

Note: verify whether the ILI9341 driver's address window call takes `(x0, y0, width, height)` or `(x0, y0, x1, y1)` — use the correct form to avoid off-by-one at the bounding box edge.

---

## Files Changed

- `main/display.c` — petal constants, `display_init()` scratch buffer allocation, per-frame ring/bar blit
- `components/ili9341/` — confirm/update `max_transfer_sz` if needed
- `main/display.h` — no public API changes; all new constants internal to display.c
