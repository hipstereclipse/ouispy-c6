/*
 * OUI-Spy C6 — ST7789V3 LCD display driver
 * Uses esp_lcd high-level API for the 172×320 IPS panel.
 * SPDX-License-Identifier: MIT
 */
#include "display.h"
#include "map_tile.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "display";
static uint16_t *s_solid_buf = NULL;
static size_t s_solid_buf_pixels = 0;
static SemaphoreHandle_t s_display_mutex = NULL;

#define DISPLAY_DMA_STRIP_ROWS 16

static esp_lcd_panel_handle_t s_panel    = NULL;
static esp_lcd_panel_io_handle_t s_io    = NULL;
static map_pin_t s_local_map_pins[MAX_SHARED_MAP_PINS + 3];

static bool map_terminal_diagnostics_enabled(void)
{
    return g_app.advanced_serial_logging_enabled
        || g_app.web_diagnostics_enabled
        || g_app.serial_log_verbosity >= SERIAL_LOG_DEBUG;
}

static void log_shared_map_placeholder(app_mode_t mode, int pin_count, int requested_zoom)
{
    if (!map_terminal_diagnostics_enabled()) return;

    static uint32_t s_last_placeholder_log_ms = 0;
    uint32_t now_ms = uptime_ms();
    if ((now_ms - s_last_placeholder_log_ms) < 2000U) return;
    s_last_placeholder_log_ms = now_ms;

    ESP_LOGW(TAG,
             "map render mode=%s pins=%d requested_zoom=%d no_pins_and_no_tiles",
             mode == MODE_SKY_SPY ? "sky" : "flock",
             pin_count,
             requested_zoom);
}

static void log_shared_map_render(app_mode_t mode,
                                  int pin_count,
                                  bool fallback_view,
                                  int requested_zoom,
                                  int zoom,
                                  int best_zoom,
                                  bool tiles_available,
                                  bool has_tiles,
                                  double center_lat,
                                  double center_lon,
                                  int tile_x0,
                                  int tile_y0,
                                  int tile_x1,
                                  int tile_y1)
{
    if (!map_terminal_diagnostics_enabled()) return;

    static uint32_t s_last_render_sig = 0;
    static uint32_t s_last_render_log_ms = 0;

    uint32_t now_ms = uptime_ms();
    uint32_t sig = ((uint32_t)(mode & 0xFF) << 24)
                 ^ ((uint32_t)(pin_count & 0xFF) << 16)
                 ^ ((uint32_t)(requested_zoom & 0xFF) << 8)
                 ^ (uint32_t)(zoom & 0xFF)
                 ^ (fallback_view ? 0x00000001u : 0u)
                 ^ (tiles_available ? 0x00000002u : 0u)
                 ^ (has_tiles ? 0x00000004u : 0u)
                 ^ ((uint32_t)(best_zoom & 0xFF) << 4);

    if (sig == s_last_render_sig && (now_ms - s_last_render_log_ms) < 2000U) return;

    s_last_render_sig = sig;
    s_last_render_log_ms = now_ms;

    ESP_LOGI(TAG,
             "map render mode=%s pins=%d fallback=%d req_zoom=%d zoom=%d best_png=%d tiles_available=%d tiles_drawn=%d center=%.5f,%.5f tile_span=%d..%d/%d..%d",
             mode == MODE_SKY_SPY ? "sky" : "flock",
             pin_count,
             fallback_view ? 1 : 0,
             requested_zoom,
             zoom,
             best_zoom,
             tiles_available ? 1 : 0,
             has_tiles ? 1 : 0,
             center_lat,
             center_lon,
             tile_x0,
             tile_x1,
             tile_y0,
             tile_y1);
}

/* 5×7 bitmap font (ASCII 32-126) - minimal built-in font */
#include "font5x7.h"

static bool display_lock(TickType_t timeout_ticks)
{
    return !s_display_mutex || xSemaphoreTakeRecursive(s_display_mutex, timeout_ticks) == pdTRUE;
}

static void display_unlock(void)
{
    if (s_display_mutex) {
        xSemaphoreGiveRecursive(s_display_mutex);
    }
}

static uint16_t *display_get_solid_buf(size_t pixels)
{
    if (pixels == 0) return NULL;
    if (pixels <= s_solid_buf_pixels && s_solid_buf) return s_solid_buf;

    uint16_t *next = heap_caps_realloc(s_solid_buf, pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!next) return NULL;

    s_solid_buf = next;
    s_solid_buf_pixels = pixels;
    return s_solid_buf;
}

