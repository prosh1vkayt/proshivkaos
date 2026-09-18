/* gui/touch/app_wifi.c — экран Wi-Fi: список сетей и подключение.
 *
 * Как в Android: карточка радио с переключателем, список сетей по силе
 * сигнала, а по касанию — подключение. Для закрытой сети снизу
 * выезжает поле пароля с экранной клавиатурой; открытая подключается
 * сразу. Наверху — строка состояния подключения: «подключаюсь…»,
 * «в сети — адрес», «не удалось».
 *
 * Радио и подключение живут в фоне (arch/arm64/wlan*.c); экран лишь
 * опрашивает hal_wifi_* через tick() и перерисовывается на изменениях. */
#include "touch_app.h"
#include "icon.h"
#include "touch_theme.h"
#include "hal_input.h"
#include "hal_wifi.h"

/* Заглушки для сборок без радио — телефонная сборка их перекрывает. */
__attribute__((weak)) int hal_wifi_state(void) { return HAL_WIFI_UNSUPPORTED; }
__attribute__((weak)) void hal_wifi_enable(void) { }
__attribute__((weak)) void hal_wifi_scan(void) { }
__attribute__((weak)) int hal_wifi_count(void) { return 0; }
__attribute__((weak)) int hal_wifi_get(int i, hal_wifi_net_t *o) { (void)i; (void)o; return 0; }
__attribute__((weak)) uint32_t hal_wifi_generation(void) { return 0; }
__attribute__((weak)) void hal_wifi_connect(int i, const char *p) { (void)i; (void)p; }
__attribute__((weak)) void hal_wifi_disconnect(void) { }
__attribute__((weak)) int hal_wifi_conn_state(void) { return HAL_WIFI_CONN_IDLE; }
__attribute__((weak)) const char *hal_wifi_conn_ssid(void) { return ""; }
__attribute__((weak)) void hal_wifi_ip(char *b) { b[0] = 0; }

#define MAX_SHOWN 16

static int g_x, g_y, g_w, g_h;
static uint32_t g_seen_gen = 0xFFFFFFFFu;
static int g_seen_state = -1, g_seen_conn = -1;
static int g_pressed = -1;          /* 0 — переключатель, 1 — «обновить» */
static int g_scroll, g_drag_y0, g_drag_scroll0, g_drag_dist;

static hal_wifi_net_t g_list[MAX_SHOWN];
static int g_list_idx[MAX_SHOWN];
static int g_count;

/* Ввод пароля. */
static int g_pw_mode;               /* показываем поле пароля */
static int g_pw_target;             /* индекс сети (scan-order) */
static int g_pw_secure;
static char g_pw[64];
static int g_pw_len;
static char g_pw_ssid[33];

static int header_h(void) { return TM.touch; }
static int status_h(void) { return TM.touch * 2; }
static int row_h(void)    { return TM.touch + TM.pad * 2; }
static int pw_h(void)     { return TM.touch * 2; }

static void wifi_init(void) {
    g_seen_gen = 0xFFFFFFFFu;
    g_pressed = -1; g_scroll = 0; g_pw_mode = 0;
}

static void wifi_layout(int x, int y, int w, int h) { g_x = x; g_y = y; g_w = w; g_h = h; }

static void refresh_list(void) {
    int n = hal_wifi_count();
    g_count = 0;
    for (int i = 0; i < n && g_count < MAX_SHOWN; i++) {
        hal_wifi_net_t net;
        if (!hal_wifi_get(i, &net)) continue;
        int k = g_count++;
        while (k > 0 && g_list[k - 1].rssi < net.rssi) {
            g_list[k] = g_list[k - 1]; g_list_idx[k] = g_list_idx[k - 1]; k--;
        }
        g_list[k] = net; g_list_idx[k] = i;
    }
}

static int wifi_tick(void) {
    uint32_t gen = hal_wifi_generation();
    int st = hal_wifi_state(), cs = hal_wifi_conn_state();
    if (gen == g_seen_gen && st == g_seen_state && cs == g_seen_conn) return 0;
    g_seen_gen = gen; g_seen_state = st; g_seen_conn = cs;
    refresh_list();
    return 1;
}

