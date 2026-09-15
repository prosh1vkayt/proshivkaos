/* gui/hal_gfx.c — примитивы рисования. Архитектурно-нейтральный файл:
 * всё, что тут есть, работает поверх gui/gfxfb.c и ничего не знает ни про
 * VGA-порты, ни про ramfb.
 *
 * ИСТИННЫЙ ЦВЕТ И СГЛАЖИВАНИЕ. Раньше каждая фигура набиралась по пикселю,
 * а переходы цвета — дизерингом из палитры в 256 цветов. Теперь:
 *   - градиенты гладкие: цвет считается на каждую строку, строка
 *     заливается целиком;
 *   - скругления сглажены: у пикселя на краю дуги считается, какая доля
 *     его площади внутри фигуры (по четырём подстрокам), и цвет
 *     смешивается с фоном в этой доле;
 *   - тени мягкие и полупрозрачные, а не «через пиксель».
 * И всё это отрезками строк, а не по пикселю: прямые участки — одна
 * заливка на строку, по пикселю считаются только края дуг.
 */
#include "hal_gfx.h"
#include "gfxfb.h"
#include "palette.h"

const uint8_t *font8x8_get_glyph(char c);

static inline uint32_t pal(uint8_t c) { return g_palette_xrgb[c]; }
uint32_t hal_gfx_palette_rgb(uint8_t idx) { return pal(idx); }

int hal_gfx_width(void)  { return gfxfb_width(); }
int hal_gfx_height(void) { return gfxfb_height(); }

void hal_gfx_put_pixel(int x, int y, uint8_t color) { gfxfb_put_pixel(x, y, color); }
void hal_gfx_clear(uint8_t color)                   { gfxfb_clear(color); }
void hal_gfx_present(void)                          { gfxfb_present(); }

void hal_gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    gfxfb_fill_rect(x, y, w, h, color);
}

void hal_gfx_fill_rect_rgb(int x, int y, int w, int h, uint32_t rgb) {
    gfxfb_fill_rect_rgb(x, y, w, h, rgb);
}

void hal_gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
    gfxfb_fill_rect(x, y, w, 1, color);
    gfxfb_fill_rect(x, y + h - 1, w, 1, color);
    gfxfb_fill_rect(x, y, 1, h, color);
    gfxfb_fill_rect(x + w - 1, y, 1, h, color);
}

void hal_gfx_blit(int x, int y, int w, int h, const uint8_t *data) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            gfxfb_put_pixel(x + i, y + j, data[j * w + i]);
}

/* ---------------- Цвет ---------------- */

/* Цвет на доле t/n пути от a к b. */
static uint32_t lerp_rgb(uint32_t a, uint32_t b, int t, int n) {
    if (n <= 0) return a;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return GFX_RGB(ar + (br - ar) * t / n, ag + (bg - ag) * t / n, ab + (bb - ab) * t / n);
}

void hal_gfx_gradient_v_rgb(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++)
        gfxfb_fill_rect_rgb(x, y + j, w, 1, lerp_rgb(top, bottom, j, h > 1 ? h - 1 : 1));
}

/* Имена с «dither» остались от палитры: переход теперь гладкий. */
void hal_gfx_dither_gradient_v(int x, int y, int w, int h,
                                uint8_t top_color, uint8_t bottom_color) {
    hal_gfx_gradient_v_rgb(x, y, w, h, pal(top_color), pal(bottom_color));
}

void hal_gfx_dither_gradient_multi(int x, int y, int w, int h,
                                    const uint8_t *colors, int color_count) {
    if (color_count < 1) return;
    if (color_count == 1) { gfxfb_fill_rect(x, y, w, h, colors[0]); return; }

    int bands = color_count - 1;
    int span = (h > 1) ? h - 1 : 1;
    for (int j = 0; j < h; j++) {
        int pos  = j * bands;                 /* в долях span */
        int band = pos / span;
        if (band >= bands) band = bands - 1;
        int t = pos - band * span;
        gfxfb_fill_rect_rgb(x, y + j, w, 1,
                            lerp_rgb(pal(colors[band]), pal(colors[band + 1]), t, span));
    }
}

/* ---------------- Скругления ---------------- */

static uint64_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

#define AA_SUB     4            /* подстрок на пиксель                  */
#define AA_FULL    (AA_SUB * 256)
#define RADIUS_MAX 512

/* Покрытие пикселей левой дуги в строке j фигуры высоты h и радиуса r:
 * cov[i] для i в [0, r), от 0 до AA_FULL. Правая дуга — зеркально.
 * Возвращает 0, если строка вне скруглений (вся строка залита целиком). */
