/*
 * OUI-Spy C6 — Slippy-map tile math + minimal PNG decoder
 *
 * The PNG decoder uses the ROM-resident tinfl (raw-deflate) decompressor
 * that ships in every ESP32-C6 ROM.  It handles non-interlaced 8-bit
 * Grayscale / RGB / Palette / GrayA / RGBA images — the formats produced
 * by every common OSM tile server.
 *
 * Memory budget (all heap, freed after each tile):
 *   tinfl_decompressor   ~11 KB
 *   LZ dictionary         32 KB
 *   compressed IDAT buf   variable  (capped at 48 KB)
 *   two row buffers       2 × (256×4+1) ≈ 2 KB
 *   LCD row buf (DMA)     172×2 = 344 B
 *   total peak            ≈ 50 KB
 *
 * SPDX-License-Identifier: MIT
 */
#include "map_tile.h"
#include "display.h"
#include "app_common.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "miniz.h"          /* ROM tinfl */

#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "map_tile";
#define MAX_IDAT_BYTES (48 * 1024)   /* refuse tiles with > 48 KB compressed */
#define MAX_TILE_FILE_BYTES (96 * 1024)
#define TILE_MIN_DECODE_HEAP (96 * 1024)
#define TILE_ZOOM_CACHE_MAX_LEVELS 20

typedef struct {
    int zoom;
    int representative_tile_x;
    int representative_tile_y;
    int min_tile_x;
    int max_tile_x;
    int min_tile_y;
    int max_tile_y;
} tile_zoom_level_info_t;

typedef struct {
    bool valid;
    int any_zoom;
    int png_zoom;
    int any_zooms[TILE_ZOOM_CACHE_MAX_LEVELS];
    size_t any_zoom_count;
    int png_zooms[TILE_ZOOM_CACHE_MAX_LEVELS];
    size_t png_zoom_count;
    tile_zoom_level_info_t any_levels[TILE_ZOOM_CACHE_MAX_LEVELS];
    size_t any_level_count;
    tile_zoom_level_info_t png_levels[TILE_ZOOM_CACHE_MAX_LEVELS];
    size_t png_level_count;
    int any_tile_x;
    int any_tile_y;
    int any_min_tile_x;
    int any_max_tile_x;
    int any_min_tile_y;
    int any_max_tile_y;
    int png_tile_x;
    int png_tile_y;
    int png_min_tile_x;
    int png_max_tile_x;
    int png_min_tile_y;
    int png_max_tile_y;
    int64_t scanned_at_us;
    char root[MAP_TILE_PATH_MAX];
} tile_zoom_cache_t;

static tile_zoom_cache_t s_tile_zoom_cache = {
    .valid = false,
    .any_zoom = -1,
    .png_zoom = -1,
    .any_zooms = {0},
    .any_zoom_count = 0,
    .png_zooms = {0},
    .png_zoom_count = 0,
    .any_levels = {{0}},
    .any_level_count = 0,
    .png_levels = {{0}},
    .png_level_count = 0,
    .any_tile_x = 0,
    .any_tile_y = 0,
    .any_min_tile_x = 0,
    .any_max_tile_x = 0,
    .any_min_tile_y = 0,
    .any_max_tile_y = 0,
    .png_tile_x = 0,
    .png_tile_y = 0,
    .png_min_tile_x = 0,
    .png_max_tile_x = 0,
    .png_min_tile_y = 0,
    .png_max_tile_y = 0,
    .scanned_at_us = 0,
    .root = {0},
};
static bool s_tile_zoom_cache_scanning = false;

static void reset_tile_zoom_cache(tile_zoom_cache_t *cache, int64_t scanned_at_us)
{
    if (!cache) return;

    memset(cache, 0, sizeof(*cache));
    cache->valid = false;
    cache->any_zoom = -1;
    cache->png_zoom = -1;
    cache->scanned_at_us = scanned_at_us;
}

static bool copy_string_checked(char *dst, size_t dst_sz, const char *src)
{
    size_t src_len = 0;

    if (!dst || dst_sz == 0 || !src) return false;

    src_len = strlen(src);
    if (src_len >= dst_sz) return false;

    memcpy(dst, src, src_len + 1);
    return true;
}

static bool join_path_checked(char *dst, size_t dst_sz, const char *base, const char *name)
{
    size_t base_len = 0;
    size_t name_len = 0;
    size_t write_pos = 0;
    bool need_sep = false;

    if (!dst || dst_sz == 0 || !base || !name) return false;

    base_len = strlen(base);
    name_len = strlen(name);
    need_sep = (base_len > 0 && base[base_len - 1] != '/');

    if (base_len + (need_sep ? 1U : 0U) + name_len >= dst_sz) return false;

    memcpy(dst, base, base_len);
    write_pos = base_len;
    if (need_sep) {
        dst[write_pos++] = '/';
    }
    memcpy(dst + write_pos, name, name_len);
    dst[write_pos + name_len] = '\0';
    return true;
}

static bool build_zoom_path(char *dst, size_t dst_sz, const char *map_root, int zoom)
{
    char zoom_component[16];
    int written = 0;

    written = snprintf(zoom_component, sizeof(zoom_component), "%d", zoom);
    if (written < 0 || (size_t)written >= sizeof(zoom_component)) return false;

    return join_path_checked(dst, dst_sz, map_root, zoom_component);
}

static bool build_tile_path(char *dst,
                            size_t dst_sz,
                            const char *map_root,
                            int zoom,
                            int tx,
                            int ty,
                            const char *ext)
{
    char relative_path[64];
    int written = 0;

    if (!ext) return false;

    written = snprintf(relative_path, sizeof(relative_path), "%d/%d/%d.%s", zoom, tx, ty, ext);
    if (written < 0 || (size_t)written >= sizeof(relative_path)) return false;

    return join_path_checked(dst, dst_sz, map_root, relative_path);
}

static const tile_zoom_level_info_t *tile_zoom_levels(bool png_only, size_t *out_count)
{
    if (out_count) {
        *out_count = png_only ? s_tile_zoom_cache.png_level_count : s_tile_zoom_cache.any_level_count;
    }
    return png_only ? s_tile_zoom_cache.png_levels : s_tile_zoom_cache.any_levels;
}

static const tile_zoom_level_info_t *find_level_info(bool png_only, int zoom)
{
    size_t level_count = 0;
    const tile_zoom_level_info_t *levels = tile_zoom_levels(png_only, &level_count);

    for (size_t i = 0; i < level_count; i++) {
        if (levels[i].zoom == zoom) return &levels[i];
    }
    return NULL;
}

