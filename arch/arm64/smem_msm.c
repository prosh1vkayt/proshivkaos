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

/* ВЫДЕЛИТЬ ПРЕДМЕТ.
 *
 * До сих пор мы только читали то, что заводили другие. Но есть предметы,
 * которые по протоколу заводит сторона приложений, — например, наша
 * половина SMP2P к процессору Wi-Fi. Пока её нет, прошивка Pronto
 * считает, что говорить не с кем, и дальше в загрузке не идёт.
 *
 * Куча в SMEM общая, и выделяют из неё все процессоры, поэтому оглавление
 * меняется только под аппаратным замком: регистр TCSR_MUTEX номер 3.
 * Взять замок — записать туда номер своего процессора (приложения — 1) и
 * прочитать: если прочиталось своё, замок наш. Порядок записей — как в
 * ядре (qcom_smem_alloc_global): сначала смещение и длина, и только потом
 * флаг «выделено», чтобы чужой читатель без замка не увидел полуготовую
 * запись. */
#define SMEM_HWLOCK             0x01908000ull
#define SMEM_LOCK_APPS          1u
#define SMEM_HEAP_FREE          196
#define SMEM_HEAP_AVAIL         200

static int smem_lock(void) {
    for (int i = 0; i < 100000; i++) {
        mmio_write32(SMEM_HWLOCK, SMEM_LOCK_APPS);
        if (mmio_read32(SMEM_HWLOCK) == SMEM_LOCK_APPS) return 1;
    }
    return 0;
}

static void smem_unlock(void) {
    if (mmio_read32(SMEM_HWLOCK) == SMEM_LOCK_APPS) mmio_write32(SMEM_HWLOCK, 0);
}

uint64_t smem_alloc(int id, uint32_t size) {
    if (!g_ready || id < 0 || id >= SMEM_ITEM_COUNT) return 0;
    uint32_t have = 0;
    uint64_t p = smem_item(id, &have);
    if (p) return have >= size ? p : 0;

    if (!smem_lock()) { early_con_puts("SMEM: zamok zanyat\n"); return 0; }
    uint32_t e = SMEM_TOC_OFF + (uint32_t)id * SMEM_TOC_ENTRY;
    uint64_t result = 0;
    size = (size + 7u) & ~7u;
    if (!smem_u32(e + TOC_ALLOCATED) && size <= smem_u32(SMEM_HEAP_AVAIL)) {
        uint32_t off = smem_u32(SMEM_HEAP_FREE);
        mmio_write32(BOARD_SMEM_BASE + e + TOC_OFFSET, off);
        mmio_write32(BOARD_SMEM_BASE + e + TOC_SIZE, size);
        mmio_write32(BOARD_SMEM_BASE + e + TOC_AUX_BASE, 0);
        dsb_sy();
        mmio_write32(BOARD_SMEM_BASE + e + TOC_ALLOCATED, 1);
        mmio_write32(BOARD_SMEM_BASE + SMEM_HEAP_FREE, off + size);
        mmio_write32(BOARD_SMEM_BASE + SMEM_HEAP_AVAIL, smem_u32(SMEM_HEAP_AVAIL) - size);
        dsb_sy();
        result = BOARD_SMEM_BASE + off;
        for (uint32_t i = 0; i < size; i += 4) mmio_write32(result + i, 0);
    }
    smem_unlock();
    if (!result) smem_item_debug(id);
    return result ? result : smem_item(id, 0);
}

/* РАЗДЕЛЫ НА ПАРУ ПРОЦЕССОРОВ.
 *
 * Общее оглавление — не вся SMEM. В конце области (последние 4 КиБ)
 * лежит таблица разделов "$TOC": у каждой пары процессоров свой кусок
 * памяти, куда пишут только двое. На этом телефоне их шесть, и пара
 * «приложения — Wi-Fi» (хосты 0 и 4) — одна из них. Pronto кладёт туда
 * всё, что адресовано нам: таблицу каналов SMD, их состояния и очереди,
 * свою половину SMP2P. В общем оглавлении этого нет — и долго казалось,
 * что прошивка просто не доходит до создания каналов.
 *
 * Внутри раздела предметы идут цепочкой от начала: заголовок на 16 байт
 * (метка A5A5, номер, длина, выравнивания), следом данные. Кэшируемые
 * предметы растут навстречу, от конца; их мы тоже просматриваем.
 * Раскладка — drivers/soc/qcom/smem.c. */
#define SMEM_PTABLE_MAGIC       0x434F5424u    /* "$TOC" */
#define SMEM_PART_MAGIC         0x54525024u    /* "$PRT" */
#define SMEM_PART_HDR_BYTES     32
#define SMEM_PRIV_HDR_BYTES     16
#define SMEM_PRIV_CANARY        0xA5A5u
#define SMEM_MAX_HOSTS          16

static uint32_t g_part_off[SMEM_MAX_HOSTS], g_part_size[SMEM_MAX_HOSTS];
static uint32_t g_part_cacheline[SMEM_MAX_HOSTS];
static int g_parts_scanned = 0;

