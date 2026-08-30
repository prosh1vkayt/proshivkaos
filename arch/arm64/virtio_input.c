/* arch/arm64/virtio_input.c — драйвер устройств ввода virtio-mmio.
 *
 * Это ARM64-замена связке arch/x86/mouse.c + arch/x86/keyboard.c. Разница
 * принципиальная, и она же — главная причина, почему интерфейс пришлось
 * переделывать под тач:
 *
 *   PS/2-мышь шлёт ОТНОСИТЕЛЬНЫЕ смещения ("сдвинулись на 3 вправо") —
 *   у неё есть курсор, который где-то стоит между движениями.
 *   Тачскрин шлёт АБСОЛЮТНУЮ точку ("палец сейчас вот здесь") — и между
 *   касаниями у него нет вообще никакого состояния.
 *
 * Под QEMU роль тачскрина играет virtio-tablet-device: он, как и настоящий
 * сенсор телефона, репортит абсолютные координаты в собственной сетке
 * (обычно 0..32767), которую мы масштабируем в пиксели экрана.
 *
 * ДВА ТРАНСПОРТА. Спецификация virtio знает две несовместимые версии
 * virtio-mmio, и обе живы:
 *   версия 1 ("legacy")  — очередь описывается ОДНИМ адресом: номером
 *                          страницы, с которой начинается непрерывный
 *                          блок из трёх колец подряд;
 *   версия 2 ("modern")  — у каждого из трёх колец свой 64-битный адрес.
 * Машина QEMU virt по умолчанию отдаёт именно версию 1 (свойство
 * force-legacy у транспорта включено), а вот загрузчики реальных
 * устройств и другие платы — чаще версию 2. Поддержаны обе: разница
 * упрятана в setup_device(), остальной код о ней не знает.
 *
 * Прерывания не используем — всё опрашивается в главном цикле, ровно как
 * PS/2 на x86.
 */
#include "arm64.h"

/* ---- Регистры транспорта, общие для обеих версий ---- */
#define VM_MAGIC            0x000   /* 0x74726976 = "virt" */
#define VM_VERSION          0x004
#define VM_DEVICE_ID        0x008
#define VM_DEV_FEAT         0x010
#define VM_DEV_FEAT_SEL     0x014
#define VM_DRV_FEAT         0x020
#define VM_DRV_FEAT_SEL     0x024
#define VM_QUEUE_SEL        0x030
#define VM_QUEUE_NUM_MAX    0x034
#define VM_QUEUE_NUM        0x038
#define VM_QUEUE_NOTIFY     0x050
#define VM_INT_STATUS       0x060
#define VM_INT_ACK          0x064
#define VM_STATUS           0x070
#define VM_CONFIG           0x100

/* ---- Только версия 1 (legacy) ---- */
#define VM_GUEST_PAGE_SIZE  0x028
#define VM_QUEUE_ALIGN      0x03c
#define VM_QUEUE_PFN        0x040

/* ---- Только версия 2 (modern) ---- */
#define VM_QUEUE_READY      0x044
#define VM_QUEUE_DESC_LO    0x080
#define VM_QUEUE_DESC_HI    0x084
#define VM_QUEUE_DRIVER_LO  0x090
#define VM_QUEUE_DRIVER_HI  0x094
#define VM_QUEUE_DEVICE_LO  0x0a0
#define VM_QUEUE_DEVICE_HI  0x0a4

#define VIRTIO_MAGIC        0x74726976
#define VIRTIO_ID_INPUT     18

#define ST_ACKNOWLEDGE      1
#define ST_DRIVER           2
#define ST_DRIVER_OK        4
#define ST_FEATURES_OK      8       /* только в версии 2 */

#define VRING_DESC_F_WRITE  2       /* дескриптор — для записи УСТРОЙСТВОМ */

/* Селекторы конфигурационного пространства virtio-input */
#define CFG_ABS_INFO        0x12
#define ABS_AXIS_X          0x00
#define ABS_AXIS_Y          0x01

#define VQ_SIZE      64             /* длина очереди событий на устройство */
#define VQ_ALIGN     4096           /* выравнивание used-кольца в legacy    */
#define VQ_RING_SIZE 8192           /* с запасом на все три кольца          */
#define MAX_DEVICES  4

