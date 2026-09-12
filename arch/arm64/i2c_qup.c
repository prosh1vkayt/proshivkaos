/* arch/arm64/i2c_qup.c — ведущий I2C на контроллере Qualcomm QUP (версия 2).
 *
 * QUP (Qualcomm Universal Peripheral) — универсальный блок, который в
 * зависимости от настройки работает то как SPI, то как I2C. Отсюда его
 * непривычная организация: вместо «регистра данных» две очереди (FIFO), а
 * вместо отдельных команд «выдать старт», «выдать стоп» — поток КОМАНДНЫХ
 * МЕТОК, которые кладутся в очередь вперемешку с данными.
 *
 * Транзакция описывается маленькой программой:
 *
 *     START, адрес_и_направление
 *     DATAWR, сколько_байт
 *     байт, байт, байт...
 *
 * ГЛАВНОЕ, ЧТО ЗДЕСЬ ВАЖНО ПОНЯТЬ, и на чём мы спотыкались несколько
 * заходов подряд: чтение регистра устройства — это НЕ одна транзакция и НЕ
 * две независимые. Это одна СЕССИЯ блока из двух подтранзакций. Различие
 * не в словах:
 *
 *   - сброс блока делается ОДИН РАЗ на всю сессию, а не перед каждой
 *     подтранзакцией; сброс посреди сессии освобождает шину и теряет
 *     повторный старт;
 *   - между подтранзакциями блок переводится в ПАУЗУ, а не в сброс;
 *   - вторая и последующие подтранзакции обязаны выставлять разряд 31 в
 *     счётчиках — «настройка меняется на ходу». Без него блок считает
 *     смену счётчиков в непокое ошибкой;
 *   - при чтении во входную очередь приходят ДВА ЛИШНИХ БАЙТА меток
 *     перед данными, и счётчик приёма задаётся с их учётом.
 *
 * Всё это выписано из драйвера ядра (drivers/i2c/busses/i2c-qup.c,
 * функции qup_i2c_xfer_v2, qup_i2c_conf_xfer_v2, qup_i2c_conf_count_v2).
 * Он — единственный проверенный образец для этого процессора: загрузчик
 * Qualcomm на msm8953 шиной I2C не пользуется вовсе.
 *
 * Прерывания не используются — как и везде в этой системе, всё
 * опрашивается. Каждый цикл ожидания ограничен счётчиком: драйвер
 * устройства, которого нет на шине, обязан не вешать систему.
 */
#include "arm64.h"

/* ---- Регистры QUP ---- */
#define QUP_CONFIG              0x000
#define QUP_STATE               0x004
#define QUP_IO_MODE             0x008
#define QUP_SW_RESET            0x00C
#define QUP_OPERATIONAL         0x018
#define QUP_ERROR_FLAGS         0x01C
#define QUP_ERROR_FLAGS_EN      0x020
#define QUP_OPERATIONAL_MASK    0x028
#define QUP_HW_VERSION          0x030
#define QUP_MX_OUTPUT_CNT       0x100   /* блочный режим: сколько выдать   */
#define QUP_OUT_FIFO_CNT        0x10C   /* слов сейчас в очереди передачи  */
#define QUP_OUT_FIFO_BASE       0x110
#define QUP_MX_WRITE_CNT        0x150   /* режим очередей: сколько выдать  */
#define QUP_MX_INPUT_CNT        0x200   /* блочный режим: сколько принять  */
#define QUP_MX_READ_CNT         0x208   /* режим очередей: сколько принять */
#define QUP_IN_FIFO_CNT         0x214
#define QUP_IN_FIFO_BASE        0x218
#define QUP_I2C_CLK_CTL         0x400
#define QUP_I2C_STATUS          0x404
#define QUP_I2C_MASTER_GEN      0x408
#define QUP_V2_TAGS_EN          1u

/* Состояние блока */
#define QUP_STATE_MASK          0x3
#define QUP_STATE_VALID         (1u << 2)
#define QUP_I2C_MAST_GEN        (1u << 4)
#define QUP_RESET_STATE         0
#define QUP_RUN_STATE           1
#define QUP_PAUSE_STATE         3

