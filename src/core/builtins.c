/*
 * Built-in GML functions: registry, math, strings, arrays, structs,
 * variables, scripts and miscellaneous game functions.
 */
#include "runtime.h"
#include "builtins_internal.h"
#include "../platform/platform.h"
#include <ctype.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* registry                                                            */
/* ------------------------------------------------------------------ */

typedef struct { int32_t name_id; BuiltinFn fn; const char *name; } BuiltinEntry;
static BuiltinEntry *g_fns;
static int32_t g_nfns, g_capfns;
static int32_t *g_fn_by_name; /* intern id -> index */
static int32_t g_fn_by_name_cap;

void builtin_register(const char *name, BuiltinFn fn) {
    int32_t id = d3_intern(name);
    if (id >= g_fn_by_name_cap) {
        int32_t ncap = g_fn_by_name_cap ? g_fn_by_name_cap : 1024;
        while (ncap <= id) ncap *= 2;
        g_fn_by_name = d3_realloc(g_fn_by_name, (size_t)ncap * sizeof(int32_t));
        for (int32_t i = g_fn_by_name_cap; i < ncap; i++) g_fn_by_name[i] = -1;
        g_fn_by_name_cap = ncap;
    }
    if (g_fn_by_name[id] >= 0) { g_fns[g_fn_by_name[id]].fn = fn; return; }
    if (g_nfns == g_capfns) {
        g_capfns = g_capfns ? g_capfns * 2 : 512;
        g_fns = d3_realloc(g_fns, (size_t)g_capfns * sizeof(BuiltinEntry));
    }
    g_fns[g_nfns] = (BuiltinEntry){id, fn, d3_intern_name(id)};
    g_fn_by_name[id] = g_nfns++;
}

int32_t builtin_lookup(const char *name) {
    int32_t id = d3_intern(name);
    return id < g_fn_by_name_cap ? g_fn_by_name[id] : -1;
}
BuiltinFn builtin_fn(int32_t index) { return (index >= 0 && index < g_nfns) ? g_fns[index].fn : NULL; }
const char *builtin_name(int32_t index) { return (index >= 0 && index < g_nfns) ? g_fns[index].name : "?"; }

/* Temporary string conversion for arguments. Uses a small ring of buffers. */
const char *ARGS(int32_t argc, Value *a, int32_t i) {
    static Value ring[8];
    static int pos;
    if (i >= argc) return "";
    if (a[i].k == V_STRING) return a[i].s->chars;
    pos = (pos + 1) & 7;
    v_release(ring[pos]);
    ring[pos] = v_to_string(a[i]);
    return ring[pos].s->chars;
}

/* ------------------------------------------------------------------ */
/* UTF-8 helpers                                                       */
/* ------------------------------------------------------------------ */

int utf8_decode(const char *s, int32_t len, int32_t *pos) {
    const uint8_t *u = (const uint8_t *)s;
    int32_t p = *pos;
    if (p >= len) return 0;
    uint32_t c = u[p];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (p + n > len) n = 1;
    if (n == 2) c = ((c & 0x1F) << 6) | (u[p + 1] & 0x3F);
    else if (n == 3) c = ((c & 0x0F) << 12) | ((u[p + 1] & 0x3F) << 6) | (u[p + 2] & 0x3F);
    else if (n == 4) c = ((c & 0x07) << 18) | ((u[p + 1] & 0x3F) << 12) | ((u[p + 2] & 0x3F) << 6) | (u[p + 3] & 0x3F);
    *pos = p + n;
    return (int)c;
}

int utf8_encode(uint32_t c, char *out) {
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

int32_t utf8_length(const char *s, int32_t len) {
    int32_t n = 0;
    for (int32_t i = 0; i < len; i++)
        if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
    return n;
}

/* Byte offset of the (0-based) character index; len if past the end. */
int32_t utf8_offset(const char *s, int32_t len, int32_t index) {
    if (index <= 0) return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < len; i++) {
        if (((uint8_t)s[i] & 0xC0) != 0x80) {
            if (n == index) return i;
            n++;
        }
    }
    return len;
}

/* ------------------------------------------------------------------ */
/* random (deterministic xorshift, seedable like GML)                  */
/* ------------------------------------------------------------------ */

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static int64_t g_seed;
double rng_next(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (double)(g_rng >> 11) / 9007199254740992.0;
}
static void rng_seed(int64_t s) {
    g_seed = s;
    g_rng = (uint64_t)s * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    if (!g_rng) g_rng = 1;
    for (int i = 0; i < 4; i++) rng_next();
}

/* ------------------------------------------------------------------ */
/* math                                                                */
/* ------------------------------------------------------------------ */

#define R1(name, expr) static Value bi_##name(int32_t argc, Value *a) { double x = ARGR(argc, a, 0, 0); (void)x; return v_real(expr); }
#define R2(name, expr) static Value bi_##name(int32_t argc, Value *a) { double x = ARGR(argc, a, 0, 0), y = ARGR(argc, a, 1, 0); (void)x; (void)y; return v_real(expr); }

static double gml_round(double x) {
    /* GML rounds halves to even (banker's rounding) */
    double r = nearbyint(x);
    return r;
}

R1(abs, fabs(x))
R1(sign, x > 0 ? 1 : x < 0 ? -1 : 0)
R1(round, gml_round(x))
R1(floor, floor(x))
R1(ceil, ceil(x))
R1(frac, x - trunc(x))
R1(sqrt, x < 0 ? 0 : sqrt(x))
R1(sqr, x * x)
R1(exp, exp(x))
R1(ln, log(x))
R1(log2, log2(x))
R1(log10, log10(x))
R1(sin, sin(x))
R1(cos, cos(x))
R1(tan, tan(x))
R1(arcsin, asin(x))
R1(arccos, acos(x))
R1(arctan, atan(x))
R1(dsin, sin(x * M_PI / 180))
R1(dcos, cos(x * M_PI / 180))
R1(dtan, tan(x * M_PI / 180))
R1(darcsin, asin(x) * 180 / M_PI)
R1(darccos, acos(x) * 180 / M_PI)
R1(darctan, atan(x) * 180 / M_PI)
R1(degtorad, x * M_PI / 180)
R1(radtodeg, x * 180 / M_PI)
R2(power, pow(x, y))
R2(logn, log(y) / log(x))
R2(arctan2, atan2(x, y))
R2(darctan2, atan2(x, y) * 180 / M_PI)
R2(lengthdir_x, x * cos(y * M_PI / 180))
R2(lengthdir_y, -x * sin(y * M_PI / 180))
R2(random_range, x + rng_next() * (y - x))

static Value bi_random(int32_t argc, Value *a) { return v_real(rng_next() * ARGR(argc, a, 0, 1)); }
static Value bi_irandom(int32_t argc, Value *a) {
    int64_t n = (int64_t)floor(ARGR(argc, a, 0, 0));
    return v_real(n <= 0 ? 0 : (double)(int64_t)(rng_next() * (double)(n + 1)));
}
static Value bi_irandom_range(int32_t argc, Value *a) {
    double lo = floor(ARGR(argc, a, 0, 0)), hi = floor(ARGR(argc, a, 1, 0));
    if (hi < lo) { double t = lo; lo = hi; hi = t; }
    return v_real(lo + floor(rng_next() * (hi - lo + 1)));
}
static Value bi_choose(int32_t argc, Value *a) {
    if (argc <= 0) return v_undef();
    int32_t i = (int32_t)(rng_next() * argc);
    if (i >= argc) i = argc - 1;
    return v_copy(a[i]);
}
static Value bi_randomize(int32_t argc, Value *a) {
    (void)argc; (void)a;
    rng_seed((int64_t)plat_time_us());
    return v_real((double)g_seed);
}
static Value bi_random_set_seed(int32_t argc, Value *a) { rng_seed((int64_t)ARGR(argc, a, 0, 0)); return v_undef(); }
static Value bi_random_get_seed(int32_t argc, Value *a) { (void)argc; (void)a; return v_real((double)g_seed); }

