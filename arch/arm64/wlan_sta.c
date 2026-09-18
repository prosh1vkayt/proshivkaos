/* arch/arm64/wlan_sta.c — подключение к точке доступа как станция.
 *
 * Сканирование показало сети; здесь мы к одной подключаемся. Прошивка
 * WCN36xx работает в режиме «программного MAC»: аутентификацию и
 * ассоциацию ведёт хост обычными управляющими кадрами 802.11, а прошивке
 * лишь сообщают контекст. Порядок — как у mac80211 с драйвером wcn36xx:
 *
 *   SET_LINK_ST PREASSOC   на bssid, чтобы принимать его кадры;
 *   JOIN                    настроить радио на канал точки;
 *   CONFIG_BSS (add)        завести контекст сети, получить индексы;
 *   кадр Authentication     открытая система, ждём ответ;
 *   кадр Association Req     ждём Association Response с AID;
 *   SET_LINK_ST POSTASSOC + CONFIG_BSS(update) + CONFIG_STA;
 *   для WPA2 — четырёхстороннее рукопожатие по EAPOL (net/crypto.c),
 *   ключи в прошивку (SET_STAKEY, SET_BSSKEY); шифрует уже она;
 *   net_link_up() — DHCP и дальше сеть.
 *
 * Кадры ходят через DXE (wlan_dxe.c): управляющие — высоким каналом,
 * данные (EAPOL, потом IP) — низким. Всё это машина состояний,
 * прокручиваемая из фоновой прокрутки: интерфейс при этом жив. */
#include "arm64.h"
#include "hal_time.h"
#include "wcn_hal.h"
#include "wcn_bd.h"
#include "../../net/crypto.h"
#include "../../net/net.h"

/* из wlan.c */
const uint8_t *wlan_self_mac(void);
int  wlan_self_sta_index(void);
int  wlan_self_dpu_index(void);
int  wlan_self_dpu_sign(void);
int  wlan_hal_send(const void *msg, uint32_t len);
int  wlan_hal_ready(void);
/* из wlan_dxe.c */
int  wlan_dxe_tx(int high, const uint8_t *bd, uint32_t bd_len, const uint8_t *frame, uint32_t len);

#define HAL_JOIN_REQ            20
#define HAL_JOIN_RSP            21
#define HAL_CONFIG_BSS_REQ      16
#define HAL_CONFIG_BSS_RSP      17
#define HAL_CONFIG_STA_REQ      12
#define HAL_CONFIG_STA_RSP      13
#define HAL_SET_LINK_ST_REQ     44
#define HAL_SET_LINK_ST_RSP     45
#define HAL_SET_BSSKEY_REQ      24
#define HAL_SET_BSSKEY_RSP      25
#define HAL_SET_STAKEY_REQ      26
#define HAL_SET_STAKEY_RSP      27

#define LINK_IDLE       0
#define LINK_PREASSOC   1
#define LINK_POSTASSOC  2
#define ED_NONE         0
#define ED_CCMP         4
#define TX_RX           2
#define RX_ONLY         1

#define BD_RATE_MGMT    2
#define BD_RATE_DATA    0
#define BMU_WQ_TX       25
#define TX_U_WQ_ID      0x9

/* Состояния подключения. */
enum {
    C_IDLE, C_LINK_PRE, C_JOIN, C_BSS_ADD,
    C_AUTH, C_ASSOC, C_LINK_POST, C_BSS_UPD, C_STA_CFG,
    C_HANDSHAKE, C_SET_PTK, C_SET_GTK, C_DHCP, C_UP, C_FAIL
};

static int g_cst = C_IDLE;
static uint64_t g_deadline;
static int g_tries;

static uint8_t g_bssid[6];
static uint8_t g_ssid[33];
static uint8_t g_ssid_len;
static uint8_t g_channel;
static uint8_t g_secure;           /* 0 — открытая, 2 — WPA2 */
static char    g_pass[64];

static uint8_t g_bss_index = 0xFF, g_bss_sta_index, g_bss_dpu, g_bss_sign;
static uint16_t g_aid;
static uint16_t g_seq_ctrl;

/* WPA2 */
static uint8_t g_pmk[32], g_ptk[48], g_anonce[32], g_snonce[32];
static uint8_t g_gtk[32]; static uint8_t g_gtk_len, g_gtk_id;
static uint64_t g_replay;
static int g_msg3_secure;
static uint8_t g_key_ver = 2;

static int8_t g_signal;

static void say(const char *s) { early_con_puts(s); }
static void say_dec(uint32_t v) {
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    char o[12]; int k = 0; while (n) o[k++] = b[--n]; o[k] = 0; say(o);
}

extern void wlan_conn_changed(void);   /* дёрнуть поколение в wlan.c */

