#include "display.h"
#include "ili9341.h"
#include "pitch.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "display";

/* ---- Compile-time display mode and orientation --------------------------------- */
#define STROBE_MODE_ARC_SOLID    0   /* solid white segments */
#define STROBE_MODE_ARC_CHECKER  1   /* 8x8 checkerboard segments */
#define STROBE_MODE_RACK         2   /* horizontal rack-style strobe (TODO) */

#define ORIENT_PORTRAIT          0
#define ORIENT_LANDSCAPE         1

#define STROBE_MODE   STROBE_MODE_ARC_CHECKER
#define ORIENTATION   ORIENT_PORTRAIT

/* ---- Display geometry ---------------------------------------------------------- */
#if ORIENTATION == ORIENT_PORTRAIT
#  define LCD_W  240
#  define LCD_H  320
#else
#  define LCD_W  320
#  define LCD_H  240
#endif

#define CX          (LCD_W / 2)
#define CY          127
#define N_SEG       18
#define R_INNER     60
#define R_OUTER     114
#define STRIP_H     8
#define CENTS_TO_RPM  0.3f   /* 1 RPM per cent, 30 RPM at 100 cents */
#define COL_SEG     0xFFFF
#define COL_BG      0x0000
#define FILL_RATIO  0.45f

/* 100-degree arc centred at the top of the circle */
#define ARC_MIN_ANGLE  (-7.0f * (float)M_PI / 9.0f)   /* -140 degrees */
#define ARC_MAX_ANGLE  (-2.0f * (float)M_PI / 9.0f)   /* -40  degrees */

/* Bounding box for the arc region.
 * Arc centre is at (CX, CY) = (120, 127).
 * RING_Y0 = CY - R_OUTER = 127 - 114 = 13.
 * Bottom edge = CY - R_INNER*sin(40°) ≈ 127 - 60*0.643 = 88, +5 margin → RING_H = 81. */
#define RING_X0  (CX - R_OUTER)
#define RING_Y0  (CY - R_OUTER)
#define RING_W   (R_OUTER * 2 + 1)
#define RING_H   81

/* Note name rendered below the arc */
#define NOTE_W   100
#define NOTE_H   36
#define NOTE_X0  (CX - NOTE_W / 2)
#define NOTE_Y0  104

#define BAR_X0   20
#define BAR_Y0   274
#define BAR_W    200
#define BAR_H    19

/* ---- Minimal 8x8 bitmap font (A-G, #, -) -------------------------------------- */
#define GLYPH_W 8
#define GLYPH_H 8
#define NOTE_SCALE 4

typedef struct { char c; uint8_t rows[GLYPH_H]; } glyph_t;

static const glyph_t s_glyphs[] = {
    {'A', {0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x00}},
    {'B', {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}},
    {'C', {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}},
    {'D', {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}},
    {'E', {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}},
    {'F', {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}},
    {'G', {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}},
    {'#', {0x24,0x24,0x7E,0x24,0x7E,0x24,0x24,0x00}},
    {'-', {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}},
    {'0', {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}},
    {'1', {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}},
    {'2', {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}},
    {'3', {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}},
    {'4', {0x06,0x0E,0x1E,0x36,0x7E,0x06,0x06,0x00}},
    {'5', {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}},
    {'6', {0x3C,0x60,0x60,0x7C,0x66,0x66,0x3C,0x00}},
    {'7', {0x7E,0x06,0x0C,0x18,0x18,0x18,0x18,0x00}},
    {'8', {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}},
    {'9', {0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}},
};

static const uint8_t *glyph_lookup(char c) {
    for (size_t i = 0; i < sizeof(s_glyphs) / sizeof(s_glyphs[0]); i++)
        if (s_glyphs[i].c == c) return s_glyphs[i].rows;
    return NULL;
}

/* ---- Shared state ------------------------------------------------------------- */
static float     s_phase  = 0.0f;
static float     s_ref_hz = 0.0f;
static int64_t   s_last_t = 0;

static uint16_t *s_ring_buf = NULL;
static uint16_t *s_bar_buf  = NULL;
static uint16_t *s_note_buf = NULL;

/* ---- State passed from math layer to renderer ---------------------------------- */
typedef struct {
    float        phase;
    float        cents;
    const char  *note;
    uint16_t     col_seg;
} strobe_state_t;

/* ---- Shared sub-renderers (used by all modes) ---------------------------------- */

