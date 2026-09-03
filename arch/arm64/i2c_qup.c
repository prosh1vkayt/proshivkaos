/* arch/arm64/i2c_qup.c — ведущий I2C на контроллере Qualcomm QUP (версия 2).
 *
 * QUP (Qualcomm Universal Peripheral) — это универсальный блок, который в
 * зависимости от настройки работает то как SPI, то как I2C. Отсюда его
 * непривычная организация: вместо «регистра данных» у него две очереди
 * (FIFO), а вместо отдельных регистров «выдать старт», «выдать стоп» —
 * поток КОМАНДНЫХ МЕТОК, которые кладутся в очередь вперемешку с данными.
 *
 * То есть транзакция описывается не последовательностью обращений к
 * регистрам, а маленькой программой:
 *
 *     START, адрес_и_направление
 *     DATAWR, сколько_байт
 *     байт, байт, байт...
 *     START, адрес_и_направление|чтение     <- повторный старт
 *     DATARD_STOP, сколько_байт
 *
 * Всё это целиком заталкивается в выходную очередь, после чего блок сам
 * отыгрывает транзакцию на шине, а прочитанное складывает во входную
 * очередь.
 *
 * Прерывания не используются — как и везде в этой системе, всё
 * опрашивается. Для тачскрина этого достаточно с запасом: обмен идёт на
 * 400 кГц, полный пакет из десяти касаний это 63 байта, около полутора
 * миллисекунд.
 *
 * ВАЖНО ПРО ЗАВИСАНИЯ. Драйвер устройства, которого нет на шине (или на
 * шине, у которой не включён такт), обязан не вешать систему. Поэтому
 * каждый цикл ожидания здесь ограничен счётчиком, а не «ждём, пока
 * получится». На реальном железе первая же ошибка в адресах привела бы к
 * вечному ожиданию флага, который никогда не поднимется.
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
#define QUP_MX_OUTPUT_CNT       0x100   /* только блочный режим */
#define QUP_MX_WRITE_CNT        0x150   /* режим очередей: сколько выдать */
#define QUP_OUT_FIFO_CNT        0x10C
#define QUP_OUT_FIFO_BASE       0x110
#define QUP_MX_INPUT_CNT        0x200   /* только блочный режим */
#define QUP_MX_READ_CNT         0x208   /* режим очередей: сколько принять */
#define QUP_IN_READ_CUR         0x20C
#define QUP_IN_FIFO_CNT         0x214
#define QUP_IN_FIFO_BASE        0x218
#define QUP_I2C_CLK_CTL         0x400
#define QUP_I2C_STATUS          0x404
#define QUP_I2C_MASTER_GEN      0x408        /* включение меток второго
                                                поколения                 */
#define QUP_V2_TAGS_EN          1u
/* Делитель скорости шины: считается при подъёме блока, а записывается уже
 * после перевода его в работу — так делает драйвер изготовителя. */
static uint32_t g_clk_ctl = 0;

#define QUP_OPERATIONAL_RESET   0xFF0        /* залипшие флаги блока   */
#define QUP_I2C_STATUS_RESET    0xFFFFFC     /* залипшие флаги шины    */

/* Регистр состояния блока */
#define QUP_STATE_MASK          0x3
#define QUP_STATE_VALID         (1u << 2)
#define QUP_I2C_MAST_GEN        (1u << 4)
#define QUP_RESET_STATE         0
#define QUP_RUN_STATE           1
#define QUP_PAUSE_STATE         3

/* Настройка ядра */
#define QUP_CONFIG_MINI_CORE_I2C (2u << 8)
#define QUP_CONFIG_N_8BIT        7u        /* число бит в слове минус один */
#define QUP_CONFIG_NO_INPUT      (1u << 7) /* передача без чтения          */

/* Режим очередей. Нули в полях режима означают простой FIFO — без блочной
 * передачи и без DMA. PACK/UNPACK заставляют блок упаковывать байты в
 * 32-битные слова, иначе на каждый байт уходило бы отдельное слово. */
#define QUP_IO_MODE_PACK_EN     (1u << 15)
#define QUP_IO_MODE_UNPACK_EN   (1u << 14)

/* Флаги текущего состояния обмена */
#define QUP_OUT_FIFO_NOT_EMPTY  (1u << 4)
#define QUP_IN_NOT_EMPTY        (1u << 5)
#define QUP_OUT_FULL            (1u << 6)
#define QUP_OUT_SVC_FLAG        (1u << 8)
#define QUP_IN_SVC_FLAG         (1u << 9)
#define QUP_MX_OUTPUT_DONE      (1u << 10)
#define QUP_MX_INPUT_DONE       (1u << 11)

