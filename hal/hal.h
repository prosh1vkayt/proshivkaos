/* hal/hal.h
 * proshivkaOS NEXT — единственная точка входа в архитектурно-зависимый код.
 * Ядро, shell и fs подключают только этот заголовок и никогда не знают,
 * x86 они или ARM64.
 *
 * -nostdlib окружение: своих size_t/uint32_t и т.п. у нас нет из <stdint.h>,
 * поэтому объявляем минимальный набор типов сами (freestanding <stdint.h>
 * от GCC разрешён стандартом даже без libc — используем его, он не тянет libc).
 */
#ifndef PROSHIVKAOS_HAL_H
#define PROSHIVKAOS_HAL_H

#include <stdint.h>   /* freestanding header, идёт с компилятором, не с libc */
#include <stddef.h>   /* size_t — тоже freestanding */

/* Версия системы. Одна строка на весь проект: её показывают и neofetch в
 * терминале, и экран "о системе". */
#define PROSHIVKAOS_VERSION "0.3.0"

/* ---------------- Консоль ---------------- */
void hal_console_init(void);
void hal_console_putc(char c);
void hal_console_write(const char *s);
int  hal_console_getc(void);      /* -1, если нет данных (неблокирующий poll) */
char hal_console_getc_blocking(void);

/* Цвет консоли: 16-цветная VGA-палитра (0..15), см. hal_console_colors ниже.
 * На бэкендах без цвета (будущий ARM64/UART) — просто no-op. */
void hal_console_set_color(uint8_t fg, uint8_t bg);
void hal_console_reset_color(void);

enum {
    HAL_COLOR_BLACK = 0,  HAL_COLOR_BLUE = 1,       HAL_COLOR_GREEN = 2,
    HAL_COLOR_CYAN  = 3,  HAL_COLOR_RED = 4,        HAL_COLOR_MAGENTA = 5,
    HAL_COLOR_BROWN = 6,  HAL_COLOR_LIGHT_GREY = 7, HAL_COLOR_DARK_GREY = 8,
    HAL_COLOR_LIGHT_BLUE = 9,  HAL_COLOR_LIGHT_GREEN = 10, HAL_COLOR_LIGHT_CYAN = 11,
    HAL_COLOR_LIGHT_RED = 12,  HAL_COLOR_LIGHT_MAGENTA = 13, HAL_COLOR_YELLOW = 14,
    HAL_COLOR_WHITE = 15
};

/* ---------------- Память ---------------- */
void  hal_mem_init(void);
void *hal_mem_alloc(size_t size);
void  hal_mem_free(void *ptr);
size_t hal_mem_used(void);    /* сколько байт кучи занято  */
size_t hal_mem_total(void);   /* сколько всего доступно     */

/* ---------------- Потоки (заготовка API, реализация — этап 2) ---------------- */
typedef struct hal_thread hal_thread_t;
hal_thread_t *hal_thread_create(void (*entry)(void *arg), void *arg, size_t stack_size);
void hal_thread_yield(void);

/* ---------------- Прочее ---------------- */

/* Инициализация платформы — САМОЕ первое, что делает kmain(), до консоли и
 * до памяти. На x86 делать нечего (GRUB оставляет процессор в пригодном
 * состоянии). На ARM64 здесь включается MMU: без него всё ОЗУ считается
 * некэшируемым, и отрисовка кадра тормозит на порядок. */
void hal_arch_init(void);

/* Человекочитаемое имя платформы ("X86", "ARM64") — для neofetch и экрана
 * "О системе". Единственный способ узнать архитектуру выше HAL, не
 * заводя #ifdef в прикладном коде. */
const char *hal_arch_name(void);

/* Модель процессора человекочитаемо. На ARM64 разбирается регистр MIDR_EL1
 * (производитель + номер ядра), на x86 читается строка производителя через
 * CPUID. Нужно экрану "о системе" — и заодно это первое, что хочешь
 * увидеть, запустив систему на незнакомом железе. */
const char *hal_cpu_name(void);

void hal_panic(const char *msg) __attribute__((noreturn));
void hal_cpu_halt(void);          /* остановить CPU до след. прерывания (cli+hlt / wfi) */

#endif /* PROSHIVKAOS_HAL_H */
