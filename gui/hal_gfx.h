/* gui/hal_gfx.h — пиксельный графический HAL.
 *
 * Реализация разложена на три файла:
 *   gui/hal_gfx.c            — примитивы рисования, общие для всех платформ
 *   arch/x86/hal_gfx_x86.c   — инициализация экрана: mode13h / VBE
 *   arch/arm64/hal_gfx_arm64.c — инициализация экрана: ramfb / дисплей телефона
 *
 * Рисование везде идёт ИНДЕКСАМИ палитры (см. gui/palette.h), даже там,
 * где у железа честный 32-битный цвет: так весь код интерфейса, написанный
 * под mode13h, работает на телефоне без единой правки, а преобразование
 * в RGB происходит один раз за кадр внутри gfxfb_present().
 */
#ifndef PROSHIVKAOS_HAL_GFX_H
#define PROSHIVKAOS_HAL_GFX_H

#include <stdint.h>

/* Индексы палитры. Значения цветов — в gui/palette.c. */
enum {
    /* 0..15 — классические 16 цветов VGA */
    GFX_BLACK = 0, GFX_BLUE = 1, GFX_GREEN = 2, GFX_CYAN = 3,
    GFX_RED = 4, GFX_MAGENTA = 5, GFX_BROWN = 6, GFX_LIGHT_GREY = 7,
    GFX_DARK_GREY = 8, GFX_LIGHT_BLUE = 9, GFX_LIGHT_GREEN = 10,
    GFX_LIGHT_CYAN = 11, GFX_LIGHT_RED = 12, GFX_LIGHT_MAGENTA = 13,
    GFX_YELLOW = 14, GFX_WHITE = 15,

    /* 16..21 — интерфейс в духе Windows XP (Luna): десктопная сборка */
    GFX_XP_TITLE_LIGHT = 16,  /* верх градиента title bar */
    GFX_XP_TITLE_DARK  = 17,  /* низ градиента title bar / панель задач */
    GFX_XP_BODY        = 18,  /* тело окна — светлый бежево-серый */
    GFX_XP_START_GREEN = 19,  /* кнопка "Пуск" */
    GFX_XP_BORDER      = 20,  /* синяя окантовка окна */
    GFX_XP_CLOSE_RED   = 21,  /* кнопка закрытия */

    /* 22..31 — обои рабочего стола (холмы/небо): десктопная сборка */
    GFX_SKY_DEEP     = 22,
    GFX_SKY_MID      = 23,
    GFX_SKY_HORIZON  = 24,
    GFX_HILL_DARK    = 25,
    GFX_HILL_MID     = 26,
    GFX_HILL_LIGHT   = 27,
    GFX_SUN_YELLOW   = 28,
    GFX_CLOUD_WHITE  = 29,
    GFX_HILL_SHADOW  = 30,
    GFX_SUN_GLOW     = 31,

    /* 32..47 — тач-интерфейс (тёмная тема): touch-сборка.
     * В desktop-сборке эти индексы затираются палитрой запечённых обоев,
     * и наоборот — touch-сборка запечённые обои не подключает вовсе
     * (они landscape 4:3, на портретном экране телефона бесполезны). */
    GFX_UI_BG          = 32,
    GFX_UI_SURFACE     = 33,
    GFX_UI_SURFACE_2   = 34,
    GFX_UI_ACCENT      = 35,
    GFX_UI_ACCENT_DARK = 36,
    GFX_UI_TEXT        = 37,
    GFX_UI_TEXT_DIM    = 38,
    GFX_UI_DIVIDER     = 39,
    GFX_UI_KEY         = 40,
    GFX_UI_KEY_DOWN    = 41,
    GFX_UI_WARN        = 42,
    GFX_UI_OK          = 43,
    GFX_UI_GRAD_TOP    = 44,
    GFX_UI_GRAD_MID    = 45,
    GFX_UI_GRAD_BOT    = 46,
    GFX_UI_SHADOW      = 47
};

/* ---------------- Инициализация экрана (арх-зависимая часть) ---------------- */
void hal_gfx_init(void);
int  hal_gfx_width(void);
int  hal_gfx_height(void);

