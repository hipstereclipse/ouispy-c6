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
#include "map_tile.h"
#include "web_server.h"
#include "flock_you.h"
#include "fox_hunter.h"
#include "sky_spy.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

static const char *TAG = "main";

#define LED_PHASE_CORE_READY    80,  78,  72
#define LED_PHASE_BOOT_SPLASH   40,  55,  38
#define LED_PHASE_WIFI_INIT     60,  45,  10
#define LED_PHASE_BLE_INIT      15,  35,  65
#define LED_PHASE_SNIFFER_INIT  15,  50,  55
#define LED_PHASE_MODE_START    30,  42,  28
#define UI_GPS_READY_TIMEOUT_MS 20000
#define UI_HOLD_MENU_WARN_MS    3500
#define UI_HOLD_MENU_TOTAL_MS   5000
#define UI_BORDER_PREVIEW_RAMP_UP_MS    10000
#define UI_BORDER_PREVIEW_RAMP_DOWN_MS   3000

enum {
    SET_ITEM_AP_BROADCAST = 0,
    SET_ITEM_SINGLE_AP_NAME,
    SET_ITEM_SLEEP_TIMEOUT,
    SET_ITEM_MENU_LED,
    SET_ITEM_BORDER_STYLE,
    SET_ITEM_DETECT_FLOCK_LOW,
    SET_ITEM_DETECT_FLOCK_HIGH,
    SET_ITEM_DETECT_FLOCK_CUSTOM,
    SET_ITEM_DETECT_FOX_LOW,
    SET_ITEM_DETECT_FOX_HIGH,
    SET_ITEM_DETECT_FOX_CUSTOM,
    SET_ITEM_DETECT_SKY_LOW,
    SET_ITEM_DETECT_SKY_HIGH,
    SET_ITEM_DETECT_SKY_CUSTOM,
    SET_ITEM_SOUND_FLOCK,
    SET_ITEM_SOUND_FOX,
    SET_ITEM_SOUND_SKY,
    SET_ITEM_SHORTCUT_MODE,
    SET_ITEM_SHORTCUT_ACTION,
    SET_ITEM_SHORTCUT_BACK,
    SET_ITEM_GPS_TAGGING,
    SET_ITEM_SD_LOGS,
    SET_ITEM_ADV_LOGS,
    SET_ITEM_GPS_DIAG,
    SET_ITEM_WEB_DIAG,
    SET_ITEM_ADV_SERIAL_LOG,
    SET_ITEM_SERIAL_LOG,
    SET_ITEM_SD_FORMAT,
    SET_ITEM_BACK,
    SET_ITEM_COUNT,
};

typedef enum {
    SETTINGS_CAT_CONNECTIVITY = 0,
    SETTINGS_CAT_DISPLAY,
    SETTINGS_CAT_FLOCK_IMPACT,
    SETTINGS_CAT_FOX_IMPACT,
    SETTINGS_CAT_SKY_IMPACT,
    SETTINGS_CAT_SOUND,
    SETTINGS_CAT_CONTROLS,
    SETTINGS_CAT_LOGGING,
    SETTINGS_CAT_MAINTENANCE,
    SETTINGS_CAT_COUNT,
} settings_category_t;

#define SETTINGS_ROOT_EXIT_ENTRY   (-1)
#define SETTINGS_SUBMENU_BACK_ENTRY (-2)

/* ── Display task handles ── */
static TaskHandle_t s_select_task = NULL;
static TaskHandle_t s_settings_task = NULL;
static TaskHandle_t s_reset_warn_task = NULL;
static int s_settings_category = -1;
static int s_settings_root_cursor = 0;   /* preserved root-menu cursor */

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

static const int SETTINGS_CAT_CONNECTIVITY_ITEMS[] = {
    SET_ITEM_AP_BROADCAST,
    SET_ITEM_SINGLE_AP_NAME,
    SET_ITEM_GPS_TAGGING,
};

static const int SETTINGS_CAT_DISPLAY_ITEMS[] = {
    SET_ITEM_SLEEP_TIMEOUT,
    SET_ITEM_MENU_LED,
    SET_ITEM_BORDER_STYLE,
};

static const int SETTINGS_CAT_FLOCK_IMPACT_ITEMS[] = {
    SET_ITEM_DETECT_FLOCK_LOW,
    SET_ITEM_DETECT_FLOCK_HIGH,
    SET_ITEM_DETECT_FLOCK_CUSTOM,
};

static const int SETTINGS_CAT_FOX_IMPACT_ITEMS[] = {
    SET_ITEM_DETECT_FOX_LOW,
    SET_ITEM_DETECT_FOX_HIGH,
    SET_ITEM_DETECT_FOX_CUSTOM,
};

static const int SETTINGS_CAT_SKY_IMPACT_ITEMS[] = {
    SET_ITEM_DETECT_SKY_LOW,
    SET_ITEM_DETECT_SKY_HIGH,
    SET_ITEM_DETECT_SKY_CUSTOM,
};

static const int SETTINGS_CAT_SOUND_ITEMS[] = {
    SET_ITEM_SOUND_FLOCK,
    SET_ITEM_SOUND_FOX,
    SET_ITEM_SOUND_SKY,
};

static const int SETTINGS_CAT_CONTROLS_ITEMS[] = {
    SET_ITEM_SHORTCUT_MODE,
    SET_ITEM_SHORTCUT_ACTION,
    SET_ITEM_SHORTCUT_BACK,
};

static const int SETTINGS_CAT_LOGGING_ITEMS[] = {
    SET_ITEM_SD_LOGS,
    SET_ITEM_ADV_LOGS,
    SET_ITEM_GPS_DIAG,
    SET_ITEM_WEB_DIAG,
    SET_ITEM_ADV_SERIAL_LOG,
    SET_ITEM_SERIAL_LOG,
};

static const int SETTINGS_CAT_MAINTENANCE_ITEMS[] = {
    SET_ITEM_SD_FORMAT,
};

static const struct {
    const char *title;
    const char *summary;
    const int *items;
    int item_count;
} SETTINGS_CATEGORIES[SETTINGS_CAT_COUNT] = {
    [SETTINGS_CAT_CONNECTIVITY] = {"Connectivity", "AP name, broadcast, GPS", SETTINGS_CAT_CONNECTIVITY_ITEMS, 3},
    [SETTINGS_CAT_DISPLAY] = {"Display", "Sleep, LED color, border", SETTINGS_CAT_DISPLAY_ITEMS, 3},
        [SETTINGS_CAT_FLOCK_IMPACT] = {"Flock Visuals", "LED color, effect, accent", SETTINGS_CAT_FLOCK_IMPACT_ITEMS, 3},
        [SETTINGS_CAT_FOX_IMPACT] = {"Fox Visuals", "LED color, effect, accent", SETTINGS_CAT_FOX_IMPACT_ITEMS, 3},
        [SETTINGS_CAT_SKY_IMPACT] = {"Sky Visuals", "LED color, effect, accent", SETTINGS_CAT_SKY_IMPACT_ITEMS, 3},
    [SETTINGS_CAT_SOUND] = {"Sound Profiles", "Per-applet audio behavior", SETTINGS_CAT_SOUND_ITEMS, 3},
    [SETTINGS_CAT_CONTROLS] = {"Button Shortcuts", "Quick actions per button", SETTINGS_CAT_CONTROLS_ITEMS, 3},
    [SETTINGS_CAT_LOGGING] = {"Logging", "Storage, diagnostics, serial", SETTINGS_CAT_LOGGING_ITEMS, 6},
    [SETTINGS_CAT_MAINTENANCE] = {"Maintenance", "microSD tools and service", SETTINGS_CAT_MAINTENANCE_ITEMS, 1},
};

