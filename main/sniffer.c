/*
 * OUI-Spy C6 — WiFi promiscuous sniffer for drone Remote ID
 *
 * Captures management frames (beacons + action frames) and checks for:
 *   1) ASTM F3411 NAN action frames (OUI 50:6F:9A type 0x13)
 *   2) ASTM F3411 beacon vendor IEs (OUI 50:6F:9A type 0x0D)
 *   3) DJI DroneID beacon vendor IEs (OUI 26:37:12)
 *
 * The promiscuous callback is minimal — it copies the frame and posts
 * to a FreeRTOS queue for processing off the WiFi driver task.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sniffer.h"
#include "app_common.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <string.h>

static const char *TAG = "sniffer";

#define SNIFFER_QUEUE_SIZE 16
#define MAX_FRAME_LEN      384

typedef struct {
    uint8_t  frame[MAX_FRAME_LEN];
    uint16_t len;
    int8_t   rssi;
} sniff_item_t;

static QueueHandle_t s_queue = NULL;
static sniffer_drone_cb_t s_cb = NULL;
static TaskHandle_t s_task = NULL;

/* ── OUI constants ── */
static const uint8_t OUI_WFA[3]  = {0x50, 0x6F, 0x9A}; /* WiFi Alliance */
static const uint8_t OUI_DJI[3]  = {0x26, 0x37, 0x12}; /* DJI */

/* NAN Service ID for OpenDroneID = SHA-256("org.opendroneid.remoteid") truncated */
static const uint8_t NAN_SVC_ID[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};

/* ── Promiscuous callback (runs in WiFi driver context — keep short!) ── */
static void IRAM_ATTR promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len > MAX_FRAME_LEN) len = MAX_FRAME_LEN;

    sniff_item_t item;
    item.len  = len;
    item.rssi = pkt->rx_ctrl.rssi;
    memcpy(item.frame, pkt->payload, len);

    /* Non-blocking post — drop if queue full */
    xQueueSendFromISR(s_queue, &item, NULL);
}

/* ── Parse beacon vendor-specific IEs ── */
static void parse_beacon_ies(const uint8_t *frame, uint16_t len, int8_t rssi)
{
    /* 802.11 header = 24 bytes, fixed beacon body = 12 bytes */
    if (len < 36) return;
    const uint8_t *src_mac = &frame[10]; /* addr2 = SA */

    uint16_t offset = 36;
    while (offset + 2 <= len) {
        uint8_t ie_tag = frame[offset];
        uint8_t ie_len = frame[offset + 1];
        if (offset + 2 + ie_len > len) break;

        if (ie_tag == 0xDD && ie_len >= 4) {
            const uint8_t *ie_data = &frame[offset + 2];
            /* ASTM F3411 beacon: OUI 50:6F:9A type 0x0D */
            if (ie_len >= 5 &&
                memcmp(ie_data, OUI_WFA, 3) == 0 && ie_data[3] == 0x0D) {
                if (s_cb) {
                    s_cb(src_mac, rssi, &ie_data[4], ie_len - 4, PROTO_ASTM_WIFI);
                }
            }
            /* DJI DroneID: OUI 26:37:12 */
            if (memcmp(ie_data, OUI_DJI, 3) == 0) {
                if (s_cb) {
                    s_cb(src_mac, rssi, ie_data, ie_len, PROTO_DJI);
                }
            }
        }
        offset += 2 + ie_len;
    }
}

/* ── Parse NAN action frames ── */
static void parse_action_frame(const uint8_t *frame, uint16_t len, int8_t rssi)
{
    /* 802.11 header = 24 bytes */
    if (len < 30) return;
    const uint8_t *src_mac = &frame[10];
    const uint8_t *body = &frame[24];
    uint16_t body_len = len - 24;

    /* Public Action frame: category=0x04, action=0x09 (vendor specific) */
    if (body_len < 8) return;
    if (body[0] != 0x04 || body[1] != 0x09) return;

    /* OUI check for WiFi Alliance NAN */
    if (memcmp(&body[2], OUI_WFA, 3) != 0) return;
    if (body[5] != 0x13) return; /* NAN OUI Type */

    /* Search for Service Descriptor with our Service ID */
    uint16_t pos = 6;
    while (pos + 8 <= body_len) {
        /* Look for the ODID service ID in the NAN attributes */
        if (pos + 6 <= body_len &&
            memcmp(&body[pos], NAN_SVC_ID, 6) == 0) {
            /* Found ODID NAN payload — pass remainder as message pack */
            if (s_cb) {
                s_cb(src_mac, rssi, &body[pos], body_len - pos, PROTO_ASTM_WIFI);
            }
            return;
        }
        pos++;
    }
}

/* ── Processing task ── */
static void sniffer_task(void *arg)
{
    sniff_item_t item;
    while (1) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (item.len < 2) continue;

            uint16_t fc = item.frame[0] | (item.frame[1] << 8);
            uint8_t subtype = (fc >> 4) & 0x0F;

            switch (subtype) {
            case 0x08: /* Beacon */
                parse_beacon_ies(item.frame, item.len, item.rssi);
                break;
            case 0x05: /* Probe Response — may also carry vendor IEs */
                parse_beacon_ies(item.frame, item.len, item.rssi);
                break;
            case 0x0D: /* Action */
                parse_action_frame(item.frame, item.len, item.rssi);
                break;
            default:
                break;
            }
        }
    }
}

void sniffer_init(void)
{
    s_queue = xQueueCreate(SNIFFER_QUEUE_SIZE, sizeof(sniff_item_t));
    configASSERT(s_queue);
}

void sniffer_start(sniffer_drone_cb_t cb)
{
    s_cb = cb;

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    if (xTaskCreate(sniffer_task, "sniffer", TASK_STACK_SNIFFER, NULL, 6, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sniffer task");
        s_task = NULL;
        return;
    }
    ESP_LOGI(TAG, "Sniffer started");
}

void sniffer_stop(void)
{
    esp_wifi_set_promiscuous(false);
    s_cb = NULL;
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    ESP_LOGI(TAG, "Sniffer stopped");
}
