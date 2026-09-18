/* net/net.c — сетевой стек (см. net.h).
 *
 * Намеренно маленький: один интерфейс, IPv4, без фрагментации и без TCP.
 * Этого хватает, чтобы получить адрес по DHCP, найти имя через DNS и
 * пропинговать что угодно в интернете.
 *
 * ARP. Кэш на восемь адресов. Пакет к адресу, которого нет в кэше, не
 * теряется: он ложится в единственную «очередь ожидания» и уходит, как
 * только придёт ответ ARP. Первый ping поэтому не пропадает.
 *
 * DHCP. Обычный обмен DISCOVER -> OFFER -> REQUEST -> ACK с повторами
 * каждые две секунды; ответы просим широковещательно (флаг broadcast),
 * потому что своего адреса у нас ещё нет. */
#include "net.h"
#include "hal_time.h"

static uint8_t  g_mac[6];
static uint32_t g_ip, g_mask, g_gw, g_dns;
static int      g_link;
static uint16_t g_ip_id = 1;
static uint32_t g_rx, g_tx;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint32_t g16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t g32(const uint8_t *p) { return (g16(p) << 16) | g16(p + 2); }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p16(p, v >> 16); p16(p + 2, v); }
static void cp(uint8_t *d, const uint8_t *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[i] = s[i]; }
static void zero(uint8_t *d, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[i] = 0; }