static const char *serial_log_name(uint8_t idx)
{
    switch (idx) {
    case SERIAL_LOG_ERROR: return "ERROR";
    case SERIAL_LOG_WARN: return "WARN";
    case SERIAL_LOG_DEBUG: return "DEBUG";
    case SERIAL_LOG_VERBOSE: return "VERBOSE";
    case SERIAL_LOG_INFO:
    default: return "INFO";
    }
}

static bool ui_gps_tag_active(void)
{
    uint32_t now_ms = uptime_ms();
    bool gps_ready_fresh = g_app.gps_client_ready
                        && (now_ms > g_app.gps_client_ready_ms)
                        && ((now_ms - g_app.gps_client_ready_ms) <= UI_GPS_READY_TIMEOUT_MS);
    return g_app.gps_tagging_enabled && gps_ready_fresh && (g_app.wifi_clients > 0);
}

static void ui_draw_logging_badge(uint16_t bg)
{
    bool logging_active = storage_ext_logging_active();
    uint16_t fg = logging_active ? rgb565(74, 222, 128) : rgb565(239, 68, 68);
    display_draw_rect(LCD_H_RES - 14, DISPLAY_STATUS_TEXT_Y, 6, 8, bg);
    display_draw_text(LCD_H_RES - 14, DISPLAY_STATUS_TEXT_Y, "l", fg, bg);
}

static void set_init_phase_led(uint8_t r, uint8_t g, uint8_t b)
{
    led_ctrl_set_forced(r, g, b);
}

static void get_menu_led_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (idx) {
    case MENU_LED_AQUAMARINE:   *r = 35;  *g = 218; *b = 168; break; /* teal-cyan         */
    case MENU_LED_SAPPHIRE:     *r = 45;  *g = 120; *b = 255; break; /* vivid blue        */
    case MENU_LED_AMBER:        *r = 255; *g = 148; *b = 12;  break; /* warm orange       */
    case MENU_LED_ALEXANDRITE:  *r = 195; *g = 55;  *b = 165; break; /* purple-crimson    */
    case MENU_LED_RUBY:         *r = 255; *g = 38;  *b = 75;  break; /* deep red          */
    case MENU_LED_PERIDOT:      *r = 135; *g = 220; *b = 55;  break; /* yellow-green      */
    case MENU_LED_MOONSTONE:    *r = 175; *g = 215; *b = 255; break; /* ice blue-white    */
    case MENU_LED_DIAMOND:      *r = 248; *g = 248; *b = 255; break; /* pure bright white */
    case MENU_LED_AMETHYST:     *r = 175; *g = 75;  *b = 255; break; /* violet-purple     */
    case MENU_LED_EMERALD:      *r = 25;  *g = 195; *b = 95;  break; /* vivid green       */
    case MENU_LED_CITRINE:      *r = 255; *g = 215; *b = 35;  break; /* warm yellow       */
    case MENU_LED_CARNELIAN:    *r = 255; *g = 85;  *b = 28;  break; /* orange-red        */
    case MENU_LED_ROSE_QUARTZ:  *r = 255; *g = 130; *b = 175; break; /* soft pink         */
    case MENU_LED_TANZANITE:    *r = 75;  *g = 65;  *b = 225; break; /* indigo-blue       */
    case MENU_LED_LABRADORITE:  *r = 65;  *g = 175; *b = 215; break; /* aurora blue-teal  */
    case MENU_LED_CHALCOPYRITE: *r = 195; *g = 200; *b = 55;  break; /* iridescent gold-green */
    case MENU_LED_MALACHITE:    *r = 0;   *g = 200; *b = 130; break; /* bright teal-green */
    case MENU_LED_TOPAZ:
    default:                    *r = 255; *g = 190; *b = 36;  break; /* golden-orange     */
    }
}

static const char *menu_led_name(uint8_t idx)
{
    switch (idx) {
    case MENU_LED_AQUAMARINE:   return "AQUAMARINE";
    case MENU_LED_SAPPHIRE:     return "SAPPHIRE";
    case MENU_LED_AMBER:        return "AMBER";
    case MENU_LED_ALEXANDRITE:  return "ALEXANDRITE";
    case MENU_LED_RUBY:         return "RUBY";
    case MENU_LED_PERIDOT:      return "PERIDOT";
    case MENU_LED_MOONSTONE:    return "MOONSTONE";
    case MENU_LED_DIAMOND:      return "DIAMOND";
    case MENU_LED_AMETHYST:     return "AMETHYST";
    case MENU_LED_EMERALD:      return "EMERALD";
    case MENU_LED_CITRINE:      return "CITRINE";
    case MENU_LED_CARNELIAN:    return "CARNELIAN";
    case MENU_LED_ROSE_QUARTZ:  return "ROSE QUARTZ";
    case MENU_LED_TANZANITE:    return "TANZANITE";
    case MENU_LED_LABRADORITE:  return "LABRADORITE";
    case MENU_LED_CHALCOPYRITE: return "CHALCOPYRITE";
    case MENU_LED_MALACHITE:    return "MALACHITE";
    case MENU_LED_TOPAZ:
    default:                    return "TOPAZ";
    }
}

