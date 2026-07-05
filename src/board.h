#ifndef GAME_OF_LIFE_BOARD_H
#define GAME_OF_LIFE_BOARD_H

#include <stdbool.h>
#include <stddef.h>

/* A rectangular grid of cells. Each cell is either alive or dead. */
typedef struct {
    int width;
    int height;
    unsigned char *cells; /* row-major, width * height entries, 0 or 1 */
} Board;

/* Create a board of the given size with all cells dead.
   Returns false on allocation failure. */
bool board_init(Board *board, int width, int height);

/* Release memory owned by the board. */
void board_free(Board *board);

/* Read/write a single cell. Coordinates outside the board are treated as dead
   (board_get) or ignored (board_set). */
bool board_get(const Board *board, int x, int y);
void board_set(Board *board, int x, int y, bool alive);

/* Set every cell to dead. */
void board_clear(Board *board);

/* Copy the cells of `src` into `dst`. Both boards must have identical
   dimensions. */
void board_copy(Board *dst, const Board *src);

/* Fill the board with a random pattern. `density` in [0.0, 1.0] is the
   probability that a given cell starts alive. */
void board_randomize(Board *board, double density);

/* Advance `current` one generation into `next` using Conway's rules.
   The two boards must have identical dimensions. When `wrap` is true the grid
   is toroidal (edges wrap around); when false it is a finite grid where cells
   beyond the border are treated as dead (patterns leaving the edge vanish). */
void board_step(const Board *current, Board *next, bool wrap);

#endif /* GAME_OF_LIFE_BOARD_H */