/* packed этим структурам не нужен: раскладка полей в спецификации virtio
 * совпадает с естественным выравниванием C, а packed заставлял компилятор
 * считать указатели на их поля потенциально невыровненными. */
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];      /* длина = qsize, за ним ещё uint16_t used_event */
} vring_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[];   /* длина = qsize, за ним uint16_t avail_event */
} vring_used_t;

typedef struct {
    /* Все три кольца лежат в одном непрерывном блоке, выровненном на
       страницу. Так требует legacy-транспорт (он знает только адрес
       начала блока), а modern такой раскладке не мешает — ему всё равно,
       лишь бы каждое кольцо было выровнено. */
    uint8_t ring[VQ_RING_SIZE] __attribute__((aligned(VQ_ALIGN)));

    vinput_event_t buf[VQ_SIZE] __attribute__((aligned(64)));

    vring_desc_t  *desc;
    vring_avail_t *avail;
    vring_used_t  *used;

    uint64_t base;
    uint32_t qsize;
    uint16_t last_used;
    uint16_t avail_idx;

    int in_use;
    int has_abs;
    int abs_max_x, abs_max_y;
} vinput_dev_t;

static vinput_dev_t g_devices[MAX_DEVICES];
static int g_device_count = 0;
static int g_abs_device   = -1;

/* Хвостовые поля колец (used_event / avail_event) в спецификации идут
 * сразу за массивом переменной длины — обращаемся к ним по смещению. */
static inline uint16_t *avail_ring(vinput_dev_t *d) { return d->avail->ring; }
static inline vring_used_elem_t *used_ring(vinput_dev_t *d) { return d->used->ring; }

/* Чтение abs-info из конфигурации: "в какой сетке этот сенсор меряет". */
static int read_abs_max(uint64_t base, uint8_t axis, int *out_max) {
    mmio_write8(base + VM_CONFIG + 0, CFG_ABS_INFO);
    mmio_write8(base + VM_CONFIG + 1, axis);
    isb();

    uint8_t size = mmio_read8(base + VM_CONFIG + 2);
    if (size == 0) return 0;   /* устройство такой оси не имеет */

    /* struct virtio_input_absinfo { le32 min, max, fuzz, flat, res; }
       начинается со смещения 8; нас интересует max. */
    *out_max = (int)mmio_read32(base + VM_CONFIG + 8 + 4);
    return 1;
}

