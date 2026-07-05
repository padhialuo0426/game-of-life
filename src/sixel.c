#include "sixel.h"

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
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

char *sixel_render_board(const Board *board, int cell_px,
                         int cursor_x, int cursor_y, bool cursor_on,
                         size_t *out_len) {
    if (cell_px < 1) cell_px = 1;
    const int W = board->width * cell_px;
    const int H = board->height * cell_px;
    if (W <= 0 || H <= 0) return NULL;

    /* Intermediate bitmap: one colour index per pixel. */
    unsigned char *pix = malloc((size_t)W * (size_t)H);
    if (pix == NULL) return NULL;

    const bool gap = cell_px >= GRID_MIN_CELL_PX;
    for (int cy = 0; cy < board->height; cy++) {
        for (int cx = 0; cx < board->width; cx++) {
            const unsigned char base = board_get(board, cx, cy) ? PIX_ALIVE : PIX_DEAD;
            const bool is_cursor = cursor_on && cx == cursor_x && cy == cursor_y;
            for (int py = 0; py < cell_px; py++) {
                for (int px = 0; px < cell_px; px++) {
                    unsigned char c = base;
                    /* Grid lines on the right/bottom edge of each cell. */
                    if (gap && (px == cell_px - 1 || py == cell_px - 1)) {
                        c = PIX_GRID;
                    }
                    /* Cursor outline sits on the cell's border. */
                    if (is_cursor && (px == 0 || py == 0 ||
                                      px == cell_px - 1 || py == cell_px - 1)) {
                        c = PIX_CURSOR;
                    }
                    pix[(size_t)(cy * cell_px + py) * (size_t)W +
                        (size_t)(cx * cell_px + px)] = c;
                }
            }
        }
    }

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
        free(pix);
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
    free(pix);

    if (b.oom) {
        free(b.data);
        return NULL;
    }
    b.data[b.len] = '\0';
    if (out_len) *out_len = b.len;
    return b.data;
}
