/* arch/arm64/blackbox.c — что система делала в последний миг.
 *
 * ЗАЧЕМ. Аппарат перезагружается сам через случайное время от десяти
 * секунд до нескольких минут. Микросхема питания говорит, ЧТО
 * произошло — процессор отпустил линию удержания питания, — но не
 * говорит, ЧЕМ он в этот момент был занят. А журнал обрывается на
 * последней напечатанной строке, то есть обычно задолго до смерти:
 * работающая система печатает редко.
 *
 * Здесь ведётся запись, которая переживает перезагрузку: несколько слов
 * в области ОЗУ, исключённой из общего пула ради посмертных журналов.
 * Система постоянно отмечает в ней, чем занята и сколько прожила, а при
 * следующем запуске первым делом это читает и докладывает.
 *
 * Разница между "умер через сорок секунд" и "умер через сорок секунд,
 * опрашивая тачскрин" — это разница между очередным кругом догадок и
 * одним прицельным опытом.
 *
 * ПОЧЕМУ ИМЕННО СЮДА. Область сохранённых сообщений размечена зонами:
 * дампы, консоль, трассировка, сообщения. Мы пишем в зону трассировки —
 * ею ни Android, ни мы не пользуемся, а лежит она в той же памяти,
 * которую загрузчик исключил из общего пула и потому не затирает.
 */
#include "arm64.h"
#include "boards/board.h"
#include "hal_time.h"

#ifdef BOARD_PSTORE_BASE

/* Зона трассировки: сразу за консолью. */
#define BB_ADDR   (BOARD_PSTORE_BASE + BOARD_PSTORE_CONSOLE_OFF + BOARD_PSTORE_CONSOLE_SIZE)

#define BB_MAGIC_OFF   0
#define BB_TAG_OFF     4
#define BB_UPTIME_OFF  8
#define BB_BOOTS_OFF   12

#define BB_MAGIC  0x50534F42u    /* "POSB" младшим байтом вперёд */

static const char *tag_name(uint32_t tag) {
    switch (tag) {
    case BB_TAG_BOOT:    return "podyom sistemy";
    case BB_TAG_IDLE:    return "prostoy (nichego ne delali)";
    case BB_TAG_INPUT:   return "opros vvoda";
    case BB_TAG_TOUCH:   return "obmen s tachskrinom po I2C";
    case BB_TAG_RENDER:  return "otrisovka kadra";
    case BB_TAG_USB:     return "razbor sobytiy USB";
    case BB_TAG_RPM:     return "razgovor s soprocessorom pitaniya";
    default:             return "neizvestno";
    }
}

/* Отметить, чем заняты. Вызывается часто, поэтому делает ровно одну
 * запись: время пишется отдельно, раз в секунду. */
void blackbox_mark(uint32_t tag) {
    mmio_write32(BB_ADDR + BB_TAG_OFF, tag);
}

/* Отметить, сколько прожили. Раз в секунду — чаще незачем, а запись в
 * эту память идёт прямо на шину, минуя кэш. */
void blackbox_heartbeat(void) {
    static uint64_t last = 0;
    uint64_t now = hal_time_ms();
    if (now - last < 1000) return;
    last = now;
    mmio_write32(BB_ADDR + BB_UPTIME_OFF, (uint32_t)now);
}

/* Прочитать запись прошлого запуска и начать свою. */
void blackbox_init(void) {
    uint32_t magic  = mmio_read32(BB_ADDR + BB_MAGIC_OFF);
    uint32_t tag    = mmio_read32(BB_ADDR + BB_TAG_OFF);
    uint32_t uptime = mmio_read32(BB_ADDR + BB_UPTIME_OFF);
    uint32_t boots  = mmio_read32(BB_ADDR + BB_BOOTS_OFF);

    if (magic == BB_MAGIC) {
        early_con_puts("BB: proshlyy zapusk prozhil ");
        early_con_hex32(uptime);
        early_con_puts(" ms, zanimalsya: ");
        early_con_puts(tag_name(tag));
        early_con_puts("\n");
        boots++;
    } else {
        early_con_puts("BB: zapisi o proshlom zapuske net (pervyy raz)\n");
        boots = 1;
    }

    mmio_write32(BB_ADDR + BB_MAGIC_OFF, BB_MAGIC);
    mmio_write32(BB_ADDR + BB_TAG_OFF, BB_TAG_BOOT);
    mmio_write32(BB_ADDR + BB_UPTIME_OFF, 0);
    mmio_write32(BB_ADDR + BB_BOOTS_OFF, boots);
    dsb_sy();

    early_con_puts("BB: zapusk nomer ");
    early_con_hex32(boots);
    early_con_puts("\n");
}

#else

void blackbox_mark(uint32_t tag) { (void)tag; }
void blackbox_heartbeat(void) { }
void blackbox_init(void) { }

#endif