static uint8_t level_color(int rssi) {
    if (rssi >= -55) return GFX_UI_OK;
    if (rssi >= -70) return GFX_UI_ACCENT;
    if (rssi >= -80) return GFX_UI_WARN;
    return GFX_UI_TEXT_DIM;
}

static void status_rects(int *sx, int *sy, int *sw, int *sh, int *rx, int *ry, int *rs) {
    int cx = g_x + TM.pad, cy = g_y + header_h() + TM.pad;
    int cw = g_w - TM.pad * 2, ch = status_h();
    int sww = TM.touch * 3 / 2, swh = TM.touch / 2;
    *sx = cx + cw - TM.pad * 2 - sww;
    *sy = cy + (ch - swh) / 2;
    *sw = sww; *sh = swh;
    *rs = TM.touch;
    *rx = *sx - TM.gap - *rs;
    *ry = cy + (ch - *rs) / 2;
}

static const char *state_text(int st, int count) {
    static char buf[32];
    switch (st) {
    case HAL_WIFI_UNSUPPORTED: return "NO RADIO IN THIS BUILD";
    case HAL_WIFI_OFF:         return "OFF";
    case HAL_WIFI_STARTING:    return "TURNING ON...";
    case HAL_WIFI_SCANNING:    return "SEARCHING...";
    case HAL_WIFI_FAILED:      return "RADIO ERROR";
    default: {
        char num[12]; int pos = 0; buf[0] = 0;
        pos = touch_strcat(buf, pos, sizeof(buf), touch_itoa(count, num, 0));
        touch_strcat(buf, pos, sizeof(buf), count == 1 ? " NETWORK" : " NETWORKS");
        return buf;
    }
    }
}

/* Полоса состояния подключения. Возвращает свою высоту (0 — ничего). */
static int draw_conn_banner(int x, int y, int w) {
    int cs = hal_wifi_conn_state();
    if (cs == HAL_WIFI_CONN_IDLE) return 0;
    int h = status_h();
    uint8_t col = cs == HAL_WIFI_CONN_ONLINE ? GFX_UI_OK :
                  cs == HAL_WIFI_CONN_FAILED ? GFX_UI_WARN : GFX_UI_SURFACE_2;
    touch_draw_card(x, y, w, h, col == GFX_UI_SURFACE_2 ? GFX_UI_SURFACE_2 : GFX_UI_SURFACE);
    int is = TM.touch;
    icon_draw(cs == HAL_WIFI_CONN_FAILED ? ICON_WIFI_OFF : ICON_WIFI,
              x + TM.pad * 2, y + (h - is) / 2, is, hal_gfx_palette_rgb(col));
    int tx = x + TM.pad * 4 + is;
    char l1[48]; int p = 0; l1[0] = 0;
    if (cs == HAL_WIFI_CONN_ONLINE) p = touch_strcat(l1, p, sizeof(l1), "CONNECTED  ");
    else if (cs == HAL_WIFI_CONN_FAILED) p = touch_strcat(l1, p, sizeof(l1), "FAILED  ");
    else p = touch_strcat(l1, p, sizeof(l1), "CONNECTING  ");
    touch_strcat(l1, p, sizeof(l1), hal_wifi_conn_ssid());
    hal_gfx_draw_string_scaled(tx, y + h / 2 - FONT_H * TM.scale_small - TM.gap / 4,
                               l1, GFX_UI_TEXT, TM.scale_small);
    char l2[24];
    if (cs == HAL_WIFI_CONN_ONLINE) { hal_wifi_ip(l2); }
    else if (cs == HAL_WIFI_CONN_FAILED) { l2[0] = 0; touch_strcat(l2, 0, sizeof(l2), "TAP TO RETRY"); }
    else { l2[0] = 0; touch_strcat(l2, 0, sizeof(l2), "PLEASE WAIT"); }
    hal_gfx_draw_string_scaled(tx, y + h / 2 + TM.gap / 4, l2, GFX_UI_TEXT_DIM, TM.scale_small);
    return h + TM.pad;
}