/* Настройка ядра */
#define QUP_CONFIG_MINI_CORE_I2C (2u << 8)
#define QUP_CONFIG_N_V2          7u        /* число бит в слове минус один */
#define QUP_CONFIG_NO_INPUT      (1u << 7) /* передача без чтения          */

/* Режим очередей. Нули в полях режима означают простой FIFO — без блочной
 * передачи и без DMA. PACK/UNPACK заставляют блок упаковывать байты в
 * 32-битные слова, иначе на каждый байт уходило бы отдельное слово. */
#define QUP_IO_MODE_PACK_EN     (1u << 15)
#define QUP_IO_MODE_UNPACK_EN   (1u << 14)
#define QUP_IO_MODE_REPACK      (QUP_IO_MODE_PACK_EN | QUP_IO_MODE_UNPACK_EN)

/* Разряды размеров очередей в исходном значении QUP_IO_MODE */
#define IO_OUT_BLOCK_SIZE(x)    (((x) >> 0) & 0x03)
#define IO_OUT_FIFO_SIZE(x)     (((x) >> 2) & 0x07)
#define IO_IN_BLOCK_SIZE(x)     (((x) >> 5) & 0x03)
#define IO_IN_FIFO_SIZE(x)      (((x) >> 7) & 0x07)

/* Текущее состояние обмена */
#define QUP_OUT_NOT_EMPTY       (1u << 4)
#define QUP_IN_NOT_EMPTY        (1u << 5)
#define QUP_OUT_FULL            (1u << 6)
#define QUP_OUT_SVC_FLAG        (1u << 8)
#define QUP_IN_SVC_FLAG         (1u << 9)
#define QUP_MX_OUTPUT_DONE      (1u << 10)
#define QUP_MX_INPUT_DONE       (1u << 11)

#define QUP_OPERATIONAL_RESET   0x000FF0     /* залипшие флаги блока   */
#define QUP_I2C_STATUS_RESET    0xFFFFFCu    /* залипшие флаги шины    */

/* Ошибки */
#define I2C_STATUS_BUS_ACTIVE   (1u << 8)
#define I2C_STATUS_ERROR_MASK   0x38000FCu
#define QUP_STATUS_ERROR_FLAGS  0x7Cu

/* Командные метки протокола версии 2 */
#define TAG_START               0x81
#define TAG_DATAWR              0x82
#define TAG_DATAWR_STOP         0x83
#define TAG_DATARD              0x85
#define TAG_DATARD_STOP         0x87

/* Разрешение менять счётчики, не выходя из работы. */
#define QUP_MX_CONFIG_DURING_RUN (1u << 31)

/* При чтении блок кладёт во входную очередь две метки перед данными. */
#define RX_TAG_LEN              2

#define POLL_LIMIT              200000    /* оборотов ожидания на флаг */
#define XFER_MAX                80        /* максимум байт в одну сторону */

/* ---- Состояние драйвера ---- */

static uint32_t g_clk_ctl     = 0;
static uint32_t g_hw_version  = 0;
static int      g_out_fifo_sz = 16;
static int      g_in_fifo_sz  = 16;
static uint32_t g_last_status = 0;
/* Подробный след каждого шага сессии.
 *
 * Пока шина не работала, он был единственным способом понять, на чём
 * именно она спотыкается, и стоил своего места в журнале. Теперь она
 * работает, а след даёт по семь строк на каждое обращение к тачскрину —
 * то есть заслоняет собой всё остальное. Снимки при отказе остаются;
 * включить след обратно можно вызовом i2c_qup_trace(1). */
static int      g_trace       = 0;

uint32_t i2c_qup_last_status(void) { return g_last_status; }
void     i2c_qup_trace(int on)     { g_trace = on; }

/* ---- Снимки состояния ---- */

