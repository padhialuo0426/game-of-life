#include "render/sixel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pixel colour indices written into the intermediate bitmap. */
enum { PIX_DEAD = 0, PIX_ALIVE = 1, PIX_GRID = 2, PIX_CURSOR = 3, PIX_COUNT = 4 };

/* Palette, as RGB percentages (0..100) — the form the sixel colour introducer
   expects (#reg;2;r;g;b). */
static const int PALETTE[PIX_COUNT][3] = {
    {8, 8, 12},   /* dead:   dark panel so the board rectangle is visible   */
    {25, 90, 45}, /* alive:  green                                          */
    {16, 16, 22}, /* grid:   subtle lines between cells                     */
    {95, 85, 20}, /* cursor: yellow outline (edit mode)                     */
};

/* A grid gap is only drawn when cells are big enough for it not to swallow the
   cell; below this a cell is a solid block with no separating line. */
#define GRID_MIN_CELL_PX 4

/* ------------------------------------------------------------------ */
/* Growable output buffer                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom; /* sticky: set on any allocation failure */
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->oom) return;
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 8192;
    while (nc < b->len + extra + 1) nc *= 2;
    char *nd = realloc(b->data, nc);
    if (nd == NULL) {
        b->oom = true;
        return;
    }
    b->data = nd;
    b->cap = nc;
}

static void buf_putc(Buf *b, char c) {
    buf_reserve(b, 1);
    if (b->oom) return;
    b->data[b->len++] = c;
}

static void buf_puts(Buf *b, const char *s) {
    size_t l = strlen(s);
    buf_reserve(b, l);
    if (b->oom) return;
    memcpy(b->data + b->len, s, l);
    b->len += l;
}

static void buf_putint(Buf *b, int v) {
    char tmp[16];
    int l = snprintf(tmp, sizeof(tmp), "%d", v);
    if (l > 0) {
        buf_reserve(b, (size_t)l);
        if (b->oom) return;
        memcpy(b->data + b->len, tmp, (size_t)l);
        b->len += (size_t)l;
    }
}

/* Emit one run of `count` copies of sixel byte `ch` (0x3F..0x7E), using the
   run-length form only when it actually saves space. */
static void emit_run(Buf *b, int count, char ch) {
    if (count <= 0) return;
    if (count >= 4) {
        buf_putc(b, '!');
        buf_putint(b, count);
        buf_putc(b, ch);
    } else {
        for (int i = 0; i < count; i++) buf_putc(b, ch);
    }
}

/* ------------------------------------------------------------------ */
/* Incremental canvas                                                 */
/* ------------------------------------------------------------------ */

struct SixelCanvas {
    int cols, rows;      /* size in cells                                */
    int cell_px;         /* pixels per cell (square)                     */
    int W, H;            /* pixel dimensions = cols/rows * cell_px       */
    bool gap;            /* draw grid lines between cells                */
    unsigned char *pix;  /* W*H colour indices                           */
};

SixelCanvas *sixel_canvas_new(int cols, int rows, int cell_px) {
    if (cell_px < 1) cell_px = 1;
    if (cols < 1 || rows < 1) return NULL;

    SixelCanvas *c = malloc(sizeof(*c));
    if (c == NULL) return NULL;
    c->cols = cols;
    c->rows = rows;
    c->cell_px = cell_px;
    c->W = cols * cell_px;
    c->H = rows * cell_px;
    c->gap = cell_px >= GRID_MIN_CELL_PX;
    if (c->W <= 0 || c->H <= 0) {
        free(c);
        return NULL;
    }
    c->pix = malloc((size_t)c->W * (size_t)c->H);
    if (c->pix == NULL) {
        free(c);
        return NULL;
    }
    memset(c->pix, PIX_DEAD, (size_t)c->W * (size_t)c->H);

    /* Grid lines on the right/bottom edge of every cell, drawn once up front so
       plotting live cells never has to re-check them. */
    if (c->gap) {
        for (int cy = 0; cy < rows; cy++) {
            for (int cx = 0; cx < cols; cx++) {
                for (int py = 0; py < cell_px; py++) {
                    for (int px = 0; px < cell_px; px++) {
                        if (px == cell_px - 1 || py == cell_px - 1) {
                            c->pix[(size_t)(cy * cell_px + py) * (size_t)c->W +
                                   (size_t)(cx * cell_px + px)] = PIX_GRID;
                        }
                    }
                }
            }
        }
    }
    return c;
}

void sixel_canvas_free(SixelCanvas *canvas) {
    if (canvas == NULL) return;
    free(canvas->pix);
    free(canvas);
}