static void display_fill_solid_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_panel || w <= 0 || h <= 0) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= LCD_H_RES || y >= LCD_V_RES) return;
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;

    int chunk_rows = h < DISPLAY_DMA_STRIP_ROWS ? h : DISPLAY_DMA_STRIP_ROWS;
    uint16_t *buf = NULL;
    while (chunk_rows > 0) {
        buf = display_get_solid_buf((size_t)w * (size_t)chunk_rows);
        if (buf) break;
        chunk_rows /= 2;
    }

    if (!buf) {
        ESP_LOGE(TAG, "display_fill_solid_rect: OOM w=%d h=%d", w, h);
        return;
    }

    for (int row = 0; row < h; row += chunk_rows) {
        int draw_rows = (row + chunk_rows > h) ? (h - row) : chunk_rows;
        size_t px_count = (size_t)w * (size_t)draw_rows;
        for (size_t i = 0; i < px_count; i++) buf[i] = color;
        esp_lcd_panel_draw_bitmap(s_panel, x, y + row, x + w, y + row + draw_rows, buf);
    }
}

void display_init(void)
{
    if (!s_display_mutex) {
        s_display_mutex = xSemaphoreCreateRecursiveMutex();
        if (!s_display_mutex) {
            ESP_LOGE(TAG, "Failed to create display mutex");
        }
    }

    /* ── Backlight via LEDC PWM ── */
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_ch = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_ch);

    /* ── SPI bus ── */
    spi_bus_config_t buscfg = {
        .sclk_io_num   = PIN_LCD_SCLK,
        .mosi_io_num   = PIN_LCD_MOSI,
        .miso_io_num   = PIN_SD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* ── Panel IO ── */
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = PIN_LCD_DC,
        .cs_gpio_num       = PIN_LCD_CS,
        .pclk_hz           = 40 * 1000 * 1000,
        .spi_mode          = 0,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &s_io));

    /* ── ST7789 Panel ── */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num   = PIN_LCD_RST,
        .rgb_ele_order    = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel   = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel));

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, LCD_COL_OFFSET, LCD_ROW_OFFSET);
    esp_lcd_panel_disp_on_off(s_panel, true);

    display_set_brightness(g_app.lcd_brightness);
    display_fill(0x0000);

    ESP_LOGI(TAG, "Display initialized %dx%d", LCD_H_RES, LCD_V_RES);
}

void display_set_brightness(uint8_t level)
{
    if (!display_lock(pdMS_TO_TICKS(50))) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    display_unlock();
}

void display_fill(uint16_t color)
{
    if (!display_lock(pdMS_TO_TICKS(200))) return;
    display_fill_solid_rect(0, 0, LCD_H_RES, LCD_V_RES, color);
    display_unlock();
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!display_lock(pdMS_TO_TICKS(200))) return;
    display_fill_solid_rect(x, y, w, h, color);
    display_unlock();
}

void display_blit_rgb565(int x, int y, int w, int h, const uint16_t *data)
{
    if (!data || w <= 0 || h <= 0) return;
    if (!display_lock(pdMS_TO_TICKS(200))) return;
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, data);
    display_unlock();
}

void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg)
{
    if (!text) return;
    if (!display_lock(pdMS_TO_TICKS(200))) return;
    int start_x = x;
    /* Each character is 6 pixels wide (5 + 1 gap), 8 tall */
    const int cw = 6, ch = 8;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            x = start_x;
            y += ch + 1;
            continue;
        }
        uint8_t c = (uint8_t)*p;
        if (c < 32 || c > 126) c = '?';

        uint16_t cbuf[6 * 8];
        for (int row = 0; row < ch; row++) {
            uint8_t bits = (row < 7) ? font5x7[c - 32][row] : 0;
            for (int col = 0; col < cw; col++) {
                cbuf[row * cw + col] = (col < 5 && (bits & (1 << (4 - col)))) ? fg : bg;
            }
        }
        if (x + cw <= LCD_H_RES && y + ch <= LCD_V_RES) {
            esp_lcd_panel_draw_bitmap(s_panel, x, y, x + cw, y + ch, cbuf);
        }
        x += cw;
    }
    display_unlock();
}

int display_text_width(const char *text)
{
    if (!text) return 0;

    int line_width = 0;
    int max_width = 0;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            if (line_width > max_width) max_width = line_width;
            line_width = 0;
            continue;
        }
        line_width += 6;
    }

    return line_width > max_width ? line_width : max_width;
}

void display_draw_text_centered(int y, const char *text, uint16_t fg, uint16_t bg)
{
    if (!display_lock(pdMS_TO_TICKS(200))) return;
    int width = display_text_width(text);
    int x = (LCD_H_RES - width) / 2;
    if (x < 0) x = 0;
    display_draw_text(x, y, text, fg, bg);
    display_unlock();
}

void display_draw_text_scaled_centered(int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    if (!display_lock(pdMS_TO_TICKS(200))) return;
    int width = display_text_width(text) * scale;
    int x = (LCD_H_RES - width) / 2;
    if (x < 0) x = 0;
    display_draw_text_scaled(x, y, text, fg, bg, scale);
    display_unlock();
}

