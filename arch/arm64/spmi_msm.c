/* arch/arm64/spmi_msm.c — шина к микросхеме питания (SPMI, арбитр версии 2).
 *
 * ЗАЧЕМ ЭТО ПОНАДОБИЛОСЬ. Два тупика подряд упёрлись в одно и то же.
 * Тачскрин не отвечает даже на выданный вручную I2C — то есть его на шине
 * электрически нет. Приёмопередатчик USB не захватывает частоту, а его
 * регистр состояния читается нулём. И то и другое питается от отдельных
 * источников внутри микросхемы питания: тачскрин от pm8953_l10 и l6,
 * приёмопередатчик от l3, l7 и l13. Загрузчик, уходя, гасит то, чем сам
 * не пользуется.
 *
 * ЧТО ТАКОЕ SPMI. Двухпроводная шина между процессором и микросхемой
 * питания. Напрямую по ней не ходят: между ними стоит АРБИТР, который
 * раздаёт доступ нескольким ведущим (процессор, модем, сопроцессор
 * питания). Программа обращается не к устройству, а к КАНАЛУ арбитра.
 *
 * И вот главная особенность, из-за которой этот файл не в три строки:
 * соответствие "адрес устройства -> номер канала" нигде не записано. Оно
 * лежит в таблице внутри самого арбитра, и её надо прочитать и разобрать.
 * Каждая запись таблицы содержит PPID — старшие разряды адреса вместе с
 * номером ведомого; канал ищется перебором по совпадению.
 *
 * Здесь только чтение и запись одного байта. Ни прерываний, ни владения
 * шиной, ни пакетных передач: всё, что нам нужно от микросхемы питания, —
 * посмотреть состояние источника и, если он выключен, включить.
 */
#include "arm64.h"
#include "boards/board.h"

/* Смещения внутри канала (одинаковы у окна записи и окна наблюдения) */
#define ARB_CMD         0x00
#define ARB_CONFIG      0x04
#define ARB_STATUS      0x08
#define ARB_WDATA0      0x10
#define ARB_RDATA0      0x18

/* Таблица каналов лежит в общем окне арбитра */
#define ARB_APID_MAP(n) (0x800 + 4 * (uint32_t)(n))
#define ARB_MAX_APID    256

/* Состояние обмена */
#define ARB_ST_DONE     (1u << 0)
#define ARB_ST_FAILURE  (1u << 1)
#define ARB_ST_DENIED   (1u << 2)
#define ARB_ST_DROPPED  (1u << 3)

/* Коды операций арбитра. Нам нужны только длинные формы: в них адрес
   ведомого задаётся целиком, а не восемью разрядами. */
#define ARB_OP_EXT_WRITEL   0
#define ARB_OP_EXT_READL    1

#define POLL_LIMIT      100000

/* Канал -> PPID. 0xFFFF означает "канал не занят". */
static uint16_t g_ppid[ARB_MAX_APID];
static int      g_ready = 0;

int spmi_init(void) {
    int used = 0;
    for (int apid = 0; apid < ARB_MAX_APID; apid++) {
        uint32_t v = mmio_read32(BOARD_SPMI_CORE_BASE + ARB_APID_MAP(apid));
        if (v) { g_ppid[apid] = (uint16_t)((v >> 8) & 0xFFF); used++; }
        else     g_ppid[apid] = 0xFFFF;
    }

    early_con_puts("SPMI: kanalov zanyato ");
    early_con_hex32((uint32_t)used);
    early_con_puts(" iz ");
    early_con_hex32(ARB_MAX_APID);
    early_con_puts("\n");

    /* Пустая таблица означает, что мы читаем не то место: арбитр всегда
       раздаёт хотя бы несколько каналов. */
    g_ready = (used > 0);
    if (!g_ready) early_con_puts("SPMI: tablica pusta — arbitr ne otvechaet\n");
    return g_ready;
}

/* Найти канал, через который видно нужное устройство. */
static int find_apid(uint8_t sid, uint16_t addr) {
    uint16_t want = (uint16_t)(((uint16_t)sid << 8) | (addr >> 8));
    for (int i = 0; i < ARB_MAX_APID; i++)
        if (g_ppid[i] == want) return i;
    return -1;
}

static int wait_done(uint64_t status_reg) {
    for (int i = 0; i < POLL_LIMIT; i++) {
        uint32_t st = mmio_read32(status_reg);
        if (st & ARB_ST_DONE)
            return !(st & (ARB_ST_FAILURE | ARB_ST_DENIED | ARB_ST_DROPPED));
    }
    return 0;
}

int spmi_read(uint8_t sid, uint16_t addr, uint8_t *out) {
    if (!g_ready) return 0;
    int apid = find_apid(sid, addr);
    if (apid < 0) return 0;

    /* Чтение идёт через окно наблюдения, запись — через своё. */
    uint64_t ch = BOARD_SPMI_OBSRVR_BASE + 0x8000ULL * (uint64_t)apid;

    mmio_write32(ch + ARB_CMD,
                 ((uint32_t)ARB_OP_EXT_READL << 27) | ((addr & 0xFFu) << 4) | 0u);
    dsb_sy();

    if (!wait_done(ch + ARB_STATUS)) return 0;

    *out = (uint8_t)(mmio_read32(ch + ARB_RDATA0) & 0xFF);
    return 1;
}

int spmi_write(uint8_t sid, uint16_t addr, uint8_t value) {
    if (!g_ready) return 0;
    int apid = find_apid(sid, addr);
    if (apid < 0) return 0;

    uint64_t ch = BOARD_SPMI_CHNLS_BASE + 0x8000ULL * (uint64_t)apid;

    mmio_write32(ch + ARB_WDATA0, value);
    mmio_write32(ch + ARB_CMD,
                 ((uint32_t)ARB_OP_EXT_WRITEL << 27) | ((addr & 0xFFu) << 4) | 0u);
    dsb_sy();

    return wait_done(ch + ARB_STATUS);
}
