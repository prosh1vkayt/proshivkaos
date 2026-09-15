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

/* Сбой процессора: вид, адрес команды и адрес данных. Адрес команды
   хранится смещением от начала образа — образ грузится не туда, куда
   собран, и абсолютный адрес ни с чем в карте символов не сопоставить. */
#define BB_FAULT_MAGIC_OFF  16
#define BB_FAULT_ESR_OFF    20
#define BB_FAULT_ELR_OFF    24
#define BB_FAULT_FARLO_OFF  28
#define BB_FAULT_FARHI_OFF  32
#define BB_FAULT_INDEX_OFF  36
#define BB_FAULT_MAGIC      0x544C4146u   /* "FALT" */

/* СЛЕД: последние смены занятий со временем.
 *
 * Одна метка говорит, где застала смерть, но не как туда пришли. А
 * когда смерть сдвигается от безобидной правки в другом конце системы,
 * нужна именно дорога: что шло перед чем и сколько длилось. Пишется
 * только смена занятия, а не каждая отметка, — иначе опрос ввода в
 * простое за долю секунды вытеснил бы всё остальное. */
#define BB_TRAIL_IDX_OFF    44
#define BB_TRAIL_OFF        48
#define BB_TRAIL_LEN        32     /* записей; по слову на запись */

/* ДЛИНА ЖУРНАЛА — чтобы достать хвост прошлого запуска.
 *
 * Журнал загрузки пишется в ту же сохраняемую память и переживает сброс,
 * но новый запуск начинает писать его с начала и помнит только свою
 * длину. Конец старого при этом цел — новый до него ещё не дописал. Если
 * знать, где он кончался, последние строки умершего запуска можно
 * забрать. По проводу они не доходят: поток отстаёт на секунды, и смерть
 * обрывает его раньше. */
#define BB_CONSOLE_LEN_OFF  (BB_TRAIL_OFF + 4 * BB_TRAIL_LEN)
#define PREV_TAIL_MAX       8192

static char     g_prev_tail[PREV_TAIL_MAX];
static uint32_t g_prev_tail_len = 0;

extern char __image_start[];

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
    case BB_TAG_I2C_STATE: return "I2C: smena sostoyaniya bloka";
    case BB_TAG_I2C_PUSH:  return "I2C: zapis v ochered peredachi";
    case BB_TAG_I2C_XFER:  return "I2C: ozhidanie konca peredachi";
    case BB_TAG_I2C_IDLE:  return "I2C: ozhidanie osvobozhdeniya shiny";
    case BB_TAG_DRAW_BG:   return "kadr: fon i rabochiy stol";
    case BB_TAG_DRAW_APP:  return "kadr: prilozhenie";
    case BB_TAG_DRAW_OSK:  return "kadr: klaviatura";
    case BB_TAG_DRAW_BARS: return "kadr: paneli";
    case BB_TAG_PRESENT:   return "kadr: vyvod na ekran";
    case 16:               return "kadr: oboi";
    case 17:               return "kadr: chasy";
    case 18:               return "kadr: ikonki";
    case 19:               return "ikonka: ten";
    case 20:               return "ikonka: telo";
    case 21:               return "ikonka: podpis";
    default:             return "neizvestno";
    }
}

/* Отметить, чем заняты. Вызывается часто, поэтому делает ровно одну
 * запись: время пишется отдельно, раз в секунду. */
