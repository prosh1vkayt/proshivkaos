/* arch/arm64/boot_anim.c — то, что видно, пока система поднимается.
 *
 * ЗАЧЕМ. До сих пор при загрузке на экран шёл текстовый журнал: полсотни
 * строк про такты, шины, источники питания и состояния каналов. Для
 * отладки это было единственным окном внутрь — а как только система
 * заработала, оно превратилось в помеху. И не только внешнюю: каждая
 * строка это отрисовка знаков по всему экрану, то есть время, отнятое у
 * самой загрузки.
 *
 * Теперь журнал уходит туда, где он и нужен — в провод и в сохраняемую
 * область, — а на экране показывается ход загрузки. В отладочной сборке
 * (CONFIG_DEBUG_TOUCH_PROBE) всё наоборот: текст на экране, анимации
 * нет, потому что одно поверх другого нечитаемо.
 *
 * ПОЧЕМУ БЕЗ БУКВ. Рисовать надпись значило бы завести здесь второй
 * шрифтовой вывод — а он уже есть в early_con.c и в gui/. Ни тот ни
 * другой тут не годятся: первый пишет строки сверху вниз, второй ещё не
 * поднят. Поэтому знак чисто геометрический: рамка со вложенным
 * квадратом и полоса хода загрузки. Зато рисуется это почти бесплатно и
 * работает с самого первого шага, когда ничего кроме адреса кадра ещё
 * нет.
 *
 * Пишем прямо в память экрана, как early_fb.c: gfxfb на этом этапе ещё
 * не привязан, а ждать его — значит не показать ничего в самый долгий
 * отрезок загрузки.
 */
#include "arm64.h"
#include "boards/board.h"
#include "hal_time.h"

#ifdef BOARD_HAS_STATIC_FB

/* Цвета. Порядок байтов в памяти экрана обратный имени формата: синий,
 * зелёный, красный (см. gui/gfxfb.c). */
#define BG_R   14
#define BG_G   14
#define BG_B   18

#define FG_R   70
#define FG_G   150
#define FG_B   255

#define DIM_R  40
#define DIM_G  44
#define DIM_B  56

#define BAR_W_PCT   56          /* ширина полосы в процентах экрана */
#define BAR_H       (BOARD_FB_HEIGHT / 240)
#define MARK_SIZE   (BOARD_FB_WIDTH / 5)

static int g_active = 0;
static int g_done = 0, g_total = 1;

static void fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BOARD_FB_WIDTH)  w = BOARD_FB_WIDTH - x;
    if (y + h > BOARD_FB_HEIGHT) h = BOARD_FB_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)BOARD_FB_ADDR
                            + (long)(y + j) * BOARD_FB_STRIDE + (long)x * 3;
        for (int i = 0; i < w; i++) {
            p[i * 3 + 0] = b;
            p[i * 3 + 1] = g;
            p[i * 3 + 2] = r;
        }
    }
}

static int bar_x(void) { return BOARD_FB_WIDTH * (100 - BAR_W_PCT) / 200; }
static int bar_w(void) { return BOARD_FB_WIDTH * BAR_W_PCT / 100; }
static int bar_y(void) { return BOARD_FB_HEIGHT * 62 / 100; }

int boot_anim_active(void) { return g_active; }

void boot_anim_begin(void) {
    fill(0, 0, BOARD_FB_WIDTH, BOARD_FB_HEIGHT, BG_R, BG_G, BG_B);

    /* Знак: рамка со вложенным квадратом. Толщина рамки — двадцатая часть
       её размера, чтобы на любом разрешении выглядело одинаково. */
    int s = MARK_SIZE;
    int t = s / 12;
    if (t < 2) t = 2;
    int mx = (BOARD_FB_WIDTH - s) / 2;
    int my = BOARD_FB_HEIGHT * 38 / 100 - s / 2;

    fill(mx, my, s, t, FG_R, FG_G, FG_B);
    fill(mx, my + s - t, s, t, FG_R, FG_G, FG_B);
    fill(mx, my, t, s, FG_R, FG_G, FG_B);
    fill(mx + s - t, my, t, s, FG_R, FG_G, FG_B);
    fill(mx + s / 3, my + s / 3, s / 3, s / 3, FG_R, FG_G, FG_B);

    /* Ложе полосы — сразу целиком, чтобы было видно, сколько осталось. */
    fill(bar_x(), bar_y(), bar_w(), BAR_H, DIM_R, DIM_G, DIM_B);

    g_active = 1;
    g_done = 0;
    g_total = 1;
}

