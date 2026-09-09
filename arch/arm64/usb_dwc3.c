/* arch/arm64/usb_dwc3.c — контроллер USB в режиме устройства.
 *
 * ЧТО ЭТО. В телефоне стоит контроллер Synopsys DesignWare USB3 в обвязке
 * Qualcomm. Он умеет быть и хостом, и устройством; нам нужно второе —
 * чтобы компьютер увидел телефон как подключённое устройство и мог читать
 * с него журнал загрузки в реальном времени.
 *
 * ЗАЧЕМ. До сих пор круг отладки замыкался так: собрать образ, загрузить,
 * подождать, вручную вернуть телефон в загрузчик, поднять recovery,
 * вытащить журнал из области ОЗУ. Пять минут и живой человек на каждый
 * шаг. Провод USB уже воткнут — по нему и надо разговаривать.
 *
 * ЧЕМ ЭТОТ КОНТРОЛЛЕР НЕОБЫЧЕН. Он не имеет регистров "выдать байт".
 * Обмен описывается ОПИСАТЕЛЯМИ ПЕРЕДАЧ (TRB) в обычной памяти:
 * процессор кладёт описатель, говорит "начать передачу", и дальше
 * контроллер сам ходит в память. О результатах он сообщает, складывая
 * записи в ОЧЕРЕДЬ СОБЫТИЙ — тоже в обычной памяти.
 *
 * Отсюда два следствия, которые определяют половину этого файла:
 *
 *   1. Память под описатели и очередь событий должна быть НЕКЭШИРУЕМОЙ.
 *      Контроллер о кэшах процессора не знает: то, что мы записали, для
 *      него может ещё лежать в кэше, а то, что записал он, у нас может
 *      быть перекрыто старой строкой. Для этого в компоновщике выделена
 *      отдельная область (__dma_start), а в mmu.c она отображается без
 *      кэширования.
 *
 *   2. Прерывания не нужны. Очередь событий опрашивается — как и всё
 *      остальное в этой системе.
 *
 * ПОРЯДОК ПОДЪЁМА взят из руководства Qualcomm по этому блоку и сверен с
 * загрузчиком, который поднимает тот же контроллер на том же процессоре
 * ради fastboot. Порядок там не декоративный: приёмопередатчик
 * настраивается, пока контроллер удерживается в сбросе, и никак иначе.
 */
#include "arm64.h"
#include "boards/board.h"
#include "hal_time.h"

/* ---------------- Регистры контроллера ---------------- */
/* Глобальные (общие для режимов хоста и устройства) */
#define GSBUSCFG0        0xC100
#define GCTL             0xC110
#define GSTS             0xC118
#define GSNPSID          0xC120
#define GUSB2PHYCFG0     0xC200
#define GUSB3PIPECTL0    0xC2C0
#define GEVNTADRLO0      0xC400
#define GEVNTADRHI0      0xC404
#define GEVNTSIZ0        0xC408
#define GEVNTCOUNT0      0xC40C

/* Режим устройства */
#define DCFG             0xC700
#define DCTL             0xC704
#define DEVTEN           0xC708
#define DSTS             0xC70C
#define DALEPENA         0xC720
#define DEP_PAR2(n)      (0xC800 + (n) * 0x10)
#define DEP_PAR1(n)      (0xC804 + (n) * 0x10)
#define DEP_PAR0(n)      (0xC808 + (n) * 0x10)
#define DEP_CMD(n)       (0xC80C + (n) * 0x10)

#define GCTL_DSBLCLKGTNG    (1u << 0)
#define GCTL_DISSCRAMBLE    (1u << 3)
#define GCTL_SCALEDOWN_MASK (3u << 4)
#define GCTL_DEBUGATTACH    (1u << 8)
#define GCTL_SOFITPSYNC     (1u << 10)
#define GCTL_CORESOFTRESET  (1u << 11)
#define GCTL_PRTCAPDIR_MASK (3u << 12)
#define GCTL_PRTCAP_DEVICE  (2u << 12)
#define GCTL_U2RSTECN       (1u << 16)
#define GCTL_PWRDNSCALE(x)  ((uint32_t)(x) << 19)
#define GCTL_PWRDNSCALE_MSK (0x1FFFu << 19)

#define GUSB2_PHYSOFTRST    (1u << 31)
#define GUSB2_SUSPHY        (1u << 6)
#define GUSB2_ENBLSLPM      (1u << 8)

#define DCTL_RUN_STOP       (1u << 31)
#define DCTL_CSFTRST        (1u << 30)

#define DCFG_SPEED_MASK     0x7u
#define DCFG_SPEED_HS       0
#define DCFG_DEVADDR(a)     ((uint32_t)(a) << 3)
#define DCFG_DEVADDR_MASK   (0x7Fu << 3)

#define DEVTEN_DISCONNECT   (1u << 0)
#define DEVTEN_USBRESET     (1u << 1)
#define DEVTEN_CONNECTDONE  (1u << 2)
#define DEVTEN_ULSTCNG      (1u << 3)
#define DEVTEN_SUSPEND      (1u << 6)

#define DSTS_SPEED_MASK     0x7u

