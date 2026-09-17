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
#include "boards/board.h"
#include "hal_time.h"

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

/* Сбой мог случиться посреди выдачи — тогда признак выдачи так и остался
   поднятым, и всё, что обработчик сбоя напечатает, молча пропало бы. */
void usb_pos_after_fault(void) {
    g_in_pull = 0;
}

void usb_log_write(const char *s) {
    if (g_in_pull) return;          /* не наматывать собственные жалобы */
    while (*s) {
        uint32_t next = (g_head + 1) % LOG_RING;
        if (next == g_tail) g_tail = (g_tail + 1) % LOG_RING;  /* вытесняем */
        g_ring[g_head] = *s++;
        g_head = next;
    }
}

/* Повтор всего журнала с самого начала.
 *
 * Кольцо выше хранит только то, что не успело уйти. Но хост подключается
 * через несколько секунд после старта, а самое ценное — первые строки:
 * причина отказа почти всегда там. Поэтому по команде отдаём целиком
 * сохранённый журнал этой загрузки, который ведётся с первой строки. */
static const volatile unsigned char *g_replay = 0;
static uint32_t g_replay_len = 0;
static uint32_t g_replay_pos = 0;

int usb_pos_pull(uint8_t *dst, int max) {
    int n = 0;
    g_in_pull = 1;

    /* Сначала повтор, если он заказан: иначе свежие строки перемешались
       бы со старыми и порядок в журнале перестал бы что-то значить. */
    while (n < max && g_replay_pos < g_replay_len)
        dst[n++] = g_replay[g_replay_pos++];

    if (g_replay_pos >= g_replay_len) {
        while (n < max && g_tail != g_head) {
            dst[n++] = (uint8_t)g_ring[g_tail];
            g_tail = (g_tail + 1) % LOG_RING;
        }
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

/* Команды от компьютера. Односимвольные — разбирать здесь нечего, а
 * поводов для ошибки меньше.
 *
 *   p  отзовись
 *   v  скажи, кто ты и на какой скорости
 *   d  отдай весь журнал этой загрузки с самого начала
 *   b  перезагрузись в загрузчик
 *   s  перезагрузись в систему
 *
 * Последние две и делают круг отладки замкнутым: посмотреть журнал,
 * поправить, собрать, вернуть телефон в загрузчик и загрузить снова —
 * теперь всё это делается с компьютера, без рук у стола. */
/* ОПАСНЫЕ КОМАНДЫ — ТОЛЬКО ПО ОТЛИЧИТЕЛЬНОЙ ПОСЛЕДОВАТЕЛЬНОСТИ.
 *
 * Сперва перезагрузка вызывалась одним байтом 'b'. Это оказалось прямой
 * причиной того, что аппарат выключался сам через случайное время: в
 * потоке по проводу достаточно одного такого байта, а он там берётся из
 * чего угодно — из недосчитанной длины приёма, из неочищенного буфера,
 * из чужого обращения к устройству.
 *
 * Подпись в микросхеме питания это и подтвердила: выключение по
 * PS_HOLD, то есть процессор отпустил линию сам. Отпускает её ровно наш
 * код перезагрузки — значит его и вызывали, просто не мы.
 *
 * Теперь перед буквой требуется "POS": четыре байта подряд случайно не
 * складываются. Безобидные команды (журнал, отзыв) остаются
 * односимвольными — ошибиться в них нечем. */
static const char g_magic[] = "POS";
static int g_magic_pos = 0;

/* Команды с данными: после буквы идут ещё несколько байт.
 *
 *   POSk <знак>             нажать клавишу
 *   POSt <x:2> <y:2>        коснуться точки (старший байт первым)
 *
 * Обе — для нагрузочной проверки: компьютер набирает быстрее человека. */
static uint8_t g_arg_cmd = 0;
static uint8_t g_arg[8];
static int     g_arg_have = 0, g_arg_need = 0;

/* Ввод живёт в hal_input_arm64.c; в сборке без него — заглушки. */
__attribute__((weak)) void hal_input_inject_key(int key) { (void)key; }
__attribute__((weak)) void hal_input_inject_tap(int x, int y) { (void)x; (void)y; }

static void do_arg_command(void) {
    if (g_arg_cmd == 'k')
        hal_input_inject_key(g_arg[0]);
    else if (g_arg_cmd == 't')
        hal_input_inject_tap((g_arg[0] << 8) | g_arg[1], (g_arg[2] << 8) | g_arg[3]);
    else if (g_arg_cmd == 'u') {
        /* POSu <UNIX:4> <пояс в минутах:2, со знаком> — старший байт первым. */
        uint32_t unix_utc = ((uint32_t)g_arg[0] << 24) | ((uint32_t)g_arg[1] << 16) |
                            ((uint32_t)g_arg[2] << 8) | g_arg[3];
        int tz = (int16_t)(((uint16_t)g_arg[4] << 8) | g_arg[5]);
        hal_time_set_wall(unix_utc, tz);
        usb_log_write("POS: vremya ustanovleno\n");
    }
#ifdef CONFIG_CPUFREQ_MSM8953
    else if (g_arg_cmd == 'f') {
        extern int msm8953_cpu_boost(int idx);
        msm8953_cpu_boost(g_arg[0]);
    }
#endif
}

static void do_command(uint8_t c) {
    switch (c) {
    case 'p': {
        /* С временем работы: по нему компьютер сопоставляет свои часы с
           часами телефона, и момент смерти, замеченный снаружи, можно
           положить рядом со следом чёрного ящика. */
        static const char hexd[] = "0123456789ABCDEF";
        char line[48] = "POS: ping ";
        uint32_t ms = (uint32_t)hal_time_ms();
        int n = 10;
        for (int sh = 28; sh >= 0; sh -= 4) line[n++] = hexd[(ms >> sh) & 0xF];
#ifdef BOARD_IMEM_RESTART_REASON
        /* Самый горячий датчик кристалла, десятые доли градуса (TSENS,
           второе поколение: регистр состояния датчика, младшие 12 разрядов
           — знаковая температура, разряд 21 — значение годное). */
        {
            int best = -10000;
            for (int sn = 0; sn < 16; sn++) {
                uint32_t st = mmio_read32(0x004A9000UL + 0xA0 + 4u * (uint32_t)sn);
                if (!(st & (1u << 21))) continue;
                int t = (int)(st & 0xFFF);
                if (t & 0x800) t -= 0x1000;
                if (t > best) best = t;
            }
            line[n++] = ' ';
            line[n++] = 't';
            uint32_t v = (uint32_t)(best < 0 ? 0 : best);
            for (int sh = 12; sh >= 0; sh -= 4) line[n++] = hexd[(v >> sh) & 0xF];
        }
#endif
        line[n++] = '\n';
        line[n] = 0;
        usb_log_write(line);
        break;
    }

    case 'v':
        usb_log_write("POS: proshivkaOS NEXT, mido, skorost ");
        usb_log_write(g_speed >= 480 ? "vysokaya\n" : "polnaya\n");
        break;

    case 'o': {
        /* Хвост журнала ПРОШЛОГО запуска — того, что умер. */
        const char *tail; uint32_t n;
        if (blackbox_prev_tail(&tail, &n)) {
            usb_log_write("POS: hvost proshlogo zapuska:\n");
            /* Только самый конец: провод отдаёт медленно, а следующая
               смерть может прийти раньше, чем дойдёт всё. */
            if (n > 1200) { tail += n - 1200; n = 1200; }
            g_replay = (const volatile unsigned char *)tail;
            g_replay_len = n;
            g_replay_pos = 0;
        } else {
            usb_log_write("POS: hvosta proshlogo zapuska net\n");
        }
        break;
    }

    case 'S':
    case 'F': {
        /* СНИМОК ЭКРАНА. S — то, что нарисовано в бэкбуфере; F — то, что
           на самом деле лежит в памяти экрана (проверка вывода кадра: если
           полоса не выведена, на F её нет). Уменьшено втрое с усреднением
           по квадрату 3x3: 360x640 при полном разрешении телефона.
           Заголовок: "POSSHOT", ширина и высота по два байта. */
        extern uint32_t *gfxfb_backbuffer32(void);
        extern int gfxfb_width(void), gfxfb_height(void);
        static uint8_t shot[7 + 4 + 360 * 640 * 3];
        const uint32_t *fb = gfxfb_backbuffer32();
        int W = gfxfb_width(), H = gfxfb_height();
        int w = W / 3, h = H / 3;
        if (w > 360) w = 360;
        if (h > 640) h = 640;
        const char *hdr = "POSSHOT";
        for (int i = 0; i < 7; i++) shot[i] = (uint8_t)hdr[i];
        shot[7] = (uint8_t)(w >> 8); shot[8] = (uint8_t)w;
        shot[9] = (uint8_t)(h >> 8); shot[10] = (uint8_t)h;
        uint8_t *d = shot + 11;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                unsigned r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 3; dy++) {
                    for (int dx = 0; dx < 3; dx++) {
                        uint32_t v;
#ifdef BOARD_HAS_STATIC_FB
                        if (c == 'F') {
                            const volatile uint8_t *px = (const volatile uint8_t *)
                                (BOARD_FB_ADDR + (uint64_t)(y * 3 + dy) * BOARD_FB_STRIDE +
                                 (uint64_t)(x * 3 + dx) * 3);
                            v = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16);
                        } else
#endif
                            v = fb[(y * 3 + dy) * W + x * 3 + dx];
                        r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255;
                    }
                }
                *d++ = (uint8_t)(r / 9); *d++ = (uint8_t)(g / 9); *d++ = (uint8_t)(b / 9);
            }
        }
        g_replay = (const volatile unsigned char *)shot;
        g_replay_len = (uint32_t)(d - shot);
        g_replay_pos = 0;
        g_tail = g_head;
        break;
    }

    case 'm': {
        extern void smp_report(void);
        smp_report();
        break;
    }