/* Сколько шагов пройдено из скольких. Полоса заполняется слева. */
void boot_anim_stage(int done, int total) {
    if (!g_active || total <= 0) return;
    if (done < 0) done = 0;
    if (done > total) done = total;

    g_done = done;
    g_total = total;

    fill(bar_x(), bar_y(), bar_w() * done / total, BAR_H, FG_R, FG_G, FG_B);
}

/* Один шаг вперёд.
 *
 * Общее число шагов задано здесь, а не считается: точное количество
 * зависит от платы и от того, что на ней нашлось, а полоса должна ползти
 * равномерно. Если шагов окажется меньше, полоса просто не дойдёт до
 * конца — и это честнее, чем дёргаться рывками. */
#define BOOT_STEPS_TOTAL 12
static int g_step = 0;

void boot_anim_step(void) {
    if (!g_active) return;
    if (g_step < BOOT_STEPS_TOTAL) g_step++;
    boot_anim_stage(g_step, BOOT_STEPS_TOTAL);
}

/* Бегущий блик — чтобы было видно, что система работает, а не встала.
 *
 * Вызывается из фоновой прокрутки, то есть из каждого ожидания в
 * системе. Рисует ровно один небольшой прямоугольник, поэтому его можно
 * звать сколь угодно часто. */
#define TICK_INTERVAL_MS 33     /* тридцать кадров в секунду — глазу хватает */

void boot_anim_tick(void) {
    if (!g_active) return;

    /* НЕ ЧАЩЕ ТРИДЦАТИ РАЗ В СЕКУНДУ.
     *
     * Вызывают нас из фоновой прокрутки, то есть из КАЖДОГО оборота
     * каждого ожидания в системе — тысячи раз в секунду. Каждый такой
     * оборот перерисовывал полосу целиком, а это четырнадцать килобайт
     * в память экрана. Выходили сотни мегабайт в секунду на шину,
     * которой они не нужны: загрузка умирала через несколько секунд.
     *
     * Ограничение по часам, а не по счётчику вызовов: вызывают
     * неравномерно, и от счётчика блик дёргался бы. */
    static uint64_t last = 0;
    uint64_t now = hal_time_ms();
    if (now - last < TICK_INTERVAL_MS) return;
    last = now;

    int filled = bar_w() * g_done / g_total;
    int rest = bar_w() - filled;
    if (rest <= 0) return;

    int gw = bar_w() / 12;
    if (gw < 4) return;

    /* Положение блика — от времени, а не от счётчика вызовов: вызывают
       нас неравномерно, и от счётчика блик дёргался бы. */
    int period = 1200;
    int phase = (int)(hal_time_ms() % (uint64_t)period);
    int gx = bar_x() + filled + (rest - gw) * phase / period;

    /* Стираем ложе позади и рисуем блик на новом месте. Полосу целиком
       не перерисовываем: это и есть та экономия, ради которой всё
       затевалось. */
    fill(bar_x() + filled, bar_y(), rest, BAR_H, DIM_R, DIM_G, DIM_B);
    fill(gx, bar_y(), gw, BAR_H, FG_R / 2, FG_G / 2, FG_B / 2);
}

void boot_anim_end(void) {
    if (!g_active) return;
    g_active = 0;
    /* Экран не гасим: сразу за этим система рисует первый кадр, и лишняя
       заливка была бы видна как вспышка. */
}

#else  /* платы без готового кадра от загрузчика */

/* У эмулятора экран появляется уже после подъёма графики, и показывать
 * ход загрузки попросту негде — а до графики там доезжают за
 * миллисекунды. Пустые тела вместо условной компиляции у вызывающих:
 * им не должно быть дела до того, есть ли на плате экран. */
void boot_anim_begin(void) { }
void boot_anim_stage(int done, int total) { (void)done; (void)total; }
void boot_anim_step(void) { }
void boot_anim_tick(void) { }
void boot_anim_end(void) { }
int  boot_anim_active(void) { return 0; }


#endif /* BOARD_HAS_STATIC_FB */

/* Показать прямоугольник, который отдаётся на экран.
 *
 * Отладочное, но живёт здесь, а не в gfxfb.c: тому нельзя знать про
 * early_con — он общий с x86-сборкой, где никакого early_con нет. */
void gfxfb_debug_box(int x0, int y0, int x1, int y1, int w, int h, int p) {
    early_con_puts("BOX ");
    early_con_hex32((uint32_t)x0); early_con_puts(",");
    early_con_hex32((uint32_t)y0); early_con_puts(" - ");
    early_con_hex32((uint32_t)x1); early_con_puts(",");
    early_con_hex32((uint32_t)y1); early_con_puts("  ekran ");
    early_con_hex32((uint32_t)w); early_con_puts("x");
    early_con_hex32((uint32_t)h); early_con_puts(" shag ");
    early_con_hex32((uint32_t)p); early_con_puts("\n");
}
