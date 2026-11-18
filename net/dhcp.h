/* net/dhcp.h — DHCP-клиент (RFC 2131): DISCOVER → OFFER → REQUEST → ACK. */
#ifndef NET_DHCP_H
#define NET_DHCP_H

/* Получает адрес, маску, шлюз и DNS и записывает их в net_cfg.
 * 0 — успех; -1 — сервер не ответил или отказал (net_cfg не тронут). */
int dhcp_acquire(void);

#endif
