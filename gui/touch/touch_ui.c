/* gui/touch/touch_ui.c — оболочка тач-сборки: домашний экран, статусбар,
 * панель навигации, переключатель приложений, распознавание жестов.
 *
 * Чем это принципиально отличается от gui/wm.c (десктопного оконного
 * менеджера) и почему нельзя было просто "уменьшить окна":
 *
 * 1. Нет перекрывающихся окон. На экране шириной 480 точек две форточки
 *    с заголовками — это две нечитаемые полоски. Приложение занимает
 *    экран целиком, переключение — через список запущенных.
 * 2. Нет курсора и нет наведения. Палец либо касается, либо нет; состояния
 *    "мышь висит над кнопкой" не существует, поэтому подсветка привязана
 *    к нажатию, а не к позиции.
 * 3. Палец закрывает то, во что тыкает. Отсюда минимальный размер цели
 *    (см. gui/touch/touch_theme.c) и срабатывание по ОТПУСКАНИЮ, а не по
 *    нажатию: так можно отвести палец и передумать.
 * 4. Жесты вместо клавиш. Свайп снизу вверх — домой; это единственный
 *    способ выйти из приложения, когда физических кнопок нет.
 *
 * Идея та же, что у postmarketOS с оболочкой Phosh: ядро и приложения
 * остаются обычными, "телефонность" целиком живёт в слое оболочки.
 */
#include "touch_ui.h"
#include "touch_app.h"
#include "touch_theme.h"
#include "osk.h"
#include "hal.h"
#include "hal_gfx.h"
#include "hal_input.h"
#include "hal_time.h"

void gui_draw_cursor(int x, int y);   /* gui/cursor.c */

/* ---------------- Состояние оболочки ---------------- */

#define SCREEN_HOME    0
#define SCREEN_APP     1
#define SCREEN_RECENTS 2

static const touch_app_t *const g_apps[] = {
    &app_terminal,
    &app_files,
    &app_settings,
    &app_about
};
#define APP_COUNT ((int)(sizeof(g_apps) / sizeof(g_apps[0])))

static int g_screen  = SCREEN_HOME;
static int g_current = -1;                 /* индекс активного приложения */
static int g_launched[APP_COUNT];          /* уже запускалось (есть в списке) */

static int g_text_scale = 2;
static int g_wallpaper_mode = WALLPAPER_GRADIENT;
static int g_touch_dots = 0;

/* Последнее касание — для необязательной отметки на экране. */
static int g_last_touch_x = -1, g_last_touch_y = -1;
static int g_pointer_down = 0;

/* Жест: откуда начали и когда. */
static int g_gesture_x0, g_gesture_y0;
static uint64_t g_gesture_t0;
static int g_gesture_from_nav;

/* Подсветка нажатого элемента оболочки. */
static int g_pressed_icon = -1;
static int g_pressed_nav  = -1;

static uint64_t g_last_render_ms = 0;

/* ---------------- Настройки (доступ из app_settings) ---------------- */

int  touch_ui_text_scale(void) { return g_text_scale; }
int  touch_ui_wallpaper_mode(void) { return g_wallpaper_mode; }
int  touch_ui_touch_dots(void) { return g_touch_dots; }
void touch_ui_set_wallpaper_mode(int mode) { g_wallpaper_mode = mode; }
void touch_ui_set_touch_dots(int enabled) { g_touch_dots = enabled; }

static void relayout_current_app(void);

void touch_ui_set_text_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    g_text_scale = scale;
    relayout_current_app();   /* терминал пересчитает ширину строки */
}

void touch_ui_go_home(void) {
    g_screen = SCREEN_HOME;
    osk_set_visible(0);
}

/* ---------------- Раскладка ---------------- */

/* Область приложения = экран минус статусбар, навбар и клавиатура. */
static void app_area(int *x, int *y, int *w, int *h) {
    touch_theme_content_rect(x, y, w, h);
    *h -= osk_height();
    if (*h < TM.touch) *h = TM.touch;
}

static void relayout_current_app(void) {
    if (g_current < 0) return;

    int x, y, w, h;
    app_area(&x, &y, &w, &h);
    g_apps[g_current]->layout(x, y, w, h);

    osk_layout(0, y + h, TM.screen_w);
}

static void launch_app(int index) {
    if (index < 0 || index >= APP_COUNT) return;

    g_current = index;
    g_screen  = SCREEN_APP;

    if (!g_launched[index]) {
        g_apps[index]->init();
        g_launched[index] = 1;
    }

    /* Клавиатура появляется сама, если приложению есть куда печатать —
       иначе пользователю пришлось бы искать, чем её вызвать. */
    osk_set_visible(g_apps[index]->wants_keyboard());
    relayout_current_app();
}

