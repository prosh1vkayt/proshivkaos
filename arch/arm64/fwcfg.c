/* arch/arm64/fwcfg.c — интерфейс fw_cfg гипервизора QEMU.
 *
 * Это "канал связи" между прошивкой/ядром и самим QEMU: через него
 * запрашивают карту памяти, ACPI-таблицы, параметры загрузки — и, что нам
 * тут и нужно, настраивают ramfb (простейший фреймбуфер без единого
 * драйвера дисплея, см. arch/arm64/ramfb.c).
 *
 * Аналогов на x86 у нас два: PCI-перебор в arch/x86/pci.c (там мы тоже
 * ищем устройство и вытаскиваем у него адрес буфера) и VBE-регистры в
 * arch/x86/vbe.c. Роль та же — узнать у платформы, куда писать пиксели.
 *
 * Особенность: fw_cfg общается в BIG-ENDIAN, независимо от порядка байт
 * процессора. AArch64 у нас little-endian, поэтому все многобайтные поля
 * проходят через bswap*.
 */
#include "arm64.h"

/* Смещения регистров (docs/specs/fw_cfg.rst в дереве QEMU) */
#define FWCFG_REG_DATA  0x00
#define FWCFG_REG_SEL   0x08
#define FWCFG_REG_DMA   0x10

#define FWCFG_SIG       0x00   /* селектор: сигнатура "QEMU"   */
#define FWCFG_FILE_DIR  0x19   /* селектор: каталог файлов      */

/* Биты поля control в структуре DMA-запроса */
#define DMA_ERROR   0x01
#define DMA_READ    0x02
#define DMA_SKIP    0x04
#define DMA_SELECT  0x08
#define DMA_WRITE   0x10

typedef struct {
    uint32_t control;   /* big-endian */
    uint32_t length;    /* big-endian */
    uint64_t address;   /* big-endian */
} __attribute__((packed)) fwcfg_dma_t;

typedef struct {
    uint32_t size;      /* big-endian */
    uint16_t select;    /* big-endian */
    uint16_t reserved;
    char     name[56];
} __attribute__((packed)) fwcfg_file_t;

/* Буфер под DMA-запрос: выровнен на строку кэша, чтобы обслуживание кэша
 * не задевало соседние переменные. */
static fwcfg_dma_t g_dma __attribute__((aligned(64)));

static int g_available = 0;

static void fwcfg_select(uint16_t key) {
    mmio_write16(VIRT_FWCFG_BASE + FWCFG_REG_SEL, bswap16(key));
}

/* Чтение идёт побайтно и последовательно: каждое обращение к регистру
 * данных сдвигает внутренний курсор выбранного "файла" на байт вперёд. */
static void fwcfg_read(void *dst, uint32_t len) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++)
        p[i] = mmio_read8(VIRT_FWCFG_BASE + FWCFG_REG_DATA);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int fwcfg_init(void) {
    char sig[4] = {0, 0, 0, 0};

    fwcfg_select(FWCFG_SIG);
    fwcfg_read(sig, 4);

    /* Устройство обязано представиться четырьмя байтами "QEMU". Если их
       нет — мы не под QEMU (или fw_cfg не подключён), и трогать DMA-регистр
       нельзя: по этому адресу может не быть вообще ничего. */
    g_available = (sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U');
    return g_available;
}

int fwcfg_find_file(const char *name, uint16_t *select, uint32_t *size) {
    if (!g_available) return 0;

    uint32_t count_be;
    fwcfg_select(FWCFG_FILE_DIR);
    fwcfg_read(&count_be, 4);

    uint32_t count = bswap32(count_be);
    if (count > 4096) return 0;   /* защита от мусора: столько файлов не бывает */

    /* Каталог читается строго последовательно, одним проходом — назад
       отмотать нельзя, поэтому перебираем все записи подряд. */
    for (uint32_t i = 0; i < count; i++) {
        fwcfg_file_t f;
        fwcfg_read(&f, sizeof(f));
        f.name[sizeof(f.name) - 1] = '\0';

        if (str_eq(f.name, name)) {
            *select = bswap16(f.select);
            *size   = bswap32(f.size);
            return 1;
        }
    }
    return 0;
}

int fwcfg_dma_write(uint16_t select, const void *data, uint32_t len) {
    if (!g_available) return 0;

    /* Данные читает само устройство, напрямую из ОЗУ — значит, наши
       записи обязаны быть уже в ОЗУ, а не в кэше процессора. */
    arch_dcache_clean(data, len);

    g_dma.control = bswap32(((uint32_t)select << 16) | DMA_SELECT | DMA_WRITE);
    g_dma.length  = bswap32(len);
    g_dma.address = bswap64((uint64_t)(uintptr_t)data);
    arch_dcache_clean(&g_dma, sizeof(g_dma));
    dsb_sy();

    /* Запись адреса запроса в DMA-регистр — это и есть "звонок"
       устройству: как только 64-битное значение уехало, QEMU начинает
       обработку. */
    mmio_write64(VIRT_FWCFG_BASE + FWCFG_REG_DMA,
                 bswap64((uint64_t)(uintptr_t)&g_dma));

    /* Готовность устройство отмечает, обнулив поле control (или подняв в
       нём бит ошибки) — опять же записью в ОЗУ мимо нашего кэша. */
    for (int guard = 0; guard < 1000000; guard++) {
        arch_dcache_invalidate(&g_dma, sizeof(g_dma));
        uint32_t ctl = bswap32(g_dma.control);

        if (ctl & DMA_ERROR) return 0;
        if (ctl == 0)        return 1;
    }
    return 0;   /* не дождались — считаем, что интерфейса нет */
}
