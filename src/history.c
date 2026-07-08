#include "history.h"

#include <stdlib.h>

/* Snapshots are stored in a direct-mapped ring: the entry for generation g lives
   in slot g % cap and records its own gen so a slot can be told apart from a
   stale one that wrapped onto the same index. A generation is retained iff it is
   in the window (newest - cap, newest] and its slot still carries that gen. */
typedef struct {
    long gen;
    EngineSnapshot *snap;
} Slot;

struct History {
    Slot *buf;
    size_t cap;
    long newest;
    bool any;
    size_t bytes;  /* total engine_snapshot_bytes of the retained snapshots */
    size_t budget; /* byte budget; 0 = unlimited */
};

History *history_new(size_t capacity, size_t budget_bytes) {
    if (capacity == 0) capacity = 1;
    History *h = malloc(sizeof(*h));
    if (h == NULL) return NULL;
    h->buf = calloc(capacity, sizeof(Slot)); /* snap = NULL, gen = 0 */
    if (h->buf == NULL) {
        free(h);
        return NULL;
    }
    h->cap = capacity;
    h->newest = 0;
    h->any = false;
    h->bytes = 0;
    h->budget = budget_bytes;
    return h;
}

void history_free(History *h) {
    if (h == NULL) return;
    for (size_t i = 0; i < h->cap; i++) engine_snapshot_free(h->buf[i].snap);
    free(h->buf);
    free(h);
}

void history_clear(History *h) {
    if (h == NULL) return;
    for (size_t i = 0; i < h->cap; i++) {
        engine_snapshot_free(h->buf[i].snap);
        h->buf[i].snap = NULL;
        h->buf[i].gen = 0;
    }
    h->any = false;
    h->newest = 0;
    h->bytes = 0;
}

/* Is generation `gen` currently retained? */
static bool retained(const History *h, long gen) {
    if (!h->any || gen < 0 || gen > h->newest) return false;
    if (gen <= h->newest - (long)h->cap) return false;
    const Slot *s = &h->buf[(size_t)(gen % (long)h->cap)];
    return s->snap != NULL && s->gen == gen;
}

void history_record(History *h, long gen, EngineSnapshot *snap) {
    if (h == NULL || snap == NULL || gen < 0) {
        engine_snapshot_free(snap);
        return;
    }
    /* A non-contiguous generation means the timeline branched (e.g. we rewound
       then advanced again): drop the old line so stale futures can't be read. */
    if (h->any && gen != h->newest + 1) history_clear(h);

    Slot *s = &h->buf[(size_t)(gen % (long)h->cap)];
    h->bytes -= engine_snapshot_bytes(s->snap); /* 0 for NULL */
    engine_snapshot_free(s->snap);
    s->snap = snap;
    s->gen = gen;
    h->newest = gen;
    h->any = true;
    h->bytes += engine_snapshot_bytes(snap);

    /* Enforce the byte budget: free the oldest retained snapshots first. The
       newest is always kept (so the ring is never empty and a snapshot larger
       than the whole budget still bounds memory at ~one snapshot); a rewind
       past the evicted range replays from an earlier base instead. */
    if (h->budget > 0 && h->bytes > h->budget) {
        long lo = h->newest - (long)h->cap + 1;
        if (lo < 0) lo = 0;
        for (long g = lo; g < h->newest && h->bytes > h->budget; g++) {
            Slot *e = &h->buf[(size_t)(g % (long)h->cap)];
            if (e->snap != NULL && e->gen == g) {
                h->bytes -= engine_snapshot_bytes(e->snap);
                engine_snapshot_free(e->snap);
                e->snap = NULL;
                e->gen = 0;
            }
        }
    }
}

const EngineSnapshot *history_get(const History *h, long gen) {
    if (h == NULL || !retained(h, gen)) return NULL;
    return h->buf[(size_t)(gen % (long)h->cap)].snap;
}

const EngineSnapshot *history_floor(const History *h, long gen, long *out_gen) {
    if (h == NULL || !h->any) return NULL;
    long g = gen < h->newest ? gen : h->newest;
    const long floor = h->newest - (long)h->cap; /* exclusive lower bound */
    for (; g > floor && g >= 0; g--) {
        if (retained(h, g)) {
            if (out_gen) *out_gen = g;
            return h->buf[(size_t)(g % (long)h->cap)].snap;
        }
    }
    return NULL;
}

size_t history_bytes(const History *h) {
    if (h == NULL) return 0;
    return h->bytes;
}
