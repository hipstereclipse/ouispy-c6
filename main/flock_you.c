/*
 * OUI-Spy C6 — Flock You: passive BLE surveillance hardware detector
 *
 * Detection heuristics:
 *   1. MAC OUI prefix matching (20 Flock Safety prefixes)
 *   2. Device name substring matching (FS Ext Battery, Penguin, Flock, Pigvision)
 *   3. Manufacturer Company ID 0x09C8 (XUNTONG)
 *   4. Raven GATT service UUID fingerprinting
 *   5. Raven firmware version estimation
 *
 * SPDX-License-Identifier: MIT
 */
#include "flock_you.h"
#include "app_common.h"
#include "ble_scanner.h"
#include "buzzer.h"
#include "led_ctrl.h"
#include "display.h"
#include "storage_ext.h"
#include <string.h>
#include <ctype.h>
#include <math.h>

static const char *TAG = "flock";

#define FLOCK_LED_WARNING_FLASH_MS   120
#define FLOCK_LED_WARNING_FLASHES     3
#define FLOCK_LED_STALE_MS          4000
#define FLOCK_GPS_READY_TIMEOUT_MS  20000

static inline bool flock_gps_tag_active(uint32_t now_ms)
{
    bool gps_ready_fresh = g_app.gps_client_ready
                        && (now_ms > g_app.gps_client_ready_ms)
                        && ((now_ms - g_app.gps_client_ready_ms) <= FLOCK_GPS_READY_TIMEOUT_MS);
    return g_app.gps_tagging_enabled && gps_ready_fresh && (g_app.wifi_clients > 0);
}

static void flock_draw_logging_badge(uint16_t bg)
{
    bool logging_active = storage_ext_logging_active();
    uint16_t fg = logging_active ? rgb565(74, 222, 128) : rgb565(239, 68, 68);
    display_draw_rect(LCD_H_RES - 14, DISPLAY_STATUS_TEXT_Y, 6, 8, bg);
    display_draw_text(LCD_H_RES - 14, DISPLAY_STATUS_TEXT_Y, "l", fg, bg);
}

static TaskHandle_t s_led_task = NULL;
static volatile uint32_t s_warning_started_ms = 0;

/* ── Flock Safety OUI Prefixes ── */
static const uint8_t FLOCK_OUI[][3] = {
    {0xEC, 0x1B, 0xBD}, {0x58, 0x8E, 0x81}, {0x90, 0x35, 0xEA},
    {0xB4, 0xE3, 0xF9}, {0x04, 0x0D, 0x84}, {0xF0, 0x82, 0xC0},
    {0xD8, 0xF3, 0xBC}, {0x74, 0x4C, 0xA1}, {0x14, 0x5A, 0xFC},
    {0xE4, 0xAA, 0xEA}, {0x3C, 0x91, 0x80}, {0x80, 0x30, 0x49},
    {0x08, 0x3A, 0x88}, {0xB4, 0x1E, 0x52}, {0xD0, 0x39, 0x57},
    {0x24, 0xB2, 0xB9}, {0x00, 0xF4, 0x8D}, {0xE0, 0x0A, 0xF6},
};
#define FLOCK_OUI_COUNT (sizeof(FLOCK_OUI) / sizeof(FLOCK_OUI[0]))

/* Raven GATT service UUIDs (16-bit) */
#define RAVEN_UUID_DEVINFO  0x180A
#define RAVEN_UUID_GPS      0x3100
#define RAVEN_UUID_POWER    0x3200
#define RAVEN_UUID_NETWORK  0x3300
#define RAVEN_UUID_UPLOAD   0x3400
#define RAVEN_UUID_ERROR    0x3500

/* XUNTONG company ID used by Flock devices broadcasting no name */
#define CID_XUNTONG 0x09C8

/* ── Name patterns (case-insensitive substring) ── */
static const char *FLOCK_NAMES[] = {
    "fs ext battery", "penguin", "flock", "pigvision",
};
#define FLOCK_NAME_COUNT 4

static int8_t flock_clamp_rssi(int8_t rssi)
{
    if (rssi < -95) return -95;
    if (rssi > -35) return -35;
    return rssi;
}

static uint8_t flock_signal_level(int8_t rssi)
{
    int level = ((int)flock_clamp_rssi(rssi) + 95) * 255 / 60;
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    return (uint8_t)level;
}

static int flock_heartbeat_period_ms(int8_t rssi)
{
    int strength = flock_signal_level(rssi);
    return 1450 - (strength * 800 / 255);
}