#ifdef BOARD_IMEM_RESTART_REASON
    case 'c': {
        /* Тактирование ядер — только чтение. ФАПЧ ядер (HF PLL), выбор
           источника у двух кластеров и у межкластерной шины, ускоритель
           памяти ядер и переключатель её питания (APM). */
        static const struct { const char *name; uint32_t addr; } regs[] = {
            { "pll mode ", 0x0B116000 }, { "pll l    ", 0x0B116004 },
            { "pll alpha", 0x0B116008 }, { "pll 0c   ", 0x0B11600C },
            { "pll user ", 0x0B116010 }, { "pll cfg  ", 0x0B116014 },
            { "pll cfghi", 0x0B116018 }, { "pll tstlo", 0x0B11601C },
            { "pll tsthi", 0x0B116020 }, { "pll 24   ", 0x0B116024 },
            { "c0 cmd   ", 0x0B111050 }, { "c0 cfg   ", 0x0B111054 },
            { "c1 cmd   ", 0x0B011050 }, { "c1 cfg   ", 0x0B011054 },
            { "cci cmd  ", 0x0B1D1050 }, { "cci cfg  ", 0x0B1D1054 },
            { "apm stat ", 0x0B1112B0 }, { "memacc l1", 0x019461D4 },
            { "memacc l2", 0x019461D8 },
        };
        static const char hexd[] = "0123456789ABCDEF";
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            char line[40];
            int n = 0;
            const char *nm = "CLK: ";
            while (*nm) line[n++] = *nm++;
            for (const char *q = regs[i].name; *q; q++) line[n++] = *q;
            line[n++] = ' ';
            uint32_t v = mmio_read32(regs[i].addr);
            for (int sh = 28; sh >= 0; sh -= 4) line[n++] = hexd[(v >> sh) & 0xF];
            line[n++] = '\n';
            line[n] = 0;
            usb_log_write(line);
        }
        break;
    }
