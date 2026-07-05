#include "sparse.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

void sparse_copy(SparseWorld *dst, const SparseWorld *src) {
    if (dst == src) return;
    map_clear(&dst->live);
    for (size_t i = 0; i < src->live.cap; i++) {
        if (src->live.occ[i]) {
            set_add(&dst->live, src->live.keys[i]);
        }
    }
}

void sparse_step(SparseWorld *w) {
    if (w->live.len == 0) return;

    /* Tally how many live neighbours each candidate cell has. */
    Map counts;
    size_t cap = 64;
    while (cap < w->live.len * 4) cap *= 2; /* headroom for ~8x fan-out */
    if (!map_init(&counts, cap, true)) return; /* OOM: leave world unchanged */

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