static Value bi_min(int32_t argc, Value *a) {
    if (!argc) return v_real(0);
    double m = v_to_real(a[0]);
    for (int32_t i = 1; i < argc; i++) { double x = v_to_real(a[i]); if (x < m) m = x; }
    return v_real(m);
}
static Value bi_max(int32_t argc, Value *a) {
    if (!argc) return v_real(0);
    double m = v_to_real(a[0]);
    for (int32_t i = 1; i < argc; i++) { double x = v_to_real(a[i]); if (x > m) m = x; }
    return v_real(m);
}
static Value bi_mean(int32_t argc, Value *a) {
    double s = 0;
    for (int32_t i = 0; i < argc; i++) s += v_to_real(a[i]);
    return v_real(argc ? s / argc : 0);
}
static int cmp_double(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return a < b ? -1 : a > b;
}
static Value bi_median(int32_t argc, Value *a) {
    if (!argc) return v_real(0);
    double *v = d3_malloc((size_t)argc * sizeof(double));
    for (int32_t i = 0; i < argc; i++) v[i] = v_to_real(a[i]);
    qsort(v, (size_t)argc, sizeof(double), cmp_double);
    double m = v[argc / 2];
    free(v);
    return v_real(m);
}
static Value bi_clamp(int32_t argc, Value *a) {
    double x = ARGR(argc, a, 0, 0), lo = ARGR(argc, a, 1, 0), hi = ARGR(argc, a, 2, 0);
    return v_real(x < lo ? lo : x > hi ? hi : x);
}
static Value bi_lerp(int32_t argc, Value *a) {
    double x = ARGR(argc, a, 0, 0), y = ARGR(argc, a, 1, 0), t = ARGR(argc, a, 2, 0);
    return v_real(x + (y - x) * t);
}
static Value bi_point_distance(int32_t argc, Value *a) {
    double dx = ARGR(argc, a, 2, 0) - ARGR(argc, a, 0, 0), dy = ARGR(argc, a, 3, 0) - ARGR(argc, a, 1, 0);
    return v_real(sqrt(dx * dx + dy * dy));
}
static Value bi_point_direction(int32_t argc, Value *a) {
    double dx = ARGR(argc, a, 2, 0) - ARGR(argc, a, 0, 0), dy = ARGR(argc, a, 3, 0) - ARGR(argc, a, 1, 0);
    double d = atan2(-dy, dx) * 180 / M_PI;
    if (d < 0) d += 360;
    return v_real(d);
}
static Value bi_angle_difference(int32_t argc, Value *a) {
    double d = fmod(ARGR(argc, a, 0, 0) - ARGR(argc, a, 1, 0) + 180, 360);
    if (d < 0) d += 360;
    return v_real(d - 180);
}
static Value bi_point_in_rectangle(int32_t argc, Value *a) {
    double px = ARGR(argc, a, 0, 0), py = ARGR(argc, a, 1, 0);
    return v_bool(px >= ARGR(argc, a, 2, 0) && px <= ARGR(argc, a, 4, 0) && py >= ARGR(argc, a, 3, 0) &&
                  py <= ARGR(argc, a, 5, 0));
}
static Value bi_rectangle_in_rectangle(int32_t argc, Value *a) {
    double l1 = ARGR(argc, a, 0, 0), t1 = ARGR(argc, a, 1, 0), r1 = ARGR(argc, a, 2, 0), b1 = ARGR(argc, a, 3, 0);
    double l2 = ARGR(argc, a, 4, 0), t2 = ARGR(argc, a, 5, 0), r2 = ARGR(argc, a, 6, 0), b2 = ARGR(argc, a, 7, 0);
    if (r1 < l2 || l1 > r2 || b1 < t2 || t1 > b2) return v_real(0);
    if (l1 >= l2 && r1 <= r2 && t1 >= t2 && b1 <= b2) return v_real(1);
    return v_real(2);
}

/* ------------------------------------------------------------------ */
/* type functions                                                      */
/* ------------------------------------------------------------------ */

static Value bi_real(int32_t argc, Value *a) { return v_real(argc ? v_to_real(a[0]) : 0); }
static Value bi_int64(int32_t argc, Value *a) { return v_int64(argc ? v_to_int64(a[0]) : 0); }
static Value bi_bool(int32_t argc, Value *a) { return v_bool(argc ? v_to_bool(a[0]) : false); }
static Value bi_string(int32_t argc, Value *a) {
    if (!argc) return v_str("");
    if (argc == 1) return v_to_string(a[0]);
    /* string("{0} {1}", a, b) template form */
    const char *fmt = ARGS(argc, a, 0);
    size_t cap = strlen(fmt) + 64, len = 0;
    char *out = d3_malloc(cap);
    for (const char *p = fmt; *p; p++) {
        if (*p == '{' && isdigit((unsigned char)p[1])) {
            char *end;
            long n = strtol(p + 1, &end, 10);
            if (*end == '}' && n + 1 < argc) {
                const char *s = ARGS(argc, a, (int32_t)n + 1);
                size_t sl = strlen(s);
                while (len + sl + 1 >= cap) { cap *= 2; out = d3_realloc(out, cap); }
                memcpy(out + len, s, sl);
                len += sl;
                p = end;
                continue;
            }
        }
        if (len + 2 >= cap) { cap *= 2; out = d3_realloc(out, cap); }
        out[len++] = *p;
    }
    Value r = v_strn(out, len);
    free(out);
    return r;
}
static Value bi_is_real(int32_t argc, Value *a) { return v_bool(argc && (a[0].k == V_REAL || a[0].k == V_INT32 || a[0].k == V_INT64)); }
static Value bi_is_numeric(int32_t argc, Value *a) { return v_bool(argc && v_is_number(a[0])); }
static Value bi_is_string(int32_t argc, Value *a) { return v_bool(argc && a[0].k == V_STRING); }
static Value bi_is_array(int32_t argc, Value *a) { return v_bool(argc && a[0].k == V_ARRAY); }
static Value bi_is_undefined(int32_t argc, Value *a) { return v_bool(!argc || a[0].k == V_UNDEF); }
static Value bi_is_struct(int32_t argc, Value *a) { return v_bool(argc && a[0].k == V_STRUCT); }
static Value bi_is_method(int32_t argc, Value *a) { return v_bool(argc && a[0].k == V_METHOD); }
static Value bi_is_bool(int32_t argc, Value *a) { return v_bool(argc && a[0].k == V_BOOL); }
static Value bi_is_int32(int32_t argc, Value *a) { return v_bool(argc && a[0].k == V_INT32); }
static Value bi_is_int64(int32_t argc, Value *a) { return v_bool(argc && a[0].k == V_INT64); }
static Value bi_is_ptr(int32_t argc, Value *a) { return v_bool(argc && a[0].k == V_PTR); }
static Value bi_is_nan(int32_t argc, Value *a) { return v_bool(argc && v_is_number(a[0]) && isnan(v_to_real(a[0]))); }
static Value bi_is_infinity(int32_t argc, Value *a) { return v_bool(argc && v_is_number(a[0]) && isinf(v_to_real(a[0]))); }
static Value bi_typeof(int32_t argc, Value *a) {
    if (!argc) return v_str("undefined");
    return v_str(v_kind_name(a[0]));
}

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

#define STR_ARG(i) (argc > (i) && a[i].k == V_STRING ? a[i].s : NULL)

static Value str_arg_value(int32_t argc, Value *a, int32_t i) {
    if (i < argc) return v_to_string(a[i]);
    return v_str("");
}

