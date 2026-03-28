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
    MODE_COUNT
} app_mode_t;

/* ── Detection Method Bitmask ────────────────────────────── */

#define DETECT_OUI      0x01
#define DETECT_NAME     0x02
#define DETECT_CID      0x04
#define DETECT_UUID     0x08
#define DETECT_RAVEN    0x10

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

    /* Sky Spy drone list */
    drone_info_t    drones[MAX_DRONES];
    int             drone_count;
    SemaphoreHandle_t drone_mutex;

    /* Hardware preferences (persisted via NVS) */
    uint8_t         lcd_brightness;   /* 0-255 */
    bool            sound_enabled;
    bool            led_enabled;

    /* Web server handle */
    httpd_handle_t  http_server;

    /* Runtime stats */
    uint32_t        uptime_sec;
    uint32_t        free_heap;
    uint8_t         wifi_clients;

    /* UI navigation (button-driven cursor) */
    int             ui_cursor;        /* current selection index  */
    int             ui_item_count;    /* selectable items in view */
} app_state_t;

/* ── Global Instance ─────────────────────────────────────── */

extern app_state_t g_app;

/* ── Utility Functions ───────────────────────────────────── */

void     app_state_init(void);
uint32_t uptime_ms(void);
void     mac_to_str(const uint8_t mac[6], char *buf, size_t buf_len);
bool     mac_equal(const uint8_t a[6], const uint8_t b[6]);
int      mac_from_str(const char *str, uint8_t mac[6]);
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
