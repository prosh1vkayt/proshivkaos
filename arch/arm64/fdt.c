/* arch/arm64/fdt.c — разбор Flattened Device Tree. */
#include "fdt.h"

/* Формат блоба (спецификация Devicetree, глава 5). Заголовок, за ним блок
 * структуры из токенов и блок строк с именами свойств. */
#define FDT_MAGIC        0xD00DFEEDu

#define FDT_BEGIN_NODE   0x00000001u
#define FDT_END_NODE     0x00000002u
#define FDT_PROP         0x00000003u
#define FDT_NOP          0x00000004u
#define FDT_END          0x00000009u

/* Значения по умолчанию из спецификации, если родитель не объявил своих. */
#define DEFAULT_ADDR_CELLS 2
#define DEFAULT_SIZE_CELLS 1

#define MAX_DEPTH 32

static const uint8_t *g_blob = 0;
static uint32_t g_struct_off, g_strings_off, g_struct_size, g_strings_size;
static uint32_t g_total_size;

/* Сборка big-endian слова из отдельных байтов.
 *
 * Именно побайтно, а не разыменованием uint32_t: во-первых, дерево лежит
 * там, куда его положил загрузчик, и выравнивание указателя нам никто не
 * обещал; во-вторых, разбор идёт до включения MMU, где невыровненный
 * доступ падает в исключение, а не отрабатывает медленно. */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static uint32_t str_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Токены и имена выровнены по границе четырёх байт. */
static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

int fdt_valid(void) { return g_blob != 0; }

int fdt_init(const void *blob) {
    g_blob = 0;
    if (!blob) return 0;

    const uint8_t *p = (const uint8_t *)blob;
    if (be32(p) != FDT_MAGIC) return 0;

    uint32_t total       = be32(p + 4);
    uint32_t struct_off  = be32(p + 8);
    uint32_t strings_off = be32(p + 12);
    uint32_t version     = be32(p + 20);
    uint32_t strings_sz  = be32(p + 32);
    uint32_t struct_sz   = be32(p + 36);

    /* Санитарная проверка. Мусорный указатель, случайно совпавший по
       сигнатуре, дальше увёл бы разбор в произвольную память — а мы ещё
       и без обработчика исключений на этом этапе. */
    if (version < 16) return 0;                       /* size_dt_struct появился в v17 */
    if (total < 64 || total > 64u * 1024u * 1024u) return 0;
    if (struct_off  >= total || strings_off >= total) return 0;
    if (struct_sz   >  total || strings_sz  >  total) return 0;
    if (struct_off + struct_sz  > total) return 0;
    if (strings_off + strings_sz > total) return 0;

    g_blob        = p;
    g_total_size  = total;
    g_struct_off  = struct_off;
    g_strings_off = strings_off;
    g_struct_size = struct_sz;
    g_strings_size = strings_sz;
    return 1;
}

/* Имя свойства лежит в блоке строк по смещению из самого свойства.
 * Смещение приходит из самого дерева, то есть ему нельзя доверять:
 * проверяем и по размеру блока строк, и по общему размеру блоба. */
static const char *prop_name(uint32_t nameoff) {
    if (nameoff >= g_strings_size) return "";
    if (g_strings_off + nameoff >= g_total_size) return "";
    return (const char *)(g_blob + g_strings_off + nameoff);
}

/* Есть ли строка needle в списке строк списка compatible.
 * Свойство compatible — это несколько строк подряд, каждая с нулём на
 * конце: "qcom,apq8053-mtp\0qcom,apq8053\0qcom,mtp\0". */
