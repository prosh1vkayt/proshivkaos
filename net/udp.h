/* net/udp.h — UDP поверх netcore. */
#ifndef NET_UDP_H
#define NET_UDP_H

#include <stdint.h>
#include <stddef.h>

/* Отправить датаграмму. NET_OK / NET_ERR_ARP / NET_ERR_TX (см. netcore.h). */
int udp_send(const uint8_t dst[4], uint16_t sport, uint16_t dport,
             const void *data, size_t len);

/* Широковещательная отправка с адреса 0.0.0.0 — для DHCP, пока своего
 * адреса ещё нет. */
int udp_send_broadcast(uint16_t sport, uint16_t dport,
                       const void *data, size_t len);

/* Ждать датаграмму на локальный порт. Возвращает длину данных или -1 по
 * таймауту (limit — число итераций опроса, обычно NET_POLL_LIMIT).
 * src_ip и src_port можно передать NULL. */
int udp_recv(uint16_t port, void *buf, size_t max,
             uint8_t src_ip[4], uint16_t *src_port, long limit);

#endif
