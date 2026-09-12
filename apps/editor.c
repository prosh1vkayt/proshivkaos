/* apps/editor.c — полноэкранный текстовый редактор в стиле micro/nano.
 *
 * Хранит текст как массив строк фиксированного размера (без malloc —
 * hal_mem_alloc/free пока заготовка, см. hal.h) — этого достаточно, раз
 * RamFS сама ограничивает файл RAMFS_MAX_FILE_SIZE байт (см. ramfs.h).
 *
 * Полная перерисовка экрана на каждое нажатие клавиши — не оптимизация,
 * а сознательный выбор простоты: запись в буфер 0xB8000 — это просто
 * memory-mapped запись, 80x25 символов перерисовываются за микросекунды,
 * а инкрементальный diff-рендеринг (как в реальных редакторах поверх
 * терминала) добавил бы сложности без ощутимой пользы на этом железе.
 *
 * Границы: без переносов длинных строк (длинная строка обрезается по
 * ширине текстовой области, без горизонтального скролла) и без undo —
 * дальше можно нарастить при необходимости.
 */
#include "hal.h"
#include "ramfs.h"
#include "editor.h"

#define SCREEN_ROWS      25
#define SCREEN_COLS      80
#define GUTTER_WIDTH       5   /* "9999 " — 4 цифры номера строки + пробел */
#define TEXT_COLS        (SCREEN_COLS - GUTTER_WIDTH)
#define TEXT_ROWS        (SCREEN_ROWS - 1)  /* нижняя строка — статус-бар */

#define EDITOR_MAX_LINES  200
#define EDITOR_LINE_CAP   120

#define CTRL_KEY(k) ((k) & 0x1f)

typedef struct {
    char text[EDITOR_LINE_CAP + 1];
    int  len;
} editor_line_t;

static editor_line_t lines[EDITOR_MAX_LINES];
static int  line_count;
static int  cur_row, cur_col;   /* логическая позиция курсора в тексте */
static int  top_line;           /* первая видимая строка (вертикальный скролл) */
static int  dirty;              /* есть несохранённые изменения */
static char cur_filename[RAMFS_MAX_PATH_LEN];
static char status_msg[SCREEN_COLS];

/* ---------------- Мелкие строковые хелперы (без libc, только static) ---------------- */

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

static void put_str(char *dst, int *pos, const char *s) {
    while (*s && *pos < SCREEN_COLS) dst[(*pos)++] = *s++;
}

static void put_int(char *dst, int *pos, int n) {
    char tmp[12];
    int  i = 0;

    if (n == 0) {
        tmp[i++] = '0';
    } else {
        int neg = n < 0;
        if (neg) n = -n;
        while (n > 0) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
        if (neg) tmp[i++] = '-';
    }
    while (i > 0 && *pos < SCREEN_COLS) dst[(*pos)++] = tmp[--i];
}

/* ---------------- Ввод ---------------- */

/* hal_console_getc_blocking() возвращает char и обрезает коды стрелок
 * (>255), поэтому для редактора крутим свой спин-луп поверх неблокирующего
 * hal_console_getc(), который возвращает полноценный int. */
static int editor_read_key(void) {
    int c;
    do { c = hal_console_getc(); } while (c < 0);
    return c;
}

/* ---------------- Редактирование буфера строк ---------------- */

static void line_insert_char(int row, int col, char c) {
    editor_line_t *l = &lines[row];
    if (l->len >= EDITOR_LINE_CAP) return; /* строка уже на пределе EDITOR_LINE_CAP */

    for (int i = l->len; i > col; i--) l->text[i] = l->text[i - 1];
    l->text[col] = c;
    l->len++;
    l->text[l->len] = '\0';
    dirty = 1;
}

static void line_delete_char(int row, int col) {
    editor_line_t *l = &lines[row];
    if (col >= l->len) return;

    for (int i = col; i < l->len - 1; i++) l->text[i] = l->text[i + 1];
    l->len--;
    l->text[l->len] = '\0';
    dirty = 1;
}

