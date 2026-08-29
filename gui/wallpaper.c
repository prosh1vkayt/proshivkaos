/* gui/wallpaper.c — рисует обои рабочего стола.
 *
 * Основной путь: встроенная в бинарник картинка (gui/wallpaper_data.h) —
 * настоящая сглаженная графика, "запечённая" на этапе сборки (суперсэмплинг
 * + дизеринг в адаптивную 224-цветную палитру конкретно под неё), а не
 * нарисованная нашими же кривыми примитивами в рантайме.
 *
 * Запасной путь: если текущее разрешение не совпадает ни с одной из
 * встроенных картинок (кто-то добавит новый пресет разрешения без
 * картинки под него) — рисуем старый процедурный пейзаж примитивами,
 * чтобы рабочий стол не остался пустым/чёрным.
 */
#include "hal_gfx.h"
#include "wallpaper.h"
#include "wallpaper_data.h"

/* ---------------- запасной процедурный пейзаж (старая версия) ---------------- */

typedef struct { int x_pct, h_pct; } hill_point_t;

static const hill_point_t hill_back[]  = {
    {0, 35}, {25, 45}, {50, 30}, {75, 50}, {100, 35}
};
static const hill_point_t hill_front[] = {
    {0, 20}, {20, 30}, {45, 15}, {70, 35}, {100, 18}
};

static int interpolate_height(const hill_point_t *pts, int n, int x_pct) {
    for (int i = 0; i < n - 1; i++) {
        if (x_pct >= pts[i].x_pct && x_pct <= pts[i + 1].x_pct) {
            int x0 = pts[i].x_pct,     h0 = pts[i].h_pct;
            int x1 = pts[i + 1].x_pct, h1 = pts[i + 1].h_pct;
            if (x1 == x0) return h0;
            return h0 + (h1 - h0) * (x_pct - x0) / (x1 - x0);
        }
    }
    return pts[n - 1].h_pct;
}

static void draw_hill_layer(int screen_w, int screen_h,
                             const hill_point_t *pts, int n, uint8_t color) {
    for (int x = 0; x < screen_w; x++) {
        int x_pct = x * 100 / screen_w;
        int h_pct = interpolate_height(pts, n, x_pct);
        int y_top = screen_h - (h_pct * screen_h / 100);
        for (int y = y_top; y < screen_h; y++)
            hal_gfx_put_pixel(x, y, color);
    }
}

static void draw_filled_circle(int cx, int cy, int r, uint8_t color) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                hal_gfx_put_pixel(cx + x, cy + y, color);
}

static void draw_cloud(int cx, int cy, int r) {
    draw_filled_circle(cx - r, cy,       r,               GFX_CLOUD_WHITE);
    draw_filled_circle(cx,     cy - r/2, r + r / 3,        GFX_CLOUD_WHITE);
    draw_filled_circle(cx + r, cy,       r,               GFX_CLOUD_WHITE);
}

static void draw_procedural_fallback(int screen_w, int screen_h) {
    int horizon_y = screen_h * 62 / 100;

    int band1 = horizon_y * 40 / 100;
    int band2 = horizon_y * 75 / 100;
    for (int y = 0; y < horizon_y; y++) {
        uint8_t color = (y < band1) ? GFX_SKY_DEEP
                       : (y < band2) ? GFX_SKY_MID
                                     : GFX_SKY_HORIZON;
        for (int x = 0; x < screen_w; x++)
            hal_gfx_put_pixel(x, y, color);
    }

    for (int y = horizon_y; y < screen_h; y++)
        for (int x = 0; x < screen_w; x++)
            hal_gfx_put_pixel(x, y, GFX_HILL_MID);

    int sun_r = (screen_w < screen_h ? screen_w : screen_h) * 5 / 100;
    if (sun_r < 4) sun_r = 4;
    int sun_x = screen_w * 82 / 100;
    int sun_y = screen_h * 14 / 100;
    draw_filled_circle(sun_x, sun_y, sun_r + sun_r / 2, GFX_SUN_GLOW);
    draw_filled_circle(sun_x, sun_y, sun_r, GFX_SUN_YELLOW);

    draw_cloud(screen_w * 20 / 100, screen_h * 12 / 100, sun_r / 2 + 2);
    draw_cloud(screen_w * 45 / 100, screen_h * 8  / 100, sun_r / 2 + 1);

    draw_hill_layer(screen_w, screen_h, hill_back,
                     sizeof(hill_back) / sizeof(hill_back[0]), GFX_HILL_DARK);
    draw_hill_layer(screen_w, screen_h, hill_front,
                     sizeof(hill_front) / sizeof(hill_front[0]), GFX_HILL_LIGHT);
}

/* ---------------- водяной знак (поверх чего угодно) ---------------- */

static void draw_watermark(int x, int y, const char *text) {
    hal_gfx_draw_string_transparent(x + 1, y + 1, text, GFX_BLACK);
    hal_gfx_draw_string_transparent(x, y, text, GFX_CLOUD_WHITE);
}

/* ---------------- основная точка входа ---------------- */

void gui_draw_wallpaper(int screen_w, int screen_h) {
    static int last_w = -1, last_h = -1;

    const uint8_t *pixels = 0;
    const uint8_t *palette = 0;

    if (screen_w == 320 && screen_h == 200) {
        pixels = wallpaper_320x200_pixels;
        palette = wallpaper_320x200_palette;
    } else if (screen_w == 640 && screen_h == 480) {
        pixels = wallpaper_640x480_pixels;
        palette = wallpaper_640x480_palette;
    } else if (screen_w == 800 && screen_h == 600) {
        pixels = wallpaper_800x600_pixels;
        palette = wallpaper_800x600_palette;
    }

    if (pixels) {
        /* палитру шьём в DAC только при смене разрешения — она не
           меняется от кадра к кадру, незачем гонять 224 записи каждый
           раз при перерисовке (а перерисовка происходит на каждое
           движение мыши) */
        if (screen_w != last_w || screen_h != last_h) {
            hal_gfx_set_image_palette(palette, WALLPAPER_PALETTE_COLORS, WALLPAPER_PALETTE_START);
            last_w = screen_w;
            last_h = screen_h;
        }
        hal_gfx_blit(0, 0, screen_w, screen_h, pixels);
    } else {
        draw_procedural_fallback(screen_w, screen_h);
        last_w = -1; last_h = -1;   /* сбрасываем — вдруг вернёмся на картиночное разрешение */
    }

    draw_watermark(4, 4, "SHHH, IT'S PROTOTYPE, DO NOT LEAK...");
}