static void render_bar(float cents) {
    int fill_w = (int)((fabsf(cents) / 50.0f) * 100.0f);
    if (fill_w > 100) fill_w = 100;
    int fill_x = (cents < 0.0f) ? (120 - fill_w) : 120;
    uint16_t fill_col = (fabsf(cents) <= 5.0f) ? 0xE007u : 0x20FDu;

    memset(s_bar_buf, 0, BAR_W * BAR_H * sizeof(uint16_t));

    for (int row = 6; row <= 13; row++)
        for (int col = 0; col < BAR_W; col++)
            s_bar_buf[row * BAR_W + col] = 0x8210u;

    if (fill_w > 0) {
        int lx0 = fill_x - BAR_X0;
        int lx1 = lx0 + fill_w;
        if (lx0 < 0) lx0 = 0;
        if (lx1 > BAR_W) lx1 = BAR_W;
        for (int row = 6; row <= 13; row++)
            for (int col = lx0; col < lx1; col++)
                s_bar_buf[row * BAR_W + col] = fill_col;
    }

    for (int row = 2; row <= 17; row++)
        for (int col = 99; col <= 101; col++)
            s_bar_buf[row * BAR_W + col] = 0xFFFFu;

    ili9341_draw_bitmap(BAR_X0, BAR_Y0, BAR_W, BAR_H, s_bar_buf);
}

static void render_note(const char *ref_note) {
    int nlen = (int)strlen(ref_note);
    const uint8_t *glyphs[4] = {NULL, NULL, NULL, NULL};
    for (int i = 0; i < nlen && i < 4; i++) glyphs[i] = glyph_lookup(ref_note[i]);
    int n_dig  = (nlen > 0 && ref_note[nlen-1] >= '0' && ref_note[nlen-1] <= '9') ? 1 : 0;
    int n_ltr  = nlen - n_dig;
    int ltr_w  = n_ltr * GLYPH_W * NOTE_SCALE;
    int dig_w  = n_dig * GLYPH_W * (NOTE_SCALE / 2);
    int txt_lx  = (NOTE_W - (ltr_w + dig_w)) / 2;
    int txt_ly  = (NOTE_H - GLYPH_H * NOTE_SCALE) / 2;
    int ltr_lx1 = txt_lx + ltr_w;

    memset(s_note_buf, 0, NOTE_W * NOTE_H * sizeof(uint16_t));

    for (int ly = txt_ly; ly < txt_ly + GLYPH_H * NOTE_SCALE && ly < NOTE_H; ly++) {
        int row_in_glyph = (ly - txt_ly) / NOTE_SCALE;
        for (int lx = txt_lx; lx < ltr_lx1 && lx < NOTE_W; lx++) {
            int ci   = (lx - txt_lx) / (GLYPH_W * NOTE_SCALE);
            int gcol = ((lx - txt_lx) % (GLYPH_W * NOTE_SCALE)) / NOTE_SCALE;
            const uint8_t *bmp = (ci < n_ltr) ? glyphs[ci] : NULL;
            if (bmp && (bmp[row_in_glyph] & (0x80 >> gcol)))
                s_note_buf[ly * NOTE_W + lx] = COL_SEG;
        }
        if (n_dig) {
            int dig_row = (ly - txt_ly) / (NOTE_SCALE / 2);
            if (dig_row < GLYPH_H) {
                for (int lx = ltr_lx1; lx < ltr_lx1 + dig_w && lx < NOTE_W; lx++) {
                    int gcol = ((lx - ltr_lx1) % (GLYPH_W * (NOTE_SCALE / 2))) / (NOTE_SCALE / 2);
                    const uint8_t *bmp = glyphs[n_ltr];
                    if (bmp && (bmp[dig_row] & (0x80 >> gcol)))
                        s_note_buf[ly * NOTE_W + lx] = COL_SEG;
                }
            }
        }
    }

    ili9341_draw_bitmap(NOTE_X0, NOTE_Y0, NOTE_W, NOTE_H, s_note_buf);
}

/* ---- Renderers ----------------------------------------------------------------- */

