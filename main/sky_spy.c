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
    if (g_app.drone_count < MAX_DRONES) {
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
        led_ctrl_set(0, 150, 255);
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

/* ── Display task — Navy aerospace aesthetic ── */
static void sky_display_task(void *arg)
{
    int frame = 0;
    while (g_app.current_mode == MODE_SKY_SPY) {
        frame++;
        expire_drones();

        /* Mist, slate, and natural blue palette */
        uint16_t bg = rgb565(226, 233, 235);
        uint16_t accent = rgb565(76, 103, 130);
        uint16_t dim_blue = rgb565(160, 179, 188);
        uint16_t cyan = rgb565(79, 132, 145);
        uint16_t panel_bg = rgb565(243, 247, 247);
        uint16_t panel_alt = rgb565(231, 238, 240);
        uint16_t text_main = rgb565(39, 55, 67);
        uint16_t text_dim = rgb565(92, 110, 122);
        uint16_t footer = rgb565(197, 208, 212);

        /* Status bar: electric blue */
        display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 26, accent);
        display_draw_rect(0, DISPLAY_STATUS_DIV_Y, LCD_H_RES, 2, rgb565(105, 130, 153));
        display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, "SKY SPY", 0xFFFF, accent);
        display_draw_text_centered(DISPLAY_STATUS_SUB_Y, "192.168.4.1", rgb565(214, 227, 232), accent);

        /* Content area */
        display_draw_rect(0, DISPLAY_CONTENT_TOP, LCD_H_RES, DISPLAY_FOOTER_BAR_Y - DISPLAY_CONTENT_TOP, bg);

        char buf[64];
        if (g_app.drone_count == 0) {
            /* No drones: animated radar display */

            /* Radar grid lines — horizontal */
            for (int gy = 60; gy < 200; gy += 35) {
                display_draw_hline(20, gy, LCD_H_RES - 40, dim_blue);
            }
            /* Vertical grid lines */
            for (int gx = 30; gx < LCD_H_RES - 20; gx += 28) {
                display_draw_rect(gx, 50, 1, 150, dim_blue);
            }

            /* Radar circle area */
            int cx = LCD_H_RES / 2;
            int cy = 120;
            /* Crosshair through center */
            display_draw_rect(cx - 30, cy, 60, 1, dim_blue);
            display_draw_rect(cx, cy - 30, 1, 60, dim_blue);

            /* Animated sweep dot */
            static const int8_t sweep_x[] = {0,10,18,24,26,24,18,10,0,-10,-18,-24,-26,-24,-18,-10};
            static const int8_t sweep_y[] = {-26,-24,-18,-10,0,10,18,24,26,24,18,10,0,-10,-18,-24};
            int si = frame % 16;
            display_draw_rect(cx + sweep_x[si] - 2, cy + sweep_y[si] - 2, 5, 5, cyan);
            /* Trail dots */
            int si2 = (frame + 14) % 16;
            display_draw_rect(cx + sweep_x[si2] - 1, cy + sweep_y[si2] - 1, 3, 3, rgb565(145, 170, 180));

            /* Scanning text */
            display_draw_text(30, 170, "SCANNING AIRSPACE", text_main, bg);

            const char *scan_anim[] = {"WiFi ch6 + BLE .", "WiFi ch6 + BLE ..", "WiFi ch6 + BLE ...", "WiFi ch6 + BLE ...."};
            display_draw_text(12, 185, scan_anim[frame % 4], text_dim, bg);

            /* Protocol status indicators */
            display_draw_bordered_rect(4, 210, 78, 16, dim_blue, panel_bg);
            display_draw_text(8, 214, "WiFi: ON", text_main, panel_bg);
            display_draw_bordered_rect(88, 210, 78, 16, dim_blue, panel_bg);
            display_draw_text(92, 214, "BLE: ON", text_main, panel_bg);

            /* Gentle blue LED pulse while scanning */
            if (frame % 6 < 3) {
                led_ctrl_set(0, 20, 60);
            } else {
                led_ctrl_off();
            }
        } else {
            /* Drones detected */
            snprintf(buf, sizeof(buf), "DRONES: %d", g_app.drone_count);
            display_draw_bordered_rect(4, 38, LCD_H_RES - 8, 20, accent, panel_bg);
            display_draw_text(8, 44, buf, text_main, panel_bg);

            /* Blue divider */
            display_draw_hline(4, 62, LCD_H_RES - 8, accent);

            /* Keep UI cursor in range for drone list */
            g_app.ui_item_count = g_app.drone_count;
            if (g_app.ui_cursor >= g_app.drone_count)
                g_app.ui_cursor = g_app.drone_count > 0 ? g_app.drone_count - 1 : 0;

            /* Drone cards with protocol color bands + cursor */
            for (int i = 0; i < g_app.drone_count && i < 4; i++) {
                drone_info_t *d = &g_app.drones[i];
                char mac_str[18];
                mac_to_str(d->mac, mac_str, sizeof(mac_str));

                int y_base = 68 + i * 52;
                bool selected = (i == g_app.ui_cursor && g_app.drone_count > 0);
                uint16_t card_bg = selected ? rgb565(214, 225, 229) : panel_alt;

                /* Protocol color band on left */
                uint16_t proto_color;
                const char *proto_label;
                if (d->protocol == PROTO_DJI) {
                    proto_color = rgb565(160, 120, 60);
                    proto_label = "DJI";
                } else if (d->protocol == PROTO_ASTM_BLE) {
                    proto_color = cyan;
                    proto_label = "BLE";
                } else {
                    proto_color = rgb565(92, 124, 150);
                    proto_label = "WiFi";
                }

                /* Card background */
                display_draw_rect(4, y_base, LCD_H_RES - 8, 46, card_bg);
                /* Protocol accent bar — pulses on selection */
                uint16_t bar_col = (selected && (frame % 2)) ? rgb565(247, 249, 249) : proto_color;
                display_draw_rect(4, y_base, 4, 46, bar_col);
                /* Top border */
                uint16_t border = selected ? accent : dim_blue;
                display_draw_rect(4, y_base, LCD_H_RES - 8, 1, border);

                /* Protocol tag + RSSI */
                snprintf(buf, sizeof(buf), "%s  %ddBm", proto_label, d->rssi);
                display_draw_text(12, y_base + 4, buf, proto_color, card_bg);

                /* ID or MAC */
                if (d->basic_id[0]) {
                    display_draw_text(12, y_base + 16, d->basic_id, text_main, card_bg);
                } else {
                    display_draw_text(12, y_base + 16, mac_str, text_main, card_bg);
                }

                /* Location / speed */
                if (d->has_location) {
                    snprintf(buf, sizeof(buf), "%.4f, %.4f", d->lat, d->lon);
                    display_draw_text(12, y_base + 28, buf, rgb565(71, 119, 83), card_bg);
                }
                if (d->speed > 0.1f) {
                    snprintf(buf, sizeof(buf), "%.0fm/s %.0fm", d->speed, d->altitude);
                    display_draw_text(12, y_base + 36, buf, text_dim, card_bg);
                }
            }

            /* Bright blue LED pulse when drones active */
            led_ctrl_pulse(0, 100, 255, 500);
        }

        /* Bottom bar */
        display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer);
        snprintf(buf, sizeof(buf), "Heap:%lukB  Drones:%d",
                 (unsigned long)(g_app.free_heap / 1024), g_app.drone_count);
        display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, buf, text_main, footer);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

void sky_spy_start(void)
{
    ESP_LOGI(TAG, "Starting Sky Spy mode");
    buzzer_melody_sky();

    g_app.drone_count = 0;

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
    led_ctrl_off();
    buzzer_off();
    ESP_LOGI(TAG, "Sky Spy stopped");
}

int sky_spy_drone_count(void)
{
    return g_app.drone_count;
}
