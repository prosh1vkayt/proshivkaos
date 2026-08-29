/* arch/arm64/ramfb.c — вывод изображения через ramfb.
 *
 * ramfb — самый простой способ получить экран на ARM64 из всех
 * существующих: мы просто сообщаем QEMU "вот адрес буфера в моей памяти,
 * вот его размеры и формат — показывай его как экран". Дальше QEMU сам
 * читает этот буфер и рисует окно. Никакого драйвера видеокарты,
 * прерываний и очередей команд, в отличие от virtio-gpu.
 *
 * Прямая аналогия с x86-путём: в arch/x86/vbe.c мы точно так же говорим
 * видеокарте "хочу 640x480x8" и получаем адрес линейного буфера. Разница
 * только в том, что там адрес выдаёт железо, а тут его выдаём мы сами.
 *
 * ВАЖНО: устройство должно быть подключено к машине (-device ramfb),
 * иначе файла "etc/ramfb" в каталоге fw_cfg просто не окажется, и мы
 * честно вернём 0 — так же, как vbe_set_mode() возвращает 0, когда VBE
 * не найден.
 */
#include "arm64.h"

/* DRM_FORMAT_XRGB8888 — четырёхбуквенный код 'XR24' в little-endian.
 * Тот же формат, в котором работает подавляющее большинство дисплеев
 * телефонов: 4 байта на пиксель, старший байт не используется. */
#define FOURCC_XRGB8888 0x34325258

typedef struct {
    uint64_t addr;      /* big-endian: физический адрес буфера */
    uint32_t fourcc;    /* big-endian: формат пикселя           */
    uint32_t flags;     /* big-endian: зарезервировано, 0       */
    uint32_t width;     /* big-endian */
    uint32_t height;    /* big-endian */
    uint32_t stride;    /* big-endian: длина строки в байтах    */
} __attribute__((packed)) ramfb_cfg_t;

static ramfb_cfg_t g_cfg __attribute__((aligned(64)));

int ramfb_setup(void *framebuffer, int width, int height) {
    uint16_t select;
    uint32_t size;

    if (!fwcfg_find_file("etc/ramfb", &select, &size))
        return 0;

    if (size != sizeof(g_cfg))
        return 0;   /* формат структуры изменился — лучше не гадать */

    g_cfg.addr   = bswap64((uint64_t)(uintptr_t)framebuffer);
    g_cfg.fourcc = bswap32(FOURCC_XRGB8888);
    g_cfg.flags  = 0;
    g_cfg.width  = bswap32((uint32_t)width);
    g_cfg.height = bswap32((uint32_t)height);
    g_cfg.stride = bswap32((uint32_t)width * 4);

    return fwcfg_dma_write(select, &g_cfg, sizeof(g_cfg));
}
