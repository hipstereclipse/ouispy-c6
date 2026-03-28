/*
 * OUI-Spy C6 — Main entry point
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_common.h"
#include "display.h"
#include "led_ctrl.h"
#include "buzzer.h"
#include "button.h"
#include "nvs_store.h"
#include "storage_ext.h"
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
#define UI_GPS_READY_TIMEOUT_MS 8000

enum {
    SET_ITEM_AP_BROADCAST = 0,
    SET_ITEM_SINGLE_AP_NAME,
    SET_ITEM_SLEEP_TIMEOUT,
    SET_ITEM_MENU_LED,
    SET_ITEM_SOUND_FLOCK,
    SET_ITEM_SOUND_FOX,
    SET_ITEM_SOUND_SKY,
    SET_ITEM_SHORTCUT_MODE,
    SET_ITEM_SHORTCUT_ACTION,
    SET_ITEM_SHORTCUT_BACK,
    SET_ITEM_SD_LOGS,
    SET_ITEM_SD_FORMAT,
    SET_ITEM_BACK,
    SET_ITEM_COUNT,
};

/* ── Display task handles ── */
static TaskHandle_t s_select_task = NULL;
static TaskHandle_t s_settings_task = NULL;
static TaskHandle_t s_reset_warn_task = NULL;

/* ── AP configuration per mode ── */
typedef struct {
    const char *ssid;
    const char *pass;
    uint8_t     channel;
    uint16_t    accent;
} mode_config_t;

static const mode_config_t MODE_CFG[MODE_COUNT] = {
    [MODE_SELECT]     = {"ouispy-c6",   "ouispy123",   6, RGB565_CONST(88, 28, 135)},
    [MODE_FLOCK_YOU]  = {"flockyou-c6", "flockyou123", 6, RGB565_CONST(200, 60, 60)},
    [MODE_FOX_HUNTER] = {"foxhunt-c6",  "foxhunt123",  6, RGB565_CONST(200, 100, 20)},
    [MODE_SKY_SPY]    = {"skyspy-c6",   "skyspy1234",  6, RGB565_CONST(20, 120, 40)},
    [MODE_SETTINGS]   = {"ouispy-c6",   "ouispy123",   6, RGB565_CONST(88, 28, 135)},
};

static void mode_ap_credentials(app_mode_t mode, const char **ssid, const char **pass, uint8_t *channel)
{
    app_mode_ap_credentials(mode, ssid, pass, channel);
}

static const uint16_t SLEEP_OPTIONS[] = {0, 15, 30, 60, 120, 300};

static bool ui_gps_tag_active(void)
{
    uint32_t now_ms = uptime_ms();
    bool gps_ready_fresh = g_app.gps_client_ready
                        && (now_ms > g_app.gps_client_ready_ms)
                        && ((now_ms - g_app.gps_client_ready_ms) <= UI_GPS_READY_TIMEOUT_MS);
    return g_app.gps_tagging_enabled && gps_ready_fresh && (g_app.wifi_clients > 0);
}

static void set_init_phase_led(uint8_t r, uint8_t g, uint8_t b)
{
    led_ctrl_set_forced(r, g, b);
}

static void get_menu_led_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (idx) {
    case MENU_LED_MINT:    *r = 35;  *g = 220; *b = 160; break;
    case MENU_LED_SKY:     *r = 50;  *g = 140; *b = 255; break;
    case MENU_LED_AMBER:   *r = 250; *g = 145; *b = 10;  break;
    case MENU_LED_MAGENTA: *r = 220; *g = 70;  *b = 200; break;
    case MENU_LED_RUBY:    *r = 255; *g = 45;  *b = 95;  break;
    case MENU_LED_LIME:    *r = 115; *g = 255; *b = 25;  break;
    case MENU_LED_ICE:     *r = 120; *g = 235; *b = 255; break;
    case MENU_LED_WHITE:   *r = 245; *g = 245; *b = 245; break;
    case MENU_LED_DEEP_PURPLE: *r = 150; *g = 70; *b = 255; break;
    case MENU_LED_GOLD:
    default:               *r = 255; *g = 190; *b = 36;  break;
    }
}

static const char *menu_led_name(uint8_t idx)
{
    switch (idx) {
    case MENU_LED_MINT: return "MINT";
    case MENU_LED_SKY: return "SKY";
    case MENU_LED_AMBER: return "AMBER";
    case MENU_LED_MAGENTA: return "MAGENTA";
    case MENU_LED_RUBY: return "RUBY";
    case MENU_LED_LIME: return "LIME";
    case MENU_LED_ICE: return "ICE";
    case MENU_LED_WHITE: return "WHITE";
    case MENU_LED_DEEP_PURPLE: return "PURPLE";
    case MENU_LED_GOLD:
    default: return "GOLD";
    }
}

