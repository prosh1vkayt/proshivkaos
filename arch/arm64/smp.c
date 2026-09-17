/* arch/arm64/smp.c — остальные ядра процессора.
 *
 * ЗАЧЕМ. Система работала на одном ядре из восьми. Отрисовка и вывод кадра
 * — это миллионы независимых пикселей: полосы кадра можно считать на
 * разных ядрах одновременно, и вывод полного кадра становится во столько
 * раз быстрее, сколько ядер работает.
 *
 * КАК ЗАПУСКАЮТСЯ. Стандартным вызовом PSCI CPU_ON к доверенной среде (на
 * телефоне SMC, под эмулятором HVC — способ записан в дереве устройств).
 * Берём ядра своего кластера (аффинность 1 та же, 0 — от 1 до 3): их такт
 * уже переведён на ФАПЧ вместе с нашим (см. cpufreq_msm8953.c).
 *
 * Ядро стартует с выключенным MMU. Всё, что оно прочтёт до включения MMU —
 * свой стек, таблицы, значения регистров, — лежит в одной структуре,
 * которую мы перед запуском выталкиваем из кэша в ОЗУ: без трансляции ядро
 * читает память мимо кэша, и незаписанное туда увидело бы устаревшим.
 *
 * КАК РАБОТАЮТ. Прерываний в системе нет, поэтому ядра ждут работы в WFE, а
 * основное будит их командой SEV. Защёлка события в WFE не даёт пропустить
 * побудку, даже если она пришла чуть раньше, чем ядро уснуло. WFE, а не
 * WFI: на WFI контроллер сна может погасить ядро целиком.
 */
#include "arm64.h"
#include "fdt.h"
#include "hal_time.h"
#include "boards/board.h"

#define SMP_MAX        4
#define STACK_WORDS    8192             /* 64 КиБ на ядро */

#define PSCI_CPU_ON    0xC4000003u

typedef void (*par_fn)(void *arg, int part, int parts);

typedef struct {
    uint64_t sp, ttbr, tcr, mair, sctlr, vbar, idx;
} smp_ctx_t;

static uint64_t  g_stacks[SMP_MAX][STACK_WORDS];
static smp_ctx_t g_ctx[SMP_MAX];

static volatile int      g_cores = 1;
static volatile int      g_online[SMP_MAX];
static volatile uint32_t g_gen = 0;
static volatile uint32_t g_done[SMP_MAX];
static par_fn            volatile g_fn;
static void             *volatile g_arg;
static volatile int      g_parts = 1;

void mmu_secondary_regs(uint64_t *ttbr, uint64_t *tcr, uint64_t *mair, uint64_t *sctlr);
void smp_secondary_main(uint64_t idx);
extern char _vectors[];
extern char smp_secondary_entry[];

__asm__(
    ".text\n"
    ".global smp_secondary_entry\n"
    ".type smp_secondary_entry, %function\n"
    "smp_secondary_entry:\n"
    "    msr  daifset, #0xf\n"
    "    mov  x19, x0\n"
    "    ldr  x1, [x19, #0]\n"
    "    mov  sp, x1\n"
    "    ldr  x1, [x19, #40]\n"
    "    msr  vbar_el1, x1\n"
    "    ldr  x1, [x19, #24]\n"
    "    msr  mair_el1, x1\n"
    "    ldr  x1, [x19, #16]\n"
    "    msr  tcr_el1, x1\n"
    "    ldr  x1, [x19, #8]\n"
    "    msr  ttbr0_el1, x1\n"
    "    isb\n"
    "    tlbi vmalle1\n"
    "    ldr  x1, [x19, #32]\n"
    "    msr  sctlr_el1, x1\n"
    "    isb\n"
    "    ldr  x0, [x19, #48]\n"
    "    bl   smp_secondary_main\n"
    "1:  wfe\n"
    "    b    1b\n"
);


/* HVC с тем же порядком регистров — для эмулятора. */
uint64_t smp_hvc_raw(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3);
__asm__(
    ".text\n"
    ".global smp_hvc_raw\n"
    ".type smp_hvc_raw, %function\n"
    "smp_hvc_raw:\n"
    "    hvc #0\n"
    "    ret\n"
    ".global smp_smc_raw4\n"
    ".type smp_smc_raw4, %function\n"
    "smp_smc_raw4:\n"
    "    smc #0\n"
    "    ret\n"
);
uint64_t smp_smc_raw4(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3);

