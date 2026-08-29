/* gui/cursor.c — классическая стрелка-курсор, 9x13 пикселей.
 * Чёрная заливка + белая обводка на 1px, чтобы было видно на любом фоне.
 */
#include "hal_gfx.h"
#include "cursor.h"

/* '#' = чёрный (заливка стрелки), '.' = прозрачно */
static const char *cursor_shape[13] = {
    "#........",
    "##.......",
    "#.#......",
    "#..#.....",
    "#...#....",
    "#....#...",
    "#.....#..",
    "#......#.",
    "#.......#",
    "#....###.",
    "#..##....",
    "#.#......",
    "##.......",
};

void gui_draw_cursor(int x, int y) {
    for (int row = 0; row < 13; row++) {
        const char *line = cursor_shape[row];
        for (int col = 0; col < 9; col++) {
            if (line[col] != '#') continue;

            /* белая обводка вокруг чёрного пикселя — для контраста */
            hal_gfx_put_pixel(x + col - 1, y + row,     GFX_WHITE);
            hal_gfx_put_pixel(x + col + 1, y + row,     GFX_WHITE);
            hal_gfx_put_pixel(x + col,     y + row - 1, GFX_WHITE);
            hal_gfx_put_pixel(x + col,     y + row + 1, GFX_WHITE);
        }
    }
    for (int row = 0; row < 13; row++) {
        const char *line = cursor_shape[row];
        for (int col = 0; col < 9; col++) {
            if (line[col] == '#')
                hal_gfx_put_pixel(x + col, y + row, GFX_BLACK);
        }
    }
}