/* Однострочный снимок. Печатается на каждом шаге сессии, а не только при
 * отказе: круг отладки на этом аппарате — сборка, перезагрузка через
 * загрузчик, чтение журнала из области сохранённых сообщений, — и дешевле
 * напечатать лишнее, чем ходить второй раз за одним регистром. */
static void trace(uint64_t base, const char *what) {
    if (!g_trace) return;
    early_con_puts("  ");
    early_con_puts(what);
    early_con_puts(" ST=");
    early_con_hex32(mmio_read32(base + QUP_STATE));
    early_con_puts(" OP=");
    early_con_hex32(mmio_read32(base + QUP_OPERATIONAL));
    early_con_puts(" I2C=");
    early_con_hex32(mmio_read32(base + QUP_I2C_STATUS));
    early_con_puts(" O=");
    early_con_hex32(mmio_read32(base + QUP_OUT_FIFO_CNT));
    early_con_puts(" I=");
    early_con_hex32(mmio_read32(base + QUP_IN_FIFO_CNT));
    early_con_puts("\n");
}

void i2c_qup_dump(uint64_t base, const char *where) {
    early_con_puts("I2C ");
    early_con_puts(where);
    early_con_puts(":\n");
    early_con_puts("  ST=");
    early_con_hex32(mmio_read32(base + QUP_STATE));
    early_con_puts(" OP=");
    early_con_hex32(mmio_read32(base + QUP_OPERATIONAL));
    early_con_puts(" ERR=");
    early_con_hex32(mmio_read32(base + QUP_ERROR_FLAGS));
    early_con_puts("\n  I2C=");
    early_con_hex32(mmio_read32(base + QUP_I2C_STATUS));
    early_con_puts(" CFG=");
    early_con_hex32(mmio_read32(base + QUP_CONFIG));
    early_con_puts(" IO=");
    early_con_hex32(mmio_read32(base + QUP_IO_MODE));
    early_con_puts("\n  WCNT=");
    early_con_hex32(mmio_read32(base + QUP_MX_WRITE_CNT));
    early_con_puts(" RCNT=");
    early_con_hex32(mmio_read32(base + QUP_MX_READ_CNT));
    early_con_puts(" O=");
    early_con_hex32(mmio_read32(base + QUP_OUT_FIFO_CNT));
    early_con_puts(" I=");
    early_con_hex32(mmio_read32(base + QUP_IN_FIFO_CNT));
    early_con_puts("\n");
}

/* Расшифровка состояния шины словами.
 *
 * Шестнадцатеричное число в журнале приходится каждый раз разбирать по
 * разрядам вручную, а разряды здесь отвечают на совершенно разные
 * вопросы. Неподтверждение означает, что шина работает и передача дошла
 * до устройства, а устройство промолчало — его нет или оно обесточено.
 * Ошибка шины означает противоположное: виноваты мы. */
void i2c_qup_explain(uint32_t st) {
    early_con_puts("  shina:");
    if (st & (1u << 2)) early_con_puts(" OSHIBKA-SHINY");
    if (st & (1u << 3)) early_con_puts(" NET-PODTVERZHDENIYA");
    if (st & (1u << 4)) early_con_puts(" POTERYA-ARBITRAZHA");
    if (st & (1u << 5)) early_con_puts(" NEDOPUSTIMAYA-ZAPIS");
    if (st & (3u << 6)) early_con_puts(" SBOY");
    if (st & (1u << 23)) early_con_puts(" NEDOPUSTIMAYA-METKA");
    if (st & (1u << 24)) early_con_puts(" NEVERNYY-ADRES-CHTENIYA");
    if (st & (1u << 25)) early_con_puts(" NEVERNAYA-POSLEDOVATELNOST");
    if (st & I2C_STATUS_BUS_ACTIVE) early_con_puts(" shina-zanyata");
    if (!(st & (I2C_STATUS_ERROR_MASK | I2C_STATUS_BUS_ACTIVE)))
        early_con_puts(" chisto");
    early_con_puts("\n");
}

/* ---- Смена состояния ---- */