static int arc_row_coverage(int j, int h, int r, uint16_t *cov) {
    if (r <= 0 || (j >= r && j < h - r)) return 0;

    for (int i = 0; i < r; i++) cov[i] = 0;
    int64_t r256 = (int64_t)r * 256;

    for (int s = 0; s < AA_SUB; s++) {
        int64_t yy = (int64_t)j * 256 + (s * 256 + 128) / AA_SUB;
        int64_t dy = (j < r) ? r256 - yy : yy - (int64_t)(h - r) * 256;
        int64_t left;                          /* левая граница, 1/256 пикселя */
        if (dy <= 0)          left = 0;
        else if (dy >= r256)  left = r256;
        else                  left = r256 - (int64_t)isqrt64((uint64_t)(r256 * r256 - dy * dy));

        for (int i = 0; i < r; i++) {
            int64_t c = (int64_t)(i + 1) * 256 - left;
            if (c <= 0) continue;
            cov[i] += (uint16_t)(c >= 256 ? 256 : c);
        }
    }
    return 1;
}

int hal_gfx_in_rounded_rect(int i, int j, int w, int h, int radius) {
    if (radius <= 0) return 1;
    int cx, cy;
    if      (i < radius)      cx = radius;
    else if (i >= w - radius) cx = w - 1 - radius;
    else                      return 1;
    if      (j < radius)      cy = radius;
    else if (j >= h - radius) cy = h - 1 - radius;
    else                      return 1;
    int dx = i - cx, dy = j - cy;
    return (dx * dx + dy * dy) <= radius * radius;
}

static int clamp_radius(int r, int w, int h) {
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r > RADIUS_MAX) r = RADIUS_MAX;
    return r;
}

/* Сглаженный скруглённый прямоугольник с вертикальным градиентом.
 * hole — прямоугольник, который всё равно будет закрыт сверху (для тени):
 * внутри него смешивать незачем. */
typedef struct { int x0, y0, x1, y1; } hole_t;

static void rrect_fill(int x, int y, int w, int h, int r,
                       uint32_t top, uint32_t bottom, uint32_t alpha255,
                       const hole_t *hole) {
    if (w <= 0 || h <= 0 || alpha255 == 0) return;
    r = clamp_radius(r, w, h);
    uint16_t cov[RADIUS_MAX];

    for (int j = 0; j < h; j++) {
        uint32_t col = (top == bottom) ? top : lerp_rgb(top, bottom, j, h > 1 ? h - 1 : 1);
        int yy = y + j;
        int mid_x0 = x, mid_x1 = x + w;

        if (arc_row_coverage(j, h, r, cov)) {
            for (int i = 0; i < r; i++) {
                uint32_t a = (uint32_t)cov[i] * alpha255 / AA_FULL;
                if (!a) continue;
                gfxfb_blend_pixel(x + i, yy, col, a);
                gfxfb_blend_pixel(x + w - 1 - i, yy, col, a);
            }
            mid_x0 = x + r;
            mid_x1 = x + w - r;
        }

        if (hole && yy >= hole->y0 && yy < hole->y1) {
            int a0 = mid_x0, a1 = hole->x0 < mid_x1 ? hole->x0 : mid_x1;
            if (a1 > a0) gfxfb_span_rgb(a0, yy, a1 - a0, col, alpha255);
            int b0 = hole->x1 > mid_x0 ? hole->x1 : mid_x0, b1 = mid_x1;
            if (b1 > b0) gfxfb_span_rgb(b0, yy, b1 - b0, col, alpha255);
        } else if (mid_x1 > mid_x0) {
            gfxfb_span_rgb(mid_x0, yy, mid_x1 - mid_x0, col, alpha255);
        }
    }
}

/* Сглаженный контур толщиной в пиксель: покрытие внешней фигуры минус
 * покрытие внутренней, сдвинутой на пиксель внутрь. */
