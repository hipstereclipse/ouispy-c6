/*
 * OUI-Spy C6 — Slippy-map tile math + PNG decoder for the LCD map view
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAP_TILE_SIZE 256

/* Convert lat/lon to global pixel coordinates at the given zoom level.
 * The world is 2^zoom * 256 pixels wide in Web Mercator. */
void map_tile_latlon_to_pixel(double lat, double lon, int zoom,
                              double *out_px, double *out_py);

/* Draw a cropped portion of a tile image from /sdcard/map/{z}/{tx}/{ty}.{ext}
 * to an arbitrary rectangle on the LCD.
 *   src_x,src_y : top-left pixel offset within the 256×256 tile
 *   dst_x,dst_y : LCD destination top-left
 *   w, h        : size to draw (clipped to tile bounds automatically)
 * Returns true if the tile was found and decoded successfully. */
bool map_tile_draw(int zoom, int tile_x, int tile_y,
                   int src_x, int src_y,
                   int dst_x, int dst_y,
                   int w, int h);

/* Quick check whether /sdcard/map/{z}/{tx}/{ty}.* exists. */
bool map_tile_exists(int zoom, int tile_x, int tile_y);

/* Return highest zoom level that has a directory in /sdcard/map/, or -1. */
int  map_tile_max_zoom(void);
