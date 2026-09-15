/* arch/arm64/cpufreq_msm8953.c — частота ядер процессора.
 *
 * ЗАЧЕМ. Загрузчик оставляет ядра на 800 МГц от общей ФАПЧ GPLL0 —
 * безопасная частота, на которой можно ничего не знать о питании. Штатный
 * максимум этого кристалла — 2016 МГц, в два с половиной раза больше. Вся
 * отрисовка, весь вывод кадра, вся система упирались в эти 800 МГц:
 * бенчмарк показал ровно 800 по счётчику тактов.
 *
 * КАК УСТРОЕНО (по drivers/clk/msm/clock-cpu-8953.c и clock-pll.c ядра
 * msm-4.9 и по дереву устройств самого телефона):
 *
 *   ФАПЧ ядер (HF PLL) по 0x0B116000 умножает кварц 19,2 МГц на L:
 *   2016 МГц — это ровно L = 105. У каждого кластера и у межкластерной
 *   шины CCI свой выбор источника: 4 — GPLL0, 5 — выход ФАПЧ. Делитель
 *   половинный: в регистре 2*d - 1.
 *
 *   Напряжение ядер — источник S5 на PM8953, которым никто, кроме нас,
 *   после загрузчика не управляет. Потолки напряжения по ступеням частоты
 *   взяты из узла cpr4-ctrl дерева устройств (qcom,cpr-voltage-ceiling):
 *   это максимум, который нужен самому худшему кристаллу.
 *
 * ПОРЯДОК, В КОТОРОМ НЕЛЬЗЯ ОШИБИТЬСЯ:
 *   1. сперва напряжение, потом частота (обратный порядок — сбой ядра);
 *   2. ФАПЧ настраивается и захватывает частоту, пока ядра ещё на GPLL0 —
 *      не захватилась, значит просто отказываемся, ничего не переключив;
 *   3. только потом выбор источника переключается на ФАПЧ.
 */
#include "arm64.h"
#include "hal_time.h"

#define HFPLL           0x0B116000UL
#define PLL_MODE        0x00
#define PLL_L           0x08
#define PLL_ALPHA       0x10
#define PLL_USER        0x18
#define PLL_CFG_LO      0x20
#define PLL_CFG_HI      0x24
#define PLL_TEST_LO     0x30
#define PLL_TEST_HI     0x34

#define MODE_OUTCTRL    (1u << 0)
#define MODE_BYPASSNL   (1u << 1)
#define MODE_RESET_N    (1u << 2)
#define MODE_LOCKED     (1u << 31)

#define MUX_C0          0x0B111050UL    /* кластер ядер 0-3 — наш */
#define MUX_CCI         0x0B1D1050UL    /* межкластерная шина */
#define MUX_CFG         0x04
#define CMD_UPDATE      (1u << 0)

#define SRC_GPLL0       4
#define SRC_HFPLL       5

typedef struct { uint16_t mhz; uint8_t l; uint16_t ceiling_mv; } opp_t;

/* Ступени из qcom,speed0-bin-v0-cl и потолки из qcom,cpr-voltage-ceiling. */
static const opp_t g_opp[] = {
    {  652,  34,  715 },
    { 1036,  54,  790 },
    { 1401,  73,  860 },
    { 1689,  88,  865 },
    { 1804,  94,  920 },
    { 1958, 102,  990 },
    { 2016, 105, 1065 },
};
#define OPP_COUNT ((int)(sizeof(g_opp) / sizeof(g_opp[0])))

int  rpm_vote_mx_level(uint32_t level);
extern uint64_t arch_cycles_per_second(void);

static void say(const char *s) { early_con_puts(s); }
static void say_hex(const char *s, uint32_t v) { early_con_puts(s); early_con_hex32(v); early_con_puts("\n"); }

/* S5: FTS2.5, диапазон 0 — 80 мВ плюс 5 мВ на шаг. */
static int apc_mv(void) {
    uint8_t range = 0, vset = 0;
    spmi_read(1, 0x2041 - 1, &range);
    spmi_read(1, 0x2041, &vset);
    return range == 0 ? 80 + 5 * (int)vset : -1;
}

static int apc_raise_to(int mv) {
    if (mv > 1140) mv = 1140;                   /* предел по дереву */
    int now = apc_mv();
    if (now < 0) { say("CPU: S5 ne v tom diapazone, ne trogaem\n"); return 0; }
    if (now >= mv) return 1;                    /* только вверх */
    uint8_t vset = (uint8_t)((mv - 80 + 4) / 5);
    if (!spmi_write(1, 0x2041, vset)) return 0;
    spmi_write(1, 0x2045, 0x80);                /* PWM: на ток ядер */
    hal_time_delay_ms(20);                      /* пусть установится */
    return apc_mv() >= mv;
}