static void cfail(const char *why) {
    say("STA: "); say(why); say("\n");
    g_cst = C_FAIL;
    wlan_conn_changed();
}

/* ---------------- случайные числа для nonce ---------------- */
static uint32_t g_rng = 0x9E3779B9u;
static void fill_random(uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        g_rng ^= (uint32_t)hal_time_us();
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
        p[i] = (uint8_t)(g_rng >> 24);
    }
}

/* ---------------- HAL-сообщения ---------------- */

static void hal_hdr(void *m, uint16_t type, uint16_t ver, uint32_t len) {
    struct wcn36xx_hal_msg_header *h = m;
    h->msg_type = type; h->msg_version = ver; h->len = len;
}

static void fill_rates(struct wcn36xx_hal_supported_rates *r) {
    static const uint16_t dsss[4] = { 0x82, 0x84, 0x8B, 0x96 };
    static const uint16_t ofdm[8] = { 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C };
    r->op_rate_mode = STA_11n;
    for (int i = 0; i < 4; i++) r->dsss_rates[i] = dsss[i];
    for (int i = 0; i < 8; i++) r->ofdm_rates[i] = ofdm[i];
    r->supported_mcs_set[0] = 0xFF;
}

/* Контекст станции. В режиме станции (STA) прошивка всюду ждёт одно и то
   же (wcn36xx_smd_set_sta_params): mac — НАШ адрес, bssid — адрес точки,
   type = 0, а индекс станции — это НАШ self_sta_index из ADD_STA_SELF, а
   не индекс из ответа CONFIG_BSS (на этом и падало — прошивка отклоняла
   CONFIG_STA). До ассоциации точки-собеседника ещё нет: bssid и aid
   пустые, скорости — умолчания. */
static void fill_sta_v1(struct wcn36xx_hal_config_sta_params_v1 *s, int have_ap) {
    for (int i = 0; i < 6; i++) s->mac[i] = wlan_self_mac()[i];
    if (have_ap) for (int i = 0; i < 6; i++) s->bssid[i] = g_bssid[i];
    s->type = 0;
    s->sta_index = wlan_self_sta_index();
    s->aid = have_ap ? g_aid : 0;
    s->short_preamble_supported = 1;
    s->listen_interval = 1;
    s->wmm_enabled = 0;
    s->ht_capable = 1;
    s->tx_channel_width_set = 0;
    s->max_ampdu_size = 3;
    s->max_ampdu_density = 5;
    s->sgi_20Mhz = 1;
    s->green_field_capable = 0;
    s->mimo_ps = WCN36XX_HAL_HT_MIMO_PS_STATIC;
    s->encrypt_type = g_secure ? ED_CCMP : ED_NONE;
    s->bssid_index = g_bss_index == 0xFF ? 0 : g_bss_index;
    fill_rates((struct wcn36xx_hal_supported_rates *)&s->supported_rates);
}

/* Длина сообщения config_sta/bss с поправкой на отсутствие полей VHT:
   у Iris WCN3660/3680 в этой прошивке их нет, драйвер вычитает разницу. */
#define STA_V1_TRIM  WCN36XX_DIFF_STA_PARAMS_V1_NOVHT
#define BSS_V1_TRIM  WCN36XX_DIFF_BSS_PARAMS_V1_NOVHT

static void send_config_sta(void) {
    static struct wcn36xx_hal_config_sta_req_msg_v1 m;
    for (uint32_t i = 0; i < sizeof(m); i++) ((uint8_t *)&m)[i] = 0;
    hal_hdr(&m, HAL_CONFIG_STA_REQ, 0, sizeof(m) - STA_V1_TRIM);
    fill_sta_v1(&m.sta_params, 1);
    m.sta_params.action = 0;
    wlan_hal_send(&m, sizeof(m) - STA_V1_TRIM);
    say("STA: CONFIG_STA\n");
}

static void send_config_bss(int update) {
    static struct wcn36xx_hal_config_bss_req_msg_v1 m;
    for (uint32_t i = 0; i < sizeof(m); i++) ((uint8_t *)&m)[i] = 0;
    hal_hdr(&m, HAL_CONFIG_BSS_REQ, 0, sizeof(m) - BSS_V1_TRIM);
    struct wcn36xx_hal_config_bss_params_v1 *b = &m.bss_params;
    for (int i = 0; i < 6; i++) { b->bssid[i] = g_bssid[i]; b->self_mac_addr[i] = wlan_self_mac()[i]; }
    b->bss_type = WCN36XX_HAL_INFRASTRUCTURE_MODE;
    b->oper_mode = 1;                                /* станция */
    b->nw_type = WCN36XX_HAL_11G_NW_TYPE;
    b->short_slot_time_supported = 1;
    b->beacon_interval = 100;
    b->dtim_period = 1;
    b->oper_channel = g_channel;
    b->ext_channel = 0;                              /* PHY_SINGLE_CHANNEL_CENTERED */
    b->ssid.length = g_ssid_len;
    for (int i = 0; i < g_ssid_len; i++) b->ssid.ssid[i] = g_ssid[i];
    b->action = update;
    b->ht = 1;
    b->wcn36xx_hal_persona = WCN36XX_HAL_STA_MODE;
    b->max_tx_power = 0x14;
    /* Пока не ассоциированы (add) — точки-собеседника ещё нет. */
    fill_sta_v1(&b->sta, update);
    wlan_hal_send(&m, sizeof(m) - BSS_V1_TRIM);
    say(update ? "STA: CONFIG_BSS (update)\n" : "STA: CONFIG_BSS (add)\n");
}

