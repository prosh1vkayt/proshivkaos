/* gui/wallpaper_data.h — встроенные в бинарник ядра картинки обоев.
 *
 * Как это получилось: НЕ во время выполнения ОС, а заранее на хосте —
 * картинка отрисована и сглажена (суперсэмплинг + Lanczos), затем
 * сдизерена (Floyd-Steinberg) в АДАПТИВНУЮ 224-цветную палитру именно
 * под эту картинку. У нас нет декодера PNG/JPEG в самом ядре (и не
 * будет — это огромный объём кода ради сомнительной пользы в
 * nostdlib-окружении), поэтому картинка просто "запечена" в бинарник
 * как обычный C-массив байт — индексов в палитру.
 *
 * Палитра: индексы 0-31 заняты фиксированными цветами интерфейса
 * (см. hal_gfx.h) — их трогать нельзя. Индексы 32-255 (224 штуки) —
 * под конкретную картинку, программируются в DAC каждый раз перед
 * отрисовкой обоев (vga13h_set_palette_range()).
 *
 * Если понадобится другая картинка — пришлите PNG/JPEG нужного
 * разрешения (320x200 / 640x480 / 800x600), я прогоню через тот же
 * конвейер (суперсэмплинг + дизеринг) и перегенерирую эти файлы.
 */
#ifndef GUI_WALLPAPER_DATA_H
#define GUI_WALLPAPER_DATA_H

#include <stdint.h>

#define WALLPAPER_PALETTE_COLORS 224
#define WALLPAPER_PALETTE_START  32   /* первый использумый DAC-индекс */

extern const uint8_t wallpaper_320x200_pixels[320 * 200];
extern const uint8_t wallpaper_320x200_palette[WALLPAPER_PALETTE_COLORS * 3];

extern const uint8_t wallpaper_640x480_pixels[640 * 480];
extern const uint8_t wallpaper_640x480_palette[WALLPAPER_PALETTE_COLORS * 3];

extern const uint8_t wallpaper_800x600_pixels[800 * 600];
extern const uint8_t wallpaper_800x600_palette[WALLPAPER_PALETTE_COLORS * 3];

#endif