void sixel_canvas_set_alive(SixelCanvas *canvas, int col, int row) {
    if (col < 0 || col >= canvas->cols || row < 0 || row >= canvas->rows) return;
    const int cp = canvas->cell_px;
    const int ox = col * cp, oy = row * cp;
    for (int py = 0; py < cp; py++) {
        for (int px = 0; px < cp; px++) {
            /* Leave the grid lines untouched so cells stay separated. */
            if (canvas->gap && (px == cp - 1 || py == cp - 1)) continue;
            canvas->pix[(size_t)(oy + py) * (size_t)canvas->W + (size_t)(ox + px)] =
                PIX_ALIVE;
        }
    }
}

void sixel_canvas_set_cursor(SixelCanvas *canvas, int col, int row) {
    if (col < 0 || col >= canvas->cols || row < 0 || row >= canvas->rows) return;
    const int cp = canvas->cell_px;
    const int ox = col * cp, oy = row * cp;
    for (int py = 0; py < cp; py++) {
        for (int px = 0; px < cp; px++) {
            if (px == 0 || py == 0 || px == cp - 1 || py == cp - 1) {
                canvas->pix[(size_t)(oy + py) * (size_t)canvas->W +
                            (size_t)(ox + px)] = PIX_CURSOR;
            }
        }
    }
}

char *sixel_canvas_encode(const SixelCanvas *canvas, size_t *out_len) {
    const int W = canvas->W, H = canvas->H;
    const unsigned char *pix = canvas->pix;

    Buf b = {0};

    /* Device Control String start + raster attributes (1:1 pixel aspect). */
    buf_puts(&b, "\033Pq\"1;1;");
    buf_putint(&b, W);
    buf_putc(&b, ';');
    buf_putint(&b, H);

    /* Colour registers. */
    for (int i = 0; i < PIX_COUNT; i++) {
        buf_putc(&b, '#');
        buf_putint(&b, i);
        buf_puts(&b, ";2;");
        buf_putint(&b, PALETTE[i][0]);
        buf_putc(&b, ';');
        buf_putint(&b, PALETTE[i][1]);
        buf_putc(&b, ';');
        buf_putint(&b, PALETTE[i][2]);
    }

    /* Sixel data, in horizontal bands of six pixel rows. For each band, each
       colour that appears is written as one overlay pass. */
    unsigned char *line = malloc((size_t)W); /* one sixel byte per column */
    if (line == NULL) {
        free(b.data);
        return NULL;
    }
    for (int base = 0; base < H; base += 6) {
        const int rows = (H - base) < 6 ? (H - base) : 6;
        for (int color = 0; color < PIX_COUNT; color++) {
            bool any = false;
            for (int x = 0; x < W; x++) {
                unsigned char bits = 0;
                for (int r = 0; r < rows; r++) {
                    if (pix[(size_t)(base + r) * (size_t)W + (size_t)x] == color) {
                        bits |= (unsigned char)(1u << r);
                    }
                }
                if (bits) any = true;
                line[x] = (unsigned char)(0x3F + bits);
            }
            if (!any) continue; /* this colour paints nothing in this band */

            buf_putc(&b, '#');
            buf_putint(&b, color);
            int run = 1;
            for (int x = 1; x <= W; x++) {
                if (x < W && line[x] == line[x - 1]) {
                    run++;
                } else {
                    emit_run(&b, run, (char)line[x - 1]);
                    run = 1;
                }
            }
            buf_putc(&b, '$'); /* graphics CR: overlay the next colour */
        }
        buf_putc(&b, '-'); /* graphics NL: next band */
    }

    buf_puts(&b, "\033\\"); /* String Terminator */

    free(line);

    if (b.oom) {
        free(b.data);
        return NULL;
    }
    b.data[b.len] = '\0';
    if (out_len) *out_len = b.len;
    return b.data;
}

/* ------------------------------------------------------------------ */
/* Board rendering (built on the incremental canvas)                  */
/* ------------------------------------------------------------------ */

char *sixel_render_board(const Board *board, int cell_px,
                         int cursor_x, int cursor_y, bool cursor_on,
                         size_t *out_len) {
    SixelCanvas *c = sixel_canvas_new(board->width, board->height, cell_px);
    if (c == NULL) return NULL;

    for (int cy = 0; cy < board->height; cy++) {
        for (int cx = 0; cx < board->width; cx++) {
            if (board_get(board, cx, cy)) sixel_canvas_set_alive(c, cx, cy);
        }
    }
    if (cursor_on && cursor_x >= 0 && cursor_y >= 0) {
        sixel_canvas_set_cursor(c, cursor_x, cursor_y);
    }

    char *img = sixel_canvas_encode(c, out_len);
    sixel_canvas_free(c);
    return img;
}
