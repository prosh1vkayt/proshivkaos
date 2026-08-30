/* arch/arm64/boards/mido.h — Xiaomi Redmi Note 4 / 4X, кодовое имя "mido".
 * Snapdragon 625 (Qualcomm MSM8953), 8 x Cortex-A53, экран 1080x1920.
 *
 * ВСЕ значения ниже сняты с ЖИВОГО устройства через adb, а не взяты из
 * чужих деревьев и не выведены по аналогии. Источники указаны у каждого
 * блока. Там, где значение проверить не удалось, это прямо сказано.
 *
 * Напоминание: с появлением разбора device tree (arch/arm64/fdt.c) эти
 * константы стали ЗАПАСНЫМ вариантом. Сначала система читает дерево,
 * которое передал загрузчик, и лишь потом смотрит сюда.
 */
#ifndef ARCH_ARM64_BOARD_MIDO_H
#define ARCH_ARM64_BOARD_MIDO_H

#define BOARD_NAME "XIAOMI MIDO"

/* ---------------- Последовательный порт ----------------
 * Подтверждено трижды: узел serial@78af000 (compatible
 * "qcom,msm-lsuart-v14") в живом дереве устройств, строка
 * earlycon=msm_hsl_uart,0x78af000 в /proc/cmdline работающего Android и
 * та же строка в стоковом boot.img.
 *
 * Физически выведен на разъём наушников и требует кабеля с делителем
 * напряжения. Без него вывод есть, но увидеть его нечем.
 *
 * Второй порт, uart@78b0000 ("qcom,msm-hsuart-v14"), — не консоль: на
 * телефонах этой линейки он уходит на модуль Bluetooth.
 */
#define BOARD_UART_KIND  UART_KIND_MSM
#define BOARD_UART_BASE  0x078AF000UL

/* ---------------- Экран ----------------
 * Загрузчик оставляет работающий фреймбуфер: ядро Linux на этом аппарате
 * печатает при старте "mdss_mdp_probe: bootloader display is on" и
 * "dsi_panel_device_register: Continuous splash enabled". Значит,
 * инициализировать дисплей не нужно — достаточно писать по этому адресу.
 *
 * Адрес и размер: узел reserved-memory/splash_region@0x90001000 с меткой
 * "cont_splash_mem", размер 0x13FF000 — прочитано из живого дерева.
 * Разрешение и шаг строки: /sys/class/graphics/fb0 на работающем
 * устройстве отдаёт 1080x1920, stride 4352, bits_per_pixel 32.
 *
 * ВНИМАНИЕ, ЕДИНСТВЕННОЕ НЕПРОВЕРЕННОЕ МЕСТО. Шаг 4352 и 32 бита — это
 * то, что настроил себе драйвер Linux. Что именно оставил ПОСЛЕ СЕБЯ
 * загрузчик, проверить с работающего Android нельзя: /dev/mem в этом ядре
 * отключён (CONFIG_DEVMEM не задан), а regmap для GCC не экспортирован.
 * Выбран вариант Linux как наиболее вероятный: непрерывный сплеш означает,
 * что драйвер подхватывает уже настроенный тракт, а не перенастраивает его
 * с нуля.
 *
 * Если картинка появится, но будет искажена, менять надо ровно эти две
 * строки, и симптом подскажет, как именно:
 *   изображение "едет" по диагонали  -> не тот шаг строки; попробовать
 *                                       (1080 * 3) = 3240 и формат RGB24;
 *   цвета перепутаны, геометрия цела -> не тот формат; оставить шаг,
 *                                       сменить XRGB32 на RGB24.
 */
#define BOARD_HAS_STATIC_FB 1
#define BOARD_FB_ADDR    0x90001000UL
#define BOARD_FB_WIDTH   1080
#define BOARD_FB_HEIGHT  1920
#define BOARD_FB_STRIDE  4352            /* байт на строку, из fb0/stride  */
#define BOARD_FB_FORMAT  GFXFB_FMT_XRGB32

/* ---------------- Тактирование (GCC) ----------------
 * Узел qcom,gcc@1800000, compatible "qcom,gcc-8953", размер 0x80000 —
 * прочитано из живого дерева.
 */
#define BOARD_GCC_BASE   0x01800000UL

/* ---------------- Контроллер выводов (TLMM) ----------------
 * Узел pinctrl@1000000, compatible "qcom,msm8953-pinctrl", размер
 * 0x300000 — из живого дерева.
 */
#define BOARD_TLMM_BASE  0x01000000UL

/* ---------------- Шина I2C тачскрина ----------------
 * Узел i2c@78b7000, compatible "qcom,i2c-msm-v2", размер 0x600.
 * qcom,clk-freq-in  = 19200000 (тактовая ядра QUP, идёт напрямую от XO —
 *                     подтверждено через /sys/kernel/debug/clk: parent
 *                     blsp1_qup3_i2c_apps_clk_src, исток xo_clk_src)
 * qcom,clk-freq-out =   400000 (скорость шины, fast mode)
 *
 * На шине висят четыре возможных контроллера сенсора — focaltech@38,
 * goodix_ts@14, imagis@50, synaptics@4b. Установлен один, какой именно
 * решается на заводе; в этом экземпляре, по /proc/bus/input/devices,
 * работает ft5435_ts на 3-0038.
 */
#define BOARD_I2C_TS_BASE      0x078B7000UL
#define BOARD_I2C_TS_CORE_HZ   19200000
#define BOARD_I2C_TS_BUS_HZ    400000

/* Ветвь тактов именно этой шины: BLSP1 QUP3. Смещения — в gcc_msm8953.c. */
#define BOARD_I2C_TS_QUP_INDEX 3

/* ---------------- Тачскрин ----------------
 * Узел focaltech@38 на шине выше. Всё прочитано из живого дерева:
 *   compatible               = "focaltech,5435"
 *   reg                      = 0x38
 *   focaltech,irq-gpio       = <&tlmm 65 0x2008>
 *   focaltech,reset-gpio     = <&tlmm 64 0>
 *   focaltech,display-coords = <0 0 1080 1920>
 *   focaltech,num-max-touches = 10
 *   focaltech,hard-reset-delay-ms = 200
 *   focaltech,soft-reset-delay-ms = 200
 */
#define BOARD_TS_I2C_ADDR      0x38
#define BOARD_TS_IRQ_GPIO      65
#define BOARD_TS_RESET_GPIO    64
#define BOARD_TS_MAX_X         1080
#define BOARD_TS_MAX_Y         1920
#define BOARD_TS_MAX_TOUCHES   10
#define BOARD_TS_RESET_DELAY_MS 200

#endif
