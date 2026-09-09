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
    /* 0. Постоянный журнал. Продолжает метку, поставленную в boot.S ещё
     *    до спуска из EL2, и подхватывает вывод порта — с этого момента
     *    каждая строка переживает перезагрузку и читается из Android.
     *    Первым делом потому, что смысл он имеет ровно до тех пор, пока
     *    не заработало что-то более удобное. */
    /* Полоса 4, жёлтая: код на C пошёл. Полосы 0-3 ставит boot.S; с этой
       начинается то, что уже может сломаться по-настоящему. */
    early_fb_band(4, 255, 255, 0);

    /* Адрес загрузки больше не показываем полосами: он выяснен и записан
     * в arch/arm64/boards/mido.h — загрузчик кладёт образ по 0x10080000.
     * Полосы адреса занимали до двенадцати позиций и перекрывали
     * диагностические, что однажды уже сбило проверку с толку. В журнал
     * адрес по-прежнему пишет boot.S. */

#if defined(BOARD_WDOG_BASE) && !defined(CONFIG_WDOG_KEEP)
    /* Сторожевой таймер — прежде всего остального.
     *
     * Загрузчик оставляет его заведённым, и если систему не гладить, он
     * перезагрузит телефон через одиннадцать секунд. Пока мы отлаживаемся
     * по полосам на экране, это означает, что любое зависание успевает
     * стать перезагрузкой раньше, чем его успеешь рассмотреть. */
    mmio_write32(BOARD_WDOG_BASE + BOARD_WDOG_EN_OFF, 0);
    dsb_sy();
#endif

    ramoops_init(boot_info);

    /* Отметки этапов. Каждая следующая означает, что предыдущий шаг
     * прошёл целиком: журнал обрывается ровно там, где система умерла, и
     * это единственный способ узнать место падения, пока к отладочному
     * порту не припаян кабель. */
    early_con_init();
    early_con_color("proshivkaOS NEXT\n", 120, 220, 255);

    ramoops_write("[1] razbor dereva ustroystv\n");
    early_con_puts("1 razbor dereva\n");
    platform_probe(boot_info);

    ramoops_write("[2] derevo razobrano, podnimaem port\n");
    early_fb_band(5, 0, 255, 255);      /* голубая: дерево разобрано */
    early_con_puts("2 port, ekran ");
    early_con_hex(platform()->fb_addr);
    early_con_puts(" model ");
    early_con_puts(platform()->model ? platform()->model : "?");
    early_con_puts("\n");
    uart_init();
    uart_report_platform();

    ramoops_write("[3] vklyuchaem MMU i kesh\n");
    early_fb_band(6, 255, 0, 255);      /* сиреневая: порт поднят */
    early_con_puts("3 vklyuchaem MMU\n");
    mmu_init(boot_info);

    /* Щель между включением MMU и этой точкой оказалась смертельной, а
       действий в ней всего два. Размечаем каждое отдельно, и НЕ ПОДРЯД:
       две соседние полосы похожих цветов на фотографии сливаются в одну,
       и прошлый раз это уже стоило нам круга. Между ними оставлен
       пропуск. */
    early_fb_band(17, 0, 255, 255);     /* ярко-голубая: вернулись из mmu_init */
    early_con_puts("4 MMU vklyuchen\n");

    ramoops_write("[4] sistemnyy schetchik\n");
    early_fb_band(19, 200, 0, 255);     /* фиолетовая: журнал пережит */

    early_fb_band(7, 255, 128, 0);      /* оранжевая: MMU и кэш включены */
    hal_time_init();

    early_fb_band(8, 128, 255, 128);   /* салатовая: счётчик пошёл */
    early_con_puts("5 schetchik, arch gotov\n");

