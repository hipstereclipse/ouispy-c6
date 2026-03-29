/*
 * OUI-Spy C6 — Sky Spy: ASTM F3411 + DJI drone detector
 *
 * Dual-protocol detection:
 *   WiFi: NAN action frames + beacon vendor IEs (promiscuous on ch6)
 *   BLE:  Service Data UUID 0xFFFA (ASTM), Company ID 0x2CA3 (DJI)
 *
 * Parses OpenDroneID message types 0-5 for location, ID, and operator data.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sky_spy.h"
#include "app_common.h"
#include "ble_scanner.h"
#include "sniffer.h"
#include "buzzer.h"
#include "led_ctrl.h"
#include "display.h"
#include "map_tile.h"
#include "storage_ext.h"
#include <string.h>
#include <math.h>

static const char *TAG = "skyspy";

/* ── DJI OUI prefixes ── */
static const uint8_t DJI_OUI[][3] = {
    {0x60, 0x60, 0x1F}, {0x34, 0xD2, 0x62}, {0x48, 0x1C, 0xB9},
    {0xE4, 0x7A, 0x2C}, {0x58, 0xB8, 0x58}, {0x04, 0xA8, 0x5A},
    {0x8C, 0x58, 0x23}, {0x0C, 0x9A, 0xE6}, {0x88, 0x29, 0x85},
    {0x4C, 0x43, 0xF6},
};
#define DJI_OUI_COUNT (sizeof(DJI_OUI) / sizeof(DJI_OUI[0]))

/* Parrot OUI prefixes */
static const uint8_t PARROT_OUI[][3] = {
    {0x00, 0x12, 0x1C}, {0x00, 0x26, 0x7E}, {0x90, 0x03, 0xB7},
    {0xA0, 0x14, 0x3D}, {0x90, 0x3A, 0xE6},
};
#define PARROT_OUI_COUNT (sizeof(PARROT_OUI) / sizeof(PARROT_OUI[0]))

#define BLE_SERVICE_REMOTEID  0xFFFA
#define DJI_BLE_CID           0x2CA3
#define DRONE_TIMEOUT_MS      30000
#define SKY_GPS_READY_TIMEOUT_MS 20000

static inline bool sky_gps_tag_active(uint32_t now_ms)
{
    bool gps_ready_fresh = g_app.gps_client_ready
                        && (now_ms > g_app.gps_client_ready_ms)
                        && ((now_ms - g_app.gps_client_ready_ms) <= SKY_GPS_READY_TIMEOUT_MS);
    return g_app.gps_tagging_enabled && gps_ready_fresh && (g_app.wifi_clients > 0);
}

static void sky_draw_logging_badge(uint16_t bg)
{
    bool logging_active = storage_ext_logging_active();
    uint16_t fg = logging_active ? rgb565(74, 222, 128) : rgb565(239, 68, 68);
    display_draw_rect(LCD_H_RES - 14, DISPLAY_STATUS_TEXT_Y, 6, 8, bg);
    display_draw_text(LCD_H_RES - 14, DISPLAY_STATUS_TEXT_Y, "l", fg, bg);
}

/* ── OpenDroneID minimal parser ── */

typedef struct {
    uint8_t msg_type;
    uint8_t proto_ver;
} odid_header_t;

static void parse_odid_basic_id(const uint8_t *msg, drone_info_t *d)
{
    d->id_type = (msg[1] >> 4) & 0x0F;
    d->ua_type = msg[1] & 0x0F;
    memcpy(d->basic_id, &msg[2], 20);
    d->basic_id[20] = '\0';
}

static void parse_odid_location(const uint8_t *msg, drone_info_t *d)
{
    /* Latitude at bytes 4-7, Longitude at bytes 8-11 (int32 × 1e-7) */
    int32_t lat_raw = msg[4] | (msg[5] << 8) | (msg[6] << 16) | (msg[7] << 24);
    int32_t lon_raw = msg[8] | (msg[9] << 8) | (msg[10] << 16) | (msg[11] << 24);
    d->lat = lat_raw * 1e-7;
    d->lon = lon_raw * 1e-7;

    /* Altitude at bytes 12-13 (uint16, encoded as (alt+1000)/0.5) */
    uint16_t alt_raw = msg[12] | (msg[13] << 8);
    d->altitude = (alt_raw * 0.5f) - 1000.0f;

    /* Height at bytes 14-15 */
    uint16_t height_raw = msg[14] | (msg[15] << 8);
    d->height = (height_raw * 0.5f) - 1000.0f;

    /* Speed at byte 2 (units of 0.25 m/s * multiplier) */
    d->speed = msg[2] * 0.25f;
    /* Direction at byte 3 */
    d->direction = msg[3];

    d->has_location = (d->lat != 0.0 || d->lon != 0.0);
}

