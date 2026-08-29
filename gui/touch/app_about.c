/* gui/touch/app_about.c — экран "О системе" в духе ColorOS.
 *
 * Ровно тот же экран, что открывается на телефоне в Настройки -> Об
 * устройстве: карточка-герой с объёмной «планетой» и названием системы,
 * под ней две плитки поменьше и список характеристик.
 *
 * Здесь используется почти всё, что появилось в графическом слое ради
 * этого экрана:
 *   - шкала из 24 оттенков (GFX_HERO_RAMP) — на восьми цветах на сфере
 *     такого размера были бы видны концентрические кольца вместо объёма;
 *   - hal_gfx_fill_sphere() со смещённым источником света;
 *   - отсечение (hal_gfx_set_clip) — «планета» намеренно вылезает за край
 *     карточки и обрезается по её скруглению, как в оригинале;
 *   - сегментная полоса занятости памяти.
 *
 * Содержимое длиннее экрана, поэтому список прокручивается перетаскиванием.
 * Заголовок при этом остаётся на месте.
 */
#include "touch_app.h"
#include "touch_theme.h"
#include "hal.h"
#include "hal_time.h"
#include "hal_input.h"

static int g_x, g_y, g_w, g_h;
static int g_scroll = 0;
static int g_content_h = 0;

/* Жест: отличаем прокрутку от нажатия на стрелку "назад". */
static int g_drag_start_y = 0;
static int g_drag_start_scroll = 0;
static int g_drag_distance = 0;

static void about_init(void) {
    g_scroll = 0;
}

static void about_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
}

/* ---------------- размеры блоков ---------------- */

static int header_h(void)  { return TM.touch; }
static int hero_h(void)    { return TM.touch * 6; }
static int mini_h(void)    { return TM.touch * 3; }
static int row_h(void)     { return TM.touch * 5 / 4; }
#define ROW_COUNT 7

static int rows_card_h(void) { return TM.pad * 2 + ROW_COUNT * row_h(); }

static int scroll_area_y(void) { return g_y + header_h(); }
static int scroll_area_h(void) { return g_h - header_h(); }

/* ---------------- вспомогательные строки ---------------- */

/* "480X960" */
static void screen_text(char *buf, int max) {
    char num[12];
    int pos = touch_strcat(buf, 0, max, touch_itoa(TM.screen_w, num, 0));
    pos = touch_strcat(buf, pos, max, "X");
    touch_strcat(buf, pos, max, touch_itoa(TM.screen_h, num, 0));
}

/* "12K / 4096K" */
static void memory_text(char *buf, int max) {
    char num[12];
    int pos = touch_strcat(buf, 0, max, touch_itoa((int)(hal_mem_used() / 1024), num, 0));
    pos = touch_strcat(buf, pos, max, "K / ");
    pos = touch_strcat(buf, pos, max, touch_itoa((int)(hal_mem_total() / 1024), num, 0));
    touch_strcat(buf, pos, max, "K");
}

/* "00:01:23" */
static void uptime_text(char *buf, int max) {
    char num[12];
    unsigned long total = (unsigned long)(hal_time_ms() / 1000);

    int pos = touch_strcat(buf, 0, max, touch_itoa((int)(total / 3600), num, 2));
    pos = touch_strcat(buf, pos, max, ":");
    pos = touch_strcat(buf, pos, max, touch_itoa((int)((total % 3600) / 60), num, 2));
    pos = touch_strcat(buf, pos, max, ":");
    touch_strcat(buf, pos, max, touch_itoa((int)(total % 60), num, 2));
}

/* "PROSHIVKA ARM64" — то, что на телефоне было бы моделью аппарата. */
static void device_text(char *buf, int max) {
    int pos = touch_strcat(buf, 0, max, "PROSHIVKA ");
    touch_strcat(buf, pos, max, hal_arch_name());
}

/* ---------------- карточка-герой ---------------- */