static Value bi_string_length(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t n = utf8_length(s.s->chars, s.s->len);
    v_release(s);
    return v_real(n);
}
static Value bi_string_byte_length(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t n = s.s->len;
    v_release(s);
    return v_real(n);
}
static Value bi_string_char_at(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t idx = ARGI(argc, a, 1, 1);
    if (idx < 1) idx = 1;
    int32_t off = utf8_offset(s.s->chars, s.s->len, idx - 1);
    Value r;
    if (off >= s.s->len) r = v_str("");
    else {
        int32_t p = off;
        utf8_decode(s.s->chars, s.s->len, &p);
        r = v_strn(s.s->chars + off, (size_t)(p - off));
    }
    v_release(s);
    return r;
}
static Value bi_string_ord_at(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t idx = ARGI(argc, a, 1, 1);
    if (idx < 1) idx = 1;
    int32_t off = utf8_offset(s.s->chars, s.s->len, idx - 1);
    int c = off < s.s->len ? utf8_decode(s.s->chars, s.s->len, &off) : -1;
    v_release(s);
    return v_real(c);
}
static Value bi_string_byte_at(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t idx = ARGI(argc, a, 1, 1) - 1;
    int c = (idx >= 0 && idx < s.s->len) ? (uint8_t)s.s->chars[idx] : 0;
    v_release(s);
    return v_real(c);
}
static Value bi_string_copy(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t idx = ARGI(argc, a, 1, 1), cnt = ARGI(argc, a, 2, 0);
    if (idx < 1) idx = 1;
    if (cnt < 0) cnt = 0;
    int32_t o1 = utf8_offset(s.s->chars, s.s->len, idx - 1);
    int32_t o2 = utf8_offset(s.s->chars, s.s->len, idx - 1 + cnt);
    Value r = v_strn(s.s->chars + o1, (size_t)(o2 - o1));
    v_release(s);
    return r;
}
static int32_t char_index_of_byte(const char *s, int32_t byte_off) { return utf8_length(s, byte_off); }
static Value bi_string_pos(int32_t argc, Value *a) {
    Value sub = str_arg_value(argc, a, 0), s = str_arg_value(argc, a, 1);
    const char *f = sub.s->len ? strstr(s.s->chars, sub.s->chars) : NULL;
    int32_t r = f ? char_index_of_byte(s.s->chars, (int32_t)(f - s.s->chars)) + 1 : 0;
    v_release(sub); v_release(s);
    return v_real(r);
}
static Value bi_string_pos_ext(int32_t argc, Value *a) {
    Value sub = str_arg_value(argc, a, 0), s = str_arg_value(argc, a, 1);
    int32_t start = ARGI(argc, a, 2, 0);
    int32_t off = utf8_offset(s.s->chars, s.s->len, start);
    const char *f = sub.s->len ? strstr(s.s->chars + off, sub.s->chars) : NULL;
    int32_t r = f ? char_index_of_byte(s.s->chars, (int32_t)(f - s.s->chars)) + 1 : 0;
    v_release(sub); v_release(s);
    return v_real(r);
}
static Value bi_string_last_pos(int32_t argc, Value *a) {
    Value sub = str_arg_value(argc, a, 0), s = str_arg_value(argc, a, 1);
    const char *last = NULL;
    if (sub.s->len)
        for (const char *p = strstr(s.s->chars, sub.s->chars); p; p = strstr(p + 1, sub.s->chars)) last = p;
    int32_t r = last ? char_index_of_byte(s.s->chars, (int32_t)(last - s.s->chars)) + 1 : 0;
    v_release(sub); v_release(s);
    return v_real(r);
}
static Value bi_string_delete(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t idx = ARGI(argc, a, 1, 1), cnt = ARGI(argc, a, 2, 0);
    if (idx < 1) idx = 1;
    if (cnt < 0) cnt = 0;
    int32_t o1 = utf8_offset(s.s->chars, s.s->len, idx - 1);
    int32_t o2 = utf8_offset(s.s->chars, s.s->len, idx - 1 + cnt);
    size_t n = (size_t)(s.s->len - (o2 - o1));
    char *buf = d3_malloc(n + 1);
    memcpy(buf, s.s->chars, (size_t)o1);
    memcpy(buf + o1, s.s->chars + o2, (size_t)(s.s->len - o2));
    Value r = v_strn(buf, n);
    free(buf);
    v_release(s);
    return r;
}
static Value bi_string_insert(int32_t argc, Value *a) {
    Value sub = str_arg_value(argc, a, 0), s = str_arg_value(argc, a, 1);
    int32_t idx = ARGI(argc, a, 2, 1);
    if (idx < 1) idx = 1;
    int32_t o = utf8_offset(s.s->chars, s.s->len, idx - 1);
    size_t n = (size_t)(s.s->len + sub.s->len);
    char *buf = d3_malloc(n + 1);
    memcpy(buf, s.s->chars, (size_t)o);
    memcpy(buf + o, sub.s->chars, (size_t)sub.s->len);
    memcpy(buf + o + sub.s->len, s.s->chars + o, (size_t)(s.s->len - o));
    Value r = v_strn(buf, n);
    free(buf);
    v_release(sub); v_release(s);
    return r;
}
static Value replace_impl(int32_t argc, Value *a, bool all, bool count_only) {
    Value s = str_arg_value(argc, a, count_only ? 1 : 0), sub = str_arg_value(argc, a, count_only ? 0 : 1);
    Value rep = count_only ? v_str("") : str_arg_value(argc, a, 2);
    if (!sub.s->len) {
        v_release(sub); v_release(rep);
        if (count_only) { v_release(s); return v_real(0); }
        return s;
    }
    size_t cap = (size_t)s.s->len + 16, len = 0;
    char *out = d3_malloc(cap);
    const char *p = s.s->chars;
    int32_t count = 0;
    for (;;) {
        const char *f = strstr(p, sub.s->chars);
        if (!f || (!all && count >= 1)) break;
        count++;
        size_t pre = (size_t)(f - p);
        while (len + pre + (size_t)rep.s->len + 1 >= cap) { cap *= 2; out = d3_realloc(out, cap); }
        memcpy(out + len, p, pre);
        len += pre;
        memcpy(out + len, rep.s->chars, (size_t)rep.s->len);
        len += (size_t)rep.s->len;
        p = f + sub.s->len;
    }
    size_t rest = strlen(p);
    while (len + rest + 1 >= cap) { cap *= 2; out = d3_realloc(out, cap); }
    memcpy(out + len, p, rest);
    len += rest;
    Value r = count_only ? v_real(count) : v_strn(out, len);
    free(out);
    v_release(s); v_release(sub); v_release(rep);
    return r;
}
static Value bi_string_replace(int32_t argc, Value *a) { return replace_impl(argc, a, false, false); }
static Value bi_string_replace_all(int32_t argc, Value *a) { return replace_impl(argc, a, true, false); }
static Value bi_string_count(int32_t argc, Value *a) { return replace_impl(argc, a, true, true); }
static Value case_impl(int32_t argc, Value *a, bool upper) {
    Value s = str_arg_value(argc, a, 0);
    char *buf = d3_malloc((size_t)s.s->len + 1);
    for (int32_t i = 0; i < s.s->len; i++) {
        unsigned char c = (unsigned char)s.s->chars[i];
        buf[i] = (char)(c < 0x80 ? (upper ? toupper(c) : tolower(c)) : c);
    }
    Value r = v_strn(buf, (size_t)s.s->len);
    free(buf);
    v_release(s);
    return r;
}
static Value bi_string_upper(int32_t argc, Value *a) { return case_impl(argc, a, true); }
static Value bi_string_lower(int32_t argc, Value *a) { return case_impl(argc, a, false); }
static Value bi_string_repeat(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t n = ARGI(argc, a, 1, 0);
    if (n < 0) n = 0;
    size_t len = (size_t)s.s->len * (size_t)n;
    char *buf = d3_malloc(len + 1);
    for (int32_t i = 0; i < n; i++) memcpy(buf + (size_t)i * (size_t)s.s->len, s.s->chars, (size_t)s.s->len);
    Value r = v_strn(buf, len);
    free(buf);
    v_release(s);
    return r;
}
static Value filter_impl(int32_t argc, Value *a, int mode) {
    Value s = str_arg_value(argc, a, 0);
    char *buf = d3_malloc((size_t)s.s->len + 1);
    size_t n = 0;
    for (int32_t i = 0; i < s.s->len; i++) {
        unsigned char c = (unsigned char)s.s->chars[i];
        bool keep = (mode == 0 && isalpha(c)) || (mode == 1 && isdigit(c)) || (mode == 2 && isalnum(c));
        if (keep) buf[n++] = (char)c;
    }
    Value r = v_strn(buf, n);
    free(buf);
    v_release(s);
    return r;
}
static Value bi_string_letters(int32_t argc, Value *a) { return filter_impl(argc, a, 0); }
static Value bi_string_digits(int32_t argc, Value *a) { return filter_impl(argc, a, 1); }
static Value bi_string_lettersdigits(int32_t argc, Value *a) { return filter_impl(argc, a, 2); }
static Value bi_string_format(int32_t argc, Value *a) {
    double x = ARGR(argc, a, 0, 0);
    int tot = ARGI(argc, a, 1, 0), dec = ARGI(argc, a, 2, 0);
    char buf[128];
    snprintf(buf, sizeof buf, "%*.*f", tot + (dec > 0 ? dec + 1 : 0), dec, x);
    return v_str(buf);
}
static Value bi_chr(int32_t argc, Value *a) {
    char buf[5];
    int n = utf8_encode((uint32_t)ARGI(argc, a, 0, 0), buf);
    return v_strn(buf, (size_t)n);
}
static Value bi_ansi_char(int32_t argc, Value *a) {
    char c = (char)ARGI(argc, a, 0, 0);
    return v_strn(&c, 1);
}
static Value bi_ord(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t p = 0;
    int c = s.s->len ? utf8_decode(s.s->chars, s.s->len, &p) : 0;
    v_release(s);
    return v_real(c);
}
static Value bi_string_trim(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    int32_t b = 0, e = s.s->len;
    while (b < e && isspace((unsigned char)s.s->chars[b])) b++;
    while (e > b && isspace((unsigned char)s.s->chars[e - 1])) e--;
    Value r = v_strn(s.s->chars + b, (size_t)(e - b));
    v_release(s);
    return r;
}
static Value bi_string_starts_with(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0), p = str_arg_value(argc, a, 1);
    bool r = p.s->len <= s.s->len && memcmp(s.s->chars, p.s->chars, (size_t)p.s->len) == 0;
    v_release(s); v_release(p);
    return v_bool(r);
}
static Value bi_string_ends_with(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0), p = str_arg_value(argc, a, 1);
    bool r = p.s->len <= s.s->len && memcmp(s.s->chars + s.s->len - p.s->len, p.s->chars, (size_t)p.s->len) == 0;
    v_release(s); v_release(p);
    return v_bool(r);
}
static Value bi_string_split(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0), d = str_arg_value(argc, a, 1);
    bool remove_empty = argc > 2 && v_to_bool(a[2]);
    Value arr = v_array_new(0);
    const char *p = s.s->chars;
    if (d.s->len) {
        for (;;) {
            const char *f = strstr(p, d.s->chars);
            size_t n = f ? (size_t)(f - p) : strlen(p);
            if (n || !remove_empty) arr_push(arr.a, v_strn(p, n));
            if (!f) break;
            p = f + d.s->len;
        }
    } else arr_push(arr.a, v_copy(s));
    v_release(s); v_release(d);
    return arr;
}
static Value bi_string_hash_to_newline(int32_t argc, Value *a) {
    Value s = str_arg_value(argc, a, 0);
    char *buf = d3_malloc((size_t)s.s->len + 1);
    for (int32_t i = 0; i < s.s->len; i++) buf[i] = s.s->chars[i] == '#' ? '\n' : s.s->chars[i];
    Value r = v_strn(buf, (size_t)s.s->len);
    free(buf);
    v_release(s);
    return r;
}

