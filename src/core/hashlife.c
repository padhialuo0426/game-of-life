#include "core/hashlife.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Hashlife node model                                                */
/*                                                                     */
/* A level-L node covers a 2^L x 2^L square. Level 0 is a single cell  */
/* (two canonical shared instances: OFF and ON). A level-(L>=1) node   */
/* has four level-(L-1) children (nw = smaller x & smaller y, i.e.     */
/* north-west with y growing downward). Nodes are canonical: two nodes */
/* with identical children are the same pointer (interned in a hash    */
/* table), so identical subpatterns are shared and time/space collapse */
/* on repetition.                                                      */
/* ------------------------------------------------------------------ */

typedef struct Node Node;
struct Node {
    Node    *nw, *ne, *sw, *se; /* children (NULL only for level 0) */
    Node    *result;            /* cached MAXIMAL-step result (advance 2^(L-2)) */
    Node    *part_res;          /* cached partial-step result for step 2^part_j */
    Node    *hnext;             /* intern hash-table chain */
    uint64_t pop;               /* live-cell population */
    uint32_t level;
    int32_t  part_j;            /* step exponent cached in part_res, or -1 */
    uint8_t  on;                /* level 0 only: 1 = live */
};

/* Cap the root level so live-cell world coordinates stay within +/-2^30 (well
   inside int, matching the sparse backend's COORD_LIMIT). A level-L root spans
   [-2^(L-1), 2^(L-1)); L = 31 gives +/-2^30. The largest single leap is then
   2^(L-2) = 2^29 generations. */
#define HL_MAX_LEVEL 31
#define HL_COORD_LIMIT (1 << 30)

/* Node-pool memory ceiling (no GC in v1). ~640 MB of nodes before hl_advance
   refuses to grow the tree further and reports hl_exhausted. */
#define HL_MAX_NODES (8u * 1000u * 1000u)

struct Hashlife {
    /* Intern table: buckets of Node* chained via hnext. */
    Node   **buckets;
    size_t   nbuckets;   /* power of two */
    size_t   nnodes;     /* interned nodes (level >= 1) */

    Node    *off, *on;   /* the two canonical level-0 leaves */

    Node   **empties;    /* empties[L] = canonical all-dead level-L node */
    size_t   nempties;   /* allocated length of the empties array */

    Node    *root;
    uint32_t rlev;       /* root level */
    bool     exhausted;  /* memory cap hit during the last advance */
    bool     oom;        /* an allocation failed; the world is frozen */
};

/* ------------------------------------------------------------------ */
/* Interning                                                          */
/* ------------------------------------------------------------------ */

static uint64_t hash_children(const Node *a, const Node *b,
                              const Node *c, const Node *d) {
    uint64_t h = 1469598103934665603ULL; /* FNV offset basis */
    const uintptr_t p[4] = {(uintptr_t)a, (uintptr_t)b,
                            (uintptr_t)c, (uintptr_t)d};
    for (int i = 0; i < 4; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;         /* FNV prime */
        h ^= h >> 29;
    }
    return h;
}

static bool intern_grow(Hashlife *h) {
    size_t ncap = h->nbuckets * 2;
    Node **nb = calloc(ncap, sizeof(Node *));
    if (nb == NULL) return false;
    size_t mask = ncap - 1;
    for (size_t i = 0; i < h->nbuckets; i++) {
        Node *n = h->buckets[i];
        while (n != NULL) {
            Node *next = n->hnext;
            uint64_t idx = hash_children(n->nw, n->ne, n->sw, n->se) & mask;
            n->hnext = nb[idx];
            nb[idx] = n;
            n = next;
        }
    }
    free(h->buckets);
    h->buckets = nb;
    h->nbuckets = ncap;
    return true;
}

/* Canonical level-(>=1) node with the given children. Interns: returns the
   existing node if an identical one exists, else allocates and inserts one.
   Returns NULL on allocation failure (h->oom set by the caller path). */
