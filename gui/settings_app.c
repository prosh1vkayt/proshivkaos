/* gui/settings_app.c */
#include "settings_app.h"

void settings_app_init(settings_app_t *app, const gui_window_t *win, uint8_t *desktop_bg) {
    gconsole_init(&app->console, win, GFX_BLACK, GFX_LIGHT_GREY);
    app->desktop_bg = desktop_bg;

    gconsole_write(&app->console, "SETTINGS");
    gconsole_write(&app->console, "\n\nTHEME:");
    gconsole_write(&app->console, "\nL LIGHT D DARK");
    gconsole_write(&app->console, "\n\nBACKGROUND:");
    gconsole_write(&app->console, "\nW WALLPAPER");
    gconsole_write(&app->console, "\n1 CYAN 2 GREEN");
    gconsole_write(&app->console, "\n3 BLUE 4 GREY");
    gconsole_write(&app->console, "\n\nRESOLUTION:");
    gconsole_write(&app->console, "\nA 320X200");
    gconsole_write(&app->console, "\nB 640X480");
    gconsole_write(&app->console, "\nC 800X600");
}

int settings_app_handle_char(settings_app_t *app, char c) {
    uint8_t new_color;
    switch (c) {
        case 'l': case 'L': return SETTINGS_THEME_LIGHT;
        case 'd': case 'D': return SETTINGS_THEME_DARK;

        case 'w': case 'W': return SETTINGS_WALLPAPER;

        case '1': new_color = GFX_CYAN;       break;
        case '2': new_color = GFX_GREEN;      break;
        case '3': new_color = GFX_LIGHT_BLUE; break;
        case '4': new_color = GFX_DARK_GREY;  break;

        case 'a': case 'A': return SETTINGS_RES_320x200;
        case 'b': case 'B': return SETTINGS_RES_640x480;
        case 'c': case 'C': return SETTINGS_RES_800x600;

        default: return SETTINGS_NOTHING;
    }

    *app->desktop_bg = new_color;
    return SETTINGS_COLOR_CHANGED;
}
