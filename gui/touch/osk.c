/* gui/touch/osk.c — экранная клавиатура. */
#include "osk.h"
#include "touch_theme.h"
#include "hal_input.h"

/* Служебные коды клавиш. Обычные клавиши несут свой ASCII, эти три —
 * значения вне ASCII, чтобы не столкнуться с печатными символами. */
#define K_LAYER  0x101   /* переключить раскладку букв/цифр */
#define K_HIDE   0x102   /* убрать клавиатуру                */

#define MAX_ROWS  4
#define MAX_KEYS 12

typedef struct {
    const char *label;
    int         code;    /* ASCII или K_* */
    int         weight;  /* относительная ширина: 1 — обычная клавиша */
} osk_key_t;

typedef struct {
    osk_key_t keys[MAX_KEYS];
    int count;
} osk_row_t;

/* Раскладка букв. Строчных глифов в шрифте нет (см. gui/font8x8.c), и
 * это осознанное оформление системы — поэтому Shift не нужен, а
 * освободившееся место отдано под более крупные полезные клавиши. */
static const osk_row_t g_letters[MAX_ROWS] = {
    {{{"Q",'q',1},{"W",'w',1},{"E",'e',1},{"R",'r',1},{"T",'t',1},
      {"Y",'y',1},{"U",'u',1},{"I",'i',1},{"O",'o',1},{"P",'p',1}}, 10},
    {{{"A",'a',1},{"S",'s',1},{"D",'d',1},{"F",'f',1},{"G",'g',1},
      {"H",'h',1},{"J",'j',1},{"K",'k',1},{"L",'l',1}}, 9},
    {{{"Z",'z',1},{"X",'x',1},{"C",'c',1},{"V",'v',1},{"B",'b',1},
      {"N",'n',1},{"M",'m',1},{"-",'-',1},{"<-",'\b',2}}, 9},
    {{{"123",K_LAYER,2},{"/",'/',1},{".",'.',1},
      {" ",' ',5},{"ENT",'\n',2},{"V",K_HIDE,1}}, 6}
};

/* Цифры и символы — всё, что нужно для командной строки: пути, флаги,
 * перенаправления. */
static const osk_row_t g_symbols[MAX_ROWS] = {
    {{{"1",'1',1},{"2",'2',1},{"3",'3',1},{"4",'4',1},{"5",'5',1},
      {"6",'6',1},{"7",'7',1},{"8",'8',1},{"9",'9',1},{"0",'0',1}}, 10},
    {{{"-",'-',1},{"_",'_',1},{"/",'/',1},{":",':',1},{";",';',1},
      {"(",'(',1},{")",')',1},{"$",'$',1},{"&",'&',1},{"@",'@',1}}, 10},
    {{{"*",'*',1},{"\"",'"',1},{"'",'\'',1},{",",',',1},{"?",'?',1},
      {"!",'!',1},{"~",'~',1},{"=",'=',1},{"<-",'\b',2}}, 9},
    {{{"ABC",K_LAYER,2},{".",'.',1},{"#",'#',1},
      {" ",' ',5},{"ENT",'\n',2},{"V",K_HIDE,1}}, 6}
};

static int g_layer   = 0;   /* 0 — буквы, 1 — символы */
static int g_visible = 0;

static int g_x, g_y, g_w, g_h;
static int g_key_h, g_row_gap;

/* Индекс нажатой сейчас клавиши (строка*MAX_KEYS + позиция), -1 — нет. */
static int g_pressed = -1;

static const osk_row_t *current_rows(void) {
    return g_layer ? g_symbols : g_letters;
}

void osk_init(void) {
    g_layer = 0;
    g_visible = 0;
    g_pressed = -1;
}

int osk_height(void) {
    if (!g_visible) return 0;

    /* Клавиша ровно в минимальную тач-цель по высоте — меньше делать
       нельзя, иначе набор превращается в лотерею. */
    int key_h = TM.touch;
    int gap   = TM.gap / 2;
    return MAX_ROWS * key_h + (MAX_ROWS + 1) * gap;
}

void osk_layout(int x, int y, int w) {
    g_x = x;
    g_w = w;
    g_key_h  = TM.touch;
    g_row_gap = TM.gap / 2;
    g_h = MAX_ROWS * g_key_h + (MAX_ROWS + 1) * g_row_gap;
    g_y = y;
}

/* Геометрия одной клавиши. Ширина считается по весам: сумма весов в ряду
 * делит доступную ширину, поэтому пробел (вес 5) сам растягивается на
 * пять обычных клавиш при любом разрешении экрана. */