static void parse_odid_system(const uint8_t *msg, drone_info_t *d)
{
    /* Operator lat at bytes 1-4 */
    int32_t lat_raw = msg[1] | (msg[2] << 8) | (msg[3] << 16) | (msg[4] << 24);
    int32_t lon_raw = msg[5] | (msg[6] << 8) | (msg[7] << 16) | (msg[8] << 24);
    d->pilot_lat = lat_raw * 1e-7;
    d->pilot_lon = lon_raw * 1e-7;
    d->has_pilot = (d->pilot_lat != 0.0 || d->pilot_lon != 0.0);

    uint16_t alt_raw = msg[17] | (msg[18] << 8);
    d->pilot_alt = (alt_raw * 0.5f) - 1000.0f;
}

static void parse_odid_operator_id(const uint8_t *msg, drone_info_t *d)
{
    memcpy(d->operator_id, &msg[1], 20);
    d->operator_id[20] = '\0';
}

static void parse_odid_message(const uint8_t *msg, uint8_t len, drone_info_t *d)
{
    if (len < 25) return;
    uint8_t msg_type = (msg[0] >> 4) & 0x0F;

    switch (msg_type) {
    case 0: parse_odid_basic_id(msg, d); break;
    case 1: parse_odid_location(msg, d); break;
    case 4: parse_odid_system(msg, d); break;
    case 5: parse_odid_operator_id(msg, d); break;
    default: break;
    }
}

static void parse_odid_message_pack(const uint8_t *data, uint16_t len, drone_info_t *d)
{
    if (len < 2) return;
    uint8_t msg_count = data[1];
    if (msg_count > 10) msg_count = 10;

    uint16_t offset = 2;
    for (uint8_t i = 0; i < msg_count && offset + 25 <= len; i++) {
        parse_odid_message(&data[offset], 25, d);
        offset += 25;
    }
}

/* ── DJI DroneID parser (subcmd 0x10 = Flight Telemetry) ── */
static void parse_dji_telemetry(const uint8_t *data, uint16_t len, drone_info_t *d)
{
    if (len < 58) return;
    uint8_t subcmd = data[3];
    if (subcmd != 0x10) return;

    /* Serial number at offset 4, 20 bytes */
    memcpy(d->basic_id, &data[4], 20);
    d->basic_id[20] = '\0';
    d->id_type = 1; /* Serial */

    /* Longitude/Latitude as doubles at known offsets */
    if (len >= 44) {
        double lon, lat;
        memcpy(&lon, &data[24], 8);
        memcpy(&lat, &data[32], 8);
        if (fabs(lon) <= 180.0 && fabs(lat) <= 90.0) {
            d->lon = lon;
            d->lat = lat;
            d->has_location = true;
        }
    }
}

/* ── Find or create drone by MAC ── */
static drone_info_t *get_drone(const uint8_t *mac, int8_t rssi, uint8_t protocol)
{
    if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return NULL;

    uint32_t now = uptime_ms();
    drone_info_t *d = NULL;

    for (int i = 0; i < g_app.drone_count; i++) {
        if (mac_equal(g_app.drones[i].mac, mac)) {
            d = &g_app.drones[i];
            d->rssi = rssi;
            d->last_seen = now;
            xSemaphoreGive(g_app.drone_mutex);
            return d;
        }
    }

    /* New drone */
    if (g_app.drone_count < g_app.max_drones_allowed) {
        d = &g_app.drones[g_app.drone_count];
        memset(d, 0, sizeof(*d));
        memcpy(d->mac, mac, 6);
        d->rssi = rssi;
        d->last_seen = now;
        d->protocol = protocol;
        g_app.drone_count++;

        ESP_LOGI(TAG, "NEW drone #%d [%02X:%02X:%02X:%02X:%02X:%02X] proto=%d",
                 g_app.drone_count, mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5], protocol);

        buzzer_tone(1800, 100);
        led_ctrl_set(255, 140, 0);
    }

    xSemaphoreGive(g_app.drone_mutex);
    return d;
}