static bool append_zoom_level(tile_zoom_level_info_t *levels,
                              size_t *level_count,
                              int zoom,
                              int representative_tile_x,
                              int representative_tile_y,
                              int min_tile_x,
                              int max_tile_x,
                              int min_tile_y,
                              int max_tile_y)
{
    if (!levels || !level_count || *level_count >= TILE_ZOOM_CACHE_MAX_LEVELS) return false;

    tile_zoom_level_info_t *level = &levels[*level_count];
    level->zoom = zoom;
    level->representative_tile_x = representative_tile_x;
    level->representative_tile_y = representative_tile_y;
    level->min_tile_x = min_tile_x;
    level->max_tile_x = max_tile_x;
    level->min_tile_y = min_tile_y;
    level->max_tile_y = max_tile_y;
    (*level_count)++;
    return true;
}

static void level_normalized_bounds(const tile_zoom_level_info_t *level,
                                    double *out_min_x,
                                    double *out_max_x,
                                    double *out_min_y,
                                    double *out_max_y)
{
    if (!level) return;

    double denom = (double)(1U << level->zoom);
    if (out_min_x) *out_min_x = (double)level->min_tile_x / denom;
    if (out_max_x) *out_max_x = (double)(level->max_tile_x + 1) / denom;
    if (out_min_y) *out_min_y = (double)level->min_tile_y / denom;
    if (out_max_y) *out_max_y = (double)(level->max_tile_y + 1) / denom;
}

static bool normalized_point_in_level(const tile_zoom_level_info_t *level,
                                      double normalized_x,
                                      double normalized_y)
{
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;

    if (!level) return false;

    level_normalized_bounds(level, &min_x, &max_x, &min_y, &max_y);
    return normalized_x >= min_x && normalized_x < max_x
        && normalized_y >= min_y && normalized_y < max_y;
}

static bool choose_preferred_focus_point(bool png_only,
                                         double *out_normalized_x,
                                         double *out_normalized_y,
                                         int *out_zoom)
{
    size_t level_count = 0;
    const tile_zoom_level_info_t *levels = tile_zoom_levels(png_only, &level_count);
    bool found = false;
    double best_x = 0.0;
    double best_y = 0.0;
    int best_support = -1;
    int best_zoom = -1;

    if (!levels || level_count == 0) return false;

    for (size_t i = 0; i < level_count; i++) {
        int support = 0;
        double candidate_x = 0.0;
        double candidate_y = 0.0;
        double denom = (double)(1U << levels[i].zoom);

        candidate_x = ((double)levels[i].representative_tile_x + 0.5) / denom;
        candidate_y = ((double)levels[i].representative_tile_y + 0.5) / denom;

        for (size_t j = 0; j < level_count; j++) {
            if (normalized_point_in_level(&levels[j], candidate_x, candidate_y)) {
                support++;
            }
        }

        if (!found || support > best_support
            || (support == best_support && levels[i].zoom > best_zoom)) {
            found = true;
            best_support = support;
            best_zoom = levels[i].zoom;
            best_x = candidate_x;
            best_y = candidate_y;
        }
    }

    if (!found) return false;

    if (out_normalized_x) *out_normalized_x = best_x;
    if (out_normalized_y) *out_normalized_y = best_y;
    if (out_zoom) *out_zoom = best_zoom;
    return true;
}

static int level_view_coverage_score(const tile_zoom_level_info_t *level,
                                     double center_lat,
                                     double center_lon,
                                     int viewport_w,
                                     int viewport_h,
                                     bool *out_center_covered)
{
    double cpx = 0.0;
    double cpy = 0.0;
    double origin_px = 0.0;
    double origin_py = 0.0;
    int tile_x0 = 0;
    int tile_x1 = 0;
    int tile_y0 = 0;
    int tile_y1 = 0;
    int center_tx = 0;
    int center_ty = 0;
    int overlap_x0 = 0;
    int overlap_x1 = 0;
    int overlap_y0 = 0;
    int overlap_y1 = 0;
    bool center_covered = false;

    if (!level || viewport_w <= 0 || viewport_h <= 0) return -1;

    map_tile_latlon_to_pixel(center_lat, center_lon, level->zoom, &cpx, &cpy);
    origin_px = cpx - (viewport_w / 2.0);
    origin_py = cpy - (viewport_h / 2.0);
    tile_x0 = (int)floor(origin_px / MAP_TILE_SIZE);
    tile_y0 = (int)floor(origin_py / MAP_TILE_SIZE);
    tile_x1 = (int)floor((origin_px + viewport_w - 1) / MAP_TILE_SIZE);
    tile_y1 = (int)floor((origin_py + viewport_h - 1) / MAP_TILE_SIZE);
    center_tx = (int)floor(cpx / MAP_TILE_SIZE);
    center_ty = (int)floor(cpy / MAP_TILE_SIZE);

    center_covered = center_tx >= level->min_tile_x && center_tx <= level->max_tile_x
                  && center_ty >= level->min_tile_y && center_ty <= level->max_tile_y;
    if (out_center_covered) *out_center_covered = center_covered;

    overlap_x0 = tile_x0 > level->min_tile_x ? tile_x0 : level->min_tile_x;
    overlap_x1 = tile_x1 < level->max_tile_x ? tile_x1 : level->max_tile_x;
    overlap_y0 = tile_y0 > level->min_tile_y ? tile_y0 : level->min_tile_y;
    overlap_y1 = tile_y1 < level->max_tile_y ? tile_y1 : level->max_tile_y;

    if (overlap_x1 < overlap_x0 || overlap_y1 < overlap_y0) {
        return center_covered ? 1 : 0;
    }

    return (overlap_x1 - overlap_x0 + 1) * (overlap_y1 - overlap_y0 + 1)
         + (center_covered ? 8 : 0);
}

static void fill_debug_family(map_tile_debug_family_t *out_family,
                              int zoom,
                              int tile_x,
                              int tile_y,
                              int min_tile_x,
                              int max_tile_x,
                              int min_tile_y,
                              int max_tile_y,
                              const int *zooms,
                              size_t zoom_count)
{
    if (!out_family) return;

    memset(out_family, 0, sizeof(*out_family));
    out_family->available = (zoom >= 0);
    out_family->highest_zoom = zoom;
    out_family->representative_tile_x = tile_x;
    out_family->representative_tile_y = tile_y;
    out_family->min_tile_x = min_tile_x;
    out_family->max_tile_x = max_tile_x;
    out_family->min_tile_y = min_tile_y;
    out_family->max_tile_y = max_tile_y;
    out_family->zoom_count = zoom_count;

    size_t copy_count = zoom_count;
    if (copy_count > (sizeof(out_family->zooms) / sizeof(out_family->zooms[0]))) {
        copy_count = sizeof(out_family->zooms) / sizeof(out_family->zooms[0]);
    }
    for (size_t i = 0; i < copy_count; i++) {
        out_family->zooms[i] = zooms[i];
    }
}

