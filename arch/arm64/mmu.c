/* arch/arm64/mmu.c — плоское (identity) отображение памяти и включение кэшей.
 *
 * Почему это вообще нужно, если виртуальная память нам пока не сдалась:
 * при выключенном MMU архитектура ARM обязывает трактовать ЛЮБОЕ обращение
 * к памяти как Device-nGnRnE, то есть строго некэшируемое и неупорядочиваемое.
 * Отрисовка кадра — это миллион записей в фреймбуфер плюс копирование
 * бэкбуфера; на некэшируемой памяти это в разы медленнее, интерфейс
 * начинает ощутимо тормозить даже под эмулятором. Поэтому MMU включаем —
 * но не ради изоляции процессов (её пока нет), а именно ради кэшей.
 *
 * КАК РАЗМЕЧЕНО. Верхний уровень — четыре дескриптора по 1 ГиБ,
 * виртуальный адрес равен физическому. Три верхних гигабайта описаны
 * блоками целиком, а первый разбит на куски по 2 МиБ через таблицу
 * второго уровня. Разбит не для красоты: на телефоне граница между
 * регистрами устройств и ОЗУ проходит ВНУТРИ первого гигабайта, а
 * гигабайтным блоком такую границу не выразить.
 *
 * Граница задаётся платой (BOARD_RAM_START):
 *
 *   QEMU virt   регистры ниже 0x40000000, ОЗУ с 0x40000000 — весь первый
 *               гигабайт остаётся памятью устройств, как и было;
 *   mido        регистры ниже 0x10000000, ОЗУ с 0x10000000.
 *
 * ЧЕМ ЭТО ВАЖНО. Загрузчик телефона кладёт 64-битное ядро по адресу
 * "начало ОЗУ плюс 0x80000", то есть по 0x10080000 — в первый гигабайт.
 * Пока весь первый гигабайт считался памятью устройств, включение MMU
 * убивало систему на первой же выборке команды: исполнять код из памяти
 * устройств архитектура не разрешает. Обработчик исключений лежал там же
 * и падал следом, так что не оставалось даже сообщения — телефон просто
 * зависал до сторожевого таймера.
 */
#include "arm64.h"
#include "boards/board.h"

/* Биты дескриптора блока (ARM ARM, D5.3) */
#define DESC_BLOCK      (1ULL << 0)   /* тип: блок (валидный, не таблица) */
#define DESC_VALID      (1ULL << 0)
#define DESC_ATTR(n)    ((uint64_t)(n) << 2)   /* индекс в MAIR_EL1        */
#define DESC_AP_RW_EL1  (0ULL << 6)   /* чтение/запись только из EL1       */
#define DESC_SH_INNER   (3ULL << 8)   /* inner shareable                    */
#define DESC_AF         (1ULL << 10)  /* Access Flag: без него — сбой доступа */

#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1

#define DESC_TABLE      (3ULL << 0)   /* дескриптор таблицы, а не блока    */
#define L2_BLOCK_SHIFT  21            /* блок второго уровня — 2 МиБ        */
#define L2_ENTRIES      512           /* 512 * 2 МиБ = первый гигабайт      */

/* Таблицы трансляции. Выравнивание на 4 КиБ — требование архитектуры
 * (младшие биты адреса таблицы под сам адрес не отводятся). */
static uint64_t g_l1_table[4] __attribute__((aligned(4096)));
static uint64_t g_l2_low[L2_ENTRIES] __attribute__((aligned(4096)));

static int g_mmu_on = 0;

int mmu_enabled(void) { return g_mmu_on; }

