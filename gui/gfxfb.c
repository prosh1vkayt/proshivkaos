/* gui/gfxfb.c — двойная буферизация и вывод кадра.
 *
 * ИСТИННЫЙ ЦВЕТ. Бэкбуфер хранит пиксели как 0x00RRGGBB, по четыре байта,
 * а не индексы палитры. Восьмибитная палитра давала 256 цветов на всё: любой
 * градиент набирался дизерингом, «через пиксель», и интерфейс выглядел
 * пиксельным при любом разрешении. Теперь градиенты гладкие, края
 * сглаживаются, есть полупрозрачность — то, без чего не бывает интерфейса
 * уровня Android.
 *
 * Цвета темы (GFX_UI_* и прочие индексы) по-прежнему работают: индекс
 * превращается в RGB в момент рисования. Приложения менять не нужно.
 *
 * Формат видеопамяти — дело вывода кадра: RGB24 у телефона, XRGB32 у
 * эмулятора, индексы палитры у старого VGA на x86 (для него цвет подбирается
 * по таблице ближайших).
 */
#include <stddef.h>

#include "gfxfb.h"
#include "palette.h"
#include "hal_time.h"
#include "hal.h"

__attribute__((weak)) void arch_dcache_clean(const void *addr, size_t len) {
    (void)addr; (void)len;
}

static uint32_t g_last_present_us = 0, g_last_present_px = 0;

void gfxfb_last_present(uint32_t *us, uint32_t *pixels) {
    *us = g_last_present_us;
    *pixels = g_last_present_px;
}

/* С какой площади задание стоит раздавать ядрам. Ниже — дешевле сделать
   самому: строка состояния с часами (70 тысяч пикселей раз в секунду)
   при прежнем пороге в 60 тысяч будила спящие ядра каждую секунду, и
   телефон не остывал. */
#define PARALLEL_MIN_PIXELS 300000L

static volatile uint8_t *g_fb = 0;
static int g_width  = 0;
static int g_height = 0;
static int g_pitch  = 0;
static int g_format = GFXFB_FMT_PAL8;

static uint32_t g_back[GFXFB_MAX_PIXELS];

/* Прямоугольник отсечения. Нулевая ширина означает "отсечения нет". */
static int g_clip_x = 0, g_clip_y = 0, g_clip_w = 0, g_clip_h = 0;

/* ЧТО ИЗМЕНИЛОСЬ С ПРОШЛОГО КАДРА.
 *
 * Раньше кадр отдавался целиком всегда. На телефоне это шесть мегабайт
 * записей в память экрана — и не в обычную, а в память устройства, где
 * каждая запись идёт строго по порядку и ничего не объединяется. Выходило
 * порядка ста миллисекунд на кадр. Пока этого не было видно, всё казалось
 * "медленным вообще"; а стоило начать печатать на экранной клавиатуре —
 * и каждое подсвечивание клавиши обходилось в целый кадр.
 *
 * Теперь помнится, что нарисовано, и отдаётся только это.
 *
 * НЕСКОЛЬКО ПРЯМОУГОЛЬНИКОВ, А НЕ ОДИН. Сперва помнился один охватывающий
 * — и на экранной клавиатуре он оказался худшим случаем: нажатие меняет
 * подсветку клавиши внизу экрана и клетку терминала вверху, а охватывающий
 * прямоугольник двух далёких точек — почти весь экран. Каждое нажатие
 * снова стоило целого кадра.
 *
 * Теперь их до четырёх. Близкие и пересекающиеся сливаются (иначе рядом
 * стоящие буквы строки дали бы по прямоугольнику на каждую), а когда
 * места нет — новый сливается с тем, чей охват вырастет меньше всего.
 * Отдать лишнее не страшно; не отдать нарисованное нельзя — поэтому
 * слияние всегда только расширяет. */
#define DIRTY_MAX   4
#define DIRTY_NEAR  24          /* ближе этого — один прямоугольник */

typedef struct { int x0, y0, x1, y1; } drect_t;

static drect_t g_dirty[DIRTY_MAX];
static int     g_ndirty = 0;

/* Кадр лежит в некэшируемой памяти. Тогда выталкивать кэш после
 * отрисовки не нужно — а обход шести мегабайт строками кэша стоил
 * столько же, сколько сама отрисовка. */
static int g_fb_uncached = 0;

static void dirty_none(void) { g_ndirty = 0; }

static void dirty_all(void) {
    g_dirty[0].x0 = 0; g_dirty[0].y0 = 0;
    g_dirty[0].x1 = g_width; g_dirty[0].y1 = g_height;
    g_ndirty = 1;
}

