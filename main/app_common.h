/*
 * OUI-Spy C6 — Common types, constants, and shared state
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Board Variant ───────────────────────────────────────── */
/*
 * BOARD_TOUCH selects between the two supported boards:
 *   0 = Waveshare ESP32-C6-LCD-1.47        (ST7789, non-touch)  [default]
 *   1 = Waveshare ESP32-C6-Touch-LCD-1.47  (JD9853, capacitive touch)
 *
 * Set via compiler flag:  -DBOARD_TOUCH=1
 * Or in CMakeLists.txt:   target_compile_definitions(${COMPONENT_LIB} PUBLIC BOARD_TOUCH=1)
 */
#ifndef BOARD_TOUCH
#define BOARD_TOUCH 0
#endif

/* ── Pin Definitions ─────────────────────────────────────── */

#if BOARD_TOUCH
/* Waveshare ESP32-C6-Touch-LCD-1.47
 * SPI bus is on GPIO 1/2/3; I2C (touch) on GPIO 18/19.
 * GPIO 18 & 19 are NOT available for buzzer/back-button. */
#define PIN_LCD_MOSI    2
#define PIN_LCD_SCLK    1
#define PIN_LCD_CS      14
#define PIN_LCD_DC      15
#define PIN_LCD_RST     22
#define PIN_LCD_BL      23
#define PIN_SD_MISO     3
#define PIN_SD_CS       4
#define PIN_RGB_LED     8
#define PIN_BOOT_BTN    9
#define PIN_BTN_MODE    10
#define PIN_BTN_ACTION  11
#define PIN_BTN_BACK    (-1)  /* Not available — GPIO 19 is I2C_SCL */
#define PIN_BUZZER      (-1)  /* Not available — GPIO 18 is I2C_SDA */
#define HAS_BTN_BACK    0
#define HAS_BUZZER      0
#define HAS_RGB_LED     0     /* No WS2812 — use on-screen indicator instead */

#else
/* Waveshare ESP32-C6-LCD-1.47 (non-touch, original) */
#define PIN_LCD_MOSI    6
#define PIN_LCD_SCLK    7
#define PIN_LCD_CS      14
#define PIN_LCD_DC      15
#define PIN_LCD_RST     21
#define PIN_LCD_BL      22
#define PIN_SD_MISO     5
#define PIN_SD_CS       4
#define PIN_RGB_LED     8
#define PIN_BOOT_BTN    9
#define PIN_BTN_MODE    10   /* Cycle modes       – wire between GPIO10 and GND */
#define PIN_BTN_ACTION  11   /* Contextual action  – wire between GPIO11 and GND */
#define PIN_BTN_BACK    19   /* Back / cancel      – wire between GPIO19 and GND */
#define PIN_BUZZER      18   /* Piezo buzzer (+)   – wire + to GPIO18, – to GND  */
#define HAS_BTN_BACK    1
#define HAS_BUZZER      1
#define HAS_RGB_LED     1     /* WS2812 addressable RGB LED on GPIO 8 */
#endif

/* ── Display Constants ───────────────────────────────────── */

#define LCD_H_RES       172
#define LCD_V_RES       320
#define LCD_COL_OFFSET  34
#define LCD_ROW_OFFSET  0

#define TASK_STACK_BUTTON   4096
#define TASK_STACK_UI       8192
#define TASK_STACK_WS       6144
#define TASK_STACK_SNIFFER  4096

/* ── Application Modes ───────────────────────────────────── */

typedef enum {
    MODE_SELECT    = 0,
    MODE_FLOCK_YOU = 1,
    MODE_FOX_HUNTER= 2,
    MODE_SKY_SPY   = 3,
    MODE_SETTINGS  = 4,
    MODE_COUNT
} app_mode_t;

