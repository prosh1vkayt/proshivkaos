/* arch/arm64/firmware.c — прошивки сопроцессоров, вшитые в образ.
 *
 * Файловой системы у нас нет, а прошивкам сопроцессоров она и не нужна:
 * tools/fwpack.py при сборке складывает их в один блок, блок попадает в
 * образ (build/<цель>/fwblob.S), а здесь файл ищется по имени. */
#include "arm64.h"

extern const uint8_t _fw_blob_start[];

typedef struct {
    char     name[56];
    uint32_t off, size;
} fw_entry_t;

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const uint8_t *fw_find(const char *name, uint32_t *size) {
    const uint8_t *b = _fw_blob_start;
    if (b[0] != 'P' || b[1] != 'O' || b[2] != 'S' || b[3] != 'F' || b[4] != 'W') return 0;
    uint32_t count = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) |
                     ((uint32_t)b[11] << 24);
    const fw_entry_t *e = (const fw_entry_t *)(b + 12);
    for (uint32_t i = 0; i < count; i++, e++) {
        if (name_eq(e->name, name)) {
            if (size) *size = e->size;
            return b + e->off;
        }
    }
    return 0;
}
