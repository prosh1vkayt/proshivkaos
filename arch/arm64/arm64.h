/* arch/arm64/arm64.h — внутренние объявления ARM64-бэкенда.
 * Этот заголовок НЕ подключается ничем за пределами arch/arm64/ —
 * наружу торчит только hal.h / hal_gfx.h / hal_input.h / hal_time.h.
 */
#ifndef ARCH_ARM64_H
#define ARCH_ARM64_H

#include <stdint.h>
#include <stddef.h>

/* ---------------- Карта памяти QEMU "virt" ----------------
 * Реальный телефон отдаёт эти адреса через device tree; на этапе
 * отладки под QEMU они фиксированы и совпадают из версии в версию.
 * Для порта на конкретный SoC меняются только эти константы. */
#define VIRT_UART0_BASE    0x09000000UL   /* PL011 UART        */
#define VIRT_RTC_BASE      0x09010000UL   /* PL031 RTC          */
#define VIRT_FWCFG_BASE    0x09020000UL   /* QEMU fw_cfg (MMIO) */
#define VIRT_VIRTIO_BASE   0x0a000000UL   /* virtio-mmio слот 0 */
#define VIRT_VIRTIO_STRIDE 0x200UL
#define VIRT_VIRTIO_SLOTS  32

/* ---------------- MMIO ---------------- */
static inline void     mmio_write32(uint64_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }
static inline uint32_t mmio_read32 (uint64_t addr)             { return *(volatile uint32_t *)addr; }
static inline void     mmio_write16(uint64_t addr, uint16_t v) { *(volatile uint16_t *)addr = v; }
static inline uint16_t mmio_read16 (uint64_t addr)             { return *(volatile uint16_t *)addr; }
static inline void     mmio_write8 (uint64_t addr, uint8_t v)  { *(volatile uint8_t  *)addr = v; }
static inline uint8_t  mmio_read8  (uint64_t addr)             { return *(volatile uint8_t  *)addr; }
static inline void     mmio_write64(uint64_t addr, uint64_t v) { *(volatile uint64_t *)addr = v; }
static inline uint64_t mmio_read64 (uint64_t addr)             { return *(volatile uint64_t *)addr; }

/* Барьеры. dsb — дождаться завершения ВСЕХ обращений к памяти, dmb —
 * только упорядочить. Нужны везде, где мы говорим "железо, забирай":
 * без них процессор имеет право переставить запись дескриптора после
 * записи в регистр-звонок, и устройство прочитает мусор. */
static inline void dsb_sy(void) { __asm__ volatile ("dsb sy" ::: "memory"); }
static inline void dmb_sy(void) { __asm__ volatile ("dmb sy" ::: "memory"); }
static inline void isb(void)    { __asm__ volatile ("isb"    ::: "memory"); }

/* Big-endian помощники: fw_cfg — единственный интерфейс QEMU, который
 * общается в сетевом порядке байт вне зависимости от порядка байт CPU. */
static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t bswap32(uint32_t v) {
    return ((v >> 24) & 0x000000FFu) | ((v >> 8) & 0x0000FF00u) |
           ((v <<  8) & 0x00FF0000u) | ((v << 24) & 0xFF000000u);
}
static inline uint64_t bswap64(uint64_t v) {
    return ((uint64_t)bswap32((uint32_t)v) << 32) | bswap32((uint32_t)(v >> 32));
}

/* ---------------- Последовательный порт ----------------
 * Диспетчер в arch/arm64/uart.c: какой контроллер за этими функциями —
 * PL011 или Qualcomm UARTDM — выясняется во время выполнения по device
 * tree. Вызывающий код разницы не видит. */
void uart_init(void);
void uart_putc(char c);
void uart_write(const char *s);
int  uart_getc(void);            /* -1, если нечего читать */
void uart_write_hex(uint64_t v); /* отладочный вывод — до появления GUI */
void uart_report_platform(void); /* что удалось выяснить про железо */

/* ---------------- MMU и кэши ---------------- */
/* Без MMU все обращения к ОЗУ трактуются как Device-nGnRnE (некэшируемые),
 * и отрисовка кадра в фреймбуфер получается в десятки раз медленнее.
 * Поэтому включаем MMU с плоским (identity) отображением 0..4 ГиБ. */
void mmu_init(void);
int  mmu_enabled(void);