/* Команды конечной точки */
#define DEPCMD_SETEPCONFIG      0x1
#define DEPCMD_SETTRANSFRESOURCE 0x2
#define DEPCMD_SETSTALL         0x4
#define DEPCMD_CLEARSTALL       0x5
#define DEPCMD_STARTTRANSFER    0x6
#define DEPCMD_ENDTRANSFER      0x8
#define DEPCMD_STARTNEWCONFIG   0x9
#define DEPCMD_CMDACT           (1u << 10)
#define DEPCMD_HIPRI_FORCERM    (1u << 11)
#define DEPCMD_PARAM(x)         ((uint32_t)(x) << 16)
#define DEPCMD_STATUS(v)        (((v) >> 12) & 0xF)

/* Поля настройки конечной точки */
#define DEPCFG0_ACTION(x)       ((uint32_t)(x) << 30)
#define DEPCFG0_BURST(x)        ((uint32_t)(x) << 22)
#define DEPCFG0_FIFONUM(x)      ((uint32_t)(x) << 17)
#define DEPCFG0_MAXPKT(x)       ((uint32_t)(x) << 3)
#define DEPCFG0_TYPE(x)         ((uint32_t)(x) << 1)
#define DEPCFG1_EPNUM(x)        ((uint32_t)(x) << 26)
#define DEPCFG1_EPDIR(x)        ((uint32_t)(x) << 25)
#define DEPCFG1_XFER_COMPLETE   (1u << 8)
#define DEPCFG1_XFER_NOT_READY  (1u << 10)

#define EP_TYPE_CONTROL         0
#define EP_TYPE_BULK            2

/* Описатель передачи */
#define TRB_HWO                 (1u << 0)
#define TRB_LST                 (1u << 1)
#define TRB_CHN                 (1u << 2)
#define TRB_CSP                 (1u << 3)
#define TRB_CTL(x)              ((uint32_t)(x) << 4)
#define TRB_ISP_IMI             (1u << 10)
#define TRB_IOC                 (1u << 11)

#define TRBCTL_NORMAL           1
#define TRBCTL_CONTROL_SETUP    2
#define TRBCTL_CONTROL_STATUS2  3
#define TRBCTL_CONTROL_STATUS3  4
#define TRBCTL_CONTROL_DATA     5

/* Виды событий */
#define DEVT_DISCONNECT         0
#define DEVT_USBRESET           1
#define DEVT_CONNECTDONE        2
#define DEVT_LINKSTATE          3
#define DEVT_SUSPEND            6

#define DEPEVT_XFERCOMPLETE     1
#define DEPEVT_XFERINPROGRESS   2
#define DEPEVT_XFERNOTREADY     3
#define DEPEVT_EPCMDCMPLT       7

#define DEPEVT_STATUS_CTRL_DATA   1
#define DEPEVT_STATUS_CTRL_STATUS 2

/* ---------------- Обвязка Qualcomm ---------------- */
#define QSCRATCH_GENERAL_CFG        0x08
#define QSCRATCH_RAM1_REG           0x0C
#define QSCRATCH_HS_PHY_CTRL        0x10
#define QSCRATCH_PARAMETER_OVERRIDE 0x14
#define QSCRATCH_HS_PHY_CTRL_COMMON 0xEC

#define GENCFG_PIPE_UTMI_CLK_SEL    (1u << 0)
#define GENCFG_DBM_EN               (1u << 1)
#define GENCFG_PIPE3_PHYSTATUS_SW   (1u << 3)
#define GENCFG_PIPE_UTMI_CLK_DIS    (1u << 8)

#define HSPHY_COMMONONN             (1u << 11)
#define HSPHY_VBUSVLDEXT0           (1u << 13)
#define HSPHY_UTMI_OTG_VBUS_VALID   (1u << 20)
#define HSPHY_FREECLK_SEL           (1u << 25)
#define HSPHY_SW_SESSVLD_SEL        (1u << 28)
#define HSPHY_COMMON_VBUSVLDEXTSEL0 (1u << 12)

/* Приёмопередатчик QUSB2 */
#define QUSB2_PLL_TEST          0x04
#define QUSB2_PLL_TUNE          0x08
#define QUSB2_PLL_USER_CTL1     0x0C
#define QUSB2_PLL_USER_CTL2     0x10
#define QUSB2_PLL_PWR_CTL       0x18
#define QUSB2_PLL_AUTOPGM_CTL1  0x1C
#define QUSB2_PLL_STATUS        0x38
#define QUSB2_PORT_TUNE1        0x80
#define QUSB2_PORT_TUNE2        0x84
#define QUSB2_PORT_TUNE3        0x88
#define QUSB2_PORT_TUNE4        0x8C
#define QUSB2_PORT_TEST2        0x9C
#define QUSB2_PORT_POWERDOWN    0xB4
#define QUSB2_PLL_LOCK          0x20

#define AHB2PHY_TOP_CFG         0x10

/* ---------------- Размещение в некэшируемой области ---------------- */
#define EVT_BYTES       4096
#define EVT_ENTRIES     (EVT_BYTES / 4)