static int poll_state_mask(uint64_t base, uint32_t want, uint32_t mask) {
    for (int i = 0; i < POLL_LIMIT; i++) {
        uint32_t st = mmio_read32(base + QUP_STATE);
        if ((st & QUP_STATE_VALID) && (st & mask) == want) return 1;
    }
    return 0;
}

/* Перевести блок в состояние и УБЕДИТЬСЯ, что он туда встал.
 *
 * Проверка именно на равенство поля состояния, а не на «нужные разряды
 * взведены»: раньше здесь стояло второе, и переход в сброс (значение 0)
 * не проверялся вовсе — условие выполнялось само собой. */
static int set_state(uint64_t base, uint32_t state) {
    if (!poll_state_mask(base, 0, 0)) return 0;       /* дождаться VALID */
    mmio_write32(base + QUP_STATE, state);
    dsb_sy();
    return poll_state_mask(base, state, QUP_STATE_MASK);
}

/* ---- Инициализация ---- */

/* core_hz — частота, которой тактуется сам блок (у нас 19.2 МГц от кварца),
 * bus_hz — желаемая скорость шины (400 кГц). */
int i2c_qup_init(uint64_t base, uint32_t core_hz, uint32_t bus_hz) {
    mmio_write32(base + QUP_SW_RESET, 1);
    dsb_sy();

    if (!poll_state_mask(base, QUP_RESET_STATE, QUP_STATE_MASK)) {
        early_con_puts("I2C: blok ne vstal v sbros, ST=");
        early_con_hex32(mmio_read32(base + QUP_STATE));
        early_con_puts(" (ne vklyuchen takt? ne tot adres?)\n");
        return 0;
    }

    /* ПАСПОРТ БЛОКА. Проверка, что мы вообще разговариваем с QUP, а не с
     * пустотой: у живого блока версия имеет вид 2.x, а исходное значение
     * режима хранит размеры очередей. Если здесь нули или все единицы —
     * либо адрес не тот, либо такт не включён, и дальше идти незачем. */
    g_hw_version = mmio_read32(base + QUP_HW_VERSION);

    uint32_t io = mmio_read32(base + QUP_IO_MODE);
    static const int blk_sizes[3] = { 4, 16, 32 };
    int ob = IO_OUT_BLOCK_SIZE(io), ib = IO_IN_BLOCK_SIZE(io);
    int out_blk = (ob < 3) ? blk_sizes[ob] : 16;
    int in_blk  = (ib < 3) ? blk_sizes[ib] : 16;
    g_out_fifo_sz = out_blk * (2 << IO_OUT_FIFO_SIZE(io));
    g_in_fifo_sz  = in_blk  * (2 << IO_IN_FIFO_SIZE(io));

    early_con_puts("I2C: versiya bloka ");
    early_con_hex32(g_hw_version);
    early_con_puts(" ochered vyd/pri ");
    early_con_hex32((uint32_t)g_out_fifo_sz);
    early_con_puts("/");
    early_con_hex32((uint32_t)g_in_fifo_sz);
    early_con_puts(" bayt\n");

    if (g_hw_version == 0 || g_hw_version == 0xFFFFFFFFu) {
        early_con_puts("I2C: pasport pustoy — blok ne otvechaet\n");
        return 0;
    }

    mmio_write32(base + QUP_OPERATIONAL, QUP_OPERATIONAL_RESET);
    mmio_write32(base + QUP_ERROR_FLAGS_EN, 0x7);
    mmio_write32(base + QUP_OPERATIONAL_MASK, 0);
    mmio_write32(base + QUP_I2C_STATUS, QUP_I2C_STATUS_RESET);
    dsb_sy();

    /* Делитель скорости шины. Формула изготовителя: за период на шине блок
       отсчитывает (делитель + 3) * 2 своих тактов. Верхние разряды —
       задержка удержания данных, тройка стандартна для быстрого режима. */
    uint32_t fs_div = (core_hz / bus_hz) / 2;
    fs_div = (fs_div > 3) ? (fs_div - 3) : 0;
    if (fs_div > 0xFF) fs_div = 0xFF;
    g_clk_ctl = ((3u & 0x7) << 8) | fs_div;

    early_con_puts("I2C: delitel takta shiny ");
    early_con_hex32(g_clk_ctl);
    early_con_puts("\n");

    return 1;
}

