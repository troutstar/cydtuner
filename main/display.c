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
#define STROBE_MODE_ARC_SOLID    0   /* solid white arc segments */
#define STROBE_MODE_ARC_CHECKER  1   /* 8x8 checkerboard arc segments */
#define STROBE_MODE_RACK         2   /* horizontal rack-style scrolling strobe */
#define STROBE_MODE_MOIRE        3   /* two overlapping vertical-line grids */
#define STROBE_MODE_MOIRE_CIRCLES 4  /* two overlapping concentric-circle grids */
#define STROBE_MODE_MOIRE_SPIRAL  5  /* two overlapping Archimedean spirals, one rotates */

#define ORIENT_PORTRAIT          0
#define ORIENT_LANDSCAPE         1

#define STROBE_MODE   STROBE_MODE_RACK
#define ORIENTATION   ORIENT_LANDSCAPE

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
#define CENTS_TO_RPM  0.3f
#define COL_SEG     0xFFFF
#define COL_BG      0x0000
#define FILL_RATIO  0.45f

/* Arc geometry */
#define ARC_MIN_ANGLE  (-7.0f * (float)M_PI / 9.0f)   /* -140 degrees */
#define ARC_MAX_ANGLE  (-2.0f * (float)M_PI / 9.0f)   /* -40  degrees */
#define RING_X0  (CX - R_OUTER)
#define RING_Y0  (CY - R_OUTER)
#define RING_W   (R_OUTER * 2 + 1)
#define RING_H   81

/* Strip buffer width — wide enough for both arc (RING_W=229) and rack (LCD_W=240) */
#define STRIP_BUF_W  LCD_W

/* Rack geometry */
#define RACK_Y0         70    /* top of strobe band */
#define RACK_H          60    /* height of strobe band */
#define RACK_SEG_W      20    /* stripe period in pixels */
#define RACK_NOTE_Y0    150   /* note label below band */
#define RACK_SPEED      4.0f  /* visual speed multiplier vs arc phase */

/* Moire geometry */
#define MOIRE_P1        10    /* static grid period in pixels */
#define MOIRE_P2        11    /* moving grid period — difference creates interference bands */
#define MOIRE_LINE_W     2    /* grid line width */
#define MOIRE_SPEED      1.0f /* phase-to-scroll multiplier */
#define MOIRE_H          160  /* rows used by moire pattern (top of screen) */
#define MOIRE_NOTE_Y0    (MOIRE_H + (LCD_H - MOIRE_H - NOTE_H) / 2)  /* centred in bottom zone */

/* Arc note position */
#define NOTE_W   120
#define NOTE_H   72
#define NOTE_X0  (CX - NOTE_W / 2)
#define NOTE_Y0  104

#define BAR_W    200
#define BAR_H    19
#define BAR_X0   ((LCD_W - BAR_W) / 2)   /* centred horizontally */
#define BAR_Y0   200
#define BAR_CX   (BAR_X0 + BAR_W / 2)    /* screen x of bar centre */

/* ---- Minimal 8x8 bitmap font -------------------------------------------------- */
#define GLYPH_W    8
#define GLYPH_H    8
#define NOTE_SCALE 8
#define DIG_SCALE  3   /* superscript octave digit — smaller, top-right of main letter */

typedef struct { char c; uint8_t rows[GLYPH_H]; } glyph_t;

