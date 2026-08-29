/* gui/wm.c — оконный менеджер proshivkaOS NEXT (XP-стиль).
 * Рабочий стол при старте чист — окна открываются через меню "Пуск".
 * Мышь/тачпад: клик — фокус/меню, тяга за title bar — перенос окна,
 * тяга за правый нижний угол — ресайз. Tab по-прежнему переключает
 * фокус клавиатурой между уже открытыми окнами.
 */
#include "hal_gfx.h"
#include "window.h"
#include "wallpaper.h"
#include "cursor.h"
#include "terminal_app.h"
#include "settings_app.h"
#include "mouse.h"
#include "rtc.h"

int keyboard_poll(void);          /* arch/x86/keyboard.c */

#define THEME_LIGHT 0
#define THEME_DARK  1

#define FOCUS_NONE     0
#define FOCUS_TERMINAL 1
#define FOCUS_SETTINGS 2

static gui_window_t term_win;
static gui_window_t set_win;
static terminal_app_t term;
static settings_app_t settings;

static uint8_t desktop_bg = GFX_CYAN;
static int desktop_wallpaper = 1;
static int theme = THEME_LIGHT;
static int screen_w = 320, screen_h = 200;
static int mouse_x, mouse_y;
static int start_menu_open = 0;
static int focus = FOCUS_NONE;

static void apply_theme(void) {
    if (theme == THEME_DARK) {
        gconsole_set_colors(&term.console, GFX_WHITE, GFX_DARK_GREY);
        gconsole_set_colors(&settings.console, GFX_WHITE, GFX_DARK_GREY);
    } else {
        gconsole_set_colors(&term.console, GFX_BLACK, GFX_WHITE);
        gconsole_set_colors(&settings.console, GFX_BLACK, GFX_LIGHT_GREY);
    }
}

static void toggle_maximize(gui_window_t *win) {
    if (win->maximized) {
        win->x = win->saved_x; win->y = win->saved_y;
        win->w = win->saved_w; win->h = win->saved_h;
        win->maximized = 0;
    } else {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
        win->x = 0; win->y = 0;
        win->w = screen_w; win->h = screen_h - TASKBAR_H;
        win->maximized = 1;
    }
}

/* Расставляет окна заново под текущее разрешение экрана — вызывается
 * при старте и после каждой смены разрешения. */
static void layout_windows(void) {
    if (term_win.x + term_win.w > screen_w) term_win.x = screen_w - term_win.w;
    if (set_win.x + set_win.w > screen_w)   set_win.x  = screen_w - set_win.w;
    if (term_win.x < 0) term_win.x = 0;
    if (set_win.x < 0)  set_win.x  = 0;
    if (term_win.y + term_win.h > screen_h - TASKBAR_H)
        term_win.y = screen_h - TASKBAR_H - term_win.h;
    if (set_win.y + set_win.h > screen_h - TASKBAR_H)
        set_win.y = screen_h - TASKBAR_H - set_win.h;
    if (term_win.y < 0) term_win.y = 0;
    if (set_win.y < 0)  set_win.y  = 0;
}

static void render_frame(void) {
    if (desktop_wallpaper)
        gui_draw_wallpaper(screen_w, screen_h);
    else
        gui_draw_desktop(desktop_bg);

    if (term_win.open && !term_win.minimized) {
        gui_draw_window_chrome(&term_win, theme == THEME_DARK);
        gconsole_render(&term.console);
    }
    if (set_win.open && !set_win.minimized) {
        gui_draw_window_chrome(&set_win, theme == THEME_DARK);
        gconsole_render(&settings.console);
    }

    gui_draw_taskbar(screen_w, screen_h);

    int tb_index = 0;
    if (term_win.open) {
        gui_draw_taskbar_button(screen_w, screen_h, tb_index++, "TERMINAL",
                                 focus == FOCUS_TERMINAL && !term_win.minimized,
                                 theme == THEME_DARK);
    }
    if (set_win.open) {
        gui_draw_taskbar_button(screen_w, screen_h, tb_index++, "SETTINGS",
                                 focus == FOCUS_SETTINGS && !set_win.minimized,
                                 theme == THEME_DARK);
    }

    if (start_menu_open)
        gui_draw_start_menu(screen_h, theme == THEME_DARK);

    gui_draw_cursor(mouse_x, mouse_y);

    hal_gfx_present();
}

static void apply_resolution(int w, int h) {
    if (!hal_gfx_set_resolution(w, h))
        return;

    screen_w = hal_gfx_width();
    screen_h = hal_gfx_height();
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;
    layout_windows();

    if (term_win.maximized) { term_win.x = 0; term_win.y = 0; term_win.w = screen_w; term_win.h = screen_h - TASKBAR_H; }
    if (set_win.maximized)  { set_win.x  = 0; set_win.y  = 0; set_win.w  = screen_w; set_win.h  = screen_h - TASKBAR_H; }

    render_frame();
}

