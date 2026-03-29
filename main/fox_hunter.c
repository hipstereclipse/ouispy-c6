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
#include "storage_ext.h"
#include <string.h>
#include <math.h>
#include <limits.h>

static const char *TAG = "fox";
static TaskHandle_t s_beep_task = NULL;

#define TARGET_LOST_MS  15000
#define TARGET_LOST_SCREEN_DELAY_MS  22000
#define FOX_NEARBY_SECTION_MAX     4
#define FOX_FOLLOWING_SECTION_MAX  4
#define FOX_DETECTED_SECTION_MAX   6
#define FOX_NEARBY_RECENT_MS       8000
#define FOX_FOLLOWING_RECENT_MS    12000
#define FOX_FOLLOWING_MIN_SEEN_MS  20000
#define FOX_DETECTED_RECENT_MS     30000
#define FOX_GPS_READY_TIMEOUT_MS    20000

typedef struct {
    int nearby_count;
    int following_count;
    int detected_count;
    ble_device_t nearby[FOX_NEARBY_SECTION_MAX];
    ble_device_t following[FOX_FOLLOWING_SECTION_MAX];
    ble_device_t detected[FOX_DETECTED_SECTION_MAX];
} fox_candidate_snapshot_t;

static void fox_log_identity_event(const char *event,
                                   const uint8_t mac[6],
                                   const char *label,
                                   const char *extra)
{
    char mac_str[18] = "none";
    char msg[192];

    if (mac) {
        mac_to_str(mac, mac_str, sizeof(mac_str));
    }

    if (label && label[0] && extra && extra[0]) {
        snprintf(msg, sizeof(msg), "%s mac=%s label=%s %s", event, mac_str, label, extra);
    } else if (label && label[0]) {
        snprintf(msg, sizeof(msg), "%s mac=%s label=%s", event, mac_str, label);
    } else if (extra && extra[0]) {
        snprintf(msg, sizeof(msg), "%s mac=%s %s", event, mac_str, extra);
    } else {
        snprintf(msg, sizeof(msg), "%s mac=%s", event, mac_str);
    }

    storage_ext_append_identity("fox", msg);
}

static inline bool fox_gps_tag_active(uint32_t now_ms)
{
    bool gps_ready_fresh = g_app.gps_client_ready
                        && (now_ms > g_app.gps_client_ready_ms)
                        && ((now_ms - g_app.gps_client_ready_ms) <= FOX_GPS_READY_TIMEOUT_MS);
    return g_app.gps_tagging_enabled && gps_ready_fresh && (g_app.wifi_clients > 0);
}

static void fox_draw_logging_badge(uint16_t bg)
{
    bool logging_active = storage_ext_logging_active();
    uint16_t fg = logging_active ? rgb565(74, 222, 128) : rgb565(239, 68, 68);
    display_draw_rect(LCD_H_RES - 14, DISPLAY_STATUS_TEXT_Y, 6, 8, bg);
    display_draw_text(LCD_H_RES - 14, DISPLAY_STATUS_TEXT_Y, "l", fg, bg);
}

int fox_hunter_registry_capacity(void)
{
    return storage_ext_is_available() ? FOX_REGISTRY_MAX : FOX_REGISTRY_BASE_MAX;
}

static void fox_target_display_name(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!g_app.fox_target_set) return;

    for (int i = 0; i < g_app.fox_registry_count; i++) {
        fox_reg_entry_t *e = &g_app.fox_registry[i];
        if (!mac_equal(e->mac, g_app.fox_target_mac)) continue;
        if (e->nickname[0]) {
            snprintf(out, out_len, "%s", e->nickname);
            return;
        }
        if (e->label[0]) {
            snprintf(out, out_len, "%s", e->label);
            return;
        }
        if (e->original_name[0]) {
            snprintf(out, out_len, "%s", e->original_name);
            return;
        }
    }

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < g_app.fox_nearby_count; i++) {
            ble_device_t *d = &g_app.fox_nearby[i];
            if (mac_equal(d->mac, g_app.fox_target_mac) && d->name[0]) {
                snprintf(out, out_len, "%s", d->name);
                xSemaphoreGive(g_app.device_mutex);
                return;
            }
        }

        for (int i = 0; i < g_app.device_count; i++) {
            ble_device_t *d = &g_app.devices[i];
            if (mac_equal(d->mac, g_app.fox_target_mac) && d->name[0]) {
                snprintf(out, out_len, "%s", d->name);
                xSemaphoreGive(g_app.device_mutex);
                return;
            }
        }
        xSemaphoreGive(g_app.device_mutex);
    }
}

static bool fox_idx_in_list(const int *list, int list_count, int value)
{
    for (int i = 0; i < list_count; i++) {
        if (list[i] == value) return true;
    }
    return false;
}

/* Requires g_app.device_mutex held while g_app.fox_nearby is being examined. */
static int fox_build_likely_nearby_locked(int *out_idx, int out_max, uint32_t now)
{
    int count = 0;

    for (int i = 0; i < g_app.fox_nearby_count; i++) {
        ble_device_t *d = &g_app.fox_nearby[i];
        uint32_t age_ms = (now > d->last_seen) ? (now - d->last_seen) : 0;

        if (age_ms > FOX_NEARBY_RECENT_MS) continue;
        if (d->rssi < -95) continue;

        if (count < out_max) {
            out_idx[count++] = i;
            continue;
        }

        int weakest_pos = 0;
        for (int j = 1; j < count; j++) {
            ble_device_t *a = &g_app.fox_nearby[out_idx[j]];
            ble_device_t *b = &g_app.fox_nearby[out_idx[weakest_pos]];
            if (a->rssi < b->rssi ||
                (a->rssi == b->rssi && a->rssi_best < b->rssi_best)) {
                weakest_pos = j;
            }
        }

        ble_device_t *cur = &g_app.fox_nearby[i];
        ble_device_t *weakest = &g_app.fox_nearby[out_idx[weakest_pos]];
        if (cur->rssi > weakest->rssi ||
            (cur->rssi == weakest->rssi && cur->rssi_best > weakest->rssi_best)) {
            out_idx[weakest_pos] = i;
        }
    }

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            ble_device_t *a = &g_app.fox_nearby[out_idx[i]];
            ble_device_t *b = &g_app.fox_nearby[out_idx[j]];
            bool swap = false;
            if (b->rssi > a->rssi) swap = true;
            else if (b->rssi == a->rssi && b->rssi_best > a->rssi_best) swap = true;
            else if (b->rssi == a->rssi && b->rssi_best == a->rssi_best && b->hit_count > a->hit_count) swap = true;
            if (swap) {
                int tmp = out_idx[i];
                out_idx[i] = out_idx[j];
                out_idx[j] = tmp;
            }
        }
    }

    return count;
}

