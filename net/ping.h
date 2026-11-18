/* net/ping.h — ARP + IPv4 + ICMP echo поверх hal_net_*. Не зависит от
 * архитектуры: нужны только hal_net_send/hal_net_recv/hal_net_get_mac. */
#ifndef NET_PING_H
#define NET_PING_H

#include <stdint.h>

#define PING_OK         0
#define PING_NO_REPLY  -1   /* ответ не пришёл за отведённое время */
#define PING_NO_ARP    -2   /* не удалось узнать MAC (цели или шлюза) */
#define PING_TX_ERR    -3   /* драйвер не смог отправить кадр */

/* Один ICMP echo. dst — IPv4 адрес из 4 байт. */
int net_ping(const uint8_t dst[4], uint16_t seq);

#endif
