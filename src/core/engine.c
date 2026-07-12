#include "core/engine.h"

#include "core/hashlife.h"
#include "core/sparse.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The engine seam dispatches through a small vtable so the world model can be
   chosen at runtime: the sparse hash-set engine (default) or the Hashlife
   hashed-quadtree engine (GOL_HASHLIFE). The UI, history ring and jump loop
   only ever call engine_*; they never know which backend is live. This mirrors
   the function-pointer dispatch render() already uses for the pixel encoders. */
typedef struct {
    void  *(*create)(void);
    void   (*destroy)(void *impl);
    void   (*advance)(void *impl, long n);
    bool   (*exhausted)(const void *impl);
    size_t (*population)(const void *impl);
    bool   (*get)(const void *impl, int x, int y);
    void   (*set)(void *impl, int x, int y, bool alive);
    void   (*clear)(void *impl);
    bool   (*bounds)(const void *impl, int *minx, int *miny, int *maxx, int *maxy);
    void   (*query)(const void *impl, int x0, int y0, int x1, int y1,
                    void (*fn)(int, int, void *), void *ud);
    /* Snapshot: returns an opaque backend handle (NULL on failure). */
    void  *(*snap)(const void *impl);
    void   (*restore)(void *impl, const void *snap);
    void   (*snap_free)(void *snap);
    size_t (*snap_bytes)(const void *snap);
    bool   leaps;
} EngineVTable;

struct LifeEngine {
    const EngineVTable *vt;
    void *impl;
};

/* A snapshot carries its backend's vtable so it can be freed / measured without
   an engine, and restored through the matching backend. Only one backend is
   live per run, so an engine and its snapshots always share a vtable. */
struct EngineSnapshot {
    const EngineVTable *vt;
    void *data;
};

/* ------------------------------------------------------------------ */
/* Sparse backend adapter                                             */
/* ------------------------------------------------------------------ */

/* The sparse snapshot: a flat array of live (x, y) pairs. */
typedef struct {
    int32_t *xy;
    size_t count;
} SparseSnap;

static void *sp_create(void) { return sparse_new(); }
static void sp_destroy(void *w) { sparse_free(w); }
static void sp_advance(void *w, long n) { for (long i = 0; i < n; i++) sparse_step(w); }
static bool sp_exhausted(const void *w) { (void)w; return false; }
static size_t sp_population(const void *w) { return sparse_count(w); }
static bool sp_get(const void *w, int x, int y) { return sparse_get(w, x, y); }
static void sp_set(void *w, int x, int y, bool a) { sparse_set(w, x, y, a); }
static void sp_clear(void *w) { sparse_clear(w); }
static bool sp_bounds(const void *w, int *a, int *b, int *c, int *d) {
    return sparse_bounds(w, a, b, c, d);
}
static void sp_query(const void *w, int x0, int y0, int x1, int y1,
                     void (*fn)(int, int, void *), void *ud) {
    sparse_query(w, x0, y0, x1, y1, fn, ud);
}

typedef struct { int32_t *xy; size_t n; } SpCollector;
static void sp_collect(int x, int y, void *ud) {
    SpCollector *c = ud;
    c->xy[2 * c->n] = (int32_t)x;
    c->xy[2 * c->n + 1] = (int32_t)y;
    c->n++;
}
static void *sp_snap(const void *w) {
    SparseSnap *s = malloc(sizeof(*s));
    if (s == NULL) return NULL;
    const size_t n = sparse_count(w);
    s->count = 0;
    s->xy = NULL;
    if (n > 0) {
        s->xy = malloc(n * 2 * sizeof(int32_t));
        if (s->xy == NULL) { free(s); return NULL; }
    }
    SpCollector c = {s->xy, 0};
    sparse_query(w, INT_MIN, INT_MIN, INT_MAX, INT_MAX, sp_collect, &c);
    s->count = c.n;
    return s;
}
static void sp_restore(void *w, const void *snap) {
    sparse_clear(w);
    const SparseSnap *s = snap;
    if (s == NULL) return;
    for (size_t i = 0; i < s->count; i++)
        sparse_set(w, s->xy[2 * i], s->xy[2 * i + 1], true);
}
static void sp_snap_free(void *snap) {
    SparseSnap *s = snap;
    if (s == NULL) return;
    free(s->xy);
    free(s);
}
static size_t sp_snap_bytes(const void *snap) {
    const SparseSnap *s = snap;
    if (s == NULL) return 0;
    return sizeof(*s) + s->count * 2 * sizeof(int32_t);
}

static const EngineVTable SPARSE_VT = {
    sp_create, sp_destroy, sp_advance, sp_exhausted, sp_population,
    sp_get, sp_set, sp_clear, sp_bounds, sp_query,
    sp_snap, sp_restore, sp_snap_free, sp_snap_bytes, false,
};