static int8_t flock_strongest_recent_rssi(void)
{
    int8_t strongest = -127;
    uint32_t now = uptime_ms();

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return strongest;
    }

    for (int i = 0; i < g_app.device_count; i++) {
        ble_device_t *device = &g_app.devices[i];
        if (!device->is_flock) continue;
        if ((now - device->last_seen) > FLOCK_LED_STALE_MS) continue;
        if (device->rssi > strongest) strongest = device->rssi;
    }

    xSemaphoreGive(g_app.device_mutex);
    return strongest;
}

static void flock_led_apply_warning(uint32_t now)
{
    uint32_t elapsed = now - s_warning_started_ms;
    uint32_t phase = elapsed / FLOCK_LED_WARNING_FLASH_MS;
    uint32_t total_phases = FLOCK_LED_WARNING_FLASHES * 2;

    if (phase >= total_phases) {
        s_warning_started_ms = 0;
        return;
    }

    if ((phase % 2U) == 0U) {
        led_ctrl_set(255, 96, 0);
    } else {
        led_ctrl_off();
    }
}

static void flock_led_apply_heartbeat(int8_t rssi, uint32_t now)
{
    if (rssi <= -120) {
        /* No active signal — breathing indicates scan state */
        uint32_t period = 2800;
        uint32_t phase = now % period;
        /* Cosine curve 0→1→0 for smooth breathing */
        float t = (float)phase / (float)period;
        float breath = (1.0f - cosf(t * 2.0f * 3.14159265f)) * 0.5f;
        breath = breath * breath;                 /* gamma */
        float floor = 0.06f;
        breath = floor + breath * (1.0f - floor);

        if (g_app.device_count == 0) {
            /* Nothing detected yet: visible green pulse */
            led_ctrl_set((uint8_t)(8  * breath),
                         (uint8_t)(140 * breath),
                         (uint8_t)(30  * breath));
        } else {
            /* Something was detected previously: purple breathing */
            led_ctrl_set((uint8_t)(80 * breath),
                         (uint8_t)(15 * breath),
                         (uint8_t)(100 * breath));
        }
        return;
    }

    uint8_t strength = flock_signal_level(rssi);
    uint8_t peak_red = (uint8_t)(48 + (strength * 207 / 255));
    uint8_t ember_green = (uint8_t)(strength / 18);
    int period_ms = flock_heartbeat_period_ms(rssi);
    uint32_t beat = now % (uint32_t)period_ms;

    if (beat < 90) {
        led_ctrl_set(peak_red, ember_green, 0);
    } else if (beat < 160) {
        led_ctrl_set((uint8_t)(peak_red / 5), 0, 0);
    } else if (beat < 240) {
        led_ctrl_set((uint8_t)(peak_red * 4 / 5), 0, 0);
    } else if (beat < 360) {
        led_ctrl_set((uint8_t)(peak_red / 6), 0, 0);
    } else {
        led_ctrl_off();
    }
}

static void flock_led_task(void *arg)
{
    while (g_app.current_mode == MODE_FLOCK_YOU) {
        uint32_t now = uptime_ms();

        if (s_warning_started_ms != 0) {
            flock_led_apply_warning(now);
        } else {
            flock_led_apply_heartbeat(flock_strongest_recent_rssi(), now);
        }

        vTaskDelay(pdMS_TO_TICKS(60));
    }

    led_ctrl_off();
    s_led_task = NULL;
    vTaskDelete(NULL);
}

/* ── Helpers ── */
static bool check_oui(const uint8_t mac[6])
{
    for (int i = 0; i < (int)FLOCK_OUI_COUNT; i++) {
        if (memcmp(mac, FLOCK_OUI[i], 3) == 0) return true;
    }
    return false;
}

static bool check_name(const char *name)
{
    if (!name || name[0] == '\0') return false;
    /* Lowercase copy for case-insensitive comparison */
    char lower[DEVICE_NAME_LEN];
    for (int i = 0; i < DEVICE_NAME_LEN - 1 && name[i]; i++) {
        lower[i] = tolower((unsigned char)name[i]);
        lower[i + 1] = '\0';
    }
    for (int i = 0; i < FLOCK_NAME_COUNT; i++) {
        if (strstr(lower, FLOCK_NAMES[i])) return true;
    }
    return false;
}

