/* gui/terminal_app.c */
#include "terminal_app.h"
#include "hal_time.h"

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static void str_copy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int tokenize(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) { *p = '\0'; p++; }
    }
    return argc;
}

/* Резолвит аргумент-путь относительно текущей директории терминала:
 * "/abs/path" -> как есть; ".." -> родитель cwd; "." или "" -> cwd;
 * "~" -> домашняя директория; иначе -> cwd + "/" + arg. */
static void resolve_path(terminal_app_t *app, const char *arg, char *out, size_t max) {
    if (!arg || arg[0] == '\0') { str_copy(out, app->cwd, max); return; }

    if (arg[0] == '/') { str_copy(out, arg, max); return; }
    if (str_eq(arg, "~")) { str_copy(out, TERM_HOME_DIR, max); return; }
    if (str_eq(arg, ".")) { str_copy(out, app->cwd, max); return; }

    if (str_eq(arg, "..")) {
        size_t len = str_len(app->cwd);
        size_t last_slash = 0;
        int found = 0;
        for (size_t i = 0; i < len; i++)
            if (app->cwd[i] == '/') { last_slash = i; found = 1; }

        if (!found || last_slash == 0) str_copy(out, "/", max);
        else {
            size_t n = last_slash;
            if (n >= max) n = max - 1;
            for (size_t i = 0; i < n; i++) out[i] = app->cwd[i];
            out[n] = '\0';
        }
        return;
    }

    /* обычное имя — присоединяем к cwd */
    size_t cwd_len = str_len(app->cwd);
    if (cwd_len == 1 && app->cwd[0] == '/') {
        out[0] = '/';
        str_copy(out + 1, arg, max - 1);
    } else {
        str_copy(out, app->cwd, max);
        size_t n = str_len(out);
        if (n < max - 1) out[n++] = '/';
        out[n] = '\0';
        str_copy(out + n, arg, max - n);
    }
}

/* Показывает cwd в промпте с подстановкой "~" вместо домашней директории,
 * как в настоящем bash. */
static void write_prompt_path(terminal_app_t *app) {
    size_t home_len = str_len(TERM_HOME_DIR);

    if (str_eq(app->cwd, TERM_HOME_DIR)) {
        gconsole_write(&app->console, "~");
        return;
    }

    /* если cwd лежит внутри домашней директории — подставляем "~" */
    int under_home = 1;
    for (size_t i = 0; i < home_len; i++)
        if (app->cwd[i] != TERM_HOME_DIR[i]) { under_home = 0; break; }
    if (under_home && app->cwd[home_len] != '/') under_home = 0;

    if (under_home) {
        gconsole_write(&app->console, "~");
        gconsole_write(&app->console, app->cwd + home_len);
    } else {
        gconsole_write(&app->console, app->cwd);
    }
}

static void print_prompt(terminal_app_t *app) {
    gconsole_putc(&app->console, '\n');
    gconsole_set_ink(&app->console, GFX_LIGHT_RED);   /* root-промпт красным — конвенция */
    gconsole_write(&app->console, "root@proshivkaOS:");
    write_prompt_path(app);
    gconsole_write(&app->console, "# ");
    gconsole_set_ink(&app->console, app->console.fg);  /* дальше ввод обычным цветом */
}

static void print_error(terminal_app_t *app, const char *msg) {
    gconsole_set_ink(&app->console, GFX_LIGHT_RED);
    gconsole_putc(&app->console, '\n');
    gconsole_write(&app->console, msg);
    gconsole_set_ink(&app->console, app->console.fg);
}

static void write_padded(gconsole_t *gc, unsigned int v, int width) {
    char buf[12];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n < width) buf[n++] = '0';
    for (int i = n - 1; i >= 0; i--) gconsole_putc(gc, buf[i]);
}