/* Поле ввода пароля внизу. */
static void draw_pw(void) {
    int h = pw_h();
    int y = g_y + g_h - h;
    touch_draw_card(g_x, y, g_w, h, GFX_UI_SURFACE_2);
    char head[48]; int p = 0; head[0] = 0;
    p = touch_strcat(head, p, sizeof(head), "PASSWORD FOR ");
    touch_strcat(head, p, sizeof(head), g_pw_ssid);
    hal_gfx_draw_string_scaled(g_x + TM.pad * 2, y + TM.pad, head, GFX_UI_TEXT_DIM, TM.scale_small);
    char stars[33];
    int n = g_pw_len < 32 ? g_pw_len : 32;
    for (int i = 0; i < n; i++) stars[i] = '*';
    stars[n] = 0;
    int fy = y + TM.pad * 2 + FONT_H * TM.scale_small;
    hal_gfx_fill_rect(g_x + TM.pad * 2, fy, g_w - TM.pad * 4, TM.touch, GFX_UI_BG);
    hal_gfx_draw_string_scaled(g_x + TM.pad * 3, fy + (TM.touch - FONT_H * TM.scale) / 2,
                               n ? stars : "", GFX_UI_TEXT, TM.scale);
    /* кнопка CONNECT */
    int bw = TM.touch * 3, bh = TM.touch, bx = g_x + g_w - TM.pad * 2 - bw, by = fy;
    touch_draw_button(bx, by, bw, bh, "CONNECT", 0, GFX_UI_ACCENT, GFX_UI_ACCENT_DARK);
}

