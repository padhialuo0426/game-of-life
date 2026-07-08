#define _DEFAULT_SOURCE

#include "kitty.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- pixel colours ----------------------------------------------------- */
#define DEAD_R  10
#define DEAD_G  10
#define DEAD_B  10
#define LIVE_R  220
#define LIVE_G  220
#define LIVE_B  220
#define GRID_R  30
#define GRID_G  30
#define GRID_B  30
#define CURSOR_R 180
#define CURSOR_G 100
#define CURSOR_B 0

struct KittyCanvas {
    int cols, rows;       /* cell grid dimensions */
    int cell_px;          /* screen pixels per cell */
    int pw, ph;           /* total pixel width / height */
    uint8_t *pixels;      /* RGB buffer: ph rows, pw*3 bytes per row */
    bool has_cursor;
    int cur_col, cur_row;
};

/* ---- helpers ------------------------------------------------------------ */

/* Fill a rectangle [px, px+pw) x [py, py+ph) with (r,g,b), clipped to canvas. */
static void fill_rect(KittyCanvas *c, int px, int py, int pw, int ph,
                      uint8_t r, uint8_t g, uint8_t b) {
    if (px < 0 || py < 0) return;
    if (px + pw > c->pw) pw = c->pw - px;
    if (py + ph > c->ph) ph = c->ph - py;
    if (pw <= 0 || ph <= 0) return;
    for (int y = py; y < py + ph; y++) {
        uint8_t *row = c->pixels + (size_t)y * c->pw * 3;
        for (int x = px; x < px + pw; x++) {
            size_t off = (size_t)x * 3;
            row[off]     = r;
            row[off + 1] = g;
            row[off + 2] = b;
        }
    }
}

/* Draw a 1-pixel-thick grid line at the left/top edge of every cell except row 0
   and column 0 (so the grid lives "between" cells). Only drawn when cell_px >= 5,
   matching the sixel canvas's grid threshold. */
static void draw_grid(KittyCanvas *c) {
    if (c->cell_px < 5) return;
    /* horizontal grid lines */
    for (int row = 1; row < c->rows; row++) {
        int py = row * c->cell_px;
        for (int x = 0; x < c->pw; x++) {
            size_t off = ((size_t)py * c->pw + (size_t)x) * 3;
            c->pixels[off]     = GRID_R;
            c->pixels[off + 1] = GRID_G;
            c->pixels[off + 2] = GRID_B;
        }
    }
    /* vertical grid lines */
    for (int col = 1; col < c->cols; col++) {
        int px = col * c->cell_px;
        for (int y = 0; y < c->ph; y++) {
            size_t off = ((size_t)y * c->pw + (size_t)px) * 3;
            c->pixels[off]     = GRID_R;
            c->pixels[off + 1] = GRID_G;
            c->pixels[off + 2] = GRID_B;
        }
    }
}

/* ---- public API --------------------------------------------------------- */

KittyCanvas *kitty_canvas_new(int cols, int rows, int cell_px) {
    if (cols <= 0 || rows <= 0 || cell_px <= 0) return NULL;
    KittyCanvas *c = calloc(1, sizeof(*c));
    if (c == NULL) return NULL;
    c->cols    = cols;
    c->rows    = rows;
    c->cell_px = cell_px;
    c->pw      = cols * cell_px;
    c->ph      = rows * cell_px;
    size_t nbytes = (size_t)c->pw * c->ph * 3;
    c->pixels = malloc(nbytes);
    if (c->pixels == NULL) { free(c); return NULL; }
    /* Fill with dead colour, then overlay the grid. */
    for (size_t i = 0; i < nbytes; i += 3) {
        c->pixels[i]     = DEAD_R;
        c->pixels[i + 1] = DEAD_G;
        c->pixels[i + 2] = DEAD_B;
    }
    draw_grid(c);
    return c;
}

void kitty_canvas_set_alive(KittyCanvas *c, int col, int row) {
    if (col < 0 || col >= c->cols || row < 0 || row >= c->rows) return;
    int px = col * c->cell_px;
    int py = row * c->cell_px;
    fill_rect(c, px, py, c->cell_px, c->cell_px, LIVE_R, LIVE_G, LIVE_B);
    /* Redraw the interior grid lines that the fill may have overwritten. */
    if (c->cell_px >= 5) {
        for (int cc = col + 1; cc < c->cols && cc * c->cell_px < px + c->cell_px; cc++)
            ; /* grid would be at multiples of cell_px; the fill is within one cell */
        /* Redraw the bottom and right inner grid edges of this cell. */
        int bx = (col + 1) * c->cell_px;
        if (bx < c->pw) {
            for (int y = py; y < py + c->cell_px; y++) {
                size_t off = ((size_t)y * c->pw + (size_t)bx) * 3;
                c->pixels[off] = GRID_R; c->pixels[off+1] = GRID_G; c->pixels[off+2] = GRID_B;
            }
        }
        int by = (row + 1) * c->cell_px;
        if (by < c->ph) {
            for (int x = px; x < px + c->cell_px; x++) {
                size_t off = ((size_t)by * c->pw + (size_t)x) * 3;
                c->pixels[off] = GRID_R; c->pixels[off+1] = GRID_G; c->pixels[off+2] = GRID_B;
            }
        }
    }
}

