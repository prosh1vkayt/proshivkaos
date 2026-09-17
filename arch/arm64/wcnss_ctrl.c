/* arch/arm64/wcnss_ctrl.c — разговор с прошивкой Wi-Fi по каналу WCNSS_CTRL.
 *
 * Запущенный Pronto ещё не радио: прошивке нужен NV-файл — таблица
 * калибровки и настроек под конкретную плату (WCNSS_qcom_wlan_nv.bin из
 * vendor телефона). Отдаёт его приложение по каналу WCNSS_CTRL, и порядок
 * тот же, что у drivers/soc/qcom/wcnss_ctrl.c в Linux:
 *
 *   1. запрос версии — прошивка отвечает четырьмя числами;
 *   2. NV-файл кусками по 3072 байта, у последнего поднят признак last;
 *   3. ответ на загрузку: 1 — готово, 2 — «иду на холодную калибровку»,
 *      и тогда ещё ждём сообщения о её завершении.
 *
 * Каждое сообщение начинается восемью байтами: тип и полная длина. */
#include "arm64.h"
#include "hal_time.h"
#include "smd.h"

const uint8_t *fw_find(const char *name, uint32_t *size);

#define IPC_SMD_WCNSS          0x00020000u
#define HOST_WCNSS             4
#define EDGE_WCNSS             6

#define MSG_VERSION_REQ        0x01000000u
#define MSG_VERSION_RESP       0x01000001u
#define MSG_DOWNLOAD_NV_REQ    0x01000002u
#define MSG_DOWNLOAD_NV_RESP   0x01000003u
#define MSG_CBC_COMPLETE_IND   0x0100000Cu

#define NV_FRAGMENT            3072u

enum { ST_IDLE, ST_WAIT_CHANNEL, ST_OPENING, ST_VERSION, ST_NV, ST_NV_WAIT, ST_CBC_WAIT, ST_READY, ST_FAILED };

static smd_chan_t g_ch;
static int g_st = ST_IDLE;
static uint64_t g_deadline;
static const uint8_t *g_nv;
static uint32_t g_nv_size, g_nv_off;
static uint16_t g_seq;
static int g_busy;
static uint8_t g_ver[4];

static void say(const char *s) { early_con_puts(s); }

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void say_dec(uint32_t v) {
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    char out[12]; int k = 0;
    while (n) out[k++] = b[--n];
    out[k] = 0;
    say(out);
}

static void rx(smd_chan_t *ch, const uint8_t *d, uint32_t len) {
    (void)ch;
    if (len < 8) return;
    uint32_t type = get32(d);
    if (type == MSG_VERSION_RESP && len >= 12) {
        for (int i = 0; i < 4; i++) g_ver[i] = d[8 + i];
        say("WCNSS: versiya prosivki ");
        say_dec(g_ver[0]); say("."); say_dec(g_ver[1]); say(" ");
        say_dec(g_ver[2]); say("."); say_dec(g_ver[3]); say("\n");
        if (g_st == ST_VERSION) {
            g_nv = fw_find("WCNSS_qcom_wlan_nv.bin", &g_nv_size);
            if (!g_nv) { say("WCNSS: NV-fayla v obraze net\n"); g_st = ST_FAILED; return; }
            g_nv_off = 0; g_seq = 0;
            g_st = ST_NV;
            say("WCNSS: otdayu NV-fayl, bayt "); say_dec(g_nv_size); say("\n");
        }
    } else if (type == MSG_DOWNLOAD_NV_RESP && len >= 9) {
        uint8_t status = d[8];
        say("WCNSS: NV prinyat, otvet "); say_dec(status); say("\n");
        if (status == 2) {
            g_st = ST_CBC_WAIT;
            g_deadline = hal_time_ms() + 15000;
            say("WCNSS: prosivka ushla na holodnuyu kalibrovku\n");
        } else {
            g_st = ST_READY;
            say("WCNSS: radio gotovo\n");
        }
    } else if (type == MSG_CBC_COMPLETE_IND) {
        say("WCNSS: holodnaya kalibrovka zavershena, radio gotovo\n");
        g_st = ST_READY;
    } else {
        say("WCNSS: soobshchenie ");
        early_con_hex32(type);
        say(" dlina ");
        say_dec(len);
        say("\n");
    }
}

/* Начать: канал прошивка заводит не сразу после запуска, поэтому ждём
   его появления в фоне, до десяти секунд. */
void wcnss_ctrl_start(void) {
    if (g_st != ST_IDLE && g_st != ST_FAILED && g_st != ST_READY) return;
    g_st = ST_WAIT_CHANNEL;
    g_deadline = hal_time_ms() + 10000;
}

void hal_wcnss_pump(void) {
    if (g_st == ST_IDLE || g_st == ST_FAILED || g_busy) return;
    static uint64_t last;
    uint64_t now = hal_time_ms();
    if (now - last < 10) return;
    last = now;
    g_busy = 1;

    if (g_st != ST_WAIT_CHANNEL) smd_chan_poll(&g_ch, rx);

    switch (g_st) {
    case ST_WAIT_CHANNEL: {
        static uint64_t tried;
        if (now - tried < 200) break;
        tried = now;
        if (smd_chan_open(&g_ch, "WCNSS_CTRL", HOST_WCNSS, EDGE_WCNSS, IPC_SMD_WCNSS)) {
            g_st = ST_OPENING;
            g_deadline = now + 5000;
        } else if (now > g_deadline) {
            say("WCNSS: kanal WCNSS_CTRL tak i ne poyavilsya\n");
            g_st = ST_FAILED;
        }
        break;
    }
    case ST_OPENING:
        if (g_ch.state == SMD_OPENED) {
            uint8_t m[8];
            put32(m, MSG_VERSION_REQ);
            put32(m + 4, 8);
            if (smd_chan_send(&g_ch, m, 8)) {
                g_st = ST_VERSION;
                g_deadline = now + 10000;
                say("WCNSS: sprashivayu versiyu\n");
            }
        } else if (now > g_deadline) {
            say("WCNSS: kanal ne otkrylsya\n");
            g_st = ST_FAILED;
        }
        break;
    case ST_NV: {
        static uint8_t req[16 + NV_FRAGMENT];
        uint32_t left = g_nv_size - g_nv_off;
        uint32_t frag = left <= NV_FRAGMENT ? left : NV_FRAGMENT;
        uint32_t last_frag = left <= NV_FRAGMENT;
        put32(req, MSG_DOWNLOAD_NV_REQ);
        put32(req + 4, 16 + frag);
        req[8] = (uint8_t)g_seq; req[9] = (uint8_t)(g_seq >> 8);
        req[10] = (uint8_t)last_frag; req[11] = 0;
        put32(req + 12, frag);
        for (uint32_t i = 0; i < frag; i++) req[16 + i] = g_nv[g_nv_off + i];
        if (smd_chan_send(&g_ch, req, 16 + frag)) {
            g_nv_off += frag;
            g_seq++;
            if (last_frag) {
                g_st = ST_NV_WAIT;
                g_deadline = now + 10000;
                say("WCNSS: NV otdan, kuskov "); say_dec(g_seq); say("\n");
            }
        }
        break;
    }
    case ST_VERSION:
    case ST_NV_WAIT:
    case ST_CBC_WAIT:
        if (now > g_deadline) {
            say("WCNSS: prosivka ne otvetila vovremya\n");
            g_st = g_st == ST_CBC_WAIT ? ST_READY : ST_FAILED;
        }
        break;
    default:
        break;
    }
    g_busy = 0;
}

int wcnss_ctrl_ready(void) { return g_st == ST_READY; }