/* ---------------- Статусбар ---------------- */

static void draw_status_bar(void) {
    hal_gfx_fill_rect(0, 0, TM.screen_w, TM.status_h, GFX_UI_BG);
    hal_gfx_fill_rect(0, TM.status_h - 1, TM.screen_w, 1, GFX_UI_DIVIDER);

    int ty = (TM.status_h - FONT_H * TM.scale_small) / 2;

    const char *title = (g_screen == SCREEN_APP && g_current >= 0)
                        ? g_apps[g_current]->name
                        : "PROSHIVKAOS";
    hal_gfx_draw_string_scaled(TM.pad, ty, title, GFX_UI_TEXT, TM.scale_small);

    /* Часы справа */
    int year, month, day, hour, min, sec;
    hal_time_rtc(&year, &month, &day, &hour, &min, &sec);

    char buf[16], num[12];
    int pos = touch_strcat(buf, 0, sizeof(buf), touch_itoa(hour, num, 2));
    pos = touch_strcat(buf, pos, sizeof(buf), ":");
    touch_strcat(buf, pos, sizeof(buf), touch_itoa(min, num, 2));

    int tw = hal_gfx_string_width(buf, TM.scale_small);
    hal_gfx_draw_string_scaled(TM.screen_w - TM.pad - tw, ty, buf,
                                GFX_UI_TEXT, TM.scale_small);

    /* Индикатор батареи. Настоящего датчика заряда на QEMU нет, а на
       телефоне он читается через PMIC по I2C — это отдельный драйвер.
       Пока рисуем корпус без заливки, чтобы место под него было
       предусмотрено в раскладке. */
    int bw = TM.status_h / 2 + 6, bh = TM.status_h / 2 - 2;
    int bx = TM.screen_w - TM.pad - tw - TM.pad - bw;
    int by = (TM.status_h - bh) / 2;
    hal_gfx_draw_rect(bx, by, bw, bh, GFX_UI_TEXT_DIM);
    hal_gfx_fill_rect(bx + bw, by + bh / 3, 2, bh / 3, GFX_UI_TEXT_DIM);
}

/* ---------------- Панель навигации ---------------- */

#define NAV_BACK    0
#define NAV_HOME    1
#define NAV_RECENTS 2
#define NAV_COUNT   3

static void nav_button_rect(int index, int *x, int *y, int *w, int *h) {
    int slot = TM.screen_w / NAV_COUNT;
    *w = TM.touch;
    *h = TM.touch;
    *x = index * slot + (slot - *w) / 2;
    *y = TM.screen_h - TM.nav_h + (TM.nav_h - *h) / 2;
}

/* Значки нарисованы примитивами, а не буквами: стрелка и круг узнаются
 * мгновенно, а слова BACK/HOME на трёх языках читать некогда. */
static void draw_nav_glyph(int index, int x, int y, int size, uint8_t color) {
    int cx = x + size / 2;
    int cy = y + size / 2;
    int r  = size / 4;

    if (index == NAV_BACK) {
        /* треугольник влево */
        for (int i = 0; i < r; i++)
            hal_gfx_fill_rect(cx - r / 2 + i, cy - i, 2, i * 2 + 1, color);
    } else if (index == NAV_HOME) {
        /* кольцо */
        for (int j = -r; j <= r; j++)
            for (int i = -r; i <= r; i++) {
                int d = i * i + j * j;
                if (d <= r * r && d >= (r - 2) * (r - 2))
                    hal_gfx_put_pixel(cx + i, cy + j, color);
            }
    } else {
        /* квадрат-контур */
        hal_gfx_draw_rect(cx - r, cy - r, r * 2, r * 2, color);
        hal_gfx_draw_rect(cx - r + 1, cy - r + 1, r * 2 - 2, r * 2 - 2, color);
    }
}

static void draw_nav_bar(void) {
    int y = TM.screen_h - TM.nav_h;
    hal_gfx_fill_rect(0, y, TM.screen_w, TM.nav_h, GFX_UI_BG);
    hal_gfx_fill_rect(0, y, TM.screen_w, 1, GFX_UI_DIVIDER);

    for (int i = 0; i < NAV_COUNT; i++) {
        int bx, by, bw, bh;
        nav_button_rect(i, &bx, &by, &bw, &bh);

        if (g_pressed_nav == i)
            hal_gfx_fill_rounded_rect(bx, by, bw, bh, GFX_UI_SURFACE, TM.radius);

        draw_nav_glyph(i, bx, by, bw,
                        (g_pressed_nav == i) ? GFX_UI_ACCENT : GFX_UI_TEXT_DIM);
    }
}

