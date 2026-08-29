/* arch/x86/pit.c — миллисекундное время на x86.
 *
 * Зачем: CMOS RTC из arch/x86/rtc.c умеет только секунды, а тач-интерфейс
 * без миллисекунд не сделать — тап от свайпа отличается не столько длиной
 * жеста, сколько его длительностью, и на секундном разрешении это
 * неразличимо.
 *
 * Как: считаем такты TSC (rdtsc — счётчик тактов процессора, монотонный
 * и почти бесплатный), а сколько их приходится на миллисекунду, узнаём
 * один раз при старте, откалибровавшись по таймеру PIT 8253. PIT тикает
 * на фиксированной частоте 1.193182 МГц, заданной кварцем ещё в IBM PC,
 * и потому одинаков на любой машине и в любом эмуляторе.
 */
#include <stdint.h>
#include "ports.h"

#define PIT_FREQ_HZ    1193182u
#define PIT_CH2_DATA   0x42
#define PIT_CMD        0x43
#define PIT_GATE_PORT  0x61     /* бит 0 — гейт канала 2, бит 5 — его выход */

static uint64_t g_tsc_per_ms = 0;
static uint64_t g_tsc_start  = 0;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Канал 2 PIT выбран намеренно: канал 0 занят системным таймером (IRQ0),
 * а канал 2 исторически подключён к динамику и потому свободен — им
 * можно спокойно мерить интервалы, ничего не сломав. */
void pit_calibrate(void) {
    const uint32_t ticks = PIT_FREQ_HZ / 100;   /* 10 мс в тиках PIT */

    uint8_t gate = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, (uint8_t)((gate & ~0x02) | 0x01));  /* гейт вкл, динамик выкл */

    /* канал 2, сначала младший потом старший байт, режим 0 (одиночный
       отсчёт), двоичный счёт */
    outb(PIT_CMD, 0xB0);
    outb(PIT_CH2_DATA, (uint8_t)(ticks & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(ticks >> 8));

    /* перезапуск отсчёта: дёрнуть гейт */
    gate = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, (uint8_t)(gate & ~0x01));
    outb(PIT_GATE_PORT, (uint8_t)(gate | 0x01));

    uint64_t start = rdtsc();
    /* бит 5 порта 0x61 поднимается, когда канал 2 досчитал до нуля */
    uint32_t guard = 0;
    while (!(inb(PIT_GATE_PORT) & 0x20)) {
        if (++guard > 100000000u) break;   /* страховка от зависания на странном железе */
    }
    uint64_t end = rdtsc();

    uint64_t per_10ms = end - start;
    g_tsc_per_ms = per_10ms / 10;
    if (g_tsc_per_ms == 0) g_tsc_per_ms = 1;   /* не делить на ноль, если калибровка не удалась */

    g_tsc_start = rdtsc();
}

uint64_t pit_uptime_ms(void) {
    if (g_tsc_per_ms == 0) return 0;
    return (rdtsc() - g_tsc_start) / g_tsc_per_ms;
}