/* ──────────────────────────────────────────────────────────
 *  Slippy-tile coordinate helpers  (Web Mercator / EPSG:3857)
 * ────────────────────────────────────────────────────────── */

void map_tile_latlon_to_pixel(double lat, double lon, int zoom,
                              double *out_px, double *out_py)
{
    double n = (double)(1 << zoom);
    *out_px = (lon + 180.0) / 360.0 * n * MAP_TILE_SIZE;
    double lat_rad = lat * M_PI / 180.0;
    *out_py = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI)
              / 2.0 * n * MAP_TILE_SIZE;
}

/* ──────────────────────────────────────────────────────────
 *  File helpers
 * ────────────────────────────────────────────────────────── */

static bool entry_is_dot_dir(const char *name)
{
    return name && (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

static const char *resolve_map_root(void)
{
    struct stat st;

    /* 1. Try well-known fixed paths first (fast path). */
    static const char *roots[] = {
        "/sdcard/map",
        "/sdcard/maps",
        "/sdcard/Map",
        "/sdcard/Maps",
        "/sdcard/tiles",
        "/sdcard/Tiles",
        "/sdcard/sdcard/map",
        "/sdcard/sdcard/maps",
    };

    for (size_t i = 0; i < (sizeof(roots) / sizeof(roots[0])); i++) {
        if (stat(roots[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            ESP_LOGI(TAG, "map root (known): %s", roots[i]);
            return roots[i];
        }
    }

    /* 2. Check whether /sdcard itself contains numeric zoom directories
     *    (tiles placed directly at /sdcard/Z/X/Y.ext). */
    {
        DIR *sd = opendir("/sdcard");
        if (sd) {
            struct dirent *e;
            bool found_zoom = false;
            while ((e = readdir(sd)) != NULL) {
                if (entry_is_dot_dir(e->d_name)) continue;
                char *end = NULL;
                long val = strtol(e->d_name, &end, 10);
                if (end && *end == '\0' && val >= 0 && val <= 19) {
                    char sub[MAP_TILE_PATH_MAX];
                    if (!join_path_checked(sub, sizeof(sub), "/sdcard", e->d_name)) continue;
                    if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
                        found_zoom = true;
                        break;
                    }
                }
            }
            closedir(sd);
            if (found_zoom) {
                ESP_LOGI(TAG, "map root (direct): /sdcard");
                return "/sdcard";
            }
        }
    }

    /* 3. Auto-discover: scan top-level /sdcard directories for any child
     *    that contains a numeric zoom directory (0-19). */
    {
        static char discovered[MAP_TILE_PATH_MAX];
        DIR *sd = opendir("/sdcard");
        if (sd) {
            struct dirent *e;
            while ((e = readdir(sd)) != NULL) {
                if (entry_is_dot_dir(e->d_name)) continue;
                char candidate[MAP_TILE_PATH_MAX];
                if (!join_path_checked(candidate, sizeof(candidate), "/sdcard", e->d_name)) continue;
                if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                /* Skip known non-tile directories */
                if (strcmp(e->d_name, "ouispy_logs") == 0 ||
                    strcmp(e->d_name, "System Volume Information") == 0) continue;

                DIR *sub = opendir(candidate);
                if (!sub) continue;
                struct dirent *se;
                bool has_zoom = false;
                while ((se = readdir(sub)) != NULL) {
                    if (entry_is_dot_dir(se->d_name)) continue;
                    char *zend = NULL;
                    long zval = strtol(se->d_name, &zend, 10);
                    if (zend && *zend == '\0' && zval >= 0 && zval <= 19) {
                        char zpath[MAP_TILE_PATH_MAX];
                        if (!join_path_checked(zpath, sizeof(zpath), candidate, se->d_name)) continue;
                        if (stat(zpath, &st) == 0 && S_ISDIR(st.st_mode)) {
                            has_zoom = true;
                            break;
                        }
                    }
                }
                closedir(sub);
                if (has_zoom) {
                    if (!copy_string_checked(discovered, sizeof(discovered), candidate)) {
                        continue;
                    }
                    closedir(sd);
                    ESP_LOGI(TAG, "map root (discovered): %s", discovered);
                    return discovered;
                }
            }
            closedir(sd);
        }
    }

    ESP_LOGW(TAG, "no map root found on /sdcard");
    return NULL;
}

static bool find_matching_tile_in_zoom(const char *zoom_path,
                                       const char *const *exts,
                                       size_t ext_count,
                                       int *out_tile_x,
                                       int *out_tile_y)
{
    DIR *x_root = opendir(zoom_path);
    if (!x_root) return false;

    bool found = false;
    struct dirent *x_entry = NULL;
    char x_path[MAP_TILE_PATH_MAX];

    while (!found && (x_entry = readdir(x_root)) != NULL) {
        if (entry_is_dot_dir(x_entry->d_name)) continue;

        char *x_end = NULL;
        long x_val = strtol(x_entry->d_name, &x_end, 10);
        if (!x_end || *x_end != '\0' || x_val < 0 || x_val > INT32_MAX) continue;

        struct stat x_st = {0};
    if (!join_path_checked(x_path, sizeof(x_path), zoom_path, x_entry->d_name)) continue;
        if (stat(x_path, &x_st) != 0 || !S_ISDIR(x_st.st_mode)) continue;

        DIR *tile_dir = opendir(x_path);
        if (!tile_dir) continue;

        struct dirent *tile_entry = NULL;
        while (!found && (tile_entry = readdir(tile_dir)) != NULL) {
            if (entry_is_dot_dir(tile_entry->d_name)) continue;

            char *tile_end = NULL;
            long y_val = strtol(tile_entry->d_name, &tile_end, 10);
            if (!tile_end || tile_end == tile_entry->d_name || *tile_end != '.' || y_val < 0 || y_val > INT32_MAX) {
                continue;
            }

            for (size_t i = 0; i < ext_count; i++) {
                if (strcmp(tile_end + 1, exts[i]) == 0) {
                    found = true;
                    if (out_tile_x) *out_tile_x = (int)x_val;
                    if (out_tile_y) *out_tile_y = (int)y_val;
                    break;
                }
            }
        }

        closedir(tile_dir);
    }

    closedir(x_root);
    return found;
}

static bool scan_tile_bounds_in_zoom(const char *zoom_path,
                                     const char *const *exts,
                                     size_t ext_count,
                                     int *out_min_tile_x,
                                     int *out_max_tile_x,
                                     int *out_min_tile_y,
                                     int *out_max_tile_y)
{
    DIR *x_root = opendir(zoom_path);
    if (!x_root) return false;

    bool found = false;
    struct dirent *x_entry = NULL;
    char x_path[MAP_TILE_PATH_MAX];
    int min_tile_x = 0;
    int max_tile_x = 0;
    int min_tile_y = 0;
    int max_tile_y = 0;

    while ((x_entry = readdir(x_root)) != NULL) {
        if (entry_is_dot_dir(x_entry->d_name)) continue;

        char *x_end = NULL;
        long x_val = strtol(x_entry->d_name, &x_end, 10);
        if (!x_end || *x_end != '\0' || x_val < 0 || x_val > INT32_MAX) continue;

        struct stat x_st = {0};
    if (!join_path_checked(x_path, sizeof(x_path), zoom_path, x_entry->d_name)) continue;
        if (stat(x_path, &x_st) != 0 || !S_ISDIR(x_st.st_mode)) continue;

        DIR *tile_dir = opendir(x_path);
        if (!tile_dir) continue;

        struct dirent *tile_entry = NULL;
        while ((tile_entry = readdir(tile_dir)) != NULL) {
            if (entry_is_dot_dir(tile_entry->d_name)) continue;

            char *tile_end = NULL;
            long y_val = strtol(tile_entry->d_name, &tile_end, 10);
            if (!tile_end || tile_end == tile_entry->d_name || *tile_end != '.' || y_val < 0 || y_val > INT32_MAX) {
                continue;
            }

            bool ext_match = false;
            for (size_t i = 0; i < ext_count; i++) {
                if (strcmp(tile_end + 1, exts[i]) == 0) {
                    ext_match = true;
                    break;
                }
            }
            if (!ext_match) continue;

            if (!found) {
                min_tile_x = (int)x_val;
                max_tile_x = (int)x_val;
                min_tile_y = (int)y_val;
                max_tile_y = (int)y_val;
                found = true;
            } else {
                if ((int)x_val < min_tile_x) min_tile_x = (int)x_val;
                if ((int)x_val > max_tile_x) max_tile_x = (int)x_val;
                if ((int)y_val < min_tile_y) min_tile_y = (int)y_val;
                if ((int)y_val > max_tile_y) max_tile_y = (int)y_val;
            }
        }

        closedir(tile_dir);
    }

    closedir(x_root);
    if (!found) return false;

    if (out_min_tile_x) *out_min_tile_x = min_tile_x;
    if (out_max_tile_x) *out_max_tile_x = max_tile_x;
    if (out_min_tile_y) *out_min_tile_y = min_tile_y;
    if (out_max_tile_y) *out_max_tile_y = max_tile_y;
    return true;
}

static void refresh_tile_zoom_cache(void)
{
    const char *map_root = resolve_map_root();
    int64_t now_us = esp_timer_get_time();

    ESP_LOGI(TAG, "refresh_cache root=%s", map_root ? map_root : "(none)");

    if (!map_root) {
        reset_tile_zoom_cache(&s_tile_zoom_cache, now_us);
        s_tile_zoom_cache.valid = true;
        s_tile_zoom_cache.root[0] = '\0';
        s_tile_zoom_cache_scanning = false;
        return;
    }

    if (s_tile_zoom_cache.valid) {
        return;
    }
    if (s_tile_zoom_cache_scanning) {
        return;
    }

    static const char *const any_exts[] = {"png", "jpg", "jpeg", "webp"};
    static const char *const png_exts[] = {"png"};
    tile_zoom_cache_t new_cache;

    s_tile_zoom_cache_scanning = true;
    reset_tile_zoom_cache(&new_cache, 0);   /* updated after scan completes */
    if (!copy_string_checked(new_cache.root, sizeof(new_cache.root), map_root)) {
        ESP_LOGW(TAG, "map root path too long: %s", map_root);
        reset_tile_zoom_cache(&s_tile_zoom_cache, now_us);
        s_tile_zoom_cache.valid = true;
        s_tile_zoom_cache_scanning = false;
        return;
    }

    for (int z = 19; z >= 0; z--) {
        char zoom_path[MAP_TILE_PATH_MAX];
        struct stat st = {0};
        int any_tile_x = 0;
        int any_tile_y = 0;
        int png_tile_x = 0;
        int png_tile_y = 0;
        int any_min_tile_x = 0;
        int any_max_tile_x = 0;
        int any_min_tile_y = 0;
        int any_max_tile_y = 0;
        int png_min_tile_x = 0;
        int png_max_tile_x = 0;
        int png_min_tile_y = 0;
        int png_max_tile_y = 0;

        if (!build_zoom_path(zoom_path, sizeof(zoom_path), map_root, z)) continue;
        if (stat(zoom_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        bool any_found = find_matching_tile_in_zoom(zoom_path, any_exts,
                                                    sizeof(any_exts) / sizeof(any_exts[0]),
                                                    &any_tile_x, &any_tile_y);
        bool png_found = find_matching_tile_in_zoom(zoom_path, png_exts,
                                                    sizeof(png_exts) / sizeof(png_exts[0]),
                                                    &png_tile_x, &png_tile_y);

        if (any_found
            && scan_tile_bounds_in_zoom(zoom_path, any_exts, sizeof(any_exts) / sizeof(any_exts[0]),
                                        &any_min_tile_x, &any_max_tile_x,
                                        &any_min_tile_y, &any_max_tile_y)) {
            if (new_cache.any_zoom_count < (sizeof(new_cache.any_zooms) / sizeof(new_cache.any_zooms[0]))) {
                new_cache.any_zooms[new_cache.any_zoom_count++] = z;
            }
            append_zoom_level(new_cache.any_levels,
                              &new_cache.any_level_count,
                              z,
                              any_tile_x,
                              any_tile_y,
                              any_min_tile_x,
                              any_max_tile_x,
                              any_min_tile_y,
                              any_max_tile_y);
            if (new_cache.any_zoom < 0) {
                new_cache.any_zoom = z;
                new_cache.any_tile_x = any_tile_x;
                new_cache.any_tile_y = any_tile_y;
                new_cache.any_min_tile_x = any_min_tile_x;
                new_cache.any_max_tile_x = any_max_tile_x;
                new_cache.any_min_tile_y = any_min_tile_y;
                new_cache.any_max_tile_y = any_max_tile_y;
            }
            ESP_LOGI(TAG, "  zoom %d: any tiles x=%d..%d y=%d..%d", z,
                     any_min_tile_x, any_max_tile_x, any_min_tile_y, any_max_tile_y);
        }

        if (png_found
            && scan_tile_bounds_in_zoom(zoom_path, png_exts, sizeof(png_exts) / sizeof(png_exts[0]),
                                        &png_min_tile_x, &png_max_tile_x,
                                        &png_min_tile_y, &png_max_tile_y)) {
            if (new_cache.png_zoom_count < (sizeof(new_cache.png_zooms) / sizeof(new_cache.png_zooms[0]))) {
                new_cache.png_zooms[new_cache.png_zoom_count++] = z;
            }
            append_zoom_level(new_cache.png_levels,
                              &new_cache.png_level_count,
                              z,
                              png_tile_x,
                              png_tile_y,
                              png_min_tile_x,
                              png_max_tile_x,
                              png_min_tile_y,
                              png_max_tile_y);
            if (new_cache.png_zoom < 0) {
                new_cache.png_zoom = z;
                new_cache.png_tile_x = png_tile_x;
                new_cache.png_tile_y = png_tile_y;
                new_cache.png_min_tile_x = png_min_tile_x;
                new_cache.png_max_tile_x = png_max_tile_x;
                new_cache.png_min_tile_y = png_min_tile_y;
                new_cache.png_max_tile_y = png_max_tile_y;
            }
        }

        /* Scan all zoom levels — do not break early */
    }

    ESP_LOGI(TAG, "tile cache: any_zooms=%d png_zooms=%d root=%s",
             (int)new_cache.any_zoom_count, (int)new_cache.png_zoom_count, map_root);

    new_cache.valid = true;
    new_cache.scanned_at_us = esp_timer_get_time();
    s_tile_zoom_cache = new_cache;
    s_tile_zoom_cache_scanning = false;
}

void map_tile_invalidate_cache(void)
{
    s_tile_zoom_cache_scanning = false;
    s_tile_zoom_cache.valid = false;
    s_tile_zoom_cache.scanned_at_us = 0;
}

bool map_tile_cache_ready(void)
{
    return s_tile_zoom_cache.valid && !s_tile_zoom_cache_scanning && s_tile_zoom_cache.scanned_at_us > 0;
}

void map_tile_warm_cache(void)
{
    refresh_tile_zoom_cache();
}

#define TILE_WARM_STACK  12288

static void tile_warm_task(void *arg)
{
    refresh_tile_zoom_cache();
    vTaskDelete(NULL);
}

void map_tile_warm_cache_async(void)
{
    if (s_tile_zoom_cache.valid || s_tile_zoom_cache_scanning) return;            /* already warm/in progress */
    if (xTaskCreate(tile_warm_task, "tile_warm",
                    TILE_WARM_STACK, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "tile_warm task create failed, warming synchronously");
        refresh_tile_zoom_cache();                  /* fallback */
    }
}

static bool find_tile_path(int zoom, int tx, int ty,
                           char *path, size_t path_sz, bool *is_png)
{
    const char *map_root = resolve_map_root();
    if (!map_root) return false;

    struct stat st;
    /* Try PNG first (most common for OSM), then JPG/JPEG/WEBP */
    const char *exts[] = {"png", "jpg", "jpeg", "webp"};
    for (int i = 0; i < 4; i++) {
        if (!build_tile_path(path, path_sz, map_root, zoom, tx, ty, exts[i])) continue;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (is_png) *is_png = (i == 0);
            return true;
        }
    }
    return false;
}

bool map_tile_exists(int zoom, int tile_x, int tile_y)
{
    char path[MAP_TILE_PATH_MAX];
    return find_tile_path(zoom, tile_x, tile_y, path, sizeof(path), NULL);
}

int map_tile_max_zoom(void)
{
    refresh_tile_zoom_cache();
    return s_tile_zoom_cache.any_zoom;
}

int map_tile_max_png_zoom(void)
{
    refresh_tile_zoom_cache();
    return s_tile_zoom_cache.png_zoom;
}

size_t map_tile_available_zooms(int *out_zooms, size_t max_zooms, bool png_only)
{
    refresh_tile_zoom_cache();

    if (!out_zooms || max_zooms == 0) return 0;

    const int *src_zooms = png_only ? s_tile_zoom_cache.png_zooms : s_tile_zoom_cache.any_zooms;
    size_t src_count = png_only ? s_tile_zoom_cache.png_zoom_count : s_tile_zoom_cache.any_zoom_count;
    size_t count = src_count < max_zooms ? src_count : max_zooms;

    for (size_t i = 0; i < count; i++) {
        out_zooms[i] = src_zooms[src_count - 1 - i];
    }

    return count;
}

size_t map_tile_browsable_zooms(int *out_zooms, size_t max_zooms)
{
    int png_zooms[TILE_ZOOM_CACHE_MAX_LEVELS];
    int any_zooms[TILE_ZOOM_CACHE_MAX_LEVELS];
    size_t png_count = 0;
    size_t any_count = 0;
    size_t written = 0;

    if (!out_zooms || max_zooms == 0) return 0;

    png_count = map_tile_available_zooms(png_zooms, sizeof(png_zooms) / sizeof(png_zooms[0]), true);
    any_count = map_tile_available_zooms(any_zooms, sizeof(any_zooms) / sizeof(any_zooms[0]), false);

    for (size_t i = 0; i < png_count && written < max_zooms; i++) {
        out_zooms[written++] = png_zooms[i];
    }

    for (size_t i = 0; i < any_count && written < max_zooms; i++) {
        bool already_listed = false;
        for (size_t j = 0; j < written; j++) {
            if (out_zooms[j] == any_zooms[i]) {
                already_listed = true;
                break;
            }
        }
        if (!already_listed) {
            out_zooms[written++] = any_zooms[i];
        }
    }

    return written;
}

bool map_tile_available_zoom_json(char *buf, size_t buf_sz, bool png_only)
{
    int zooms[20];
    size_t zoom_count = 0;
    size_t len = 0;
    bool wrote_any = false;

    if (!buf || buf_sz < 3) return false;
    buf[0] = '[';
    buf[1] = ']';
    buf[2] = '\0';

    zoom_count = map_tile_available_zooms(zooms, sizeof(zooms) / sizeof(zooms[0]), png_only);
    if (zoom_count == 0) return false;

    len = 1;
    for (size_t i = 0; i < zoom_count; i++) {
        int written = snprintf(buf + len, buf_sz - len, "%s%d", wrote_any ? "," : "", zooms[i]);
        if (written <= 0 || (size_t)written >= (buf_sz - len)) {
            buf[0] = '[';
            buf[1] = ']';
            buf[2] = '\0';
            return false;
        }
        len += (size_t)written;
        wrote_any = true;
    }

    if (len + 2 > buf_sz) {
        buf[0] = '[';
        buf[1] = ']';
        buf[2] = '\0';
        return false;
    }
    buf[len++] = ']';
    buf[len] = '\0';
    return wrote_any;
}

bool map_tile_get_fallback_center(bool png_only, int *out_zoom, double *out_lat, double *out_lon)
{
    refresh_tile_zoom_cache();

    int zoom = png_only ? s_tile_zoom_cache.png_zoom : s_tile_zoom_cache.any_zoom;
    double normalized_x = 0.0;
    double normalized_y = 0.0;
    if (zoom < 0) return false;

    if (!choose_preferred_focus_point(png_only, &normalized_x, &normalized_y, &zoom)) {
        const tile_zoom_level_info_t *level = find_level_info(png_only, zoom);
        if (!level) return false;

        double denom = (double)(1U << zoom);
        normalized_x = ((double)level->representative_tile_x + 0.5) / denom;
        normalized_y = ((double)level->representative_tile_y + 0.5) / denom;
    }

    if (out_zoom) *out_zoom = zoom;
    if (out_lon) *out_lon = normalized_x * 360.0 - 180.0;
    if (out_lat) {
        double lat_rad = atan(sinh(M_PI * (1.0 - (2.0 * normalized_y))));
        *out_lat = lat_rad * 180.0 / M_PI;
    }
    return true;
}

int map_tile_resolve_view_zoom(int requested_zoom,
                               double center_lat,
                               double center_lon,
                               int viewport_w,
                               int viewport_h,
                               bool png_only)
{
    size_t level_count = 0;
    const tile_zoom_level_info_t *levels = NULL;
    int best_zoom = requested_zoom;
    int best_score = -1;
    int best_distance = INT32_MAX;

    refresh_tile_zoom_cache();
    levels = tile_zoom_levels(png_only, &level_count);
    if (!levels || level_count == 0) return requested_zoom;

    for (size_t i = 0; i < level_count; i++) {
        bool center_covered = false;
        int score = level_view_coverage_score(&levels[i],
                                              center_lat,
                                              center_lon,
                                              viewport_w,
                                              viewport_h,
                                              &center_covered);
        int distance = abs(levels[i].zoom - requested_zoom);

        if (score < 0) continue;
        if (score > best_score || (score == best_score && distance < best_distance)
            || (score == best_score && distance == best_distance && center_covered)) {
            best_score = score;
            best_distance = distance;
            best_zoom = levels[i].zoom;
        }
    }

    return best_zoom;
}

void map_tile_get_debug_info(map_tile_debug_info_t *out_info)
{
    if (!out_info) return;

    refresh_tile_zoom_cache();
    memset(out_info, 0, sizeof(*out_info));

    out_info->root_available = (s_tile_zoom_cache.root[0] != '\0');
    copy_string_checked(out_info->root, sizeof(out_info->root), s_tile_zoom_cache.root);

    if (s_tile_zoom_cache.scanned_at_us > 0) {
        int64_t now_us = esp_timer_get_time();
        int64_t age_us = now_us - s_tile_zoom_cache.scanned_at_us;
        if (age_us < 0) age_us = 0;
        if (age_us > ((int64_t)INT32_MAX * 1000LL)) {
            out_info->cache_age_ms = INT32_MAX;
        } else {
            out_info->cache_age_ms = (int32_t)(age_us / 1000LL);
        }
    }

    fill_debug_family(&out_info->any,
                      s_tile_zoom_cache.any_zoom,
                      s_tile_zoom_cache.any_tile_x,
                      s_tile_zoom_cache.any_tile_y,
                      s_tile_zoom_cache.any_min_tile_x,
                      s_tile_zoom_cache.any_max_tile_x,
                      s_tile_zoom_cache.any_min_tile_y,
                      s_tile_zoom_cache.any_max_tile_y,
                      s_tile_zoom_cache.any_zooms,
                      s_tile_zoom_cache.any_zoom_count);
    fill_debug_family(&out_info->png,
                      s_tile_zoom_cache.png_zoom,
                      s_tile_zoom_cache.png_tile_x,
                      s_tile_zoom_cache.png_tile_y,
                      s_tile_zoom_cache.png_min_tile_x,
                      s_tile_zoom_cache.png_max_tile_x,
                      s_tile_zoom_cache.png_min_tile_y,
                      s_tile_zoom_cache.png_max_tile_y,
                      s_tile_zoom_cache.png_zooms,
                      s_tile_zoom_cache.png_zoom_count);
}

/* ──────────────────────────────────────────────────────────
 *  Minimal PNG parser  (non-interlaced, 8-bit only)
 * ────────────────────────────────────────────────────────── */

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  p[3];
}

typedef struct {
    uint32_t width, height;
    uint8_t  bit_depth, color_type, interlace;
    uint8_t  bpp;           /* bytes per raw pixel (for filter math) */
    uint8_t  palette[256][3];
    int      palette_count;
} png_hdr_t;

static int png_bytes_per_pixel(uint8_t color_type)
{
    switch (color_type) {
    case 0: return 1;   /* grayscale */
    case 2: return 3;   /* RGB       */
    case 3: return 1;   /* palette   */
    case 4: return 2;   /* gray+A    */
    case 6: return 4;   /* RGBA      */
    default: return 0;
    }
}

/* Apply one of the five PNG row-filters in-place.
 * `row` has `row_bytes` data bytes; `prev` is the previous row (may be NULL). */
static void png_unfilter_row(uint8_t filter, uint8_t *row,
                             const uint8_t *prev,
                             int row_bytes, int bpp)
{
    switch (filter) {
    case 0: break;
    case 1:                     /* Sub  */
        for (int i = bpp; i < row_bytes; i++)
            row[i] += row[i - bpp];
        break;
    case 2:                     /* Up   */
        if (prev)
            for (int i = 0; i < row_bytes; i++)
                row[i] += prev[i];
        break;
    case 3:                     /* Average */
        for (int i = 0; i < row_bytes; i++) {
            uint8_t a = (i >= bpp) ? row[i - bpp] : 0;
            uint8_t b = prev ? prev[i] : 0;
            row[i] += (uint8_t)((a + b) / 2);
        }
        break;
    case 4: {                   /* Paeth  */
        for (int i = 0; i < row_bytes; i++) {
            int a = (i >= bpp) ? row[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int p = a + b - c;
            int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            row[i] += (uint8_t)((pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c);
        }
        break;
    }
    }
}

/* Convert one raw pixel to byte-swapped RGB565 matching rgb565(). */
static inline uint16_t px_to_565(const uint8_t *px, const png_hdr_t *h)
{
    uint8_t r, g, b;
    switch (h->color_type) {
    case 0:  r = g = b = px[0]; break;
    case 2:  r = px[0]; g = px[1]; b = px[2]; break;
    case 3: {
        uint8_t idx = px[0];
        if (idx < (uint8_t)h->palette_count) {
            r = h->palette[idx][0]; g = h->palette[idx][1]; b = h->palette[idx][2];
        } else { r = g = b = 0; }
        break;
    }
    case 4:  r = g = b = px[0]; break;          /* gray+alpha → gray */
    case 6:  r = px[0]; g = px[1]; b = px[2]; break;  /* RGBA → RGB */
    default: r = g = b = 128; break;
    }
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);                  /* LE→BE byte-swap */
}

/* ── Streaming PNG decode context ── */
typedef struct {
    /* PNG header */
    png_hdr_t hdr;

    /* Row assembly */
    uint8_t *row_cur;
    uint8_t *row_prev;
    int      row_bytes;     /* bpp * width */
    int      row_pos;       /* bytes accumulated in cur (includes filter byte) */
    int      current_row;   /* row index being assembled (0 … height-1) */

    /* Source crop parameters */
    int src_x0, src_y0;
    int out_w, out_h;

    /* LCD blit target */
    int dst_x0, dst_y0;
    uint16_t *lcd_row;      /* DMA-capable output buffer (out_w pixels) */

    bool ok;
} png_ctx_t;

/* Called whenever tinfl outputs a chunk of decompressed bytes.
 * Accumulates raw PNG scanlines, unfilters, converts, blits. */
static void png_feed_bytes(png_ctx_t *ctx, const uint8_t *data, size_t len)
{
    const int full_row = 1 + ctx->row_bytes;   /* filter byte + pixel data */

    while (len > 0 && ctx->ok) {
        /* How many bytes left to complete the current row? */
        int need = full_row - ctx->row_pos;
        int take = (int)len < need ? (int)len : need;

        if (ctx->row_pos == 0 && take > 0) {
            /* First byte is the filter type — store aside */
            ctx->row_cur[0] = 0;   /* will be set below */
        }

        /* Copy into row_cur.  Index 0 of row_cur holds the filter byte;
         * pixel data occupies [1 … row_bytes]. */
        if (ctx->row_pos == 0) {
            /* filter byte */
            ctx->row_cur[0] = data[0];
            if (take > 1)
                memcpy(ctx->row_cur + 1, data + 1, take - 1);
        } else {
            memcpy(ctx->row_cur + ctx->row_pos, data, take);
        }
        ctx->row_pos += take;
        data += take;
        len  -= take;

        if (ctx->row_pos < full_row) continue;  /* row not complete yet */

        /* ── Full row ready ── */
        ctx->row_pos = 0;

        uint8_t filter = ctx->row_cur[0];
        uint8_t *pixels = ctx->row_cur + 1;

        png_unfilter_row(filter, pixels,
                         ctx->current_row > 0 ? ctx->row_prev + 1 : NULL,
                         ctx->row_bytes, ctx->hdr.bpp);

        /* Only process visible rows */
        int y = ctx->current_row;
        if (y >= ctx->src_y0 && y < ctx->src_y0 + ctx->out_h) {
            /* Convert the visible x-range to RGB565 */
            int lcd_y = ctx->dst_y0 + (y - ctx->src_y0);
            for (int i = 0; i < ctx->out_w; i++) {
                int sx = ctx->src_x0 + i;
                if (sx < 0 || sx >= (int)ctx->hdr.width) {
                    ctx->lcd_row[i] = 0;
                } else {
                    ctx->lcd_row[i] = px_to_565(pixels + sx * ctx->hdr.bpp, &ctx->hdr);
                }
            }
            display_blit_rgb565(ctx->dst_x0, lcd_y, ctx->out_w, 1, ctx->lcd_row);
        }

        /* Swap cur/prev for filter reference */
        uint8_t *tmp = ctx->row_prev;
        ctx->row_prev = ctx->row_cur;
        ctx->row_cur  = tmp;
        ctx->current_row++;

        /* Early exit once we've passed the crop region */
        if (ctx->current_row >= ctx->src_y0 + ctx->out_h) {
            ctx->ok = true;   /* decoded everything we need */
            return;
        }
    }
}

/* ──────────────────────────────────────────────────────────
 *  map_tile_draw  — read, decompress, blit one tile crop
 * ────────────────────────────────────────────────────────── */

bool map_tile_draw(int zoom, int tile_x, int tile_y,
                   int src_x, int src_y,
                   int dst_x, int dst_y,
                   int w, int h)
{
    char path[128];
    bool is_png = false;
    struct stat st;
    size_t free_heap = esp_get_free_heap_size();
    size_t max_idat_bytes = MAX_IDAT_BYTES;
    if (!find_tile_path(zoom, tile_x, tile_y, path, sizeof(path), &is_png))
        return false;
    if (!is_png) return false;   /* only PNG supported for now */

    if (free_heap < TILE_MIN_DECODE_HEAP) {
        ESP_LOGW(TAG, "Skipping tile %s: low heap (%u bytes)", path, (unsigned)free_heap);
        return false;
    }
    if (stat(path, &st) == 0) {
        size_t file_size = (size_t)st.st_size;
        size_t heap_budget = free_heap / 3U;
        if (file_size > MAX_TILE_FILE_BYTES || file_size > heap_budget) {
            ESP_LOGW(TAG, "Skipping tile %s: file=%u heap=%u budget=%u",
                     path, (unsigned)file_size, (unsigned)free_heap, (unsigned)heap_budget);
            return false;
        }
    }
    if ((free_heap / 4U) < max_idat_bytes) {
        max_idat_bytes = free_heap / 4U;
    }
    if (max_idat_bytes < (16 * 1024)) {
        ESP_LOGW(TAG, "Skipping tile %s: IDAT budget too small (%u bytes)", path, (unsigned)max_idat_bytes);
        return false;
    }

    ESP_LOGD(TAG, "tile_draw z=%d tx=%d ty=%d heap=%lu",
             zoom, tile_x, tile_y, (unsigned long)esp_get_free_heap_size());

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    bool result = false;
    uint8_t *idat_buf = NULL;
    size_t   idat_len = 0;
    size_t   idat_cap = 0;

    /* ── 1. Read and validate PNG signature + IHDR ── */
    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8) goto done;
    static const uint8_t PNG_SIG[8] = {137,80,78,71,13,10,26,10};
    if (memcmp(sig, PNG_SIG, 8) != 0) goto done;

    png_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    /* Read chunks sequentially, collecting IHDR, PLTE, and IDAT data */

    for (;;) {
        uint8_t chunk_hdr[8];
        if (fread(chunk_hdr, 1, 8, f) != 8) break;
        uint32_t clen = read_be32(chunk_hdr);
        uint32_t ctype = read_be32(chunk_hdr + 4);

        if (ctype == 0x49484452) {  /* IHDR */
            uint8_t ihdr[13];
            if (clen < 13 || fread(ihdr, 1, 13, f) != 13) goto done;
            hdr.width      = read_be32(ihdr);
            hdr.height     = read_be32(ihdr + 4);
            hdr.bit_depth  = ihdr[8];
            hdr.color_type = ihdr[9];
            hdr.interlace  = ihdr[12];
            hdr.bpp        = (uint8_t)png_bytes_per_pixel(hdr.color_type);
            if (hdr.bit_depth != 8 || hdr.interlace != 0 || hdr.bpp == 0
                || hdr.width == 0 || hdr.width > 256
                || hdr.height == 0 || hdr.height > 256) {
                ESP_LOGD(TAG, "Unsupported PNG: %" PRIu32 "x%" PRIu32 " bd=%d ct=%d il=%d",
                         hdr.width, hdr.height, hdr.bit_depth, hdr.color_type, hdr.interlace);
                goto done;
            }
            fseek(f, 4, SEEK_CUR);  /* skip CRC */
        }
        else if (ctype == 0x504C5445) {  /* PLTE */
            if (clen > 768) { fseek(f, clen + 4, SEEK_CUR); continue; }
            uint8_t plte[768];
            if (fread(plte, 1, clen, f) != clen) goto done;
            hdr.palette_count = (int)(clen / 3);
            for (int i = 0; i < hdr.palette_count && i < 256; i++) {
                hdr.palette[i][0] = plte[i*3];
                hdr.palette[i][1] = plte[i*3+1];
                hdr.palette[i][2] = plte[i*3+2];
            }
            fseek(f, 4, SEEK_CUR);  /* skip CRC */
        }
        else if (ctype == 0x49444154) {  /* IDAT */
            if (idat_len + clen > max_idat_bytes) {
                ESP_LOGW(TAG, "Tile IDAT too large (%u + %u > %u)",
                         (unsigned)idat_len, (unsigned)clen, (unsigned)max_idat_bytes);
                goto done;
            }
            if (idat_len + clen > idat_cap) {
                size_t new_cap = idat_len + clen + 1024;
                if (new_cap > max_idat_bytes) new_cap = max_idat_bytes;
                uint8_t *new_buf = realloc(idat_buf, new_cap);
                if (!new_buf) goto done;
                idat_buf = new_buf;
                idat_cap = new_cap;
            }
            if (fread(idat_buf + idat_len, 1, clen, f) != clen) goto done;
            idat_len += clen;
            fseek(f, 4, SEEK_CUR);  /* skip CRC */
        }
        else if (ctype == 0x49454E44) {  /* IEND */
            break;
        }
        else {
            fseek(f, clen + 4, SEEK_CUR);  /* skip unknown chunk + CRC */
        }
    }

    if (!idat_buf || idat_len < 6 || hdr.width == 0) goto done;

    /* ── 2. Set up decode context ── */
    int row_bytes = (int)(hdr.bpp * hdr.width);
    int row_alloc = 1 + row_bytes;  /* filter byte + pixel data */

    /* Clamp crop region to tile bounds */
    if (src_x < 0) { dst_x -= src_x; w += src_x; src_x = 0; }
    if (src_y < 0) { dst_y -= src_y; h += src_y; src_y = 0; }
    if (src_x + w > (int)hdr.width)  w = (int)hdr.width  - src_x;
    if (src_y + h > (int)hdr.height) h = (int)hdr.height - src_y;
    if (w <= 0 || h <= 0) { result = true; goto done; }

    png_ctx_t *ctx = calloc(1, sizeof(png_ctx_t));
    if (!ctx) goto done;
    ctx->hdr      = hdr;
    ctx->row_bytes = row_bytes;
    ctx->src_x0   = src_x;
    ctx->src_y0   = src_y;
    ctx->out_w    = w;
    ctx->out_h    = h;
    ctx->dst_x0   = dst_x;
    ctx->dst_y0   = dst_y;
    ctx->ok       = true;

    ctx->row_cur  = malloc(row_alloc);
    ctx->row_prev = calloc(1, row_alloc);  /* zeroed = no previous row */
    ctx->lcd_row  = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!ctx->row_cur || !ctx->row_prev || !ctx->lcd_row) {
        free(ctx->row_cur); free(ctx->row_prev); free(ctx->lcd_row);
        free(ctx); goto done;
    }

    /* ── 3. Decompress IDAT via ROM tinfl ── */
    tinfl_decompressor *decomp = malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
    if (!decomp || !dict) {
        free(decomp); free(dict);
        free(ctx->row_cur); free(ctx->row_prev); free(ctx->lcd_row);
        free(ctx); goto done;
    }
    tinfl_init(decomp);

    {
        /* Skip the 2-byte zlib header manually so we feed raw deflate to tinfl.
         * zlib header: CMF (byte 0) + FLG (byte 1).  Skip them. */
        const uint8_t *zin = idat_buf + 2;
        size_t         zin_left = idat_len - 2;
        size_t         dict_ofs = 0;

        for (;;) {
            size_t in_bytes  = zin_left;
            size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;

            int flags = (zin_left > 0) ? TINFL_FLAG_HAS_MORE_INPUT : 0;

            tinfl_status status = tinfl_decompress(
                decomp, zin, &in_bytes,
                dict, dict + dict_ofs, &out_bytes, flags);

            zin      += in_bytes;
            zin_left -= in_bytes;

            if (out_bytes > 0 && ctx->ok) {
                png_feed_bytes(ctx, dict + dict_ofs, out_bytes);
            }

            dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);

            if (status == TINFL_STATUS_DONE) break;
            if (status < 0) {
                ESP_LOGD(TAG, "tinfl error %d", status);
                ctx->ok = false;
                break;
            }
            /* Early exit when we've already drawn all needed rows */
            if (ctx->current_row >= ctx->src_y0 + ctx->out_h) break;
        }
    }

    result = ctx->ok;

    free(decomp);
    free(dict);
    free(ctx->row_cur);
    free(ctx->row_prev);
    free(ctx->lcd_row);
    free(ctx);

done:
    free(idat_buf);
    fclose(f);
    return result;
}
