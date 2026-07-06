#include "engine.h"

#include "sparse.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

/* Current backend: the sparse hash-set engine. A Hashlife backend would replace
   the bodies here (and the snapshot representation) while keeping engine.h. */
struct LifeEngine {
    SparseWorld *w;
};

/* Sparse snapshot: a flat array of the live cells' (x, y) pairs (2*count ints).
   A Hashlife snapshot would instead be a single canonical node id. */
struct EngineSnapshot {
    int32_t *xy;
    size_t count;
};

LifeEngine *engine_new(void) {
    LifeEngine *e = malloc(sizeof(*e));
    if (e == NULL) return NULL;
    e->w = sparse_new();
    if (e->w == NULL) {
        free(e);
        return NULL;
    }
    return e;
}

void engine_free(LifeEngine *e) {
    if (e == NULL) return;
    sparse_free(e->w);
    free(e);
}

void engine_advance(LifeEngine *e, long n) {
    for (long i = 0; i < n; i++) sparse_step(e->w);
}

size_t engine_population(const LifeEngine *e) { return sparse_count(e->w); }
bool engine_get(const LifeEngine *e, int x, int y) { return sparse_get(e->w, x, y); }
void engine_set(LifeEngine *e, int x, int y, bool alive) { sparse_set(e->w, x, y, alive); }
void engine_clear(LifeEngine *e) { sparse_clear(e->w); }

bool engine_bounds(const LifeEngine *e, int *minx, int *miny,
                   int *maxx, int *maxy) {
    return sparse_bounds(e->w, minx, miny, maxx, maxy);
}

void engine_query(const LifeEngine *e, int x0, int y0, int x1, int y1,
                  void (*fn)(int x, int y, void *ud), void *ud) {
    sparse_query(e->w, x0, y0, x1, y1, fn, ud);
}

/* Collect (x, y) pairs into a pre-sized buffer. */
typedef struct {
    int32_t *xy;
    size_t n;
} Collector;

static void collect_cb(int x, int y, void *ud) {
    Collector *c = (Collector *)ud;
    c->xy[2 * c->n] = (int32_t)x;
    c->xy[2 * c->n + 1] = (int32_t)y;
    c->n++;
}

EngineSnapshot *engine_snapshot(const LifeEngine *e) {
    EngineSnapshot *s = malloc(sizeof(*s));
    if (s == NULL) return NULL;
    const size_t n = sparse_count(e->w);
    s->count = 0;
    s->xy = NULL;
    if (n > 0) {
        s->xy = malloc(n * 2 * sizeof(int32_t));
        if (s->xy == NULL) {
            free(s);
            return NULL;
        }
    }
    Collector c = {s->xy, 0};
    /* Whole-world query: coordinates are bounded well within int, so INT_MIN /
       INT_MAX select every live cell. */
    sparse_query(e->w, INT_MIN, INT_MIN, INT_MAX, INT_MAX, collect_cb, &c);
    s->count = c.n;
    return s;
}

void engine_restore(LifeEngine *e, const EngineSnapshot *s) {
    sparse_clear(e->w);
    if (s == NULL) return;
    for (size_t i = 0; i < s->count; i++) {
        sparse_set(e->w, s->xy[2 * i], s->xy[2 * i + 1], true);
    }
}

void engine_snapshot_free(EngineSnapshot *s) {
    if (s == NULL) return;
    free(s->xy);
    free(s);
}

size_t engine_snapshot_bytes(const EngineSnapshot *s) {
    if (s == NULL) return 0;
    return sizeof(*s) + s->count * 2 * sizeof(int32_t);
}
