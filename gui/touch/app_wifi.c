/* gui/touch/app_wifi.c — экран Wi-Fi.
 *
 * Как в Android: сверху карточка с переключателем и состоянием радио,
 * под ней — найденные сети, сильные выше. У каждой — значок уровня,
 * имя, замок, если сеть закрыта, и строка с каналом и уровнем в dBm.
 *
 * Радио поднимается и сканирует в фоне (arch/arm64/wlan.c), поэтому
 * экран не ждёт: он спрашивает hal_wifi_generation() на каждом обороте
 * оболочки и перерисовывается, когда что-то изменилось. */
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
__attribute__((weak)) int hal_wifi_get(int index, hal_wifi_net_t *out) { (void)index; (void)out; return 0; }
__attribute__((weak)) uint32_t hal_wifi_generation(void) { return 0; }

#define MAX_SHOWN 16

static int g_x, g_y, g_w, g_h;
static uint32_t g_seen_gen = 0xFFFFFFFFu;
static int g_seen_state = -1;
static int g_pressed = -1;          /* 0 — переключатель, 1 — «обновить» */
static int g_scroll, g_drag_y0, g_drag_scroll0, g_drag_dist, g_content_h;

static hal_wifi_net_t g_list[MAX_SHOWN];
static int g_count;

static int header_h(void) { return TM.touch; }
static int status_h(void) { return TM.touch * 2; }
static int row_h(void)    { return TM.touch + TM.pad * 2; }

static void wifi_init(void) {
    g_seen_gen = 0xFFFFFFFFu;
    g_pressed = -1;
    g_scroll = 0;
}

static void wifi_layout(int x, int y, int w, int h) {
    g_x = x; g_y = y; g_w = w; g_h = h;
}

/* Сети — в локальную копию, отсортированные по уровню. */
static void refresh_list(void) {
    int n = hal_wifi_count();
    g_count = 0;
    for (int i = 0; i < n && g_count < MAX_SHOWN; i++) {
        hal_wifi_net_t net;
        if (!hal_wifi_get(i, &net)) continue;
        int k = g_count++;
        while (k > 0 && g_list[k - 1].rssi < net.rssi) { g_list[k] = g_list[k - 1]; k--; }
        g_list[k] = net;
    }
}

static int wifi_tick(void) {
    uint32_t gen = hal_wifi_generation();
    int st = hal_wifi_state();
    if (gen == g_seen_gen && st == g_seen_state) return 0;
    g_seen_gen = gen;
    g_seen_state = st;
    refresh_list();
    return 1;
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
        char num[12];
        int pos = 0;
        buf[0] = 0;
        pos = touch_strcat(buf, pos, sizeof(buf), touch_itoa(count, num, 0));
        touch_strcat(buf, pos, sizeof(buf), count == 1 ? " NETWORK" : " NETWORKS");
        return buf;
    }
    }
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

/* Значок уровня: цвет по силе сигнала, как «палочки» в статусбаре. */
static uint8_t level_color(int rssi) {
    if (rssi >= -55) return GFX_UI_OK;
    if (rssi >= -70) return GFX_UI_ACCENT;
    if (rssi >= -80) return GFX_UI_WARN;
    return GFX_UI_TEXT_DIM;
}