static int32_t fox_follow_score(const ble_device_t *d, uint32_t now)
{
    uint32_t age_ms = (now > d->last_seen) ? (now - d->last_seen) : 0;
    if (age_ms > FOX_FOLLOWING_RECENT_MS) return INT32_MIN;

    uint32_t seen_ms = (d->last_seen > d->first_seen) ? (d->last_seen - d->first_seen) : 0;
    if (seen_ms < FOX_FOLLOWING_MIN_SEEN_MS) return INT32_MIN;
    if (d->hit_count < 4) return INT32_MIN;
    if (d->rssi < -82 && d->rssi_best < -78) return INT32_MIN;

    int32_t seen_score = (int32_t)(seen_ms / 1000U);
    if (seen_score > 900) seen_score = 900;

    int32_t rssi_score = d->rssi + 95;
    if (rssi_score < 0) rssi_score = 0;
    if (rssi_score > 80) rssi_score = 80;

    int32_t best_score = d->rssi_best + 95;
    if (best_score < 0) best_score = 0;
    if (best_score > 80) best_score = 80;

    int32_t hit_score = d->hit_count;
    if (hit_score > 600) hit_score = 600;

    int32_t age_penalty = (int32_t)(age_ms / 250U);
    if (age_penalty > 120) age_penalty = 120;

    return (seen_score * 4) + (rssi_score * 3) + (best_score * 2) + (hit_score / 4) - (age_penalty * 3);
}

static int32_t fox_detected_score(const ble_device_t *d, uint32_t now)
{
    uint32_t age_ms = (now > d->last_seen) ? (now - d->last_seen) : 0;
    if (age_ms > FOX_DETECTED_RECENT_MS) return INT32_MIN;

    int32_t recency = 300 - (int32_t)(age_ms / 100U);
    if (recency < 0) recency = 0;

    int32_t rssi_score = d->rssi + 100;
    if (rssi_score < 0) rssi_score = 0;
    if (rssi_score > 85) rssi_score = 85;

    int32_t hit_score = d->hit_count;
    if (hit_score > 200) hit_score = 200;

    return (recency * 3) + (rssi_score * 2) + hit_score;
}

/* Requires g_app.device_mutex held while g_app.fox_nearby is being examined. */
static int fox_build_likely_following_locked(int *out_idx, int out_max,
                                             const int *exclude_idx, int exclude_count,
                                             uint32_t now)
{
    int cand_idx[FOX_NEARBY_MAX] = {0};
    int32_t cand_score[FOX_NEARBY_MAX] = {0};
    int cand_count = 0;

    for (int i = 0; i < g_app.fox_nearby_count; i++) {
        if (fox_idx_in_list(exclude_idx, exclude_count, i)) continue;

        int32_t score = fox_follow_score(&g_app.fox_nearby[i], now);
        if (score == INT32_MIN) continue;

        cand_idx[cand_count] = i;
        cand_score[cand_count] = score;
        cand_count++;
    }

    for (int i = 0; i < cand_count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < cand_count; j++) {
            if (cand_score[j] > cand_score[best]) {
                best = j;
            } else if (cand_score[j] == cand_score[best]) {
                ble_device_t *dj = &g_app.fox_nearby[cand_idx[j]];
                ble_device_t *db = &g_app.fox_nearby[cand_idx[best]];
                if (dj->rssi > db->rssi ||
                    (dj->rssi == db->rssi && dj->hit_count > db->hit_count)) {
                    best = j;
                }
            }
        }
        if (best != i) {
            int tmp_i = cand_idx[i];
            cand_idx[i] = cand_idx[best];
            cand_idx[best] = tmp_i;
            int32_t tmp_s = cand_score[i];
            cand_score[i] = cand_score[best];
            cand_score[best] = tmp_s;
        }
    }

    int out_count = (cand_count < out_max) ? cand_count : out_max;
    for (int i = 0; i < out_count; i++) {
        out_idx[i] = cand_idx[i];
    }

    return out_count;
}

/* Requires g_app.device_mutex held while g_app.fox_nearby is being examined. */
static int fox_build_detected_locked(int *out_idx, int out_max,
                                     const int *exclude_a, int exclude_a_count,
                                     const int *exclude_b, int exclude_b_count,
                                     uint32_t now)
{
    int cand_idx[FOX_NEARBY_MAX] = {0};
    int32_t cand_score[FOX_NEARBY_MAX] = {0};
    int cand_count = 0;

    for (int i = 0; i < g_app.fox_nearby_count; i++) {
        if (fox_idx_in_list(exclude_a, exclude_a_count, i)) continue;
        if (fox_idx_in_list(exclude_b, exclude_b_count, i)) continue;

        int32_t score = fox_detected_score(&g_app.fox_nearby[i], now);
        if (score == INT32_MIN) continue;

        cand_idx[cand_count] = i;
        cand_score[cand_count] = score;
        cand_count++;
    }

    for (int i = 0; i < cand_count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < cand_count; j++) {
            if (cand_score[j] > cand_score[best]) {
                best = j;
            } else if (cand_score[j] == cand_score[best]) {
                ble_device_t *dj = &g_app.fox_nearby[cand_idx[j]];
                ble_device_t *db = &g_app.fox_nearby[cand_idx[best]];
                uint32_t age_j = (now > dj->last_seen) ? (now - dj->last_seen) : 0;
                uint32_t age_b = (now > db->last_seen) ? (now - db->last_seen) : 0;
                if (age_j < age_b || (age_j == age_b && dj->rssi > db->rssi)) {
                    best = j;
                }
            }
        }
        if (best != i) {
            int tmp_i = cand_idx[i];
            cand_idx[i] = cand_idx[best];
            cand_idx[best] = tmp_i;
            int32_t tmp_s = cand_score[i];
            cand_score[i] = cand_score[best];
            cand_score[best] = tmp_s;
        }
    }

    int out_count = (cand_count < out_max) ? cand_count : out_max;
    for (int i = 0; i < out_count; i++) {
        out_idx[i] = cand_idx[i];
    }

    return out_count;
}