static Node *intern(Hashlife *h, uint32_t level,
                    Node *nw, Node *ne, Node *sw, Node *se) {
    uint64_t hv = hash_children(nw, ne, sw, se);
    size_t idx = (size_t)hv & (h->nbuckets - 1);
    for (Node *n = h->buckets[idx]; n != NULL; n = n->hnext) {
        if (n->nw == nw && n->ne == ne && n->sw == sw && n->se == se &&
            n->level == level) {
            return n;
        }
    }
    if ((h->nnodes + 1) * 10 >= h->nbuckets * 7) {
        if (intern_grow(h)) {
            idx = (size_t)hv & (h->nbuckets - 1);
        }
    }
    Node *n = calloc(1, sizeof(Node));
    if (n == NULL) { h->oom = true; return NULL; }
    n->nw = nw; n->ne = ne; n->sw = sw; n->se = se;
    n->level = level;
    n->pop = nw->pop + ne->pop + sw->pop + se->pop;
    n->part_j = -1;
    n->hnext = h->buckets[idx];
    h->buckets[idx] = n;
    h->nnodes++;
    return n;
}

/* The canonical all-dead node of a level, memoised in h->empties. */
static Node *empty_node(Hashlife *h, uint32_t level) {
    if (level == 0) return h->off;
    if (level >= h->nempties) {
        size_t nn = h->nempties ? h->nempties : 4;
        while (level >= nn) nn *= 2;
        Node **ne = realloc(h->empties, nn * sizeof(Node *));
        if (ne == NULL) { h->oom = true; return NULL; }
        for (size_t i = h->nempties; i < nn; i++) ne[i] = NULL;
        h->empties = ne;
        h->nempties = nn;
    }
    if (h->empties[level] == NULL) {
        Node *c = empty_node(h, level - 1);
        if (c == NULL) return NULL;
        h->empties[level] = intern(h, level, c, c, c, c);
    }
    return h->empties[level];
}

/* ------------------------------------------------------------------ */
/* Base case: a level-2 (4x4) node advanced one generation -> level-1 */
/* ------------------------------------------------------------------ */

/* Conway B3/S23 on the centre 2x2 of a 4x4 grid. Row r (0 = top / smaller y),
   col c (0 = left / smaller x). Returns the level-1 node of the four centre
   cells at the next generation. */
static Node *base_result(Hashlife *h, const Node *m) {
    const Node *nw = m->nw, *ne = m->ne, *sw = m->sw, *se = m->se;
    int g[4][4];
    g[0][0] = nw->nw->on; g[0][1] = nw->ne->on; g[0][2] = ne->nw->on; g[0][3] = ne->ne->on;
    g[1][0] = nw->sw->on; g[1][1] = nw->se->on; g[1][2] = ne->sw->on; g[1][3] = ne->se->on;
    g[2][0] = sw->nw->on; g[2][1] = sw->ne->on; g[2][2] = se->nw->on; g[2][3] = se->ne->on;
    g[3][0] = sw->sw->on; g[3][1] = sw->se->on; g[3][2] = se->sw->on; g[3][3] = se->se->on;

    Node *out[2][2];
    for (int r = 1; r <= 2; r++) {
        for (int c = 1; c <= 2; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++)
                    if (dr || dc) n += g[r + dr][c + dc];
            bool live = g[r][c] ? (n == 2 || n == 3) : (n == 3);
            out[r - 1][c - 1] = live ? h->on : h->off;
        }
    }
    return intern(h, 1, out[0][0], out[0][1], out[1][0], out[1][1]);
}