#ifdef CONFIG_USB_DWC3
    /* 6. Провод.
     *
     * Поднимается здесь, а не позже, по одной причине: с этого мгновения
     * всё, что система печатает, уходит в компьютер прямо во время
     * работы. Раньше журнал доставался только через recovery после
     * перезагрузки — круг отладки занимал минуты и требовал человека на
     * каждом шаге.
     *
     * Ему нужен счётчик времени (задержки приёмопередатчика расписаны в
     * микросекундах), поэтому не раньше пятого шага. */
    usb_dwc3_init();
#endif

    ramoops_write("[5] hal_arch_init zavershen\n");
}

/* Отладочная метка из переносимого кода — см. hal/hal.h. Здесь она
 * превращается в полосу на экране; на платах без такой возможности вызов
 * раскрывается в пустоту сам собой. */
void hal_debug_mark(int index, unsigned char r, unsigned char g, unsigned char b) {
    early_fb_band(index, r, g, b);
}

void hal_debug_text(const char *s) { early_con_puts(s); }

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
    /* Защита от повторного входа.
     *
     * Сбой внутри обработчика сбоев — не редкость: он печатает, а печать
     * обращается к памяти, и если сломана как раз она, всё повторяется по
     * кругу. Экран при этом заполняется одинаковыми сообщениями, и первое
     * — единственное содержательное — уезжает за край.
     *
     * Поэтому со второго раза просто останавливаемся: то, что уже
     * напечатано, ценнее того, что напечаталось бы дальше. */
    static int g_in_handler = 0;
    if (g_in_handler) {
        for (;;) __asm__ volatile ("wfi");
    }
    g_in_handler = 1;

    /* ПЕРВЫМ ДЕЛОМ — на экран, до любого вывода в порт.
     *
     * Пока к отладочному порту не припаян кабель, сообщение обработчика
     * не видно нигде, и сбой неотличим от зависания: экран замирает, а
     * через несколько секунд сторожевой таймер перезагружает телефон.
     * Полоса отвечает на главный вопрос — упали мы или встали, — а две
     * следующие называют вид сбоя. */
    early_fb_band(11, 255, 0, 0);       /* красная: мы в обработчике */

    /* Вид сбоя кодируется ЦВЕТОМ одной полосы, а не её положением:
       положений не хватает, а цвет различить не сложнее. */
    switch ((esr >> 26) & 0x3F) {
        case 0x20: case 0x21:  early_fb_band(13, 255, 255, 255); break; /* белая: не смогли прочитать команду */
        case 0x24: case 0x25:  early_fb_band(13, 255, 255,   0); break; /* жёлтая: не смогли обратиться к данным */
        case 0x2F:             early_fb_band(13, 255,   0, 255); break; /* сиреневая: внешний сбой шины */
        default:               early_fb_band(13,   0, 255, 255); break; /* голубая: прочее */
    }

    /* Полное описание сбоя текстом. Раньше оно уходило в порт, которого
       на этом аппарате никто не слышит, и о падении можно было судить
       только по красной полосе. */
    early_con_color("\n*** SBOY ***\n", 255, 80, 80);
    early_con_puts(vector_name(index));
    early_con_puts(" ");
    early_con_puts(exception_class_name(esr));
    early_con_puts("\nESR "); early_con_hex(esr);
    early_con_puts("\nFAR "); early_con_hex(far);
    early_con_puts("\nELR "); early_con_hex(elr);
    early_con_puts("\n");

    uart_write("\n*** ARM64 EXCEPTION ***\n");
    uart_write("  vector : "); uart_write(vector_name(index));
    uart_write(" (#"); uart_write_hex(index); uart_write(")\n");
    uart_write("  cause  : "); uart_write(exception_class_name(esr)); uart_putc('\n');
    uart_write("  ESR_EL1: "); uart_write_hex(esr); uart_putc('\n');
    uart_write("  FAR_EL1: "); uart_write_hex(far); uart_putc('\n');
    uart_write("  ELR_EL1: "); uart_write_hex(elr); uart_putc('\n');

    hal_panic(exception_class_name(esr));
}
