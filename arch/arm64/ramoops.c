/* arch/arm64/ramoops.c — журнал, переживающий перезагрузку.
 *
 * ЗАЧЕМ. Отлаживать систему на голом железе принято через UART, но на этом
 * телефоне порт выведен на разъём наушников и требует паяного кабеля с
 * делителем напряжения. Пока его нет, система, упавшая до первой строки
 * вывода, неотличима от системы, до которой загрузчик вообще не доехал.
 *
 * Выход нашёлся в самом телефоне. Ядро Linux на нём сообщает при старте:
 *
 *     ramoops: attached 0x100000@0x9ff00000
 *     console [pstore0] enabled
 *
 * Это ramoops — область ОЗУ, намеренно исключённая из общего пула
 * (reserved-memory/pstore_reserve_mem_region) и потому не затираемая ни
 * загрузчиком, ни ядром. Linux складывает туда консоль, чтобы после
 * внезапной перезагрузки показать, что происходило перед ней.
 *
 * Содержимое ОЗУ переживает тёплый сброс. Значит, мы можем писать туда
 * СВОЙ лог — а потом перезагрузить телефон в Android и прочитать его
 * обычным файлом:
 *
 *     adb shell su -c 'cat /sys/fs/pstore/console-ramoops'
 *
 * Получается последовательный порт без кабеля. Медленный (одно сообщение
 * на перезагрузку), зато работающий на любом этапе — включая самую первую
 * инструкцию, задолго до того, как разобрано дерево устройств.
 *
 * ФОРМАТ. Он предельно простой и не менялся годами (fs/pstore/ram_core.c):
 *
 *     struct persistent_ram_buffer {
 *         uint32_t sig;     0x43474244, в ASCII "DBGC"
 *         uint32_t start;   курсор записи
 *         uint32_t size;    сколько байт данных достоверны
 *         uint8_t  data[];
 *     };
 *
 * При старте ядро проверяет сигнатуру. Совпала и поля осмысленные — оно
 * забирает данные и отдаёт их файлом console-ramoops. Не совпала — молча
 * начинает с чистого листа. То есть испортить этим ничего нельзя: худшее,
 * что бывает при ошибке, — пустой файл.
 *
 * РАЗМЕЩЕНИЕ. Область на 1 МиБ поделена на зоны, и консоль лежит не с
 * начала. Порядок задан в ramoops_probe(): сначала дампы паник, затем
 * консоль, затем ftrace и pmsg. Размеры зон телефон сообщает сам:
 *
 *     /sys/module/ramoops/parameters/{mem_size,console_size,ftrace_size,pmsg_size}
 *     = 1048576, 524288, 4096, 32768
 *
 * Отсюда зона дампов = 1048576 - 524288 - 4096 - 32768 = 0x77000, и консоль
 * начинается на 0x77000 от начала области. Значения вынесены в mido.h.
 *
 * КЭШ. Писать приходится с включённым MMU, то есть в кэшируемую память. При
 * жёстком сбросе кэш не выгружается, и лог пропал бы вместе с ним — поэтому
 * после каждой записи данные явно выталкиваются в ОЗУ.
 */
#include "arm64.h"
#include "boards/board.h"
#include "fdt.h"

#ifdef CONFIG_LOG_RAMOOPS

#define PRAM_SIG        0x43474244u    /* "DBGC" */
#define PRAM_HDR_BYTES  12             /* sig + start + size */

static volatile uint8_t *g_base = 0;   /* начало зоны консоли  */
static volatile uint8_t *g_data = 0;   /* начало области данных */
static uint32_t g_capacity = 0;
static uint32_t g_used     = 0;
static int      g_ready    = 0;

static void hdr_write(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_base + off) = v;
}

/* Вытолкнуть в ОЗУ то, что мы записали. Без этого лог остался бы в кэше
 * процессора и пропал бы при сбросе — ровно в том случае, ради которого всё
 * и затевалось. */
static void flush(const void *p, uint32_t len) {
    arch_dcache_clean(p, len);
}

/* Найти область журнала в дереве устройств.
 *
 * Зашивать адрес в код нельзя: он задаётся дереву конкретной системы и
 * между прошивками меняется. На этом же телефоне под Android область
 * лежала по 0x9FF00000, а под mainline-ядром — по 0xBFE80000. Промах на
 * этом месте не ломает ничего заметного, но делает всю затею бесполезной:
 * мы пишем в одно место, система телефона ищет в другом.
 *
 * Зоны внутри области ядро раскладывает по порядку (fs/pstore/ram.c,
 * ramoops_probe): сначала дампы паник, следом консоль, затем ftrace и
 * pmsg. Нам нужна консоль, поэтому её начало — это размер области минус
 * размеры всех зон, которые идут ПОСЛЕ дампов.
 *
 * Возвращает 0, если дерева нет или узла в нём не оказалось; тогда в ход
 * идут константы платы. */
