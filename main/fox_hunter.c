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

/* ── Beep + display loop -- Amber hunting aesthetic ── */
static void fox_beep_task(void *arg)
{
    int frame = 0;
    while (g_app.current_mode == MODE_FOX_HUNTER) {
        frame++;
        uint32_t now = uptime_ms();
        bool target_visible = g_app.fox_target_found &&
                              (now - g_app.fox_last_seen < TARGET_LOST_MS);

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

        /* LED mode label for footer area */
        const char *led_mode_str = g_app.fox_led_mode ? "STING" : "DETECTOR";

        if (!g_app.fox_target_set) {
            /* No target -- solid orange LED (Detector) or off (Sting) */
            if (g_app.fox_led_mode == 0) {
                led_ctrl_set(255, 120, 0);
            } else {
                led_ctrl_off();
            }

            display_draw_bordered_rect(20, 80, LCD_H_RES - 40, 80, border_col, panel_bg);
            display_draw_text(28, 90, "NO TARGET SET", text_main, panel_bg);
            display_draw_hline(20, 120, LCD_H_RES - 40, dim_accent);
            display_draw_text(28, 126, "Use web UI or", text_dim, panel_bg);
            display_draw_text(28, 138, "Flock You list", text_dim, panel_bg);

            /* Animated crosshair */
            int cx = LCD_H_RES / 2, cy = 190;
            int arm = 12 + (frame % 3) * 2;
            display_draw_rect(cx - arm, cy, arm * 2, 2, dim_accent);
            display_draw_rect(cx, cy - arm, 2, arm * 2, dim_accent);
        } else {
            char mac_str[18];
            mac_to_str(g_app.fox_target_mac, mac_str, sizeof(mac_str));

            /* Target info section */
            display_draw_bordered_rect(4, 38, LCD_H_RES - 8, 28, border_col, panel_bg);
            display_draw_text(8, 40, "TARGET LOCKED", text_main, panel_bg);
            display_draw_text(8, 52, mac_str, rgb565(253, 186, 116), panel_bg);

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

                /* Proximity bar */
                display_draw_rect(4, 122, 2, 6, accent);
                display_draw_rect(LCD_H_RES - 6, 122, 2, 6, accent);
                int bar_w = rssi_to_bar(g_app.fox_rssi);
                display_draw_rect(8, 124, bar_w, 16, rssi_col);
                display_draw_rect(8 + bar_w, 124, LCD_H_RES - 16 - bar_w, 16, rgb565(39, 39, 42));

                /* Signal strength bars */
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

                if (g_app.fox_led_mode == 0) {
                    /* Detector: blink red, speed = RSSI proximity */
                    led_ctrl_set(220, 20, 10);
                    vTaskDelay(pdMS_TO_TICKS(interval / 2));
                    led_ctrl_off();
                    vTaskDelay(pdMS_TO_TICKS(interval / 2));
                } else {
                    /* Sting: bright blue, brightness = signal strength */
                    float str = (g_app.fox_rssi + 100.0f) / 80.0f;
                    if (str < 0.0f) str = 0.0f;
                    if (str > 1.0f) str = 1.0f;
                    uint8_t br = (uint8_t)(30 + str * 225);
                    led_ctrl_set(0, (uint8_t)(br / 4), br);
                    vTaskDelay(pdMS_TO_TICKS(interval));
                }
            } else {
                /* Signal lost -- LED shows green (Detector) or off (Sting) */
                if (g_app.fox_led_mode == 0) {
                    led_ctrl_set(0, 120, 0);
                } else {
                    led_ctrl_off();
                }

                display_draw_bordered_rect(20, 90, LCD_H_RES - 40, 50, rgb565(127, 29, 29), rgb565(30, 10, 10));
                display_draw_text(40, 100, "SIGNAL LOST", rgb565(248, 113, 113), rgb565(30, 10, 10));

                const char *search_anim[] = {"Searching.", "Searching..", "Searching..."};
                display_draw_text(30, 120, search_anim[frame % 3], text_dim, rgb565(30, 10, 10));

                /* Crosshair animation */
                int cx = LCD_H_RES / 2, cy = 180;
                uint16_t cross_col = (frame % 2) ? dim_accent : rgb565(82, 40, 12);
                display_draw_rect(cx - 20, cy, 40, 2, cross_col);
                display_draw_rect(cx, cy - 20, 2, 40, cross_col);

                buzzer_off();
            }
        }

        /* LED mode indicator above footer */
        {
            uint16_t mode_col = g_app.fox_led_mode ? rgb565(56, 189, 248) : rgb565(74, 222, 128);
            char mode_label[24];
            snprintf(mode_label, sizeof(mode_label), "LED: %s", led_mode_str);
            display_draw_text(4, DISPLAY_FOOTER_BAR_Y - 12, mode_label, mode_col, bg);
            display_draw_text(LCD_H_RES - 60, DISPLAY_FOOTER_BAR_Y - 12, "Hold=Tog", text_dim, bg);
        }

        /* Bottom bar with WiFi info */
        display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer_bg);
        display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, 1, border_col);
        char info[48];
        snprintf(info, sizeof(info), "foxhunt123  %dCli  %lukB",
                 g_app.wifi_clients, (unsigned long)(g_app.free_heap / 1024));
        display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, info, text_dim, footer_bg);

        if (!target_visible || !g_app.fox_target_set) {
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