static void send_join(void) {
    static struct wcn36xx_hal_join_req_msg m;
    for (uint32_t i = 0; i < sizeof(m); i++) ((uint8_t *)&m)[i] = 0;
    hal_hdr(&m, HAL_JOIN_REQ, 0, sizeof(m));
    for (int i = 0; i < 6; i++) { m.bssid[i] = g_bssid[i]; m.self_sta_mac_addr[i] = wlan_self_mac()[i]; }
    m.channel = g_channel;
    m.link_state = LINK_PREASSOC;
    m.max_tx_power = 0x14;
    wlan_hal_send(&m, sizeof(m));
    say("STA: JOIN kanal "); say_dec(g_channel); say("\n");
}

static void send_link_state(int state) {
    static struct wcn36xx_hal_set_link_state_req_msg m;
    for (uint32_t i = 0; i < sizeof(m); i++) ((uint8_t *)&m)[i] = 0;
    hal_hdr(&m, HAL_SET_LINK_ST_REQ, 0, sizeof(m));
    for (int i = 0; i < 6; i++) { m.bssid[i] = g_bssid[i]; m.self_mac_addr[i] = wlan_self_mac()[i]; }
    m.state = state;
    wlan_hal_send(&m, sizeof(m));
}

static void send_stakey(void) {
    static struct wcn36xx_hal_set_sta_key_req_msg m;
    for (uint32_t i = 0; i < sizeof(m); i++) ((uint8_t *)&m)[i] = 0;
    hal_hdr(&m, HAL_SET_STAKEY_REQ, 0, sizeof(m));
    m.set_sta_key_params.sta_index = g_bss_sta_index;
    m.set_sta_key_params.enc_type = ED_CCMP;
    m.set_sta_key_params.key[0].id = 0;
    m.set_sta_key_params.key[0].unicast = 1;
    m.set_sta_key_params.key[0].direction = TX_RX;
    m.set_sta_key_params.key[0].length = 16;
    for (int i = 0; i < 16; i++) m.set_sta_key_params.key[0].key[i] = g_ptk[32 + i];   /* TK */
    m.set_sta_key_params.single_tid_rc = 1;
    wlan_hal_send(&m, sizeof(m));
    say("STA: SET_STAKEY (PTK)\n");
}

static void send_bsskey(void) {
    static struct wcn36xx_hal_set_bss_key_req_msg m;
    for (uint32_t i = 0; i < sizeof(m); i++) ((uint8_t *)&m)[i] = 0;
    hal_hdr(&m, HAL_SET_BSSKEY_REQ, 0, sizeof(m));
    m.bss_idx = g_bss_index;
    m.enc_type = ED_CCMP;
    m.num_keys = 1;
    m.keys[0].id = g_gtk_id;
    m.keys[0].unicast = 0;
    m.keys[0].direction = RX_ONLY;
    m.keys[0].length = 16;
    for (int i = 0; i < 16; i++) m.keys[0].key[i] = g_gtk[i];
    wlan_hal_send(&m, sizeof(m));
    say("STA: SET_BSSKEY (GTK)\n");
}

/* ---------------- кадры 802.11 ---------------- */

static void put_hdr(uint8_t *f, uint16_t fc, const uint8_t *a1, const uint8_t *a2, const uint8_t *a3) {
    f[0] = (uint8_t)fc; f[1] = (uint8_t)(fc >> 8); f[2] = 0; f[3] = 0;
    for (int i = 0; i < 6; i++) { f[4 + i] = a1[i]; f[10 + i] = a2[i]; f[16 + i] = a3[i]; }
    g_seq_ctrl += 0x10;
    f[22] = (uint8_t)g_seq_ctrl; f[23] = (uint8_t)(g_seq_ctrl >> 8);
}