/* Ошибки шины */
#define QUP_I2C_NACK_FLAG       (1u << 3)
#define QUP_I2C_ARB_LOST        (1u << 4)
#define QUP_I2C_BUS_ERROR       (1u << 2)
#define QUP_I2C_INVALID_WRITE   (1u << 5)
#define QUP_I2C_FAILED_MASK     (QUP_I2C_NACK_FLAG | QUP_I2C_ARB_LOST | \
                                 QUP_I2C_BUS_ERROR | QUP_I2C_INVALID_WRITE)

/* Командные метки протокола версии 2 */
#define TAG_START               0x81
#define TAG_DATAWR              0x82
#define TAG_DATAWR_STOP         0x83
#define TAG_DATARD              0x85
#define TAG_DATARD_STOP         0x87

/* Разрешить менять счётчики, не выходя из рабочего состояния. */
#define QUP_MX_CONFIG_DURING_RUN (1u << 31)

#define POLL_LIMIT              200000    /* оборотов ожидания на флаг */
#define XFER_MAX                80        /* максимум байт в одну сторону */

/* ---- Вспомогательное ---- */

static int wait_state_valid(uint64_t base) {
    for (int i = 0; i < POLL_LIMIT; i++)
        if (mmio_read32(base + QUP_STATE) & QUP_STATE_VALID) return 1;
    return 0;
}

static int set_state(uint64_t base, uint32_t state) {
    if (!wait_state_valid(base)) return 0;

    mmio_write32(base + QUP_STATE, state);
    dsb_sy();

    /* Ждём, пока блок ДЕЙСТВИТЕЛЬНО окажется в запрошенном состоянии, а не
     * просто сообщит, что его состояние достоверно.
     *
     * Разница не умозрительная: блок отказывался переходить в рабочее
     * состояние и оставался в сбросе, а прежняя проверка этого не
     * замечала и рапортовала успех. Дальше мы наливали данные в очередь
     * стоящему блоку и ждали, пока он их отдаст. */
    /* Условие взято у изготовителя (драйвер загрузчика для этого же
       процессора): требуется, чтобы биты запрошенного состояния были
       ВЗВЕДЕНЫ вместе с признаком достоверности, а не чтобы поле было в
       точности равно запрошенному. */
    for (int i = 0; i < POLL_LIMIT; i++) {
        uint32_t st = mmio_read32(base + QUP_STATE);
        if ((st & (QUP_STATE_VALID | state)) == (QUP_STATE_VALID | state))
            return 1;
    }
    return 0;
}

/* Дождаться, пока блок объявит себя ведущим на шине. Загрузчик делает это
 * перед каждой сменой состояния, и не зря: пока признак не поднят, блок в
 * работу не переходит. */
static int wait_master(uint64_t base) {
    for (int i = 0; i < POLL_LIMIT; i++)
        if (mmio_read32(base + QUP_STATE) & QUP_I2C_MAST_GEN) return 1;
    return 0;
}

/* Ждём, пока в выходной очереди появится место. */
static int wait_out_room(uint64_t base) {
    for (int i = 0; i < POLL_LIMIT; i++)
        if (!(mmio_read32(base + QUP_OPERATIONAL) & QUP_OUT_FULL)) return 1;
    return 0;
}

/* Ждём, пока во входной очереди появятся данные. */
static int wait_in_data(uint64_t base) {
    for (int i = 0; i < POLL_LIMIT; i++)
        if (mmio_read32(base + QUP_OPERATIONAL) & QUP_IN_NOT_EMPTY) return 1;
    return 0;
}

/* ---- Инициализация ---- */

/* core_hz — частота, которой тактуется сам блок (у нас 19.2 МГц от кварца),
 * bus_hz — желаемая скорость шины (400 кГц). */