void display_draw_status(const char *mode_name, uint16_t accent_color)
{
    if (!display_lock(pdMS_TO_TICKS(300))) return;
    /* Top status bar */
    display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, DISPLAY_STATUS_BAR_H, accent_color);
    display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, mode_name, 0xFFFF, accent_color);

    /* IP address in status bar */
    display_draw_text_centered(DISPLAY_STATUS_SUB_Y, "192.168.4.1", 0xFFFF, accent_color);

    /* Bottom info bar — dark, unobtrusive */
    uint16_t footer_bg = rgb565(22, 22, 26);
    uint16_t footer_fg = rgb565(120, 118, 114);
    display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer_bg);
    char info[32];
    snprintf(info, sizeof(info), "Heap:%lukB", (unsigned long)(g_app.free_heap / 1024));
    display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, info, footer_fg, footer_bg);
    display_unlock();
}

void display_draw_hline(int x, int y, int w, uint16_t color)
{
    if (!display_lock(pdMS_TO_TICKS(200))) return;
    display_draw_rect(x, y, w, 2, color);
    display_unlock();
}

void display_draw_bordered_rect(int x, int y, int w, int h, uint16_t border, uint16_t fill)
{
    if (!display_lock(pdMS_TO_TICKS(200))) return;
    display_draw_rect(x, y, w, h, fill);
    display_draw_rect(x, y, w, 1, border);
    display_draw_rect(x, y + h - 1, w, 1, border);
    display_draw_rect(x, y, 1, h, border);
    display_draw_rect(x + w - 1, y, 1, h, border);
    display_unlock();
}

void display_draw_text_scaled(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    if (!text || scale < 1) return;
    if (!display_lock(pdMS_TO_TICKS(300))) return;
    const int cw = 6 * scale, ch = 8 * scale;
    int start_x = x;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') { x = start_x; y += ch + scale; continue; }
        uint8_t c = (uint8_t)*p;
        if (c < 32 || c > 126) c = '?';

        int buf_sz = cw * ch;
        uint16_t *cbuf = heap_caps_malloc(buf_sz * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!cbuf) continue;

        for (int row = 0; row < ch; row++) {
            int font_row = row / scale;
            uint8_t bits = (font_row < 7) ? font5x7[c - 32][font_row] : 0;
            for (int col = 0; col < cw; col++) {
                int font_col = col / scale;
                cbuf[row * cw + col] = (font_col < 5 && (bits & (1 << (4 - font_col)))) ? fg : bg;
            }
        }
        if (x + cw <= LCD_H_RES && y + ch <= LCD_V_RES) {
            esp_lcd_panel_draw_bitmap(s_panel, x, y, x + cw, y + ch, cbuf);
        }
        free(cbuf);
        x += cw;
    }
    display_unlock();
}

/* Use indexed tile zoom levels when present so the shared map can still open
 * and browse non-PNG tile sets, even when the LCD renderer has to fall back
 * to the grid view. */
static int map_tile_zoom_for_idx(uint8_t idx)
{
    int zooms[20];
    size_t zoom_count = map_tile_browsable_zooms(zooms, sizeof(zooms) / sizeof(zooms[0]));

    if (zoom_count > 0) {
        return zooms[idx % zoom_count];
    }

    static const int fallback_zooms[] = {9, 10, 11, 12, 13};
    return fallback_zooms[idx % (sizeof(fallback_zooms) / sizeof(fallback_zooms[0]))];
}

static uint16_t map_pin_color(map_pin_kind_t kind)
{
    switch (kind) {
    case MAP_PIN_KIND_SELF:
        return rgb565(34, 211, 238);
    case MAP_PIN_KIND_DRONE:
        return rgb565(56, 189, 248);
    case MAP_PIN_KIND_FOX:
        return rgb565(251, 146, 60);
    case MAP_PIN_KIND_FLOCK:
    default:
        return rgb565(248, 113, 113);
    }
}

static bool map_pin_matches_mac(const map_pin_t *pin, const uint8_t mac[6])
{
    return pin && mac && memcmp(pin->mac, mac, 6) == 0;
}

typedef struct {
    int shared_raw;
    int shared_valid;
    int shared_invalid;
    bool shared_truncated;
    bool map_mutex_timeout;
    bool drone_mutex_timeout;
    int fox_added;
    int fox_duplicate;
    int fox_invalid;
    int sky_added;
    int sky_duplicate;
    int sky_invalid;
} map_pin_collect_diag_t;

