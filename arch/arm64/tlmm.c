/* arch/arm64/tlmm.c — выводы общего назначения (GPIO) на Qualcomm TLMM.
 *
 * TLMM (Top Level Mode Multiplexer) — блок, который решает, чем является
 * каждый физический вывод корпуса: обычным GPIO, ножкой I2C, ножкой UART и
 * так далее. Тачскрину от него нужны ровно два вывода: линия сброса
 * (мы ею дёргаем) и линия прерывания (её мы опрашиваем).
 *
 * Раскладка регистров у TLMM устроена непривычно, но удобно: у каждого
 * вывода свой отдельный «тайл» размером 4 КиБ, и адрес считается простым
 * умножением номера вывода. Никаких битовых полей, размазанных по общим
 * регистрам, — поэтому драйвер такой короткий.
 *
 * База 0x01000000 и размер 0x300000 (то есть 768 выводов по 4 КиБ) сняты с
 * живого устройства: узел pinctrl@1000000, compatible
 * "qcom,msm8953-pinctrl".
 */
#include "arm64.h"
#include "boards/board.h"

#define TLMM_TILE_SIZE   0x1000
#define TLMM_MAX_GPIO    150      /* у msm8953 их 142; с запасом на проверку */

/* Смещения внутри тайла вывода */
#define TLMM_GPIO_CFG    0x00
#define TLMM_GPIO_IN_OUT 0x04

/* Поля регистра настройки вывода */
#define CFG_PULL_SHIFT   0
#define CFG_PULL_NONE    0
#define CFG_PULL_DOWN    1
#define CFG_PULL_KEEPER  2
#define CFG_PULL_UP      3
#define CFG_FUNC_SHIFT   2        /* 0 = обычный GPIO, прочее — альтернативы */
#define CFG_DRV_SHIFT    6        /* сила драйвера: (мА / 2) - 1            */
#define CFG_OE           (1u << 9) /* разрешение выхода                      */

/* Регистр состояния линии */
#define IN_OUT_IN        (1u << 0) /* чтение: текущий уровень на выводе      */
#define IN_OUT_OUT       (1u << 1) /* запись: уровень, который выдаём        */

static uint64_t gpio_tile(int gpio) {
    return BOARD_TLMM_BASE + (uint64_t)gpio * TLMM_TILE_SIZE;
}

/* Настроить вывод как выход и выставить уровень. */
void tlmm_gpio_output(int gpio, int value) {
    if (gpio < 0 || gpio >= TLMM_MAX_GPIO) return;

    uint64_t t = gpio_tile(gpio);

    /* Уровень выставляем ДО включения выхода: иначе на линии успевает
       мелькнуть предыдущее значение. Для линии сброса тачскрина такой
       выброс — это лишний незапланированный сброс. */
    mmio_write32(t + TLMM_GPIO_IN_OUT, value ? IN_OUT_OUT : 0);

    uint32_t cfg = (CFG_PULL_NONE << CFG_PULL_SHIFT) |
                   (0u << CFG_FUNC_SHIFT) |          /* обычный GPIO       */
                   (1u << CFG_DRV_SHIFT)  |          /* 4 мА — хватает     */
                   CFG_OE;
    mmio_write32(t + TLMM_GPIO_CFG, cfg);
    dsb_sy();
}

/* Настроить вывод как вход с подтяжкой вверх.
 * Линия прерывания тачскрина активна низким уровнем, поэтому в покое её
 * нужно что-то удерживать в единице — этим и занимается подтяжка. */
void tlmm_gpio_input(int gpio, int pull_up) {
    if (gpio < 0 || gpio >= TLMM_MAX_GPIO) return;

    uint64_t t = gpio_tile(gpio);

    uint32_t cfg = ((pull_up ? CFG_PULL_UP : CFG_PULL_NONE) << CFG_PULL_SHIFT) |
                   (0u << CFG_FUNC_SHIFT) |
                   (1u << CFG_DRV_SHIFT);
    /* CFG_OE не ставим — это вход. */
    mmio_write32(t + TLMM_GPIO_CFG, cfg);
    dsb_sy();
}

void tlmm_gpio_set(int gpio, int value) {
    if (gpio < 0 || gpio >= TLMM_MAX_GPIO) return;
    mmio_write32(gpio_tile(gpio) + TLMM_GPIO_IN_OUT, value ? IN_OUT_OUT : 0);
    dsb_sy();
}

int tlmm_gpio_get(int gpio) {
    if (gpio < 0 || gpio >= TLMM_MAX_GPIO) return 0;
    return (mmio_read32(gpio_tile(gpio) + TLMM_GPIO_IN_OUT) & IN_OUT_IN) ? 1 : 0;
}
