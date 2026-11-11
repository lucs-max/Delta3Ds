#include "value.h"

double g_math_epsilon = 0.00001;

/* Implemented by the runtime: frees an instance whose last reference went away. */
void runtime_instance_freed(Obj *o);

GMStr *gmstr_new(const char *s, size_t n) {
    GMStr *g = d3_malloc(sizeof(GMStr) + n + 1);
    g->rc = 1;
    g->len = (int32_t)n;
    if (n) memcpy(g->inline_data, s, n);
    g->inline_data[n] = 0;
    g->chars = g->inline_data;
    return g;
}

GMStr *gmstr_static(const char *s, size_t n) {
    GMStr *g = d3_malloc(sizeof(GMStr));
    g->rc = -1;
    g->len = (int32_t)n;
    g->chars = s;
    return g;
}

Value v_strn(const char *s, size_t n) {
    Value v;
    v.k = V_STRING;
    v.s = gmstr_new(s, n);
    return v;
}
Value v_str(const char *s) { return v_strn(s, strlen(s)); }
Value v_str_own(GMStr *s) { Value v; v.k = V_STRING; v.s = s; return v; }

Value v_array_new(int32_t len) {
    GMArray *a = d3_calloc(1, sizeof *a);
    a->rc = 1;
    a->owner = 0;
    if (len > 0) arr_resize(a, len);
    Value v;
    v.k = V_ARRAY;
    v.a = a;
    return v;
}

Obj *obj_new_struct(void) {
    Obj *o = d3_calloc(1, sizeof *o);
    o->rc = 1;
    o->kind = OBJ_STRUCT;
    varmap_init(&o->vars);
    return o;
}

Value v_struct_new(void) {
    Value v;
    v.k = V_STRUCT;
    v.o = obj_new_struct();
    return v;
}

Value v_method(int32_t func, Value bound) {
    GMMethod *m = d3_calloc(1, sizeof *m);
    m->rc = 1;
    m->func = func;
    m->bound = v_copy(bound);
    Value v;
    v.k = V_METHOD;
    v.m = m;
    return v;
}

void obj_retain(Obj *o) {
    if (o && o->rc >= 0) o->rc++;
}

void obj_release(Obj *o) {
    if (!o || o->rc < 0) return;
    if (--o->rc > 0) return;
    if (o->kind == OBJ_INSTANCE) {
        runtime_instance_freed(o);
        return;
    }
    varmap_free(&o->vars);
    free(o);
}

void v_retain(Value v) {
    switch (v.k) {
    case V_STRING: if (v.s->rc >= 0) v.s->rc++; break;
    case V_ARRAY: v.a->rc++; break;
    case V_STRUCT: obj_retain(v.o); break;
    case V_METHOD: v.m->rc++; break;
    default: break;
    }
}

void v_release(Value v) {
    switch (v.k) {
    case V_STRING:
        if (v.s->rc >= 0 && --v.s->rc == 0) free(v.s);
        break;
    case V_ARRAY:
        if (--v.a->rc == 0) {
            for (int32_t i = 0; i < v.a->len; i++) v_release(v.a->items[i]);
            free(v.a->items);
            free(v.a);
        }
        break;
    case V_STRUCT: obj_release(v.o); break;
    case V_METHOD:
        if (--v.m->rc == 0) {
            v_release(v.m->bound);
            free(v.m);
        }
        break;
    default: break;
    }
}

/* ---- conversion ---- */
double v_to_real(Value v) {
    switch (v.k) {
    case V_REAL: return v.r;
    case V_INT32: return v.i;
    case V_INT64: return (double)v.l;
    case V_BOOL: return v.i;
    case V_STRING: {
        /* GML real("..."): leading numeric part; invalid -> 0 */
        char *end;
        double d = strtod(v.s->chars, &end);
        return end == v.s->chars ? 0.0 : d;
    }
    case V_PTR: return (double)(intptr_t)v.p;
    default: return 0.0;
    }
}