/* ---------------- Домашний экран ---------------- */

static void icon_rect(int index, int *x, int *y, int *w, int *h) {
    int col = index % TM.grid_cols;
    int row = index / TM.grid_cols;

    int ax, ay, aw, ah;
    touch_theme_content_rect(&ax, &ay, &aw, &ah);

    /* Сетка начинается под блоком часов */
    int grid_top = ay + TM.touch * 4;

    *w = TM.icon;
    *h = TM.icon;
    *x = ax + TM.pad + col * (TM.icon + TM.gap);
    *y = grid_top + row * (TM.icon + TM.gap + FONT_H * TM.scale_small + TM.pad);
}

static void draw_home(void) {
    int ax, ay, aw, ah;
    touch_theme_content_rect(&ax, &ay, &aw, &ah);

    /* Крупные часы — то, ради чего чаще всего и включают экран. */
    int year, month, day, hour, min, sec;
    hal_time_rtc(&year, &month, &day, &hour, &min, &sec);

    char buf[24], num[12];
    int pos = touch_strcat(buf, 0, sizeof(buf), touch_itoa(hour, num, 2));
    pos = touch_strcat(buf, pos, sizeof(buf), ":");
    touch_strcat(buf, pos, sizeof(buf), touch_itoa(min, num, 2));

    int clock_scale = TM.scale + 2;
    hal_gfx_draw_string_centered(0, ay + TM.touch, TM.screen_w, buf,
                                  GFX_UI_TEXT, clock_scale);

    pos = touch_strcat(buf, 0, sizeof(buf), touch_itoa(day, num, 2));
    pos = touch_strcat(buf, pos, sizeof(buf), ".");
    pos = touch_strcat(buf, pos, sizeof(buf), touch_itoa(month, num, 2));
    pos = touch_strcat(buf, pos, sizeof(buf), ".");
    touch_strcat(buf, pos, sizeof(buf), touch_itoa(year, num, 4));

    hal_gfx_draw_string_centered(0, ay + TM.touch + FONT_H * clock_scale + TM.pad,
                                  TM.screen_w, buf, GFX_UI_TEXT_DIM, TM.scale_small);

    for (int i = 0; i < APP_COUNT; i++) {
        int x, y, w, h;
        icon_rect(i, &x, &y, &w, &h);

        /* Иконка, не помещающаяся в область под сеткой, не рисуется вовсе:
           обрезанная наполовину панелью навигации она выглядит как сбой
           отрисовки, а не как "прокрутите ниже". */
        if (y + h + FONT_H * TM.scale_small + TM.pad > ay + ah) continue;

        touch_draw_app_icon(x, y, w, g_apps[i]->glyph, g_apps[i]->name,
                             g_apps[i]->color, g_apps[i]->color2,
                             g_pressed_icon == i);
    }
}

/* ---------------- Список запущенных приложений ---------------- */

static int recents_card_rect(int slot, int *x, int *y, int *w, int *h) {
    int ax, ay, aw, ah;
    touch_theme_content_rect(&ax, &ay, &aw, &ah);

    int card_h = TM.touch * 2;
    *x = ax + TM.pad * 2;
    *y = ay + TM.pad * 2 + slot * (card_h + TM.gap);
    *w = aw - TM.pad * 4;
    *h = card_h;
    return 1;
}

static void draw_recents(void) {
    int ax, ay, aw, ah;
    touch_theme_content_rect(&ax, &ay, &aw, &ah);

    int slot = 0;
    for (int i = 0; i < APP_COUNT; i++) {
        if (!g_launched[i]) continue;

        int x, y, w, h;
        recents_card_rect(slot, &x, &y, &w, &h);
        touch_draw_card(x, y, w, h, GFX_UI_SURFACE);

        int icon = h - TM.pad * 2;
        hal_gfx_draw_glossy_button(x + TM.pad, y + TM.pad, icon, icon,
                                    g_apps[i]->color, g_apps[i]->color2,
                                    GFX_UI_DIVIDER, icon / 4);
        hal_gfx_draw_string_centered(x + TM.pad, y + TM.pad + (icon - FONT_H * TM.scale) / 2,
                                      icon, g_apps[i]->glyph, GFX_UI_TEXT, TM.scale);

        hal_gfx_draw_string_scaled(x + TM.pad * 2 + icon,
                                    y + (h - FONT_H * TM.scale_small) / 2,
                                    g_apps[i]->name, GFX_UI_TEXT, TM.scale_small);
        slot++;
    }

    if (slot == 0)
        hal_gfx_draw_string_centered(0, ay + ah / 2, TM.screen_w,
                                      "NO RUNNING APPS", GFX_UI_TEXT_DIM, TM.scale_small);
}

