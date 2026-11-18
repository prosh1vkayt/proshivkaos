/* shell/shell.c — встроенная командная строка поверх RamFS.
 * Использует только hal.h и ramfs.h — работает одинаково на x86 и ARM64.
 *
 * ВАЖНО про cmd_edit(): полноэкранный редактор в apps/editor.c зовёт
 * hal_console_clear()/hal_console_goto(), которые пока реализованы только
 * для x86 (см. комментарий у их объявления в hal.h). Поэтому сборка
 * ARCH=arm64 text/run сейчас не слинкуется, пока в hal_console_arm64.c не
 * появятся такие же clear/goto — см. hal.h.
 */
#include "hal.h"
#include "ramfs.h"
#include "editor.h"
#include "ping.h"
#include "netcore.h"
#include "udp.h"
#include "dhcp.h"
#include "dns.h"
#include "tcp.h"
#include "lineedit.h"

#define CMD_BUF_SIZE   128
#define CMD_MAX_ARGS   16

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void put_ip(const uint8_t *ip);
static void put_mac(const uint8_t *m);
static void put_dec(unsigned v);
static int  parse_ip(const char *s, uint8_t out[4]);


/* Разбивает строку на аргументы по пробелу (in-place, без malloc). */
static int tokenize(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;

        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }
    return argc;
}

/* ---------------- Команды ---------------- */

static void cmd_ls(void) {
    const ramfs_file_t *files[RAMFS_MAX_FILES];
    size_t n = ramfs_list(files, RAMFS_MAX_FILES);

    if (n == 0) {
        hal_console_write("(pusto)\n");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        hal_console_set_color(HAL_COLOR_LIGHT_GREEN, HAL_COLOR_BLACK);
        hal_console_write(files[i]->name);
        hal_console_reset_color();
        hal_console_putc('\n');
    }
}

static void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: cat <file>\n");
        return;
    }

    const ramfs_file_t *f = ramfs_lookup(argv[1]);
    if (!f) {
        hal_console_write("cat: fayl ne nayden: ");
        hal_console_write(argv[1]);
        hal_console_putc('\n');
        return;
    }

    for (size_t i = 0; i < f->size; i++)
        hal_console_putc((char)f->data[i]);
    if (f->size == 0 || f->data[f->size - 1] != '\n')
        hal_console_putc('\n');
}

static void cmd_touch(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: touch <file>\n");
        return;
    }

    if (ramfs_touch(argv[1]) != 0)
        hal_console_write("touch: ne udalos sozdat fayl\n");
}

static void cmd_neofetch(void) {
    hal_console_set_color(HAL_COLOR_LIGHT_CYAN, HAL_COLOR_BLACK);
    hal_console_write(
        "        /\\      \n"
        "       /  \\     \n"
        "      / /\\ \\    \n"
        "     / ____ \\   \n"
        "    /_/    \\_\\  \n"
    );
    hal_console_reset_color();

    hal_console_set_color(HAL_COLOR_YELLOW, HAL_COLOR_BLACK);
    hal_console_write("proshivkaOS NEXT\n");
    hal_console_reset_color();
    hal_console_write("-----------------\n");
    hal_console_write("kernel : microkernel, etap 1\n");
    hal_console_write("hal    : console/mem/thread(stub)\n");
    hal_console_write("fs     : RamFS (in-memory)\n");
    hal_console_write("arch   : x86 (multiboot)\n");
}

static void cmd_clear(void) {
    hal_console_clear();
}

static void cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            hal_console_putc(' ');
        hal_console_write(argv[i]);
    }
    hal_console_putc('\n');
}

static void put_hex8(uint8_t v) {
    const char *h = "0123456789abcdef";
    hal_console_putc(h[v >> 4]);
    hal_console_putc(h[v & 15]);
}

static void put_mac(const uint8_t *m) {
    for (int i = 0; i < 6; i++) {
        if (i) hal_console_putc(':');
        put_hex8(m[i]);
    }
}

static void cmd_ifconfig(void) {
    hal_console_write("mac : "); put_mac(net_cfg.mac); hal_console_putc('\n');
    hal_console_write("ip  : "); put_ip(net_cfg.ip);
    hal_console_write(net_cfg.from_dhcp ? "  (dhcp)\n" : "  (default)\n");
    hal_console_write("mask: "); put_ip(net_cfg.mask); hal_console_putc('\n');
    hal_console_write("gw  : "); put_ip(net_cfg.gw);   hal_console_putc('\n');
    hal_console_write("dns : "); put_ip(net_cfg.dns);  hal_console_putc('\n');
}

