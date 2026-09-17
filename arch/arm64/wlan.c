/* arch/arm64/wlan.c — WLAN_CTRL: управляющий протокол радио (HAL).
 *
 * После WCNSS_CTRL прошивка откалибрована, но WLAN в ней ещё не запущен.
 * Дальше разговор идёт по каналу WLAN_CTRL на языке HAL — том же, что у
 * драйвера wcn36xx в Linux (drivers/net/wireless/ath/wcn36xx, hal.h):
 *
 *   заголовок   тип u16, версия u16, полная длина u32
 *   тело        по типу сообщения; ответ — тип запроса плюс один
 *
 * Порядок запуска как в wcn36xx_start():
 *   DOWNLOAD_NV   ещё раз NV-файл, теперь HAL-у, кусками по 3072 байта
 *                 (первые четыре байта файла — признак, их не шлют);
 *   START         тип драйвера и параметры TLV: антенны, пороги, повторы;
 *                 в ответе — версия WLAN и число станций;
 *   FEATURE_CAPS  обмен списками возможностей.
 *
 * Сами кадры 802.11 (маяки, данные) по этому каналу не ходят — для них
 * есть DXE, кольца в общей памяти. Здесь только управление. */
#include "arm64.h"
#include "hal_time.h"
#include "smd.h"

const uint8_t *fw_find(const char *name, uint32_t *size);

#define IPC_SMD_WCNSS            0x00020000u

#define HAL_START_REQ            0
#define HAL_START_RSP            1
#define HAL_DOWNLOAD_NV_REQ      55
#define HAL_DOWNLOAD_NV_RSP      56
#define HAL_FEATURE_CAPS_REQ     175
#define HAL_FEATURE_CAPS_RSP     176
#define HAL_INIT_SCAN_REQ        4
#define HAL_INIT_SCAN_RSP        5
#define HAL_START_SCAN_REQ       6
#define HAL_START_SCAN_RSP       7
#define HAL_END_SCAN_REQ         8
#define HAL_END_SCAN_RSP         9
#define HAL_FINISH_SCAN_REQ      10
#define HAL_FINISH_SCAN_RSP      11
#define HAL_ADD_STA_SELF_REQ     125
#define HAL_ADD_STA_SELF_RSP     126
#define HAL_SYS_MODE_SCAN        2

#define SCAN_DWELL_MS            120
#define SCAN_LAST_CHANNEL        13

typedef void (*dxe_rx_fn)(const uint8_t *bd, uint32_t size);
int  wlan_dxe_init(void);
void wlan_dxe_smsm_init(void);
int  wlan_dxe_poll(dxe_rx_fn fn);

#define NV_FRAGMENT              3072u

/* Параметры START — таблица wcn36xx_cfg_vals (номер, значение). */
static const struct { uint16_t id; uint32_t val; } g_cfg[] = {
    {   1,      1 },   /* CURRENT_TX_ANTENNA */
    {   2,      1 },   /* CURRENT_RX_ANTENNA */
    {   3,      0 },   /* LOW_GAIN_OVERRIDE */
    {   4,    785 },   /* POWER_STATE_PER_CHAIN */
    {   5,      5 },   /* CAL_PERIOD */
    {   6,      1 },   /* CAL_CONTROL */
    {   7,      0 },   /* PROXIMITY */
    {   8,      3 },   /* NETWORK_DENSITY */
    {   9,   6000 },   /* MAX_MEDIUM_TIME */
    {  10,     64 },   /* MAX_MPDUS_IN_AMPDU */
    {  11,   2347 },   /* RTS_THRESHOLD */
    {  12,     15 },   /* SHORT_RETRY_LIMIT */
    {  13,     15 },   /* LONG_RETRY_LIMIT */
    {  14,   8000 },   /* FRAGMENTATION_THRESHOLD */
    {  15,      5 },   /* DYNAMIC_THRESHOLD_ZERO */
    {  16,     10 },   /* DYNAMIC_THRESHOLD_ONE */
    {  17,     15 },   /* DYNAMIC_THRESHOLD_TWO */
    {  18,      0 },   /* FIXED_RATE */
    {  19,      4 },   /* RETRYRATE_POLICY */
    {  20,      0 },   /* RETRYRATE_SECONDARY */
    {  21,      0 },   /* RETRYRATE_TERTIARY */
    {  22,      5 },   /* FORCE_POLICY_PROTECTION */
    {  23,      1 },   /* FIXED_RATE_MULTICAST_24GHZ */
    {  24,      5 },   /* FIXED_RATE_MULTICAST_5GHZ */
    {  26,      5 },   /* DEFAULT_RATE_INDEX_5GHZ */
    {  27,     40 },   /* MAX_BA_SESSIONS */
    {  28,    200 },   /* PS_DATA_INACTIVITY_TIMEOUT */
    {  29,      1 },   /* PS_ENABLE_BCN_FILTER */
    {  30,      1 },   /* PS_ENABLE_RSSI_MONITOR */
    {  31,     20 },   /* NUM_BEACON_PER_RSSI_AVERAGE */
    {  32,     10 },   /* STATS_PERIOD */
    {  33,  30000 },   /* CFP_MAX_DURATION */
    {  34,      0 },   /* FRAME_TRANS_ENABLED */
    {  40,    128 },   /* BA_THRESHOLD_HIGH */
    {  41,   2560 },   /* MAX_BA_BUFFERS */
    {  57,      0 },   /* DYNAMIC_PS_POLL_VALUE */
    {  64,      1 },   /* TX_PWR_CTRL_ENABLE */
    {  75,      1 },   /* ENABLE_CLOSE_LOOP */
    {  99,      0 },   /* ENABLE_LPWR_IMG_TRANSITION */
    {  87, 120000 },   /* BTC_STATIC_LEN_LE_BT */
    {  91,  30000 },   /* BTC_STATIC_LEN_LE_WLAN */
    {  98,     10 },   /* MAX_ASSOC_LIMIT */
    { 100,      0 },   /* ENABLE_MCC_ADAPTIVE_SCHEDULER */
    { 210,    133 },   /* ENABLE_DYNAMIC_RA_START_RATE */
    { 215,   1000 },   /* LINK_FAIL_TX_CNT */
};

