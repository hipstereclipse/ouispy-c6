/*
 * OUI-Spy C6 — Slippy-map tile math + PNG decoder for the LCD map view
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MAP_TILE_SIZE 256
#define MAP_TILE_DEBUG_MAX_ZOOMS 20
#define MAP_TILE_PATH_MAX 320

typedef struct {
    bool available;
    int highest_zoom;
    int representative_tile_x;
    int representative_tile_y;
    int min_tile_x;
    int max_tile_x;
    int min_tile_y;
    int max_tile_y;
    size_t zoom_count;
    int zooms[MAP_TILE_DEBUG_MAX_ZOOMS];
} map_tile_debug_family_t;

typedef struct {
    bool root_available;
    char root[MAP_TILE_PATH_MAX];
    int32_t cache_age_ms;
    map_tile_debug_family_t any;
    map_tile_debug_family_t png;
} map_tile_debug_info_t;

/* Return LCD-oriented browsable zoom levels.
 * PNG-backed zooms are listed first so zoom cycling prefers levels that can
 * actually draw imagery on-device, then non-PNG-only levels are appended. */
size_t map_tile_browsable_zooms(int *out_zooms, size_t max_zooms);

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

/* Return highest zoom level that has at least one tile in /sdcard/map/, or -1. */
int  map_tile_max_zoom(void);

/* Return highest zoom level that has at least one PNG tile for LCD rendering, or -1. */
int  map_tile_max_png_zoom(void);

/* Collect available zoom levels in ascending order. Returns the number written. */
size_t map_tile_available_zooms(int *out_zooms, size_t max_zooms, bool png_only);

/* Write a JSON array of available zoom levels into the provided buffer. */
bool map_tile_available_zoom_json(char *buf, size_t buf_sz, bool png_only);

/* Return a representative tile center from downloaded tiles. */
bool map_tile_get_fallback_center(bool png_only, int *out_zoom, double *out_lat, double *out_lon);

/* Resolve the requested zoom to the best-covered level for the current map
 * center and viewport, preferring levels whose tile bounds overlap the view. */
int map_tile_resolve_view_zoom(int requested_zoom,
                               double center_lat,
                               double center_lon,
                               int viewport_w,
                               int viewport_h,
                               bool png_only);

/* Snapshot the cached tile-scan state for diagnostics and web debugging. */
void map_tile_get_debug_info(map_tile_debug_info_t *out_info);

/* Discard the cached scan result so the next access triggers a fresh SD scan.
 * Call this whenever the card is mounted, unmounted, or tiles are written. */
void map_tile_invalidate_cache(void);

/* Returns true if the tile-zoom cache is already populated (no filesystem I/O
 * required).  Use this from tight render loops to avoid blocking the UI task. */
bool map_tile_cache_ready(void);

/* Synchronously populate the tile-zoom cache.  Call from a task with adequate
 * stack and time budget (e.g. mode startup) so the UI task never triggers it. */
void map_tile_warm_cache(void);

/* Fire-and-forget async version — spawns a one-shot FreeRTOS task so the
 * caller (and the main loop) are not blocked while the SD card is scanned.
 * Safe to call multiple times; redundant calls return immediately. */
void map_tile_warm_cache_async(void);