static void draw_hero(int x, int y, int w, int h) {
    int r = TM.radius * 3;

    /* Фон карточки: фиолетовый градиент сверху вниз. Рисуем прямоугольником,
       скругление наведём в конце — так проще, чем учить градиент форме. */
    static const uint8_t bg[3] = { GFX_HERO_BG_TOP, GFX_HERO_BG_MID, GFX_HERO_BG_BOT };
    hal_gfx_dither_gradient_multi(x, y, w, h, bg, 3);

    /* «Планета». Радиус подобран так, чтобы верхние углы карточки остались
       фоном, а низ сферы ушёл за нижний край: тело читается как огромное, а
       не как кружок посередине. Источник света вынесен в верхний левый угол
       самой сферы — именно смещение света, а не концентрические кольца из
       центра, создаёт объём. */
    int pr  = (h * 3) / 4;
    int pcx = x + w / 2 - w / 16;
    int pcy = y + h / 2 + h / 4;
    hal_gfx_fill_sphere(pcx, pcy, pr,
                         pcx - pr / 2, pcy - pr * 2 / 3,
                         GFX_HERO_RAMP, GFX_HERO_RAMP_COUNT);

    /* Текст поверх. Порядок сверху вниз повторяет оригинал: модель мелко,
       название системы крупно, версия, состояние обновления. */
    char buf[40];
    device_text(buf, sizeof(buf));
    hal_gfx_draw_string_centered(x, y + h / 6, w, buf, GFX_UI_TEXT_BRIGHT, TM.scale_small);

    hal_gfx_draw_string_centered(x, y + h / 6 + FONT_H * TM.scale_small + TM.pad * 2, w,
                                  "PROSHIVKAOS", GFX_UI_TEXT_BRIGHT, TM.scale + 1);

    hal_gfx_draw_string_centered(x, y + h / 6 + FONT_H * TM.scale_small + TM.pad * 2
                                        + FONT_H * (TM.scale + 1) + TM.pad,
                                  w, PROSHIVKAOS_VERSION, GFX_UI_TEXT_BRIGHT, TM.scale);

    hal_gfx_draw_string_centered(x, y + h - TM.touch, w,
                                  "VERSION UP TO DATE", GFX_UI_TEXT_BRIGHT, TM.scale_small);

    /* Скругление углов: возвращаем фон приложения там, где карточки быть
       не должно. Дешевле, чем отсекать каждый примитив по форме. */
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (!hal_gfx_in_rounded_rect(i, j, w, h, r))
                hal_gfx_put_pixel(x + i, y + j, GFX_UI_BG);
}

/* ---------------- две плитки под героем ---------------- */

static void draw_mini_cards(int x, int y, int w, int h) {
    int cw = (w - TM.gap) / 2;
    char buf[40];

    /* --- Левая: имя устройства --- */
    touch_draw_card(x, y, cw, h, GFX_UI_SURFACE);

    /* Значок телефона — рамка со скруглением и «динамиком» сверху. */
    int ix = x + TM.pad * 2, iy = y + TM.pad * 2;
    int iw = TM.touch / 2, ih = TM.touch * 3 / 4;
    hal_gfx_draw_rounded_rect(ix, iy, iw, ih, GFX_UI_ACCENT2, 3);
    hal_gfx_fill_rect(ix + iw / 3, iy + 3, iw / 3, 2, GFX_UI_ACCENT2);

    hal_gfx_draw_string_scaled(x + TM.pad * 2, y + h / 2 - FONT_H * TM.scale_small,
                                "DEVICE NAME", GFX_UI_TEXT_DIM, TM.scale_small);
    device_text(buf, sizeof(buf));
    hal_gfx_draw_string_scaled(x + TM.pad * 2, y + h / 2 + TM.pad,
                                buf, GFX_UI_TEXT, TM.scale_small);

    /* --- Правая: память с сегментной полосой --- */
    int rx = x + cw + TM.gap;
    touch_draw_card(rx, y, cw, h, GFX_UI_SURFACE);

    /* Доли условные: своего учёта «что именно занимает память» у ядра нет,
       но показать структуру занятого полезнее, чем один серый прямоугольник. */
    static const uint8_t seg_colors[4] = {
        GFX_SEG_BLUE, GFX_SEG_PURPLE, GFX_SEG_ORANGE, GFX_SEG_TEAL
    };
    int used_kb  = (int)(hal_mem_used() / 1024);
    int total_kb = (int)(hal_mem_total() / 1024);
    if (total_kb <= 0) total_kb = 1;

    /* Ядро и его статические буферы занимают память вне кучи — показываем
       их отдельной долей, иначе полоса всегда была бы почти пустой. */
    int kernel_kb = 1024;
    int seg_weights[4] = { kernel_kb, used_kb, used_kb / 2, 0 };

    touch_draw_segment_bar(rx + TM.pad * 2, y + TM.pad * 2,
                            cw - TM.pad * 4, TM.touch / 4,
                            seg_colors, seg_weights, 4,
                            total_kb, GFX_UI_SURFACE_3);

    hal_gfx_draw_string_scaled(rx + TM.pad * 2, y + h / 2 - FONT_H * TM.scale_small,
                                "MEMORY", GFX_UI_TEXT_DIM, TM.scale_small);
    memory_text(buf, sizeof(buf));
    hal_gfx_draw_string_scaled(rx + TM.pad * 2, y + h / 2 + TM.pad,
                                buf, GFX_UI_TEXT, TM.scale_small);
}

/* ---------------- список характеристик ---------------- */