static long drect_area(const drect_t *r) {
    return (long)(r->x1 - r->x0) * (long)(r->y1 - r->y0);
}

static void drect_unite(drect_t *a, const drect_t *b) {
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

static int drect_near(const drect_t *a, const drect_t *b) {
    return a->x0 <= b->x1 + DIRTY_NEAR && b->x0 <= a->x1 + DIRTY_NEAR &&
           a->y0 <= b->y1 + DIRTY_NEAR && b->y0 <= a->y1 + DIRTY_NEAR;
}

static void dirty_add(int x0, int y0, int x1, int y1) {
    drect_t n = { x0, y0, x1, y1 };

    for (int i = 0; i < g_ndirty; i++) {
        const drect_t *r = &g_dirty[i];
        if (r->x0 <= x0 && r->y0 <= y0 && r->x1 >= x1 && r->y1 >= y1)
            return;                             /* уже помечено */
    }

    /* Сливаем со всеми близкими — слияние может сделать близким и
       следующий, поэтому до тех пор, пока есть что сливать. */
    for (int again = 1; again; ) {
        again = 0;
        for (int i = 0; i < g_ndirty; i++) {
            if (drect_near(&g_dirty[i], &n)) {
                drect_unite(&n, &g_dirty[i]);
                g_dirty[i] = g_dirty[--g_ndirty];
                again = 1;
                break;
            }
        }
    }

    if (g_ndirty < DIRTY_MAX) { g_dirty[g_ndirty++] = n; return; }

    int best = 0; long best_growth = 0;
    for (int i = 0; i < g_ndirty; i++) {
        drect_t u = g_dirty[i];
        drect_unite(&u, &n);
        long growth = drect_area(&u) - drect_area(&g_dirty[i]);
        if (i == 0 || growth < best_growth) { best = i; best_growth = growth; }
    }
    drect_unite(&g_dirty[best], &n);
}

static inline void dirty_point(int x, int y) {
    dirty_add(x, y, x + 1, y + 1);
}

static void dirty_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    dirty_add(x, y, x + w, y + h);
}


void gfxfb_mark_all_dirty(void) { dirty_all(); }
void gfxfb_set_uncached(int on) { g_fb_uncached = on ? 1 : 0; }

void gfxfb_set_clip(int x, int y, int w, int h) {
    g_clip_x = x; g_clip_y = y;
    g_clip_w = w; g_clip_h = h;
}

void gfxfb_reset_clip(void) {
    g_clip_w = 0;
    g_clip_h = 0;
}

void gfxfb_bind(volatile void *fb, int width, int height, int pitch_bytes, int fmt) {
    g_fb     = (volatile uint8_t *)fb;
    g_width  = width;
    g_height = height;
    g_pitch  = pitch_bytes;
    g_format = fmt;
    dirty_all();          /* новый экран отдаём целиком */
}

int gfxfb_width(void)  { return g_width; }
int gfxfb_height(void) { return g_height; }

uint32_t *gfxfb_backbuffer32(void) { return g_back; }

/* ---------------- Отсечение ---------------- */

/* Обрезать прямоугольник по экрану и по отсечению. 0 — ничего не осталось. */
static int clip_rect(int *x0, int *y0, int *x1, int *y1) {
    if (g_clip_w > 0) {
        if (*x0 < g_clip_x) *x0 = g_clip_x;
        if (*y0 < g_clip_y) *y0 = g_clip_y;
        if (*x1 > g_clip_x + g_clip_w) *x1 = g_clip_x + g_clip_w;
        if (*y1 > g_clip_y + g_clip_h) *y1 = g_clip_y + g_clip_h;
    }
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > g_width)  *x1 = g_width;
    if (*y1 > g_height) *y1 = g_height;
    return *x0 < *x1 && *y0 < *y1;
}

static inline int point_visible(int x, int y) {
    if (x < 0 || y < 0 || x >= g_width || y >= g_height) return 0;
    if (g_clip_w > 0 &&
        (x < g_clip_x || y < g_clip_y ||
         x >= g_clip_x + g_clip_w || y >= g_clip_y + g_clip_h))
        return 0;
    return 1;
}

/* ---------------- Цвет ---------------- */

/* Смешать src поверх dst с непрозрачностью a (0..256). Красный и синий
 * считаются одним умножением — они лежат в разных байтах и не мешают друг
 * другу, — зелёный отдельно. */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t na = 256 - a;
    uint32_t rb = (((src & 0xFF00FFu) * a + (dst & 0xFF00FFu) * na) >> 8) & 0xFF00FFu;
    uint32_t g  = (((src & 0x00FF00u) * a + (dst & 0x00FF00u) * na) >> 8) & 0x00FF00u;
    return rb | g;
}

