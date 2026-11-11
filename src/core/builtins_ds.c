/*
 * ds_map, ds_list, ds_grid, ds_stack, ds_queue and JSON.
 */
#include "builtins_internal.h"
#include <ctype.h>

enum { DS_MAP = 1, DS_LIST = 2, DS_STACK = 3, DS_QUEUE = 4, DS_GRID = 5 };

typedef struct {
    Value key, val;
    bool used;
} MapEntry;

typedef struct {
    int type;
    /* map: insertion-ordered entries + hash index */
    MapEntry *entries;
    int32_t nentries, cap;
    int32_t *index; /* hash -> entry index, -1 empty */
    uint32_t index_cap;
    int32_t live;
    /* list / stack / queue */
    Value *items;
    int32_t len, icap, head;
    uint8_t *marks; /* list: 1 = map, 2 = list (for json) */
    /* grid */
    int32_t gw, gh;
    Value *cells;
} DsObj;

static DsObj **g_ds;
static int32_t g_nds, g_capds;

static int32_t ds_new(int type) {
    int32_t id = -1;
    for (int32_t i = 0; i < g_nds; i++)
        if (!g_ds[i]) { id = i; break; }
    if (id < 0) {
        if (g_nds == g_capds) {
            g_capds = g_capds ? g_capds * 2 : 64;
            g_ds = d3_realloc(g_ds, (size_t)g_capds * sizeof(DsObj *));
        }
        id = g_nds++;
    }
    g_ds[id] = d3_calloc(1, sizeof(DsObj));
    g_ds[id]->type = type;
    return id;
}

static DsObj *ds_get(Value v, int type) {
    int32_t id = v_to_int(v);
    if (id < 0 || id >= g_nds || !g_ds[id] || g_ds[id]->type != type) return NULL;
    return g_ds[id];
}

static void ds_free_contents(DsObj *d);

static void ds_destroy_id(int32_t id) {
    if (id < 0 || id >= g_nds || !g_ds[id]) return;
    ds_free_contents(g_ds[id]);
    free(g_ds[id]);
    g_ds[id] = NULL;
}

/* ---- map ---- */
static uint32_t key_hash(Value k) {
    if (k.k == V_STRING) {
        uint32_t h = 2166136261u;
        for (int32_t i = 0; i < k.s->len; i++) { h ^= (uint8_t)k.s->chars[i]; h *= 16777619u; }
        return h;
    }
    double d = v_to_real(k);
    uint64_t u;
    memcpy(&u, &d, 8);
    return (uint32_t)(u ^ (u >> 32)) * 2654435761u;
}
static bool key_eq(Value a, Value b) {
    if ((a.k == V_STRING) != (b.k == V_STRING)) return false;
    if (a.k == V_STRING) return a.s->len == b.s->len && !memcmp(a.s->chars, b.s->chars, (size_t)a.s->len);
    return v_to_real(a) == v_to_real(b);
}
static void map_reindex(DsObj *m) {
    uint32_t cap = 16;
    while (cap < (uint32_t)m->live * 2 + 2) cap *= 2;
    /* compact deleted entries */
    int32_t w = 0;
    for (int32_t r = 0; r < m->nentries; r++)
        if (m->entries[r].used) m->entries[w++] = m->entries[r];
    m->nentries = w;
    free(m->index);
    m->index = d3_malloc(cap * sizeof(int32_t));
    for (uint32_t i = 0; i < cap; i++) m->index[i] = -1;
    m->index_cap = cap;
    for (int32_t e = 0; e < m->nentries; e++) {
        uint32_t j = key_hash(m->entries[e].key) & (cap - 1);
        while (m->index[j] >= 0) j = (j + 1) & (cap - 1);
        m->index[j] = e;
    }
}
static MapEntry *map_find(DsObj *m, Value k) {
    if (!m->index_cap) return NULL;
    uint32_t j = key_hash(k) & (m->index_cap - 1);
    while (m->index[j] >= 0) {
        MapEntry *e = &m->entries[m->index[j]];
        if (e->used && key_eq(e->key, k)) return e;
        j = (j + 1) & (m->index_cap - 1);
    }
    return NULL;
}
static void map_set(DsObj *m, Value k, Value v, bool replace_only) {
    MapEntry *e = map_find(m, k);
    if (e) { v_release(e->val); e->val = v; return; }
    if (replace_only) { v_release(v); return; }
    if (m->nentries == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->entries = d3_realloc(m->entries, (size_t)m->cap * sizeof(MapEntry));
    }
    m->entries[m->nentries++] = (MapEntry){v_copy(k), v, true};
    m->live++;
    if ((uint32_t)m->nentries * 2 >= m->index_cap) map_reindex(m);
    else {
        uint32_t j = key_hash(k) & (m->index_cap - 1);
        while (m->index[j] >= 0) j = (j + 1) & (m->index_cap - 1);
        m->index[j] = m->nentries - 1;
    }
}
static void map_delete(DsObj *m, Value k) {
    MapEntry *e = map_find(m, k);
    if (!e) return;
    v_release(e->key);
    v_release(e->val);
    e->used = false;
    m->live--;
    map_reindex(m);
}

