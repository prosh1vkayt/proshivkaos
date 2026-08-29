/* gui/gconsole.h — текстовая консоль поверх пиксельного окна.
 * Хранит текст как ЛОГИЧЕСКИЕ строки (как реальный терминал/скроллбек),
 * а не как фиксированную сетку "столбцы x строки" — перенос по словам/
 * символам пересчитывается заново при каждой отрисовке, исходя из
 * ТЕКУЩЕГО размера окна. Поэтому ресайз окна меняет разбивку на строки
 * "живьём", как в реальном терминале (iTerm/Windows Terminal/gnome-terminal).
 */
#ifndef GUI_GCONSOLE_H
#define GUI_GCONSOLE_H

#include "hal_gfx.h"
#include "window.h"

#define GC_MAX_LINES    64
#define GC_MAX_LINE_LEN 120

typedef struct {
    const gui_window_t *win;      /* может быть 0, если задан явный rect */

    /* Явная область вывода вместо окна. Нужна тач-сборке: там приложения
     * занимают весь экран и никакого gui_window_t с заголовком и рамкой
     * у них нет — есть просто прямоугольник, выделенный оболочкой. */
    int use_rect;
    int rect_x, rect_y, rect_w, rect_h;

    /* Масштаб шрифта. На экране телефона 8x8 нечитаемо: при плотности
     * ~300 dpi высота буквы получается меньше миллиметра. */
    int scale;

    char lines[GC_MAX_LINES][GC_MAX_LINE_LEN];
    uint8_t color[GC_MAX_LINES][GC_MAX_LINE_LEN];   /* цвет каждого символа отдельно */
    int  line_len[GC_MAX_LINES];
    int  line_count;
    uint8_t fg, bg;
    uint8_t ink;   /* цвет, которым пишутся СЛЕДУЮЩИЕ символы — см. gconsole_set_ink() */
} gconsole_t;

void gconsole_init(gconsole_t *gc, const gui_window_t *win, uint8_t fg, uint8_t bg);
void gconsole_clear(gconsole_t *gc);
void gconsole_putc(gconsole_t *gc, char c);
void gconsole_write(gconsole_t *gc, const char *s);
void gconsole_render(gconsole_t *gc);   /* пересчитывает перенос строк и рисует */

void gconsole_set_colors(gconsole_t *gc, uint8_t fg, uint8_t bg);

/* Рисовать не в окно, а в заданный прямоугольник. */
void gconsole_set_rect(gconsole_t *gc, int x, int y, int w, int h);

/* Масштаб шрифта (1 — как на десктопе, 2-3 — для телефона). */
void gconsole_set_scale(gconsole_t *gc, int scale);

/* Сколько символов помещается в строку при текущих размерах и масштабе —
 * приложениям нужно знать это, чтобы форматировать вывод. */
int  gconsole_cols(const gconsole_t *gc);

/* Цвет для следующих символов, которые будут записаны через putc/write —
 * не трогает уже написанный текст. Сбросить к цвету по умолчанию:
 * gconsole_set_ink(gc, gc->fg). */
void gconsole_set_ink(gconsole_t *gc, uint8_t color);

#endif
