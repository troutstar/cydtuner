#include "display.h"
#include "ili9341.h"
#include "pitch.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "display";

/* Faster atan2 approximation — max error ~0.017 rad (~1 degree).
 * Segment width is 10 degrees so 1-degree error is imperceptible.
 * Roughly 8x faster than atan2f on Xtensa LX6. */
static inline float fast_atan2f(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float a  = (ax > ay) ? ay / ax : ax / ay;
    float s  = a * a;
    float r  = a * (1.0f + s * (-0.3258530f + s * 0.0860374f));
    if (ax < ay) r = (float)M_PI_2 - r;
    if (x < 0.0f) r = (float)M_PI - r;
    if (y < 0.0f) r = -r;
    return r;
}

#define LCD_W       240
#define LCD_H       320
#define CX          (LCD_W / 2)
#define CY          (LCD_H / 2)
#define N_SEG       36
#define R_INNER     60
#define R_OUTER     100
#define STRIP_H     8    /* render ring in horizontal strips; buf = RING_W*STRIP_H*2 ≈ 3KB */
#define K_SPEED     20.0f
#define COL_SEG     0xFFFF
#define COL_BG      0x0000
#define FILL_RATIO  0.22f     /* fraction of each segment arc that is lit */

/* Bounding boxes for partial-region blits.
 * RING: X0 = CX - R_OUTER, Y0 = CY - R_OUTER, W/H = R_OUTER * 2 + 1
 * The +1 is required: the render loop is inclusive (y <= CY + R_OUTER),
 * so local row/col 200 is a valid write — needs a 201-element dimension. */
#define RING_X0  (CX - R_OUTER)
#define RING_Y0  (CY - R_OUTER)
#define RING_W   (R_OUTER * 2 + 1)
#define RING_H   (R_OUTER * 2 + 1)
#define BAR_X0   20
#define BAR_Y0   274
#define BAR_W    200
#define BAR_H    19

/* ---- Minimal 8x8 bitmap font (A-G, #, -) for note name overlay ---------- */
#define GLYPH_W 8
#define GLYPH_H 8
#define NOTE_SCALE 4   /* render at 4x → 32x32 per character */

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

static float     s_phase    = 0.0f;
static float     s_ref_hz   = 0.0f;   /* hysteresis reference note — see display_render_strobe */
static int64_t   s_last_t   = 0;
static uint16_t *s_ring_buf = NULL;
static uint16_t *s_bar_buf  = NULL;

