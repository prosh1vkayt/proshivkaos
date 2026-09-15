/* gui/gfxfb.h — двойная буферизация + вывод кадра на экран.
 *
 * Переехал из arch/x86/gfxfb.h: файл никогда не был x86-специфичным —
 * это просто «нарисуй кадр в массив в ОЗУ, потом отдай его целиком».
 * Единственное, что действительно зависит от платформы, — В КАКОМ
 * ФОРМАТЕ видеопамять ждёт пиксели:
 *
 *   x86 mode13h / VBE 8bpp : байт на пиксель, значение = индекс палитры,
 *                             цвет подставляет аппаратный DAC видеокарты;
 *   ARM64 ramfb / телефон  : четыре байта на пиксель, честный XRGB8888,
 *                             никакой аппаратной палитры не существует.
 *
 * Бэкбуфер при этом всегда в истинном цвете, 0x00RRGGBB: индексы палитры
 * превращаются в RGB в момент рисования, а формат видеопамяти учитывается
 * только при выводе кадра.
 */
#ifndef GUI_GFXFB_H
#define GUI_GFXFB_H

#include <stdint.h>

/* Максимум пикселей в кадре — 1080x1920 (Full HD в портрете). Столько
 * нужно под экран Redmi Note 4: он отдаёт готовый фреймбуфер именно этого
 * разрешения, и рисовать в буфер меньшего размера было бы некуда. Это 8 МиБ
 * .bss под бэкбуфер в истинном цвете. */
#define GFXFB_MAX_PIXELS (1080 * 1920)

/* Сколько байт занимает самая длинная строка кадра. Три байта на пиксель
 * — самый плотный из поддерживаемых форматов, но строку собирают и для
 * четырёхбайтовых, поэтому берём с запасом. */
#define GFXFB_MAX_LINE_BYTES (1080 * 4)

enum {
    GFXFB_FMT_PAL8   = 0,   /* 1 байт/пиксель, индекс палитры  */
    GFXFB_FMT_XRGB32 = 1,   /* 4 байта/пиксель, 0x00RRGGBB     */
    GFXFB_FMT_RGB24  = 2    /* 3 байта/пиксель, R,G,B по порядку —
                             * формат "r8g8b8" фреймбуфера, который
                             * оставляет загрузчик реальных телефонов
                             * (см. arch/arm64/boards/mido.h) */
};

/* pitch_bytes — длина строки в БАЙТАХ. У ramfb и у большинства
 * контроллеров дисплея она может быть больше, чем width * bpp
 * (выравнивание строки), поэтому считать её самостоятельно нельзя. */
void gfxfb_bind(volatile void *fb, int width, int height, int pitch_bytes, int fmt);

int  gfxfb_width(void);
int  gfxfb_height(void);

void gfxfb_put_pixel(int x, int y, uint8_t color);

/* ---- Истинный цвет: rgb = 0x00RRGGBB, непрозрачность 0..255 ---- */
#define GFX_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

void gfxfb_put_rgb(int x, int y, uint32_t rgb);
void gfxfb_blend_pixel(int x, int y, uint32_t rgb, uint32_t alpha255);
void gfxfb_fill_rect_rgb(int x, int y, int w, int h, uint32_t rgb);
void gfxfb_blend_rect_rgb(int x, int y, int w, int h, uint32_t rgb, uint32_t alpha255);
void gfxfb_span_rgb(int x, int y, int w, uint32_t rgb, uint32_t alpha255);
void gfxfb_blit_alpha8(int x, int y, int w, int h, const uint8_t *mask, int stride,
                       uint32_t rgb);
void gfxfb_blit_rgb(int x, int y, int w, int h, const uint32_t *src, int stride);
uint32_t *gfxfb_backbuffer32(void);

/* Прямоугольник отсечения: пиксели за его пределами молча отбрасываются.
 * Нужен там, где содержимое заведомо больше отведённого места — прокрутка
 * длинного списка, круг, вылезающий за край карточки. Без него каждый
 * такой случай приходилось бы обрезать вручную в вызывающем коде. */
void gfxfb_set_clip(int x, int y, int w, int h);
void gfxfb_reset_clip(void);
void gfxfb_clear(uint8_t color);

/* Залить прямоугольник разом — на порядок дешевле, чем по пикселю. */
void gfxfb_fill_rect(int x, int y, int w, int h, uint8_t color);

/* Считать кадр изменившимся целиком. Нужно там, где содержимое памяти
 * экрана поменял кто-то помимо нас. */
void gfxfb_mark_all_dirty(void);

/* Кадр лежит в некэшируемой памяти: выталкивать кэш после отрисовки не
 * нужно, а обход всего кадра строками кэша стоит дорого. */
void gfxfb_set_uncached(int on);

/* Скопировать бэкбуфер в видеопамять (с преобразованием формата, если
 * надо) — один раз в конце отрисовки кадра, не после каждого примитива. */
void gfxfb_present(void);

/* Замер последнего вывода кадра: сколько микросекунд и сколько пикселей
 * ушло в память экрана. Для бенчмарка и поиска узких мест. */
void gfxfb_last_present(uint32_t *us, uint32_t *pixels);

/* Прямой доступ к бэкбуферу — для быстрых операций вроде блита картинки
 * построчно, где проверка границ на каждый пиксель себя не окупает. */
uint8_t *gfxfb_backbuffer(void);

#endif