static void log_map_pin_collection(const map_pin_collect_diag_t *diag, int total)
{
    if (!diag || !map_terminal_diagnostics_enabled()) return;

    static uint32_t s_last_sig = 0;
    static uint32_t s_last_log_ms = 0;

    uint32_t now_ms = uptime_ms();
    uint32_t sig = (uint32_t)(total & 0xFF)
                 ^ ((uint32_t)(diag->shared_raw & 0xFF) << 8)
                 ^ ((uint32_t)(diag->shared_valid & 0xFF) << 16)
                 ^ ((uint32_t)(diag->shared_invalid & 0xFF) << 24)
                 ^ ((uint32_t)(diag->fox_added & 0x0F) << 4)
                 ^ ((uint32_t)(diag->sky_added & 0x0F) << 12)
                 ^ (diag->shared_truncated ? 0x00000001u : 0u)
                 ^ (diag->map_mutex_timeout ? 0x00000002u : 0u)
                 ^ (diag->drone_mutex_timeout ? 0x00000004u : 0u);

    if (sig == s_last_sig && (now_ms - s_last_log_ms) < 2000U) return;
    s_last_sig = sig;
    s_last_log_ms = now_ms;

    ESP_LOGI(TAG,
             "map pins total=%d shared(raw=%d valid=%d invalid=%d trunc=%d map_lock_to=%d) fox(add=%d dup=%d invalid=%d) sky(add=%d dup=%d invalid=%d drone_lock_to=%d)",
             total,
             diag->shared_raw,
             diag->shared_valid,
             diag->shared_invalid,
             diag->shared_truncated ? 1 : 0,
             diag->map_mutex_timeout ? 1 : 0,
             diag->fox_added,
             diag->fox_duplicate,
             diag->fox_invalid,
             diag->sky_added,
             diag->sky_duplicate,
             diag->sky_invalid,
             diag->drone_mutex_timeout ? 1 : 0);
}

static void log_map_tile_debug_summary(void)
{
    if (!map_terminal_diagnostics_enabled()) return;

    map_tile_debug_info_t dbg = {0};
    map_tile_get_debug_info(&dbg);

    static uint32_t s_last_sig = 0;
    static uint32_t s_last_log_ms = 0;

    uint32_t now_ms = uptime_ms();
    uint32_t sig = (dbg.root_available ? 0x01u : 0u)
                 ^ ((uint32_t)(dbg.png.highest_zoom & 0x1F) << 2)
                 ^ ((uint32_t)(dbg.any.highest_zoom & 0x1F) << 8)
                 ^ ((uint32_t)(dbg.png.zoom_count & 0x1F) << 14)
                 ^ ((uint32_t)(dbg.any.zoom_count & 0x1F) << 20)
                 ^ (dbg.png.available ? 0x20000000u : 0u)
                 ^ (dbg.any.available ? 0x40000000u : 0u);

    if (sig == s_last_sig && (now_ms - s_last_log_ms) < 2000U) return;
    s_last_sig = sig;
    s_last_log_ms = now_ms;

    ESP_LOGI(TAG,
             "map tiles root=%s age_ms=%ld any(avail=%d z=%d count=%u bounds=%d..%d/%d..%d) png(avail=%d z=%d count=%u bounds=%d..%d/%d..%d)",
             dbg.root_available ? dbg.root : "(missing)",
             (long)dbg.cache_age_ms,
             dbg.any.available ? 1 : 0,
             dbg.any.highest_zoom,
             (unsigned)dbg.any.zoom_count,
             dbg.any.min_tile_x,
             dbg.any.max_tile_x,
             dbg.any.min_tile_y,
             dbg.any.max_tile_y,
             dbg.png.available ? 1 : 0,
             dbg.png.highest_zoom,
             (unsigned)dbg.png.zoom_count,
             dbg.png.min_tile_x,
             dbg.png.max_tile_x,
             dbg.png.min_tile_y,
             dbg.png.max_tile_y);

    if (dbg.any.available && !dbg.png.available) {
        ESP_LOGW(TAG,
                 "map tiles found but no PNG tiles for LCD renderer (JPG/WEBP-only sets cannot draw on device yet)");
    }
}

