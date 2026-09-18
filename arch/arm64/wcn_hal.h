/* arch/arm64/wcn_hal.h — структуры протокола HAL прошивки Wi-Fi.
 *
 * Сообщения CONFIG_BSS и CONFIG_STA — это сотни байт упакованных полей, и
 * считать смещения руками значит ошибиться. Поэтому берём сам hal.h из
 * драйвера wcn36xx (ISC, third_party/wcn36xx) и даём ему то, чего он ждёт
 * от ядра Linux: типы фиксированной ширины и пару макросов. */
#ifndef ARM64_WCN_HAL_H
#define ARM64_WCN_HAL_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint16_t __be16;

#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef BIT
#define BIT(n) (1u << (n))
#endif
#define ETH_ALEN               6
#define WLAN_MAX_KEY_LEN       32
#define IEEE80211_MAX_SSID_LEN 32

#include "../../third_party/wcn36xx/hal.h"

#endif
