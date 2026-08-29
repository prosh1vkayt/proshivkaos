/* fs/ramfs.c — плоский список записей с полными путями вместо настоящего
 * дерева указателей: проще реализовать без malloc/free (у нас его и нет —
 * только bump-аллокатор в hal_mem.c), при этом снаружи ведёт себя как
 * обычная ФС с директориями.
 */
#include "ramfs.h"

static ramfs_file_t g_entries[RAMFS_MAX_ENTRIES];

static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Пути без ведущего '/' считаются лежащими в корне — так старый плоский
 * текстовый shell.c (ramfs_touch("welcome.txt")) продолжает работать
 * ровно как раньше, когда директорий не было вообще. */
static void normalize_path(const char *in, char *out, size_t max) {
    if (in[0] == '/') {
        str_copy(out, in, max);
    } else {
        out[0] = '/';
        str_copy(out + 1, in, max - 1);
    }

    /* срезаем конечный слэш, если путь не просто "/" */
    size_t len = str_len(out);
    if (len > 1 && out[len - 1] == '/')
        out[len - 1] = '\0';
}

/* Последний сегмент пути — то, что показывает ls */
static void extract_name(const char *path, char *out, size_t max) {
    size_t len = str_len(path);
    size_t start = 0;
    for (size_t i = 0; i < len; i++)
        if (path[i] == '/') start = i + 1;

    if (start >= len) { str_copy(out, "/", max); return; }
    str_copy(out, path + start, max);
}

/* Родительская директория пути; "/" для всего, что лежит прямо в корне */
static void parent_dir(const char *path, char *out, size_t max) {
    size_t len = str_len(path);
    size_t last_slash = 0;
    int found = 0;

    for (size_t i = 0; i < len; i++)
        if (path[i] == '/') { last_slash = i; found = 1; }

    if (!found || last_slash == 0) {
        str_copy(out, "/", max);
        return;
    }

    size_t n = last_slash;
    if (n >= max) n = max - 1;
    for (size_t i = 0; i < n; i++) out[i] = path[i];
    out[n] = '\0';
}

static ramfs_file_t *find_slot(const char *norm_path) {
    for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++)
        if (g_entries[i].used && str_eq(g_entries[i].path, norm_path))
            return &g_entries[i];
    return NULL;
}

static ramfs_file_t *alloc_entry(void) {
    for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++)
        if (!g_entries[i].used)
            return &g_entries[i];
    return NULL;
}

static int create_entry(const char *path, int is_dir) {
    char norm[RAMFS_MAX_PATH_LEN];
    normalize_path(path, norm, sizeof(norm));

    if (find_slot(norm)) return 0;   /* уже существует — не ошибка (как touch/mkdir -p) */

    ramfs_file_t *e = alloc_entry();
    if (!e) return -1;

    str_copy(e->path, norm, sizeof(e->path));
    extract_name(norm, e->name, sizeof(e->name));
    e->is_dir = is_dir;
    e->size = 0;
    e->used = 1;
    return 0;
}

void ramfs_init(void) {
    for (size_t i = 0; i < RAMFS_MAX_ENTRIES; i++)
        g_entries[i].used = 0;

    create_entry("/", 1);
    create_entry("/welcome.txt", 0);
    ramfs_write("/welcome.txt", "proshivkaOS NEXT RamFS rabotaet.\n", 34);
}

int ramfs_touch(const char *path) { return create_entry(path, 0); }
int ramfs_mkdir(const char *path) { return create_entry(path, 1); }

int ramfs_write(const char *path, const char *data, size_t len) {
    char norm[RAMFS_MAX_PATH_LEN];
    normalize_path(path, norm, sizeof(norm));

    ramfs_file_t *e = find_slot(norm);
    if (!e || e->is_dir) return -1;

    if (len > RAMFS_MAX_FILE_SIZE) len = RAMFS_MAX_FILE_SIZE;
    for (size_t i = 0; i < len; i++) e->data[i] = (uint8_t)data[i];
    e->size = len;
    return 0;
}

const ramfs_file_t *ramfs_lookup(const char *path) {
    char norm[RAMFS_MAX_PATH_LEN];
    normalize_path(path, norm, sizeof(norm));
    return find_slot(norm);
}

int ramfs_is_dir(const char *path) {
    const ramfs_file_t *e = ramfs_lookup(path);
    return e && e->is_dir;
}

size_t ramfs_list_dir(const char *dir_path, const ramfs_file_t **out, size_t max) {
    char norm_dir[RAMFS_MAX_PATH_LEN];
    normalize_path(dir_path, norm_dir, sizeof(norm_dir));

    size_t n = 0;
    for (size_t i = 0; i < RAMFS_MAX_ENTRIES && n < max; i++) {
        if (!g_entries[i].used) continue;
        if (str_eq(g_entries[i].path, "/")) continue;   /* корень сам себя не листит */

        char parent[RAMFS_MAX_PATH_LEN];
        parent_dir(g_entries[i].path, parent, sizeof(parent));
        if (str_eq(parent, norm_dir))
            out[n++] = &g_entries[i];
    }
    return n;
}

size_t ramfs_list(const ramfs_file_t **out, size_t max) {
    return ramfs_list_dir("/", out, max);
}