static int collect_local_map_pins(app_mode_t mode,
                                  map_pin_t *out_pins,
                                  int max_pins,
                                  map_pin_collect_diag_t *diag)
{
    if (!out_pins || max_pins <= 0) return 0;

    map_pin_collect_diag_t local_diag = {0};
    if (!diag) diag = &local_diag;

    int count = 0;
    if (g_app.map_mutex && xSemaphoreTake(g_app.map_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        int shared_count = g_app.shared_map_pin_count;
        diag->shared_raw = shared_count;
        if (shared_count > max_pins) diag->shared_truncated = true;
        if (shared_count > MAX_SHARED_MAP_PINS) shared_count = MAX_SHARED_MAP_PINS;
        for (int i = 0; i < shared_count && count < max_pins; i++) {
            map_pin_t *src = &g_app.shared_map_pins[i];
            if (!isfinite(src->lat) || !isfinite(src->lon)) {
                diag->shared_invalid++;
                continue;
            }
            out_pins[count++] = *src;
            diag->shared_valid++;
        }
        xSemaphoreGive(g_app.map_mutex);
    } else if (g_app.map_mutex) {
        diag->map_mutex_timeout = true;
    }

    if (g_app.fox_target_gps_samples > 0 && count < max_pins) {
        if (!isfinite(g_app.fox_target_lat) || !isfinite(g_app.fox_target_lon)) {
            diag->fox_invalid++;
        }
    }
    if (g_app.fox_target_gps_samples > 0 && isfinite(g_app.fox_target_lat) && isfinite(g_app.fox_target_lon) && count < max_pins) {
        bool exists = false;
        for (int i = 0; i < count; i++) {
            if (g_app.fox_target_set && map_pin_matches_mac(&out_pins[i], g_app.fox_target_mac)) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            map_pin_t *pin = &out_pins[count++];
            memset(pin, 0, sizeof(*pin));
            if (g_app.fox_target_set) memcpy(pin->mac, g_app.fox_target_mac, sizeof(pin->mac));
            snprintf(pin->label, sizeof(pin->label), "Fox target");
            pin->lat = g_app.fox_target_lat;
            pin->lon = g_app.fox_target_lon;
            pin->radius_m = g_app.fox_target_radius_m;
            pin->rssi = g_app.fox_rssi;
            pin->kind = MAP_PIN_KIND_FOX;
            diag->fox_added++;
        } else {
            diag->fox_duplicate++;
        }
    }

    if (g_app.sky_tracked_gps_samples > 0 && count < max_pins) {
        if (!isfinite(g_app.sky_tracked_lat) || !isfinite(g_app.sky_tracked_lon)) {
            diag->sky_invalid++;
        }
    }
    if (g_app.sky_tracked_gps_samples > 0 && isfinite(g_app.sky_tracked_lat) && isfinite(g_app.sky_tracked_lon) && count < max_pins) {
        bool exists = false;
        uint8_t tracked_mac[6] = {0};
        if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (g_app.sky_tracked_drone_idx >= 0 && g_app.sky_tracked_drone_idx < g_app.drone_count) {
                memcpy(tracked_mac, g_app.drones[g_app.sky_tracked_drone_idx].mac, sizeof(tracked_mac));
            }
            xSemaphoreGive(g_app.drone_mutex);
        } else {
            diag->drone_mutex_timeout = true;
        }
        for (int i = 0; i < count; i++) {
            if (map_pin_matches_mac(&out_pins[i], tracked_mac)) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            map_pin_t *pin = &out_pins[count++];
            memset(pin, 0, sizeof(*pin));
            memcpy(pin->mac, tracked_mac, sizeof(pin->mac));
            snprintf(pin->label, sizeof(pin->label), "Sky track");
            pin->lat = g_app.sky_tracked_lat;
            pin->lon = g_app.sky_tracked_lon;
            pin->radius_m = g_app.sky_tracked_radius_m;
            pin->rssi = -70;
            pin->kind = MAP_PIN_KIND_DRONE;
            diag->sky_added++;
        } else {
            diag->sky_duplicate++;
        }
    }

    if (mode == MODE_SKY_SPY && count < max_pins) {
        bool has_live = false;
        drone_info_t best = {0};
        if (xSemaphoreTake(g_app.drone_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            for (int i = 0; i < g_app.drone_count; i++) {
                const drone_info_t *d = &g_app.drones[i];
                if (!d->has_location) continue;
                if (!isfinite(d->lat) || !isfinite(d->lon)) continue;
                if (!has_live || d->last_seen > best.last_seen) {
                    best = *d;
                    has_live = true;
                }
            }
            xSemaphoreGive(g_app.drone_mutex);
        } else {
            diag->drone_mutex_timeout = true;
        }

        if (has_live) {
            bool exists = false;
            for (int i = 0; i < count; i++) {
                if (map_pin_matches_mac(&out_pins[i], best.mac)) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                map_pin_t *pin = &out_pins[count++];
                memset(pin, 0, sizeof(*pin));
                memcpy(pin->mac, best.mac, sizeof(pin->mac));
                snprintf(pin->label, sizeof(pin->label), "Live drone");
                pin->lat = best.lat;
                pin->lon = best.lon;
                pin->radius_m = 0.0f;
                pin->rssi = best.rssi;
                pin->kind = MAP_PIN_KIND_DRONE;
                diag->sky_added++;
            } else {
                diag->sky_duplicate++;
            }
        }
    }

    return count;
}

void display_draw_shared_map_view(app_mode_t mode)
{
    bool diag = map_terminal_diagnostics_enabled();
    if (diag) {
        ESP_LOGI(TAG, "map_view ENTER mode=%d heap=%lu stack=%u",
                 (int)mode,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    if (!display_lock(pdMS_TO_TICKS(500))) {
        if (diag) ESP_LOGW(TAG, "map_view display_lock TIMEOUT");
        return;
    }
    if (diag) ESP_LOGI(TAG, "map_view lock acquired");
    map_pin_t *pins = s_local_map_pins;
    map_pin_collect_diag_t pin_diag = {0};
    int pin_count = collect_local_map_pins(mode,
                                           pins,
                                           (int)(sizeof(s_local_map_pins) / sizeof(s_local_map_pins[0])),
                                           &pin_diag);
    if (diag) ESP_LOGI(TAG, "map_view pins=%d", pin_count);
    log_map_pin_collection(&pin_diag, pin_count);

    /* Only log tile debug info once per map-open session and only when the
     * tile cache is already warm (never trigger the slow SD scan here). */
    {
        static bool s_tile_diag_logged = false;
        if (!g_app.local_map_open) s_tile_diag_logged = false;
        if (!s_tile_diag_logged && map_tile_cache_ready()) {
            log_map_tile_debug_summary();
            s_tile_diag_logged = true;
        }
    }

    uint16_t bg = (mode == MODE_SKY_SPY) ? rgb565(3, 8, 3) : rgb565(9, 9, 11);
    uint16_t panel = (mode == MODE_SKY_SPY) ? rgb565(4, 18, 4) : rgb565(20, 20, 24);
    uint16_t border = (mode == MODE_SKY_SPY) ? rgb565(12, 96, 12) : rgb565(63, 63, 70);
    uint16_t grid = (mode == MODE_SKY_SPY) ? rgb565(0, 42, 0) : rgb565(52, 52, 58);
    uint16_t accent = (mode == MODE_SKY_SPY) ? rgb565(20, 255, 0) : rgb565(248, 113, 113);
    uint16_t text_main = (mode == MODE_SKY_SPY) ? rgb565(188, 255, 188) : rgb565(250, 250, 250);
    uint16_t text_dim = (mode == MODE_SKY_SPY) ? rgb565(80, 160, 80) : rgb565(161, 161, 170);
    uint16_t footer = (mode == MODE_SKY_SPY) ? rgb565(2, 4, 2) : rgb565(9, 9, 11);
    const char *title = (mode == MODE_SKY_SPY) ? "SKY MAP" : "FLOCK MAP";

    const int map_x = 0;
    const int map_y = 34;
    const int map_w = LCD_H_RES;
    const int map_h = 210;
    int footer_zoom = map_tile_zoom_for_idx(g_app.local_map_zoom_idx);

    /* Clear only the regions outside the tile area to avoid the visible
     * full-screen flash that causes map flicker on periodic redraws. */
    display_draw_rect(0, 0, LCD_H_RES, map_y, bg);
    display_draw_rect(0, map_y + map_h, LCD_H_RES, LCD_V_RES - (map_y + map_h), bg);

    display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 26, panel);
    display_draw_rect(0, DISPLAY_STATUS_DIV_Y, LCD_H_RES, 2, accent);
    display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, title, text_main, panel);
    display_draw_text_centered(DISPLAY_STATUS_SUB_Y, "Shared pinned-device view", text_dim, panel);

    {
        /* Center on pins when available, otherwise on a representative downloaded tile. */
        double center_lat = 0.0, center_lon = 0.0;
        bool fallback_view = false;
        bool cache_ok = map_tile_cache_ready();
        int any_zoom = cache_ok ? map_tile_max_zoom() : -1;
        int best_zoom = cache_ok ? map_tile_max_png_zoom() : -1;
        int requested_zoom = map_tile_zoom_for_idx(g_app.local_map_zoom_idx);
        bool prefer_png_center = (best_zoom >= 0);
        if (diag) ESP_LOGI(TAG, "map_view zoom_for_idx=%d cache=%d", requested_zoom, cache_ok);
        if (pin_count > 0) {
            for (int i = 0; i < pin_count; i++) {
                center_lat += pins[i].lat;
                center_lon += pins[i].lon;
            }
            center_lat /= (double)pin_count;
            center_lon /= (double)pin_count;
        } else if (cache_ok) {
            /* Only attempt fallback when tile cache is pre-warmed — never
             * trigger the slow SD directory scan from this render path. */
            int fallback_zoom = -1;
            if (map_tile_get_fallback_center(prefer_png_center, &fallback_zoom, &center_lat, &center_lon)) {
                fallback_view = true;
            }
        }

        if (pin_count <= 0 && !fallback_view) {
            log_shared_map_placeholder(mode, pin_count, requested_zoom);
            /* No pins and no tile fallback — show placeholder */
            display_draw_rect(map_x, map_y, map_w, map_h, bg);
            display_draw_bordered_rect(8, map_y + 6, LCD_H_RES - 16, map_h - 12, border, panel);
            for (int x = 26; x < LCD_H_RES - 16; x += 18)
                display_draw_rect(x, map_y + 7, 1, map_h - 14, grid);
            for (int y = map_y + 24; y < map_y + map_h - 12; y += 18)
                display_draw_rect(9, y, LCD_H_RES - 18, 1, grid);
            display_draw_text_centered(102, "NO MAP POINTS", text_main, panel);
            if (!cache_ok) {
                display_draw_text_centered(114, "Tiles loading...", text_dim, panel);
                display_draw_text_centered(126, "Try again in a moment", text_dim, panel);
            } else if (any_zoom >= 0 && best_zoom < 0) {
                display_draw_text_centered(114, "Tiles found, but LCD needs PNG", text_dim, panel);
                display_draw_text_centered(126, "Convert JPG/WEBP tiles to PNG", text_dim, panel);
            } else {
                display_draw_text_centered(114, "Open the web UI to save pins", text_dim, panel);
                display_draw_text_centered(126, "or track Fox/Sky targets live", text_dim, panel);
            }
            goto draw_footer;
        }

        if (diag) ESP_LOGI(TAG, "map_view center=%.5f,%.5f fallback=%d", center_lat, center_lon, fallback_view);
        /* Choose tile zoom level — only query tile system if cache is warm */
        int zoom = requested_zoom;
        bool tiles_available = (best_zoom >= 0);

        /* Resolve to the best-covered renderable zoom for this center, then cap
         * at 16 to bound tile count and decode cost. */
        if (tiles_available) {
            zoom = map_tile_resolve_view_zoom(requested_zoom,
                                              center_lat,
                                              center_lon,
                                              map_w,
                                              map_h,
                                              true);
        }
        if (tiles_available && zoom > best_zoom) zoom = best_zoom;
        if (zoom > 16) zoom = 16;
        footer_zoom = zoom;

        /* Skip tile rendering when free heap is below ~60 KB (PNG decode
         * temporarily allocates ~50 KB per tile). */
        if (tiles_available && esp_get_free_heap_size() < 60 * 1024) {
            if (diag) ESP_LOGW(TAG, "map_view skip tiles: low heap=%lu",
                     (unsigned long)esp_get_free_heap_size());
            tiles_available = false;
        }
        if (diag) ESP_LOGI(TAG, "map_view best_png=%d tiles_avail=%d heap=%lu",
                 best_zoom, tiles_available, (unsigned long)esp_get_free_heap_size());

        /* Compute center pixel in global tile-pixel space */
        double cpx, cpy;
        map_tile_latlon_to_pixel(center_lat, center_lon, zoom, &cpx, &cpy);

        /* The map viewport on the LCD: map_w × map_h pixels.
         * Global pixel coords for the top-left corner: */
        double origin_px = cpx - (map_w / 2.0);
        double origin_py = cpy - (map_h / 2.0);

        /* Always seed the viewport with a deterministic fallback background so
         * sparse tile sets do not leave stale or black gaps between draws. */
        display_draw_rect(map_x, map_y, map_w, map_h, panel);
        for (int x = map_x + 18; x < map_x + map_w; x += 18)
            display_draw_rect(x, map_y, 1, map_h, grid);
        for (int y = map_y + 18; y < map_y + map_h; y += 18)
            display_draw_rect(map_x, y, map_w, 1, grid);

        /* ── Draw tile background ── */
        bool has_tiles = false;
        int tile_x0 = -1;
        int tile_y0 = -1;
        int tile_x1 = -1;
        int tile_y1 = -1;
        if (tiles_available) {
            /* Determine which tiles cover the viewport */
            tile_x0 = (int)floor(origin_px / MAP_TILE_SIZE);
            tile_y0 = (int)floor(origin_py / MAP_TILE_SIZE);
            tile_x1 = (int)floor((origin_px + map_w - 1) / MAP_TILE_SIZE);
            tile_y1 = (int)floor((origin_py + map_h - 1) / MAP_TILE_SIZE);

            int max_tile = (1 << zoom) - 1;

            if (diag) ESP_LOGI(TAG, "map_view tile_draw start z=%d span=%d..%d/%d..%d heap=%lu",
                     zoom, tile_x0, tile_x1, tile_y0, tile_y1,
                     (unsigned long)esp_get_free_heap_size());
            for (int ty = tile_y0; ty <= tile_y1; ty++) {
                for (int tx = tile_x0; tx <= tile_x1; tx++) {
                    /* Wrap tile-x for world wrapping, skip out-of-range y */
                    int wtx = tx & max_tile;
                    if (ty < 0 || ty > max_tile) continue;

                    /* Pixel offset of this tile's top-left corner relative to viewport origin */
                    int tile_screen_x = (int)(tx * MAP_TILE_SIZE - origin_px);
                    int tile_screen_y = (int)(ty * MAP_TILE_SIZE - origin_py);

                    /* Source crop within the 256×256 tile */
                    int sx = 0, sy = 0;
                    int dx = map_x + tile_screen_x;
                    int dy = map_y + tile_screen_y;
                    int tw = MAP_TILE_SIZE, th = MAP_TILE_SIZE;

                    /* Clip left */
                    if (dx < map_x) { sx = map_x - dx; tw -= sx; dx = map_x; }
                    /* Clip top */
                    if (dy < map_y) { sy = map_y - dy; th -= sy; dy = map_y; }
                    /* Clip right */
                    if (dx + tw > map_x + map_w) tw = (map_x + map_w) - dx;
                    /* Clip bottom */
                    if (dy + th > map_y + map_h) th = (map_y + map_h) - dy;
                    if (tw <= 0 || th <= 0) continue;

                    if (map_tile_draw(zoom, wtx, ty, sx, sy, dx, dy, tw, th)) {
                        has_tiles = true;
                    }
                }
            }
        }

        log_shared_map_render(mode,
                              pin_count,
                              fallback_view,
                              requested_zoom,
                              zoom,
                              best_zoom,
                              tiles_available,
                              has_tiles,
                              center_lat,
                              center_lon,
                              tile_x0,
                              tile_y0,
                              tile_x1,
                              tile_y1);

        /* ── Center crosshair ── */
        int cx = map_x + map_w / 2;
        int cy = map_y + map_h / 2;
        display_draw_rect(cx - 8, cy, 17, 1, accent);
        display_draw_rect(cx, cy - 8, 1, 17, accent);

        /* ── Draw pin markers ── */
        if (!fallback_view) {
            for (int i = 0; i < pin_count; i++) {
                double ppx, ppy;
                map_tile_latlon_to_pixel(pins[i].lat, pins[i].lon, zoom, &ppx, &ppy);
                int px = map_x + (int)lrint(ppx - origin_px);
                int py = map_y + (int)lrint(ppy - origin_py);

                if (px < map_x + 2 || px > map_x + map_w - 5
                 || py < map_y + 2 || py > map_y + map_h - 5) continue;

                uint16_t pin_col = map_pin_color(pins[i].kind);
                display_draw_rect(px - 3, py - 3, 7, 7, rgb565(0, 0, 0));
                display_draw_rect(px - 2, py - 2, 5, 5, pin_col);

                char short_label[8] = {0};
                char kind_char = (pins[i].kind == MAP_PIN_KIND_DRONE) ? 'D'
                               : (pins[i].kind == MAP_PIN_KIND_FOX)   ? 'X'
                               : (pins[i].kind == MAP_PIN_KIND_SELF)  ? 'G' : 'F';
                if (pins[i].label[0]) {
                    snprintf(short_label, sizeof(short_label), "%c:%.3s", kind_char, pins[i].label);
                } else {
                    short_label[0] = kind_char;
                    short_label[1] = '*';
                    short_label[2] = '\0';
                }
                display_draw_text(px + 5, py - 3, short_label, rgb565(0, 0, 0), rgb565(0, 0, 0));
                display_draw_text(px + 4, py - 4, short_label, text_main, rgb565(0, 0, 0));
            }
        }

        /* ── Info bar below map ── */
        char summary[48];
        if (fallback_view) {
            snprintf(summary, sizeof(summary), "Tile fallback  Z%d%s",
                     zoom, has_tiles ? "" : " (no tiles)");
        } else {
            snprintf(summary, sizeof(summary), "Pins:%d  Z%d%s",
                     pin_count, zoom, has_tiles ? "" : " (no tiles)");
        }
        display_draw_text(4, map_y + map_h + 2, summary, text_main, bg);

        int list_y = map_y + map_h + 14;
        if (fallback_view) {
            display_draw_text(4, list_y, "Browsing available map tiles", text_main, bg);
            display_draw_text(4, list_y + 11, "Stable area with best zoom overlap", text_dim, bg);
        } else {
            for (int i = 0; i < pin_count && i < 3; i++) {
                char line[40];
                const char *kind = (pins[i].kind == MAP_PIN_KIND_DRONE) ? "D"
                                 : (pins[i].kind == MAP_PIN_KIND_FOX) ? "X"
                                 : (pins[i].kind == MAP_PIN_KIND_SELF) ? "G" : "F";
                snprintf(line, sizeof(line), "%s %s %ddBm", kind,
                         pins[i].label[0] ? pins[i].label : "Pinned", (int)pins[i].rssi);
                display_draw_text(4, list_y, line, map_pin_color(pins[i].kind), bg);
                list_y += 11;
            }
        }
    }

draw_footer:
    display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer);
    display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, 1, border);
    char foot[32];
    if (mode == MODE_FLOCK_YOU)
        snprintf(foot, sizeof(foot), "3x:back 2x:Z%d hold:sel", footer_zoom);
    else
        snprintf(foot, sizeof(foot), "3x:back 2x:Z%d", footer_zoom);
    display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, foot, text_dim, footer);
    display_unlock();
    if (diag) ESP_LOGI(TAG, "map_view DONE heap=%lu stack=%u",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
