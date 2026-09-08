/* arch/arm64/i2c_bitbang.c — I2C, выдаваемый вручную на обычных выводах.
 *
 * ЗАЧЕМ ЭТО НУЖНО, КОГДА ЕСТЬ АППАРАТНЫЙ КОНТРОЛЛЕР.
 *
 * Затем, что аппаратный контроллер и микросхема на шине — две разные
 * гипотезы, а снаружи их отказ выглядит одинаково: "устройство не
 * ответило". Пока обе проверяются одним и тем же путём, различить их
 * нечем. Здесь шина выдаётся руками, вообще без QUP: если по этому пути
 * ответ есть, значит микросхема жива, запитана и адрес верен, а виноват
 * контроллер. Если ответа нет и здесь — дело в питании или в самой
 * микросхеме, и настраивать QUP дальше бессмысленно.
 *
 * И это не только измерительный прибор. Оно работает: 63 байта пакета
 * касаний на 100 кГц занимают около шести миллисекунд, при опросе
 * шестьдесят раз в секунду это треть кадра. Небыстро, но тачскрин на этом
 * живёт, поэтому драйвер тачскрина умеет ходить и этим путём тоже.
 *
 * КАК ЭТО ВОЗМОЖНО НА TLMM. У I2C обе линии с открытым стоком: устройства
 * умеют только притягивать линию к нулю, а в единицу её возвращают внешние
 * резисторы. У вывода TLMM открытого стока нет, но он и не нужен —
 * достаточно переключать вывод между "выход, ноль" и "вход": во втором
 * состоянии вывод отпущен, и линию поднимает резистор. Ровно это делают
 * line_low и line_release.
 *
 * ВРЕМЯ. Задержки отсчитываются по системному счётчику (cntvct_el0), а не
 * пустыми оборотами: частота ядра здесь неизвестна и меняется, а счётчик
 * идёт от кварца 19.2 МГц и не зависит ни от чего.
 */
#include "arm64.h"
#include "boards/board.h"

static int g_sda = -1;
static int g_scl = -1;
static uint32_t g_half_us = 5;      /* половина периода: 5 мкс ~ 100 кГц */

/* ---- Время ---- */

static inline uint64_t bb_cnt(void) {
    uint64_t v;
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static void bb_delay_us(uint32_t us) {
    uint64_t f;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) f = 19200000;
    uint64_t deadline = bb_cnt() + (f * (uint64_t)us) / 1000000u + 1;
    while (bb_cnt() < deadline) { }
}

/* ---- Линии ---- */

static void line_low(int gpio)     { tlmm_gpio_output(gpio, 0); }
static void line_release(int gpio) { tlmm_gpio_input(gpio, 0); }
static int  line_read(int gpio)    { return tlmm_gpio_get(gpio); }

/* Отпустить такт и дождаться, пока линия действительно поднимется.
 *
 * Ведомому разрешено удерживать такт в нуле, пока он не готов — это
 * называется растягиванием такта, и FocalTech этим пользуется после
 * сброса. Если не ждать, а просто идти дальше, обмен рассыплется. */
static int scl_high(void) {
    line_release(g_scl);
    for (int i = 0; i < 1000; i++) {
        if (line_read(g_scl)) { bb_delay_us(g_half_us); return 1; }
        bb_delay_us(1);
    }
    return 0;   /* такт зажат — шина мертва */
}

static void scl_low(void) {
    line_low(g_scl);
    bb_delay_us(g_half_us);
}

/* ---- Элементарные посылки ---- */

static void bb_start(void) {
    line_release(g_sda);
    scl_high();
    line_low(g_sda);            /* спад данных при высоком такте */
    bb_delay_us(g_half_us);
    scl_low();
}

static void bb_restart(void) {
    line_release(g_sda);
    bb_delay_us(g_half_us);
    scl_high();
    line_low(g_sda);
    bb_delay_us(g_half_us);
    scl_low();
}

static void bb_stop(void) {
    line_low(g_sda);
    bb_delay_us(g_half_us);
    scl_high();
    line_release(g_sda);        /* подъём данных при высоком такте */
    bb_delay_us(g_half_us);
}

/* Выдать байт. Возвращает 1, если ведомый подтвердил приём. */
static int bb_write_byte(uint8_t v) {
    for (int i = 7; i >= 0; i--) {
        if (v & (1u << i)) line_release(g_sda);
        else               line_low(g_sda);
        bb_delay_us(g_half_us);
        if (!scl_high()) return 0;
        scl_low();
    }

    /* Девятый такт — подтверждение. Линию отпускаем, её тянет ведомый. */
    line_release(g_sda);
    bb_delay_us(g_half_us);
    if (!scl_high()) return 0;
    int ack = (line_read(g_sda) == 0);
    scl_low();
    return ack;
}

