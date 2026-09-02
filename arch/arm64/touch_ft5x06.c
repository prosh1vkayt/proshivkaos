/* arch/arm64/touch_ft5x06.c — тачскрин FocalTech FT5435 (семейство FT5x06).
 *
 * Что это за микросхема. FT5x06 — ёмкостный контроллер сенсора: он сам
 * сканирует матрицу, сам распознаёт касания и отдаёт готовый список точек.
 * Процессору остаётся прочитать по I2C короткий пакет. Никакой обработки
 * сигнала на нашей стороне не нужно — в этом смысле драйвер простой.
 *
 * Конкретный экземпляр — FT5435, подтверждён на живом устройстве: узел
 * focaltech@38 с compatible "focaltech,5435" и работающий драйвер
 * ft5435_ts в /proc/bus/input/devices.
 *
 * Регистровая карта у всего семейства общая:
 *
 *   0x00  режим работы (нас интересует только рабочий, значение 0)
 *   0x01  распознанный жест (аппаратные жесты не используем)
 *   0x02  сколько точек касания сейчас на экране, младшие четыре бита
 *   0x03  начало массива точек, по шесть байт на точку:
 *           +0  [7:6] событие, [3:0] старшие биты X
 *           +1  младшие биты X
 *           +2  [7:4] номер пальца, [3:0] старшие биты Y
 *           +3  младшие биты Y
 *           +4  сила нажатия
 *           +5  площадь пятна
 *   0xA3  идентификатор семейства (у FT5435 это 0x54)
 *   0xA6  версия прошивки контроллера
 *
 * Координаты приходят СРАЗУ В ПИКСЕЛЯХ панели (0..1079, 0..1919) — это
 * приятное отличие от virtio-tablet в эмуляторе, который отдаёт их в своей
 * абстрактной сетке 0..32767 и требует масштабирования.
 *
 * Прерывание от контроллера (вывод 65) не используется как прерывание:
 * в системе нет обработчиков, всё опрашивается в главном цикле. Но сама
 * линия полезна и в опросном режиме — она показывает, есть ли вообще
 * новые данные, и позволяет не дёргать шину впустую.
 */
#include "arm64.h"
#include "boards/board.h"
#include "hal_time.h"

/* Регистры контроллера */
#define FT_REG_MODE         0x00
#define FT_REG_TD_STATUS    0x02
#define FT_REG_TOUCH_BASE   0x03
#define FT_REG_CHIP_ID      0xA3
#define FT_REG_FW_VERSION   0xA6
#define FT_REG_VENDOR_ID    0xA8

#define FT_CHIP_ID_FT5435   0x54    /* совпадает с focaltech,family-id из дерева */

/* Событие в старших двух битах первого байта точки */
#define FT_EVENT_DOWN       0
#define FT_EVENT_UP         1
#define FT_EVENT_CONTACT    2

#define FT_BYTES_PER_TOUCH  6
#define FT_HEADER_BYTES     3       /* режим, жест, число точек */

static uint64_t g_i2c_base = 0;
static int      g_ready    = 0;

/* Последнее известное состояние — из него собираются события для hal_input. */
static int g_x = 0, g_y = 0, g_pressed = 0;

/* Аппаратный сброс контроллера.
 *
 * Нужен обязательно: загрузчик оставляет микросхему в неопределённом
 * состоянии, а на некоторых экземплярах — в режиме пониженного потребления,
 * из которого она не отвечает на I2C вовсе. Задержки взяты из дерева
 * устройств (focaltech,hard-reset-delay-ms = 200). */
static void ft5x06_reset(void) {
    tlmm_gpio_output(BOARD_TS_RESET_GPIO, 1);
    hal_time_delay_ms(5);

    tlmm_gpio_set(BOARD_TS_RESET_GPIO, 0);
    hal_time_delay_ms(20);              /* удержание в сбросе */

    tlmm_gpio_set(BOARD_TS_RESET_GPIO, 1);
    hal_time_delay_ms(BOARD_TS_RESET_DELAY_MS);   /* прошивка стартует */
}

