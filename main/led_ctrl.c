/*
 * OUI-Spy C6 — LED status indicator
 *
 * HAS_RGB_LED=1 : drives WS2812 addressable LED via RMT.
 * HAS_RGB_LED=0 : draws an animated detection border around the LCD perimeter.
 *                 Runtime style is driven by g_app.active_border_style.
 *
 * SPDX-License-Identifier: MIT
 */
#include "led_ctrl.h"
#include "app_common.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "led";

/* ── Breathing task (shared by both backends) ── */
static TaskHandle_t s_breathe_task = NULL;
static volatile bool s_breathe_running = false;
static bool s_breathe_force = false;
static uint8_t s_effect_intensity = 255;
static uint8_t s_breathe_r, s_breathe_g, s_breathe_b;
static int s_breathe_period_ms;

static inline uint8_t scale_by_effect_intensity(uint8_t ch)
{
    return (uint8_t)((ch * (uint16_t)s_effect_intensity) / 255U);
}

/* ================================================================
 *  WS2812 addressable RGB LED backend
 * ================================================================ */
#if HAS_RGB_LED

#include "led_strip.h"

static led_strip_handle_t s_led = NULL;

static void led_ctrl_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

void led_ctrl_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num         = PIN_RGB_LED,
        .max_leds               = 1,
        .led_model              = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,  /* No DMA on ESP32-C6 RMT */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt_cfg, &s_led));
    led_strip_clear(s_led);
    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", PIN_RGB_LED);
}

void led_ctrl_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led || !g_app.led_enabled) return;
    led_ctrl_write(scale_by_effect_intensity(r),
                   scale_by_effect_intensity(g),
                   scale_by_effect_intensity(b));
}

void led_ctrl_set_forced(uint8_t r, uint8_t g, uint8_t b)
{
    led_ctrl_write(scale_by_effect_intensity(r),
                   scale_by_effect_intensity(g),
                   scale_by_effect_intensity(b));
}

void led_ctrl_off(void)
{
    if (!s_led) return;
    led_strip_clear(s_led);
}

void led_ctrl_pulse(uint8_t r, uint8_t g, uint8_t b, int period_ms)
{
    static bool on = false;
    on = !on;
    if (on) {
        led_ctrl_set(r, g, b);
    } else {
        led_ctrl_off();
    }
}

/* ================================================================
 *  Animated detection border backend  (touch board — no physical LED)
 *
 *  The border is drawn on the outermost LED_BORDER_THICK pixels of
 *  each edge.  Animation is driven purely by the current time so
 *  the breathing task just calls led_ctrl_set_forced() at ~33 fps.
 * ================================================================ */
#else /* !HAS_RGB_LED */

#include "display.h"

#define LED_EFFECT_MAX_DEPTH  14

/* ── Lightweight PRNG for glitch/flames (xorshift32) ── */
static uint32_t s_rng_state = 0xDEADBEEF;
static uint32_t xorshift32(void)
{
    uint32_t x = s_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng_state = x;
    return x;
}

/* current RGB being rendered (for clear detection) */
static uint8_t  s_cur_r = 0, s_cur_g = 0, s_cur_b = 0;
static bool     s_border_visible = false;
static uint8_t  s_touch_backlight_level = 0xFF;
static bool     s_overlay_intrusive = false;
static uint32_t s_last_overlay_refresh_ms = 0;

/* ── Helper: scale a color channel by a 0.0-1.0 brightness factor ── */
static inline uint8_t ch_scale(uint8_t ch, float f)
{
    int v = (int)(ch * f);
    return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
}

/* ── Helper: get current ms timestamp ── */
static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline uint8_t max3_u8(uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t m = a > b ? a : b;
    return m > c ? m : c;
}

