#include "sparse.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* Below this population the serial stepper wins: thread spin-up and the extra
   per-band bookkeeping cost more than the tally they parallelise. */
#define SPARSE_PARALLEL_MIN 20000

/* Cells are not allowed to spawn beyond this range, which keeps neighbour
   arithmetic well within int and is far larger than any reachable pattern. */
#define COORD_LIMIT (1 << 30)

/* ------------------------------------------------------------------ */
/* Open-addressing hash table (linear probing)                        */
/*                                                                     */
/* Used two ways: as a plain set of live cells (cnt == NULL) and as a  */
/* neighbour-count map (cnt holds a small tally per key).              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t *keys;
    uint8_t  *occ; /* 1 = slot in use */
    uint8_t  *cnt; /* neighbour counts, or NULL for a pure set */
    size_t    cap; /* power of two */
    size_t    len;
} Map;

static uint64_t pack(int x, int y) {
    return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
}

static void unpack(uint64_t k, int *x, int *y) {
    *x = (int)(int32_t)(uint32_t)(k >> 32);
    *y = (int)(int32_t)(uint32_t)(k & 0xffffffffu);
}

/* SplitMix64 finaliser: scrambles the packed key into a good hash. */
static uint64_t hash64(uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static bool map_init(Map *m, size_t cap, bool with_cnt) {
    m->keys = calloc(cap, sizeof(uint64_t));
    m->occ = calloc(cap, sizeof(uint8_t));
    m->cnt = with_cnt ? calloc(cap, sizeof(uint8_t)) : NULL;
    if (m->keys == NULL || m->occ == NULL || (with_cnt && m->cnt == NULL)) {
        free(m->keys);
        free(m->occ);
        free(m->cnt);
        m->keys = NULL;
        m->occ = NULL;
        m->cnt = NULL;
        return false;
    }
    m->cap = cap;
    m->len = 0;
    return true;
}

static void map_free(Map *m) {
    free(m->keys);
    free(m->occ);
    free(m->cnt);
    m->keys = NULL;
    m->occ = NULL;
    m->cnt = NULL;
    m->cap = 0;
    m->len = 0;
}

static void map_clear(Map *m) {
    memset(m->occ, 0, m->cap * sizeof(uint8_t));
    m->len = 0;
}

/* Insert a key assuming it is not present and there is room. Returns the slot's
   count pointer position via the return of a helper; here just used internally
   during rehash. */
static void raw_put(Map *m, uint64_t key, uint8_t cnt) {
    size_t mask = m->cap - 1;
    size_t i = hash64(key) & mask;
    while (m->occ[i]) {
        i = (i + 1) & mask;
    }
    m->occ[i] = 1;
    m->keys[i] = key;
    if (m->cnt) m->cnt[i] = cnt;
    m->len++;
}

/* Grow to double capacity and reinsert. Returns false on allocation failure
   (leaving the original table intact). */
static bool map_grow(Map *m) {
    Map n;
    if (!map_init(&n, m->cap * 2, m->cnt != NULL)) {
        return false;
    }
    for (size_t i = 0; i < m->cap; i++) {
        if (m->occ[i]) {
            raw_put(&n, m->keys[i], m->cnt ? m->cnt[i] : 0);
        }
    }
    map_free(m);
    *m = n;
    return true;
}

/* Grow if the table is getting full (load factor 0.70). */
static bool map_reserve_one(Map *m) {
    if ((m->len + 1) * 10 >= m->cap * 7) {
        return map_grow(m);
    }
    return true;
}

static bool set_has(const Map *m, uint64_t key) {
    if (m->len == 0) return false;
    size_t mask = m->cap - 1;
    size_t i = hash64(key) & mask;
    while (m->occ[i]) {
        if (m->keys[i] == key) return true;
        i = (i + 1) & mask;
    }
    return false;
}

static void set_add(Map *m, uint64_t key) {
    if (!map_reserve_one(m)) return; /* out of memory: best effort */
    size_t mask = m->cap - 1;
    size_t i = hash64(key) & mask;
    while (m->occ[i]) {
        if (m->keys[i] == key) return; /* already present */
        i = (i + 1) & mask;
    }
    m->occ[i] = 1;
    m->keys[i] = key;
    m->len++;
}

/* Classic linear-probing deletion with backward shifting (no tombstones). */
static void set_del(Map *m, uint64_t key) {
    if (m->len == 0) return;
    size_t mask = m->cap - 1;
    size_t i = hash64(key) & mask;
    while (m->occ[i] && m->keys[i] != key) {
        i = (i + 1) & mask;
    }
    if (!m->occ[i]) return; /* not present */

    size_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!m->occ[j]) break;
        size_t home = hash64(m->keys[j]) & mask;
        /* Is `home` cyclically outside the open interval (i, j]? If so, the
           entry at j may move up to fill the hole at i. */
        bool movable = (i <= j) ? (home <= i || home > j)
                                : (home <= i && home > j);
        if (movable) {
            m->keys[i] = m->keys[j];
            m->occ[i] = 1;
            i = j;
        }
    }
    m->occ[i] = 0;
    m->len--;
}