static void wifi_render(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);
    int st = hal_wifi_state();
    int on = st >= HAL_WIFI_STARTING && st != HAL_WIFI_FAILED;

    int top = g_y + header_h() + TM.pad * 2 + status_h();
    int banner = draw_conn_banner(g_x + TM.pad, top - TM.pad, g_w - TM.pad * 2);
    int list_y = top + banner;
    int list_h = g_y + g_h - list_y - (g_pw_mode ? pw_h() : 0);
    if (list_h < TM.touch) list_h = TM.touch;

    int content = g_count * (row_h() + TM.gap) + TM.pad;
    int max_scroll = content > list_h ? content - list_h : 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    hal_gfx_set_clip(g_x, list_y, g_w, list_h);
    for (int i = 0; i < g_count; i++) {
        const hal_wifi_net_t *n = &g_list[i];
        int x = g_x + TM.pad, w = g_w - TM.pad * 2, h = row_h();
        int y = list_y + TM.pad + i * (h + TM.gap) - g_scroll;
        if (y + h < list_y || y > list_y + list_h) continue;
        touch_draw_card(x, y, w, h, GFX_UI_SURFACE);
        int is = TM.touch * 3 / 4, ix = x + TM.pad * 2, iy = y + (h - is) / 2;
        icon_draw(ICON_WIFI, ix, iy, is, hal_gfx_palette_rgb(level_color(n->rssi)));
        int tx = ix + is + TM.pad * 2;
        int l1 = y + h / 2 - FONT_H * TM.scale_small - TM.gap / 4;
        int l2 = y + h / 2 + TM.gap / 4;
        hal_gfx_draw_string_scaled(tx, l1, n->ssid[0] ? n->ssid : "(HIDDEN NETWORK)",
                                   n->ssid[0] ? GFX_UI_TEXT : GFX_UI_TEXT_DIM, TM.scale_small);
        char sub[40], num[12]; int p = 0; sub[0] = 0;
        p = touch_strcat(sub, p, sizeof(sub), "CH ");
        p = touch_strcat(sub, p, sizeof(sub), touch_itoa(n->channel, num, 0));
        p = touch_strcat(sub, p, sizeof(sub), "  ");
        p = touch_strcat(sub, p, sizeof(sub), touch_itoa(n->rssi, num, 0));
        p = touch_strcat(sub, p, sizeof(sub), " DBM  ");
        touch_strcat(sub, p, sizeof(sub), n->security == HAL_WIFI_WPA2 ? "WPA2" :
                     n->security == HAL_WIFI_PROTECTED ? "SECURED" : "OPEN");
        hal_gfx_draw_string_scaled(tx, l2, sub, GFX_UI_TEXT_DIM, TM.scale_small);
        if (n->security != HAL_WIFI_OPEN) {
            int ls = TM.touch / 2;
            icon_draw(ICON_LOCK, x + w - TM.pad * 2 - ls, y + (h - ls) / 2, ls,
                      hal_gfx_palette_rgb(GFX_UI_TEXT_DIM));
        }
    }
    hal_gfx_reset_clip();

    /* заголовок и карточка радио поверх */
    hal_gfx_fill_rect(g_x, g_y, g_w, top - TM.pad - g_y, GFX_UI_BG);
    touch_draw_screen_header(g_x, g_y, g_w, "WI-FI");
    int cx = g_x + TM.pad, cy = g_y + header_h() + TM.pad, cw = g_w - TM.pad * 2, ch = status_h();
    touch_draw_card(cx, cy, cw, ch, GFX_UI_SURFACE_2);
    int is = TM.touch;
    icon_draw(on ? ICON_WIFI : ICON_WIFI_OFF, cx + TM.pad * 2, cy + (ch - is) / 2, is,
              hal_gfx_palette_rgb(on ? GFX_UI_ACCENT : GFX_UI_TEXT_DIM));
    int tx = cx + TM.pad * 4 + is;
    hal_gfx_draw_string_scaled(tx, cy + ch / 2 - FONT_H * TM.scale_small - TM.gap / 4,
                               "WI-FI", GFX_UI_TEXT, TM.scale_small);
    hal_gfx_draw_string_scaled(tx, cy + ch / 2 + TM.gap / 4, state_text(st, g_count),
                               st == HAL_WIFI_FAILED ? GFX_UI_WARN : GFX_UI_TEXT_DIM, TM.scale_small);
    if (st != HAL_WIFI_UNSUPPORTED) {
        int sx, sy, sw, sh, rx, ry, rs;
        status_rects(&sx, &sy, &sw, &sh, &rx, &ry, &rs);
        touch_draw_switch(sx, sy, sw, sh, on);
        if (st == HAL_WIFI_READY) {
            uint8_t c = g_pressed == 1 ? GFX_UI_ACCENT_DARK : GFX_UI_ACCENT;
            icon_draw(ICON_REFRESH, rx + rs / 8, ry + rs / 8, rs * 3 / 4, hal_gfx_palette_rgb(c));
        }
    }

    if (g_pw_mode) draw_pw();
}

static int hit_control(int px, int py) {
    int sx, sy, sw, sh, rx, ry, rs;
    status_rects(&sx, &sy, &sw, &sh, &rx, &ry, &rs);
    if (touch_hit_padded(px, py, sx, sy, sw, sh)) return 0;
    if (touch_hit_padded(px, py, rx, ry, rs, rs)) return 1;
    return -1;
}

static int list_geom(int *ly, int *lh) {
    int top = g_y + header_h() + TM.pad * 2 + status_h();
    int banner = hal_wifi_conn_state() != HAL_WIFI_CONN_IDLE ? status_h() + TM.pad : 0;
    *ly = top + banner;
    *lh = g_y + g_h - *ly - (g_pw_mode ? pw_h() : 0);
    return top - TM.pad;
}

static void begin_password(int scan_idx, const hal_wifi_net_t *n) {
    g_pw_mode = 1; g_pw_len = 0; g_pw[0] = 0;
    g_pw_target = scan_idx; g_pw_secure = n->security;
    int i = 0; for (; i < 32 && n->ssid[i]; i++) g_pw_ssid[i] = n->ssid[i];
    g_pw_ssid[i] = 0;
    touch_ui_keyboard(1);
}

