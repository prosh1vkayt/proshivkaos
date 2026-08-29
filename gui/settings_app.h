/* gui/settings_app.h — приложение "Настройки": смена цвета рабочего стола
 * (клавиши 1-4) и разрешения экрана (клавиши A/B/C). Простое статичное
 * меню — задел под более сложные настройки на следующем этапе.
 */
#ifndef GUI_SETTINGS_APP_H
#define GUI_SETTINGS_APP_H

#include "gconsole.h"

typedef struct {
    gconsole_t console;
    uint8_t *desktop_bg;   /* указатель на цвет фона рабочего стола в wm.c */
} settings_app_t;

/* Результат обработки клавиши: */
#define SETTINGS_NOTHING       0
#define SETTINGS_COLOR_CHANGED 1
#define SETTINGS_RES_320x200   2
#define SETTINGS_RES_640x480   3
#define SETTINGS_RES_800x600   4
#define SETTINGS_WALLPAPER     5
#define SETTINGS_THEME_LIGHT   6
#define SETTINGS_THEME_DARK    7

void settings_app_init(settings_app_t *app, const gui_window_t *win, uint8_t *desktop_bg);
int  settings_app_handle_char(settings_app_t *app, char c);

#endif
