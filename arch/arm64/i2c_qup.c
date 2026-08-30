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
#define QUP_MX_OUTPUT_CNT       0x100
#define QUP_OUT_FIFO_BASE       0x110
#define QUP_MX_INPUT_CNT        0x200
#define QUP_IN_FIFO_BASE        0x218
#define QUP_I2C_CLK_CTL         0x400
#define QUP_I2C_STATUS          0x404

/* Регистр состояния блока */
#define QUP_STATE_MASK          0x3
#define QUP_STATE_VALID         (1u << 2)
#define QUP_RESET_STATE         0
#define QUP_RUN_STATE           1
#define QUP_PAUSE_STATE         3

/* Настройка ядра */
#define QUP_CONFIG_MINI_CORE_I2C (2u << 8)
#define QUP_CONFIG_N_8BIT        7u        /* число бит в слове минус один */

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
    return wait_state_valid(base);
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
    /* Полный сброс блока: после загрузчика он может быть в любом
       состоянии, а нам нужно предсказуемое. */
    mmio_write32(base + QUP_SW_RESET, 1);
    dsb_sy();

    /* После сброса блок сам приходит в состояние RESET; дожидаемся, пока
       он сообщит, что состояние достоверно. Если не дождались — блок не
       тактируется, и дальше идти бессмысленно. */
    if (!wait_state_valid(base)) {
        uart_write("i2c: blok ne otvechaet posle sbrosa, base=");
        uart_write_hex(base);
        uart_write(" (ne vklyuchen takt?)\n");
        return 0;
    }

    if (!set_state(base, QUP_RESET_STATE)) return 0;

    /* Ядро работает как I2C, слово — восемь бит. */
    mmio_write32(base + QUP_CONFIG, QUP_CONFIG_MINI_CORE_I2C | QUP_CONFIG_N_8BIT);

    /* Простые очереди с упаковкой байтов в слова. */
    mmio_write32(base + QUP_IO_MODE, QUP_IO_MODE_PACK_EN | QUP_IO_MODE_UNPACK_EN);

    /* Делитель скорости шины. Формула аппаратуры: за один период на шине
       блок отсчитывает (делитель + 3) * 2 своих тактов. Отсюда обратное
       выражение. Верхние восемь бит — задержка удержания данных, значение 3
       стандартное для fast mode. */
    uint32_t fs_div = (core_hz / bus_hz) / 2;
    fs_div = (fs_div > 3) ? (fs_div - 3) : 0;
    if (fs_div > 0xFF) fs_div = 0xFF;
    mmio_write32(base + QUP_I2C_CLK_CTL, (3u << 8) | fs_div);

    /* Ошибки шины хотим видеть все. */
    mmio_write32(base + QUP_ERROR_FLAGS_EN, 0x7);
    mmio_write32(base + QUP_OPERATIONAL_MASK, 0);
    dsb_sy();

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

/* Проверить, не сорвалась ли транзакция на шине. */
static int check_bus(uint64_t base) {
    uint32_t st = mmio_read32(base + QUP_I2C_STATUS);
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

    /* Счётчики: сколько байт блок должен выдать наружу и сколько принять.
       Старший бит разрешает менять их, не покидая рабочего состояния. */
    if (!set_state(base, QUP_RESET_STATE)) return 0;

    mmio_write32(base + QUP_MX_OUTPUT_CNT, QUP_MX_CONFIG_DURING_RUN | (uint32_t)n);
    mmio_write32(base + QUP_MX_INPUT_CNT,
                 QUP_MX_CONFIG_DURING_RUN | (uint32_t)(rlen > 0 ? rlen : 0));
    dsb_sy();

    if (!set_state(base, QUP_RUN_STATE)) return 0;

    if (!push_out(base, out, n)) {
        uart_write("i2c: ochered vyvoda ne osvobodilas\n");
        set_state(base, QUP_RESET_STATE);
        return 0;
    }

    if (rlen > 0) {
        if (!pull_in(base, rbuf, rlen)) {
            /* Молчание на чтении — самый частый симптом отсутствующего или
               незапитанного устройства. */
            set_state(base, QUP_RESET_STATE);
            return 0;
        }
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