static const glyph_t s_glyphs[] = {
    {'A', {0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x00}},
    {'B', {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}},
    {'C', {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}},
    {'D', {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}},
    {'E', {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}},
    {'F', {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00}},
    {'G', {0x7C,0xC0,0xC0,0xCF,0xC3,0xC3,0x7E,0x00}},
    {'#', {0x24,0x24,0x7E,0x24,0x7E,0x24,0x24,0x00}},
    {'-', {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}},
    {'+', {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}},
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
static float     s_phase      = 0.0f;
static float     s_ref_hz     = 0.0f;
static int64_t   s_last_t     = 0;
static float     s_a4_disp_hz = 440.0f;

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

/* ---- Shared sub-renderers ----------------------------------------------------- */

static void render_bar(float cents) {
    int fill_w = (int)((fabsf(cents) / 50.0f) * 100.0f);
    if (fill_w > 100) fill_w = 100;
    int fill_x = (cents < 0.0f) ? (BAR_CX - fill_w) : BAR_CX;
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

/* Fill s_note_buf with glyph pixels (no blit). Used by both render_note_at
 * and render_moire (cutout mask). */
static void fill_note_buf(const char *ref_note, uint16_t color) {
    int nlen = (int)strlen(ref_note);
    const uint8_t *glyphs[4] = {NULL, NULL, NULL, NULL};
    for (int i = 0; i < nlen && i < 4; i++) glyphs[i] = glyph_lookup(ref_note[i]);
    int n_dig   = (nlen > 0 && ref_note[nlen-1] >= '0' && ref_note[nlen-1] <= '9') ? 1 : 0;
    int n_ltr   = nlen - n_dig;
    int ltr_w   = n_ltr * GLYPH_W * NOTE_SCALE;
    int dig_w   = n_dig * GLYPH_W * DIG_SCALE;
    /* Centre on the main letter only; digit sits top-right as superscript */
    int txt_lx  = (NOTE_W - ltr_w) / 2;
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
                s_note_buf[ly * NOTE_W + lx] = color;
        }
        /* Superscript digit: DIG_SCALE, top-aligned with main letter */
        if (n_dig) {
            int dig_row = (ly - txt_ly) / DIG_SCALE;
            if (dig_row < GLYPH_H) {
                for (int lx = ltr_lx1; lx < ltr_lx1 + dig_w && lx < NOTE_W; lx++) {
                    int gcol = ((lx - ltr_lx1) % (GLYPH_W * DIG_SCALE)) / DIG_SCALE;
                    const uint8_t *bmp = glyphs[n_ltr];
                    if (bmp && (bmp[dig_row] & (0x80 >> gcol)))
                        s_note_buf[ly * NOTE_W + lx] = color;
                }
            }
        }
    }
}

static void render_note_at(const char *ref_note, int y0) {
    fill_note_buf(ref_note, COL_SEG);
    ili9341_draw_bitmap(NOTE_X0, y0, NOTE_W, NOTE_H, s_note_buf);
}

/* ---- A4 header strip ----------------------------------------------------------- */
/* Renders a 20-px strip at the top of the screen showing the A4 reference.
 * Format: "- A4 440 +" where '-' (left) and '+' (right) indicate tap zones.
 * Uses s_ring_buf in STRIP_H-row passes — call only when s_ring_buf is free. */

#define A4_STRIP_H  20
#define A4_SCALE     2   /* 16x16 px glyphs */

static void render_glyph_to_buf(char c, int gx0, int gy0_abs,
                                 int sy, int rows, uint16_t color)
{
    const uint8_t *bmp = glyph_lookup(c);
    if (!bmp) return;
    for (int row = 0; row < GLYPH_H; row++) {
        for (int col = 0; col < GLYPH_W; col++) {
            if (!(bmp[row] & (0x80 >> col))) continue;
            for (int sr = 0; sr < A4_SCALE; sr++) {
                int py = gy0_abs + row * A4_SCALE + sr;
                if (py < sy || py >= sy + rows) continue;
                for (int sc = 0; sc < A4_SCALE; sc++) {
                    int px = gx0 + col * A4_SCALE + sc;
                    if (px >= 0 && px < LCD_W)
                        s_ring_buf[(py - sy) * LCD_W + px] = color;
                }
            }
        }
    }
}

