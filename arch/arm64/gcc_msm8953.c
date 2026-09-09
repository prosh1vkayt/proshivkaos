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
/* Прочитать регистр ветви — чтобы показать его состояние на экране.
 * Значение говорит больше, чем "получилось/не получилось": бит CLK_OFF
 * отвечает, пошёл ли такт вообще, а всё остальное показывает, добрались
 * ли мы до нужного регистра или читаем пустоту. */
uint32_t gcc_qup_i2c_cbcr(int qup_index) {
    if (qup_index < 1 || qup_index > 4) return 0xFFFFFFFFu;
    return mmio_read32(gcc_base() + g_blsp1_qup_i2c[qup_index - 1].cbcr);
}

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

/* ======================= Тактирование USB 3.0 ============================
 *
 * У блока USB тактов не один, а пять, и все они нужны одновременно:
 *
 *   master     основной такт контроллера, 133.33 МГц от общей ФАПЧ
 *   sleep      идёт даже когда контроллер спит — им он просыпается
 *   mock_utmi  подставной такт приёмопередатчика, 19.2 МГц от кварца;
 *              нужен, пока настоящий от приёмопередатчика ещё не пошёл
 *   phy_cfg    доступ к регистрам приёмопередатчика по шине
 *   вентиль    отдельная задвижка питания всего блока (GDSC)
 *
 * Вентиль питания здесь важнее прочего: пока он закрыт, регистры блока
 * читаются нулями или вешают шину, и никакая настройка контроллера
 * смысла не имеет. Поэтому он открывается первым и с проверкой.
 */

/* Ветви и корневые генераторы */
#define GCC_USB30_MASTER_CBCR       0x3F000
#define GCC_USB30_SLEEP_CBCR        0x3F004
#define GCC_USB30_MOCK_UTMI_CBCR    0x3F008
#define GCC_USB30_MASTER_CMD_RCGR   0x3F00C
#define GCC_USB30_MASTER_CFG_RCGR   0x3F010
#define GCC_USB30_MOCK_CMD_RCGR     0x3F020
#define GCC_USB30_MOCK_CFG_RCGR     0x3F024
#define GCC_USB_PHY_CFG_AHB_CBCR    0x3F080
#define GCC_USB30_GDSCR             0x3F078

/* Сбросы блоков */
#define GCC_USB_30_BCR              0x3F070
#define GCC_QUSB2_PHY_BCR           0x4103C

/* Вентиль питания (Globally Distributed Switch Controller) */
#define GDSC_SW_COLLAPSE            (1u << 0)   /* 1 = питание снято  */
#define GDSC_PWR_ON                 (1u << 31)  /* 1 = питание подано */

/* Настройка корневого генератора: источник и делитель.
 * Делитель хранится как (2 * N - 1) — так его задаёт изготовитель. */
#define RCG_CFG(src, div2m1)        (((uint32_t)(src) << 8) | (uint32_t)(div2m1))
#define RCG_SRC_XO                  0
#define RCG_SRC_GPLL0               1

static int rcg_set(uint32_t cmd_off, uint32_t cfg_off, uint32_t cfg) {
    mmio_write32(gcc_base() + cfg_off, cfg);
    dsb_sy();
    uint32_t cmd = mmio_read32(gcc_base() + cmd_off);
    mmio_write32(gcc_base() + cmd_off, cmd | CMD_RCGR_ROOT_EN | CMD_RCGR_UPDATE);
    dsb_sy();

    for (int i = 0; i < 500; i++) {
        if (!(mmio_read32(gcc_base() + cmd_off) & CMD_RCGR_UPDATE)) return 1;
        for (volatile int d = 0; d < 200; d++) { }
    }
    return 0;
}

static int branch_on(uint32_t cbcr) {
    uint32_t v = mmio_read32(gcc_base() + cbcr);
    mmio_write32(gcc_base() + cbcr, v | CBCR_CLK_ENABLE);
    dsb_sy();
    return wait_branch_on(cbcr);
}

/* Кратковременный сброс блока. Разряд 0 регистра сброса держит блок в
   сбросе, пока мы его не снимем. */
void gcc_usb_block_reset(int qusb2_phy) {
    uint32_t off = qusb2_phy ? GCC_QUSB2_PHY_BCR : GCC_USB_30_BCR;
    uint32_t v = mmio_read32(gcc_base() + off);
    mmio_write32(gcc_base() + off, v | 1u);
    dsb_sy();
    for (volatile int d = 0; d < 2000; d++) { }
    mmio_write32(gcc_base() + off, v & ~1u);
    dsb_sy();
    for (volatile int d = 0; d < 2000; d++) { }
}

int gcc_enable_usb30(void) {
    /* 1. Вентиль питания. Снимаем требование "свернуть" и ждём, пока
          питание действительно подадут. Загрузчик обычно оставляет его
          открытым — он сам пользуется USB для fastboot, — но полагаться
          на это нельзя: он же его и закрывает, уходя. */
    uint32_t g = mmio_read32(gcc_base() + GCC_USB30_GDSCR);
    mmio_write32(gcc_base() + GCC_USB30_GDSCR, g & ~GDSC_SW_COLLAPSE);
    dsb_sy();

    int powered = 0;
    for (int i = 0; i < 500; i++) {
        if (mmio_read32(gcc_base() + GCC_USB30_GDSCR) & GDSC_PWR_ON) { powered = 1; break; }
        for (volatile int d = 0; d < 200; d++) { }
    }

    early_con_puts("USB: ventil pitaniya ");
    early_con_hex32(mmio_read32(gcc_base() + GCC_USB30_GDSCR));
    early_con_puts(powered ? " otkryt\n" : " NE OTKRYLSYA\n");
    if (!powered) return 0;

    /* 2. Корневые генераторы. Основной — от общей ФАПЧ с делением на
          шесть (133.33 МГц), подставной — прямо от кварца. */
    int m1 = rcg_set(GCC_USB30_MASTER_CMD_RCGR, GCC_USB30_MASTER_CFG_RCGR,
                     RCG_CFG(RCG_SRC_GPLL0, 11));
    int m2 = rcg_set(GCC_USB30_MOCK_CMD_RCGR, GCC_USB30_MOCK_CFG_RCGR,
                     RCG_CFG(RCG_SRC_XO, 1));

    /* 3. Ветви. */
    int b1 = branch_on(GCC_USB30_MASTER_CBCR);
    int b2 = branch_on(GCC_USB30_SLEEP_CBCR);
    int b3 = branch_on(GCC_USB30_MOCK_UTMI_CBCR);
    int b4 = branch_on(GCC_USB_PHY_CFG_AHB_CBCR);

    early_con_puts("USB: takty gen ");
    early_con_puts(m1 ? "1" : "0");
    early_con_puts(m2 ? "1" : "0");
    early_con_puts(" vetvi ");
    early_con_puts(b1 ? "1" : "0");
    early_con_puts(b2 ? "1" : "0");
    early_con_puts(b3 ? "1" : "0");
    early_con_puts(b4 ? "1" : "0");
    early_con_puts(" (master sleep utmi phy)\n");

    return (b1 && b2 && b3 && b4);
}
