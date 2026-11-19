/* net/netcmd.c — реализация сетевых команд поверх netcore/udp/dhcp/dns/tcp.
 * Вывод — только через net_out_t, поэтому один и тот же код работает и в
 * текстовой консоли, и в окне терминала. Все ожидания внутри можно
 * прервать (см. net_set_pump в netcore.h): тогда команда печатает
 * «interrupted» и возвращается. */
#include "netcore.h"
#include "udp.h"
#include "dhcp.h"
#include "dns.h"
#include "tcp.h"
#include "ping.h"
#include "netcmd.h"

/* ---------------- строка-сборщик ---------------- */

typedef struct { char b[200]; size_t n; } sb_t;

static void sb_init(sb_t *s) { s->n = 0; s->b[0] = '\0'; }
static void sb_str(sb_t *s, const char *t) {
    while (*t && s->n < sizeof s->b - 1) s->b[s->n++] = *t++;
    s->b[s->n] = '\0';
}
static void sb_dec(sb_t *s, unsigned v) {
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && s->n < sizeof s->b - 1) s->b[s->n++] = t[--n];
    s->b[s->n] = '\0';
}
static void sb_ip(sb_t *s, const uint8_t *ip) {
    for (int i = 0; i < 4; i++) { if (i) sb_str(s, "."); sb_dec(s, ip[i]); }
}
static void sb_mac(sb_t *s, const uint8_t *m) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char t[3] = { hx[m[i] >> 4], hx[m[i] & 15], 0 };
        if (i) sb_str(s, ":");
        sb_str(s, t);
    }
}

/* ---------------- вывод ---------------- */

static void emit(const net_out_t *o, const char *s) {
    while (*s) o->putc(o->ctx, *s++);
}

static void line(const net_out_t *o, int style, const char *s) {
    if (o->lead_nl) o->putc(o->ctx, '\n');
    if (style && o->style) o->style(o->ctx, style);
    emit(o, s);
    if (style && o->style) o->style(o->ctx, NET_STYLE_NORMAL);
    if (!o->lead_nl) o->putc(o->ctx, '\n');
    if (o->flush) o->flush(o->ctx);
}

static void info(const net_out_t *o, const char *s)  { line(o, NET_STYLE_NORMAL, s); }
static void fail(const net_out_t *o, const char *s)  { line(o, NET_STYLE_ERROR, s); }
static void say(const net_out_t *o, const sb_t *s)   { line(o, NET_STYLE_NORMAL, s->b); }

/* ---------------- разбор аргументов ---------------- */

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
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

/* Печатает «interrupted», если пользователь прервал операцию. */
static int was_interrupted(const net_out_t *o) {
    if (!net_aborted()) return 0;
    fail(o, "^C interrupted");
    return 1;
}

static void dns_error(const net_out_t *o, int r) {
    switch (r) {
    case DNS_ERR_NAME:     fail(o, "dns: bad name"); break;
    case DNS_ERR_TIMEOUT:  fail(o, "dns: no answer (run dhcp?)"); break;
    case DNS_ERR_NXDOMAIN: fail(o, "dns: no such name"); break;
    case DNS_ERR_NOADDR:   fail(o, "dns: no A record"); break;
    case DNS_ERR_NET:      fail(o, "dns: send error"); break;
    default:               fail(o, "dns: server error"); break;
    }
}

/* Имя или IP-литерал -> адрес. 0 — успех (иначе сообщение уже выведено). */
static int resolve_host(const net_out_t *o, const char *host, uint8_t ip[4]) {
    if (parse_ip(host, ip) == 0) return 0;
    int r = dns_resolve(host, ip);
    if (r == DNS_OK) return 0;
    if (!was_interrupted(o)) dns_error(o, r);
    return -1;
}

/* ---------------- команды ---------------- */

