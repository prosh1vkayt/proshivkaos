/* gui/touch/app_settings.c — настройки в тач-формате.
 *
 * Десктопная версия (gui/settings_app.c) — это текстовое меню, где пункты
 * выбираются клавишами 1/2/3/A/B/C. На телефоне такой интерфейс не
 * работает вообще: физических клавиш нет, а вызывать экранную клавиатуру
 * ради выбора пункта меню абсурдно. Поэтому здесь настоящие тач-элементы:
 * ряды-переключатели и кнопки, в которые попадаешь пальцем.
 */
#include "touch_app.h"
#include "touch_theme.h"
#include "hal_input.h"

#define ROW_TEXT_SIZE 0
#define ROW_WALLPAPER 1
#define ROW_DOTS      2
#define ROW_SCREEN    3
#define ROW_COUNT     4

static int g_x, g_y, g_w, g_h;
static int g_row_h;
static int g_pressed_row = -1;
static int g_pressed_slot = -1;

static void settings_init(void) {
    g_pressed_row = -1;
}

static void settings_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
    g_row_h = TM.touch + TM.pad * 2;
}

static void row_rect(int index, int *x, int *y, int *w, int *h) {
    *x = g_x + TM.pad;
    *y = g_y + TM.pad + index * (g_row_h + TM.gap);
    *w = g_w - TM.pad * 2;
    *h = g_row_h;
}

/* Прямоугольник управляющего элемента внутри ряда. slot — номер кнопки
 * справа налево, count — сколько их всего в ряду. */
static void control_rect(int row, int slot, int count, int *x, int *y, int *w, int *h) {
    int rx, ry, rw, rh;
    row_rect(row, &rx, &ry, &rw, &rh);

    int cw = TM.touch;
    int gap = TM.gap / 2;
    int total = count * cw + (count - 1) * gap;

    *x = rx + rw - TM.pad - total + slot * (cw + gap);
    *y = ry + (rh - cw) / 2;
    *w = cw;
    *h = cw;
}

static void draw_row(int index, const char *title) {
    int x, y, w, h;
    row_rect(index, &x, &y, &w, &h);

    touch_draw_card(x, y, w, h, GFX_UI_SURFACE);
    hal_gfx_draw_string_scaled(x + TM.pad * 2, y + (h - FONT_H * TM.scale_small) / 2,
                                title, GFX_UI_TEXT, TM.scale_small);
}

static void settings_render(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);

    /* --- Размер текста --- */
    draw_row(ROW_TEXT_SIZE, "TEXT SIZE");
    static const char *size_labels[3] = { "S", "M", "L" };
    for (int i = 0; i < 3; i++) {
        int x, y, w, h;
        control_rect(ROW_TEXT_SIZE, i, 3, &x, &y, &w, &h);
        int active = (touch_ui_text_scale() == i + 1);
        int pressed = (g_pressed_row == ROW_TEXT_SIZE && g_pressed_slot == i);
        touch_draw_button(x, y, w, h, size_labels[i], pressed,
                           active ? GFX_UI_ACCENT      : GFX_UI_SURFACE_2,
                           active ? GFX_UI_ACCENT_DARK : GFX_UI_SURFACE);
    }

    /* --- Обои --- */
    draw_row(ROW_WALLPAPER, "WALLPAPER");
    static const char *wp_labels[2] = { "GRAD", "FLAT" };
    for (int i = 0; i < 2; i++) {
        int x, y, w, h;
        control_rect(ROW_WALLPAPER, i, 2, &x, &y, &w, &h);
        int active = (touch_ui_wallpaper_mode() == i);
        int pressed = (g_pressed_row == ROW_WALLPAPER && g_pressed_slot == i);
        touch_draw_button(x, y, w, h, wp_labels[i], pressed,
                           active ? GFX_UI_ACCENT      : GFX_UI_SURFACE_2,
                           active ? GFX_UI_ACCENT_DARK : GFX_UI_SURFACE);
    }

    /* --- Отметки касаний --- */
    draw_row(ROW_DOTS, "TOUCH MARKS");
    {
        int x, y, w, h;
        control_rect(ROW_DOTS, 0, 1, &x, &y, &w, &h);
        /* Переключатель шире квадратной тач-цели — растим его ВЛЕВО от
           правого края зоны, иначе он вылезал за границу экрана. */
        int sw = TM.touch * 3 / 2;
        touch_draw_switch(x + w - sw, y + h / 4, sw, h / 2, touch_ui_touch_dots());
    }

    /* --- Разрешение экрана (только информация) --- */
    draw_row(ROW_SCREEN, "SCREEN");
    {
        char buf[32], num[12];
        int pos = 0;
        buf[0] = '\0';
        pos = touch_strcat(buf, pos, sizeof(buf), touch_itoa(TM.screen_w, num, 0));
        pos = touch_strcat(buf, pos, sizeof(buf), "X");
        pos = touch_strcat(buf, pos, sizeof(buf), touch_itoa(TM.screen_h, num, 0));

        int rx, ry, rw, rh;
        row_rect(ROW_SCREEN, &rx, &ry, &rw, &rh);
        int tw = hal_gfx_string_width(buf, TM.scale_small);
        hal_gfx_draw_string_scaled(rx + rw - TM.pad * 2 - tw,
                                    ry + (rh - FONT_H * TM.scale_small) / 2,
                                    buf, GFX_UI_TEXT_DIM, TM.scale_small);
    }
}

/* Какая кнопка под пальцем. Возвращает 1 и заполняет row/slot. */
static int find_control(int px, int py, int *row, int *slot) {
    static const int counts[ROW_COUNT] = { 3, 2, 1, 0 };

    for (int r = 0; r < ROW_COUNT; r++) {
        for (int s = 0; s < counts[r]; s++) {
            int x, y, w, h;
            control_rect(r, s, counts[r], &x, &y, &w, &h);
            if (touch_hit_padded(px, py, x, y, w, h)) {
                *row = r; *slot = s;
                return 1;
            }
        }
    }
    return 0;
}

static void settings_on_touch(int type, int x, int y) {
    int row, slot;

    if (type == HAL_EV_POINTER_DOWN) {
        if (find_control(x, y, &row, &slot)) {
            g_pressed_row = row;
            g_pressed_slot = slot;
        }
        return;
    }

    if (type != HAL_EV_POINTER_UP) return;

    int was_row = g_pressed_row, was_slot = g_pressed_slot;
    g_pressed_row = g_pressed_slot = -1;
    if (was_row < 0) return;

    /* Срабатывание — только если палец отпущен на том же элементе. */
    if (!find_control(x, y, &row, &slot)) return;
    if (row != was_row || slot != was_slot) return;

    switch (row) {
        case ROW_TEXT_SIZE: touch_ui_set_text_scale(slot + 1);   break;
        case ROW_WALLPAPER: touch_ui_set_wallpaper_mode(slot);    break;
        case ROW_DOTS:      touch_ui_set_touch_dots(!touch_ui_touch_dots()); break;
        default: break;
    }
}

static void settings_on_key(int key) { (void)key; }
static int  settings_wants_keyboard(void) { return 0; }

const touch_app_t app_settings = {
    .name  = "SETTINGS",
    .glyph = "*",
    .color = GFX_UI_WARN,
    .color2 = GFX_BROWN,
    .init  = settings_init,
    .layout = settings_layout,
    .render = settings_render,
    .on_touch = settings_on_touch,
    .on_key = settings_on_key,
    .wants_keyboard = settings_wants_keyboard
};
