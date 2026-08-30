/* hal/hal_input_arm64.c — hal_input_* поверх virtio-input и UART.
 *
 * Парный файл к hal/hal_input_x86.c. Задача у него зеркальная: там надо
 * было СОБРАТЬ абсолютную точку из относительных смещений мыши, здесь
 * абсолютная точка приходит готовой, но её надо перевести из сетки
 * сенсора (обычно 0..32767) в пиксели экрана.
 *
 * Третий источник — настоящий тачскрин телефона (FocalTech FT5x06 на шине
 * I2C, см. arch/arm64/touch_ft5x06.c). Он подключается только в сборках под
 * платы, где он есть; в остальных вместо него работают слабые заглушки
 * ниже, и остальной код разницы не замечает.
 *
 * Второй источник ввода — сам последовательный порт. Это не костыль, а
 * ровно то, как работают с телефоном на ранних этапах порта (и как это
 * описано в docs/ARM64_PLAN.md п.4): экранной клавиатуры может не быть,
 * тачскрин может ещё не завестись, а UART работает всегда. Поэтому всё,
 * что прилетает в serial-консоль, попадает в систему как нажатия клавиш.
 */
#include "hal_input.h"
#include "arm64.h"

int uart_getc(void);

/* Слабые заглушки драйвера тачскрина. Их перекрывают настоящие функции из
 * arch/arm64/touch_ft5x06.c, если он попал в сборку. Так слой ввода
 * обходится без единого #ifdef по плате. */
__attribute__((weak)) int  ft5x06_init(void)  { return 0; }
__attribute__((weak)) int  ft5x06_ready(void) { return 0; }
__attribute__((weak)) int  ft5x06_poll(int *x, int *y, int *pressed) {
    (void)x; (void)y; (void)pressed; return 0;
}
__attribute__((weak)) void ft5x06_range(int *max_x, int *max_y) {
    (void)max_x; (void)max_y;
}

/* Коды из linux/input-event-codes.h — virtio-input использует именно их,
 * ту же нумерацию, что и настоящий драйвер тачскрина в Linux. */
#define EV_SYN        0x00
#define EV_KEY        0x01
#define EV_REL        0x02
#define EV_ABS        0x03

#define SYN_REPORT    0x00
#define REL_X         0x00
#define REL_Y         0x01
#define ABS_X         0x00
#define ABS_Y         0x01
#define BTN_LEFT      0x110
#define BTN_TOUCH     0x14a

#define QUEUE_SIZE 64

static hal_input_event_t g_queue[QUEUE_SIZE];
static int g_head = 0, g_tail = 0;

static int g_screen_w = 480, g_screen_h = 960;

/* Сетка сенсора. Пока virtio_input_abs_range() не сказал иное — значение
 * по умолчанию virtio-tablet. */
static int g_abs_max_x = 32767, g_abs_max_y = 32767;
static int g_has_abs = 0;

/* Состояние, накапливаемое между двумя EV_SYN. Устройство шлёт координату
 * по X, координату по Y и состояние касания РАЗНЫМИ событиями, а
 * "кадром" их делает EV_SYN/SYN_REPORT — только по нему можно считать,
 * что точка целиком описана. Без этой группировки палец на диагональном
 * свайпе двигался бы ступеньками: сначала по X, потом по Y. */
static int g_x = 0, g_y = 0;
static int g_pressed = 0;
static int g_pending_pressed = 0;
static int g_dirty = 0;

static void push(int type, int x, int y, int pressed, int key) {
    int next = (g_head + 1) % QUEUE_SIZE;
    if (next == g_tail) return;   /* переполнение — теряем самое новое */

    g_queue[g_head].type    = type;
    g_queue[g_head].x       = x;
    g_queue[g_head].y       = y;
    g_queue[g_head].pressed = pressed;
    g_queue[g_head].key     = key;
    g_head = next;
}

static int g_device_count = 0;
static int g_touch_ok = 0;      /* поднялся настоящий тачскрин телефона */

void hal_input_init(void) {
    virtio_input_init();
    g_device_count = virtio_input_device_count();

    int mx, my;
    if (virtio_input_abs_range(&mx, &my)) {
        g_abs_max_x = mx;
        g_abs_max_y = my;
        g_has_abs = 1;
    }

    /* Настоящий тачскрин пробуем поднять только если virtio ничего не дал.
       Под эмулятором virtio есть всегда, и лезть на несуществующую шину
       I2C незачем; на телефоне всё ровно наоборот. */
    if (g_device_count == 0) {
        g_touch_ok = ft5x06_init();
        if (g_touch_ok) {
            ft5x06_range(&g_abs_max_x, &g_abs_max_y);
            g_has_abs = 1;
        }
    }

    g_head = g_tail = 0;
    g_pressed = g_pending_pressed = 0;
    g_x = g_screen_w / 2;
    g_y = g_screen_h / 2;
}

void hal_input_set_screen(int w, int h) {
    g_screen_w = w;
    g_screen_h = h;
    g_x = w / 2;
    g_y = h / 2;
}