static inline float clamp01f(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static inline float maxf2(float a, float b)
{
    return a > b ? a : b;
}

static float edge_peak(float pos, float center, float width)
{
    float d = fabsf(pos - center);
    if (d >= width) return 0.0f;
    d = 1.0f - (d / width);
    return d * d * (3.0f - 2.0f * d);
}

static uint16_t glow565(uint8_t r, uint8_t g, uint8_t b, float brightness, float white_mix)
{
    uint8_t rr;
    uint8_t gg;
    uint8_t bb;
    brightness = clamp01f(brightness);
    white_mix = clamp01f(white_mix);

    rr = ch_scale(r, brightness);
    gg = ch_scale(g, brightness);
    bb = ch_scale(b, brightness);

    rr = (uint8_t)(rr + (float)(255 - rr) * white_mix);
    gg = (uint8_t)(gg + (float)(255 - gg) * white_mix);
    bb = (uint8_t)(bb + (float)(255 - bb) * white_mix);
    return rgb565(rr, gg, bb);
}

static bool border_style_is_intrusive(uint8_t style)
{
    return style == BORDER_STYLE_RADIATION || style == BORDER_STYLE_GLITCH;
}

static void touch_overlay_request_refresh(bool force)
{
    uint32_t now = now_ms();
    if (!force && (now - s_last_overlay_refresh_ms) < 140U) {
        return;
    }

    g_app.ui_refresh_token++;
    s_last_overlay_refresh_ms = now;
}

static float flame_profile(float pos, float span, float t, float phase)
{
    float wave0 = 0.5f + 0.5f * sinf(pos * 0.085f + t * 8.0f + phase);
    float wave1 = 0.5f + 0.5f * sinf(pos * 0.039f - t * 5.6f + phase * 1.7f);
    float wave2 = 0.5f + 0.5f * sinf(pos * 0.170f + t * 11.5f + phase * 0.63f);
    float surge_pos = fmodf(t * (22.0f + phase * 3.0f), span + 28.0f) - 14.0f;
    float surge = edge_peak(pos, surge_pos, 12.0f + span * 0.08f);

    return clamp01f(0.14f + wave0 * 0.38f + wave1 * 0.26f + wave2 * 0.16f + surge * 0.36f);
}

static int effect_depth_for_color(uint8_t r, uint8_t g, uint8_t b)
{
    int depth = LED_BORDER_THICK + 2 + (max3_u8(r, g, b) * (LED_EFFECT_MAX_DEPTH - (LED_BORDER_THICK + 2))) / 255;
    if (depth < LED_BORDER_THICK) depth = LED_BORDER_THICK;
    if (depth > LED_EFFECT_MAX_DEPTH) depth = LED_EFFECT_MAX_DEPTH;
    return depth;
}

static int effect_depth_limited(uint8_t r, uint8_t g, uint8_t b, int max_depth)
{
    int depth = effect_depth_for_color(r, g, b);
    if (depth > max_depth) depth = max_depth;
    return depth;
}

static void touch_backlight_apply(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t base = g_app.lcd_brightness;
    uint8_t peak = max3_u8(r, g, b);
    uint8_t boost = (uint8_t)(24 + ((255 - base) / 2));
    uint8_t level = base;

    if (peak > 0) {
        uint16_t raised = (uint16_t)base + ((uint16_t)peak * boost) / 255U;
        level = (raised > 255U) ? 255U : (uint8_t)raised;
    }

    if (level != s_touch_backlight_level) {
        display_set_brightness(level);
        s_touch_backlight_level = level;
    }
}

static void clear_effect_band(int depth)
{
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;
    if (depth <= 0) depth = LED_BORDER_THICK;
    if (depth > LED_EFFECT_MAX_DEPTH) depth = LED_EFFECT_MAX_DEPTH;

    display_draw_rect(0, 0, W, depth, 0x0000);
    display_draw_rect(0, H - depth, W, depth, 0x0000);
    display_draw_rect(0, depth, depth, H - 2 * depth, 0x0000);
    display_draw_rect(W - depth, depth, depth, H - 2 * depth, 0x0000);
}

/* ── Clear the entire border back to black ── */
void led_ctrl_border_clear(void)
{
    clear_effect_band(LED_BORDER_THICK);
    if (s_border_visible) {
        touch_overlay_request_refresh(true);
    }
    s_overlay_intrusive = false;
    s_border_visible = false;
}

/* ──────────────────────────────────────────────────────────
 *  STYLE: PULSE — smooth sine glow on all four edges
 * ────────────────────────────────────────────────────────── */
static void draw_pulse(uint8_t r, uint8_t g, uint8_t b)
{
    const int D = effect_depth_limited(r, g, b, LED_BORDER_THICK + 8);
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;

    uint32_t t = now_ms();
    /* Slow secondary modulation for visual interest */
    float phase = (float)(t % 1200) / 1200.0f;
    float wave  = (1.0f - cosf(phase * 2.0f * 3.14159265f)) * 0.5f;

    for (int layer = 0; layer < D; layer++) {
        float edge = 1.0f - (float)layer / (float)D;
        float brightness = 0.08f + edge * edge * (0.34f + 0.58f * wave);
        float white = 0.02f + edge * 0.06f * wave;
        uint16_t c = glow565(r, g, b, brightness, white);

        display_draw_rect(layer, layer, W - 2 * layer, 1, c);
        display_draw_rect(layer, H - 1 - layer, W - 2 * layer, 1, c);
        display_draw_rect(layer, layer + 1, 1, H - 2 * (layer + 1), c);
        display_draw_rect(W - 1 - layer, layer + 1, 1, H - 2 * (layer + 1), c);
    }
}

/* ──────────────────────────────────────────────────────────
 *  STYLE: FLAMES — layered heat plumes hugging the frame
 * ────────────────────────────────────────────────────────── */
static void draw_flames(uint8_t r, uint8_t g, uint8_t b)
{
    const int T = LED_BORDER_THICK;
    const int D = effect_depth_limited(r, g, b, LED_BORDER_THICK + 4);
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;
    float t = (float)now_ms() * 0.001f;
    float intensity = (float)max3_u8(r, g, b) / 255.0f;
    uint16_t base = glow565(r, g, b, 0.08f + intensity * 0.06f, 0.02f);

    display_draw_rect(0, 0, W, D, base);
    display_draw_rect(0, H - D, W, D, base);
    display_draw_rect(0, D, D, H - 2 * D, base);
    display_draw_rect(W - D, D, D, H - 2 * D, base);

    for (int x = 0; x < W; x++) {
        float bottom_shape = flame_profile((float)x, (float)W, t, 0.35f);
        float top_shape = flame_profile((float)x, (float)W, t * 0.92f + 0.8f, 1.65f) * 0.78f;
        int bottom_h = T + (int)((float)(D - T) * bottom_shape);
        int top_h = T + (int)((float)(D - T) * top_shape);

        for (int dy = 0; dy < bottom_h; dy++) {
            float edge = 1.0f - ((float)dy / (float)bottom_h);
            float body = bottom_shape * (0.28f + edge * edge * (0.72f + intensity * 0.12f));
            float white = clamp01f((edge - 0.54f) * 1.6f) * (0.14f + bottom_shape * 0.34f);
            display_draw_rect(x, H - 1 - dy, 1, 1, glow565(r, g, b, body, white));
        }

        for (int dy = 0; dy < top_h; dy++) {
            float edge = 1.0f - ((float)dy / (float)top_h);
            float body = top_shape * (0.22f + edge * edge * 0.58f);
            float white = clamp01f((edge - 0.62f) * 1.2f) * (0.08f + top_shape * 0.18f);
            display_draw_rect(x, dy, 1, 1, glow565(r, g, b, body, white));
        }
    }

    for (int y = D; y < H - D; y++) {
        float left_shape = flame_profile((float)y, (float)H, t * 1.03f, 2.25f) * 0.84f;
        float right_shape = flame_profile((float)y, (float)H, t * 0.97f + 0.35f, 3.10f) * 0.84f;
        int left_w = T + (int)((float)(D - T) * left_shape);
        int right_w = T + (int)((float)(D - T) * right_shape);

        for (int dx = 0; dx < left_w; dx++) {
            float edge = 1.0f - ((float)dx / (float)left_w);
            float body = left_shape * (0.24f + edge * edge * 0.60f);
            float white = clamp01f((edge - 0.60f) * 1.25f) * (0.08f + left_shape * 0.20f);
            display_draw_rect(dx, y, 1, 1, glow565(r, g, b, body, white));
        }

        for (int dx = 0; dx < right_w; dx++) {
            float edge = 1.0f - ((float)dx / (float)right_w);
            float body = right_shape * (0.24f + edge * edge * 0.60f);
            float white = clamp01f((edge - 0.60f) * 1.25f) * (0.08f + right_shape * 0.20f);
            display_draw_rect(W - 1 - dx, y, 1, 1, glow565(r, g, b, body, white));
        }
    }
}

/* ──────────────────────────────────────────────────────────
 *  STYLE: RADIATION — display-wide scattered static with no perimeter ring
 * ────────────────────────────────────────────────────────── */
static void draw_radiation(uint8_t r, uint8_t g, uint8_t b)
{
    const int D = effect_depth_limited(r, g, b, LED_BORDER_THICK + 4);
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;
    float intensity = (float)max3_u8(r, g, b) / 255.0f;
    int speck_count = 18 + (int)(intensity * 90.0f);
    int cluster_count = 3 + (int)(intensity * 8.0f);

    clear_effect_band(D);

    for (int i = 0; i < speck_count; i++) {
        int x = (int)(xorshift32() % (uint32_t)W);
        int y = (int)(xorshift32() % (uint32_t)H);
        int w = 1 + (int)((xorshift32() % 100U) < (uint32_t)(intensity * 18.0f));
        int h = 1 + (int)((xorshift32() % 100U) < (uint32_t)(intensity * 12.0f));
        float brightness = 0.10f + intensity * 0.16f + ((float)(xorshift32() % 100U) / 100.0f) * 0.34f;
        float white = 0.10f + intensity * 0.14f + ((float)(xorshift32() % 100U) / 100.0f) * 0.20f;

        if (x + w > W) w = W - x;
        if (y + h > H) h = H - y;
        display_draw_rect(x, y, w, h, glow565(r, g, b, brightness, white));
    }

    for (int i = 0; i < cluster_count; i++) {
        int x = (int)(xorshift32() % (uint32_t)W);
        int y = (int)(xorshift32() % (uint32_t)H);
        int radius = 1 + (int)(xorshift32() % 3U);

        for (int sy = -radius; sy <= radius; sy++) {
            for (int sx = -radius; sx <= radius; sx++) {
                float dist = sqrtf((float)(sx * sx + sy * sy));
                float energy = 1.0f - (dist / (float)(radius + 1));
                int px = x + sx;
                int py = y + sy;

                if (energy <= 0.0f || (xorshift32() & 3U) == 0U) continue;
                if (px < 0 || px >= W || py < 0 || py >= H) continue;

                display_draw_rect(px, py, 1, 1,
                                  glow565(r, g, b,
                                          0.10f + energy * (0.22f + intensity * 0.12f),
                                          0.06f + energy * 0.16f));
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────
 *  STYLE: GLITCH — random horizontal scan-line tears
 * ────────────────────────────────────────────────────────── */
static void draw_glitch(uint8_t r, uint8_t g, uint8_t b)
{
    const int D = effect_depth_for_color(r, g, b);
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;
    uint8_t intensity = max3_u8(r, g, b);
    int n_tears = 2 + (intensity * 26 / 255);
    int n_vtears = 1 + (intensity * 10 / 255);
    int n_screen_tears = intensity < 48 ? 1 : 1 + (intensity * 20 / 255);
    int n_blocks = intensity < 96 ? 0 : ((intensity - 96) * 10 / 159);

    /* Clear whole border first so old glitch lines disappear */
    clear_effect_band(D);

    /* Stronger signal = denser and longer tears that push deeper inward. */
    for (int i = 0; i < n_tears; i++) {
        int y = (int)(xorshift32() % H);
        int len = D + (int)(xorshift32() % (W / 4 + (intensity * W / 510)));
        float f = 0.4f + 0.6f * ((float)(xorshift32() % 100) / 100.0f);

        /* Color shift: mostly keeps hue but occasionally inverts a channel */
        uint8_t gr = ch_scale(r, f);
        uint8_t gg = ch_scale(g, f);
        uint8_t gb = ch_scale(b, f);
        if (xorshift32() % (intensity > 180 ? 2 : 3) == 0) { uint8_t t2 = gr; gr = gb; gb = t2; }

        uint16_t c = rgb565(gr, gg, gb);
        int thickness = 1 + (int)(xorshift32() % (1 + intensity / 96));

        /* Left-side tear */
        if (xorshift32() & 1) {
            int draw_len = len > (W / 2 + D) ? (W / 2 + D) : len;
            display_draw_rect(0, y, draw_len, thickness, c);
        }
        /* Right-side tear */
        if (xorshift32() & 1) {
            int start = W - len;
            if (start < W / 2 - D) start = W / 2 - D;
            display_draw_rect(start, y, W - start, thickness, c);
        }
    }

    for (int i = 0; i < n_vtears; i++) {
        int x = (int)(xorshift32() % W);
        int len = 1 + (int)(xorshift32() % D);
        float f = 0.5f + 0.5f * ((float)(xorshift32() % 100) / 100.0f);
        uint16_t c = rgb565(ch_scale(r, f), ch_scale(g, f), ch_scale(b, f));
        int thickness = 1 + (int)(xorshift32() % (1 + intensity / 128));

        if (xorshift32() & 1)
            display_draw_rect(x, 0, thickness, len, c);
        if (xorshift32() & 1)
            display_draw_rect(x, H - len, thickness, len, c);
    }

    /* Whole-screen interference: starts subtle, becomes severe as proximity rises. */
    for (int i = 0; i < n_screen_tears; i++) {
        int y = D + (int)(xorshift32() % (H - (2 * D)));
        int h = 1 + (int)(xorshift32() % (1 + intensity / 96));
        int x = (int)(xorshift32() % (W / 2));
        int w = 8 + (int)(xorshift32() % (12 + intensity));
        uint8_t flick = (uint8_t)(40 + (xorshift32() % 216));
        uint16_t c;

        if (w > W - x) w = W - x;
        if ((xorshift32() & 3U) == 0U) {
            c = 0x0000;
        } else {
            uint8_t gr = ch_scale(r ? r : flick, 0.45f + 0.55f * (flick / 255.0f));
            uint8_t gg = ch_scale(g ? g : (uint8_t)(flick / 2), 0.35f + 0.65f * (flick / 255.0f));
            uint8_t gb = ch_scale(b ? b : flick, 0.45f + 0.55f * (flick / 255.0f));
            if (xorshift32() & 1U) {
                uint8_t swap = gr;
                gr = gb;
                gb = swap;
            }
            c = rgb565(gr, gg, gb);
        }
        display_draw_rect(x, y, w, h, c);
    }

    for (int i = 0; i < n_blocks; i++) {
        int w = 4 + (int)(xorshift32() % (8 + intensity / 10));
        int h = 2 + (int)(xorshift32() % (5 + intensity / 24));
        int x = D + (int)(xorshift32() % (W - (2 * D) - w));
        int y = D + (int)(xorshift32() % (H - (2 * D) - h));
        uint16_t c = (xorshift32() & 1U)
            ? rgb565(ch_scale(r, 0.8f), ch_scale(g, 0.8f), ch_scale(b, 0.8f))
            : 0x0000;
        display_draw_rect(x, y, w, h, c);
    }

    s_border_visible = true;
}

/* ──────────────────────────────────────────────────────────
 *  STYLE: VIPER — chasing lit segments orbiting the perimeter
 * ────────────────────────────────────────────────────────── */
static void draw_viper(uint8_t r, uint8_t g, uint8_t b)
{
    const int D = effect_depth_limited(r, g, b, LED_BORDER_THICK + 8);
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;
    int perim = 2 * (W + H);
    float intensity = (float)max3_u8(r, g, b) / 255.0f;

    uint32_t t = now_ms();
    /* Two segments chasing each other, 180° apart */
    int seg_len = perim / 6;
    int head0 = (int)(((float)(t % 1600) / 1600.0f) * perim) % perim;
    int head1 = (head0 + perim / 2) % perim;

    /* Helper: is perimeter position p inside a segment with given head? */
    #define IN_SEG(p, head) ({ \
        int d = (p) - (head); \
        if (d < 0) d += perim; \
        d < seg_len; \
    })

    /* Top edge */
    for (int x = 0; x < W; x++) {
        int p = x;
        bool lit = IN_SEG(p, head0) || IN_SEG(p, head1);
        for (int layer = 0; layer < D; layer++) {
            float edge = 1.0f - (float)layer / (float)D;
            float brightness = lit
                ? (0.16f + edge * (0.74f + intensity * 0.12f))
                : (0.02f + edge * (0.08f + intensity * 0.03f));
            float white = lit ? (0.04f + edge * 0.08f) : 0.0f;
            uint16_t c = glow565(r, g, b, brightness, white);
            display_draw_rect(x, layer, 1, 1, c);
        }
    }
    /* Right edge */
    for (int y = 0; y < H; y++) {
        int p = W + y;
        bool lit = IN_SEG(p, head0) || IN_SEG(p, head1);
        for (int layer = 0; layer < D; layer++) {
            float edge = 1.0f - (float)layer / (float)D;
            float brightness = lit
                ? (0.16f + edge * (0.74f + intensity * 0.12f))
                : (0.02f + edge * (0.08f + intensity * 0.03f));
            float white = lit ? (0.04f + edge * 0.08f) : 0.0f;
            uint16_t c = glow565(r, g, b, brightness, white);
            display_draw_rect(W - 1 - layer, y, 1, 1, c);
        }
    }
    /* Bottom edge */
    for (int x = W - 1; x >= 0; x--) {
        int p = W + H + (W - 1 - x);
        bool lit = IN_SEG(p, head0) || IN_SEG(p, head1);
        for (int layer = 0; layer < D; layer++) {
            float edge = 1.0f - (float)layer / (float)D;
            float brightness = lit
                ? (0.16f + edge * (0.74f + intensity * 0.12f))
                : (0.02f + edge * (0.08f + intensity * 0.03f));
            float white = lit ? (0.04f + edge * 0.08f) : 0.0f;
            uint16_t c = glow565(r, g, b, brightness, white);
            display_draw_rect(x, H - 1 - layer, 1, 1, c);
        }
    }
    /* Left edge */
    for (int y = H - 1; y >= 0; y--) {
        int p = 2 * W + H + (H - 1 - y);
        bool lit = IN_SEG(p, head0) || IN_SEG(p, head1);
        for (int layer = 0; layer < D; layer++) {
            float edge = 1.0f - (float)layer / (float)D;
            float brightness = lit
                ? (0.16f + edge * (0.74f + intensity * 0.12f))
                : (0.02f + edge * (0.08f + intensity * 0.03f));
            float white = lit ? (0.04f + edge * 0.08f) : 0.0f;
            uint16_t c = glow565(r, g, b, brightness, white);
            display_draw_rect(layer, y, 1, 1, c);
        }
    }
    #undef IN_SEG
}

/* ──────────────────────────────────────────────────────────
 *  STYLE: SONAR — expanding ring pulses from corners inward
 * ────────────────────────────────────────────────────────── */
static void draw_sonar(uint8_t r, uint8_t g, uint8_t b)
{
    const int D = effect_depth_limited(r, g, b, LED_BORDER_THICK + 10);
    const int W = LCD_H_RES;
    const int H = LCD_V_RES;
    float intensity = (float)max3_u8(r, g, b) / 255.0f;

    uint32_t t = now_ms();
    int active_layer = (int)((t / 120) % D);

    for (int layer = 0; layer < D; layer++) {
        int dist = abs(layer - active_layer);
        float ring = 1.0f - (float)dist / (float)D;
        float edge = 1.0f - (float)layer / (float)D;
        float brightness;
        float white;

        if (ring < 0.0f) ring = 0.0f;
        ring = ring * ring;
        brightness = 0.03f + edge * 0.05f + ring * (0.72f + intensity * 0.12f);
        white = 0.01f + ring * 0.10f;
        uint16_t c = glow565(r, g, b, brightness, white);

        display_draw_rect(layer, layer, W - 2 * layer, 1, c);
        display_draw_rect(layer, H - 1 - layer, W - 2 * layer, 1, c);
        display_draw_rect(layer, layer + 1, 1, H - 2 * (layer + 1), c);
        display_draw_rect(W - 1 - layer, layer + 1, 1, H - 2 * (layer + 1), c);
    }
}

/* ── Dispatch to the currently-selected style ── */
static void border_draw(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t style = g_app.active_border_style;
    bool intrusive = border_style_is_intrusive(style);

    if (style == BORDER_STYLE_NONE) {
        if (s_border_visible) led_ctrl_border_clear();
        return;
    }

    if (s_overlay_intrusive && !intrusive) {
        touch_overlay_request_refresh(true);
    }

    switch (style) {
    case BORDER_STYLE_FLAMES:    draw_flames(r, g, b);    break;
    case BORDER_STYLE_RADIATION: draw_radiation(r, g, b);  break;
    case BORDER_STYLE_GLITCH:    draw_glitch(r, g, b);     break;
    case BORDER_STYLE_VIPER:     draw_viper(r, g, b);      break;
    case BORDER_STYLE_SONAR:     draw_sonar(r, g, b);      break;
    case BORDER_STYLE_PULSE:
    default:                     draw_pulse(r, g, b);      break;
    }

    s_overlay_intrusive = intrusive;
    if (intrusive) {
        touch_overlay_request_refresh(false);
    }
    s_border_visible = true;
}

void led_ctrl_init(void)
{
    s_touch_backlight_level = 0xFF;
    ESP_LOGI(TAG, "On-screen detection border (%d px, style %d)", LED_BORDER_THICK, g_app.active_border_style);
}

void led_ctrl_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!g_app.led_enabled) return;
    r = scale_by_effect_intensity(r);
    g = scale_by_effect_intensity(g);
    b = scale_by_effect_intensity(b);
    s_cur_r = r; s_cur_g = g; s_cur_b = b;
    border_draw(r, g, b);
    touch_backlight_apply(r, g, b);
}

void led_ctrl_set_forced(uint8_t r, uint8_t g, uint8_t b)
{
    r = scale_by_effect_intensity(r);
    g = scale_by_effect_intensity(g);
    b = scale_by_effect_intensity(b);
    s_cur_r = r; s_cur_g = g; s_cur_b = b;
    border_draw(r, g, b);
    touch_backlight_apply(r, g, b);
}

void led_ctrl_off(void)
{
    s_cur_r = s_cur_g = s_cur_b = 0;
    led_ctrl_border_clear();
    touch_backlight_apply(0, 0, 0);
}

void led_ctrl_pulse(uint8_t r, uint8_t g, uint8_t b, int period_ms)
{
    static bool on = false;
    on = !on;
    if (on) {
        led_ctrl_set(r, g, b);
    } else {
        led_ctrl_off();
    }
}

#endif /* HAS_RGB_LED */

/* ================================================================
 *  Breathing task — backend-agnostic
 * ================================================================ */
static void breathe_task(void *arg)
{
    const int step_ms = 30;

    while (s_breathe_running) {
        if (!s_breathe_force && !g_app.led_enabled) {
            led_ctrl_off();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        float phase = (float)(now % (uint32_t)s_breathe_period_ms)
                    / (float)s_breathe_period_ms;
        /* Sine curve: smooth 0→1→0 */
        float brightness = (1.0f - cosf(phase * 2.0f * 3.14159265f)) * 0.5f;

        /* Gamma for perceptual smoothness */
        brightness = brightness * brightness;

        /* Floor so LED never fully extinguishes */
        float floor = 0.04f;
        brightness = floor + brightness * (1.0f - floor);

        uint8_t r = (uint8_t)(s_breathe_r * brightness);
        uint8_t g = (uint8_t)(s_breathe_g * brightness);
        uint8_t b = (uint8_t)(s_breathe_b * brightness);

        led_ctrl_set_forced(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    led_ctrl_off();
    s_breathe_task = NULL;
    vTaskDelete(NULL);
}

static void led_ctrl_breathe_start(uint8_t r, uint8_t g, uint8_t b, int period_ms, bool force)
{
    led_ctrl_breathe_stop();

    s_breathe_r = r;
    s_breathe_g = g;
    s_breathe_b = b;
    s_breathe_force = force;
    s_breathe_period_ms = period_ms > 200 ? period_ms : 200;
    s_breathe_running = true;

    if (xTaskCreate(breathe_task, "led_breathe", 2048, NULL, 2,
                    &s_breathe_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create breathe task");
        s_breathe_running = false;
        s_breathe_task = NULL;
    }
}

void led_ctrl_breathe(uint8_t r, uint8_t g, uint8_t b, int period_ms)
{
    led_ctrl_breathe_start(r, g, b, period_ms, false);
}

void led_ctrl_set_effect_intensity(uint8_t intensity)
{
    s_effect_intensity = intensity;
}

void led_ctrl_breathe_forced(uint8_t r, uint8_t g, uint8_t b, int period_ms)
{
    led_ctrl_breathe_start(r, g, b, period_ms, true);
}

void led_ctrl_breathe_stop(void)
{
    s_breathe_running = false;
    s_breathe_force = false;
    if (s_breathe_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
        s_breathe_task = NULL;
    }
}