static void render_a4_strip(void)
{
    char str[8];
    snprintf(str, sizeof(str), "A4 %d", (int)(s_a4_disp_hz + 0.5f));

    const int gw    = GLYPH_W * A4_SCALE;   /* 16 px per glyph */
    const int gh    = GLYPH_H * A4_SCALE;   /* 16 px per glyph */
    const int gy0   = (A4_STRIP_H - gh) / 2;  /* vertical offset in strip */
    int       nch   = (int)strlen(str);
    int       tx0   = (LCD_W - nch * gw) / 2; /* center text */

    /* '-' tap indicator on the left, '+' on the right */
    const int left_x  = 4;
    const int right_x = LCD_W - gw - 4;

    for (int sy = 0; sy < A4_STRIP_H; sy += STRIP_H) {
        int rows = sy + STRIP_H <= A4_STRIP_H ? STRIP_H : (A4_STRIP_H - sy);
        memset(s_ring_buf, 0, (size_t)(LCD_W * rows) * sizeof(uint16_t));

        /* Center label */
        for (int ci = 0; ci < nch; ci++)
            render_glyph_to_buf(str[ci], tx0 + ci * gw, gy0, sy, rows, 0xFFFFu);

        /* Tap-zone indicators */
        render_glyph_to_buf('-', left_x,  gy0, sy, rows, 0x07E0u);  /* green */
        render_glyph_to_buf('+', right_x, gy0, sy, rows, 0x07E0u);

        ili9341_draw_bitmap(0, sy, LCD_W, rows, s_ring_buf);
    }
}

void display_set_a4(float hz)
{
    if (hz == s_a4_disp_hz) return;
    s_a4_disp_hz = hz;
    render_a4_strip();
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

    render_note_at(s->note, NOTE_Y0);
    render_bar(s->cents);
}

static void render_rack(const strobe_state_t *s) {
    /* Convert phase (radians) to pixel scroll offset.
     * One full 2π cycle = N_SEG stripe periods of RACK_SEG_W pixels each. */
    float phase_px = s->phase * (float)RACK_SEG_W * (float)N_SEG * RACK_SPEED / (2.0f * (float)M_PI);
    int offset = (int)phase_px;
    int lit_w  = (int)(RACK_SEG_W * FILL_RATIO);

    for (int sy = RACK_Y0; sy < RACK_Y0 + RACK_H; sy += STRIP_H) {
        int ey = sy + STRIP_H - 1;
        if (ey >= RACK_Y0 + RACK_H) ey = RACK_Y0 + RACK_H - 1;
        if (ey >= LCD_H) ey = LCD_H - 1;
        if (sy < 0) continue;
        int rows = ey - sy + 1;

        memset(s_ring_buf, 0, LCD_W * rows * sizeof(uint16_t));

        int band_center = RACK_Y0 + RACK_H / 2;
        for (int y = sy; y <= ey; y++) {
            /* Chevron: tilt stripes by cents deviation. At ±50 cents the tilt
             * equals ±RACK_SEG_W pixels from center to band edge. */
            int chevron = (int)(-s->cents * (float)RACK_SEG_W * fabsf((float)(y - band_center))
                                / (50.0f * (float)(RACK_H / 2)));
            for (int x = 0; x < LCD_W; x++) {
                int rel = ((x - offset - chevron) % RACK_SEG_W + RACK_SEG_W) % RACK_SEG_W;
                if (rel < lit_w)
                    s_ring_buf[(y - sy) * LCD_W + x] = s->col_seg;
            }
        }

        ili9341_draw_bitmap(0, sy, LCD_W, rows, s_ring_buf);
    }

    render_note_at(s->note, RACK_NOTE_Y0);
}

static void render_moire_note_zone(const strobe_state_t *s) {
    uint16_t note_col = (fabsf(s->cents) <= 3.0f) ? 0xE007u : 0xFFFFu;
    fill_note_buf(s->note, note_col);
    const int ny0 = MOIRE_NOTE_Y0;
    const int ny1 = ny0 + NOTE_H;

    for (int sy = MOIRE_H; sy < LCD_H; sy += STRIP_H) {
        int ey = sy + STRIP_H - 1;
        if (ey >= LCD_H) ey = LCD_H - 1;
        int rows = ey - sy + 1;

        memset(s_ring_buf, 0, LCD_W * rows * sizeof(uint16_t));

        for (int y = sy; y <= ey; y++) {
            if (y < ny0 || y >= ny1) continue;
            int note_row = y - ny0;
            for (int x = NOTE_X0; x < NOTE_X0 + NOTE_W; x++) {
                if (s_note_buf[note_row * NOTE_W + (x - NOTE_X0)] != 0)
                    s_ring_buf[(y - sy) * LCD_W + x] = note_col;
            }
        }

        ili9341_draw_bitmap(0, sy, LCD_W, rows, s_ring_buf);
    }
}

