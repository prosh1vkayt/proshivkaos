/* arch/arm64/cpu.c — аналог arch/x86/cpu.c плюс обработчик исключений.
 *
 * x86: "cli; hlt" — запретить прерывания и встать до следующего.
 * ARM64: "wfi" (Wait For Interrupt) — ровно та же идея, только маскировка
 * прерываний живёт отдельно, в регистре DAIF (её нам уже поставил boot.S).
 */
#include "hal.h"
#include "hal_time.h"
#include "arm64.h"

const char *hal_arch_name(void) { return "ARM64"; }

void hal_arch_init(void) {
    /* Порядок важен: сначала MMU (иначе всё ОЗУ некэшируемое и медленное),
       потом системный счётчик — hal_time_ms() нужен уже драйверам. */
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
