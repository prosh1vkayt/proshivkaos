/* arch/arm64/early_con.c — печать текста прямо в кадр.
 *
 * ЗАЧЕМ ЕЩЁ ОДИН ВЫВОД, когда есть полосы. Полоса отвечает ровно на один
 * вопрос — дошли или нет, — и каждый следующий вопрос стоит отдельной
 * перезагрузки телефона и отдельной фотографии. Текст отвечает на все
 * сразу: одна фотография показывает и пройденные этапы, и найденное
 * железо, и, если что-то упало, полное описание сбоя с адресами.
 *
 * ПОЧЕМУ МИМО ГРАФИЧЕСКОГО СЛОЯ. Этот вывод нужен именно тогда, когда
 * графический слой не работает или до него ещё не дошло. Поэтому здесь
 * нет ни бэкбуфера, ни палитры, ни отсечения, ни gfxfb — только запись
 * пикселей по адресу кадра, который оставил загрузчик. Единственная
 * общая с системой вещь — растр шрифта, и тот только читается.
 *
 * Порядок байтов пикселя тот же, что и везде на этом аппарате: синий,
 * зелёный, красный. Название формата "r8g8b8" перечисляет разряды
 * значения, а не байты в памяти.
 */
#include "arm64.h"
#include "boards/board.h"

#ifdef CONFIG_EARLY_FB_MARKS

const uint8_t *font8x8_get_glyph(char c);   /* gui/font8x8.c */

/* Втрое — чтобы читалось на фотографии экрана с рук. При 1080 точках в
 * строке это 45 знаков, чего хватает для всех наших сообщений. */
#define SCALE     3
#define GLYPH_W   (8 * SCALE)
#define GLYPH_H   (8 * SCALE)
#define LINE_H    (GLYPH_H + 6)   /* просвет: строки вплотную читаются хуже */

/* Верхние строки отданы полосам из boot.S: они ставятся до любого C и
 * должны пережить очистку. */
#define TEXT_TOP  (4 * 64)

static int g_cx = 0;
static int g_cy = TEXT_TOP;

/* Пока текста нет, о ходе загрузки говорят полосы. Как только текст пошёл,
 * полосы надо прекратить: они рисуются по тем же строкам и затирают его.
 * Полосы из boot.S это не затрагивает — они выше и ставятся раньше. */
static int g_active = 0;

static void put_px(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= BOARD_FB_WIDTH || y >= BOARD_FB_HEIGHT) return;

    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)BOARD_FB_ADDR
                        + (long)y * BOARD_FB_STRIDE + (long)x * 3;
    p[0] = b;
    p[1] = g;
    p[2] = r;
}

/* Шрифт покрывает только заглавные — приводим к ним, как это делает
 * основной вывод в gui/hal_gfx.c. */
static char up(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static void draw_glyph(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t *glyph = font8x8_get_glyph(up(c));

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!((bits >> (7 - col)) & 1)) continue;

            /* Один знак растра — квадрат SCALE на SCALE точек. */
            for (int dy = 0; dy < SCALE; dy++)
                for (int dx = 0; dx < SCALE; dx++)
                    put_px(x + col * SCALE + dx, y + row * SCALE + dy, r, g, b);
        }
    }
}

/* Очистить область под текст. Полосы из boot.S сверху не трогаем: они
 * ставятся раньше всего и говорят, что загрузчик до нас доехал. */
int early_con_active(void) { return g_active; }

void early_con_init(void) {
    for (int y = TEXT_TOP; y < BOARD_FB_HEIGHT; y++) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)BOARD_FB_ADDR
                            + (long)y * BOARD_FB_STRIDE;
        for (long i = 0; i + 2 < BOARD_FB_STRIDE; i += 3) {
            p[i + 0] = 24;    /* тёмно-серый: текст поверх читается, а */
            p[i + 1] = 20;    /* чёрное поле сливалось бы с погасшим   */
            p[i + 2] = 16;    /* экраном и путало                      */
        }
    }
    g_cx = 0;
    g_cy = TEXT_TOP;
    g_active = 1;
}

void early_con_color(const char *s, uint8_t r, uint8_t g, uint8_t b) {
    while (*s) {
        if (*s == '\n') {
            g_cx = 0;
            g_cy += LINE_H;
        } else {
            if (g_cx + GLYPH_W > BOARD_FB_WIDTH) { g_cx = 0; g_cy += LINE_H; }
            if (g_cy + GLYPH_H <= BOARD_FB_HEIGHT)
                draw_glyph(g_cx, g_cy, *s, r, g, b);
            g_cx += GLYPH_W;
        }
        s++;
    }
}

void early_con_puts(const char *s) { early_con_color(s, 220, 220, 220); }

void early_con_hex(uint64_t v) {
    static const char d[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'X';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = d[(v >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
    early_con_puts(buf);
}

#endif /* CONFIG_EARLY_FB_MARKS */
