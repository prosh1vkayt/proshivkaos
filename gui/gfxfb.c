/* gui/gfxfb.c — реализация двойной буферизации и вывода кадра. */
#include <stddef.h>

#include "gfxfb.h"
#include "palette.h"

/* Обслуживание кэша нужно только на ARM64 (там дисплей читает наш буфер
 * из ОЗУ мимо кэша процессора). На x86 видеопамять отображена как
 * uncacheable самим чипсетом, и делать ничего не надо — поэтому здесь
 * слабые (weak) заглушки, которые ARM64-сборка перекрывает настоящими
 * реализациями из arch/arm64/mmu.c. Это единственный способ оставить
 * этот файл общим для обеих архитектур без #ifdef внутри него. */
__attribute__((weak)) void arch_dcache_clean(const void *addr, size_t len) {
    (void)addr; (void)len;
}

static volatile uint8_t *g_fb = 0;
static int g_width  = 0;
static int g_height = 0;
static int g_pitch  = 0;
static int g_format = GFXFB_FMT_PAL8;

static uint8_t g_backbuffer[GFXFB_MAX_PIXELS];

/* Строка кадра в обычной памяти и готовые три байта на каждый цвет.
 *
 * Собирать строку у себя, а переносить целиком — дешевле, чем писать в
 * память экрана по байту: та память некэшируемая, и каждое обращение
 * туда стоит на порядок дороже обращения к своей. */
static uint32_t g_line[(GFXFB_MAX_LINE_BYTES + 3) / 4];
static uint8_t  g_rgb24[PALETTE_SIZE][3];
static uint32_t g_rgb24_stamp = 0;

static void rgb24_table_sync(void) {
    /* Палитра меняется редко (тема оформления), поэтому таблица
       пересчитывается не каждый кадр, а когда изменился отпечаток. */
    uint32_t stamp = 0;
    for (int i = 0; i < PALETTE_SIZE; i++)
        stamp = stamp * 31u + g_palette_xrgb[i];

    if (stamp == g_rgb24_stamp) return;
    g_rgb24_stamp = stamp;

    for (int i = 0; i < PALETTE_SIZE; i++) {
        uint32_t rgb = g_palette_xrgb[i];
        g_rgb24[i][0] = (uint8_t)(rgb);         /* B */
        g_rgb24[i][1] = (uint8_t)(rgb >> 8);    /* G */
        g_rgb24[i][2] = (uint8_t)(rgb >> 16);   /* R */
    }
}

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
 * Теперь помнится охватывающий прямоугольник всего нарисованного, и
 * отдаётся только он. Подсветка клавиши стоит долю процента кадра.
 *
 * Прямоугольник намеренно ОДИН, а не список: список точнее, но требует
 * решать, когда объединять его куски, а когда отдавать по отдельности.
 * Для нашей отрисовки — фон, кнопки, текст — охватывающего достаточно, и
 * ошибиться в нём негде. */
static int g_dx0 = 0, g_dy0 = 0, g_dx1 = 0, g_dy1 = 0;

/* Кадр лежит в некэшируемой памяти. Тогда выталкивать кэш после
 * отрисовки не нужно — а обход шести мегабайт строками кэша стоил
 * столько же, сколько сама отрисовка. */
static int g_fb_uncached = 0;

static void dirty_none(void) { g_dx0 = g_dy0 = 0; g_dx1 = g_dy1 = 0; }

static void dirty_all(void) {
    g_dx0 = 0; g_dy0 = 0; g_dx1 = g_width; g_dy1 = g_height;
}

static inline void dirty_point(int x, int y) {
    if (g_dx1 == 0) {                      /* было пусто */
        g_dx0 = x; g_dy0 = y; g_dx1 = x + 1; g_dy1 = y + 1;
        return;
    }
    if (x < g_dx0)      g_dx0 = x;
    if (y < g_dy0)      g_dy0 = y;
    if (x + 1 > g_dx1)  g_dx1 = x + 1;
    if (y + 1 > g_dy1)  g_dy1 = y + 1;
}

static void dirty_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    dirty_point(x, y);
    dirty_point(x + w - 1, y + h - 1);
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

uint8_t *gfxfb_backbuffer(void) { return g_backbuffer; }

void gfxfb_put_pixel(int x, int y, uint8_t color) {
    if (x < 0 || y < 0 || x >= g_width || y >= g_height) return;
    if (g_clip_w > 0 &&
        (x < g_clip_x || y < g_clip_y ||
         x >= g_clip_x + g_clip_w || y >= g_clip_y + g_clip_h))
        return;
    g_backbuffer[y * g_width + x] = color;
    dirty_point(x, y);
}

/* Залить прямоугольник разом.
 *
 * Раньше заливка шла через gfxfb_put_pixel по одному пикселю, то есть на
 * каждый пиксель приходились две проверки границ, проверка отсечения и
 * пересчёт грязной области. На заливке фона целого экрана это два
 * миллиона таких проходов там, где достаточно тысячи девятисот двадцати
 * заполнений строк. */
/* Заполнить одинаковым байтом, по восемь байт за раз.
 *
 * Бэкбуфер — обычная кэшируемая память, и побайтная запись в неё стоит
 * ровно в восемь раз дороже, чем нужно. Заливка целого экрана это два
 * миллиона байт: разница в четверть миллиона обращений против двух
 * миллионов заметна даже на фоне отдачи кадра. */