static void render_arc(const strobe_state_t *s, int checker) {
    const float seg_span = 2.0f * (float)M_PI / N_SEG;
    const float lit_span = seg_span * FILL_RATIO;
    const float r_inner2 = (float)(R_INNER * R_INNER);
    const float r_outer2 = (float)(R_OUTER * R_OUTER);

    for (int sy = RING_Y0; sy < RING_Y0 + RING_H; sy += STRIP_H) {
        int ey = sy + STRIP_H - 1;
        if (ey >= RING_Y0 + RING_H) ey = RING_Y0 + RING_H - 1;
        if (ey >= LCD_H) ey = LCD_H - 1;
        if (sy < 0) continue;
        int rows = ey - sy + 1;

        memset(s_ring_buf, 0, RING_W * rows * sizeof(uint16_t));

        for (int y = sy; y <= ey; y++) {
            float dy = (float)(y - CY);
            float xspan2 = r_outer2 - dy * dy;
            if (xspan2 < 0.0f) continue;
            int xi = (int)sqrtf(xspan2);

            int x0 = CX - xi, x1 = CX + xi;
            if (x0 < 0) x0 = 0;
            if (x1 >= LCD_W) x1 = LCD_W - 1;

            for (int x = x0; x <= x1; x++) {
                float dx = (float)(x - CX);
                float d2 = dx * dx + dy * dy;
                if (d2 > r_outer2 || d2 < r_inner2) continue;

                float angle = atan2f(dy, dx);
                if (angle < ARC_MIN_ANGLE || angle > ARC_MAX_ANGLE) continue;

                float rel = fmodf(angle - s->phase, seg_span);
                if (rel < 0.0f) rel += seg_span;
                if (rel < lit_span) {
                    if (!checker || (((x >> 3) + (y >> 3)) & 1) == 0)
                        s_ring_buf[(y - sy) * RING_W + (x - RING_X0)] = s->col_seg;
                }
            }
        }

        ili9341_draw_bitmap(RING_X0, sy, RING_W, rows, s_ring_buf);
    }

    render_note(s->note);
    render_bar(s->cents);
}

static void render_mode(const strobe_state_t *s) {
#if STROBE_MODE == STROBE_MODE_ARC_SOLID
    render_arc(s, 0);
#elif STROBE_MODE == STROBE_MODE_ARC_CHECKER
    render_arc(s, 1);
#else
    render_arc(s, 1);  /* fallback until other modes are implemented */
#endif
}

/* ---- Public API --------------------------------------------------------------- */

esp_err_t display_init(void) {
    s_ring_buf = heap_caps_malloc(RING_W * STRIP_H * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_ring_buf) { ESP_LOGE(TAG, "s_ring_buf alloc failed"); abort(); }

    s_bar_buf = heap_caps_malloc(BAR_W * BAR_H * sizeof(uint16_t),
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bar_buf) { ESP_LOGE(TAG, "s_bar_buf alloc failed"); abort(); }

    s_note_buf = heap_caps_malloc(NOTE_W * NOTE_H * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_note_buf) { ESP_LOGE(TAG, "s_note_buf alloc failed"); abort(); }

    ili9341_fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    s_last_t = esp_timer_get_time();
    return ESP_OK;
}

void display_render_strobe(float detected_hz, const char *note) {
    int64_t now = esp_timer_get_time();
    float dt = (s_last_t == 0) ? 0.033f : (float)(now - s_last_t) / 1e6f;
    if (dt > 0.1f) dt = 0.1f;
    s_last_t = now;

    if (detected_hz <= 0.0f) {
        s_ref_hz = 0.0f;
        memset(s_ring_buf, 0, RING_W * STRIP_H * sizeof(uint16_t));
        for (int sy = RING_Y0; sy < RING_Y0 + RING_H; sy += STRIP_H) {
            int rows = sy + STRIP_H <= RING_Y0 + RING_H ? STRIP_H : (RING_Y0 + RING_H - sy);
            ili9341_draw_bitmap(RING_X0, sy, RING_W, rows, s_ring_buf);
        }
        memset(s_note_buf, 0, NOTE_W * NOTE_H * sizeof(uint16_t));
        ili9341_draw_bitmap(NOTE_X0, NOTE_Y0, NOTE_W, NOTE_H, s_note_buf);
        render_bar(0.0f);
        return;
    }

    /* Hysteresis: snap reference note only when deviation exceeds 65 cents */
    float nearest_hz = pitch_hz_to_nearest_hz(detected_hz);
    if (s_ref_hz <= 0.0f) s_ref_hz = nearest_hz;
    if (fabsf(1200.0f * log2f(detected_hz / s_ref_hz)) > 65.0f)
        s_ref_hz = nearest_hz;

    float cents = 1200.0f * log2f(detected_hz / s_ref_hz);

    /* Phase: 1 RPM per cent, 30 RPM at 100 cents */
    float rpm  = cents * CENTS_TO_RPM;
    float dphi = rpm * 2.0f * (float)M_PI * dt / 60.0f;
    s_phase += dphi;
    s_phase = fmodf(s_phase, 2.0f * (float)M_PI);

    char ref_note[4];
    pitch_hz_to_note(s_ref_hz, ref_note, sizeof(ref_note));

    strobe_state_t state = {
        .phase   = s_phase,
        .cents   = cents,
        .note    = ref_note,
        .col_seg = (fabsf(cents) <= 5.0f) ? 0xE007u : 0xFFFFu,
    };

    render_mode(&state);
}