int32_t v_to_int(Value v) {
    switch (v.k) {
    case V_INT32: case V_BOOL: return v.i;
    case V_INT64: return (int32_t)v.l;
    default: {
        double d = v_to_real(v);
        /* GML rounds to nearest when converting reals to ints */
        if (d != d) return 0;
        if (d >= 2147483647.0) return INT32_MAX;
        if (d <= -2147483648.0) return INT32_MIN;
        return (int32_t)llround(d);
    }
    }
}

int64_t v_to_int64(Value v) {
    switch (v.k) {
    case V_INT32: case V_BOOL: return v.i;
    case V_INT64: return v.l;
    default: {
        double d = v_to_real(v);
        if (d != d) return 0;
        return (int64_t)llround(d);
    }
    }
}

bool v_to_bool(Value v) {
    switch (v.k) {
    case V_REAL: return v.r > 0.5;
    case V_INT32: case V_BOOL: return v.i > 0;
    case V_INT64: return v.l > 0;
    case V_STRING: return v_to_real(v) > 0.5;
    case V_ARRAY: case V_STRUCT: case V_METHOD: case V_PTR: return true;
    default: return false;
    }
}

static void format_real(double d, char *buf, size_t n) {
    if (d != d) { snprintf(buf, n, "NaN"); return; }
    if (isinf(d)) { snprintf(buf, n, d > 0 ? "inf" : "-inf"); return; }
    double r = floor(d);
    if (r == d && fabs(d) < 1e15) {
        snprintf(buf, n, "%.0f", d);
        if (strcmp(buf, "-0") == 0) snprintf(buf, n, "0");
        return;
    }
    /* GML prints non-integers with two decimals */
    snprintf(buf, n, "%.2f", d);
    if (strcmp(buf, "-0.00") == 0) snprintf(buf, n, "0.00");
}

void v_format(Value v, char *buf, size_t n) {
    switch (v.k) {
    case V_UNDEF: snprintf(buf, n, "undefined"); break;
    case V_REAL: format_real(v.r, buf, n); break;
    case V_INT32: snprintf(buf, n, "%d", (int)v.i); break;
    case V_INT64: snprintf(buf, n, "%lld", (long long)v.l); break;
    case V_BOOL: snprintf(buf, n, "%s", v.i ? "true" : "false"); break;
    case V_STRING: snprintf(buf, n, "%s", v.s->chars); break;
    case V_ARRAY: {
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, n - pos, "[ ");
        for (int32_t i = 0; i < v.a->len && pos + 8 < n; i++) {
            char tmp[256];
            Value e = v.a->items[i];
            if (e.k == V_STRING) snprintf(tmp, sizeof tmp, "\"%s\"", e.s->chars);
            else v_format(e, tmp, sizeof tmp);
            pos += (size_t)snprintf(buf + pos, n - pos, "%s%s", i ? "," : "", tmp);
        }
        if (pos < n) snprintf(buf + pos, n - pos, " ]");
        break;
    }
    case V_STRUCT: snprintf(buf, n, "{ }"); break;
    case V_METHOD: snprintf(buf, n, "function %d", (int)v.m->func); break;
    case V_PTR: snprintf(buf, n, "%p", v.p); break;
    default: snprintf(buf, n, "?"); break;
    }
}

Value v_to_string(Value v) {
    if (v.k == V_STRING) return v_copy(v);
    char buf[512];
    v_format(v, buf, sizeof buf);
    return v_str(buf);
}

const char *v_kind_name(Value v) {
    static const char *names[] = {"undefined", "number", "int32", "int64", "bool", "string",
                                  "array", "struct", "method", "ptr"};
    return v.k < D3_ARRAY_LEN(names) ? names[v.k] : "unknown";
}

bool v_equal(Value a, Value b) {
    if (v_is_number(a) && v_is_number(b)) {
        if ((a.k == V_INT32 || a.k == V_BOOL) && (b.k == V_INT32 || b.k == V_BOOL)) return a.i == b.i;
        if (a.k == V_INT64 && b.k == V_INT64) return a.l == b.l;
        return fabs(v_to_real(a) - v_to_real(b)) <= g_math_epsilon;
    }
    if (a.k == V_STRING && b.k == V_STRING)
        return a.s == b.s || (a.s->len == b.s->len && memcmp(a.s->chars, b.s->chars, (size_t)a.s->len) == 0);
    if (a.k != b.k) return false;
    switch (a.k) {
    case V_UNDEF: return true;
    case V_ARRAY: return a.a == b.a;
    case V_STRUCT: return a.o == b.o;
    case V_METHOD: return a.m == b.m || (a.m->func == b.m->func);
    case V_PTR: return a.p == b.p;
    default: return false;
    }
}

