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
};

History *history_new(size_t capacity) {
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
    engine_snapshot_free(s->snap);
    s->snap = snap;
    s->gen = gen;
    h->newest = gen;
    h->any = true;
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
    size_t total = 0;
    for (size_t i = 0; i < h->cap; i++) {
        if (h->buf[i].snap != NULL) total += engine_snapshot_bytes(h->buf[i].snap);
    }
    return total;
}
