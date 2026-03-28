/*
 * OUI-Spy C6 — Fox Hunter: BLE proximity tracker
 *
 * Locks onto a single MAC and maps RSSI to a buzzer cadence,
 * functioning like a Geiger counter for Bluetooth signals.
 * Targets can be imported from Flock You detections or set via web UI.
 *
 * RSSI → cadence mapping (7 zones):
 *   -25 to -35 dBm → 10-25ms  (machine gun)
 *   -35 to -45 dBm → 25-75ms  (ultra fast)
 *   -45 to -55 dBm → 75-150ms (rapid)
 *   -55 to -65 dBm → 150-250ms (quick)
 *   -65 to -75 dBm → 250-400ms (moderate)
 *   -75 to -85 dBm → 400-600ms (steady)
 *   Below -85 dBm  → 800ms     (slow)
 *
 * SPDX-License-Identifier: MIT
 */
#include "fox_hunter.h"
#include "app_common.h"
#include "ble_scanner.h"
#include "buzzer.h"
#include "led_ctrl.h"
#include "nvs_store.h"
#include "display.h"
#include <string.h>
#include <math.h>

static const char *TAG = "fox";
static TaskHandle_t s_beep_task = NULL;

#define TARGET_LOST_MS  5000

/* ── RSSI to beep interval (ms) ── */
static int rssi_to_interval(int8_t rssi)
{
    if (rssi >= -35) return 15;
    if (rssi >= -45) return 50;
    if (rssi >= -55) return 110;
    if (rssi >= -65) return 200;
    if (rssi >= -75) return 325;
    if (rssi >= -85) return 500;
    return 800;
}

/* ── RSSI to color gradient (cool slate -> warm amber -> hot gold) ── */
static void rssi_to_color(int8_t rssi, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Map -100..-20 -> 0..1 */
    float t = (rssi + 100.0f) / 80.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *r = (uint8_t)(100.0f + t * 118.0f);
    *g = (uint8_t)(72.0f + t * 96.0f);
    *b = (uint8_t)(48.0f - t * 36.0f);
}

/* ── Proximity bar for display ── */
static int rssi_to_bar(int8_t rssi)
{
    /* 0 bars at -100, full at -20 */
    int bars = (rssi + 100) * LCD_H_RES / 80;
    if (bars < 0) bars = 0;
    if (bars > LCD_H_RES - 8) bars = LCD_H_RES - 8;
    return bars;
}

static const char *rssi_to_proximity_label(int8_t rssi)
{
    if (rssi >= -45) return "VERY CLOSE";
    if (rssi >= -60) return "CLOSE";
    if (rssi >= -72) return "NEAR";
    if (rssi >= -84) return "FAR";
    return "VERY FAR";
}

int fox_hunter_registry_view_count(void)
{
    int nearby = g_app.fox_nearby_count;
    if (nearby < 0) nearby = 0;
    return g_app.fox_registry_count + nearby;
}

/* ── Scan callback ── */
static void fox_scan_cb(const uint8_t *addr, int8_t rssi,
                        const uint8_t *adv_data, uint8_t adv_len,
                        const uint8_t *name, uint8_t name_len)
{
    /* ── Track active target ── */
    if (g_app.fox_target_set && mac_equal(addr, g_app.fox_target_mac)) {
        g_app.fox_rssi = rssi;
        if (rssi > g_app.fox_rssi_best) g_app.fox_rssi_best = rssi;
        g_app.fox_target_found = true;
        g_app.fox_last_seen = uptime_ms();
    }

    /* ── Accumulate all nearby BLE devices for candidate selection ── */
    char name_str[DEVICE_NAME_LEN] = {0};
    if (name_len > 0) {
        int n = (name_len < DEVICE_NAME_LEN - 1) ? name_len : DEVICE_NAME_LEN - 1;
        memcpy(name_str, name, n);
    }

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    uint32_t now = uptime_ms();
    int idx = -1;
    for (int i = 0; i < g_app.fox_nearby_count; i++) {
        if (mac_equal(g_app.fox_nearby[i].mac, addr)) { idx = i; break; }
    }

    if (idx >= 0) {
        ble_device_t *d = &g_app.fox_nearby[idx];
        d->rssi      = rssi;
        if (rssi > d->rssi_best) d->rssi_best = rssi;
        d->last_seen = now;
        if (d->hit_count < 65535) d->hit_count++;
        if (name_str[0] && d->name[0] == '\0')
            strncpy(d->name, name_str, DEVICE_NAME_LEN - 1);
    } else if (g_app.fox_nearby_count < FOX_NEARBY_MAX) {
        ble_device_t *d = &g_app.fox_nearby[g_app.fox_nearby_count];
        memset(d, 0, sizeof(*d));
        memcpy(d->mac, addr, 6);
        strncpy(d->name, name_str, DEVICE_NAME_LEN - 1);
        d->rssi       = rssi;
        d->rssi_best  = rssi;
        d->first_seen = now;
        d->last_seen  = now;
        d->hit_count  = 1;
        g_app.fox_nearby_count++;
    }

    xSemaphoreGive(g_app.device_mutex);
}