int ft5x06_init(void) {
    g_ready = 0;
    g_i2c_base = BOARD_I2C_TS_BASE;

    /* 1. Такт шины. Загрузчик его не включает — он тачскрином не
          пользуется, — поэтому без этого шага блок QUP просто не отвечает. */
    early_con_puts("TS: takt shiny QUP");
    early_con_hex((uint64_t)BOARD_I2C_TS_QUP_INDEX);
    int clk = gcc_enable_blsp1_qup_i2c(BOARD_I2C_TS_QUP_INDEX);
    early_con_puts(clk ? " OK CBCR " : " NE POSHYOL CBCR ");
    early_con_hex((uint64_t)gcc_qup_i2c_cbcr(BOARD_I2C_TS_QUP_INDEX));
    early_con_puts("\n");

    if (!clk) {
        uart_write("ts: ne udalos vklyuchit takt shiny I2C\n");
        return 0;
    }

    /* 2. Линия прерывания — вход с подтяжкой вверх: сигнал активен низким
          уровнем, и в покое линию должно что-то удерживать в единице. */
    tlmm_gpio_input(BOARD_TS_IRQ_GPIO, 1);

    /* 3. Контроллер шины. */
    int bus = i2c_qup_init(g_i2c_base, BOARD_I2C_TS_CORE_HZ, BOARD_I2C_TS_BUS_HZ);
    early_con_puts(bus ? "TS: shina I2C podnyata\n" : "TS: shina I2C NE podnyalas\n");
    if (!bus) {
        uart_write("ts: I2C ne inicializirovan\n");
        return 0;
    }

    /* 4. Сброс микросхемы. */
    ft5x06_reset();

    /* 5. Проверка, что на шине именно то, что мы ожидаем. Это же и первая
          настоящая проверка, что вся цепочка такт-шина-выводы собрана
          верно: если ответ пришёл и он осмысленный, значит работает всё. */
    early_con_puts("TS: sbros vypolnen, sprashivaem 0x38\n");

    uint8_t chip_id = 0;
    if (!i2c_qup_read_regs(g_i2c_base, BOARD_TS_I2C_ADDR, FT_REG_CHIP_ID, &chip_id, 1)) {
        early_con_color("TS: kontroller ne otvechaet\n", 255, 180, 0);
        uart_write("ts: kontroller ne otvechaet po adresu 0x38\n");
        return 0;
    }

    early_con_puts("TS: otvetil, id ");
    early_con_hex((uint64_t)chip_id);
    early_con_puts("\n");

    uint8_t fw = 0;
    i2c_qup_read_regs(g_i2c_base, BOARD_TS_I2C_ADDR, FT_REG_FW_VERSION, &fw, 1);

    uart_write("ts: FocalTech id=");
    uart_write_hex(chip_id);
    uart_write(" fw=");
    uart_write_hex(fw);

    if (chip_id != FT_CHIP_ID_FT5435) {
        /* Не отказываемся работать: на этой плате завод ставил контроллеры
           четырёх разных производителей, и у родственных микросхем
           FocalTech протокол тот же самый при другом идентификаторе.
           Просто сообщаем, что встретили не то, что ожидали. */
        uart_write(" (ozhidalsya 0x54, protokol tot zhe — probuem)");
    }
    uart_putc('\n');

    /* 6. Рабочий режим. */
    i2c_qup_write_reg(g_i2c_base, BOARD_TS_I2C_ADDR, FT_REG_MODE, 0x00);

    g_ready = 1;
    return 1;
}

int ft5x06_ready(void) { return g_ready; }

/* Опрос контроллера. Возвращает 1, если состояние касания изменилось. */
int ft5x06_poll(int *x, int *y, int *pressed) {
    if (!g_ready) return 0;

    /* Линия прерывания активна низким уровнем. Единица означает «новых
       данных нет» — в этом случае шину не трогаем вовсе. Экономия здесь не
       ради тактов процессора, а ради самой шины: лишний обмен на 400 кГц
       занимает её на полтора миллисекунда и мешает следующему опросу. */
    if (tlmm_gpio_get(BOARD_TS_IRQ_GPIO)) {
        /* Ничего не пришло. Но если палец только что подняли, контроллер
           мог не выставить прерывание на событие отпускания — состояние
           отдаём как есть. */
        return 0;
    }

    /* Читаем заголовок и все точки одним обменом: раздельные чтения дали бы
       рассогласованную картину, если палец сдвинулся между ними. */
    uint8_t buf[FT_HEADER_BYTES + FT_BYTES_PER_TOUCH * BOARD_TS_MAX_TOUCHES];
    int want = FT_HEADER_BYTES + FT_BYTES_PER_TOUCH * BOARD_TS_MAX_TOUCHES;

    if (!i2c_qup_read_regs(g_i2c_base, BOARD_TS_I2C_ADDR, FT_REG_MODE, buf, want))
        return 0;

    int points = buf[FT_REG_TD_STATUS] & 0x0F;
    if (points > BOARD_TS_MAX_TOUCHES) points = BOARD_TS_MAX_TOUCHES;

    if (points == 0) {
        /* Пальцев на экране нет. Сообщаем об отпускании ровно один раз. */
        if (g_pressed) {
            g_pressed = 0;
            *x = g_x; *y = g_y; *pressed = 0;
            return 1;
        }
        return 0;
    }

    /* Берём первую точку. Мультитач контроллер отдаёт, но интерфейсу он
       пока не нужен: модель ввода в hal_input.h описывает один указатель.
       Расширять её имеет смысл вместе с жестами вроде щипка. */
    const uint8_t *p = &buf[FT_REG_TOUCH_BASE];

    int event = (p[0] >> 6) & 0x03;
    int nx = ((int)(p[0] & 0x0F) << 8) | p[1];
    int ny = ((int)(p[2] & 0x0F) << 8) | p[3];

    /* Защита от мусора: если шина сбойнула, координаты улетают за пределы
       панели, и без проверки такое касание увело бы интерфейс неизвестно
       куда. */
    if (nx < 0 || nx >= BOARD_TS_MAX_X || ny < 0 || ny >= BOARD_TS_MAX_Y)
        return 0;

    g_x = nx;
    g_y = ny;

    int now_pressed = (event != FT_EVENT_UP);

    int changed = (now_pressed != g_pressed) || now_pressed;
    g_pressed = now_pressed;

    *x = g_x; *y = g_y; *pressed = g_pressed;
    return changed;
}

/* Диапазон координат — для масштабирования в пиксели экрана.
 * У этой микросхемы он совпадает с разрешением панели один в один. */
void ft5x06_range(int *max_x, int *max_y) {
    *max_x = BOARD_TS_MAX_X;
    *max_y = BOARD_TS_MAX_Y;
}