static void cmd_dhcp(void) {
    hal_console_write("dhcp: zapros adresa...\n");
    if (dhcp_acquire() != 0) {
        hal_console_write("dhcp: server ne otvetil\n");
        return;
    }
    hal_console_write("dhcp: poluchen ");
    put_ip(net_cfg.ip);
    hal_console_putc('\n');
}

/* "9999" -> 9999. 0 = успех. */
static int parse_port(const char *s, uint16_t *out) {
    unsigned v = 0;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned)(*s - '0');
        if (v > 65535) return -1;
        s++;
    }
    if (*s || v == 0) return -1;
    *out = (uint16_t)v;
    return 0;
}

/* udpsend <ip> <port> <text...> — одна датаграмма, ответ не ждём.
 * Проверка с хоста при запуске в QEMU (10.0.2.2 — это сам хост):
 *     nc -u -l 9999        и в системе:   udpsend 10.0.2.2 9999 privet */
static void cmd_udpsend(int argc, char *argv[]) {
    uint8_t ip[4];
    uint16_t port;
    if (argc < 4 || parse_ip(argv[1], ip) != 0 || parse_port(argv[2], &port) != 0) {
        hal_console_write("usage: udpsend <a.b.c.d> <port> <text>\n");
        return;
    }

    char text[128];
    size_t len = 0;
    for (int i = 3; i < argc; i++) {
        if (i > 3 && len < sizeof text - 1) text[len++] = ' ';
        for (const char *p = argv[i]; *p && len < sizeof text - 1; p++)
            text[len++] = *p;
    }
    text[len++] = '\n';

    int r = udp_send(ip, 40000, port, text, len);
    if (r == NET_OK)           hal_console_write("otpravleno\n");
    else if (r == NET_ERR_ARP) hal_console_write("udpsend: net MAC dlya next-hop (arp)\n");
    else                       hal_console_write("udpsend: oshibka otpravki\n");
}

static void print_dns_error(int r) {
    switch (r) {
    case DNS_ERR_NAME:     hal_console_write("dns: nepravilnoe imya\n"); break;
    case DNS_ERR_TIMEOUT:  hal_console_write("dns: server ne otvetil (est li adres? komanda dhcp)\n"); break;
    case DNS_ERR_NXDOMAIN: hal_console_write("dns: takogo imeni net\n"); break;
    case DNS_ERR_NOADDR:   hal_console_write("dns: net A-zapisi\n"); break;
    case DNS_ERR_NET:      hal_console_write("dns: oshibka otpravki\n"); break;
    default:               hal_console_write("dns: server vernul oshibku\n"); break;
    }
}

/* nslookup <имя> */
static void cmd_nslookup(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: nslookup <name>\n");
        return;
    }
    uint8_t ip[4];
    int r = dns_resolve(argv[1], ip);
    if (r != DNS_OK) { print_dns_error(r); return; }

    hal_console_write(argv[1]);
    hal_console_write(" -> ");
    put_ip(ip);
    hal_console_putc('\n');
}

/* Дописывает строку в буфер запроса без выхода за границы. */
static void req_add(char *buf, size_t *len, size_t cap, const char *s) {
    while (*s && *len < cap - 1) buf[(*len)++] = *s++;
    buf[*len] = '\0';
}

/* http <хост> [путь] — HTTP/1.0 GET по порту 80, вывод ответа на экран.
 * HTTPS не поддерживается: пробуй сайты, которые отвечают по http
 * (example.com, neverssl.com). */