/* ---- Очереди ---- */

/* Затолкать поток меток и данных в выходную очередь.
 *
 * Очередь принимает 32-битными словами по четыре байта, младший байт
 * первым. Незаполненный хвост дописывается как есть: блок отсчитает
 * ровно столько байт, сколько объявлено в счётчике. */
static int push_out(uint64_t base, const uint8_t *tags, int tlen,
                    const uint8_t *data, int dlen) {
    uint32_t word = 0;
    int pos = 0;

    for (int i = 0; i < tlen + dlen; i++) {
        uint8_t b = (i < tlen) ? tags[i] : data[i - tlen];
        word |= (uint32_t)b << (8 * pos);

        if (++pos == 4) {
            for (int t = 0; ; t++) {
                if (!(mmio_read32(base + QUP_OPERATIONAL) & QUP_OUT_FULL)) break;
                if (t >= POLL_LIMIT) return 0;
            }
            mmio_write32(base + QUP_OUT_FIFO_BASE, word);
            word = 0;
            pos = 0;
        }
    }

    if (pos) {
        for (int t = 0; ; t++) {
            if (!(mmio_read32(base + QUP_OPERATIONAL) & QUP_OUT_FULL)) break;
            if (t >= POLL_LIMIT) return 0;
        }
        mmio_write32(base + QUP_OUT_FIFO_BASE, word);
    }

    dsb_sy();
    return 1;
}

/* Дождаться конца подтранзакции.
 *
 * Заменяет обработчик прерывания драйвера ядра: там на каждое событие
 * приходит прерывание, здесь мы крутим тот же разбор флагов в цикле.
 * Возврат 1 — подтранзакция отработала. */
static int wait_xfer(uint64_t base, uint8_t *rx, int total_rx) {
    int got = 0;

    for (int i = 0; i < POLL_LIMIT; i++) {
        uint32_t op  = mmio_read32(base + QUP_OPERATIONAL);
        uint32_t bus = mmio_read32(base + QUP_I2C_STATUS) & I2C_STATUS_ERROR_MASK;
        uint32_t err = mmio_read32(base + QUP_ERROR_FLAGS) & QUP_STATUS_ERROR_FLAGS;

        if (bus || err) {
            g_last_status = mmio_read32(base + QUP_I2C_STATUS);
            /* Флаги сбрасываются записью их же значения. Не сбросить —
               значит обречь следующую подтранзакцию на ту же ошибку. */
            if (err) mmio_write32(base + QUP_ERROR_FLAGS, err);
            if (bus) mmio_write32(base + QUP_I2C_STATUS, bus);
            mmio_write32(base + QUP_STATE, QUP_RESET_STATE);
            dsb_sy();
            return 0;
        }

        /* Флаги обслуживания сбрасываются записью их же значения — иначе
           блок считает, что мы ещё не отреагировали, и стоит. */
        if (op & QUP_OUT_SVC_FLAG) mmio_write32(base + QUP_OPERATIONAL, QUP_OUT_SVC_FLAG);
        if (op & QUP_IN_SVC_FLAG)  mmio_write32(base + QUP_OPERATIONAL, QUP_IN_SVC_FLAG);

        if (total_rx > 0) {
            while (got < total_rx &&
                   (mmio_read32(base + QUP_OPERATIONAL) & QUP_IN_NOT_EMPTY)) {
                uint32_t w = mmio_read32(base + QUP_IN_FIFO_BASE);
                for (int b = 0; b < 4 && got < total_rx; b++, got++)
                    rx[got] = (uint8_t)((w >> (8 * b)) & 0xFF);
            }
            if (got >= total_rx) return 1;
        } else {
            /* Для выдачи в режиме очередей признаком конца служит флаг
               обслуживания при опустевшей очереди: полный признак
               "счётчик отработан" у этого блока запаздывает, о чём прямо
               написано в драйвере ядра. */
            if (op & QUP_MX_OUTPUT_DONE) return 1;
            if ((op & QUP_OUT_SVC_FLAG) &&
                mmio_read32(base + QUP_OUT_FIFO_CNT) == 0) return 1;
        }
    }

    g_last_status = mmio_read32(base + QUP_I2C_STATUS);
    return 0;
}

