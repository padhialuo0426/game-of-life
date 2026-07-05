#ifndef GAME_OF_LIFE_SPARSE_H
#define GAME_OF_LIFE_SPARSE_H

#include <stdbool.h>
#include <stddef.h>

/* An unbounded ("infinite") Game of Life world, stored sparsely as a hash set
   of live cell coordinates. Memory and per-generation cost scale with the live
   population, not with any grid area, so the world can span a vast coordinate
   range (roughly +/-2^30 on each axis) while only ever holding the live cells.

   Coordinates are signed ints. Cells whose neighbours would fall outside the
   supported coordinate range simply do not spawn there (the range is far larger
   than any pattern reached at interactive speeds). */
typedef struct SparseWorld SparseWorld;

/* Create an empty world / release one. sparse_new returns NULL on allocation
   failure. */
SparseWorld *sparse_new(void);
void sparse_free(SparseWorld *w);

/* Remove every live cell. */
void sparse_clear(SparseWorld *w);

/* Test / set the state of a single cell. */
bool sparse_get(const SparseWorld *w, int x, int y);
void sparse_set(SparseWorld *w, int x, int y, bool alive);

/* Number of live cells. */
size_t sparse_count(const SparseWorld *w);

/* Advance the world one generation in place using Conway's rules. */
void sparse_step(SparseWorld *w);

/* Bounding box of the live cells (inclusive). Returns false and leaves the
   outputs untouched when the world is empty. */
bool sparse_bounds(const SparseWorld *w, int *minx, int *miny,
                   int *maxx, int *maxy);

/* Replace the contents of dst with a copy of src's live cells. */
void sparse_copy(SparseWorld *dst, const SparseWorld *src);

#endif /* GAME_OF_LIFE_SPARSE_H */