/* Принять байт. ack — подтверждать ли приём (последний байт не
   подтверждают, иначе ведомый продолжит выдавать). */
static uint8_t bb_read_byte(int ack) {
    uint8_t v = 0;

    line_release(g_sda);
    for (int i = 7; i >= 0; i--) {
        bb_delay_us(g_half_us);
        if (!scl_high()) return v;
        if (line_read(g_sda)) v |= (1u << i);
        scl_low();
    }

    if (ack) line_low(g_sda);
    else     line_release(g_sda);
    bb_delay_us(g_half_us);
    scl_high();
    scl_low();
    line_release(g_sda);
    return v;
}

/* ---- Внешний интерфейс ---- */

/* Освободить зависшую шину.
 *
 * Если ведомый остался посреди выдачи байта, он держит данные в нуле и
 * старта не увидит — шина заперта. Лечится девятью тактами вхолостую: за
 * них ведомый досчитает свой байт, отпустит линию, и дальше можно выдать
 * стоп. */
static void bb_recover(void) {
    line_release(g_sda);
    for (int i = 0; i < 9 && line_read(g_sda) == 0; i++) {
        scl_low();
        scl_high();
    }
    bb_stop();
}

int i2c_bb_init(int sda_gpio, int scl_gpio, uint32_t bus_hz) {
    g_sda = sda_gpio;
    g_scl = scl_gpio;

    /* Половина периода в микросекундах. Ниже двух не опускаемся: сама
       смена настройки вывода занимает заметное время, и попытка выжать
       больше даст только несимметричный такт. */
    uint32_t half = (bus_hz > 0) ? (500000u / bus_hz) : 5u;
    g_half_us = (half < 2) ? 2u : half;

    line_release(g_sda);
    line_release(g_scl);
    bb_delay_us(10);

    int sda = line_read(g_sda);
    int scl = line_read(g_scl);

    early_con_puts("BB: linii SDA=");
    early_con_puts(sda ? "1" : "0");
    early_con_puts(" SCL=");
    early_con_puts(scl ? "1" : "0");
    early_con_puts(" polovina takta ");
    early_con_hex((uint64_t)g_half_us);
    early_con_puts(" mks\n");

    if (!sda || !scl) {
        bb_recover();
        sda = line_read(g_sda);
        scl = line_read(g_scl);
        early_con_puts("BB: posle osvobozhdeniya SDA=");
        early_con_puts(sda ? "1" : "0");
        early_con_puts(" SCL=");
        early_con_puts(scl ? "1" : "0");
        early_con_puts("\n");
    }

    return (sda && scl);
}

/* Есть ли кто-нибудь по адресу. Старт, адрес, смотрим подтверждение. */
int i2c_bb_probe(uint8_t addr) {
    bb_start();
    int ack = bb_write_byte((uint8_t)(addr << 1));
    bb_stop();
    return ack;
}

/* Обойти всю шину и напечатать, кто на ней есть.
 *
 * Самый прямой ответ на вопрос "а тачскрин вообще жив". Ответ по адресу
 * 0x38 означает, что микросхема запитана и отвечает; пустая шина — что
 * питания нет или сенсор не тот. Возвращает число найденных устройств. */
int i2c_bb_scan(void) {
    int found = 0;
    early_con_puts("BB: obhod shiny:");
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_bb_probe(a)) {
            early_con_puts(" ");
            early_con_hex8(a);
            found++;
        }
    }
    if (!found) early_con_puts(" pusto");
    early_con_puts("\n");
    return found;
}

int i2c_bb_xfer(uint8_t addr, const uint8_t *wbuf, int wlen,
                uint8_t *rbuf, int rlen) {
    if (g_sda < 0 || g_scl < 0) return 0;

    if (wlen > 0) {
        bb_start();
        if (!bb_write_byte((uint8_t)(addr << 1))) { bb_stop(); return 0; }
        for (int i = 0; i < wlen; i++)
            if (!bb_write_byte(wbuf[i])) { bb_stop(); return 0; }
    }

    if (rlen > 0) {
        if (wlen > 0) bb_restart();
        else          bb_start();

        if (!bb_write_byte((uint8_t)((addr << 1) | 1))) { bb_stop(); return 0; }
        for (int i = 0; i < rlen; i++)
            rbuf[i] = bb_read_byte(i < rlen - 1);
    }

    bb_stop();
    return 1;
}

int i2c_bb_read_regs(uint8_t addr, uint8_t reg, uint8_t *out, int len) {
    return i2c_bb_xfer(addr, &reg, 1, out, len);
}

int i2c_bb_write_reg(uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_bb_xfer(addr, buf, 2, 0, 0);
}
