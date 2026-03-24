#include "display.h"
#include "ili9341.h"
#include "pitch.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

#define LCD_W       240
#define LCD_H       320
#define CX          (LCD_W / 2)
#define CY          (LCD_H / 2)
#define N_SEG       12
#define R_INNER     60
#define R_OUTER     100
#define K_SPEED     20.0f
#define COL_SEG     0xFFFF
#define COL_BG      0x0000
#define FILL_RATIO  0.5f      /* fraction of each segment arc that is lit */

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
};

static const uint8_t *glyph_lookup(char c) {
    for (size_t i = 0; i < sizeof(s_glyphs) / sizeof(s_glyphs[0]); i++)
        if (s_glyphs[i].c == c) return s_glyphs[i].rows;
    return NULL;
}

static float    s_phase  = 0.0f;
static int64_t  s_last_t = 0;
static uint16_t s_row_buf[LCD_W];

esp_err_t display_init(void) {
    ili9341_fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    s_last_t = esp_timer_get_time();
    return ESP_OK;
}

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

void display_render_strobe(float detected_hz, const char *note) {
    int64_t now = esp_timer_get_time();
    float dt = (s_last_t == 0) ? 0.033f : (float)(now - s_last_t) / 1e6f;
    if (dt > 0.1f) dt = 0.1f;
    s_last_t = now;

    float nearest_hz = pitch_hz_to_nearest_hz(detected_hz);
    float cents      = pitch_hz_to_cents(detected_hz);

    s_phase += 2.0f * (float)M_PI * (detected_hz - nearest_hz) / nearest_hz * K_SPEED * dt;
    s_phase = fmodf(s_phase, 2.0f * (float)M_PI);

    uint16_t col_seg = (fabsf(cents) <= 5.0f) ? 0xE007u : 0xFFFFu;

    const float seg_span = 2.0f * (float)M_PI / N_SEG;
    const float lit_span = seg_span * FILL_RATIO;
    const float r_inner2 = (float)(R_INNER * R_INNER);
    const float r_outer2 = (float)(R_OUTER * R_OUTER);

    /* Pre-compute note glyph pointers and text bounding box so we can
     * render the note inline with the ring scan — no second pass, no flicker. */
    int nlen = note ? (int)strlen(note) : 0;
    const uint8_t *glyphs[4] = {NULL, NULL, NULL, NULL};
    for (int i = 0; i < nlen && i < 4; i++) glyphs[i] = glyph_lookup(note[i]);
    int txt_x0 = CX - (nlen * GLYPH_W * NOTE_SCALE) / 2;
    int txt_y0 = CY - (GLYPH_H * NOTE_SCALE) / 2;
    int txt_x1 = txt_x0 + nlen * GLYPH_W * NOTE_SCALE;
    int txt_y1 = txt_y0 + GLYPH_H * NOTE_SCALE;

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

    render_bar(cents);
}
