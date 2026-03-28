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

/* ── Pin Definitions ─────────────────────────────────────── */

/* Onboard (fixed by PCB) */
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

/* External hardware (active-low buttons, active buzzer/speaker) */
#define PIN_BTN_MODE    10   /* Cycle modes       – wire between GPIO10 and GND */
#define PIN_BTN_ACTION  11   /* Contextual action  – wire between GPIO11 and GND */
#define PIN_BTN_BACK    19   /* Back / cancel      – wire between GPIO19 and GND */
#define PIN_BUZZER      18   /* Piezo buzzer (+)   – wire + to GPIO18, – to GND  */

/* ── Display Constants ───────────────────────────────────── */

#define LCD_H_RES       172
#define LCD_V_RES       320
#define LCD_COL_OFFSET  34
#define LCD_ROW_OFFSET  0

#define TASK_STACK_BUTTON   3072
#define TASK_STACK_UI       6144
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
    MENU_LED_GOLD = 0,
    MENU_LED_MINT,
    MENU_LED_SKY,
    MENU_LED_AMBER,
    MENU_LED_MAGENTA,
    MENU_LED_RUBY,
    MENU_LED_LIME,
    MENU_LED_ICE,
    MENU_LED_WHITE,
    MENU_LED_DEEP_PURPLE,
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

#define MAX_DRONES       16
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

#define FOX_REGISTRY_MAX     8
#define FOX_REG_LABEL_LEN   16
#define FOX_REG_NICK_LEN    24
#define FOX_REG_NOTES_LEN   96
#define FOX_REG_SECTION_LEN 12

typedef struct {
    uint8_t  mac[6];
    char     label[FOX_REG_LABEL_LEN];
    char     original_name[DEVICE_NAME_LEN];
    char     nickname[FOX_REG_NICK_LEN];
    char     notes[FOX_REG_NOTES_LEN];
    char     section[FOX_REG_SECTION_LEN]; /* auto|wifi|flock|drone|ble|known */
} fox_reg_entry_t;

/* Alias: fox nearby list reuses ble_device_t storage */
typedef ble_device_t fox_nearby_t;

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

    /* WiFi AP connected client MACs */
    uint8_t         wifi_client_macs[WIFI_MAX_AP_CLIENTS][6];

    /* Sky Spy drone list */
    drone_info_t    drones[MAX_DRONES];
    int             drone_count;
    SemaphoreHandle_t drone_mutex;

    /* Currently tracked sky spy drone GPS (captured when selected with GPS enabled) */
    int             sky_tracked_drone_idx;  /* -1 if none */
    double          sky_tracked_lat, sky_tracked_lon;

    /* Hardware preferences (persisted via NVS) */
    uint8_t         lcd_brightness;   /* 0-255 */
    bool            sound_enabled;
    bool            led_enabled;
    bool            ap_broadcast_enabled;
    bool            single_ap_name_enabled; /* true = UniSpy-C6 for every mode */
    uint16_t        display_sleep_timeout_sec; /* 0 disables sleep */
    uint8_t         menu_led_color;
    uint8_t         sound_profile_flock;
    uint8_t         sound_profile_fox;
    uint8_t         sound_profile_sky;
    uint8_t         shortcut_mode_btn;
    uint8_t         shortcut_action_btn;
    uint8_t         shortcut_back_btn;
    bool            use_microsd_logs;
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
    uint32_t        last_input_ms;
    bool            display_sleeping;
} app_state_t;

/* ── Global Instance ─────────────────────────────────────── */

extern app_state_t g_app;

/* ── Utility Functions ───────────────────────────────────── */

void     app_state_init(void);
void     app_mode_ap_credentials(app_mode_t mode, const char **ssid, const char **pass, uint8_t *channel);
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
