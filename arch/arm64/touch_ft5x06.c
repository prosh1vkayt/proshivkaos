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

/* КАКИМ ПУТЁМ ХОДИМ НА ШИНУ.
 *
 * Путей два: аппаратный контроллер QUP и выдача шины вручную на обычных
 * выводах. Второй медленнее в несколько раз, но не зависит ни от чего,
 * кроме самих выводов. Выбор делается один раз при подъёме — тем, кто
 * реально ответил. */
enum { TS_BUS_NONE = 0, TS_BUS_QUP, TS_BUS_BITBANG };
static int g_bus = TS_BUS_NONE;

static int ts_read(uint8_t reg, uint8_t *out, int len) {
    if (g_bus == TS_BUS_QUP)
        return i2c_qup_read_regs(g_i2c_base, BOARD_TS_I2C_ADDR, reg, out, len);
    if (g_bus == TS_BUS_BITBANG)
        return i2c_bb_read_regs(BOARD_TS_I2C_ADDR, reg, out, len);
    return 0;
}

static int ts_write(uint8_t reg, uint8_t value) {
    if (g_bus == TS_BUS_QUP)
        return i2c_qup_write_reg(g_i2c_base, BOARD_TS_I2C_ADDR, reg, value);
    if (g_bus == TS_BUS_BITBANG)
        return i2c_bb_write_reg(BOARD_TS_I2C_ADDR, reg, value);
    return 0;
}

/* Вернуть выводы шины в состояние обычных входов. Нужно перед ручной
   выдачей: пока вывод отдан контроллеру, мы им не распоряжаемся. */
static void ts_pins_to_gpio(void) {
    tlmm_gpio_input(BOARD_I2C_TS_SDA_GPIO, 0);
    tlmm_gpio_input(BOARD_I2C_TS_SCL_GPIO, 0);
}

