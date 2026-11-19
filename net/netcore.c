/* net/netcore.c — ARP, IPv4 и контрольные суммы поверх hal_net_*.
 * Всё собирается в байтовых массивах (без struct): не зависит от
 * выравнивания и порядка байт. Буферы статические — стека не едим. */
#include "hal.h"
#include "netcore.h"

net_cfg_t net_cfg = {
    {0, 0, 0, 0, 0, 0},
    {10, 0, 2, 15},
    {255, 255, 255, 0},
    {10, 0, 2, 2},
    {10, 0, 2, 3},
    0
};

static const uint8_t BCAST_IP[4]  = {255, 255, 255, 255};
static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t rxf[1536];
static uint8_t txf[1514];

/* Кэш ARP на одну запись. */
static uint8_t cache_ip[4];
static uint8_t cache_mac[6];
static int     cache_ok;

void net_cfg_init(void) {
    hal_net_get_mac(net_cfg.mac);
}

/* ---------------- насос событий ---------------- */

static net_pump_fn pump_fn;
static unsigned    pump_ctr;
static int         abort_flag;

void net_set_pump(net_pump_fn fn) { pump_fn = fn; }
int  net_aborted(void)            { return abort_flag; }
void net_abort_clear(void)        { abort_flag = 0; }

/* Опрос карты — это порт ввода-вывода, на эмуляторе он не мгновенный;
 * раз в 2048 вызовов (порядка сотых долей секунды) отдаём управление
 * интерфейсу. */
void net_pump(void) {
    if (!pump_fn) return;
    if (++pump_ctr & 0x7FFu) return;
    if (pump_fn()) abort_flag = 1;
}

/* ---------------- контрольные суммы ---------------- */

uint32_t net_csum_add(uint32_t sum, const uint8_t *d, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += net_get16(d + i);
    if (n & 1)
        sum += (uint32_t)d[n - 1] << 8;
    return sum;
}