enum { W_IDLE, W_WAIT_CHANNEL, W_OPENING, W_NV, W_NV_WAIT, W_START_SEND,
       W_START_WAIT, W_CAPS_SEND, W_CAPS_WAIT, W_SELF_SEND, W_SELF_WAIT,
       W_READY, W_FAILED,
       /* сканирование */
       S_INIT_SEND, S_INIT_WAIT, S_START_SEND, S_START_WAIT, S_DWELL,
       S_END_SEND, S_END_WAIT, S_FINISH_SEND, S_FINISH_WAIT };

static smd_chan_t g_ch;
static int g_st = W_IDLE, g_busy;
static uint64_t g_deadline;
static const uint8_t *g_nv;
static uint32_t g_nv_size, g_nv_off;
static uint16_t g_frag;
static uint8_t g_buf[4096];
static uint8_t g_scan_ch;
static uint64_t g_dwell_end;
static const uint8_t g_mac[6] = { 0x02, 0x50, 0x4F, 0x53, 0x4E, 0x58 };   /* 02:"POSNX" */

/* Найденные сети. */
#define MAX_NETS 32
typedef struct {
    uint8_t bssid[6];
    char    ssid[33];
    uint8_t channel;
    int8_t  rssi;
    uint8_t secure;
} wlan_net_t;
static wlan_net_t g_nets[MAX_NETS];
static int g_net_count;
static uint32_t g_frames;

static void say(const char *s) { early_con_puts(s); }
static void say_dec(uint32_t v) {
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    char out[12]; int k = 0;
    while (n) out[k++] = b[--n];
    out[k] = 0;
    say(out);
}
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

static uint32_t hal_hdr(uint8_t *b, uint16_t type, uint32_t len) {
    put16(b, type); put16(b + 2, 0); put32(b + 4, len);
    return 8;
}

static void fail(const char *why) {
    say("WLAN: "); say(why); say("\n");
    g_st = W_FAILED;
}