static void wifi_render(void) {
    hal_gfx_fill_rect(g_x, g_y, g_w, g_h, GFX_UI_BG);

    int st = hal_wifi_state();
    int on = st >= HAL_WIFI_STARTING && st != HAL_WIFI_FAILED;

    /* --- список (под заголовком и карточкой состояния, прокручивается) --- */
    int list_y = g_y + header_h() + TM.pad * 2 + status_h();
    int list_h = g_y + g_h - list_y;
    g_content_h = g_count * (row_h() + TM.gap) + TM.pad;
    int max_scroll = g_content_h > list_h ? g_content_h - list_h : 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    hal_gfx_set_clip(g_x, list_y, g_w, list_h);
    for (int i = 0; i < g_count; i++) {
        const hal_wifi_net_t *n = &g_list[i];
        int x = g_x + TM.pad, w = g_w - TM.pad * 2, h = row_h();
        int y = list_y + TM.pad + i * (h + TM.gap) - g_scroll;
        if (y + h < list_y || y > list_y + list_h) continue;
        touch_draw_card(x, y, w, h, GFX_UI_SURFACE);

        int is = TM.touch * 3 / 4;
        int ix = x + TM.pad * 2, iy = y + (h - is) / 2;
        icon_draw(ICON_WIFI, ix, iy, is, hal_gfx_palette_rgb(level_color(n->rssi)));

        int tx = ix + is + TM.pad * 2;
        int line1 = y + h / 2 - FONT_H * TM.scale_small - TM.gap / 4;
        int line2 = y + h / 2 + TM.gap / 4;
        hal_gfx_draw_string_scaled(tx, line1, n->ssid[0] ? n->ssid : "(HIDDEN NETWORK)",
                                   n->ssid[0] ? GFX_UI_TEXT : GFX_UI_TEXT_DIM, TM.scale_small);

        char sub[40], num[12];
        int pos = 0;
        sub[0] = 0;
        pos = touch_strcat(sub, pos, sizeof(sub), "CH ");
        pos = touch_strcat(sub, pos, sizeof(sub), touch_itoa(n->channel, num, 0));
        pos = touch_strcat(sub, pos, sizeof(sub), "  ");
        pos = touch_strcat(sub, pos, sizeof(sub), touch_itoa(n->rssi, num, 0));
        pos = touch_strcat(sub, pos, sizeof(sub), " DBM  ");
        touch_strcat(sub, pos, sizeof(sub),
                     n->security == HAL_WIFI_WPA2 ? "WPA2" :
                     n->security == HAL_WIFI_PROTECTED ? "SECURED" : "OPEN");
        hal_gfx_draw_string_scaled(tx, line2, sub, GFX_UI_TEXT_DIM, TM.scale_small);

        if (n->security != HAL_WIFI_OPEN) {
            int ls = TM.touch / 2;
            icon_draw(ICON_LOCK, x + w - TM.pad * 2 - ls, y + (h - ls) / 2, ls,
                      hal_gfx_palette_rgb(GFX_UI_TEXT_DIM));
        }
    }
    hal_gfx_reset_clip();

    /* --- заголовок и карточка состояния поверх --- */
    hal_gfx_fill_rect(g_x, g_y, g_w, list_y - g_y, GFX_UI_BG);
    touch_draw_screen_header(g_x, g_y, g_w, "WI-FI");

    int cx = g_x + TM.pad, cy = g_y + header_h() + TM.pad;
    int cw = g_w - TM.pad * 2, ch = status_h();
    touch_draw_card(cx, cy, cw, ch, GFX_UI_SURFACE_2);

    int is = TM.touch;
    icon_draw(on ? ICON_WIFI : ICON_WIFI_OFF, cx + TM.pad * 2, cy + (ch - is) / 2, is,
              hal_gfx_palette_rgb(on ? GFX_UI_ACCENT : GFX_UI_TEXT_DIM));
    int tx = cx + TM.pad * 4 + is;
    hal_gfx_draw_string_scaled(tx, cy + ch / 2 - FONT_H * TM.scale_small - TM.gap / 4,
                               "WI-FI", GFX_UI_TEXT, TM.scale_small);
    hal_gfx_draw_string_scaled(tx, cy + ch / 2 + TM.gap / 4, state_text(st, g_count),
                               st == HAL_WIFI_FAILED ? GFX_UI_WARN : GFX_UI_TEXT_DIM,
                               TM.scale_small);

    if (st != HAL_WIFI_UNSUPPORTED) {
        int sx, sy, sw, sh, rx, ry, rs;
        status_rects(&sx, &sy, &sw, &sh, &rx, &ry, &rs);
        touch_draw_switch(sx, sy, sw, sh, on);
        if (st == HAL_WIFI_READY) {
            uint8_t c = g_pressed == 1 ? GFX_UI_ACCENT_DARK : GFX_UI_ACCENT;
            icon_draw(ICON_REFRESH, rx + rs / 8, ry + rs / 8, rs * 3 / 4, hal_gfx_palette_rgb(c));
        }
    }
}

static int hit_control(int px, int py) {
    int sx, sy, sw, sh, rx, ry, rs;
    status_rects(&sx, &sy, &sw, &sh, &rx, &ry, &rs);
    if (touch_hit_padded(px, py, sx, sy, sw, sh)) return 0;
    if (touch_hit_padded(px, py, rx, ry, rs, rs)) return 1;
    return -1;
}

static void wifi_on_touch(int type, int x, int y) {
    if (type == HAL_EV_POINTER_DOWN) {
        g_pressed = hit_control(x, y);
        g_drag_y0 = y;
        g_drag_scroll0 = g_scroll;
        g_drag_dist = 0;
        return;
    }
    if (type == HAL_EV_POINTER_MOVE) {
        int d = y - g_drag_y0;
        g_drag_dist = d < 0 ? -d : d;
        if (g_pressed < 0) g_scroll = g_drag_scroll0 - d;
        return;
    }
    if (type != HAL_EV_POINTER_UP) return;

    int bx, by, bw, bh;
    touch_header_back_rect(g_x, g_y, &bx, &by, &bw, &bh);
    if (g_drag_dist < TM.touch / 4 && touch_hit_padded(x, y, bx, by, bw, bh)) {
        g_pressed = -1;
        touch_ui_go_home();
        return;
    }

    int was = g_pressed;
    g_pressed = -1;
    if (was < 0 || hit_control(x, y) != was) return;

    int st = hal_wifi_state();
    if (was == 0 && (st == HAL_WIFI_OFF || st == HAL_WIFI_FAILED)) hal_wifi_enable();
    else if (was == 1 && st == HAL_WIFI_READY) hal_wifi_scan();
    g_seen_gen = 0xFFFFFFFFu;
}

static void wifi_on_key(int key) { (void)key; }
static int  wifi_wants_keyboard(void) { return 0; }

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
