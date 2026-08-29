/* gui/touch/app_about.c — "О системе".
 *
 * Тач-версия команды neofetch из терминала. Полезна не только как
 * украшение: это первое, что открываешь на реальном устройстве, чтобы
 * убедиться, что ядро определило разрешение экрана и тип ввода именно
 * так, как ожидалось.
 */
#include "touch_app.h"
#include "touch_theme.h"
#include "hal.h"
#include "hal_time.h"
#include "hal_input.h"

static int g_x, g_y, g_w, g_h;

static void about_init(void) { }

static void about_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
}

/* Строка вида "КЛЮЧ            значение" внутри карточки. */
static void draw_field(int y, const char *key, const char *value) {
    hal_gfx_draw_string_scaled(g_x + TM.pad * 3, y, key, GFX_UI_TEXT_DIM, TM.scale_small);

    int vw = hal_gfx_string_width(value, TM.scale_small);
    hal_gfx_draw_string_scaled(g_x + g_w - TM.pad * 3 - vw, y, value,
                                GFX_UI_TEXT, TM.scale_small);
}

static void about_render(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);

    char buf[40], num[12];
    int pos;

    /* --- Шапка с логотипом --- */
    int logo_h = TM.touch * 3;
    touch_draw_card(g_x + TM.pad, g_y + TM.pad, g_w - TM.pad * 2, logo_h, GFX_UI_SURFACE);

    int badge = TM.touch * 3 / 2;
    int bx = g_x + (g_w - badge) / 2;
    int by = g_y + TM.pad + TM.pad;
    hal_gfx_draw_glossy_button(bx, by, badge, badge, GFX_UI_ACCENT, GFX_UI_ACCENT_DARK,
                                GFX_UI_DIVIDER, badge / 4);
    hal_gfx_draw_string_centered(bx, by + (badge - FONT_H * (TM.scale + 1)) / 2, badge,
                                  "P", GFX_UI_TEXT, TM.scale + 1);

    hal_gfx_draw_string_centered(g_x, by + badge + TM.pad, g_w,
                                  "PROSHIVKAOS NEXT", GFX_UI_TEXT, TM.scale_small);

    /* --- Поля --- */
    int y = g_y + TM.pad * 2 + logo_h + TM.pad;
    int step = TM.touch * 3 / 4;
    int fields_h = step * 6 + TM.pad * 2;
    touch_draw_card(g_x + TM.pad, y - TM.pad, g_w - TM.pad * 2, fields_h, GFX_UI_SURFACE);

    draw_field(y, "ARCH", hal_arch_name());
    y += step;

    pos = touch_strcat(buf, 0, sizeof(buf), touch_itoa(TM.screen_w, num, 0));
    pos = touch_strcat(buf, pos, sizeof(buf), "X");
    touch_strcat(buf, pos, sizeof(buf), touch_itoa(TM.screen_h, num, 0));
    draw_field(y, "SCREEN", buf);
    y += step;

    draw_field(y, "INPUT", hal_input_has_cursor() ? "POINTER" : "TOUCHSCREEN");
    y += step;

    draw_field(y, "FS", "RAMFS");
    y += step;

    /* Память: занято из общего, в килобайтах */
    pos = touch_strcat(buf, 0, sizeof(buf), touch_itoa((int)(hal_mem_used() / 1024), num, 0));
    pos = touch_strcat(buf, pos, sizeof(buf), "K / ");
    pos = touch_strcat(buf, pos, sizeof(buf), touch_itoa((int)(hal_mem_total() / 1024), num, 0));
    touch_strcat(buf, pos, sizeof(buf), "K");
    draw_field(y, "HEAP", buf);
    y += step;

    /* Аптайм ЧЧ:ММ:СС */
    {
        unsigned long total = (unsigned long)(hal_time_ms() / 1000);
        pos = touch_strcat(buf, 0, sizeof(buf), touch_itoa((int)(total / 3600), num, 2));
        pos = touch_strcat(buf, pos, sizeof(buf), ":");
        pos = touch_strcat(buf, pos, sizeof(buf), touch_itoa((int)((total % 3600) / 60), num, 2));
        pos = touch_strcat(buf, pos, sizeof(buf), ":");
        touch_strcat(buf, pos, sizeof(buf), touch_itoa((int)(total % 60), num, 2));
        draw_field(y, "UPTIME", buf);
    }
}

static void about_on_touch(int type, int x, int y) { (void)type; (void)x; (void)y; }
static void about_on_key(int key) { (void)key; }
static int  about_wants_keyboard(void) { return 0; }

const touch_app_t app_about = {
    .name  = "ABOUT",
    .glyph = "?",
    .color = GFX_UI_SURFACE_2,
    .color2 = GFX_UI_SURFACE,
    .init  = about_init,
    .layout = about_layout,
    .render = about_render,
    .on_touch = about_on_touch,
    .on_key = about_on_key,
    .wants_keyboard = about_wants_keyboard
};