static void ds_free_contents(DsObj *d) {
    for (int32_t i = 0; i < d->nentries; i++)
        if (d->entries[i].used) { v_release(d->entries[i].key); v_release(d->entries[i].val); }
    free(d->entries);
    free(d->index);
    for (int32_t i = 0; i < d->len; i++) v_release(d->items[d->head + i]);
    free(d->items);
    free(d->marks);
    if (d->cells) for (int32_t i = 0; i < d->gw * d->gh; i++) v_release(d->cells[i]);
    free(d->cells);
    memset(d, 0, sizeof *d);
}

int32_t dsmap_create(void) { return ds_new(DS_MAP); }

static Value bi_ds_map_create(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(ds_new(DS_MAP)); }
static Value bi_ds_map_destroy(int32_t argc, Value *a) { if (ds_get(argc ? a[0] : v_undef(), DS_MAP)) ds_destroy_id(ARGI(argc, a, 0, -1)); return v_undef(); }
static Value bi_ds_map_set(int32_t argc, Value *a) {
    DsObj *m = argc > 2 ? ds_get(a[0], DS_MAP) : NULL;
    if (m) map_set(m, a[1], v_copy(a[2]), false);
    return v_undef();
}
static Value bi_ds_map_add(int32_t argc, Value *a) {
    DsObj *m = argc > 2 ? ds_get(a[0], DS_MAP) : NULL;
    if (!m || map_find(m, a[1])) return v_bool(false);
    map_set(m, a[1], v_copy(a[2]), false);
    return v_bool(true);
}
static Value bi_ds_map_replace(int32_t argc, Value *a) { return bi_ds_map_set(argc, a); }
static Value bi_ds_map_find_value(int32_t argc, Value *a) {
    DsObj *m = argc > 1 ? ds_get(a[0], DS_MAP) : NULL;
    MapEntry *e = m ? map_find(m, a[1]) : NULL;
    return e ? v_copy(e->val) : v_undef();
}
static Value bi_ds_map_exists(int32_t argc, Value *a) {
    DsObj *m = argc > 1 ? ds_get(a[0], DS_MAP) : NULL;
    return v_bool(m && map_find(m, a[1]));
}
static Value bi_ds_map_delete(int32_t argc, Value *a) {
    DsObj *m = argc > 1 ? ds_get(a[0], DS_MAP) : NULL;
    if (m) map_delete(m, a[1]);
    return v_undef();
}
static Value bi_ds_map_size(int32_t argc, Value *a) { DsObj *m = argc ? ds_get(a[0], DS_MAP) : NULL; return v_real(m ? m->live : 0); }
static Value bi_ds_map_clear(int32_t argc, Value *a) {
    DsObj *m = argc ? ds_get(a[0], DS_MAP) : NULL;
    if (m) { ds_free_contents(m); m->type = DS_MAP; }
    return v_undef();
}
static Value bi_ds_map_empty(int32_t argc, Value *a) { DsObj *m = argc ? ds_get(a[0], DS_MAP) : NULL; return v_bool(!m || m->live == 0); }
static Value bi_ds_map_find_first(int32_t argc, Value *a) {
    DsObj *m = argc ? ds_get(a[0], DS_MAP) : NULL;
    if (m) for (int32_t i = 0; i < m->nentries; i++) if (m->entries[i].used) return v_copy(m->entries[i].key);
    return v_undef();
}
static Value bi_ds_map_find_next(int32_t argc, Value *a) {
    DsObj *m = argc > 1 ? ds_get(a[0], DS_MAP) : NULL;
    if (!m) return v_undef();
    bool found = false;
    for (int32_t i = 0; i < m->nentries; i++) {
        if (!m->entries[i].used) continue;
        if (found) return v_copy(m->entries[i].key);
        if (key_eq(m->entries[i].key, a[1])) found = true;
    }
    return v_undef();
}
static Value bi_ds_map_find_last(int32_t argc, Value *a) {
    DsObj *m = argc ? ds_get(a[0], DS_MAP) : NULL;
    if (m) for (int32_t i = m->nentries - 1; i >= 0; i--) if (m->entries[i].used) return v_copy(m->entries[i].key);
    return v_undef();
}
static Value bi_ds_map_keys_to_array(int32_t argc, Value *a) {
    DsObj *m = argc ? ds_get(a[0], DS_MAP) : NULL;
    Value arr = v_array_new(0);
    if (m) for (int32_t i = 0; i < m->nentries; i++) if (m->entries[i].used) arr_push(arr.a, v_copy(m->entries[i].key));
    return arr;
}
static Value bi_ds_map_copy(int32_t argc, Value *a) {
    DsObj *d = argc > 1 ? ds_get(a[0], DS_MAP) : NULL, *s = argc > 1 ? ds_get(a[1], DS_MAP) : NULL;
    if (!d || !s || d == s) return v_undef();
    ds_free_contents(d);
    d->type = DS_MAP;
    for (int32_t i = 0; i < s->nentries; i++)
        if (s->entries[i].used) map_set(d, s->entries[i].key, v_copy(s->entries[i].val), false);
    return v_undef();
}