static int compat_list_has(const char *list, uint32_t len, const char *needle) {
    uint32_t i = 0;
    while (i < len) {
        const char *entry = list + i;
        if (str_eq(entry, needle)) return 1;
        i += str_len(entry) + 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * Обход дерева.
 *
 * Единственный проход по блоку структуры со стеком разрядностей адреса и
 * размера: на каждом уровне вложенности они свои, и узел кодирует свой reg
 * теми, что объявил его РОДИТЕЛЬ. Без этого разбора reg у Redmi Note 4
 * читался бы неверно — там корень объявляет двухсловные адреса, а шина
 * /soc внутри переходит на однословные.
 * ------------------------------------------------------------------- */

/* Режимы поиска: по строке compatible либо по абсолютному пути. */
#define MODE_COMPAT 0
#define MODE_PATH   1

/* Совпадает ли имя узла с компонентом пути: либо целиком, либо до '@'. */
static int node_name_matches(const char *node, const char *want, uint32_t want_len) {
    uint32_t i = 0;
    while (i < want_len && node[i] && node[i] == want[i]) i++;

    if (i == want_len && (node[i] == '\0' || node[i] == '@')) return 1;
    return 0;
}

static int walk(int mode, const char *needle, fdt_node_t *out) {
    if (!g_blob) return 0;

    uint32_t off = g_struct_off;
    uint32_t end = g_struct_off + g_struct_size;

    int depth = 0;
    uint8_t ac[MAX_DEPTH], sc[MAX_DEPTH];
    ac[0] = DEFAULT_ADDR_CELLS;
    sc[0] = DEFAULT_SIZE_CELLS;

    /* Для режима поиска по пути: сколько компонентов уже совпало и на
       какой глубине это произошло. */
    const char *path_rest = needle;
    int matched_depth = 0;

    uint32_t cur_props_off = 0;

    while (off + 4 <= end) {
        uint32_t token = be32(g_blob + off);

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)(g_blob + off + 4);
            uint32_t name_len = str_len(name);
            cur_props_off = off + 4 + align4(name_len + 1);

            if (depth + 1 >= MAX_DEPTH) return 0;    /* дерево глубже, чем бывает */
            depth++;
            /* Ребёнок наследует разрядности родителя, пока не объявит свои. */
            ac[depth] = ac[depth - 1];
            sc[depth] = sc[depth - 1];

            if (mode == MODE_PATH && depth == matched_depth + 1) {
                /* Корень пути — пустое имя, его пропускаем. */
                if (depth == 1) {
                    matched_depth = 1;
                    while (*path_rest == '/') path_rest++;
                    if (*path_rest == '\0') {          /* запрошен сам корень */
                        out->props_off  = cur_props_off;
                        out->addr_cells = ac[0];
                        out->size_cells = sc[0];
                        return 1;
                    }
                } else {
                    while (*path_rest == '/') path_rest++;

                    const char *slash = path_rest;
                    while (*slash && *slash != '/') slash++;
                    uint32_t comp_len = (uint32_t)(slash - path_rest);

                    if (comp_len && node_name_matches(name, path_rest, comp_len)) {
                        path_rest += comp_len;
                        matched_depth = depth;

                        while (*path_rest == '/') path_rest++;
                        if (*path_rest == '\0') {       /* путь пройден целиком */
                            out->props_off  = cur_props_off;
                            out->addr_cells = ac[depth - 1];
                            out->size_cells = sc[depth - 1];
                            return 1;
                        }
                    }
                }
            }

            off = cur_props_off;
            continue;
        }

        if (token == FDT_PROP) {
            if (off + 12 > end) return 0;
            uint32_t len     = be32(g_blob + off + 4);
            uint32_t nameoff = be32(g_blob + off + 8);
            const uint8_t *data = g_blob + off + 12;
            const char *name = prop_name(nameoff);

            /* Разрядности, объявленные ЭТИМ узлом, действуют на его детей. */
            if (len >= 4 && str_eq(name, "#address-cells")) ac[depth] = (uint8_t)be32(data);
            if (len >= 4 && str_eq(name, "#size-cells"))    sc[depth] = (uint8_t)be32(data);

            if (mode == MODE_COMPAT && str_eq(name, "compatible") &&
                compat_list_has((const char *)data, len, needle)) {
                out->props_off  = cur_props_off;
                out->addr_cells = ac[depth - 1];
                out->size_cells = sc[depth - 1];
                return 1;
            }

            off += 12 + align4(len);
            continue;
        }

        if (token == FDT_END_NODE) {
            if (mode == MODE_PATH && depth == matched_depth && depth > 1) {
                /* Вышли из узла, который считали совпавшим — значит дальше
                   по этой ветке нужного пути нет. */
                return 0;
            }
            if (depth > 0) depth--;
            off += 4;
            continue;
        }

        if (token == FDT_NOP) { off += 4; continue; }
        if (token == FDT_END)  break;

        return 0;    /* неизвестный токен — дерево испорчено */
    }
    return 0;
}

