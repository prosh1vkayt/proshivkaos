/* gui/wallpaper.h — "одна картинка" рабочего стола: небо + холмы + солнце,
 * нарисованные примитивами (своего декодера картинок у нас нет — только
 * RamFS с текстовыми файлами, PNG/BMP-парсер сюда не тащим).
 */
#ifndef GUI_WALLPAPER_H
#define GUI_WALLPAPER_H

void gui_draw_wallpaper(int screen_w, int screen_h);

#endif