/* ---- list ---- */
static void list_ensure(DsObj *l, int32_t n) {
    if (l->head > 0) {
        memmove(l->items, l->items + l->head, (size_t)l->len * sizeof(Value));
        l->head = 0;
    }
    if (n <= l->icap) return;
    int32_t cap = l->icap ? l->icap : 16;
    while (cap < n) cap *= 2;
    l->items = d3_realloc(l->items, (size_t)cap * sizeof(Value));
    l->marks = d3_realloc(l->marks, (size_t)cap);
    memset(l->marks + l->icap, 0, (size_t)(cap - l->icap));
    l->icap = cap;
}
static void list_add(DsObj *l, Value v) {
    list_ensure(l, l->len + 1);
    l->items[l->len] = v;
    l->marks[l->len] = 0;
    l->len++;
}
static Value bi_ds_list_create(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(ds_new(DS_LIST)); }
static Value bi_ds_list_destroy(int32_t argc, Value *a) { if (argc && ds_get(a[0], DS_LIST)) ds_destroy_id(v_to_int(a[0])); return v_undef(); }
static Value bi_ds_list_add(int32_t argc, Value *a) {
    DsObj *l = argc ? ds_get(a[0], DS_LIST) : NULL;
    if (l) for (int32_t i = 1; i < argc; i++) list_add(l, v_copy(a[i]));
    return v_undef();
}
static Value bi_ds_list_size(int32_t argc, Value *a) { DsObj *l = argc ? ds_get(a[0], DS_LIST) : NULL; return v_real(l ? l->len : 0); }
static Value bi_ds_list_empty(int32_t argc, Value *a) { DsObj *l = argc ? ds_get(a[0], DS_LIST) : NULL; return v_bool(!l || !l->len); }
static Value bi_ds_list_find_value(int32_t argc, Value *a) {
    DsObj *l = argc > 1 ? ds_get(a[0], DS_LIST) : NULL;
    int32_t i = ARGI(argc, a, 1, -1);
    return (l && i >= 0 && i < l->len) ? v_copy(l->items[l->head + i]) : v_undef();
}
static Value bi_ds_list_find_index(int32_t argc, Value *a) {
    DsObj *l = argc > 1 ? ds_get(a[0], DS_LIST) : NULL;
    if (l) for (int32_t i = 0; i < l->len; i++) if (v_equal(l->items[l->head + i], a[1])) return v_real(i);
    return v_real(-1);
}
static Value bi_ds_list_set(int32_t argc, Value *a) {
    DsObj *l = argc > 2 ? ds_get(a[0], DS_LIST) : NULL;
    int32_t i = ARGI(argc, a, 1, -1);
    if (!l || i < 0) return v_undef();
    while (l->len <= i) list_add(l, v_real(0));
    v_release(l->items[l->head + i]);
    l->items[l->head + i] = v_copy(a[2]);
    return v_undef();
}
static Value bi_ds_list_insert(int32_t argc, Value *a) {
    DsObj *l = argc > 2 ? ds_get(a[0], DS_LIST) : NULL;
    int32_t i = ARGI(argc, a, 1, 0);
    if (!l) return v_undef();
    if (i < 0) i = 0;
    if (i > l->len) i = l->len;
    list_ensure(l, l->len + 1);
    memmove(&l->items[i + 1], &l->items[i], (size_t)(l->len - i) * sizeof(Value));
    memmove(&l->marks[i + 1], &l->marks[i], (size_t)(l->len - i));
    l->items[i] = v_copy(a[2]);
    l->marks[i] = 0;
    l->len++;
    return v_undef();
}
static Value bi_ds_list_delete(int32_t argc, Value *a) {
    DsObj *l = argc > 1 ? ds_get(a[0], DS_LIST) : NULL;
    int32_t i = ARGI(argc, a, 1, -1);
    if (!l || i < 0 || i >= l->len) return v_undef();
    list_ensure(l, l->len);
    v_release(l->items[i]);
    memmove(&l->items[i], &l->items[i + 1], (size_t)(l->len - i - 1) * sizeof(Value));
    memmove(&l->marks[i], &l->marks[i + 1], (size_t)(l->len - i - 1));
    l->len--;
    return v_undef();
}
static Value bi_ds_list_clear(int32_t argc, Value *a) {
    DsObj *l = argc ? ds_get(a[0], DS_LIST) : NULL;
    if (l) { for (int32_t i = 0; i < l->len; i++) v_release(l->items[l->head + i]); l->len = 0; l->head = 0; }
    return v_undef();
}
static int cmp_asc(const void *x, const void *y) { return v_compare(*(const Value *)x, *(const Value *)y); }
static int cmp_desc(const void *x, const void *y) { return -v_compare(*(const Value *)x, *(const Value *)y); }
static Value bi_ds_list_sort(int32_t argc, Value *a) {
    DsObj *l = argc ? ds_get(a[0], DS_LIST) : NULL;
    if (l) {
        list_ensure(l, l->len);
        qsort(l->items, (size_t)l->len, sizeof(Value), argc > 1 && !v_to_bool(a[1]) ? cmp_desc : cmp_asc);
    }
    return v_undef();
}
static Value bi_ds_list_shuffle(int32_t argc, Value *a) {
    DsObj *l = argc ? ds_get(a[0], DS_LIST) : NULL;
    if (!l) return v_undef();
    list_ensure(l, l->len);
    for (int32_t i = l->len - 1; i > 0; i--) {
        int32_t j = (int32_t)(rng_next() * (i + 1));
        Value t = l->items[i];
        l->items[i] = l->items[j];
        l->items[j] = t;
    }
    return v_undef();
}
static Value bi_ds_list_copy(int32_t argc, Value *a) {
    DsObj *d = argc > 1 ? ds_get(a[0], DS_LIST) : NULL, *s = argc > 1 ? ds_get(a[1], DS_LIST) : NULL;
    if (!d || !s || d == s) return v_undef();
    for (int32_t i = 0; i < d->len; i++) v_release(d->items[d->head + i]);
    d->len = 0;
    d->head = 0;
    for (int32_t i = 0; i < s->len; i++) list_add(d, v_copy(s->items[s->head + i]));
    return v_undef();
}
static Value bi_ds_list_mark_as_map(int32_t argc, Value *a) {
    DsObj *l = argc > 1 ? ds_get(a[0], DS_LIST) : NULL;
    int32_t i = ARGI(argc, a, 1, -1);
    if (l && i >= 0 && i < l->len) { list_ensure(l, l->len); l->marks[i] = 1; }
    return v_undef();
}
static Value bi_ds_list_mark_as_list(int32_t argc, Value *a) {
    DsObj *l = argc > 1 ? ds_get(a[0], DS_LIST) : NULL;
    int32_t i = ARGI(argc, a, 1, -1);
    if (l && i >= 0 && i < l->len) { list_ensure(l, l->len); l->marks[i] = 2; }
    return v_undef();
}

