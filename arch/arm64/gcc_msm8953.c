/* arch/arm64/gcc_msm8953.c — включение тактовых сигналов блоков QUP.
 *
 * Зачем это вообще нужно. На Snapdragon почти вся периферия по умолчанию
 * обесточена по тактам: регистры блока читаются, но возвращают мусор или
 * виснут, пока соответствующая ветвь тактового дерева не включена. Порт
 * UART нам достался уже настроенным — им пользовался сам загрузчик, — а вот
 * шину I2C тачскрина загрузчик не трогает, и включать её приходится самим.
 *
 * Проверено на живом устройстве: /sys/kernel/debug/clk/
 * gcc_blsp1_qup3_i2c_apps_clk в работающем Android показывает enable = 0,
 * rate = 19200000, parent = blsp1_qup3_i2c_apps_clk_src, исток —
 * xo_clk_src. То есть такт гасится, когда не нужен, и после передачи
 * управления от загрузчика он с высокой вероятностью выключен.
 *
 * ЧТО ЗДЕСЬ НЕ ПРОВЕРЕНО. Смещения регистров внутри GCC снять с устройства
 * не удалось: /dev/mem в этом ядре отключён, а regmap для GCC в debugfs не
 * экспортирован (там только spmi и кодек). Значения соответствуют раскладке
 * BLSP для этого семейства (msm8916/8937/8953 совпадают). Поэтому здесь
 * есть то, чего обычно в драйверах тактов не делают: ПРОВЕРКА РЕЗУЛЬТАТА.
 * После включения читается бит CLK_OFF, и если такт не поднялся, функция
 * честно возвращает ошибку вместо того, чтобы оставить систему висеть на
 * первом же обращении к I2C.
 */
#include "arm64.h"
#include "boards/board.h"

/* Голосование за общие ветви: несколько подсистем могут держать один и тот
 * же такт включённым, поэтому у него не бит включения, а регистр голосов. */
#define GCC_APCS_CLOCK_BRANCH_ENA_VOTE  0x45004
#define VOTE_BLSP1_AHB                  (1u << 10)

/* Ветвь такта (Clock Branch Control Register) */
#define CBCR_CLK_ENABLE                 (1u << 0)
#define CBCR_CLK_OFF                    (1u << 31)  /* 1 = такт стоит */

/* Корневой генератор (Root Clock Generator) */
#define CMD_RCGR_UPDATE                 (1u << 0)
#define CMD_RCGR_ROOT_EN                (1u << 1)
#define CMD_RCGR_ROOT_OFF               (1u << 31)

/* Смещения блоков BLSP1 QUP. Индекс — номер QUP (1..4), у нашей шины
 * тачскрина это QUP3 (i2c@78b7000). */
typedef struct {
    uint32_t cmd_rcgr;   /* корневой генератор такта                */
    uint32_t cbcr;       /* ветвь, идущая непосредственно в блок    */
} qup_clk_t;

static const qup_clk_t g_blsp1_qup_i2c[4] = {
    { 0x02000, 0x02008 },   /* QUP1 */
    { 0x03000, 0x03010 },   /* QUP2 */
    { 0x04000, 0x04020 },   /* QUP3 — шина тачскрина */
    { 0x05000, 0x05020 },   /* QUP4 */
};

static uint64_t gcc_base(void) { return BOARD_GCC_BASE; }

/* Ждём, пока ветвь сообщит, что такт пошёл. Возврат 1 — пошёл. */
static int wait_branch_on(uint32_t cbcr) {
    for (int i = 0; i < 200; i++) {
        if (!(mmio_read32(gcc_base() + cbcr) & CBCR_CLK_OFF))
            return 1;
        /* Пауза дешёвыми оборотами: hal_time на этом этапе может быть ещё
           не проинициализирован, а связываться с ним ради микросекунд
           незачем. */
        for (volatile int d = 0; d < 200; d++) { }
    }
    return 0;
}

/* Включить тактирование шины I2C номер qup_index (1..4) на BLSP1.
 * Возвращает 1, если такт реально пошёл. */
int gcc_enable_blsp1_qup_i2c(int qup_index) {
    if (qup_index < 1 || qup_index > 4) return 0;

    const qup_clk_t *clk = &g_blsp1_qup_i2c[qup_index - 1];

    /* 1. Интерфейсный такт всей группы BLSP1. Он общий для всех QUP и
          управляется голосованием, а не прямым включением. */
    uint32_t vote = mmio_read32(gcc_base() + GCC_APCS_CLOCK_BRANCH_ENA_VOTE);
    mmio_write32(gcc_base() + GCC_APCS_CLOCK_BRANCH_ENA_VOTE, vote | VOTE_BLSP1_AHB);
    dsb_sy();

    /* 2. Корневой генератор конкретного QUP. Для I2C он тактуется прямо от
          опорного кварца 19.2 МГц без деления — это состояние сброса, так
          что перенастраивать источник не нужно, достаточно разрешить
          корень и применить настройку. */
    uint32_t cmd = mmio_read32(gcc_base() + clk->cmd_rcgr);
    mmio_write32(gcc_base() + clk->cmd_rcgr, cmd | CMD_RCGR_ROOT_EN | CMD_RCGR_UPDATE);
    dsb_sy();

    /* Бит UPDATE аппаратура сбрасывает сама, когда применит настройку. */
    for (int i = 0; i < 200; i++) {
        if (!(mmio_read32(gcc_base() + clk->cmd_rcgr) & CMD_RCGR_UPDATE)) break;
        for (volatile int d = 0; d < 200; d++) { }
    }

    /* 3. Собственно ветвь, идущая в блок QUP. */
    uint32_t cbcr = mmio_read32(gcc_base() + clk->cbcr);
    mmio_write32(gcc_base() + clk->cbcr, cbcr | CBCR_CLK_ENABLE);
    dsb_sy();

    if (!wait_branch_on(clk->cbcr)) {
        uart_write("gcc: takt BLSP1 QUP");
        uart_write_hex((uint64_t)qup_index);
        uart_write(" ne podnyalsya (CBCR ");
        uart_write_hex(mmio_read32(gcc_base() + clk->cbcr));
        uart_write(")\n");
        return 0;
    }
    return 1;
}
