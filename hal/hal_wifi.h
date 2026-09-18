/* hal/hal_wifi.h — Wi-Fi для интерфейса.
 *
 * Радио есть только на телефоне (arch/arm64/wlan.c). В остальных сборках
 * действуют слабые заглушки из gui/touch/app_wifi.c: состояние
 * HAL_WIFI_UNSUPPORTED и пустой список — приложение честно говорит, что
 * радио здесь нет. */
#ifndef HAL_WIFI_H
#define HAL_WIFI_H

#include <stdint.h>

#define HAL_WIFI_UNSUPPORTED 0
#define HAL_WIFI_OFF         1
#define HAL_WIFI_STARTING    2
#define HAL_WIFI_SCANNING    3
#define HAL_WIFI_READY       4
#define HAL_WIFI_FAILED      5

#define HAL_WIFI_OPEN        0
#define HAL_WIFI_PROTECTED   1   /* бит Privacy без RSN: WEP/WPA */
#define HAL_WIFI_WPA2        2

typedef struct {
    char    ssid[33];            /* пусто — скрытая сеть */
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;                /* dBm */
    uint8_t security;
} hal_wifi_net_t;

int      hal_wifi_state(void);
/* Включить радио (запуск Pronto) — вернётся не сразу, радио поднимается
   в фоне; за ходом следить по hal_wifi_state(). */
void     hal_wifi_enable(void);
void     hal_wifi_scan(void);
int      hal_wifi_count(void);
int      hal_wifi_get(int index, hal_wifi_net_t *out);
/* Растёт при каждом изменении состояния или списка — чтобы понять, пора ли
   перерисовать экран. */
uint32_t hal_wifi_generation(void);

/* ---- подключение ---- */
#define HAL_WIFI_CONN_IDLE      0
#define HAL_WIFI_CONN_WORKING   1   /* идёт подключение */
#define HAL_WIFI_CONN_ONLINE    2   /* подключено, адрес получен */
#define HAL_WIFI_CONN_FAILED    3

/* Подключиться к сети из списка (индекс как в hal_wifi_get). Для закрытой
   сети нужен пароль; для открытой pass игнорируется. */
void     hal_wifi_connect(int index, const char *pass);
void     hal_wifi_disconnect(void);
int      hal_wifi_conn_state(void);
/* Имя сети, к которой подключены/подключаемся; 0 — ни к какой. */
const char *hal_wifi_conn_ssid(void);
/* Локальный IPv4 (0, пока нет). buf >= 16 байт. */
void     hal_wifi_ip(char *buf);

#endif