/* Обслуживание кэша для DMA. Устройство (ramfb-дисплей, virtio) читает
 * ОЗУ мимо кэша процессора, поэтому после записи данных для устройства
 * их надо вытолкнуть (clean), а перед чтением записанного устройством —
 * выбросить свою устаревшую копию (invalidate). */
void arch_dcache_clean(const void *addr, size_t len);

/* --- Постоянный журнал (arch/arm64/ramoops.c) -------------------------
 * Пишет в область ОЗУ, переживающую перезагрузку, чтобы лог можно было
 * прочитать из Android после неудачного старта. На платах без такой
 * области не собирается вовсе — см. CONFIG_LOG_RAMOOPS. */
#ifdef CONFIG_LOG_RAMOOPS
void ramoops_init(void);
void ramoops_putc(char c);
void ramoops_write(const char *s);
#else
#define ramoops_init()    do { } while (0)
#define ramoops_putc(c)   do { (void)(c); } while (0)
#define ramoops_write(s)  do { (void)(s); } while (0)
#endif
void arch_dcache_invalidate(void *addr, size_t len);

/* ---------------- fw_cfg (QEMU) ---------------- */
int  fwcfg_init(void);
/* Найти файл в каталоге fw_cfg по имени ("etc/ramfb"); возвращает 1 и
 * записывает селектор + размер. */
int  fwcfg_find_file(const char *name, uint16_t *select, uint32_t *size);
/* Записать буфер в файл fw_cfg через DMA-интерфейс. 1 = успех. */
int  fwcfg_dma_write(uint16_t select, const void *data, uint32_t len);

/* ---------------- ramfb (дисплей) ---------------- */
/* Просит QEMU показывать наш буфер как экран. Формат — XRGB8888.
 * 1 = успех; 0, если ramfb не подключён (нет -device ramfb). */
int  ramfb_setup(void *framebuffer, int width, int height);

/* ---------------- Тактирование (Qualcomm GCC) ----------------
 * Периферия на Snapdragon по умолчанию обесточена по тактам. Порт UART
 * достался нам уже включённым от загрузчика, а шину I2C тачскрина
 * приходится поднимать самим. */
int  gcc_enable_blsp1_qup_i2c(int qup_index);

/* ---------------- Выводы общего назначения (Qualcomm TLMM) ---------------- */
void tlmm_gpio_output(int gpio, int value);
void tlmm_gpio_input(int gpio, int pull_up);
void tlmm_gpio_set(int gpio, int value);
int  tlmm_gpio_get(int gpio);

/* ---------------- Ведущий I2C (Qualcomm QUP v2) ---------------- */
int  i2c_qup_init(uint64_t base, uint32_t core_hz, uint32_t bus_hz);
/* Записать wlen байт, затем через ПОВТОРНЫЙ СТАРТ прочитать rlen.
 * Любую из частей можно опустить, передав нулевую длину. */
int  i2c_qup_xfer(uint64_t base, uint8_t addr,
                   const uint8_t *wbuf, int wlen, uint8_t *rbuf, int rlen);
int  i2c_qup_write_reg(uint64_t base, uint8_t addr, uint8_t reg, uint8_t value);
int  i2c_qup_read_regs(uint64_t base, uint8_t addr, uint8_t reg, uint8_t *out, int len);

/* ---------------- Тачскрин FocalTech FT5x06 / FT5435 ----------------
 * Собирается только для плат, у которых он есть (см. Makefile, BOARD).
 * На прочих сборках вместо этих функций подставляются слабые заглушки в
 * hal/hal_input_arm64.c, и слой ввода про разницу не знает. */
int  ft5x06_init(void);
int  ft5x06_ready(void);
int  ft5x06_poll(int *x, int *y, int *pressed);
void ft5x06_range(int *max_x, int *max_y);

/* ---------------- virtio-input (тач/клавиатура) ---------------- */
void virtio_input_init(void);

typedef struct {
    uint16_t type;
    uint16_t code;
    int32_t  value;
} vinput_event_t;

/* Неблокирующий опрос: 1 и заполненное событие, если оно есть. */
int  virtio_input_poll(vinput_event_t *ev);
/* Диапазон абсолютных координат тачскрина (для масштабирования в пиксели).
 * 0, если абсолютного устройства не нашлось. */
int  virtio_input_abs_range(int *max_x, int *max_y);
/* Сколько устройств ввода удалось поднять. На реальном телефоне ноль. */
int  virtio_input_device_count(void);

#endif
