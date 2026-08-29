/* gui/palette.c — значения всех фиксированных цветов системы. */
#include "palette.h"

palette_rgb_t g_palette[PALETTE_SIZE];
uint32_t      g_palette_xrgb[PALETTE_SIZE];

static const palette_rgb_t g_fixed[PALETTE_FIXED_COLORS] = {
    /* ---- 0..15: классические 16 цветов VGA ---- */
    {0,   0,   0},    /* 0  black            */
    {0,   0,   170},  /* 1  classic blue     */
    {0,   170, 0},    /* 2  green            */
    {0,   170, 170},  /* 3  cyan             */
    {170, 0,   0},    /* 4  red              */
    {170, 0,   170},  /* 5  magenta          */
    {85,  85,  0},    /* 6  brown            */
    {170, 170, 170},  /* 7  light grey       */
    {85,  85,  85},   /* 8  dark grey        */
    {85,  85,  255},  /* 9  light blue       */
    {85,  255, 85},   /* 10 light green      */
    {85,  255, 255},  /* 11 light cyan       */
    {255, 85,  85},   /* 12 light red        */
    {255, 85,  255},  /* 13 light magenta    */
    {255, 255, 85},   /* 14 yellow           */
    {255, 255, 255},  /* 15 white            */

    /* ---- 16..21: Windows XP (Luna), десктопная сборка ---- */
    {160, 220, 255},  /* 16 XP_TITLE_LIGHT */
    {20,  80,  220},  /* 17 XP_TITLE_DARK  */
    {224, 228, 240},  /* 18 XP_BODY        */
    {40,  180, 40},   /* 19 XP_START_GREEN */
    {32,  100, 220},  /* 20 XP_BORDER      */
    {220, 40,  40},   /* 21 XP_CLOSE_RED   */

    /* ---- 22..31: обои десктопа (холмы и небо) ---- */
    {28,  60,  120},  /* 22 SKY_DEEP     */
    {80,  140, 208},  /* 23 SKY_MID      */
    {180, 208, 232},  /* 24 SKY_HORIZON  */
    {40,  88,  40},   /* 25 HILL_DARK    */
    {68,  140, 60},   /* 26 HILL_MID     */
    {120, 188, 88},   /* 27 HILL_LIGHT   */
    {255, 228, 120},  /* 28 SUN_YELLOW   */
    {248, 248, 248},  /* 29 CLOUD_WHITE  */
    {20,  48,  20},   /* 30 HILL_SHADOW  */
    {255, 244, 200},  /* 31 SUN_GLOW     */

    /* ---- 32..47: палитра тач-интерфейса ----
     * Тёмная по умолчанию — не мода, а расчёт: на телефоне экран светит
     * в лицо, и на OLED тёмный фон вдобавок буквально не потребляет
     * энергию (чёрный пиксель = выключенный пиксель). Акцент бирюзовый,
     * чтобы не путался с синим "хромом" десктопной сборки. */
    {16,  18,  24},   /* 32 UI_BG          — фон системы             */
    {30,  34,  44},   /* 33 UI_SURFACE     — карточки, панели         */
    {46,  52,  66},   /* 34 UI_SURFACE_2   — приподнятые элементы      */
    {0,   200, 190},  /* 35 UI_ACCENT      — акцент                    */
    {0,   140, 140},  /* 36 UI_ACCENT_DARK — низ градиента акцента     */
    {236, 240, 248},  /* 37 UI_TEXT        — основной текст            */
    {130, 140, 160},  /* 38 UI_TEXT_DIM    — второстепенный текст      */
    {60,  66,  82},   /* 39 UI_DIVIDER     — разделители               */
    {52,  58,  74},   /* 40 UI_KEY         — клавиша экранной клавиатуры */
    {96,  104, 126},  /* 41 UI_KEY_DOWN    — нажатая клавиша           */
    {240, 160, 60},   /* 42 UI_WARN                                     */
    {90,  210, 120},  /* 43 UI_OK                                       */
    {26,  36,  74},   /* 44 UI_GRAD_TOP    — обои: верх                 */
    {40,  70,  120},  /* 45 UI_GRAD_MID    — обои: середина             */
    {14,  20,  36},   /* 46 UI_GRAD_BOT    — обои: низ                  */
    {8,   9,   12},   /* 47 UI_SHADOW      — тень под карточками        */
};

static void refresh_xrgb(int index) {
    g_palette_xrgb[index] = ((uint32_t)g_palette[index].r << 16) |
                            ((uint32_t)g_palette[index].g <<  8) |
                            ((uint32_t)g_palette[index].b);
}

void palette_init(void) {
    for (int i = 0; i < PALETTE_FIXED_COLORS; i++) {
        g_palette[i] = g_fixed[i];
        refresh_xrgb(i);
    }
    for (int i = PALETTE_FIXED_COLORS; i < PALETTE_SIZE; i++) {
        g_palette[i].r = g_palette[i].g = g_palette[i].b = 0;
        refresh_xrgb(i);
    }
}

void palette_set_range(const uint8_t *rgb_255, int count, int start_index) {
    for (int i = 0; i < count; i++) {
        int idx = start_index + i;
        if (idx < 0 || idx >= PALETTE_SIZE) continue;

        g_palette[idx].r = rgb_255[i * 3 + 0];
        g_palette[idx].g = rgb_255[i * 3 + 1];
        g_palette[idx].b = rgb_255[i * 3 + 2];
        refresh_xrgb(idx);
    }
}