static void cmd_ifconfig(const net_out_t *o) {
    sb_t s;
    sb_init(&s); sb_str(&s, "mac : "); sb_mac(&s, net_cfg.mac); say(o, &s);
    sb_init(&s); sb_str(&s, "ip  : "); sb_ip(&s, net_cfg.ip);
    sb_str(&s, net_cfg.from_dhcp ? " (dhcp)" : " (default)"); say(o, &s);
    sb_init(&s); sb_str(&s, "mask: "); sb_ip(&s, net_cfg.mask); say(o, &s);
    sb_init(&s); sb_str(&s, "gw  : "); sb_ip(&s, net_cfg.gw);   say(o, &s);
    sb_init(&s); sb_str(&s, "dns : "); sb_ip(&s, net_cfg.dns);  say(o, &s);
}

static void cmd_arping(const net_out_t *o, int argc, char *argv[]) {
    uint8_t ip[4], mac[6];
    if (argc >= 2) {
        if (parse_ip(argv[1], ip) != 0) { fail(o, "usage: arping [a.b.c.d]"); return; }
    } else {
        net_copy(ip, net_cfg.gw, 4);
    }

    net_arp_flush();
    int r = net_arp_resolve(ip, mac);
    if (r == NET_OK) {
        sb_t s; sb_init(&s);
        sb_str(&s, "reply from "); sb_ip(&s, ip); sb_str(&s, ": "); sb_mac(&s, mac);
        say(o, &s);
    } else if (!was_interrupted(o)) {
        fail(o, r == NET_ERR_TX ? "arping: send error" : "arping: no reply");
    }
}

static void cmd_ping(const net_out_t *o, int argc, char *argv[]) {
    if (argc < 2) { fail(o, "usage: ping <ip|name>"); return; }

    uint8_t ip[4];
    if (resolve_host(o, argv[1], ip) != 0) return;

    int ok = 0;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        int r = net_ping(ip, seq);
        sb_t s; sb_init(&s);

        if (r == PING_OK) {
            ok++;
            sb_str(&s, "reply from "); sb_ip(&s, ip);
            sb_str(&s, ": seq="); sb_dec(&s, seq);
            say(o, &s);
        } else if (was_interrupted(o)) {
            return;
        } else if (r == PING_NO_ARP) {
            fail(o, "ping: no route (ARP failed)");
            return;
        } else if (r == PING_TX_ERR) {
            fail(o, "ping: send error");
            return;
        } else {
            sb_str(&s, "timeout: seq="); sb_dec(&s, seq);
            line(o, NET_STYLE_ERROR, s.b);
        }
    }

    sb_t s; sb_init(&s);
    sb_str(&s, "--- "); sb_ip(&s, ip); sb_str(&s, ": ");
    sb_dec(&s, (unsigned)ok); sb_str(&s, "/4 received");
    say(o, &s);
}

static void cmd_dhcp(const net_out_t *o) {
    info(o, "dhcp: requesting address...");
    if (dhcp_acquire() != 0) {
        if (!was_interrupted(o)) fail(o, "dhcp: no server answered");
        return;
    }
    sb_t s; sb_init(&s);
    sb_str(&s, "dhcp: got "); sb_ip(&s, net_cfg.ip);
    sb_str(&s, ", gw "); sb_ip(&s, net_cfg.gw);
    sb_str(&s, ", dns "); sb_ip(&s, net_cfg.dns);
    say(o, &s);
}

static void cmd_udpsend(const net_out_t *o, int argc, char *argv[]) {
    uint8_t ip[4];
    uint16_t port;
    if (argc < 4 || parse_ip(argv[1], ip) != 0 || parse_port(argv[2], &port) != 0) {
        fail(o, "usage: udpsend <ip> <port> <text>");
        return;
    }

    char text[128];
    size_t len = 0;
    for (int i = 3; i < argc; i++) {
        if (i > 3 && len < sizeof text - 2) text[len++] = ' ';
        for (const char *p = argv[i]; *p && len < sizeof text - 2; p++) text[len++] = *p;
    }
    text[len++] = '\n';

    int r = udp_send(ip, 40000, port, text, len);
    if (r == NET_OK)           info(o, "sent");
    else if (was_interrupted(o)) { }
    else if (r == NET_ERR_ARP) fail(o, "udpsend: no route (ARP failed)");
    else                       fail(o, "udpsend: send error");
}

