#ifndef GAME_OF_LIFE_HISTORY_H
#define GAME_OF_LIFE_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

#include "core/engine.h"

/* A bounded ring of engine snapshots tagged by generation, giving cheap rewind
   to a recent generation without recomputing. It retains the most recent
   `capacity` recorded generations; recording past that discards (and frees) the
   oldest. Retention is also bounded by a total byte budget: when the retained
   snapshots exceed `budget_bytes`, the oldest are freed first (the newest is
   always kept), so a huge world cannot run the process out of memory — deep
   rewinds then replay from an earlier base instead. Engine-agnostic — it only
   stores opaque EngineSnapshots. */
typedef struct History History;

/* Create a ring holding up to `capacity` generations within `budget_bytes`
   (0 = unlimited) / release one. Returns NULL on allocation failure (capacity 0
   is treated as 1). */
History *history_new(size_t capacity, size_t budget_bytes);
void history_free(History *h);

/* Drop all retained snapshots (e.g. after an edit changes the timeline). */
void history_clear(History *h);

/* Record the state at generation `gen`. Takes ownership of `snap` (the ring
   frees it); a NULL `snap` is ignored. Generations are expected to advance by
   one; recording a `gen` that is not the previous newest + 1 signals a new
   timeline and clears the ring first. */
void history_record(History *h, long gen, EngineSnapshot *snap);

/* The snapshot recorded for exactly `gen`, or NULL if not retained. */
const EngineSnapshot *history_get(const History *h, long gen);

/* The retained snapshot with the largest generation <= `gen`, or NULL if none
   qualifies. On success *out_gen receives that generation. Used to pick the
   nearest base to replay a jump from. */
const EngineSnapshot *history_floor(const History *h, long gen, long *out_gen);

/* Total bytes held by the retained snapshots (for memory accounting). */
size_t history_bytes(const History *h);

#endif /* GAME_OF_LIFE_HISTORY_H */
