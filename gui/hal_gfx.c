/* gui/hal_gfx.c — примитивы рисования. Архитектурно-нейтральный файл:
 * всё, что тут есть, работает поверх gui/gfxfb.c и ничего не знает ни про
 * VGA-порты, ни про ramfb. Инициализация экрана и смена разрешения живут
 * отдельно, в arch/<платформа>/hal_gfx_*.c — только они и различаются.
 */
#include "hal_gfx.h"
#include "gfxfb.h"

const uint8_t *font8x8_get_glyph(char c);

/* Матрица упорядоченного дизеринга Bayer 2x2 — даёт 4 воспринимаемых
 * уровня смешения из двух цветов. Одна на весь файл: ею рисуются и
 * градиенты, и кнопки, и тени. */
static const int g_bayer[2][2] = { {0, 2}, {3, 1} };

int hal_gfx_width(void)  { return gfxfb_width(); }
int hal_gfx_height(void) { return gfxfb_height(); }

void hal_gfx_put_pixel(int x, int y, uint8_t color) {
    gfxfb_put_pixel(x, y, color);
}

void hal_gfx_clear(uint8_t color) {
    gfxfb_clear(color);
}

void hal_gfx_present(void) {
    gfxfb_present();
}

void hal_gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            gfxfb_put_pixel(x + i, y + j, color);
}

void hal_gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
    for (int i = 0; i < w; i++) {
        gfxfb_put_pixel(x + i, y, color);
        gfxfb_put_pixel(x + i, y + h - 1, color);
    }
    for (int j = 0; j < h; j++) {
        gfxfb_put_pixel(x, y + j, color);
        gfxfb_put_pixel(x + w - 1, y + j, color);
    }
}

void hal_gfx_blit(int x, int y, int w, int h, const uint8_t *data) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            gfxfb_put_pixel(x + i, y + j, data[j * w + i]);
}

void hal_gfx_dither_gradient_v(int x, int y, int w, int h,
                                uint8_t top_color, uint8_t bottom_color) {
    for (int j = 0; j < h; j++) {
        /* какая доля высоты пройдена (0..3) — определяет, где смесь
           начинает перевешивать в сторону bottom_color */
        int threshold = (h > 1) ? (j * 4) / (h - 1) : 0;
        if (threshold > 3) threshold = 3;

        for (int i = 0; i < w; i++) {
            int b = g_bayer[j & 1][i & 1];
            gfxfb_put_pixel(x + i, y + j, (b < threshold) ? bottom_color : top_color);
        }
    }
}

void hal_gfx_dither_gradient_multi(int x, int y, int w, int h,
                                    const uint8_t *colors, int color_count) {
    if (color_count < 1) return;
    if (color_count == 1) { hal_gfx_fill_rect(x, y, w, h, colors[0]); return; }

    /* Высота делится на (color_count - 1) полос, в каждой — обычный
       дизеринг между соседней парой цветов. Получается плавный переход
       через сколько угодно опорных цветов. */
    int bands = color_count - 1;
    for (int j = 0; j < h; j++) {
        int pos   = (h > 1) ? (j * bands * 4) / (h - 1) : 0;   /* 0 .. bands*4 */
        int band  = pos / 4;
        if (band >= bands) band = bands - 1;
        int threshold = pos - band * 4;
        if (threshold > 3) threshold = 3;

        uint8_t top = colors[band];
        uint8_t bot = colors[band + 1];

        for (int i = 0; i < w; i++) {
            int b = g_bayer[j & 1][i & 1];
            gfxfb_put_pixel(x + i, y + j, (b < threshold) ? bot : top);
        }
    }
}

/* Попадает ли точка (i,j) ЗА пределы скруглённого угла прямоугольника w x h.
 *
 * Раньше угол просто срезался по диагонали (dx + dy >= radius). При
 * radius 2-3, как в десктопном интерфейсе, разницы с окружностью нет —
 * но иконкам приложений на телефоне нужен радиус в четверть их размера,
 * и там диагональный срез превращал квадрат в правильный восьмиугольник.
 * Поэтому теперь честная проверка расстояния до центра скругления. */
static int in_corner_cut(int i, int j, int w, int h, int radius) {
    if (radius <= 0) return 0;

    int cx, cy;
    if      (i < radius)      cx = radius;
    else if (i >= w - radius) cx = w - 1 - radius;
    else                      return 0;   /* по горизонтали — прямой участок */

    if      (j < radius)      cy = radius;
    else if (j >= h - radius) cy = h - 1 - radius;
    else                      return 0;   /* по вертикали — прямой участок */

    int dx = i - cx, dy = j - cy;
    return (dx * dx + dy * dy) > radius * radius;
}

/* Контур скруглённого прямоугольника: пиксель принадлежит контуру, если он
 * внутри фигуры, а хотя бы один сосед — уже снаружи. Так обводка точно
 * повторяет заливку при любом радиусе, без отдельной формулы для дуги. */
static void rounded_outline(int x, int y, int w, int h, uint8_t color, int radius) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (in_corner_cut(i, j, w, h, radius)) continue;
            if (i == 0 || j == 0 || i == w - 1 || j == h - 1 ||
                in_corner_cut(i - 1, j, w, h, radius) ||
                in_corner_cut(i + 1, j, w, h, radius) ||
                in_corner_cut(i, j - 1, w, h, radius) ||
                in_corner_cut(i, j + 1, w, h, radius))
                gfxfb_put_pixel(x + i, y + j, color);
        }
    }
}

void hal_gfx_set_clip(int x, int y, int w, int h) { gfxfb_set_clip(x, y, w, h); }
void hal_gfx_reset_clip(void)                      { gfxfb_reset_clip(); }