static const char *sound_profile_name(uint8_t idx)
{
    switch (idx) {
    case SOUND_PROFILE_CHIRP: return "CHIRP";
    case SOUND_PROFILE_SONAR: return "SONAR";
    case SOUND_PROFILE_RETRO: return "RETRO";
    case SOUND_PROFILE_ALARM: return "ALARM";
    case SOUND_PROFILE_STANDARD:
    default: return "STANDARD";
    }
}

static const char *shortcut_name(uint8_t idx)
{
    switch (idx) {
    case SHORTCUT_NEXT_MODE: return "NEXT MODE";
    case SHORTCUT_MODE_SELECT: return "MODE SELECT";
    case SHORTCUT_SETTINGS: return "SETTINGS";
    case SHORTCUT_TOGGLE_SOUND: return "TOGGLE SOUND";
    case SHORTCUT_TOGGLE_LED: return "TOGGLE LED";
    case SHORTCUT_FOX_LED_MODE: return "FOX LED MODE";
    case SHORTCUT_NONE:
    default: return "NONE";
    }
}

static uint8_t mode_sound_profile(app_mode_t mode)
{
    switch (mode) {
    case MODE_FLOCK_YOU: return g_app.sound_profile_flock;
    case MODE_FOX_HUNTER: return g_app.sound_profile_fox;
    case MODE_SKY_SPY: return g_app.sound_profile_sky;
    default: return SOUND_PROFILE_STANDARD;
    }
}

static void wake_display_on_input(void)
{
    g_app.last_input_ms = uptime_ms();
    if (g_app.display_sleeping) {
        g_app.display_sleeping = false;
        display_set_brightness(g_app.lcd_brightness);
    }
}

static void restart_ap_for_mode(app_mode_t mode)
{
    if (mode < 0 || mode >= MODE_COUNT) return;
    const char *ssid = NULL;
    const char *pass = NULL;
    uint8_t channel = 6;
    mode_ap_credentials(mode, &ssid, &pass, &channel);

    esp_err_t err = wifi_manager_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_manager_stop returned %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    err = wifi_manager_start_ap(ssid, pass, channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP for mode %d: %s", mode, esp_err_to_name(err));
    }
}

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

static void stop_current_mode(void)
{
    led_ctrl_breathe_stop();
    switch (g_app.current_mode) {
    case MODE_FLOCK_YOU:  flock_you_stop(); break;
    case MODE_FOX_HUNTER: fox_hunter_stop(); break;
    case MODE_SKY_SPY:    sky_spy_stop(); break;
    case MODE_SELECT:
    case MODE_SETTINGS:
    default:
        break;
    }
}

