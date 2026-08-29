/* apps/hello.c — пример прикладной программы поверх HAL/app_api.
 * Не знает, x86 под ней или ARM64.
 */
#include "app_api.h"

void app_main(void) {
    app_print("privet ot prilozheniya poverh HAL!\n");

    void *buf = app_alloc(64);
    (void)buf; /* демонстрация выделения памяти прикладным кодом */

    app_print("gotovo.\n");
}