void gui_main(void) {
    hal_gfx_init();
    mouse_init();

    screen_w = hal_gfx_width();
    screen_h = hal_gfx_height();
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;

    term_win.x = 8;   term_win.y = 8;  term_win.w = 150; term_win.h = 150;
    set_win.x  = 165; set_win.y = 8;   set_win.w  = 140; set_win.h  = 150;
    term_win.title = "TERMINAL";
    set_win.title  = "SETTINGS";
    term_win.focused = 0;
    set_win.focused  = 0;
    term_win.open = 0;    /* рабочий стол при старте чист */
    set_win.open  = 0;
    term_win.minimized = 0; set_win.minimized = 0;
    term_win.maximized = 0; set_win.maximized = 0;

    layout_windows();

    terminal_app_init(&term, &term_win);
    settings_app_init(&settings, &set_win, &desktop_bg);
    apply_theme();

    render_frame();

    int dragging = 0, resizing = 0;
    gui_window_t *drag_target = 0;
    gui_window_t *resize_target = 0;

    for (;;) {
        rtc_uptime_tick();

        int handled_something = 0;

        int c = keyboard_poll();
        if (c != -1) {
            handled_something = 1;

            if (c == '\t') {
                if (focus == FOCUS_TERMINAL && set_win.open) focus = FOCUS_SETTINGS;
                else if (focus == FOCUS_SETTINGS && term_win.open) focus = FOCUS_TERMINAL;
                else if (term_win.open) focus = FOCUS_TERMINAL;
                else if (set_win.open) focus = FOCUS_SETTINGS;
                term_win.focused = (focus == FOCUS_TERMINAL);
                set_win.focused  = (focus == FOCUS_SETTINGS);
            } else if (focus == FOCUS_TERMINAL) {
                terminal_app_handle_char(&term, (char)c);
            } else if (focus == FOCUS_SETTINGS) {
                int r = settings_app_handle_char(&settings, (char)c);
                if (r == SETTINGS_RES_320x200) apply_resolution(320, 200);
                else if (r == SETTINGS_RES_640x480) apply_resolution(640, 480);
                else if (r == SETTINGS_RES_800x600) apply_resolution(800, 600);
                else if (r == SETTINGS_WALLPAPER) desktop_wallpaper = 1;
                else if (r == SETTINGS_COLOR_CHANGED) desktop_wallpaper = 0;
                else if (r == SETTINGS_THEME_LIGHT) { theme = THEME_LIGHT; apply_theme(); }
                else if (r == SETTINGS_THEME_DARK)  { theme = THEME_DARK;  apply_theme(); }
            }
        }

        mouse_event_t mev;
        if (mouse_poll(&mev)) {
            handled_something = 1;

            mouse_x += mev.dx;
            mouse_y += mev.dy;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= screen_w) mouse_x = screen_w - 1;
            if (mouse_y >= screen_h) mouse_y = screen_h - 1;

            if (mev.left) {
                if (dragging) {
                    drag_target->x += mev.dx;
                    drag_target->y += mev.dy;
                } else if (resizing) {
                    resize_target->w += mev.dx;
                    resize_target->h += mev.dy;
                    if (resize_target->w < MIN_WIN_W) resize_target->w = MIN_WIN_W;
                    if (resize_target->h < MIN_WIN_H) resize_target->h = MIN_WIN_H;
                } else if (start_menu_open) {
                    int ix, iy, iw, ih;
                    gui_start_menu_item_rect(screen_h, 0, &ix, &iy, &iw, &ih);
                    if (gui_point_in_rect(mouse_x, mouse_y, ix, iy, iw, ih)) {
                        term_win.open = 1;
                        focus = FOCUS_TERMINAL;
                        term_win.focused = 1; set_win.focused = 0;
                    } else {
                        gui_start_menu_item_rect(screen_h, 1, &ix, &iy, &iw, &ih);
                        if (gui_point_in_rect(mouse_x, mouse_y, ix, iy, iw, ih)) {
                            set_win.open = 1;
                            focus = FOCUS_SETTINGS;
                            term_win.focused = 0; set_win.focused = 1;
                        }
                    }
                    start_menu_open = 0;
                } else {
                    int sx, sy, sw, sh;
                    gui_start_button_rect(screen_w, screen_h, &sx, &sy, &sw, &sh);

                    int term_tb_index = -1, set_tb_index = -1;
                    int idx = 0;
                    if (term_win.open) term_tb_index = idx++;
                    if (set_win.open)  set_tb_index  = idx++;

                    int term_tb_x = 0, term_tb_y = 0, term_tb_w = 0, term_tb_h = 0;
                    int set_tb_x = 0, set_tb_y = 0, set_tb_w = 0, set_tb_h = 0;
                    if (term_tb_index >= 0)
                        gui_taskbar_button_rect(screen_w, screen_h, term_tb_index,
                                                 &term_tb_x, &term_tb_y, &term_tb_w, &term_tb_h);
                    if (set_tb_index >= 0)
                        gui_taskbar_button_rect(screen_w, screen_h, set_tb_index,
                                                 &set_tb_x, &set_tb_y, &set_tb_w, &set_tb_h);

                    if (gui_point_in_rect(mouse_x, mouse_y, sx, sy, sw, sh)) {
                        start_menu_open = 1;

                    } else if (term_tb_index >= 0 &&
                               gui_point_in_rect(mouse_x, mouse_y, term_tb_x, term_tb_y, term_tb_w, term_tb_h)) {
                        if (term_win.minimized) {
                            term_win.minimized = 0;
                            focus = FOCUS_TERMINAL;
                            term_win.focused = 1; set_win.focused = 0;
                        } else if (focus == FOCUS_TERMINAL) {
                            term_win.minimized = 1;
                        } else {
                            focus = FOCUS_TERMINAL;
                            term_win.focused = 1; set_win.focused = 0;
                        }

                    } else if (set_tb_index >= 0 &&
                               gui_point_in_rect(mouse_x, mouse_y, set_tb_x, set_tb_y, set_tb_w, set_tb_h)) {
                        if (set_win.minimized) {
                            set_win.minimized = 0;
                            focus = FOCUS_SETTINGS;
                            set_win.focused = 1; term_win.focused = 0;
                        } else if (focus == FOCUS_SETTINGS) {
                            set_win.minimized = 1;
                        } else {
                            focus = FOCUS_SETTINGS;
                            set_win.focused = 1; term_win.focused = 0;
                        }

                    } else if (term_win.open && !term_win.minimized && gui_point_in_close_button(mouse_x, mouse_y, &term_win)) {
                        term_win.open = 0;
                        if (focus == FOCUS_TERMINAL) focus = set_win.open ? FOCUS_SETTINGS : FOCUS_NONE;
                        term_win.focused = 0; set_win.focused = (focus == FOCUS_SETTINGS);
                    } else if (set_win.open && !set_win.minimized && gui_point_in_close_button(mouse_x, mouse_y, &set_win)) {
                        set_win.open = 0;
                        if (focus == FOCUS_SETTINGS) focus = term_win.open ? FOCUS_TERMINAL : FOCUS_NONE;
                        set_win.focused = 0; term_win.focused = (focus == FOCUS_TERMINAL);

                    } else if (term_win.open && !term_win.minimized && gui_point_in_maximize_button(mouse_x, mouse_y, &term_win)) {
                        toggle_maximize(&term_win);
                        focus = FOCUS_TERMINAL; term_win.focused = 1; set_win.focused = 0;
                    } else if (set_win.open && !set_win.minimized && gui_point_in_maximize_button(mouse_x, mouse_y, &set_win)) {
                        toggle_maximize(&set_win);
                        focus = FOCUS_SETTINGS; set_win.focused = 1; term_win.focused = 0;

                    } else if (term_win.open && !term_win.minimized && gui_point_in_minimize_button(mouse_x, mouse_y, &term_win)) {
                        term_win.minimized = 1;
                    } else if (set_win.open && !set_win.minimized && gui_point_in_minimize_button(mouse_x, mouse_y, &set_win)) {
                        set_win.minimized = 1;

                    } else if (term_win.open && !term_win.minimized && !term_win.maximized &&
                               gui_point_in_resize_grip(mouse_x, mouse_y, &term_win)) {
                        resizing = 1; resize_target = &term_win;
                        focus = FOCUS_TERMINAL; term_win.focused = 1; set_win.focused = 0;
                    } else if (set_win.open && !set_win.minimized && !set_win.maximized &&
                               gui_point_in_resize_grip(mouse_x, mouse_y, &set_win)) {
                        resizing = 1; resize_target = &set_win;
                        focus = FOCUS_SETTINGS; set_win.focused = 1; term_win.focused = 0;

                    } else if (term_win.open && !term_win.minimized && !term_win.maximized &&
                               gui_point_in_titlebar(mouse_x, mouse_y, &term_win)) {
                        dragging = 1; drag_target = &term_win;
                        focus = FOCUS_TERMINAL; term_win.focused = 1; set_win.focused = 0;
                    } else if (set_win.open && !set_win.minimized && !set_win.maximized &&
                               gui_point_in_titlebar(mouse_x, mouse_y, &set_win)) {
                        dragging = 1; drag_target = &set_win;
                        focus = FOCUS_SETTINGS; set_win.focused = 1; term_win.focused = 0;

                    } else if (term_win.open && !term_win.minimized &&
                               gui_point_in_rect(mouse_x, mouse_y, term_win.x, term_win.y, term_win.w, term_win.h)) {
                        focus = FOCUS_TERMINAL; term_win.focused = 1; set_win.focused = 0;
                    } else if (set_win.open && !set_win.minimized &&
                               gui_point_in_rect(mouse_x, mouse_y, set_win.x, set_win.y, set_win.w, set_win.h)) {
                        focus = FOCUS_SETTINGS; set_win.focused = 1; term_win.focused = 0;
                    }
                }
            } else {
                dragging = 0;
                resizing = 0;
                drag_target = 0;
                resize_target = 0;
            }
        }

        if (handled_something)
            render_frame();
    }
}