static void run_command(terminal_app_t *app, char *line) {
    char *argv[4];
    int argc = tokenize(line, argv, 4);
    if (argc == 0) return;

    char path[RAMFS_MAX_PATH_LEN];

    if (str_eq(argv[0], "ls")) {
        const ramfs_file_t *files[RAMFS_MAX_ENTRIES];
        size_t n = ramfs_list_dir(app->cwd, files, RAMFS_MAX_ENTRIES);
        if (n == 0) {
            gconsole_write(&app->console, "\n(empty)");
        } else {
            for (size_t i = 0; i < n; i++) {
                gconsole_putc(&app->console, '\n');
                if (files[i]->is_dir) gconsole_set_ink(&app->console, GFX_LIGHT_BLUE);
                gconsole_write(&app->console, files[i]->name);
                if (files[i]->is_dir) gconsole_write(&app->console, "/");
                gconsole_set_ink(&app->console, app->console.fg);
            }
        }

    } else if (str_eq(argv[0], "pwd")) {
        gconsole_putc(&app->console, '\n');
        gconsole_write(&app->console, app->cwd);

    } else if (str_eq(argv[0], "cd")) {
        resolve_path(app, argc >= 2 ? argv[1] : "~", path, sizeof(path));
        if (!ramfs_is_dir(path)) {
            print_error(app, "no such directory");
        } else {
            str_copy(app->cwd, path, sizeof(app->cwd));
        }

    } else if (str_eq(argv[0], "mkdir")) {
        if (argc < 2) { print_error(app, "usage: mkdir <dir>"); return; }
        resolve_path(app, argv[1], path, sizeof(path));
        ramfs_mkdir(path);

    } else if (str_eq(argv[0], "cat")) {
        if (argc < 2) { print_error(app, "usage: cat <file>"); return; }
        resolve_path(app, argv[1], path, sizeof(path));
        const ramfs_file_t *f = ramfs_lookup(path);
        if (!f || f->is_dir) { print_error(app, "not found"); return; }
        gconsole_putc(&app->console, '\n');
        for (size_t i = 0; i < f->size; i++)
            gconsole_putc(&app->console, (char)f->data[i]);

    } else if (str_eq(argv[0], "touch")) {
        if (argc < 2) { print_error(app, "usage: touch <file>"); return; }
        resolve_path(app, argv[1], path, sizeof(path));
        ramfs_touch(path);
        gconsole_write(&app->console, "\nok");

    } else if (str_eq(argv[0], "edit")) {
        if (argc < 2) { print_error(app, "usage: edit <file>"); return; }
        resolve_path(app, argv[1], path, sizeof(path));
        ramfs_touch(path);
        str_copy(app->edit_filename, path, sizeof(app->edit_filename));
        app->edit_total = 0;
        app->in_edit_mode = 1;
        gconsole_write(&app->console, "\n-- edit mode, line '.' = save --");

    } else if (str_eq(argv[0], "clear")) {
        gconsole_clear(&app->console);
        return;   /* без \n перед следующим промптом — экран и так пуст */

    } else if (str_eq(argv[0], "date")) {
        int y, mo, d, h, mi, s;
        hal_time_rtc(&y, &mo, &d, &h, &mi, &s);
        gconsole_putc(&app->console, '\n');
        write_padded(&app->console, (unsigned)y, 4);
        gconsole_putc(&app->console, '-');
        write_padded(&app->console, (unsigned)mo, 2);
        gconsole_putc(&app->console, '-');
        write_padded(&app->console, (unsigned)d, 2);
        gconsole_putc(&app->console, ' ');
        write_padded(&app->console, (unsigned)h, 2);
        gconsole_putc(&app->console, ':');
        write_padded(&app->console, (unsigned)mi, 2);
        gconsole_putc(&app->console, ':');
        write_padded(&app->console, (unsigned)s, 2);

    } else if (str_eq(argv[0], "uptime")) {
        unsigned long total = (unsigned long)(hal_time_ms() / 1000);
        unsigned int hh = (unsigned int)(total / 3600);
        unsigned int mm = (unsigned int)((total % 3600) / 60);
        unsigned int ss = (unsigned int)(total % 60);
        gconsole_putc(&app->console, '\n');
        write_padded(&app->console, hh, 2);
        gconsole_putc(&app->console, 'H');
        gconsole_putc(&app->console, ' ');
        write_padded(&app->console, mm, 2);
        gconsole_putc(&app->console, 'M');
        gconsole_putc(&app->console, ' ');
        write_padded(&app->console, ss, 2);
        gconsole_putc(&app->console, 'S');

    } else if (str_eq(argv[0], "neofetch")) {
        gconsole_write(&app->console, "\nPROSHIVKAOS NEXT ");
        gconsole_write(&app->console, PROSHIVKAOS_VERSION);
        gconsole_write(&app->console, "\nARCH: ");
        gconsole_write(&app->console, hal_arch_name());
        gconsole_write(&app->console, "\nCPU : ");
        gconsole_write(&app->console, hal_cpu_name());
        gconsole_write(&app->console, "\nFS  : RAMFS");

    } else if (str_eq(argv[0], "help")) {
        gconsole_write(&app->console, "\nLS CD PWD MKDIR CAT TOUCH EDIT");
        gconsole_write(&app->console, "\nCLEAR DATE UPTIME NEOFETCH HELP");

    } else {
        print_error(app, "unknown command");
    }
}

void terminal_app_init(terminal_app_t *app, const gui_window_t *win) {
    terminal_app_init_colors(app, win, GFX_BLACK, GFX_WHITE);
}

void terminal_app_init_colors(terminal_app_t *app, const gui_window_t *win,
                               uint8_t fg, uint8_t bg) {
    gconsole_init(&app->console, win, fg, bg);
    app->line_len = 0;
    app->in_edit_mode = 0;
    app->edit_total = 0;

    ramfs_mkdir(TERM_HOME_DIR);
    str_copy(app->cwd, TERM_HOME_DIR, sizeof(app->cwd));

    gconsole_write(&app->console, "PROSHIVKAOS TERMINAL");
    print_prompt(app);
}

void terminal_app_handle_char(terminal_app_t *app, char c) {
    if (c == '\n') {
        app->line[app->line_len] = '\0';

        if (app->in_edit_mode) {
            if (str_eq(app->line, ".")) {
                ramfs_write(app->edit_filename, app->edit_buf, app->edit_total);
                app->in_edit_mode = 0;
                gconsole_write(&app->console, "\nsaved");
            } else {
                size_t len = str_len(app->line);
                if (app->edit_total + len + 1 < RAMFS_MAX_FILE_SIZE) {
                    for (size_t i = 0; i < len; i++)
                        app->edit_buf[app->edit_total++] = app->line[i];
                    app->edit_buf[app->edit_total++] = '\n';
                }
                gconsole_putc(&app->console, '\n');
            }
        } else {
            run_command(app, app->line);
        }

        app->line_len = 0;
        if (!app->in_edit_mode) print_prompt(app);
        else gconsole_write(&app->console, "\n. ");

    } else if (c == '\b') {
        if (app->line_len > 0) {
            app->line_len--;
            gconsole_putc(&app->console, '\b');
        }
    } else if (app->line_len < TERM_LINE_BUF - 1) {
        app->line[app->line_len++] = c;
        gconsole_putc(&app->console, c);
    }
}