void smp_secondary_main(uint64_t idx) {
    g_online[idx] = 1;
    uint32_t seen = g_gen;
    for (;;) {
        uint32_t gen = g_gen;
        if (gen != seen) {
            seen = gen;
            g_fn(g_arg, (int)idx, g_parts);
            g_done[idx] = gen;
        } else {
            __asm__ volatile ("wfe");
        }
    }
}

int smp_cores(void) { return g_cores; }

/* Разделить работу на все ядра и дождаться всех. part 0 — наша доля. */
void hal_parallel(par_fn fn, void *arg) {
    int n = g_cores;
    if (n <= 1) { fn(arg, 0, 1); return; }

    g_fn = fn;
    g_arg = arg;
    g_parts = n;
    uint32_t gen = g_gen + 1;
    g_gen = gen;
    __asm__ volatile ("sev");

    fn(arg, 0, n);

    uint64_t deadline = hal_time_ms() + 2000;
    for (int i = 1; i < n; i++) {
        while (g_done[i] != gen) {
            if (hal_time_ms() > deadline) {
                /* Ядро не отозвалось: доделываем его долю сами и больше на
                   него не рассчитываем. Лучше медленно, чем недорисованно. */
                early_con_puts("SMP: yadro ne otvetilo, rabotaem bez nego\n");
                for (int k = i; k < n; k++)
                    if (g_done[k] != gen) fn(arg, k, n);
                g_cores = i;
                return;
            }
            __asm__ volatile ("yield");
        }
    }
}

static int psci_hvc = 0;

static int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx) {
    uint64_t r = psci_hvc ? smp_hvc_raw(PSCI_CPU_ON, mpidr, entry, ctx)
                          : smp_smc_raw4(PSCI_CPU_ON, mpidr, entry, ctx);
    return (int64_t)r;
}

void smp_init(void) {
    fdt_node_t node;
    const char *method = 0;
    if (fdt_valid() &&
        (fdt_find_compatible("arm,psci-1.0", &node) ||
         fdt_find_compatible("arm,psci-0.2", &node) ||
         fdt_find_compatible("arm,psci", &node)))
        method = fdt_prop_str(&node, "method");
#ifdef BOARD_QEMU_VIRT
    /* Эмулятор, загрузивший ELF, дерево не передаёт; PSCI у него — HVC. */
    if (!method) method = "hvc";
#endif
    if (!method) { early_con_puts("SMP: PSCI v dereve net, odno yadro\n"); return; }
    psci_hvc = (method[0] == 'h');

    uint64_t ttbr, tcr, mair, sctlr, mpidr;
    mmu_secondary_regs(&ttbr, &tcr, &mair, &sctlr);
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint64_t base = mpidr & 0xFF00FFFF00ull;       /* та же аффинность 1..3 */
    if ((mpidr & 0xFF) != 0) { early_con_puts("SMP: my ne yadro 0, ne trogaem\n"); return; }

    int cores = 1;
    for (int i = 1; i < SMP_MAX; i++) {
        smp_ctx_t *c = &g_ctx[i];
        c->sp = (uint64_t)(uintptr_t)&g_stacks[i][STACK_WORDS - 2];
        c->sp &= ~15ull;
        c->ttbr = ttbr; c->tcr = tcr; c->mair = mair; c->sctlr = sctlr;
        c->vbar = (uint64_t)(uintptr_t)_vectors;
        c->idx = (uint64_t)i;
        arch_dcache_clean(c, sizeof(*c));
        dsb_sy();

        int64_t r = psci_cpu_on(base | (uint64_t)i,
                                (uint64_t)(uintptr_t)smp_secondary_entry,
                                (uint64_t)(uintptr_t)c);
        if (r != 0) {
            early_con_puts("SMP: PSCI CPU_ON otkazal, kod ");
            early_con_hex32((uint32_t)r);
            early_con_puts("\n");
            break;
        }
        uint64_t deadline = hal_time_ms() + 500;
        while (!g_online[i] && hal_time_ms() < deadline)
            __asm__ volatile ("yield");
        if (!g_online[i]) {
            early_con_puts("SMP: yadro zapushcheno, no ne vyshlo na svyaz\n");
            break;
        }
        cores = i + 1;
    }
    g_cores = cores;
    uart_write("SMP: rabotaet yader ");
    uart_write_hex((uint64_t)cores);
    uart_write("\n");
    early_con_puts("SMP: rabotaet yader ");
    early_con_hex32((uint32_t)cores);
    early_con_puts("\n");
}