static inline uint32_t alpha256(uint32_t a255) { return a255 + (a255 >> 7); }

/* Залить n слов одним значением — по восемь байт за раз. */
static void fill32(uint32_t *p, int n, uint32_t v) {
    if (n <= 0) return;
    if (((uintptr_t)p & 4) != 0) { *p++ = v; n--; }
    uint64_t pat = (uint64_t)v | ((uint64_t)v << 32);
    while (n >= 2) { *(uint64_t *)(void *)p = pat; p += 2; n -= 2; }
    if (n) *p = v;
}

/* ---------------- Рисование ---------------- */

void gfxfb_put_rgb(int x, int y, uint32_t rgb) {
    if (!point_visible(x, y)) return;
    g_back[(long)y * g_width + x] = rgb;
    dirty_point(x, y);
}

void gfxfb_put_pixel(int x, int y, uint8_t color) {
    gfxfb_put_rgb(x, y, g_palette_xrgb[color]);
}

void gfxfb_blend_pixel(int x, int y, uint32_t rgb, uint32_t alpha255) {
    if (alpha255 == 0 || !point_visible(x, y)) return;
    uint32_t *p = &g_back[(long)y * g_width + x];
    *p = (alpha255 >= 255) ? rgb : blend(*p, rgb, alpha256(alpha255));
    dirty_point(x, y);
}

typedef struct { int x0, x1, y0, y1; uint32_t rgb, a; } fill_job_t;

static void fill_band(void *arg, int part, int parts) {
    const fill_job_t *f = (const fill_job_t *)arg;
    int h = f->y1 - f->y0;
    int a = f->y0 + h * part / parts, b = f->y0 + h * (part + 1) / parts;
    for (int j = a; j < b; j++)
        fill32(&g_back[(long)j * g_width + f->x0], f->x1 - f->x0, f->rgb);
}

/* Для фигур, которые отмечают грязную область сами, один раз на всю фигуру:
   отмечать её на каждый пиксель края дороже самого смешивания. */
void gfxfb_blend_pixel_nodirty(int x, int y, uint32_t rgb, uint32_t alpha255) {
    if (alpha255 == 0 || !point_visible(x, y)) return;
    uint32_t *p = &g_back[(long)y * g_width + x];
    *p = (alpha255 >= 255) ? rgb : blend(*p, rgb, alpha256(alpha255));
}

void gfxfb_blend_span_nodirty(int x, int y, int w, uint32_t rgb, uint32_t alpha255) {
    if (w <= 0 || alpha255 == 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + 1;
    if (!clip_rect(&x0, &y0, &x1, &y1)) return;
    uint32_t *p = &g_back[(long)y * g_width];
    if (alpha255 >= 255) { fill32(p + x0, x1 - x0, rgb); return; }
    uint32_t a = alpha256(alpha255);
    for (int i = x0; i < x1; i++) p[i] = blend(p[i], rgb, a);
}

/* Строка пикселей с покрытием: пиксель x + i*dir получает долю cov[i] из
   cov_full. Отсечение — один раз на строку. */
void gfxfb_blend_cov_row(int x, int y, const uint16_t *cov, int n, int dir,
                         uint32_t rgb, uint32_t alpha255, uint32_t cov_full) {
    if (n <= 0 || alpha255 == 0) return;
    if (y < 0 || y >= g_height) return;
    if (g_clip_w > 0 && (y < g_clip_y || y >= g_clip_y + g_clip_h)) return;
    int lo = 0, hi = g_width;
    if (g_clip_w > 0) {
        if (g_clip_x > lo) lo = g_clip_x;
        if (g_clip_x + g_clip_w < hi) hi = g_clip_x + g_clip_w;
    }
    uint32_t *row = &g_back[(long)y * g_width];
    uint32_t mul = alpha255 + (alpha255 >> 7);          /* 0..256 */
    for (int i = 0; i < n; i++) {
        int px = x + i * dir;
        if (px < lo || px >= hi) continue;
        uint32_t a = (uint32_t)((uint64_t)cov[i] * mul / cov_full);   /* 0..256 */
        if (a == 0) continue;
        row[px] = (a >= 256) ? rgb : blend(row[px], rgb, a);
    }
}

void gfxfb_mark_dirty(int x, int y, int w, int h) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_rect(&x0, &y0, &x1, &y1)) return;
    dirty_rect(x0, y0, x1 - x0, y1 - y0);
}