static void editor_insert_newline(void) {
    if (line_count >= EDITOR_MAX_LINES) return; /* достигнут предел EDITOR_MAX_LINES */

    for (int i = line_count; i > cur_row + 1; i--) lines[i] = lines[i - 1];

    editor_line_t *cur  = &lines[cur_row];
    editor_line_t *next = &lines[cur_row + 1];

    int tail_len = cur->len - cur_col;
    for (int i = 0; i < tail_len; i++) next->text[i] = cur->text[cur_col + i];
    next->text[tail_len] = '\0';
    next->len = tail_len;

    cur->text[cur_col] = '\0';
    cur->len = cur_col;

    line_count++;
    cur_row++;
    cur_col = 0;
    dirty = 1;
}

static void editor_delete_backward(void) {
    if (cur_col > 0) {
        line_delete_char(cur_row, cur_col - 1);
        cur_col--;
        return;
    }
    if (cur_row == 0) return;

    editor_line_t *prev = &lines[cur_row - 1];
    editor_line_t *cur  = &lines[cur_row];
    int new_col = prev->len;

    for (int i = 0; i < cur->len && prev->len < EDITOR_LINE_CAP; i++)
        prev->text[prev->len++] = cur->text[i];
    prev->text[prev->len] = '\0';

    for (int i = cur_row; i < line_count - 1; i++) lines[i] = lines[i + 1];
    line_count--;
    cur_row--;
    cur_col = new_col;
    dirty = 1;
}

static void editor_delete_forward(void) {
    editor_line_t *cur = &lines[cur_row];

    if (cur_col < cur->len) {
        line_delete_char(cur_row, cur_col);
        return;
    }
    if (cur_row >= line_count - 1) return;

    editor_line_t *next = &lines[cur_row + 1];
    for (int i = 0; i < next->len && cur->len < EDITOR_LINE_CAP; i++)
        cur->text[cur->len++] = next->text[i];
    cur->text[cur->len] = '\0';

    for (int i = cur_row + 1; i < line_count - 1; i++) lines[i] = lines[i + 1];
    line_count--;
    dirty = 1;
}

static void move_cursor(int key) {
    editor_line_t *l = &lines[cur_row];

    switch (key) {
        case HAL_KEY_LEFT:
            if (cur_col > 0) cur_col--;
            else if (cur_row > 0) { cur_row--; cur_col = lines[cur_row].len; }
            break;
        case HAL_KEY_RIGHT:
            if (cur_col < l->len) cur_col++;
            else if (cur_row < line_count - 1) { cur_row++; cur_col = 0; }
            break;
        case HAL_KEY_UP:
            if (cur_row > 0) cur_row--;
            break;
        case HAL_KEY_DOWN:
            if (cur_row < line_count - 1) cur_row++;
            break;
        case HAL_KEY_HOME:
            cur_col = 0;
            break;
        case HAL_KEY_END:
            cur_col = lines[cur_row].len;
            break;
        case HAL_KEY_PAGE_UP:
            cur_row -= TEXT_ROWS;
            if (cur_row < 0) cur_row = 0;
            break;
        case HAL_KEY_PAGE_DOWN:
            cur_row += TEXT_ROWS;
            if (cur_row >= line_count) cur_row = line_count - 1;
            break;
        default:
            break;
    }

    /* после смены строки курсор мог оказаться правее её конца */
    int len = lines[cur_row].len;
    if (cur_col > len) cur_col = len;
}

/* ---------------- Загрузка / сохранение через RamFS ---------------- */

static void editor_reset(void) {
    line_count = 1;
    lines[0].len = 0;
    lines[0].text[0] = '\0';
    cur_row = cur_col = 0;
    top_line = 0;
    dirty = 0;
    status_msg[0] = '\0';
}

static void editor_load(const char *path) {
    editor_reset();
    str_copy(cur_filename, path, sizeof(cur_filename));

    const ramfs_file_t *f = ramfs_lookup(path);
    if (!f || f->size == 0) return;

    int row = 0, col = 0;
    lines[0].len = 0;
    lines[0].text[0] = '\0';

    for (size_t i = 0; i < f->size && row < EDITOR_MAX_LINES; i++) {
        char c = (char)f->data[i];
        if (c == '\n') {
            lines[row].text[col] = '\0';
            lines[row].len = col;
            row++;
            col = 0;
            if (row < EDITOR_MAX_LINES) { lines[row].len = 0; lines[row].text[0] = '\0'; }
        } else if (col < EDITOR_LINE_CAP) {
            lines[row].text[col++] = c;
            /* символы сверх EDITOR_LINE_CAP в строке молча отбрасываются */
        }
    }

    if (row < EDITOR_MAX_LINES && (col > 0 || row == 0)) {
        lines[row].text[col] = '\0';
        lines[row].len = col;
        row++;
    }

    line_count = row > 0 ? row : 1;
}