/* ------------------------------------------------------------------ */
/* arrays                                                              */
/* ------------------------------------------------------------------ */

static Value bi_array_create(int32_t argc, Value *a) {
    int32_t n = ARGI(argc, a, 0, 0);
    Value arr = v_array_new(n);
    if (argc > 1)
        for (int32_t i = 0; i < n; i++) { v_release(arr.a->items[i]); arr.a->items[i] = v_copy(a[1]); }
    return arr;
}
static Value bi_array_length(int32_t argc, Value *a) {
    return v_real(argc && a[0].k == V_ARRAY ? a[0].a->len : 0);
}
static Value bi_array_length_2d(int32_t argc, Value *a) {
    if (!argc || a[0].k != V_ARRAY) return v_real(0);
    Value *row = arr_at(a[0].a, ARGI(argc, a, 1, 0), false);
    return v_real(row && row->k == V_ARRAY ? row->a->len : 0);
}
static Value bi_array_push(int32_t argc, Value *a) {
    if (!argc || a[0].k != V_ARRAY) return v_undef();
    for (int32_t i = 1; i < argc; i++) arr_push(a[0].a, v_copy(a[i]));
    return v_undef();
}
static Value bi_array_pop(int32_t argc, Value *a) {
    if (!argc || a[0].k != V_ARRAY || !a[0].a->len) return v_undef();
    GMArray *arr = a[0].a;
    Value v = arr->items[arr->len - 1];
    arr->items[arr->len - 1] = v_undef();
    arr->len--;
    return v;
}
static Value bi_array_insert(int32_t argc, Value *a) {
    if (!argc || a[0].k != V_ARRAY) return v_undef();
    GMArray *arr = a[0].a;
    int32_t idx = ARGI(argc, a, 1, 0);
    if (idx < 0) idx = 0;
    if (idx > arr->len) arr_resize(arr, idx);
    int32_t n = argc - 2;
    if (n <= 0) return v_undef();
    int32_t old = arr->len;
    arr_resize(arr, old + n);
    memmove(&arr->items[idx + n], &arr->items[idx], (size_t)(old - idx) * sizeof(Value));
    for (int32_t i = 0; i < n; i++) arr->items[idx + i] = v_copy(a[2 + i]);
    return v_undef();
}
static Value bi_array_delete(int32_t argc, Value *a) {
    if (!argc || a[0].k != V_ARRAY) return v_undef();
    GMArray *arr = a[0].a;
    int32_t idx = ARGI(argc, a, 1, 0), n = ARGI(argc, a, 2, 1);
    if (idx < 0 || idx >= arr->len || n <= 0) return v_undef();
    if (idx + n > arr->len) n = arr->len - idx;
    for (int32_t i = 0; i < n; i++) v_release(arr->items[idx + i]);
    memmove(&arr->items[idx], &arr->items[idx + n], (size_t)(arr->len - idx - n) * sizeof(Value));
    arr->len -= n;
    return v_undef();
}
static Value bi_array_resize(int32_t argc, Value *a) {
    if (argc && a[0].k == V_ARRAY) arr_resize(a[0].a, ARGI(argc, a, 1, 0));
    return v_undef();
}
static Value bi_array_get(int32_t argc, Value *a) {
    if (!argc || a[0].k != V_ARRAY) return v_undef();
    Value *e = arr_at(a[0].a, ARGI(argc, a, 1, 0), false);
    return e ? v_copy(*e) : v_undef();
}
static Value bi_array_set(int32_t argc, Value *a) {
    if (argc < 3 || a[0].k != V_ARRAY) return v_undef();
    Value *e = arr_at(a[0].a, ARGI(argc, a, 1, 0), true);
    if (e) { v_release(*e); *e = v_copy(a[2]); }
    return v_undef();
}
static Value bi_array_copy(int32_t argc, Value *a) {
    /* array_copy(dest, dest_index, src, src_index, length) */
    if (argc < 5 || a[0].k != V_ARRAY || a[2].k != V_ARRAY) return v_undef();
    GMArray *d = a[0].a, *s = a[2].a;
    int32_t di = ARGI(argc, a, 1, 0), si = ARGI(argc, a, 3, 0), n = ARGI(argc, a, 4, 0);
    for (int32_t i = 0; i < n; i++) {
        Value *src = arr_at(s, si + i, false);
        if (!src) break;
        Value v = v_copy(*src);
        Value *dst = arr_at(d, di + i, true);
        if (dst) { v_release(*dst); *dst = v; } else v_release(v);
    }
    return v_undef();
}
static Value bi_array_equals(int32_t argc, Value *a) {
    if (argc < 2 || a[0].k != V_ARRAY || a[1].k != V_ARRAY) return v_bool(false);
    if (a[0].a->len != a[1].a->len) return v_bool(false);
    for (int32_t i = 0; i < a[0].a->len; i++)
        if (!v_equal(a[0].a->items[i], a[1].a->items[i])) return v_bool(false);
    return v_bool(true);
}
static Value bi_array_contains(int32_t argc, Value *a) {
    if (argc < 2 || a[0].k != V_ARRAY) return v_bool(false);
    for (int32_t i = 0; i < a[0].a->len; i++)
        if (v_equal(a[0].a->items[i], a[1])) return v_bool(true);
    return v_bool(false);
}
static Value bi_array_get_index(int32_t argc, Value *a) {
    if (argc < 2 || a[0].k != V_ARRAY) return v_real(-1);
    for (int32_t i = 0; i < a[0].a->len; i++)
        if (v_equal(a[0].a->items[i], a[1])) return v_real(i);
    return v_real(-1);
}
static bool g_sort_asc;
static Value g_sort_fn;
static int cmp_values(const void *x, const void *y) {
    const Value *a = x, *b = y;
    if (g_sort_fn.k == V_METHOD) {
        Value args[2] = {*a, *b};
        Value r = vm_call_method(g_sort_fn, cur_self(), cur_other(), 2, args);
        double d = v_to_real(r);
        v_release(r);
        return d < 0 ? -1 : d > 0;
    }
    int c = v_compare(*a, *b);
    return g_sort_asc ? c : -c;
}
static Value bi_array_sort(int32_t argc, Value *a) {
    if (!argc || a[0].k != V_ARRAY) return v_undef();
    g_sort_fn = argc > 1 && a[1].k == V_METHOD ? a[1] : v_undef();
    g_sort_asc = argc > 1 ? v_to_bool(a[1]) : true;
    qsort(a[0].a->items, (size_t)a[0].a->len, sizeof(Value), cmp_values);
    return v_undef();
}