static int ramoops_from_fdt(const void *dtb, uint64_t *addr, uint32_t *size) {
    if (!fdt_init(dtb)) return 0;

    fdt_node_t node;
    if (!fdt_find_compatible("ramoops", &node)) return 0;

    uint64_t base = 0, total = 0;
    if (!fdt_reg(&node, 0, &base, &total) || !base || !total) return 0;

    uint32_t console = 0, ftrace = 0, pmsg = 0;
    if (!fdt_prop_u32(&node, "console-size", &console) || console == 0)
        return 0;               /* без консольной зоны писать некуда */
    fdt_prop_u32(&node, "ftrace-size", &ftrace);
    fdt_prop_u32(&node, "pmsg-size", &pmsg);

    /* Зоны обязаны помещаться в область — иначе дерево описывает не то,
       что мы думаем, и лучше отступить к константам платы. */
    uint64_t tail = (uint64_t)console + ftrace + pmsg;
    if (tail >= total) return 0;

    *addr = base + (total - tail);
    *size = console;
    return 1;
}

/* Перенести раннюю метку, поставленную в boot.S.
 *
 * boot.S пишет по константе времени сборки — до разбора дерева адрес взять
 * неоткуда. Если дерево указало другое место, метка осталась бы лежать в
 * стороне и потерялась. Переносим её, чтобы журнал остался цельным: строка
 * про полученное управление ценнее всех последующих, потому что она одна
 * доказывает, что загрузчик до нас доехал. */
static uint32_t carry_over_early_mark(volatile uint8_t *dst_data, uint32_t capacity) {
    const volatile uint8_t *src = (const volatile uint8_t *)BOARD_PSTORE_CONSOLE_ADDR;

    if (*(const volatile uint32_t *)(src + 0) != PRAM_SIG) return 0;

    uint32_t n = *(const volatile uint32_t *)(src + 8);
    if (n == 0 || n > capacity) return 0;

    for (uint32_t i = 0; i < n; i++)
        dst_data[i] = src[PRAM_HDR_BYTES + i];
    return n;
}

void ramoops_init(const void *dtb) {
    uint64_t addr = BOARD_PSTORE_CONSOLE_ADDR;
    uint32_t size = BOARD_PSTORE_CONSOLE_SIZE;

    int from_fdt = ramoops_from_fdt(dtb, &addr, &size);

    g_base     = (volatile uint8_t *)(uintptr_t)addr;
    g_data     = g_base + PRAM_HDR_BYTES;
    g_capacity = size - PRAM_HDR_BYTES;

    if (from_fdt && addr != (uint64_t)BOARD_PSTORE_CONSOLE_ADDR) {
        /* Дерево увело нас в другое место — забираем метку с собой. */
        g_used = carry_over_early_mark(g_data, g_capacity);
    } else {
        /* Пишем туда же, куда писал boot.S: продолжаем с его метки.
           Признак — уже проставленная сигнатура и осмысленная длина. */
        uint32_t sig  = *(volatile uint32_t *)(g_base + 0);
        uint32_t used = *(volatile uint32_t *)(g_base + 8);
        g_used = (sig == PRAM_SIG && used <= g_capacity) ? used : 0;
    }

    hdr_write(0, PRAM_SIG);
    hdr_write(4, 0);          /* start: кольцо не проворачивалось */
    hdr_write(8, g_used);

    g_ready = 1;
    flush((const void *)g_base, PRAM_HDR_BYTES);
    if (g_used) flush((const void *)g_data, g_used);

    ramoops_write(from_fdt ? "[log] oblast zhurnala vzyata iz dereva\n"
                           : "[log] oblast zhurnala: konstanta platy\n");
}

/* Где на самом деле лежит журнал.
 *
 * Спрашивает arch/arm64/mmu.c: область надо отобразить, а её адрес до
 * разбора дерева неизвестен — он может отличаться от константы платы.
 * Именно на этом однажды и попались: карта отображала константу, а писали
 * мы по адресу из дерева, и первая же запись после включения MMU падала. */
int ramoops_region(uint64_t *base, uint32_t *size) {
    if (!g_ready) return 0;
    *base = (uint64_t)(uintptr_t)g_base;
    *size = BOARD_PSTORE_CONSOLE_SIZE;
    return 1;
}

void ramoops_putc(char c) {
    if (!g_ready) return;

    /* Кольца намеренно нет. Лог одной загрузки короткий, а вот потерять его
       начало — потерять самое ценное: причину падения почти всегда видно в
       первых строках, а не в последних. Поэтому по заполнении просто
       перестаём писать. */
    if (g_used >= g_capacity) return;

    g_data[g_used++] = (uint8_t)c;

    hdr_write(8, g_used);
    flush((const void *)(g_data + g_used - 1), 1);
    flush((const void *)g_base, PRAM_HDR_BYTES);
}

void ramoops_write(const char *s) {
    while (*s) ramoops_putc(*s++);
}

#endif /* CONFIG_LOG_RAMOOPS */