/* ------------------------------------------------------------------ */
/* Generalised memoised result                                        */
/*                                                                     */
/* result(m, j) advances node m (level L) by 2^j generations, 0<=j<=L-2, */
/* and returns the centre as a level-(L-1) node. The maximal step      */
/* (j == L-2) is cached permanently in m->result; a partial step       */
/* (j < L-2) is cached in a single (part_j, part_res) slot — within one */
/* top-level leap every node is asked for exactly one step size, so the */
/* single slot never thrashes inside a leap, while the expensive deep   */
/* maximal results stay memoised across leaps.                          */
/* ------------------------------------------------------------------ */

/* The centre level-(L-1) subnode of a level-L node (L >= 2): no time advance. */
static Node *centre(Hashlife *h, const Node *m) {
    return intern(h, m->level - 1, m->nw->se, m->ne->sw, m->sw->ne, m->se->nw);
}

static Node *result(Hashlife *h, Node *m, int j);

/* Advance m by 2^j generations (0 <= j <= L-2) and return its centre, a
   level-(L-1) node. Canonical two-stage construction (Gosper / Johnston): nine
   overlapping subnodes are each advanced 2^s1 (stage 1), reassembled into four,
   then either centre-extracted (partial step: stage-2 adds 0) or advanced again
   2^(L-3) (maximal step: stage-2 adds the other half). */