/* ---- Одна подтранзакция сессии ---- */

/* is_first — первая в сессии (ей задаётся такт шины и она не требует
 *            разрешения менять настройку на ходу);
 * is_last  — последняя (после неё блок не ставится на паузу). */
static int sub_xfer(uint64_t base, uint8_t addr, int is_rx, int is_first, int is_last,
                    const uint8_t *wbuf, int len, uint8_t *rxraw) {
    uint8_t tags[8];
    int n = 0;

    tags[n++] = TAG_START;
    tags[n++] = (uint8_t)((addr << 1) | (is_rx ? 1 : 0));
    if (is_last) tags[n++] = is_rx ? TAG_DATARD_STOP : TAG_DATAWR_STOP;
    else         tags[n++] = is_rx ? TAG_DATARD      : TAG_DATAWR;
    tags[n++] = (uint8_t)len;

    int total_tx = n + (is_rx ? 0 : len);
    int total_rx = is_rx ? (RX_TAG_LEN + len) : 0;

    /* Посылка печатается только под следом.
     *
     * Пока шина не работала, эти две строки были самым ценным в журнале.
     * Теперь тачскрин опрашивается десятки раз в секунду, и каждая
     * строка — это отрисовка текста по экрану, запись в сохраняемую
     * область и отправка в провод. Отладка шины превратилась в главный
     * потребитель времени системы. */
    if (g_trace) {
        early_con_puts(is_rx ? "I2C chtenie:" : "I2C zapis:");
        for (int i = 0; i < n; i++) { early_con_puts(" "); early_con_hex8(tags[i]); }
        if (!is_rx) for (int i = 0; i < len; i++) { early_con_puts(" "); early_con_hex8(wbuf[i]); }
        early_con_puts("\n");
    }

    /* Счётчики. Разряд 31 — «настройка меняется на ходу»; он обязателен
       для всех подтранзакций кроме первой, потому что все остальные
       задаются, когда блок уже не в сбросе. */
    uint32_t run = is_first ? 0 : QUP_MX_CONFIG_DURING_RUN;
    uint32_t cfg = QUP_CONFIG_MINI_CORE_I2C | QUP_CONFIG_N_V2;

    mmio_write32(base + QUP_MX_WRITE_CNT, run | (uint32_t)total_tx);
    if (total_rx) mmio_write32(base + QUP_MX_READ_CNT, run | (uint32_t)total_rx);
    else          cfg |= QUP_CONFIG_NO_INPUT;
    mmio_write32(base + QUP_CONFIG, cfg);
    dsb_sy();

    trace(base, "schetchiki zadany");

    if (is_first) {
        if (!set_state(base, QUP_RUN_STATE)) {
            i2c_qup_dump(base, "ne poshyol v rabotu");
            return 0;
        }
        mmio_write32(base + QUP_I2C_CLK_CTL, g_clk_ctl);
        dsb_sy();
        if (!set_state(base, QUP_PAUSE_STATE)) {
            i2c_qup_dump(base, "ne vstal na pauzu");
            return 0;
        }
        trace(base, "takt zadan, pauza");
    }

    /* Очередь наполняется в паузе: блок в работе начинает передачу сразу,
       и досыпать ему данные уже поздно. */
    if (!push_out(base, tags, n, wbuf, is_rx ? 0 : len)) {
        i2c_qup_dump(base, "ochered ne osvobodilas");
        return 0;
    }
    trace(base, "ochered napolnena");

    if (!set_state(base, QUP_RUN_STATE)) {
        i2c_qup_dump(base, "ne vozobnovil rabotu");
        return 0;
    }
    trace(base, "pushchen");

    if (!wait_xfer(base, rxraw, total_rx)) {
        i2c_qup_dump(base, is_rx ? "otvet ne prishyol" : "vydacha ne zavershilas");
        i2c_qup_explain(g_last_status);
        return 0;
    }
    trace(base, "otrabotal");

    /* Все подтранзакции кроме последней заканчиваются паузой: сброс здесь
       освободил бы шину и потерял повторный старт. */
    if (!is_last && !set_state(base, QUP_PAUSE_STATE)) {
        i2c_qup_dump(base, "ne vstal na pauzu mezhdu chastyami");
        return 0;
    }

    return 1;
}