static void render_mode_select_screen(int cursor)
{
    uint16_t bg          = rgb565(12, 10, 16);
    uint16_t card_bg     = rgb565(24, 20, 32);
    uint16_t card_alt    = rgb565(20, 18, 28);
    uint16_t gold        = rgb565(251, 191, 36);
    uint16_t purple_dim  = rgb565(88, 28, 135);
    uint16_t border      = rgb565(55, 48, 70);
    uint16_t text_main   = rgb565(250, 250, 250);
    uint16_t text_dim    = rgb565(161, 161, 170);
    uint16_t selected_bg = rgb565(38, 30, 52);
    uint16_t text_link   = rgb565(196, 168, 255);
    bool gps_tag_active  = ui_gps_tag_active();

    struct {
        const char *label;
        const char *desc;
        uint16_t accent_col;
    } entries[] = {
        {"1: FLOCK YOU",  "ALPR scanner",   rgb565(248, 113, 113)},
        {"2: FOX HUNTER", "BLE tracker",    rgb565(251, 146, 60)},
        {"3: SKY SPY",    "Drone detector", rgb565(56, 189, 248)},
        {"4: SETTINGS",   "Customize unit", rgb565(168, 85, 247)},
    };

    if (cursor < 0) cursor = 0;
    if (cursor > 3) cursor = 3;

    display_fill(bg);

    display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 32, purple_dim);
    display_draw_rect(0, DISPLAY_STATUS_BAR_Y + 30, LCD_H_RES, 2, gold);
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 7, "OUI-SPY C6", gold, purple_dim);
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 19, "MAIN MENU", rgb565(216, 180, 254), purple_dim);

    display_draw_bordered_rect(8, 46, LCD_H_RES - 16, 58, border, card_bg);
    display_draw_text(14, 53, "WiFi:", text_dim, card_bg);
    const char *ap_ssid = MODE_CFG[MODE_SELECT].ssid;
    const char *ap_pass = MODE_CFG[MODE_SELECT].pass;
    uint8_t ap_channel = MODE_CFG[MODE_SELECT].channel;
    mode_ap_credentials(g_app.current_mode, &ap_ssid, &ap_pass, &ap_channel);

    display_draw_text(44, 53, ap_ssid, text_main, card_bg);
    display_draw_text(14, 67, "Pass:", text_dim, card_bg);
    display_draw_text(44, 67, ap_pass, text_main, card_bg);
    display_draw_text(14, 81, g_app.ap_broadcast_enabled ? "AP:":"AP:", text_dim, card_bg);
    display_draw_text(44, 81, g_app.ap_broadcast_enabled ? "Broadcast" : "Hidden", text_link, card_bg);
    display_draw_text(14, 93, "GPS:", text_dim, card_bg);
    display_draw_text(44, 93,
                      gps_tag_active ? "TAG ON" : "TAG OFF",
                      gps_tag_active ? rgb565(74, 222, 128) : rgb565(248, 113, 113),
                      card_bg);

    display_draw_text_centered(116, "Select a mode", text_dim, bg);

    for (int i = 0; i < 4; i++) {
        int y = 132 + i * 28;
        bool selected = (i == cursor);
        uint16_t row_bg = selected ? selected_bg : card_alt;
        uint16_t row_fg = selected ? rgb565(248, 245, 240) : text_main;
        uint16_t row_desc = selected ? rgb565(178, 175, 168) : text_dim;
        uint16_t row_border = selected ? entries[i].accent_col : border;

        display_draw_bordered_rect(8, y, LCD_H_RES - 16, 24, row_border, row_bg);
        display_draw_rect(12, y + 3, 5, 18, entries[i].accent_col);
        if (selected) {
            display_draw_text(LCD_H_RES - 34, y + 8, "GO", entries[i].accent_col, row_bg);
        }
        display_draw_text(24, y + 4, entries[i].label, row_fg, row_bg);
        display_draw_text(24, y + 14, entries[i].desc, row_desc, row_bg);
    }

    /* microSD card status - bottom of display */
    storage_status_t microsd_status = storage_ext_get_status();
    uint16_t microsd_color = microsd_status == STORAGE_STATUS_AVAILABLE
                                 ? rgb565(52, 211, 153)
                                 : (microsd_status == STORAGE_STATUS_NEEDS_FORMAT ? rgb565(251, 191, 36)
                                                                                   : rgb565(249, 115, 22));
    const char *microsd_text = microsd_status == STORAGE_STATUS_AVAILABLE
                                   ? "microSD: Available"
                                   : (microsd_status == STORAGE_STATUS_NEEDS_FORMAT ? "microSD: Needs format"
                                                                                    : "microSD: Not found");
    display_draw_text_centered(240, microsd_text, microsd_color, bg);

    display_draw_text_centered(262, "Click next   DblClk prev", text_dim, bg);
    display_draw_text_centered(274, "Hold select  Hold 5s reset", text_dim, bg);
}

