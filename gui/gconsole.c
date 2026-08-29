/* gui/gconsole.c */
#include "gconsole.h"

static void push_new_line(gconsole_t *gc) {
    if (gc->line_count < GC_MAX_LINES) {
        gc->line_count++;
    } else {
        /* скроллбек заполнен — сдвигаем всё на одну строку вверх,
           теряем самую старую (как реальный терминал с ограниченной
           историей) */
        for (int i = 1; i < GC_MAX_LINES; i++) {
            gc->line_len[i - 1] = gc->line_len[i];
            for (int j = 0; j < gc->line_len[i]; j++) {
                gc->lines[i - 1][j] = gc->lines[i][j];
                gc->color[i - 1][j] = gc->color[i][j];
            }
        }
    }
    gc->line_len[gc->line_count - 1] = 0;
}

void gconsole_init(gconsole_t *gc, const gui_window_t *win, uint8_t fg, uint8_t bg) {
    gc->win = win;
    gc->fg = fg;
    gc->bg = bg;
    gc->ink = fg;
    gc->use_rect = 0;
    gc->rect_x = gc->rect_y = gc->rect_w = gc->rect_h = 0;
    gc->scale = 1;
    gconsole_clear(gc);
}

void gconsole_set_rect(gconsole_t *gc, int x, int y, int w, int h) {
    gc->use_rect = 1;
    gc->rect_x = x; gc->rect_y = y;
    gc->rect_w = w; gc->rect_h = h;
}

void gconsole_set_scale(gconsole_t *gc, int scale) {
    gc->scale = (scale < 1) ? 1 : scale;
}

/* Единая точка получения области вывода: окно или явный прямоугольник. */
static void content_rect(const gconsole_t *gc, int *x, int *y, int *w, int *h) {
    if (gc->use_rect) {
        *x = gc->rect_x; *y = gc->rect_y;
        *w = gc->rect_w; *h = gc->rect_h;
    } else {
        gui_window_content_rect(gc->win, x, y, w, h);
    }
}

int gconsole_cols(const gconsole_t *gc) {
    int cx, cy, cw, ch;
    content_rect(gc, &cx, &cy, &cw, &ch);
    int cols = cw / (FONT_W * gc->scale);
    return cols < 1 ? 1 : cols;
}

void gconsole_clear(gconsole_t *gc) {
    gc->line_count = 1;
    gc->line_len[0] = 0;
}

void gconsole_set_colors(gconsole_t *gc, uint8_t fg, uint8_t bg) {
    gc->fg = fg;
    gc->bg = bg;
    gc->ink = fg;
}

void gconsole_set_ink(gconsole_t *gc, uint8_t color) {
    gc->ink = color;
}

void gconsole_putc(gconsole_t *gc, char c) {
    if (c == '\n') {
        push_new_line(gc);
        return;
    }
    if (c == '\b') {
        int i = gc->line_count - 1;
        if (gc->line_len[i] > 0) gc->line_len[i]--;
        return;
    }

    int i = gc->line_count - 1;
    if (gc->line_len[i] >= GC_MAX_LINE_LEN) {
        push_new_line(gc);
        i = gc->line_count - 1;
    }
    int pos = gc->line_len[i]++;
    gc->lines[i][pos] = c;
    gc->color[i][pos] = gc->ink;
}

void gconsole_write(gconsole_t *gc, const char *s) {
    while (*s) gconsole_putc(gc, *s++);
}

/* Сколько "визуальных" строк займёт логическая строка длины len при
   переносе по cols символов в ряд (минимум 1 — даже пустая строка
   занимает одну визуальную строку). */
static int wrapped_row_count(int len, int cols) {
    if (len <= 0) return 1;
    return (len + cols - 1) / cols;
}

void gconsole_render(gconsole_t *gc) {
    int cx, cy, cw, ch;
    content_rect(gc, &cx, &cy, &cw, &ch);

    int scale = (gc->scale < 1) ? 1 : gc->scale;
    int cell_w = FONT_W * scale;
    int cell_h = FONT_H * scale;

    int cols = cw / cell_w;
    int rows = ch / cell_h;
    if (cols < 1 || rows < 1) return;   /* область слишком мала — рисовать нечего */

    hal_gfx_fill_rect(cx, cy, cols * cell_w, rows * cell_h, gc->bg);

    /* проход 1: считаем, сколько всего визуальных строк дал бы весь
       скроллбек при текущей ширине cols */
    int total_visual = 0;
    for (int i = 0; i < gc->line_count; i++)
        total_visual += wrapped_row_count(gc->line_len[i], cols);

    int skip = (total_visual > rows) ? (total_visual - rows) : 0;

    /* проход 2: рисуем только "хвост" — последние rows визуальных строк */
    int counter = 0;
    int row_y = cy;
    int emitted = 0;

    for (int i = 0; i < gc->line_count && emitted < rows; i++) {
        int len = gc->line_len[i];
        int chunks = wrapped_row_count(len, cols);

        for (int k = 0; k < chunks && emitted < rows; k++) {
            if (counter >= skip) {
                int start = k * cols;
                int chunk_len = len - start;
                if (chunk_len > cols) chunk_len = cols;
                if (chunk_len < 0) chunk_len = 0;

                for (int ch = 0; ch < chunk_len; ch++)
                    hal_gfx_draw_char_scaled(cx + ch * cell_w, row_y,
                                              gc->lines[i][start + ch],
                                              gc->color[i][start + ch], gc->bg, scale);

                row_y += cell_h;
                emitted++;
            }
            counter++;
        }
    }
}