/* Строка версии из ответа: до 64 байт, без гарантии нуля. */
static void say_str(const uint8_t *p, int max) {
    char s[65]; int i = 0;
    for (; i < max && i < 64 && p[i]; i++) s[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
    s[i] = 0;
    say(s);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Кадр из DXE: впереди дескриптор приёма (слова big-endian, поля — как
   struct wcn36xx_rx_bd), дальше сам кадр 802.11. Нужны маяки и ответы
   на пробы: из них — имя сети, канал, уровень и есть ли шифрование. */
static void on_frame(const uint8_t *bd, uint32_t size) {
    g_frames++;
    uint32_t w0 = be32(bd), w3 = be32(bd + 12), w4 = be32(bd + 16), w7 = be32(bd + 28);
    uint32_t hdr_off = (w3 >> 16) & 0xFF, data_off = (w3 >> 7) & 0x1FF;
    uint32_t hdr_len = (w3 >> 24) & 0xFF, mpdu_len = (w4 >> 16) & 0xFFFF;
    if (data_off <= hdr_off || mpdu_len < hdr_len || hdr_off < 0x4C ||
        hdr_off + mpdu_len > size || mpdu_len < 36) return;
    const uint8_t *f = bd + hdr_off;
    uint32_t fc = (uint32_t)f[0] | ((uint32_t)f[1] << 8);
    if ((fc & 0xFC) != 0x80 && (fc & 0xFC) != 0x50) return;       /* маяк / ответ на пробу */

    int rssi = -(100 - (int)((w7 >> 24) & 0xFF));
    uint8_t ch = (uint8_t)((((w0 >> 11) & 1) << 4) | ((w0 >> 13) & 0xF));
    uint32_t cap = (uint32_t)f[34] | ((uint32_t)f[35] << 8);
    char ssid[33] = { 0 };
    uint8_t secure = (cap & 0x10) ? 1 : 0;
    for (uint32_t o = 36; o + 2 <= mpdu_len;) {
        uint8_t id = f[o], l = f[o + 1];
        if (o + 2 + l > mpdu_len) break;
        if (id == 0 && l <= 32) { for (int i = 0; i < l; i++) ssid[i] = (char)f[o + 2 + i]; ssid[l] = 0; }
        else if (id == 3 && l >= 1) ch = f[o + 2];
        else if (id == 48) secure = 2;                      /* RSN — WPA2 */
        o += 2u + l;
    }

    int k = 0;
    for (; k < g_net_count; k++) {
        int same = 1;
        for (int i = 0; i < 6; i++) if (g_nets[k].bssid[i] != f[16 + i]) { same = 0; break; }
        if (same) break;
    }
    if (k == g_net_count) {
        if (g_net_count >= MAX_NETS) return;
        g_net_count++;
        for (int i = 0; i < 6; i++) g_nets[k].bssid[i] = f[16 + i];
        g_nets[k].rssi = -127;
    }
    wlan_net_t *n = &g_nets[k];
    for (int i = 0; i < 33; i++) n->ssid[i] = ssid[i];
    n->channel = ch;
    n->secure = secure;
    if (rssi > n->rssi) n->rssi = (int8_t)rssi;
}

static void wlan_scan_report(void) {
    say("WLAN: skan zavershen, kadrov "); say_dec(g_frames);
    say(", setey "); say_dec((uint32_t)g_net_count); say("\n");
    static const char hexd[] = "0123456789abcdef";
    for (int k = 0; k < g_net_count; k++) {
        wlan_net_t *n = &g_nets[k];
        char mac[18];
        for (int i = 0; i < 6; i++) {
            mac[i * 3] = hexd[n->bssid[i] >> 4]; mac[i * 3 + 1] = hexd[n->bssid[i] & 15];
            mac[i * 3 + 2] = i < 5 ? ':' : 0;
        }
        say("WLAN:   "); say(n->ssid[0] ? n->ssid : "<skrytaya>");
        say("  "); say(mac);
        say("  kanal "); say_dec(n->channel);
        say("  "); say(n->rssi < 0 ? "-" : ""); say_dec((uint32_t)(n->rssi < 0 ? -n->rssi : n->rssi)); say(" dBm");
        say(n->secure == 2 ? "  WPA2" : n->secure ? "  zashchishchena" : "  otkrytaya");
        say("\n");
    }
}

void wlan_scan(void) {
    if (g_st != W_READY) { say("WLAN: radio ne gotovo k skanirovaniyu\n"); return; }
    g_net_count = 0; g_frames = 0;
    g_st = S_INIT_SEND;
}

static void rx(smd_chan_t *ch, const uint8_t *d, uint32_t len) {
    (void)ch;
    if (len < 8) return;
    uint32_t type = get16(d);
    if (type == HAL_DOWNLOAD_NV_RSP && g_st == W_NV_WAIT) {
        uint32_t status = len >= 12 ? get32(d + 8) : 0xFFFFFFFFu;
        if (status) { say("WLAN: HAL ne prinyal kusok NV, kod "); say_dec(status); fail("stop"); return; }
        g_st = g_nv_off >= g_nv_size ? W_START_SEND : W_NV;
        if (g_nv_off >= g_nv_size) say("WLAN: NV otdan HAL-u\n");
    } else if (type == HAL_START_RSP && g_st == W_START_WAIT) {
        if (len < 144) { fail("korotkiy otvet START"); return; }
        uint32_t status = get16(d + 8);
        say("WLAN: START otvet "); say_dec(status);
        say(", stanciy "); say_dec(d[10]); say(", BSS "); say_dec(d[11]);
        say(", API "); say_dec(d[15]); say("."); say_dec(d[14]); say(".");
        say_dec(d[13]); say("."); say_dec(d[12]); say("\n");
        say("WLAN: versiya WLAN '"); say_str(d + 80, 64); say("'\n");
        say("WLAN: versiya CRM '"); say_str(d + 16, 64); say("'\n");
        if (status) { fail("START otklonyon"); return; }
        g_st = W_CAPS_SEND;
    } else if (type == HAL_ADD_STA_SELF_RSP && g_st == W_SELF_WAIT) {
        uint32_t status = len >= 12 ? get32(d + 8) : 1;
        say("WLAN: svoya stanciya, otvet "); say_dec(status);
        say(", indeks "); say_dec(len >= 13 ? d[12] : 0); say("\n");
        if (status) { fail("ADD_STA_SELF otklonyon"); return; }
        wlan_dxe_smsm_init();
        wlan_dxe_init();
        g_st = W_READY;
        say("WLAN: HAL zapushchen, radio vklyucheno\n");
        g_st = S_INIT_SEND;
    } else if ((type == HAL_INIT_SCAN_RSP && g_st == S_INIT_WAIT) ||
               (type == HAL_START_SCAN_RSP && g_st == S_START_WAIT) ||
               (type == HAL_END_SCAN_RSP && g_st == S_END_WAIT) ||
               (type == HAL_FINISH_SCAN_RSP && g_st == S_FINISH_WAIT)) {
        uint32_t status = len >= 12 ? get32(d + 8) : 1;
        if (status) {
            say("WLAN: skan, soobshchenie "); say_dec(type); say(" otvet "); say_dec(status); say("\n");
        }
        if (g_st == S_INIT_WAIT) { g_scan_ch = 1; g_st = S_START_SEND; say("WLAN: skaniruyu kanaly 1-13\n"); }
        else if (g_st == S_START_WAIT) { g_st = S_DWELL; g_dwell_end = hal_time_ms() + SCAN_DWELL_MS; }
        else if (g_st == S_END_WAIT) {
            if (g_scan_ch < SCAN_LAST_CHANNEL) { g_scan_ch++; g_st = S_START_SEND; }
            else g_st = S_FINISH_SEND;
        } else {
            wlan_scan_report();
            g_st = W_READY;
        }
    } else if (type == HAL_FEATURE_CAPS_RSP && g_st == W_CAPS_WAIT) {
        say("WLAN: vozmozhnosti prosivki");
        for (uint32_t i = 0; i < 4 && 8 + 4 * (i + 1) <= len; i++) { say(" "); early_con_hex32(get32(d + 8 + 4 * i)); }
        say("\n");
        g_st = W_SELF_SEND;
    } else {
        say("WLAN: soobshchenie HAL "); say_dec(type); say(" dlina "); say_dec(len); say("\n");
    }
}

void wlan_hal_start(void) {
    if (g_st != W_IDLE && g_st != W_FAILED) return;
    g_st = W_WAIT_CHANNEL;
    g_deadline = hal_time_ms() + 10000;
}

int wlan_hal_ready(void) { return g_st == W_READY; }
int wlan_scan_count(void) { return g_net_count; }

void hal_wlan_pump(void) {
    if (g_st == W_IDLE || g_st == W_FAILED || g_busy) return;
    static uint64_t last;
    uint64_t now = hal_time_ms();
    if (now - last < 10) return;
    last = now;
    g_busy = 1;

    if (g_st != W_WAIT_CHANNEL) smd_chan_poll(&g_ch, rx);
    wlan_dxe_poll(on_frame);

    switch (g_st) {
    case W_WAIT_CHANNEL: {
        static uint64_t tried;
        if (now - tried < 200) break;
        tried = now;
        if (smd_chan_open(&g_ch, "WLAN_CTRL", 4, 6, IPC_SMD_WCNSS)) {
            g_st = W_OPENING;
            g_deadline = now + 5000;
        } else if (now > g_deadline) {
            fail("kanal WLAN_CTRL ne poyavilsya");
        }
        break;
    }
    case W_OPENING:
        if (g_ch.state == SMD_OPENED) {
            g_nv = fw_find("WCNSS_qcom_wlan_nv.bin", &g_nv_size);
            if (!g_nv || g_nv_size <= 4) { fail("NV-fayla net"); break; }
            g_nv += 4; g_nv_size -= 4;
            g_nv_off = 0; g_frag = 0;
            g_st = W_NV;
        } else if (now > g_deadline) {
            fail("WLAN_CTRL ne otkrylsya");
        }
        break;
    case W_NV: {
        uint32_t left = g_nv_size - g_nv_off;
        uint32_t n = left > NV_FRAGMENT ? NV_FRAGMENT : left;
        uint32_t o = hal_hdr(g_buf, HAL_DOWNLOAD_NV_REQ, 16 + n);
        put16(g_buf + o, g_frag); put16(g_buf + o + 2, left <= NV_FRAGMENT);
        put32(g_buf + o + 4, n);
        for (uint32_t i = 0; i < n; i++) g_buf[16 + i] = g_nv[g_nv_off + i];
        if (smd_chan_send(&g_ch, g_buf, 16 + n)) {
            g_nv_off += n; g_frag++;
            g_st = W_NV_WAIT;
            g_deadline = now + 10000;
        }
        break;
    }
    case W_START_SEND: {
        uint32_t o = hal_hdr(g_buf, HAL_START_REQ, 0);
        put32(g_buf + o, 0);            /* DRIVER_TYPE_PRODUCTION */
        put32(g_buf + o + 4, 0);
        uint32_t len = 16;
        for (uint32_t i = 0; i < sizeof(g_cfg) / sizeof(g_cfg[0]); i++) {
            put16(g_buf + len, g_cfg[i].id); put16(g_buf + len + 2, 4);
            put16(g_buf + len + 4, 0);       put16(g_buf + len + 6, 0);
            put32(g_buf + len + 8, g_cfg[i].val);
            len += 12;
        }
        put32(g_buf + 4, len);
        put32(g_buf + 12, len - 16);
        if (smd_chan_send(&g_ch, g_buf, len)) {
            say("WLAN: START, parametrov "); say_dec(sizeof(g_cfg) / sizeof(g_cfg[0])); say("\n");
            g_st = W_START_WAIT;
            g_deadline = now + 10000;
        }
        break;
    }
    case W_CAPS_SEND: {
        uint32_t o = hal_hdr(g_buf, HAL_FEATURE_CAPS_REQ, 24);
        for (uint32_t i = 0; i < 16; i++) g_buf[o + i] = 0;
        if (smd_chan_send(&g_ch, g_buf, 24)) {
            g_st = W_CAPS_WAIT;
            g_deadline = now + 10000;
        }
        break;
    }
    case W_SELF_SEND: {
        hal_hdr(g_buf, HAL_ADD_STA_SELF_REQ, 18);
        for (int i = 0; i < 6; i++) g_buf[8 + i] = g_mac[i];
        put32(g_buf + 14, 0);
        if (smd_chan_send(&g_ch, g_buf, 18)) { g_st = W_SELF_WAIT; g_deadline = now + 10000; }
        break;
    }
    case S_INIT_SEND: {
        for (int i = 0; i < 48; i++) g_buf[i] = 0;
        hal_hdr(g_buf, HAL_INIT_SCAN_REQ, 48);
        put32(g_buf + 8, HAL_SYS_MODE_SCAN);
        if (smd_chan_send(&g_ch, g_buf, 48)) { g_st = S_INIT_WAIT; g_deadline = now + 10000; }
        break;
    }
    case S_START_SEND:
        hal_hdr(g_buf, HAL_START_SCAN_REQ, 9);
        g_buf[8] = g_scan_ch;
        if (smd_chan_send(&g_ch, g_buf, 9)) { g_st = S_START_WAIT; g_deadline = now + 10000; }
        break;
    case S_DWELL:
        if (now >= g_dwell_end) g_st = S_END_SEND;
        break;
    case S_END_SEND:
        hal_hdr(g_buf, HAL_END_SCAN_REQ, 9);
        g_buf[8] = g_scan_ch;
        if (smd_chan_send(&g_ch, g_buf, 9)) { g_st = S_END_WAIT; g_deadline = now + 10000; }
        break;
    case S_FINISH_SEND: {
        for (int i = 0; i < 53; i++) g_buf[i] = 0;
        hal_hdr(g_buf, HAL_FINISH_SCAN_REQ, 53);
        put32(g_buf + 8, HAL_SYS_MODE_SCAN);
        g_buf[12] = 1;                               /* рабочий канал */
        if (smd_chan_send(&g_ch, g_buf, 53)) { g_st = S_FINISH_WAIT; g_deadline = now + 10000; }
        break;
    }
    case W_NV_WAIT:
    case W_START_WAIT:
    case W_CAPS_WAIT:
    case W_SELF_WAIT:
    case S_INIT_WAIT:
    case S_START_WAIT:
    case S_END_WAIT:
    case S_FINISH_WAIT:
        if (now > g_deadline) fail("HAL ne otvetil vovremya");
        break;
    default:
        break;
    }
    g_busy = 0;
}