/* ------------------------------------------------------------------ */
/* structs / variables / methods                                       */
/* ------------------------------------------------------------------ */

static Obj *target_obj(Value v) {
    if (v.k == V_STRUCT) return v.o;
    return resolve_single(v);
}

static Value bi_variable_global_exists(int32_t argc, Value *a) {
    return v_bool(varmap_get(&R->global.vars, d3_intern(ARGS(argc, a, 0))) != NULL);
}
static Value bi_variable_global_get(int32_t argc, Value *a) { return var_get(&R->global, d3_intern(ARGS(argc, a, 0))); }
static Value bi_variable_global_set(int32_t argc, Value *a) {
    var_set(&R->global, d3_intern(ARGS(argc, a, 0)), argc > 1 ? v_copy(a[1]) : v_undef());
    return v_undef();
}
static Value bi_variable_instance_exists(int32_t argc, Value *a) {
    return v_bool(argc > 1 && var_exists(target_obj(a[0]), d3_intern(ARGS(argc, a, 1))));
}
static Value bi_variable_instance_get(int32_t argc, Value *a) {
    return argc > 1 ? var_get(target_obj(a[0]), d3_intern(ARGS(argc, a, 1))) : v_undef();
}
static Value bi_variable_instance_set(int32_t argc, Value *a) {
    if (argc > 2) var_set(target_obj(a[0]), d3_intern(ARGS(argc, a, 1)), v_copy(a[2]));
    return v_undef();
}
static Value bi_variable_struct_remove(int32_t argc, Value *a) {
    if (argc > 1 && a[0].k == V_STRUCT) varmap_remove(&a[0].o->vars, d3_intern(ARGS(argc, a, 1)));
    return v_undef();
}
static Value names_of(Obj *o) {
    Value arr = v_array_new(0);
    if (!o) return arr;
    for (uint32_t i = 0; i < o->vars.cap; i++)
        if (o->vars.slots[i].key >= 0) arr_push(arr.a, v_str(d3_intern_name(o->vars.slots[i].key)));
    return arr;
}
static Value bi_variable_struct_get_names(int32_t argc, Value *a) { return names_of(argc ? target_obj(a[0]) : NULL); }
static Value bi_variable_struct_names_count(int32_t argc, Value *a) {
    Obj *o = argc ? target_obj(a[0]) : NULL;
    return v_real(o ? o->vars.count : 0);
}
static Value bi_method(int32_t argc, Value *a) {
    if (argc < 2) return v_undef();
    Value fn = a[1];
    int32_t func = -1;
    if (fn.k == V_METHOD) func = fn.m->func;
    else if (v_is_number(fn)) {
        int32_t s = v_to_int(fn);
        if (s >= 0 && s < R->dw.nscripts) func = R->dw.scripts[s].code;
    }
    if (func == -1) return v_undef();
    Value bound = a[0];
    if (bound.k != V_STRUCT && v_is_number(bound)) {
        int32_t t = v_to_int(bound);
        if (t == IT_SELF) {
            Obj *s = cur_self();
            Instance *in = as_instance(s);
            if (in) bound = v_real(in->id);
            else if (s && s->kind == OBJ_STRUCT) { Value sv; sv.k = V_STRUCT; sv.o = s; bound = sv; }
            else bound = v_undef();
        }
    }
    return v_method(func, bound);
}
static Value bi_method_get_index(int32_t argc, Value *a) {
    if (argc && a[0].k == V_METHOD) return v_copy(a[0]);
    return v_undef();
}
static Value bi_method_get_self(int32_t argc, Value *a) {
    if (argc && a[0].k == V_METHOD) return v_copy(a[0].m->bound);
    return v_undef();
}
static Value bi_script_execute(int32_t argc, Value *a) {
    if (!argc) return v_undef();
    return vm_call_method(a[0], cur_self(), cur_other(), argc - 1, a + 1);
}
static Value bi_script_execute_ext(int32_t argc, Value *a) {
    if (!argc) return v_undef();
    if (argc > 1 && a[1].k == V_ARRAY) {
        GMArray *arr = a[1].a;
        int32_t off = ARGI(argc, a, 2, 0), n = argc > 3 ? ARGI(argc, a, 3, arr->len) : arr->len - off;
        if (off < 0) off = 0;
        if (off + n > arr->len) n = arr->len - off;
        return vm_call_method(a[0], cur_self(), cur_other(), n > 0 ? n : 0, arr->items + off);
    }
    return vm_call_method(a[0], cur_self(), cur_other(), 0, NULL);
}
static Value bi_script_exists(int32_t argc, Value *a) {
    if (!argc) return v_bool(false);
    if (a[0].k == V_METHOD) return v_bool(true);
    int32_t s = v_to_int(a[0]);
    return v_bool(s >= 0 && s < R->dw.nscripts);
}
static Value bi_script_get_name(int32_t argc, Value *a) {
    if (argc && a[0].k == V_METHOD && a[0].m->func >= 0) {
        const char *n = R->dw.code[a[0].m->func].name;
        if (!strncmp(n, "gml_Script_", 11)) n += 11;
        return v_str(n);
    }
    int32_t s = ARGI(argc, a, 0, -1);
    return v_str(s >= 0 && s < R->dw.nscripts ? R->dw.scripts[s].name : "<undefined>");
}

/* internal compiler helpers */
static Value bi_new_array(int32_t argc, Value *a) {
    Value arr = v_array_new(argc);
    for (int32_t i = 0; i < argc; i++) { v_release(arr.a->items[i]); arr.a->items[i] = v_copy(a[i]); }
    return arr;
}
static Value bi_new_object(int32_t argc, Value *a) {
    if (!argc) return v_struct_new();
    Value s = v_struct_new();
    Value ctor = a[0];
    if (ctor.k == V_METHOD && ctor.m->func >= 0) {
        Value r = vm_call_code(ctor.m->func, s.o, cur_self(), argc - 1, a + 1);
        v_release(r);
    } else if (v_is_number(ctor)) {
        int32_t sc = v_to_int(ctor);
        if (sc >= 0 && sc < R->dw.nscripts) v_release(vm_call_code(R->dw.scripts[sc].code, s.o, cur_self(), argc - 1, a + 1));
    }
    return s;
}
static Value self_value(Obj *o) {
    if (!o) return v_undef();
    Instance *in = as_instance(o);
    if (in) return v_real(in->id);
    if (o->kind == OBJ_GLOBAL) return v_real(IT_GLOBAL);
    Value v;
    v.k = V_STRUCT;
    v.o = o;
    obj_retain(o);
    return v;
}
static Value bi_this(int32_t argc, Value *a) { (void)argc; (void)a; return self_value(cur_self()); }
static Value bi_other(int32_t argc, Value *a) { (void)argc; (void)a; return self_value(cur_other()); }
static Value bi_global(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(IT_GLOBAL); }
static Value bi_get_instance(int32_t argc, Value *a) { return argc ? v_copy(a[0]) : v_undef(); }
static Value bi_noop(int32_t argc, Value *a) { (void)argc; (void)a; return v_undef(); }
static Value bi_true(int32_t argc, Value *a) { (void)argc; (void)a; return v_bool(true); }
static Value bi_false(int32_t argc, Value *a) { (void)argc; (void)a; return v_bool(false); }
static Value bi_zero(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(0); }
static Value bi_minus_one(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(-1); }
static Value bi_empty_string(int32_t argc, Value *a) { (void)argc; (void)a; return v_str(""); }

/* ------------------------------------------------------------------ */
/* assets                                                              */
/* ------------------------------------------------------------------ */

