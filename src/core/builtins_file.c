/*
 * Files: text files, INI files and simple file system queries.
 * Like GameMaker's sandbox, reads look in the save directory first and then
 * in the game directory; writes always go to the save directory.
 */
#include "builtins_internal.h"
#include "../platform/platform.h"
#include <ctype.h>
#include <sys/stat.h>

static bool path_is_absolute(const char *p) { return p[0] == '/' || strchr(p, ':') != NULL; }

static void normalize(char *p) {
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

void file_resolve_write(const char *name, char *out, size_t n) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", name);
    normalize(tmp);
    if (path_is_absolute(tmp)) snprintf(out, n, "%s", tmp);
    else snprintf(out, n, "%s%s", plat_save_dir(), tmp);
}

void file_resolve_read(const char *name, char *out, size_t n) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", name);
    normalize(tmp);
    if (path_is_absolute(tmp)) { snprintf(out, n, "%s", tmp); return; }
    snprintf(out, n, "%s%s", plat_save_dir(), tmp);
    if (exists(out)) return;
    snprintf(out, n, "%s%s", R->dw.dir, tmp);
}

static void mkdirs_for(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (!strchr(tmp + (strncmp(tmp, "sdmc:", 5) == 0 ? 5 : 0), ':') || 1) mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

/* ------------------------------------------------------------------ */
/* text files                                                          */
/* ------------------------------------------------------------------ */

#define MAX_FILES 32
typedef struct {
    bool used, writing;
    FILE *f;
    char *data; /* read mode: whole file */
    size_t len, pos;
} TextFile;
static TextFile g_files[MAX_FILES];

static int32_t tf_alloc(void) {
    for (int32_t i = 0; i < MAX_FILES; i++) if (!g_files[i].used) return i;
    return -1;
}
static TextFile *tf_arg(int32_t argc, Value *a) {
    int32_t i = ARGI(argc, a, 0, -1);
    return (i >= 0 && i < MAX_FILES && g_files[i].used) ? &g_files[i] : NULL;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = d3_malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    if (len) *len = got;
    return buf;
}

static Value bi_file_text_open_read(int32_t argc, Value *a) {
    char path[600];
    file_resolve_read(ARGS(argc, a, 0), path, sizeof path);
    int32_t i = tf_alloc();
    if (i < 0) return v_real(-1);
    size_t len;
    char *d = slurp(path, &len);
    if (!d) return v_real(-1);
    g_files[i] = (TextFile){true, false, NULL, d, len, 0};
    /* skip UTF-8 BOM */
    if (len >= 3 && (uint8_t)d[0] == 0xEF && (uint8_t)d[1] == 0xBB && (uint8_t)d[2] == 0xBF) g_files[i].pos = 3;
    return v_real(i);
}
static Value open_write(int32_t argc, Value *a, const char *mode) {
    char path[600];
    file_resolve_write(ARGS(argc, a, 0), path, sizeof path);
    mkdirs_for(path);
    int32_t i = tf_alloc();
    if (i < 0) return v_real(-1);
    FILE *f = fopen(path, mode);
    if (!f) { LOGW("cannot write %s", path); return v_real(-1); }
    g_files[i] = (TextFile){true, true, f, NULL, 0, 0};
    return v_real(i);
}
static Value bi_file_text_open_write(int32_t argc, Value *a) { return open_write(argc, a, "wb"); }
static Value bi_file_text_open_append(int32_t argc, Value *a) { return open_write(argc, a, "ab"); }
static Value bi_file_text_close(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    if (!t) return v_undef();
    if (t->f) fclose(t->f);
    free(t->data);
    memset(t, 0, sizeof *t);
    return v_undef();
}
static Value bi_file_text_write_string(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    if (t && t->f) fputs(ARGS(argc, a, 1), t->f);
    return v_undef();
}
static Value bi_file_text_write_real(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    if (t && t->f) {
        double d = ARGR(argc, a, 1, 0);
        if (d == floor(d) && fabs(d) < 1e15) fprintf(t->f, "%.0f", d);
        else fprintf(t->f, "%.6f", d);
    }
    return v_undef();
}
static Value bi_file_text_writeln(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    if (t && t->f) fputs("\r\n", t->f);
    return v_undef();
}
static Value bi_file_text_read_string(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    if (!t || !t->data) return v_str("");
    size_t s = t->pos, e = s;
    while (e < t->len && t->data[e] != '\n' && t->data[e] != '\r') e++;
    t->pos = e;
    return v_strn(t->data + s, e - s);
}
static Value bi_file_text_read_real(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    if (!t || !t->data) return v_real(0);
    while (t->pos < t->len && (t->data[t->pos] == ' ' || t->data[t->pos] == '\t')) t->pos++;
    char *end;
    double d = strtod(t->data + t->pos, &end);
    t->pos = (size_t)(end - t->data);
    return v_real(d);
}
static Value bi_file_text_readln(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    if (!t || !t->data) return v_str("");
    size_t s = t->pos, e = s;
    while (e < t->len && t->data[e] != '\n') e++;
    size_t stop = e;
    if (e < t->len) e++;
    t->pos = e;
    return v_strn(t->data + s, stop - s);
}
static Value bi_file_text_eof(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    return v_bool(!t || !t->data || t->pos >= t->len);
}
static Value bi_file_text_eoln(int32_t argc, Value *a) {
    TextFile *t = tf_arg(argc, a);
    return v_bool(!t || !t->data || t->pos >= t->len || t->data[t->pos] == '\n' || t->data[t->pos] == '\r');
}

static Value bi_file_exists(int32_t argc, Value *a) {
    char path[600];
    file_resolve_read(ARGS(argc, a, 0), path, sizeof path);
    struct stat st;
    return v_bool(stat(path, &st) == 0 && !S_ISDIR(st.st_mode));
}
static Value bi_file_delete(int32_t argc, Value *a) {
    char path[600];
    file_resolve_write(ARGS(argc, a, 0), path, sizeof path);
    return v_bool(remove(path) == 0);
}
static Value bi_file_rename(int32_t argc, Value *a) {
    char p1[600], p2[600];
    file_resolve_write(ARGS(argc, a, 0), p1, sizeof p1);
    file_resolve_write(ARGS(argc, a, 1), p2, sizeof p2);
    return v_bool(rename(p1, p2) == 0);
}
static Value bi_file_copy(int32_t argc, Value *a) {
    char p1[600], p2[600];
    file_resolve_read(ARGS(argc, a, 0), p1, sizeof p1);
    file_resolve_write(ARGS(argc, a, 1), p2, sizeof p2);
    size_t len;
    char *d = slurp(p1, &len);
    if (!d) return v_bool(false);
    mkdirs_for(p2);
    FILE *f = fopen(p2, "wb");
    if (f) { fwrite(d, 1, len, f); fclose(f); }
    free(d);
    return v_bool(f != NULL);
}
static Value bi_directory_exists(int32_t argc, Value *a) {
    char path[600];
    file_resolve_read(ARGS(argc, a, 0), path, sizeof path);
    struct stat st;
    return v_bool(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}
static Value bi_directory_create(int32_t argc, Value *a) {
    char path[600];
    file_resolve_write(ARGS(argc, a, 0), path, sizeof path);
    size_t n = strlen(path);
    if (n + 1 < sizeof path && path[n - 1] != '/') { path[n] = '/'; path[n + 1] = 0; }
    mkdirs_for(path);
    return v_undef();
}
static Value bi_filename_name(int32_t argc, Value *a) {
    const char *p = ARGS(argc, a, 0), *s = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > s) s = b;
    return v_str(s ? s + 1 : p);
}
static Value bi_filename_path(int32_t argc, Value *a) {
    const char *p = ARGS(argc, a, 0), *s = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > s) s = b;
    return s ? v_strn(p, (size_t)(s - p + 1)) : v_str("");
}
static Value bi_filename_ext(int32_t argc, Value *a) {
    const char *p = ARGS(argc, a, 0), *d = strrchr(p, '.');
    return v_str(d ? d : "");
}
static Value bi_filename_change_ext(int32_t argc, Value *a) {
    const char *p = ARGS(argc, a, 0), *d = strrchr(p, '.');
    char buf[600];
    snprintf(buf, sizeof buf, "%.*s%s", (int)(d ? d - p : (long)strlen(p)), p, ARGS(argc, a, 1));
    return v_str(buf);
}