static const char *border_style_name(uint8_t idx)
{
    switch (idx) {
    case BORDER_STYLE_FLAMES:    return "FLAMES";
    case BORDER_STYLE_RADIATION: return "RADIATION";
    case BORDER_STYLE_GLITCH:    return "GLITCH";
    case BORDER_STYLE_VIPER:     return "VIPER";
    case BORDER_STYLE_SONAR:     return "SONAR";
    case BORDER_STYLE_NONE:      return "OFF";
    case BORDER_STYLE_PULSE:
    default:                     return "PULSE";
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

static bool settings_item_uses_visual_preview(int item)
{
    switch (item) {
    case SET_ITEM_MENU_LED:
    case SET_ITEM_BORDER_STYLE:
    case SET_ITEM_DETECT_FLOCK_LOW:
    case SET_ITEM_DETECT_FLOCK_HIGH:
    case SET_ITEM_DETECT_FLOCK_CUSTOM:
    case SET_ITEM_DETECT_FOX_LOW:
    case SET_ITEM_DETECT_FOX_HIGH:
    case SET_ITEM_DETECT_FOX_CUSTOM:
    case SET_ITEM_DETECT_SKY_LOW:
    case SET_ITEM_DETECT_SKY_HIGH:
    case SET_ITEM_DETECT_SKY_CUSTOM:
        return true;
    default:
        return false;
    }
}

static app_mode_t settings_preview_mode_for_item(int item)
{
    switch (item) {
    case SET_ITEM_DETECT_FLOCK_LOW:
    case SET_ITEM_DETECT_FLOCK_HIGH:
    case SET_ITEM_DETECT_FLOCK_CUSTOM:
        return MODE_FLOCK_YOU;
    case SET_ITEM_DETECT_FOX_LOW:
    case SET_ITEM_DETECT_FOX_HIGH:
    case SET_ITEM_DETECT_FOX_CUSTOM:
        return MODE_FOX_HUNTER;
    case SET_ITEM_DETECT_SKY_LOW:
    case SET_ITEM_DETECT_SKY_HIGH:
    case SET_ITEM_DETECT_SKY_CUSTOM:
        return MODE_SKY_SPY;
    case SET_ITEM_MENU_LED:
    case SET_ITEM_BORDER_STYLE:
    default:
        return MODE_SELECT;
    }
}

static uint8_t settings_preview_intensity_for_item(int item, uint32_t now)
{
    uint32_t cycle_ms = UI_BORDER_PREVIEW_RAMP_UP_MS + UI_BORDER_PREVIEW_RAMP_DOWN_MS;
    uint32_t pos = cycle_ms ? (now % cycle_ms) : 0;
    uint8_t preview = 255;

    if (item == SET_ITEM_BORDER_STYLE) {
        if (pos < UI_BORDER_PREVIEW_RAMP_UP_MS) {
            preview = (uint8_t)(72 + ((uint32_t)183 * pos) / UI_BORDER_PREVIEW_RAMP_UP_MS);
        } else {
            uint32_t down = pos - UI_BORDER_PREVIEW_RAMP_UP_MS;
            preview = (uint8_t)(255 - ((uint32_t)183 * down) / UI_BORDER_PREVIEW_RAMP_DOWN_MS);
        }
    } else if (settings_item_uses_visual_preview(item)) {
        float raw = (pos < UI_BORDER_PREVIEW_RAMP_UP_MS)
            ? ((float)pos / (float)UI_BORDER_PREVIEW_RAMP_UP_MS)
            : (1.0f - ((float)(pos - UI_BORDER_PREVIEW_RAMP_UP_MS) / (float)UI_BORDER_PREVIEW_RAMP_DOWN_MS));
        float eff = app_detection_behavior_strength(settings_preview_mode_for_item(item), raw);
        if (eff < 0.0f) eff = 0.0f;
        if (eff > 1.0f) eff = 1.0f;
        preview = (uint8_t)(46 + (eff * 209.0f));
    }

    return preview;
}

static bool settings_is_root_menu(void)
{
    return (s_settings_category < 0 || s_settings_category >= SETTINGS_CAT_COUNT);
}

static void settings_sync_item_count(void)
{
    if (settings_is_root_menu()) {
        g_app.ui_item_count = SETTINGS_CAT_COUNT + 1;
        return;
    }
    g_app.ui_item_count = SETTINGS_CATEGORIES[s_settings_category].item_count + 1;
}

static const char *settings_item_label(int item)
{
    switch (item) {
    case SET_ITEM_AP_BROADCAST: return "AP Broadcast";
    case SET_ITEM_SINGLE_AP_NAME: return "Single AP Name";
    case SET_ITEM_SLEEP_TIMEOUT: return "Sleep Timeout";
    case SET_ITEM_MENU_LED: return "Menu LED";
    case SET_ITEM_BORDER_STYLE: return "Detect Border";
    case SET_ITEM_DETECT_FLOCK_LOW: return "Board LED";
    case SET_ITEM_DETECT_FLOCK_HIGH: return "Display FX";
    case SET_ITEM_DETECT_FLOCK_CUSTOM: return "Accent Color";
    case SET_ITEM_DETECT_FOX_LOW: return "Board LED";
    case SET_ITEM_DETECT_FOX_HIGH: return "Display FX";
    case SET_ITEM_DETECT_FOX_CUSTOM: return "Accent Color";
    case SET_ITEM_DETECT_SKY_LOW: return "Board LED";
    case SET_ITEM_DETECT_SKY_HIGH: return "Display FX";
    case SET_ITEM_DETECT_SKY_CUSTOM: return "Accent Color";
    case SET_ITEM_SOUND_FLOCK: return "Sound: Flock";
    case SET_ITEM_SOUND_FOX: return "Sound: Fox";
    case SET_ITEM_SOUND_SKY: return "Sound: Sky";
    case SET_ITEM_SHORTCUT_MODE: return "Shortcut BTN10";
    case SET_ITEM_SHORTCUT_ACTION: return "Shortcut BTN11";
    case SET_ITEM_SHORTCUT_BACK: return "Shortcut BTN19";
    case SET_ITEM_GPS_TAGGING: return "GPS Tagging";
    case SET_ITEM_SD_LOGS: return "microSD Logs";
    case SET_ITEM_ADV_LOGS: return "Advanced Logs";
    case SET_ITEM_GPS_DIAG: return "GPS Diagnostics";
    case SET_ITEM_WEB_DIAG: return "Web Diagnostics";
    case SET_ITEM_ADV_SERIAL_LOG: return "Advanced Serial";
    case SET_ITEM_SERIAL_LOG: return "Serial Logging";
    case SET_ITEM_SD_FORMAT: return "Format microSD";
    case SET_ITEM_BACK: return "Back to Menu";
    default: return "Setting";
    }
}

static void settings_item_value(int item, char *val, size_t val_sz)
{
    switch (item) {
    case SET_ITEM_AP_BROADCAST:
        snprintf(val, val_sz, "%s", g_app.ap_broadcast_enabled ? "ON" : "OFF");
        break;
    case SET_ITEM_SLEEP_TIMEOUT:
        snprintf(val, val_sz, "%s", g_app.display_sleep_timeout_sec == 0 ? "OFF" : "SECONDS");
        break;
    case SET_ITEM_SINGLE_AP_NAME:
        snprintf(val, val_sz, "%s", g_app.single_ap_name_enabled ? "UNISPY" : "PER MODE");
        break;
    case SET_ITEM_MENU_LED:
        snprintf(val, val_sz, "%s", menu_led_name(g_app.menu_led_color));
        break;
    case SET_ITEM_BORDER_STYLE:
        snprintf(val, val_sz, "%s", border_style_name(g_app.border_style));
        break;
    case SET_ITEM_DETECT_FLOCK_LOW:
        snprintf(val, val_sz, "%s", menu_led_name(g_app.detect_flock_low));
        break;
    case SET_ITEM_DETECT_FLOCK_HIGH:
        snprintf(val, val_sz, "%s", border_style_name(g_app.detect_flock_high));
        break;
    case SET_ITEM_DETECT_FLOCK_CUSTOM:
        snprintf(val, val_sz, "%s", menu_led_name(g_app.detect_flock_custom));
        break;
    case SET_ITEM_DETECT_FOX_LOW:
        snprintf(val, val_sz, "%s", menu_led_name(g_app.detect_fox_low));
        break;
    case SET_ITEM_DETECT_FOX_HIGH:
        snprintf(val, val_sz, "%s", border_style_name(g_app.detect_fox_high));
        break;
    case SET_ITEM_DETECT_FOX_CUSTOM:
        snprintf(val, val_sz, "%s", menu_led_name(g_app.detect_fox_custom));
        break;
    case SET_ITEM_DETECT_SKY_LOW:
        snprintf(val, val_sz, "%s", menu_led_name(g_app.detect_sky_low));
        break;
    case SET_ITEM_DETECT_SKY_HIGH:
        snprintf(val, val_sz, "%s", border_style_name(g_app.detect_sky_high));
        break;
    case SET_ITEM_DETECT_SKY_CUSTOM:
        snprintf(val, val_sz, "%s", menu_led_name(g_app.detect_sky_custom));
        break;
    case SET_ITEM_SOUND_FLOCK:
        snprintf(val, val_sz, "%s", sound_profile_name(g_app.sound_profile_flock));
        break;
    case SET_ITEM_SOUND_FOX:
        snprintf(val, val_sz, "%s", sound_profile_name(g_app.sound_profile_fox));
        break;
    case SET_ITEM_SOUND_SKY:
        snprintf(val, val_sz, "%s", sound_profile_name(g_app.sound_profile_sky));
        break;
    case SET_ITEM_SHORTCUT_MODE:
        snprintf(val, val_sz, "%s", shortcut_name(g_app.shortcut_mode_btn));
        break;
    case SET_ITEM_SHORTCUT_ACTION:
        snprintf(val, val_sz, "%s", shortcut_name(g_app.shortcut_action_btn));
        break;
    case SET_ITEM_SHORTCUT_BACK:
        snprintf(val, val_sz, "%s", shortcut_name(g_app.shortcut_back_btn));
        break;
    case SET_ITEM_GPS_TAGGING:
        snprintf(val, val_sz, "%s", g_app.gps_tagging_enabled ? "ON" : "OFF");
        break;
    case SET_ITEM_SD_LOGS:
        snprintf(val, val_sz, "%s", g_app.use_microsd_logs ? "PREFER SD" : "RAM ONLY");
        break;
    case SET_ITEM_ADV_LOGS:
        snprintf(val, val_sz, "%s", g_app.advanced_logging_enabled ? "ON" : "OFF");
        break;
    case SET_ITEM_GPS_DIAG:
        snprintf(val, val_sz, "%s", g_app.gps_diagnostics_enabled ? "ON" : "OFF");
        break;
    case SET_ITEM_WEB_DIAG:
        snprintf(val, val_sz, "%s", g_app.web_diagnostics_enabled ? "ON" : "OFF");
        break;
    case SET_ITEM_ADV_SERIAL_LOG:
        snprintf(val, val_sz, "%s", g_app.advanced_serial_logging_enabled ? "ON" : "OFF");
        break;
    case SET_ITEM_SERIAL_LOG:
        snprintf(val, val_sz, "%s", serial_log_name(g_app.serial_log_verbosity));
        break;
    case SET_ITEM_SD_FORMAT:
        snprintf(val, val_sz, "%s", storage_ext_status_str(storage_ext_get_status()));
        break;
    case SET_ITEM_BACK:
        snprintf(val, val_sz, "%s", "> EXIT");
        break;
    default:
        snprintf(val, val_sz, "%s", "");
        break;
    }
}

static int settings_visible_entry_at(int cursor)
{
    if (settings_is_root_menu()) {
        return (cursor >= SETTINGS_CAT_COUNT) ? SETTINGS_ROOT_EXIT_ENTRY : cursor;
    }

    if (cursor >= SETTINGS_CATEGORIES[s_settings_category].item_count) {
        return SETTINGS_SUBMENU_BACK_ENTRY;
    }
    return SETTINGS_CATEGORIES[s_settings_category].items[cursor];
}

static int settings_current_leaf_item(void)
{
    if (settings_is_root_menu()) return -1;
    if (g_app.ui_cursor < 0 || g_app.ui_cursor >= SETTINGS_CATEGORIES[s_settings_category].item_count) return -1;
    return SETTINGS_CATEGORIES[s_settings_category].items[g_app.ui_cursor];
}

static void settings_open_category(int category)
{
    if (category < 0 || category >= SETTINGS_CAT_COUNT) return;
    s_settings_root_cursor = g_app.ui_cursor;  /* remember root position */
    s_settings_category = category;
    g_app.ui_cursor = 0;
    settings_sync_item_count();
}

static void settings_close_category(void)
{
    s_settings_category = -1;
    g_app.ui_cursor = s_settings_root_cursor;  /* restore root position */
    settings_sync_item_count();
    if (g_app.ui_cursor >= g_app.ui_item_count)
        g_app.ui_cursor = g_app.ui_item_count - 1;
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

static void draw_hold_menu_progress(float progress)
{
    uint8_t r = 0, g = 0, b = 0;
    uint16_t accent;
    int fill_h;
    int fill_w;
    int band_h;

    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    get_menu_led_rgb(g_app.menu_led_color, &r, &g, &b);
    accent = rgb565(r, g, b);
    fill_h = (int)((LCD_V_RES / 2.0f) * progress);
    fill_w = (int)((LCD_H_RES / 2.0f) * progress);
    band_h = LCD_V_RES - 2 * fill_h;

    if (fill_h > 0) {
        display_draw_rect(0, 0, LCD_H_RES, fill_h, accent);
        display_draw_rect(0, LCD_V_RES - fill_h, LCD_H_RES, fill_h, accent);
    }
    if (fill_w > 0 && band_h > 0) {
        display_draw_rect(0, fill_h, fill_w, band_h, accent);
        display_draw_rect(LCD_H_RES - fill_w, fill_h, fill_w, band_h, accent);
    }

    if ((LCD_H_RES - 2 * fill_w) > 42 && band_h > 22) {
        uint16_t text = progress > 0.6f ? rgb565(14, 10, 16) : rgb565(248, 245, 240);
        display_draw_text_centered(148, "HOLD TO MENU", text, accent);
        display_draw_text_centered(162, progress >= 0.95f ? "RELEASING" : "KEEP HOLDING", text, accent);
    }
}

static void reset_warning_flash_task(void *arg)
{
    button_id_t btn = (button_id_t)(intptr_t)arg;
    uint32_t start_ms = uptime_ms();

    while (g_app.current_mode != MODE_SELECT
            && g_app.current_mode != MODE_SETTINGS
            && !g_app.mode_change_pending
            && button_is_pressed(btn)) {
        uint32_t elapsed = uptime_ms() - start_ms;
        float progress = (float)elapsed / (float)(UI_HOLD_MENU_TOTAL_MS - UI_HOLD_MENU_WARN_MS);
        draw_hold_menu_progress(progress);
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    if (!g_app.mode_change_pending) {
        g_app.ui_refresh_token++;
    }

    s_reset_warn_task = NULL;
    vTaskDelete(NULL);
}

static void stop_current_mode(void)
{
    led_ctrl_breathe_stop();
    switch (g_app.current_mode) {
    case MODE_FLOCK_YOU:  g_app.local_map_open = false; flock_you_stop(); break;
    case MODE_FOX_HUNTER: fox_hunter_stop(); break;
    case MODE_SKY_SPY:    g_app.local_map_open = false; sky_spy_stop(); break;
    case MODE_SELECT:
    case MODE_SETTINGS:
    default:
        break;
    }
}

static void render_mode_select_screen(int cursor)
{
    uint16_t bg          = rgb565(10, 9, 15);
    uint16_t panel       = rgb565(22, 20, 30);
    uint16_t panel_alt   = rgb565(16, 15, 23);
    uint16_t gold        = rgb565(251, 191, 36);
    uint16_t purple_dim  = rgb565(88, 28, 135);
    uint16_t border      = rgb565(55, 48, 70);
    uint16_t text_main   = rgb565(250, 250, 250);
    uint16_t text_dim    = rgb565(161, 161, 170);
    uint16_t text_soft   = rgb565(122, 122, 136);
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
    const int entry_count = 4;

    if (cursor < 0) cursor = 0;
    if (cursor >= entry_count) cursor = entry_count - 1;

    display_fill(bg);

    /* Header: gold crown accent at top, solid purple block below */
    display_draw_rect(0, 0, LCD_H_RES, 44, purple_dim);
    display_draw_rect(0, 0, LCD_H_RES, 2, gold);
    display_draw_text_centered(10, "OUI-SPY C6", gold, purple_dim);
    display_draw_text_centered(22, "MAIN MENU", rgb565(216, 180, 254), purple_dim);
    display_draw_rect(0, 42, LCD_H_RES, 2, rgb565(42, 36, 56));
    ui_draw_logging_badge(purple_dim);

    const char *ap_ssid = MODE_CFG[MODE_SELECT].ssid;
    const char *ap_pass = MODE_CFG[MODE_SELECT].pass;
    uint8_t ap_channel = MODE_CFG[MODE_SELECT].channel;
    mode_ap_credentials(g_app.current_mode, &ap_ssid, &ap_pass, &ap_channel);

    display_draw_bordered_rect(8, 46, LCD_H_RES - 16, 70, entries[cursor].accent_col, panel);
    display_draw_rect(12, 50, 6, 62, entries[cursor].accent_col);
    display_draw_text(24, 54, entries[cursor].label, text_main, panel);
    display_draw_text(24, 68, entries[cursor].desc, text_link, panel);
    display_draw_text(24, 82, ap_ssid, text_main, panel);
    display_draw_text(24, 96, ap_pass, text_dim, panel);
    display_draw_text(LCD_H_RES - 38, 58, "GO", entries[cursor].accent_col, panel);
    display_draw_text(LCD_H_RES - 42, 72, "NOW", text_soft, panel);

    display_draw_bordered_rect(8, 122, 48, 22, border, panel_alt);
    display_draw_text(12, 128, "AP", text_soft, panel_alt);
    display_draw_text(28, 128, g_app.ap_broadcast_enabled ? "OPEN" : "HIDE", text_link, panel_alt);

    display_draw_bordered_rect(62, 122, 52, 22, border, panel_alt);
    display_draw_text(66, 128, "GPS", text_soft, panel_alt);
    display_draw_text(88, 128, gps_tag_active ? "ON" : "OFF",
                      gps_tag_active ? rgb565(74, 222, 128) : rgb565(248, 113, 113), panel_alt);

    display_draw_bordered_rect(120, 122, 44, 22, border, panel_alt);
    display_draw_text(124, 128, "CH", text_soft, panel_alt);
    char ch_buf[8];
    snprintf(ch_buf, sizeof(ch_buf), "%u", ap_channel);
    display_draw_text(142, 128, ch_buf, text_main, panel_alt);

    display_draw_text_centered(156, "Select a mode", text_dim, bg);

    for (int i = 0; i < entry_count; i++) {
        int y = 176 + i * 22;
        bool selected = (i == cursor);
        uint16_t row_bg = selected ? rgb565(38, 30, 52) : panel_alt;
        uint16_t row_fg = selected ? rgb565(248, 245, 240) : text_main;
        uint16_t row_desc = selected ? rgb565(188, 182, 205) : text_dim;
        uint16_t row_border = selected ? entries[i].accent_col : border;

        display_draw_bordered_rect(8, y, LCD_H_RES - 16, 18, row_border, row_bg);
        display_draw_rect(12, y + 2, 4, 14, entries[i].accent_col);
        if (selected) {
            display_draw_text(LCD_H_RES - 26, y + 5, ">", entries[i].accent_col, row_bg);
        }
        display_draw_text(22, y + 2, entries[i].label, row_fg, row_bg);
        display_draw_text(102, y + 2, entries[i].desc, row_desc, row_bg);
    }

    /* microSD card status - bottom of display */
    storage_status_t microsd_status = storage_ext_get_status();
    uint16_t microsd_color = microsd_status == STORAGE_STATUS_AVAILABLE
                                 ? rgb565(52, 211, 153)
                                 : (microsd_status == STORAGE_STATUS_NEEDS_FORMAT ? rgb565(251, 191, 36)
                                                                                   : rgb565(249, 115, 22));
    char microsd_text[40];
    if (microsd_status == STORAGE_STATUS_AVAILABLE) {
        uint32_t used_kb = storage_ext_used_kb();
        uint32_t total_kb = storage_ext_total_kb();
        if (total_kb >= 1024U) {
            unsigned long used_tenths = (unsigned long)((used_kb * 10U + 512U) / 1024U);
            unsigned long total_mb = (unsigned long)((total_kb + 512U) / 1024U);
            snprintf(microsd_text, sizeof(microsd_text), "microSD: %lu.%lu/%luMB", used_tenths / 10UL, used_tenths % 10UL, total_mb);
        } else if (total_kb > 0) {
            snprintf(microsd_text, sizeof(microsd_text), "microSD: %lu/%lu KB", (unsigned long)used_kb, (unsigned long)total_kb);
        } else {
            snprintf(microsd_text, sizeof(microsd_text), "microSD: Mounted");
        }
    } else if (microsd_status == STORAGE_STATUS_NEEDS_FORMAT) {
        snprintf(microsd_text, sizeof(microsd_text), "microSD: Needs format");
    } else {
        snprintf(microsd_text, sizeof(microsd_text), "microSD: Not found");
    }
    display_draw_bordered_rect(8, 274, LCD_H_RES - 16, 18, border, panel_alt);
    display_draw_text_centered(280, microsd_text, microsd_color, panel_alt);
    display_draw_text_centered(298, "Click next  DblClk prev  Hold launch", text_dim, bg);
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

    char val[32];
    bool root_menu = settings_is_root_menu();
    int visible_count = root_menu ? (SETTINGS_CAT_COUNT + 1) : (SETTINGS_CATEGORIES[s_settings_category].item_count + 1);
    const char *header_title = root_menu ? "SETTINGS" : SETTINGS_CATEGORIES[s_settings_category].title;
    const char *header_hint = root_menu ? "Hold=open section  Triple=exit" : "Hold=change  Triple=back";
    if (cursor < 0) cursor = 0;
    if (cursor >= visible_count) cursor = visible_count - 1;

    display_fill(bg);
    display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 32, rgb565(38, 24, 54));
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 8, header_title, rgb565(235, 220, 255), rgb565(38, 24, 54));
    display_draw_text_centered(DISPLAY_STATUS_BAR_Y + 19, header_hint, text_dim, rgb565(38, 24, 54));
    ui_draw_logging_badge(rgb565(38, 24, 54));

    int start = cursor - 3;
    if (start < 0) start = 0;
    if (start > visible_count - 7) start = visible_count - 7;
    if (start < 0) start = 0;

    for (int i = 0; i < 7; i++) {
        int idx = start + i;
        if (idx >= visible_count) break;
        int y = 42 + i * 30;
        bool sel = (idx == cursor);
        int entry = settings_visible_entry_at(idx);
        const char *label = NULL;

        display_draw_bordered_rect(6, y, LCD_H_RES - 12, 26,
                                   sel ? accent : border,
                                   sel ? rgb565(32, 26, 44) : card);
        if (root_menu) {
            if (entry == SETTINGS_ROOT_EXIT_ENTRY) {
                label = "Exit Settings";
                snprintf(val, sizeof(val), "%s", "> MENU");
            } else {
                label = SETTINGS_CATEGORIES[entry].title;
                snprintf(val, sizeof(val), "%s", SETTINGS_CATEGORIES[entry].summary);
            }
        } else {
            if (entry == SETTINGS_SUBMENU_BACK_ENTRY) {
                label = "Back to Settings";
                snprintf(val, sizeof(val), "%s", "> SECTIONS");
            } else {
                label = settings_item_label(entry);
                settings_item_value(entry, val, sizeof(val));
            }
        }

        display_draw_text(12, y + 5, label, sel ? text_main : text_dim, sel ? rgb565(32, 26, 44) : card);

        int value_x = LCD_H_RES - 10 - display_text_width(val);
        if (value_x < 92) {
            value_x = 92;
        }
        display_draw_text(value_x, y + 15, val,
                          sel ? rgb565(196, 168, 255) : text_dim,
                          sel ? rgb565(32, 26, 44) : card);
    }

    snprintf(val, sizeof(val), "GPS %s", gps_tag_active ? "READY" : (g_app.gps_tagging_enabled ? "WAITING" : "OFF"));
    display_draw_text_centered(270,
                               val,
                               gps_tag_active ? rgb565(74, 222, 128) : (g_app.gps_tagging_enabled ? rgb565(251, 191, 36) : text_dim),
                               bg);

    if (!root_menu && s_settings_category == SETTINGS_CAT_DISPLAY && g_app.display_sleep_timeout_sec > 0) {
        snprintf(val, sizeof(val), "%us", (unsigned)g_app.display_sleep_timeout_sec);
        display_draw_text_centered(286, val, text_dim, bg);
    } else if (!root_menu) {
        snprintf(val, sizeof(val), "%d items", SETTINGS_CATEGORIES[s_settings_category].item_count);
        display_draw_text_centered(286, val, text_dim, bg);
    } else {
        snprintf(val, sizeof(val), "%d sections + exit", SETTINGS_CAT_COUNT);
        display_draw_text_centered(286, val, text_dim, bg);
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
    int last_gps_active = -1;
    int last_sd_status = -1;
    int last_sd_used_bucket = -1;
    int last_logging_active = -1;
    int last_logging_blocked = -1;
    uint8_t r, g, b;
    get_menu_led_rgb(g_app.menu_led_color, &r, &g, &b);
    led_ctrl_breathe_forced(r, g, b, 2200);

    while (g_app.current_mode == MODE_SELECT) {
        bool gps_tag_active = ui_gps_tag_active();
        storage_status_t sd_status = storage_ext_get_status();
        bool logging_active = storage_ext_logging_active();
        bool logging_blocked = storage_ext_logging_blocked();
        int sd_used_bucket = 0;
        if (sd_status == STORAGE_STATUS_AVAILABLE) {
            sd_used_bucket = (int)(storage_ext_used_kb() / 64U);
        }

        if (g_app.ui_cursor >= 4) g_app.ui_cursor = 3;
        if (g_app.ui_cursor < 0) g_app.ui_cursor = 0;

        if (g_app.ui_cursor != last_cursor
                || last_gps_active != (int)gps_tag_active
                || last_sd_status != (int)sd_status
                || last_sd_used_bucket != sd_used_bucket
                || last_logging_active != (int)logging_active
                || last_logging_blocked != (int)logging_blocked) {
            render_mode_select_screen(g_app.ui_cursor);
            last_cursor = g_app.ui_cursor;
            last_gps_active = (int)gps_tag_active;
            last_sd_status = (int)sd_status;
            last_sd_used_bucket = sd_used_bucket;
            last_logging_active = (int)logging_active;
            last_logging_blocked = (int)logging_blocked;
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
    int last_category = -2;  /* track category changes for reliable re-render */
    int last_gps_active = -1;
    int last_gps_enabled = -1;
    int last_sd_status = -1;
    int last_sd_logs = -1;
    int last_logging_active = -1;
    int last_logging_blocked = -1;
    uint32_t last_refresh_token = 0;
    bool visual_preview_active = false;
    int visual_preview_item = -1;
    uint8_t visual_preview_color = 0xFF;
    uint8_t visual_preview_style = 0xFF;
    uint32_t border_preview_started_ms = 0;
    while (g_app.current_mode == MODE_SETTINGS) {
        bool gps_tag_active = ui_gps_tag_active();
        storage_status_t sd_status = storage_ext_get_status();
        bool logging_active = storage_ext_logging_active();
        bool logging_blocked = storage_ext_logging_blocked();

        settings_sync_item_count();
        if (g_app.ui_cursor >= g_app.ui_item_count) g_app.ui_cursor = g_app.ui_item_count - 1;
        if (g_app.ui_cursor < 0) g_app.ui_cursor = 0;

        int preview_item = settings_current_leaf_item();
        bool wants_visual_preview = settings_item_uses_visual_preview(preview_item);
        if (wants_visual_preview) {
            uint32_t now = uptime_ms();

            if (visual_preview_item != preview_item || border_preview_started_ms == 0) {
                border_preview_started_ms = now;
            }

            led_ctrl_set_effect_intensity(settings_preview_intensity_for_item(preview_item, now - border_preview_started_ms));

            app_mode_t preview_mode = settings_preview_mode_for_item(preview_item);
            uint8_t preview_color_idx = (preview_mode == MODE_SELECT)
                ? g_app.menu_led_color
                : app_mode_display_color(preview_mode);
            uint8_t preview_style = (preview_mode == MODE_SELECT)
                ? g_app.border_style
                : app_mode_display_style(preview_mode);

            if (!visual_preview_active
                    || visual_preview_item != preview_item
                    || visual_preview_color != preview_color_idx
                    || visual_preview_style != preview_style) {
                uint8_t r = 0, g = 0, b = 0;
                if (preview_mode != MODE_SELECT) {
                    g_app.border_style = preview_style;
                }
                app_palette_rgb(preview_color_idx, &r, &g, &b);
                led_ctrl_breathe_forced(r, g, b, 2200);
                visual_preview_active = true;
                visual_preview_item = preview_item;
                visual_preview_color = preview_color_idx;
                visual_preview_style = preview_style;
            }
        } else if (visual_preview_active) {
            border_preview_started_ms = 0;
            led_ctrl_set_effect_intensity(255);
            led_ctrl_breathe_stop();
            led_ctrl_off();
            visual_preview_active = false;
            visual_preview_item = -1;
            visual_preview_color = 0xFF;
            visual_preview_style = 0xFF;
        } else {
            border_preview_started_ms = 0;
            led_ctrl_set_effect_intensity(255);
        }

        if (g_app.ui_cursor != last_cursor
                || s_settings_category != last_category
                || last_gps_active != (int)gps_tag_active
                || last_gps_enabled != (int)g_app.gps_tagging_enabled
                || last_sd_status != (int)sd_status
                || last_sd_logs != (int)g_app.use_microsd_logs
                || last_logging_active != (int)logging_active
                || last_logging_blocked != (int)logging_blocked
                || last_refresh_token != g_app.ui_refresh_token) {
            render_settings_screen(g_app.ui_cursor);
            last_cursor = g_app.ui_cursor;
            last_category = s_settings_category;
            last_refresh_token = g_app.ui_refresh_token;
            last_gps_active = (int)gps_tag_active;
            last_gps_enabled = (int)g_app.gps_tagging_enabled;
            last_sd_status = (int)sd_status;
            last_sd_logs = (int)g_app.use_microsd_logs;
            last_logging_active = (int)logging_active;
            last_logging_blocked = (int)logging_blocked;
        }
        vTaskDelay(pdMS_TO_TICKS(90));
    }
    if (visual_preview_active) {
        led_ctrl_set_effect_intensity(255);
        led_ctrl_breathe_stop();
        led_ctrl_off();
    }
    s_settings_task = NULL;
    vTaskDelete(NULL);
}

static void start_mode(app_mode_t mode)
{
    const char *names[] = {"SELECT", "FLOCK YOU", "FOX HUNTER", "SKY SPY", "SETTINGS"};

    ESP_LOGI(TAG, "Starting mode %d", mode);
    app_apply_mode_visual_prefs(mode);

    /* Check if WiFi AP config actually changes for this mode */
    const char *old_ssid = NULL, *old_pass = NULL;
    const char *new_ssid = NULL, *new_pass = NULL;
    uint8_t old_ch = 0, new_ch = 0;
    app_mode_ap_credentials(g_app.current_mode, &old_ssid, &old_pass, &old_ch);
    app_mode_ap_credentials(mode, &new_ssid, &new_pass, &new_ch);

    bool ap_changed = !g_app.http_server
                   || (strcmp(old_ssid, new_ssid) != 0)
                   || (strcmp(old_pass, new_pass) != 0)
                   || (old_ch != new_ch);

    g_app.current_mode = mode;
    g_app.last_input_ms = uptime_ms();
    g_app.display_sleeping = false;
    display_set_brightness(g_app.lcd_brightness);

    if (ap_changed) {
        restart_ap_for_mode(mode);
        web_server_stop();
        vTaskDelay(pdMS_TO_TICKS(50));
        web_server_start();
    }

    nvs_store_save_mode(mode == MODE_SETTINGS ? MODE_SELECT : mode);
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
        s_settings_category = -1;
        s_settings_root_cursor = 0;
        g_app.ui_cursor = 0;
        settings_sync_item_count();
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

static void apply_settings_leaf_action(int item)
{
    const char *log_msg = NULL;

    switch (item) {
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

    case SET_ITEM_BORDER_STYLE:
        g_app.border_style = (g_app.border_style + 1) % BORDER_STYLE_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "border_style_updated";
        break;

    case SET_ITEM_DETECT_FLOCK_LOW:
        g_app.detect_flock_low = (g_app.detect_flock_low + 1) % MENU_LED_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_flock_low_updated";
        break;

    case SET_ITEM_DETECT_FLOCK_HIGH:
        g_app.detect_flock_high = (g_app.detect_flock_high + 1) % BORDER_STYLE_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_flock_high_updated";
        break;

    case SET_ITEM_DETECT_FLOCK_CUSTOM:
        g_app.detect_flock_custom = (g_app.detect_flock_custom + 1) % MENU_LED_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_flock_custom_updated";
        break;

    case SET_ITEM_DETECT_FOX_LOW:
        g_app.detect_fox_low = (g_app.detect_fox_low + 1) % MENU_LED_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_fox_low_updated";
        break;

    case SET_ITEM_DETECT_FOX_HIGH:
        g_app.detect_fox_high = (g_app.detect_fox_high + 1) % BORDER_STYLE_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_fox_high_updated";
        break;

    case SET_ITEM_DETECT_FOX_CUSTOM:
        g_app.detect_fox_custom = (g_app.detect_fox_custom + 1) % MENU_LED_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_fox_custom_updated";
        break;

    case SET_ITEM_DETECT_SKY_LOW:
        g_app.detect_sky_low = (g_app.detect_sky_low + 1) % MENU_LED_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_sky_low_updated";
        break;

    case SET_ITEM_DETECT_SKY_HIGH:
        g_app.detect_sky_high = (g_app.detect_sky_high + 1) % BORDER_STYLE_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_sky_high_updated";
        break;

    case SET_ITEM_DETECT_SKY_CUSTOM:
        g_app.detect_sky_custom = (g_app.detect_sky_custom + 1) % MENU_LED_COUNT;
        buzzer_play_profile(SOUND_PROFILE_CHIRP);
        log_msg = "detect_sky_custom_updated";
        break;

    case SET_ITEM_SOUND_FLOCK:
        g_app.sound_profile_flock = (g_app.sound_profile_flock + 1) % SOUND_PROFILE_COUNT;
        buzzer_play_profile_forced(g_app.sound_profile_flock);
        log_msg = "sound_flock_updated";
        break;

    case SET_ITEM_SOUND_FOX:
        g_app.sound_profile_fox = (g_app.sound_profile_fox + 1) % SOUND_PROFILE_COUNT;
        buzzer_play_profile_forced(g_app.sound_profile_fox);
        log_msg = "sound_fox_updated";
        break;

    case SET_ITEM_SOUND_SKY:
        g_app.sound_profile_sky = (g_app.sound_profile_sky + 1) % SOUND_PROFILE_COUNT;
        buzzer_play_profile_forced(g_app.sound_profile_sky);
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

    case SET_ITEM_GPS_TAGGING:
        g_app.gps_tagging_enabled = !g_app.gps_tagging_enabled;
        if (!g_app.gps_tagging_enabled) {
            g_app.gps_client_ready = false;
            g_app.gps_client_ready_ms = 0;
        }
        buzzer_play_profile(SOUND_PROFILE_STANDARD);
        log_msg = g_app.gps_tagging_enabled ? "gps_tagging_enabled" : "gps_tagging_disabled";
        break;

    case SET_ITEM_SD_LOGS:
        g_app.use_microsd_logs = !g_app.use_microsd_logs;
        buzzer_play_profile(SOUND_PROFILE_RETRO);
        log_msg = g_app.use_microsd_logs ? "sd_logs_enabled" : "sd_logs_disabled";
        break;

    case SET_ITEM_ADV_LOGS:
        g_app.advanced_logging_enabled = !g_app.advanced_logging_enabled;
        buzzer_play_profile(SOUND_PROFILE_RETRO);
        log_msg = g_app.advanced_logging_enabled ? "advanced_logs_enabled" : "advanced_logs_disabled";
        break;

    case SET_ITEM_GPS_DIAG:
        g_app.gps_diagnostics_enabled = !g_app.gps_diagnostics_enabled;
        buzzer_beep(40);
        log_msg = g_app.gps_diagnostics_enabled ? "gps_diagnostics_enabled" : "gps_diagnostics_disabled";
        break;

    case SET_ITEM_WEB_DIAG:
        g_app.web_diagnostics_enabled = !g_app.web_diagnostics_enabled;
        buzzer_beep(40);
        log_msg = g_app.web_diagnostics_enabled ? "web_diagnostics_enabled" : "web_diagnostics_disabled";
        break;

    case SET_ITEM_ADV_SERIAL_LOG:
        g_app.advanced_serial_logging_enabled = !g_app.advanced_serial_logging_enabled;
        buzzer_beep(40);
        log_msg = g_app.advanced_serial_logging_enabled ? "advanced_serial_logging_enabled" : "advanced_serial_logging_disabled";
        break;

    case SET_ITEM_SERIAL_LOG:
        g_app.serial_log_verbosity = (g_app.serial_log_verbosity + 1) % SERIAL_LOG_COUNT;
        app_apply_runtime_logging_prefs();
        buzzer_beep(40);
        log_msg = "serial_logging_updated";
        break;

    case SET_ITEM_SD_FORMAT: {
        /* Only format a detected card. The storage layer will still handle
         * unreadable filesystems, but won't treat "not found" as wipeable. */
        esp_err_t err = storage_ext_format();
        if (err == ESP_OK) {
            buzzer_play_profile(SOUND_PROFILE_SONAR);
            log_msg = "sd_format_success";
        } else {
            buzzer_tone(300, 150);  /* Error tone */
            log_msg = "sd_format_failed";
        }
        break;
    }

    default:
        break;
    }

    nvs_store_save_prefs();
    app_apply_runtime_logging_prefs();
    if (log_msg) {
        storage_ext_append_log("settings", log_msg);
    }
}

static void apply_settings_item_action(void)
{
    int entry = settings_visible_entry_at(g_app.ui_cursor);

    if (settings_is_root_menu()) {
        if (entry == SETTINGS_ROOT_EXIT_ENTRY) {
            g_app.requested_mode = MODE_SELECT;
            g_app.mode_change_pending = true;
            buzzer_tone(900, 60);
            return;
        }
        settings_open_category(entry);
        buzzer_tone(1050, 40);
        g_app.ui_refresh_token++;
        return;
    }

    if (entry == SETTINGS_SUBMENU_BACK_ENTRY) {
        settings_close_category();
        buzzer_tone(820, 40);
        g_app.ui_refresh_token++;
        return;
    }

    apply_settings_leaf_action(entry);
    g_app.ui_refresh_token++;
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
        if ((g_app.current_mode == MODE_FLOCK_YOU || g_app.current_mode == MODE_SKY_SPY)
            && g_app.local_map_open) {
            int zooms[20];
            size_t zoom_count = map_tile_browsable_zooms(zooms, sizeof(zooms) / sizeof(zooms[0]));
            size_t zoom_options = (zoom_count > 0) ? zoom_count : 5;
            g_app.local_map_zoom_idx = (g_app.local_map_zoom_idx + 1) % zoom_options;
            g_app.ui_refresh_token++;
            buzzer_tone(1000, 40);
        } else if (g_app.current_mode == MODE_FOX_HUNTER) {
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
            if (settings_is_root_menu()) {
                g_app.requested_mode = MODE_SELECT;
                g_app.mode_change_pending = true;
                buzzer_tone(900, 60);
            } else {
                settings_close_category();
                g_app.ui_refresh_token++;
                buzzer_tone(820, 40);
            }
        } else if (g_app.current_mode == MODE_FOX_HUNTER && g_app.fox_registry_open) {
            g_app.fox_registry_open = false;
            g_app.ui_cursor = 0;
            g_app.ui_item_count = 0;
            g_app.ui_refresh_token++;
            buzzer_tone(800, 40);
        } else if (g_app.current_mode == MODE_FOX_HUNTER) {
            if (g_app.fox_target_set) {
                fox_hunter_clear_target();
                storage_ext_append_log("fox", "target_cleared_by_triple_click");
                buzzer_tone(700, 80);
            } else {
                buzzer_beep(30);
            }
        } else if (g_app.current_mode == MODE_FLOCK_YOU
                   || g_app.current_mode == MODE_SKY_SPY) {
            g_app.local_map_open = !g_app.local_map_open;
            g_app.ui_refresh_token++;
            buzzer_tone(g_app.local_map_open ? 1000 : 800, 60);
        } else if (g_app.current_mode != MODE_SELECT) {
            g_app.requested_mode = MODE_SELECT;
            g_app.mode_change_pending = true;
            buzzer_tone(900, 60);
        }
        break;

    case BTN_EVT_HOLD:
        if (g_app.current_mode != MODE_SETTINGS)
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
                g_app.ui_refresh_token++;
                buzzer_tone(1200, 40);
            } else if (g_app.ui_item_count > 0) {
                fox_hunter_registry_select_view_index(g_app.ui_cursor);
                buzzer_tone(1400, 120);
            }
        }
        break;

    case BTN_EVT_LONG_HOLD_WARN:
        if (g_app.current_mode != MODE_SELECT && g_app.current_mode != MODE_SETTINGS && s_reset_warn_task == NULL) {
            if (xTaskCreate(reset_warning_flash_task, "reset_warn", 3072, (void *)(intptr_t)btn, 3, &s_reset_warn_task) != pdPASS) {
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

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *fw_version = (app_desc && app_desc->version[0]) ? app_desc->version : "unknown";
    bool resume_saved_mode = nvs_store_mark_firmware_seen(fw_version);
    app_mode_t saved_mode = MODE_SELECT;
    if (!resume_saved_mode) {
        ESP_LOGI(TAG, "New firmware version detected (%s); starting in mode selector", fw_version);
    } else {
        ESP_LOGI(TAG, "Starting in mode selector; saved-mode auto-resume disabled for stability");
    }
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