typedef enum {
    MENU_LED_TOPAZ        = 0,   /* golden yellow  — maps from old GOLD         */
    MENU_LED_AQUAMARINE   = 1,   /* teal           — maps from old MINT         */
    MENU_LED_SAPPHIRE     = 2,   /* vivid blue     — maps from old SKY          */
    MENU_LED_AMBER        = 3,   /* orange         — unchanged                  */
    MENU_LED_ALEXANDRITE  = 4,   /* purple-crimson — maps from old MAGENTA      */
    MENU_LED_RUBY         = 5,   /* red            — unchanged                  */
    MENU_LED_PERIDOT      = 6,   /* yellow-green   — maps from old LIME         */
    MENU_LED_MOONSTONE    = 7,   /* ice blue-white — maps from old ICE          */
    MENU_LED_DIAMOND      = 8,   /* bright white   — maps from old WHITE        */
    MENU_LED_AMETHYST     = 9,   /* violet-purple  — maps from old DEEP_PURPLE  */
    MENU_LED_EMERALD      = 10,
    MENU_LED_CITRINE      = 11,
    MENU_LED_CARNELIAN    = 12,
    MENU_LED_ROSE_QUARTZ  = 13,
    MENU_LED_TANZANITE    = 14,
    MENU_LED_LABRADORITE  = 15,
    MENU_LED_CHALCOPYRITE = 16,
    MENU_LED_MALACHITE    = 17,
    MENU_LED_COUNT,
} menu_led_color_t;

typedef enum {
    SOUND_PROFILE_STANDARD = 0,
    SOUND_PROFILE_CHIRP,
    SOUND_PROFILE_SONAR,
    SOUND_PROFILE_RETRO,
    SOUND_PROFILE_ALARM,
    SOUND_PROFILE_COUNT,
} sound_profile_t;

/* Detection border animation style (touch board on-screen indicator) */
typedef enum {
    BORDER_STYLE_PULSE = 0,      /* Smooth sine-wave glow on all edges        */
    BORDER_STYLE_FLAMES,         /* Bottom-up flicker with random hot spots   */
    BORDER_STYLE_RADIATION,      /* Corner bursts that rotate clockwise       */
    BORDER_STYLE_GLITCH,         /* Random horizontal scan-line tears         */
    BORDER_STYLE_VIPER,          /* Chasing segments that orbit the perimeter */
    BORDER_STYLE_SONAR,          /* Expanding rings from the corners inward   */
    BORDER_STYLE_NONE,           /* Disabled — no on-screen border at all     */
    BORDER_STYLE_COUNT,
} border_style_t;

typedef enum {
    SHORTCUT_NONE = 0,
    SHORTCUT_NEXT_MODE,
    SHORTCUT_MODE_SELECT,
    SHORTCUT_SETTINGS,
    SHORTCUT_TOGGLE_SOUND,
    SHORTCUT_TOGGLE_LED,
    SHORTCUT_FOX_LED_MODE,
    SHORTCUT_COUNT,
} shortcut_action_t;

typedef enum {
    SERIAL_LOG_ERROR = 0,
    SERIAL_LOG_WARN,
    SERIAL_LOG_INFO,
    SERIAL_LOG_DEBUG,
    SERIAL_LOG_VERBOSE,
    SERIAL_LOG_COUNT,
} serial_log_verbosity_t;

/* ── Detection Method Bitmask ────────────────────────────── */

#define DETECT_OUI      0x01
#define DETECT_NAME     0x02
#define DETECT_CID      0x04
#define DETECT_UUID     0x08
#define DETECT_RAVEN    0x10
#define DETECT_FOX      0x20  /* Detected by fox hunter passive scan */

/* ── Protocol Identifiers ────────────────────────────────── */

#define PROTO_ASTM_WIFI  0
#define PROTO_ASTM_BLE   1
#define PROTO_DJI        2

/* ── BLE Device Record ───────────────────────────────────── */

#define MAX_BLE_DEVICES  200
#define DEVICE_NAME_LEN  32

typedef struct {
    uint8_t  mac[6];
    char     name[DEVICE_NAME_LEN];
    int8_t   rssi;
    int8_t   rssi_best;
    uint8_t  detect_flags;         /* bitmask of DETECT_* */
    uint32_t first_seen;           /* uptime ms */
    uint32_t last_seen;            /* uptime ms */
    uint16_t hit_count;
    bool     is_flock;
    bool     is_raven;
    uint8_t  raven_fw;             /* 0=unknown 1=1.1x 2=1.2x 3=1.3x */
    uint16_t company_id;
} ble_device_t;

/* ── Drone Record ────────────────────────────────────────── */