static int32_t find_named(const char *name, int kind) {
    DataWin *dw = &R->dw;
    switch (kind) {
    case 0: for (int32_t i = 0; i < dw->nobjects; i++) if (!strcmp(dw->objects[i].name, name)) return i; break;
    case 1: for (int32_t i = 0; i < dw->nsprites; i++) if (!strcmp(dw->sprites[i].name, name)) return i; break;
    case 2: for (int32_t i = 0; i < dw->nsounds; i++) if (!strcmp(dw->sounds[i].name, name)) return i; break;
    case 3: for (int32_t i = 0; i < dw->nrooms; i++) if (!strcmp(dw->rooms[i].name, name)) return i; break;
    case 4: for (int32_t i = 0; i < dw->nbackgrounds; i++) if (!strcmp(dw->backgrounds[i].name, name)) return i; break;
    case 5: for (int32_t i = 0; i < dw->nscripts; i++) if (!strcmp(dw->scripts[i].name, name)) return i; break;
    case 6: for (int32_t i = 0; i < dw->nfonts; i++) if (!strcmp(dw->fonts[i].name, name)) return i; break;
    }
    return -1;
}
static Value bi_asset_get_index(int32_t argc, Value *a) {
    const char *n = ARGS(argc, a, 0);
    for (int k = 0; k < 7; k++) {
        int32_t i = find_named(n, k);
        if (i >= 0) {
            if (k == 5) {
                int32_t c = R->dw.scripts[i].code;
                if (c >= 0) return v_method(c, v_undef());
            }
            return v_real(i);
        }
    }
    return v_real(-1);
}
static Value bi_asset_get_type(int32_t argc, Value *a) {
    static const int types[] = {0, 1, 2, 3, 4, 5, 6}; /* asset_object, sprite, sound, room, tiles, script, font */
    const char *n = ARGS(argc, a, 0);
    for (int k = 0; k < 7; k++) if (find_named(n, k) >= 0) return v_real(types[k]);
    return v_real(-1);
}
static Value bi_object_get_name(int32_t argc, Value *a) {
    int32_t o = ARGI(argc, a, 0, -1);
    return v_str(o >= 0 && o < R->dw.nobjects ? R->dw.objects[o].name : "<undefined>");
}
static Value bi_object_get_parent(int32_t argc, Value *a) {
    int32_t o = ARGI(argc, a, 0, -1);
    return v_real(o >= 0 && o < R->dw.nobjects ? R->dw.objects[o].parent : -1);
}
static Value bi_object_get_sprite(int32_t argc, Value *a) {
    int32_t o = ARGI(argc, a, 0, -1);
    return v_real(o >= 0 && o < R->dw.nobjects ? R->dw.objects[o].sprite : -1);
}
static Value bi_object_exists(int32_t argc, Value *a) {
    int32_t o = ARGI(argc, a, 0, -1);
    return v_bool(o >= 0 && o < R->dw.nobjects);
}
static Value bi_object_is_ancestor(int32_t argc, Value *a) {
    int32_t o = ARGI(argc, a, 0, -1), p = ARGI(argc, a, 1, -1);
    return v_bool(o != p && object_is_ancestor(o, p));
}
static Value bi_room_get_name(int32_t argc, Value *a) {
    int32_t r = ARGI(argc, a, 0, -1);
    return v_str(r >= 0 && r < R->dw.nrooms ? R->dw.rooms[r].name : "<undefined>");
}
static Value bi_room_exists(int32_t argc, Value *a) {
    int32_t r = ARGI(argc, a, 0, -1);
    return v_bool(r >= 0 && r < R->dw.nrooms);
}
static Value bi_audio_get_name(int32_t argc, Value *a) {
    int32_t s = ARGI(argc, a, 0, -1);
    return v_str(s >= 0 && s < R->dw.nsounds ? R->dw.sounds[s].name : "<undefined>");
}
static Value bi_font_get_name(int32_t argc, Value *a) {
    int32_t s = ARGI(argc, a, 0, -1);
    return v_str(s >= 0 && s < R->dw.nfonts ? R->dw.fonts[s].name : "<undefined>");
}

/* ------------------------------------------------------------------ */
/* rooms / game                                                        */
/* ------------------------------------------------------------------ */

static Value bi_room_goto(int32_t argc, Value *a) { room_goto(ARGI(argc, a, 0, 0)); return v_undef(); }
static Value bi_room_goto_next(int32_t argc, Value *a) {
    (void)argc; (void)a;
    int32_t r = room_find_by_order(R->room, 1);
    if (r >= 0) room_goto(r);
    return v_undef();
}
static Value bi_room_goto_previous(int32_t argc, Value *a) {
    (void)argc; (void)a;
    int32_t r = room_find_by_order(R->room, -1);
    if (r >= 0) room_goto(r);
    return v_undef();
}
static Value bi_room_restart(int32_t argc, Value *a) { (void)argc; (void)a; room_goto(R->room); return v_undef(); }
static Value bi_room_next(int32_t argc, Value *a) { return v_real(room_find_by_order(ARGI(argc, a, 0, 0), 1)); }
static Value bi_room_previous(int32_t argc, Value *a) { return v_real(room_find_by_order(ARGI(argc, a, 0, 0), -1)); }
static Value bi_game_end(int32_t argc, Value *a) { (void)argc; (void)a; R->game_end_pending = true; return v_undef(); }
static Value bi_game_restart(int32_t argc, Value *a) { (void)argc; (void)a; R->game_restart_pending = true; return v_undef(); }

char g_game_change_request[512];
static Value bi_game_change(int32_t argc, Value *a) {
    /* game_change(working_directory, launch_parameters): used by chapter
     * select menus to start another data.win. The launcher picks it up. */
    const char *dir = ARGS(argc, a, 0);
    const char *params = ARGS(argc, a, 1);
    const char *game = strstr(params, "-game ");
    char file[256] = "data.win";
    if (game) {
        snprintf(file, sizeof file, "%s", game + 6);
        char *sp = strchr(file, ' ');
        if (sp) *sp = 0;
    }
    while (*dir == '/' || *dir == '.') dir++;
    snprintf(g_game_change_request, sizeof g_game_change_request, "%s%s/%s", R->dw.dir, dir, file);
    LOGI("game_change -> %s", g_game_change_request);
    R->game_end_pending = true;
    return v_undef();
}

static Value bi_event_inherited(int32_t argc, Value *a) {
    (void)argc; (void)a;
    Instance *i = as_instance(cur_self());
    if (i) event_run_inherited(i, cur_other());
    return v_undef();
}
static Value bi_event_perform(int32_t argc, Value *a) {
    Instance *i = as_instance(cur_self());
    if (i) event_run(i, ARGI(argc, a, 0, 0), ARGI(argc, a, 1, 0), cur_other());
    return v_undef();
}
static Value bi_event_user(int32_t argc, Value *a) {
    Instance *i = as_instance(cur_self());
    if (i) event_run(i, EV_OTHER, OTHER_USER0 + ARGI(argc, a, 0, 0), cur_other());
    return v_undef();
}
static Value bi_event_perform_object(int32_t argc, Value *a) {
    Instance *i = as_instance(cur_self());
    int32_t obj = ARGI(argc, a, 0, -1);
    int32_t code = event_find_code(obj, ARGI(argc, a, 1, 0), ARGI(argc, a, 2, 0), NULL);
    if (i && code >= 0) v_release(vm_call_code(code, &i->base, cur_other(), 0, NULL));
    return v_undef();
}

/* ------------------------------------------------------------------ */
/* debug / system                                                      */
/* ------------------------------------------------------------------ */

