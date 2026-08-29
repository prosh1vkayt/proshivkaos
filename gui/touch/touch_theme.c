/* gui/touch/touch_theme.c */
#include "touch_theme.h"

touch_metrics_t TM;

void touch_theme_init(int screen_w, int screen_h) {
    TM.screen_w = screen_w;
    TM.screen_h = screen_h;

    /* Масштаб шрифта привязан к ширине экрана: на 480 точках буква
       становится 16x16 (читаемо), на 720 — 24x24. Делить именно ширину, а
       не высоту: у телефонов высота гуляет от 16:9 до 21:9, а ширина в
       пределах класса устройств почти постоянна. */
    TM.scale = screen_w / 240;
    if (TM.scale < 1) TM.scale = 1;
    if (TM.scale > 4) TM.scale = 4;

    TM.scale_small = (TM.scale > 1) ? TM.scale - 1 : 1;

    /* Минимальная тач-цель ~ 1/11 ширины экрана. На 480 это 43 px, что при
       типичной для такого разрешения плотности даёт искомые ~7 мм. */
    TM.touch  = screen_w / 11;
    if (TM.touch < 24) TM.touch = 24;

    TM.pad    = TM.touch / 5;
    TM.gap    = TM.touch / 4;
    TM.radius = TM.touch / 8;
    if (TM.radius < 2) TM.radius = 2;

    TM.status_h = TM.touch * 2 / 3;
    TM.nav_h    = TM.touch + TM.pad;

    /* На ландшафтном экране три огромные иконки в ряд смотрятся нелепо и
       занимают всю высоту — там колонок больше. Портрет остаётся каноничным
       телефонным: три в ряд. */
    TM.grid_cols = (screen_w > screen_h) ? 5 : 3;
    /* Иконка: ширина минус внешние отступы и просветы, поделённая на колонки. */
    TM.icon = (screen_w - TM.pad * 2 - TM.gap * (TM.grid_cols - 1)) / TM.grid_cols;
}

void touch_theme_content_rect(int *x, int *y, int *w, int *h) {
    *x = 0;
    *y = TM.status_h;
    *w = TM.screen_w;
    *h = TM.screen_h - TM.status_h - TM.nav_h;
}

void touch_draw_wallpaper(void) {
    static const uint8_t grad[3] = { GFX_UI_GRAD_TOP, GFX_UI_GRAD_MID, GFX_UI_GRAD_BOT };
    hal_gfx_dither_gradient_multi(0, 0, TM.screen_w, TM.screen_h, grad, 3);
}

void touch_draw_card(int x, int y, int w, int h, uint8_t color) {
    hal_gfx_drop_shadow(x, y, w, h, TM.radius, TM.radius > 3 ? 3 : TM.radius);
    hal_gfx_fill_rounded_rect(x, y, w, h, color, TM.radius);
}

void touch_draw_button(int x, int y, int w, int h, const char *label,
                        int pressed, uint8_t top_color, uint8_t bottom_color) {
    /* Нажатие переворачивает градиент — кнопка визуально "вдавливается". */
    uint8_t top = pressed ? bottom_color : top_color;
    uint8_t bot = pressed ? top_color    : bottom_color;

    hal_gfx_draw_glossy_button(x, y, w, h, top, bot, GFX_UI_DIVIDER, TM.radius);

    if (label) {
        int ty = y + (h - FONT_H * TM.scale_small) / 2;
        hal_gfx_draw_string_centered(x, ty, w, label, GFX_UI_TEXT, TM.scale_small);
    }
}

void touch_draw_app_icon(int x, int y, int size, const char *glyph,
                          const char *label, uint8_t color, uint8_t dark, int pressed) {
    int r = size / 4;   /* заметно круглее карточек — так иконка читается
                           как иконка, а не как кусок интерфейса */

    if (!pressed)
        hal_gfx_drop_shadow(x, y, size, size, r, 3);

    /* Нажатая иконка слегка "вдавливается": градиент переворачивается. */
    uint8_t top = pressed ? dark  : color;
    uint8_t bot = pressed ? color : dark;
    hal_gfx_draw_glossy_button(x, y, size, size, top, bot, GFX_UI_DIVIDER, r);

    if (glyph) {
        int gs = TM.scale + 1;
        int gy = y + (size - FONT_H * gs) / 2;
        hal_gfx_draw_string_centered(x, gy, size, glyph, GFX_UI_TEXT, gs);
    }

    if (label) {
        int ly = y + size + TM.pad / 2;
        hal_gfx_draw_string_centered(x, ly, size, label, GFX_UI_TEXT, TM.scale_small);
    }
}