static void render_settings_screen(int cursor)
{
    uint16_t bg = rgb565(10, 11, 16);
    uint16_t card = rgb565(20, 21, 30);
    uint16_t border = rgb565(55, 48, 70);
    uint16_t accent = rgb565(168, 85, 247);
    uint16_t text_main = rgb565(246, 246, 250);
    uint16_t text_dim = rgb565(161, 161, 170);
    bool gps_tag_active = ui_gps_tag_active();

    char val[28];
    if (cursor < 0) cursor = 0;
    if (cursor >= SET_ITEM_COUNT) cursor = SET_ITEM_COUNT - 1;

    display_fill(bg);
    display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 32, rgb565(38, 24, 54));
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 8, "SETTINGS", rgb565(235, 220, 255), rgb565(38, 24, 54));
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 19, "Click=next DblClk=prev Hold=change", text_dim, rgb565(38, 24, 54));
    display_draw_text(120, DISPLAY_STATUS_BAR_Y + 8, "GPS", text_dim, rgb565(38, 24, 54));
    display_draw_text(140, DISPLAY_STATUS_BAR_Y + 8,
                      gps_tag_active ? "ON" : "OFF",
                      gps_tag_active ? rgb565(74, 222, 128) : rgb565(248, 113, 113),
                      rgb565(38, 24, 54));

    const char *labels[SET_ITEM_COUNT] = {
        "AP Broadcast",
        "Single AP Name",
        "Sleep Timeout",
        "Menu LED",
        "Sound: Flock",
        "Sound: Fox",
        "Sound: Sky",
        "Shortcut BTN10",
        "Shortcut BTN11",
        "Shortcut BTN19",
        "microSD Logs",
        "Format microSD",
        "Back to Menu",
    };

    int start = cursor - 3;
    if (start < 0) start = 0;
    if (start > SET_ITEM_COUNT - 7) start = SET_ITEM_COUNT - 7;
    if (start < 0) start = 0;

    for (int i = 0; i < 7; i++) {
        int idx = start + i;
        if (idx >= SET_ITEM_COUNT) break;
        int y = 42 + i * 30;
        bool sel = (idx == cursor);

        display_draw_bordered_rect(6, y, LCD_H_RES - 12, 26,
                                   sel ? accent : border,
                                   sel ? rgb565(32, 26, 44) : card);
        display_draw_text(12, y + 5, labels[idx], sel ? text_main : text_dim, sel ? rgb565(32, 26, 44) : card);

        switch (idx) {
        case SET_ITEM_AP_BROADCAST:
            snprintf(val, sizeof(val), "%s", g_app.ap_broadcast_enabled ? "ON" : "OFF");
            break;
        case SET_ITEM_SLEEP_TIMEOUT:
            snprintf(val, sizeof(val), "%s", g_app.display_sleep_timeout_sec == 0 ? "OFF" : "SECONDS");
            break;
        case SET_ITEM_SINGLE_AP_NAME:
            snprintf(val, sizeof(val), "%s", g_app.single_ap_name_enabled ? "UNISPY" : "PER MODE");
            break;
        case SET_ITEM_MENU_LED:
            snprintf(val, sizeof(val), "%s", menu_led_name(g_app.menu_led_color));
            break;
        case SET_ITEM_SOUND_FLOCK:
            snprintf(val, sizeof(val), "%s", sound_profile_name(g_app.sound_profile_flock));
            break;
        case SET_ITEM_SOUND_FOX:
            snprintf(val, sizeof(val), "%s", sound_profile_name(g_app.sound_profile_fox));
            break;
        case SET_ITEM_SOUND_SKY:
            snprintf(val, sizeof(val), "%s", sound_profile_name(g_app.sound_profile_sky));
            break;
        case SET_ITEM_SHORTCUT_MODE:
            snprintf(val, sizeof(val), "%s", shortcut_name(g_app.shortcut_mode_btn));
            break;
        case SET_ITEM_SHORTCUT_ACTION:
            snprintf(val, sizeof(val), "%s", shortcut_name(g_app.shortcut_action_btn));
            break;
        case SET_ITEM_SHORTCUT_BACK:
            snprintf(val, sizeof(val), "%s", shortcut_name(g_app.shortcut_back_btn));
            break;
        case SET_ITEM_SD_LOGS:
            snprintf(val, sizeof(val), "%s", g_app.use_microsd_logs ? "PREFER SD" : "INTERNAL");
            break;
        case SET_ITEM_SD_FORMAT:
            snprintf(val, sizeof(val), "%s", storage_ext_status_str(storage_ext_get_status()));
            break;
        case SET_ITEM_BACK:
        default:
            snprintf(val, sizeof(val), "%s", "HOLD");
            break;
        }

        int value_x = LCD_H_RES - 10 - display_text_width(val);
        if (value_x < 92) {
            value_x = 92;
        }
        display_draw_text(value_x, y + 15, val,
                          sel ? rgb565(196, 168, 255) : text_dim,
                          sel ? rgb565(32, 26, 44) : card);
    }

    display_draw_text_centered(272, "Triple-click: Exit settings", text_dim, bg);

    if (g_app.display_sleep_timeout_sec > 0) {
        snprintf(val, sizeof(val), "%us", (unsigned)g_app.display_sleep_timeout_sec);
        display_draw_text_centered(286, val, text_dim, bg);
    } else {
        display_draw_text_centered(286, "Sleep disabled", text_dim, bg);
    }
}

static void render_boot_splash(void)
{
    uint16_t bg = rgb565(12, 10, 16);
    uint16_t panel = rgb565(24, 20, 32);
    uint16_t border = rgb565(63, 63, 70);
    uint16_t gold = rgb565(251, 191, 36);
    uint16_t violet = rgb565(168, 85, 247);
    uint16_t text_dim = rgb565(161, 161, 170);
    uint16_t text_faint = rgb565(113, 113, 122);

    display_fill(bg);

    /* Framed boot card keeps the logo centered and avoids clipping artifacts. */
    display_draw_bordered_rect(8, 44, LCD_H_RES - 16, 128, border, panel);
    display_draw_rect(8, 44, LCD_H_RES - 16, 3, gold);

    display_draw_text_centered(64, "OUI-SPY", gold, panel);
    display_draw_text_scaled_centered(78, "C6", violet, panel, 2);
    display_draw_text_centered(102, "RF INTELLIGENCE TOOL", text_dim, panel);
    display_draw_hline(20, 120, LCD_H_RES - 40, border);
    display_draw_text_centered(132, "ESP32-C6 EDITION", text_dim, panel);
    display_draw_text_centered(184, "Initializing...", text_faint, bg);
}

