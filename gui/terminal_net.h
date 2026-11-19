/* gui/terminal_net.h — подключает сетевые команды к окну терминала. */
#ifndef GUI_TERMINAL_NET_H
#define GUI_TERMINAL_NET_H

/* Инициализирует сетевую карту и регистрирует команды ifconfig, arping,
 * ping, dhcp, udpsend, nslookup, http в терминале.
 *
 *   redraw — попросить интерфейс перерисоваться (зовётся после каждой
 *            выведенной строки);
 *   pump   — обработать события интерфейса, пока команда ждёт ответа;
 *            вернуть не 0, если команду нужно прервать (Ctrl+C). */
void terminal_net_install(void (*redraw)(void), int (*pump)(void));

#endif