void gfxfb_fill_rect_rgb(int x, int y, int w, int h, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_rect(&x0, &y0, &x1, &y1)) return;
    fill_job_t job = { x0, x1, y0, y1, rgb, 0 };
    if ((long)(x1 - x0) * (y1 - y0) >= PARALLEL_MIN_PIXELS) hal_parallel(fill_band, &job);
    else fill_band(&job, 0, 1);
    dirty_rect(x0, y0, x1 - x0, y1 - y0);
}

void gfxfb_fill_rect(int x, int y, int w, int h, uint8_t color) {
    gfxfb_fill_rect_rgb(x, y, w, h, g_palette_xrgb[color]);
}

void gfxfb_blend_rect_rgb(int x, int y, int w, int h, uint32_t rgb, uint32_t alpha255) {
    if (w <= 0 || h <= 0 || alpha255 == 0) return;
    if (alpha255 >= 255) { gfxfb_fill_rect_rgb(x, y, w, h, rgb); return; }
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_rect(&x0, &y0, &x1, &y1)) return;
    uint32_t a = alpha256(alpha255);
    for (int j = y0; j < y1; j++) {
        uint32_t *p = &g_back[(long)j * g_width];
        for (int i = x0; i < x1; i++) p[i] = blend(p[i], rgb, a);
    }
    dirty_rect(x0, y0, x1 - x0, y1 - y0);
}

/* Отрезок строки с непрозрачностью — основа всех сглаженных фигур. */
void gfxfb_span_rgb(int x, int y, int w, uint32_t rgb, uint32_t alpha255) {
    if (alpha255 >= 255) gfxfb_fill_rect_rgb(x, y, w, 1, rgb);
    else                 gfxfb_blend_rect_rgb(x, y, w, 1, rgb, alpha255);
}

/* Наложить маску покрытия (0..255) цветом rgb — так рисуются сглаженные
 * буквы и иконки, отрендеренные заранее. */
void gfxfb_blit_alpha8(int x, int y, int w, int h, const uint8_t *mask, int stride,
                       uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_rect(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++) {
        const uint8_t *m = &mask[(long)(j - y) * stride + (x0 - x)];
        uint32_t *p = &g_back[(long)j * g_width + x0];
        for (int i = 0; i < x1 - x0; i++) {
            uint32_t a = m[i];
            if (a == 0) continue;
            p[i] = (a == 255) ? rgb : blend(p[i], rgb, alpha256(a));
        }
    }
    dirty_rect(x0, y0, x1 - x0, y1 - y0);
}

/* Скопировать готовые пиксели 0x00RRGGBB. */
void gfxfb_blit_rgb(int x, int y, int w, int h, const uint32_t *src, int stride) {
    if (w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!clip_rect(&x0, &y0, &x1, &y1)) return;
    for (int j = y0; j < y1; j++) {
        const uint32_t *s = &src[(long)(j - y) * stride + (x0 - x)];
        uint32_t *p = &g_back[(long)j * g_width + x0];
        for (int i = 0; i < x1 - x0; i++) p[i] = s[i];
    }
    dirty_rect(x0, y0, x1 - x0, y1 - y0);
}

void gfxfb_clear(uint8_t color) {
    fill32(g_back, (int)((long)g_width * (long)g_height), g_palette_xrgb[color]);
    dirty_all();
}

/* ---------------- Вывод кадра ---------------- */

/* Для индексной видеопамяти: ближайший цвет палитры по 15-битному RGB.
 * Пересчитывается, когда меняется палитра (обои на x86 её переписывают). */
static uint8_t  g_quant[32768];
static uint32_t g_quant_stamp = 1;

static void quant_sync(void) {
    uint32_t stamp = 0;
    for (int i = 0; i < PALETTE_SIZE; i++)
        stamp = stamp * 31u + g_palette_xrgb[i];
    if (stamp == g_quant_stamp) return;
    g_quant_stamp = stamp;

    for (int q = 0; q < 32768; q++) {
        int r = ((q >> 10) & 31) * 255 / 31;
        int g = ((q >> 5) & 31) * 255 / 31;
        int b = (q & 31) * 255 / 31;
        int best = 0, best_d = 0x7FFFFFFF;
        for (int i = 0; i < PALETTE_SIZE; i++) {
            int dr = r - g_palette[i].r, dg = g - g_palette[i].g, db = b - g_palette[i].b;
            int d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best_d = d; best = i; if (!d) break; }
        }
        g_quant[q] = (uint8_t)best;
    }
}

/* Вывод полосы строк [y0, y1). Полосы независимы — каждая пишет свои
   строки памяти экрана, — поэтому их считают разные ядра одновременно. */