int hal_gfx_in_rounded_rect(int i, int j, int w, int h, int radius) {
    return !in_corner_cut(i, j, w, h, radius);
}

void hal_gfx_fill_rounded_rect(int x, int y, int w, int h, uint8_t color, int radius) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (!in_corner_cut(i, j, w, h, radius))
                gfxfb_put_pixel(x + i, y + j, color);
}

void hal_gfx_draw_rounded_rect(int x, int y, int w, int h, uint8_t color, int radius) {
    rounded_outline(x, y, w, h, color, radius);
}

void hal_gfx_draw_glossy_button(int x, int y, int w, int h,
                                 uint8_t top_color, uint8_t bottom_color,
                                 uint8_t border_color, int radius) {
    for (int j = 0; j < h; j++) {
        int threshold = (h > 1) ? (j * 4) / (h - 1) : 0;
        if (threshold > 3) threshold = 3;

        for (int i = 0; i < w; i++) {
            if (in_corner_cut(i, j, w, h, radius)) continue;
            int b = g_bayer[j & 1][i & 1];
            gfxfb_put_pixel(x + i, y + j, (b < threshold) ? bottom_color : top_color);
        }
    }

    /* Блик — ВТОРОЙ строкой, а не первой. Раньше он рисовался по самой
       верхней грани и тут же затирался контуром, который идёт по той же
       строке: кнопки выходили без блика вообще. */
    if (h > 2) {
        /* Блик набирается через пиксель, а не сплошной линией: на светлой
           кнопке разницы почти нет, а на тёмной сплошная белая полоса
           читается как царапина, а не как отблеск. */
        for (int i = 0; i < w; i++)
            if (!in_corner_cut(i, 1, w, h, radius) && g_bayer[1][i & 1] < 2)
                gfxfb_put_pixel(x + i, y + 1, GFX_WHITE);
    }

    rounded_outline(x, y, w, h, border_color, radius);
}

void hal_gfx_fill_sphere(int cx, int cy, int r, int light_x, int light_y,
                          int ramp_start, int ramp_count) {
    if (r <= 0 || ramp_count <= 0) return;
    if (ramp_count > 32) ramp_count = 32;

    /* Самая дальняя от источника света точка круга — противоположный край,
       то есть расстояние от света до центра плюс радиус. По этой величине
       и растягивается шкала, иначе на сильно смещённом свете половина
       оттенков не использовалась бы вовсе. */
    int lx = light_x - cx, ly = light_y - cy;
    int light_dist = 0;
    while ((light_dist + 1) * (light_dist + 1) <= lx * lx + ly * ly)
        light_dist++;                       /* целочисленный корень */
    int max_d = light_dist + r;
    if (max_d < 1) max_d = 1;

    /* Каждая ступень шкалы делится ещё на четыре подступени, а между двумя
       соседними цветами пиксели раскидываются матрицей Bayer. Из 24 цветов
       получается 96 воспринимаемых уровней — без этого на сфере размером в
       пол-экрана отчётливо видны концентрические кольца.

       Границы хранятся сразу в КВАДРАТАХ расстояния, чтобы не извлекать
       корень для каждого из сотен тысяч пикселей, а искать по ним —
       двоичным поиском, а не перебором. */
    int sub_count = ramp_count * 4;
    int bound[128];
    for (int i = 0; i < sub_count; i++) {
        int d = (int)(((long)max_d * (i + 1)) / sub_count);
        bound[i] = d * d;
    }

    int r2 = r * r;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r2) continue;        /* вне круга */

            int dx = (cx + x) - light_x;
            int dy = (cy + y) - light_y;
            int d2 = dx * dx + dy * dy;

            int lo = 0, hi = sub_count - 1, sub = sub_count - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (d2 <= bound[mid]) { sub = mid; hi = mid - 1; }
                else                  { lo = mid + 1; }
            }

            int step = sub / 4;
            int frac = sub % 4;
            if (step >= ramp_count - 1) { step = ramp_count - 1; frac = 0; }

            int b = g_bayer[(cy + y) & 1][(cx + x) & 1];
            int idx = (b < frac) ? step + 1 : step;
            if (idx > ramp_count - 1) idx = ramp_count - 1;

            gfxfb_put_pixel(cx + x, cy + y, (uint8_t)(ramp_start + idx));
        }
    }
}

void hal_gfx_drop_shadow(int x, int y, int w, int h, int radius, int depth) {
    if (depth < 1) return;

    /* Настоящей полупрозрачности на индексной палитре нет, поэтому тень —
     * это тот же силуэт, сдвинутый вниз-вправо и набранный "через пиксель"
     * по матрице дизеринга: на глаз получается полупрозрачная подложка.
     *
     * Предыдущая версия рисовала несколько слоёв с расширением во все
     * стороны — из-за расширения вверх и влево над каждой карточкой
     * оставалась тёмная полоса, которую сама карточка уже не закрывала. */
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (in_corner_cut(i, j, w, h, radius)) continue;
            if (g_bayer[j & 1][i & 1] >= 2) continue;   /* плотность ~50% */
            gfxfb_put_pixel(x + i + depth, y + j + depth, GFX_UI_SHADOW);
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
        for (int col = 0; col < FONT_W; col++) {
            uint8_t color = ((bits >> (7 - col)) & 1) ? fg : bg;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    gfxfb_put_pixel(x + col * scale + sx, y + row * scale + sy, color);
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
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        gfxfb_put_pixel(cx + col * scale + sx, y + row * scale + sy, fg);
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