void blackbox_mark(uint32_t tag) {
    static uint32_t last = 0xFFFFFFFFu;
    static uint32_t idx = 0;
    if (tag == last) return;
    last = tag;

    mmio_write32(BB_ADDR + BB_TAG_OFF, tag);

    /* Слово записи: метка в старшем байте, миллисекунды в младших трёх
       (хватает на четыре с половиной часа — дальше просто по кругу). */
    uint32_t ms = (uint32_t)hal_time_ms() & 0x00FFFFFFu;
    mmio_write32(BB_ADDR + BB_TRAIL_OFF + 4 * (idx % BB_TRAIL_LEN), (tag << 24) | ms);
    idx++;
    mmio_write32(BB_ADDR + BB_TRAIL_IDX_OFF, idx);
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

/* Записать сбой. Вызывается обработчиком исключений первым делом.
 *
 * ЗАЧЕМ. Сбой, случившийся внутри опроса провода, по проводу уже не
 * рассказать: обработчик честно печатает, но печать глотается, пока идёт
 * выдача, и снаружи виден только обрыв. А экран без человека у стола
 * никто не прочтёт. Эта запись переживает перезагрузку, и следующая
 * загрузка скажет адрес сбоя сама. */
void blackbox_fault(uint64_t index, uint64_t esr, uint64_t far, uint64_t elr) {
    uint64_t base = (uint64_t)(uintptr_t)__image_start;
    mmio_write32(BB_ADDR + BB_FAULT_ESR_OFF, (uint32_t)esr);
    mmio_write32(BB_ADDR + BB_FAULT_ELR_OFF, (uint32_t)(elr - base));
    mmio_write32(BB_ADDR + BB_FAULT_FARLO_OFF, (uint32_t)far);
    mmio_write32(BB_ADDR + BB_FAULT_FARHI_OFF, (uint32_t)(far >> 32));
    mmio_write32(BB_ADDR + BB_FAULT_INDEX_OFF, (uint32_t)index);
    mmio_write32(BB_ADDR + BB_FAULT_MAGIC_OFF, BB_FAULT_MAGIC);
    dsb_sy();
}

/* Пока запись прошлого запуска не прочитана, длину не трогаем: журнал
   начинает писаться раньше чёрного ящика и затёр бы её первой же строкой. */
static int g_started = 0;

void blackbox_note_console(uint32_t used) {
    if (g_started)
        mmio_write32(BB_ADDR + BB_CONSOLE_LEN_OFF, used);
}

int blackbox_prev_tail(const char **data, uint32_t *len) {
    if (!g_prev_tail_len) return 0;
    *data = g_prev_tail;
    *len = g_prev_tail_len;
    return 1;
}

/* Сохранить конец прошлого журнала, пока новый его не затёр. */
static void save_prev_tail(void) {
    const volatile unsigned char *log;
    uint32_t now_len;
    uint32_t prev_len = mmio_read32(BB_ADDR + BB_CONSOLE_LEN_OFF);
    if (!ramoops_snapshot(&log, &now_len)) return;
    if (prev_len > BOARD_PSTORE_CONSOLE_SIZE) return;   /* мусор */
    if (prev_len <= now_len) return;                    /* уже затёрт */

    uint32_t from = prev_len > PREV_TAIL_MAX ? prev_len - PREV_TAIL_MAX : 0;
    if (from < now_len) from = now_len;
    for (uint32_t i = from; i < prev_len; i++)
        g_prev_tail[g_prev_tail_len++] = (char)log[i];
}

/* Прочитать запись прошлого запуска и начать свою. */
void blackbox_init(void) {
    uint32_t magic  = mmio_read32(BB_ADDR + BB_MAGIC_OFF);
    uint32_t tag    = mmio_read32(BB_ADDR + BB_TAG_OFF);
    uint32_t uptime = mmio_read32(BB_ADDR + BB_UPTIME_OFF);
    uint32_t boots  = mmio_read32(BB_ADDR + BB_BOOTS_OFF);

    if (magic == BB_MAGIC) {
        save_prev_tail();
        early_con_puts("BB: proshlyy zapusk prozhil ");
        early_con_hex32(uptime);
        early_con_puts(" ms, zanimalsya: ");
        early_con_puts(tag_name(tag));
        early_con_puts("\n");
        uint32_t n = mmio_read32(BB_ADDR + BB_TRAIL_IDX_OFF);
        uint32_t shown = n < BB_TRAIL_LEN ? n : BB_TRAIL_LEN;
        if (shown) {
            early_con_puts("BB: sled (metka@ms), starye pervymi:\n");
            for (uint32_t k = 0; k < shown; k++) {
                uint32_t w = mmio_read32(BB_ADDR + BB_TRAIL_OFF +
                                         4 * ((n - shown + k) % BB_TRAIL_LEN));
                early_con_puts((k % 6) == 0 ? "BB:  " : " ");
                early_con_hex8((uint8_t)(w >> 24));
                early_con_puts("@");
                early_con_hex32(w & 0x00FFFFFFu);
                if ((k % 6) == 5 || k + 1 == shown) early_con_puts("\n");
            }
        }
        if (g_prev_tail_len) {
            early_con_puts("BB: hvost proshlogo zhurnala sohranyon, bayt ");
            early_con_hex32(g_prev_tail_len);
            early_con_puts(" (komanda o po provodu)\n");
        }
        if (mmio_read32(BB_ADDR + BB_FAULT_MAGIC_OFF) == BB_FAULT_MAGIC) {
            early_con_puts("BB: i UPAL: ESR ");
            early_con_hex32(mmio_read32(BB_ADDR + BB_FAULT_ESR_OFF));
            early_con_puts(" komanda +");
            early_con_hex32(mmio_read32(BB_ADDR + BB_FAULT_ELR_OFF));
            early_con_puts(" dannye ");
            early_con_hex32(mmio_read32(BB_ADDR + BB_FAULT_FARHI_OFF));
            early_con_hex32(mmio_read32(BB_ADDR + BB_FAULT_FARLO_OFF));
            early_con_puts(" vektor ");
            early_con_hex32(mmio_read32(BB_ADDR + BB_FAULT_INDEX_OFF));
            early_con_puts("\n");
        }
        boots++;
    } else {
        early_con_puts("BB: zapisi o proshlom zapuske net (pervyy raz)\n");
        boots = 1;
    }

    mmio_write32(BB_ADDR + BB_MAGIC_OFF, BB_MAGIC);
    mmio_write32(BB_ADDR + BB_TAG_OFF, BB_TAG_BOOT);
    mmio_write32(BB_ADDR + BB_UPTIME_OFF, 0);
    mmio_write32(BB_ADDR + BB_BOOTS_OFF, boots);
    mmio_write32(BB_ADDR + BB_FAULT_MAGIC_OFF, 0);
    mmio_write32(BB_ADDR + BB_TRAIL_IDX_OFF, 0);
    dsb_sy();

    early_con_puts("BB: zapusk nomer ");
    early_con_hex32(boots);
    early_con_puts("\n");
    g_started = 1;
}

#else

void blackbox_mark(uint32_t tag) { (void)tag; }
void blackbox_heartbeat(void) { }
void blackbox_init(void) { }
void blackbox_fault(uint64_t index, uint64_t esr, uint64_t far, uint64_t elr) {
    (void)index; (void)esr; (void)far; (void)elr;
}
void blackbox_note_console(uint32_t used) { (void)used; }
int  blackbox_prev_tail(const char **data, uint32_t *len) { (void)data; (void)len; return 0; }

#endif