#define DMA_EVT         0x0000
#define DMA_TRB0        0x1000
#define DMA_TRB_IN      0x1010
#define DMA_TRB_OUT     0x1020
#define DMA_SETUP       0x1100          /* пакет запроса, 8 байт        */
#define DMA_CTRL        0x1200          /* ответ на управляющий запрос  */
#define DMA_IN          0x1400          /* что отдаём хосту             */
#define DMA_OUT         0x1800          /* что приняли от хоста         */

#define CTRL_MAX        512
#define BULK_MAX        1024

typedef struct { volatile uint32_t bpl, bph, size, ctrl; } trb_t;

/* Физические номера точек. У этого контроллера точка с номером n и
   направлением d занимает физический номер n*2+d, где d=1 означает
   "к хосту". Управляющая точка — нулевая, обе половины. */
#define PHYS_EP0_OUT    0
#define PHYS_EP0_IN     1
#define PHYS_BULK_OUT   2
#define PHYS_BULK_IN    3

static uint64_t g_base = 0;
static uint8_t *g_dma  = 0;
static int      g_up   = 0;
static int      g_configured = 0;
static uint32_t g_evt_pos = 0;
static uint16_t g_ep0_max = 64;
static int      g_in_busy = 0;
static int      g_out_armed = 0;
static uint8_t  g_pending_addr = 0;

/* Состояние управляющей передачи */
static int g_ctrl_data_len = 0;   /* сколько байт данных в этой передаче */
static int g_ctrl_dir_in   = 0;   /* данные идут к хосту                 */

/* Какие передачи сейчас идут и под каким номером ресурса.
 *
 * Номер нужен, чтобы передачу можно было ОБОРВАТЬ. При сбросе шины
 * изготовитель требует оборвать все идущие передачи, иначе точка
 * останется занятой и следующая передача на ней будет отвергнута —
 * перечисление на этом и заканчивается, молча. Номер сообщает сам
 * контроллер: он кладёт его в регистр команды после запуска. */
static uint8_t g_running[4];
static uint8_t g_res_idx[4];

static void *dma_at(uint32_t off) { return (void *)(g_dma + off); }
static uint32_t dma_pa(uint32_t off) { return (uint32_t)(uintptr_t)(g_dma + off); }

static uint32_t rd(uint32_t off)            { return mmio_read32(g_base + off); }
static void     wr(uint32_t off, uint32_t v){ mmio_write32(g_base + off, v); }

static void note(const char *s) {
    early_con_puts("USB: ");
    early_con_puts(s);
    early_con_puts("\n");
}

static void note_hex(const char *s, uint32_t v) {
    early_con_puts("USB: ");
    early_con_puts(s);
    early_con_puts(" ");
    early_con_hex32(v);
    early_con_puts("\n");
}

/* ---------------- Приёмопередатчик ---------------- */

/* Последовательность изготовителя для QUSB2 на этом процессоре.
 *
 * Числа берутся ИЗ ДЕРЕВА УСТРОЙСТВ ЭТОГО ТЕЛЕФОНА, а не из общих
 * значений для процессора: у свойства qcom,qusb-phy-init-seq они парами
 * "значение, смещение", и для mido три из них отличаются от тех, что
 * пишет загрузчик. Смысл у них электрический — сопротивление линий, ток
 * передатчика, настройки умножителя частоты, — и подбирать их самим
 * нельзя. */
static const struct { uint16_t off; uint8_t val; } g_qusb2_seq[] = {
    { 0x80, 0xF8 },   /* сопротивление линий            */
    { 0x84, 0x53 },   /* ток передатчика                */
    { 0x88, 0x93 },   /* время установления автокалибровки */
    { 0x8C, 0xCF },
    { 0x9C, 0x14 },
    { 0x08, 0x30 },   /* умножитель частоты             */
    { 0x0C, 0x79 },
    { 0x10, 0x21 },
    { 0x90, 0x00 },
    { 0x1C, 0x9F },
    { 0x18, 0x00 },
};

