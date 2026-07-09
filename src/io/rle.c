#include "io/rle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

static void set_err(char *err, size_t cap, const char *msg) {
    if (err != NULL && cap > 0) {
        snprintf(err, cap, "%s", msg);
    }
}

/* ------------------------------------------------------------------ */
/* Growable int-pair list                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int *xy;
    size_t count;
    size_t cap;
    bool oom;
} Cells;

static void cells_add(Cells *c, int x, int y) {
    if (c->oom) return;
    if (c->count == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 256;
        int *nx = realloc(c->xy, nc * 2 * sizeof(int));
        if (nx == NULL) {
            c->oom = true;
            return;
        }
        c->xy = nx;
        c->cap = nc;
    }
    c->xy[2 * c->count] = x;
    c->xy[2 * c->count + 1] = y;
    c->count++;
}

/* ------------------------------------------------------------------ */
/* Load                                                               */
/* ------------------------------------------------------------------ */

/* Hard limits so a corrupt or hostile file cannot hang the program or overflow
   the pen coordinates: no single run longer than RLE_RUN_MAX, no more than
   RLE_CELLS_MAX live cells in total, and the pen stays within RLE_COORD_MAX
   (comfortably inside int — the sparse world itself clamps at 2^30). */
#define RLE_RUN_MAX    10000000L   /* 10M cells per run token */
#define RLE_CELLS_MAX  10000000UL  /* 10M live cells per pattern */
#define RLE_COORD_MAX  (1L << 30)

bool rle_load(const char *path, int **cells, size_t *count,
              char *err, size_t errcap) {
    *cells = NULL;
    *count = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        set_err(err, errcap, "cannot open file");
        return false;
    }

    Cells c = {0};
    int cx = 0, cy = 0; /* current pen position */
    long run = 0;       /* pending run-length count */
    bool have_run = false;
    bool header_done = false; /* skipped the leading "x = ..." line yet? */
    bool done = false;        /* saw the '!' terminator */
    bool at_line_start = true;
    bool odd_states_live = false; /* rule family maps odd states -> alive */

    int ch;
    while (!done && (ch = fgetc(f)) != EOF) {
        /* Comment line (#...) — skip to end of line. */
        if (at_line_start && ch == '#') {
            while ((ch = fgetc(f)) != EOF && ch != '\n') { }
            at_line_start = true;
            continue;
        }
        /* The first non-comment line beginning with x/X is the header. Capture
           it to read the rule name: [R]History / [R]Super rules mark dead cells
           that were once alive with *even* states, so for those only the odd
           states count as live; every other multi-state family (e.g.
           Generations) treats any non-zero state as a live cell. */
        if (at_line_start && !header_done && (ch == 'x' || ch == 'X')) {
            header_done = true;
            char hdr[200];
            size_t hn = 0;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {
                if (hn + 1 < sizeof(hdr)) hdr[hn++] = (char)ch;
            }
            hdr[hn] = '\0';
            const char *r = strstr(hdr, "rule");
            if (r != NULL) {
                r += 4;
                while (*r == ' ' || *r == '\t' || *r == '=') r++;
                size_t rl = strcspn(r, " \t\r,");
                if ((rl >= 7 && strncasecmp(r + rl - 7, "History", 7) == 0) ||
                    (rl >= 5 && strncasecmp(r + rl - 5, "Super", 5) == 0)) {
                    odd_states_live = true;
                }
            }
            at_line_start = true;
            continue;
        }
        header_done = true;

        if (ch == '\n' || ch == '\r') { at_line_start = true; continue; }
        at_line_start = false;

        if (ch >= '0' && ch <= '9') {
            if (run <= RLE_RUN_MAX) run = run * 10 + (ch - '0'); /* saturate */
            have_run = true;
            continue;
        }

        long n = have_run ? run : 1;
        run = 0;
        have_run = false;

        /* Reject rather than loop/overflow on an absurd run or pen position:
           a corrupt file must fail cleanly, never hang or wrap coordinates. */
        bool bad = n > RLE_RUN_MAX;

        /* Cell tags, including Golly's multi-state RLE: 'b' and '.' are state
           0, 'o' is state 1, 'A'..'X' are states 1..24, and a 'p'..'y' prefix
           on a following 'A'..'X' encodes states 25..240. (Uppercase 'B'/'O'
           are states 2/15 per Golly — live in every family, since LifeHistory's
           even "was alive" markers are handled by odd_states_live above.) */
        int state = -1; /* -1: not a cell tag */
        if (ch == 'b' || ch == '.') {
            state = 0;
        } else if (ch == 'o') {
            state = 1;
        } else if (ch >= 'A' && ch <= 'X') {
            state = 1 + (ch - 'A');
        } else if (ch >= 'p' && ch <= 'y') {
            int c2 = fgetc(f);
            if (c2 >= 'A' && c2 <= 'X') {
                state = (ch - 'p' + 1) * 24 + (c2 - 'A' + 1);
            } else if (c2 != EOF) {
                ungetc(c2, f); /* stray prefix letter: ignore the tag */
            }
        }

        if (state >= 0) {
            bool alive = odd_states_live ? (state % 2 == 1) : (state != 0);
            if (bad || (long)cx + n > RLE_COORD_MAX ||
                (alive && c.count + (size_t)n > RLE_CELLS_MAX)) {
                c.count = 0; done = false; goto malformed;
            }
            if (alive) {
                for (long i = 0; i < n; i++) cells_add(&c, cx + (int)i, cy);
            }
            cx += (int)n;
        } else switch (ch) {
            case '$': /* end of line(s) */
                if (bad || (long)cy + n > RLE_COORD_MAX) { c.count = 0; done = false; goto malformed; }
                cy += (int)n;
                cx = 0;
                break;
            case '!': /* end of pattern */
                done = true;
                break;
            default:
                /* Ignore stray whitespace/tabs and unknown tag characters. */
                break;
        }
        if (c.oom) break;
    }
