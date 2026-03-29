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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "miniz.h"          /* ROM tinfl */

#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "map_tile";
#define MAP_ROOT "/sdcard/map"
#define MAX_IDAT_BYTES (48 * 1024)   /* refuse tiles with > 48 KB compressed */

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

static bool find_tile_path(int zoom, int tx, int ty,
                           char *path, size_t path_sz, bool *is_png)
{
    struct stat st;
    /* Try PNG first (most common for OSM), then JPG/JPEG/WEBP */
    const char *exts[] = {"png", "jpg", "jpeg", "webp"};
    for (int i = 0; i < 4; i++) {
        snprintf(path, path_sz, "%s/%d/%d/%d.%s", MAP_ROOT, zoom, tx, ty, exts[i]);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (is_png) *is_png = (i == 0);
            return true;
        }
    }
    return false;
}

bool map_tile_exists(int zoom, int tile_x, int tile_y)
{
    char path[128];
    return find_tile_path(zoom, tile_x, tile_y, path, sizeof(path), NULL);
}

int map_tile_max_zoom(void)
{
    struct stat st;
    for (int z = 19; z >= 0; z--) {
        char path[64];
        snprintf(path, sizeof(path), "%s/%d", MAP_ROOT, z);
        if (stat(path, &st) == 0) return z;
    }
    return -1;
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
    if (!find_tile_path(zoom, tile_x, tile_y, path, sizeof(path), &is_png))
        return false;
    if (!is_png) return false;   /* only PNG supported for now */

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
            if (idat_len + clen > MAX_IDAT_BYTES) {
                ESP_LOGW(TAG, "Tile IDAT too large (%u + %u)", (unsigned)idat_len, (unsigned)clen);
                goto done;
            }
            if (idat_len + clen > idat_cap) {
                size_t new_cap = idat_len + clen + 1024;
                if (new_cap > MAX_IDAT_BYTES) new_cap = MAX_IDAT_BYTES;
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
