/* gui/icon.h — векторные иконки Material Symbols, растрированные заранее
 * утилитой tools/icongen.py и масштабируемые на лету (gui/icon.c). */
#ifndef GUI_ICON_H
#define GUI_ICON_H

#include <stdint.h>

/* Порядок — как NAMES в tools/icongen.py. */
enum {
    ICON_TERMINAL, ICON_FOLDER, ICON_SETTINGS, ICON_INFO,
    ICON_NAV_BACK, ICON_NAV_HOME, ICON_NAV_RECENTS, ICON_CHEVRON_RIGHT,
    ICON_ARROW_BACK, ICON_FILE, ICON_KEYBOARD_HIDE, ICON_BATTERY,
    ICON_COUNT
};

#define ICON_SIZES 3

typedef struct { const uint8_t *mask[ICON_SIZES]; } icon_def_t;

extern const icon_def_t g_icons[ICON_COUNT];
extern const int g_icon_sizes[ICON_SIZES];

/* Нарисовать иконку размером size x size с левым верхним углом в (x, y). */
void icon_draw(int id, int x, int y, int size, uint32_t rgb);

#endif