static void cmd_nslookup(const net_out_t *o, int argc, char *argv[]) {
    if (argc < 2) { fail(o, "usage: nslookup <name>"); return; }

    uint8_t ip[4];
    int r = dns_resolve(argv[1], ip);
    if (r != DNS_OK) {
        if (!was_interrupted(o)) dns_error(o, r);
        return;
    }
    sb_t s; sb_init(&s);
    sb_str(&s, argv[1]); sb_str(&s, " -> "); sb_ip(&s, ip);
    say(o, &s);
}

static void req_add(char *buf, size_t *len, size_t cap, const char *s) {
    while (*s && *len < cap - 1) buf[(*len)++] = *s++;
    buf[*len] = '\0';
}

#define HTTP_SHOW_MAX 2000

static void cmd_http(const net_out_t *o, int argc, char *argv[]) {
    if (argc < 2) { fail(o, "usage: http <host> [path]"); return; }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";

    uint8_t ip[4];
    if (resolve_host(o, host, ip) != 0) return;

    sb_t s; sb_init(&s);
    sb_str(&s, "connecting to "); sb_ip(&s, ip); sb_str(&s, ":80");
    say(o, &s);

    int r = tcp_connect(ip, 80);
    if (r != TCP_OK) {
        if (!was_interrupted(o))
            fail(o, r == TCP_ERR_RESET   ? "http: port closed (RST)"
                  : r == TCP_ERR_TIMEOUT ? "http: connect timeout"
                                         : "http: connect error");
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
        if (!was_interrupted(o)) fail(o, "http: send error");
        tcp_close();
        return;
    }

    /* Тело ответа идёт «сплошным потоком» — перевод строки перед ним один. */
    if (o->lead_nl) o->putc(o->ctx, '\n');

    uint8_t buf[512];
    unsigned total = 0, shown = 0;
    int aborted = 0, err = 0;
    for (;;) {
        int n = tcp_recv(buf, sizeof buf, NET_POLL_LIMIT);
        if (n < 0) { if (net_aborted()) aborted = 1; else err = 1; break; }
        if (n == 0) break;                         /* сервер закрыл соединение */

        for (int i = 0; i < n && shown < HTTP_SHOW_MAX; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            o->putc(o->ctx, (c == '\n' || (c >= 32 && c < 127)) ? c : '.');
            shown++;
        }
        total += (unsigned)n;
        if (o->flush) o->flush(o->ctx);
    }
    tcp_close();

    if (!o->lead_nl) o->putc(o->ctx, '\n');
    if (aborted) { was_interrupted(o); return; }
    if (err) fail(o, "http: receive timeout/error");

    sb_init(&s);
    sb_str(&s, "--- received "); sb_dec(&s, total); sb_str(&s, " bytes");
    if (total > shown) sb_str(&s, " (first 2000 shown)");
    say(o, &s);
}

/* ---------------- таблица и вход ---------------- */

static const char *const NAMES[] = {
    "ifconfig", "arping", "ping", "dhcp", "udpsend", "nslookup", "http"
};

size_t netcmd_count(void) { return sizeof NAMES / sizeof NAMES[0]; }
const char *netcmd_name(size_t i) { return i < netcmd_count() ? NAMES[i] : 0; }

int netcmd_is(const char *name) {
    for (size_t i = 0; i < netcmd_count(); i++)
        if (str_eq(name, NAMES[i])) return 1;
    return 0;
}

int netcmd_exec(const net_out_t *o, int argc, char *argv[]) {
    if (argc < 1 || !netcmd_is(argv[0])) return 0;

    net_abort_clear();
    const char *c = argv[0];
    if      (str_eq(c, "ifconfig")) cmd_ifconfig(o);
    else if (str_eq(c, "arping"))   cmd_arping(o, argc, argv);
    else if (str_eq(c, "ping"))     cmd_ping(o, argc, argv);
    else if (str_eq(c, "dhcp"))     cmd_dhcp(o);
    else if (str_eq(c, "udpsend"))  cmd_udpsend(o, argc, argv);
    else if (str_eq(c, "nslookup")) cmd_nslookup(o, argc, argv);
    else if (str_eq(c, "http"))     cmd_http(o, argc, argv);
    return 1;
}
