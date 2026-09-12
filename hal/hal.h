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
 * до памяти.
 *
 * boot_info — то, что передал загрузчик: на x86 это указатель на структуру
 * Multiboot (не используется), на ARM64 — на device tree. Дерево здесь и
 * разбирается: из него берутся адреса порта, контроллера прерываний и
 * готового экрана. Именно поэтому сюда нужен параметр — раньше эти адреса
 * были захардкожены, и система работала только под QEMU.
 *
 * На x86 делать нечего (GRUB оставляет процессор в пригодном состоянии).
 * На ARM64 здесь же включается MMU: без него всё ОЗУ считается
 * некэшируемым, и отрисовка кадра тормозит на порядок. */
void hal_arch_init(void *boot_info);

/* Отладочная метка на экране: полоса номер index заданного цвета.
 *
 * Нужна там, где другого вывода нет вовсе. На телефоне отладочный порт
 * выведен на разъём наушников и требует паяного кабеля, журнал без прав
 * администратора не прочитать — остаётся экран. Сколько полос успело
 * появиться, до того места и дошла загрузка.
 *
 * На платформах, где это не нужно или невозможно, раскрывается в пустоту:
 * см. cpu.c в каталоге нужной архитектуры. */
void hal_debug_mark(int index, unsigned char r, unsigned char g, unsigned char b);

/* Строка отладочного вывода туда же, куда идут метки. На телефоне
 * печатается прямо в кадр, минуя графический слой, — то есть работает и
 * тогда, когда тот сломан или до него ещё не дошло. */
void hal_debug_text(const char *s);

/* Ход загрузки для того, кто смотрит на экран.
 *
 * Отладочные метки выше рассказывают, ГДЕ система, и адресованы тому, кто
 * разбирается. Эти две — только о том, что она движется, и адресованы
 * тому, кто просто ждёт. На платах, где показывать нечем, обе
 * раскрываются в пустоту. */
void hal_debug_progress(void);
void hal_debug_boot_done(void);

/* Человекочитаемое имя платформы ("X86", "ARM64") — для neofetch и экрана
 * "О системе". Единственный способ узнать архитектуру выше HAL, не
 * заводя #ifdef в прикладном коде. */
const char *hal_arch_name(void);

/* Модель процессора человекочитаемо. На ARM64 разбирается регистр MIDR_EL1
 * (производитель + номер ядра), на x86 читается строка производителя через
 * CPUID. Нужно экрану "о системе" — и заодно это первое, что хочешь
 * увидеть, запустив систему на незнакомом железе. */
const char *hal_cpu_name(void);

/* Название платы. На ARM64 — строка model из device tree, то есть то, как
 * представился САМ загрузчик устройства ("Qualcomm Technologies, Inc.
 * APQ8053 MTP"). Это не украшение: на телефоне без отладочного кабеля
 * экран "о системе" — единственный способ увидеть, что дерево разобрано и
 * адреса взяты из железа, а не из зашитых констант. */
const char *hal_board_name(void);

void hal_panic(const char *msg) __attribute__((noreturn));
void hal_cpu_halt(void);          /* остановить CPU до след. прерывания (cli+hlt / wfi) */

#endif /* PROSHIVKAOS_HAL_H */
