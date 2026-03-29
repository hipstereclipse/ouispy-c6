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

static esp_lcd_panel_handle_t s_panel    = NULL;
static esp_lcd_panel_io_handle_t s_io    = NULL;

/* 5×7 bitmap font (ASCII 32-126) - minimal built-in font */
#include "font5x7.h"

void display_init(void)
{
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
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_fill(uint16_t color)
{
    /* Fill in strips of 40 lines to stay within DMA budget */
    const int strip_h = 40;
    int buf_sz = LCD_H_RES * strip_h * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "display_fill: OOM");
        return;
    }
    for (int i = 0; i < LCD_H_RES * strip_h; i++) buf[i] = color;

    for (int y = 0; y < LCD_V_RES; y += strip_h) {
        int h = (y + strip_h > LCD_V_RES) ? (LCD_V_RES - y) : strip_h;
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + h, buf);
    }
    free(buf);
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    int buf_sz = w * h * sizeof(uint16_t);
    uint16_t *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    if (!buf) return;
    for (int i = 0; i < w * h; i++) buf[i] = color;
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, buf);
    free(buf);
}

void display_blit_rgb565(int x, int y, int w, int h, const uint16_t *data)
{
    if (!data || w <= 0 || h <= 0) return;
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, data);
}

void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg)
{
    if (!text) return;
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
    int width = display_text_width(text);
    int x = (LCD_H_RES - width) / 2;
    if (x < 0) x = 0;
    display_draw_text(x, y, text, fg, bg);
}

void display_draw_text_scaled_centered(int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    int width = display_text_width(text) * scale;
    int x = (LCD_H_RES - width) / 2;
    if (x < 0) x = 0;
    display_draw_text_scaled(x, y, text, fg, bg, scale);
}

void display_draw_status(const char *mode_name, uint16_t accent_color)
{
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
}

void display_draw_hline(int x, int y, int w, uint16_t color)
{
    display_draw_rect(x, y, w, 2, color);
}

void display_draw_bordered_rect(int x, int y, int w, int h, uint16_t border, uint16_t fill)
{
    display_draw_rect(x, y, w, h, fill);
    display_draw_rect(x, y, w, 1, border);
    display_draw_rect(x, y + h - 1, w, 1, border);
    display_draw_rect(x, y, 1, h, border);
    display_draw_rect(x + w - 1, y, 1, h, border);
}

void display_draw_text_scaled(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    if (!text || scale < 1) return;
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
}

/* Tile zoom level for each of the 5 zoom index values */
static int map_tile_zoom_for_idx(uint8_t idx)
{
    static const int zooms[] = {9, 10, 11, 12, 13};
    if (idx >= 5) idx = 4;
    return zooms[idx];
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

static int collect_local_map_pins(map_pin_t *out_pins, int max_pins)
{
    if (!out_pins || max_pins <= 0) return 0;

    int count = 0;
    for (int i = 0; i < g_app.shared_map_pin_count && count < max_pins; i++) {
        map_pin_t *src = &g_app.shared_map_pins[i];
        if (!isfinite(src->lat) || !isfinite(src->lon)) continue;
        out_pins[count++] = *src;
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
        }
    }

    if (g_app.sky_tracked_gps_samples > 0 && isfinite(g_app.sky_tracked_lat) && isfinite(g_app.sky_tracked_lon) && count < max_pins) {
        bool exists = false;
        uint8_t tracked_mac[6] = {0};
        if (g_app.sky_tracked_drone_idx >= 0 && g_app.sky_tracked_drone_idx < g_app.drone_count) {
            memcpy(tracked_mac, g_app.drones[g_app.sky_tracked_drone_idx].mac, sizeof(tracked_mac));
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
        }
    }

    return count;
}