static void smem_scan_partitions(void) {
    if (g_parts_scanned) return;
    g_parts_scanned = 1;
    uint32_t pt = BOARD_SMEM_SIZE - 0x1000;
    if (smem_u32(pt) != SMEM_PTABLE_MAGIC) return;
    uint32_t n = smem_u32(pt + 8);
    if (n > 32) n = 32;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = pt + 32 + i * 48;
        uint32_t off = smem_u32(e), size = smem_u32(e + 4);
        uint32_t hosts = smem_u32(e + 12);
        uint32_t h0 = hosts & 0xFFFF, h1 = hosts >> 16;
        if (!off || !size || off + size > BOARD_SMEM_SIZE) continue;
        if (h0 != 0 && h1 != 0) continue;           /* не наша пара */
        uint32_t remote = h0 == 0 ? h1 : h0;
        if (remote >= SMEM_MAX_HOSTS) continue;
        if (smem_u32(off) != SMEM_PART_MAGIC) continue;
        g_part_off[remote] = off;
        g_part_size[remote] = size;
        g_part_cacheline[remote] = smem_u32(e + 16);
    }
}

/* Предмет в разделе пары «приложения — host»; нет раздела — общее
   оглавление. */
uint64_t smem_item_host(int host, int id, uint32_t *size) {
    if (!g_ready) return 0;
    smem_scan_partitions();
    if (host < 0 || host >= SMEM_MAX_HOSTS || !g_part_size[host])
        return smem_item(id, size);

    uint32_t base = g_part_off[host];
    uint32_t end = base + smem_u32(base + 12);          /* offset_free_uncached */
    uint32_t e = base + SMEM_PART_HDR_BYTES;
    while (e + SMEM_PRIV_HDR_BYTES <= end) {
        if ((mmio_read16(BOARD_SMEM_BASE + e) & 0xFFFF) != SMEM_PRIV_CANARY) return 0;
        uint32_t item = mmio_read16(BOARD_SMEM_BASE + e + 2);
        uint32_t sz = smem_u32(e + 4);
        uint32_t pad_data = mmio_read16(BOARD_SMEM_BASE + e + 8);
        uint32_t pad_hdr = mmio_read16(BOARD_SMEM_BASE + e + 10);
        if ((int)item == id) {
            if (size) *size = sz - pad_data;
            return BOARD_SMEM_BASE + e + SMEM_PRIV_HDR_BYTES + pad_hdr;
        }
        e += SMEM_PRIV_HDR_BYTES + pad_hdr + sz;
    }

    /* Кэшируемые — от конца раздела вниз. */
    uint32_t cl = g_part_cacheline[host] ? g_part_cacheline[host] : 1;
    uint32_t hdr_al = (SMEM_PRIV_HDR_BYTES + cl - 1) / cl * cl;
    uint32_t lim = base + smem_u32(base + 16);          /* offset_free_cached */
    uint32_t c = base + g_part_size[host] - hdr_al;
    while (c > lim && c > base) {
        if ((mmio_read16(BOARD_SMEM_BASE + c) & 0xFFFF) != SMEM_PRIV_CANARY) break;
        uint32_t item = mmio_read16(BOARD_SMEM_BASE + c + 2);
        uint32_t sz = smem_u32(c + 4);
        uint32_t pad_data = mmio_read16(BOARD_SMEM_BASE + c + 8);
        if ((int)item == id) {
            if (size) *size = sz - pad_data;
            return BOARD_SMEM_BASE + c - sz;
        }
        c -= sz + hdr_al;
    }
    return 0;
}

uint64_t smem_alloc_host(int host, int id, uint32_t size) {
    if (!g_ready) return 0;
    smem_scan_partitions();
    if (host < 0 || host >= SMEM_MAX_HOSTS || !g_part_size[host])
        return smem_alloc(id, size);

    uint32_t have = 0;
    uint64_t p = smem_item_host(host, id, &have);
    if (p) return have >= size ? p : 0;

    if (!smem_lock()) { early_con_puts("SMEM: zamok zanyat\n"); return 0; }
    uint64_t result = 0;
    uint32_t base = g_part_off[host];
    uint32_t free_unc = smem_u32(base + 12);
    uint32_t free_cached = smem_u32(base + 16);
    uint32_t al = (size + 7u) & ~7u;
    uint32_t e = base + free_unc;
    if (free_unc + SMEM_PRIV_HDR_BYTES + al <= free_cached) {
        uint64_t a = BOARD_SMEM_BASE + e;
        mmio_write16(a, (uint16_t)SMEM_PRIV_CANARY);
        mmio_write16(a + 2, (uint16_t)id);
        mmio_write32(a + 4, al);
        mmio_write16(a + 8, (uint16_t)(al - size));
        mmio_write16(a + 10, 0);
        mmio_write32(a + 12, 0);
        result = a + SMEM_PRIV_HDR_BYTES;
        for (uint32_t i = 0; i < al; i += 4) mmio_write32(result + i, 0);
        dsb_sy();
        mmio_write32(BOARD_SMEM_BASE + base + 12, free_unc + SMEM_PRIV_HDR_BYTES + al);
        dsb_sy();
    }
    smem_unlock();
    if (!result) early_con_puts("SMEM: v razdele net mesta\n");
    return result;
}