/* ---- stack / queue ---- */
static Value bi_ds_stack_create(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(ds_new(DS_STACK)); }
static Value bi_ds_queue_create(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(ds_new(DS_QUEUE)); }
static DsObj *sq_get(Value v) {
    DsObj *d = ds_get(v, DS_STACK);
    return d ? d : ds_get(v, DS_QUEUE);
}
static Value bi_ds_stack_push(int32_t argc, Value *a) {
    DsObj *d = argc ? sq_get(a[0]) : NULL;
    if (d) for (int32_t i = 1; i < argc; i++) list_add(d, v_copy(a[i]));
    return v_undef();
}
static Value bi_ds_stack_pop(int32_t argc, Value *a) {
    DsObj *d = argc ? ds_get(a[0], DS_STACK) : NULL;
    if (!d || !d->len) return v_undef();
    d->len--;
    return d->items[d->head + d->len];
}
static Value bi_ds_stack_top(int32_t argc, Value *a) {
    DsObj *d = argc ? ds_get(a[0], DS_STACK) : NULL;
    return (d && d->len) ? v_copy(d->items[d->head + d->len - 1]) : v_undef();
}
static Value bi_ds_queue_dequeue(int32_t argc, Value *a) {
    DsObj *d = argc ? ds_get(a[0], DS_QUEUE) : NULL;
    if (!d || !d->len) return v_undef();
    Value v = d->items[d->head];
    d->head++;
    d->len--;
    return v;
}
static Value bi_ds_queue_head(int32_t argc, Value *a) {
    DsObj *d = argc ? ds_get(a[0], DS_QUEUE) : NULL;
    return (d && d->len) ? v_copy(d->items[d->head]) : v_undef();
}
static Value bi_ds_sq_size(int32_t argc, Value *a) { DsObj *d = argc ? sq_get(a[0]) : NULL; return v_real(d ? d->len : 0); }
static Value bi_ds_sq_empty(int32_t argc, Value *a) { DsObj *d = argc ? sq_get(a[0]) : NULL; return v_bool(!d || !d->len); }
static Value bi_ds_sq_destroy(int32_t argc, Value *a) { if (argc && sq_get(a[0])) ds_destroy_id(v_to_int(a[0])); return v_undef(); }
static Value bi_ds_sq_clear(int32_t argc, Value *a) {
    DsObj *d = argc ? sq_get(a[0]) : NULL;
    if (d) { for (int32_t i = 0; i < d->len; i++) v_release(d->items[d->head + i]); d->len = 0; d->head = 0; }
    return v_undef();
}

