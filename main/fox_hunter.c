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

/* ── RSSI to color gradient (burgundy→amber) ── */
static void rssi_to_color(int8_t rssi, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Map -100..-20 → 0..1 */
    float t = (rssi + 100.0f) / 80.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *r = (uint8_t)(118.0f + t * 86.0f);
    *g = (uint8_t)(54.0f + t * 92.0f);
    *b = (uint8_t)(60.0f - t * 48.0f);
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

/* ── Scan callback ── */
static void fox_scan_cb(const uint8_t *addr, int8_t rssi,
                        const uint8_t *adv_data, uint8_t adv_len,
                        const uint8_t *name, uint8_t name_len)
{
    if (!g_app.fox_target_set) return;
    if (!mac_equal(addr, g_app.fox_target_mac)) return;

    g_app.fox_rssi = rssi;
    if (rssi > g_app.fox_rssi_best) g_app.fox_rssi_best = rssi;
    g_app.fox_target_found = true;
    g_app.fox_last_seen = uptime_ms();
}

/* ── Beep + display loop — Amber hunting aesthetic ── */
static void fox_beep_task(void *arg)
{
    int frame = 0;
    while (g_app.current_mode == MODE_FOX_HUNTER) {
        frame++;
        uint32_t now = uptime_ms();
        bool target_visible = g_app.fox_target_found &&
                              (now - g_app.fox_last_seen < TARGET_LOST_MS);

        /* Sand, brass, and oxblood palette */
        uint16_t bg = rgb565(239, 229, 212);
        uint16_t accent = rgb565(169, 125, 62);
        uint16_t dim_amber = rgb565(193, 166, 122);
        uint16_t panel_bg = rgb565(248, 241, 228);
        uint16_t text_main = rgb565(68, 51, 31);
        uint16_t text_dim = rgb565(117, 96, 68);
        uint16_t footer = rgb565(205, 193, 173);
        uint16_t status_fill = rgb565(232, 220, 204);
        uint16_t status_border = rgb565(160, 120, 78);
        uint16_t status_text = rgb565(92, 61, 37);

        /* Status bar: warm amber */
        display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 26, accent);
        display_draw_rect(0, DISPLAY_STATUS_DIV_Y, LCD_H_RES, 2, rgb565(139, 99, 48));
        display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, "FOX HUNTER", rgb565(40, 29, 16), accent);
        display_draw_text_centered(DISPLAY_STATUS_SUB_Y, "192.168.4.1", rgb565(83, 61, 30), accent);

        /* Content area */
        display_draw_rect(0, DISPLAY_CONTENT_TOP, LCD_H_RES, DISPLAY_FOOTER_BAR_Y - DISPLAY_CONTENT_TOP, bg);

        if (!g_app.fox_target_set) {
            /* No target — show crosshair waiting animation */
            display_draw_bordered_rect(20, 80, LCD_H_RES - 40, 80, dim_amber, panel_bg);
            display_draw_text(28, 90, "NO TARGET SET", text_main, panel_bg);
            display_draw_hline(20, 120, LCD_H_RES - 40, dim_amber);
            display_draw_text(28, 126, "Use web UI or", text_dim, panel_bg);
            display_draw_text(28, 138, "Flock You list", text_dim, panel_bg);

            /* Animated crosshair */
            int cx = LCD_H_RES / 2, cy = 190;
            int arm = 12 + (frame % 3) * 2;
            display_draw_rect(cx - arm, cy, arm * 2, 2, dim_amber);
            display_draw_rect(cx, cy - arm, 2, arm * 2, dim_amber);
        } else {
            char mac_str[18];
            mac_to_str(g_app.fox_target_mac, mac_str, sizeof(mac_str));

            /* Target info section */
            display_draw_bordered_rect(4, 38, LCD_H_RES - 8, 28, dim_amber, panel_bg);
            display_draw_text(8, 40, "TARGET LOCKED", text_main, panel_bg);
            display_draw_text(8, 52, mac_str, rgb565(89, 68, 40), panel_bg);

            /* Amber divider */
            display_draw_hline(4, 70, LCD_H_RES - 8, accent);

            if (target_visible) {
                char buf[32];

                /* Large RSSI display */
                uint8_t cr, cg, cb;
                rssi_to_color(g_app.fox_rssi, &cr, &cg, &cb);
                uint16_t rssi_col = rgb565(cr, cg, cb);

                snprintf(buf, sizeof(buf), "%d", g_app.fox_rssi);
                display_draw_text_scaled(20, 78, buf, rssi_col, bg, 3);
                display_draw_text(LCD_H_RES - 40, 86, "dBm", text_dim, bg);

                /* Best RSSI */
                snprintf(buf, sizeof(buf), "Best: %d dBm", g_app.fox_rssi_best);
                display_draw_text(8, 106, buf, text_main, bg);

                /* Crosshair-framed proximity bar */
                display_draw_rect(4, 122, 2, 6, accent);  /* left tick */
                display_draw_rect(LCD_H_RES - 6, 122, 2, 6, accent);  /* right tick */
                int bar_w = rssi_to_bar(g_app.fox_rssi);
                display_draw_rect(8, 124, bar_w, 16, rssi_col);
                display_draw_rect(8 + bar_w, 124, LCD_H_RES - 16 - bar_w, 16, rgb565(206, 194, 171));

                /* Signal strength bars (20 bars) */
                int pct = (g_app.fox_rssi + 100) * 100 / 80;
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                for (int i = 0; i < 20; i++) {
                    int bar_h = 4 + i;
                    int bx = 8 + i * 8;
                    bool active = (i * 5) < pct;
                    uint16_t bar_col = active ? rssi_col : rgb565(200, 188, 164);
                    display_draw_rect(bx, 152 + (24 - bar_h), 6, bar_h, bar_col);
                }

                /* Percentage */
                snprintf(buf, sizeof(buf), "%d%%", pct);
                display_draw_text(72, 180, buf, text_main, bg);

                /* Status indicator */
                display_draw_bordered_rect(20, 196, LCD_H_RES - 40, 20, status_border, status_fill);
                display_draw_text(40, 202, "TRACKING", status_text, status_fill);

                /* Beep and LED */
                int interval = rssi_to_interval(g_app.fox_rssi);
                uint32_t freq = 800 + (uint32_t)((g_app.fox_rssi + 100) * 20);
                buzzer_tone(freq, 20);
                led_ctrl_set(cr, cg, cb);
                vTaskDelay(pdMS_TO_TICKS(interval));
                led_ctrl_off();
            } else {
                /* Signal lost — pulsing amber crosshair */
                display_draw_bordered_rect(20, 90, LCD_H_RES - 40, 50, rgb565(155, 111, 98), rgb565(236, 220, 214));
                display_draw_text(40, 100, "SIGNAL LOST", rgb565(126, 58, 58), rgb565(236, 220, 214));

                const char *search_anim[] = {"Searching.", "Searching..", "Searching..."};
                display_draw_text(30, 120, search_anim[frame % 3], text_dim, rgb565(236, 220, 214));

                /* Dimming crosshair animation */
                int cx = LCD_H_RES / 2, cy = 180;
                uint16_t cross_col = (frame % 2) ? dim_amber : rgb565(180, 162, 130);
                display_draw_rect(cx - 20, cy, 40, 2, cross_col);
                display_draw_rect(cx, cy - 20, 2, 40, cross_col);

                led_ctrl_off();
                buzzer_off();
            }
        }

        /* Bottom bar */
        display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer);
        char info[32];
        snprintf(info, sizeof(info), "Heap:%lukB", (unsigned long)(g_app.free_heap / 1024));
        display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, info, text_main, footer);

        if (!target_visible || !g_app.fox_target_set) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    vTaskDelete(NULL);
}

void fox_hunter_start(void)
{
    ESP_LOGI(TAG, "Starting Fox Hunter mode");
    buzzer_melody_fox();

    g_app.fox_target_found = false;
    g_app.fox_rssi = -100;
    g_app.fox_rssi_best = -100;

    /* Try to load saved target from NVS */
    if (nvs_store_load_fox_target(g_app.fox_target_mac) == 0) {
        g_app.fox_target_set = true;
        char mac_str[18];
        mac_to_str(g_app.fox_target_mac, mac_str, sizeof(mac_str));
        ESP_LOGI(TAG, "Loaded target: %s", mac_str);
    }

    /* High-duty BLE scan for maximum responsiveness */
    ble_scanner_start(fox_scan_cb, 16, 15, false);

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

bool fox_hunter_has_target(void)
{
    return g_app.fox_target_set;
}