int i2c_qup_init(uint64_t base, uint32_t core_hz, uint32_t bus_hz) {
    /*
     * ПОСЛЕДОВАТЕЛЬНОСТЬ ВЗЯТА У ИЗГОТОВИТЕЛЯ и повторена по шагам.
     *
     * Порядок здесь не украшение. Раньше мы делали примерно то же, но
     * по-своему, и блок отказывался переходить в работу: состояние
     * оставалось "сброс", хотя ошибок не возникало. Драйвер загрузчика
     * для ЭТОГО ЖЕ процессора делает так:
     *
     *   сброс -> дождаться -> обнулить настройку -> сбросить залипшие
     *   флаги -> разрешить ошибки -> задать настройку -> ОБНУЛИТЬ
     *   управление тактом шины -> сбросить состояние шины
     *
     * Обнуление настройки перед её заданием и обнуление такта шины —
     * ровно те два шага, которых у нас не было.
     */
    mmio_write32(base + QUP_SW_RESET, 1);
    dsb_sy();

    if (!wait_state_valid(base)) {
        uart_write("i2c: blok ne otvechaet posle sbrosa, base=");
        uart_write_hex(base);
        uart_write(" (ne vklyuchen takt?)\n");
        return 0;
    }

    mmio_write32(base + QUP_CONFIG, 0);
    mmio_write32(base + QUP_OPERATIONAL, QUP_OPERATIONAL_RESET);
    mmio_write32(base + QUP_ERROR_FLAGS_EN, 0x7);

    mmio_write32(base + QUP_CONFIG, QUP_CONFIG_MINI_CORE_I2C | QUP_CONFIG_N_8BIT);

    /*
     * ВКЛЮЧЕНИЕ МЕТОК. Без этой строки всё остальное бессмысленно.
     *
     * Блок второго поколения умеет два языка. В старом транзакция
     * задаётся регистрами, в новом — потоком меток вперемешку с данными
     * (0x81 старт, 0x82 передать, 0x87 принять и остановиться). Мы всё
     * это время говорили метками, не сказав блоку, что перешли на них.
     *
     * Он и не шёл в работу: настройка получалась противоречивой —
     * счётчики заданы под один язык, поток под другой. Ошибок при этом
     * не возникало, потому что и передачи не было.
     *
     * Строка взята из драйвера ядра, где она стоит сразу за заданием
     * настройки: writel(QUP_V2_TAGS_EN, base + QUP_I2C_MASTER_GEN).
     */
    mmio_write32(base + QUP_I2C_MASTER_GEN, QUP_V2_TAGS_EN);

    /* Управление тактом шины пока обнуляем; настоящее значение блок
       получит уже в работе — так делает загрузчик. */
    mmio_write32(base + QUP_I2C_CLK_CTL, 0);
    mmio_write32(base + QUP_I2C_STATUS, QUP_I2C_STATUS_RESET);
    dsb_sy();

    /* Делитель скорости шины. Формула изготовителя: за период на шине блок
       отсчитывает (делитель + 3) * 2 своих тактов. Верхние биты — задержка
       удержания данных, тройка стандартна для быстрого режима. */
    uint32_t fs_div = (core_hz / bus_hz) / 2;
    fs_div = (fs_div > 3) ? (fs_div - 3) : 0;
    if (fs_div > 0xFF) fs_div = 0xFF;
    g_clk_ctl = ((3u & 0x7) << 8) | fs_div;

    return 1;
}

/* ---- Обмен ---- */

/* Затолкать подготовленный поток меток и данных в выходную очередь. */
static int push_out(uint64_t base, const uint8_t *buf, int len) {
    int i = 0;
    while (i < len) {
        if (!wait_out_room(base)) return 0;

        /* Очередь принимает 32-битными словами, по четыре байта за раз,
           младший байт первым. Хвост короче четырёх дополняем нулями —
           блок всё равно отсчитает ровно столько байт, сколько объявлено
           в счётчике MX_OUTPUT_CNT. */
        uint32_t word = 0;
        for (int b = 0; b < 4 && i < len; b++, i++)
            word |= (uint32_t)buf[i] << (8 * b);

        mmio_write32(base + QUP_OUT_FIFO_BASE, word);
    }
    dsb_sy();
    return 1;
}

/* Забрать прочитанное из входной очереди. */
static int pull_in(uint64_t base, uint8_t *buf, int len) {
    int i = 0;
    while (i < len) {
        if (!wait_in_data(base)) return 0;

        uint32_t word = mmio_read32(base + QUP_IN_FIFO_BASE);
        for (int b = 0; b < 4 && i < len; b++, i++)
            buf[i] = (uint8_t)((word >> (8 * b)) & 0xFF);
    }
    return 1;
}