static void rrect_outline(int x, int y, int w, int h, int r, uint32_t col, uint32_t alpha255) {
    if (w <= 2 || h <= 2) { rrect_fill(x, y, w, h, r, col, col, alpha255, 0); return; }
    r = clamp_radius(r, w, h);
    int ri = r > 0 ? r - 1 : 0;
    uint16_t co[RADIUS_MAX], ci[RADIUS_MAX];

    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (j == 0 || j == h - 1) {
            /* Верхняя и нижняя грань: внешняя дуга целиком — это и есть контур. */
            if (arc_row_coverage(j, h, r, co)) {
                for (int i = 0; i < r; i++) {
                    uint32_t a = (uint32_t)co[i] * alpha255 / AA_FULL;
                    gfxfb_blend_pixel(x + i, yy, col, a);
                    gfxfb_blend_pixel(x + w - 1 - i, yy, col, a);
                }
                gfxfb_span_rgb(x + r, yy, w - 2 * r, col, alpha255);
            } else {
                gfxfb_span_rgb(x, yy, w, col, alpha255);
            }
            continue;
        }

        int outer = arc_row_coverage(j, h, r, co);
        int inner = arc_row_coverage(j - 1, h - 2, ri, ci);
        if (!outer) {
            if (!inner) {
                gfxfb_blend_pixel(x, yy, col, alpha255);
                gfxfb_blend_pixel(x + w - 1, yy, col, alpha255);
                continue;
            }
            for (int i = 0; i < r; i++) co[i] = AA_FULL;
        }
        for (int i = 0; i < r && i < RADIUS_MAX; i++) {
            int in = 0;
            if (i >= 1) in = inner ? ci[i - 1] : AA_FULL;
            int a = (int)co[i] - in;
            if (a <= 0) continue;
            uint32_t aa = (uint32_t)a * alpha255 / AA_FULL;
            gfxfb_blend_pixel(x + i, yy, col, aa);
            gfxfb_blend_pixel(x + w - 1 - i, yy, col, aa);
        }
    }
}

void hal_gfx_set_clip(int x, int y, int w, int h) { gfxfb_set_clip(x, y, w, h); }
void hal_gfx_reset_clip(void)                      { gfxfb_reset_clip(); }

void hal_gfx_fill_rounded_rect_rgb(int x, int y, int w, int h, uint32_t rgb,
                                   int radius, uint32_t alpha255) {
    rrect_fill(x, y, w, h, radius, rgb, rgb, alpha255, 0);
}

void hal_gfx_fill_rounded_rect(int x, int y, int w, int h, uint8_t color, int radius) {
    rrect_fill(x, y, w, h, radius, pal(color), pal(color), 255, 0);
}

void hal_gfx_draw_rounded_rect(int x, int y, int w, int h, uint8_t color, int radius) {
    rrect_outline(x, y, w, h, radius, pal(color), 255);
}

void hal_gfx_draw_glossy_button(int x, int y, int w, int h,
                                 uint8_t top_color, uint8_t bottom_color,
                                 uint8_t border_color, int radius) {
    rrect_fill(x, y, w, h, radius, pal(top_color), pal(bottom_color), 255, 0);

    /* Блик — полупрозрачная белая строка сразу под верхней гранью. */
    if (h > 4) {
        int r = clamp_radius(radius, w, h);
        gfxfb_blend_rect_rgb(x + r, y + 1, w - 2 * r, 1, GFX_RGB(255, 255, 255), 60);
    }

    rrect_outline(x, y, w, h, radius, pal(border_color), 160);
}

void hal_gfx_drop_shadow(int x, int y, int w, int h, int radius, int depth) {
    if (depth < 1) return;

    /* Мягкая тень: несколько слоёв, каждый шире предыдущего и прозрачнее,
       сдвинутые вниз. Внутри фигуры смешивать незачем — её нарисуют
       сверху, поэтому середина пропускается. */
    uint32_t col = pal(GFX_UI_SHADOW);
    int layers = depth * 2;
    int r = clamp_radius(radius, w, h);
    hole_t hole = { x + r, y + r, x + w - r, y + h - r };

    for (int k = 0; k < layers; k++) {
        int e = layers - k;                    /* насколько шире фигуры */
        rrect_fill(x - e + 1, y - e + 1 + depth, w + 2 * e - 2, h + 2 * e - 2,
                   radius + e, col, col, 90 / layers + 6, &hole);
    }
}

/* ---------------- Шар ---------------- */