static void cmd_http(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: http <host> [path]\n");
        return;
    }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";

    uint8_t ip[4];
    if (parse_ip(host, ip) != 0) {
        int r = dns_resolve(host, ip);
        if (r != DNS_OK) { print_dns_error(r); return; }
    }

    hal_console_write("connect "); put_ip(ip); hal_console_write(":80\n");
    int r = tcp_connect(ip, 80);
    if (r != TCP_OK) {
        hal_console_write(r == TCP_ERR_RESET   ? "http: port zakryt (RST)\n"
                        : r == TCP_ERR_TIMEOUT ? "http: taymaut soedineniya\n"
                                               : "http: oshibka soedineniya\n");
        return;
    }

    char req[256];
    size_t rl = 0;
    req[0] = '\0';
    req_add(req, &rl, sizeof req, "GET ");
    req_add(req, &rl, sizeof req, path);
    req_add(req, &rl, sizeof req, " HTTP/1.0\r\nHost: ");
    req_add(req, &rl, sizeof req, host);
    req_add(req, &rl, sizeof req, "\r\nUser-Agent: proshivkaos\r\nConnection: close\r\n\r\n");

    if (tcp_send(req, rl) < 0) {
        hal_console_write("http: oshibka otpravki\n");
        tcp_close();
        return;
    }

    enum { SHOW_MAX = 2000 };
    uint8_t buf[512];
    unsigned total = 0, shown = 0;
    for (;;) {
        int n = tcp_recv(buf, sizeof buf, NET_POLL_LIMIT);
        if (n < 0) { hal_console_write("\nhttp: taymaut/oshibka priema\n"); break; }
        if (n == 0) break;                     /* сервер закрыл соединение */

        for (int i = 0; i < n; i++) {
            if (shown >= SHOW_MAX) break;
            char c = (char)buf[i];
            if (c == '\r') continue;
            hal_console_putc((c == '\n' || (c >= 32 && c < 127)) ? c : '.');
            shown++;
        }
        total += (unsigned)n;
    }
    tcp_close();

    hal_console_write("\n--- polucheno ");
    put_dec(total);
    hal_console_write(" bayt");
    if (total > shown) hal_console_write(" (pokazano pervye 2000)");
    hal_console_putc('\n');
}

/* ARP-запрос шлюзу QEMU (10.0.2.2) с нашего адреса 10.0.2.15.
 * Если пришёл ответ — передача и приём работают. */
static void cmd_arping(void) {
    static const uint8_t ip_me[4] = {10, 0, 2, 15};
    static const uint8_t ip_gw[4] = {10, 0, 2, 2};
    uint8_t mac[6], f[60] = {0};
    hal_net_get_mac(mac);

    for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6 + i] = mac[i]; }
    f[12] = 0x08; f[13] = 0x06;          /* ethertype ARP */
    f[14] = 0x00; f[15] = 0x01;          /* Ethernet */
    f[16] = 0x08; f[17] = 0x00;          /* IPv4 */
    f[18] = 6;    f[19] = 4;
    f[20] = 0x00; f[21] = 0x01;          /* request */
    for (int i = 0; i < 6; i++) f[22 + i] = mac[i];
    for (int i = 0; i < 4; i++) { f[28 + i] = ip_me[i]; f[38 + i] = ip_gw[i]; }

    if (hal_net_send(f, 42) != 0) {
        hal_console_write("arping: oshibka otpravki\n");
        return;
    }

    uint8_t r[1536];
    for (long t = 0; t < 20000000; t++) {
        size_t n = hal_net_recv(r, sizeof r);
        if (n >= 42 && r[12] == 0x08 && r[13] == 0x06 && r[21] == 0x02) {
            hal_console_write("otvet ot 10.0.2.2, mac ");
            put_mac(r + 22);
            hal_console_putc('\n');
            return;
        }
    }
    hal_console_write("arping: net otveta\n");
}

static int parse_ip(const char *s, uint8_t out[4]) {
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            if (v > 255) return -1;
            s++;
        }
        out[i] = (uint8_t)v;
        if (i < 3) { if (*s != '.') return -1; s++; }
    }
    return *s == '\0' ? 0 : -1;
}

static void put_dec(unsigned v) {
    char b[11]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) hal_console_putc(b[--n]);
}

static void put_ip(const uint8_t *ip) {
    for (int i = 0; i < 4; i++) { if (i) hal_console_putc('.'); put_dec(ip[i]); }
}

static void cmd_ping(int argc, char *argv[]) {
    uint8_t ip[4];
    if (argc < 2 || parse_ip(argv[1], ip) != 0) {
        hal_console_write("usage: ping <a.b.c.d>\n");
        return;
    }

    int ok = 0;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        int r = net_ping(ip, seq);
        if (r == PING_OK) {
            ok++;
            hal_console_write("otvet ot "); put_ip(ip);
            hal_console_write(": seq="); put_dec(seq); hal_console_putc('\n');
        } else if (r == PING_NO_ARP) {
            hal_console_write("ping: net MAC dlya next-hop (arp)\n");
            return;
        } else if (r == PING_TX_ERR) {
            hal_console_write("ping: oshibka otpravki\n");
            return;
        } else {
            hal_console_write("seq="); put_dec(seq);
            hal_console_write(": net otveta\n");
        }
    }
    hal_console_write("--- "); put_ip(ip); hal_console_write(": ");
    put_dec((unsigned)ok); hal_console_write("/4 polucheno\n");
}