static void mode_select_task(void *arg)
{
    int last_cursor = -1;
    uint8_t r, g, b;
    get_menu_led_rgb(g_app.menu_led_color, &r, &g, &b);
    led_ctrl_breathe(r, g, b, 2200);

    while (g_app.current_mode == MODE_SELECT) {
        if (g_app.ui_cursor >= 4) g_app.ui_cursor = 3;
        if (g_app.ui_cursor < 0) g_app.ui_cursor = 0;

        if (g_app.ui_cursor != last_cursor) {
            render_mode_select_screen(g_app.ui_cursor);
            last_cursor = g_app.ui_cursor;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    led_ctrl_breathe_stop();
    led_ctrl_off();
    s_select_task = NULL;
    vTaskDelete(NULL);
}

static void settings_task(void *arg)
{
    int last_cursor = -1;
    while (g_app.current_mode == MODE_SETTINGS) {
        if (g_app.ui_cursor >= SET_ITEM_COUNT) g_app.ui_cursor = SET_ITEM_COUNT - 1;
        if (g_app.ui_cursor < 0) g_app.ui_cursor = 0;
        if (g_app.ui_cursor != last_cursor) {
            render_settings_screen(g_app.ui_cursor);
            last_cursor = g_app.ui_cursor;
        }
        vTaskDelay(pdMS_TO_TICKS(90));
    }
    s_settings_task = NULL;
    vTaskDelete(NULL);
}

static void start_mode(app_mode_t mode)
{
    const char *names[] = {"SELECT", "FLOCK YOU", "FOX HUNTER", "SKY SPY", "SETTINGS"};

    ESP_LOGI(TAG, "Starting mode %d", mode);
    g_app.current_mode = mode;
    g_app.last_input_ms = uptime_ms();
    g_app.display_sleeping = false;
    display_set_brightness(g_app.lcd_brightness);

    restart_ap_for_mode(mode);

    web_server_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    web_server_start();

    nvs_store_save_mode(mode);
    storage_ext_append_log("mode", names[mode]);
    display_fill(0x0000);
    display_draw_status(names[mode], MODE_CFG[mode].accent);

    switch (mode) {
    case MODE_FLOCK_YOU:
        g_app.ui_cursor = 0;
        g_app.ui_item_count = g_app.device_count;
        flock_you_start();
        buzzer_play_profile(mode_sound_profile(mode));
        break;

    case MODE_FOX_HUNTER:
        g_app.ui_cursor = 0;
        g_app.ui_item_count = 0;
        fox_hunter_start();
        buzzer_play_profile(mode_sound_profile(mode));
        break;

    case MODE_SKY_SPY:
        g_app.ui_cursor = 0;
        g_app.ui_item_count = g_app.drone_count;
        sky_spy_start();
        buzzer_play_profile(mode_sound_profile(mode));
        break;

    case MODE_SETTINGS:
        g_app.ui_cursor = 0;
        g_app.ui_item_count = SET_ITEM_COUNT;
        if (xTaskCreate(settings_task, "settings_ui", TASK_STACK_UI, NULL, 3, &s_settings_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create settings task");
            s_settings_task = NULL;
        }
        break;

    case MODE_SELECT:
        g_app.ui_cursor = 0;
        g_app.ui_item_count = 4;
        if (xTaskCreate(mode_select_task, "sel_display", TASK_STACK_UI, NULL, 3, &s_select_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create mode select task");
            s_select_task = NULL;
        }
        break;

    case MODE_COUNT:
    default:
        g_app.current_mode = MODE_SELECT;
        g_app.ui_cursor = 0;
        g_app.ui_item_count = 4;
        break;
    }
}

static bool execute_shortcut(shortcut_action_t action)
{
    static const app_mode_t op_modes[] = {MODE_FLOCK_YOU, MODE_FOX_HUNTER, MODE_SKY_SPY};

    switch (action) {
    case SHORTCUT_NEXT_MODE: {
        for (int i = 0; i < 3; i++) {
            if (g_app.current_mode == op_modes[i]) {
                g_app.requested_mode = op_modes[(i + 1) % 3];
                g_app.mode_change_pending = true;
                buzzer_tone(1200, 60);
                return true;
            }
        }
        return false;
    }
    case SHORTCUT_MODE_SELECT:
        g_app.requested_mode = MODE_SELECT;
        g_app.mode_change_pending = true;
        buzzer_tone(800, 60);
        return true;

    case SHORTCUT_SETTINGS:
        g_app.requested_mode = MODE_SETTINGS;
        g_app.mode_change_pending = true;
        buzzer_tone(1000, 60);
        return true;

    case SHORTCUT_TOGGLE_SOUND:
        g_app.sound_enabled = !g_app.sound_enabled;
        nvs_store_save_prefs();
        if (g_app.sound_enabled) buzzer_beep(50);
        return true;

    case SHORTCUT_TOGGLE_LED:
        g_app.led_enabled = !g_app.led_enabled;
        nvs_store_save_prefs();
        if (!g_app.led_enabled) led_ctrl_off();
        return true;

    case SHORTCUT_FOX_LED_MODE:
        if (g_app.current_mode == MODE_FOX_HUNTER) {
            g_app.fox_led_mode = (g_app.fox_led_mode + 1) % 2;
            buzzer_tone(g_app.fox_led_mode ? 1400 : 900, 60);
            return true;
        }
        return false;

    case SHORTCUT_NONE:
    default:
        return false;
    }
}

static uint8_t *shortcut_ref_for_button(button_id_t btn)
{
    if (btn == BTN_MODE) return &g_app.shortcut_mode_btn;
    if (btn == BTN_ACTION) return &g_app.shortcut_action_btn;
    if (btn == BTN_BACK) return &g_app.shortcut_back_btn;
    return NULL;
}

static void apply_settings_item_action(void)
{
    const char *log_msg = NULL;

    switch (g_app.ui_cursor) {
    case SET_ITEM_AP_BROADCAST:
        g_app.ap_broadcast_enabled = !g_app.ap_broadcast_enabled;
        restart_ap_for_mode(g_app.current_mode);
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = g_app.ap_broadcast_enabled ? "ap_broadcast_on" : "ap_broadcast_off";
        break;

    case SET_ITEM_SINGLE_AP_NAME:
        g_app.single_ap_name_enabled = !g_app.single_ap_name_enabled;
        restart_ap_for_mode(g_app.current_mode);
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = g_app.single_ap_name_enabled ? "ap_name_unified" : "ap_name_per_mode";
        break;

    case SET_ITEM_SLEEP_TIMEOUT: {
        int idx = 0;
        for (int i = 0; i < (int)(sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0])); i++) {
            if (SLEEP_OPTIONS[i] == g_app.display_sleep_timeout_sec) {
                idx = i;
                break;
            }
        }
        idx = (idx + 1) % (int)(sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]));
        g_app.display_sleep_timeout_sec = SLEEP_OPTIONS[idx];
        buzzer_play_profile(SOUND_PROFILE_STANDARD);
        log_msg = "display_sleep_updated";
        break;
    }

    case SET_ITEM_MENU_LED:
        g_app.menu_led_color = (g_app.menu_led_color + 1) % MENU_LED_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "menu_led_updated";
        break;

    case SET_ITEM_SOUND_FLOCK:
        g_app.sound_profile_flock = (g_app.sound_profile_flock + 1) % SOUND_PROFILE_COUNT;
        buzzer_play_profile(g_app.sound_profile_flock);
        log_msg = "sound_flock_updated";
        break;

    case SET_ITEM_SOUND_FOX:
        g_app.sound_profile_fox = (g_app.sound_profile_fox + 1) % SOUND_PROFILE_COUNT;
        buzzer_play_profile(g_app.sound_profile_fox);
        log_msg = "sound_fox_updated";
        break;

    case SET_ITEM_SOUND_SKY:
        g_app.sound_profile_sky = (g_app.sound_profile_sky + 1) % SOUND_PROFILE_COUNT;
        buzzer_play_profile(g_app.sound_profile_sky);
        log_msg = "sound_sky_updated";
        break;

    case SET_ITEM_SHORTCUT_MODE:
        g_app.shortcut_mode_btn = (g_app.shortcut_mode_btn + 1) % SHORTCUT_COUNT;
        buzzer_beep(40);
        log_msg = "shortcut_btn10_updated";
        break;

    case SET_ITEM_SHORTCUT_ACTION:
        g_app.shortcut_action_btn = (g_app.shortcut_action_btn + 1) % SHORTCUT_COUNT;
        buzzer_beep(40);
        log_msg = "shortcut_btn11_updated";
        break;

    case SET_ITEM_SHORTCUT_BACK:
        g_app.shortcut_back_btn = (g_app.shortcut_back_btn + 1) % SHORTCUT_COUNT;
        buzzer_beep(40);
        log_msg = "shortcut_btn19_updated";
        break;

    case SET_ITEM_SD_LOGS:
        g_app.use_microsd_logs = !g_app.use_microsd_logs;
        buzzer_play_profile(SOUND_PROFILE_RETRO);
        log_msg = g_app.use_microsd_logs ? "sd_logs_enabled" : "sd_logs_disabled";
        break;

    case SET_ITEM_SD_FORMAT: {
        storage_status_t status = storage_ext_get_status();
        if (status == STORAGE_STATUS_NOT_FOUND) {
            buzzer_tone(400, 100);  /* Error tone */
            log_msg = "sd_format_failed_no_card";
        } else if (status == STORAGE_STATUS_AVAILABLE) {
            buzzer_tone(400, 100);  /* Error tone */
            log_msg = "sd_already_formatted";
        } else {
            /* Format the card */
            esp_err_t err = storage_ext_format();
            if (err == ESP_OK) {
                buzzer_play_profile(SOUND_PROFILE_SONAR);
                log_msg = "sd_format_success";
            } else {
                buzzer_tone(300, 150);  /* Error tone */
                log_msg = "sd_format_failed";
            }
        }
        break;
    }

    case SET_ITEM_BACK:
        g_app.requested_mode = MODE_SELECT;
        g_app.mode_change_pending = true;
        buzzer_tone(900, 60);
        break;

    default:
        break;
    }

    nvs_store_save_prefs();
    if (log_msg) {
        storage_ext_append_log("settings", log_msg);
    }
    render_settings_screen(g_app.ui_cursor);
}

