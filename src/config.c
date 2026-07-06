#define _POSIX_C_SOURCE 200809L /* getline, ssize_t */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A parsed pattern: a list of rows of varying length. */
typedef struct {
    char **rows;
    int count;
    int capacity;
    int max_width;
} Pattern;

static bool pattern_push(Pattern *p, const char *line, size_t len) {
    if (p->count == p->capacity) {
        int cap = p->capacity == 0 ? 16 : p->capacity * 2;
        char **rows = realloc(p->rows, (size_t)cap * sizeof(char *));
        if (rows == NULL) {
            return false;
        }
        p->rows = rows;
        p->capacity = cap;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';
    p->rows[p->count++] = copy;
    if ((int)len > p->max_width) {
        p->max_width = (int)len;
    }
    return true;
}

static void pattern_free(Pattern *p) {
    for (int i = 0; i < p->count; i++) {
        free(p->rows[i]);
    }
    free(p->rows);
}

static bool is_alive_char(char c) {
    return c != '.' && c != ' ' && c != '\0';
}

bool config_load_file(Board *board, const char *path, char *errbuf,
                      size_t errbuf_size) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        if (errbuf) {
            snprintf(errbuf, errbuf_size, "cannot open '%s'", path);
        }
        return false;
    }

    Pattern pat = {0};
    char *line = NULL;
    size_t linecap = 0;
    ssize_t n;
    bool ok = true;

    while ((n = getline(&line, &linecap, f)) != -1) {
        /* Strip trailing newline / carriage return. */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n > 0 && (line[0] == '!' || line[0] == '#')) {
            continue; /* comment */
        }
        if (!pattern_push(&pat, line, (size_t)n)) {
            ok = false;
            break;
        }
    }
    free(line);
    fclose(f);

    if (!ok) {
        if (errbuf) {
            snprintf(errbuf, errbuf_size, "out of memory reading '%s'", path);
        }
        pattern_free(&pat);
        return false;
    }
    if (pat.count == 0 || pat.max_width == 0) {
        if (errbuf) {
            snprintf(errbuf, errbuf_size, "'%s' contains no pattern", path);
        }
        pattern_free(&pat);
        return false;
    }

    /* Grow the board to hold the whole pattern (never shrink below the seed
       size) so a pattern larger than the -w/-h seed region is not clipped — the
       world is unbounded, so a loaded pattern must survive intact. */
    if (pat.max_width > board->width || pat.count > board->height) {
        int bw = pat.max_width > board->width ? pat.max_width : board->width;
        int bh = pat.count > board->height ? pat.count : board->height;
        board_free(board);
        if (!board_init(board, bw, bh)) {
            if (errbuf) {
                snprintf(errbuf, errbuf_size,
                         "pattern too large to allocate (%dx%d)", bw, bh);
            }
            pattern_free(&pat);
            return false;
        }
    }

    /* Center the pattern in the board. */
    board_clear(board);
    int offx = (board->width - pat.max_width) / 2;
    int offy = (board->height - pat.count) / 2;
    for (int r = 0; r < pat.count; r++) {
        const char *row = pat.rows[r];
        for (int c = 0; row[c] != '\0'; c++) {
            if (is_alive_char(row[c])) {
                board_set(board, offx + c, offy + r, true);
            }
        }
    }

    pattern_free(&pat);
    return true;
}