static void cnt_inc(Map *m, uint64_t key) {
    if (!map_reserve_one(m)) return;
    size_t mask = m->cap - 1;
    size_t i = hash64(key) & mask;
    while (m->occ[i]) {
        if (m->keys[i] == key) {
            if (m->cnt[i] < 255) m->cnt[i]++;
            return;
        }
        i = (i + 1) & mask;
    }
    m->occ[i] = 1;
    m->keys[i] = key;
    m->cnt[i] = 1;
    m->len++;
}

/* ------------------------------------------------------------------ */
/* Public world                                                       */
/* ------------------------------------------------------------------ */

struct SparseWorld {
    Map live;
};

SparseWorld *sparse_new(void) {
    SparseWorld *w = malloc(sizeof(*w));
    if (w == NULL) return NULL;
    if (!map_init(&w->live, 64, false)) {
        free(w);
        return NULL;
    }
    return w;
}

void sparse_free(SparseWorld *w) {
    if (w == NULL) return;
    map_free(&w->live);
    free(w);
}

void sparse_clear(SparseWorld *w) {
    map_clear(&w->live);
}

bool sparse_get(const SparseWorld *w, int x, int y) {
    return set_has(&w->live, pack(x, y));
}

void sparse_set(SparseWorld *w, int x, int y, bool alive) {
    if (x <= -COORD_LIMIT || x >= COORD_LIMIT ||
        y <= -COORD_LIMIT || y >= COORD_LIMIT) {
        return;
    }
    if (alive) {
        set_add(&w->live, pack(x, y));
    } else {
        set_del(&w->live, pack(x, y));
    }
}

size_t sparse_count(const SparseWorld *w) {
    return w->live.len;
}

bool sparse_bounds(const SparseWorld *w, int *minx, int *miny,
                   int *maxx, int *maxy) {
    if (w->live.len == 0) return false;
    int lox = 0, loy = 0, hix = 0, hiy = 0;
    bool first = true;
    for (size_t i = 0; i < w->live.cap; i++) {
        if (!w->live.occ[i]) continue;
        int x, y;
        unpack(w->live.keys[i], &x, &y);
        if (first) {
            lox = hix = x;
            loy = hiy = y;
            first = false;
        } else {
            if (x < lox) lox = x;
            if (x > hix) hix = x;
            if (y < loy) loy = y;
            if (y > hiy) hiy = y;
        }
    }
    *minx = lox;
    *miny = loy;
    *maxx = hix;
    *maxy = hiy;
    return true;
}

void sparse_query(const SparseWorld *w, int x0, int y0, int x1, int y1,
                  void (*fn)(int x, int y, void *ud), void *ud) {
    for (size_t i = 0; i < w->live.cap; i++) {
        if (!w->live.occ[i]) continue;
        int x, y;
        unpack(w->live.keys[i], &x, &y);
        if (x >= x0 && x < x1 && y >= y0 && y < y1) {
            fn(x, y, ud);
        }
    }
}

void sparse_copy(SparseWorld *dst, const SparseWorld *src) {
    if (dst == src) return;
    map_clear(&dst->live);
    for (size_t i = 0; i < src->live.cap; i++) {
        if (src->live.occ[i]) {
            set_add(&dst->live, src->live.keys[i]);
        }
    }
}

/* Smallest power of two >= n (and >= 64). Caps are powers of two so the hash
   mask (cap - 1) works. */
static size_t pow2_cap(size_t n) {
    size_t c = 64;
    while (c < n) c *= 2;
    return c;
}

/* The single-threaded reference stepper. Correct on its own; also the fallback
   when the world is small or a parallel allocation fails. */
static void sparse_step_serial(SparseWorld *w) {
    /* Tally how many live neighbours each candidate cell has. */
    Map counts;
    if (!map_init(&counts, pow2_cap(w->live.len * 4), true)) return; /* OOM: no-op */

    for (size_t i = 0; i < w->live.cap; i++) {
        if (!w->live.occ[i]) continue;
        int x, y;
        unpack(w->live.keys[i], &x, &y);
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                long nx = (long)x + dx;
                long ny = (long)y + dy;
                if (nx <= -COORD_LIMIT || nx >= COORD_LIMIT ||
                    ny <= -COORD_LIMIT || ny >= COORD_LIMIT) {
                    continue;
                }
                cnt_inc(&counts, pack((int)nx, (int)ny));
            }
        }
    }

    /* A cell lives next generation if it has exactly 3 neighbours, or exactly 2
       and it is currently alive. */
    Map next;
    if (!map_init(&next, w->live.cap, false)) {
        map_free(&counts);
        return;
    }
    for (size_t i = 0; i < counts.cap; i++) {
        if (!counts.occ[i]) continue;
        uint8_t c = counts.cnt[i];
        uint64_t key = counts.keys[i];
        if (c == 3 || (c == 2 && set_has(&w->live, key))) {
            set_add(&next, key);
        }
    }

    map_free(&counts);
    map_free(&w->live);
    w->live = next;
}

