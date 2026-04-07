/*
 * OUI-Spy C6 — ST7789V3 LCD display driver
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "app_common.h"

#define DISPLAY_SAFE_TOP        6
#define DISPLAY_SAFE_BOTTOM     6
#define DISPLAY_STATUS_BAR_Y    DISPLAY_SAFE_TOP
#define DISPLAY_STATUS_BAR_H    24
#define DISPLAY_STATUS_TEXT_Y   (DISPLAY_STATUS_BAR_Y + 4)
#define DISPLAY_STATUS_SUB_Y    (DISPLAY_STATUS_BAR_Y + 14)
#define DISPLAY_STATUS_DIV_Y    (DISPLAY_STATUS_BAR_Y + DISPLAY_STATUS_BAR_H)
#define DISPLAY_CONTENT_TOP     (DISPLAY_STATUS_DIV_Y + 4)
#define DISPLAY_FOOTER_BAR_H    16
#define DISPLAY_FOOTER_BAR_Y    (LCD_V_RES - DISPLAY_SAFE_BOTTOM - DISPLAY_FOOTER_BAR_H)
#define DISPLAY_FOOTER_TEXT_Y   (DISPLAY_FOOTER_BAR_Y + 4)

void display_init(void);
void display_set_brightness(uint8_t level);
void display_fill(uint16_t color);
void display_draw_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
int display_text_width(const char *text);
void display_draw_text_centered(int y, const char *text, uint16_t fg, uint16_t bg);
void display_draw_text_scaled_centered(int y, const char *text, uint16_t fg, uint16_t bg, int scale);
void display_draw_status(const char *mode_name, uint16_t accent_color);
void display_draw_hline(int x, int y, int w, uint16_t color);
void display_draw_bordered_rect(int x, int y, int w, int h, uint16_t border, uint16_t fill);
void display_draw_text_scaled(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale);
void display_blit_rgb565(int x, int y, int w, int h, const uint16_t *data);
void display_draw_shared_map_view(app_mode_t mode, bool initial_draw);
