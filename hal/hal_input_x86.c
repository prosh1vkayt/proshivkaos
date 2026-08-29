/* hal/hal_input_x86.c — hal_input_* поверх PS/2-мыши и PS/2-клавиатуры.
 *
 * Главная работа этого файла — превратить ОТНОСИТЕЛЬНЫЕ смещения мыши
 * (dx/dy) в АБСОЛЮТНЫЕ координаты указателя, потому что тачскрин на
 * ARM64 отдаёт сразу абсолютную точку, и оконный менеджер должен видеть
 * одну и ту же модель на обеих архитектурах. Положение курсора теперь
 * живёт здесь, а не в gui/wm.c — там ему было не место: на телефоне
 * никакого "положения курсора" между касаниями просто не существует.
 *
 * Заодно тут разбираются фронты нажатия: железо говорит "кнопка сейчас
 * нажата", а интерфейсу нужны события "нажали"/"ведут"/"отпустили".
 */
#include "hal_input.h"
#include "mouse.h"

int keyboard_poll(void);   /* arch/x86/keyboard.c */

#define QUEUE_SIZE 32

static hal_input_event_t g_queue[QUEUE_SIZE];
static int g_head = 0, g_tail = 0;

static int g_screen_w = 320, g_screen_h = 200;
static int g_x = 160, g_y = 100;
static int g_pressed = 0;

static void push(int type, int x, int y, int pressed, int key) {
    int next = (g_head + 1) % QUEUE_SIZE;
    if (next == g_tail) return;   /* очередь переполнена — теряем самое новое
                                     событие; на практике не случается, цикл
                                     разбирает её каждый кадр */
    g_queue[g_head].type    = type;
    g_queue[g_head].x       = x;
    g_queue[g_head].y       = y;
    g_queue[g_head].pressed = pressed;
    g_queue[g_head].key     = key;
    g_head = next;
}

void hal_input_init(void) {
    mouse_init();
    g_head = g_tail = 0;
    g_pressed = 0;
}

void hal_input_set_screen(int w, int h) {
    g_screen_w = w;
    g_screen_h = h;
    g_x = w / 2;
    g_y = h / 2;
}

int hal_input_has_cursor(void) { return 1; }

void hal_input_pointer_pos(int *x, int *y) {
    *x = g_x;
    *y = g_y;
}

/* Опрашивает железо и складывает всё, что нашлось, в очередь. */
static void pump_hardware(void) {
    int c = keyboard_poll();
    if (c != -1)
        push(HAL_EV_KEY, g_x, g_y, g_pressed, c);

    mouse_event_t mev;
    while (mouse_poll(&mev)) {
        int moved = (mev.dx != 0 || mev.dy != 0);

        g_x += mev.dx;
        g_y += mev.dy;
        if (g_x < 0) g_x = 0;
        if (g_y < 0) g_y = 0;
        if (g_x >= g_screen_w) g_x = g_screen_w - 1;
        if (g_y >= g_screen_h) g_y = g_screen_h - 1;

        int now = mev.left ? 1 : 0;

        if (now && !g_pressed) {
            g_pressed = 1;
            push(HAL_EV_POINTER_DOWN, g_x, g_y, 1, 0);
        } else if (!now && g_pressed) {
            g_pressed = 0;
            push(HAL_EV_POINTER_UP, g_x, g_y, 0, 0);
        } else if (moved) {
            push(HAL_EV_POINTER_MOVE, g_x, g_y, g_pressed, 0);
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