/* ── WiFi sniffer callback ── */
static void sky_wifi_cb(const uint8_t *src_mac, int8_t rssi,
                        const uint8_t *payload, uint16_t len, uint8_t protocol)
{
    drone_info_t *d = get_drone(src_mac, rssi, protocol);
    if (!d) return;

    if (protocol == PROTO_ASTM_WIFI) {
        parse_odid_message_pack(payload, len, d);
    } else if (protocol == PROTO_DJI) {
        parse_dji_telemetry(payload, len, d);
    }
}

/* ── BLE scan callback ── */
static void sky_ble_cb(const uint8_t *addr, int8_t rssi,
                       const uint8_t *adv_data, uint8_t adv_len,
                       const uint8_t *name, uint8_t name_len)
{
    /* Scan advertisement data for Service Data UUID 0xFFFA or DJI CID */
    uint8_t pos = 0;
    while (pos + 1 < adv_len) {
        uint8_t field_len = adv_data[pos];
        if (field_len == 0 || pos + 1 + field_len > adv_len) break;
        uint8_t field_type = adv_data[pos + 1];

        /* Service Data - 16-bit UUID (type 0x16) */
        if (field_type == 0x16 && field_len >= 5) {
            uint16_t uuid = adv_data[pos + 2] | (adv_data[pos + 3] << 8);
            if (uuid == BLE_SERVICE_REMOTEID) {
                /* Application code check */
                if (adv_data[pos + 4] == 0x0D) {
                    drone_info_t *d = get_drone(addr, rssi, PROTO_ASTM_BLE);
                    if (d) {
                        /* Single message (BLE4) or message pack (BLE5) */
                        uint16_t payload_len = field_len - 3;
                        const uint8_t *payload = &adv_data[pos + 5];
                        if (payload_len >= 25) {
                            if (payload_len >= 27) {
                                parse_odid_message_pack(payload, payload_len, d);
                            } else {
                                parse_odid_message(payload, payload_len, d);
                            }
                        }
                    }
                }
            }
        }

        /* Manufacturer Specific (type 0xFF) */
        if (field_type == 0xFF && field_len >= 3) {
            uint16_t cid = adv_data[pos + 2] | (adv_data[pos + 3] << 8);
            if (cid == DJI_BLE_CID) {
                drone_info_t *d = get_drone(addr, rssi, PROTO_DJI);
                (void)d; /* DJI BLE data is minimal */
            }
        }

        pos += 1 + field_len;
    }

    /* Also check MAC OUI for known drone manufacturers */
    for (int i = 0; i < (int)DJI_OUI_COUNT; i++) {
        if (memcmp(addr, DJI_OUI[i], 3) == 0) {
            get_drone(addr, rssi, PROTO_DJI);
            return;
        }
    }
    for (int i = 0; i < (int)PARROT_OUI_COUNT; i++) {
        if (memcmp(addr, PARROT_OUI[i], 3) == 0) {
            get_drone(addr, rssi, PROTO_ASTM_WIFI);
            return;
        }
    }
}

/* ── Expiry: remove drones not seen for DRONE_TIMEOUT_MS ── */
static void expire_drones(void)
{
    if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    uint32_t now = uptime_ms();
    for (int i = g_app.drone_count - 1; i >= 0; i--) {
        if ((now - g_app.drones[i].last_seen) > DRONE_TIMEOUT_MS) {
            /* Remove by shifting */
            if (i < g_app.drone_count - 1) {
                memmove(&g_app.drones[i], &g_app.drones[i + 1],
                        (g_app.drone_count - 1 - i) * sizeof(drone_info_t));
            }
            g_app.drone_count--;
        }
    }
    xSemaphoreGive(g_app.drone_mutex);
}