void kitty_canvas_set_cursor(KittyCanvas *c, int col, int row) {
    if (col < 0 || col >= c->cols || row < 0 || row >= c->rows) return;
    c->has_cursor = true;
    c->cur_col = col;
    c->cur_row = row;
}

/* ---- encoding ----------------------------------------------------------- */

/* Base64 alphabet. */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode len bytes as base64; returns a malloc'd NUL-terminated string and
   writes its length (excluding the NUL) to *out_len. */
static char *b64_encode(const uint8_t *data, size_t len, size_t *out_len) {
    size_t elen = ((len + 2) / 3) * 4;
    char *out = malloc(elen + 1);
    if (out == NULL) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)data[i + 2];
        out[j++] = B64[(v >> 18) & 0x3f];
        out[j++] = B64[(v >> 12) & 0x3f];
        out[j++] = (i + 1 < len) ? B64[(v >> 6) & 0x3f] : '=';
        out[j++] = (i + 2 < len) ? B64[v & 0x3f]       : '=';
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

/* Save every pixel in the 2-pixel-wide outline of the cursor cell, then draw the
   outline in CURSOR colour. Returns a malloc'd buffer the caller must free, or
   NULL on allocation failure. The buffer format is a count followed by
   (offset, r, g, b) triples. */
static uint8_t *cursor_save_and_draw(KittyCanvas *c, int *nbytes) {
    const int t = 2;                   /* outline thickness in pixels */
    int cx = c->cur_col * c->cell_px;
    int cy = c->cur_row * c->cell_px;
    int cw_px = c->cell_px, ch_px = c->cell_px;

    /* Count pixels in the outline. */
    int count = 0;
    /* top + bottom strips */
    for (int dx = 0; dx < cw_px; dx++) {
        if (cx + dx < c->pw) {
            for (int ty = 0; ty < t; ty++) {
                if (cy + ty          < c->ph) count++;
                if (cy + ch_px - 1 - ty >= 0 && ty < t) count++;
            }
        }
    }
    /* left + right strips (skip corners counted above) */
    for (int dy = t; dy < ch_px - t; dy++) {
        if (cy + dy < c->ph) {
            for (int tx = 0; tx < t; tx++) {
                if (cx + tx            < c->pw) count++;
                if (cx + cw_px - 1 - tx >= 0 && tx < t) count++;
            }
        }
    }

    int buf_sz = sizeof(int) + count * ((int)sizeof(size_t) + 3);
    uint8_t *buf = malloc((size_t)buf_sz);
    if (buf == NULL) return NULL;
    memcpy(buf, &count, sizeof(int));
    uint8_t *wp = buf + sizeof(int);
    int saved = 0;

    for (int dx = 0; dx < cw_px; dx++) {
        int px = cx + dx;
        if (px >= c->pw) continue;
        for (int ty = 0; ty < t; ty++) {
            int py_top = cy + ty;
            if (py_top < c->ph) {
                size_t off = ((size_t)py_top * c->pw + (size_t)px) * 3;
                memcpy(wp, &off, sizeof(size_t)); wp += sizeof(size_t);
                *wp++ = c->pixels[off]; *wp++ = c->pixels[off+1]; *wp++ = c->pixels[off+2];
                c->pixels[off] = CURSOR_R; c->pixels[off+1] = CURSOR_G; c->pixels[off+2] = CURSOR_B;
                saved++;
            }
            int py_bot = cy + ch_px - 1 - ty;
            if (py_bot >= 0 && py_bot < c->ph) {
                size_t off = ((size_t)py_bot * c->pw + (size_t)px) * 3;
                memcpy(wp, &off, sizeof(size_t)); wp += sizeof(size_t);
                *wp++ = c->pixels[off]; *wp++ = c->pixels[off+1]; *wp++ = c->pixels[off+2];
                c->pixels[off] = CURSOR_R; c->pixels[off+1] = CURSOR_G; c->pixels[off+2] = CURSOR_B;
                saved++;
            }
        }
    }
    for (int dy = t; dy < ch_px - t; dy++) {
        int py = cy + dy;
        if (py >= c->ph) continue;
        for (int tx = 0; tx < t; tx++) {
            int px_l = cx + tx;
            if (px_l < c->pw) {
                size_t off = ((size_t)py * c->pw + (size_t)px_l) * 3;
                memcpy(wp, &off, sizeof(size_t)); wp += sizeof(size_t);
                *wp++ = c->pixels[off]; *wp++ = c->pixels[off+1]; *wp++ = c->pixels[off+2];
                c->pixels[off] = CURSOR_R; c->pixels[off+1] = CURSOR_G; c->pixels[off+2] = CURSOR_B;
                saved++;
            }
            int px_r = cx + cw_px - 1 - tx;
            if (px_r >= 0 && px_r < c->pw) {
                size_t off = ((size_t)py * c->pw + (size_t)px_r) * 3;
                memcpy(wp, &off, sizeof(size_t)); wp += sizeof(size_t);
                *wp++ = c->pixels[off]; *wp++ = c->pixels[off+1]; *wp++ = c->pixels[off+2];
                c->pixels[off] = CURSOR_R; c->pixels[off+1] = CURSOR_G; c->pixels[off+2] = CURSOR_B;
                saved++;
            }
        }
    }
    *nbytes = (int)(wp - buf);
    (void)saved;
    return buf;
}

