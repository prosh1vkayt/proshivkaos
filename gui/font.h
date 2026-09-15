/* gui/font.h — сглаженные шрифты (Roboto, Roboto Mono), растрированные
 * заранее утилитой tools/fontgen.py. См. gui/font.c. */
#ifndef GUI_FONT_H
#define GUI_FONT_H

#include <stdint.h>

#define FONT_SCALES 9            /* индекс — масштаб старой сетки 8x8: 1..8 */

typedef struct {
    uint16_t cp;                 /* код символа Юникода                      */
    uint8_t  w, h;               /* размер маски                             */
    int8_t   bx;                 /* сдвиг маски вправо от точки пера         */
    int8_t   by;                 /* верх маски над базовой линией            */
    uint16_t adv64;              /* шаг пера, 1/64 пикселя                   */
    uint32_t off;                /* смещение маски в массиве bits            */
} font_glyph_t;

typedef struct {
    uint8_t  em, ascent, descent;
    uint16_t count;
    const font_glyph_t *glyphs;  /* отсортированы по cp                       */
    const uint8_t *bits;
} font_face_t;

extern const font_face_t g_font_ui[FONT_SCALES];
extern const font_face_t g_font_mono[FONT_SCALES];

/* Нарисовать строку UTF-8; y — ВЕРХ клетки старой сетки (8*scale),
 * базовая линия на 7*scale. Возвращает ширину в пикселях. */
int  font_draw(const font_face_t *face, int x, int y, int scale, const char *utf8, uint32_t rgb);
int  font_width(const font_face_t *face, const char *utf8);

/* Одна буква с выравниванием по центру клетки шириной cell_w. */
void font_draw_cp_centered(const font_face_t *face, int x, int y, int scale, int cell_w,
                           uint32_t cp, uint32_t rgb);

/* Ширина шага моноширинного шрифта, округлённая вверх, пиксели. */
int  font_mono_advance(int scale);

/* Разобрать следующий символ UTF-8. */
uint32_t font_next_cp(const char **s);

#endif