void hal_gfx_fill_sphere(int cx, int cy, int r, int light_x, int light_y,
                          int ramp_start, int ramp_count) {
    if (r <= 0 || ramp_count <= 0) return;
    if (ramp_count > 32) ramp_count = 32;

    int lx = light_x - cx, ly = light_y - cy;
    int light_dist = (int)isqrt64((uint64_t)(lx * lx + ly * ly));
    int max_d = light_dist + r;
    if (max_d < 1) max_d = 1;

    /* Шкала расстояний в квадратах: шестнадцать подступеней на ступень
       палитры, между ступенями цвет смешивается — колец не видно. */
    enum { SUBS = 16 };
    int sub_count = ramp_count * SUBS;
    static int bound[32 * SUBS];
    for (int i = 0; i < sub_count; i++) {
        int d = (int)(((long)max_d * (i + 1)) / sub_count);
        bound[i] = d * d;
    }

    int r2 = r * r, rin = (r - 1) * (r - 1), rout = (r + 1) * (r + 1);
    for (int y = -r - 1; y <= r + 1; y++) {
        for (int x = -r - 1; x <= r + 1; x++) {
            int q = x * x + y * y;
            if (q > rout) continue;

            int dx = (cx + x) - light_x, dy = (cy + y) - light_y;
            int d2 = dx * dx + dy * dy;
            int lo = 0, hi = sub_count - 1, sub = sub_count - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (d2 <= bound[mid]) { sub = mid; hi = mid - 1; }
                else                  { lo = mid + 1; }
            }
            int step = sub / SUBS, frac = sub % SUBS;
            if (step >= ramp_count - 1) { step = ramp_count - 1; frac = 0; }
            uint32_t c = lerp_rgb(pal((uint8_t)(ramp_start + step)),
                                  pal((uint8_t)(ramp_start + (step + 1 < ramp_count ? step + 1 : step))),
                                  frac, SUBS);

            if (q <= rin) { gfxfb_put_rgb(cx + x, cy + y, c); continue; }
            /* Край: доля пикселя внутри окружности. */
            int dist256 = (int)isqrt64((uint64_t)q * 65536u);
            int a = r * 256 + 128 - dist256;
            if (a <= 0) continue;
            gfxfb_blend_pixel(cx + x, cy + y, c, a >= 256 ? 255 : (uint32_t)a);
            (void)r2;
        }
    }
}

/* ---------------- Текст ---------------- */

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

void hal_gfx_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    const uint8_t *glyph = font8x8_get_glyph(to_upper(c));

    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++)
            gfxfb_put_pixel(x + col, y + row, ((bits >> (7 - col)) & 1) ? fg : bg);
    }
}

void hal_gfx_draw_string(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += FONT_H; }
        else { hal_gfx_draw_char(cx, y, *s, fg, bg); cx += FONT_W; }
        s++;
    }
}

static void draw_char_transparent(int x, int y, char c, uint8_t fg) {
    const uint8_t *glyph = font8x8_get_glyph(to_upper(c));

    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++)
            if ((bits >> (7 - col)) & 1)
                gfxfb_put_pixel(x + col, y + row, fg);
    }
}

void hal_gfx_draw_string_transparent(int x, int y, const char *s, uint8_t fg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += FONT_H; }
        else { draw_char_transparent(cx, y, *s, fg); cx += FONT_W; }
        s++;
    }
}

void hal_gfx_draw_char_scaled(int x, int y, char c, uint8_t fg, uint8_t bg, int scale) {
    if (scale < 1) scale = 1;
    const uint8_t *glyph = font8x8_get_glyph(to_upper(c));

    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        /* Подряд идущие точки одного цвета — одним прямоугольником.
           Раньше каждая из шестидесяти четырёх точек буквы была
           отдельным вызовом; у типичной буквы в ряду две-три смены цвета,
           так что вызовов становится втрое-вчетверо меньше. Квадратик
           размером scale на scale вместо scale*scale отдельных точек был
           первым шагом той же экономии. */
        int col = 0;
        while (col < FONT_W) {
            int on = (bits >> (7 - col)) & 1;
            int start = col;
            while (col < FONT_W && (((bits >> (7 - col)) & 1) == on)) col++;
            gfxfb_fill_rect(x + start * scale, y + row * scale,
                            (col - start) * scale, scale, on ? fg : bg);
        }
    }
}

void hal_gfx_draw_string_scaled(int x, int y, const char *s, uint8_t fg, int scale) {
    if (scale < 1) scale = 1;

    int cx = x;
    while (*s) {
        if (*s == '\n') {
            cx = x;
            y += FONT_H * scale;
            s++;
            continue;
        }

        const uint8_t *glyph = font8x8_get_glyph(to_upper(*s));
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                if (!((bits >> (7 - col)) & 1)) continue;
                gfxfb_fill_rect(cx + col * scale, y + row * scale, scale, scale, fg);
            }
        }
        cx += FONT_W * scale;
        s++;
    }
}

int hal_gfx_string_width(const char *s, int scale) {
    if (scale < 1) scale = 1;

    int n = 0, best = 0;
    while (*s) {
        if (*s == '\n') { if (n > best) best = n; n = 0; }
        else n++;
        s++;
    }
    if (n > best) best = n;
    return best * FONT_W * scale;
}

void hal_gfx_draw_string_centered(int x, int y, int w, const char *s, uint8_t fg, int scale) {
    int sw = hal_gfx_string_width(s, scale);
    hal_gfx_draw_string_scaled(x + (w - sw) / 2, y, s, fg, scale);
}