/* ---- grid ---- */
static Value bi_ds_grid_create(int32_t argc, Value *a) {
    int32_t id = ds_new(DS_GRID);
    DsObj *g = g_ds[id];
    g->gw = ARGI(argc, a, 0, 0);
    g->gh = ARGI(argc, a, 1, 0);
    if (g->gw < 0) g->gw = 0;
    if (g->gh < 0) g->gh = 0;
    g->cells = d3_malloc((size_t)(g->gw * g->gh + 1) * sizeof(Value));
    for (int32_t i = 0; i < g->gw * g->gh; i++) g->cells[i] = v_real(0);
    return v_real(id);
}
static Value *grid_cell(int32_t argc, Value *a) {
    DsObj *g = argc > 2 ? ds_get(a[0], DS_GRID) : NULL;
    int32_t x = ARGI(argc, a, 1, -1), y = ARGI(argc, a, 2, -1);
    if (!g || x < 0 || y < 0 || x >= g->gw || y >= g->gh) return NULL;
    return &g->cells[y * g->gw + x];
}
static Value bi_ds_grid_get(int32_t argc, Value *a) { Value *c = grid_cell(argc, a); return c ? v_copy(*c) : v_undef(); }
static Value bi_ds_grid_set(int32_t argc, Value *a) {
    Value *c = grid_cell(argc, a);
    if (c && argc > 3) { v_release(*c); *c = v_copy(a[3]); }
    return v_undef();
}
static Value bi_ds_grid_add(int32_t argc, Value *a) {
    Value *c = grid_cell(argc, a);
    if (c && argc > 3) { double d = v_to_real(*c) + v_to_real(a[3]); v_release(*c); *c = v_real(d); }
    return v_undef();
}
static Value bi_ds_grid_width(int32_t argc, Value *a) { DsObj *g = argc ? ds_get(a[0], DS_GRID) : NULL; return v_real(g ? g->gw : 0); }
static Value bi_ds_grid_height(int32_t argc, Value *a) { DsObj *g = argc ? ds_get(a[0], DS_GRID) : NULL; return v_real(g ? g->gh : 0); }
static Value bi_ds_grid_clear(int32_t argc, Value *a) {
    DsObj *g = argc > 1 ? ds_get(a[0], DS_GRID) : NULL;
    if (g) for (int32_t i = 0; i < g->gw * g->gh; i++) { v_release(g->cells[i]); g->cells[i] = v_copy(a[1]); }
    return v_undef();
}
static Value bi_ds_grid_resize(int32_t argc, Value *a) {
    DsObj *g = argc > 2 ? ds_get(a[0], DS_GRID) : NULL;
    if (!g) return v_undef();
    int32_t nw = ARGI(argc, a, 1, 0), nh = ARGI(argc, a, 2, 0);
    if (nw < 0) nw = 0;
    if (nh < 0) nh = 0;
    Value *nc = d3_malloc((size_t)(nw * nh + 1) * sizeof(Value));
    for (int32_t y = 0; y < nh; y++)
        for (int32_t x = 0; x < nw; x++)
            nc[y * nw + x] = (x < g->gw && y < g->gh) ? v_copy(g->cells[y * g->gw + x]) : v_real(0);
    for (int32_t i = 0; i < g->gw * g->gh; i++) v_release(g->cells[i]);
    free(g->cells);
    g->cells = nc;
    g->gw = nw;
    g->gh = nh;
    return v_undef();
}
static Value bi_ds_grid_destroy(int32_t argc, Value *a) { if (argc && ds_get(a[0], DS_GRID)) ds_destroy_id(v_to_int(a[0])); return v_undef(); }