static int qusb2_phy_init(void) {
    const uint64_t phy = BOARD_QUSB2_PHY_BASE;

    /* ПИТАНИЕ ЗДЕСЬ НЕ ТРОГАЕМ, И ЭТО ОСОЗНАННО.
     *
     * Приёмопередатчик питается тремя источниками (l3 цифровая часть,
     * l7 аналоговая, l13 линия), и сперва здесь стояла их проверка с
     * включением. Обход шины показал, что все три и так работают —
     * загрузчик их не гасит, потому что сам пользуется USB для fastboot.
     *
     * Стоило этим обращениям стать настоящими (до того они уходили в
     * пустоту из-за неверного номера ведомого), как образ перестал
     * подниматься вовсе: телефон молча уходил в перезагрузку ещё до
     * того, как появиться на проводе. Причина выяснилась не сразу и
     * записана в spmi_msm.c: запись в узел, которым распоряжается не
     * наше ядро исполнения, роняет процессор мгновенно и без жалоб.
     * Теперь владелец спрашивается заранее, но и без этого работа тут
     * лишняя: источники и так работают.
     */
    /* 1. СНЯТЬ ЗАЖИМ. Пока питание 1.8 В отсутствовало, цифровые выходы
     *    приёмопередатчика были прижаты к нулю, чтобы не наводить помех
     *    на обесточенную часть. Зажим не снимается сам: его отпускают
     *    отдельным регистром, и до этого блок ведёт себя как мёртвый. */
    mmio_write32(BOARD_QUSB2_CLAMP_DIG_N, 1);
    dsb_sy();

    /* 2. Блок приёмопередатчика — в сброс и обратно. */
    gcc_usb_block_reset(1);

    /* 3. Шина к регистрам приёмопередатчика: одно состояние ожидания. */
    mmio_write32(BOARD_USB_AHB2PHY_BASE + AHB2PHY_TOP_CFG, 0x11);
    dsb_sy();

    /* 4. Держим приёмопередатчик выключенным, пока настраиваем. */
    mmio_write32(phy + QUSB2_PORT_POWERDOWN, 0x23);
    dsb_sy();

    for (unsigned i = 0; i < sizeof(g_qusb2_seq) / sizeof(g_qusb2_seq[0]); i++)
        mmio_write32(phy + g_qusb2_seq[i].off, g_qusb2_seq[i].val);
    dsb_sy();

    /* 5. ЖИВ ЛИ БЛОК ВООБЩЕ.
     *
     * Читаем обратно то, что только что записали. Это разделяет два
     * отказа, снаружи одинаковых: "умножитель не захватил частоту" и
     * "регистров нет вовсе". Если записанное не читается, дело не в
     * настройке — блок обесточен или зажат, и настраивать нечего. */
    uint32_t back1 = mmio_read32(phy + QUSB2_PORT_TUNE1);
    uint32_t back2 = mmio_read32(phy + QUSB2_PORT_TUNE2);
    early_con_puts("USB: PHY otvet ");
    early_con_hex32(back1);
    early_con_puts(" ");
    early_con_hex32(back2);
    early_con_puts(" (zhdyom 000000F8 00000053)\n");

    /* 6. Включаем. */
    mmio_write32(phy + QUSB2_PORT_POWERDOWN, 0x22);
    dsb_sy();
    hal_time_delay_us(150);

    /* 7. ОПОРНЫЙ ТАКТ. Схема опорного такта на этой плате парная
     *    (в дереве устройств qcom,phy-clk-scheme = "cml"), а не
     *    одиночная. Разница не косметическая: при одиночной схеме
     *    источник выбирается разрядом внутри приёмопередатчика, при
     *    парной такт приходит снаружи и его надо отдельно включить.
     *    Раньше здесь безусловно выставлялся разряд одиночной схемы —
     *    то есть приёмопередатчику говорили брать такт оттуда, где его
     *    нет. */
    mmio_write32(BOARD_QUSB2_REF_CLK_EN,
                 mmio_read32(BOARD_QUSB2_REF_CLK_EN) | 1u);
    dsb_sy();
    hal_time_delay_us(100);

    for (int i = 0; i < 200; i++) {
        if (mmio_read32(phy + QUSB2_PLL_STATUS) & QUSB2_PLL_LOCK) {
            note("priyomoperedatchik zahvatil chastotu");
            return 1;
        }
        hal_time_delay_us(5);
    }

    note_hex("priyomoperedatchik NE zahvatil chastotu, status",
             mmio_read32(phy + QUSB2_PLL_STATUS));
    return 0;
}

/* ---------------- Обвязка ---------------- */

static void qscratch_set(uint32_t off, uint32_t clear, uint32_t set) {
    uint64_t a = BOARD_USB_QSCRATCH_BASE + off;
    uint32_t v = mmio_read32(a);
    mmio_write32(a, (v & ~clear) | set);
    dsb_sy();
}

static void wrapper_init(void) {
    /* Обход блока пакетной пересылки: он нужен только скоростным режимам
       с прямым доступом, а мы работаем простыми очередями. */
    qscratch_set(QSCRATCH_GENERAL_CFG, GENCFG_DBM_EN, 0);

    /* Вся встроенная память — одной конфигурацией. */
    mmio_write32(BOARD_USB_QSCRATCH_BASE + QSCRATCH_RAM1_REG, 0);

    /* Приёмопередатчик высокой скорости: общий генератор не глушить. */
    qscratch_set(QSCRATCH_HS_PHY_CTRL, HSPHY_FREECLK_SEL, HSPHY_COMMONONN);

    /* Электрические параметры линии — значение изготовителя. */
    mmio_write32(BOARD_USB_QSCRATCH_BASE + QSCRATCH_PARAMETER_OVERRIDE, 0xD190E4);
    dsb_sy();
}

/* Работаем только на высокой скорости.
 *
 * Разъём у этого телефона обычный micro-USB: пар для сверхскоростного
 * режима в нём просто нет. Но контроллер об этом не знает и будет ждать
 * такта от сверхскоростного приёмопередатчика, которого мы не поднимаем.
 * Эти три строки подсовывают ему вместо него такт от приёмопередатчика
 * высокой скорости. */