static int mux_update(uint64_t base) {
    mmio_write32(base, mmio_read32(base) | CMD_UPDATE);
    for (int i = 0; i < 5000; i++) {
        if (!(mmio_read32(base) & CMD_UPDATE)) return 1;
        hal_time_delay_us(1);
    }
    return 0;
}

static int hfpll_start(uint8_t l) {
    /* Выключенный ФАПЧ (загрузчик его не трогает) — только тогда можно
       настраивать, не боясь выбить почву из-под ядер. */
    uint32_t mode = mmio_read32(HFPLL + PLL_MODE);
    if (mode & MODE_OUTCTRL) {
        say("CPU: FAPCH uzhe vklyuchena kem-to, ne trogaem\n");
        return 0;
    }

    uint32_t user = mmio_read32(HFPLL + PLL_USER);
    user &= ~(3u << 8);   user |= 0x100;        /* выходной делитель */
    user &= ~(1u << 12);                        /* без предделителя */
    user |= (1u << 0) | (1u << 3);              /* основной и ранний выходы */
    user &= ~(1u << 24);                        /* без M/N */
    mmio_write32(HFPLL + PLL_USER, user);
    mmio_write32(HFPLL + PLL_ALPHA, 0);
    mmio_write32(HFPLL + PLL_CFG_LO, 0x200D4828);
    mmio_write32(HFPLL + PLL_CFG_HI, 0x00000006);
    mmio_write32(HFPLL + PLL_TEST_LO, 0x1C000000);
    mmio_write32(HFPLL + PLL_TEST_HI, 0x00004000);
    mmio_write32(HFPLL + PLL_L, l);

    mode = mmio_read32(HFPLL + PLL_MODE);
    mode |= MODE_BYPASSNL;
    mmio_write32(HFPLL + PLL_MODE, mode);
    hal_time_delay_us(10);
    mode |= MODE_RESET_N;
    mmio_write32(HFPLL + PLL_MODE, mode);
    hal_time_delay_us(200);

    int locked = 0;
    for (int i = 0; i < 1000; i++) {
        if (mmio_read32(HFPLL + PLL_MODE) & MODE_LOCKED) {
            hal_time_delay_us(1);
            if (mmio_read32(HFPLL + PLL_MODE) & MODE_LOCKED) { locked = 1; break; }
        }
        hal_time_delay_us(100);
    }
    if (!locked) {
        say_hex("CPU: FAPCH ne zahvatila chastotu, MODE=", mmio_read32(HFPLL + PLL_MODE));
        mmio_write32(HFPLL + PLL_MODE, 0);      /* выключить обратно */
        return 0;
    }

    mode |= MODE_OUTCTRL;
    mmio_write32(HFPLL + PLL_MODE, mode);
    return 1;
}

/* Разогнать ядра нашего кластера до ступени idx. Возвращает частоту по
 * счётчику тактов, МГц, или 0, если отказались. */
int msm8953_cpu_boost(int idx) {
    if (idx < 0 || idx >= OPP_COUNT) return 0;
    const opp_t *o = &g_opp[idx];

    uint32_t cfg = mmio_read32(MUX_C0 + MUX_CFG);
    if (((cfg >> 8) & 7) != SRC_GPLL0) {
        say_hex("CPU: yadra ne na GPLL0, istochnik uzhe ", cfg);
        return 0;
    }

    say("CPU: podnimaem napryazhenie yader\n");
    if (!apc_raise_to(o->ceiling_mv + 10)) {
        say("CPU: napryazhenie ne podnyalos, otkaz\n");
        return 0;
    }
    say_hex("CPU: S5 teper mV ", (uint32_t)apc_mv());

    /* ФАПЧ питается от MX: по драйверу ей нужен хотя бы уровень SVS. */
    rpm_vote_mx_level(256);

    say("CPU: zapuskaem FAPCH yader\n");
    if (!hfpll_start(o->l)) return 0;
    say("CPU: FAPCH zahvatila chastotu\n");

    /* Межкластерная шина — частота ФАПЧ на 2,5, как в драйвере. */
    mmio_write32(MUX_CCI + MUX_CFG, (SRC_HFPLL << 8) | 4);
    if (!mux_update(MUX_CCI)) say("CPU: CCI ne obnovilas\n");

    /* И ядра. После этой записи мы уже на новой частоте. */
    mmio_write32(MUX_C0 + MUX_CFG, (SRC_HFPLL << 8) | 1);
    if (!mux_update(MUX_C0)) say("CPU: vybor istochnika yader ne obnovilsya\n");

    uint64_t hz = arch_cycles_per_second();
    say_hex("CPU: chastota po schetchiku taktov, MGc ", (uint32_t)(hz / 1000000u));
    return (int)(hz / 1000000u);
}