static Value bi_ds_exists(int32_t argc, Value *a) {
    int32_t id = ARGI(argc, a, 0, -1), type = ARGI(argc, a, 1, -1);
    /* ds_type_map = 1, list = 2, stack = 3, grid = 4? GameMaker: map 1, list 2, stack 3, queue 4, grid 5, priority 6 */
    if (id < 0 || id >= g_nds || !g_ds[id]) return v_bool(false);
    return v_bool(type < 0 || g_ds[id]->type == type);
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

typedef struct { const char *s; const char *p; bool as_ds; } JP;

static void jp_ws(JP *j) { while (*j->p && isspace((unsigned char)*j->p)) j->p++; }
static Value jp_value(JP *j, int *mark);

static Value jp_string(JP *j) {
    j->p++; /* opening quote */
    size_t cap = 32, len = 0;
    char *buf = d3_malloc(cap);
    while (*j->p && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && *j->p) {
            char e = *j->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int k = 0; k < 4 && isxdigit((unsigned char)*j->p); k++) {
                    char h = *j->p++;
                    cp = cp * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower(h) - 'a' + 10));
                }
                char tmp[4];
                int n = utf8_encode(cp, tmp);
                while (len + 5 >= cap) { cap *= 2; buf = d3_realloc(buf, cap); }
                memcpy(buf + len, tmp, (size_t)n);
                len += (size_t)n;
                continue;
            }
            default: c = e; break;
            }
        }
        if (len + 2 >= cap) { cap *= 2; buf = d3_realloc(buf, cap); }
        buf[len++] = c;
    }
    if (*j->p == '"') j->p++;
    Value v = v_strn(buf, len);
    free(buf);
    return v;
}

static Value jp_object(JP *j) {
    j->p++;
    int32_t map_id = -1;
    Value st = v_undef();
    if (j->as_ds) map_id = ds_new(DS_MAP);
    else st = v_struct_new();
    for (;;) {
        jp_ws(j);
        if (*j->p == '}') { j->p++; break; }
        if (*j->p != '"') break;
        Value key = jp_string(j);
        jp_ws(j);
        if (*j->p == ':') j->p++;
        int mark = 0;
        Value val = jp_value(j, &mark);
        if (j->as_ds) map_set(g_ds[map_id], key, val, false);
        else varmap_set(&st.o->vars, d3_intern(key.s->chars), val);
        v_release(key);
        jp_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        break;
    }
    return j->as_ds ? v_real(map_id) : st;
}

