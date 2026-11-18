/* hal/hal_net_stub.c — слабые заглушки сети для платформ без драйвера.
 * Нужны, чтобы shell/shell.c (общий для всех архитектур, без #ifdef)
 * линковался везде. Настоящий драйвер (arch/x86/net_rtl8139.c) определяет
 * те же символы без __attribute__((weak)) и перекрывает эти.
 */
#include <stdint.h>
#include <stddef.h>

__attribute__((weak)) int    hal_net_init(void) { return -1; }
__attribute__((weak)) void   hal_net_get_mac(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = 0; }
__attribute__((weak)) int    hal_net_send(const void *d, size_t n) { (void)d; (void)n; return -1; }
__attribute__((weak)) size_t hal_net_recv(void *o, size_t m) { (void)o; (void)m; return 0; }