esp_err_t display_init(void) {
    s_ring_buf = heap_caps_malloc(RING_W * STRIP_H * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_ring_buf) { ESP_LOGE(TAG, "s_ring_buf alloc failed"); abort(); }

    s_bar_buf = heap_caps_malloc(BAR_W * BAR_H * sizeof(uint16_t),
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bar_buf) { ESP_LOGE(TAG, "s_bar_buf alloc failed"); abort(); }

    ili9341_fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    s_last_t = esp_timer_get_time();
    return ESP_OK;
}

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
        render_bar(0.0f);
        return;
    }

    /* Hysteresis reference: snap to nearest note only when deviation exceeds 65 cents.
     * Both label and rotation use s_ref_hz so they always agree — when the label
     * changes note, the rotation direction changes with it. */
    float nearest_hz = pitch_hz_to_nearest_hz(detected_hz);
    if (s_ref_hz <= 0.0f)
        s_ref_hz = nearest_hz;
    if (fabsf(1200.0f * log2f(detected_hz / s_ref_hz)) > 65.0f)
        s_ref_hz = nearest_hz;

    float cents = 1200.0f * log2f(detected_hz / s_ref_hz);

    /* Clamp per-frame delta to half a segment width (Nyquist for 36-segment pattern).
     * Exceeding this causes the wagon-wheel effect — apparent backward rotation. */
    float dphi = 2.0f * (float)M_PI * (detected_hz - s_ref_hz) / s_ref_hz * K_SPEED * dt;
    const float max_dphi = (float)M_PI / (float)N_SEG;
    if (dphi >  max_dphi) dphi =  max_dphi;
    if (dphi < -max_dphi) dphi = -max_dphi;
    s_phase += dphi;
    s_phase = fmodf(s_phase, 2.0f * (float)M_PI);

    uint16_t col_seg = (fabsf(cents) <= 5.0f) ? 0xE007u : 0xFFFFu;

    const float seg_span = 2.0f * (float)M_PI / N_SEG;
    const float lit_span = seg_span * FILL_RATIO;
    const float r_inner2 = (float)(R_INNER * R_INNER);
    const float r_outer2 = (float)(R_OUTER * R_OUTER);

    /* Note text derived from hysteresis reference so label always matches
     * the rotation direction (both use s_ref_hz as the reference note). */
    char ref_note[4];
    pitch_hz_to_note(s_ref_hz, ref_note, sizeof(ref_note));
    int nlen = (int)strlen(ref_note);
    const uint8_t *glyphs[4] = {NULL, NULL, NULL, NULL};
    for (int i = 0; i < nlen && i < 4; i++) glyphs[i] = glyph_lookup(ref_note[i]);
    /* Octave digit (last char if '0'-'9') renders as superscript at half scale */
    int n_dig  = (nlen > 0 && ref_note[nlen-1] >= '0' && ref_note[nlen-1] <= '9') ? 1 : 0;
    int n_ltr  = nlen - n_dig;
    int ltr_w  = n_ltr * GLYPH_W * NOTE_SCALE;
    int dig_w  = n_dig * GLYPH_W * (NOTE_SCALE / 2);
    int txt_x0 = CX - (ltr_w + dig_w) / 2;
    int txt_y0 = CY - (GLYPH_H * NOTE_SCALE) / 2;
    int ltr_x1 = txt_x0 + ltr_w;
    int txt_x1 = ltr_x1 + dig_w;
    int txt_y1 = txt_y0 + GLYPH_H * NOTE_SCALE;
    int dig_y1 = txt_y0 + GLYPH_H * (NOTE_SCALE / 2);  /* superscript: upper half */

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
                float d2  = dx * dx + dy * dy;
                uint16_t col;
                if (d2 > r_outer2) {
                    col = COL_BG;
                } else if (d2 < r_inner2) {
                    /* Inner circle: note letters at full scale, octave superscript at half */
                    col = COL_BG;
                    if (nlen && x >= txt_x0 && x < txt_x1 &&
                                y >= txt_y0 && y < txt_y1) {
                        if (x < ltr_x1) {
                            int ci   = (x - txt_x0) / (GLYPH_W * NOTE_SCALE);
                            int gcol = ((x - txt_x0) % (GLYPH_W * NOTE_SCALE)) / NOTE_SCALE;
                            int grow = (y - txt_y0) / NOTE_SCALE;
                            const uint8_t *bmp = (ci < n_ltr) ? glyphs[ci] : NULL;
                            if (bmp && (bmp[grow] & (0x80 >> gcol))) col = COL_SEG;
                        } else if (n_dig && y < dig_y1) {
                            int gcol = ((x - ltr_x1) % (GLYPH_W * (NOTE_SCALE / 2))) / (NOTE_SCALE / 2);
                            int grow = (y - txt_y0) / (NOTE_SCALE / 2);
                            const uint8_t *bmp = glyphs[n_ltr];
                            if (bmp && (bmp[grow] & (0x80 >> gcol))) col = COL_SEG;
                        }
                    }
                } else {
                    float angle = atan2f(dy, dx);
                    float rel   = fmodf(angle - s_phase, seg_span);
                    if (rel < 0.0f) rel += seg_span;
                    col = (rel < lit_span) ? col_seg : COL_BG;
                }
                s_ring_buf[(y - sy) * RING_W + (x - RING_X0)] = col;
            }
        }

        ili9341_draw_bitmap(RING_X0, sy, RING_W, rows, s_ring_buf);
    }

    render_bar(cents);
}