static void draw_row(int x, int y, int w, int index, const char *key, const char *value) {
    int rh = row_h();

    hal_gfx_draw_string_scaled(x + TM.pad * 2, y + (rh - FONT_H * TM.scale_small) / 2,
                                key, GFX_UI_TEXT, TM.scale_small);

    int vw = hal_gfx_string_width(value, TM.scale_small);
    hal_gfx_draw_string_scaled(x + w - TM.pad * 2 - vw, y + (rh - FONT_H * TM.scale_small) / 2,
                                value, GFX_UI_TEXT_DIM, TM.scale_small);

    /* Разделитель между строками, но не после последней. */
    if (index < ROW_COUNT - 1)
        hal_gfx_fill_rect(x + TM.pad * 2, y + rh - 1, w - TM.pad * 4, 1, GFX_UI_DIVIDER_SOFT);
}

static void draw_rows_card(int x, int y, int w, int h) {
    touch_draw_card(x, y, w, h, GFX_UI_SURFACE);

    char screen[24], mem[32], up[16];
    screen_text(screen, sizeof(screen));
    memory_text(mem, sizeof(mem));
    uptime_text(up, sizeof(up));

    int ry = y + TM.pad;
    int i = 0;
    draw_row(x, ry, w, i++, "PROCESSOR",  hal_cpu_name());          ry += row_h();
    draw_row(x, ry, w, i++, "ARCH",        hal_arch_name());         ry += row_h();
    draw_row(x, ry, w, i++, "DISPLAY",     screen);                  ry += row_h();
    draw_row(x, ry, w, i++, "INPUT",       hal_input_has_cursor() ? "POINTER" : "TOUCHSCREEN");
    ry += row_h();
    draw_row(x, ry, w, i++, "MEMORY",      mem);                     ry += row_h();
    draw_row(x, ry, w, i++, "FILESYSTEM",  "RAMFS");                 ry += row_h();
    draw_row(x, ry, w, i++, "BUILD",       __DATE__);
}

/* ---------------- отрисовка ---------------- */

static void about_render(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);
    touch_draw_screen_header(g_x, g_y, g_w, "ABOUT DEVICE");

    int area_y = scroll_area_y();
    int area_h = scroll_area_h();

    /* Всё, что ниже, может не поместиться — обрезаем по области прокрутки,
       иначе длинный список залезал бы на заголовок и на панель навигации. */
    hal_gfx_set_clip(g_x, area_y, g_w, area_h);

    int cw = g_w - TM.pad * 2;
    int cx = g_x + TM.pad;
    int y  = area_y + TM.pad - g_scroll;

    draw_hero(cx, y, cw, hero_h());
    y += hero_h() + TM.gap;

    draw_mini_cards(cx, y, cw, mini_h());
    y += mini_h() + TM.gap;

    draw_rows_card(cx, y, cw, rows_card_h());
    y += rows_card_h() + TM.pad;

    hal_gfx_reset_clip();

    /* Полная высота содержимого — нужна, чтобы не дать прокрутить в пустоту. */
    g_content_h = TM.pad + hero_h() + TM.gap + mini_h() + TM.gap + rows_card_h() + TM.pad;
}

/* ---------------- ввод ---------------- */

static void clamp_scroll(void) {
    int max = g_content_h - scroll_area_h();
    if (max < 0) max = 0;
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0)   g_scroll = 0;
}

static void about_on_touch(int type, int x, int y) {
    int bx, by, bw, bh;
    touch_header_back_rect(g_x, g_y, &bx, &by, &bw, &bh);

    if (type == HAL_EV_POINTER_DOWN) {
        g_drag_start_y = y;
        g_drag_start_scroll = g_scroll;
        g_drag_distance = 0;
        return;
    }

    if (type == HAL_EV_POINTER_MOVE) {
        int dy = y - g_drag_start_y;
        if (dy < 0) dy = -dy;
        if (dy > g_drag_distance) g_drag_distance = dy;

        if (g_drag_distance > TM.touch / 3) {
            g_scroll = g_drag_start_scroll - (y - g_drag_start_y);
            clamp_scroll();
        }
        return;
    }

    if (type != HAL_EV_POINTER_UP) return;

    /* Тянули — значит прокручивали, нажатие не засчитываем. */
    if (g_drag_distance > TM.touch / 3) return;

    if (touch_hit_padded(x, y, bx, by, bw, bh))
        touch_ui_go_home();
}

static void about_on_key(int key) { (void)key; }
static int  about_wants_keyboard(void) { return 0; }

const touch_app_t app_about = {
    .name  = "ABOUT",
    .glyph = "?",
    .color = GFX_UI_ACCENT3,
    .color2 = GFX_UI_ACCENT3_DARK,
    .init  = about_init,
    .layout = about_layout,
    .render = about_render,
    .on_touch = about_on_touch,
    .on_key = about_on_key,
    .wants_keyboard = about_wants_keyboard
};