/* ── Display task -- Naval CIC radar aesthetic ── */
static void sky_display_task(void *arg)
{
    int frame = 0;
    int last_drone_count = -1;
    int last_cursor = -1;
    int last_wifi_clients = -1;
    int last_gps_active = -1;
    int last_logging_active = -1;
    int last_logging_blocked = -1;
    int last_map_open = -1;
    bool was_map_open = false;
    bool full_drawn = false;  /* force first full draw */
    uint32_t last_refresh_token = 0;

    while (g_app.current_mode == MODE_SKY_SPY) {
        frame++;
        expire_drones();
        uint32_t now_ms = uptime_ms();
        bool gps_tag_active = sky_gps_tag_active(now_ms);
        bool logging_active = storage_ext_logging_active();
        bool logging_blocked = storage_ext_logging_blocked();

        /* Dirty check: data sections only redraw when content changed */
        bool data_dirty = !full_drawn
                        || (g_app.drone_count != last_drone_count)
                        || (g_app.ui_cursor != last_cursor)
                        || (g_app.wifi_clients != last_wifi_clients)
                        || ((int)gps_tag_active != last_gps_active)
                        || ((int)logging_active != last_logging_active)
                        || ((int)logging_blocked != last_logging_blocked)
                        || ((int)g_app.local_map_open != last_map_open)
                        || (g_app.ui_refresh_token != last_refresh_token);

        /* Naval CIC palette — near-black with phosphor green */
        uint16_t bg        = rgb565(2, 4, 2);
        uint16_t phosphor  = rgb565(20, 255, 0);
        uint16_t phos_mid  = rgb565(10, 148, 0);
        uint16_t phos_dim  = rgb565(5, 72, 0);
        uint16_t phos_faint= rgb565(2, 36, 0);
        uint16_t grid_col  = rgb565(0, 24, 0);
        uint16_t amber     = rgb565(255, 176, 0);
        uint16_t amber_dim = rgb565(120, 82, 0);
        uint16_t red_alert = rgb565(255, 48, 32);
        uint16_t text_main = rgb565(15, 220, 0);
        uint16_t text_dim  = rgb565(8, 110, 0);
        uint16_t panel_bg  = rgb565(2, 10, 0);
        uint16_t border_col= rgb565(0, 48, 0);
        uint16_t footer_bg = rgb565(2, 4, 2);
        const char *ap_ssid = NULL;
        const char *ap_pass = NULL;
        app_mode_ap_credentials(MODE_SKY_SPY, &ap_ssid, &ap_pass, NULL);

        /* Static sections: status bar, content bg, footer — only on data change */
        last_map_open = (int)g_app.local_map_open;
        last_refresh_token = g_app.ui_refresh_token;

        /* When local map is open, render the shared map view instead */
        if (g_app.local_map_open) {
            if (data_dirty) {
                last_drone_count = g_app.drone_count;
                last_cursor = g_app.ui_cursor;
                last_wifi_clients = g_app.wifi_clients;
                last_gps_active = (int)gps_tag_active;
                last_logging_active = (int)logging_active;
                last_logging_blocked = (int)logging_blocked;
            }
            display_draw_shared_map_view(MODE_SKY_SPY);
            was_map_open = true;
            full_drawn = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        /* Full clear when returning from map view to flush leftover tile pixels */
        if (was_map_open) {
            display_fill(rgb565(2, 4, 2));
            was_map_open = false;
        }

        /* Static header: only redraw on data change */
        if (data_dirty) {
            last_drone_count = g_app.drone_count;
            last_cursor = g_app.ui_cursor;
            last_wifi_clients = g_app.wifi_clients;
            last_gps_active = (int)gps_tag_active;
            last_logging_active = (int)logging_active;
            last_logging_blocked = (int)logging_blocked;
            full_drawn = true;

            /* Status bar — dark navy with green type */
            display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 26, rgb565(0, 12, 0));
            display_draw_rect(0, DISPLAY_STATUS_DIV_Y, LCD_H_RES, 2, phos_mid);
            display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, "SKY SPY", phosphor, rgb565(0, 12, 0));
            display_draw_text_centered(DISPLAY_STATUS_SUB_Y, ap_ssid, phos_mid, rgb565(0, 12, 0));
            sky_draw_logging_badge(rgb565(0, 12, 0));
        }

        /* Radar area bg — always clear for sweep animation */
        display_draw_rect(0, DISPLAY_CONTENT_TOP, LCD_H_RES, 196 - DISPLAY_CONTENT_TOP, bg);

        char buf[64];

        /* ── Radar scope — always visible ── */
        int cx = LCD_H_RES / 2;   /* 86 */
        int cy = 120;
        int r_outer = 62;

        /* Range rings (concentric circles drawn as pixel arcs) */
        for (int ring = 1; ring <= 3; ring++) {
            int rr = ring * r_outer / 3;
            uint16_t ring_col = (ring == 3) ? phos_dim : grid_col;
            /* Draw ring as short horizontal segments around circumference */
            for (int a = 0; a < 32; a++) {
                float angle = (float)a * 6.2831853f / 32.0f;
                int px = cx + (int)(rr * sinf(angle));
                int py = cy - (int)(rr * cosf(angle));
                if (px >= 1 && px < LCD_H_RES - 1 && py >= 38 && py < DISPLAY_FOOTER_BAR_Y - 2)
                    display_draw_rect(px, py, 2, 2, ring_col);
            }
        }

        /* Crosshair */
        display_draw_rect(cx - r_outer, cy, r_outer * 2 + 1, 1, phos_faint);
        display_draw_rect(cx, cy - r_outer, 1, r_outer * 2 + 1, phos_faint);

        /* Rotating sweep line (8 segments along the line) */
        float sweep_angle = (float)(frame % 60) * 6.2831853f / 60.0f;
        for (int s = 8; s <= r_outer; s += 3) {
            int sx = cx + (int)(s * sinf(sweep_angle));
            int sy = cy - (int)(s * cosf(sweep_angle));
            if (sx >= 0 && sx < LCD_H_RES && sy >= 38 && sy < DISPLAY_FOOTER_BAR_Y - 2) {
                uint16_t sc = (s > r_outer * 2 / 3) ? phos_dim : phos_mid;
                display_draw_rect(sx, sy, 2, 2, sc);
            }
        }
        /* Bright tip of sweep */
        {
            int tx = cx + (int)(r_outer * sinf(sweep_angle));
            int ty = cy - (int)(r_outer * cosf(sweep_angle));
            if (tx >= 1 && tx < LCD_H_RES - 2 && ty >= 38 && ty < DISPLAY_FOOTER_BAR_Y - 4)
                display_draw_rect(tx - 1, ty - 1, 3, 3, phosphor);
        }

        /* Fading trail (two previous positions) */
        for (int t = 1; t <= 2; t++) {
            float ta = (float)((frame - t * 3 + 60) % 60) * 6.2831853f / 60.0f;
            uint16_t trail_col = (t == 1) ? phos_faint : grid_col;
            for (int s = 12; s <= r_outer; s += 6) {
                int sx = cx + (int)(s * sinf(ta));
                int sy = cy - (int)(s * cosf(ta));
                if (sx >= 0 && sx < LCD_H_RES && sy >= 38 && sy < DISPLAY_FOOTER_BAR_Y - 2)
                    display_draw_rect(sx, sy, 2, 2, trail_col);
            }
        }

        /* Cardinal labels */
        display_draw_text(cx - 2, cy - r_outer - 10, "N", phos_dim, bg);
        display_draw_text(cx - 2, cy + r_outer + 3,  "S", phos_dim, bg);

        /* Drone blips on radar — always drawn (radar area clears each frame) */
        for (int i = 0; i < g_app.drone_count; i++) {
            drone_info_t *d = &g_app.drones[i];
            uint8_t hash = d->mac[4] ^ d->mac[5] ^ d->mac[3];
            float blip_angle = (float)hash * 6.2831853f / 256.0f;
            int dist = r_outer - ((d->rssi + 100) * r_outer / 70);
            if (dist < 6) dist = 6;
            if (dist > r_outer) dist = r_outer;
            int bx = cx + (int)(dist * sinf(blip_angle));
            int by = cy - (int)(dist * cosf(blip_angle));
            if (bx >= 2 && bx < LCD_H_RES - 4 && by >= 40 && by < 196) {
                uint16_t blip_col = ((frame + i) % 3) ? amber : amber_dim;
                display_draw_rect(bx - 2, by - 2, 5, 5, blip_col);
                display_draw_rect(bx - 1, by - 1, 3, 3, amber);
            }
        }

        /* ── Data sections + footer — only redraw on data change ── */
        if (data_dirty) {
            /* Clear data area below radar */
            display_draw_rect(0, 196, LCD_H_RES, DISPLAY_FOOTER_BAR_Y - 196, bg);

            if (g_app.drone_count == 0) {
                display_draw_text_centered(196, "NO CONTACTS", text_main, bg);
                display_draw_text_centered(208, "SCANNING ...", text_dim, bg);

                display_draw_bordered_rect(4, 224, 78, 16, border_col, panel_bg);
                display_draw_text(8, 228, "WiFi RDY", phos_mid, panel_bg);
                display_draw_bordered_rect(88, 224, 78, 16, border_col, panel_bg);
                display_draw_text(92, 228, "BLE  RDY", phos_mid, panel_bg);

                display_draw_text(4, 248, "THREAT LEVEL:", text_dim, bg);
                display_draw_text(86, 248, "CLEAR", phos_mid, bg);
            } else {
                snprintf(buf, sizeof(buf), "CONTACTS: %d", g_app.drone_count);
                display_draw_text(4, 196, buf, amber, bg);

                display_draw_text(4, 208, "THREAT LEVEL:", text_dim, bg);
                if (g_app.drone_count >= 3) {
                    display_draw_text(86, 208, "HIGH", red_alert, bg);
                } else {
                    display_draw_text(86, 208, "ACTIVE", amber, bg);
                }

                g_app.ui_item_count = g_app.drone_count;
                if (g_app.ui_cursor >= g_app.drone_count)
                    g_app.ui_cursor = g_app.drone_count > 0 ? g_app.drone_count - 1 : 0;

                int max_show = 3;
                int start = 0;
                if (g_app.ui_cursor >= max_show) start = g_app.ui_cursor - max_show + 1;

                for (int idx = start, row = 0; idx < g_app.drone_count && row < max_show; idx++, row++) {
                    drone_info_t *d = &g_app.drones[idx];
                    char mac_str[18];
                    mac_to_str(d->mac, mac_str, sizeof(mac_str));

                    int y_base = 222 + row * 22;
                    bool selected = (idx == g_app.ui_cursor);

                    const char *proto_label;
                    uint16_t proto_col;
                    if (d->protocol == PROTO_DJI) {
                        proto_col = amber; proto_label = "DJI";
                    } else if (d->protocol == PROTO_ASTM_BLE) {
                        proto_col = phos_mid; proto_label = "BLE";
                    } else {
                        proto_col = phos_dim; proto_label = "WFI";
                    }

                    uint16_t row_bg = selected ? rgb565(8, 20, 8) : bg;
                    uint16_t row_border = selected ? phos_mid : border_col;
                    display_draw_rect(4, y_base, LCD_H_RES - 8, 18, row_bg);
                    display_draw_rect(4, y_base, LCD_H_RES - 8, 1, row_border);
                    display_draw_rect(4, y_base, 3, 18, proto_col);

                    snprintf(buf, sizeof(buf), "%s %ddB", proto_label, d->rssi);
                    display_draw_text(10, y_base + 2, buf, proto_col, row_bg);

                    const char *id_str = d->basic_id[0] ? d->basic_id : mac_str;
                    char id_short[20];
                    strncpy(id_short, id_str, 16);
                    id_short[16] = '\0';
                    display_draw_text(10, y_base + 10, id_short, selected ? phosphor : text_dim, row_bg);
                }

                g_app.ui_item_count = g_app.drone_count;
            }

            /* Footer — WiFi info */
            display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer_bg);
            display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, 1, border_col);
            display_draw_rect(0, DISPLAY_FOOTER_BAR_Y - 12, LCD_H_RES, 10, bg);
            display_draw_text_centered(DISPLAY_FOOTER_BAR_Y - 10,
                                       gps_tag_active ? "GPS TAG: ON" : "GPS TAG: OFF",
                                       gps_tag_active ? rgb565(74, 222, 128) : rgb565(248, 113, 113),
                                       bg);
            snprintf(buf, sizeof(buf), "%s  %dCli  %lukB",
                     ap_pass,
                     g_app.wifi_clients, (unsigned long)(g_app.free_heap / 1024));
            display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, buf, text_dim, footer_bg);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelete(NULL);
}

void sky_spy_start(void)
{
    ESP_LOGI(TAG, "Starting Sky Spy mode");
    led_ctrl_breathe_stop();
    buzzer_melody_sky();

    g_app.drone_count = 0;

    /* Breathing green LED for Sky Spy scan state */
    led_ctrl_breathe(8, 140, 30, 3000);

    /* Start WiFi sniffer (promiscuous on current AP channel) */
    sniffer_start(sky_wifi_cb);

    /* Start BLE scan with 50% duty for wifi coexistence */
    ble_scanner_start(sky_ble_cb, 100, 50, true);

    if (xTaskCreate(sky_display_task, "sky_lcd", TASK_STACK_UI, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Sky Spy display task");
    }
}

void sky_spy_stop(void)
{
    sniffer_stop();
    ble_scanner_stop();
    led_ctrl_breathe_stop();
    led_ctrl_off();
    buzzer_off();
    ESP_LOGI(TAG, "Sky Spy stopped");
}

int sky_spy_drone_count(void)
{
    return g_app.drone_count;
}
