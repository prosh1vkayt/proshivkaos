/* gui/terminal_net.c — клей между терминалом и сетевыми командами.
 * Сами команды — в net/netcmd.c и ничего не знают про окна: сюда они
 * получают только net_out_t, который пишет в консоль терминала. */
#include "terminal_app.h"
#include "terminal_net.h"
#include "netcore.h"
#include "netcmd.h"

int hal_net_init(void);        /* arch/x86/net_rtl8139.c; то же объявлено в hal.h */

static void (*redraw_cb)(void);
static int have_nic;

static void out_putc(void *ctx, char c) {
    terminal_app_t *a = (terminal_app_t *)ctx;
    gconsole_putc(&a->console, c);
}

static void out_style(void *ctx, int style) {
    terminal_app_t *a = (terminal_app_t *)ctx;
    gconsole_set_ink(&a->console, style == NET_STYLE_ERROR ? GFX_LIGHT_RED : a->console.fg);
}

static void out_flush(void *ctx) {
    (void)ctx;
    if (redraw_cb) redraw_cb();
}

static int net_command(terminal_app_t *app, int argc, char *argv[]) {
    if (argc < 1 || !netcmd_is(argv[0])) return 0;

    net_out_t out = { out_putc, out_style, out_flush, app, 1 };

    if (!have_nic) {
        out_putc(app, '\n');
        out_style(app, NET_STYLE_ERROR);
        gconsole_write(&app->console, "no network card (QEMU: -device rtl8139)");
        out_style(app, NET_STYLE_NORMAL);
        return 1;
    }

    /* Пока команда ждёт ответа, интерфейс продолжает жить (насос), а ввод в
     * терминал заблокирован флагом busy — кроме Ctrl+C. */
    app->busy = 1;
    app->cancel = 0;
    netcmd_exec(&out, argc, argv);
    app->busy = 0;
    app->cancel = 0;
    return 1;
}

void terminal_net_install(void (*redraw)(void), int (*pump)(void)) {
    redraw_cb = redraw;

    have_nic = (hal_net_init() == 0);
    if (have_nic) net_cfg_init();
    net_set_pump(pump);

    terminal_app_set_ext_command(net_command);
    terminal_app_set_ext_help("IFCONFIG ARPING PING DHCP UDPSEND NSLOOKUP HTTP");
}