static void editor_save(void) {
    static char out[RAMFS_MAX_FILE_SIZE];
    size_t total = 0;

    for (int i = 0; i < line_count; i++) {
        for (int j = 0; j < lines[i].len && total < RAMFS_MAX_FILE_SIZE - 1; j++)
            out[total++] = lines[i].text[j];
        if (total < RAMFS_MAX_FILE_SIZE - 1)
            out[total++] = '\n';
    }

    ramfs_write(cur_filename, out, total);
    dirty = 0;
    str_copy(status_msg, "sohraneno", sizeof(status_msg));
}

/* ---------------- Отрисовка ---------------- */

static void editor_scroll(void) {
    if (cur_row < top_line) top_line = cur_row;
    if (cur_row >= top_line + TEXT_ROWS) top_line = cur_row - TEXT_ROWS + 1;
    if (top_line < 0) top_line = 0;
}

static void editor_draw_rows(void) {
    for (int screen_row = 0; screen_row < TEXT_ROWS; screen_row++) {
        int file_row = top_line + screen_row;
        hal_console_goto(screen_row, 0);

        if (file_row < line_count) {
            hal_console_set_color(HAL_COLOR_DARK_GREY, HAL_COLOR_BLACK);
            char numbuf[8];
            int  p = 0;
            put_int(numbuf, &p, file_row + 1);
            for (int pad = p; pad < 4; pad++) hal_console_putc(' ');
            for (int i = 0; i < p; i++) hal_console_putc(numbuf[i]);
            hal_console_putc(' ');
            hal_console_reset_color();

            int n = lines[file_row].len;
            if (n > TEXT_COLS) n = TEXT_COLS;
            for (int i = 0; i < n; i++) hal_console_putc(lines[file_row].text[i]);
            for (int i = GUTTER_WIDTH + n; i < SCREEN_COLS; i++) hal_console_putc(' ');
        } else {
            hal_console_set_color(HAL_COLOR_BLUE, HAL_COLOR_BLACK);
            hal_console_putc('~');
            hal_console_reset_color();
            for (int i = 1; i < SCREEN_COLS; i++) hal_console_putc(' ');
        }
    }
}

static void editor_draw_status(void) {
    char line[SCREEN_COLS];
    int  pos = 0;

    put_str(line, &pos, cur_filename);
    if (dirty) put_str(line, &pos, "*");
    put_str(line, &pos, "  Ln ");
    put_int(line, &pos, cur_row + 1);
    put_str(line, &pos, ", Col ");
    put_int(line, &pos, cur_col + 1);
    if (status_msg[0]) {
        put_str(line, &pos, "  -- ");
        put_str(line, &pos, status_msg);
    }
    put_str(line, &pos, "  ^S sohranit  ^Q vyhod  ^F poisk");

    /* до SCREEN_COLS-1, а не SCREEN_COLS: заполнить самую правую нижнюю
     * ячейку экрана через putc() значило бы дать vga_putc() перевести
     * курсор за пределы экрана и вызвать скролл всего кадра — см.
     * arch/x86/vga.c. Одна пустая клетка внизу справа — цена простоты. */
    for (int i = pos; i < SCREEN_COLS - 1; i++) line[i] = ' ';

    hal_console_goto(SCREEN_ROWS - 1, 0);
    hal_console_set_color(HAL_COLOR_BLACK, HAL_COLOR_LIGHT_GREY);
    for (int i = 0; i < SCREEN_COLS - 1; i++) hal_console_putc(line[i]);
    hal_console_reset_color();
}

static void editor_refresh(void) {
    editor_scroll();
    editor_draw_rows();
    editor_draw_status();
    hal_console_goto(cur_row - top_line, cur_col + GUTTER_WIDTH);
}

/* ---------------- Строка ввода снизу (для поиска и подтверждений) ---------------- */