/* ---- Сессия целиком ---- */

/* Транзакция «записать, затем прочитать через повторный старт».
 *
 * rlen == 0 превращает вызов в обычную запись, wlen == 0 — в чистое чтение.
 * Возвращает 1 при успехе. */
int i2c_qup_xfer(uint64_t base, uint8_t addr,
                 const uint8_t *wbuf, int wlen,
                 uint8_t *rbuf, int rlen) {
    if (wlen < 0 || rlen < 0 || wlen > XFER_MAX || rlen > XFER_MAX) return 0;
    if (wlen == 0 && rlen == 0) return 0;

    uint8_t rxraw[RX_TAG_LEN + XFER_MAX];

    /* Сброс — ОДИН на всю сессию. */
    mmio_write32(base + QUP_SW_RESET, 1);
    dsb_sy();
    if (!poll_state_mask(base, QUP_RESET_STATE, QUP_STATE_MASK)) {
        i2c_qup_dump(base, "ne vstal v sbros");
        return 0;
    }

    mmio_write32(base + QUP_CONFIG, QUP_CONFIG_MINI_CORE_I2C | QUP_CONFIG_N_V2);
    mmio_write32(base + QUP_I2C_MASTER_GEN, QUP_V2_TAGS_EN);
    dsb_sy();

    /* Блок должен объявить себя ведущим — иначе в работу он не пойдёт. */
    if (!poll_state_mask(base, QUP_I2C_MAST_GEN, QUP_I2C_MAST_GEN)) {
        i2c_qup_dump(base, "ne stal vedushchim");
        return 0;
    }

    /* Режим очередей задаётся один раз, в сбросе: обе стороны работают
       очередями, поэтому блочные счётчики обнуляются. */
    mmio_write32(base + QUP_MX_OUTPUT_CNT, 0);
    mmio_write32(base + QUP_MX_INPUT_CNT, 0);
    mmio_write32(base + QUP_IO_MODE, QUP_IO_MODE_REPACK);
    dsb_sy();
    trace(base, "sessiya otkryta");

    int ok = 1;
    int first = 1;

    if (wlen > 0) {
        ok = sub_xfer(base, addr, 0, first, rlen == 0, wbuf, wlen, 0);
        first = 0;
    }

    if (ok && rlen > 0) {
        ok = sub_xfer(base, addr, 1, first, 1, 0, rlen, rxraw);
        /* Первые два байта входной очереди — метки, а не данные. */
        if (ok) for (int i = 0; i < rlen; i++) rbuf[i] = rxraw[RX_TAG_LEN + i];
    }

    /* Дождаться, пока шина освободится, и только потом сбрасывать. */
    if (ok) {
        for (int i = 0; i < POLL_LIMIT; i++)
            if (!(mmio_read32(base + QUP_I2C_STATUS) & I2C_STATUS_BUS_ACTIVE)) break;
    }
    g_last_status = mmio_read32(base + QUP_I2C_STATUS);

    set_state(base, QUP_RESET_STATE);
    return ok;
}

/* Короткие обёртки под типовые операции работы с регистрами устройства. */

int i2c_qup_write_reg(uint64_t base, uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_qup_xfer(base, addr, buf, 2, 0, 0);
}

int i2c_qup_read_regs(uint64_t base, uint8_t addr, uint8_t reg, uint8_t *out, int len) {
    return i2c_qup_xfer(base, addr, &reg, 1, out, len);
}
