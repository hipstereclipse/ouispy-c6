/*
 * OUI-Spy C6 — ST7789V3 LCD display driver
 * Uses esp_lcd high-level API for the 172×320 IPS panel.
 * SPDX-License-Identifier: MIT
 */
#include "display.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

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
        .miso_io_num   = -1,
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
        .rgb_ele_order    = LCD_RGB_ELEMENT_ORDER_BGR,
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

    /* Bottom info bar */
    uint16_t footer_bg = rgb565(214, 208, 196);
    uint16_t footer_fg = rgb565(58, 54, 49);
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
