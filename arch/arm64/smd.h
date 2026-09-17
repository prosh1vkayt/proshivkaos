/* arch/arm64/smd.h — канал SMD к сопроцессору (кроме питания, см. smd_rpm.c). */
#ifndef ARM64_SMD_H
#define ARM64_SMD_H

#include <stdint.h>

#define SMD_CLOSED   0
#define SMD_OPENING  1
#define SMD_OPENED   2

typedef struct {
    char     name[21];
    int      host;          /* хозяин раздела SMEM: 4 — Wi-Fi          */
    int      edge;          /* край SMD: 6 — приложения и Wi-Fi         */
    uint32_t ipc_bit;       /* бит звонка в регистре межпроцессорных    */
    uint64_t info;          /* запись состояния: наша половина, их      */
    uint64_t tx, rx;        /* очереди                                  */
    uint32_t fifo_size;
    int      word;          /* пословная запись состояния               */
    int      state;         /* наше состояние                           */
    int      remote_state;
    uint32_t pkt_size;      /* длина принимаемой посылки, 0 — ждём заголовок */
} smd_chan_t;

typedef void (*smd_rx_fn)(smd_chan_t *ch, const uint8_t *data, uint32_t len);

/* Найти канал по имени в разделе пары «приложения — host» и начать
   открытие. 1 — нашёлся. */
int  smd_chan_open(smd_chan_t *ch, const char *name, int host, int edge, uint32_t ipc_bit);
/* Шаг: состояния, приём посылок. Не блокирует. */
void smd_chan_poll(smd_chan_t *ch, smd_rx_fn rx);
/* Отправить посылку. 0 — нет места (повторить позже) или канал не открыт. */
int  smd_chan_send(smd_chan_t *ch, const uint8_t *data, uint32_t len);

#endif
