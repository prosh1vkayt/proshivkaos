# wcn36xx HAL

`hal.h` — описание управляющего протокола прошивки Wi-Fi Qualcomm (WCN36xx /
Pronto): типы сообщений, структуры запросов и ответов, номера параметров.
Взят без изменений из ядра Linux, `drivers/net/wireless/ath/wcn36xx/hal.h`.

Лицензия — ISC, текст в начале файла (Copyright (c) 2013 Eugene Krasnikov).
Используется в `arch/arm64/wlan*.c` через `arch/arm64/wcn_hal.h`.
