/* arch/arm64/smem_msm.c — общая память процессоров (SMEM).
 *
 * ЧТО ЭТО. В этом аппарате не один процессор, а несколько: наш
 * прикладной, модем, звуковой и — самый для нас важный — СОПРОЦЕССОР
 * ПИТАНИЯ. Разговаривают они через область памяти, видимую всем сразу.
 *
 * Область устроена как справочник: в её начале лежит оглавление на
 * пятьсот двенадцать записей, и каждая говорит, по какому смещению и
 * какой длины лежит предмет с таким номером. Номера предметов
 * закреплены: тринадцатый — список каналов связи, четырнадцатый и
 * дальше — состояния этих каналов, триста тридцать восьмой и дальше —
 * их очереди.
 *
 * ЗАЧЕМ НАМ ЭТО. Источник питания тачскрина нам не принадлежит: попытка
 * включить его записью в микросхему питания роняет процессор, потому что
 * распоряжается им сопроцессор питания. Значит, надо не приказывать, а
 * попросить — и просьба идёт через эту самую общую память.
 *
 * ОСТОРОЖНО С ДОСТУПОМ. Область общая с другими процессорами, поэтому
 * отображается некэшируемой (см. mmu.c) и читается только выровненными
 * обращениями: побайтно и по 32 бита. Структуры целиком не копируем —
 * компилятор мог бы взять их парой регистров, а такой доступ к памяти
 * устройств запрещён.
 */
#include "arm64.h"
#include "boards/board.h"

/* Раскладка заголовка области */
#define SMEM_PROC_COMM_BYTES    64          /* 4 записи по 16 байт      */
#define SMEM_VERSION_OFF        SMEM_PROC_COMM_BYTES
#define SMEM_VERSION_COUNT      32
#define SMEM_TOC_OFF            (SMEM_VERSION_OFF + SMEM_VERSION_COUNT * 4 + 16)
#define SMEM_ITEM_COUNT         512
#define SMEM_TOC_ENTRY          16

/* Смещения внутри записи оглавления */
#define TOC_ALLOCATED           0
#define TOC_OFFSET              4
#define TOC_SIZE                8
#define TOC_AUX_BASE            12
#define TOC_AUX_MASK            0xFFFFFFFCu

/* Какой из тридцати двух номеров версий отвечает за раскладку области */
#define SMEM_SBL_VERSION_INDEX  7
#define SMEM_HEAP_VERSION       11    /* простое оглавление — умеем     */
#define SMEM_PART_VERSION       12    /* разделы — не умеем             */

static int g_ready = 0;

static uint32_t smem_u32(uint32_t off) {
    return mmio_read32(BOARD_SMEM_BASE + off);
}

int smem_init(void) {
    uint32_t version = smem_u32(SMEM_VERSION_OFF + SMEM_SBL_VERSION_INDEX * 4) >> 16;

    early_con_puts("SMEM: versiya razmetki ");
    early_con_hex32(version);
    early_con_puts("\n");

    if (version != SMEM_HEAP_VERSION) {
        /* Разделы (версия 12) устроены иначе: оглавление там своё у
           каждого хозяина. Мы этого не умеем, и делать вид, что умеем,
           хуже, чем честно отказаться. */
        early_con_puts("SMEM: takaya razmetka ne podderzhivaetsya\n");
        g_ready = 0;
        return 0;
    }

    g_ready = 1;
    return 1;
}

/* Показать запись оглавления как есть.
 *
 * Нужно, когда предмет "не находится": причин может быть три — он не
 * выделен вовсе, он лежит в дополнительной области памяти, которую мы
 * не отображаем, или мы вычислили не тот номер. Сырая запись отвечает
 * сразу на все. */
void smem_item_debug(int id) {
    uint32_t e = SMEM_TOC_OFF + (uint32_t)id * SMEM_TOC_ENTRY;
    early_con_puts("SMEM: predmet ");
    early_con_hex32((uint32_t)id);
    early_con_puts(" vydelen ");
    early_con_hex32(smem_u32(e + TOC_ALLOCATED));
    early_con_puts(" smeshchenie ");
    early_con_hex32(smem_u32(e + TOC_OFFSET));
    early_con_puts(" dlina ");
    early_con_hex32(smem_u32(e + TOC_SIZE));
    early_con_puts(" oblast ");
    early_con_hex32(smem_u32(e + TOC_AUX_BASE));
    early_con_puts("\n");
}

/* Найти предмет по номеру. Возвращает адрес или 0. */
uint64_t smem_item(int id, uint32_t *size) {
    if (!g_ready || id < 0 || id >= SMEM_ITEM_COUNT) return 0;

    uint32_t e = SMEM_TOC_OFF + (uint32_t)id * SMEM_TOC_ENTRY;
    if (!smem_u32(e + TOC_ALLOCATED)) return 0;

    /* ПРЕДМЕТ МОЖЕТ ЛЕЖАТЬ НЕ В ОСНОВНОЙ ОБЛАСТИ.
     *
     * Сперва такие записи здесь отбрасывались как неподдерживаемые — и
     * именно на этом всё и встало: состояние и очереди канала к
     * сопроцессору питания лежат как раз в дополнительной области.
     * Она объявлена в дереве устройств ("aux-mem1"), адрес у неё низкий
     * и потому уже отображён.
     *
     * Чужие области по-прежнему отбрасываем: отображения у них нет, и
     * обращение туда было бы не ошибкой чтения, а падением. */
    uint32_t aux = smem_u32(e + TOC_AUX_BASE) & TOC_AUX_MASK;
    uint64_t base = BOARD_SMEM_BASE;

    if (aux) {
        if ((uint64_t)aux != (uint64_t)BOARD_SMEM_AUX_BASE) return 0;
        base = (uint64_t)BOARD_SMEM_AUX_BASE;
    }

    if (size) *size = smem_u32(e + TOC_SIZE);
    return base + smem_u32(e + TOC_OFFSET);
}