/* Extract 16-bit little-endian company ID from manufacturer-specific data */
static uint16_t extract_company_id(const uint8_t *adv, uint8_t len)
{
    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t field_len = adv[pos];
        if (field_len == 0 || pos + 1 + field_len > len) break;
        uint8_t field_type = adv[pos + 1];
        if (field_type == 0xFF && field_len >= 3) { /* Manufacturer Specific */
            return adv[pos + 2] | (adv[pos + 3] << 8);
        }
        pos += 1 + field_len;
    }
    return 0;
}

/* Check for Raven service UUIDs in advertisement */
static uint8_t check_raven_uuids(const uint8_t *adv, uint8_t len)
{
    bool has_gps = false, has_power = false, has_network = false;
    bool has_upload = false, has_error = false;

    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t field_len = adv[pos];
        if (field_len == 0 || pos + 1 + field_len > len) break;
        uint8_t field_type = adv[pos + 1];

        /* Complete/Incomplete List of 16-bit UUIDs */
        if ((field_type == 0x02 || field_type == 0x03) && field_len >= 3) {
            for (int i = 2; i + 1 < 1 + field_len; i += 2) {
                uint16_t uuid = adv[pos + i] | (adv[pos + i + 1] << 8);
                if (uuid == RAVEN_UUID_GPS)     has_gps = true;
                if (uuid == RAVEN_UUID_POWER)   has_power = true;
                if (uuid == RAVEN_UUID_NETWORK) has_network = true;
                if (uuid == RAVEN_UUID_UPLOAD)  has_upload = true;
                if (uuid == RAVEN_UUID_ERROR)   has_error = true;
            }
        }
        pos += 1 + field_len;
    }

    if (!has_gps) return 0; /* Not a Raven */

    /* Firmware estimation based on UUID combination */
    if (has_error && has_upload && has_network) return 3; /* 1.3.x */
    if (has_upload && has_network)               return 2; /* 1.2.x */
    if (has_gps && has_power)                    return 1; /* 1.1.x */
    return 1; /* Default to 1.1.x if GPS present */
}

/* ── Scan callback ── */
static void flock_scan_cb(const uint8_t *addr, int8_t rssi,
                          const uint8_t *adv_data, uint8_t adv_len,
                          const uint8_t *name, uint8_t name_len)
{
    uint8_t detect = 0;
    bool is_raven = false;
    uint8_t raven_fw = 0;
    uint16_t cid = 0;

    /* Heuristic 1: OUI match */
    if (check_oui(addr)) detect |= DETECT_OUI;

    /* Heuristic 2: Name match */
    char name_str[DEVICE_NAME_LEN] = {0};
    if (name_len > 0) {
        int copy_len = (name_len < DEVICE_NAME_LEN - 1) ? name_len : DEVICE_NAME_LEN - 1;
        memcpy(name_str, name, copy_len);
    }
    if (check_name(name_str)) detect |= DETECT_NAME;

    /* Heuristic 3: Company ID */
    cid = extract_company_id(adv_data, adv_len);
    if (cid == CID_XUNTONG) detect |= DETECT_CID;

    /* Heuristic 4 & 5: Raven UUID fingerprint */
    raven_fw = check_raven_uuids(adv_data, adv_len);
    if (raven_fw > 0) {
        detect |= DETECT_UUID | DETECT_RAVEN;
        is_raven = true;
    }

    /* Skip devices that don't match any heuristic */
    if (detect == 0) return;

    /* ── Deduplicate by MAC, update or insert ── */
    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    uint32_t now = uptime_ms();
    int idx = -1;
    for (int i = 0; i < g_app.device_count; i++) {
        if (mac_equal(g_app.devices[i].mac, addr)) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        /* Update existing */
        ble_device_t *d = &g_app.devices[idx];
        d->rssi      = rssi;
        if (rssi > d->rssi_best) d->rssi_best = rssi;
        d->last_seen = now;
        d->hit_count++;
        d->detect_flags |= detect;
        if (name_str[0] && d->name[0] == '\0') {
            strncpy(d->name, name_str, DEVICE_NAME_LEN - 1);
        }
    } else if (g_app.device_count < MAX_BLE_DEVICES) {
        /* Insert new */
        ble_device_t *d = &g_app.devices[g_app.device_count];
        memset(d, 0, sizeof(*d));
        memcpy(d->mac, addr, 6);
        strncpy(d->name, name_str, DEVICE_NAME_LEN - 1);
        d->rssi        = rssi;
        d->rssi_best   = rssi;
        d->detect_flags= detect;
        d->first_seen  = now;
        d->last_seen   = now;
        d->hit_count   = 1;
        d->is_flock    = (detect & (DETECT_OUI | DETECT_NAME | DETECT_CID)) != 0;
        d->is_raven    = is_raven;
        d->raven_fw    = raven_fw;
        d->company_id  = cid;
        g_app.device_count++;

        ESP_LOGI(TAG, "NEW device #%d [%02X:%02X:%02X:%02X:%02X:%02X] RSSI=%d flags=0x%02X",
                 g_app.device_count, addr[0], addr[1], addr[2],
                 addr[3], addr[4], addr[5], rssi, detect);

        /* Alert on new detection */
        buzzer_beep(50);
        if (d->is_flock) {
            s_warning_started_ms = now;
        }
    }

    xSemaphoreGive(g_app.device_mutex);
}

