/* net/dns.c — минимальный резолвер: один вопрос (тип A), один ответ.
 * Цепочки CNAME проходятся по ответу (сервер обычно кладёт A следом). */
#include "netcore.h"
#include "udp.h"
#include "dns.h"

#define DNS_PORT_LOCAL   40053
#define DNS_PORT_SERVER  53
#define PARSE_NOT_OURS  -100        /* внутренний код: пакет не тот, ждём дальше */

static uint16_t id_counter = 0x4400;

/* Пропускает имя в ответе (метки или указатель сжатия). Возвращает смещение
 * за именем или 0 при ошибке (0 не бывает правильным: заголовок — 12 байт). */
static size_t skip_name(const uint8_t *r, size_t n, size_t p) {
    while (p < n) {
        uint8_t len = r[p];
        if (len == 0) return p + 1;
        if ((len & 0xC0) == 0xC0) return (p + 2 <= n) ? p + 2 : 0;
        p += 1 + (size_t)len;
    }
    return 0;
}

static int parse(const uint8_t *r, size_t n, uint16_t id, uint8_t out[4]) {
    if (n < 12 || net_get16(r) != id) return PARSE_NOT_OURS;

    uint16_t flags = net_get16(r + 2);
    if (!(flags & 0x8000)) return PARSE_NOT_OURS;         /* это не ответ */
    int rcode = flags & 0x0F;
    if (rcode == 3) return DNS_ERR_NXDOMAIN;
    if (rcode != 0) return DNS_ERR_SERVER;

    uint16_t qd = net_get16(r + 4), an = net_get16(r + 6);
    size_t p = 12;

    for (uint16_t i = 0; i < qd; i++) {
        p = skip_name(r, n, p);
        if (!p || p + 4 > n) return DNS_ERR_SERVER;
        p += 4;                                           /* QTYPE + QCLASS */
    }
    for (uint16_t i = 0; i < an; i++) {
        p = skip_name(r, n, p);
        if (!p || p + 10 > n) return DNS_ERR_SERVER;
        uint16_t type  = net_get16(r + p);
        uint16_t rdlen = net_get16(r + p + 8);
        p += 10;
        if (p + rdlen > n) return DNS_ERR_SERVER;
        if (type == 1 && rdlen == 4) {                    /* A */
            net_copy(out, r + p, 4);
            return DNS_OK;
        }
        p += rdlen;                                       /* CNAME и прочее — мимо */
    }
    return DNS_ERR_NOADDR;
}

int dns_resolve(const char *name, uint8_t out[4]) {
    static uint8_t q[300];
    static uint8_t r[512];

    uint16_t id = ++id_counter;
    for (int i = 0; i < 12; i++) q[i] = 0;
    net_put16(q, id);
    net_put16(q + 2, 0x0100);                             /* RD: нужна рекурсия */
    net_put16(q + 4, 1);                                  /* один вопрос */

    size_t p = 12;
    const char *s = name;
    while (*s) {
        const char *e = s;
        while (*e && *e != '.') e++;
        size_t len = (size_t)(e - s);
        if (len == 0 || len > 63 || p + 1 + len + 5 > 254 + 12) return DNS_ERR_NAME;
        q[p++] = (uint8_t)len;
        for (size_t i = 0; i < len; i++) q[p++] = (uint8_t)s[i];
        s = e;
        if (*s == '.') s++;                               /* завершающая точка допустима */
    }
    if (p == 12) return DNS_ERR_NAME;
    q[p++] = 0;
    net_put16(q + p, 1);  p += 2;                         /* QTYPE = A */
    net_put16(q + p, 1);  p += 2;                         /* QCLASS = IN */

    for (int attempt = 0; attempt < 3 && !net_aborted(); attempt++) {
        if (udp_send(net_cfg.dns, DNS_PORT_LOCAL, DNS_PORT_SERVER, q, p) != NET_OK)
            return DNS_ERR_NET;

        for (int k = 0; k < 4; k++) {
            int n = udp_recv(DNS_PORT_LOCAL, r, sizeof r, 0, 0, NET_POLL_LIMIT / 4);
            if (n < 0) break;                             /* таймаут — повторить запрос */
            int res = parse(r, (size_t)n, id, out);
            if (res != PARSE_NOT_OURS) return res;
        }
    }
    return DNS_ERR_TIMEOUT;
}