static void present_rows_rgb24(int x0, int x1, int y0, int y1) {
    uint32_t line_words[(GFXFB_MAX_LINE_BYTES + 3) / 4 + 1];
    uint8_t *line = (uint8_t *)line_words;
    int span  = x1 - x0;
    int bytes = span * 3;
    int words = bytes >> 2;

    for (int y = y0; y < y1; y++) {
        const uint32_t *src = &g_back[(long)y * g_width + x0];
        uint8_t *d = line;
        for (int x = 0; x < span; x++, d += 3) {
            uint32_t v = src[x];
            d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16);
        }

        volatile uint8_t *dst = g_fb + (long)y * g_pitch + (long)x0 * 3;
        if ((((uintptr_t)dst) & 3) == 0) {
            volatile uint32_t *d32 = (volatile uint32_t *)dst;
            for (int i = 0; i < words; i++) d32[i] = line_words[i];
            for (int i = words << 2; i < bytes; i++) dst[i] = line[i];
        } else {
            for (int i = 0; i < bytes; i++) dst[i] = line[i];
        }
    }
}

typedef struct { int x0, x1, y0, y1; } band_job_t;

static void band_bounds(const band_job_t *j, int part, int parts, int *a, int *b) {
    int h = j->y1 - j->y0;
    *a = j->y0 + h * part / parts;
    *b = j->y0 + h * (part + 1) / parts;
}

static void present_band(void *arg, int part, int parts) {
    const band_job_t *j = (const band_job_t *)arg;
    int a, b;
    band_bounds(j, part, parts, &a, &b);
    present_rows_rgb24(j->x0, j->x1, a, b);
}

static void present_band_xrgb(void *arg, int part, int parts) {
    const band_job_t *j = (const band_job_t *)arg;
    int a, b;
    band_bounds(j, part, parts, &a, &b);
    for (int y = a; y < b; y++) {
        volatile uint32_t *dst = (volatile uint32_t *)(g_fb + (long)y * g_pitch) + j->x0;
        const uint32_t *src = &g_back[(long)y * g_width + j->x0];
        for (int x = 0; x < j->x1 - j->x0; x++) dst[x] = src[x];
    }
}

static void present_rect(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_width)  x1 = g_width;
    if (y1 > g_height) y1 = g_height;
    if (x0 >= x1 || y0 >= y1) return;

    if (g_format == GFXFB_FMT_RGB24) {
        /* Три байта на пиксель: синий, зелёный, красный — ровно младшие
           три байта значения 0x00RRGGBB. Проверено на живом устройстве.

           Начало выравнено на четыре пикселя — тогда перенос в память
           экрана идёт целыми словами. Мелкое (буква, подсветка клавиши)
           выводится сразу, крупное — полосами на всех ядрах. */
        x0 &= ~3;
        x1 = (x1 + 3) & ~3;
        if (x1 > g_width) x1 = g_width;

        if ((long)(x1 - x0) * (y1 - y0) < PARALLEL_MIN_PIXELS) {
            present_rows_rgb24(x0, x1, y0, y1);
        } else {
            band_job_t job = { x0, x1, y0, y1 };
            hal_parallel(present_band, &job);
        }
    } else if (g_format == GFXFB_FMT_XRGB32) {
        band_job_t job = { x0, x1, y0, y1 };
        if ((long)(x1 - x0) * (y1 - y0) < PARALLEL_MIN_PIXELS) present_band_xrgb(&job, 0, 1);
        else hal_parallel(present_band_xrgb, &job);
    } else {
        quant_sync();
        for (int y = y0; y < y1; y++) {
            volatile uint8_t *dst = g_fb + (long)y * g_pitch + x0;
            const uint32_t *src = &g_back[(long)y * g_width + x0];
            for (int x = 0; x < x1 - x0; x++) {
                uint32_t v = src[x];
                dst[x] = g_quant[((v >> 9) & 0x7C00) | ((v >> 6) & 0x3E0) | ((v >> 3) & 0x1F)];
            }
        }
    }

    if (!g_fb_uncached) {
        arch_dcache_clean((const void *)(g_fb + (long)y0 * g_pitch),
                          (size_t)g_pitch * (size_t)(y1 - y0));
    }
}

void gfxfb_present(void) {
    if (!g_fb) return;
    if (g_ndirty == 0) return;

    uint64_t t0 = hal_time_us();
    uint32_t px = 0;

    for (int i = 0; i < g_ndirty; i++) {
        present_rect(g_dirty[i].x0, g_dirty[i].y0, g_dirty[i].x1, g_dirty[i].y1);
        px += (uint32_t)drect_area(&g_dirty[i]);
    }

    dirty_none();
    g_last_present_us = (uint32_t)(hal_time_us() - t0);
    g_last_present_px = px;
}
