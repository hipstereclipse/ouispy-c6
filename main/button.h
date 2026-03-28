/*
 * OUI-Spy C6 — Smart GPIO button handler
 *
 * Gestures detected per button:
 *   Single click  — navigate forward
 *   Double click  — navigate backward
 *   Hold  (~1 s)  — select / activate
 *   Long hold warning (5 s) — flash LED before reset
 *   Long hold (7 s) — return to mode selector
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>

typedef enum {
    BTN_BOOT   = 0,
    BTN_MODE   = 1,
    BTN_ACTION = 2,
    BTN_BACK   = 3,
    BTN_COUNT
} button_id_t;

typedef enum {
    BTN_EVT_CLICK,          /* single click  */
    BTN_EVT_DOUBLE_CLICK,   /* double click   */
    BTN_EVT_TRIPLE_CLICK,   /* triple click   */
    BTN_EVT_HOLD,           /* ~1 s hold      */
    BTN_EVT_LONG_HOLD_WARN, /* 5 s hold       */
    BTN_EVT_LONG_HOLD,      /* 7 s hold       */
} button_event_t;

typedef void (*button_event_cb_t)(button_id_t btn, button_event_t event);

void button_init(button_event_cb_t event_cb);
bool button_is_pressed(button_id_t btn);
