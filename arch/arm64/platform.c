/* arch/arm64/platform.c — определение железа по device tree. */
#include "platform.h"
#include "fdt.h"
#include "gfxfb.h"
#include "boards/board.h"

static platform_info_t g_pi;

const platform_info_t *platform(void) { return &g_pi; }

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* ---------------- последовательный порт ---------------- */

/* Список известных нам контроллеров. Порядок важен: сначала то, что
 * встречается чаще, но реально решает не он, а stdout-path — см. ниже. */
static const struct { const char *compat; int kind; } g_uart_types[] = {
    { "arm,pl011",             UART_KIND_PL011 },
    { "qcom,msm-lsuart-v14",   UART_KIND_MSM   },
    { "qcom,msm-uartdm-v1.4",  UART_KIND_MSM   },
    { "qcom,msm-uartdm",       UART_KIND_MSM   },
    { "qcom,msm-hsuart-v14",   UART_KIND_MSM   },
};
#define UART_TYPE_COUNT ((int)(sizeof(g_uart_types) / sizeof(g_uart_types[0])))

/* По какому compatible узнаётся тип контроллера. */
static int kind_of_node(const fdt_node_t *n) {
    for (int i = 0; i < UART_TYPE_COUNT; i++) {
        uint32_t len = 0;
        const char *list = (const char *)fdt_prop(n, "compatible", &len);
        if (!list) return UART_KIND_NONE;

        uint32_t off = 0;
        while (off < len) {
            const char *entry = list + off;
            if (str_eq(entry, g_uart_types[i].compat))
                return g_uart_types[i].kind;
            while (off < len && list[off]) off++;
            off++;
        }
    }
    return UART_KIND_NONE;
}

/* stdout-path — единственное место в дереве, где прямо сказано, какой из
 * нескольких портов является КОНСОЛЬЮ. У телефона их обычно два, и второй
 * уходит на модуль Bluetooth: угадав неверно, вывод отправишь в эфир.
 *
 * Формат бывает трёх видов: полный путь "/pl011@9000000", путь со
 * скоростью "/soc/serial@78af000:115200n8" и короткий псевдоним
 * "serial0:115200n8", который надо развернуть через узел /aliases. */
static int uart_from_stdout_path(fdt_node_t *out) {
    fdt_node_t chosen;
    if (!fdt_path("/chosen", &chosen)) return 0;

    const char *sp = fdt_prop_str(&chosen, "stdout-path");
    if (!sp || !*sp) return 0;

    /* Отрезаем параметры скорости после двоеточия. */
    char path[128];
    int n = 0;
    while (sp[n] && sp[n] != ':' && n < (int)sizeof(path) - 1) {
        path[n] = sp[n];
        n++;
    }
    path[n] = '\0';
    if (n == 0) return 0;

    if (path[0] == '/')
        return fdt_path(path, out);

    /* Не путь, а псевдоним — разворачиваем через /aliases. */
    fdt_node_t aliases;
    if (!fdt_path("/aliases", &aliases)) return 0;

    const char *real = fdt_prop_str(&aliases, path);
    if (!real || real[0] != '/') return 0;
    return fdt_path(real, out);
}

static void probe_uart(void) {
    fdt_node_t node;
    uint64_t addr = 0;

    /* 1. Консоль, назначенная загрузчиком. */
    if (uart_from_stdout_path(&node)) {
        int kind = kind_of_node(&node);
        if (kind != UART_KIND_NONE && fdt_reg(&node, 0, &addr, 0) && addr) {
            g_pi.uart_kind = kind;
            g_pi.uart_base = addr;
            g_pi.uart_from_fdt = 1;
            return;
        }
    }

    /* 2. Первый попавшийся знакомый контроллер. Так работает Redmi Note 4:
       в его дереве stdout-path нет вовсе, зато есть узел с compatible
       "qcom,msm-lsuart-v14" по адресу 0x78AF000 — тот же, что прописан в
       earlycon стоковой командной строки. */
    for (int i = 0; i < UART_TYPE_COUNT; i++) {
        if (!fdt_find_compatible(g_uart_types[i].compat, &node)) continue;
        if (!fdt_reg(&node, 0, &addr, 0) || !addr) continue;

        g_pi.uart_kind = g_uart_types[i].kind;
        g_pi.uart_base = addr;
        g_pi.uart_from_fdt = 1;
        return;
    }

    /* 3. Дерева нет или порт в нём не описан — берём константу платы. */
    g_pi.uart_kind = BOARD_UART_KIND;
    g_pi.uart_base = BOARD_UART_BASE;
    g_pi.uart_from_fdt = 0;
}

