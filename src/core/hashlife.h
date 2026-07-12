#ifndef GAME_OF_LIFE_HASHLIFE_H
#define GAME_OF_LIFE_HASHLIFE_H

#include <stdbool.h>
#include <stddef.h>

/* A Hashlife (Gosper hashed-quadtree) engine for Conway's Game of Life. It is
   the second backend behind the engine.h seam, selected at runtime by the
   GOL_HASHLIFE environment variable. Its strength is far-forward jumping on
   structured / repetitive patterns: hl_advance(2^k) is a single memoised leap
   over 2^k generations, roughly O(log n) amortised, versus the sparse engine's
   O(n * population) of individual steps.

   The world is an immutable, canonically-interned quadtree of nodes: identical
   subpatterns share one node, so both space and time collapse on repetition.
   A snapshot is therefore just a root node id — O(1) to capture and keep, which
   is what makes the history ring nearly free with this backend.

   v1 does NOT garbage-collect the node pool: on chaotic / noisy patterns the
   pool grows without bound, so a memory-cap guard (hl_exhausted) stops advancing
   before the process is exhausted rather than running it out of memory. The
   interactive-edit and small-pattern cases are better served by the sparse
   engine, which stays the default. */
typedef struct Hashlife Hashlife;

/* An O(1) snapshot: a canonical root node id plus the root level. Valid only for
   the lifetime of the Hashlife it came from (it references that engine's node
   pool, which v1 never frees before the engine itself). */
typedef struct HashlifeSnapshot HashlifeSnapshot;

/* Create an empty world / release one. hl_new returns NULL on allocation
   failure. hl_free releases the whole node pool (and thus invalidates every
   outstanding snapshot from this engine). */
Hashlife *hl_new(void);
void hl_free(Hashlife *h);

/* Remove every live cell (repoints the root at the empty tree; the interned
   node pool is retained so outstanding snapshots stay valid). */
void hl_clear(Hashlife *h);

/* Test / set a single cell. Coordinates outside +/-2^30 are ignored on set. */
bool hl_get(const Hashlife *h, int x, int y);
void hl_set(Hashlife *h, int x, int y, bool alive);

/* Number of live cells (the root node's cached population). */
size_t hl_population(const Hashlife *h);

/* Advance n generations (n >= 0), leaping by memoised powers of two. If the
   memory-cap guard trips mid-advance, it stops early and hl_exhausted() returns
   true; the world is left at a valid earlier generation (a whole power-of-two
   leap is all-or-nothing, so the count is exact up to where it stopped). */
void hl_advance(Hashlife *h, long n);

/* Set by hl_advance when the node-pool memory cap was hit and it could not
   complete the requested advance. Cleared at the start of each hl_advance. */
bool hl_exhausted(const Hashlife *h);

/* Bounding box of the live cells (inclusive). Returns false and leaves the
   outputs untouched when the world is empty. */
bool hl_bounds(const Hashlife *h, int *minx, int *miny, int *maxx, int *maxy);

/* Invoke fn(x, y, ud) for every live cell inside the half-open rectangle
   [x0, x1) x [y0, y1). Cost is O(overlapping tree nodes + live cells inside). */
void hl_query(const Hashlife *h, int x0, int y0, int x1, int y1,
              void (*fn)(int x, int y, void *ud), void *ud);

/* Capture / restore / release a snapshot. hl_snapshot returns NULL on failure;
   hl_restore(h, NULL) empties the world. */
HashlifeSnapshot *hl_snapshot(const Hashlife *h);
void hl_restore(Hashlife *h, const HashlifeSnapshot *s);
void hl_snapshot_free(HashlifeSnapshot *s);
size_t hl_snapshot_bytes(const HashlifeSnapshot *s);

#endif /* GAME_OF_LIFE_HASHLIFE_H */