#ifdef _OPENMP
/* Multi-core stepper. The world is split into horizontal bands of rows; each
   thread owns a disjoint set of *output* rows [b0, b1) and reads a one-row halo
   on each side. Because a cell's neighbours are within ±1 row, a thread that
   tallies every live cell in rows [b0-1, b1] has complete neighbour counts for
   all of its output rows — with no other thread touching those rows. So there is
   no shared writable state during the parallel region (each thread has a private
   count map and result set; w->live is only read), hence no locks and no merge
   step, and the result is bit-for-bit identical to the serial stepper (the union
   of the per-band result sets is the same set of live cells).

   Returns false if it could not run (allocation failure), so the caller can fall
   back to the serial path with the world still untouched. */
static bool sparse_step_parallel(SparseWorld *w) {
    /* Compact the live cells into a contiguous array (serial, one pass) and take
       the y-extent while we are at it. Threads then scan this array (len items)
       rather than the hash table's slots — the table's cap can be many times the
       population after a wave of deaths, so scanning slots would waste most of
       each thread's time and cap the speedup. */
    size_t n = w->live.len;
    uint64_t *arr = malloc(n * sizeof(uint64_t));
    if (arr == NULL) return false;
    int ymin = INT_MAX, ymax = INT_MIN;
    size_t k = 0;
    for (size_t i = 0; i < w->live.cap && k < n; i++) {
        if (!w->live.occ[i]) continue;
        uint64_t key = w->live.keys[i];
        arr[k++] = key;
        int x, y;
        unpack(key, &x, &y);
        (void)x;
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    /* A cell can be *born* one row beyond the current extent, so output rows run
       [ymin-1, ymax+1]. out_lo is that low bound; span covers the whole range. */
    long out_lo = (long)ymin - 1;
    long span = ((long)ymax + 1) - out_lo + 1;

    int T = omp_get_max_threads();
    if ((long)T > span) T = (int)span; /* no more threads than rows */
    if (T < 1) T = 1;

    Map *results = calloc((size_t)T, sizeof(Map));
    if (results == NULL) { free(arr); return false; }
    bool ok = true;

    #pragma omp parallel num_threads(T)
    {
        int t = omp_get_thread_num();
        long b0 = out_lo + span * t / T;       /* first output row (inclusive) */
        long b1 = out_lo + span * (t + 1) / T; /* last output row  (exclusive) */

        Map counts, res;
        bool tok = map_init(&counts, pow2_cap((w->live.len / (size_t)T + 1) * 4), true);
        if (tok) {
            tok = map_init(&res, 64, false);
            if (!tok) map_free(&counts); /* res failed: don't leak counts */
        }
        if (!tok) {
            #pragma omp atomic write
            ok = false;
        } else {
            /* Tally neighbours from every live cell that can influence an output
               row in this band: rows [b0-1, b1]. */
            for (size_t i = 0; i < n; i++) {
                int x, y;
                unpack(arr[i], &x, &y);
                if (y < b0 - 1 || y > b1) continue;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        long nx = (long)x + dx;
                        long ny = (long)y + dy;
                        if (nx <= -COORD_LIMIT || nx >= COORD_LIMIT ||
                            ny <= -COORD_LIMIT || ny >= COORD_LIMIT) {
                            continue;
                        }
                        cnt_inc(&counts, pack((int)nx, (int)ny));
                    }
                }
            }
            /* Emit only this band's own output rows so bands stay disjoint. */
            for (size_t i = 0; i < counts.cap; i++) {
                if (!counts.occ[i]) continue;
                uint64_t key = counts.keys[i];
                int nx, ny;
                unpack(key, &nx, &ny);
                (void)nx;
                if (ny < b0 || ny >= b1) continue;
                uint8_t c = counts.cnt[i];
                if (c == 3 || (c == 2 && set_has(&w->live, key))) {
                    set_add(&res, key);
                }
            }
            map_free(&counts);
            results[t] = res;
        }
    }
    free(arr);

    if (!ok) {
        for (int t = 0; t < T; t++) map_free(&results[t]);
        free(results);
        return false;
    }

    /* Concatenate the disjoint per-band result sets into the new world. */
    size_t total = 0;
    for (int t = 0; t < T; t++) total += results[t].len;
    Map next;
    if (!map_init(&next, pow2_cap(total * 10 / 7 + 1), false)) {
        for (int t = 0; t < T; t++) map_free(&results[t]);
        free(results);
        return false;
    }
    for (int t = 0; t < T; t++) {
        Map *r = &results[t];
        for (size_t i = 0; i < r->cap; i++) {
            if (r->occ[i]) raw_put(&next, r->keys[i], 0); /* disjoint: no dup check */
        }
        map_free(r);
    }
    free(results);

    map_free(&w->live);
    w->live = next;
    return true;
}
#endif /* _OPENMP */

void sparse_step(SparseWorld *w) {
    if (w->live.len == 0) return;
#ifdef _OPENMP
    if (w->live.len >= SPARSE_PARALLEL_MIN && omp_get_max_threads() > 1) {
        if (sparse_step_parallel(w)) return;
        /* else fall through to the serial path (world untouched) */
    }
#endif
    sparse_step_serial(w);
}