/* ---------------- контроллер прерываний ---------------- */

static void probe_gic(void) {
    static const char *const compat[] = {
        "arm,gic-400", "arm,cortex-a15-gic", "arm,cortex-a7-gic",
        "qcom,msm-qgic2", "arm,gic-v3"
    };
    fdt_node_t node;
    uint64_t addr = 0;

    for (int i = 0; i < (int)(sizeof(compat) / sizeof(compat[0])); i++) {
        if (!fdt_find_compatible(compat[i], &node)) continue;
        if (!fdt_reg(&node, 0, &addr, 0)) continue;
        g_pi.gic_base = addr;
        return;
    }
}

/* ---------------- готовый фреймбуфер ---------------- */

/* Формат из свойства format узла simple-framebuffer. Поддерживаем ровно
 * те два, что умеет выводить gui/gfxfb.c. */
static int fb_format_of(const char *s) {
    if (!s) return -1;
    if (str_eq(s, "r8g8b8"))                        return GFXFB_FMT_RGB24;
    if (str_eq(s, "a8r8g8b8") || str_eq(s, "x8r8g8b8")) return GFXFB_FMT_XRGB32;
    return -1;   /* r5g6b5 и прочее пока не выводим */
}

static void probe_framebuffer(void) {
    fdt_node_t node;

    /* Узел может лежать и в корне, и внутри /chosen — спецификация
       simple-framebuffer допускает оба варианта. */
    if (!fdt_find_compatible("simple-framebuffer", &node))
        return;

    uint64_t addr = 0;
    uint32_t w = 0, h = 0, stride = 0;
    if (!fdt_reg(&node, 0, &addr, 0) || !addr) return;
    if (!fdt_prop_u32(&node, "width", &w))   return;
    if (!fdt_prop_u32(&node, "height", &h))  return;
    if (!fdt_prop_u32(&node, "stride", &stride)) return;

    int fmt = fb_format_of(fdt_prop_str(&node, "format"));
    if (fmt < 0) return;
    if (w == 0 || h == 0 || stride == 0) return;

    g_pi.fb_valid  = 1;
    g_pi.fb_addr   = addr;
    g_pi.fb_width  = (int)w;
    g_pi.fb_height = (int)h;
    g_pi.fb_stride = (int)stride;
    g_pi.fb_format = fmt;
}

/* ---------------- точка входа ---------------- */

void platform_probe(const void *dtb) {
    g_pi.fdt_ok        = 0;
    g_pi.model         = BOARD_NAME;
    g_pi.uart_kind     = UART_KIND_NONE;
    g_pi.uart_base     = 0;
    g_pi.uart_from_fdt = 0;
    g_pi.gic_base      = 0;
    g_pi.fb_valid      = 0;

    g_pi.fdt_ok = fdt_init(dtb);

    if (g_pi.fdt_ok) {
        const char *m = fdt_model();
        if (m && *m) g_pi.model = m;

        probe_gic();
        probe_framebuffer();
    }

    /* Порт определяем в любом случае: даже без дерева нужно куда-то
       печатать, и константа платы для этого и заведена. */
    probe_uart();

    /* Дерево есть, но фреймбуфера в нём не оказалось — так устроено
       стоковое дерево Redmi Note 4: узла simple-framebuffer в нём нет,
       это изобретение мейнлайна. Берём адрес из константы платы. */
#ifdef BOARD_HAS_STATIC_FB
    if (!g_pi.fb_valid) {
        g_pi.fb_valid  = 1;
        g_pi.fb_addr   = BOARD_FB_ADDR;
        g_pi.fb_width  = BOARD_FB_WIDTH;
        g_pi.fb_height = BOARD_FB_HEIGHT;
        g_pi.fb_stride = BOARD_FB_STRIDE;
        g_pi.fb_format = BOARD_FB_FORMAT;
    }
#endif
}