/* LCD status update task — Terracotta surveillance aesthetic */
static void flock_display_task(void *arg)
{
    int last_device_count = -1;
    int last_cursor = -1;
    int last_wifi_clients = -1;
    int last_gps_active = -1;
    int last_logging_active = -1;
    int last_logging_blocked = -1;

    while (g_app.current_mode == MODE_FLOCK_YOU) {
        uint32_t now_ms = uptime_ms();
        bool gps_tag_active = flock_gps_tag_active(now_ms);
        bool logging_active = storage_ext_logging_active();
        bool logging_blocked = storage_ext_logging_blocked();

        /* Check if anything worth redrawing has changed */
        bool dirty = (g_app.device_count != last_device_count)
                   || (g_app.ui_cursor != last_cursor)
                   || (g_app.wifi_clients != last_wifi_clients)
                   || ((int)gps_tag_active != last_gps_active)
                   || ((int)logging_active != last_logging_active)
                   || ((int)logging_blocked != last_logging_blocked);

        if (!dirty) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        last_device_count = g_app.device_count;
        last_cursor = g_app.ui_cursor;
        last_wifi_clients = g_app.wifi_clients;
        last_gps_active = (int)gps_tag_active;
        last_logging_active = (int)logging_active;
        last_logging_blocked = (int)logging_blocked;

        /* Dark zinc base with vivid red accent */
        uint16_t bg = rgb565(9, 9, 11);
        uint16_t accent = rgb565(248, 113, 113);
        uint16_t dim_accent = rgb565(153, 27, 27);
        uint16_t raven_col = rgb565(168, 85, 247);
        uint16_t flock_num_col = rgb565(251, 146, 60);
        uint16_t panel_bg = rgb565(24, 24, 27);
        uint16_t panel_alt = rgb565(20, 20, 22);
        uint16_t text_main = rgb565(250, 250, 250);
        uint16_t text_dim = rgb565(161, 161, 170);
        uint16_t text_soft = rgb565(113, 113, 122);
        uint16_t footer = rgb565(9, 9, 11);
        uint16_t border_col = rgb565(63, 63, 70);
        const char *ap_ssid = NULL;
        const char *ap_pass = NULL;
        app_mode_ap_credentials(MODE_FLOCK_YOU, &ap_ssid, &ap_pass, NULL);

        /* Status bar */
        display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 26, dim_accent);
        display_draw_rect(0, DISPLAY_STATUS_DIV_Y, LCD_H_RES, 2, accent);
        display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, "FLOCK YOU", text_main, dim_accent);
        display_draw_text_centered(DISPLAY_STATUS_SUB_Y, ap_ssid, rgb565(228, 228, 231), dim_accent);
        flock_draw_logging_badge(dim_accent);

        /* Content area */
        display_draw_rect(0, DISPLAY_CONTENT_TOP, LCD_H_RES, DISPLAY_FOOTER_BAR_Y - DISPLAY_CONTENT_TOP, bg);

        /* Device counter section */
        char buf[64];
        display_draw_bordered_rect(4, 38, LCD_H_RES - 8, 36, border_col, panel_bg);
        display_draw_text(10, 42, "DETECTED TARGETS", text_main, panel_bg);
        snprintf(buf, sizeof(buf), "%d", g_app.device_count);
        display_draw_text_scaled(10, 54, buf, accent, panel_bg, 2);

        /* Count breakdown */
        int flock_cnt = 0, raven_cnt = 0;
        for (int i = 0; i < g_app.device_count; i++) {
            if (g_app.devices[i].is_flock) flock_cnt++;
            if (g_app.devices[i].is_raven) raven_cnt++;
        }
        display_draw_text(56, 58, "F:", text_soft, panel_bg);
        snprintf(buf, sizeof(buf), "%d", flock_cnt);
        display_draw_text(68, 58, buf, flock_num_col, panel_bg);
        display_draw_text(88, 58, "R:", text_soft, panel_bg);
        snprintf(buf, sizeof(buf), "%d", raven_cnt);
        display_draw_text(100, 58, buf, raven_col, panel_bg);

        display_draw_text(122, 42, "GPS TAG", text_soft, panel_bg);
        display_draw_text(132, 54,
                          gps_tag_active ? "ON" : "OFF",
                          gps_tag_active ? rgb565(74, 222, 128) : rgb565(248, 113, 113),
                          panel_bg);
        /* Accent divider line */
        display_draw_hline(4, 78, LCD_H_RES - 8, accent);

        /* Scanning indicator intentionally omitted to avoid periodic UI flicker.
         * Scan activity is conveyed by LED behavior and web interface updates. */

        /* Keep UI cursor in range for device list */
        g_app.ui_item_count = g_app.device_count;
        if (g_app.ui_cursor >= g_app.device_count)
            g_app.ui_cursor = g_app.device_count > 0 ? g_app.device_count - 1 : 0;

        /* Device list — last 5 with left accent bar + cursor highlight */
        int start = (g_app.device_count > 5) ? g_app.device_count - 5 : 0;
        if (g_app.ui_cursor < start) start = g_app.ui_cursor;
        if (g_app.ui_cursor >= start + 5) start = g_app.ui_cursor - 4;

        for (int i = start, row = 0; i < g_app.device_count && row < 5; i++, row++) {
            int y_pos = 98 + row * 30;
            bool selected = (i == g_app.ui_cursor && g_app.device_count > 0);

            uint16_t indicator = g_app.devices[i].is_raven ? raven_col : accent;

            if (selected) {
                uint16_t sel_bg = rgb565(39, 39, 42);
                display_draw_rect(4, y_pos, 3, 24, indicator);
                display_draw_rect(10, y_pos, LCD_H_RES - 16, 24, sel_bg);

                char mac_str[18];
                mac_to_str(g_app.devices[i].mac, mac_str, sizeof(mac_str));
                display_draw_text(14, y_pos + 2, mac_str, text_main, sel_bg);

                snprintf(buf, sizeof(buf), "%ddBm x%d",
                         g_app.devices[i].rssi, g_app.devices[i].hit_count);
                display_draw_text(14, y_pos + 12, buf, accent, sel_bg);
            } else {
                display_draw_rect(4, y_pos, 3, 24, indicator);
                display_draw_rect(10, y_pos, LCD_H_RES - 16, 24, panel_alt);

                char mac_str[18];
                mac_to_str(g_app.devices[i].mac, mac_str, sizeof(mac_str));
                display_draw_text(14, y_pos + 2, mac_str,
                                  g_app.devices[i].is_raven ? raven_col : text_main,
                                  panel_alt);

                snprintf(buf, sizeof(buf), "%ddBm x%d",
                         g_app.devices[i].rssi, g_app.devices[i].hit_count);
                display_draw_text(14, y_pos + 12, buf, text_dim, panel_alt);
            }
        }

        /* Bottom bar with WiFi info */
        display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer);
        display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, 1, border_col);
        snprintf(buf, sizeof(buf), "%s  %dCli  %lukB",
             ap_pass,
                 g_app.wifi_clients, (unsigned long)(g_app.free_heap / 1024));
        display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, buf, text_dim, footer);

        vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelete(NULL);
}

void flock_you_start(void)
{
    ESP_LOGI(TAG, "Starting Flock You mode");
    led_ctrl_breathe_stop();
    buzzer_melody_flock();

    /* Start BLE scan: 100ms interval, 50ms window for coexistence */
    ble_scanner_start(flock_scan_cb, 100, 50, false);

    if (xTaskCreate(flock_led_task, "flock_led", TASK_STACK_UI, NULL, 2, &s_led_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Flock You LED task");
        s_led_task = NULL;
    }

    /* LCD status task */
    if (xTaskCreate(flock_display_task, "flock_lcd", TASK_STACK_UI, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Flock You display task");
    }
}

void flock_you_stop(void)
{
    ble_scanner_stop();
    s_warning_started_ms = 0;
    led_ctrl_off();
    ESP_LOGI(TAG, "Flock You stopped");
}

int flock_you_device_count(void)
{
    return g_app.device_count;
}