static void hs_only_mode(void) {
    qscratch_set(QSCRATCH_GENERAL_CFG, 0, GENCFG_PIPE_UTMI_CLK_DIS);
    hal_time_delay_us(5);
    qscratch_set(QSCRATCH_GENERAL_CFG, 0,
                 GENCFG_PIPE_UTMI_CLK_SEL | GENCFG_PIPE3_PHYSTATUS_SW);
    hal_time_delay_us(5);
    qscratch_set(QSCRATCH_GENERAL_CFG, GENCFG_PIPE_UTMI_CLK_DIS, 0);
}

/* Сообщить контроллеру, что питание на разъёме есть.
 *
 * Сигнал наличия питания на этой плате в контроллер напрямую не заведён —
 * им распоряжается микросхема зарядки. Управлять ею мы не умеем, поэтому
 * говорим контроллеру, что питание есть, вручную. Если провод не воткнут,
 * ничего плохого не произойдёт: линию мы подтянем, а хоста на ней не
 * окажется. */
static void vbus_override(void) {
    qscratch_set(QSCRATCH_HS_PHY_CTRL_COMMON, 0, HSPHY_COMMON_VBUSVLDEXTSEL0);
    qscratch_set(QSCRATCH_HS_PHY_CTRL, 0,
                 HSPHY_VBUSVLDEXT0 | HSPHY_UTMI_OTG_VBUS_VALID | HSPHY_SW_SESSVLD_SEL);
}

/* ---------------- Команды конечных точек ---------------- */

static int depcmd(int ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2) {
    wr(DEP_PAR2(ep), p2);
    wr(DEP_PAR1(ep), p1);
    wr(DEP_PAR0(ep), p0);
    wr(DEP_CMD(ep), cmd | DEPCMD_CMDACT);
    dsb_sy();

    for (int i = 0; i < 200000; i++) {
        uint32_t v = rd(DEP_CMD(ep));
        if (!(v & DEPCMD_CMDACT)) {
            uint32_t st = DEPCMD_STATUS(v);
            if (st) {
                early_con_puts("USB: komanda tochke ");
                early_con_hex32((uint32_t)ep);
                early_con_puts(" otvergnuta, status ");
                early_con_hex32(st);
                early_con_puts("\n");
                return 0;
            }
            return 1;
        }
    }
    note_hex("komanda tochke ne zavershilas, ep", (uint32_t)ep);
    return 0;
}

/* Настроить точку. action: 0 — задать заново, 2 — изменить на ходу. */
static int ep_config(int phys, int type, int maxpkt, int action) {
    uint32_t p0 = DEPCFG0_ACTION(action) | DEPCFG0_BURST(0) |
                  DEPCFG0_MAXPKT(maxpkt) | DEPCFG0_TYPE(type);
    if (phys & 1) p0 |= DEPCFG0_FIFONUM(phys >> 1);   /* очередь передачи — только для точек к хосту */

    uint32_t p1 = DEPCFG1_EPNUM(phys >> 1) | DEPCFG1_EPDIR(phys & 1) |
                  DEPCFG1_XFER_COMPLETE | DEPCFG1_XFER_NOT_READY;

    return depcmd(phys, DEPCMD_SETEPCONFIG, p0, p1, 0);
}