void mmu_init(void) {
    if (g_mmu_on) return;

    /* Первый гигабайт — по кускам в 2 МиБ: граница между регистрами и ОЗУ
       проходит внутри него, и гигабайтным блоком её не выразить. */
    for (int i = 0; i < L2_ENTRIES; i++) {
        uint64_t phys = (uint64_t)i << L2_BLOCK_SHIFT;

        if (phys < (uint64_t)BOARD_RAM_START) {
            /* Регистры устройств. Кэшировать их нельзя ни при каких
               обстоятельствах: чтение регистра статуса из кэша вернёт
               вчерашнее значение. */
            g_l2_low[i] = phys | DESC_BLOCK | DESC_ATTR(MAIR_IDX_DEVICE) |
                          DESC_AP_RW_EL1 | DESC_AF;
        } else {
            /* Обычная память. Здесь может лежать и сам образ — загрузчик
               телефона кладёт ядро именно в начало ОЗУ. */
            g_l2_low[i] = phys | DESC_BLOCK | DESC_ATTR(MAIR_IDX_NORMAL) |
                          DESC_AP_RW_EL1 | DESC_SH_INNER | DESC_AF;
        }
    }

    /* Верхний уровень: первый гигабайт через таблицу выше, остальные три —
       блоками целиком, они целиком ОЗУ. */
    g_l1_table[0] = (uint64_t)(uintptr_t)g_l2_low | DESC_TABLE;

    for (int i = 1; i < 4; i++) {
        uint64_t phys = (uint64_t)i << 30;
        g_l1_table[i] = phys | DESC_BLOCK | DESC_ATTR(MAIR_IDX_NORMAL) |
                        DESC_AP_RW_EL1 | DESC_SH_INNER | DESC_AF;
    }

    /* MAIR_EL1: attr0 = 0x00 (Device-nGnRnE), attr1 = 0xFF (Normal,
       write-back, read/write-allocate и для внутреннего, и для внешнего кэша) */
    uint64_t mair = (0x00ULL << (8 * MAIR_IDX_DEVICE)) |
                    (0xFFULL << (8 * MAIR_IDX_NORMAL));

    /* TCR_EL1:
       T0SZ = 32      -> виртуальное адресное пространство 4 ГиБ (32 бита),
                          начальный уровень трансляции — первый, 4 записи
       IRGN0/ORGN0 = 1 -> сами таблицы страниц лежат в кэшируемой памяти
       SH0 = 3         -> inner shareable
       TG0 = 0         -> гранула 4 КиБ
       EPD1 = 1        -> старшая половина (TTBR1) не используется вообще
       IPS = 1         -> физические адреса 36 бит (64 ГиБ) — с запасом */
    uint64_t tcr = (32ULL)        |
                   (1ULL  <<  8)  |
                   (1ULL  << 10)  |
                   (3ULL  << 12)  |
                   (0ULL  << 14)  |
                   (1ULL  << 23)  |
                   (1ULL  << 32);

    __asm__ volatile ("msr mair_el1, %0"  :: "r"(mair));
    __asm__ volatile ("msr tcr_el1, %0"   :: "r"(tcr));
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)g_l1_table));

    /* Таблицы должны быть видны обходчику до того, как он начнёт по ним
       ходить. MMU сейчас выключен, значит записи шли прямо в ОЗУ, но
       порядок надо закрепить явно. Заодно выбрасываем кэш команд: дальше
       выборка пойдёт уже через трансляцию. */
    dsb_sy();
    __asm__ volatile ("ic iallu");
    dsb_sy();
    isb();

    /* Старые записи TLB могли остаться от загрузчика — выкидываем все. */
    __asm__ volatile ("tlbi vmalle1");
    __asm__ volatile ("dsb nsh");
    isb();

    uint64_t sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0);    /* M — включить MMU              */
    sctlr |= (1ULL << 2);    /* C — включить кэш данных       */
    sctlr |= (1ULL << 12);   /* I — включить кэш инструкций   */
    __asm__ volatile ("msr sctlr_el1, %0" :: "r"(sctlr));
    isb();

    g_mmu_on = 1;
}

/* Размер строки кэша данных из CTR_EL0. Поле DminLine — это log2 от
 * количества 4-байтных слов в самой короткой строке кэша данных. */
static uint64_t dcache_line_size(void) {
    uint64_t ctr;
    __asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
    return 4ULL << ((ctr >> 16) & 0xF);
}

/* Вытолкнуть свои записи из кэша в ОЗУ, чтобы их увидело устройство.
 * Нужно после отрисовки кадра (ramfb-дисплей читает наш буфер напрямую)
 * и после заполнения дескрипторов virtio. */
void arch_dcache_clean(const void *addr, size_t len) {
    if (!g_mmu_on) return;   /* кэш выключен — данные и так уже в ОЗУ */

    uint64_t line = dcache_line_size();
    uint64_t p    = (uint64_t)(uintptr_t)addr & ~(line - 1);
    uint64_t end  = (uint64_t)(uintptr_t)addr + len;

    for (; p < end; p += line)
        __asm__ volatile ("dc cvac, %0" :: "r"(p) : "memory");
    dsb_sy();
}

/* Выбросить свою (возможно, устаревшую) копию — данные туда только что
 * записало устройство мимо кэша. Нужно перед чтением used-кольца virtio. */
void arch_dcache_invalidate(void *addr, size_t len) {
    if (!g_mmu_on) return;

    uint64_t line = dcache_line_size();
    uint64_t p    = (uint64_t)(uintptr_t)addr & ~(line - 1);
    uint64_t end  = (uint64_t)(uintptr_t)addr + len;

    /* civac (clean+invalidate), а не ivac: диапазон может не совпадать со
       строками кэша по краям, и чистый invalidate потерял бы соседние
       данные, попавшие в ту же строку. */
    for (; p < end; p += line)
        __asm__ volatile ("dc civac, %0" :: "r"(p) : "memory");
    dsb_sy();
}