static Value jp_array(JP *j) {
    j->p++;
    int32_t list_id = -1;
    Value arr = v_undef();
    if (j->as_ds) list_id = ds_new(DS_LIST);
    else arr = v_array_new(0);
    for (;;) {
        jp_ws(j);
        if (*j->p == ']') { j->p++; break; }
        int mark = 0;
        Value val = jp_value(j, &mark);
        if (j->as_ds) {
            DsObj *l = g_ds[list_id];
            list_add(l, val);
            l->marks[l->len - 1] = (uint8_t)mark;
        } else arr_push(arr.a, val);
        jp_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; break; }
        break;
    }
    return j->as_ds ? v_real(list_id) : arr;
}

static Value jp_value(JP *j, int *mark) {
    jp_ws(j);
    *mark = 0;
    char c = *j->p;
    if (c == '{') { *mark = 1; return jp_object(j); }
    if (c == '[') { *mark = 2; return jp_array(j); }
    if (c == '"') return jp_string(j);
    if (!strncmp(j->p, "true", 4)) { j->p += 4; return v_bool(true); }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; return v_bool(false); }
    if (!strncmp(j->p, "null", 4)) { j->p += 4; return v_undef(); }
    char *end;
    double d = strtod(j->p, &end);
    if (end == j->p) { if (*j->p) j->p++; return v_undef(); }
    j->p = end;
    return v_real(d);
}

Value json_to_dsmap(const char *text) {
    JP j = {text, text, true};
    jp_ws(&j);
    int mark = 0;
    Value v = jp_value(&j, &mark);
    if (mark == 1) return v;
    /* GameMaker wraps non-object roots in a map under "default" */
    int32_t id = ds_new(DS_MAP);
    Value key = v_str("default");
    map_set(g_ds[id], key, v, false);
    v_release(key);
    return v_real(id);
}

static Value bi_json_decode(int32_t argc, Value *a) { return json_to_dsmap(ARGS(argc, a, 0)); }
static Value bi_json_parse(int32_t argc, Value *a) {
    JP j = {ARGS(argc, a, 0), ARGS(argc, a, 0), false};
    int mark = 0;
    return jp_value(&j, &mark);
}

typedef struct { char *buf; size_t len, cap; } SB;
static void sb_put(SB *b, const char *s, size_t n) {
    while (b->len + n + 1 >= b->cap) { b->cap = b->cap ? b->cap * 2 : 256; b->buf = d3_realloc(b->buf, b->cap); }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}
static void sb_str(SB *b, const char *s) {
    sb_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { sb_put(b, "\\", 1); sb_put(b, s, 1); }
        else if (*s == '\n') sb_put(b, "\\n", 2);
        else if (*s == '\r') sb_put(b, "\\r", 2);
        else if (*s == '\t') sb_put(b, "\\t", 2);
        else sb_put(b, s, 1);
    }
    sb_put(b, "\"", 1);
}
static void json_write(SB *b, Value v, int mark, bool ds);
static void json_write_map(SB *b, DsObj *m) {
    sb_put(b, "{", 1);
    bool first = true;
    for (int32_t i = 0; i < m->nentries; i++) {
        if (!m->entries[i].used) continue;
        if (!first) sb_put(b, ",", 1);
        first = false;
        Value ks = v_to_string(m->entries[i].key);
        sb_str(b, ks.s->chars);
        v_release(ks);
        sb_put(b, ":", 1);
        json_write(b, m->entries[i].val, 0, true);
    }
    sb_put(b, "}", 1);
}
static void json_write(SB *b, Value v, int mark, bool ds) {
    char num[64];
    if (ds && mark == 1) { DsObj *m = ds_get(v, DS_MAP); if (m) { json_write_map(b, m); return; } }
    if (ds && mark == 2) {
        DsObj *l = ds_get(v, DS_LIST);
        if (l) {
            sb_put(b, "[", 1);
            for (int32_t i = 0; i < l->len; i++) {
                if (i) sb_put(b, ",", 1);
                json_write(b, l->items[l->head + i], l->marks ? l->marks[l->head + i] : 0, true);
            }
            sb_put(b, "]", 1);
            return;
        }
    }
    switch (v.k) {
    case V_STRING: sb_str(b, v.s->chars); break;
    case V_UNDEF: sb_put(b, "null", 4); break;
    case V_BOOL: sb_put(b, v.i ? "true" : "false", v.i ? 4 : 5); break;
    case V_ARRAY:
        sb_put(b, "[", 1);
        for (int32_t i = 0; i < v.a->len; i++) { if (i) sb_put(b, ",", 1); json_write(b, v.a->items[i], 0, ds); }
        sb_put(b, "]", 1);
        break;
    case V_STRUCT: {
        sb_put(b, "{", 1);
        bool first = true;
        for (uint32_t i = 0; i < v.o->vars.cap; i++) {
            if (v.o->vars.slots[i].key < 0) continue;
            if (!first) sb_put(b, ",", 1);
            first = false;
            sb_str(b, d3_intern_name(v.o->vars.slots[i].key));
            sb_put(b, ":", 1);
            json_write(b, v.o->vars.slots[i].v, 0, ds);
        }
        sb_put(b, "}", 1);
        break;
    }
    default: {
        double d = v_to_real(v);
        if (d == floor(d) && fabs(d) < 1e15) snprintf(num, sizeof num, "%.0f", d);
        else snprintf(num, sizeof num, "%.10g", d);
        sb_put(b, num, strlen(num));
    }
    }
}
static Value bi_json_encode(int32_t argc, Value *a) {
    SB b = {0};
    if (argc) json_write(&b, a[0], 1, true);
    Value r = v_str(b.buf ? b.buf : "{}");
    free(b.buf);
    return r;
}
static Value bi_json_stringify(int32_t argc, Value *a) {
    SB b = {0};
    if (argc) json_write(&b, a[0], 0, false);
    Value r = v_str(b.buf ? b.buf : "");
    free(b.buf);
    return r;
}
static Value bi_ds_map_add_map(int32_t argc, Value *a) { return bi_ds_map_add(argc, a); }
static Value bi_ds_map_add_list(int32_t argc, Value *a) { return bi_ds_map_add(argc, a); }