/* Смена разрешения. На x86: 320x200 (mode13h) всегда, прочее — через VBE
 * под QEMU/Bochs/VirtualBox. На ARM64: любое разрешение, ramfb просто
 * пересоздаёт поверхность. Возвращает 0, если режим недоступен — экран
 * при этом остаётся как был, ничего не портится. */
int  hal_gfx_set_resolution(int width, int height);
int  hal_gfx_is_vbe(void);   /* x86: 1, если активен VBE, а не mode13h */

/* Программирует палитру под конкретную картинку. Индексы ниже
 * PALETTE_FIXED_COLORS трогать нельзя — это цвета интерфейса. */
void hal_gfx_set_image_palette(const uint8_t *rgb_255, int count, int start_index);

/* ---------------- Примитивы (общие для всех платформ) ---------------- */
void hal_gfx_put_pixel(int x, int y, uint8_t color);
void hal_gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void hal_gfx_draw_rect(int x, int y, int w, int h, uint8_t color); /* контур, 1px */
void hal_gfx_clear(uint8_t color);

/* Прямой блит массива индексов палитры (w*h байт, по строкам сверху вниз). */
void hal_gfx_blit(int x, int y, int w, int h, const uint8_t *data);

/* Вертикальный градиент через упорядоченный дизеринг (Bayer 2x2): вместо
 * жёсткой границы двух цветов — плавный переход при той же палитре. */
void hal_gfx_dither_gradient_v(int x, int y, int w, int h,
                                uint8_t top_color, uint8_t bottom_color);

/* Многоцветный вертикальный градиент: дизерингом смешивает соседние цвета
 * из списка. Нужен для обоев тач-сборки, где двух цветов на весь высокий
 * экран телефона откровенно мало. */
void hal_gfx_dither_gradient_multi(int x, int y, int w, int h,
                                    const uint8_t *colors, int color_count);

/* Залитый прямоугольник со скруглёнными (срезанными) углами. */
void hal_gfx_fill_rounded_rect(int x, int y, int w, int h, uint8_t color, int radius);
/* Его контур — совпадает со срезом заливки. */
void hal_gfx_draw_rounded_rect(int x, int y, int w, int h, uint8_t color, int radius);

/* Глянцевая кнопка: дизеринг-градиент + блик у верхней грани + скругление. */
void hal_gfx_draw_glossy_button(int x, int y, int w, int h,
                                 uint8_t top_color, uint8_t bottom_color,
                                 uint8_t border_color, int radius);

/* Мягкая тень под карточкой — несколько скруглённых прямоугольников со
 * сдвигом. На индексной палитре альфа-канала нет, поэтому "мягкость"
 * имитируется дизерингом теневого цвета. */
void hal_gfx_drop_shadow(int x, int y, int w, int h, int radius, int depth);

/* Показать нарисованный кадр (двойная буферизация) — ОДИН раз в конце
 * отрисовки кадра, не после каждого примитива. */
void hal_gfx_present(void);

/* ---------------- Текст ---------------- */
/* Шрифт 8x8, только заглавные/цифры/пунктуация (см. gui/font8x8.c);
 * строчные буквы приводятся к заглавным автоматически. */
#define FONT_W 8
#define FONT_H 8

void hal_gfx_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);
void hal_gfx_draw_string(int x, int y, const char *s, uint8_t fg, uint8_t bg);

/* Без заливки фона — рисуются только "включённые" пиксели глифа.
 * Нужно для текста поверх картинок и градиентов. */
void hal_gfx_draw_string_transparent(int x, int y, const char *s, uint8_t fg);

/* Масштабированный текст. На экране телефона 8x8 нечитаемо: при плотности
 * ~300 dpi это буква высотой меньше миллиметра. scale=2..4 — рабочий
 * диапазон; каждый пиксель глифа рисуется квадратом scale x scale. */
void hal_gfx_draw_char_scaled(int x, int y, char c, uint8_t fg, uint8_t bg, int scale);
void hal_gfx_draw_string_scaled(int x, int y, const char *s, uint8_t fg, int scale);

/* Ширина строки в пикселях при заданном масштабе — для центрирования. */
int  hal_gfx_string_width(const char *s, int scale);

/* Строка, центрированная по горизонтали внутри прямоугольника. */
void hal_gfx_draw_string_centered(int x, int y, int w, const char *s, uint8_t fg, int scale);

#endif