static void key_rect(int row, int idx, int *x, int *y, int *w, int *h) {
    const osk_row_t *rows = current_rows();
    const osk_row_t *r = &rows[row];

    int total_weight = 0;
    for (int i = 0; i < r->count; i++) total_weight += r->keys[i].weight;

    int gap = TM.gap / 3;
    if (gap < 1) gap = 1;

    int avail = g_w - TM.pad * 2 - gap * (r->count - 1);
    int unit  = avail / total_weight;

    int offset = 0;
    for (int i = 0; i < idx; i++)
        offset += rows[row].keys[i].weight * unit + gap;

    *x = g_x + TM.pad + offset;
    *y = g_y + g_row_gap + row * (g_key_h + g_row_gap);
    *w = r->keys[idx].weight * unit;
    *h = g_key_h;
}

void osk_render(void) {
    if (!g_visible) return;

    /* Подложка: клавиатура должна визуально отделяться от приложения,
       иначе символы клавиш сливаются с текстом под ними. */
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_SURFACE);
    hal_gfx_fill_rect(g_x, g_y, g_w, 1, GFX_UI_DIVIDER);

    const osk_row_t *rows = current_rows();

    for (int row = 0; row < MAX_ROWS; row++) {
        for (int i = 0; i < rows[row].count; i++) {
            int x, y, w, h;
            key_rect(row, i, &x, &y, &w, &h);

            int is_pressed = (g_pressed == row * MAX_KEYS + i);
            int code = rows[row].keys[i].code;

            /* Служебные клавиши другого цвета — так их находишь глазом,
               не вчитываясь в подписи. */
            uint8_t top, bot;
            if (is_pressed) {
                top = GFX_UI_KEY_DOWN; bot = GFX_UI_KEY;
            } else if (code == '\n') {
                top = GFX_UI_ACCENT;   bot = GFX_UI_ACCENT_DARK;
            } else if (code == K_LAYER || code == K_HIDE || code == '\b') {
                top = GFX_UI_SURFACE_2; bot = GFX_UI_SURFACE;
            } else {
                top = GFX_UI_KEY;      bot = GFX_UI_SURFACE;
            }

            hal_gfx_draw_glossy_button(x, y, w, h, top, bot, GFX_UI_DIVIDER, TM.radius);

            const char *label = rows[row].keys[i].label;
            int ls = (code == K_LAYER || code == K_HIDE) ? TM.scale_small : TM.scale;
            int ty = y + (h - FONT_H * ls) / 2;
            hal_gfx_draw_string_centered(x, ty, w, label, GFX_UI_TEXT, ls);
        }
    }
}

int osk_contains(int x, int y) {
    if (!g_visible) return 0;
    return touch_hit(x, y, g_x, g_y, g_w, g_h);
}

/* Найти клавишу под пальцем. */
static int find_key(int px, int py, int *out_row, int *out_idx) {
    const osk_row_t *rows = current_rows();

    for (int row = 0; row < MAX_ROWS; row++) {
        for (int i = 0; i < rows[row].count; i++) {
            int x, y, w, h;
            key_rect(row, i, &x, &y, &w, &h);
            if (touch_hit(px, py, x, y, w, h)) {
                *out_row = row;
                *out_idx = i;
                return 1;
            }
        }
    }
    return 0;
}

int osk_handle_pointer(int type, int x, int y) {
    if (!g_visible) return 0;

    int row, idx;

    if (type == HAL_EV_POINTER_DOWN) {
        if (find_key(x, y, &row, &idx))
            g_pressed = row * MAX_KEYS + idx;
        return 0;
    }

    if (type == HAL_EV_POINTER_MOVE) {
        /* Палец уехал с клавиши — снимаем подсветку, но нажатие не
           отменяем совсем: вернётся обратно, снова подсветится. */
        if (find_key(x, y, &row, &idx)) g_pressed = row * MAX_KEYS + idx;
        else                            g_pressed = -1;
        return 0;
    }

    if (type == HAL_EV_POINTER_UP) {
        int was = g_pressed;
        g_pressed = -1;
        if (was < 0) return 0;

        /* Символ выдаём только если палец отпущен НА ТОЙ ЖЕ клавише. */
        if (!find_key(x, y, &row, &idx)) return 0;
        if (row * MAX_KEYS + idx != was) return 0;

        int code = current_rows()[row].keys[idx].code;

        if (code == K_LAYER) { g_layer = !g_layer; return 0; }
        if (code == K_HIDE)  { g_visible = 0;      return 0; }
        return code;
    }

    return 0;
}

void osk_set_visible(int visible) {
    g_visible = visible;
    if (!visible) g_pressed = -1;
}

int osk_visible(void) { return g_visible; }