static void on_button_event(button_id_t btn, button_event_t evt)
{
    wake_display_on_input();

    if (evt == BTN_EVT_CLICK && g_app.current_mode != MODE_SELECT && g_app.current_mode != MODE_SETTINGS) {
        uint8_t *sc = shortcut_ref_for_button(btn);
        if (sc && execute_shortcut((shortcut_action_t)(*sc))) {
            return;
        }
    }

    switch (evt) {
    case BTN_EVT_QUINTUPLE_CLICK:
        if (g_app.current_mode == MODE_FLOCK_YOU) {
            int target_idx = -1;
            if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                /* Prefer currently selected flock camera, else strongest visible camera. */
                if (g_app.device_count > 0 && g_app.ui_cursor >= 0 && g_app.ui_cursor < g_app.device_count) {
                    ble_device_t *sel = &g_app.devices[g_app.ui_cursor];
                    if (sel->is_flock && !sel->is_raven) {
                        target_idx = g_app.ui_cursor;
                    }
                }
                if (target_idx < 0) {
                    int best_rssi = -127;
                    for (int i = 0; i < g_app.device_count; i++) {
                        if (g_app.devices[i].is_flock && !g_app.devices[i].is_raven && g_app.devices[i].rssi >= best_rssi) {
                            best_rssi = g_app.devices[i].rssi;
                            target_idx = i;
                        }
                    }
                }
                xSemaphoreGive(g_app.device_mutex);
            }

            if (target_idx >= 0) {
                fox_hunter_set_target_from_flock(target_idx);
                g_app.requested_mode = MODE_FOX_HUNTER;
                g_app.mode_change_pending = true;
                storage_ext_append_log("flock", "quintuple_click_track_in_fox");
                buzzer_tone(1500, 110);
            } else {
                buzzer_tone(520, 90);
            }
        } else if (g_app.current_mode == MODE_SKY_SPY) {
            int target_idx = -1;
            if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                /* Prefer selected drone row, else strongest currently visible drone. */
                if (g_app.drone_count > 0 && g_app.ui_cursor >= 0 && g_app.ui_cursor < g_app.drone_count) {
                    target_idx = g_app.ui_cursor;
                }
                if (target_idx < 0) {
                    int best_rssi = -127;
                    for (int i = 0; i < g_app.drone_count; i++) {
                        if (g_app.drones[i].rssi >= best_rssi) {
                            best_rssi = g_app.drones[i].rssi;
                            target_idx = i;
                        }
                    }
                }
                xSemaphoreGive(g_app.drone_mutex);
            }

            if (target_idx >= 0) {
                uint8_t target_mac[6];
                bool have_target = false;
                if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    if (target_idx < g_app.drone_count) {
                        memcpy(target_mac, g_app.drones[target_idx].mac, sizeof(target_mac));
                        have_target = true;
                    }
                    xSemaphoreGive(g_app.drone_mutex);
                }

                if (have_target) {
                    fox_hunter_set_target(target_mac);
                    g_app.requested_mode = MODE_FOX_HUNTER;
                    g_app.mode_change_pending = true;
                    storage_ext_append_log("sky", "quintuple_click_track_in_fox");
                    buzzer_tone(1500, 110);
                } else {
                    buzzer_tone(520, 90);
                }
            } else {
                buzzer_tone(520, 90);
            }
        }
        break;

    case BTN_EVT_CLICK:
        if (g_app.ui_item_count > 0) {
            g_app.ui_cursor = (g_app.ui_cursor + 1) % g_app.ui_item_count;
        }
        buzzer_beep(20);
        break;

    case BTN_EVT_DOUBLE_CLICK:
        if (g_app.current_mode == MODE_FOX_HUNTER) {
            if (!g_app.fox_registry_open) {
                g_app.fox_led_mode = (g_app.fox_led_mode + 1) % 2;
                buzzer_tone(g_app.fox_led_mode ? 1400 : 900, 60);
            } else if (g_app.ui_item_count > 0) {
                g_app.ui_cursor = (g_app.ui_cursor - 1 + g_app.ui_item_count) % g_app.ui_item_count;
                buzzer_beep(20);
            }
        } else if (g_app.ui_item_count > 0) {
            g_app.ui_cursor = (g_app.ui_cursor - 1 + g_app.ui_item_count) % g_app.ui_item_count;
            buzzer_beep(20);
        } else {
            buzzer_beep(20);
        }
        break;

    case BTN_EVT_TRIPLE_CLICK:
        if (g_app.current_mode == MODE_SETTINGS) {
            g_app.requested_mode = MODE_SELECT;
            g_app.mode_change_pending = true;
            buzzer_tone(900, 60);
        } else if (g_app.current_mode == MODE_FOX_HUNTER && g_app.fox_registry_open) {
            g_app.fox_registry_open = false;
            g_app.ui_cursor = 0;
            g_app.ui_item_count = 0;
            buzzer_tone(800, 40);
        } else if (g_app.current_mode == MODE_FOX_HUNTER) {
            if (g_app.fox_target_set) {
                fox_hunter_clear_target();
                storage_ext_append_log("fox", "target_cleared_by_triple_click");
                buzzer_tone(700, 80);
            } else {
                buzzer_beep(30);
            }
        } else if (g_app.current_mode != MODE_SELECT) {
            g_app.requested_mode = MODE_SELECT;
            g_app.mode_change_pending = true;
            buzzer_tone(900, 60);
        }
        break;

    case BTN_EVT_HOLD:
        buzzer_tone(900, 60);
        if (g_app.current_mode == MODE_SELECT) {
            static const app_mode_t sel_modes[] = {
                MODE_FLOCK_YOU, MODE_FOX_HUNTER, MODE_SKY_SPY, MODE_SETTINGS
            };
            if (g_app.ui_cursor >= 0 && g_app.ui_cursor < 4) {
                g_app.requested_mode = sel_modes[g_app.ui_cursor];
                g_app.mode_change_pending = true;
            }
            buzzer_tone(1100, 60);
        } else if (g_app.current_mode == MODE_SETTINGS) {
            apply_settings_item_action();
        } else if (g_app.current_mode == MODE_FLOCK_YOU) {
            if (g_app.device_count > 0 && g_app.ui_cursor < g_app.device_count) {
                fox_hunter_set_target_from_flock(g_app.ui_cursor);
                buzzer_tone(1200, 120);
            }
        } else if (g_app.current_mode == MODE_FOX_HUNTER) {
            if (!g_app.fox_registry_open) {
                g_app.fox_registry_open = true;
                g_app.ui_cursor = 0;
                g_app.ui_item_count = fox_hunter_registry_view_count();
                buzzer_tone(1200, 40);
            } else if (g_app.ui_item_count > 0) {
                fox_hunter_registry_select_view_index(g_app.ui_cursor);
                buzzer_tone(1400, 120);
            }
        }
        break;

    case BTN_EVT_LONG_HOLD_WARN:
        if (g_app.current_mode != MODE_SELECT && g_app.current_mode != MODE_SETTINGS && s_reset_warn_task == NULL) {
            if (xTaskCreate(reset_warning_flash_task, "reset_warn", 2048, NULL, 3, &s_reset_warn_task) != pdPASS) {
                s_reset_warn_task = NULL;
            }
        }
        break;

    case BTN_EVT_LONG_HOLD:
        buzzer_tone(600, 300);
        g_app.requested_mode = MODE_SELECT;
        g_app.mode_change_pending = true;
        break;
    }
}