static void ts_pins_to_i2c(void) {
    tlmm_gpio_func(BOARD_I2C_TS_SDA_GPIO, BOARD_I2C_TS_PIN_FUNC,
                   BOARD_I2C_TS_PIN_DRIVE, 0);
    tlmm_gpio_func(BOARD_I2C_TS_SCL_GPIO, BOARD_I2C_TS_PIN_FUNC,
                   BOARD_I2C_TS_PIN_DRIVE, 0);
}

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

    /* 3. СНАЧАЛА СМОТРИМ, ЖИВА ЛИ ШИНА.
     *
     * Обе линии I2C подтянуты к питанию внешними резисторами, и питаются
     * они от того же источника, что и сам тачскрин (vcc_i2c). Если оно не
     * подано, подтяжек нет: линии не в единице, и дальше без управления
     * источниками не продвинуться.
     *
     * Читаем уровни как обычные входы без подтяжки. Обе единицы — шина
     * свободна и питание есть. */
    ts_pins_to_gpio();
    hal_time_delay_ms(1);

    early_con_puts("TS: linii shiny SDA=");
    early_con_puts(tlmm_gpio_get(BOARD_I2C_TS_SDA_GPIO) ? "1" : "0");
    early_con_puts(" SCL=");
    early_con_puts(tlmm_gpio_get(BOARD_I2C_TS_SCL_GPIO) ? "1" : "0");
    early_con_puts(" (1 1 = shina svobodna i pitanie est)\n");

    /* 4. Сброс микросхемы. Делается до любого обмена и не требует шины —
          только линию сброса. Загрузчик оставляет контроллер в
          неопределённом состоянии, а иногда и в режиме пониженного
          потребления, из которого он на I2C не отвечает вовсе. */
    ft5x06_reset();
    early_con_puts("TS: sbros vypolnen\n");

    /* 5. РУЧНАЯ ВЫДАЧА ШИНЫ — ПЕРВОЙ.
     *
     * Порядок здесь не случайный. Мы несколько заходов подряд не могли
     * различить два отказа: "контроллер настроен неверно" и "микросхемы
     * на шине нет". Ручная выдача этот вопрос закрывает, потому что не
     * использует ничего, кроме выводов. Ответ по адресу 0x38 означает,
     * что микросхема запитана, адрес верен, и виноват контроллер; пустая
     * шина означает обратное, и настраивать контроллер дальше незачем. */
    int bb_id_ok = 0;
    uint8_t bb_id = 0;

    if (i2c_bb_init(BOARD_I2C_TS_SDA_GPIO, BOARD_I2C_TS_SCL_GPIO, 100000)) {
        i2c_bb_scan();
        bb_id_ok = i2c_bb_read_regs(BOARD_TS_I2C_ADDR, FT_REG_CHIP_ID, &bb_id, 1);
        early_con_puts(bb_id_ok ? "BB: 0x38 otvetil, id " : "BB: 0x38 molchit, id ");
        early_con_hex8(bb_id);
        early_con_puts("\n");
    } else {
        early_con_color("BB: shina ne svobodna — pohozhe net pitaniya\n", 255, 180, 0);
    }

    /* 6. Аппаратный контроллер. Выводы отдаём ему. */
    early_con_puts("TS: vyvody do ");
    early_con_hex32(tlmm_gpio_cfg(BOARD_I2C_TS_SDA_GPIO));
    ts_pins_to_i2c();
    early_con_puts(" posle ");
    early_con_hex32(tlmm_gpio_cfg(BOARD_I2C_TS_SDA_GPIO));
    early_con_puts("\n");

    uint8_t chip_id = 0;
    int qup_id_ok = 0;

    if (i2c_qup_init(g_i2c_base, BOARD_I2C_TS_CORE_HZ, BOARD_I2C_TS_BUS_HZ)) {
        early_con_puts("TS: sprashivaem 0x38 cherez QUP\n");
        g_bus = TS_BUS_QUP;
        qup_id_ok = ts_read(FT_REG_CHIP_ID, &chip_id, 1);
        if (!qup_id_ok) {
            early_con_color("TS: QUP ne poluchil otvet, sostoyanie ", 255, 180, 0);
            early_con_hex32(i2c_qup_last_status());
            early_con_puts("\n");
            i2c_qup_explain(i2c_qup_last_status());
        }
    } else {
        early_con_color("TS: QUP ne podnyalsya\n", 255, 180, 0);
    }

    /* 7. Выбор пути. Аппаратный предпочтительнее — он в разы быстрее, —
          но работающий медленный путь лучше неработающего быстрого. */
    if (qup_id_ok) {
        g_bus = TS_BUS_QUP;
        early_con_puts("TS: rabotaem cherez QUP\n");
    } else if (bb_id_ok) {
        ts_pins_to_gpio();
        i2c_bb_init(BOARD_I2C_TS_SDA_GPIO, BOARD_I2C_TS_SCL_GPIO, 100000);
        g_bus = TS_BUS_BITBANG;
        chip_id = bb_id;
        early_con_color("TS: rabotaem vruchnuyu (QUP ne otvetil)\n", 255, 220, 60);
    } else {
        g_bus = TS_BUS_NONE;
        early_con_color("TS: kontroller ne otvechaet ni odnim putyom\n", 255, 80, 80);
        uart_write("ts: kontroller ne otvechaet po adresu 0x38\n");
        return 0;
    }

    early_con_puts("TS: otvetil, id ");
    early_con_hex8(chip_id);
    early_con_puts("\n");

    uint8_t fw = 0;
    ts_read(FT_REG_FW_VERSION, &fw, 1);

    uart_write("ts: FocalTech id=");
    uart_write_hex(chip_id);
    uart_write(" fw=");
    uart_write_hex(fw);

    if (chip_id != FT_CHIP_ID_FT5435) {
        /* Не отказываемся работать: на этой плате завод ставил контроллеры
           четырёх разных производителей, и у родственных микросхем
           FocalTech протокол тот же самый при другом идентификаторе. */
        uart_write(" (ozhidalsya 0x54, protokol tot zhe — probuem)");
    }
    uart_putc('\n');

    /* 8. Рабочий режим. */
    ts_write(FT_REG_MODE, 0x00);

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

    if (!ts_read(FT_REG_MODE, buf, want))
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