/* Requires g_app.device_mutex held while g_app.fox_nearby is being examined. */
static void fox_build_candidate_snapshot_locked(fox_candidate_snapshot_t *snap, uint32_t now)
{
    int nearby_idx[FOX_NEARBY_SECTION_MAX] = {0};
    int following_idx[FOX_FOLLOWING_SECTION_MAX] = {0};
    int detected_idx[FOX_DETECTED_SECTION_MAX] = {0};

    memset(snap, 0, sizeof(*snap));
    snap->nearby_count = fox_build_likely_nearby_locked(nearby_idx, FOX_NEARBY_SECTION_MAX, now);
    snap->following_count = fox_build_likely_following_locked(
        following_idx,
        FOX_FOLLOWING_SECTION_MAX,
        nearby_idx,
        snap->nearby_count,
        now
    );
    snap->detected_count = fox_build_detected_locked(
        detected_idx,
        FOX_DETECTED_SECTION_MAX,
        nearby_idx,
        snap->nearby_count,
        following_idx,
        snap->following_count,
        now
    );

    for (int i = 0; i < snap->nearby_count; i++) {
        snap->nearby[i] = g_app.fox_nearby[nearby_idx[i]];
    }
    for (int i = 0; i < snap->following_count; i++) {
        snap->following[i] = g_app.fox_nearby[following_idx[i]];
    }
    for (int i = 0; i < snap->detected_count; i++) {
        snap->detected[i] = g_app.fox_nearby[detected_idx[i]];
    }
}

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

static int rssi_to_percent(int8_t rssi)
{
    int pct = (rssi + 100) * 100 / 80;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static const char *rssi_to_cadence_label(int8_t rssi)
{
    int interval = rssi_to_interval(rssi);
    if (interval <= 30) return "Machine";
    if (interval <= 90) return "Rapid";
    if (interval <= 200) return "Fast";
    if (interval <= 380) return "Steady";
    if (interval <= 600) return "Slow";
    return "Faint";
}

static const char *rssi_to_proximity_label(int8_t rssi)
{
    if (rssi >= -45) return "VERY CLOSE";
    if (rssi >= -60) return "CLOSE";
    if (rssi >= -72) return "NEAR";
    if (rssi >= -84) return "FAR";
    return "VERY FAR";
}

int fox_hunter_registry_view_count(void)
{
    int nearby = 0;
    int following = 0;
    int detected = 0;

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        uint32_t now = uptime_ms();
        int nearby_idx[FOX_NEARBY_SECTION_MAX] = {0};
        int following_idx[FOX_FOLLOWING_SECTION_MAX] = {0};
        int detected_idx[FOX_DETECTED_SECTION_MAX] = {0};

        nearby = fox_build_likely_nearby_locked(nearby_idx, FOX_NEARBY_SECTION_MAX, now);
        following = fox_build_likely_following_locked(
            following_idx,
            FOX_FOLLOWING_SECTION_MAX,
            nearby_idx,
            nearby,
            now
        );
        detected = fox_build_detected_locked(
            detected_idx,
            FOX_DETECTED_SECTION_MAX,
            nearby_idx,
            nearby,
            following_idx,
            following,
            now
        );

        xSemaphoreGive(g_app.device_mutex);
    }

    return g_app.fox_registry_count + nearby + following + detected;
}

/* ── Scan callback ── */
static void fox_scan_cb(const uint8_t *addr, int8_t rssi,
                        const uint8_t *adv_data, uint8_t adv_len,
                        const uint8_t *name, uint8_t name_len)
{
    /* ── Track active target ── */
    if (g_app.fox_target_set && mac_equal(addr, g_app.fox_target_mac)) {
        g_app.fox_rssi = rssi;
        if (rssi > g_app.fox_rssi_best) g_app.fox_rssi_best = rssi;
        g_app.fox_target_found = true;
        g_app.fox_last_seen = uptime_ms();
    }

    /* ── Accumulate all nearby BLE devices for candidate selection ── */
    char name_str[DEVICE_NAME_LEN] = {0};
    if (name_len > 0) {
        int n = (name_len < DEVICE_NAME_LEN - 1) ? name_len : DEVICE_NAME_LEN - 1;
        memcpy(name_str, name, n);
    }

    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    uint32_t now = uptime_ms();
    int idx = -1;
    for (int i = 0; i < g_app.fox_nearby_count; i++) {
        if (mac_equal(g_app.fox_nearby[i].mac, addr)) { idx = i; break; }
    }

    if (idx >= 0) {
        ble_device_t *d = &g_app.fox_nearby[idx];
        d->rssi      = rssi;
        if (rssi > d->rssi_best) d->rssi_best = rssi;
        d->last_seen = now;
        if (d->hit_count < 65535) d->hit_count++;
        if (name_str[0] && d->name[0] == '\0')
            strncpy(d->name, name_str, DEVICE_NAME_LEN - 1);
    } else if (g_app.fox_nearby_count < FOX_NEARBY_MAX) {
        ble_device_t *d = &g_app.fox_nearby[g_app.fox_nearby_count];
        memset(d, 0, sizeof(*d));
        memcpy(d->mac, addr, 6);
        strncpy(d->name, name_str, DEVICE_NAME_LEN - 1);
        d->rssi       = rssi;
        d->rssi_best  = rssi;
        d->first_seen = now;
        d->last_seen  = now;
        d->hit_count  = 1;
        g_app.fox_nearby_count++;
    } else {
        /* Array full — evict the stalest entry to make room */
        int victim = 0;
        uint32_t oldest = g_app.fox_nearby[0].last_seen;
        for (int i = 1; i < g_app.fox_nearby_count; i++) {
            if (g_app.fox_nearby[i].last_seen < oldest) {
                oldest = g_app.fox_nearby[i].last_seen;
                victim = i;
            }
        }
        /* Only evict if the victim is staler than the new device */
        if (oldest < now) {
            ble_device_t *d = &g_app.fox_nearby[victim];
            memset(d, 0, sizeof(*d));
            memcpy(d->mac, addr, 6);
            strncpy(d->name, name_str, DEVICE_NAME_LEN - 1);
            d->rssi       = rssi;
            d->rssi_best  = rssi;
            d->first_seen = now;
            d->last_seen  = now;
            d->hit_count  = 1;
        }
    }

    xSemaphoreGive(g_app.device_mutex);
}