/* Отправить управляющий кадр (высокий канал DXE). */
static void tx_mgmt(const uint8_t *frame, uint32_t len) {
    struct wcn36xx_tx_bd bd;
    for (uint32_t i = 0; i < sizeof(bd); i++) ((uint8_t *)&bd)[i] = 0;
    bd.dpu_rf = BMU_WQ_TX;
    bd.dpu_ne = 1;
    bd.sta_index = wlan_self_sta_index();
    bd.dpu_desc_idx = wlan_self_dpu_index();
    bd.bd_rate = BD_RATE_MGMT;
    bd.queue_id = TX_U_WQ_ID;
    bd.pdu.bd_ssn = 1;                       /* FILL_DPU_NON_QOS */
    bd.pdu.mpdu_header_len = 24;
    bd.pdu.mpdu_header_off = sizeof(bd);
    bd.pdu.mpdu_data_off = sizeof(bd) + 24;
    bd.pdu.mpdu_len = len;
    bd.tx_bd_sign = 0xbdbdbdbd;
    wcn_buff_to_be(&bd, sizeof(bd) / 4);
    wlan_dxe_tx(1, (const uint8_t *)&bd, sizeof(bd), frame, len);
}

/* Отправить кадр данных (низкий канал): EAPOL до установки ключей. */
static void tx_data(const uint8_t *frame, uint32_t len, int encrypt) {
    struct wcn36xx_tx_bd bd;
    for (uint32_t i = 0; i < sizeof(bd); i++) ((uint8_t *)&bd)[i] = 0;
    bd.dpu_rf = BMU_WQ_TX;
    bd.dpu_ne = encrypt ? 0 : 1;
    bd.dpu_sign = g_bss_sign;
    bd.sta_index = g_bss_sta_index;
    bd.dpu_desc_idx = g_bss_dpu;
    bd.bd_rate = BD_RATE_DATA;
    bd.queue_id = TX_U_WQ_ID;
    bd.pdu.bd_ssn = 1;
    bd.pdu.mpdu_header_len = 24;
    bd.pdu.mpdu_header_off = sizeof(bd);
    bd.pdu.mpdu_data_off = sizeof(bd) + 24;
    bd.pdu.mpdu_len = len;
    bd.tx_bd_sign = 0xbdbdbdbd;
    wcn_buff_to_be(&bd, sizeof(bd) / 4);
    wlan_dxe_tx(0, (const uint8_t *)&bd, sizeof(bd), frame, len);
}

static void send_auth(void) {
    uint8_t f[30];
    put_hdr(f, 0x00B0, g_bssid, wlan_self_mac(), g_bssid);   /* Authentication */
    f[24] = 0; f[25] = 0;      /* алгоритм: открытая система */
    f[26] = 1; f[27] = 0;      /* номер шага 1 */
    f[28] = 0; f[29] = 0;      /* код статуса */
    tx_mgmt(f, 30);
    say("STA: kadr Authentication\n");
}

static void send_assoc(void) {
    uint8_t f[128];
    uint32_t o = 24;
    put_hdr(f, 0x0000, g_bssid, wlan_self_mac(), g_bssid);   /* Association Request */
    /* capability: ESS + (Privacy, если WPA2) + short preamble/slot */
    uint16_t cap = 0x0001 | 0x0020 | 0x0400 | (g_secure ? 0x0010 : 0);
    f[o++] = (uint8_t)cap; f[o++] = (uint8_t)(cap >> 8);
    f[o++] = 10; f[o++] = 0;                        /* listen interval */
    f[o++] = 0; f[o++] = g_ssid_len;               /* SSID */
    for (int i = 0; i < g_ssid_len; i++) f[o++] = g_ssid[i];
    /* поддерживаемые скорости 1..11 (DSSS) */
    f[o++] = 1; f[o++] = 4; f[o++] = 0x82; f[o++] = 0x84; f[o++] = 0x8B; f[o++] = 0x96;
    /* расширенные скорости 6..54 (OFDM) */
    f[o++] = 50; f[o++] = 8;
    f[o++] = 0x0C; f[o++] = 0x12; f[o++] = 0x18; f[o++] = 0x24;
    f[o++] = 0x30; f[o++] = 0x48; f[o++] = 0x60; f[o++] = 0x6C;
    if (g_secure == 2) {
        /* RSN IE: WPA2-PSK, CCMP для пары и группы */
        f[o++] = 48; f[o++] = 20;
        f[o++] = 1; f[o++] = 0;                             /* версия */
        f[o++] = 0x00; f[o++] = 0x0F; f[o++] = 0xAC; f[o++] = 4;   /* группа: CCMP */
        f[o++] = 1; f[o++] = 0;                             /* пар. наборов: 1 */
        f[o++] = 0x00; f[o++] = 0x0F; f[o++] = 0xAC; f[o++] = 4;   /* пара: CCMP */
        f[o++] = 1; f[o++] = 0;                             /* AKM: 1 */
        f[o++] = 0x00; f[o++] = 0x0F; f[o++] = 0xAC; f[o++] = 2;   /* AKM: PSK */
        f[o++] = 0; f[o++] = 0;                             /* RSN capabilities */
    }
    tx_mgmt(f, o);
    say("STA: kadr Association Request\n");
}