malformed:

    fclose(f);

    if (c.oom) {
        free(c.xy);
        set_err(err, errcap, "out of memory");
        return false;
    }
    if (!done && c.count == 0) {
        free(c.xy);
        set_err(err, errcap, "not a valid RLE pattern");
        return false;
    }

    *cells = c.xy;
    *count = c.count;
    return true;
}

/* ------------------------------------------------------------------ */
/* Save                                                               */
/* ------------------------------------------------------------------ */

/* Sort by row then column so we can emit rows in order. */
static int cmp_cell(const void *a, const void *b) {
    const int *pa = (const int *)a, *pb = (const int *)b;
    if (pa[1] != pb[1]) return (pa[1] > pb[1]) - (pa[1] < pb[1]); /* y */
    return (pa[0] > pb[0]) - (pa[0] < pb[0]);                     /* x */
}

/* Emit one RLE token (`n` copies of `tag`), wrapping output lines near 70
   columns as the format conventionally does. */
static void emit_token(FILE *f, int *col, long n, char tag) {
    if (n <= 0) return;
    char tok[24];
    int len = (n == 1) ? snprintf(tok, sizeof(tok), "%c", tag)
                       : snprintf(tok, sizeof(tok), "%ld%c", n, tag);
    if (*col + len > 70) {
        fputc('\n', f);
        *col = 0;
    }
    fwrite(tok, 1, (size_t)len, f);
    *col += len;
}

bool rle_save(const char *path, const int *cells, size_t count,
              char *err, size_t errcap) {
    /* Copy and normalise to a top-left origin. */
    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    for (size_t i = 0; i < count; i++) {
        int x = cells[2 * i], y = cells[2 * i + 1];
        if (i == 0 || x < minx) minx = x;
        if (i == 0 || y < miny) miny = y;
        if (i == 0 || x > maxx) maxx = x;
        if (i == 0 || y > maxy) maxy = y;
    }
    const int w = count ? (maxx - minx + 1) : 0;
    const int h = count ? (maxy - miny + 1) : 0;

    int *sorted = NULL;
    if (count > 0) {
        sorted = malloc(count * 2 * sizeof(int));
        if (sorted == NULL) {
            set_err(err, errcap, "out of memory");
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            sorted[2 * i] = cells[2 * i] - minx;
            sorted[2 * i + 1] = cells[2 * i + 1] - miny;
        }
        qsort(sorted, count, 2 * sizeof(int), cmp_cell);
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        free(sorted);
        set_err(err, errcap, "cannot open file for writing");
        return false;
    }

    fprintf(f, "x = %d, y = %d, rule = B3/S23\n", w, h);

    int col = 0;      /* current output column, for line wrapping */
    int cur_x = 0;    /* pen position while emitting */
    int cur_y = 0;
    size_t i = 0;
    while (i < count) {
        int ry = sorted[2 * i + 1];
        if (ry > cur_y) {
            emit_token(f, &col, ry - cur_y, '$'); /* blank rows */
            cur_y = ry;
            cur_x = 0;
        }
        /* One maximal run of consecutive live cells in this row. */
        int rx = sorted[2 * i];
        size_t j = i + 1;
        while (j < count && sorted[2 * j + 1] == ry && sorted[2 * j] == sorted[2 * (j - 1)] + 1) {
            j++;
        }
        int len = (int)(j - i);
        if (rx > cur_x) emit_token(f, &col, rx - cur_x, 'b'); /* dead gap */
        emit_token(f, &col, len, 'o');
        cur_x = rx + len;
        i = j;
    }
    emit_token(f, &col, 1, '!');
    fputc('\n', f);

    free(sorted);
    if (ferror(f)) {
        fclose(f);
        set_err(err, errcap, "write error");
        return false;
    }
    if (fclose(f) != 0) {
        set_err(err, errcap, "write error");
        return false;
    }
    return true;
}
