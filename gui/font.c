/* gui/font.c — сглаженный текст из заранее растрированных масок.
 *
 * Буква — это маска прозрачности 0..255, отрисованная на компьютере
 * утилитой tools/fontgen.py из Roboto. Здесь её только накладывают нужным
 * цветом, поэтому текст выглядит так же, как в Android, а стоит почти
 * столько же, сколько прежний пиксельный шрифт 8x8.
 */
#include "font.h"
#include "gfxfb.h"

uint32_t font_next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = *p;
    int n = 0;
    if (cp < 0x80)            n = 0;
    else if ((cp >> 5) == 6)  { cp &= 0x1F; n = 1; }
    else if ((cp >> 4) == 14) { cp &= 0x0F; n = 2; }
    else if ((cp >> 3) == 30) { cp &= 0x07; n = 3; }
    else                      { *s += 1; return '?'; }
    p++;
    for (int i = 0; i < n; i++, p++) {
        if ((*p & 0xC0) != 0x80) { *s = (const char *)p; return '?'; }
        cp = (cp << 6) | (*p & 0x3F);
    }
    *s = (const char *)p;
    return cp;
}

static const font_glyph_t *find_glyph(const font_face_t *f, uint32_t cp) {
    int lo = 0, hi = (int)f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t c = f->glyphs[mid].cp;
        if (c == cp) return &f->glyphs[mid];
        if (c < cp) lo = mid + 1; else hi = mid - 1;
    }
    return cp == '?' ? 0 : find_glyph(f, '?');
}

static void draw_glyph(const font_face_t *f, const font_glyph_t *g, int pen_x, int baseline,
                       uint32_t rgb) {
    if (!g || g->w == 0) return;
    gfxfb_blit_alpha8(pen_x + g->bx, baseline - g->by, g->w, g->h,
                      f->bits + g->off, g->w, rgb);
}

int font_draw(const font_face_t *f, int x, int y, int scale, const char *s, uint32_t rgb) {
    int baseline = y + 7 * scale;
    long pen64 = (long)x * 64;
    while (*s) {
        uint32_t cp = font_next_cp(&s);
        const font_glyph_t *g = find_glyph(f, cp);
        if (!g) continue;
        draw_glyph(f, g, (int)((pen64 + 32) >> 6), baseline, rgb);
        pen64 += g->adv64;
    }
    return (int)((pen64 - (long)x * 64 + 63) >> 6);
}

int font_width(const font_face_t *f, const char *s) {
    long w64 = 0;
    while (*s) {
        const font_glyph_t *g = find_glyph(f, font_next_cp(&s));
        if (g) w64 += g->adv64;
    }
    return (int)((w64 + 63) >> 6);
}

void font_draw_cp_centered(const font_face_t *f, int x, int y, int scale, int cell_w,
                           uint32_t cp, uint32_t rgb) {
    const font_glyph_t *g = find_glyph(f, cp);
    if (!g) return;
    int adv = (g->adv64 + 32) >> 6;
    draw_glyph(f, g, x + (cell_w - adv) / 2, y + 7 * scale, rgb);
}

int font_mono_advance(int scale) {
    if (scale < 1) scale = 1;
    if (scale >= FONT_SCALES) scale = FONT_SCALES - 1;
    const font_glyph_t *g = find_glyph(&g_font_mono[scale], 'M');
    return g ? (g->adv64 + 63) >> 6 : 8 * scale;
}