/* Restore from the buffer produced by cursor_save_and_draw. */
static void cursor_restore(KittyCanvas *c, const uint8_t *buf) {
    int count;
    memcpy(&count, buf, sizeof(int));
    const uint8_t *rp = buf + sizeof(int);
    for (int i = 0; i < count; i++) {
        size_t off;
        memcpy(&off, rp, sizeof(size_t)); rp += sizeof(size_t);
        c->pixels[off]     = *rp++;
        c->pixels[off + 1] = *rp++;
        c->pixels[off + 2] = *rp++;
    }
}

char *kitty_canvas_encode(KittyCanvas *c, size_t *out_len) {
    /* Temporarily draw the cursor outline before compressing. */
    uint8_t *savebuf = NULL;
    int save_bytes = 0;
    if (c->has_cursor) {
        savebuf = cursor_save_and_draw(c, &save_bytes);
        if (savebuf == NULL) return NULL;
    }

    /* zlib-compress the pixel buffer (best compression — the images are highly
       redundant since most cells are dead). */
    uLongf src_len = (uLongf)c->pw * c->ph * 3;
    uLongf dst_bnd = compressBound(src_len);
    uint8_t *zbuf = malloc(dst_bnd);
    if (zbuf == NULL) { free(savebuf); return NULL; }
    int zr = compress2(zbuf, &dst_bnd, c->pixels, src_len, Z_BEST_COMPRESSION);
    if (zr != Z_OK) { free(zbuf); free(savebuf); return NULL; }

    /* Restore cursor pixels now the compression is done. */
    if (savebuf) { cursor_restore(c, savebuf); free(savebuf); }

    /* Base64-encode the compressed payload. */
    size_t b64_len = 0;
    char *b64 = b64_encode(zbuf, dst_bnd, &b64_len);
    free(zbuf);
    if (b64 == NULL) return NULL;

    /* Build the KGP escape sequence (Kitty Graphics Protocol):
       \033_G  a=T  action: transmit + display
               f=24 24-bit RGB, 8 bits per channel, no alpha
               s=<pw>,v=<ph> pixel dimensions
               o=z  payload is zlib-compressed
               z=-1 place the image behind text (so popups/HUD read on top)
               C=1  don't move the cursor after placing
               ;<base64-encoded zlib-compressed payload>
       \033\   ST (string terminator, two bytes) */
    char hdr[160];
    int hl = snprintf(hdr, sizeof(hdr),
                      "\033_Ga=T,f=24,s=%d,v=%d,o=z,z=-1,C=1;", c->pw, c->ph);

    /* KGP string terminator is ESC \ (two bytes). */
    size_t total = (size_t)hl + b64_len + 2;
    char *out = malloc(total + 1); /* +1 for trailing NUL convenience */
    if (out == NULL) { free(b64); return NULL; }

    memcpy(out, hdr, (size_t)hl);
    memcpy(out + hl, b64, b64_len);
    out[hl + b64_len]     = '\033';
    out[hl + b64_len + 1] = '\\';
    out[total] = '\0';

    free(b64);
    *out_len = total;
    return out;
}

void kitty_canvas_free(KittyCanvas *c) {
    if (c == NULL) return;
    free(c->pixels);
    free(c);
}
