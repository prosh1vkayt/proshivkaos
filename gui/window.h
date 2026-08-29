/* gui/window.h — окно в стиле Windows XP: синяя градиентная title bar,
 * тело в свете/тёмной теме, ресайз за правый нижний угол, панель задач
 * с кнопкой "Пуск" и всплывающим меню запуска приложений.
 */
#ifndef PROSHIVKAOS_WINDOW_H
#define PROSHIVKAOS_WINDOW_H

#include "hal_gfx.h"

#define TITLE_BAR_H  10
#define BORDER       2
#define TASKBAR_H    14
#define RESIZE_GRIP  6
#define MIN_WIN_W    60
#define MIN_WIN_H    40

typedef struct {
    int x, y, w, h;
    const char *title;
    int focused;
    int open;                /* 0 = окно скрыто (не открыто через Пуск) */
    int minimized;           /* 1 = свёрнуто (не рисуется, но есть в панели задач) */
    int maximized;           /* 1 = развёрнуто на весь экран */
    int saved_x, saved_y, saved_w, saved_h;   /* геометрия до разворота — для восстановления */
} gui_window_t;

void gui_draw_desktop(uint8_t bg_color);
void gui_draw_window_chrome(const gui_window_t *win, int dark_theme);
void gui_draw_taskbar(int screen_w, int screen_h);

/* Область содержимого окна (внутри рамки и под title bar) —
 * приложение рисует/пишет текст именно сюда. */
void gui_window_content_rect(const gui_window_t *win, int *cx, int *cy, int *cw, int *ch);

/* Хит-тесты для мыши */
int gui_point_in_rect(int px, int py, int rx, int ry, int rw, int rh);
int gui_point_in_titlebar(int px, int py, const gui_window_t *win);
int gui_point_in_close_button(int px, int py, const gui_window_t *win);
int gui_point_in_maximize_button(int px, int py, const gui_window_t *win);
int gui_point_in_minimize_button(int px, int py, const gui_window_t *win);
int gui_point_in_resize_grip(int px, int py, const gui_window_t *win);

/* Кнопка "Пуск" в панели задач */
void gui_start_button_rect(int screen_w, int screen_h, int *x, int *y, int *w, int *h);

/* Кнопки открытых окон в панели задач (справа от "Пуск") */
#define TASKBAR_BUTTON_W 60
void gui_taskbar_button_rect(int screen_w, int screen_h, int index, int *x, int *y, int *w, int *h);
void gui_draw_taskbar_button(int screen_w, int screen_h, int index, const char *title,
                              int active, int dark_theme);

/* Всплывающее меню "Пуск": 2 пункта, TERMINAL и SETTINGS */
#define START_MENU_ITEMS 2
void gui_draw_start_menu(int screen_h, int dark_theme);
void gui_start_menu_item_rect(int screen_h, int index, int *x, int *y, int *w, int *h);

#endif