int v_compare(Value a, Value b) {
    if (a.k == V_STRING && b.k == V_STRING) return strcmp(a.s->chars, b.s->chars);
    if (a.k == V_UNDEF || b.k == V_UNDEF) return a.k == b.k ? 0 : (a.k == V_UNDEF ? -1 : 1);
    double x = v_to_real(a), y = v_to_real(b);
    if (fabs(x - y) <= g_math_epsilon) return 0;
    return x < y ? -1 : 1;
}

/* ---- arrays ---- */
void arr_resize(GMArray *a, int32_t len) {
    if (len < 0) len = 0;
    if (len > a->cap) {
        int32_t ncap = a->cap ? a->cap : 4;
        while (ncap < len) ncap *= 2;
        a->items = d3_realloc(a->items, (size_t)ncap * sizeof(Value));
        a->cap = ncap;
    }
    for (int32_t i = a->len; i < len; i++) a->items[i] = v_real(0); /* GML pads arrays with 0 */
    for (int32_t i = len; i < a->len; i++) v_release(a->items[i]);
    a->len = len;
}

Value *arr_at(GMArray *a, int32_t idx, bool grow) {
    if (idx < 0) return NULL;
    if (idx >= a->len) {
        if (!grow) return NULL;
        arr_resize(a, idx + 1);
    }
    return &a->items[idx];
}

void arr_push(GMArray *a, Value v) {
    arr_resize(a, a->len + 1);
    a->items[a->len - 1] = v;
}

/* ---- variable maps ---- */
void varmap_init(VarMap *m) {
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
}

void varmap_free(VarMap *m) {
    for (uint32_t i = 0; i < m->cap; i++)
        if (m->slots[i].key >= 0) v_release(m->slots[i].v);
    free(m->slots);
    m->slots = NULL;
    m->cap = m->count = 0;
}

static void varmap_grow(VarMap *m) {
    uint32_t ncap = m->cap ? m->cap * 2 : 16;
    VarSlot *ns = d3_malloc(ncap * sizeof *ns);
    for (uint32_t i = 0; i < ncap; i++) ns[i].key = -1;
    for (uint32_t i = 0; i < m->cap; i++) {
        if (m->slots[i].key < 0) continue;
        uint32_t j = ((uint32_t)m->slots[i].key * 2654435761u) & (ncap - 1);
        while (ns[j].key >= 0) j = (j + 1) & (ncap - 1);
        ns[j] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
}

Value *varmap_get(VarMap *m, int32_t key) {
    if (!m->cap) return NULL;
    uint32_t j = ((uint32_t)key * 2654435761u) & (m->cap - 1);
    while (m->slots[j].key != -1) {
        if (m->slots[j].key == key) return &m->slots[j].v;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

Value *varmap_get_or_add(VarMap *m, int32_t key) {
    Value *v = varmap_get(m, key);
    if (v) return v;
    if ((m->count + 1) * 4 >= m->cap * 3) varmap_grow(m);
    uint32_t j = ((uint32_t)key * 2654435761u) & (m->cap - 1);
    /* reuse tombstones (-2) */
    while (m->slots[j].key >= 0) j = (j + 1) & (m->cap - 1);
    m->slots[j].key = key;
    m->slots[j].v = v_undef();
    m->count++;
    return &m->slots[j].v;
}

void varmap_set(VarMap *m, int32_t key, Value v) {
    Value *slot = varmap_get_or_add(m, key);
    Value old = *slot;
    *slot = v;
    v_release(old);
}

bool varmap_remove(VarMap *m, int32_t key) {
    Value *v = varmap_get(m, key);
    if (!v) return false;
    VarSlot *s = (VarSlot *)((char *)v - offsetof(VarSlot, v));
    v_release(s->v);
    s->key = -2; /* tombstone keeps probe chains intact */
    s->v = v_undef();
    m->count--;
    return true;
}