/* Полноэкранный редактор (apps/editor.c) — binды в стиле micro:
 * Ctrl+S sohranit, Ctrl+Q vyhod, Ctrl+F poisk. Файл создаётся при
 * отсутствии автоматически внутри editor_run() при первом сохранении. */
static void cmd_edit(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: edit <file>\n");
        return;
    }

    ramfs_touch(argv[1]);
    editor_run(argv[1]);
}

static void cmd_help(void) {
    hal_console_write(
        "Dostupnye komandy:\n"
        "  ls                 - spisok faylov\n"
        "  cat <file>         - pokazat soderzhimoe fayla\n"
        "  touch <file>       - sozdat pustoy fayl\n"
        "  clear              - ochistit konsol\n"
        "  echo               - vyvodit tekst\n"
        "  edit <file>        - polnoekrannyy redaktor\n"
        "                       (^S sohranit, ^Q vyhod, ^F poisk)\n"
        "  neofetch           - info o sisteme\n"
        "  help               - eta spravka\n"
        "  ifconfig           - MAC setevoy karty\n"
        "  arping             - proverka seti (ARP k 10.0.2.2)\n"
        "  ping <ip>          - ICMP echo\n"
        "  dhcp               - poluchit adres po DHCP\n"
        "  udpsend <ip> <p> <text> - otpravit UDP\n"
        "  nslookup <name>    - uznat IP po imeni (DNS)\n"
        "  http <host> [path] - HTTP GET po portu 80\n"
    );
}

static void print_prompt(void) {
    hal_console_set_color(HAL_COLOR_LIGHT_MAGENTA, HAL_COLOR_BLACK);
    hal_console_write("proshivkaos> ");
    hal_console_reset_color();
}

/* Имена команд для Tab. ДЕРЖАТЬ В СИНХРОНЕ с цепочкой else-if в shell_run():
 * добавил команду — добавь её имя сюда. */
static const char *const COMMANDS[] = {
    "ls", "cat", "touch", "edit", "neofetch", "help",
    "clear", "echo",
    "ifconfig", "arping", "ping", "dhcp", "udpsend", "nslookup", "http",
};

/* ---------------- Главный цикл ---------------- */

void shell_run(void) {
    char line[CMD_BUF_SIZE];
    char *argv[CMD_MAX_ARGS];

    cmd_help();

    if (hal_net_init() != 0)
            hal_console_write("net: karta ne naydena\n");
    if (hal_net_init() == 0) net_cfg_init();
    else hal_console_write("net: karta ne naydena\n");

    for (;;) {
        print_prompt();
                line_read(line, CMD_BUF_SIZE, print_prompt,
                         COMMANDS, sizeof COMMANDS / sizeof COMMANDS[0]);

        int argc = tokenize(line, argv, CMD_MAX_ARGS);
        if (argc == 0)
            continue;

        if (str_eq(argv[0], "ls"))            cmd_ls();
        else if (str_eq(argv[0], "cat"))      cmd_cat(argc, argv);
        else if (str_eq(argv[0], "touch"))    cmd_touch(argc, argv);
        else if (str_eq(argv[0], "edit"))     cmd_edit(argc, argv);
        else if (str_eq(argv[0], "neofetch")) cmd_neofetch();
        else if (str_eq(argv[0], "ifconfig")) cmd_ifconfig();
        else if (str_eq(argv[0], "arping"))   cmd_arping();
        else if (str_eq(argv[0], "help"))     cmd_help();
        else if (str_eq(argv[0], "clear"))    cmd_clear();
        else if (str_eq(argv[0], "echo"))    cmd_echo(argc, argv);
        else if (str_eq(argv[0], "ping")) cmd_ping(argc, argv);
        else if (str_eq(argv[0], "dhcp"))    cmd_dhcp();
        else if (str_eq(argv[0], "udpsend")) cmd_udpsend(argc, argv);
        else if (str_eq(argv[0], "nslookup")) cmd_nslookup(argc, argv);
        else if (str_eq(argv[0], "http"))     cmd_http(argc, argv);
        else {
            hal_console_write("neizvestnaya komanda: ");
            hal_console_write(argv[0]);
            hal_console_putc('\n');
        }
    }
}