static int ep_resource(int phys) {
    return depcmd(phys, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
}

/* Положить описатель и запустить передачу. */
static int start_xfer(int phys, uint32_t trb_off, uint32_t buf_off,
                      int len, int trbctl) {
    trb_t *t = (trb_t *)dma_at(trb_off);
    t->bpl  = dma_pa(buf_off);
    t->bph  = 0;
    t->size = (uint32_t)len;
    t->ctrl = TRB_CTL(trbctl) | TRB_HWO | TRB_LST | TRB_IOC | TRB_ISP_IMI;
    dsb_sy();

    /* Адрес описателя: старшая половина в первом параметре, младшая во
       втором. Порядок именно такой — так задан интерфейс команды. */
    if (!depcmd(phys, DEPCMD_STARTTRANSFER, 0, dma_pa(trb_off), 0)) return 0;

    if (phys < 4) {
        g_res_idx[phys] = (uint8_t)((rd(DEP_CMD(phys)) >> 16) & 0x7F);
        g_running[phys] = 1;
    }
    return 1;
}

/* Оборвать идущую передачу. Без этого точка остаётся занятой. */
static void end_xfer(int phys) {
    if (phys >= 4 || !g_running[phys]) return;
    depcmd(phys, DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM |
                 DEPCMD_PARAM(g_res_idx[phys]), 0, 0, 0);
    g_running[phys] = 0;
}

/* ---------------- Управляющая точка ---------------- */

static void ep0_await_setup(void) {
    uint8_t *s = (uint8_t *)dma_at(DMA_SETUP);
    for (int i = 0; i < 8; i++) s[i] = 0;
    dsb_sy();
    start_xfer(PHYS_EP0_OUT, DMA_TRB0, DMA_SETUP, 8, TRBCTL_CONTROL_SETUP);
}

static void ep0_stall(void) {
    depcmd(PHYS_EP0_OUT, DEPCMD_SETSTALL, 0, 0, 0);
    ep0_await_setup();
}

/* Разобрать запрос и приготовить ответ. */
static void ep0_setup(void) {
    const uint8_t *req = (const uint8_t *)dma_at(DMA_SETUP);
    uint16_t wlen = (uint16_t)(req[6] | (req[7] << 8));

    const uint8_t *data = 0;
    int len = 0;

    int ok = usb_pos_setup(req, &data, &len, &g_pending_addr);

    early_con_puts("USB: zapros ");
    for (int i = 0; i < 8; i++) { early_con_hex8(req[i]); early_con_puts(" "); }
    early_con_puts(ok ? "OK " : "OTKAZ ");
    early_con_hex32((uint32_t)len);
    early_con_puts("\n");

    if (!ok) { ep0_stall(); return; }

    /* Адрес назначается сразу: хост ждёт, что следующий обмен пойдёт уже
       по новому адресу, а подтверждение уходит ещё по старому. Контроллер
       разбирается с этим сам, если адрес записан до подтверждения. */
    if (g_pending_addr) {
        uint32_t cfg = rd(DCFG) & ~DCFG_DEVADDR_MASK;
        wr(DCFG, cfg | DCFG_DEVADDR(g_pending_addr));
        dsb_sy();
        g_pending_addr = 0;
    }

    g_ctrl_dir_in = (req[0] & 0x80) ? 1 : 0;

    if (wlen == 0 || len == 0) {
        /* Данных нет — остаётся только подтверждение. Оно запросится
           событием "готов к следующему шагу". */
        g_ctrl_data_len = 0;
        return;
    }

    if (len > (int)wlen) len = wlen;
    g_ctrl_data_len = len;

    if (g_ctrl_dir_in) {
        uint8_t *dst = (uint8_t *)dma_at(DMA_CTRL);
        if (len > CTRL_MAX) len = CTRL_MAX;
        for (int i = 0; i < len; i++) dst[i] = data[i];
        dsb_sy();
        start_xfer(PHYS_EP0_IN, DMA_TRB0, DMA_CTRL, len, TRBCTL_CONTROL_DATA);
    } else {
        start_xfer(PHYS_EP0_OUT, DMA_TRB0, DMA_CTRL, len, TRBCTL_CONTROL_DATA);
    }
}

/* ---------------- Точки для потока данных ---------------- */

static void bulk_arm_out(void) {
    if (g_out_armed) return;
    if (start_xfer(PHYS_BULK_OUT, DMA_TRB_OUT, DMA_OUT, BULK_MAX, TRBCTL_NORMAL))
        g_out_armed = 1;
}

/* Отдать хосту очередной кусок журнала, если точка свободна. */
static void bulk_pump_in(void) {
    if (!g_configured || g_in_busy) return;

    uint8_t *buf = (uint8_t *)dma_at(DMA_IN);
    int n = usb_pos_pull(buf, BULK_MAX);
    if (n <= 0) return;

    dsb_sy();
    if (start_xfer(PHYS_BULK_IN, DMA_TRB_IN, DMA_IN, n, TRBCTL_NORMAL))
        g_in_busy = 1;
}

/* Включить точки выбранной конфигурации. */
static void enable_bulk(void) {
    /* Перераспределение ресурсов передачи для новой конфигурации.
       Номер ресурса здесь обязан быть 2 — так описан интерфейс. */
    end_xfer(PHYS_BULK_OUT);
    end_xfer(PHYS_BULK_IN);

    depcmd(PHYS_EP0_OUT, DEPCMD_STARTNEWCONFIG | DEPCMD_PARAM(2), 0, 0, 0);

    ep_config(PHYS_BULK_OUT, EP_TYPE_BULK, 512, 0);
    ep_resource(PHYS_BULK_OUT);
    ep_config(PHYS_BULK_IN, EP_TYPE_BULK, 512, 0);
    ep_resource(PHYS_BULK_IN);

    wr(DALEPENA, rd(DALEPENA) | (1u << PHYS_BULK_OUT) | (1u << PHYS_BULK_IN));
    dsb_sy();

    g_in_busy = 0;
    g_out_armed = 0;
    bulk_arm_out();

    note("tochki potoka dannyh vklyucheny");
}

/* ---------------- События ---------------- */

static void on_reset(void) {
    note("shina sbroshena hostom");
    g_configured = 0;
    g_in_busy = 0;
    g_out_armed = 0;

    /* Изготовитель требует прямо: при сбросе шины оборвать все идущие
       передачи. Оставленная незавершённой передача держит точку занятой,
       и следующая на ней будет отвергнута — перечисление кончится, не
       начавшись, и без единой жалобы. */
    for (int i = 0; i < 4; i++) end_xfer(i);

    wr(DALEPENA, (1u << PHYS_EP0_OUT) | (1u << PHYS_EP0_IN));
    wr(DCFG, rd(DCFG) & ~DCFG_DEVADDR_MASK);
    dsb_sy();

    usb_pos_reset();
    ep0_await_setup();
}

static void on_connect_done(void) {
    uint32_t speed = rd(DSTS) & DSTS_SPEED_MASK;
    g_ep0_max = (speed == 2) ? 8 : 64;      /* 2 — низкая скорость */

    early_con_puts("USB: host soedinilsya, skorost ");
    early_con_hex32(speed);
    early_con_puts(" (0=vysokaya 1=polnaya 2=nizkaya)\n");

    /* Размер пакета управляющей точки зависит от скорости, а узнаётся она
       только сейчас. Меняем настройку на ходу — для этого и существует
       действие "изменить". */
    ep_config(PHYS_EP0_OUT, EP_TYPE_CONTROL, g_ep0_max, 2);
    ep_config(PHYS_EP0_IN,  EP_TYPE_CONTROL, g_ep0_max, 2);

    /* Заново взводить управляющую точку здесь НЕЛЬЗЯ: она уже взведена —
       либо при подъёме, либо при сбросе шины, который приходит перед
       этим событием. Вторая передача на занятой точке была бы отвергнута,
       и дальше первого вопроса хоста дело бы не пошло. */
    usb_pos_speed(speed == 0 ? 480 : 12);
}

static void on_ep_event(uint32_t ev) {
    int ep     = (int)((ev >> 1) & 0x1F);
    int kind   = (int)((ev >> 6) & 0xF);
    int status = (int)((ev >> 12) & 0xF);

    if (kind == DEPEVT_XFERCOMPLETE && ep < 4) g_running[ep] = 0;

    switch (kind) {
    case DEPEVT_XFERCOMPLETE:
        if (ep == PHYS_EP0_OUT || ep == PHYS_EP0_IN) {
            trb_t *t = (trb_t *)dma_at(DMA_TRB0);
            uint32_t ctl = t->ctrl;
            uint32_t type = (ctl >> 4) & 0x3F;

            if (type == TRBCTL_CONTROL_SETUP) {
                ep0_setup();
            } else if (type == TRBCTL_CONTROL_STATUS2 ||
                       type == TRBCTL_CONTROL_STATUS3) {
                ep0_await_setup();
            } else if (type == TRBCTL_CONTROL_DATA && !g_ctrl_dir_in) {
                int got = g_ctrl_data_len - (int)(t->size & 0xFFFFFF);
                usb_pos_ctrl_data((const uint8_t *)dma_at(DMA_CTRL), got);
            }
        } else if (ep == PHYS_BULK_IN) {
            g_in_busy = 0;
        } else if (ep == PHYS_BULK_OUT) {
            trb_t *t = (trb_t *)dma_at(DMA_TRB_OUT);
            int got = BULK_MAX - (int)(t->size & 0xFFFFFF);
            g_out_armed = 0;
            if (got > 0) usb_pos_received((const uint8_t *)dma_at(DMA_OUT), got);
            bulk_arm_out();
        }
        break;

    case DEPEVT_XFERNOTREADY:
        if (ep == PHYS_EP0_OUT || ep == PHYS_EP0_IN) {
            if (status == DEPEVT_STATUS_CTRL_STATUS) {
                /* Подтверждение. Двухшаговое, если данных не было. */
                int ctl = g_ctrl_data_len ? TRBCTL_CONTROL_STATUS3
                                          : TRBCTL_CONTROL_STATUS2;
                start_xfer(ep, DMA_TRB0, DMA_CTRL, 0, ctl);
            }
        } else if (ep == PHYS_BULK_OUT) {
            bulk_arm_out();
        }
        break;

    default:
        break;
    }
}

static void handle_event(uint32_t ev) {
    if (ev & 1) {
        int type = (int)((ev >> 8) & 0xF);
        switch (type) {
        case DEVT_USBRESET:    on_reset(); break;
        case DEVT_CONNECTDONE: on_connect_done(); break;
        case DEVT_DISCONNECT:
            note("host otsoedinilsya");
            g_configured = 0;
            usb_pos_reset();
            break;
        case DEVT_LINKSTATE:
        case DEVT_SUSPEND:
        default:
            break;
        }
    } else {
        on_ep_event(ev);
    }
}

/* ---------------- Внешний интерфейс ---------------- */

int usb_dwc3_ready(void)      { return g_configured; }
void usb_dwc3_set_configured(int on) {
    if (on && !g_configured) { g_configured = 1; enable_bulk(); }
    else if (!on) g_configured = 0;
}

/* Фоновая прокрутка: вызывается из каждого ожидания в системе (см.
 * hal_time.h). Хост задаёт вопросы и ждёт ответа миллисекунды — стоящая
 * в задержке система для него неотличима от отключённой. */
void hal_usb_pump(void) { usb_dwc3_poll(); }

void usb_dwc3_poll(void) {
    static int busy = 0;
    if (!g_up || busy) return;
    busy = 1;

    uint32_t count = rd(GEVNTCOUNT0) & 0xFFFC;
    if (count) {
        uint32_t left = count;
        volatile uint32_t *q = (volatile uint32_t *)dma_at(DMA_EVT);
        while (left) {
            uint32_t ev = q[g_evt_pos];
            g_evt_pos = (g_evt_pos + 1) % EVT_ENTRIES;
            left -= 4;
            handle_event(ev);
        }
        wr(GEVNTCOUNT0, count);
        dsb_sy();
    }

    bulk_pump_in();
    busy = 0;
}

int usb_dwc3_init(void) {
    extern char __dma_start[];

    g_base = BOARD_USB_CORE_BASE;
    g_dma  = (uint8_t *)__dma_start;

    note("podnimaem kontroller");

    if (!gcc_enable_usb30()) {
        note("takty ne podnyalis, dalshe nechego delat");
        return 0;
    }

    /* Настройка обвязки и приёмопередатчика делается, пока ядро
       контроллера удерживается в сбросе. Порядок задан изготовителем. */
    wr(GCTL, rd(GCTL) | GCTL_CORESOFTRESET);
    dsb_sy();
    hal_time_delay_us(100);

    wrapper_init();
    int phy_ok = qusb2_phy_init();
    if (!phy_ok)
        note("bez chastoty priyomoperedatchika komandy tochkam ne proydut");

    /* Сброс цифрового стыка с приёмопередатчиком. */
    wr(GUSB2PHYCFG0, rd(GUSB2PHYCFG0) | GUSB2_PHYSOFTRST);
    dsb_sy();
    hal_time_delay_us(100);
    wr(GUSB2PHYCFG0, rd(GUSB2PHYCFG0) & ~GUSB2_PHYSOFTRST);
    dsb_sy();
    hal_time_delay_us(100);

    wr(GCTL, rd(GCTL) & ~GCTL_CORESOFTRESET);
    dsb_sy();
    hal_time_delay_us(100);

    hs_only_mode();
    vbus_override();

    /* Сброс части, отвечающей за режим устройства. */
    wr(DCTL, rd(DCTL) | DCTL_CSFTRST);
    dsb_sy();
    int reset_ok = 0;
    for (int i = 0; i < 200000; i++)
        if (!(rd(DCTL) & DCTL_CSFTRST)) { reset_ok = 1; break; }

    note_hex("pasport kontrollera", rd(GSNPSID));
    if (!reset_ok) { note("sbros rezhima ustroystva ne zavershilsya"); return 0; }

    /* Приёмопередатчику не давать засыпать: спящий он не увидит хоста, а
       будить его нам нечем — прерываний в системе нет. */
    wr(GUSB2PHYCFG0, rd(GUSB2PHYCFG0) & ~(GUSB2_SUSPHY | GUSB2_ENBLSLPM));

    /* Общая настройка: режим устройства, без замедления, без отладочного
       подключения. */
    uint32_t gctl = rd(GCTL);
    gctl &= ~(GCTL_PRTCAPDIR_MASK | GCTL_SCALEDOWN_MASK | GCTL_DEBUGATTACH |
              GCTL_SOFITPSYNC | GCTL_DISSCRAMBLE | GCTL_PWRDNSCALE_MSK);
    gctl |= GCTL_PRTCAP_DEVICE | GCTL_U2RSTECN | GCTL_PWRDNSCALE(2);
    wr(GCTL, gctl);
    dsb_sy();

    /* Очередь событий. Выравнена на свой размер — этого требует
       изготовитель. */
    uint8_t *evt = (uint8_t *)dma_at(DMA_EVT);
    for (int i = 0; i < EVT_BYTES; i++) evt[i] = 0;
    dsb_sy();

    wr(GEVNTADRLO0, dma_pa(DMA_EVT));
    wr(GEVNTADRHI0, 0);
    wr(GEVNTSIZ0, EVT_BYTES);
    wr(GEVNTCOUNT0, 0);
    g_evt_pos = 0;
    dsb_sy();

    /* Работаем только на высокой скорости: разъём другого и не позволяет. */
    wr(DCFG, (rd(DCFG) & ~(DCFG_SPEED_MASK | DCFG_DEVADDR_MASK)) | DCFG_SPEED_HS);

    wr(DEVTEN, DEVTEN_DISCONNECT | DEVTEN_USBRESET | DEVTEN_CONNECTDONE |
               DEVTEN_ULSTCNG | DEVTEN_SUSPEND);
    dsb_sy();

    /* Управляющая точка. Ресурсы передачи распределяются заново, начиная
       с нулевого — это делается один раз на подъём. */
    if (!depcmd(PHYS_EP0_OUT, DEPCMD_STARTNEWCONFIG | DEPCMD_PARAM(0), 0, 0, 0)) {
        note("ne udalos raspredelit resursy peredachi");
        return 0;
    }

    if (!ep_config(PHYS_EP0_OUT, EP_TYPE_CONTROL, 64, 0) ||
        !ep_config(PHYS_EP0_IN,  EP_TYPE_CONTROL, 64, 0) ||
        !ep_resource(PHYS_EP0_OUT) ||
        !ep_resource(PHYS_EP0_IN)) {
        note("upravlyayushchaya tochka ne nastroilas");
        return 0;
    }

    wr(DALEPENA, (1u << PHYS_EP0_OUT) | (1u << PHYS_EP0_IN));
    dsb_sy();

    g_up = 1;
    ep0_await_setup();

    /* Подтягиваем линию — с этого мгновения хост нас видит. */
    wr(DCTL, rd(DCTL) | DCTL_RUN_STOP);
    dsb_sy();

    note_hex("zapushchen, DSTS", rd(DSTS));
    return 1;
}