#define MAX_DRONES       64
#define MAX_DRONES_NO_SD 16
#define DRONE_ID_LEN     21
#define WIFI_MAX_AP_CLIENTS  4
#define FOX_NEARBY_MAX       40

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t last_seen;            /* uptime ms */
    char     basic_id[DRONE_ID_LEN];
    uint8_t  id_type;
    uint8_t  ua_type;
    double   lat, lon;
    float    altitude;
    float    height;
    float    speed;
    float    direction;
    double   pilot_lat, pilot_lon;
    float    pilot_alt;
    char     operator_id[DRONE_ID_LEN];
    uint8_t  protocol;             /* PROTO_ASTM_WIFI / _BLE / _DJI */
    bool     has_location;
    bool     has_pilot;
} drone_info_t;

/* ── Fox Hunter Target Registry ───────────────────────────── */

#define FOX_REGISTRY_BASE_MAX 8
#define FOX_REGISTRY_MAX     32
#define FOX_REG_LABEL_LEN   16
#define FOX_REG_NICK_LEN    24
#define FOX_REG_NOTES_LEN   96
#define FOX_REG_SECTION_LEN 12
#define MAP_PIN_LABEL_LEN   24
#define MAX_SHARED_MAP_PINS 48

typedef struct {
    uint8_t  mac[6];
    char     label[FOX_REG_LABEL_LEN];
    char     original_name[DEVICE_NAME_LEN];
    char     nickname[FOX_REG_NICK_LEN];
    char     notes[FOX_REG_NOTES_LEN];
    char     section[FOX_REG_SECTION_LEN]; /* auto|wifi|flock|drone|ble|known */
    double   pinned_lat;                   /* 0 = no pin saved */
    double   pinned_lon;
    float    pinned_radius_m;
} fox_reg_entry_t;

/* Alias: fox nearby list reuses ble_device_t storage */
typedef ble_device_t fox_nearby_t;

typedef enum {
    MAP_PIN_KIND_FLOCK = 0,
    MAP_PIN_KIND_FOX,
    MAP_PIN_KIND_DRONE,
    MAP_PIN_KIND_SELF,
} map_pin_kind_t;

typedef struct {
    uint8_t         mac[6];
    char            label[MAP_PIN_LABEL_LEN];
    double          lat;
    double          lon;
    float           radius_m;
    int8_t          rssi;
    map_pin_kind_t  kind;
} map_pin_t;

/* ── Global Application State ────────────────────────────── */

