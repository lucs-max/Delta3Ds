#include "common.h"
#include <stdarg.h>
#include "../platform/platform.h"

static D3LogLevel g_min_level = D3_LOG_INFO;

void d3_log(D3LogLevel lvl, const char *fmt, ...) {
    if (lvl < g_min_level && !getenv("D3_DEBUG")) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    static const char *tags[] = {"D", "I", "W", "E"};
    plat_log_line(tags[lvl], buf);
}

void d3_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_fatal(buf);
    exit(1);
}

void *d3_malloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) d3_fatal("Out of memory (malloc %u bytes)", (unsigned)n);
    return p;
}
void *d3_calloc(size_t count, size_t n) {
    void *p = calloc(count ? count : 1, n ? n : 1);
    if (!p) d3_fatal("Out of memory (calloc %u x %u bytes)", (unsigned)count, (unsigned)n);
    return p;
}
void *d3_realloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) d3_fatal("Out of memory (realloc %u bytes)", (unsigned)n);
    return q;
}
char *d3_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = d3_malloc(n);
    memcpy(d, s, n);
    return d;
}

/* ---- string interning (open addressing, FNV-1a) ---- */
typedef struct { uint32_t hash; int id; } InternSlot;
static InternSlot *g_slots;
static uint32_t g_slot_cap;
static char **g_names;
static int g_name_count, g_name_cap;

static uint32_t fnv1a(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h ? h : 1;
}

static void intern_grow(void) {
    uint32_t ncap = g_slot_cap ? g_slot_cap * 2 : 4096;
    InternSlot *ns = d3_calloc(ncap, sizeof *ns);
    for (uint32_t i = 0; i < g_slot_cap; i++) {
        if (!g_slots[i].hash) continue;
        uint32_t j = g_slots[i].hash & (ncap - 1);
        while (ns[j].hash) j = (j + 1) & (ncap - 1);
        ns[j] = g_slots[i];
    }
    free(g_slots);
    g_slots = ns;
    g_slot_cap = ncap;
}

int d3_intern_n(const char *s, size_t n) {
    if ((uint32_t)(g_name_count + 1) * 2 >= g_slot_cap) intern_grow();
    uint32_t h = fnv1a(s, n);
    uint32_t j = h & (g_slot_cap - 1);
    while (g_slots[j].hash) {
        if (g_slots[j].hash == h) {
            const char *e = g_names[g_slots[j].id];
            if (strlen(e) == n && memcmp(e, s, n) == 0) return g_slots[j].id;
        }
        j = (j + 1) & (g_slot_cap - 1);
    }
    if (g_name_count == g_name_cap) {
        g_name_cap = g_name_cap ? g_name_cap * 2 : 4096;
        g_names = d3_realloc(g_names, (size_t)g_name_cap * sizeof *g_names);
    }
    char *copy = d3_malloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = 0;
    g_names[g_name_count] = copy;
    g_slots[j].hash = h;
    g_slots[j].id = g_name_count;
    return g_name_count++;
}

int d3_intern(const char *s) { return d3_intern_n(s, strlen(s)); }
const char *d3_intern_name(int id) {
    return (id >= 0 && id < g_name_count) ? g_names[id] : "<?>";
}
int d3_intern_count(void) { return g_name_count; }

void d3_intern_reset(void) {
    for (int i = 0; i < g_name_count; i++) free(g_names[i]);
    free(g_names);
    free(g_slots);
    g_names = NULL;
    g_slots = NULL;
    g_name_count = g_name_cap = 0;
    g_slot_cap = 0;
}
