/* gui/window.c — оформление в духе Windows XP + скевоморфизм (iOS 6):
 * скруглённые углы, дизеринг-градиенты вместо жёстких границ цвета,
 * глянцевые кнопки с бликом. Тема (светлая/тёмная) по-прежнему меняет
 * только цвет консолей приложений, не хром окна.
 */
#include "window.h"

#define WIN_RADIUS  3
#define BTN_RADIUS  2

/* Прямоугольная обводка со срезанными (как у скруглённого прямоугольника)
 * углами — чтобы контур совпадал с уже скруглённой заливкой под ним. */
static void draw_rounded_outline(int x, int y, int w, int h, uint8_t color, int radius) {
    for (int i = radius; i < w - radius; i++) {
        hal_gfx_put_pixel(x + i, y, color);
        hal_gfx_put_pixel(x + i, y + h - 1, color);
    }
    for (int j = radius; j < h - radius; j++) {
        hal_gfx_put_pixel(x, y + j, color);
        hal_gfx_put_pixel(x + w - 1, y + j, color);
    }
}

void gui_draw_desktop(uint8_t bg_color) {
    hal_gfx_clear(bg_color);
}

void gui_draw_window_chrome(const gui_window_t *win, int dark_theme) {
    uint8_t body_color = dark_theme ? GFX_DARK_GREY : GFX_XP_BODY;

    /* тень окна — тоже скруглена, иначе торчащие квадратные углы тени
       выдадут "накладку" под скруглённым телом */
    hal_gfx_fill_rounded_rect(win->x + 3, win->y + 3, win->w, win->h, GFX_BLACK, WIN_RADIUS);

    /* тело окна — скруглённое, с лёгким "рельефным" бликом по верхней/
       левой грани и тенью по нижней/правой — классический скевоморфный
       приподнятый прямоугольник */
    hal_gfx_fill_rounded_rect(win->x, win->y, win->w, win->h, body_color, WIN_RADIUS);
    draw_rounded_outline(win->x, win->y, win->w, win->h, GFX_XP_BORDER, WIN_RADIUS);

    for (int i = WIN_RADIUS; i < win->w - WIN_RADIUS; i++)
        hal_gfx_put_pixel(win->x + i, win->y + 1, dark_theme ? GFX_LIGHT_GREY : GFX_WHITE);
    for (int j = WIN_RADIUS; j < win->h - WIN_RADIUS; j++)
        hal_gfx_put_pixel(win->x + 1, win->y + j, dark_theme ? GFX_LIGHT_GREY : GFX_WHITE);

    /* title bar: дизеринг-градиент вместо жёсткой границы двух цветов —
       заметно "не пиксели-пиксели" на глаз, даже на палитре в 32 цвета */
    int tb_x = win->x + BORDER;
    int tb_y = win->y + BORDER;
    int tb_w = win->w - BORDER * 2;

    if (win->focused)
        hal_gfx_dither_gradient_v(tb_x, tb_y, tb_w, TITLE_BAR_H, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK);
    else
        hal_gfx_dither_gradient_v(tb_x, tb_y, tb_w, TITLE_BAR_H, GFX_LIGHT_GREY, GFX_DARK_GREY);

    hal_gfx_draw_string_transparent(tb_x + 3, tb_y + 1, win->title, GFX_WHITE);

    /* кнопки справа налево: закрыть (красная), развернуть, свернуть —
       теперь глянцевые, со скруглением и бликом */
    int close_x = win->x + win->w - BORDER - 9;
    int max_x   = close_x - 9;
    int min_x   = max_x - 9;
    int by = tb_y + 1;

    hal_gfx_draw_glossy_button(close_x, by, 8, 8, GFX_LIGHT_RED, GFX_XP_CLOSE_RED, GFX_BLACK, BTN_RADIUS);
    hal_gfx_draw_char(close_x, by, 'X', GFX_WHITE, GFX_XP_CLOSE_RED);

    hal_gfx_draw_glossy_button(max_x, by, 8, 8, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK, GFX_XP_BORDER, BTN_RADIUS);
    hal_gfx_draw_rect(max_x + 2, by + 2, 4, 4, GFX_WHITE);

    hal_gfx_draw_glossy_button(min_x, by, 8, 8, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK, GFX_XP_BORDER, BTN_RADIUS);
    hal_gfx_fill_rect(min_x + 2, by + 5, 4, 1, GFX_WHITE);

    /* уголок для ресайза — несколько диагональных точек в углу */
    int gx = win->x + win->w - RESIZE_GRIP;
    int gy = win->y + win->h - RESIZE_GRIP;
    for (int i = 0; i < 3; i++) {
        hal_gfx_put_pixel(gx + RESIZE_GRIP - 1 - i, gy + RESIZE_GRIP - 1, GFX_XP_BORDER);
        hal_gfx_put_pixel(gx + RESIZE_GRIP - 1, gy + RESIZE_GRIP - 1 - i, GFX_XP_BORDER);
    }
}

void gui_window_content_rect(const gui_window_t *win, int *cx, int *cy, int *cw, int *ch) {
    *cx = win->x + BORDER;
    *cy = win->y + BORDER + TITLE_BAR_H + 1;
    *cw = win->w - BORDER * 2;
    *ch = win->h - BORDER * 2 - TITLE_BAR_H - 1;
}

int gui_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