typedef struct {
    /* Mode management */
    app_mode_t      current_mode;
    app_mode_t      requested_mode;
    bool            mode_change_pending;

    /* Flock You device list */
    ble_device_t    devices[MAX_BLE_DEVICES];
    int             device_count;
    SemaphoreHandle_t device_mutex;

    /* Fox Hunter target */
    uint8_t         fox_target_mac[6];
    bool            fox_target_set;
    int8_t          fox_rssi;
    int8_t          fox_rssi_best;
    bool            fox_target_found;
    uint32_t        fox_last_seen;
    uint8_t         fox_led_mode;      /* 0=Detector, 1=Sting */
    bool            fox_registry_open;  /* true = showing registry on LCD */
    fox_reg_entry_t fox_registry[FOX_REGISTRY_MAX];
    int             fox_registry_count;

    /* Fox Hunter nearby BLE candidates (cleared on mode start) */
    fox_nearby_t    fox_nearby[FOX_NEARBY_MAX];
    int             fox_nearby_count;

    /* Fox target GPS (captured when target selected with GPS enabled) */
    double          fox_target_lat, fox_target_lon;
    float           fox_target_radius_m;
    uint16_t        fox_target_gps_samples;
    float           fox_target_weight_sum;

    /* WiFi AP connected client MACs */
    uint8_t         wifi_client_macs[WIFI_MAX_AP_CLIENTS][6];

    /* Sky Spy drone list */
    drone_info_t    drones[MAX_DRONES];
    int             drone_count;
    int             max_drones_allowed;    /* 16 without microSD, 64 with microSD */
    SemaphoreHandle_t drone_mutex;

    /* Currently tracked sky spy drone GPS (captured when selected with GPS enabled) */
    int             sky_tracked_drone_idx;  /* -1 if none */
    double          sky_tracked_lat, sky_tracked_lon;
    float           sky_tracked_radius_m;
    uint16_t        sky_tracked_gps_samples;

    /* Shared map pins mirrored from the web UI for local LCD rendering */
    map_pin_t       shared_map_pins[MAX_SHARED_MAP_PINS];
    uint8_t         shared_map_pin_count;
    SemaphoreHandle_t map_mutex;
    bool            local_map_open;
    uint8_t         local_map_zoom_idx;

    /* Hardware preferences (persisted via NVS) */
    uint8_t         lcd_brightness;   /* 0-255 */
    bool            sound_enabled;
    bool            led_enabled;
    bool            ap_broadcast_enabled;
    bool            single_ap_name_enabled; /* true = UniSpy-C6 for every mode */
    uint16_t        display_sleep_timeout_sec; /* 0 disables sleep */
    uint8_t         menu_led_color;
    uint8_t         border_style;         /* active border_style_t for the current screen */
    uint8_t         detect_behavior;      /* legacy persisted key, no longer user-facing */
    uint8_t         detect_flock_low;     /* menu_led_color_t: Flock board LED color */
    uint8_t         detect_flock_high;    /* border_style_t:   Flock display effect */
    uint8_t         detect_flock_custom;  /* menu_led_color_t: Flock display accent color */
    uint8_t         detect_fox_low;       /* menu_led_color_t: Fox board LED color */
    uint8_t         detect_fox_high;      /* border_style_t:   Fox display effect */
    uint8_t         detect_fox_custom;    /* menu_led_color_t: Fox display accent color */
    uint8_t         detect_sky_low;       /* menu_led_color_t: Sky board LED color */
    uint8_t         detect_sky_high;      /* border_style_t:   Sky display effect */
    uint8_t         detect_sky_custom;    /* menu_led_color_t: Sky display accent color */
    uint8_t         sound_profile_flock;
    uint8_t         sound_profile_fox;
    uint8_t         sound_profile_sky;
    uint8_t         shortcut_mode_btn;
    uint8_t         shortcut_action_btn;
    uint8_t         shortcut_back_btn;
    bool            use_microsd_logs;
    bool            advanced_logging_enabled;
    bool            log_general_enabled;
    bool            log_flock_enabled;
    bool            log_fox_enabled;
    bool            log_sky_enabled;
    bool            log_gps_enabled;
    bool            log_saved_fox_enabled;
    bool            gps_diagnostics_enabled;
    bool            web_diagnostics_enabled;
    bool            advanced_serial_logging_enabled;
    uint8_t         serial_log_verbosity;
    bool            gps_tagging_enabled;
    bool            gps_client_ready;      /* phone/browser currently able to send GPS */
    uint32_t        gps_client_ready_ms;   /* last readiness update timestamp */

    /* Web server handle */
    httpd_handle_t  http_server;

    /* Runtime stats */
    uint32_t        uptime_sec;
    uint32_t        free_heap;
    uint8_t         wifi_clients;
    bool            microsd_available;

    /* UI navigation (button-driven cursor) */
    int             ui_cursor;        /* current selection index  */
    int             ui_item_count;    /* selectable items in view */
    bool            log_viewer_open;  /* true = showing log viewer on LCD */
    int             log_scroll_pos;   /* scroll offset in log viewer */
    uint32_t        ui_refresh_token;
    uint32_t        last_input_ms;
    bool            display_sleeping;
} app_state_t;

/* ── Global Instance ─────────────────────────────────────── */

extern app_state_t g_app;

/* ── Utility Functions ───────────────────────────────────── */

void     app_state_init(void);
void     app_apply_runtime_logging_prefs(void);
void     app_mode_ap_credentials(app_mode_t mode, const char **ssid, const char **pass, uint8_t *channel);
float    app_detection_behavior_strength(app_mode_t mode, float detected_strength);
void     app_palette_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b);
uint16_t app_palette_rgb565(uint8_t idx);
uint8_t  app_mode_led_color(app_mode_t mode);
uint8_t  app_mode_display_style(app_mode_t mode);
uint8_t  app_mode_display_color(app_mode_t mode);
void     app_apply_mode_visual_prefs(app_mode_t mode);
uint32_t uptime_ms(void);
void     mac_to_str(const uint8_t mac[6], char *buf, size_t buf_len);
bool     mac_equal(const uint8_t a[6], const uint8_t b[6]);
int      mac_from_str(const char *str, uint8_t mac[6]);
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);

/* Compile-time RGB565 with byte-swap for static const initializers */
#define RGB565_CONST(r, g, b) ((uint16_t)(                    \
    (((r) & 0xF8) | (((g) >> 5) & 0x07)) |                    \
    (((((g) & 0x1C) << 3) | (((b) >> 3) & 0x1F)) << 8)))

#ifdef __cplusplus
}
#endif