/* ---------------- WPA2: рукопожатие ---------------- */

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t rd16be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void compute_ptk(const uint8_t *anonce) {
    uint8_t data[76];
    const uint8_t *aa = g_bssid, *spa = wlan_self_mac();
    int cmp = 0;
    for (int i = 0; i < 6; i++) { if (spa[i] != aa[i]) { cmp = spa[i] < aa[i] ? -1 : 1; break; } }
    const uint8_t *lo = cmp < 0 ? spa : aa, *hi = cmp < 0 ? aa : spa;
    for (int i = 0; i < 6; i++) { data[i] = lo[i]; data[6 + i] = hi[i]; }
    const uint8_t *na = anonce, *nb = g_snonce;
    int nc = 0;
    for (int i = 0; i < 32; i++) { if (na[i] != nb[i]) { nc = na[i] < nb[i] ? -1 : 1; break; } }
    const uint8_t *nlo = nc < 0 ? na : nb, *nhi = nc < 0 ? nb : na;
    for (int i = 0; i < 32; i++) { data[12 + i] = nlo[i]; data[44 + i] = nhi[i]; }
    wpa_prf(g_pmk, 32, "Pairwise key expansion", data, 76, g_ptk, 48);
}

/* MIC кадра EAPOL-Key: HMAC-SHA1 по кадру с обнулённым полем MIC. */
static void eapol_mic(const uint8_t *frame, uint32_t len, uint8_t mic[16]) {
    static uint8_t tmp[512];
    if (len > sizeof(tmp)) return;
    for (uint32_t i = 0; i < len; i++) tmp[i] = frame[i];
    for (int i = 0; i < 16; i++) tmp[81 + i] = 0;          /* поле MIC внутри key-frame */
    uint8_t h[20];
    hmac_sha1(g_ptk, 16, tmp, len, h);                     /* KCK = ptk[0..15] */
    for (int i = 0; i < 16; i++) mic[i] = h[i];
}

/* Собрать EAPOL-Key (2/4 или 4/4) и отправить кадром данных. */
static void send_eapol(uint16_t key_info, const uint8_t *nonce, int with_mic) {
    uint8_t f[24 + 8 + 99];
    uint32_t o = 0;
    /* заголовок данных: ToDS, к точке */
    put_hdr(f, 0x0108, g_bssid, wlan_self_mac(), g_bssid);
    o = 24;
    /* LLC/SNAP + EtherType EAPOL */
    f[o++] = 0xAA; f[o++] = 0xAA; f[o++] = 0x03; f[o++] = 0; f[o++] = 0; f[o++] = 0;
    f[o++] = 0x88; f[o++] = 0x8E;
    uint32_t eapol = o;
    f[o++] = 2;                          /* версия 802.1X-2004 */
    f[o++] = 3;                          /* тип: EAPOL-Key */
    uint32_t lenpos = o; o += 2;         /* длина тела — впишем позже */
    uint32_t body = o;
    f[o++] = 2;                          /* тип ключа: RSN */
    f[o++] = (uint8_t)(key_info >> 8); f[o++] = (uint8_t)key_info;
    f[o++] = 0; f[o++] = 16;             /* длина ключа (CCMP) */
    for (int i = 0; i < 8; i++) f[o++] = (uint8_t)(g_replay >> (56 - 8 * i));
    for (int i = 0; i < 32; i++) f[o++] = nonce ? nonce[i] : 0;   /* SNonce */
    for (int i = 0; i < 16; i++) f[o++] = 0;   /* IV */
    for (int i = 0; i < 8; i++) f[o++] = 0;    /* RSC */
    for (int i = 0; i < 8; i++) f[o++] = 0;    /* Key ID */
    uint32_t micpos = o;
    for (int i = 0; i < 16; i++) f[o++] = 0;   /* MIC */
    f[o++] = 0; f[o++] = 0;                     /* длина key data = 0 */
    f[lenpos] = (uint8_t)((o - body) >> 8); f[lenpos + 1] = (uint8_t)(o - body);
    if (with_mic) {
        uint8_t mic[16];
        eapol_mic(f + eapol, o - eapol, mic);
        for (int i = 0; i < 16; i++) f[micpos + i] = mic[i];
    }
    tx_data(f, o, 0);
}