static Value bi_show_debug_message(int32_t argc, Value *a) {
    if (argc > 1) {
        Value s = bi_string(argc, a);
        LOGI("[game] %s", s.s->chars);
        v_release(s);
    } else LOGI("[game] %s", ARGS(argc, a, 0));
    return v_undef();
}
static Value bi_show_message(int32_t argc, Value *a) { plat_show_message(ARGS(argc, a, 0)); return v_undef(); }
static Value bi_show_error(int32_t argc, Value *a) {
    LOGE("show_error: %s", ARGS(argc, a, 0));
    if (argc > 1 && v_to_bool(a[1])) plat_fatal(ARGS(argc, a, 0));
    return v_undef();
}
static Value bi_get_timer(int32_t argc, Value *a) { (void)argc; (void)a; return v_real((double)(plat_time_us() - R->start_us)); }
static Value bi_date_current_datetime(int32_t argc, Value *a) {
    (void)argc; (void)a;
    /* days since 1899-12-30 (GameMaker / OLE date) */
    return v_real((double)time(NULL) / 86400.0 + 25569.0);
}
static Value bi_os_get_language(int32_t argc, Value *a) { (void)argc; (void)a; return v_str("en"); }
static Value bi_os_get_region(int32_t argc, Value *a) { (void)argc; (void)a; return v_str("US"); }
static Value bi_display_get_width(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->window_w); }
static Value bi_display_get_height(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->window_h); }
static Value bi_window_get_width(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->window_w); }
static Value bi_window_get_height(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->window_h); }
static Value bi_display_set_gui_size(int32_t argc, Value *a) {
    R->gui_w = ARGR(argc, a, 0, 0);
    R->gui_h = ARGR(argc, a, 1, 0);
    return v_undef();
}
static Value bi_display_get_gui_width(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->gui_w > 0 ? R->gui_w : R->window_w); }
static Value bi_display_get_gui_height(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->gui_h > 0 ? R->gui_h : R->window_h); }
static Value bi_window_set_size(int32_t argc, Value *a) {
    int w = ARGI(argc, a, 0, R->window_w), h = ARGI(argc, a, 1, R->window_h);
    if (w > 0 && h > 0) { R->window_w = w; R->window_h = h; plat_set_window_size(w, h); }
    return v_undef();
}
static Value bi_parameter_count(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(0); }
static Value bi_game_get_speed(int32_t argc, Value *a) {
    (void)a;
    /* gamespeed_fps = 0, gamespeed_microseconds = 1 */
    if (ARGI(argc, a, 0, 0) == 1) return v_real(1e6 / R->room_speed);
    return v_real(R->room_speed);
}
static Value bi_game_set_speed(int32_t argc, Value *a) {
    double v = ARGR(argc, a, 0, 30);
    if (ARGI(argc, a, 1, 0) == 1 && v > 0) v = 1e6 / v;
    if (v > 0) R->room_speed = v;
    return v_undef();
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

static int key_arg(int32_t argc, Value *a) {
    int k = ARGI(argc, a, 0, 0);
    if (k < 0 || k > 255) return 0;
    return k;
}
static Value bi_keyboard_check(int32_t argc, Value *a) { return v_bool(R->keys[key_arg(argc, a)]); }
static Value bi_keyboard_check_pressed(int32_t argc, Value *a) {
    int k = key_arg(argc, a);
    if (k == 1) return v_bool(R->any_key && !R->any_key_prev);
    if (k == 0) return v_bool(!R->any_key && R->any_key_prev);
    return v_bool(R->keys[k] && !R->keys_prev[k]);
}
static Value bi_keyboard_check_released(int32_t argc, Value *a) {
    int k = key_arg(argc, a);
    return v_bool(!R->keys[k] && R->keys_prev[k]);
}
static Value bi_keyboard_check_direct(int32_t argc, Value *a) { return v_bool(R->keys[key_arg(argc, a)]); }
static Value bi_keyboard_clear(int32_t argc, Value *a) {
    int k = key_arg(argc, a);
    R->keys[k] = 0;
    R->keys_prev[k] = 0;
    return v_undef();
}
static Value bi_io_clear(int32_t argc, Value *a) {
    (void)argc; (void)a;
    memset(R->keys, 0, sizeof R->keys);
    memset(R->keys_prev, 0, sizeof R->keys_prev);
    return v_undef();
}

/* ------------------------------------------------------------------ */
/* colours                                                             */
/* ------------------------------------------------------------------ */

static Value bi_make_color_rgb(int32_t argc, Value *a) {
    int r = ARGI(argc, a, 0, 0) & 255, g = ARGI(argc, a, 1, 0) & 255, b = ARGI(argc, a, 2, 0) & 255;
    return v_real(r | (g << 8) | (b << 16));
}
static Value bi_make_color_hsv(int32_t argc, Value *a) {
    double h = ARGR(argc, a, 0, 0) / 255.0 * 360.0, s = ARGR(argc, a, 1, 0) / 255.0, v = ARGR(argc, a, 2, 0) / 255.0;
    double c = v * s, x = c * (1 - fabs(fmod(h / 60.0, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    int R8 = (int)lround((r + m) * 255), G8 = (int)lround((g + m) * 255), B8 = (int)lround((b + m) * 255);
    return v_real(R8 | (G8 << 8) | (B8 << 16));
}
static Value bi_color_get_red(int32_t argc, Value *a) { return v_real(ARGI(argc, a, 0, 0) & 255); }
static Value bi_color_get_green(int32_t argc, Value *a) { return v_real((ARGI(argc, a, 0, 0) >> 8) & 255); }
static Value bi_color_get_blue(int32_t argc, Value *a) { return v_real((ARGI(argc, a, 0, 0) >> 16) & 255); }
static Value bi_merge_color(int32_t argc, Value *a) {
    int c1 = ARGI(argc, a, 0, 0), c2 = ARGI(argc, a, 1, 0);
    double t = ARGR(argc, a, 2, 0);
    int out = 0;
    for (int s = 0; s < 24; s += 8) {
        double x = (c1 >> s) & 255, y = (c2 >> s) & 255;
        out |= ((int)lround(x + (y - x) * t) & 255) << s;
    }
    return v_real(out);
}

/* ------------------------------------------------------------------ */

void builtins_register_core(void) {
#define REG(n) builtin_register(#n, bi_##n)
    REG(abs); REG(sign); REG(round); REG(floor); REG(ceil); REG(frac); REG(sqrt); REG(sqr); REG(exp);
    REG(ln); REG(log2); REG(log10); REG(sin); REG(cos); REG(tan); REG(arcsin); REG(arccos); REG(arctan);
    REG(dsin); REG(dcos); REG(dtan); REG(darcsin); REG(darccos); REG(darctan); REG(degtorad); REG(radtodeg);
    REG(power); REG(logn); REG(arctan2); REG(darctan2); REG(lengthdir_x); REG(lengthdir_y); REG(random_range);
    REG(random); REG(irandom); REG(irandom_range); REG(choose); REG(randomize); REG(random_set_seed);
    REG(random_get_seed); REG(min); REG(max); REG(mean); REG(median); REG(clamp); REG(lerp);
    REG(point_distance); REG(point_direction); REG(angle_difference); REG(point_in_rectangle);
    REG(rectangle_in_rectangle);
    builtin_register("randomise", bi_randomize);

    REG(real); REG(int64); REG(bool); REG(string); REG(is_real); REG(is_numeric); REG(is_string); REG(is_array);
    REG(is_undefined); REG(is_struct); REG(is_method); REG(is_bool); REG(is_int32); REG(is_int64); REG(is_ptr);
    REG(is_nan); REG(is_infinity); REG(typeof);

    REG(string_length); REG(string_byte_length); REG(string_char_at); REG(string_ord_at); REG(string_byte_at);
    REG(string_copy); REG(string_pos); REG(string_pos_ext); REG(string_last_pos); REG(string_delete);
    REG(string_insert); REG(string_replace); REG(string_replace_all); REG(string_count); REG(string_upper);
    REG(string_lower); REG(string_repeat); REG(string_letters); REG(string_digits); REG(string_lettersdigits);
    REG(string_format); REG(chr); REG(ansi_char); REG(ord); REG(string_trim); REG(string_starts_with);
    REG(string_ends_with); REG(string_split); REG(string_hash_to_newline);

    REG(array_create); REG(array_length); REG(array_length_2d); REG(array_push); REG(array_pop);
    REG(array_insert); REG(array_delete); REG(array_resize); REG(array_get); REG(array_set); REG(array_copy);
    REG(array_equals); REG(array_contains); REG(array_get_index); REG(array_sort);
    builtin_register("array_length_1d", bi_array_length);
    builtin_register("array_height_2d", bi_array_length);

    REG(variable_global_exists); REG(variable_global_get); REG(variable_global_set);
    REG(variable_instance_exists); REG(variable_instance_get); REG(variable_instance_set);
    builtin_register("variable_struct_exists", bi_variable_instance_exists);
    builtin_register("variable_struct_get", bi_variable_instance_get);
    builtin_register("variable_struct_set", bi_variable_instance_set);
    builtin_register("struct_exists", bi_variable_instance_exists);
    builtin_register("struct_get", bi_variable_instance_get);
    builtin_register("struct_set", bi_variable_instance_set);
    builtin_register("struct_remove", bi_variable_struct_remove);
    builtin_register("struct_get_names", bi_variable_struct_get_names);
    builtin_register("variable_instance_get_names", bi_variable_struct_get_names);
    REG(variable_struct_remove); REG(variable_struct_get_names); REG(variable_struct_names_count);
    REG(method); REG(method_get_index); REG(method_get_self); REG(script_execute); REG(script_execute_ext);
    REG(script_exists); REG(script_get_name);

    builtin_register("@@NewGMLArray@@", bi_new_array);
    builtin_register("@@NewGMLObject@@", bi_new_object);
    builtin_register("@@This@@", bi_this);
    builtin_register("@@Other@@", bi_other);
    builtin_register("@@Global@@", bi_global);
    builtin_register("@@GetInstance@@", bi_get_instance);
    builtin_register("@@CopyStatic@@", bi_noop);
    builtin_register("@@try_hook@@", bi_noop);
    builtin_register("@@try_unhook@@", bi_noop);
    builtin_register("@@finish_catch@@", bi_noop);
    builtin_register("@@finish_finally@@", bi_noop);

    REG(asset_get_index); REG(asset_get_type); REG(object_get_name); REG(object_get_parent);
    REG(object_get_sprite); REG(object_exists); REG(object_is_ancestor); REG(room_get_name); REG(room_exists);
    REG(audio_get_name); REG(font_get_name);

    REG(room_goto); REG(room_goto_next); REG(room_goto_previous); REG(room_restart); REG(room_next);
    REG(room_previous); REG(game_end); REG(game_restart); REG(game_change);
    REG(event_inherited); REG(event_perform); REG(event_user); REG(event_perform_object);

    REG(show_debug_message); REG(show_message); REG(show_error); REG(get_timer);
    REG(date_current_datetime); REG(os_get_language); REG(os_get_region); REG(display_get_width);
    REG(display_get_height); REG(window_get_width); REG(window_get_height); REG(display_set_gui_size);
    REG(display_get_gui_width); REG(display_get_gui_height); REG(window_set_size); REG(parameter_count);
    REG(game_get_speed); REG(game_set_speed);
    builtin_register("parameter_string", bi_empty_string);
    builtin_register("environment_get_variable", bi_empty_string);
    builtin_register("window_set_caption", bi_noop);
    builtin_register("window_get_caption", bi_empty_string);
    builtin_register("window_set_fullscreen", bi_noop);
    builtin_register("window_get_fullscreen", bi_false);
    builtin_register("window_center", bi_noop);
    builtin_register("window_set_position", bi_noop);
    builtin_register("window_set_cursor", bi_noop);
    builtin_register("window_has_focus", bi_true);
    builtin_register("window_get_x", bi_zero);
    builtin_register("window_get_y", bi_zero);
    builtin_register("window_set_min_width", bi_noop);
    builtin_register("window_set_min_height", bi_noop);
    builtin_register("window_enable_borderless_fullscreen", bi_noop);
    builtin_register("display_set_gui_maximize", bi_noop);
    builtin_register("display_set_gui_maximise", bi_noop);
    builtin_register("display_reset", bi_noop);
    builtin_register("display_get_frequency", bi_zero);
    builtin_register("gc_enable", bi_noop);
    builtin_register("gc_collect", bi_noop);
    builtin_register("steam_initialised", bi_false);
    builtin_register("steam_stats_ready", bi_false);
    builtin_register("steam_get_persona_name", bi_empty_string);
    builtin_register("steam_set_achievement", bi_noop);
    builtin_register("steam_get_achievement", bi_false);
    builtin_register("achievement_available", bi_false);
    builtin_register("os_is_paused", bi_false);
    builtin_register("os_is_network_connected", bi_false);
    builtin_register("code_is_compiled", bi_false);
    builtin_register("debug_event", bi_noop);
    builtin_register("show_debug_overlay", bi_noop);
    builtin_register("texture_prefetch", bi_noop);
    builtin_register("texture_flush", bi_noop);
    builtin_register("texturegroup_get_textures", bi_new_array);
    builtin_register("texture_is_ready", bi_true);
    builtin_register("font_texture_page_size", bi_noop);
    builtin_register("instance_activate_layer", bi_noop);
    builtin_register("gpu_set_texfilter", bi_noop);
    builtin_register("gpu_set_texrepeat", bi_noop);
    builtin_register("gpu_set_ztestenable", bi_noop);
    builtin_register("gpu_set_zwriteenable", bi_noop);
    builtin_register("gpu_set_alphatestenable", bi_noop);
    builtin_register("gpu_set_alphatestref", bi_noop);
    builtin_register("gpu_set_colorwriteenable", bi_noop);
    builtin_register("gpu_set_colourwriteenable", bi_noop);
    builtin_register("draw_set_swf_aa_level", bi_noop);
    builtin_register("draw_enable_alphablend", bi_noop);
    builtin_register("application_surface_enable", bi_noop);
    builtin_register("application_surface_draw_enable", bi_noop);
    builtin_register("application_surface_is_enabled", bi_true);
    builtin_register("vertex_format_begin", bi_noop);
    builtin_register("shader_set", bi_noop);
    builtin_register("shader_reset", bi_noop);
    builtin_register("shader_get_uniform", bi_minus_one);
    builtin_register("shader_get_sampler_index", bi_minus_one);
    builtin_register("shader_set_uniform_f", bi_noop);
    builtin_register("shader_set_uniform_i", bi_noop);
    builtin_register("shader_set_uniform_f_array", bi_noop);
    builtin_register("shader_is_compiled", bi_false);
    builtin_register("shaders_are_supported", bi_false);
    builtin_register("texture_set_stage", bi_noop);
    builtin_register("sprite_get_texture", bi_minus_one);
    builtin_register("surface_get_texture", bi_minus_one);
    builtin_register("texture_get_texel_width", bi_zero);
    builtin_register("texture_get_texel_height", bi_zero);
    builtin_register("gamepad_is_supported", bi_true);
    builtin_register("gamepad_is_connected", bi_false);
    builtin_register("gamepad_button_check", bi_false);
    builtin_register("gamepad_button_check_pressed", bi_false);
    builtin_register("gamepad_button_check_released", bi_false);
    builtin_register("gamepad_axis_value", bi_zero);
    builtin_register("gamepad_get_device_count", bi_zero);
    builtin_register("gamepad_get_description", bi_empty_string);
    builtin_register("gamepad_get_guid", bi_empty_string);
    builtin_register("gamepad_set_axis_deadzone", bi_noop);
    builtin_register("gamepad_button_count", bi_zero);
    builtin_register("gamepad_axis_count", bi_zero);
    builtin_register("gamepad_button_value", bi_zero);
    builtin_register("mouse_check_button", bi_false);
    builtin_register("mouse_check_button_pressed", bi_false);
    builtin_register("mouse_check_button_released", bi_false);
    builtin_register("device_mouse_check_button", bi_false);
    builtin_register("device_mouse_x", bi_zero);
    builtin_register("device_mouse_y", bi_zero);
    builtin_register("window_mouse_get_x", bi_zero);
    builtin_register("window_mouse_get_y", bi_zero);
    builtin_register("cursor_sprite", bi_noop);
    builtin_register("os_powersave_enable", bi_noop);
    builtin_register("os_lock_orientation", bi_noop);
    builtin_register("clipboard_has_text", bi_false);
    builtin_register("clipboard_get_text", bi_empty_string);
    builtin_register("clipboard_set_text", bi_noop);
    builtin_register("url_open", bi_noop);
    builtin_register("game_set_save_id", bi_noop);

    REG(keyboard_check); REG(keyboard_check_pressed); REG(keyboard_check_released); REG(keyboard_check_direct);
    REG(keyboard_clear); REG(io_clear);
    builtin_register("keyboard_set_map", bi_noop);
    builtin_register("keyboard_unset_map", bi_noop);

    REG(make_color_rgb); REG(make_color_hsv); REG(color_get_red); REG(color_get_green); REG(color_get_blue);
    REG(merge_color);
    builtin_register("make_colour_rgb", bi_make_color_rgb);
    builtin_register("make_colour_hsv", bi_make_color_hsv);
    builtin_register("colour_get_red", bi_color_get_red);
    builtin_register("colour_get_green", bi_color_get_green);
    builtin_register("colour_get_blue", bi_color_get_blue);
    builtin_register("merge_colour", bi_merge_color);
#undef REG
}

void builtins_register_all(void) {
    builtins_register_core();
    builtins_register_instances();
    builtins_register_draw();
    builtins_register_audio();
    builtins_register_ds();
    builtins_register_files();
}