/* Тачскрин курсора не имеет — рисовать стрелку посреди экрана телефона
 * незачем. Если абсолютного устройства не нашлось (значит, работаем с
 * относительной мышью), курсор всё-таки нужен. */
int hal_input_has_cursor(void) { return !g_has_abs; }

/* На реальном телефоне virtio-устройств нет (они существуют только внутри
 * QEMU), а драйвера тачскрина по I2C ещё нет — значит, ввода нет вовсе. */
int hal_input_available(void) { return g_device_count > 0 || g_touch_ok; }

void hal_input_pointer_pos(int *x, int *y) {
    *x = g_x;
    *y = g_y;
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Linux-коды клавиш основного блока совпадают со скан-кодами PS/2 Set 1 —
 * историческое наследство, благодаря которому эта таблица почти дословно
 * повторяет ту, что лежит в arch/x86/keyboard.c. */
static const char keycode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0 /*ctrl*/,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0 /*lshift*/,'\\','z','x','c','v','b','n','m',',','.','/',
    0 /*rshift*/,'*',
    0 /*alt*/,' ',
};

/* Завершение "кадра" событий: решаем, что именно произошло с указателем. */
static void flush_pointer_frame(void) {
    if (g_pending_pressed && !g_pressed) {
        g_pressed = 1;
        push(HAL_EV_POINTER_DOWN, g_x, g_y, 1, 0);
    } else if (!g_pending_pressed && g_pressed) {
        g_pressed = 0;
        push(HAL_EV_POINTER_UP, g_x, g_y, 0, 0);
    } else if (g_dirty) {
        push(HAL_EV_POINTER_MOVE, g_x, g_y, g_pressed, 0);
    }
    g_dirty = 0;
}

static void pump_hardware(void) {
    /* --- Настоящий тачскрин телефона --- */
    if (g_touch_ok) {
        int tx, ty, tp;
        if (ft5x06_poll(&tx, &ty, &tp)) {
            /* Контроллер отдаёт координаты сразу в пикселях панели, но
               экран мог оказаться другого размера (например, загрузчик
               оставил фреймбуфер меньше панели) — поэтому масштабируем
               через тот же путь, что и абсолютные координаты virtio. */
            g_x = clamp((int)(((int64_t)tx * (g_screen_w - 1)) / g_abs_max_x),
                        0, g_screen_w - 1);
            g_y = clamp((int)(((int64_t)ty * (g_screen_h - 1)) / g_abs_max_y),
                        0, g_screen_h - 1);
            g_pending_pressed = tp;
            g_dirty = 1;
            flush_pointer_frame();
        }
    }

    /* --- Клавиши из последовательного порта --- */
    int c = uart_getc();
    if (c >= 0) {
        if (c == 0x7F) c = '\b';     /* терминалы шлют DEL вместо Backspace */
        if (c == '\r') c = '\n';
        push(HAL_EV_KEY, g_x, g_y, g_pressed, c);
    }

    /* --- События virtio-input --- */
    vinput_event_t ev;
    while (virtio_input_poll(&ev)) {
        switch (ev.type) {
            case EV_ABS:
                /* Масштабирование сетки сенсора в пиксели. Умножаем ДО
                   деления: обратный порядок на целых числах схлопнул бы
                   почти любую координату в ноль. */
                if (ev.code == ABS_X) {
                    g_x = clamp((int)(((int64_t)ev.value * (g_screen_w - 1)) / g_abs_max_x),
                                0, g_screen_w - 1);
                    g_dirty = 1;
                } else if (ev.code == ABS_Y) {
                    g_y = clamp((int)(((int64_t)ev.value * (g_screen_h - 1)) / g_abs_max_y),
                                0, g_screen_h - 1);
                    g_dirty = 1;
                }
                break;

            case EV_REL:
                /* Запасной путь: если вместо тачскрина подключили обычную
                   virtio-мышь, ведём координату сами — как на x86. */
                if (ev.code == REL_X) { g_x = clamp(g_x + ev.value, 0, g_screen_w - 1); g_dirty = 1; }
                if (ev.code == REL_Y) { g_y = clamp(g_y + ev.value, 0, g_screen_h - 1); g_dirty = 1; }
                break;

            case EV_KEY:
                if (ev.code == BTN_TOUCH || ev.code == BTN_LEFT) {
                    g_pending_pressed = (ev.value != 0);
                } else if (ev.value == 1 && ev.code < 128) {
                    /* value: 1 — нажатие, 0 — отпускание, 2 — автоповтор.
                       Реагируем только на нажатие, как keyboard.c на x86. */
                    char ch = keycode_to_ascii[ev.code];
                    if (ch) push(HAL_EV_KEY, g_x, g_y, g_pressed, (int)(unsigned char)ch);
                }
                break;

            case EV_SYN:
                if (ev.code == SYN_REPORT)
                    flush_pointer_frame();
                break;

            default:
                break;
        }
    }
}

int hal_input_poll(hal_input_event_t *ev) {
    if (g_head == g_tail)
        pump_hardware();

    if (g_head == g_tail)
        return 0;

    *ev = g_queue[g_tail];
    g_tail = (g_tail + 1) % QUEUE_SIZE;
    return 1;
}
