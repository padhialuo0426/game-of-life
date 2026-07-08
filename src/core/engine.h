#ifndef GAME_OF_LIFE_ENGINE_H
#define GAME_OF_LIFE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

/* Abstraction over a Life world engine. Today it wraps the sparse hash-set
   engine (src/sparse.*); the interface is intentionally the seam a future
   Hashlife (hashed-quadtree) backend would slot into, so the Jump/history
   machinery and the UI never touch the concrete engine. The two operations a
   Hashlife backend most wants to override are marked "ENGINE SEAM". */
typedef struct LifeEngine LifeEngine;

/* A cheap-to-store snapshot of the whole world state, used by the history ring,
   Reset, and edit-commit. Opaque: the sparse backend stores a packed live-cell
   array; a Hashlife backend would store a canonical quadtree node id, which is
   O(1) to keep because nodes are shared. ENGINE SEAM. */
typedef struct EngineSnapshot EngineSnapshot;

/* Create an empty world / release one. engine_new returns NULL on failure. */
LifeEngine *engine_new(void);
void engine_free(LifeEngine *e);

/* ENGINE SEAM: advance the world n generations (n >= 0). The sparse engine
   takes n individual O(population) steps; a Hashlife engine can leap in about
   O(log n) using its 2^k step-doubling. */
void engine_advance(LifeEngine *e, long n);

/* State access — used by rendering, editing and stats. */
size_t engine_population(const LifeEngine *e);
bool engine_get(const LifeEngine *e, int x, int y);
void engine_set(LifeEngine *e, int x, int y, bool alive);
void engine_clear(LifeEngine *e);
bool engine_bounds(const LifeEngine *e, int *minx, int *miny,
                   int *maxx, int *maxy);
void engine_query(const LifeEngine *e, int x0, int y0, int x1, int y1,
                  void (*fn)(int x, int y, void *ud), void *ud);

/* ENGINE SEAM: capture / restore the whole state. engine_snapshot returns NULL
   on allocation failure. engine_restore(e, NULL) empties the world. The caller
   frees a snapshot with engine_snapshot_free. */
EngineSnapshot *engine_snapshot(const LifeEngine *e);
void engine_restore(LifeEngine *e, const EngineSnapshot *s);
void engine_snapshot_free(EngineSnapshot *s);
size_t engine_snapshot_bytes(const EngineSnapshot *s);

#endif /* GAME_OF_LIFE_ENGINE_H */
