/* net/netcore.h — общая часть сетевого слоя: настройки интерфейса,
 * контрольные суммы, ARP, отправка и приём IPv4. Поверх hal_net_*. */
#ifndef NET_NETCORE_H
#define NET_NETCORE_H

#include <stdint.h>
#include <stddef.h>

#define NET_OK         0
#define NET_ERR_ARP   -2
#define NET_ERR_TX    -3

#define NET_POLL_LIMIT 20000000L   /* итераций опроса карты за одно ожидание */

/* Все ожидания в сетевом слое — циклы опроса карты. Графическому
 * интерфейсу нельзя замирать на секунды, поэтому в каждом цикле
 * периодически вызывается «насос»: он обрабатывает клавиатуру/мышь и
 * перерисовывает окно. Если насос вернул не 0 (пользователь нажал Ctrl+C),
 * операция прерывается: net_aborted() становится истинным, и все циклы
 * ожидания сразу заканчиваются. В текстовой консоли насоса нет, и всё
 * работает как раньше. */
typedef int (*net_pump_fn)(void);
void net_set_pump(net_pump_fn fn);
void net_pump(void);               /* зовётся из циклов ожидания */
int  net_aborted(void);
void net_abort_clear(void);        /* вызывать перед каждой новой командой */

/* Условие продолжения цикла ожидания. */
#define NET_WAITING(t, limit)  ((t) < (limit) && !net_aborted())

typedef struct {
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gw[4];
    uint8_t dns[4];
    int     from_dhcp;             /* 1, если адрес получен по DHCP */
} net_cfg_t;

/* Значения по умолчанию — адреса user-режима QEMU. После `dhcp` заменяются
 * тем, что выдал сервер. */
extern net_cfg_t net_cfg;

static inline void net_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline uint16_t net_get16(const uint8_t *p)   { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void net_copy(uint8_t *d, const uint8_t *s, size_t n) { while (n--) *d++ = *s++; }
static inline int net_ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}
static inline int net_ip_zero(const uint8_t *a) { return !(a[0] | a[1] | a[2] | a[3]); }

/* Прочитать MAC из драйвера. Вызвать один раз после hal_net_init(). */
void net_cfg_init(void);

/* Интернет-контрольная сумма (RFC 1071): add можно вызывать по кускам
 * (все, кроме последнего, чётной длины), fin сворачивает и инвертирует. */
uint32_t net_csum_add(uint32_t sum, const uint8_t *d, size_t n);
uint16_t net_csum_fin(uint32_t sum);
uint16_t net_csum(const uint8_t *d, size_t n);

/* ARP */
void net_arp_flush(void);
int  net_arp_resolve(const uint8_t ip[4], uint8_t mac_out[6]);

/* Отправить IPv4-пакет с явным MAC получателя и адресом источника
 * (нужно DHCP: источник 0.0.0.0, получатель — широковещание). */
int net_ip_send_raw(const uint8_t dst_mac[6], const uint8_t src_ip[4],
                    const uint8_t dst_ip[4], uint8_t proto,
                    const uint8_t *payload, size_t len);

/* Обычная отправка: сама выбирает следующий хоп (цель или шлюз) и делает
 * ARP. Для 255.255.255.255 идёт широковещательным кадром. */
int net_ip_send(const uint8_t dst[4], uint8_t proto,
                const uint8_t *payload, size_t len);

/* Неблокирующий приём одного IPv4-пакета без фрагментации, адресованного
 * нам или широковещательного (пока адреса нет — любого). ARP-запросы к нам
 * отвечаются по ходу. Возвращает длину полезной нагрузки или 0, если
 * ничего подходящего нет. */
size_t net_ip_recv(uint8_t *proto, uint8_t src[4], uint8_t dst[4],
                   uint8_t *buf, size_t max);

#endif
