/* arch/arm64/cpu.c — аналог arch/x86/cpu.c плюс обработчик исключений.
 *
 * x86: "cli; hlt" — запретить прерывания и встать до следующего.
 * ARM64: "wfi" (Wait For Interrupt) — ровно та же идея, только маскировка
 * прерываний живёт отдельно, в регистре DAIF (её нам уже поставил boot.S).
 */
#include "hal.h"
#include "hal_time.h"
#include "arm64.h"
#include "platform.h"

const char *hal_arch_name(void) { return "ARM64"; }

/* Строка model из device tree либо — если дерева не было — имя платы,
 * зашитое при сборке. Разницу видно по самому значению. */
const char *hal_board_name(void) {
    const platform_info_t *pi = platform();
    return pi->model ? pi->model : "ARM64";
}

/* MIDR_EL1 — паспорт ядра: старший байт говорит, кто его сделал, биты
 * 15:4 — какое именно ядро. Перечислены ядра, которые реально встречаются
 * в телефонах и в QEMU; всё остальное честно показывается кодом. */
const char *hal_cpu_name(void) {
    static char buf[32];

    uint64_t midr;
    __asm__ volatile ("mrs %0, midr_el1" : "=r"(midr));

    unsigned implementer = (unsigned)((midr >> 24) & 0xFF);
    unsigned part        = (unsigned)((midr >> 4)  & 0xFFF);

    const char *vendor;
    switch (implementer) {
        case 0x41: vendor = "ARM";      break;
        case 0x51: vendor = "QUALCOMM"; break;
        case 0x4E: vendor = "NVIDIA";   break;
        case 0x53: vendor = "SAMSUNG";  break;
        case 0x61: vendor = "APPLE";    break;
        case 0x48: vendor = "HISILICON"; break;
        default:   vendor = "CPU";      break;
    }

    const char *core = 0;
    switch (part) {
        case 0xD03: core = "CORTEX-A53"; break;
        case 0xD04: core = "CORTEX-A35"; break;
        case 0xD05: core = "CORTEX-A55"; break;
        case 0xD07: core = "CORTEX-A57"; break;
        case 0xD08: core = "CORTEX-A72"; break;
        case 0xD09: core = "CORTEX-A73"; break;
        case 0xD0A: core = "CORTEX-A75"; break;
        case 0xD0B: core = "CORTEX-A76"; break;
        case 0xD0D: core = "CORTEX-A77"; break;
        case 0xD41: core = "CORTEX-A78"; break;
        case 0xD44: core = "CORTEX-X1";  break;
        case 0xD46: core = "CORTEX-A510"; break;
        case 0xD47: core = "CORTEX-A710"; break;
        case 0xD48: core = "CORTEX-X2";  break;
        default:    core = 0;            break;
    }

    int n = 0;
    for (const char *p = vendor; *p && n < 24; p++) buf[n++] = *p;
    buf[n++] = ' ';

    if (core) {
        for (const char *p = core; *p && n < 30; p++) buf[n++] = *p;
    } else {
        static const char hex[] = "0123456789ABCDEF";
        buf[n++] = '0'; buf[n++] = 'X';
        buf[n++] = hex[(part >> 8) & 0xF];
        buf[n++] = hex[(part >> 4) & 0xF];
        buf[n++] = hex[part & 0xF];
    }
    buf[n] = '\0';
    return buf;
}

void hal_arch_init(void *boot_info) {
    /* Порядок здесь не случаен, и каждый шаг зависит от предыдущего.
     *
     * 1. Разбор device tree — раньше всего. Пока неизвестен адрес порта,
     *    печатать всё равно некуда, так что и отладить этот шаг нечем:
     *    он обязан работать вслепую. Разбор идёт ещё без MMU, поэтому в
     *    arch/arm64/fdt.c всё читается побайтно (невыровненный доступ к
     *    некэшируемой памяти — гарантированный abort).
     * 2. Порт — сразу следом: с этого момента появляется отладочный вывод.
     * 3. Отчёт о найденном железе. Первое, на что смотришь, когда система
     *    молчит на новом устройстве.
     * 4. MMU — иначе всё ОЗУ некэшируемое и отрисовка кадра тормозит на
     *    порядок.
     * 5. Системный счётчик — hal_time_ms() нужен уже драйверам. */
    platform_probe(boot_info);
    uart_init();
    uart_report_platform();

    mmu_init();
    hal_time_init();
}

void hal_cpu_halt(void) {
    __asm__ volatile ("msr daifset, #0xf");   /* замаскировать D/A/I/F */
    __asm__ volatile ("wfi");
}

/* Расшифровка EC (Exception Class) — старших 6 бит ESR_EL1. Перечислены
 * только те причины, на которые реально можно налететь в ядре без
 * пользовательских процессов; всё остальное падает в "unknown". */
static const char *exception_class_name(uint64_t esr) {
    switch ((esr >> 26) & 0x3F) {
        case 0x00: return "unknown reason";
        case 0x0E: return "illegal execution state";
        case 0x15: return "svc (system call)";
        case 0x18: return "trapped msr/mrs";
        case 0x20: case 0x21: return "instruction abort (bad code address)";
        case 0x22: return "pc alignment fault";
        case 0x24: case 0x25: return "data abort (bad data address)";
        case 0x26: return "sp alignment fault";
        case 0x2F: return "serror (external abort)";
        default:   return "unhandled exception";
    }
}

static const char *vector_name(uint64_t index) {
    switch (index & 3) {
        case 0:  return "sync";
        case 1:  return "irq";
        case 2:  return "fiq";
        default: return "serror";
    }
}

/* Вызывается из arch/arm64/vectors.S. Возврата нет — уходим в панику,
 * предварительно выплюнув в UART всё, что известно о сбое: без этого
 * отладка на голом железе превращается в гадание. */
void arm64_exception_handler(uint64_t index, uint64_t esr, uint64_t far, uint64_t elr) {
    uart_write("\n*** ARM64 EXCEPTION ***\n");
    uart_write("  vector : "); uart_write(vector_name(index));
    uart_write(" (#"); uart_write_hex(index); uart_write(")\n");
    uart_write("  cause  : "); uart_write(exception_class_name(esr)); uart_putc('\n');
    uart_write("  ESR_EL1: "); uart_write_hex(esr); uart_putc('\n');
    uart_write("  FAR_EL1: "); uart_write_hex(far); uart_putc('\n');
    uart_write("  ELR_EL1: "); uart_write_hex(elr); uart_putc('\n');

    hal_panic(exception_class_name(esr));
}