static void end_password(void) {
    g_pw_mode = 0;
    touch_ui_keyboard(0);
}

static void wifi_on_touch(int type, int x, int y) {
    if (type == HAL_EV_POINTER_DOWN) {
        g_pressed = hit_control(x, y);
        g_drag_y0 = y; g_drag_scroll0 = g_scroll; g_drag_dist = 0;
        return;
    }
    if (type == HAL_EV_POINTER_MOVE) {
        int d = y - g_drag_y0; g_drag_dist = d < 0 ? -d : d;
        if (g_pressed < 0) g_scroll = g_drag_scroll0 - d;
        return;
    }
    if (type != HAL_EV_POINTER_UP) return;

    int bx, by, bw, bh;
    touch_header_back_rect(g_x, g_y, &bx, &by, &bw, &bh);
    if (g_drag_dist < TM.touch / 4 && touch_hit_padded(x, y, bx, by, bw, bh)) {
        if (g_pw_mode) { end_password(); return; }
        g_pressed = -1; touch_ui_go_home(); return;
    }

    /* кнопка CONNECT в поле пароля */
    if (g_pw_mode) {
        int fy = g_y + g_h - pw_h() + TM.pad * 2 + FONT_H * TM.scale_small;
        int cbw = TM.touch * 3, cbh = TM.touch, cbx = g_x + g_w - TM.pad * 2 - cbw;
        if (touch_hit_padded(x, y, cbx, fy, cbw, cbh)) {
            hal_wifi_connect(g_pw_target, g_pw);
            end_password();
        }
        return;
    }

    int was = g_pressed; g_pressed = -1;
    if (was >= 0 && hit_control(x, y) == was) {
        int st = hal_wifi_state();
        if (was == 0) {
            if (st == HAL_WIFI_OFF || st == HAL_WIFI_FAILED) hal_wifi_enable();
            else { hal_wifi_disconnect(); }
        } else if (was == 1 && st == HAL_WIFI_READY) hal_wifi_scan();
        g_seen_gen = 0xFFFFFFFFu;
        return;
    }
    if (g_drag_dist >= TM.touch / 4) return;

    /* попадание в строку сети */
    int ly, lh; list_geom(&ly, &lh);
    for (int i = 0; i < g_count; i++) {
        int rx = g_x + TM.pad, rw = g_w - TM.pad * 2, rh = row_h();
        int ry = ly + TM.pad + i * (rh + TM.gap) - g_scroll;
        if (ry + rh < ly || ry > ly + lh) continue;
        if (touch_hit(x, y, rx, ry, rw, rh)) {
            if (g_list[i].security == HAL_WIFI_OPEN) {
                hal_wifi_connect(g_list_idx[i], 0);
            } else {
                begin_password(g_list_idx[i], &g_list[i]);
            }
            g_seen_gen = 0xFFFFFFFFu;
            return;
        }
    }
}

static void wifi_on_key(int key) {
    if (!g_pw_mode) return;
    if (key == '\n' || key == '\r') {
        hal_wifi_connect(g_pw_target, g_pw);
        end_password();
    } else if (key == 8 || key == 127) {
        if (g_pw_len) g_pw[--g_pw_len] = 0;
    } else if (key >= 32 && key < 127 && g_pw_len < 63) {
        g_pw[g_pw_len++] = (char)key; g_pw[g_pw_len] = 0;
    }
    g_seen_gen = 0xFFFFFFFFu;
}

static int wifi_wants_keyboard(void) { return g_pw_mode; }

const touch_app_t app_wifi = {
    .name  = "WI-FI",
    .glyph = "W",
    .icon  = ICON_WIFI + 1,
    .color = GFX_UI_ACCENT,
    .color2 = GFX_UI_ACCENT_DARK,
    .init  = wifi_init,
    .layout = wifi_layout,
    .render = wifi_render,
    .on_touch = wifi_on_touch,
    .on_key = wifi_on_key,
    .wants_keyboard = wifi_wants_keyboard,
    .tick = wifi_tick
};
