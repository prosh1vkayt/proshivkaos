/* apps/app_api.h — «системная библиотека» для прикладных программ
 * proshivkaOS NEXT. Это НЕ syscalls через прерывания (пока приложения
 * линкуются статически в тот же образ, что и ядро — нет ни MMU-изоляции,
 * ни колец защиты на этом этапе). Формально это тонкая обёртка над hal.h +
 * fs/shell API, но название нарочно другое: контракт "app_api.h" не должен
 * меняться, даже когда под капотом появятся настоящие syscalls (int 0x80 /
 * SVC на ARM64) — тогда поменяется только реализация функций ниже, а
 * прикладной код, который их вызывает, останется тем же самым бинарно
 * несовместимым, но исходно совместимым кодом.
 *
 * Пример программы под HAL (см. apps/hello.c):
 *
 *   #include "app_api.h"
 *
 *   void app_main(void) {
 *       app_print("privet ot prilozheniya!\n");
 *   }
 *
 * app_main() — точка входа, которую сейчас вызывает shell напрямую (как
 * встроенную команду, статически слинкованную в ядро). На следующем этапе,
 * когда появится RamFS-загрузчик .bin-программ и переключение контекста,
 * та же функция будет запускаться как отдельный hal_thread_create(app_main, ...)
 * — код приложения переписывать не придётся.
 */
#ifndef PROSHIVKAOS_APP_API_H
#define PROSHIVKAOS_APP_API_H

#include "hal.h"

static inline void app_print(const char *s)   { hal_console_write(s); }
static inline void app_putc(char c)            { hal_console_putc(c); }
static inline char app_getc(void)              { return hal_console_getc_blocking(); }
static inline void *app_alloc(size_t size)      { return hal_mem_alloc(size); }
static inline void app_yield(void)               { hal_thread_yield(); }

/* Каждое приложение реализует эту функцию — это его "main()". */
void app_main(void);

#endif
