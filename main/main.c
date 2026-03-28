/*
 * OUI-Spy C6 — Main entry point
 *
 * Boot flow:
 *   1. Init NVS, hardware (display, LED, buzzer, buttons)
 *   2. Init WiFi AP + BLE stack + sniffer subsystem
 *   3. Start web server
 *   4. Load saved mode or start in mode selector
 *   5. Enter main loop handling mode transitions
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_common.h"
#include "display.h"
#include "led_ctrl.h"
#include "buzzer.h"
#include "button.h"
#include "nvs_store.h"
#include "wifi_manager.h"
#include "ble_scanner.h"
#include "sniffer.h"
#include "web_server.h"
#include "flock_you.h"
#include "fox_hunter.h"
#include "sky_spy.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "main";

#define LED_PHASE_CORE_READY   255, 255, 255
#define LED_PHASE_BOOT_SPLASH  108,  99, 255
#define LED_PHASE_WIFI_INIT    255, 180,   0
#define LED_PHASE_BLE_INIT       0,  64, 255
#define LED_PHASE_SNIFFER_INIT   0, 220, 220
#define LED_PHASE_MODE_START   255,   0, 180
#define LED_PHASE_MAIN_LOOP      0, 255,   0

/* ── Mode-select display task handle ── */
static TaskHandle_t s_select_task = NULL;
static TaskHandle_t s_reset_warn_task = NULL;

static void reset_warning_flash_task(void *arg)
{
    for (int i = 0; i < 3; i++) {
        led_ctrl_set_forced(255, 140, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_ctrl_off();
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    s_reset_warn_task = NULL;
    vTaskDelete(NULL);
}

static void set_init_phase_led(uint8_t r, uint8_t g, uint8_t b)
{
    led_ctrl_set_forced(r, g, b);
}

/* ── AP configuration per mode ── */
typedef struct {
    const char *ssid;
    const char *pass;
    uint8_t     channel;
    uint16_t    accent;   /* RGB565 accent color */
} mode_config_t;

static const mode_config_t MODE_CFG[MODE_COUNT] = {
    [MODE_SELECT]    = {"ouispy-c6",   "ouispy123",   6, 0x6A6F}, /* plum */
    [MODE_FLOCK_YOU] = {"flockyou-c6", "flockyou123", 6, 0x7928}, /* burgundy */
    [MODE_FOX_HUNTER]= {"foxhunt-c6",  "foxhunt123",  6, 0xA3C7}, /* bronze */
    [MODE_SKY_SPY]   = {"skyspy-c6",   "skyspy1234",  6, 0xD48C}, /* light tan */
};

/* ── Stop current mode ── */
static void stop_current_mode(void)
{
    switch (g_app.current_mode) {
    case MODE_FLOCK_YOU:  flock_you_stop(); break;
    case MODE_FOX_HUNTER: fox_hunter_stop(); break;
    case MODE_SKY_SPY:    sky_spy_stop(); break;
    case MODE_SELECT:
        /* The select task will exit on mode change by itself */
        break;
    default: break;
    }
}

static void render_mode_select_screen(int cursor)
{
    uint16_t bg          = rgb565(224, 224, 224);
    uint16_t panel_bg    = rgb565(246, 246, 244);
    uint16_t panel_alt   = rgb565(236, 236, 234);
    uint16_t accent      = rgb565(118, 118, 118);
    uint16_t accent_div  = rgb565(154, 154, 154);
    uint16_t border      = rgb565(170, 170, 166);
    uint16_t text_main   = rgb565(42, 42, 42);
    uint16_t text_dim    = rgb565(92, 92, 92);
    uint16_t selected_bg = rgb565(216, 216, 214);
    uint16_t selected_fg = rgb565(28, 28, 28);
    uint16_t text_link   = rgb565(80, 80, 80);

    struct {
        const char *label;
        const char *desc;
        uint16_t accent_col;
    } entries[] = {
        {"1: FLOCK YOU",  "ALPR scanner",   rgb565(126, 78, 82)},
        {"2: FOX HUNTER", "BLE tracker",    rgb565(142, 118, 88)},
        {"3: SKY SPY",    "Drone detector", rgb565(164, 150, 130)},
    };

    if (cursor < 0) cursor = 0;
    if (cursor > 2) cursor = 2;

    display_fill(bg);

    display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 32, accent);
    display_draw_rect(0, DISPLAY_STATUS_BAR_Y + 30, LCD_H_RES, 2, accent_div);
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 7, "OUI-SPY C6", 0xFFFF, accent);
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 19, "MODE SELECT", rgb565(240, 240, 238), accent);

    display_draw_bordered_rect(8, 46, LCD_H_RES - 16, 58, border, panel_bg);
    display_draw_text(14, 53, "WiFi:", text_dim, panel_bg);
    display_draw_text(44, 53, MODE_CFG[MODE_SELECT].ssid, text_main, panel_bg);
    display_draw_text(14, 67, "Pass:", text_dim, panel_bg);
    display_draw_text(44, 67, MODE_CFG[MODE_SELECT].pass, text_main, panel_bg);
    display_draw_text(14, 81, "Open:", text_dim, panel_bg);
    display_draw_text(44, 81, "192.168.4.1", text_link, panel_bg);

    display_draw_text_centered(116, "Select a mode", text_dim, bg);

    for (int i = 0; i < 3; i++) {
        int y = 132 + i * 34;
        bool selected = (i == cursor);
        uint16_t row_bg = selected ? selected_bg : panel_alt;
        uint16_t row_fg = selected ? selected_fg : text_main;
        uint16_t row_desc = selected ? rgb565(78, 78, 78) : text_dim;
        uint16_t row_border = selected ? entries[i].accent_col : border;

        display_draw_bordered_rect(8, y, LCD_H_RES - 16, 26, row_border, row_bg);
        display_draw_rect(12, y + 3, 5, 20, entries[i].accent_col);
        if (selected) {
            display_draw_text(LCD_H_RES - 34, y + 9, "GO", selected_fg, row_bg);
        }
        display_draw_text(24, y + 5, entries[i].label, row_fg, row_bg);
        display_draw_text(24, y + 15, entries[i].desc, row_desc, row_bg);
    }

    display_draw_hline(14, 248, LCD_H_RES - 28, border);
    display_draw_text_centered(258, "Click next   DblClk prev", text_main, bg);
    display_draw_text_centered(270, "Hold 1s select   Hold 7s reset", text_main, bg);
}