/* ---------------- Отрисовка кадра ---------------- */

static void render_frame(void) {
    if (g_screen == SCREEN_APP && g_current >= 0) {
        /* Приложение рисует свой фон само — обои под ним не нужны и
           только съедали бы время на заливку целого экрана. */
        g_apps[g_current]->render();
    } else {
        if (g_wallpaper_mode == WALLPAPER_GRADIENT)
            touch_draw_wallpaper();
        else
            hal_gfx_clear(GFX_UI_BG);

        if (g_screen == SCREEN_RECENTS) draw_recents();
        else                            draw_home();
    }

    osk_render();
    draw_status_bar();
    draw_nav_bar();

    /* Отметка касания — визуальная проверка, что тачскрин попадает туда,
       куда пользователь целится. На реальном устройстве это первое, чем
       проверяют калибровку сенсора. */
    if (g_touch_dots && g_pointer_down && g_last_touch_x >= 0) {
        int r = TM.touch / 4;
        for (int j = -r; j <= r; j++)
            for (int i = -r; i <= r; i++)
                if (i * i + j * j <= r * r)
                    hal_gfx_put_pixel(g_last_touch_x + i, g_last_touch_y + j, GFX_UI_ACCENT);
    }

    /* Курсор рисуем только там, где есть настоящий указатель (x86-сборка
       с мышью). На тачскрине его не бывает. */
    if (hal_input_has_cursor()) {
        int cx, cy;
        hal_input_pointer_pos(&cx, &cy);
        gui_draw_cursor(cx, cy);
    }

    hal_gfx_present();
}

/* ---------------- Обработка ввода ---------------- */

static int nav_button_at(int x, int y) {
    for (int i = 0; i < NAV_COUNT; i++) {
        int bx, by, bw, bh;
        nav_button_rect(i, &bx, &by, &bw, &bh);
        if (touch_hit_padded(x, y, bx, by, bw, bh)) return i;
    }
    return -1;
}

static int icon_at(int x, int y) {
    int ax, ay, aw, ah;
    touch_theme_content_rect(&ax, &ay, &aw, &ah);

    for (int i = 0; i < APP_COUNT; i++) {
        int ix, iy, iw, ih;
        icon_rect(i, &ix, &iy, &iw, &ih);
        if (iy + ih + FONT_H * TM.scale_small + TM.pad > ay + ah) continue;
        /* Подпись под иконкой тоже должна нажиматься — целиться в неё
           естественно, и промах туда воспринимается как поломка. */
        if (touch_hit(x, y, ix, iy, iw, ih + FONT_H * TM.scale_small + TM.pad))
            return i;
    }
    return -1;
}

static int recents_at(int x, int y) {
    int slot = 0;
    for (int i = 0; i < APP_COUNT; i++) {
        if (!g_launched[i]) continue;
        int cx, cy, cw, ch;
        recents_card_rect(slot, &cx, &cy, &cw, &ch);
        if (touch_hit(x, y, cx, cy, cw, ch)) return i;
        slot++;
    }
    return -1;
}

static void handle_nav_action(int button) {
    if (button == NAV_HOME) {
        touch_ui_go_home();
    } else if (button == NAV_RECENTS) {
        g_screen = SCREEN_RECENTS;
        osk_set_visible(0);
    } else if (button == NAV_BACK) {
        /* "Назад" из приложения — на домашний экран; с домашнего —
           никуда (выходить некуда, это корень интерфейса). */
        if (g_screen != SCREEN_HOME) touch_ui_go_home();
    }
}