void app_main(void)
{
    app_state_init();
    nvs_store_init();
    nvs_store_load_prefs();
    nvs_store_load_fox_registry();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  OUI-Spy C6 — RF Intelligence Tool");
    ESP_LOGI(TAG, "  Waveshare ESP32-C6-LCD-1.47");
    ESP_LOGI(TAG, "========================================");

    ESP_LOGI(TAG, "Phase 2: hardware init");
    display_init();
    led_ctrl_init();
    set_init_phase_led(LED_PHASE_CORE_READY);
    storage_ext_init();
    buzzer_init();
    button_init(on_button_event);

    render_boot_splash();
    buzzer_melody_boot();
    set_init_phase_led(LED_PHASE_BOOT_SPLASH);
    vTaskDelay(pdMS_TO_TICKS(1000));
    led_ctrl_off();

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

    app_mode_t saved_mode = nvs_store_load_mode();
    if (saved_mode >= MODE_COUNT) saved_mode = MODE_SELECT;
    ESP_LOGI(TAG, "Phase 4: starting saved mode %d", saved_mode);
    set_init_phase_led(LED_PHASE_MODE_START);
    vTaskDelay(pdMS_TO_TICKS(100));
    led_ctrl_off();
    start_mode(saved_mode);
    storage_ext_append_log("mode", "boot_mode_started");

    ESP_LOGI(TAG, "Entering main loop");
    vTaskDelay(pdMS_TO_TICKS(150));
    while (1) {
        if (g_app.mode_change_pending) {
            g_app.mode_change_pending = false;
            if (g_app.requested_mode != g_app.current_mode) {
                stop_current_mode();
                vTaskDelay(pdMS_TO_TICKS(200));
                start_mode(g_app.requested_mode);
            }
        }

        g_app.uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        g_app.free_heap = esp_get_free_heap_size();

        if (!g_app.display_sleeping && g_app.display_sleep_timeout_sec > 0) {
            uint32_t idle_ms = uptime_ms() - g_app.last_input_ms;
            if (idle_ms >= (uint32_t)g_app.display_sleep_timeout_sec * 1000U) {
                g_app.display_sleeping = true;
                display_set_brightness(0);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