static void render_moire(const strobe_state_t *s) {
    float phase_px = s->phase * (float)MOIRE_P2 * (float)N_SEG * MOIRE_SPEED
                     / (2.0f * (float)M_PI);
    int offset = (int)phase_px;

    /* Top zone: moire pattern */
    for (int sy = 0; sy < MOIRE_H; sy += STRIP_H) {
        int ey = sy + STRIP_H - 1;
        if (ey >= MOIRE_H) ey = MOIRE_H - 1;
        int rows = ey - sy + 1;

        for (int y = sy; y <= ey; y++) {
            for (int x = 0; x < LCD_W; x++) {
                int g1 = (x % MOIRE_P1) < MOIRE_LINE_W;
                int g2 = ((x + offset) % MOIRE_P2 + MOIRE_P2) % MOIRE_P2 < MOIRE_LINE_W;
                s_ring_buf[(y - sy) * LCD_W + x] = (g1 ^ g2) ? COL_SEG : COL_BG;
            }
        }

        ili9341_draw_bitmap(0, sy, LCD_W, rows, s_ring_buf);
    }

    render_moire_note_zone(s);
}

static void render_moire_circles(const strobe_state_t *s) {
    /* Concentric circles: Grid 1 static, Grid 2 radially offset with phase.
     * Different ring periods create expanding/contracting moire rings. */
    float phase_offset = s->phase * (float)MOIRE_P2 * (float)N_SEG * MOIRE_SPEED
                         / (2.0f * (float)M_PI);
    int offset = (int)phase_offset;

    const int cx = LCD_W / 2;
    const int cy = MOIRE_H / 2;

    for (int sy = 0; sy < MOIRE_H; sy += STRIP_H) {
        int ey = sy + STRIP_H - 1;
        if (ey >= MOIRE_H) ey = MOIRE_H - 1;
        int rows = ey - sy + 1;

        for (int y = sy; y <= ey; y++) {
            float dy = (float)(y - cy);
            for (int x = 0; x < LCD_W; x++) {
                float dx = (float)(x - cx);
                int r = (int)sqrtf(dx * dx + dy * dy);
                int g1 = r % MOIRE_P1 < MOIRE_LINE_W;
                int g2 = (r + offset) % MOIRE_P2 < MOIRE_LINE_W;
                s_ring_buf[(y - sy) * LCD_W + x] = (g1 ^ g2) ? COL_SEG : COL_BG;
            }
        }

        ili9341_draw_bitmap(0, sy, LCD_W, rows, s_ring_buf);
    }

    render_moire_note_zone(s);
}

static void render_moire_spiral(const strobe_state_t *s) {
    /* Two Archimedean spirals with slightly different pitches (MOIRE_P1, MOIRE_P2).
     * Spiral offset scrolls at the same rate as the line/circle moire modes.
     * Different pitches guarantee visible interference bands at all phase values. */
    float spiral_off = s->phase * (float)MOIRE_P2 * (float)N_SEG * MOIRE_SPEED
                       / (2.0f * (float)M_PI);
    const float K1  = (float)MOIRE_P1 / (2.0f * (float)M_PI);
    const float K2  = (float)MOIRE_P2 / (2.0f * (float)M_PI);
    const float PAD = 100.0f * (float)MOIRE_P2;
    const int   cx  = LCD_W / 2;
    const int   cy  = MOIRE_H / 2;

    for (int sy = 0; sy < MOIRE_H; sy += STRIP_H) {
        int ey = sy + STRIP_H - 1;
        if (ey >= MOIRE_H) ey = MOIRE_H - 1;
        int rows = ey - sy + 1;

        for (int y = sy; y <= ey; y++) {
            float dy = (float)(y - cy);
            for (int x = 0; x < LCD_W; x++) {
                float dx    = (float)(x - cx);
                float r     = sqrtf(dx * dx + dy * dy);
                float theta = atan2f(dy, dx);

                int g1 = (int)fmodf(r - K1 * theta + PAD,           (float)MOIRE_P1) < MOIRE_LINE_W;
                int g2 = (int)fmodf(r - K2 * theta + spiral_off + PAD, (float)MOIRE_P2) < MOIRE_LINE_W;

                s_ring_buf[(y - sy) * LCD_W + x] = (g1 ^ g2) ? COL_SEG : COL_BG;
            }
        }

        ili9341_draw_bitmap(0, sy, LCD_W, rows, s_ring_buf);
    }

    render_moire_note_zone(s);
}