static int setup_device(vinput_dev_t *d, uint64_t base, uint32_t version) {
    d->base = base;

    mmio_write32(base + VM_STATUS, 0);                       /* сброс */
    mmio_write32(base + VM_STATUS, ST_ACKNOWLEDGE);
    mmio_write32(base + VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER);

    if (version == 1) {
        /* В legacy 32 бита возможностей и никакого VIRTIO_F_VERSION_1.
           Ничего дополнительного нам не нужно — соглашаемся на пустой
           набор, все базовые механизмы очереди работают и без него. */
        mmio_write32(base + VM_DEV_FEAT_SEL, 0);
        (void)mmio_read32(base + VM_DEV_FEAT);
        mmio_write32(base + VM_DRV_FEAT_SEL, 0);
        mmio_write32(base + VM_DRV_FEAT, 0);
    } else {
        /* В modern обязателен VIRTIO_F_VERSION_1 — это бит 32, то есть
           нулевой бит второго 32-битного слова возможностей. */
        mmio_write32(base + VM_DEV_FEAT_SEL, 1);
        uint32_t feat_hi = mmio_read32(base + VM_DEV_FEAT);

        mmio_write32(base + VM_DRV_FEAT_SEL, 1);
        mmio_write32(base + VM_DRV_FEAT, feat_hi & 1u);
        mmio_write32(base + VM_DRV_FEAT_SEL, 0);
        mmio_write32(base + VM_DRV_FEAT, 0);

        mmio_write32(base + VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK);
        if (!(mmio_read32(base + VM_STATUS) & ST_FEATURES_OK))
            return 0;
    }

    /* --- Очередь 0 (eventq): устройство складывает в неё события ввода --- */
    mmio_write32(base + VM_QUEUE_SEL, 0);
    uint32_t max = mmio_read32(base + VM_QUEUE_NUM_MAX);
    if (max == 0) return 0;

    d->qsize = (max < VQ_SIZE) ? max : VQ_SIZE;
    mmio_write32(base + VM_QUEUE_NUM, d->qsize);

    /* Раскладка трёх колец внутри блока. Смещения задаёт СПЕЦИФИКАЦИЯ, а
       не мы: в legacy устройство вычисляет адреса avail и used само,
       исходя из размера очереди и QueueAlign, поэтому подогнать их под
       себя нельзя — можно только повторить ту же формулу. */
    uint32_t avail_off = 16u * d->qsize;
    uint32_t used_off  = (avail_off + 6u + 2u * d->qsize + (VQ_ALIGN - 1)) & ~(uint32_t)(VQ_ALIGN - 1);
    if (used_off + 6u + 8u * d->qsize > VQ_RING_SIZE)
        return 0;   /* очередь не помещается в отведённый блок */

    d->desc  = (vring_desc_t  *)(d->ring);
    d->avail = (vring_avail_t *)(d->ring + avail_off);
    d->used  = (vring_used_t  *)(d->ring + used_off);

    uint64_t ring_pa = (uint64_t)(uintptr_t)d->ring;

    if (version == 1) {
        mmio_write32(base + VM_GUEST_PAGE_SIZE, VQ_ALIGN);
        mmio_write32(base + VM_QUEUE_ALIGN, VQ_ALIGN);
        /* PFN — номер страницы, а не адрес: устройство домножит его на
           GuestPageSize обратно. */
        mmio_write32(base + VM_QUEUE_PFN, (uint32_t)(ring_pa / VQ_ALIGN));
    } else {
        uint64_t avail_pa = ring_pa + avail_off;
        uint64_t used_pa  = ring_pa + used_off;

        mmio_write32(base + VM_QUEUE_DESC_LO,   (uint32_t)ring_pa);
        mmio_write32(base + VM_QUEUE_DESC_HI,   (uint32_t)(ring_pa >> 32));
        mmio_write32(base + VM_QUEUE_DRIVER_LO, (uint32_t)avail_pa);
        mmio_write32(base + VM_QUEUE_DRIVER_HI, (uint32_t)(avail_pa >> 32));
        mmio_write32(base + VM_QUEUE_DEVICE_LO, (uint32_t)used_pa);
        mmio_write32(base + VM_QUEUE_DEVICE_HI, (uint32_t)(used_pa >> 32));
    }

    /* Каждый дескриптор указывает на восьмибайтный приёмник одного
       события и помечен "устройство сюда ПИШЕТ". */
    for (uint32_t i = 0; i < d->qsize; i++) {
        d->desc[i].addr  = (uint64_t)(uintptr_t)&d->buf[i];
        d->desc[i].len   = sizeof(vinput_event_t);
        d->desc[i].flags = VRING_DESC_F_WRITE;
        d->desc[i].next  = 0;
        avail_ring(d)[i] = (uint16_t)i;
    }
    d->avail->flags = 0;
    d->avail->idx   = (uint16_t)d->qsize;   /* все буферы отданы устройству */
    d->avail_idx    = (uint16_t)d->qsize;
    d->last_used    = 0;
    d->used->idx    = 0;
    d->used->flags  = 0;

    if (version != 1)
        mmio_write32(base + VM_QUEUE_READY, 1);

    /* Абсолютные оси спрашиваем до DRIVER_OK: конфигурация доступна уже
       на этом этапе, а знать сетку сенсора надо до первого события. */
    d->has_abs = 0;
    if (read_abs_max(base, ABS_AXIS_X, &d->abs_max_x) &&
        read_abs_max(base, ABS_AXIS_Y, &d->abs_max_y)) {
        if (d->abs_max_x > 0 && d->abs_max_y > 0)
            d->has_abs = 1;
    }

    arch_dcache_clean(d->ring, VQ_RING_SIZE);
    arch_dcache_clean(d->buf, sizeof(d->buf));
    dsb_sy();

    mmio_write32(base + VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER |
                                   (version == 1 ? 0 : ST_FEATURES_OK) | ST_DRIVER_OK);
    mmio_write32(base + VM_QUEUE_NOTIFY, 0);   /* "буферы готовы, забирай" */

    d->in_use = 1;
    return 1;
}