/* Подробный снимок блока.
 *
 * Печатается на каждом заметном шаге, а не только при отказе. Круг
 * отладки на этом аппарате длинный — сборка, перезагрузка, чтение
 * журнала, — и дешевле напечатать лишнее, чем догадываться и ходить
 * второй раз. Каждый регистр здесь отвечает на свой вопрос:
 *
 *   STATE  работает ли блок вообще (младшие два бита) и достоверно ли
 *          его состояние
 *   OP     чего он ждёт: заполнена ли очередь передачи, есть ли данные в
 *          очереди приёма, отработал ли он объявленные счётчики
 *   ERR    сорвалось ли что-то на уровне блока
 *   I2C    сорвалось ли что-то на шине: неподтверждение, потеря
 *          арбитража, ошибка шины
 *   OCNT   сколько слов сейчас в очереди передачи
 *   ICNT   сколько слов накопилось в очереди приёма
 */
void i2c_qup_dump(uint64_t base, const char *where) {
    early_con_puts("I2C ");
    early_con_puts(where);
    early_con_puts(": ST ");
    early_con_hex((uint64_t)mmio_read32(base + QUP_STATE));
    early_con_puts(" OP ");
    early_con_hex((uint64_t)mmio_read32(base + QUP_OPERATIONAL));
    early_con_puts("\n     ERR ");
    early_con_hex((uint64_t)mmio_read32(base + QUP_ERROR_FLAGS));
    early_con_puts(" I2C ");
    early_con_hex((uint64_t)mmio_read32(base + QUP_I2C_STATUS));
    early_con_puts("\n     OCNT ");
    early_con_hex((uint64_t)mmio_read32(base + QUP_OUT_FIFO_CNT));
    early_con_puts(" ICNT ");
    early_con_hex((uint64_t)mmio_read32(base + QUP_IN_FIFO_CNT));
    early_con_puts("\n");
}

/* Последнее состояние шины — чтобы показать его при отказе.
 *
 * Это самый ценный из доступных признаков: он разделяет два случая,
 * которые снаружи выглядят одинаково ("контроллер не ответил"). Если в
 * состоянии стоит признак неподтверждения, значит шина работает и
 * передача дошла до устройства, а вот устройство промолчало — оно либо
 * обесточено, либо его там нет. Любая другая ошибка (потеря арбитража,
 * сбой шины, переполнение) указывает не на устройство, а на нас. */
static uint32_t g_last_status = 0;

uint32_t i2c_qup_last_status(void) { return g_last_status; }

/* Проверить, не сорвалась ли транзакция на шине. */
static int check_bus(uint64_t base) {
    uint32_t st = mmio_read32(base + QUP_I2C_STATUS);
    g_last_status = st;
    if (st & QUP_I2C_FAILED_MASK) {
        /* Сбрасываем флаги, иначе они останутся висеть и следующая
           транзакция сразу же будет считаться ошибочной. */
        mmio_write32(base + QUP_I2C_STATUS, 0);
        return 0;
    }
    return 1;
}

/* Транзакция «записать, затем прочитать через повторный старт».
 *
 * Именно такая форма нужна тачскрину: сначала передаём номер регистра,
 * потом без освобождения шины читаем его содержимое. Отпускать шину между
 * этими двумя действиями нельзя — устройство забудет выбранный регистр.
 *
 * rlen == 0 превращает вызов в обычную запись, wlen == 0 — в чистое чтение.
 * Возвращает 1 при успехе.
 */