uint16_t net_csum_fin(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t net_csum(const uint8_t *d, size_t n) {
    return net_csum_fin(net_csum_add(0, d, n));
}

/* ---------------- ARP ---------------- */

void net_arp_flush(void) { cache_ok = 0; }

static void arp_fill(uint8_t *f, const uint8_t *dst_mac, uint16_t op,
                     const uint8_t *tha, const uint8_t *tpa) {
    for (int i = 0; i < 6; i++) { f[i] = dst_mac[i]; f[6 + i] = net_cfg.mac[i]; }
    net_put16(f + 12, 0x0806);
    net_put16(f + 14, 0x0001);           /* Ethernet */
    net_put16(f + 16, 0x0800);           /* IPv4 */
    f[18] = 6; f[19] = 4;
    net_put16(f + 20, op);
    for (int i = 0; i < 6; i++) { f[22 + i] = net_cfg.mac[i]; f[32 + i] = tha[i]; }
    for (int i = 0; i < 4; i++) { f[28 + i] = net_cfg.ip[i];  f[38 + i] = tpa[i]; }
}

/* Нас спрашивают «кто 10.0.2.15?» — отвечаем. */
static void arp_answer(const uint8_t *r, size_t n) {
    if (n < 42 || net_get16(r + 20) != 1) return;
    if (net_ip_zero(net_cfg.ip) || !net_ip_eq(r + 38, net_cfg.ip)) return;

    uint8_t f[60] = {0};
    arp_fill(f, r + 22, 2, r + 22, r + 28);
    hal_net_send(f, 42);
}

int net_arp_resolve(const uint8_t ip[4], uint8_t mac_out[6]) {
    if (cache_ok && net_ip_eq(cache_ip, ip)) {
        net_copy(mac_out, cache_mac, 6);
        return NET_OK;
    }

    static const uint8_t zero[6] = {0};
    uint8_t f[60] = {0};
    arp_fill(f, BCAST_MAC, 1, zero, ip);
    if (hal_net_send(f, 42) != 0) return NET_ERR_TX;

    for (long t = 0; NET_WAITING(t, NET_POLL_LIMIT); t++) {
        net_pump();
        size_t n = hal_net_recv(rxf, sizeof rxf);
        if (!n) continue;
        if (n >= 42 && net_get16(rxf + 12) == 0x0806) {
            if (net_get16(rxf + 20) == 2 && net_ip_eq(rxf + 28, ip)) {
                net_copy(cache_mac, rxf + 22, 6);
                net_copy(cache_ip, ip, 4);
                cache_ok = 1;
                net_copy(mac_out, cache_mac, 6);
                return NET_OK;
            }
            arp_answer(rxf, n);
        }
    }
    return NET_ERR_ARP;
}

/* ---------------- IPv4 ---------------- */

int net_ip_send_raw(const uint8_t dst_mac[6], const uint8_t src_ip[4],
                    const uint8_t dst_ip[4], uint8_t proto,
                    const uint8_t *payload, size_t len) {
    static uint16_t ip_id;
    if (len > 1480) return NET_ERR_TX;

    uint8_t *ip = txf + 14;
    net_copy(txf, dst_mac, 6);
    net_copy(txf + 6, net_cfg.mac, 6);
    net_put16(txf + 12, 0x0800);

    ip[0] = 0x45;                        /* IPv4, заголовок 20 байт */
    ip[1] = 0;
    net_put16(ip + 2, (uint16_t)(20 + len));
    net_put16(ip + 4, ++ip_id);
    net_put16(ip + 6, 0);                /* без фрагментации */
    ip[8] = 64;                          /* TTL */
    ip[9] = proto;
    net_put16(ip + 10, 0);
    net_copy(ip + 12, src_ip, 4);
    net_copy(ip + 16, dst_ip, 4);
    net_put16(ip + 10, net_csum(ip, 20));
    net_copy(ip + 20, payload, len);

    return hal_net_send(txf, 14 + 20 + len) == 0 ? NET_OK : NET_ERR_TX;
}

static int on_link(const uint8_t dst[4]) {
    for (int i = 0; i < 4; i++)
        if ((dst[i] & net_cfg.mask[i]) != (net_cfg.ip[i] & net_cfg.mask[i]))
            return 0;
    return 1;
}

int net_ip_send(const uint8_t dst[4], uint8_t proto,
                const uint8_t *payload, size_t len) {
    uint8_t mac[6];

    if (net_ip_eq(dst, BCAST_IP)) {
        net_copy(mac, BCAST_MAC, 6);
    } else {
        /* Вне нашей подсети кадр идёт через шлюз. */
        int r = net_arp_resolve(on_link(dst) ? dst : net_cfg.gw, mac);
        if (r != NET_OK) return r;
    }
    return net_ip_send_raw(mac, net_cfg.ip, dst, proto, payload, len);
}

size_t net_ip_recv(uint8_t *proto, uint8_t src[4], uint8_t dst[4],
                   uint8_t *buf, size_t max) {
    net_pump();
    size_t n = hal_net_recv(rxf, sizeof rxf);
    if (!n || n < 14) return 0;

    uint16_t type = net_get16(rxf + 12);
    if (type == 0x0806) { arp_answer(rxf, n); return 0; }
    if (type != 0x0800 || n < 14 + 20) return 0;

    const uint8_t *ip = rxf + 14;
    size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
    size_t tot = net_get16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || tot < ihl || 14 + tot > n) return 0;
    if (net_get16(ip + 6) & 0x3FFF) return 0;      /* фрагменты не собираем */

    /* Нам, широковещательно, или адреса ещё нет (DHCP). */
    if (!net_ip_zero(net_cfg.ip) &&
        !net_ip_eq(ip + 16, net_cfg.ip) && !net_ip_eq(ip + 16, BCAST_IP))
        return 0;

    size_t plen = tot - ihl;
    if (plen > max) plen = max;
    net_copy(buf, ip + ihl, plen);
    *proto = ip[9];
    net_copy(src, ip + 12, 4);
    net_copy(dst, ip + 16, 4);
    return plen;
}