static void render_mode(const strobe_state_t *s) {
#if STROBE_MODE == STROBE_MODE_ARC_SOLID
    render_arc(s, 0);
#elif STROBE_MODE == STROBE_MODE_ARC_CHECKER
    render_arc(s, 1);
#elif STROBE_MODE == STROBE_MODE_RACK
    render_rack(s);
#elif STROBE_MODE == STROBE_MODE_MOIRE
    render_moire(s);
#elif STROBE_MODE == STROBE_MODE_MOIRE_CIRCLES
    render_moire_circles(s);
#elif STROBE_MODE == STROBE_MODE_MOIRE_SPIRAL
    render_moire_spiral(s);
#endif
}

static void clear_strobe_region(void) {
#if STROBE_MODE == STROBE_MODE_MOIRE || STROBE_MODE == STROBE_MODE_MOIRE_CIRCLES || STROBE_MODE == STROBE_MODE_MOIRE_SPIRAL
    memset(s_ring_buf, 0, LCD_W * STRIP_H * sizeof(uint16_t));
    for (int sy = 0; sy < LCD_H; sy += STRIP_H) {
        int rows = (sy + STRIP_H <= LCD_H) ? STRIP_H : (LCD_H - sy);
        ili9341_draw_bitmap(0, sy, LCD_W, rows, s_ring_buf);
    }
#elif STROBE_MODE == STROBE_MODE_RACK
    memset(s_ring_buf, 0, LCD_W * STRIP_H * sizeof(uint16_t));
    for (int sy = RACK_Y0; sy < RACK_Y0 + RACK_H; sy += STRIP_H) {
        int rows = sy + STRIP_H <= RACK_Y0 + RACK_H ? STRIP_H : (RACK_Y0 + RACK_H - sy);
        ili9341_draw_bitmap(0, sy, LCD_W, rows, s_ring_buf);
    }
#else
    memset(s_ring_buf, 0, RING_W * STRIP_H * sizeof(uint16_t));
    for (int sy = RING_Y0; sy < RING_Y0 + RING_H; sy += STRIP_H) {
        int rows = sy + STRIP_H <= RING_Y0 + RING_H ? STRIP_H : (RING_Y0 + RING_H - sy);
        ili9341_draw_bitmap(RING_X0, sy, RING_W, rows, s_ring_buf);
    }
#endif
/* moire mode owns the full screen — no note/bar regions to clear */
#if STROBE_MODE != STROBE_MODE_MOIRE
    memset(s_note_buf, 0, NOTE_W * NOTE_H * sizeof(uint16_t));
#  if STROBE_MODE == STROBE_MODE_RACK
    ili9341_draw_bitmap(NOTE_X0, RACK_NOTE_Y0, NOTE_W, NOTE_H, s_note_buf);
#  else
    ili9341_draw_bitmap(NOTE_X0, NOTE_Y0, NOTE_W, NOTE_H, s_note_buf);
#  endif
#endif
}

/* ---- Public API --------------------------------------------------------------- */

esp_err_t display_init(void) {
    s_ring_buf = heap_caps_malloc(STRIP_BUF_W * STRIP_H * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_ring_buf) { ESP_LOGE(TAG, "s_ring_buf alloc failed"); abort(); }

    s_bar_buf = heap_caps_malloc(BAR_W * BAR_H * sizeof(uint16_t),
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bar_buf) { ESP_LOGE(TAG, "s_bar_buf alloc failed"); abort(); }

    s_note_buf = heap_caps_malloc(NOTE_W * NOTE_H * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_note_buf) { ESP_LOGE(TAG, "s_note_buf alloc failed"); abort(); }

    ili9341_fill_rect(0, 0, LCD_W, LCD_H, COL_BG);
    render_a4_strip();
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
        clear_strobe_region();
#if STROBE_MODE == STROBE_MODE_ARC_SOLID || STROBE_MODE == STROBE_MODE_ARC_CHECKER
        render_bar(0.0f);
#endif
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