/* ------------------------------------------------------------------ */
/* Hashlife backend adapter                                           */
/* ------------------------------------------------------------------ */

static void *hl_create(void) { return hl_new(); }
static void hl_destroy(void *w) { hl_free(w); }
static void hl_advance_(void *w, long n) { hl_advance(w, n); }
static bool hl_exhausted_(const void *w) { return hl_exhausted(w); }
static size_t hl_population_(const void *w) { return hl_population(w); }
static bool hl_get_(const void *w, int x, int y) { return hl_get(w, x, y); }
static void hl_set_(void *w, int x, int y, bool a) { hl_set(w, x, y, a); }
static void hl_clear_(void *w) { hl_clear(w); }
static bool hl_bounds_(const void *w, int *a, int *b, int *c, int *d) {
    return hl_bounds(w, a, b, c, d);
}
static void hl_query_(const void *w, int x0, int y0, int x1, int y1,
                      void (*fn)(int, int, void *), void *ud) {
    hl_query(w, x0, y0, x1, y1, fn, ud);
}
static void *hl_snap(const void *w) { return hl_snapshot(w); }
static void hl_restore_(void *w, const void *snap) { hl_restore(w, snap); }
static void hl_snap_free(void *snap) { hl_snapshot_free(snap); }
static size_t hl_snap_bytes(const void *snap) { return hl_snapshot_bytes(snap); }

static const EngineVTable HASHLIFE_VT = {
    hl_create, hl_destroy, hl_advance_, hl_exhausted_, hl_population_,
    hl_get_, hl_set_, hl_clear_, hl_bounds_, hl_query_,
    hl_snap, hl_restore_, hl_snap_free, hl_snap_bytes, true,
};

/* ------------------------------------------------------------------ */
/* Backend selection + seam                                           */
/* ------------------------------------------------------------------ */

/* GOL_HASHLIFE selects the Hashlife backend when set to anything but "0"
   (mirroring the GOL_KITTY / GOL_SIXEL override convention). */
static const EngineVTable *select_backend(void) {
    const char *env = getenv("GOL_HASHLIFE");
    if (env != NULL && strcmp(env, "0") != 0) return &HASHLIFE_VT;
    return &SPARSE_VT;
}

LifeEngine *engine_new(void) {
    LifeEngine *e = malloc(sizeof(*e));
    if (e == NULL) return NULL;
    e->vt = select_backend();
    e->impl = e->vt->create();
    if (e->impl == NULL) {
        free(e);
        return NULL;
    }
    return e;
}

void engine_free(LifeEngine *e) {
    if (e == NULL) return;
    e->vt->destroy(e->impl);
    free(e);
}

void engine_advance(LifeEngine *e, long n) { e->vt->advance(e->impl, n); }
bool engine_leaps(const LifeEngine *e) { return e->vt->leaps; }
bool engine_exhausted(const LifeEngine *e) { return e->vt->exhausted(e->impl); }

size_t engine_population(const LifeEngine *e) { return e->vt->population(e->impl); }
bool engine_get(const LifeEngine *e, int x, int y) { return e->vt->get(e->impl, x, y); }
void engine_set(LifeEngine *e, int x, int y, bool alive) { e->vt->set(e->impl, x, y, alive); }
void engine_clear(LifeEngine *e) { e->vt->clear(e->impl); }

bool engine_bounds(const LifeEngine *e, int *minx, int *miny,
                   int *maxx, int *maxy) {
    return e->vt->bounds(e->impl, minx, miny, maxx, maxy);
}

void engine_query(const LifeEngine *e, int x0, int y0, int x1, int y1,
                  void (*fn)(int x, int y, void *ud), void *ud) {
    e->vt->query(e->impl, x0, y0, x1, y1, fn, ud);
}

EngineSnapshot *engine_snapshot(const LifeEngine *e) {
    /* Both backends return NULL only on allocation failure (an empty world still
       yields a valid non-NULL handle), so NULL here always means failure. */
    void *data = e->vt->snap(e->impl);
    if (data == NULL) return NULL;
    EngineSnapshot *s = malloc(sizeof(*s));
    if (s == NULL) {
        e->vt->snap_free(data);
        return NULL;
    }
    s->vt = e->vt;
    s->data = data;
    return s;
}

void engine_restore(LifeEngine *e, const EngineSnapshot *s) {
    e->vt->restore(e->impl, s ? s->data : NULL);
}

void engine_snapshot_free(EngineSnapshot *s) {
    if (s == NULL) return;
    s->vt->snap_free(s->data);
    free(s);
}

size_t engine_snapshot_bytes(const EngineSnapshot *s) {
    if (s == NULL) return 0;
    return sizeof(*s) + s->vt->snap_bytes(s->data);
}