/* ------------------------------------------------------------------ */
/* INI                                                                 */
/* ------------------------------------------------------------------ */

typedef struct { char *section, *key, *value; } IniEntry;
static IniEntry *g_ini;
static int32_t g_nini, g_capini;
static char g_ini_path[600];
static bool g_ini_open, g_ini_dirty;

static void ini_clear(void) {
    for (int32_t i = 0; i < g_nini; i++) { free(g_ini[i].section); free(g_ini[i].key); free(g_ini[i].value); }
    g_nini = 0;
}
static char *trim_dup(const char *s, size_t n) {
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    char *d = d3_malloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static IniEntry *ini_find(const char *sec, const char *key) {
    for (int32_t i = 0; i < g_nini; i++)
        if (!strcmp(g_ini[i].section, sec) && !strcmp(g_ini[i].key, key)) return &g_ini[i];
    return NULL;
}
static void ini_put(const char *sec, const char *key, const char *val) {
    IniEntry *e = ini_find(sec, key);
    if (e) { free(e->value); e->value = d3_strdup(val); return; }
    if (g_nini == g_capini) {
        g_capini = g_capini ? g_capini * 2 : 64;
        g_ini = d3_realloc(g_ini, (size_t)g_capini * sizeof(IniEntry));
    }
    g_ini[g_nini++] = (IniEntry){d3_strdup(sec), d3_strdup(key), d3_strdup(val)};
}
static void ini_parse(const char *text) {
    char section[256] = "";
    const char *p = text;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        size_t n = (size_t)(e - p);
        const char *l = p;
        while (n && isspace((unsigned char)*l)) { l++; n--; }
        if (n && *l == '[') {
            const char *close = memchr(l, ']', n);
            if (close) {
                char *s = trim_dup(l + 1, (size_t)(close - l - 1));
                snprintf(section, sizeof section, "%s", s);
                free(s);
            }
        } else if (n && *l != ';' && *l != '#') {
            const char *eq = memchr(l, '=', n);
            if (eq) {
                char *k = trim_dup(l, (size_t)(eq - l)), *v = trim_dup(eq + 1, (size_t)(l + n - eq - 1));
                ini_put(section, k, v);
                free(k);
                free(v);
            }
        }
        p = *e ? e + 1 : e;
    }
}
static void ini_flush(void) {
    if (!g_ini_open || !g_ini_dirty) return;
    char path[600];
    file_resolve_write(g_ini_path, path, sizeof path);
    mkdirs_for(path);
    FILE *f = fopen(path, "wb");
    if (!f) { LOGW("cannot write ini %s", path); return; }
    /* write sections in first-seen order */
    for (int32_t i = 0; i < g_nini; i++) {
        bool seen = false;
        for (int32_t j = 0; j < i; j++) if (!strcmp(g_ini[j].section, g_ini[i].section)) { seen = true; break; }
        if (seen) continue;
        fprintf(f, "[%s]\r\n", g_ini[i].section);
        for (int32_t j = i; j < g_nini; j++)
            if (!strcmp(g_ini[j].section, g_ini[i].section)) fprintf(f, "%s=\"%s\"\r\n", g_ini[j].key, g_ini[j].value);
    }
    fclose(f);
    g_ini_dirty = false;
}
static Value bi_ini_open(int32_t argc, Value *a) {
    if (g_ini_open) ini_flush();
    ini_clear();
    snprintf(g_ini_path, sizeof g_ini_path, "%s", ARGS(argc, a, 0));
    char path[600];
    file_resolve_read(g_ini_path, path, sizeof path);
    char *d = slurp(path, NULL);
    if (d) { ini_parse(d); free(d); }
    g_ini_open = true;
    g_ini_dirty = false;
    return v_undef();
}
static Value bi_ini_open_from_string(int32_t argc, Value *a) {
    ini_clear();
    g_ini_path[0] = 0;
    ini_parse(ARGS(argc, a, 0));
    g_ini_open = true;
    g_ini_dirty = false;
    return v_undef();
}
static Value bi_ini_close(int32_t argc, Value *a) {
    (void)argc; (void)a;
    if (g_ini_path[0]) ini_flush();
    /* ini_close returns the ini contents as a string */
    size_t cap = 256, len = 0;
    char *out = d3_malloc(cap);
    out[0] = 0;
    for (int32_t i = 0; i < g_nini; i++) {
        char line[1024];
        int n = snprintf(line, sizeof line, "[%s] %s=\"%s\"\n", g_ini[i].section, g_ini[i].key, g_ini[i].value);
        if (n < 0) continue;
        while (len + (size_t)n + 1 >= cap) { cap *= 2; out = d3_realloc(out, cap); }
        memcpy(out + len, line, (size_t)n);
        len += (size_t)n;
        out[len] = 0;
    }
    ini_clear();
    g_ini_open = false;
    Value r = v_strn(out, len);
    free(out);
    return r;
}
static Value bi_ini_read_string(int32_t argc, Value *a) {
    IniEntry *e = ini_find(ARGS(argc, a, 0), ARGS(argc, a, 1));
    if (e) return v_str(e->value);
    return argc > 2 ? v_to_string(a[2]) : v_str("");
}
static Value bi_ini_read_real(int32_t argc, Value *a) {
    IniEntry *e = ini_find(ARGS(argc, a, 0), ARGS(argc, a, 1));
    if (e) {
        char *end;
        double d = strtod(e->value, &end);
        if (end != e->value) return v_real(d);
    }
    return v_real(ARGR(argc, a, 2, 0));
}
static Value bi_ini_write_string(int32_t argc, Value *a) {
    ini_put(ARGS(argc, a, 0), ARGS(argc, a, 1), ARGS(argc, a, 2));
    g_ini_dirty = true;
    return v_undef();
}
static Value bi_ini_write_real(int32_t argc, Value *a) {
    char buf[64];
    double d = ARGR(argc, a, 2, 0);
    if (d == floor(d) && fabs(d) < 1e15) snprintf(buf, sizeof buf, "%.0f", d);
    else snprintf(buf, sizeof buf, "%.6f", d);
    ini_put(ARGS(argc, a, 0), ARGS(argc, a, 1), buf);
    g_ini_dirty = true;
    return v_undef();
}
static Value bi_ini_key_exists(int32_t argc, Value *a) { return v_bool(ini_find(ARGS(argc, a, 0), ARGS(argc, a, 1)) != NULL); }
static Value bi_ini_section_exists(int32_t argc, Value *a) {
    const char *s = ARGS(argc, a, 0);
    for (int32_t i = 0; i < g_nini; i++) if (!strcmp(g_ini[i].section, s)) return v_bool(true);
    return v_bool(false);
}
static Value bi_ini_key_delete(int32_t argc, Value *a) {
    IniEntry *e = ini_find(ARGS(argc, a, 0), ARGS(argc, a, 1));
    if (e) {
        int32_t i = (int32_t)(e - g_ini);
        free(e->section); free(e->key); free(e->value);
        memmove(&g_ini[i], &g_ini[i + 1], (size_t)(g_nini - i - 1) * sizeof(IniEntry));
        g_nini--;
        g_ini_dirty = true;
    }
    return v_undef();
}
static Value bi_ini_section_delete(int32_t argc, Value *a) {
    const char *s = ARGS(argc, a, 0);
    int32_t w = 0;
    for (int32_t r = 0; r < g_nini; r++) {
        if (!strcmp(g_ini[r].section, s)) { free(g_ini[r].section); free(g_ini[r].key); free(g_ini[r].value); g_ini_dirty = true; }
        else g_ini[w++] = g_ini[r];
    }
    g_nini = w;
    return v_undef();
}

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