/* Разобрать входящий EAPOL-Key (1/4 или 3/4). f — кадр 802.11 целиком. */
static void handle_eapol(const uint8_t *f, uint32_t len) {
    if (len < 24 + 8 + 95) return;
    uint32_t o = 24;
    /* пропустить QoS, если он есть */
    uint16_t fc = rd16le(f);
    if ((fc & 0x0080)) o += 2;                          /* QoS data */
    if (len < o + 8 + 95) return;
    if (!(f[o] == 0xAA && f[o + 1] == 0xAA && f[o + 6] == 0x88 && f[o + 7] == 0x8E)) return;
    const uint8_t *e = f + o + 8;                       /* EAPOL */
    if (e[1] != 3) return;                              /* не EAPOL-Key */
    const uint8_t *kf = e + 4;                          /* тело key */
    uint16_t ki = rd16be(kf + 1);
    const uint8_t *nonce = kf + 13;
    uint64_t replay = 0;
    for (int i = 0; i < 8; i++) replay = (replay << 8) | kf[5 + i];

    int pairwise = (ki >> 3) & 1, mic = (ki >> 8) & 1, ack = (ki >> 7) & 1,
        secure = (ki >> 9) & 1, enc = (ki >> 12) & 1;

    if (g_cst != C_HANDSHAKE && g_cst != C_SET_PTK) return;

    if (pairwise && ack && !mic) {
        /* Сообщение 1/4: ANonce. Считаем PTK, шлём 2/4. */
        say("STA: EAPOL 1/4 (ANonce)\n");
        for (int i = 0; i < 32; i++) g_anonce[i] = nonce[i];
        g_replay = replay;
        fill_random(g_snonce, 32);
        compute_ptk(g_anonce);
        /* Версию дескриптора ключа берём ту же, что прислала точка (для
           WPA2-CCMP это 2 = HMAC-SHA1). Свою ставить нельзя: точка сверяет
           MIC по своей версии и наш ответ отвергнет. */
        g_key_ver = ki & 7;
        uint16_t out = (uint16_t)(g_key_ver | 0x0108);  /* версия + pairwise + MIC */
        send_eapol(out, g_snonce, 1);
        say("STA: EAPOL 2/4 (SNonce+MIC)\n");
        g_deadline = hal_time_ms() + 2000;
    } else if (pairwise && ack && mic && secure) {
        /* Сообщение 3/4: GTK внутри зашифрованных данных. */
        say("STA: EAPOL 3/4 (GTK)\n");
        g_replay = replay;
        uint16_t kdlen = rd16be(kf + 93);
        const uint8_t *kd = kf + 95;
        if (enc && kdlen >= 24 && kdlen <= 200 && (kdlen % 8) == 0) {
            uint8_t plain[200];
            if (aes_key_unwrap(g_ptk + 16, kd, kdlen / 8 - 1, plain)) {
                /* разбор KDE: ищем GTK (00-0F-AC тип 1) */
                uint32_t p = 0, n = kdlen - 8;
                while (p + 2 <= n) {
                    uint8_t dlen = plain[p + 1];
                    if (plain[p] == 0xDD && dlen >= 6 &&
                        plain[p + 2] == 0x00 && plain[p + 3] == 0x0F &&
                        plain[p + 4] == 0xAC && plain[p + 5] == 1) {
                        g_gtk_id = plain[p + 6] & 3;
                        g_gtk_len = dlen - 6;
                        if (g_gtk_len > 32) g_gtk_len = 32;
                        for (int i = 0; i < g_gtk_len; i++) g_gtk[i] = plain[p + 8 + i];
                    }
                    p += 2u + dlen;
                }
            } else {
                say("STA: GTK ne raskrylsya (parol neveren?)\n");
            }
        }
        /* 4/4: подтверждение */
        uint16_t out = (uint16_t)(g_key_ver | 0x0308);  /* версия + pairwise + MIC + secure */
        send_eapol(out, 0, 1);
        say("STA: EAPOL 4/4\n");
        g_msg3_secure = 1;
        g_cst = C_SET_PTK;
        g_deadline = hal_time_ms() + 3000;
    }
}

/* ---------------- разбор управляющих ответов ---------------- */

