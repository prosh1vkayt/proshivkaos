/* shell/shell.c — встроенная командная строка поверх RamFS.
 * Использует только hal.h и ramfs.h — работает одинаково на x86 и ARM64.
 */
#include "hal.h"
#include "ramfs.h"

#define CMD_BUF_SIZE   128
#define CMD_MAX_ARGS   4

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Читает одну строку с консоли (echo включён), без строчного буфера ОС —
 * echo делаем сами через hal_console_putc. */
static void read_line(char *buf, size_t max_len) {
    size_t i = 0;
    for (;;) {
        char c = hal_console_getc_blocking();

        if (c == '\n') {
            hal_console_putc('\n');
            buf[i] = '\0';
            return;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                hal_console_write("\b \b");
            }
        } else if (i < max_len - 1) {
            buf[i++] = c;
            hal_console_putc(c);
        }
    }
}

/* Разбивает строку на аргументы по пробелу (in-place, без malloc). */
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

/* ---------------- Команды ---------------- */

static void cmd_ls(void) {
    const ramfs_file_t *files[RAMFS_MAX_FILES];
    size_t n = ramfs_list(files, RAMFS_MAX_FILES);

    if (n == 0) {
        hal_console_write("(pusto)\n");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        hal_console_set_color(HAL_COLOR_LIGHT_GREEN, HAL_COLOR_BLACK);
        hal_console_write(files[i]->name);
        hal_console_reset_color();
        hal_console_putc('\n');
    }
}

static void cmd_cat(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: cat <file>\n");
        return;
    }

    const ramfs_file_t *f = ramfs_lookup(argv[1]);
    if (!f) {
        hal_console_write("cat: fayl ne nayden: ");
        hal_console_write(argv[1]);
        hal_console_putc('\n');
        return;
    }

    for (size_t i = 0; i < f->size; i++)
        hal_console_putc((char)f->data[i]);
    if (f->size == 0 || f->data[f->size - 1] != '\n')
        hal_console_putc('\n');
}

static void cmd_touch(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: touch <file>\n");
        return;
    }

    if (ramfs_touch(argv[1]) != 0)
        hal_console_write("touch: ne udalos sozdat fayl\n");
}

static void cmd_neofetch(void) {
    hal_console_set_color(HAL_COLOR_LIGHT_CYAN, HAL_COLOR_BLACK);
    hal_console_write(
        "        /\\      \n"
        "       /  \\     \n"
        "      / /\\ \\    \n"
        "     / ____ \\   \n"
        "    /_/    \\_\\  \n"
    );
    hal_console_reset_color();

    hal_console_set_color(HAL_COLOR_YELLOW, HAL_COLOR_BLACK);
    hal_console_write("proshivkaOS NEXT\n");
    hal_console_reset_color();
    hal_console_write("-----------------\n");
    hal_console_write("kernel : microkernel, etap 1\n");
    hal_console_write("hal    : console/mem/thread(stub)\n");
    hal_console_write("fs     : RamFS (in-memory)\n");
    hal_console_write("arch   : x86 (multiboot)\n");
}

static void cmd_edit(int argc, char *argv[]) {
    if (argc < 2) {
        hal_console_write("usage: edit <file>\n");
        return;
    }

    /* Файл должен существовать — создаём при отсутствии, как touch. */
    ramfs_touch(argv[1]);

    hal_console_set_color(HAL_COLOR_DARK_GREY, HAL_COLOR_BLACK);
    hal_console_write("-- redaktor: vvodi stroki, odna stroka '.' - sohranit i vyyti --\n");
    hal_console_reset_color();

    static char buf[RAMFS_MAX_FILE_SIZE];
    size_t total = 0;

    for (;;) {
        char line[CMD_BUF_SIZE];
        read_line(line, CMD_BUF_SIZE);

        if (str_eq(line, ".")) break;

        size_t len = str_len(line);
        if (total + len + 1 >= RAMFS_MAX_FILE_SIZE) {
            hal_console_write("edit: fayl slishkom bolshoy, obrezano\n");
            break;
        }

        for (size_t i = 0; i < len; i++) buf[total++] = line[i];
        buf[total++] = '\n';
    }

    ramfs_write(argv[1], buf, total);

    hal_console_set_color(HAL_COLOR_LIGHT_GREEN, HAL_COLOR_BLACK);
    hal_console_write("sohraneno: ");
    hal_console_write(argv[1]);
    hal_console_putc('\n');
    hal_console_reset_color();
}

static void cmd_help(void) {
    hal_console_write(
        "Dostupnye komandy:\n"
        "  ls               - spisok faylov\n"
        "  cat <file>        - pokazat soderzhimoe fayla\n"
        "  touch <file>       - sozdat pustoy fayl\n"
        "  edit <file>         - prostoy tekstovyy redaktor (stroka '.' - sohranit)\n"
        "  neofetch           - info o sisteme\n"
        "  help                - eta spravka\n"
    );
}

/* ---------------- Главный цикл ---------------- */

void shell_run(void) {
    char line[CMD_BUF_SIZE];
    char *argv[CMD_MAX_ARGS];

    cmd_help();

    for (;;) {
        hal_console_set_color(HAL_COLOR_LIGHT_MAGENTA, HAL_COLOR_BLACK);
        hal_console_write("proshivkaos> ");
        hal_console_reset_color();
        read_line(line, CMD_BUF_SIZE);

        int argc = tokenize(line, argv, CMD_MAX_ARGS);
        if (argc == 0)
            continue;

        if (str_eq(argv[0], "ls"))            cmd_ls();
        else if (str_eq(argv[0], "cat"))      cmd_cat(argc, argv);
        else if (str_eq(argv[0], "touch"))    cmd_touch(argc, argv);
        else if (str_eq(argv[0], "edit"))     cmd_edit(argc, argv);
        else if (str_eq(argv[0], "neofetch")) cmd_neofetch();
        else if (str_eq(argv[0], "help"))     cmd_help();
        else {
            hal_console_write("neizvestnaya komanda: ");
            hal_console_write(argv[0]);
            hal_console_putc('\n');
        }
    }
}
