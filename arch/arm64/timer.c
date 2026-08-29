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

#define RTC_DR  0x00    /* PL031: секунды с 1970-01-01 */

static uint64_t g_freq_hz  = 62500000;   /* значение QEMU — на случай CNTFRQ_EL0 == 0 */
static uint64_t g_start_tick = 0;

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
    uint32_t epoch = mmio_read32(VIRT_RTC_BASE + RTC_DR);

    int64_t days = (int64_t)(epoch / 86400);
    uint32_t rem = epoch % 86400;

    civil_from_days(days, year, month, day);
    *hour = (int)(rem / 3600);
    *min  = (int)((rem % 3600) / 60);
    *sec  = (int)(rem % 60);
}