static void fill_run(uint8_t *p, int n, uint8_t color) {
    /* Хвост до выравнивания — побайтно. */
    while (n > 0 && (((uintptr_t)p) & 7)) { *p++ = color; n--; }

    uint64_t eight = (uint64_t)color;
    eight |= eight << 8;  eight |= eight << 16;  eight |= eight << 32;

    while (n >= 8) { *(uint64_t *)p = eight; p += 8; n -= 8; }
    while (n-- > 0) *p++ = color;
}

static void fill_run_rows(int x0, int y0, int w, int h, uint8_t color) {
    for (int j = 0; j < h; j++)
        fill_run(&g_backbuffer[(long)(y0 + j) * g_width + x0], w, color);
}

void gfxfb_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;

    /* Обрезаем по экрану и по отсечению — один раз на весь прямоугольник,
       а не на каждый пиксель. */
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (g_clip_w > 0) {
        if (x0 < g_clip_x) x0 = g_clip_x;
        if (y0 < g_clip_y) y0 = g_clip_y;
        if (x1 > g_clip_x + g_clip_w) x1 = g_clip_x + g_clip_w;
        if (y1 > g_clip_y + g_clip_h) y1 = g_clip_y + g_clip_h;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_width)  x1 = g_width;
    if (y1 > g_height) y1 = g_height;
    if (x0 >= x1 || y0 >= y1) return;

    fill_run_rows(x0, y0, x1 - x0, y1 - y0, color);

    dirty_rect(x0, y0, x1 - x0, y1 - y0);
}

void gfxfb_clear(uint8_t color) {
    fill_run(g_backbuffer, (int)((long)g_width * (long)g_height), color);
    dirty_all();
}

void gfxfb_present(void) {
    if (!g_fb) return;

    /* Отдаём только то, что нарисовали. Пусто — значит и отдавать нечего:
       это самый частый случай в главном цикле, и он должен быть
       бесплатным. */
    int x0 = g_dx0, y0 = g_dy0, x1 = g_dx1, y1 = g_dy1;
    if (x0 >= x1 || y0 >= y1) return;

    rgb24_table_sync();

    if (g_format == GFXFB_FMT_RGB24) {
        /* Три байта на пиксель, без выравнивающего байта — именно так
           лежит непрерывный сплеш-фреймбуфер, который оставляет LK на
           Xiaomi-телефонах (см. arch/arm64/boards/mido.h).

           ПОРЯДОК БАЙТОВ ОБРАТНЫЙ ИМЕНИ ФОРМАТА: сначала синий, потом
           зелёный, потом красный. Название "r8g8b8" перечисляет разряды
           значения от старшего к младшему, а при обратном порядке байтов
           старший разряд ложится в памяти последним. Проверено на живом
           устройстве: полосы R,G,B выходили синей и красной наоборот.

           Начало строки выравниваем по четырём пикселям: двенадцать байт
           кратны четырём, значит перенос идёт словами без хвоста. */
        x0 &= ~3;
        x1 = (x1 + 3) & ~3;
        if (x1 > g_width) x1 = g_width;

        int span = x1 - x0;
        int bytes = span * 3;
        int words = bytes >> 2;

        for (int y = y0; y < y1; y++) {
            uint8_t *line = (uint8_t *)g_line;
            const uint8_t *src = &g_backbuffer[(long)y * g_width + x0];

            for (int x = 0; x < span; x++) {
                const uint8_t *c = g_rgb24[src[x]];
                line[x * 3 + 0] = c[0];
                line[x * 3 + 1] = c[1];
                line[x * 3 + 2] = c[2];
            }

            volatile uint8_t *dst = g_fb + (long)y * g_pitch + (long)x0 * 3;
            if ((((uintptr_t)dst) & 3) == 0) {
                volatile uint32_t *d32 = (volatile uint32_t *)dst;
                for (int i = 0; i < words; i++) d32[i] = g_line[i];
                for (int i = words << 2; i < bytes; i++) dst[i] = line[i];
            } else {
                for (int i = 0; i < bytes; i++) dst[i] = line[i];
            }
        }
    } else if (g_format == GFXFB_FMT_PAL8) {
        /* Видеокарта сама раскрасит индексы через DAC — копируем как есть. */
        for (int y = y0; y < y1; y++) {
            volatile uint8_t *dst = g_fb + (long)y * g_pitch + x0;
            const uint8_t *src = &g_backbuffer[(long)y * g_width + x0];
            for (int x = 0; x < x1 - x0; x++) dst[x] = src[x];
        }
    } else {
        /* Аппаратной палитры нет — раскрашиваем сами по таблице. */
        for (int y = y0; y < y1; y++) {
            volatile uint32_t *dst =
                (volatile uint32_t *)(g_fb + (long)y * g_pitch) + x0;
            const uint8_t *src = &g_backbuffer[(long)y * g_width + x0];
            for (int x = 0; x < x1 - x0; x++) dst[x] = g_palette_xrgb[src[x]];
        }
    }

    /* Вытолкнуть кадр из кэша процессора в ОЗУ: контроллер дисплея
       (или QEMU в роли такового) читает буфер напрямую и про наш кэш
       ничего не знает.
     *
     * НО ТОЛЬКО ЕСЛИ КАДР ВООБЩЕ КЭШИРУЕТСЯ. На телефоне он лежит в
     * некэшируемой памяти, и обход шести мегабайт строками кэша был
     * чистой потерей — по времени вровень с самой отрисовкой. */
    if (!g_fb_uncached) {
        arch_dcache_clean((const void *)(g_fb + (long)y0 * g_pitch),
                          (size_t)g_pitch * (size_t)(y1 - y0));
    }

    dirty_none();
}

