#ifndef GAME_OF_LIFE_RLE_H
#define GAME_OF_LIFE_RLE_H

#include <stdbool.h>
#include <stddef.h>

/* Read/write the community-standard RLE pattern format (the format Golly,
   LifeWiki, etc. use), e.g.:

       #C a glider
       x = 3, y = 3, rule = B3/S23
       bo$2bo$3o!

   The rule field is accepted but ignored (this program is always B3/S23).
   Coordinates are pattern-local: a loaded pattern's top-left live cell sits near
   (0, 0); the caller offsets it into the world. */

/* Load the live cells from an RLE file at `path`. On success returns true and
   sets *cells to a malloc'd array of 2*(*count) ints (x, y pairs) — the caller
   frees it — and *count to the number of live cells. On failure returns false
   and writes a short message into `err` (if err/errcap are non-NULL/non-zero). */
bool rle_load(const char *path, int **cells, size_t *count,
              char *err, size_t errcap);

/* Write `count` live cells (2*count ints, x/y pairs) to an RLE file at `path`.
   The pattern is normalised so its top-left is (0, 0). Returns false and fills
   `err` on failure. */
bool rle_save(const char *path, const int *cells, size_t count,
              char *err, size_t errcap);

#endif /* GAME_OF_LIFE_RLE_H */
