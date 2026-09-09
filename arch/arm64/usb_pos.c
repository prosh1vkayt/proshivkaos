/* arch/arm64/usb_pos.c — как система выглядит со стороны компьютера.
 *
 * Контроллер (usb_dwc3.c) умеет принимать и отдавать байты, но ничего не
 * знает о том, ЧТО именно. Всё, что делает подключённое устройство
 * устройством, — набор описаний, которые оно выдаёт хосту при
 * подключении, — лежит здесь.
 *
 * ЧТО МЫ ИЗ СЕБЯ СТРОИМ. Устройство с одним интерфейсом собственного, не
 * стандартного вида и двумя точками потока данных: из телефона в
 * компьютер идёт журнал загрузки, обратно — команды. Именно журнал был
 * целью: до сих пор его приходилось вытаскивать через recovery, теряя по
 * несколько минут и живого человека на каждый заход.
 *
 * ПОЧЕМУ ТАКИЕ НОМЕРА. Идентификатор изготовителя 0x1209 выделен
 * сообществом pid.codes для открытых любительских проектов, а номер
 * изделия 0x0001 в нём отведён под пробы. Брать чужой номер настоящего
 * изготовителя нельзя: устройство представлялось бы не тем, что оно есть,
 * и операционная система на том конце подобрала бы чужой драйвер.
 *
 * ПРО WINDOWS. macOS и Linux открывают устройство неизвестного вида без
 * всякой подготовки — достаточно libusb. Windows так не умеет: пока
 * драйвер не назначен, устройство висит с восклицательным знаком.
 * Обходится это описанием "Microsoft OS 2.0", которое Windows
 * запрашивает отдельно и по которому сама подставляет WinUSB. Оно ниже —
 * блок из ста семидесяти восьми байт, где важен каждый счётчик длины.
 */
#include "arm64.h"

#define VENDOR_ID       0x1209
#define PRODUCT_ID      0x0001

/* Собственный код запроса, по которому Windows спрашивает описание для
   себя. Значение произвольное, но должно совпадать в двух местах. */
#define MS_VENDOR_CODE  0x01

/* Типы описаний */
#define DESC_DEVICE     1
#define DESC_CONFIG     2
#define DESC_STRING     3
#define DESC_QUALIFIER  6
#define DESC_BOS        15

static uint8_t  g_config = 0;
static int      g_speed  = 0;

/* ---------------- Описания ---------------- */

static const uint8_t desc_device[18] = {
    18, DESC_DEVICE,
    0x10, 0x02,             /* версия протокола 2.1: без неё Windows не
                               спросит про описание для себя            */
    0x00, 0x00, 0x00,       /* вид устройства задаётся интерфейсом      */
    64,                     /* размер пакета управляющей точки          */
    VENDOR_ID & 0xFF, VENDOR_ID >> 8,
    PRODUCT_ID & 0xFF, PRODUCT_ID >> 8,
    0x00, 0x01,             /* версия изделия 1.0                       */
    1, 2, 3,                /* строки: изготовитель, изделие, номер      */
    1                       /* одна конфигурация                        */
};

#define CONFIG_LEN (9 + 9 + 7 + 7)

static const uint8_t desc_config[CONFIG_LEN] = {
    /* конфигурация */
    9, DESC_CONFIG, CONFIG_LEN & 0xFF, CONFIG_LEN >> 8,
    1,                      /* один интерфейс                           */
    1,                      /* её номер                                 */
    0,                      /* без названия                             */
    0x80,                   /* питание от шины, без пробуждения хоста   */
    250,                    /* до 500 мА                                */

    /* интерфейс: вид собственный, драйвер подбирается по описанию ниже */
    9, 4, 0, 0, 2, 0xFF, 0xFF, 0x00, 4,

    /* точка к хосту: журнал */
    7, 5, 0x81, 0x02, 0x00, 0x02, 0x00,
    /* точка от хоста: команды */
    7, 5, 0x01, 0x02, 0x00, 0x02, 0x00
};

/* Набор возможностей. Нужен только ради последней записи — она говорит
   Windows, каким кодом запрашивать описание для себя. */