int i2c_qup_xfer(uint64_t base, uint8_t addr,
                  const uint8_t *wbuf, int wlen,
                  uint8_t *rbuf, int rlen) {
    if (wlen < 0 || rlen < 0 || wlen > XFER_MAX || rlen > XFER_MAX) return 0;
    if (wlen == 0 && rlen == 0) return 0;

    /* Собираем «программу» транзакции целиком, чтобы затем залить её в
       очередь одним потоком. */
    uint8_t out[XFER_MAX + 8];
    int n = 0;

    if (wlen > 0) {
        out[n++] = TAG_START;
        out[n++] = (uint8_t)(addr << 1);            /* младший бит 0 — запись */
        /* Если после записи будет чтение, стоп ставить нельзя: нужен
           повторный старт, иначе устройство потеряет выбранный регистр. */
        out[n++] = (rlen > 0) ? TAG_DATAWR : TAG_DATAWR_STOP;
        out[n++] = (uint8_t)wlen;
        for (int i = 0; i < wlen; i++) out[n++] = wbuf[i];
    }

    if (rlen > 0) {
        out[n++] = TAG_START;
        out[n++] = (uint8_t)((addr << 1) | 1);      /* младший бит 1 — чтение */
        out[n++] = TAG_DATARD_STOP;
        out[n++] = (uint8_t)rlen;
    }

    /* Сама посылка побайтно. Метки задают, что блок должен сделать:
       0x81 — старт с адресом, 0x82/0x83 — передать столько-то байт (без
       остановки и с ней), 0x87 — принять столько-то и остановиться.
       Видя эти байты, можно проверить программу передачи глазами, не
       гадая, что мы на самом деле отправили. */
    early_con_puts("I2C posylka:");
    for (int i = 0; i < n; i++) {
        early_con_puts(" ");
        early_con_hex8(out[i]);
    }
    early_con_puts("\n");

    /* Блок должен объявить себя ведущим — иначе в работу он не пойдёт. */
    if (!wait_master(base)) {
        i2c_qup_dump(base, "ne stal vedushchim");
        return 0;
    }

    if (!set_state(base, QUP_RESET_STATE)) return 0;

    /*
     * ПОРЯДОК ПОВТОРЯЕТ ДРАЙВЕР ЯДРА, и каждый шаг здесь на своём месте.
     *
     * Сначала режим: обе стороны работают очередями, поэтому блочные
     * счётчики обнуляются, а в режиме включается переупаковка байтов.
     * Потом счётчики очередей и настройка. И только затем пуск.
     *
     * Самое неочевидное — что очередь заполняется В ПАУЗЕ, а не в работе.
     * Блок, переведённый в работу, начинает передачу немедленно, и
     * досыпать ему данные уже поздно: он успевает уйти вперёд. Поэтому
     * после пуска его сразу ставят на паузу, наполняют очередь целиком и
     * снова пускают — тогда вся посылка уходит одним куском.
     */
    mmio_write32(base + QUP_MX_OUTPUT_CNT, 0);
    mmio_write32(base + QUP_MX_INPUT_CNT, 0);
    mmio_write32(base + QUP_IO_MODE, QUP_IO_MODE_PACK_EN | QUP_IO_MODE_UNPACK_EN);

    uint32_t cfg = QUP_CONFIG_MINI_CORE_I2C | QUP_CONFIG_N_8BIT;
    mmio_write32(base + QUP_MX_WRITE_CNT, (uint32_t)n);
    if (rlen > 0) mmio_write32(base + QUP_MX_READ_CNT, (uint32_t)rlen);
    else          cfg |= QUP_CONFIG_NO_INPUT;
    mmio_write32(base + QUP_CONFIG, cfg);
    dsb_sy();

    if (!set_state(base, QUP_RUN_STATE)) {
        i2c_qup_dump(base, "ne poshyol v rabotu");
        return 0;
    }

    /* Такт шины задаётся здесь, уже в работе — так делает драйвер ядра. */
    mmio_write32(base + QUP_I2C_CLK_CTL, g_clk_ctl);
    dsb_sy();

    if (!set_state(base, QUP_PAUSE_STATE)) {
        i2c_qup_dump(base, "ne vstal na pauzu");
        return 0;
    }

    if (!push_out(base, out, n)) {
        i2c_qup_dump(base, "ochered ne osvobodilas");
        uart_write("i2c: ochered vyvoda ne osvobodilas\n");
        set_state(base, QUP_RESET_STATE);
        return 0;
    }

    /* Очередь полна — отпускаем блок, и он отыгрывает всю посылку. */
    if (!set_state(base, QUP_RUN_STATE)) {
        i2c_qup_dump(base, "ne vozobnovil rabotu");
        return 0;
    }

    i2c_qup_dump(base, "posle vydachi");

    if (rlen > 0) {
        if (!pull_in(base, rbuf, rlen)) {
            i2c_qup_dump(base, "otvet ne prishyol");
            set_state(base, QUP_RESET_STATE);
            return 0;
        }
        i2c_qup_dump(base, "otvet prinyat");
    } else {
        /* Для чистой записи дожидаемся, пока блок отыграет всё, что мы ему
           дали: иначе следующая транзакция начнётся поверх незавершённой. */
        int done = 0;
        for (int i = 0; i < POLL_LIMIT; i++) {
            if (mmio_read32(base + QUP_OPERATIONAL) & QUP_MX_OUTPUT_DONE) { done = 1; break; }
        }
        if (!done) { set_state(base, QUP_RESET_STATE); return 0; }
    }

    int ok = check_bus(base);
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