static void editor_prompt_line(const char *prompt, char *out, size_t max_len) {
    size_t len = 0;
    out[0] = '\0';

    for (;;) {
        char line[SCREEN_COLS];
        int  pos = 0;
        put_str(line, &pos, prompt);
        put_str(line, &pos, out);
        for (int i = pos; i < SCREEN_COLS - 1; i++) line[i] = ' ';

        hal_console_goto(SCREEN_ROWS - 1, 0);
        hal_console_set_color(HAL_COLOR_BLACK, HAL_COLOR_LIGHT_GREY);
        for (int i = 0; i < SCREEN_COLS - 1; i++) hal_console_putc(line[i]);
        hal_console_reset_color();
        hal_console_goto(SCREEN_ROWS - 1, pos);

        int c = editor_read_key();

        if (c == '\n') return;
        if (c == 27) { out[0] = '\0'; return; }  /* Esc — отмена */
        if (c == '\b') {
            if (len > 0) { len--; out[len] = '\0'; }
        } else if (c >= 32 && c < 127 && len < max_len - 1) {
            out[len++] = (char)c;
            out[len] = '\0';
        }
    }
}

/* ---------------- Поиск (Ctrl+F) ---------------- */

static int line_find_from(const editor_line_t *l, int start_col, const char *needle, int needle_len) {
    if (needle_len == 0) return -1;
    for (int i = start_col; i + needle_len <= l->len; i++) {
        int match = 1;
        for (int j = 0; j < needle_len; j++) {
            if (l->text[i + j] != needle[j]) { match = 0; break; }
        }
        if (match) return i;
    }
    return -1;
}

static void editor_search(void) {
    char query[64];
    editor_prompt_line("Poisk: ", query, sizeof(query));
    if (query[0] == '\0') return;

    int qlen = (int)str_len(query);
    int row  = cur_row;
    int col  = cur_col + 1;  /* со следующей позиции, иначе поиск не сдвинется с места */

    for (int steps = 0; steps < line_count; steps++) {
        if (col <= lines[row].len) {
            int found = line_find_from(&lines[row], col, query, qlen);
            if (found >= 0) {
                cur_row = row;
                cur_col = found;
                str_copy(status_msg, "naydeno", sizeof(status_msg));
                return;
            }
        }
        row = (row + 1) % line_count;
        col = 0;
    }
    str_copy(status_msg, "ne naydeno", sizeof(status_msg));
}

/* ---------------- Выход с подтверждением при несохранённых правках ---------------- */

static int editor_confirm_quit(void) {
    if (!dirty) return 1;

    char resp[4];
    editor_prompt_line("Est nesohranennye izmeneniya. Vyyti bez sohraneniya? (y/n): ",
                        resp, sizeof(resp));
    return (resp[0] == 'y' || resp[0] == 'Y');
}

/* ---------------- Главный цикл ---------------- */

void editor_run(const char *path) {
    editor_load(path);

    for (;;) {
        editor_refresh();
        int c = editor_read_key();

        if (c == CTRL_KEY('s')) {
            editor_save();
        } else if (c == CTRL_KEY('q')) {
            if (editor_confirm_quit()) break;
        } else if (c == CTRL_KEY('f')) {
            editor_search();
        } else if (c == HAL_KEY_UP || c == HAL_KEY_DOWN || c == HAL_KEY_LEFT ||
                   c == HAL_KEY_RIGHT || c == HAL_KEY_HOME || c == HAL_KEY_END ||
                   c == HAL_KEY_PAGE_UP || c == HAL_KEY_PAGE_DOWN) {
            move_cursor(c);
        } else if (c == HAL_KEY_DEL) {
            editor_delete_forward();
        } else if (c == '\n') {
            editor_insert_newline();
        } else if (c == '\b') {
            editor_delete_backward();
        } else if (c == '\t') {
            for (int i = 0; i < 4; i++) { line_insert_char(cur_row, cur_col, ' '); cur_col++; }
        } else if (c >= 32 && c < 127) {
            line_insert_char(cur_row, cur_col, (char)c);
            cur_col++;
        }
        /* прочие коды (голый Esc, необработанные Ctrl-комбинации) игнорируются */
    }

    hal_console_clear();
}
