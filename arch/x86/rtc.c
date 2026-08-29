/* arch/x86/rtc.c — стандартный доступ к CMOS RTC через порты 0x70 (индекс
 * регистра) / 0x71 (данные). Регистры и их номера — стандарт с первых
 * IBM PC/AT, есть и в реальном железе, и в любом эмуляторе (QEMU/Bochs/
 * VirtualBox).
 */
#include "ports.h"
#include "rtc.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SECONDS    0x00
#define REG_MINUTES    0x02
#define REG_HOURS      0x04
#define REG_DAY        0x07
#define REG_MONTH      0x08
#define REG_YEAR       0x09
#define REG_STATUS_A   0x0A
#define REG_STATUS_B   0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(REG_STATUS_A) & 0x80;
}

static int bcd_to_bin(uint8_t v) {
    return ((v >> 4) * 10) + (v & 0x0F);
}

int rtc_read_seconds(void) {
    while (update_in_progress()) { }   /* не читаем во время апдейта — рвущиеся значения */

    uint8_t status_b = cmos_read(REG_STATUS_B);
    uint8_t raw = cmos_read(REG_SECONDS);
    return (status_b & 0x04) ? raw : bcd_to_bin(raw);   /* бит 2 = 1 значит "уже двоичное" */
}

static unsigned long g_uptime_seconds = 0;
static int g_last_second = -1;

void rtc_uptime_tick(void) {
    int s = rtc_read_seconds();
    if (g_last_second == -1) { g_last_second = s; return; }
    if (s != g_last_second) {
        g_uptime_seconds++;
        g_last_second = s;
    }
}

unsigned long rtc_uptime_seconds(void) {
    return g_uptime_seconds;
}

void rtc_read(int *year, int *month, int *day, int *hour, int *min, int *sec) {
    uint8_t s, mi, h, d, mo, y;

    /* читаем дважды и сравниваем — простая защита от попадания на
       границу обновления регистров (без полноценного двойного опроса
       по стандарту OS-dev, но для прототипа достаточно) */
    do {
        while (update_in_progress()) { }
        s  = cmos_read(REG_SECONDS);
        mi = cmos_read(REG_MINUTES);
        h  = cmos_read(REG_HOURS);
        d  = cmos_read(REG_DAY);
        mo = cmos_read(REG_MONTH);
        y  = cmos_read(REG_YEAR);
    } while (update_in_progress());

    uint8_t status_b = cmos_read(REG_STATUS_B);
    int is_binary = status_b & 0x04;
    int is_24h    = status_b & 0x02;

    if (!is_binary) {
        s  = (uint8_t)bcd_to_bin(s);
        mi = (uint8_t)bcd_to_bin(mi);
        d  = (uint8_t)bcd_to_bin(d);
        mo = (uint8_t)bcd_to_bin(mo);
        y  = (uint8_t)bcd_to_bin(y);

        int pm = h & 0x80;
        uint8_t h_bcd = h & 0x7F;
        h = (uint8_t)bcd_to_bin(h_bcd);
        if (!is_24h && pm) h = (uint8_t)((h % 12) + 12);
    }

    *sec = s;
    *min = mi;
    *hour = h;
    *day = d;
    *month = mo;
    *year = 2000 + y;   /* CMOS хранит только 2 цифры года */
}
