/* arch/arm64/reboot_msm.c — перезагрузка по собственной воле.
 *
 * ЗАЧЕМ. Круг отладки был такой: собрать образ, загрузить, дождаться,
 * ВРУЧНУЮ вернуть телефон в загрузчик долгим удержанием кнопок, забрать
 * журнал. Ручной шаг посередине означал, что круг не замкнуть — нужен
 * человек у стола на каждом заходе.
 *
 * Загрузчик при старте читает из служебной памяти на кристалле слово,
 * объясняющее, зачем его перезагрузили. Записав туда нужное значение и
 * вызвав сброс, система возвращается прямо в режим загрузчика.
 *
 * ПОЧЕМУ ПРОШЛАЯ ПОПЫТКА НЕ РАБОТАЛА. Сброс делался сторожевым таймером:
 * это было единственное, что нам тогда было доступно. Аппарат после
 * такого вызова подвисал и в загрузчик не возвращался.
 *
 * Теперь понятно, чего не хватало. Сброс на этой платформе делается не
 * процессором, а МИКРОСХЕМОЙ ПИТАНИЯ: процессор всего лишь отпускает
 * линию удержания питания (PS_HOLD), а что делать дальше — выключиться
 * совсем или перезапуститься — решает микросхема по своей настройке. Не
 * настроив её, отпускать линию бессмысленно, и аппарат замирает.
 *
 * Настроить её мы раньше не могли: не было доступа к шине SPMI. Теперь
 * есть, и последовательность повторяет загрузчик:
 *
 *   запретить сброс по PS_HOLD -> подождать -> задать вид сброса ->
 *   разрешить -> отпустить линию.
 *
 * Вид сброса — тёплый: он не обесточивает кристалл, и служебная память с
 * записанным признаком переживает перезапуск. Холодный стёр бы её вместе
 * с причиной, ради которой всё и затевалось.
 */
#include "arm64.h"
#include "boards/board.h"
#include "hal_time.h"

#ifdef BOARD_IMEM_RESTART_REASON

/* Узел управления питанием внутри микросхемы */
#define PON_BASE                0x0800
#define PON_PS_HOLD_RESET_CTL   (PON_BASE + 0x5A)
#define PON_PS_HOLD_RESET_CTL2  (PON_BASE + 0x5B)
#define PON_S2_RESET_EN         0x80

/* Виды сброса, которые понимает микросхема питания */
#define PON_WARM_RESET          0x01
#define PON_HARD_RESET          0x07

/* Смещения регистров сторожевого таймера — как в драйвере ядра. */
#define WDOG_RST        0x04
#define WDOG_EN         0x08
#define WDOG_BARK_TIME  0x10
#define WDOG_BITE_TIME  0x14
#define BITE_TICKS      0x2000
#define BARK_TICKS      0x1000

/* Настроить микросхему питания на перезапуск, а не на выключение. */
static int pon_configure(uint8_t reset_type) {
    /* Запрет перед сменой настройки обязателен: менять вид сброса на
       взведённой защите микросхема не даёт. */
    if (!spmi_write(0, PON_PS_HOLD_RESET_CTL2, 0)) return 0;
    hal_time_delay_us(300);

    if (!spmi_write(0, PON_PS_HOLD_RESET_CTL, reset_type)) return 0;
    if (!spmi_write(0, PON_PS_HOLD_RESET_CTL2, PON_S2_RESET_EN)) return 0;

    return 1;
}

/* reason — что сказать загрузчику: BOARD_REBOOT_BOOTLOADER или 0 для
 * обычной загрузки. Возврата не бывает. */
void msm_reboot(uint32_t reason) {
    early_con_puts("REBOOT: prichina ");
    early_con_hex32(reason);
    early_con_puts("\n");

    /* Дать уйти тому, что уже в пути по проводу. */
    hal_time_delay_ms(50);

    mmio_write32(BOARD_IMEM_RESTART_REASON, reason);
    dsb_sy();

    if (pon_configure(PON_WARM_RESET)) {
        /* Отпускаем линию удержания питания. Дальше всё делает
           микросхема питания. */
        mmio_write32(BOARD_PSHOLD_ADDR, 0);
        dsb_sy();
        hal_time_delay_ms(200);
        early_con_puts("REBOOT: PS_HOLD otpushchen, a my zhivy — probuem storozha\n");
    } else {
        early_con_puts("REBOOT: mikroshema pitaniya ne nastroilas\n");
    }

    /* Запасной путь: сторожевой таймер. Сам по себе он аппарат подвешивал,
       но хуже уже не будет — обычный путь всё равно не сработал. */
    mmio_write32(BOARD_WDOG_BASE + WDOG_RST, 1);
    mmio_write32(BOARD_WDOG_BASE + WDOG_BARK_TIME, BARK_TICKS);
    mmio_write32(BOARD_WDOG_BASE + WDOG_BITE_TIME, BITE_TICKS);
    mmio_write32(BOARD_WDOG_BASE + WDOG_EN, 1);
    dsb_sy();

    for (;;) __asm__ volatile ("wfi");
}

void msm_reboot_bootloader(void) { msm_reboot(BOARD_REBOOT_BOOTLOADER); }
void msm_reboot_system(void)     { msm_reboot(0); }

#endif
