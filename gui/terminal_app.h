/* gui/terminal_app.h — приложение "Терминал": root@proshivkaOS-промпт,
 * реальные директории (cd/mkdir/pwd/ls), команды clear/touch/cat/edit/
 * date/uptime/neofetch/help, цветной вывод (директории синим, ошибки
 * красным, промпт красным как у root).
 */
#ifndef GUI_TERMINAL_APP_H
#define GUI_TERMINAL_APP_H

#include "gconsole.h"
#include "ramfs.h"

#define TERM_LINE_BUF   48
#define TERM_HOME_DIR   "/root"

typedef struct {
    gconsole_t console;
    char line[TERM_LINE_BUF];
    int line_len;

    char cwd[RAMFS_MAX_PATH_LEN];

    int in_edit_mode;
    char edit_filename[RAMFS_MAX_PATH_LEN];
    char edit_buf[RAMFS_MAX_FILE_SIZE];
    size_t edit_total;
} terminal_app_t;

void terminal_app_init(terminal_app_t *app, const gui_window_t *win);

/* То же самое, но с явными цветами. Нужно тач-сборке: там тема тёмная, а
 * приветственная строка и первый промпт печатаются ВНУТРИ инициализации —
 * если менять цвета после неё, эти символы остаются чёрными на чёрном
 * (цвет запоминается у каждого символа отдельно, см. gconsole_t.color). */
void terminal_app_init_colors(terminal_app_t *app, const gui_window_t *win,
                               uint8_t fg, uint8_t bg);
void terminal_app_handle_char(terminal_app_t *app, char c);

#endif