void wlan_sta_frame(const uint8_t *f, uint32_t len, int rssi) {
    if (len < 24) return;
    /* только от нашей точки */
    int from_bss = 1;
    for (int i = 0; i < 6; i++) if (f[10 + i] != g_bssid[i]) { from_bss = 0; break; }
    if (!from_bss) return;
    g_signal = (int8_t)rssi;

    uint16_t fc = rd16le(f);
    uint16_t type = fc & 0x0C, sub = fc & 0xF0;

    if (type == 0x00 && sub == 0xB0 && g_cst == C_AUTH) {          /* Authentication */
        uint16_t status = len >= 30 ? rd16le(f + 28) : 1;
        say("STA: otvet Authentication, status "); say_dec(status); say("\n");
        if (status == 0) { g_cst = C_ASSOC; g_tries = 0; send_assoc(); g_deadline = hal_time_ms() + 2000; }
        else cfail("tochka otklonila autentifikaciyu");
    } else if (type == 0x00 && (sub == 0x10) && g_cst == C_ASSOC) { /* Association Response */
        uint16_t status = len >= 28 ? rd16le(f + 26) : 1;
        g_aid = len >= 30 ? (rd16le(f + 28) & 0x3FFF) : 0;
        say("STA: otvet Association, status "); say_dec(status); say(", AID "); say_dec(g_aid); say("\n");
        if (status == 0) { g_cst = C_LINK_POST; send_link_state(LINK_POSTASSOC); g_deadline = hal_time_ms() + 2000; }
        else cfail("tochka otklonila associaciyu");
    } else if (type == 0x08) {                                      /* Data */
        uint32_t o = 24;
        if (fc & 0x0080) o += 2;                                    /* QoS data */
        if (len < o + 8) return;
        if (!(f[o] == 0xAA && f[o + 1] == 0xAA && f[o + 6] == 0x88 && f[o + 7] == 0x8E)) {
            /* не EAPOL — обычные данные: снимаем LLC/SNAP, отдаём стеку */
            if (g_cst != C_UP && g_cst != C_DHCP) return;
            uint16_t et = rd16be(f + o + 6);
            net_input(f + 16, wlan_self_mac(), et, f + o + 8, len - o - 8);
            return;
        }
        handle_eapol(f, len);
    } else if (type == 0x00 && sub == 0xC0) {                       /* Deauthentication */
        if (g_cst != C_IDLE && g_cst != C_UP) cfail("tochka otklyuchila (deauth)");
    }
}

/* Ответы HAL. 1 — сообщение наше. */
int wlan_sta_hal_rx(uint32_t type, const uint8_t *d, uint32_t len) {
    uint32_t status = len >= 12 ? (uint32_t)d[8] : 1;
    switch (type) {
    case HAL_SET_LINK_ST_RSP:
        if (g_cst == C_LINK_PRE) { g_cst = C_JOIN; send_join(); g_deadline = hal_time_ms() + 3000; }
        else if (g_cst == C_LINK_POST) { g_cst = C_BSS_UPD; send_config_bss(1); g_deadline = hal_time_ms() + 3000; }
        return 1;
    case HAL_JOIN_RSP:
        if (g_cst == C_JOIN) { g_cst = C_BSS_ADD; send_config_bss(0); g_deadline = hal_time_ms() + 3000; }
        return 1;
    case HAL_CONFIG_BSS_RSP:
        /* ответ: status, bss_index, dpu_desc_index, ucast_dpu_signature,
           ..., bss_sta_index на смещении 12 */
        if (len < 20) return 1;
        if (d[8] != 0) { cfail("CONFIG_BSS otkaz"); return 1; }
        g_bss_index = d[9];
        g_bss_dpu = d[10];
        g_bss_sign = d[11];
        g_bss_sta_index = d[16];
        if (g_cst == C_BSS_ADD) {
            say("STA: BSS zaveden, indeks "); say_dec(g_bss_index);
            say(" sta "); say_dec(g_bss_sta_index); say("\n");
            g_cst = C_AUTH; g_tries = 0; send_auth(); g_deadline = hal_time_ms() + 2000;
        } else if (g_cst == C_BSS_UPD) {
            g_cst = C_STA_CFG; send_config_sta(); g_deadline = hal_time_ms() + 3000;
        }
        return 1;
    case HAL_CONFIG_STA_RSP:
        if (g_cst == C_STA_CFG) {
            if (d[8] != 0) { cfail("CONFIG_STA otkaz"); return 1; }
            if (len >= 8) { /* sta_index в params на смещении 8+4 */ }
            say("STA: stanciya nastroena\n");
            if (g_secure == 2) {
                g_cst = C_HANDSHAKE;
                say("STA: zhdu rukopozhatie WPA2\n");
                g_deadline = hal_time_ms() + 5000;
            } else {
                g_cst = C_DHCP;
                net_link_up();
                g_deadline = hal_time_ms() + 15000;
            }
        }
        return 1;
    case HAL_SET_STAKEY_RSP:
        if (g_cst == C_SET_PTK) { g_cst = C_SET_GTK; send_bsskey(); g_deadline = hal_time_ms() + 2000; }
        return 1;
    case HAL_SET_BSSKEY_RSP:
        if (g_cst == C_SET_GTK) {
            say("STA: klyuchi ustanovleny, shifrovanie vklyucheno\n");
            g_cst = C_DHCP;
            net_link_up();
            g_deadline = hal_time_ms() + 15000;
        }
        return 1;
    }
    (void)status;
    return 0;
}

/* ---------------- прокрутка ---------------- */

