/* shell/lineedit.c — редактор строки для shell.
 *
 * Использует только hal_console_putc/write и hal_console_getc, поэтому
 * не зависит от архитектуры. Редактирование — только в конце строки
 * (Backspace); перемещение курсора по строке стрелками влево/вправо
 * требует позиционирования курсора и здесь не реализовано.
 */
#include <stddef.h>
#include "hal.h"
#include "ramfs.h"
#include "lineedit.h"

#define LINE_CAP   128              /* не меньше CMD_BUF_SIZE из shell.c */
#define HIST_MAX   16
#define MAX_MATCH  64

static char hist[HIST_MAX][LINE_CAP];
static int  hist_n;

/* Команды, у которых аргумент — имя файла. */
static const char *const FILE_CMDS[] = { "cat", "edit", "touch" };

/* ---------------- строки ---------------- */

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int starts_with(const char *s, const char *prefix, size_t plen) {
    for (size_t i = 0; i < plen; i++)
        if (s[i] != prefix[i] || !s[i]) return 0;
    return 1;
}

static int same(const char *a, size_t alen, const char *b) {
    return slen(b) == alen && starts_with(b, a, alen);
}

static void copy_str(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ---------------- ввод ---------------- */

/* hal_console_getc() отдаёт и обычные символы, и HAL_KEY_* (стрелки).
 * Цикл подстрахован на случай, если вызов неблокирующий. */
static int get_key(void) {
    int c;
    do { c = hal_console_getc(); } while (c <= 0);
    return c;
}

static void erase_chars(size_t n) {
    while (n--) hal_console_write("\b \b");
}

/* ---------------- история ---------------- */

static void hist_add(const char *line) {
    if (!line[0]) return;
    if (hist_n && same(line, slen(line), hist[hist_n - 1])) return;   /* подряд одинаковые */
    if (hist_n == HIST_MAX) {                                         /* вытесняем самую старую */
        for (int i = 1; i < HIST_MAX; i++)
            copy_str(hist[i - 1], hist[i], LINE_CAP);
        hist_n--;
    }
    copy_str(hist[hist_n++], line, LINE_CAP);
}

static void replace_line(char *buf, size_t *len, const char *src, size_t max) {
    erase_chars(*len);
    size_t n = 0;
    while (src[n] && n < max - 1) { buf[n] = src[n]; n++; }
    buf[n] = '\0';
    *len = n;
    hal_console_write(buf);
}

/* ---------------- автодополнение ---------------- */

static int is_file_cmd(const char *w, size_t wlen) {
    for (size_t i = 0; i < sizeof FILE_CMDS / sizeof FILE_CMDS[0]; i++)
        if (same(w, wlen, FILE_CMDS[i])) return 1;
    return 0;
}

static size_t collect(const char *prefix, size_t plen, int want_cmd,
                      const char *const *cmds, size_t ncmds,
                      const char **out) {
    size_t n = 0;
    if (want_cmd) {
        for (size_t i = 0; i < ncmds && n < MAX_MATCH; i++)
            if (starts_with(cmds[i], prefix, plen)) out[n++] = cmds[i];
    } else {
        const ramfs_file_t *files[RAMFS_MAX_FILES];
        size_t fn = ramfs_list(files, RAMFS_MAX_FILES);
        for (size_t i = 0; i < fn && n < MAX_MATCH; i++)
            if (starts_with(files[i]->name, prefix, plen)) out[n++] = files[i]->name;
    }
    return n;
}

static void add_char(char *buf, size_t *len, size_t max, char c) {
    if (*len < max - 1) {
        buf[(*len)++] = c;
        buf[*len] = '\0';
        hal_console_putc(c);
    }
}

static void complete(char *buf, size_t *len, size_t max, void (*prompt)(void),
                     const char *const *cmds, size_t ncmds) {
    /* начало последнего слова */
    size_t ws = *len;
    while (ws > 0 && buf[ws - 1] != ' ') ws--;
    size_t plen = *len - ws;

    int first = 1;                                   /* перед словом одни пробелы? */
    size_t a = 0;
    while (a < ws && buf[a] == ' ') a++;
    if (a < ws) first = 0;

    if (!first) {                                    /* аргумент: только для файловых команд */
        size_t b = a;
        while (b < *len && buf[b] != ' ') b++;
        if (!is_file_cmd(buf + a, b - a)) return;
    }

    const char *m[MAX_MATCH];
    size_t n = collect(buf + ws, plen, first, cmds, ncmds, m);
    if (!n) return;

    size_t common = slen(m[0]);                      /* общий префикс всех вариантов */
    for (size_t k = 1; k < n; k++) {
        size_t l = 0;
        while (m[0][l] && m[k][l] == m[0][l]) l++;
        if (l < common) common = l;
    }

    if (common > plen) {                             /* есть что дописать */
        for (size_t i = plen; i < common; i++) add_char(buf, len, max, m[0][i]);
        if (n == 1) add_char(buf, len, max, ' ');    /* единственный вариант — завершаем */
        return;
    }
    if (n == 1) {                                    /* слово уже набрано целиком */
        if (*len == 0 || buf[*len - 1] != ' ') add_char(buf, len, max, ' ');
        return;
    }

    /* Вариантов несколько, дописать нечего — показываем список. */
    hal_console_putc('\n');
    for (size_t k = 0; k < n; k++) {
        hal_console_write(m[k]);
        hal_console_write("  ");
    }
    hal_console_putc('\n');
    prompt();
    hal_console_write(buf);
}

/* ---------------- основной цикл ---------------- */

size_t line_read(char *buf, size_t max, void (*prompt)(void),
                 const char *const *cmds, size_t ncmds) {
    static char saved[LINE_CAP];                     /* строка, которую начали набирать до ↑ */
    if (max > LINE_CAP) max = LINE_CAP;

    size_t len = 0;
    int pos = hist_n;                                /* == hist_n: правим новую строку */
    buf[0] = '\0';
    saved[0] = '\0';

    for (;;) {
        int c = get_key();

        if (c == '\n' || c == '\r') {
            hal_console_putc('\n');
            buf[len] = '\0';
            hist_add(buf);
            return len;
        } else if (c == '\b' || c == 127) {
            if (len) { len--; buf[len] = '\0'; hal_console_write("\b \b"); }
        } else if (c == '\t') {
            complete(buf, &len, max, prompt, cmds, ncmds);
        } else if (c == HAL_KEY_UP) {
            if (pos > 0) {
                if (pos == hist_n) copy_str(saved, buf, LINE_CAP);
                pos--;
                replace_line(buf, &len, hist[pos], max);
            }
        } else if (c == HAL_KEY_DOWN) {
            if (pos < hist_n) {
                pos++;
                replace_line(buf, &len, pos == hist_n ? saved : hist[pos], max);
            }
        } else if (c == 0x15) {                      /* Ctrl+U */
            erase_chars(len);
            len = 0;
            buf[0] = '\0';
        } else if (c == 0x03) {                      /* Ctrl+C */
            hal_console_write("^C\n");
            buf[0] = '\0';
            return 0;
        } else if (c >= 32 && c < 127) {
            add_char(buf, &len, max, (char)c);
        }
        /* остальные коды (HAL_KEY_LEFT/RIGHT/HOME/END, прочие Ctrl) игнорируются */
    }
}