/* ── Beep + display loop -- Amber hunting aesthetic ── */
static void fox_beep_task(void *arg)
{
    int frame = 0;

    /* Snapshot state for dirty-check: avoid full redraw when nothing visual changed */
    bool last_target_set = false;
    bool last_target_visible = false;
    int8_t last_rssi_zone = -128;   /* coarse RSSI bucket for display */
    uint8_t last_led_mode = 0xFF;
    int last_wifi_clients = -1;
    bool last_registry_open = false;
    int last_reg_cursor = -1;
    int last_reg_count = -1;
    bool display_drawn = false;     /* force first draw */

    while (g_app.current_mode == MODE_FOX_HUNTER) {
        frame++;
        uint32_t now = uptime_ms();
        bool target_visible = g_app.fox_target_found &&
                              (now - g_app.fox_last_seen < TARGET_LOST_MS);

        /* Coarse RSSI bucket (5 dBm steps) — avoid redrawing for tiny fluctuations */
        int8_t rssi_zone = target_visible ? (g_app.fox_rssi / 5) : -128;

        bool dirty = !display_drawn
                   || (g_app.fox_target_set != last_target_set)
                   || (target_visible != last_target_visible)
                   || (rssi_zone != last_rssi_zone)
                   || (g_app.fox_led_mode != last_led_mode)
                   || (g_app.wifi_clients != last_wifi_clients)
                   || (g_app.fox_registry_open != last_registry_open)
                   || (g_app.fox_registry_open && (g_app.ui_cursor != last_reg_cursor
                       || g_app.fox_registry_count != last_reg_count));

        /* ── Display: only redraw when visual state changed ── */
        if (dirty) {
            last_target_set = g_app.fox_target_set;
            last_target_visible = target_visible;
            last_rssi_zone = rssi_zone;
            last_led_mode = g_app.fox_led_mode;
            last_wifi_clients = g_app.wifi_clients;
            last_registry_open = g_app.fox_registry_open;
            last_reg_cursor = g_app.ui_cursor;
            last_reg_count = g_app.fox_registry_count;
            display_drawn = true;

            /* Dark zinc base with vivid orange accent */
            uint16_t bg = rgb565(9, 9, 11);
            uint16_t accent = rgb565(251, 146, 60);
            uint16_t dim_accent = rgb565(154, 52, 18);
            uint16_t panel_bg = rgb565(24, 24, 27);
            uint16_t text_main = rgb565(250, 250, 250);
            uint16_t text_dim = rgb565(161, 161, 170);
            uint16_t footer_bg = rgb565(9, 9, 11);
            uint16_t border_col = rgb565(63, 63, 70);
            uint16_t status_fill = rgb565(30, 20, 8);
            uint16_t status_border = rgb565(251, 146, 60);
            uint16_t status_text = rgb565(253, 186, 116);

            /* Status bar */
            display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 26, dim_accent);
            display_draw_rect(0, DISPLAY_STATUS_DIV_Y, LCD_H_RES, 2, accent);
            display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, "FOX HUNTER", text_main, dim_accent);
            display_draw_text_centered(DISPLAY_STATUS_SUB_Y, "foxhunt-c6", rgb565(228, 228, 231), dim_accent);

            /* Content area */
            display_draw_rect(0, DISPLAY_CONTENT_TOP, LCD_H_RES, DISPLAY_FOOTER_BAR_Y - DISPLAY_CONTENT_TOP, bg);

            if (g_app.fox_registry_open) {
                /* ── Registry view ── */
                display_draw_text_centered(DISPLAY_CONTENT_TOP + 2, "TARGET REGISTRY (ALL)", accent, bg);
                display_draw_hline(4, DISPLAY_CONTENT_TOP + 14, LCD_H_RES - 8, dim_accent);

                int total_count = fox_hunter_registry_view_count();
                if (total_count == 0) {
                    display_draw_text_centered(100, "No saved targets", text_dim, bg);
                    display_draw_text_centered(114, "No nearby devices", text_dim, bg);
                } else {
                    g_app.ui_item_count = total_count;
                    if (g_app.ui_cursor >= total_count)
                        g_app.ui_cursor = total_count - 1;

                    int max_show = 6;
                    int start = 0;
                    if (g_app.ui_cursor >= max_show) start = g_app.ui_cursor - max_show + 1;

                    int nearby_count = 0;
                    bool have_nearby_lock = false;
                    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        nearby_count = g_app.fox_nearby_count;
                        have_nearby_lock = true;
                    }

                    for (int idx = start, row = 0; idx < total_count && row < max_show; idx++, row++) {
                        bool is_saved = (idx < g_app.fox_registry_count);
                        char mac_str[18];
                        char line1[FOX_REG_NICK_LEN + 8] = {0};
                        char line2[DEVICE_NAME_LEN + 20] = {0};

                        if (is_saved) {
                            fox_reg_entry_t *e = &g_app.fox_registry[idx];
                            mac_to_str(e->mac, mac_str, sizeof(mac_str));
                            if (e->nickname[0]) {
                                snprintf(line1, sizeof(line1), "%s", e->nickname);
                            } else if (e->label[0]) {
                                snprintf(line1, sizeof(line1), "%s", e->label);
                            } else {
                                snprintf(line1, sizeof(line1), "Saved target");
                            }
                            snprintf(line2, sizeof(line2), "%s", mac_str);
                        } else {
                            int nidx = idx - g_app.fox_registry_count;
                            if (have_nearby_lock && nidx >= 0 && nidx < nearby_count) {
                                ble_device_t *d = &g_app.fox_nearby[nidx];
                                mac_to_str(d->mac, mac_str, sizeof(mac_str));
                                if (d->name[0]) {
                                    snprintf(line1, sizeof(line1), "%s", d->name);
                                } else {
                                    snprintf(line1, sizeof(line1), "Nearby BLE");
                                }
                                snprintf(line2, sizeof(line2), "%s  %d dBm", mac_str, d->rssi);
                            } else {
                                snprintf(line1, sizeof(line1), "Nearby BLE");
                                snprintf(line2, sizeof(line2), "Unavailable");
                            }
                        }

                        int y_base = DISPLAY_CONTENT_TOP + 20 + row * 34;
                        bool selected = (idx == g_app.ui_cursor);
                        uint16_t row_bg = selected ? rgb565(30, 20, 8) : panel_bg;
                        uint16_t row_border = selected ? accent : (is_saved ? border_col : rgb565(45, 58, 76));

                        display_draw_bordered_rect(4, y_base, LCD_H_RES - 8, 30, row_border, row_bg);
                        display_draw_text(10, y_base + 3, line1, selected ? accent : text_main, row_bg);
                        display_draw_text(10, y_base + 15, line2, text_dim, row_bg);
                        if (is_saved) {
                            display_draw_text(LCD_H_RES - 60, y_base + 3, "SAVED", rgb565(74, 222, 128), row_bg);
                        } else {
                            display_draw_text(LCD_H_RES - 66, y_base + 3, "NEARBY", rgb565(56, 189, 248), row_bg);
                        }
                        if (selected) {
                            display_draw_text(LCD_H_RES - 24, y_base + 10, "GO", accent, row_bg);
                        }
                    }

                    if (have_nearby_lock) xSemaphoreGive(g_app.device_mutex);
                }

                display_draw_text_centered(DISPLAY_FOOTER_BAR_Y - 24, "DblClk=Select Hold=Prev 3xClk=Back", text_dim, bg);
            } else {
                /* ── Normal tracker view ── */
            if (!g_app.fox_target_set) {
                display_draw_bordered_rect(20, 80, LCD_H_RES - 40, 80, border_col, panel_bg);
                display_draw_text(28, 90, "NO TARGET SET", text_main, panel_bg);
                display_draw_hline(20, 120, LCD_H_RES - 40, dim_accent);
                display_draw_text(28, 126, "Hold=Registry", text_dim, panel_bg);
                display_draw_text(28, 138, "Web UI / Flock", text_dim, panel_bg);

                int cx = LCD_H_RES / 2, cy = 190;
                display_draw_rect(cx - 16, cy, 32, 2, dim_accent);
                display_draw_rect(cx, cy - 16, 2, 32, dim_accent);
            } else {
                char mac_str[18];
                mac_to_str(g_app.fox_target_mac, mac_str, sizeof(mac_str));

                display_draw_bordered_rect(4, 38, LCD_H_RES - 8, 28, border_col, panel_bg);
                display_draw_text(8, 40, "TARGET LOCKED", text_main, panel_bg);
                display_draw_text(8, 52, mac_str, rgb565(253, 186, 116), panel_bg);
                display_draw_hline(4, 70, LCD_H_RES - 8, accent);

                if (target_visible) {
                    char buf[32];
                    uint8_t cr, cg, cb;
                    rssi_to_color(g_app.fox_rssi, &cr, &cg, &cb);
                    uint16_t rssi_col = rgb565(cr, cg, cb);

                    snprintf(buf, sizeof(buf), "%d", g_app.fox_rssi);
                    display_draw_text_scaled(20, 78, buf, rssi_col, bg, 3);
                    display_draw_text(LCD_H_RES - 40, 86, "dBm", text_dim, bg);

                    snprintf(buf, sizeof(buf), "Best: %d dBm", g_app.fox_rssi_best);
                    display_draw_text(8, 106, buf, text_main, bg);

                    snprintf(buf, sizeof(buf), "Seen: now");
                    display_draw_text(96, 106, buf, text_dim, bg);

                    display_draw_rect(4, 122, 2, 6, accent);
                    display_draw_rect(LCD_H_RES - 6, 122, 2, 6, accent);
                    int bar_w = rssi_to_bar(g_app.fox_rssi);
                    display_draw_rect(8, 124, bar_w, 16, rssi_col);
                    display_draw_rect(8 + bar_w, 124, LCD_H_RES - 16 - bar_w, 16, rgb565(39, 39, 42));

                    int pct = (g_app.fox_rssi + 100) * 100 / 80;
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    for (int i = 0; i < 20; i++) {
                        int bar_h = 4 + i;
                        int bx = 8 + i * 8;
                        bool active = (i * 5) < pct;
                        uint16_t bar_col = active ? rssi_col : rgb565(39, 39, 42);
                        display_draw_rect(bx, 152 + (24 - bar_h), 6, bar_h, bar_col);
                    }

                    snprintf(buf, sizeof(buf), "%d%%", pct);
                    display_draw_text(72, 180, buf, text_main, bg);

                    snprintf(buf, sizeof(buf), "Prox: %s", rssi_to_proximity_label(g_app.fox_rssi));
                    display_draw_text_centered(191, buf, text_main, bg);

                    display_draw_bordered_rect(20, 196, LCD_H_RES - 40, 20, status_border, status_fill);
                    display_draw_text(40, 202, "TRACKING", status_text, status_fill);
                } else {
                    char buf[36];
                    uint32_t last_seen_sec = (g_app.fox_last_seen > 0 && now > g_app.fox_last_seen)
                        ? (now - g_app.fox_last_seen) / 1000U
                        : 0;

                    display_draw_bordered_rect(20, 90, LCD_H_RES - 40, 50, rgb565(127, 29, 29), rgb565(30, 10, 10));
                    display_draw_text(40, 100, "SIGNAL LOST", rgb565(248, 113, 113), rgb565(30, 10, 10));
                    display_draw_text(30, 120, "Searching...", text_dim, rgb565(30, 10, 10));
                    snprintf(buf, sizeof(buf), "Seen %lus ago", (unsigned long)last_seen_sec);
                    display_draw_text(30, 132, buf, text_dim, rgb565(30, 10, 10));
                    snprintf(buf, sizeof(buf), "Last prox: %s", rssi_to_proximity_label(g_app.fox_rssi));
                    display_draw_text(22, 146, buf, text_dim, bg);

                    int cx = LCD_H_RES / 2, cy = 180;
                    display_draw_rect(cx - 20, cy, 40, 2, dim_accent);
                    display_draw_rect(cx, cy - 20, 2, 40, dim_accent);
                }
            }
            } /* end else (normal tracker view) */

            /* LED mode indicator above footer — always shown */
            {
                uint16_t mode_col = g_app.fox_led_mode ? rgb565(56, 189, 248) : rgb565(74, 222, 128);
                const char *led_label = g_app.fox_led_mode ? "STING" : "DETECTOR";
                char mode_label[24];
                snprintf(mode_label, sizeof(mode_label), "LED: %s", led_label);
                display_draw_text(4, DISPLAY_FOOTER_BAR_Y - 12, mode_label, mode_col, bg);
            }

            /* Bottom bar with WiFi info */
            display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer_bg);
            display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, 1, border_col);
            char info[64];
            snprintf(info, sizeof(info), "foxhunt123  %dCli  %lukB",
                     g_app.wifi_clients, (unsigned long)(g_app.free_heap / 1024));
            display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, info, text_dim, footer_bg);
        } /* end if (dirty) */

        /* ── LED / buzzer: always runs regardless of display dirty ── */
        if (!g_app.fox_target_set) {
            if (g_app.fox_led_mode == 0) {
                led_ctrl_set(255, 120, 0);
            } else {
                led_ctrl_off();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        } else if (target_visible) {
            int interval = rssi_to_interval(g_app.fox_rssi);
            uint32_t freq = 800 + (uint32_t)((g_app.fox_rssi + 100) * 20);
            buzzer_tone(freq, 20);

            if (g_app.fox_led_mode == 0) {
                led_ctrl_set(220, 20, 10);
                vTaskDelay(pdMS_TO_TICKS(interval / 2));
                led_ctrl_off();
                vTaskDelay(pdMS_TO_TICKS(interval / 2));
            } else {
                float str = (g_app.fox_rssi + 100.0f) / 80.0f;
                if (str < 0.0f) str = 0.0f;
                if (str > 1.0f) str = 1.0f;
                uint8_t br = (uint8_t)(30 + str * 225);
                led_ctrl_set(0, (uint8_t)(br / 4), br);
                vTaskDelay(pdMS_TO_TICKS(interval));
            }
        } else {
            if (g_app.fox_led_mode == 0) {
                led_ctrl_set(0, 120, 0);
            } else {
                led_ctrl_off();
            }
            buzzer_off();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    vTaskDelete(NULL);
}

void fox_hunter_start(void)
{
    ESP_LOGI(TAG, "Starting Fox Hunter mode");
    led_ctrl_breathe_stop();
    buzzer_melody_fox();

    g_app.fox_target_found = false;
    g_app.fox_rssi = -100;
    g_app.fox_rssi_best = -100;

    /* Reset nearby candidate list for this session */
    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(g_app.fox_nearby, 0, sizeof(g_app.fox_nearby));
        g_app.fox_nearby_count = 0;
        xSemaphoreGive(g_app.device_mutex);
    }

    /* Try to load saved target from NVS */
    if (nvs_store_load_fox_target(g_app.fox_target_mac) == 0) {
        g_app.fox_target_set = true;
        char mac_str[18];
        mac_to_str(g_app.fox_target_mac, mac_str, sizeof(mac_str));
        ESP_LOGI(TAG, "Loaded target: %s", mac_str);
    }

    /* Balanced scan: good responsiveness while preserving WiFi coexistence */
    ble_scanner_start(fox_scan_cb, 100, 50, false);

    if (xTaskCreate(fox_beep_task, "fox_beep", TASK_STACK_UI, NULL, 3, &s_beep_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Fox Hunter task");
        s_beep_task = NULL;
    }
}

void fox_hunter_stop(void)
{
    ble_scanner_stop();
    buzzer_off();
    led_ctrl_off();
    s_beep_task = NULL;
    ESP_LOGI(TAG, "Fox Hunter stopped");
}

void fox_hunter_set_target(const uint8_t mac[6])
{
    memcpy(g_app.fox_target_mac, mac, 6);
    g_app.fox_target_set   = true;
    g_app.fox_target_found = false;
    g_app.fox_rssi         = -100;
    g_app.fox_rssi_best    = -100;
    nvs_store_save_fox_target(mac);

    char mac_str[18];
    mac_to_str(mac, mac_str, sizeof(mac_str));
    ESP_LOGI(TAG, "Target set: %s", mac_str);
}

void fox_hunter_set_target_from_flock(int device_index)
{
    if (device_index < 0 || device_index >= g_app.device_count) return;
    fox_hunter_set_target(g_app.devices[device_index].mac);
}

void fox_hunter_clear_target(void)
{
    memset(g_app.fox_target_mac, 0, sizeof(g_app.fox_target_mac));
    g_app.fox_target_set = false;
    g_app.fox_target_found = false;
    g_app.fox_last_seen = 0;
    g_app.fox_rssi = -100;
    g_app.fox_rssi_best = -100;
    nvs_store_clear_fox_target();
    ESP_LOGI(TAG, "Target cleared");
}

bool fox_hunter_has_target(void)
{
    return g_app.fox_target_set;
}

static void reg_copy_field(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

int fox_hunter_registry_add(const uint8_t mac[6], const char *label,
                            const char *original_name, const char *section)
{
    /* Check for duplicate */
    for (int i = 0; i < g_app.fox_registry_count; i++) {
        if (mac_equal(g_app.fox_registry[i].mac, mac)) {
            fox_reg_entry_t *existing = &g_app.fox_registry[i];
            bool changed = false;
            if (label && label[0] && !existing->label[0]) {
                reg_copy_field(existing->label, sizeof(existing->label), label);
                changed = true;
            }
            if (original_name && original_name[0] && !existing->original_name[0]) {
                reg_copy_field(existing->original_name, sizeof(existing->original_name), original_name);
                changed = true;
            }
            if (section && section[0] && !existing->section[0]) {
                reg_copy_field(existing->section, sizeof(existing->section), section);
                changed = true;
            }
            if (changed) nvs_store_save_fox_registry();
            return i; /* already exists */
        }
    }
    if (g_app.fox_registry_count >= FOX_REGISTRY_MAX) return -1; /* full */

    fox_reg_entry_t *e = &g_app.fox_registry[g_app.fox_registry_count];
    memset(e, 0, sizeof(*e));
    memcpy(e->mac, mac, sizeof(e->mac));
    reg_copy_field(e->label, sizeof(e->label), label);
    reg_copy_field(e->original_name, sizeof(e->original_name), original_name);
    reg_copy_field(e->section, sizeof(e->section), (section && section[0]) ? section : "auto");

    g_app.fox_registry_count++;
    nvs_store_save_fox_registry();
    ESP_LOGI(TAG, "Registry add [%d]: %s", g_app.fox_registry_count - 1, label ? label : "");
    return g_app.fox_registry_count - 1;
}

int fox_hunter_registry_update(int index, const char *nickname,
                               const char *notes, const char *section,
                               const char *label, const char *original_name)
{
    if (index < 0 || index >= g_app.fox_registry_count) return -1;

    fox_reg_entry_t *e = &g_app.fox_registry[index];

    if (nickname) reg_copy_field(e->nickname, sizeof(e->nickname), nickname);
    if (notes) reg_copy_field(e->notes, sizeof(e->notes), notes);
    if (section) reg_copy_field(e->section, sizeof(e->section), section);
    if (label) reg_copy_field(e->label, sizeof(e->label), label);
    if (original_name) reg_copy_field(e->original_name, sizeof(e->original_name), original_name);

    if (!e->section[0]) {
        reg_copy_field(e->section, sizeof(e->section), "auto");
    }

    nvs_store_save_fox_registry();
    return 0;
}

int fox_hunter_registry_remove(int index)
{
    if (index < 0 || index >= g_app.fox_registry_count) return -1;
    if (index < g_app.fox_registry_count - 1) {
        memmove(&g_app.fox_registry[index], &g_app.fox_registry[index + 1],
                (g_app.fox_registry_count - 1 - index) * sizeof(fox_reg_entry_t));
    }
    g_app.fox_registry_count--;
    nvs_store_save_fox_registry();
    return 0;
}

void fox_hunter_registry_select(int index)
{
    if (index < 0 || index >= g_app.fox_registry_count) return;
    fox_hunter_set_target(g_app.fox_registry[index].mac);
    g_app.fox_registry_open = false;
}

void fox_hunter_registry_select_view_index(int index)
{
    if (index < 0) return;

    if (index < g_app.fox_registry_count) {
        fox_hunter_registry_select(index);
        return;
    }

    int nearby_index = index - g_app.fox_registry_count;
    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        if (nearby_index >= 0 && nearby_index < g_app.fox_nearby_count) {
            fox_hunter_set_target(g_app.fox_nearby[nearby_index].mac);
            g_app.fox_registry_open = false;
        }
        xSemaphoreGive(g_app.device_mutex);
    }
}
