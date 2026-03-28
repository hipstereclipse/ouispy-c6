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

#define LED_PHASE_CORE_READY    80,  78,  72
#define LED_PHASE_BOOT_SPLASH   40,  55,  38
#define LED_PHASE_WIFI_INIT     60,  45,  10
#define LED_PHASE_BLE_INIT      15,  35,  65
#define LED_PHASE_SNIFFER_INIT  15,  50,  55
#define LED_PHASE_MODE_START    30,  42,  28
#define LED_PHASE_MAIN_LOOP     40,  55,  38

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
    [MODE_SELECT]    = {"ouispy-c6",   "ouispy123",   6, RGB565_CONST(88, 28, 135)},  /* purple dim */
    [MODE_FLOCK_YOU] = {"flockyou-c6", "flockyou123", 6, RGB565_CONST(200, 60, 60)}, /* red dim    */
    [MODE_FOX_HUNTER]= {"foxhunt-c6",  "foxhunt123",  6, RGB565_CONST(200, 100, 20)},/* orange dim */
    [MODE_SKY_SPY]   = {"skyspy-c6",   "skyspy1234",  6, RGB565_CONST(20, 120, 40)}, /* navy green */
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
    /* Dark base with gold & purple accent */
    uint16_t bg          = rgb565(12, 10, 16);
    uint16_t card_bg     = rgb565(24, 20, 32);
    uint16_t card_alt    = rgb565(20, 18, 28);
    uint16_t gold        = rgb565(251, 191, 36);
    uint16_t gold_dim    = rgb565(120, 90, 18);
    uint16_t purple      = rgb565(168, 85, 247);
    uint16_t purple_dim  = rgb565(88, 28, 135);
    uint16_t border      = rgb565(55, 48, 70);
    uint16_t text_main   = rgb565(250, 250, 250);
    uint16_t text_dim    = rgb565(161, 161, 170);
    uint16_t selected_bg = rgb565(38, 30, 52);
    uint16_t text_link   = rgb565(196, 168, 255);

    struct {
        const char *label;
        const char *desc;
        uint16_t accent_col;
    } entries[] = {
        {"1: FLOCK YOU",  "ALPR scanner",   rgb565(248, 113, 113)},
        {"2: FOX HUNTER", "BLE tracker",    rgb565(251, 146, 60)},
        {"3: SKY SPY",    "Drone detector", rgb565(56, 189, 248)},
    };

    if (cursor < 0) cursor = 0;
    if (cursor > 2) cursor = 2;

    display_fill(bg);

    display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 32, purple_dim);
    display_draw_rect(0, DISPLAY_STATUS_BAR_Y + 30, LCD_H_RES, 2, gold);
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 7, "OUI-SPY C6", gold, purple_dim);
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 19, "MODE SELECT", rgb565(216, 180, 254), purple_dim);

    display_draw_bordered_rect(8, 46, LCD_H_RES - 16, 58, border, card_bg);
    display_draw_text(14, 53, "WiFi:", text_dim, card_bg);
    display_draw_text(44, 53, MODE_CFG[MODE_SELECT].ssid, text_main, card_bg);
    display_draw_text(14, 67, "Pass:", text_dim, card_bg);
    display_draw_text(44, 67, MODE_CFG[MODE_SELECT].pass, text_main, card_bg);
    display_draw_text(14, 81, "Open:", text_dim, card_bg);
    display_draw_text(44, 81, "192.168.4.1", text_link, card_bg);

    display_draw_text_centered(116, "Select a mode", text_dim, bg);

    for (int i = 0; i < 3; i++) {
        int y = 132 + i * 34;
        bool selected = (i == cursor);
        uint16_t row_bg = selected ? selected_bg : card_alt;
        uint16_t row_fg = selected ? rgb565(248, 245, 240) : text_main;
        uint16_t row_desc = selected ? rgb565(178, 175, 168) : text_dim;
        uint16_t row_border = selected ? entries[i].accent_col : border;

        display_draw_bordered_rect(8, y, LCD_H_RES - 16, 26, row_border, row_bg);
        display_draw_rect(12, y + 3, 5, 20, entries[i].accent_col);
        if (selected) {
            display_draw_text(LCD_H_RES - 34, y + 9, "GO", entries[i].accent_col, row_bg);
        }
        display_draw_text(24, y + 5, entries[i].label, row_fg, row_bg);
        display_draw_text(24, y + 15, entries[i].desc, row_desc, row_bg);
    }

    display_draw_hline(14, 248, LCD_H_RES - 28, purple_dim);
    display_draw_text_centered(258, "Click next   DblClk prev", text_dim, bg);
    display_draw_text_centered(270, "Hold .5s select  Hold 5s reset", text_dim, bg);
}

/* ── Mode Select display task — redraw only on cursor changes ── */
static void mode_select_task(void *arg)
{
    int last_cursor = -1;

    led_ctrl_set(0, 180, 0);

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
        if (g_app.current_mode == MODE_FOX_HUNTER && !g_app.fox_registry_open) {
            /* Open the target registry overlay */
            g_app.fox_registry_open = true;
            g_app.ui_cursor = 0;
            g_app.ui_item_count = g_app.fox_registry_count;
            buzzer_tone(1200, 40);
            break;
        }
        /* Navigate backward */
        if (g_app.ui_item_count > 0) {
            g_app.ui_cursor = (g_app.ui_cursor - 1 + g_app.ui_item_count)
                              % g_app.ui_item_count;
        }
        buzzer_beep(20);
        break;

    case BTN_EVT_TRIPLE_CLICK:
        if (g_app.current_mode == MODE_FOX_HUNTER && g_app.fox_registry_open) {
            /* Close the registry overlay */
            g_app.fox_registry_open = false;
            g_app.ui_cursor = 0;
            g_app.ui_item_count = 0;
            buzzer_tone(800, 40);
        }
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
        } else if (g_app.current_mode == MODE_FOX_HUNTER) {
            if (g_app.fox_registry_open) {
                /* Select target from registry */
                if (g_app.fox_registry_count > 0 && g_app.ui_cursor < g_app.fox_registry_count) {
                    fox_hunter_registry_select(g_app.ui_cursor);
                    buzzer_tone(1400, 120);
                }
            } else {
                /* Toggle LED mode: Detector ↔ Sting */
                g_app.fox_led_mode = (g_app.fox_led_mode + 1) % 2;
                buzzer_tone(g_app.fox_led_mode ? 1400 : 900, 60);
            }
        }
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
    nvs_store_load_fox_registry();

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

    /* Boot splash -- gold & purple */
    display_fill(rgb565(12, 10, 16));
    display_draw_text_scaled_centered(80, "OUI-SPY", rgb565(251, 191, 36), rgb565(12, 10, 16), 3);
    display_draw_text_scaled_centered(110, "C6", rgb565(168, 85, 247), rgb565(12, 10, 16), 3);
    display_draw_hline(30, 145, LCD_H_RES - 60, rgb565(63, 63, 70));
    display_draw_text_centered(158, "RF Intelligence Tool", rgb565(161, 161, 170), rgb565(12, 10, 16));
    display_draw_text_centered(175, "Initializing...", rgb565(113, 113, 122), rgb565(12, 10, 16));
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
    vTaskDelay(pdMS_TO_TICKS(100));
    led_ctrl_off();
    start_mode(saved_mode);

    /* ── Phase 5: Main loop ── */
    ESP_LOGI(TAG, "Entering main loop");
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
