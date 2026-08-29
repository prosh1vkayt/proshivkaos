/* fs/ramfs.h — RamFS с директориями (плоский список записей с полными
 * путями вместо настоящего дерева — проще, но ведёт себя как обычная ФС
 * снаружи: ls/cd/mkdir/pwd работают ожидаемо).
 *
 * ОБРАТНАЯ СОВМЕСТИМОСТЬ: текстовый shell.c (make run-kernel) вызывает
 * ramfs_touch("name")/ramfs_lookup("name") без слэшей — такие имена
 * трактуются как находящиеся в корне "/", ровно как раньше, когда
 * директорий не было вообще. ramfs_list() тоже сохранён — листит корень.
 */
#ifndef PROSHIVKAOS_RAMFS_H
#define PROSHIVKAOS_RAMFS_H

#include "hal.h"

#define RAMFS_MAX_ENTRIES    48
#define RAMFS_MAX_FILES      RAMFS_MAX_ENTRIES   /* алиас — старое имя, используется в shell.c */
#define RAMFS_MAX_NAME_LEN   32    /* последний сегмент пути ("файл.txt") */
#define RAMFS_MAX_PATH_LEN   96    /* полный путь ("/root/docs/файл.txt") */
#define RAMFS_MAX_FILE_SIZE  4096

typedef struct {
    char path[RAMFS_MAX_PATH_LEN];  /* полный абсолютный путь, "/" для корня */
    char name[RAMFS_MAX_NAME_LEN];  /* последний сегмент — то, что показывает ls */
    int  is_dir;
    uint8_t data[RAMFS_MAX_FILE_SIZE];  /* не используется для директорий */
    size_t size;
    int used;
} ramfs_file_t;

void ramfs_init(void);   /* создаёт "/" и домашний каталог "/root" + welcome.txt */

/* Пути без ведущего "/" трактуются как находящиеся в корне "/" —
 * для реальной работы с текущим каталогом резолвьте путь на стороне
 * вызывающего кода (см. gui/terminal_app.c) и передавайте уже полный путь. */
int  ramfs_touch(const char *path);                        /* создать пустой файл; 0 = ok */
int  ramfs_mkdir(const char *path);                         /* создать директорию; 0 = ok */
int  ramfs_write(const char *path, const char *data, size_t len);
const ramfs_file_t *ramfs_lookup(const char *path);
int  ramfs_is_dir(const char *path);                         /* 1, если путь существует и это директория */

size_t ramfs_list(const ramfs_file_t **out, size_t max);              /* содержимое "/" — для обратной совместимости */
size_t ramfs_list_dir(const char *dir_path, const ramfs_file_t **out, size_t max);

#endif