#endif

#ifdef CONFIG_SCM
    case 'q': {
        /* Спросить TrustZone про запуск процессора Wi-Fi. */
        extern void scm_probe_wifi(void);
        scm_probe_wifi();
        break;
    }
#endif

#ifdef BOARD_IMEM_RESTART_REASON
    case 'z': {
        /* ЖУРНАЛ TRUSTZONE. Доверенная среда пишет его в служебную память
           на кристалле (узел qcom,tz-log в дереве: 0x08600720, 8 КБ), и
           обычному миру его читать разрешено — так делает и Linux. Если
           аппарат перезагружает она, причина должна лежать здесь. Слово
           за словом в свой буфер, потом отдаём как повтор. */
        static uint8_t tz[0x2000];
        for (uint32_t off = 0; off < sizeof(tz); off += 4) {
            uint32_t w = mmio_read32(0x08600720UL + off);
            tz[off] = (uint8_t)w; tz[off + 1] = (uint8_t)(w >> 8);
            tz[off + 2] = (uint8_t)(w >> 16); tz[off + 3] = (uint8_t)(w >> 24);
        }
        g_replay = (const volatile unsigned char *)tz;
        g_replay_len = sizeof(tz);
        g_replay_pos = 0;
        g_tail = g_head;
        break;
    }
