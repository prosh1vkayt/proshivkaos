/* gui/touch/app_files.c — файловый менеджер поверх RamFS.
 *
 * Тот же самый fs/ramfs.c, что обслуживает команды ls/cd/cat в терминале,
 * только показанный списком, по которому можно ходить пальцем. Никакого
 * отдельного "API файлового менеджера" не понадобилось: ramfs_list_dir()
 * уже отдаёт всё нужное.
 *
 * Прокрутка сделана перетаскиванием (инерции нет — она требует покадровой
 * анимации, а перерисовка у нас событийная), плюс список сам не даёт
 * уехать за свои границы.
 */
#include "touch_app.h"
#include "touch_theme.h"
#include "hal_input.h"
#include "ramfs.h"

#define MODE_LIST   0
#define MODE_VIEWER 1

static int  g_x, g_y, g_w, g_h;
static int  g_row_h;
static int  g_mode = MODE_LIST;
static int  g_scroll = 0;

static char g_cwd[RAMFS_MAX_PATH_LEN];
static char g_view_name[RAMFS_MAX_PATH_LEN];

/* Жест: с какой точки начали тянуть и сколько уже утянули. Нужно, чтобы
 * отличить прокрутку от нажатия на строку. */
static int g_drag_start_y = 0;
static int g_drag_start_scroll = 0;
static int g_drag_distance = 0;
static int g_pressed_row = -1;

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void go_parent(void) {
    int len = str_len(g_cwd);
    int last = 0;
    for (int i = 0; i < len; i++) if (g_cwd[i] == '/') last = i;
    if (last == 0) { str_copy(g_cwd, "/", sizeof(g_cwd)); return; }
    g_cwd[last] = '\0';
}

static void enter_dir(const char *name) {
    int n = str_len(g_cwd);
    if (n == 1 && g_cwd[0] == '/') n = 0;   /* не удваивать слэш в корне */

    char path[RAMFS_MAX_PATH_LEN];
    int pos = 0;
    for (int i = 0; i < n && pos < (int)sizeof(path) - 1; i++) path[pos++] = g_cwd[i];
    if (pos < (int)sizeof(path) - 1) path[pos++] = '/';
    path[pos] = '\0';
    touch_strcat(path, pos, sizeof(path), name);

    str_copy(g_cwd, path, sizeof(g_cwd));
    g_scroll = 0;
}

static void files_init(void) {
    str_copy(g_cwd, "/", sizeof(g_cwd));
    g_mode = MODE_LIST;
    g_scroll = 0;
}

static void files_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
    g_row_h = TM.touch;
}

/* Заголовок: путь + кнопка "вверх". Общий для обоих режимов. */
static void draw_header(const char *title) {
    int hh = TM.touch;
    hal_gfx_fill_rect(g_x, g_y, g_w, hh, GFX_UI_SURFACE_2);
    hal_gfx_fill_rect(g_x, g_y + hh - 1, g_w, 1, GFX_UI_DIVIDER);

    /* Кнопка "наверх" слева */
    int bw = TM.touch;
    touch_draw_button(g_x + TM.pad / 2, g_y + TM.pad / 2,
                       bw - TM.pad, hh - TM.pad, "<", 0,
                       GFX_UI_SURFACE_2, GFX_UI_SURFACE);

    hal_gfx_draw_string_scaled(g_x + bw + TM.pad, g_y + (hh - FONT_H * TM.scale_small) / 2,
                                title, GFX_UI_TEXT, TM.scale_small);
}

static void header_rect(int *x, int *y, int *w, int *h) {
    *x = g_x; *y = g_y; *w = TM.touch; *h = TM.touch;
}

static int list_area_y(void) { return g_y + TM.touch; }
static int list_area_h(void) { return g_h - TM.touch; }

static void render_list(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);
    draw_header(g_cwd);

    const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
    int n = (int)ramfs_list_dir(g_cwd, files, RAMFS_MAX_ENTRIES);

    int area_y = list_area_y();
    int area_h = list_area_h();

    if (n == 0) {
        hal_gfx_draw_string_centered(g_x, area_y + area_h / 2 - FONT_H * TM.scale_small / 2,
                                      g_w, "EMPTY", GFX_UI_TEXT_DIM, TM.scale_small);
        return;
    }

    for (int i = 0; i < n; i++) {
        int ry = area_y + i * g_row_h - g_scroll;
        if (ry + g_row_h < area_y || ry > area_y + area_h) continue;   /* за экраном */

        if (g_pressed_row == i)
            hal_gfx_fill_rect(g_x, ry, g_w, g_row_h, GFX_UI_SURFACE_2);

        /* Метка типа: папка — заполненный квадрат акцентом, файл — рамка. */
        int m = TM.touch / 3;
        int mx = g_x + TM.pad * 2;
        int my = ry + (g_row_h - m) / 2;
        if (files[i]->is_dir)
            hal_gfx_fill_rounded_rect(mx, my, m, m, GFX_UI_ACCENT, 2);
        else
            hal_gfx_draw_rounded_rect(mx, my, m, m, GFX_UI_TEXT_DIM, 2);

        hal_gfx_draw_string_scaled(mx + m + TM.pad * 2,
                                    ry + (g_row_h - FONT_H * TM.scale_small) / 2,
                                    files[i]->name,
                                    files[i]->is_dir ? GFX_UI_ACCENT : GFX_UI_TEXT,
                                    TM.scale_small);

        /* Размер файла справа */
        if (!files[i]->is_dir) {
            char buf[16], num[12];
            int pos = touch_strcat(buf, 0, sizeof(buf), touch_itoa((int)files[i]->size, num, 0));
            touch_strcat(buf, pos, sizeof(buf), "B");
            int tw = hal_gfx_string_width(buf, TM.scale_small);
            hal_gfx_draw_string_scaled(g_x + g_w - TM.pad * 2 - tw,
                                        ry + (g_row_h - FONT_H * TM.scale_small) / 2,
                                        buf, GFX_UI_TEXT_DIM, TM.scale_small);
        }

        hal_gfx_fill_rect(g_x + TM.pad, ry + g_row_h - 1, g_w - TM.pad * 2, 1, GFX_UI_DIVIDER);
    }
}