static const uint8_t desc_bos[33] = {
    5, DESC_BOS, 33, 0, 1,
    /* возможность вида "платформа" */
    28, 16, 5, 0,
    /* опознаватель Microsoft OS 2.0 — значение задано Microsoft */
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,     /* начиная с Windows 8.1                */
    0xB2, 0x00,                 /* длина описания ниже: 178 байт        */
    MS_VENDOR_CODE, 0x00
};

/* Описание для Windows. Говорит: этому интерфейсу подходит WinUSB, и вот
 * опознаватель, по которому программы будут его находить.
 *
 * Все длины внутри посчитаны и перепроверены: заголовок 10, поднабор
 * конфигурации 168, поднабор функции 160, признак совместимости 20,
 * свойство реестра 132. Ошибка в любом счётчике — и Windows молча
 * откажется разбирать весь блок. */
static const uint8_t desc_msos20[178] = {
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x06, 0xB2, 0x00, 0x08, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xA8, 0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00,
    0xA0, 0x00, 0x14, 0x00, 0x03, 0x00, 0x57, 0x49, 0x4E, 0x55, 0x53, 0x42,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84, 0x00,
    0x04, 0x00, 0x07, 0x00, 0x2A, 0x00, 0x44, 0x00, 0x65, 0x00, 0x76, 0x00,
    0x69, 0x00, 0x63, 0x00, 0x65, 0x00, 0x49, 0x00, 0x6E, 0x00, 0x74, 0x00,
    0x65, 0x00, 0x72, 0x00, 0x66, 0x00, 0x61, 0x00, 0x63, 0x00, 0x65, 0x00,
    0x47, 0x00, 0x55, 0x00, 0x49, 0x00, 0x44, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x7B, 0x00, 0x36, 0x00, 0x46, 0x00, 0x32, 0x00, 0x43, 0x00,
    0x38, 0x00, 0x41, 0x00, 0x33, 0x00, 0x31, 0x00, 0x2D, 0x00, 0x37, 0x00,
    0x42, 0x00, 0x34, 0x00, 0x44, 0x00, 0x2D, 0x00, 0x34, 0x00, 0x45, 0x00,
    0x32, 0x00, 0x41, 0x00, 0x2D, 0x00, 0x39, 0x00, 0x43, 0x00, 0x35, 0x00,
    0x35, 0x00, 0x2D, 0x00, 0x35, 0x00, 0x30, 0x00, 0x34, 0x00, 0x46, 0x00,
    0x35, 0x00, 0x33, 0x00, 0x35, 0x00, 0x35, 0x00, 0x35, 0x00, 0x33, 0x00,
    0x34, 0x00, 0x32, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Строки. В шине они хранятся двухбайтовыми знаками, поэтому собираются
   на лету из обычных. */
static const char *const g_strings[] = {
    0,                              /* нулевая — список языков          */
    "proshivkaOS",
    "POS USB Composite Device",
    "mido-0001",
    "POS debug log"
};
#define STRING_COUNT (int)(sizeof(g_strings) / sizeof(g_strings[0]))

static uint8_t g_strbuf[128];

static int build_string(int index, const uint8_t **out) {
    if (index == 0) {
        g_strbuf[0] = 4; g_strbuf[1] = DESC_STRING;
        g_strbuf[2] = 0x09; g_strbuf[3] = 0x04;   /* английский */
        *out = g_strbuf;
        return 4;
    }
    if (index >= STRING_COUNT) return 0;

    const char *s = g_strings[index];
    int n = 0;
    while (s[n] && (2 * n + 2 + 2) < (int)sizeof(g_strbuf)) n++;

    g_strbuf[0] = (uint8_t)(2 + 2 * n);
    g_strbuf[1] = DESC_STRING;
    for (int i = 0; i < n; i++) {
        g_strbuf[2 + 2 * i] = (uint8_t)s[i];
        g_strbuf[3 + 2 * i] = 0;
    }
    *out = g_strbuf;
    return 2 + 2 * n;
}

/* ---------------- Кольцо журнала ---------------- */

/* Всё, что система печатает на экран, копится здесь и уходит в компьютер
 * по мере готовности точки. Кольцо, а не очередь: если компьютер не
 * подключён или читает медленно, старые записи вытесняются новыми, и
 * система от этого не встаёт. Журнал на экране и в сохраняемой области
 * при этом остаётся полным — здесь только то, что успели передать. */
#define LOG_RING 16384
static char     g_ring[LOG_RING];
static uint32_t g_head = 0, g_tail = 0;
static int      g_in_pull = 0;

void usb_log_write(const char *s) {
    if (g_in_pull) return;          /* не наматывать собственные жалобы */
    while (*s) {
        uint32_t next = (g_head + 1) % LOG_RING;
        if (next == g_tail) g_tail = (g_tail + 1) % LOG_RING;  /* вытесняем */
        g_ring[g_head] = *s++;
        g_head = next;
    }
}

int usb_pos_pull(uint8_t *dst, int max) {
    int n = 0;
    g_in_pull = 1;
    while (n < max && g_tail != g_head) {
        dst[n++] = (uint8_t)g_ring[g_tail];
        g_tail = (g_tail + 1) % LOG_RING;
    }
    g_in_pull = 0;
    return n;
}

/* ---------------- Запросы ---------------- */

void usb_pos_reset(void) {
    g_config = 0;
    usb_dwc3_set_configured(0);
}

void usb_pos_speed(int mbit) {
    g_speed = mbit;
}

void usb_pos_ctrl_data(const uint8_t *data, int len) {
    (void)data; (void)len;      /* управляющих записей у нас пока нет */
}

/* Команды от компьютера. Пока их две, и обе односимвольные — этого
   достаточно, чтобы посмотреть, что канал работает в обе стороны. */
void usb_pos_received(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        if (data[i] == 'p') usb_log_write("POS: ping\n");
        if (data[i] == 'v') {
            usb_log_write("POS: proshivkaOS NEXT, mido, skorost ");
            usb_log_write(g_speed >= 480 ? "vysokaya\n" : "polnaya\n");
        }
    }
}