/* ── Beep + display loop -- Amber hunting aesthetic ── */
static void fox_beep_task(void *arg)
{
    int frame = 0;

    /* Snapshot state for dirty-check: avoid full redraw when nothing visual changed */
    bool last_target_set = false;
    bool last_target_visible = false;
    int8_t last_rssi = -128;
    uint8_t last_led_mode = 0xFF;
    int last_wifi_clients = -1;
    bool last_registry_open = false;
    int last_gps_active = -1;
    int last_logging_active = -1;
    int last_logging_blocked = -1;
    int last_show_lost_screen = -1;
    uint32_t last_ui_refresh_token = UINT32_MAX;
    char last_target_header[DEVICE_NAME_LEN] = {0};
    int last_reg_cursor = -1;
    int last_reg_count = -1;
    bool display_drawn = false;     /* force first draw */

    while (g_app.current_mode == MODE_FOX_HUNTER) {
        frame++;
        uint32_t now = uptime_ms();
        uint32_t time_since_seen_ms = (g_app.fox_last_seen > 0 && now > g_app.fox_last_seen)
                          ? (now - g_app.fox_last_seen)
                          : 0;
        bool target_visible = g_app.fox_target_found && (time_since_seen_ms < TARGET_LOST_MS);
        bool show_lost_screen = g_app.fox_target_found && (time_since_seen_ms >= TARGET_LOST_SCREEN_DELAY_MS);
        char target_header[DEVICE_NAME_LEN] = {0};
        fox_target_display_name(target_header, sizeof(target_header));
        bool gps_tag_active = fox_gps_tag_active(now);
        bool logging_active = storage_ext_logging_active();
        bool logging_blocked = storage_ext_logging_blocked();

        int8_t live_rssi = target_visible ? g_app.fox_rssi : -128;

        bool view_switched = (g_app.fox_registry_open != last_registry_open);
        bool target_state_switched = (g_app.fox_target_set != last_target_set)
                     || (target_visible != last_target_visible)
                     || (((int)show_lost_screen) != last_show_lost_screen);

        bool dirty = !display_drawn
                   || (g_app.fox_target_set != last_target_set)
                   || (target_visible != last_target_visible)
                   || (live_rssi != last_rssi)
                   || (g_app.fox_led_mode != last_led_mode)
                   || (g_app.wifi_clients != last_wifi_clients)
               || view_switched
                   || ((int)gps_tag_active != last_gps_active)
                   || ((int)logging_active != last_logging_active)
                   || ((int)logging_blocked != last_logging_blocked)
                   || ((int)show_lost_screen != last_show_lost_screen)
                   || (g_app.ui_refresh_token != last_ui_refresh_token)
                   || (strcmp(target_header, last_target_header) != 0)
                   || (g_app.fox_registry_open && (g_app.ui_cursor != last_reg_cursor
                       || g_app.fox_registry_count != last_reg_count
                       || ((frame % 2) == 0)));

        /* ── Display: only redraw when visual state changed ── */
        if (dirty) {
            bool full_redraw = !display_drawn || view_switched || target_state_switched;
            last_target_set = g_app.fox_target_set;
            last_target_visible = target_visible;
            last_rssi = live_rssi;
            last_led_mode = g_app.fox_led_mode;
            last_wifi_clients = g_app.wifi_clients;
            last_registry_open = g_app.fox_registry_open;
            last_gps_active = (int)gps_tag_active;
            last_logging_active = (int)logging_active;
            last_logging_blocked = (int)logging_blocked;
            last_show_lost_screen = (int)show_lost_screen;
            last_ui_refresh_token = g_app.ui_refresh_token;
            snprintf(last_target_header, sizeof(last_target_header), "%s", target_header);
            last_reg_cursor = g_app.ui_cursor;
            last_reg_count = g_app.fox_registry_count;
            display_drawn = true;

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
            const char *ap_ssid = NULL;
            const char *ap_pass = NULL;
            app_mode_ap_credentials(MODE_FOX_HUNTER, &ap_ssid, &ap_pass, NULL);

            if (full_redraw) {
                display_fill(bg);
            }

            /* Status bar */
            display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 26, dim_accent);
            display_draw_rect(0, DISPLAY_STATUS_DIV_Y, LCD_H_RES, 2, accent);
            display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, "FOX HUNTER", text_main, dim_accent);
            display_draw_text_centered(DISPLAY_STATUS_SUB_Y,
                                       target_header[0] ? target_header : ap_ssid,
                                       rgb565(228, 228, 231), dim_accent);
            fox_draw_logging_badge(dim_accent);

            /* Content area */
            display_draw_rect(0, DISPLAY_CONTENT_TOP, LCD_H_RES, DISPLAY_FOOTER_BAR_Y - DISPLAY_CONTENT_TOP, bg);

            if (g_app.fox_registry_open) {
                /* ── Registry view ── */
                display_draw_text_centered(DISPLAY_CONTENT_TOP + 2, "TARGET REGISTRY + TRACK PICKS", accent, bg);
                display_draw_hline(4, DISPLAY_CONTENT_TOP + 14, LCD_H_RES - 8, dim_accent);

                int total_count = fox_hunter_registry_view_count();
                if (total_count == 0) {
                    g_app.ui_item_count = 0;
                    display_draw_text_centered(100, "No saved targets", text_dim, bg);
                    display_draw_text_centered(114, "No smart candidates yet", text_dim, bg);
                } else {
                    g_app.ui_item_count = total_count;
                    if (g_app.ui_cursor >= total_count)
                        g_app.ui_cursor = total_count - 1;

                    int max_show = 6;
                    int start = 0;
                    if (g_app.ui_cursor >= max_show) start = g_app.ui_cursor - max_show + 1;

                    fox_candidate_snapshot_t snap;
                    memset(&snap, 0, sizeof(snap));
                    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        fox_build_candidate_snapshot_locked(&snap, now);
                        xSemaphoreGive(g_app.device_mutex);
                    }

                    int saved_count = g_app.fox_registry_count;
                    int nearby_count = snap.nearby_count;
                    int following_count = snap.following_count;
                    int detected_count = snap.detected_count;

                    int y = DISPLAY_CONTENT_TOP + 20;
                    int row = 0;
                    int last_group = -1;
                    for (int idx = start; idx < total_count && row < max_show; idx++) {
                        int group = 0; /* 0=saved, 1=nearby, 2=following, 3=detected */
                        if (idx >= saved_count + nearby_count + following_count) group = 3;
                        else if (idx >= saved_count + nearby_count) group = 2;
                        else if (idx >= saved_count) group = 1;

                        if (group != last_group) {
                            const char *title = (group == 0) ? "SAVED TARGETS" :
                                                (group == 1) ? "LIKELY NEARBY" :
                                                (group == 2) ? "LIKELY FOLLOWING" : "DETECTED";
                            uint16_t title_col = (group == 0) ? rgb565(74, 222, 128) :
                                                 (group == 1) ? rgb565(56, 189, 248) :
                                                 (group == 2) ? rgb565(251, 191, 36) : rgb565(163, 163, 163);
                            display_draw_text(6, y, title, title_col, bg);
                            y += 10;
                            last_group = group;
                        }

                        char mac_str[18];
                        char line1[FOX_REG_NICK_LEN + 8] = {0};
                        char line2[DEVICE_NAME_LEN + 20] = {0};
                        const char *badge = "";
                        uint16_t badge_col = text_dim;
                        bool is_saved = (group == 0);

                        if (group == 0) {
                            fox_reg_entry_t *e = &g_app.fox_registry[idx];
                            mac_to_str(e->mac, mac_str, sizeof(mac_str));
                            if (e->nickname[0]) {
                                snprintf(line1, sizeof(line1), "%s", e->nickname);
                            } else if (e->label[0]) {
                                snprintf(line1, sizeof(line1), "%s", e->label);
                            } else {
                                snprintf(line1, sizeof(line1), "Saved target");
                            }
                            snprintf(line2, sizeof(line2), "%s", mac_str);
                            badge = "SAVED";
                            badge_col = rgb565(74, 222, 128);
                        } else if (group == 1) {
                            int nidx = idx - saved_count;
                            if (nidx >= 0 && nidx < nearby_count) {
                                ble_device_t *d = &snap.nearby[nidx];
                                mac_to_str(d->mac, mac_str, sizeof(mac_str));
                                if (d->name[0]) {
                                    snprintf(line1, sizeof(line1), "%s", d->name);
                                } else {
                                    snprintf(line1, sizeof(line1), "Nearby BLE");
                                }
                                snprintf(line2, sizeof(line2), "%s  %d dBm", mac_str, d->rssi);
                                badge = "NEAR";
                                badge_col = rgb565(56, 189, 248);
                            }
                        } else if (group == 2) {
                            int fidx = idx - saved_count - nearby_count;
                            if (fidx >= 0 && fidx < following_count) {
                                ble_device_t *d = &snap.following[fidx];
                                uint32_t dwell_sec = (d->last_seen > d->first_seen) ?
                                    (d->last_seen - d->first_seen) / 1000U : 0;
                                mac_to_str(d->mac, mac_str, sizeof(mac_str));
                                if (d->name[0]) {
                                    snprintf(line1, sizeof(line1), "%s", d->name);
                                } else {
                                    snprintf(line1, sizeof(line1), "Persistent BLE");
                                }
                                snprintf(line2, sizeof(line2), "%s  %d dBm  %lus", mac_str, d->rssi, (unsigned long)dwell_sec);
                                badge = "FOLLOW";
                                badge_col = rgb565(251, 191, 36);
                            }
                        } else {
                            int didx = idx - saved_count - nearby_count - following_count;
                            if (didx >= 0 && didx < detected_count) {
                                ble_device_t *d = &snap.detected[didx];
                                uint32_t age_sec = (now > d->last_seen) ? (now - d->last_seen) / 1000U : 0;
                                mac_to_str(d->mac, mac_str, sizeof(mac_str));
                                if (d->name[0]) {
                                    snprintf(line1, sizeof(line1), "%s", d->name);
                                } else {
                                    snprintf(line1, sizeof(line1), "Detected BLE");
                                }
                                snprintf(line2, sizeof(line2), "%s  %d dBm  %lus ago", mac_str, d->rssi, (unsigned long)age_sec);
                                badge = "SEEN";
                                badge_col = rgb565(163, 163, 163);
                            }
                        }

                        int y_base = y;
                        bool selected = (idx == g_app.ui_cursor);
                        uint16_t row_bg = selected ? rgb565(30, 20, 8) : panel_bg;
                        uint16_t row_border = selected ? accent : (is_saved ? border_col :
                                            (group == 1 ? rgb565(45, 58, 76) :
                                            (group == 2 ? rgb565(92, 63, 14) : rgb565(63, 63, 70))));

                        display_draw_bordered_rect(4, y_base, LCD_H_RES - 8, 30, row_border, row_bg);
                        display_draw_text(10, y_base + 3, line1, selected ? accent : text_main, row_bg);
                        display_draw_text(10, y_base + 15, line2, text_dim, row_bg);
                        display_draw_text(LCD_H_RES - 68, y_base + 3, badge, badge_col, row_bg);
                        if (selected) {
                            display_draw_text(LCD_H_RES - 24, y_base + 10, "GO", accent, row_bg);
                        }

                        y += 34;
                        row++;
                    }
                }

                display_draw_text_centered(DISPLAY_FOOTER_BAR_Y - 24, "DblClk=Prev Hold=Select 3xClk=Back", text_dim, bg);
            } else {
                /* ── Normal tracker view ── */
            if (!g_app.fox_target_set) {
                display_draw_bordered_rect(20, 80, LCD_H_RES - 40, 80, border_col, panel_bg);
                display_draw_text(28, 90, "NO TARGET SET", text_main, panel_bg);
                display_draw_hline(20, 120, LCD_H_RES - 40, dim_accent);
                display_draw_text(28, 126, "Hold=Registry", text_dim, panel_bg);
                display_draw_text(28, 138, "Web UI / Flock", text_dim, panel_bg);

                int cx = LCD_H_RES / 2, cy = 190;
                display_draw_rect(cx - 16, cy, 32, 2, dim_accent);
                display_draw_rect(cx, cy - 16, 2, 32, dim_accent);
            } else {
                char mac_str[18];
                mac_to_str(g_app.fox_target_mac, mac_str, sizeof(mac_str));

                display_draw_bordered_rect(4, 42, LCD_H_RES - 8, 34, border_col, panel_bg);
                display_draw_text(8, 46, "TARGET LOCKED", text_main, panel_bg);
                display_draw_text(8, 60, mac_str, rgb565(253, 186, 116), panel_bg);
                display_draw_hline(4, 82, LCD_H_RES - 8, accent);

                if (target_visible) {
                    char buf[32];
                    uint8_t cr, cg, cb;
                    rssi_to_color(g_app.fox_rssi, &cr, &cg, &cb);
                    uint16_t rssi_col = rgb565(cr, cg, cb);
                    int interval_ms = rssi_to_interval(g_app.fox_rssi);
                    int pct = rssi_to_percent(g_app.fox_rssi);
                    const char *prox = rssi_to_proximity_label(g_app.fox_rssi);
                    const char *cadence = rssi_to_cadence_label(g_app.fox_rssi);

                    display_draw_bordered_rect(4, 88, LCD_H_RES - 8, 64, border_col, panel_bg);
                    display_draw_text(10, 92, "BEST / PULSE", text_dim, panel_bg);
                    snprintf(buf, sizeof(buf), "Best %d", g_app.fox_rssi_best);
                    display_draw_text(10, 106, buf, text_main, panel_bg);
                    snprintf(buf, sizeof(buf), "Beat %dms", interval_ms);
                    display_draw_text(10, 120, buf, text_main, panel_bg);
                    snprintf(buf, sizeof(buf), "Pulse %s", cadence);
                    display_draw_text(10, 134, buf, text_main, panel_bg);

                    const int right_col_x = 96;
                    const int right_col_right = LCD_H_RES - 8;
                    const int right_col_w = right_col_right - right_col_x;
                    int rssi_scale = 2;
                    int rssi_y = 104;
                    int title_w = display_text_width("LIVE SIGNAL");
                    int title_x = right_col_x + ((right_col_w - title_w) / 2);
                    if (title_x < right_col_x) title_x = right_col_x;
                    display_draw_text(title_x, 92, "LIVE SIGNAL", text_dim, panel_bg);
                    snprintf(buf, sizeof(buf), "%d", g_app.fox_rssi);
                    int rssi_scaled_w = display_text_width(buf) * rssi_scale;
                    int dbm_w = display_text_width("dBm");
                    int pair_gap = 3;
                    int dbm_x = right_col_right - dbm_w;
                    int rssi_x = dbm_x - pair_gap - rssi_scaled_w;

                    if (rssi_x < right_col_x) {
                        rssi_scale = 1;
                        rssi_y = 112;
                        rssi_scaled_w = display_text_width(buf) * rssi_scale;
                        rssi_x = dbm_x - pair_gap - rssi_scaled_w;
                    }
                    if (rssi_x < right_col_x) {
                        pair_gap = 1;
                        rssi_x = dbm_x - pair_gap - rssi_scaled_w;
                    }
                    if (rssi_x < right_col_x) {
                        rssi_x = right_col_x;
                    }
                    display_draw_text_scaled(rssi_x, rssi_y, buf, rssi_col, panel_bg, rssi_scale);
                    display_draw_text(dbm_x, 112, "dBm", text_dim, panel_bg);

                    snprintf(buf, sizeof(buf), "Range %s", prox);
                    display_draw_text(10, 148, buf, text_main, panel_bg);

                    display_draw_bordered_rect(4, 160, LCD_H_RES - 8, 42, border_col, panel_bg);
                    display_draw_text(10, 164, "HOTTER SIGNAL = FASTER BEEPS", text_dim, panel_bg);
                    display_draw_rect(8, 178, LCD_H_RES - 16, 10, rgb565(39, 39, 42));
                    int bar_w = ((LCD_H_RES - 16) * pct) / 100;
                    display_draw_rect(8, 178, bar_w, 10, rssi_col);

                    for (int i = 1; i < 5; i++) {
                        int tx = 8 + ((LCD_H_RES - 16) * i) / 5;
                        display_draw_rect(tx, 176, 1, 14, rgb565(63, 63, 70));
                    }

                    snprintf(buf, sizeof(buf), "%d%%", pct);
                    display_draw_text(10, 190, buf, text_main, panel_bg);
                    display_draw_text(52, 190, "VERY FAR .. VERY CLOSE", text_dim, panel_bg);

                    for (int i = 0; i < 16; i++) {
                        int bar_h = 4 + i;
                        int bx = 8 + i * 10;
                        bool active = (i * 100 / 16) < pct;
                        uint16_t bar_col = active ? rssi_col : rgb565(39, 39, 42);
                        display_draw_rect(bx, 212 + (18 - bar_h), 8, bar_h, bar_col);
                    }

                    snprintf(buf, sizeof(buf), "Signal class: %s", prox);
                    display_draw_text_centered(232, buf, text_main, bg);

                    display_draw_bordered_rect(20, 252, LCD_H_RES - 40, 20, status_border, status_fill);
                    if ((frame % 6) < 3) {
                        display_draw_text(26, 258, "TRACKING LOCK", status_text, status_fill);
                    } else {
                        display_draw_text(26, 258, "TRACKING LOCK .", status_text, status_fill);
                    }
                } else if (!show_lost_screen) {
                    char buf[36];
                    uint32_t last_seen_sec = (g_app.fox_last_seen > 0 && now > g_app.fox_last_seen)
                        ? (now - g_app.fox_last_seen) / 1000U
                        : 0;

                    display_draw_bordered_rect(20, 100, LCD_H_RES - 40, 58, rgb565(120, 82, 0), rgb565(30, 20, 10));
                    display_draw_text(28, 112, "SIGNAL HOLD", rgb565(251, 191, 36), rgb565(30, 20, 10));
                    display_draw_text(28, 126, "Waiting for reacquire...", text_dim, rgb565(30, 20, 10));
                    snprintf(buf, sizeof(buf), "Seen %lus ago", (unsigned long)last_seen_sec);
                    display_draw_text(28, 140, buf, text_dim, rgb565(30, 20, 10));
                    snprintf(buf, sizeof(buf), "Lost screen after %lus", (unsigned long)(TARGET_LOST_SCREEN_DELAY_MS / 1000U));
                    display_draw_text(20, 172, buf, text_dim, bg);

                    int cx = LCD_H_RES / 2, cy = 212;
                    display_draw_rect(cx - 20, cy, 40, 2, dim_accent);
                    display_draw_rect(cx, cy - 20, 2, 40, dim_accent);
                } else {
                    char buf[36];
                    uint32_t last_seen_sec = (g_app.fox_last_seen > 0 && now > g_app.fox_last_seen)
                        ? (now - g_app.fox_last_seen) / 1000U
                        : 0;

                    display_draw_bordered_rect(20, 100, LCD_H_RES - 40, 58, rgb565(127, 29, 29), rgb565(30, 10, 10));
                    display_draw_text(40, 112, "SIGNAL LOST", rgb565(248, 113, 113), rgb565(30, 10, 10));
                    display_draw_text(30, 130, "Searching...", text_dim, rgb565(30, 10, 10));
                    snprintf(buf, sizeof(buf), "Seen %lus ago", (unsigned long)last_seen_sec);
                    display_draw_text(30, 142, buf, text_dim, rgb565(30, 10, 10));
                    snprintf(buf, sizeof(buf), "Last prox: %s", rssi_to_proximity_label(g_app.fox_rssi));
                    display_draw_text(22, 172, buf, text_dim, bg);

                    int cx = LCD_H_RES / 2, cy = 212;
                    display_draw_rect(cx - 20, cy, 40, 2, dim_accent);
                    display_draw_rect(cx, cy - 20, 2, 40, dim_accent);
                }
            }
            } /* end else (normal tracker view) */

            /* LED mode indicator above footer — always shown */
            {
                uint16_t mode_col = g_app.fox_led_mode ? rgb565(56, 189, 248) : rgb565(74, 222, 128);
                const char *led_label = g_app.fox_led_mode ? "STING" : "DETECTOR";
                char mode_label[24];
                snprintf(mode_label, sizeof(mode_label), "LED: %s", led_label);
                /* Clear full label lane so shorter text never leaves stale pixels behind. */
                display_draw_rect(4, DISPLAY_FOOTER_BAR_Y - 12, 96, 10, bg);
                display_draw_text(4, DISPLAY_FOOTER_BAR_Y - 12, mode_label, mode_col, bg);

                display_draw_rect(102, DISPLAY_FOOTER_BAR_Y - 12, 66, 10, bg);
                display_draw_text(104, DISPLAY_FOOTER_BAR_Y - 12,
                                  gps_tag_active ? "GPS ON" : "GPS OFF",
                                  gps_tag_active ? rgb565(74, 222, 128) : rgb565(248, 113, 113),
                                  bg);
            }

            /* Bottom bar with WiFi info */
            display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer_bg);
            display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, 1, border_col);
            char info[64];
            snprintf(info, sizeof(info), "%s  %dCli  %lukB",
                     ap_pass,
                     g_app.wifi_clients, (unsigned long)(g_app.free_heap / 1024));
            display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, info, text_dim, footer_bg);
        } /* end if (dirty) */

        /* ── LED / buzzer: always runs regardless of display dirty ── */
        if (!g_app.fox_target_set) {
            if (g_app.fox_led_mode == 0) {
                led_ctrl_set(255, 120, 0);
            } else {
                led_ctrl_off();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        } else if (target_visible) {
            int interval = rssi_to_interval(g_app.fox_rssi);
            uint32_t freq = 800 + (uint32_t)((g_app.fox_rssi + 100) * 20);
            buzzer_tone(freq, 20);

            if (g_app.fox_led_mode == 0) {
                led_ctrl_set(220, 20, 10);
                vTaskDelay(pdMS_TO_TICKS(interval / 2));
                led_ctrl_off();
                vTaskDelay(pdMS_TO_TICKS(interval / 2));
            } else {
                float str = (g_app.fox_rssi + 100.0f) / 80.0f;
                if (str < 0.0f) str = 0.0f;
                if (str > 1.0f) str = 1.0f;
                uint8_t br = (uint8_t)(2 + (str * str) * 253.0f);
                led_ctrl_set(0, (uint8_t)(br / 4), br);
                vTaskDelay(pdMS_TO_TICKS(interval));
            }
        } else {
            if (g_app.fox_led_mode == 0) {
                led_ctrl_set(0, 120, 0);
            } else {
                led_ctrl_off();
            }
            buzzer_off();
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

    /* Reset nearby candidate list for this session */
    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(g_app.fox_nearby, 0, sizeof(g_app.fox_nearby));
        g_app.fox_nearby_count = 0;
        xSemaphoreGive(g_app.device_mutex);
    }

    /* Try to load saved target from NVS */
    if (nvs_store_load_fox_target(g_app.fox_target_mac) == 0) {
        g_app.fox_target_set = true;
        char mac_str[18];
        mac_to_str(g_app.fox_target_mac, mac_str, sizeof(mac_str));
        ESP_LOGI(TAG, "Loaded target: %s", mac_str);
        fox_log_identity_event("target_loaded", g_app.fox_target_mac, NULL, "source=nvs");
    }

    /* Balanced scan: good responsiveness while preserving WiFi coexistence */
    ble_scanner_start(fox_scan_cb, 100, 50, false);

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
    g_app.ui_refresh_token++;
    nvs_store_save_fox_target(mac);

    char mac_str[18];
    mac_to_str(mac, mac_str, sizeof(mac_str));
    ESP_LOGI(TAG, "Target set: %s", mac_str);
    fox_log_identity_event("target_set", mac, NULL, "source=fox_hunter");
}

