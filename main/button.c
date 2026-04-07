/*
 * OUI-Spy C6 — Smart GPIO button handler
 *
 * Multi-gesture state machine per button:
 *   IDLE → PRESSED on press
 *   PRESSED → fires HOLD at 1 s, WARN at 5 s, LONG_HOLD at 7 s while held
 *   PRESSED → WAIT_SECOND on release (< hold threshold)
 *   WAIT_SECOND → PRESSED on second press within 300 ms window
 *   WAIT_SECOND → fires CLICK/DOUBLE/TRIPLE/QUINTUPLE by click count
 *
 * SPDX-License-Identifier: MIT
 */
#include "button.h"
#include "app_common.h"
#include "driver/gpio.h"

static const int s_pins[BTN_COUNT] = {
    PIN_BOOT_BTN, PIN_BTN_MODE, PIN_BTN_ACTION, PIN_BTN_BACK
};

static button_event_cb_t s_event_cb = NULL;

#define DEBOUNCE_MS         50
#define DOUBLE_CLICK_MS     300
#define HOLD_SELECT_MS      500
#define HOLD_WARN_MS        3500
#define HOLD_RESET_MS       5000
#define POLL_MS             20

typedef enum {
    BTN_PHASE_IDLE,
    BTN_PHASE_PRESSED,
    BTN_PHASE_WAIT_SECOND,
} btn_phase_t;

typedef struct {
    btn_phase_t phase;
    uint32_t    press_start;
    uint32_t    release_time;
    uint8_t     click_count;
    bool        hold_fired;
    bool        warn_fired;
    bool        reset_fired;
} btn_state_t;

static btn_state_t s_state[BTN_COUNT];

static void button_poll_task(void *arg)
{
    while (1) {
        for (int i = 0; i < BTN_COUNT; i++) {
            if (s_pins[i] < 0) continue;  /* Pin not available */
            bool level = (gpio_get_level(s_pins[i]) == 0); /* Active low */
            uint32_t now = uptime_ms();

            switch (s_state[i].phase) {
            case BTN_PHASE_IDLE:
                if (level) {
                    s_state[i].phase       = BTN_PHASE_PRESSED;
                    s_state[i].press_start = now;
                    s_state[i].hold_fired  = false;
                    s_state[i].warn_fired  = false;
                    s_state[i].reset_fired = false;
                }
                break;

            case BTN_PHASE_PRESSED:
                if (!level) {
                    /* Released */
                    if (s_state[i].hold_fired || s_state[i].reset_fired) {
                        /* Was a hold gesture — don't count as click */
                        s_state[i].phase       = BTN_PHASE_IDLE;
                        s_state[i].click_count = 0;
                    } else if ((now - s_state[i].press_start) >= DEBOUNCE_MS) {
                        s_state[i].click_count++;
                        s_state[i].release_time = now;
                        s_state[i].phase = BTN_PHASE_WAIT_SECOND;
                    } else {
                        /* Bounce — ignore */
                        s_state[i].phase = BTN_PHASE_IDLE;
                    }
                } else {
                    /* Still held — check hold thresholds */
                    uint32_t held = now - s_state[i].press_start;
                    if (!s_state[i].hold_fired && held >= HOLD_SELECT_MS) {
                        s_state[i].hold_fired = true;
                        if (s_event_cb) s_event_cb((button_id_t)i, BTN_EVT_HOLD);
                    }
                    if (!s_state[i].warn_fired && held >= HOLD_WARN_MS) {
                        s_state[i].warn_fired = true;
                        if (s_event_cb) s_event_cb((button_id_t)i, BTN_EVT_LONG_HOLD_WARN);
                    }
                    if (!s_state[i].reset_fired && held >= HOLD_RESET_MS) {
                        s_state[i].reset_fired = true;
                        if (s_event_cb) s_event_cb((button_id_t)i, BTN_EVT_LONG_HOLD);
                    }
                }
                break;

            case BTN_PHASE_WAIT_SECOND:
                if (level) {
                    /* Pressed again within double-click window */
                    s_state[i].phase       = BTN_PHASE_PRESSED;
                    s_state[i].press_start = now;
                    s_state[i].hold_fired  = false;
                    s_state[i].warn_fired  = false;
                    s_state[i].reset_fired = false;
                } else if ((now - s_state[i].release_time) >= DOUBLE_CLICK_MS) {
                    /* Timeout — dispatch accumulated clicks */
                    if (s_event_cb) {
                        if (s_state[i].click_count >= 5)
                            s_event_cb((button_id_t)i, BTN_EVT_QUINTUPLE_CLICK);
                        else if (s_state[i].click_count >= 3)
                            s_event_cb((button_id_t)i, BTN_EVT_TRIPLE_CLICK);
                        else if (s_state[i].click_count >= 2)
                            s_event_cb((button_id_t)i, BTN_EVT_DOUBLE_CLICK);
                        else
                            s_event_cb((button_id_t)i, BTN_EVT_CLICK);
                    }
                    s_state[i].click_count = 0;
                    s_state[i].phase       = BTN_PHASE_IDLE;
                }
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void button_init(button_event_cb_t event_cb)
{
    s_event_cb = event_cb;

    for (int i = 0; i < BTN_COUNT; i++) {
        if (s_pins[i] < 0) {
            /* Pin not available on this board variant — skip */
            memset(&s_state[i], 0, sizeof(btn_state_t));
            continue;
        }
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_pins[i],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&io);
        memset(&s_state[i], 0, sizeof(btn_state_t));
    }

    if (xTaskCreate(button_poll_task, "btn_poll", TASK_STACK_BUTTON, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE("button", "Failed to create button poll task");
    }
}

bool button_is_pressed(button_id_t btn)
{
    if (btn >= BTN_COUNT) return false;
    if (s_pins[btn] < 0) return false;  /* Pin not available */
    return gpio_get_level(s_pins[btn]) == 0;
}
