/* arch/arm64/timer.c — hal_time_* для ARM64.
 *
 * Монотонное время: ARM Generic Timer. Это единственный таймер, который
 * есть ВЕЗДЕ — от QEMU virt до любого телефона, — и он читается обычной
 * инструкцией mrs, без единого обращения к MMIO и без прерываний.
 * CNTFRQ_EL0 говорит, сколько тиков в секунде (у QEMU 62.5 МГц),
 * CNTPCT_EL0 — сам счётчик.
 *
 * Календарное время: PL031 RTC — регистр RTCDR отдаёт секунды Unix-эпохи
 * одним 32-битным словом. Разбор в дату/время — обычная арифметика
 * григорианского календаря (алгоритм Хиннанта), никакого железа.
 */
#include "hal_time.h"
#include "arm64.h"
#include "fdt.h"

#define RTC_DR  0x00    /* PL031: секунды с 1970-01-01 */

static uint64_t g_freq_hz  = 62500000;   /* значение QEMU — на случай CNTFRQ_EL0 == 0 */
static uint64_t g_start_tick = 0;

/* Календарные часы. Ищутся в дереве устройств и на телефоне не находятся:
 * PL031 — деталь машины QEMU, а не общая принадлежность ARM.
 *
 * Раньше их адрес был зашит в код и читался на любой плате. Это тихо
 * работало в эмуляторе и намертво вешало телефон: чтение регистра, на
 * который никто не отвечает, не возвращает мусор — процессор ждёт ответа
 * с шины, которого не будет. А вызывается это из строки состояния, то
 * есть при отрисовке самого первого кадра. */
static uint64_t g_rtc_base = 0;
static int      g_rtc_ok   = 0;

/* Когда часов нет, время считаем от запуска. Показания будут неверными,
 * зато монотонными, и строка состояния не превращается в загадку. */
#define FALLBACK_EPOCH  1767225600u   /* 2026-01-01 00:00:00 UTC */

static inline uint64_t read_cntpct(void) {
    uint64_t t;
    /* isb обязателен: чтение счётчика может быть спекулятивно вынесено
       вперёд относительно предшествующего кода, и замер времени поедет. */
    __asm__ volatile ("isb" ::: "memory");
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
    return t;
}

void hal_time_init(void) {
    uint64_t f;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    if (f != 0) g_freq_hz = f;
    g_start_tick = read_cntpct();

    g_rtc_ok = 0;
    if (fdt_valid()) {
        fdt_node_t node;
        uint64_t addr = 0;
        if (fdt_find_compatible("arm,pl031", &node) &&
            fdt_reg(&node, 0, &addr, 0) && addr) {
            g_rtc_base = addr;
            g_rtc_ok   = 1;
        }
    } else {
        /* Дерева нет вовсе — значит мы почти наверняка под эмулятором,
           который его и не дал. Пробуем по старой памяти. */
        g_rtc_base = VIRT_RTC_BASE;
        g_rtc_ok   = 1;
    }
}

uint64_t hal_time_ms(void) {
    uint64_t delta = read_cntpct() - g_start_tick;
    /* Сначала делим на 1000-ю долю частоты, а не умножаем на 1000: при
       частоте 62.5 МГц умножение переполнило бы 64 бита только через
       9000 лет, но на телефонах встречаются счётчики и на 19.2 МГц с
       другими масштабами — так надёжнее и без переполнения вообще. */
    uint64_t ticks_per_ms = g_freq_hz / 1000;
    if (ticks_per_ms == 0) ticks_per_ms = 1;
    return delta / ticks_per_ms;
}

void hal_time_delay_ms(uint32_t ms) {
    uint64_t deadline = hal_time_ms() + ms;
    while (hal_time_ms() < deadline)
        __asm__ volatile ("yield");
}

/* Дата из числа дней с эпохи. Алгоритм Хиннанта: считает "эры" по 400 лет,
 * в которых календарь повторяется ровно, поэтому обходится без таблиц
 * длин месяцев и без спецслучаев для високосных лет. */
static void civil_from_days(int64_t z, int *year, int *month, int *day) {
    z += 719468;                                  /* сдвиг эпохи к 0000-03-01 */
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);                       /* день в эре   */
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* год в эре  */
    int64_t  y   = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            /* день в году  */
    uint64_t mp  = (5 * doy + 2) / 153;                                /* месяц с марта */
    uint64_t d   = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m   = mp < 10 ? mp + 3 : mp - 9;

    *year  = (int)(y + (m <= 2));
    *month = (int)m;
    *day   = (int)d;
}

void hal_time_rtc(int *year, int *month, int *day, int *hour, int *min, int *sec) {
    uint32_t epoch = g_rtc_ok ? mmio_read32(g_rtc_base + RTC_DR)
                              : (FALLBACK_EPOCH + (uint32_t)(hal_time_ms() / 1000));

    int64_t days = (int64_t)(epoch / 86400);
    uint32_t rem = epoch % 86400;

    civil_from_days(days, year, month, day);
    *hour = (int)(rem / 3600);
    *min  = (int)((rem % 3600) / 60);
    *sec  = (int)(rem % 60);
}
