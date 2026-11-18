/* net/tcp.h — минимальный TCP-клиент: одно соединение, активное открытие,
 * опрос в цикле (без прерываний и таймеров). */
#ifndef NET_TCP_H
#define NET_TCP_H

#include <stdint.h>
#include <stddef.h>

#define TCP_OK            0
#define TCP_ERR_ARP      -2
#define TCP_ERR_TX       -3
#define TCP_ERR_TIMEOUT  -4
#define TCP_ERR_RESET    -5   /* пришёл RST: порт закрыт или соединение сброшено */
#define TCP_ERR_STATE    -6   /* соединение не установлено */

/* Установить соединение (трёхстороннее рукопожатие). 0 — успех. */
int tcp_connect(const uint8_t ip[4], uint16_t port);

/* Отправить данные; возвращает len или код ошибки. Блокируется, пока
 * получатель не подтвердит приём; потерянный сегмент шлётся повторно. */
int tcp_send(const void *data, size_t len);

/* Принять данные. > 0 — число байт; 0 — собеседник закрыл соединение и
 * все данные уже прочитаны; < 0 — ошибка или таймаут (limit — число
 * итераций опроса, обычно NET_POLL_LIMIT). */
int tcp_recv(void *buf, size_t max, long limit);

/* Закрыть соединение (посылает FIN, ждёт подтверждения недолго). */
void tcp_close(void);

#endif