#endif

    case 'd':
        if (ramoops_snapshot(&g_replay, &g_replay_len)) {
            g_replay_pos = 0;
            /* Кольцо выбрасываем: всё, что в нём есть, уже входит в
               повтор. Иначе хвост прошлой выдачи вклинивается в начало
               журнала, и порядок строк перестаёт что-либо значить. */
            g_tail = g_head;
        } else {
            usb_log_write("POS: zhurnal nedostupen\n");
        }
        break;

    default:
        break;
    }
}

void usb_pos_received(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];

        /* Идут данные команды — это не буквы, разбирать их нельзя. */
        if (g_arg_need) {
            g_arg[g_arg_have++] = c;
            if (g_arg_have == g_arg_need) {
                do_arg_command();
                g_arg_need = 0;
            }
            continue;
        }

        /* Набирается ли отличительная последовательность. */
        if (g_magic_pos < (int)sizeof(g_magic) - 1) {
            if (c == (uint8_t)g_magic[g_magic_pos]) { g_magic_pos++; continue; }
            g_magic_pos = (c == (uint8_t)g_magic[0]) ? 1 : 0;
            do_command(c);
            continue;
        }

        /* Последовательность набрана — эта буква может быть опасной. */
        g_magic_pos = 0;

        if (c == 'k' || c == 't' || c == 'f' || c == 'u') {
            g_arg_cmd = c;
            g_arg_have = 0;
            g_arg_need = (c == 't') ? 4 : (c == 'u') ? 6 : 1;
            continue;
        }

#ifdef BOARD_IMEM_RESTART_REASON
        if (c == 'b') {
            usb_log_write("POS: uhozhu v zagruzchik\n");
            msm_reboot_bootloader();
            continue;
        }
        if (c == 's') {
            usb_log_write("POS: perezagruzhayus v sistemu\n");
            msm_reboot_system();
            continue;
        }
        if (c == 'w') {
            /* ОПЫТ: повиснуть намертво, не поглаживая сторожевой таймер.
             *
             * Нужен, чтобы увидеть, куда приводит смерть от его укуса —
             * в загрузчик или в Android, — не дожидаясь случайной смерти
             * по нескольку минут. Прерывания замаскированы, фоновая
             * прокрутка не вызывается: ничто не погладит таймер. */
            usb_log_write("POS: visnu bez storozha, zhdite ukusa\n");
            __asm__ volatile ("msr daifset, #0xf");
            for (;;) { }
        }
#endif
        do_command(c);
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