void display_draw_shared_map_view(app_mode_t mode)
{
    map_pin_t pins[MAX_SHARED_MAP_PINS + 2];
    int pin_count = collect_local_map_pins(pins, (int)(sizeof(pins) / sizeof(pins[0])));

    uint16_t bg = (mode == MODE_SKY_SPY) ? rgb565(3, 8, 3) : rgb565(9, 9, 11);
    uint16_t panel = (mode == MODE_SKY_SPY) ? rgb565(4, 18, 4) : rgb565(20, 20, 24);
    uint16_t border = (mode == MODE_SKY_SPY) ? rgb565(12, 96, 12) : rgb565(63, 63, 70);
    uint16_t grid = (mode == MODE_SKY_SPY) ? rgb565(0, 42, 0) : rgb565(52, 52, 58);
    uint16_t accent = (mode == MODE_SKY_SPY) ? rgb565(20, 255, 0) : rgb565(248, 113, 113);
    uint16_t text_main = (mode == MODE_SKY_SPY) ? rgb565(188, 255, 188) : rgb565(250, 250, 250);
    uint16_t text_dim = (mode == MODE_SKY_SPY) ? rgb565(80, 160, 80) : rgb565(161, 161, 170);
    uint16_t footer = (mode == MODE_SKY_SPY) ? rgb565(2, 4, 2) : rgb565(9, 9, 11);
    const char *title = (mode == MODE_SKY_SPY) ? "SKY MAP" : "FLOCK MAP";

    display_fill(bg);
    display_draw_rect(0, DISPLAY_STATUS_BAR_Y, LCD_H_RES, 26, panel);
    display_draw_rect(0, DISPLAY_STATUS_DIV_Y, LCD_H_RES, 2, accent);
    display_draw_text_centered(DISPLAY_STATUS_TEXT_Y, title, text_main, panel);
    display_draw_text_centered(DISPLAY_STATUS_SUB_Y, "Shared pinned-device view", text_dim, panel);

    const int map_x = 0;
    const int map_y = 34;
    const int map_w = LCD_H_RES;
    const int map_h = 210;

    if (pin_count <= 0) {
        /* No pins — show placeholder */
        display_draw_bordered_rect(8, map_y + 6, LCD_H_RES - 16, map_h - 12, border, panel);
        for (int x = 26; x < LCD_H_RES - 16; x += 18)
            display_draw_rect(x, map_y + 7, 1, map_h - 14, grid);
        for (int y = map_y + 24; y < map_y + map_h - 12; y += 18)
            display_draw_rect(9, y, LCD_H_RES - 18, 1, grid);
        display_draw_text_centered(102, "NO MAP POINTS", text_main, panel);
        display_draw_text_centered(114, "Open the web UI to save pins", text_dim, panel);
        display_draw_text_centered(126, "or track Fox/Sky targets live", text_dim, panel);
    } else {
        /* Center on the mean position of all pins */
        double center_lat = 0.0, center_lon = 0.0;
        for (int i = 0; i < pin_count; i++) {
            center_lat += pins[i].lat;
            center_lon += pins[i].lon;
        }
        center_lat /= (double)pin_count;
        center_lon /= (double)pin_count;

        /* Choose tile zoom level */
        int zoom = map_tile_zoom_for_idx(g_app.local_map_zoom_idx);
        int best_zoom = map_tile_max_zoom();
        bool tiles_available = (best_zoom >= 0);

        /* If we have tiles but requested zoom is higher than available, clamp */
        if (tiles_available && zoom > best_zoom) zoom = best_zoom;

        /* Compute center pixel in global tile-pixel space */
        double cpx, cpy;
        map_tile_latlon_to_pixel(center_lat, center_lon, zoom, &cpx, &cpy);

        /* The map viewport on the LCD: map_w × map_h pixels.
         * Global pixel coords for the top-left corner: */
        double origin_px = cpx - (map_w / 2.0);
        double origin_py = cpy - (map_h / 2.0);

        /* ── Draw tile background ── */
        bool has_tiles = false;
        if (tiles_available) {
            /* Determine which tiles cover the viewport */
            int tile_x0 = (int)floor(origin_px / MAP_TILE_SIZE);
            int tile_y0 = (int)floor(origin_py / MAP_TILE_SIZE);
            int tile_x1 = (int)floor((origin_px + map_w - 1) / MAP_TILE_SIZE);
            int tile_y1 = (int)floor((origin_py + map_h - 1) / MAP_TILE_SIZE);

            int max_tile = (1 << zoom) - 1;

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

        if (!has_tiles) {
            /* Fallback: draw grid when no tiles are available */
            display_draw_rect(map_x, map_y, map_w, map_h, panel);
            for (int x = map_x + 18; x < map_x + map_w; x += 18)
                display_draw_rect(x, map_y, 1, map_h, grid);
            for (int y = map_y + 18; y < map_y + map_h; y += 18)
                display_draw_rect(map_x, y, map_w, 1, grid);
        }

        /* ── Center crosshair ── */
        int cx = map_x + map_w / 2;
        int cy = map_y + map_h / 2;
        display_draw_rect(cx - 8, cy, 17, 1, accent);
        display_draw_rect(cx, cy - 8, 1, 17, accent);

        /* ── Draw pin markers ── */
        for (int i = 0; i < pin_count; i++) {
            double ppx, ppy;
            map_tile_latlon_to_pixel(pins[i].lat, pins[i].lon, zoom, &ppx, &ppy);
            int px = map_x + (int)lrint(ppx - origin_px);
            int py = map_y + (int)lrint(ppy - origin_py);

            if (px < map_x + 2 || px > map_x + map_w - 5
             || py < map_y + 2 || py > map_y + map_h - 5) continue;

            uint16_t pin_col = map_pin_color(pins[i].kind);
            /* 5×5 filled square with 1px dark border for contrast */
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
            /* Label with dark shadow for readability on tile imagery */
            display_draw_text(px + 5, py - 3, short_label, rgb565(0, 0, 0), rgb565(0, 0, 0));
            display_draw_text(px + 4, py - 4, short_label, text_main, rgb565(0, 0, 0));
        }

        /* ── Info bar below map ── */
        char summary[48];
        snprintf(summary, sizeof(summary), "Pins:%d  Z%d%s",
                 pin_count, zoom, has_tiles ? "" : " (no tiles)");
        display_draw_text(4, map_y + map_h + 2, summary, text_main, bg);

        /* Up to 3 pin details */
        int list_y = map_y + map_h + 14;
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

    display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, DISPLAY_FOOTER_BAR_H, footer);
    display_draw_rect(0, DISPLAY_FOOTER_BAR_Y, LCD_H_RES, 1, border);
    char foot[32];
    int cur_zoom = map_tile_zoom_for_idx(g_app.local_map_zoom_idx);
    if (mode == MODE_FLOCK_YOU)
        snprintf(foot, sizeof(foot), "3x:back 2x:Z%d hold:sel", cur_zoom);
    else
        snprintf(foot, sizeof(foot), "3x:back 2x:Z%d", cur_zoom);
    display_draw_text_centered(DISPLAY_FOOTER_TEXT_Y, foot, text_dim, footer);
}
