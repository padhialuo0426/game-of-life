#include "board.h"

#include <stdlib.h>
#include <string.h>

bool board_init(Board *board, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    board->cells = calloc((size_t)width * (size_t)height, sizeof(unsigned char));
    if (board->cells == NULL) {
        return false;
    }
    board->width = width;
    board->height = height;
    return true;
}

void board_free(Board *board) {
    free(board->cells);
    board->cells = NULL;
    board->width = 0;
    board->height = 0;
}

bool board_get(const Board *board, int x, int y) {
    if (x < 0 || x >= board->width || y < 0 || y >= board->height) {
        return false;
    }
    return board->cells[(size_t)y * board->width + x] != 0;
}

void board_set(Board *board, int x, int y, bool alive) {
    if (x < 0 || x >= board->width || y < 0 || y >= board->height) {
        return;
    }
    board->cells[(size_t)y * board->width + x] = alive ? 1 : 0;
}

void board_clear(Board *board) {
    memset(board->cells, 0, (size_t)board->width * board->height);
}

void board_copy(Board *dst, const Board *src) {
    memcpy(dst->cells, src->cells, (size_t)src->width * src->height);
}

void board_randomize(Board *board, double density) {
    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            double r = (double)rand() / ((double)RAND_MAX + 1.0);
            board_set(board, x, y, r < density);
        }
    }
}

/* Count the live neighbours of (x, y). With `wrap` the grid is toroidal;
   without it, out-of-range neighbours are simply dead (board_get returns
   false), so the grid behaves as a finite world. */
static int count_neighbours(const Board *board, int x, int y, bool wrap) {
    int count = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            int nx = x + dx;
            int ny = y + dy;
            if (wrap) {
                nx = (nx + board->width) % board->width;
                ny = (ny + board->height) % board->height;
            }
            if (board_get(board, nx, ny)) {
                count++;
            }
        }
    }
    return count;
}

void board_step(const Board *current, Board *next, bool wrap) {
    for (int y = 0; y < current->height; y++) {
        for (int x = 0; x < current->width; x++) {
            int neighbours = count_neighbours(current, x, y, wrap);
            bool alive = board_get(current, x, y);
            /* Conway's rules: a live cell survives with 2 or 3 neighbours;
               a dead cell becomes alive with exactly 3 neighbours. */
            bool next_alive = alive ? (neighbours == 2 || neighbours == 3)
                                    : (neighbours == 3);
            board_set(next, x, y, next_alive);
        }
    }
}