/* ── Mode Select display task — redraw only on cursor changes ── */
static void mode_select_task(void *arg)
{
    int last_cursor = -1;

    led_ctrl_set(40, 30, 80);

    while (g_app.current_mode == MODE_SELECT) {
        if (g_app.ui_cursor >= 3) g_app.ui_cursor = 2;
        if (g_app.ui_cursor < 0)  g_app.ui_cursor = 0;

        if (g_app.ui_cursor != last_cursor) {
            render_mode_select_screen(g_app.ui_cursor);
            last_cursor = g_app.ui_cursor;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    led_ctrl_off();
    s_select_task = NULL;
    vTaskDelete(NULL);
}

/* ── Start a mode ── */
static void start_mode(app_mode_t mode)
{
    ESP_LOGI(TAG, "Starting mode %d", mode);

    /* Restart WiFi AP with mode-specific SSID */
    esp_err_t err = wifi_manager_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_manager_stop returned %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    err = wifi_manager_start_ap(
        MODE_CFG[mode].ssid,
        MODE_CFG[mode].pass,
        MODE_CFG[mode].channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP for mode %d: %s", mode, esp_err_to_name(err));
    }

    /* Restart web server */
    web_server_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    web_server_start();

    g_app.current_mode = mode;
    nvs_store_save_mode(mode);

    /* Show mode on display */
    const char *names[] = {"SELECT", "FLOCK YOU", "FOX HUNTER", "SKY SPY"};
    display_fill(0x0000);
    display_draw_status(names[mode], MODE_CFG[mode].accent);

    switch (mode) {
    case MODE_FLOCK_YOU:
        g_app.ui_cursor     = 0;
        g_app.ui_item_count = g_app.device_count;
        flock_you_start();
        break;
    case MODE_FOX_HUNTER:
        g_app.ui_cursor     = 0;
        g_app.ui_item_count = 0;
        fox_hunter_start();
        break;
    case MODE_SKY_SPY:
        g_app.ui_cursor     = 0;
        g_app.ui_item_count = g_app.drone_count;
        sky_spy_start();
        break;
    case MODE_SELECT:
        g_app.ui_cursor     = 0;
        g_app.ui_item_count = 3; /* three selectable modes */
        if (xTaskCreate(mode_select_task, "sel_display", TASK_STACK_UI, NULL, 3,
                        &s_select_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create mode select task");
            s_select_task = NULL;
        }
        break;
    case MODE_COUNT:
    default:
        /* Sentinel/unknown mode: fall back to selector screen. */
        g_app.ui_cursor     = 0;
        g_app.ui_item_count = 3;
        g_app.current_mode  = MODE_SELECT;
        break;
    }
}

/* ── Button callbacks ── */
static void on_button_event(button_id_t btn, button_event_t evt)
{
    switch (evt) {
    case BTN_EVT_CLICK:
        /* Navigate forward */
        if (g_app.ui_item_count > 0) {
            g_app.ui_cursor = (g_app.ui_cursor + 1) % g_app.ui_item_count;
        }
        buzzer_beep(20);
        break;

    case BTN_EVT_DOUBLE_CLICK:
        /* Navigate backward */
        if (g_app.ui_item_count > 0) {
            g_app.ui_cursor = (g_app.ui_cursor - 1 + g_app.ui_item_count)
                              % g_app.ui_item_count;
        }
        buzzer_beep(20);
        break;

    case BTN_EVT_HOLD:
        /* Select / activate current item */
        buzzer_tone(1000, 80);
        if (g_app.current_mode == MODE_SELECT) {
            /* Map cursor 0-2 to the three operational modes */
            static const app_mode_t sel_modes[] = {
                MODE_FLOCK_YOU, MODE_FOX_HUNTER, MODE_SKY_SPY
            };
            if (g_app.ui_cursor >= 0 && g_app.ui_cursor < 3) {
                g_app.requested_mode = sel_modes[g_app.ui_cursor];
                g_app.mode_change_pending = true;
            }
        } else if (g_app.current_mode == MODE_FLOCK_YOU) {
            /* Set highlighted device as Fox Hunter target */
            if (g_app.device_count > 0 && g_app.ui_cursor < g_app.device_count) {
                fox_hunter_set_target_from_flock(g_app.ui_cursor);
                buzzer_tone(1200, 120);
            }
        }
        /* Fox Hunter / Sky Spy: no item selection needed */
        break;

    case BTN_EVT_LONG_HOLD_WARN:
        if (g_app.current_mode != MODE_SELECT && s_reset_warn_task == NULL) {
            if (xTaskCreate(reset_warning_flash_task, "reset_warn", 2048, NULL, 3, &s_reset_warn_task) != pdPASS) {
                s_reset_warn_task = NULL;
            }
        }
        break;

    case BTN_EVT_LONG_HOLD:
        /* 7-second hold: always return to mode select */
        buzzer_tone(600, 300);
        g_app.requested_mode = MODE_SELECT;
        g_app.mode_change_pending = true;
        break;
    }
}

/* ── Main task ── */
void app_main(void)
{
    /* ── Phase 1: Core init ── */
    app_state_init();
    nvs_store_init();
    nvs_store_load_prefs();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  OUI-Spy C6 — RF Intelligence Tool");
    ESP_LOGI(TAG, "  Waveshare ESP32-C6-LCD-1.47");
    ESP_LOGI(TAG, "========================================");

    /* ── Phase 2: Hardware init ── */
    ESP_LOGI(TAG, "Phase 2: hardware init");
    display_init();
    led_ctrl_init();
    set_init_phase_led(LED_PHASE_CORE_READY);
    buzzer_init();
    button_init(on_button_event);

    /* Boot splash */
    display_fill(rgb565(232, 226, 217));
    display_draw_text_scaled_centered(100, "OUI-SPY", rgb565(111, 81, 123), rgb565(232, 226, 217), 2);
    display_draw_text_scaled_centered(126, "C6", rgb565(72, 58, 74), rgb565(232, 226, 217), 2);
    display_draw_text_centered(160, "Initializing...", rgb565(117, 104, 103), rgb565(232, 226, 217));
    buzzer_melody_boot();
    set_init_phase_led(LED_PHASE_BOOT_SPLASH);
    vTaskDelay(pdMS_TO_TICKS(1000));
    led_ctrl_off();

    /* ── Phase 3: Radio init ── */
    ESP_LOGI(TAG, "Phase 3: WiFi init");
    set_init_phase_led(LED_PHASE_WIFI_INIT);
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        led_ctrl_set_forced(255, 0, 0);
    }

    ESP_LOGI(TAG, "Phase 3: BLE init");
    set_init_phase_led(LED_PHASE_BLE_INIT);
    ble_scanner_init();

    ESP_LOGI(TAG, "Phase 3: sniffer init");
    set_init_phase_led(LED_PHASE_SNIFFER_INIT);
    sniffer_init();

    /* ── Phase 4: Start initial mode ── */
    app_mode_t saved_mode = nvs_store_load_mode();
    ESP_LOGI(TAG, "Phase 4: starting saved mode %d", saved_mode);
    set_init_phase_led(LED_PHASE_MODE_START);
    start_mode(saved_mode);

    /* ── Phase 5: Main loop ── */
    ESP_LOGI(TAG, "Entering main loop");
    set_init_phase_led(LED_PHASE_MAIN_LOOP);
    vTaskDelay(pdMS_TO_TICKS(150));
    while (1) {
        /* Handle mode transitions */
        if (g_app.mode_change_pending) {
            g_app.mode_change_pending = false;
            if (g_app.requested_mode != g_app.current_mode) {
                stop_current_mode();
                vTaskDelay(pdMS_TO_TICKS(200));
                start_mode(g_app.requested_mode);
            }
        }

        /* Update runtime stats */
        g_app.uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        g_app.free_heap  = esp_get_free_heap_size();

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