int fdt_find_compatible(const char *compat, fdt_node_t *out) {
    return walk(MODE_COMPAT, compat, out);
}

int fdt_path(const char *path, fdt_node_t *out) {
    return walk(MODE_PATH, path, out);
}

const void *fdt_prop(const fdt_node_t *node, const char *name, uint32_t *len) {
    if (!g_blob || !node) return 0;

    uint32_t off = node->props_off;
    uint32_t end = g_struct_off + g_struct_size;

    /* Свойства узла идут подряд сразу за его именем и заканчиваются, как
       только встретится вложенный узел или конец текущего. */
    while (off + 4 <= end) {
        uint32_t token = be32(g_blob + off);

        if (token == FDT_NOP) { off += 4; continue; }
        if (token != FDT_PROP) return 0;          /* BEGIN_NODE или END_NODE */

        if (off + 12 > end) return 0;
        uint32_t plen    = be32(g_blob + off + 4);
        uint32_t nameoff = be32(g_blob + off + 8);

        if (str_eq(prop_name(nameoff), name)) {
            if (len) *len = plen;
            return g_blob + off + 12;
        }
        off += 12 + align4(plen);
    }
    return 0;
}

int fdt_prop_u32(const fdt_node_t *node, const char *name, uint32_t *out) {
    uint32_t len = 0;
    const void *p = fdt_prop(node, name, &len);
    if (!p || len < 4) return 0;
    *out = be32((const uint8_t *)p);
    return 1;
}

const char *fdt_prop_str(const fdt_node_t *node, const char *name) {
    uint32_t len = 0;
    const void *p = fdt_prop(node, name, &len);
    if (!p || len == 0) return 0;
    return (const char *)p;
}

/* Собрать 64-битное значение из cells слов по 32 бита (старшее первым). */
static uint64_t read_cells(const uint8_t *p, uint8_t cells) {
    uint64_t v = 0;
    for (uint8_t i = 0; i < cells; i++)
        v = (v << 32) | be32(p + i * 4);
    return v;
}

int fdt_reg(const fdt_node_t *node, int index, uint64_t *addr, uint64_t *size) {
    uint32_t len = 0;
    const uint8_t *p = (const uint8_t *)fdt_prop(node, "reg", &len);
    if (!p) return 0;

    uint8_t ac = node->addr_cells ? node->addr_cells : DEFAULT_ADDR_CELLS;
    uint8_t sc = node->size_cells;
    if (ac > 2 || sc > 2) return 0;              /* больше 64 бит не бывает */

    uint32_t entry = (uint32_t)(ac + sc) * 4u;
    if (entry == 0) return 0;
    if ((uint32_t)(index + 1) * entry > len) return 0;

    const uint8_t *e = p + (uint32_t)index * entry;
    if (addr) *addr = read_cells(e, ac);
    if (size) *size = sc ? read_cells(e + ac * 4, sc) : 0;
    return 1;
}

const char *fdt_model(void) {
    fdt_node_t root;
    if (!fdt_path("/", &root)) return 0;
    return fdt_prop_str(&root, "model");
}