static void render_viewer(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);
    draw_header(g_view_name);

    const ramfs_file_t *f = ramfs_lookup(g_view_name);
    int area_y = list_area_y() + TM.pad;
    int area_x = g_x + TM.pad * 2;

    if (!f) {
        hal_gfx_draw_string_scaled(area_x, area_y, "NOT FOUND", GFX_UI_WARN, TM.scale_small);
        return;
    }
    if (f->size == 0) {
        hal_gfx_draw_string_scaled(area_x, area_y, "(EMPTY FILE)", GFX_UI_TEXT_DIM, TM.scale_small);
        return;
    }

    /* Перенос по ширине области, с учётом прокрутки по строкам. */
    int cw = FONT_W * TM.scale_small;
    int chx = FONT_H * TM.scale_small;
    int cols = (g_w - TM.pad * 4) / cw;
    if (cols < 1) return;

    int col = 0;
    int line = 0;
    for (size_t i = 0; i < f->size; i++) {
        char c = (char)f->data[i];
        if (c == '\n') { col = 0; line++; continue; }

        int py = area_y + line * chx - g_scroll;
        if (py >= list_area_y() && py + chx <= g_y + g_h)
            hal_gfx_draw_char_scaled(area_x + col * cw, py, c,
                                      GFX_UI_TEXT, GFX_UI_BG, TM.scale_small);
        col++;
        if (col >= cols) { col = 0; line++; }
    }
}

static void files_render(void) {
    if (g_mode == MODE_VIEWER) render_viewer();
    else                       render_list();
}

/* Индекс строки списка под пальцем; -1, если мимо. */
static int row_at(int py) {
    int area_y = list_area_y();
    if (py < area_y) return -1;
    return (py - area_y + g_scroll) / g_row_h;
}

static void clamp_scroll(int content_h) {
    int max = content_h - list_area_h();
    if (max < 0) max = 0;
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0)   g_scroll = 0;
}

static void files_on_touch(int type, int x, int y) {
    int hx, hy, hw, hh;
    header_rect(&hx, &hy, &hw, &hh);

    if (type == HAL_EV_POINTER_DOWN) {
        g_drag_start_y = y;
        g_drag_start_scroll = g_scroll;
        g_drag_distance = 0;
        g_pressed_row = (g_mode == MODE_LIST) ? row_at(y) : -1;
        return;
    }

    if (type == HAL_EV_POINTER_MOVE) {
        int dy = y - g_drag_start_y;
        if (dy < 0) dy = -dy;
        if (dy > g_drag_distance) g_drag_distance = dy;

        /* Как только палец уехал заметно — это прокрутка, а не нажатие. */
        if (g_drag_distance > TM.touch / 3) {
            g_pressed_row = -1;
            g_scroll = g_drag_start_scroll - (y - g_drag_start_y);

            if (g_mode == MODE_LIST) {
                const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
                int n = (int)ramfs_list_dir(g_cwd, files, RAMFS_MAX_ENTRIES);
                clamp_scroll(n * g_row_h);
            } else {
                clamp_scroll(g_h * 4);   /* грубая оценка длины файла */
            }
        }
        return;
    }

    if (type != HAL_EV_POINTER_UP) return;

    int was_row = g_pressed_row;
    g_pressed_row = -1;

    /* Тянули — значит прокручивали, ничего не открываем. */
    if (g_drag_distance > TM.touch / 3) return;

    /* Кнопка "наверх" в заголовке */
    if (touch_hit(x, y, hx, hy, hw, hh)) {
        if (g_mode == MODE_VIEWER) { g_mode = MODE_LIST; g_scroll = 0; }
        else                        { go_parent(); g_scroll = 0; }
        return;
    }

    if (g_mode != MODE_LIST || was_row < 0) return;

    const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
    int n = (int)ramfs_list_dir(g_cwd, files, RAMFS_MAX_ENTRIES);
    if (was_row >= n) return;

    if (files[was_row]->is_dir) {
        enter_dir(files[was_row]->name);
    } else {
        str_copy(g_view_name, files[was_row]->path, sizeof(g_view_name));
        g_mode = MODE_VIEWER;
        g_scroll = 0;
    }
}

static void files_on_key(int key) { (void)key; }
static int  files_wants_keyboard(void) { return 0; }

const touch_app_t app_files = {
    .name  = "FILES",
    .glyph = "/",
    .color = GFX_UI_OK,
    .color2 = GFX_HILL_DARK,
    .init  = files_init,
    .layout = files_layout,
    .render = files_render,
    .on_touch = files_on_touch,
    .on_key = files_on_key,
    .wants_keyboard = files_wants_keyboard
};
