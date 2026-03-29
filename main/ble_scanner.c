/*
 * OUI-Spy C6 — BLE scanner using NimBLE host stack
 * SPDX-License-Identifier: MIT
 */
#include "ble_scanner.h"
#include "app_common.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include <string.h>

static const char *TAG = "ble_scan";
static ble_scan_cb_t s_user_cb = NULL;
static bool s_running = false;
static bool s_initialized = false;
static SemaphoreHandle_t s_sync_sem = NULL;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static struct ble_gap_disc_params s_last_params = {0};

/* ── Gap event callback ── */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_gap_disc_desc *desc = &event->disc;

        /* Extract device name from ad fields */
        struct ble_hs_adv_fields fields;
        uint8_t name_buf[32] = {0};
        uint8_t name_len = 0;
        if (ble_hs_adv_parse_fields(&fields, desc->data,
                                     desc->length_data) == 0) {
            if (fields.name != NULL && fields.name_len > 0) {
                name_len = (fields.name_len < 31) ? fields.name_len : 31;
                memcpy(name_buf, fields.name, name_len);
            }
        }

        if (s_user_cb) {
            s_user_cb(desc->addr.val, desc->rssi,
                      desc->data, desc->length_data,
                      name_buf, name_len);
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        /* Restart scan automatically if still wanted */
        if (s_running) {
            ESP_LOGW(TAG, "Scan completed, restarting");
            int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER,
                                  &s_last_params, gap_event, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Scan restart failed: %d", rc);
            }
        }
    }
    return 0;
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "NimBLE host synced, addr=%02X:%02X:%02X:%02X:%02X:%02X type=%u",
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0], s_own_addr_type);
    } else {
        ESP_LOGW(TAG, "NimBLE host synced, but address copy failed: %d", rc);
    }

    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
}

void ble_scanner_init(void)
{
    if (s_initialized) return;

    s_sync_sem = xSemaphoreCreateBinary();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(ble_host_task);

    /* Wait up to 2 seconds for host to sync with controller */
    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "NimBLE host sync timed out");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "BLE scanner initialized");
}

void ble_scanner_start(ble_scan_cb_t cb, uint16_t interval_ms,
                       uint16_t window_ms, bool passive)
{
    s_user_cb = cb;
    s_running = true;

    struct ble_gap_disc_params params = {
        .passive           = passive ? 1 : 0,
        .itvl              = (uint16_t)(interval_ms * 1000 / 625),
        .window            = (uint16_t)(window_ms * 1000 / 625),
        .filter_duplicates = 0,
        .limited           = 0,
    };
    s_last_params = params;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER,
                          &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE scan start failed: %d", rc);
        s_running = false;
    } else {
        ESP_LOGI(TAG, "BLE scan started itvl=%dms win=%dms", interval_ms, window_ms);
    }
}

void ble_scanner_stop(void)
{
    s_running = false;
    ble_gap_disc_cancel();
    s_user_cb = NULL;
    ESP_LOGI(TAG, "BLE scan stopped");
}

bool ble_scanner_is_running(void)
{
    return s_running;
}