void touch_draw_segment_bar(int x, int y, int w, int h,
                             const uint8_t *colors, const int *weights,
                             int count, int total_weight, uint8_t track) {
    if (total_weight <= 0) return;

    int r = h / 2;                     /* полоса-«пилюля» */
    hal_gfx_fill_rounded_rect(x, y, w, h, track, r);

    int drawn = 0;
    for (int i = 0; i < count; i++) {
        int seg_w = (int)(((long)w * weights[i]) / total_weight);

        for (int j = 0; j < h; j++) {
            for (int k = 0; k < seg_w; k++) {
                int px = drawn + k;
                if (px >= w) break;
                /* Рисуем строго внутри скруглённой дорожки, иначе цветные
                   углы торчали бы за её пределы. */
                if (!hal_gfx_in_rounded_rect(px, j, w, h, r)) continue;
                hal_gfx_put_pixel(x + px, y + j, colors[i]);
            }
        }
        drawn += seg_w;
        if (drawn >= w) break;
    }
}

void touch_header_back_rect(int x, int y, int *bx, int *by, int *bw, int *bh) {
    *bx = x;
    *by = y;
    *bw = TM.touch;
    *bh = TM.touch;
}

int touch_draw_screen_header(int x, int y, int w, const char *title) {
    int h = TM.touch;

    /* Стрелка "назад" — нарисована примитивами: она узнаётся мгновенно,
       а буква "<" в моноширинном шрифте выглядит как знак меньше. */
    int cx = x + h / 2;
    int cy = y + h / 2;
    int a  = h / 5;
    for (int i = 0; i <= a; i++) {
        hal_gfx_fill_rect(cx - a / 2 + i, cy - i, 2, 2, GFX_UI_TEXT);
        hal_gfx_fill_rect(cx - a / 2 + i, cy + i, 2, 2, GFX_UI_TEXT);
    }
    hal_gfx_fill_rect(cx - a / 2, cy, a * 2, 2, GFX_UI_TEXT);

    hal_gfx_draw_string_scaled(x + h + TM.pad, y + (h - FONT_H * TM.scale) / 2,
                                title, GFX_UI_TEXT_BRIGHT, TM.scale);
    (void)w;
    return h;
}

void touch_draw_switch(int x, int y, int w, int h, int on) {
    uint8_t track = on ? GFX_UI_ACCENT_DARK : GFX_UI_DIVIDER;
    hal_gfx_fill_rounded_rect(x, y, w, h, track, h / 2);

    int knob = h - 4;
    int kx = on ? (x + w - knob - 2) : (x + 2);
    hal_gfx_draw_glossy_button(kx, y + 2, knob, knob,
                                on ? GFX_UI_ACCENT : GFX_UI_SURFACE_2,
                                on ? GFX_UI_ACCENT_DARK : GFX_UI_SURFACE,
                                GFX_UI_DIVIDER, knob / 3);
}

int touch_hit(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

int touch_hit_padded(int px, int py, int rx, int ry, int rw, int rh) {
    /* Расширяем зону до минимальной тач-цели симметрично во все стороны:
       мелкая кнопка визуально остаётся мелкой, но промахнуться по ней
       становится трудно. */
    int gw = (rw < TM.touch) ? (TM.touch - rw) / 2 : 0;
    int gh = (rh < TM.touch) ? (TM.touch - rh) / 2 : 0;
    return touch_hit(px, py, rx - gw, ry - gh, rw + gw * 2, rh + gh * 2);
}

char *touch_itoa(int value, char *buf, int min_digits) {
    char tmp[12];
    int n = 0;
    int negative = (value < 0);
    unsigned int v = negative ? (unsigned int)(-value) : (unsigned int)value;

    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n < min_digits && n < 11) tmp[n++] = '0';
    if (negative && n < 11) tmp[n++] = '-';

    int i = 0;
    while (n > 0) buf[i++] = tmp[--n];
    buf[i] = '\0';
    return buf;
}

int touch_strcat(char *dst, int pos, int max, const char *src) {
    while (*src && pos < max - 1) dst[pos++] = *src++;
    dst[pos] = '\0';
    return pos;
}
