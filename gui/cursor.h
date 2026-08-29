/* gui/cursor.h — программный курсор мыши (перерисовывается каждый кадр
 * поверх остального — двойного буфера/аппаратного курсора у нас нет).
 */
#ifndef GUI_CURSOR_H
#define GUI_CURSOR_H

void gui_draw_cursor(int x, int y);

#endif