void fox_hunter_set_target_from_flock(int device_index)
{
    if (device_index < 0 || device_index >= g_app.device_count) return;
    fox_hunter_set_target(g_app.devices[device_index].mac);
}

void fox_hunter_clear_target(void)
{
    uint8_t prev_mac[6] = {0};
    bool had_target = g_app.fox_target_set;
    if (had_target) {
        memcpy(prev_mac, g_app.fox_target_mac, sizeof(prev_mac));
    }

    memset(g_app.fox_target_mac, 0, sizeof(g_app.fox_target_mac));
    g_app.fox_target_set = false;
    g_app.fox_target_found = false;
    g_app.fox_last_seen = 0;
    g_app.fox_rssi = -100;
    g_app.fox_rssi_best = -100;
    g_app.fox_registry_open = false;
    g_app.ui_cursor = 0;
    g_app.ui_item_count = 0;
    g_app.ui_refresh_token++;
    nvs_store_clear_fox_target();
    ESP_LOGI(TAG, "Target cleared");
    if (had_target) {
        fox_log_identity_event("target_cleared", prev_mac, NULL, "source=fox_hunter");
    }
}

bool fox_hunter_has_target(void)
{
    return g_app.fox_target_set;
}

static void reg_copy_field(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

int fox_hunter_registry_add(const uint8_t mac[6], const char *label,
                            const char *original_name, const char *section)
{
    /* Check for duplicate */
    for (int i = 0; i < g_app.fox_registry_count; i++) {
        if (mac_equal(g_app.fox_registry[i].mac, mac)) {
            fox_reg_entry_t *existing = &g_app.fox_registry[i];
            bool changed = false;
            if (label && label[0] && !existing->label[0]) {
                reg_copy_field(existing->label, sizeof(existing->label), label);
                changed = true;
            }
            if (original_name && original_name[0] && !existing->original_name[0]) {
                reg_copy_field(existing->original_name, sizeof(existing->original_name), original_name);
                changed = true;
            }
            if (section && section[0] && !existing->section[0]) {
                reg_copy_field(existing->section, sizeof(existing->section), section);
                changed = true;
            }
            if (changed) {
                char extra[96];
                snprintf(extra, sizeof(extra), "section=%s name=%s", existing->section,
                         existing->original_name[0] ? existing->original_name : "unknown");
                nvs_store_save_fox_registry();
                fox_log_identity_event("registry_updated", existing->mac,
                                       existing->label[0] ? existing->label : existing->nickname,
                                       extra);
            }
            return i; /* already exists */
        }
    }
    if (g_app.fox_registry_count >= fox_hunter_registry_capacity()) return -1; /* full */

    fox_reg_entry_t *e = &g_app.fox_registry[g_app.fox_registry_count];
    memset(e, 0, sizeof(*e));
    memcpy(e->mac, mac, sizeof(e->mac));
    reg_copy_field(e->label, sizeof(e->label), label);
    reg_copy_field(e->original_name, sizeof(e->original_name), original_name);
    reg_copy_field(e->section, sizeof(e->section), (section && section[0]) ? section : "auto");

    g_app.fox_registry_count++;
    nvs_store_save_fox_registry();
    ESP_LOGI(TAG, "Registry add [%d]: %s", g_app.fox_registry_count - 1, label ? label : "");
    {
        char extra[96];
        snprintf(extra, sizeof(extra), "section=%s name=%s", e->section,
                 e->original_name[0] ? e->original_name : "unknown");
        fox_log_identity_event("registry_added", e->mac, e->label[0] ? e->label : label, extra);
    }
    return g_app.fox_registry_count - 1;
}

int fox_hunter_registry_update(int index, const char *nickname,
                               const char *notes, const char *section,
                               const char *label, const char *original_name)
{
    if (index < 0 || index >= g_app.fox_registry_count) return -1;

    fox_reg_entry_t *e = &g_app.fox_registry[index];

    if (nickname) reg_copy_field(e->nickname, sizeof(e->nickname), nickname);
    if (notes) reg_copy_field(e->notes, sizeof(e->notes), notes);
    if (section) reg_copy_field(e->section, sizeof(e->section), section);
    if (label) reg_copy_field(e->label, sizeof(e->label), label);
    if (original_name) reg_copy_field(e->original_name, sizeof(e->original_name), original_name);

    if (!e->section[0]) {
        reg_copy_field(e->section, sizeof(e->section), "auto");
    }

    nvs_store_save_fox_registry();
    {
        char primary_label[FOX_REG_NICK_LEN + 1] = {0};
        char extra[128];
        snprintf(primary_label, sizeof(primary_label), "%s",
                 e->nickname[0] ? e->nickname : (e->label[0] ? e->label : "saved_target"));
        snprintf(extra, sizeof(extra), "section=%s name=%s",
                 e->section,
                 e->original_name[0] ? e->original_name : "unknown");
        fox_log_identity_event("registry_updated", e->mac, primary_label, extra);
    }
    return 0;
}

int fox_hunter_registry_set_gps(int index, double lat, double lon, float radius_m)
{
    if (index < 0 || index >= g_app.fox_registry_count) return -1;

    fox_reg_entry_t *e = &g_app.fox_registry[index];
    e->pinned_lat = lat;
    e->pinned_lon = lon;
    e->pinned_radius_m = radius_m;

    nvs_store_save_fox_registry();
    return 0;
}

int fox_hunter_registry_remove(int index)
{
    if (index < 0 || index >= g_app.fox_registry_count) return -1;
    fox_reg_entry_t removed = g_app.fox_registry[index];
    if (index < g_app.fox_registry_count - 1) {
        memmove(&g_app.fox_registry[index], &g_app.fox_registry[index + 1],
                (g_app.fox_registry_count - 1 - index) * sizeof(fox_reg_entry_t));
    }
    g_app.fox_registry_count--;
    nvs_store_save_fox_registry();
    {
        char primary_label[FOX_REG_NICK_LEN + 1] = {0};
        char extra[128];
        snprintf(primary_label, sizeof(primary_label), "%s",
                 removed.nickname[0] ? removed.nickname : (removed.label[0] ? removed.label : "saved_target"));
        snprintf(extra, sizeof(extra), "section=%s name=%s",
                 removed.section[0] ? removed.section : "auto",
                 removed.original_name[0] ? removed.original_name : "unknown");
        fox_log_identity_event("registry_removed", removed.mac, primary_label, extra);
    }
    return 0;
}

void fox_hunter_registry_select(int index)
{
    if (index < 0 || index >= g_app.fox_registry_count) return;
    fox_hunter_set_target(g_app.fox_registry[index].mac);
    g_app.fox_registry_open = false;
    g_app.ui_refresh_token++;
}

void fox_hunter_registry_select_view_index(int index)
{
    if (index < 0) return;

    if (index < g_app.fox_registry_count) {
        fox_hunter_registry_select(index);
        return;
    }

    int local_index = index - g_app.fox_registry_count;
    if (xSemaphoreTake(g_app.device_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        fox_candidate_snapshot_t snap;
        uint32_t now = uptime_ms();
        fox_build_candidate_snapshot_locked(&snap, now);

        if (local_index >= 0 && local_index < snap.nearby_count) {
            fox_hunter_set_target(snap.nearby[local_index].mac);
            g_app.fox_registry_open = false;
            g_app.ui_refresh_token++;
        } else {
            int follow_index = local_index - snap.nearby_count;
            if (follow_index >= 0 && follow_index < snap.following_count) {
                fox_hunter_set_target(snap.following[follow_index].mac);
                g_app.fox_registry_open = false;
                g_app.ui_refresh_token++;
            } else {
                int detected_index = follow_index - snap.following_count;
                if (detected_index >= 0 && detected_index < snap.detected_count) {
                    fox_hunter_set_target(snap.detected[detected_index].mac);
                    g_app.fox_registry_open = false;
                    g_app.ui_refresh_token++;
                }
            }
        }

        xSemaphoreGive(g_app.device_mutex);
    }
}
