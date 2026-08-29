/* gui/touch/app_terminal.c — терминал в тач-оболочке.
 *
 * Самого терминала тут почти нет: разбор команд, RamFS, cd/ls/cat/edit —
 * всё это gui/terminal_app.c, тот же самый файл, который работает в
 * десктопной сборке на x86. Здесь только адаптация под тач: вывод идёт не
 * в окно с заголовком, а в выделенный оболочкой прямоугольник, и шрифт
 * крупнее.
 *
 * Это ровно то, ради чего в проекте заведён HAL: логика приложения
 * написана один раз и не знает ни архитектуры, ни формы экрана.
 */
#include "touch_app.h"
#include "touch_theme.h"
#include "terminal_app.h"
#include "hal_input.h"

static terminal_app_t g_term;
static int g_started = 0;
static int g_area_x, g_area_y, g_area_w, g_area_h;

static void terminal_init(void) {
    if (g_started) return;

    /* win = 0: окна нет, область вывода задаётся отдельно через
       gconsole_set_rect() в terminal_layout(). */
    terminal_app_init_colors(&g_term, 0, GFX_UI_TEXT, GFX_UI_BG);
    g_started = 1;
}

static void terminal_layout(int x, int y, int w, int h) {
    g_area_x = x; g_area_y = y; g_area_w = w; g_area_h = h;

    int pad = TM.pad;
    gconsole_set_rect(&g_term.console, x + pad, y + pad, w - pad * 2, h - pad * 2);
    gconsole_set_scale(&g_term.console, touch_ui_text_scale());
}

static void terminal_render(void) {
    /* Сначала заливаем ВСЮ отведённую область, а не только сетку символов:
       по краям остаётся отступ шириной TM.pad, и без заливки сквозь него
       просвечивал бы предыдущий кадр (обои домашнего экрана). */
    hal_gfx_fill_rect(g_area_x, g_area_y, g_area_w, g_area_h, GFX_UI_BG);
    gconsole_render(&g_term.console);
}

static void terminal_on_key(int key) {
    terminal_app_handle_char(&g_term, (char)key);
}

static void terminal_on_touch(int type, int x, int y) {
    /* Терминал сам по себе не интерактивен пальцем — вся работа идёт с
       клавиатуры. Касание по тексту оставлено под будущее выделение. */
    (void)type; (void)x; (void)y;
}

static int terminal_wants_keyboard(void) { return 1; }

const touch_app_t app_terminal = {
    .name  = "TERMINAL",
    .glyph = ">_",
    .color = GFX_UI_ACCENT,
    .color2 = GFX_UI_ACCENT_DARK,
    .init  = terminal_init,
    .layout = terminal_layout,
    .render = terminal_render,
    .on_touch = terminal_on_touch,
    .on_key = terminal_on_key,
    .wants_keyboard = terminal_wants_keyboard
};