void virtio_input_init(void) {
    g_device_count = 0;
    g_abs_device   = -1;

    /* QEMU virt раскладывает 32 слота virtio-mmio подряд с шагом 0x200 и
       заполняет их с конца. Пустой слот отвечает нулевым DeviceID. */
    for (int slot = 0; slot < VIRT_VIRTIO_SLOTS && g_device_count < MAX_DEVICES; slot++) {
        uint64_t base = VIRT_VIRTIO_BASE + (uint64_t)slot * VIRT_VIRTIO_STRIDE;

        if (mmio_read32(base + VM_MAGIC) != VIRTIO_MAGIC) continue;

        uint32_t version = mmio_read32(base + VM_VERSION);
        if (version != 1 && version != 2) continue;
        if (mmio_read32(base + VM_DEVICE_ID) != VIRTIO_ID_INPUT) continue;

        vinput_dev_t *d = &g_devices[g_device_count];
        if (!setup_device(d, base, version)) {
            uart_write("virtio-input: slot ");
            uart_write_hex((uint64_t)slot);
            uart_write(" ne inicializirovan\n");
            continue;
        }

        uart_write("virtio-input: slot ");
        uart_write_hex((uint64_t)slot);
        uart_write(d->has_abs ? " tachskrin (abs)\n" : " klaviatura (key)\n");

        if (d->has_abs && g_abs_device < 0)
            g_abs_device = g_device_count;

        g_device_count++;
    }

    if (g_device_count == 0)
        uart_write("virtio-input: ustroystv vvoda ne naydeno\n");
}

int virtio_input_device_count(void) { return g_device_count; }

int virtio_input_abs_range(int *max_x, int *max_y) {
    if (g_abs_device < 0) return 0;
    *max_x = g_devices[g_abs_device].abs_max_x;
    *max_y = g_devices[g_abs_device].abs_max_y;
    return 1;
}

static int poll_device(vinput_dev_t *d, vinput_event_t *ev) {
    if (!d->in_use) return 0;

    /* used-кольцо пишет устройство напрямую в ОЗУ — наша копия в кэше
       устарела по определению, её надо выбросить перед чтением. */
    arch_dcache_invalidate(d->used, 8 + 8 * d->qsize);

    if (d->last_used == d->used->idx)
        return 0;

    uint16_t slot = (uint16_t)(d->last_used % d->qsize);
    uint32_t id   = used_ring(d)[slot].id;
    if (id >= d->qsize) {          /* мусор в кольце — пересинхронизируемся */
        d->last_used = d->used->idx;
        return 0;
    }

    arch_dcache_invalidate(&d->buf[id], sizeof(vinput_event_t));
    *ev = d->buf[id];

    d->last_used++;

    /* Буфер обработан — сразу возвращаем его устройству, иначе очередь
       быстро иссякнет и события начнут теряться при активном вводе. */
    avail_ring(d)[d->avail_idx % d->qsize] = (uint16_t)id;
    d->avail_idx++;
    dmb_sy();                       /* сначала слот, потом индекс — не наоборот */
    d->avail->idx = d->avail_idx;

    arch_dcache_clean(d->avail, 6 + 2 * d->qsize);
    dsb_sy();
    mmio_write32(d->base + VM_QUEUE_NOTIFY, 0);

    /* Прерывания замаскированы, но флаг всё равно подтверждаем — иначе
       устройство считает, что предыдущее уведомление не обработано. */
    uint32_t status = mmio_read32(d->base + VM_INT_STATUS);
    if (status) mmio_write32(d->base + VM_INT_ACK, status);

    return 1;
}

int virtio_input_poll(vinput_event_t *ev) {
    for (int i = 0; i < g_device_count; i++)
        if (poll_device(&g_devices[i], ev))
            return 1;
    return 0;
}