static Value bi_file_find_first(int32_t argc, Value *a) { (void)argc; (void)a; return v_str(""); }
static Value bi_noop_f(int32_t argc, Value *a) { (void)argc; (void)a; return v_undef(); }
static Value bi_minus_one_f(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(-1); }

void builtins_register_files(void) {
#define REG(n) builtin_register(#n, bi_##n)
    REG(file_text_open_read); REG(file_text_open_write); REG(file_text_open_append); REG(file_text_close);
    REG(file_text_write_string); REG(file_text_write_real); REG(file_text_writeln); REG(file_text_read_string);
    REG(file_text_read_real); REG(file_text_readln); REG(file_text_eof); REG(file_text_eoln);
    REG(file_exists); REG(file_delete); REG(file_rename); REG(file_copy); REG(directory_exists);
    REG(directory_create); REG(filename_name); REG(filename_path); REG(filename_ext); REG(filename_change_ext);
    REG(ini_open); REG(ini_open_from_string); REG(ini_close); REG(ini_read_string); REG(ini_read_real);
    REG(ini_write_string); REG(ini_write_real); REG(ini_key_exists); REG(ini_section_exists);
    REG(ini_key_delete); REG(ini_section_delete);
    REG(file_find_first);
    builtin_register("file_find_next", bi_file_find_first);
    builtin_register("file_find_close", bi_noop_f);
    builtin_register("buffer_create", bi_minus_one_f);
    builtin_register("buffer_delete", bi_noop_f);
    builtin_register("buffer_exists", bi_noop_f);
#undef REG
}
