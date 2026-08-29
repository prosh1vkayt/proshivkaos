/* hal/hal_time_x86.c — hal_time_* для x86-сборки.
 * Монотонное время — из arch/x86/pit.c (TSC, откалиброванный по PIT),
 * календарное — из arch/x86/rtc.c (CMOS). Парный файл для ARM64 —
 * arch/arm64/timer.c, где то же самое делают Generic Timer и PL031.
 */
#include "hal_time.h"

void     pit_calibrate(void);      /* arch/x86/pit.c */
uint64_t pit_uptime_ms(void);
void     rtc_read(int *year, int *month, int *day, int *hour, int *min, int *sec);

void hal_time_init(void) {
    pit_calibrate();
}

uint64_t hal_time_ms(void) {
    return pit_uptime_ms();
}

void hal_time_rtc(int *year, int *month, int *day, int *hour, int *min, int *sec) {
    rtc_read(year, month, day, hour, min, sec);
}

void hal_time_delay_ms(uint32_t ms) {
    uint64_t deadline = hal_time_ms() + ms;
    while (hal_time_ms() < deadline)
        __asm__ volatile ("pause");
}