static Node *result(Hashlife *h, Node *m, int j) {
    uint32_t L = m->level;
    if (m->pop == 0) return empty_node(h, L - 1);
    if (L == 2) return base_result(h, m);       /* advances 2^0 = 1; j must be 0 */

    bool maximal = (j == (int)L - 2);
    if (maximal) {
        if (m->result != NULL) return m->result;
    } else {
        if (m->part_j == j && m->part_res != NULL) return m->part_res;
    }

    /* The nine overlapping level-(L-1) subnodes (3x3 grid, quarter-step apart),
       assembled from m's children and grandchildren. */
    Node *nw = m->nw, *ne = m->ne, *sw = m->sw, *se = m->se;
    Node *n_nw = nw;
    Node *n_tc = intern(h, L - 1, nw->ne, ne->nw, nw->se, ne->sw);
    Node *n_ne = ne;
    Node *n_ml = intern(h, L - 1, nw->sw, nw->se, sw->nw, sw->ne);
    Node *n_mc = intern(h, L - 1, nw->se, ne->sw, sw->ne, se->nw);
    Node *n_mr = intern(h, L - 1, ne->sw, ne->se, se->nw, se->ne);
    Node *n_sw = sw;
    Node *n_bc = intern(h, L - 1, sw->ne, se->nw, sw->se, se->sw);
    Node *n_se = se;
    if (n_tc == NULL || n_ml == NULL || n_mc == NULL || n_mr == NULL || n_bc == NULL)
        return NULL;

    /* Stage 1: advance each of the nine by 2^s1, giving nine level-(L-2) nodes.
       s1 = min(j, L-3): the whole advance in the partial case, exactly half in
       the maximal case (the other half is stage 2). */
    int s1 = j < (int)L - 3 ? j : (int)L - 3;
    Node *b1 = result(h, n_nw, s1), *b2 = result(h, n_tc, s1), *b3 = result(h, n_ne, s1);
    Node *b4 = result(h, n_ml, s1), *b5 = result(h, n_mc, s1), *b6 = result(h, n_mr, s1);
    Node *b7 = result(h, n_sw, s1), *b8 = result(h, n_bc, s1), *b9 = result(h, n_se, s1);
    if (b1 == NULL || b2 == NULL || b3 == NULL || b4 == NULL || b5 == NULL ||
        b6 == NULL || b7 == NULL || b8 == NULL || b9 == NULL)
        return NULL;

    /* Reassemble into the four level-(L-1) quadrant nodes. */
    Node *a = intern(h, L - 1, b1, b2, b4, b5);
    Node *b = intern(h, L - 1, b2, b3, b5, b6);
    Node *c = intern(h, L - 1, b4, b5, b7, b8);
    Node *d = intern(h, L - 1, b5, b6, b8, b9);
    if (a == NULL || b == NULL || c == NULL || d == NULL) return NULL;

    /* Stage 2. */
    Node *res_nw, *res_ne, *res_sw, *res_se;
    if (maximal) {
        int s = (int)L - 3;
        res_nw = result(h, a, s); res_ne = result(h, b, s);
        res_sw = result(h, c, s); res_se = result(h, d, s);
    } else {
        res_nw = centre(h, a); res_ne = centre(h, b);
        res_sw = centre(h, c); res_se = centre(h, d);
    }
    if (res_nw == NULL || res_ne == NULL || res_sw == NULL || res_se == NULL)
        return NULL;

    Node *out = intern(h, L - 1, res_nw, res_ne, res_sw, res_se);
    if (out == NULL) return NULL;

    if (maximal) {
        m->result = out;
    } else {
        m->part_j = j;
        m->part_res = out;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Root growth                                                        */
/* ------------------------------------------------------------------ */

/* Wrap the root in a node one level larger, keeping the pattern centred. */
static bool expand_root(Hashlife *h) {
    if (h->rlev >= HL_MAX_LEVEL) return false;
    uint32_t L = h->rlev;
    Node *e = empty_node(h, L - 1);
    if (e == NULL) return false;
    Node *r = h->root;
    Node *nw = intern(h, L, e, e, e, r->nw);
    Node *ne = intern(h, L, e, e, r->ne, e);
    Node *sw = intern(h, L, e, r->sw, e, e);
    Node *se = intern(h, L, r->se, e, e, e);
    if (nw == NULL || ne == NULL || sw == NULL || se == NULL) return false;
    Node *nr = intern(h, L + 1, nw, ne, sw, se);
    if (nr == NULL) return false;
    h->root = nr;
    h->rlev = L + 1;
    return true;
}

/* True once each quadrant's population sits entirely in its inner corner (the
   central quarter of the whole root) — the classic "centred enough to step"
   condition: advancing the maximal 2^(rlev-2) can then grow into the vacated
   outer ring without the result() centre losing any live cells. */
static bool centred_enough(const Hashlife *h) {
    if (h->rlev < 3) return false;
    const Node *r = h->root;
    return r->nw->pop == r->nw->se->se->pop &&
           r->ne->pop == r->ne->sw->sw->pop &&
           r->sw->pop == r->sw->ne->ne->pop &&
           r->se->pop == r->se->nw->nw->pop;
}

/* Advance exactly 2^j generations, or return false (cap hit / OOM) without
   changing the world. All-or-nothing so the generation count stays exact. */
static bool hl_step(Hashlife *h, int j) {
    /* Grow until the root can host a 2^j leap with room for the growth: level
       >= j + 2 and the pattern confined to the central quarter. */
    while ((int)h->rlev < j + 2 || !centred_enough(h)) {
        if (h->nnodes >= HL_MAX_NODES) return false;
        if (!expand_root(h)) return false;
    }
    if (h->nnodes >= HL_MAX_NODES) return false;
    Node *nr = result(h, h->root, j);
    if (nr == NULL) return false;         /* OOM inside result */
    h->root = nr;
    h->rlev -= 1;                          /* result returns the centre, one level down */
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

Hashlife *hl_new(void) {
    Hashlife *h = calloc(1, sizeof(*h));
    if (h == NULL) return NULL;
    h->nbuckets = 1u << 16;
    h->buckets = calloc(h->nbuckets, sizeof(Node *));
    h->off = calloc(1, sizeof(Node));
    h->on = calloc(1, sizeof(Node));
    if (h->buckets == NULL || h->off == NULL || h->on == NULL) {
        free(h->buckets); free(h->off); free(h->on); free(h);
        return NULL;
    }
    h->off->level = 0; h->off->pop = 0; h->off->on = 0; h->off->part_j = -1;
    h->on->level = 0;  h->on->pop = 1;  h->on->on = 1;  h->on->part_j = -1;

    h->rlev = 3;
    h->root = empty_node(h, 3);
    if (h->root == NULL) { hl_free(h); return NULL; }
    return h;
}

void hl_free(Hashlife *h) {
    if (h == NULL) return;
    for (size_t i = 0; i < h->nbuckets; i++) {
        Node *n = h->buckets[i];
        while (n != NULL) {
            Node *next = n->hnext;
            free(n);
            n = next;
        }
    }
    free(h->buckets);
    free(h->empties);
    free(h->off);
    free(h->on);
    free(h);
}

void hl_clear(Hashlife *h) {
    h->rlev = 3;
    h->root = empty_node(h, 3);
    h->exhausted = false;
}

static Node *set_rec(Hashlife *h, Node *n, uint32_t level,
                     uint32_t lx, uint32_t ly, bool alive) {
    if (level == 0) return alive ? h->on : h->off;
    uint32_t half = 1u << (level - 1);
    Node *nw = n->nw, *ne = n->ne, *sw = n->sw, *se = n->se;
    if (lx < half && ly < half)        nw = set_rec(h, nw, level - 1, lx, ly, alive);
    else if (lx >= half && ly < half)  ne = set_rec(h, ne, level - 1, lx - half, ly, alive);
    else if (lx < half && ly >= half)  sw = set_rec(h, sw, level - 1, lx, ly - half, alive);
    else                               se = set_rec(h, se, level - 1, lx - half, ly - half, alive);
    if (nw == NULL || ne == NULL || sw == NULL || se == NULL) return NULL;
    return intern(h, level, nw, ne, sw, se);
}

void hl_set(Hashlife *h, int x, int y, bool alive) {
    if (x <= -HL_COORD_LIMIT || x >= HL_COORD_LIMIT ||
        y <= -HL_COORD_LIMIT || y >= HL_COORD_LIMIT) {
        return;
    }
    /* Grow until (x, y) lies within the root's span [-2^(rlev-1), 2^(rlev-1)). */
    for (;;) {
        long half = 1L << (h->rlev - 1);
        if (x >= -half && x < half && y >= -half && y < half) break;
        if (!expand_root(h)) return; /* at the coordinate cap: ignore */
    }
    long half = 1L << (h->rlev - 1);
    uint32_t lx = (uint32_t)((long)x + half);
    uint32_t ly = (uint32_t)((long)y + half);
    Node *nr = set_rec(h, h->root, h->rlev, lx, ly, alive);
    if (nr != NULL) h->root = nr;
}

static bool get_rec(const Hashlife *h, const Node *n, uint32_t level,
                    uint32_t lx, uint32_t ly) {
    while (level > 0) {
        uint32_t half = 1u << (level - 1);
        if (lx < half && ly < half)        { n = n->nw; }
        else if (lx >= half && ly < half)  { n = n->ne; lx -= half; }
        else if (lx < half && ly >= half)  { n = n->sw; ly -= half; }
        else                               { n = n->se; lx -= half; ly -= half; }
        level--;
    }
    (void)h;
    return n->on != 0;
}

bool hl_get(const Hashlife *h, int x, int y) {
    long half = 1L << (h->rlev - 1);
    if (x < -half || x >= half || y < -half || y >= half) return false;
    return get_rec(h, h->root, h->rlev,
                   (uint32_t)((long)x + half), (uint32_t)((long)y + half));
}

size_t hl_population(const Hashlife *h) {
    return (size_t)h->root->pop;
}

void hl_advance(Hashlife *h, long n) {
    h->exhausted = false;
    if (n <= 0 || h->oom) return;
    /* Decompose n into leaps, largest first, capped at 2^(HL_MAX_LEVEL-2). */
    const int jmax = HL_MAX_LEVEL - 2;
    while (n > 0) {
        int j = 0;
        while (((long)1 << (j + 1)) <= n && j + 1 <= jmax) j++;
        if (!hl_step(h, j)) { h->exhausted = true; return; }
        n -= (long)1 << j;
    }
}

bool hl_exhausted(const Hashlife *h) {
    return h->exhausted;
}

static void bounds_rec(const Node *n, long wx, long wy, uint32_t level,
                       long *minx, long *miny, long *maxx, long *maxy) {
    if (n->pop == 0) return;
    if (level == 0) {
        if (wx < *minx) *minx = wx;
        if (wx > *maxx) *maxx = wx;
        if (wy < *miny) *miny = wy;
        if (wy > *maxy) *maxy = wy;
        return;
    }
    long half = 1L << (level - 1);
    bounds_rec(n->nw, wx,        wy,        level - 1, minx, miny, maxx, maxy);
    bounds_rec(n->ne, wx + half, wy,        level - 1, minx, miny, maxx, maxy);
    bounds_rec(n->sw, wx,        wy + half, level - 1, minx, miny, maxx, maxy);
    bounds_rec(n->se, wx + half, wy + half, level - 1, minx, miny, maxx, maxy);
}

bool hl_bounds(const Hashlife *h, int *minx, int *miny, int *maxx, int *maxy) {
    if (h->root->pop == 0) return false;
    long half = 1L << (h->rlev - 1);
    long lox = LONG_MAX, loy = LONG_MAX, hix = LONG_MIN, hiy = LONG_MIN;
    bounds_rec(h->root, -half, -half, h->rlev, &lox, &loy, &hix, &hiy);
    *minx = (int)lox; *miny = (int)loy; *maxx = (int)hix; *maxy = (int)hiy;
    return true;
}

typedef struct {
    long x0, y0, x1, y1;
    void (*fn)(int, int, void *);
    void *ud;
} QueryCtx;

static void query_rec(const Node *n, long wx, long wy, uint32_t level,
                      const QueryCtx *q) {
    if (n->pop == 0) return;
    long size = 1L << level;
    /* Prune nodes whose square does not overlap the query rectangle. */
    if (wx >= q->x1 || wx + size <= q->x0 ||
        wy >= q->y1 || wy + size <= q->y0) {
        return;
    }
    if (level == 0) {
        q->fn((int)wx, (int)wy, q->ud);
        return;
    }
    long half = 1L << (level - 1);
    query_rec(n->nw, wx,        wy,        level - 1, q);
    query_rec(n->ne, wx + half, wy,        level - 1, q);
    query_rec(n->sw, wx,        wy + half, level - 1, q);
    query_rec(n->se, wx + half, wy + half, level - 1, q);
}

void hl_query(const Hashlife *h, int x0, int y0, int x1, int y1,
              void (*fn)(int x, int y, void *ud), void *ud) {
    QueryCtx q = {x0, y0, x1, y1, fn, ud};
    long half = 1L << (h->rlev - 1);
    query_rec(h->root, -half, -half, h->rlev, &q);
}

/* ------------------------------------------------------------------ */
/* Snapshots: just the (root, level) — O(1), shares the node pool.     */
/* ------------------------------------------------------------------ */

struct HashlifeSnapshot {
    Node    *root;
    uint32_t rlev;
};

HashlifeSnapshot *hl_snapshot(const Hashlife *h) {
    HashlifeSnapshot *s = malloc(sizeof(*s));
    if (s == NULL) return NULL;
    s->root = h->root;
    s->rlev = h->rlev;
    return s;
}

void hl_restore(Hashlife *h, const HashlifeSnapshot *s) {
    if (s == NULL) {
        hl_clear(h);
        return;
    }
    h->root = s->root;
    h->rlev = s->rlev;
}

void hl_snapshot_free(HashlifeSnapshot *s) {
    free(s);
}

size_t hl_snapshot_bytes(const HashlifeSnapshot *s) {
    if (s == NULL) return 0;
    return sizeof(*s);
}