static uint32_t sum16(const uint8_t *p, uint32_t n, uint32_t s) {
    for (uint32_t i = 0; i + 1 < n; i += 2) s += g16(p + i);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    return s;
}
static uint16_t fold(uint32_t s) {
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static uint32_t g_rand = 0x2545F491u;
static uint32_t rnd(void) {
    g_rand ^= (uint32_t)hal_time_us();
    g_rand ^= g_rand << 13; g_rand ^= g_rand >> 17; g_rand ^= g_rand << 5;
    return g_rand;
}

/* ---------------- ARP ---------------- */

#define ARP_N 8
static struct { uint32_t ip; uint8_t mac[6]; } g_arp[ARP_N];
static int g_arp_next;

static struct {
    int      used;
    uint32_t hop;
    uint64_t sent;
    int      tries;
    uint32_t len;
    uint8_t  buf[1500];
} g_pend;

static const uint8_t *arp_find(uint32_t ip) {
    for (int i = 0; i < ARP_N; i++) if (g_arp[i].ip == ip) return g_arp[i].mac;
    return 0;
}

static void arp_learn(uint32_t ip, const uint8_t *mac) {
    if (!ip) return;
    for (int i = 0; i < ARP_N; i++)
        if (g_arp[i].ip == ip) { cp(g_arp[i].mac, mac, 6); return; }
    g_arp[g_arp_next].ip = ip;
    cp(g_arp[g_arp_next].mac, mac, 6);
    g_arp_next = (g_arp_next + 1) % ARP_N;
}

static void arp_send(int op, const uint8_t *tmac, uint32_t tip, const uint8_t *dst) {
    uint8_t a[28];
    p16(a, 1); p16(a + 2, NET_ETH_IP); a[4] = 6; a[5] = 4; p16(a + 6, (uint32_t)op);
    cp(a + 8, g_mac, 6); p32(a + 14, g_ip);
    if (tmac) cp(a + 18, tmac, 6); else zero(a + 18, 6);
    p32(a + 24, tip);
    if (net_link_output(dst, NET_ETH_ARP, a, 28)) g_tx++;
}

static void flush_pending(void) {
    if (!g_pend.used) return;
    const uint8_t *mac = arp_find(g_pend.hop);
    if (!mac) return;
    if (net_link_output(mac, NET_ETH_IP, g_pend.buf, g_pend.len)) g_tx++;
    g_pend.used = 0;
}

static void arp_input(const uint8_t *a, uint32_t len) {
    if (len < 28 || g16(a) != 1 || g16(a + 2) != NET_ETH_IP) return;
    uint32_t op = g16(a + 6), sip = g32(a + 14), tip = g32(a + 24);
    arp_learn(sip, a + 8);
    if (op == 1 && g_ip && tip == g_ip) arp_send(2, a + 8, sip, a + 8);
    flush_pending();
}

/* ---------------- IPv4 ---------------- */

static uint32_t next_hop(uint32_t dst) {
    if (dst == 0xFFFFFFFFu) return dst;
    if (g_mask && (dst & g_mask) == (g_ip & g_mask)) return dst;
    return g_gw ? g_gw : dst;
}

static int ip_output(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *pl, uint32_t n) {
    static uint8_t pkt[1500];
    if (n + 20 > sizeof(pkt)) return 0;
    pkt[0] = 0x45; pkt[1] = 0; p16(pkt + 2, 20 + n); p16(pkt + 4, g_ip_id++);
    p16(pkt + 6, 0x4000); pkt[8] = 64; pkt[9] = proto; p16(pkt + 10, 0);
    p32(pkt + 12, src); p32(pkt + 16, dst);
    p16(pkt + 10, fold(sum16(pkt, 20, 0)));
    cp(pkt + 20, pl, n);

    uint32_t hop = next_hop(dst);
    if (hop == 0xFFFFFFFFu) {
        if (net_link_output(BCAST, NET_ETH_IP, pkt, 20 + n)) { g_tx++; return 1; }
        return 0;
    }
    const uint8_t *mac = arp_find(hop);
    if (mac) {
        if (net_link_output(mac, NET_ETH_IP, pkt, 20 + n)) { g_tx++; return 1; }
        return 0;
    }
    g_pend.used = 1;
    g_pend.hop = hop;
    g_pend.len = 20 + n;
    g_pend.sent = hal_time_ms();
    g_pend.tries = 1;
    cp(g_pend.buf, pkt, 20 + n);
    arp_send(1, 0, hop, BCAST);
    return 1;
}

/* ---------------- ICMP ---------------- */

#define PING_ID 0x5053            /* "PS" */
static struct { uint16_t seq; uint64_t sent_us; uint32_t rtt_ms; uint8_t ttl; int done; } g_ping[8];

int net_ping_send(uint32_t ip, uint16_t seq) {
    if (!g_ip) return 0;
    uint8_t m[8 + 32];
    m[0] = 8; m[1] = 0; p16(m + 2, 0); p16(m + 4, PING_ID); p16(m + 6, seq);
    for (int i = 0; i < 32; i++) m[8 + i] = (uint8_t)('a' + i % 23);
    p16(m + 2, fold(sum16(m, sizeof(m), 0)));
    int k = seq % 8;
    g_ping[k].seq = seq; g_ping[k].done = 0; g_ping[k].sent_us = hal_time_us();
    return ip_output(g_ip, ip, 1, m, sizeof(m));
}

int net_ping_done(uint16_t seq, uint32_t *rtt_ms, uint8_t *ttl) {
    int k = seq % 8;
    if (g_ping[k].seq != seq || !g_ping[k].done) return 0;
    if (rtt_ms) *rtt_ms = g_ping[k].rtt_ms;
    if (ttl) *ttl = g_ping[k].ttl;
    return 1;
}

static void icmp_input(uint32_t src, uint8_t ttl, const uint8_t *m, uint32_t n) {
    if (n < 8) return;
    if (m[0] == 8) {                                  /* нас пингуют — отвечаем */
        static uint8_t r[1480];
        if (n > sizeof(r)) return;
        cp(r, m, n);
        r[0] = 0; p16(r + 2, 0);
        p16(r + 2, fold(sum16(r, n, 0)));
        ip_output(g_ip, src, 1, r, n);
    } else if (m[0] == 0 && g16(m + 4) == PING_ID) {
        uint16_t seq = (uint16_t)g16(m + 6);
        int k = seq % 8;
        if (g_ping[k].seq == seq && !g_ping[k].done) {
            g_ping[k].rtt_ms = (uint32_t)((hal_time_us() - g_ping[k].sent_us + 500) / 1000);
            g_ping[k].ttl = ttl;
            g_ping[k].done = 1;
        }
    }
}

/* ---------------- UDP ---------------- */

static int udp_output(uint32_t src, uint32_t dst, uint16_t sp, uint16_t dp, const uint8_t *d, uint32_t n) {
    static uint8_t u[1480];
    if (n + 8 > sizeof(u)) return 0;
    p16(u, sp); p16(u + 2, dp); p16(u + 4, 8 + n); p16(u + 6, 0);
    cp(u + 8, d, n);
    return ip_output(src, dst, 17, u, 8 + n);      /* контрольная сумма 0 — разрешено в IPv4 */
}

/* ---------------- DHCP ---------------- */

static int g_dhcp = NET_DHCP_OFF;
static uint32_t g_xid, g_offer_ip, g_server;
static uint64_t g_dhcp_t;
static int g_dhcp_tries;

static uint32_t dhcp_build(uint8_t *b, int type) {
    zero(b, 300);
    b[0] = 1; b[1] = 1; b[2] = 6; p32(b + 4, g_xid); p16(b + 10, 0x8000);
    cp(b + 28, g_mac, 6);
    p32(b + 236, 0x63825363u);
    uint32_t o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = (uint8_t)type;
    b[o++] = 61; b[o++] = 7; b[o++] = 1; cp(b + o, g_mac, 6); o += 6;
    static const char host[] = "proshivkaos";
    b[o++] = 12; b[o++] = sizeof(host) - 1; cp(b + o, (const uint8_t *)host, sizeof(host) - 1); o += sizeof(host) - 1;
    if (type == 3) {
        b[o++] = 50; b[o++] = 4; p32(b + o, g_offer_ip); o += 4;
        b[o++] = 54; b[o++] = 4; p32(b + o, g_server); o += 4;
    }
    b[o++] = 55; b[o++] = 4; b[o++] = 1; b[o++] = 3; b[o++] = 6; b[o++] = 51;
    b[o++] = 255;
    return o < 300 ? 300 : o;
}

static void dhcp_send(int type) {
    static uint8_t b[320];
    uint32_t n = dhcp_build(b, type);
    udp_output(0, 0xFFFFFFFFu, 68, 67, b, n);
    g_dhcp_t = hal_time_ms();
    net_log(type == 1 ? "NET: DHCP DISCOVER\n" : "NET: DHCP REQUEST\n");
}

static void dhcp_input(const uint8_t *b, uint32_t n) {
    if (n < 240 || b[0] != 2 || g32(b + 4) != g_xid || g32(b + 236) != 0x63825363u) return;
    int type = 0;
    uint32_t mask = 0, gw = 0, dns = 0, server = 0;
    for (uint32_t o = 240; o + 1 < n && b[o] != 255;) {
        uint8_t code = b[o];
        if (code == 0) { o++; continue; }
        uint8_t l = b[o + 1];
        const uint8_t *v = b + o + 2;
        if (o + 2 + l > n) break;
        if (code == 53 && l >= 1) type = v[0];
        else if (code == 1 && l >= 4) mask = g32(v);
        else if (code == 3 && l >= 4) gw = g32(v);
        else if (code == 6 && l >= 4) dns = g32(v);
        else if (code == 54 && l >= 4) server = g32(v);
        o += 2u + l;
    }
    if (type == 2 && g_dhcp == NET_DHCP_DISCOVER) {           /* OFFER */
        g_offer_ip = g32(b + 16);
        g_server = server;
        g_dhcp = NET_DHCP_REQUEST;
        g_dhcp_tries = 0;
        dhcp_send(3);
    } else if (type == 5 && g_dhcp == NET_DHCP_REQUEST) {     /* ACK */
        g_ip = g32(b + 16);
        g_mask = mask ? mask : 0xFFFFFF00u;
        g_gw = gw;
        g_dns = dns ? dns : gw;
        g_dhcp = NET_DHCP_BOUND;
        char a[16], m[16], g[16];
        net_format_ip(g_ip, a); net_format_ip(g_mask, m); net_format_ip(g_gw, g);
        net_log("NET: adres "); net_log(a); net_log(" maska "); net_log(m);
        net_log(" shlyuz "); net_log(g); net_log("\n");
        /* Объявить себя (gratuitous ARP) и сразу узнать шлюз. */
        arp_send(1, 0, g_ip, BCAST);
        if (g_gw) arp_send(1, 0, g_gw, BCAST);
    } else if (type == 6) {                                   /* NAK */
        g_dhcp = NET_DHCP_DISCOVER;
        g_xid = rnd();
        dhcp_send(1);
    }
}

/* ---------------- DNS ---------------- */

#define DNS_PORT_LOCAL 40053
static uint16_t g_dns_id;
static int g_dns_state;              /* 0 — нет, 1 — ждём, 2 — есть, -1 — нет имени */
static uint32_t g_dns_ip;

int net_dns_query(const char *name) {
    if (!g_ip || !g_dns) return 0;
    uint8_t q[300];
    g_dns_id = (uint16_t)rnd();
    p16(q, g_dns_id); p16(q + 2, 0x0100); p16(q + 4, 1); p16(q + 6, 0); p16(q + 8, 0); p16(q + 10, 0);
    uint32_t o = 12;
    const char *s = name;
    while (*s) {
        uint32_t lp = o++;
        uint8_t l = 0;
        while (*s && *s != '.' && o < 280) { q[o++] = (uint8_t)*s++; l++; }
        q[lp] = l;
        if (*s == '.') s++;
    }
    q[o++] = 0;
    p16(q + o, 1); p16(q + o + 2, 1); o += 4;
    g_dns_state = 1;
    return udp_output(g_ip, g_dns, DNS_PORT_LOCAL, 53, q, o);
}

int net_dns_result(uint32_t *ip) {
    if (g_dns_state == 2) { *ip = g_dns_ip; return 1; }
    return g_dns_state == -1 ? -1 : 0;
}

static uint32_t dns_skip_name(const uint8_t *m, uint32_t n, uint32_t o) {
    while (o < n) {
        uint8_t l = m[o];
        if (l == 0) return o + 1;
        if ((l & 0xC0) == 0xC0) return o + 2;
        o += 1u + l;
    }
    return n;
}

static void dns_input(const uint8_t *m, uint32_t n) {
    if (n < 12 || g16(m) != g_dns_id || g_dns_state != 1) return;
    if ((m[3] & 0x0F) != 0) { g_dns_state = -1; return; }
    uint32_t qd = g16(m + 4), an = g16(m + 6), o = 12;
    for (uint32_t i = 0; i < qd; i++) o = dns_skip_name(m, n, o) + 4;
    for (uint32_t i = 0; i < an && o < n; i++) {
        o = dns_skip_name(m, n, o);
        if (o + 10 > n) break;
        uint32_t type = g16(m + o), cls = g16(m + o + 2), rl = g16(m + o + 8);
        o += 10;
        if (type == 1 && cls == 1 && rl == 4 && o + 4 <= n) {
            g_dns_ip = g32(m + o);
            g_dns_state = 2;
            return;
        }
        o += rl;
    }
    g_dns_state = -1;
}

/* ---------------- вход и таймеры ---------------- */

void net_init(const uint8_t mac[6]) {
    cp(g_mac, mac, 6);
    g_rand ^= ((uint32_t)mac[4] << 8) | mac[5];
}

void net_link_up(void) {
    g_link = 1;
    g_ip = 0; g_mask = 0; g_gw = 0; g_dns = 0;
    g_xid = rnd();
    g_dhcp = NET_DHCP_DISCOVER;
    g_dhcp_tries = 0;
    dhcp_send(1);
}

void net_link_down(void) {
    g_link = 0;
    g_dhcp = NET_DHCP_OFF;
    g_ip = 0;
    g_pend.used = 0;
}

void net_input(const uint8_t src[6], const uint8_t dst[6], uint16_t type, const uint8_t *p, uint32_t len) {
    (void)dst;
    g_rx++;
    if (type == NET_ETH_ARP) { arp_input(p, len); return; }
    if (type != NET_ETH_IP || len < 20 || (p[0] >> 4) != 4) return;
    uint32_t ihl = (uint32_t)(p[0] & 15) * 4, tot = g16(p + 2);
    if (ihl < 20 || tot > len || tot < ihl) return;
    if (g16(p + 6) & 0x3FFF) return;                     /* фрагменты не собираем */
    uint32_t sip = g32(p + 12), dip = g32(p + 16);
    if (g_ip && dip != g_ip && dip != 0xFFFFFFFFu && (dip | g_mask) != 0xFFFFFFFFu) return;
    arp_learn(sip, src);
    const uint8_t *pl = p + ihl;
    uint32_t n = tot - ihl;
    if (p[9] == 1) { icmp_input(sip, p[8], pl, n); return; }
    if (p[9] == 17 && n >= 8) {
        uint32_t dp = g16(pl + 2), ul = g16(pl + 4);
        if (ul < 8 || ul > n) return;
        if (dp == 68) dhcp_input(pl + 8, ul - 8);
        else if (dp == DNS_PORT_LOCAL) dns_input(pl + 8, ul - 8);
    }
}

void net_poll(void) {
    if (!g_link) return;
    uint64_t now = hal_time_ms();
    if ((g_dhcp == NET_DHCP_DISCOVER || g_dhcp == NET_DHCP_REQUEST) && now - g_dhcp_t > 2000) {
        if (++g_dhcp_tries > 8) {
            g_dhcp = NET_DHCP_FAILED;
            net_log("NET: DHCP ne otvetil\n");
        } else {
            dhcp_send(g_dhcp == NET_DHCP_DISCOVER ? 1 : 3);
        }
    }
    if (g_pend.used && now - g_pend.sent > 1000) {
        if (g_pend.tries >= 3) g_pend.used = 0;
        else { g_pend.tries++; g_pend.sent = now; arp_send(1, 0, g_pend.hop, BCAST); }
    }
}

void net_status(net_status_t *st) {
    st->ip = g_ip; st->mask = g_mask; st->gw = g_gw; st->dns = g_dns;
    st->dhcp = g_dhcp; st->rx_packets = g_rx; st->tx_packets = g_tx;
}

int net_parse_ip(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        uint32_t x = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (uint32_t)(*s++ - '0'); digits++; }
        if (!digits || x > 255) return 0;
        v = (v << 8) | x;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *ip = v;
    return 1;
}

void net_format_ip(uint32_t ip, char *b) {
    int o = 0;
    for (int part = 3; part >= 0; part--) {
        uint32_t x = (ip >> (8 * part)) & 255;
        if (x >= 100) b[o++] = (char)('0' + x / 100);
        if (x >= 10) b[o++] = (char)('0' + x / 10 % 10);
        b[o++] = (char)('0' + x % 10);
        if (part) b[o++] = '.';
    }
    b[o] = 0;
}
