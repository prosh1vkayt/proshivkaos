/* arch/arm64/boards/mido.h — Xiaomi Redmi Note 4 / 4X, кодовое имя "mido".
 * Snapdragon 625 (Qualcomm MSM8953), 8 x Cortex-A53.
 *
 * Все адреса ниже взяты из МЕЙНЛАЙН-DTS ядра Linux, а не подобраны на
 * глаз — arch/arm64/boot/dts/qcom/msm8953-xiaomi-mido.dts и
 * msm8953.dtsi (проект torvalds/linux). Это самый надёжный источник,
 * который есть без доступа к живому устройству: те же самые адреса
 * использует настоящий Linux-драйвер этого же SoC.
 *
 * НЕ ПРОВЕРЕНО НА ЖЕЛЕЗЕ. Значения верны на бумаге, но между "верно по
 * даташиту" и "работает на конкретном экземпляре платы" всегда есть
 * шаг проверки в реальном QEMU-эмуляторе… которого для msm8953 не
 * существует. Первая же попытка прошивки — и есть эта проверка.
 */
#ifndef ARCH_ARM64_BOARD_MIDO_H
#define ARCH_ARM64_BOARD_MIDO_H

#define BOARD_NAME "XIAOMI MIDO"

/* ---------------- Последовательный порт ----------------
 * Запасное значение: основной источник — device tree, где этот же порт
 * описан узлом serial@78af000 с compatible "qcom,msm-lsuart-v14".
 *
 * Адрес подтверждён из двух независимых источников: узел в дереве и
 * строка earlycon=msm_hsl_uart,0x78af000 в стоковой командной строке
 * Android (её видно, если распаковать стоковый boot.img).
 *
 * Физически порт на этом аппарате выведен на разъём наушников и требует
 * кабеля с делителем напряжения. Без него вывод есть, но увидеть его
 * нечем.
 */
#define BOARD_UART_KIND  UART_KIND_MSM
#define BOARD_UART_BASE  0x078AF000UL

/* ---------------- Готовый фреймбуфер загрузчика ----------------
 * LK уже показал сплеш-логотип и оставил после себя рабочий фреймбуфер —
 * ничего инициализировать не нужно, только начать писать по этому адресу.
 * Источник: msm8953-xiaomi-mido.dts, узел chosen/framebuffer@90001000.
 *
 *   compatible = "simple-framebuffer";
 *   reg        = <0 0x90001000 0 (1920 * 1080 * 3)>;
 *   width      = <1080>;  height = <1920>;
 *   stride     = <(1080 * 3)>;
 *   format     = "r8g8b8";
 *
 * BOARD_HAS_STATIC_FB говорит hal_gfx_arm64.c: не пытаться найти ramfb
 * через fw_cfg (на реальном железе fw_cfg просто не существует), а сразу
 * связать gfxfb с этим адресом в формате GFXFB_FMT_RGB24.
 */
#define BOARD_HAS_STATIC_FB 1
#define BOARD_FB_ADDR    0x90001000UL
#define BOARD_FB_WIDTH   1080
#define BOARD_FB_HEIGHT  1920
#define BOARD_FB_STRIDE  (1080 * 3)     /* r8g8b8: 3 байта на пиксель, без выравнивания строки */
#define BOARD_FB_FORMAT  GFXFB_FMT_RGB24

/* ---------------- Регистры платформы (для дальнейших шагов) ----------------
 * Понадобятся, когда дойдёт очередь до UART/I2C/тачскрина —
 * см. docs/PORT_MIDO.md. Пока НЕ используются ни одним .c-файлом: держим
 * их здесь одним местом, чтобы не расползались по коду произвольными
 * #define вперемешку с логикой.
 *
 * Источник — msm8953.dtsi:
 *   gcc:  clock-controller@1800000 { reg = <0x01800000 0x80000> }
 *   tlmm: pinctrl@1000000          { reg = <0x01000000 0x300000> }
 *   i2c_3 (label) -> i2c@78b7000   { compatible = "qcom,i2c-qup-v2.2.1";
 *                                     reg = <0x078b7000 0x600> }
 * На этой шине висит тачскрин: touchscreen@38, compatible
 * "edt,edt-ft5406" (семейство Focaltech FT5x06), IRQ — TLMM GPIO 65,
 * falling edge.
 *
 * Второй порт, uart@78b0000 (compatible "qcom,msm-hsuart-v14"), — не
 * консоль: на телефонах этой линейки он уходит на модуль Bluetooth.
 * Печатать в него отладку бессмысленно, поэтому в списке приоритетов
 * arch/arm64/platform.c он стоит последним.
 */
#define BOARD_GCC_BASE   0x01800000UL
#define BOARD_TLMM_BASE  0x01000000UL
#define BOARD_I2C3_BASE  0x078B7000UL   /* тачскрин FT5406, адрес 0x38 */

#endif