int gui_point_in_titlebar(int px, int py, const gui_window_t *win) {
    return gui_point_in_rect(px, py, win->x + BORDER, win->y + BORDER,
                              win->w - BORDER * 2, TITLE_BAR_H);
}

int gui_point_in_close_button(int px, int py, const gui_window_t *win) {
    int bx = win->x + win->w - BORDER - 9;
    int by = win->y + BORDER + 1;
    return gui_point_in_rect(px, py, bx, by, 8, 8);
}

int gui_point_in_maximize_button(int px, int py, const gui_window_t *win) {
    int bx = win->x + win->w - BORDER - 9 - 9;
    int by = win->y + BORDER + 1;
    return gui_point_in_rect(px, py, bx, by, 8, 8);
}

int gui_point_in_minimize_button(int px, int py, const gui_window_t *win) {
    int bx = win->x + win->w - BORDER - 9 - 18;
    int by = win->y + BORDER + 1;
    return gui_point_in_rect(px, py, bx, by, 8, 8);
}

int gui_point_in_resize_grip(int px, int py, const gui_window_t *win) {
    int gx = win->x + win->w - RESIZE_GRIP;
    int gy = win->y + win->h - RESIZE_GRIP;
    return gui_point_in_rect(px, py, gx, gy, RESIZE_GRIP, RESIZE_GRIP);
}

void gui_start_button_rect(int screen_w, int screen_h, int *x, int *y, int *w, int *h) {
    (void)screen_w;
    *x = 2;
    *y = screen_h - TASKBAR_H + 2;
    *w = 46;
    *h = TASKBAR_H - 4;
}

void gui_draw_taskbar(int screen_w, int screen_h) {
    int bar_y = screen_h - TASKBAR_H;

    /* дизеринг-градиент вместо плоской заливки — "металлический" глянец,
       как у настоящей XP-панели задач */
    hal_gfx_dither_gradient_v(0, bar_y, screen_w, TASKBAR_H, GFX_XP_TITLE_LIGHT, GFX_XP_TITLE_DARK);

    int sx, sy, sw, sh;
    gui_start_button_rect(screen_w, screen_h, &sx, &sy, &sw, &sh);
    hal_gfx_draw_glossy_button(sx, sy, sw, sh, GFX_LIGHT_GREEN, GFX_XP_START_GREEN, GFX_BLACK, BTN_RADIUS + 1);
    hal_gfx_draw_string_transparent(sx + 5, sy + (sh - FONT_H) / 2, "START", GFX_WHITE);
}

void gui_taskbar_button_rect(int screen_w, int screen_h, int index, int *x, int *y, int *w, int *h) {
    int sx, sy, sw, sh;
    gui_start_button_rect(screen_w, screen_h, &sx, &sy, &sw, &sh);
    (void)screen_w;

    *x = sx + sw + 6 + index * (TASKBAR_BUTTON_W + 4);
    *y = sy;
    *w = TASKBAR_BUTTON_W;
    *h = sh;
}

void gui_draw_taskbar_button(int screen_w, int screen_h, int index, const char *title,
                              int active, int dark_theme) {
    int x, y, w, h;
    gui_taskbar_button_rect(screen_w, screen_h, index, &x, &y, &w, &h);

    uint8_t top = active ? GFX_XP_TITLE_LIGHT : (dark_theme ? GFX_DARK_GREY : GFX_LIGHT_GREY);
    uint8_t bot = active ? GFX_XP_TITLE_DARK  : (dark_theme ? GFX_BLACK     : GFX_DARK_GREY);

    hal_gfx_draw_glossy_button(x, y, w, h, top, bot, GFX_XP_BORDER, BTN_RADIUS);
    hal_gfx_draw_string_transparent(x + 3, y + (h - FONT_H) / 2, title, GFX_WHITE);
}

static void start_menu_geometry(int screen_h, int *mx, int *my, int *mw, int *mh, int *item_h) {
    *item_h = 12;
    *mx = 2;
    *mw = 90;
    *mh = (*item_h) * START_MENU_ITEMS + 4;
    *my = screen_h - TASKBAR_H - (*mh);
}

void gui_draw_start_menu(int screen_h, int dark_theme) {
    uint8_t body_color = dark_theme ? GFX_DARK_GREY : GFX_XP_BODY;
    uint8_t text_color = dark_theme ? GFX_WHITE : GFX_BLACK;

    int mx, my, mw, mh, item_h;
    start_menu_geometry(screen_h, &mx, &my, &mw, &mh, &item_h);

    hal_gfx_fill_rounded_rect(mx, my, mw, mh, body_color, BTN_RADIUS + 1);
    draw_rounded_outline(mx, my, mw, mh, GFX_XP_BORDER, BTN_RADIUS + 1);

    hal_gfx_draw_string(mx + 3, my + 2, "TERMINAL", text_color, body_color);
    hal_gfx_draw_string(mx + 3, my + 2 + item_h, "SETTINGS", text_color, body_color);
}

void gui_start_menu_item_rect(int screen_h, int index, int *x, int *y, int *w, int *h) {
    int mx, my, mw, mh, item_h;
    start_menu_geometry(screen_h, &mx, &my, &mw, &mh, &item_h);
    (void)mh;

    *x = mx;
    *w = mw;
    *h = item_h;
    *y = my + 2 + index * item_h;
}
