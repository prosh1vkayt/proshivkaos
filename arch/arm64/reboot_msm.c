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

/* ---- Сторожевой таймер ----
 *
 * ЗАЧЕМ ОН СНОВА ЗАВЁДЕН. Загрузчик оставляет его тикающим, и первым
 * делом система его глушила: пока мы отлаживались по фотографиям
 * экрана, любое зависание успевало стать перезагрузкой раньше, чем его
 * успевали рассмотреть.
 *
 * Теперь всё наоборот. Отладка идёт по проводу и кругами по две минуты,
 * а зависшая система — это аппарат, который сам уже не оживёт: кнопку
 * питания приходится держать руками, и круг встаёт до появления
 * человека. Дважды за один вечер этого хватило, чтобы передумать.
 *
 * Срок взят с запасом в двадцать секунд: подъём системы занимает три, а
 * дальше её гладят из каждого ожидания и из каждого опроса ввода. Если
 * гладить перестали — значит и правда зависли. */
#define WDOG_TICKS_PER_SEC  32768u
#define WDOG_BITE_SECONDS   20u
#define WDOG_BARK_SECONDS   16u

void msm_watchdog_arm(void) {
    mmio_write32(BOARD_WDOG_BASE + WDOG_EN, 0);
    dsb_sy();
    mmio_write32(BOARD_WDOG_BASE + WDOG_BARK_TIME, WDOG_TICKS_PER_SEC * WDOG_BARK_SECONDS);
    mmio_write32(BOARD_WDOG_BASE + WDOG_BITE_TIME, WDOG_TICKS_PER_SEC * WDOG_BITE_SECONDS);
    mmio_write32(BOARD_WDOG_BASE + WDOG_RST, 1);
    dsb_sy();
    mmio_write32(BOARD_WDOG_BASE + WDOG_EN, 1);
    dsb_sy();

}

/* Показать, что получилось.
 *
 * Отдельно от завода, и не из любви к порядку: заводится он раньше
 * всего остального — раньше экрана и раньше журнала, — поэтому печатать
 * прямо там было некуда. Один заход так и прошёл в недоумении, завёлся
 * ли он вообще. */
void msm_watchdog_report(void) {
    early_con_puts("WDOG: vkl ");
    early_con_hex32(mmio_read32(BOARD_WDOG_BASE + WDOG_EN));
    early_con_puts(" lay ");
    early_con_hex32(mmio_read32(BOARD_WDOG_BASE + WDOG_BARK_TIME));
    early_con_puts(" ukus ");
    early_con_hex32(mmio_read32(BOARD_WDOG_BASE + WDOG_BITE_TIME));
    early_con_puts("\n");
}

/* Погладить. Вызывается из каждого ожидания и каждого опроса ввода.
 *
 * НЕ ЧАЩЕ РАЗА В СТО МИЛЛИСЕКУНД, и это не мелочь. Главный цикл
 * системы ничего не ждёт, поэтому крутится со скоростью процессора: до
 * сотен тысяч оборотов в секунду. Запись в регистр сторожевого таймера
 * с такой частотой — это непрерывный поток обращений к блоку, который
 * рассчитан на одно поглаживание в секунды.
 *
 * Пока кадр отдавался целиком, оборот занимал сто миллисекунд и вопрос
 * не возникал. Стоило отдавать только изменившуюся часть — и аппарат
 * начал молча перезагружаться через несколько секунд после выхода на
 * рабочий стол. */
#define WDOG_PET_INTERVAL_MS 100

void hal_watchdog_pet(void) {
    static uint64_t last = 0;
    uint64_t now = hal_time_ms();
    if (now - last < WDOG_PET_INTERVAL_MS) return;
    last = now;
    mmio_write32(BOARD_WDOG_BASE + WDOG_RST, 1);
}

/* Настроить микросхему питания на перезапуск, а не на выключение. */
static int pon_configure(uint8_t reset_type) {
    early_con_puts("REBOOT: uzel pitaniya 0008, hozyain ");
    early_con_hex32((uint32_t)spmi_owner_of(0, PON_PS_HOLD_RESET_CTL));
    early_con_puts("\n");

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

    /* Дать уйти тому, что уже в пути по проводу.
     *
     * Полсекунды, а не пятьдесят миллисекунд: за пятьдесят компьютер на
     * том конце не успевает вычитать последние строки, и рассказ о том,
     * ПОЧЕМУ перезагрузка пошла не туда, пропадал вместе с ней. */
    hal_time_delay_ms(600);

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
