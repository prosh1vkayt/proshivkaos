/* net/dns.h — DNS-клиент (RFC 1035): запрос A-записи по UDP к net_cfg.dns. */
#ifndef NET_DNS_H
#define NET_DNS_H

#include <stdint.h>

#define DNS_OK              0
#define DNS_ERR_NAME       -1   /* пустое имя, метка длиннее 63 или имя длиннее 253 */
#define DNS_ERR_TIMEOUT    -2   /* сервер не ответил */
#define DNS_ERR_NXDOMAIN   -3   /* такого имени нет */
#define DNS_ERR_NOADDR     -4   /* имя есть, но A-записи нет (например, только IPv6) */
#define DNS_ERR_NET        -5   /* не удалось отправить запрос */
#define DNS_ERR_SERVER     -6   /* сервер вернул ошибку (SERVFAIL, REFUSED...) */

/* Превращает имя ("example.com") в IPv4-адрес. */
int dns_resolve(const char *name, uint8_t out[4]);

#endif
