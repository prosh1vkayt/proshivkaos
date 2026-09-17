/* gui/icon.c — векторные иконки на экране.
 *
 * Маска иконки заранее растрирована из SVG (tools/icongen.py) в нескольких
 * опорных размерах. Здесь берётся ближайшая не меньшая и уменьшается
 * билинейно до нужного размера — край остаётся сглаженным при любом
 * масштабе, а памяти уходит на три размера, а не на все. */
#include "icon.h"
#include "gfxfb.h"

#define ICON_MAX 512
static uint8_t g_scaled[ICON_MAX * ICON_MAX];

void icon_draw(int id, int x, int y, int size, uint32_t rgb) {
    if (id < 0 || id >= ICON_COUNT || size <= 0) return;
    if (size > ICON_MAX) size = ICON_MAX;

    int k = 0;
    while (k < ICON_SIZES - 1 && g_icon_sizes[k] < size) k++;
    int src = g_icon_sizes[k];
    const uint8_t *m = g_icons[id].mask[k];

    if (src == size) { gfxfb_blit_alpha8(x, y, size, size, m, size, rgb); return; }

    /* Шаг по исходной маске в 16.16, выборка в центрах пикселей. */
    uint32_t step = (uint32_t)(((uint64_t)src << 16) / (uint32_t)size);
    for (int j = 0; j < size; j++) {
        int64_t sy = ((int64_t)j * step) + (step >> 1) - 32768;
        if (sy < 0) sy = 0;
        int y0 = (int)(sy >> 16), y1 = y0 + 1 < src ? y0 + 1 : y0;
        uint32_t fy = (uint32_t)(sy & 0xFFFF) >> 8;
        uint8_t *d = &g_scaled[j * size];
        for (int i = 0; i < size; i++) {
            int64_t sx = ((int64_t)i * step) + (step >> 1) - 32768;
            if (sx < 0) sx = 0;
            int x0 = (int)(sx >> 16), x1 = x0 + 1 < src ? x0 + 1 : x0;
            uint32_t fx = (uint32_t)(sx & 0xFFFF) >> 8;
            uint32_t a = m[y0 * src + x0], b = m[y0 * src + x1];
            uint32_t c = m[y1 * src + x0], e = m[y1 * src + x1];
            uint32_t top = a * (256 - fx) + b * fx;
            uint32_t bot = c * (256 - fx) + e * fx;
            d[i] = (uint8_t)((top * (256 - fy) + bot * fy) >> 16);
        }
    }
    gfxfb_blit_alpha8(x, y, size, size, g_scaled, size, rgb);
}