void wlan_sta_pump(void) {
    if (g_cst == C_IDLE || g_cst == C_UP || g_cst == C_FAIL) {
        if (g_cst == C_UP || g_cst == C_DHCP) net_poll();
        return;
    }
    net_poll();
    uint64_t now = hal_time_ms();

    if (g_cst == C_SET_PTK && g_msg3_secure) {
        /* 4/4 отправлен — ставим PTK. */
        g_msg3_secure = 0;
        send_stakey();
        g_deadline = now + 2000;
        return;
    }

    if (g_cst == C_DHCP) {
        net_status_t s;
        net_status(&s);
        if (s.dhcp == NET_DHCP_BOUND) {
            char a[16]; net_format_ip(s.ip, a);
            say("STA: podklyucheno, adres "); say(a); say("\n");
            g_cst = C_UP;
            wlan_conn_changed();
        } else if (s.dhcp == NET_DHCP_FAILED || now > g_deadline) {
            cfail("DHCP ne dal adres");
        }
        return;
    }

    if (now <= g_deadline) return;

    /* Таймаут — повтор шагов, где точка могла не расслышать кадр. */
    if (++g_tries > 4) { cfail("tochka ne otvechaet"); return; }
    switch (g_cst) {
    case C_AUTH:  send_auth();  g_deadline = now + 2000; break;
    case C_ASSOC: send_assoc(); g_deadline = now + 2000; break;
    case C_HANDSHAKE: cfail("net rukopozhatiya (parol neveren?)"); break;
    default: cfail("shag podklyucheniya ne otvetil"); break;
    }
}

/* ---------------- API ---------------- */

int wlan_sta_active(void) { return g_cst != C_IDLE && g_cst != C_FAIL; }
int wlan_sta_state(void) { return g_cst; }
int wlan_sta_connected(void) { return g_cst == C_UP; }
int8_t wlan_sta_signal(void) { return g_signal; }
const uint8_t *wlan_sta_ssid(uint8_t *len) { if (len) *len = g_ssid_len; return g_ssid; }

void wlan_sta_connect(const uint8_t *bssid, const uint8_t *ssid, uint8_t ssid_len,
                      uint8_t channel, uint8_t secure, const char *pass) {
    if (!wlan_hal_ready()) { say("STA: radio ne gotovo\n"); return; }
    for (int i = 0; i < 6; i++) g_bssid[i] = bssid[i];
    g_ssid_len = ssid_len > 32 ? 32 : ssid_len;
    for (int i = 0; i < g_ssid_len; i++) g_ssid[i] = ssid[i];
    g_ssid[g_ssid_len] = 0;
    g_channel = channel;
    g_secure = secure;
    int i = 0;
    if (pass) { for (; pass[i] && i < 63; i++) g_pass[i] = pass[i]; }
    g_pass[i] = 0;
    g_seq_ctrl = 0;
    g_aid = 0;
    g_bss_index = 0xFF;
    g_signal = 0;

    net_init(wlan_self_mac());

    if (g_secure == 2) {
        say("STA: schitayu PMK iz parolya...\n");
        wpa_psk(g_pass, g_ssid, g_ssid_len, g_pmk);   /* 4096 оборотов, ~доли секунды */
    }

    say("STA: podklyuchayus k "); say((const char *)g_ssid); say("\n");
    g_cst = C_LINK_PRE;
    g_tries = 0;
    send_link_state(LINK_PREASSOC);
    g_deadline = hal_time_ms() + 2000;
    wlan_conn_changed();
}

void wlan_sta_disconnect(void) {
    if (g_cst == C_IDLE) return;
    net_link_down();
    send_link_state(LINK_IDLE);
    g_cst = C_IDLE;
    wlan_conn_changed();
}

/* ---------------- канальный уровень для сетевого стека ---------------- */

/* net.c отдаёт кадр в стиле Ethernet: куда, тип, данные. Оборачиваем в
   802.11-кадр данных (ToDS) с LLC/SNAP и шлём точке. */
int net_link_output(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, uint32_t len) {
    if (g_cst != C_UP && g_cst != C_DHCP) return 0;
    static uint8_t f[24 + 8 + 1500];
    if (len > 1500) return 0;
    put_hdr(f, 0x0108, g_bssid, wlan_self_mac(), dst);
    uint32_t o = 24;
    f[o++] = 0xAA; f[o++] = 0xAA; f[o++] = 0x03; f[o++] = 0; f[o++] = 0; f[o++] = 0;
    f[o++] = (uint8_t)(ethertype >> 8); f[o++] = (uint8_t)ethertype;
    for (uint32_t i = 0; i < len; i++) f[o++] = p[i];
    tx_data(f, o, g_secure ? 1 : 0);
    return 1;
}

void net_log(const char *s) { early_con_puts(s); }