void builtins_register_ds(void) {
#define REG(n) builtin_register(#n, bi_##n)
    REG(ds_map_create); REG(ds_map_destroy); REG(ds_map_set); REG(ds_map_add); REG(ds_map_replace);
    REG(ds_map_find_value); REG(ds_map_exists); REG(ds_map_delete); REG(ds_map_size); REG(ds_map_clear);
    REG(ds_map_empty); REG(ds_map_find_first); REG(ds_map_find_next); REG(ds_map_find_last);
    REG(ds_map_keys_to_array); REG(ds_map_copy); REG(ds_map_add_map); REG(ds_map_add_list);
    builtin_register("ds_map_replace_map", bi_ds_map_set);
    builtin_register("ds_map_replace_list", bi_ds_map_set);
    REG(ds_list_create); REG(ds_list_destroy); REG(ds_list_add); REG(ds_list_size); REG(ds_list_empty);
    REG(ds_list_find_value); REG(ds_list_find_index); REG(ds_list_set); REG(ds_list_insert); REG(ds_list_delete);
    REG(ds_list_clear); REG(ds_list_sort); REG(ds_list_shuffle); REG(ds_list_copy); REG(ds_list_mark_as_map);
    REG(ds_list_mark_as_list);
    REG(ds_stack_create); REG(ds_queue_create); REG(ds_stack_push); REG(ds_stack_pop); REG(ds_stack_top);
    REG(ds_queue_dequeue); REG(ds_queue_head);
    builtin_register("ds_queue_enqueue", bi_ds_stack_push);
    builtin_register("ds_stack_size", bi_ds_sq_size);
    builtin_register("ds_queue_size", bi_ds_sq_size);
    builtin_register("ds_stack_empty", bi_ds_sq_empty);
    builtin_register("ds_queue_empty", bi_ds_sq_empty);
    builtin_register("ds_stack_destroy", bi_ds_sq_destroy);
    builtin_register("ds_queue_destroy", bi_ds_sq_destroy);
    builtin_register("ds_stack_clear", bi_ds_sq_clear);
    builtin_register("ds_queue_clear", bi_ds_sq_clear);
    REG(ds_grid_create); REG(ds_grid_get); REG(ds_grid_set); REG(ds_grid_add); REG(ds_grid_width);
    REG(ds_grid_height); REG(ds_grid_clear); REG(ds_grid_resize); REG(ds_grid_destroy);
    REG(ds_exists);
    REG(json_decode); REG(json_parse); REG(json_encode); REG(json_stringify);
#undef REG
}