static void handle_pointer(int type, int x, int y) {
    g_last_touch_x = x;
    g_last_touch_y = y;

    int in_nav = (y >= TM.screen_h - TM.nav_h);

    if (type == HAL_EV_POINTER_DOWN) {
        g_pointer_down = 1;
        g_gesture_x0 = x;
        g_gesture_y0 = y;
        g_gesture_t0 = hal_time_ms();
        g_gesture_from_nav = in_nav;

        if (in_nav) {
            g_pressed_nav = nav_button_at(x, y);
            return;
        }
        if (osk_contains(x, y)) { osk_handle_pointer(type, x, y); return; }

        if (g_screen == SCREEN_HOME)      g_pressed_icon = icon_at(x, y);
        else if (g_screen == SCREEN_APP && g_current >= 0)
            g_apps[g_current]->on_touch(type, x, y);
        return;
    }

    if (type == HAL_EV_POINTER_MOVE) {
        if (g_gesture_from_nav) return;   /* тянем из навбара — это жест */
        if (osk_visible() && osk_contains(x, y)) { osk_handle_pointer(type, x, y); return; }
        if (g_screen == SCREEN_APP && g_current >= 0)
            g_apps[g_current]->on_touch(type, x, y);
        return;
    }

    if (type != HAL_EV_POINTER_UP) return;

    g_pointer_down = 0;
    int pressed_nav  = g_pressed_nav;
    int pressed_icon = g_pressed_icon;
    g_pressed_nav = g_pressed_icon = -1;

    /* --- Жест "свайп снизу вверх" = домой --- */
    /* Начат у нижнего края и увёл палец вверх больше чем на полторы
       тач-цели — этого достаточно, чтобы не спутать со случайным
       дрожанием пальца на кнопке навбара. */
    if (g_gesture_from_nav && (g_gesture_y0 - y) > TM.touch * 3 / 2) {
        touch_ui_go_home();
        return;
    }

    if (g_gesture_from_nav) {
        /* Не жест, а обычное нажатие кнопки: срабатывает, только если
           отпустили на той же кнопке. */
        if (pressed_nav >= 0 && nav_button_at(x, y) == pressed_nav)
            handle_nav_action(pressed_nav);
        return;
    }

    if (osk_visible() && osk_contains(x, y)) {
        int key = osk_handle_pointer(type, x, y);
        if (key && g_screen == SCREEN_APP && g_current >= 0)
            g_apps[g_current]->on_key(key);
        /* Клавиатуру могли спрятать клавишей "убрать" — область
           приложения из-за этого выросла. */
        relayout_current_app();
        return;
    }
    /* Палец ушёл с клавиатуры мимо — снимаем подсветку клавиши. */
    if (osk_visible()) osk_handle_pointer(type, x, y);

    if (g_screen == SCREEN_HOME) {
        if (pressed_icon >= 0 && icon_at(x, y) == pressed_icon)
            launch_app(pressed_icon);
        return;
    }

    if (g_screen == SCREEN_RECENTS) {
        int app = recents_at(x, y);
        if (app >= 0) launch_app(app);
        return;
    }

    if (g_screen == SCREEN_APP && g_current >= 0)
        g_apps[g_current]->on_touch(type, x, y);
}

static void handle_key(int key) {
    /* Клавиши с физической клавиатуры (или из последовательного порта на
       ARM64) идут активному приложению — так же, как символы с экранной. */
    if (g_screen == SCREEN_APP && g_current >= 0)
        g_apps[g_current]->on_key(key);
}

/* ---------------- Точка входа ---------------- */

void touch_main(void) {
    hal_gfx_init();

    /* Тач-интерфейс рассчитан на ПОРТРЕТНЫЙ экран, а hal_gfx_init() на x86
       поднимает классический mode13h 320x200 — ландшафтный и крошечный.
       Просим нужное разрешение явно; если платформа его не даёт (нет VBE),
       остаёмся на том, что есть, и раскладка подстроится сама. */
    hal_gfx_set_resolution(PROSHIVKA_SCREEN_W, PROSHIVKA_SCREEN_H);

    int w = hal_gfx_width();
    int h = hal_gfx_height();
    if (w <= 0 || h <= 0)
        hal_panic("no display: nuzhen -device ramfb");

    touch_theme_init(w, h);
    osk_init();

    hal_input_init();
    hal_input_set_screen(w, h);

    for (int i = 0; i < APP_COUNT; i++) g_launched[i] = 0;

    /* Терминал запускаем сразу в фоне: у него в конструкторе создаётся
       домашний каталог /root, и файловому менеджеру приятнее открываться
       не в пустой корень. */
    g_apps[0]->init();
    g_launched[0] = 1;

    g_screen = SCREEN_HOME;
    render_frame();

    for (;;) {
        int dirty = 0;

        hal_input_event_t ev;
        while (hal_input_poll(&ev)) {
            if (ev.type == HAL_EV_KEY) handle_key(ev.key);
            else                        handle_pointer(ev.type, ev.x, ev.y);
            dirty = 1;
        }

        /* Раз в секунду — ради часов в статусбаре. Полная перерисовка
           экрана по таймеру, а не по событию: покадровой анимации у нас
           нет, а раз в секунду это заметно дешевле, чем гнать кадры
           постоянно. */
        uint64_t now = hal_time_ms();
        if (now - g_last_render_ms >= 1000) dirty = 1;

        if (dirty) {
            g_last_render_ms = now;
            render_frame();
        }
    }
}
