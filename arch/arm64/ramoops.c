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

void ramoops_init(void) {
    g_base     = (volatile uint8_t *)BOARD_PSTORE_CONSOLE_ADDR;
    g_data     = g_base + PRAM_HDR_BYTES;
    g_capacity = BOARD_PSTORE_CONSOLE_SIZE - PRAM_HDR_BYTES;

    /* Метку из boot.S не затираем, а продолжаем с неё: она поставлена до
       всего остального и говорит, что загрузчик до нас доехал. Признак —
       уже проставленная сигнатура и осмысленная длина. */
    uint32_t sig  = *(volatile uint32_t *)(g_base + 0);
    uint32_t size = *(volatile uint32_t *)(g_base + 8);

    if (sig == PRAM_SIG && size <= g_capacity)
        g_used = size;
    else
        g_used = 0;

    hdr_write(0, PRAM_SIG);
    hdr_write(4, 0);          /* start: кольцо не проворачивалось */
    hdr_write(8, g_used);

    g_ready = 1;
    flush((const void *)g_base, PRAM_HDR_BYTES);
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