static int zero_reply(const uint8_t **data, int *len) {
    static const uint8_t zeros[2] = { 0, 0 };
    *data = zeros;
    *len = 2;
    return 1;
}

int usb_pos_setup(const uint8_t *req, const uint8_t **data, int *len,
                  uint8_t *set_addr) {
    uint8_t  type    = req[0];
    uint8_t  request = req[1];
    uint16_t value   = (uint16_t)(req[2] | (req[3] << 8));
    uint16_t index   = (uint16_t)(req[4] | (req[5] << 8));

    *data = 0;
    *len = 0;
    *set_addr = 0;

    /* Собственный запрос: описание для Windows. */
    if ((type & 0x60) == 0x40 && request == MS_VENDOR_CODE && index == 7) {
        *data = desc_msos20;
        *len = (int)sizeof(desc_msos20);
        return 1;
    }

    if ((type & 0x60) != 0x00) return 0;    /* прочие нестандартные — отказ */

    switch (request) {
    case 0x06: {                            /* дай описание */
        int dtype = value >> 8;
        int dindex = value & 0xFF;
        if (dtype == DESC_DEVICE) {
            *data = desc_device; *len = (int)sizeof(desc_device); return 1;
        }
        if (dtype == DESC_CONFIG) {
            *data = desc_config; *len = (int)sizeof(desc_config); return 1;
        }
        if (dtype == DESC_STRING) {
            *len = build_string(dindex, data);
            return *len > 0;
        }
        if (dtype == DESC_BOS) {
            *data = desc_bos; *len = (int)sizeof(desc_bos); return 1;
        }
        /* Описание "каким было бы на другой скорости" мы не даём: работаем
           только на высокой, и подменять нечего. Отказ — законный ответ. */
        if (dtype == DESC_QUALIFIER) return 0;
        return 0;
    }

    case 0x05:                              /* назначить адрес */
        *set_addr = (uint8_t)(value & 0x7F);
        return 1;

    case 0x09:                              /* выбрать конфигурацию */
        g_config = (uint8_t)value;
        usb_dwc3_set_configured(g_config != 0);
        return 1;

    case 0x08:                              /* какая конфигурация выбрана */
        *data = &g_config; *len = 1; return 1;

    case 0x00:                              /* состояние */
        return zero_reply(data, len);

    case 0x0A:                              /* какая настройка интерфейса */
        return zero_reply(data, len);

    case 0x01:                              /* снять признак */
    case 0x03:                              /* выставить признак */
    case 0x0B:                              /* выбрать настройку интерфейса */
        return 1;

    default:
        return 0;
    }
}
