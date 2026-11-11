/*
 * GameMaker bytecode (version 15-17, GMS 2.3+ / GM 2022-2024) interpreter.
 *
 * Instructions are executed in place from the loaded data.win. Variable and
 * function reference chains were patched at load time (datawin.c) so that
 * operands hold VARI / FUNC indices.
 */
#include "runtime.h"
#include <stdarg.h>

/* opcodes */
enum {
    OP_CONV = 0x07, OP_MUL = 0x08, OP_DIV = 0x09, OP_REM = 0x0A, OP_MOD = 0x0B, OP_ADD = 0x0C,
    OP_SUB = 0x0D, OP_AND = 0x0E, OP_OR = 0x0F, OP_XOR = 0x10, OP_NEG = 0x11, OP_NOT = 0x12,
    OP_SHL = 0x13, OP_SHR = 0x14, OP_CMP = 0x15, OP_POP = 0x45, OP_PUSHI = 0x84, OP_DUP = 0x86,
    OP_CALLV = 0x99, OP_RET = 0x9C, OP_EXIT = 0x9D, OP_POPZ = 0x9E, OP_B = 0xB6, OP_BT = 0xB7,
    OP_BF = 0xB8, OP_PUSHENV = 0xBA, OP_POPENV = 0xBB, OP_PUSH = 0xC0, OP_PUSHLOC = 0xC1,
    OP_PUSHGLB = 0xC2, OP_PUSHBLTN = 0xC3, OP_CALL = 0xD9, OP_BREAK = 0xFF,
};
/* data types */
enum {
    DT_DOUBLE = 0, DT_FLOAT = 1, DT_INT32 = 2, DT_INT64 = 3, DT_BOOL = 4, DT_VAR = 5,
    DT_STRING = 6, DT_FUNCREF = 0xC /* our tag for patched function refs */, DT_INT16 = 0xF,
};
/* variable reference types */
enum { VR_ARRAY = 0x00, VR_PUSHAF = 0x10, VR_STACKTOP = 0x80, VR_POPAF = 0x90, VR_NORMAL = 0xA0, VR_INSTANCE = 0xE0 };

#define MAX_CALL_DEPTH 256

/* ------------------------------------------------------------------ */
/* error reporting                                                     */
/* ------------------------------------------------------------------ */

void vm_runtime_error(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const char *where = (R->frame && R->frame->code >= 0) ? R->dw.code[R->frame->code].name : "?";
    LOGW("[%s] %s", where, buf);
}

/* Warn once per (message key) to avoid flooding the log every frame. */
#define WARN_ONCE_SLOTS 1024
static uint32_t g_warned[WARN_ONCE_SLOTS];
static bool warn_once_check(uint32_t key) {
    uint32_t h = key * 2654435761u;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t j = (h + i) % WARN_ONCE_SLOTS;
        if (g_warned[j] == key) return false;
        if (g_warned[j] == 0) { g_warned[j] = key; return true; }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* stack                                                               */
/* ------------------------------------------------------------------ */

static inline void push(Value v) {
    if (R->sp >= R->stack_cap) {
        R->stack_cap = R->stack_cap ? R->stack_cap * 2 : 1024;
        R->stack = d3_realloc(R->stack, (size_t)R->stack_cap * sizeof(Value));
    }
    R->stack[R->sp++] = v;
}
static inline Value pop(void) {
    if (R->sp <= 0) { vm_runtime_error("stack underflow"); return v_undef(); }
    return R->stack[--R->sp];
}
static inline Value *peek(int32_t depth) { return &R->stack[R->sp - 1 - depth]; }

/* ------------------------------------------------------------------ */
/* variable access                                                     */
/* ------------------------------------------------------------------ */

typedef enum { SC_NONE, SC_OBJ, SC_LOCAL, SC_STATIC, SC_MULTI } ScopeKind;
typedef struct {
    ScopeKind kind;
    Obj *obj;
    int32_t *ids; /* SC_MULTI: instance ids */
    int32_t nids;
} Scope;

static VarMap *static_map(void) {
    int32_t c = R->frame ? R->frame->code : -1;
    if (c < 0) return NULL;
    if (!R->statics[c]) {
        R->statics[c] = d3_calloc(1, sizeof(VarMap));
        varmap_init(R->statics[c]);
    }
    return R->statics[c];
}

/* Resolves an instance designator. For writes, object designators target all
 * instances (SC_MULTI). */
static Scope scope_from_value(Value v, bool for_write) {
    Scope s = {SC_NONE, NULL, NULL, 0};
    if (v.k == V_STRUCT) { s.kind = SC_OBJ; s.obj = v.o; return s; }
    if (v.k == V_UNDEF) return s;
    int32_t n = v_to_int(v);
    switch (n) {
    case IT_SELF: case IT_BUILTIN: s.kind = SC_OBJ; s.obj = cur_self(); return s;
    case IT_OTHER: s.kind = SC_OBJ; s.obj = cur_other(); return s;
    case IT_GLOBAL: s.kind = SC_OBJ; s.obj = &R->global; return s;
    case IT_LOCAL: s.kind = SC_LOCAL; return s;
    case IT_STATIC: s.kind = SC_STATIC; return s;
    case IT_NOONE: return s;
    default: break;
    }
    if (n >= INSTANCE_ID_BASE) {
        Instance *i = instance_find_id(n);
        if (i) { s.kind = SC_OBJ; s.obj = &i->base; }
        return s;
    }
    if (n == IT_ALL || (n >= 0 && n < R->dw.nobjects)) {
        if (for_write) {
            s.nids = resolve_all(v, &s.ids);
            s.kind = SC_MULTI;
        } else {
            s.obj = resolve_single(v);
            if (s.obj) s.kind = SC_OBJ;
        }
    }
    return s;
}

static Scope scope_from_type(int32_t it, bool for_write) {
    if (it == IT_LOCAL) return (Scope){SC_LOCAL, NULL, NULL, 0};
    if (it == IT_STATIC) return (Scope){SC_STATIC, NULL, NULL, 0};
    return scope_from_value(v_int(it), for_write);
}

static Value *slot_in_obj(Obj *o, const VarEntry *ve, bool create) {
    if (!o) return NULL;
    return create ? varmap_get_or_add(&o->vars, ve->name_id) : varmap_get(&o->vars, ve->name_id);
}

/* Reads a whole variable (no array index) from a resolved object. */
static Value obj_read(Obj *o, const VarEntry *ve, int32_t index, bool indexed) {
    if (!o) return v_undef();
    if (ve->builtin >= 0 && (o->kind != OBJ_STRUCT || !varmap_get(&o->vars, ve->name_id))) {
        if (o->kind == OBJ_INSTANCE || o->kind == OBJ_GLOBAL || !bvar_is_array(ve->builtin) || true)
            return bvar_get(ve->builtin, o, indexed ? index : -1);
    }
    Value *p = slot_in_obj(o, ve, false);
    if (!p) {
        if (warn_once_check(0x10000000u ^ (uint32_t)ve->name_id))
            vm_runtime_error("variable %s not set before reading it", ve->name);
        return v_undef();
    }
    if (!indexed) return v_copy(*p);
    if (p->k != V_ARRAY) {
        if (index == 0) return v_copy(*p); /* GMS1-style scalar as array[0] */
        return v_undef();
    }
    Value *e = arr_at(p->a, index, false);
    if (!e) {
        if (warn_once_check(0x20000000u ^ (uint32_t)ve->name_id))
            vm_runtime_error("index %d out of range for %s[%d]", index, ve->name, p->a->len);
        return v_undef();
    }
    return v_copy(*e);
}

static Value *scope_slot(Scope *s, const VarEntry *ve, bool create) {
    switch (s->kind) {
    case SC_OBJ: return slot_in_obj(s->obj, ve, create);
    case SC_LOCAL:
        if (!R->frame) return NULL;
        return create ? varmap_get_or_add(&R->frame->locals, ve->name_id) : varmap_get(&R->frame->locals, ve->name_id);
    case SC_STATIC: {
        VarMap *m = static_map();
        if (!m) return NULL;
        return create ? varmap_get_or_add(m, ve->name_id) : varmap_get(m, ve->name_id);
    }
    default: return NULL;
    }
}

static Value scope_read(Scope *s, const VarEntry *ve, int32_t index, bool indexed) {
    if (s->kind == SC_OBJ) return obj_read(s->obj, ve, index, indexed);
    if (s->kind == SC_LOCAL || s->kind == SC_STATIC) {
        Value *p = scope_slot(s, ve, false);
        if (!p) return v_undef();
        if (!indexed) return v_copy(*p);
        if (p->k != V_ARRAY) return index == 0 ? v_copy(*p) : v_undef();
        Value *e = arr_at(p->a, index, false);
        return e ? v_copy(*e) : v_undef();
    }
    if (s->kind == SC_NONE && warn_once_check(0x30000000u ^ (uint32_t)ve->name_id))
        vm_runtime_error("reading %s from a non-existent instance", ve->name);
    return v_undef();
}

/* Makes sure *slot holds an array this scope may write to (copy-on-write). */
static GMArray *writable_array(Value *slot) {
    if (slot->k != V_ARRAY) {
        Value old = *slot;
        *slot = v_array_new(0);
        slot->a->owner = (int32_t)R->array_owner;
        v_release(old);
        return slot->a;
    }
    GMArray *a = slot->a;
    if (R->cow_enabled && a->rc > 1 && a->owner != (int32_t)R->array_owner) {
        Value copy = v_array_new(a->len);
        for (int32_t i = 0; i < a->len; i++) {
            v_release(copy.a->items[i]);
            copy.a->items[i] = v_copy(a->items[i]);
        }
        copy.a->owner = (int32_t)R->array_owner;
        v_release(*slot);
        *slot = copy;
        return copy.a;
    }
    return a;
}

static void obj_write(Obj *o, const VarEntry *ve, int32_t index, bool indexed, Value v) {
    if (!o) { v_release(v); return; }
    if (ve->builtin >= 0 && o->kind != OBJ_STRUCT) {
        bvar_set(ve->builtin, o, indexed ? index : -1, v);
        return;
    }
    Value *p = slot_in_obj(o, ve, true);
    if (!indexed) {
        Value old = *p;
        *p = v;
        v_release(old);
        return;
    }
    if (index < 0) { vm_runtime_error("negative array index %d for %s", index, ve->name); v_release(v); return; }
    GMArray *a = writable_array(p);
    Value *e = arr_at(a, index, true);
    Value old = *e;
    *e = v;
    v_release(old);
}

static void scope_write(Scope *s, const VarEntry *ve, int32_t index, bool indexed, Value v) {
    switch (s->kind) {
    case SC_OBJ: obj_write(s->obj, ve, index, indexed, v); return;
    case SC_LOCAL: case SC_STATIC: {
        Value *p = scope_slot(s, ve, true);
        if (!p) { v_release(v); return; }
        if (!indexed) { Value old = *p; *p = v; v_release(old); return; }
        if (index < 0) { v_release(v); return; }
        GMArray *a = writable_array(p);
        Value *e = arr_at(a, index, true);
        Value old = *e;
        *e = v;
        v_release(old);
        return;
    }
    case SC_MULTI:
        for (int32_t i = 0; i < s->nids; i++) {
            Instance *inst = instance_find_id(s->ids[i]);
            if (inst) obj_write(&inst->base, ve, index, indexed, v_copy(v));
        }
        free(s->ids);
        s->ids = NULL;
        v_release(v);
        return;
    default:
        if (warn_once_check(0x40000000u ^ (uint32_t)ve->name_id))
            vm_runtime_error("writing %s to a non-existent instance", ve->name);
        v_release(v);
        return;
    }
}

/* Public helpers for builtins (variable_instance_get etc.) */
Value var_get(Obj *target, int32_t name_id) {
    if (!target) return v_undef();
    const char *name = d3_intern_name(name_id);
    int32_t b = bvar_lookup(name);
    if (b >= 0 && target->kind != OBJ_STRUCT) return bvar_get(b, target, -1);
    Value *p = varmap_get(&target->vars, name_id);
    return p ? v_copy(*p) : v_undef();
}

void var_set(Obj *target, int32_t name_id, Value v) {
    if (!target) { v_release(v); return; }
    const char *name = d3_intern_name(name_id);
    int32_t b = bvar_lookup(name);
    if (b >= 0 && target->kind != OBJ_STRUCT) { bvar_set(b, target, -1, v); return; }
    varmap_set(&target->vars, name_id, v);
}

bool var_exists(Obj *target, int32_t name_id) {
    if (!target) return false;
    if (bvar_lookup(d3_intern_name(name_id)) >= 0 && target->kind != OBJ_STRUCT) return true;
    return varmap_get(&target->vars, name_id) != NULL;
}

/* ------------------------------------------------------------------ */
/* arguments (argument0..15, argument[], argument_count)               */
/* ------------------------------------------------------------------ */

static int32_t g_arg_ids[17];   /* intern ids of argument0..15 and "argument" */
static int32_t g_argc_id;

static int arg_number(int32_t name_id) {
    for (int i = 0; i < 17; i++)
        if (g_arg_ids[i] == name_id) return i;
    return -1;
}

static void frame_ensure_args(Frame *f, int32_t n) {
    if (n <= f->args_cap) return;
    Value *na = d3_malloc((size_t)n * sizeof(Value));
    for (int32_t i = 0; i < f->args_cap; i++) na[i] = f->args[i];
    for (int32_t i = f->args_cap; i < n; i++) na[i] = v_undef();
    free(f->args_alloc);
    f->args_alloc = na;
    f->args = na;
    f->args_cap = n;
}

/* Returns true if the variable was an argument access and handled it. */
static bool arg_read(const VarEntry *ve, int32_t index, bool indexed, Value *out) {
    if (ve->name_id == g_argc_id) {
        *out = v_real(R->frame ? R->frame->argc : 0);
        return true;
    }
    int n = arg_number(ve->name_id);
    if (n < 0 || !R->frame) return false;
    Frame *f = R->frame;
    int32_t i = (n == 16) ? (indexed ? index : 0) : n;
    if (n == 16 && !indexed) {
        Value a = v_array_new(f->argc);
        for (int32_t k = 0; k < f->argc; k++) { v_release(a.a->items[k]); a.a->items[k] = v_copy(f->args[k]); }
        *out = a;
        return true;
    }
    *out = (i >= 0 && i < f->args_cap) ? v_copy(f->args[i]) : v_undef();
    return true;
}

static bool arg_write(const VarEntry *ve, int32_t index, bool indexed, Value v) {
    int n = arg_number(ve->name_id);
    if (n < 0 || !R->frame) return false;
    Frame *f = R->frame;
    int32_t i = (n == 16) ? (indexed ? index : 0) : n;
    if (i < 0 || i > 1024) { v_release(v); return true; }
    frame_ensure_args(f, i + 1);
    v_release(f->args[i]);
    f->args[i] = v;
    if (i >= f->argc) f->argc = i + 1;
    return true;
}

/* ------------------------------------------------------------------ */
/* arithmetic                                                          */
/* ------------------------------------------------------------------ */

static inline bool is_intlike(Value v) { return v.k == V_INT32 || v.k == V_INT64 || v.k == V_BOOL; }

static Value arith(uint8_t op, Value a, Value b) {
    if (op == OP_ADD && (a.k == V_STRING || b.k == V_STRING)) {
        Value sa = v_to_string(a), sb = v_to_string(b);
        size_t la = (size_t)sa.s->len, lb = (size_t)sb.s->len;
        GMStr *s = d3_malloc(sizeof(GMStr) + la + lb + 1);
        s->rc = 1;
        s->len = (int32_t)(la + lb);
        memcpy(s->inline_data, sa.s->chars, la);
        memcpy(s->inline_data + la, sb.s->chars, lb);
        s->inline_data[la + lb] = 0;
        s->chars = s->inline_data;
        v_release(sa);
        v_release(sb);
        return v_str_own(s);
    }
    if (op == OP_MUL && a.k == V_STRING && v_is_number(b)) {
        /* "ab" * 3 is a GML error; be lenient and repeat */
        int32_t n = v_to_int(b);
        if (n < 0) n = 0;
        size_t len = (size_t)a.s->len * (size_t)n;
        GMStr *s = d3_malloc(sizeof(GMStr) + len + 1);
        s->rc = 1;
        s->len = (int32_t)len;
        for (int32_t i = 0; i < n; i++) memcpy(s->inline_data + (size_t)i * (size_t)a.s->len, a.s->chars, (size_t)a.s->len);
        s->inline_data[len] = 0;
        s->chars = s->inline_data;
        return v_str_own(s);
    }
    bool ints = is_intlike(a) && is_intlike(b);
    switch (op) {
    case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR: {
        int64_t x = v_to_int64(a), y = v_to_int64(b), r = 0;
        if (op == OP_AND) r = x & y;
        else if (op == OP_OR) r = x | y;
        else if (op == OP_XOR) r = x ^ y;
        else if (op == OP_SHL) r = (int64_t)((uint64_t)x << (y & 63));
        else r = x >> (y & 63);
        if (a.k == V_BOOL && b.k == V_BOOL && op != OP_SHL && op != OP_SHR) return v_bool(r != 0);
        if (a.k == V_INT64 || b.k == V_INT64) return v_int64(r);
        if (ints) return v_int((int32_t)r);
        return v_real((double)r);
    }
    default: break;
    }
    if (ints && (op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_REM || op == OP_MOD)) {
        int64_t x = v_to_int64(a), y = v_to_int64(b), r = 0;
        bool wide = a.k == V_INT64 || b.k == V_INT64;
        switch (op) {
        case OP_ADD: r = x + y; break;
        case OP_SUB: r = x - y; break;
        case OP_MUL: r = x * y; break;
        case OP_REM:
            if (y == 0) { vm_runtime_error("integer division by zero"); return v_real(0); }
            r = x / y; break;
        case OP_MOD:
            if (y == 0) { vm_runtime_error("modulo by zero"); return v_real(0); }
            r = x % y; break;
        }
        if (wide) return v_int64(r);
        if (r >= INT32_MIN && r <= INT32_MAX) return v_int((int32_t)r);
        return v_real((double)r);
    }
    double x = v_to_real(a), y = v_to_real(b);
    switch (op) {
    case OP_ADD: return v_real(x + y);
    case OP_SUB: return v_real(x - y);
    case OP_MUL: return v_real(x * y);
    case OP_DIV:
        if (y == 0) { vm_runtime_error("division by zero"); return v_real(0); }
        return v_real(x / y);
    case OP_REM:
        if (y == 0) { vm_runtime_error("division by zero (div)"); return v_real(0); }
        return v_real(trunc(x / y));
    case OP_MOD:
        if (y == 0) { vm_runtime_error("modulo by zero"); return v_real(0); }
        return v_real(fmod(x, y));
    default: return v_real(0);
    }
}

static bool compare(uint8_t kind, Value a, Value b) {
    if (kind == 3 || kind == 4) {
        bool eq;
        if ((a.k == V_STRING) != (b.k == V_STRING)) eq = false;
        else eq = v_equal(a, b);
        return kind == 3 ? eq : !eq;
    }
    int c = v_compare(a, b);
    switch (kind) {
    case 1: return c < 0;
    case 2: return c <= 0;
    case 5: return c >= 0;
    case 6: return c > 0;
    default: return false;
    }
}

static Value convert(Value v, uint8_t to) {
    switch (to) {
    case DT_DOUBLE: case DT_FLOAT:
        if (v.k == V_STRING || v.k == V_ARRAY || v.k == V_STRUCT || v.k == V_METHOD || v.k == V_UNDEF) return v;
        return v_real(v_to_real(v));
    case DT_INT32:
        if (v.k == V_STRING || v.k == V_ARRAY || v.k == V_STRUCT || v.k == V_METHOD || v.k == V_UNDEF) return v;
        if (v.k == V_REAL) {
            /* GML truncates-by-rounding on conv.v.i for indices: use rounding */
            return v_int(v_to_int(v));
        }
        if (v.k == V_INT64) return v_int((int32_t)v.l);
        return v_int(v.i);
    case DT_INT64:
        if (!v_is_number(v)) return v;
        return v_int64(v_to_int64(v));
    case DT_BOOL:
        if (v.k == V_STRING || v.k == V_ARRAY || v.k == V_STRUCT || v.k == V_METHOD) return v;
        return v_bool(v_to_bool(v));
    default: return v;
    }
}

/* ------------------------------------------------------------------ */
/* calls                                                               */
/* ------------------------------------------------------------------ */

static Value vm_exec(Frame *f);

Value vm_call_code(int32_t code, Obj *self, Obj *other, int32_t argc, Value *args) {
    if (code < 0 || code >= R->dw.ncode || !R->dw.code[code].start) return v_undef();
    if (R->call_depth >= MAX_CALL_DEPTH) {
        vm_runtime_error("call stack overflow calling %s", R->dw.code[code].name);
        return v_undef();
    }
    Value argbuf[16];
    Frame f;
    memset(&f, 0, sizeof f);
    f.code = code;
    f.self = self;
    f.other = other;
    int32_t cap = argc > 16 ? argc : 16;
    f.args = cap > 16 ? d3_malloc((size_t)cap * sizeof(Value)) : argbuf;
    f.args_alloc = cap > 16 ? f.args : NULL;
    f.args_cap = cap;
    for (int32_t i = 0; i < cap; i++) f.args[i] = i < argc ? v_copy(args[i]) : v_undef();
    f.argc = argc;
    varmap_init(&f.locals);
    f.parent = R->frame;

    obj_retain(self);
    obj_retain(other);
    R->frame = &f;
    R->call_depth++;
    Value ret = vm_exec(&f);
    R->call_depth--;
    R->frame = f.parent;
    obj_release(self);
    obj_release(other);

    for (int32_t i = 0; i < f.args_cap; i++) v_release(f.args[i]);
    free(f.args_alloc);
    varmap_free(&f.locals);
    return ret;
}

Value vm_call_method(Value fn, Obj *self, Obj *other, int32_t argc, Value *args) {
    if (fn.k == V_METHOD) {
        Obj *s = self;
        if (fn.m->bound.k == V_STRUCT) s = fn.m->bound.o;
        else if (fn.m->bound.k != V_UNDEF) {
            Obj *b = resolve_single(fn.m->bound);
            if (b) s = b;
        }
        if (fn.m->func >= 0) return vm_call_code(fn.m->func, s, s == self ? other : self, argc, args);
        BuiltinFn bf = builtin_fn(-fn.m->func - 1);
        if (!bf) return v_undef();
        Frame *saved = R->frame;
        Frame tmp = saved ? *saved : (Frame){0};
        tmp.self = s;
        tmp.other = self;
        R->frame = &tmp;
        Value r = bf(argc, args);
        R->frame = saved;
        return r;
    }
    if (v_is_number(fn)) {
        /* script index (GMS1 style script_execute(scr_x)) */
        int32_t idx = v_to_int(fn);
        if (idx >= 0 && idx < R->dw.nscripts) return vm_call_code(R->dw.scripts[idx].code, self, other, argc, args);
    }
    vm_runtime_error("trying to call a %s value as a function", v_kind_name(fn));
    return v_undef();
}

Value vm_call_function_index(int32_t func, Obj *self, Obj *other, int32_t argc, Value *args) {
    const FuncEntry *fe = &R->dw.funcs[func];
    if (fe->code >= 0) return vm_call_code(fe->code, self, other, argc, args);
    if (fe->builtin >= 0) {
        BuiltinFn bf = builtin_fn(fe->builtin);
        return bf(argc, args);
    }
    /* maybe a global variable holding a method */
    Value *gv = varmap_get(&R->global.vars, d3_intern(fe->name));
    if (gv && gv->k == V_METHOD) return vm_call_method(*gv, self, other, argc, args);
    if (warn_once_check(0x50000000u ^ (uint32_t)func))
        LOGW("unimplemented function: %s (returns undefined)", fe->name);
    return v_undef();
}

int32_t vm_code_for_script_name(const char *name) {
    char buf[300];
    snprintf(buf, sizeof buf, "gml_Script_%s", name);
    int id = d3_intern(buf);
    if (id < R->script_code_by_name_cap && R->script_code_by_name[id] >= 0) return R->script_code_by_name[id];
    return -1;
}

void vm_link(void) {
    DataWin *dw = &R->dw;
    /* argument names */
    char buf[32];
    for (int i = 0; i < 16; i++) {
        snprintf(buf, sizeof buf, "argument%d", i);
        g_arg_ids[i] = d3_intern(buf);
    }
    g_arg_ids[16] = d3_intern("argument");
    g_argc_id = d3_intern("argument_count");

    /* code name lookup table */
    int cap = d3_intern_count() + 1;
    R->script_code_by_name = d3_malloc((size_t)cap * sizeof(int32_t));
    for (int i = 0; i < cap; i++) R->script_code_by_name[i] = -1;
    R->script_code_by_name_cap = cap;
    for (int32_t i = 0; i < dw->ncode; i++)
        if (dw->code[i].name_id < cap && R->script_code_by_name[dw->code[i].name_id] < 0)
            R->script_code_by_name[dw->code[i].name_id] = i;

    int unresolved = 0;
    for (int32_t i = 0; i < dw->nfuncs; i++) {
        FuncEntry *fe = &dw->funcs[i];
        fe->code = vm_code_for_script_name(fe->name);
        if (fe->code < 0) {
            /* anonymous functions / struct constructors use their full code name */
            int id = d3_intern(fe->name);
            if (id < cap && R->script_code_by_name[id] >= 0) fe->code = R->script_code_by_name[id];
        }
        if (fe->code < 0) fe->builtin = builtin_lookup(fe->name);
        if (fe->code < 0 && fe->builtin < 0) {
            unresolved++;
            LOGD("unresolved function %s", fe->name);
        }
    }
    for (int32_t i = 0; i < dw->nvars; i++) {
        VarEntry *ve = &dw->vars[i];
        if (ve->inst_type == IT_LOCAL) continue;
        ve->builtin = bvar_lookup(ve->name);
    }
    LOGI("Linked %d functions (%d not implemented yet), %d variables", dw->nfuncs, unresolved, dw->nvars);
}

/* ------------------------------------------------------------------ */
/* with-statement environments                                         */
/* ------------------------------------------------------------------ */

static bool env_push(Value target) {
    if (R->nenvs == R->cap_envs) {
        R->cap_envs = R->cap_envs ? R->cap_envs * 2 : 16;
        R->envs = d3_realloc(R->envs, (size_t)R->cap_envs * sizeof(WithEnv));
    }
    WithEnv *e = &R->envs[R->nenvs];
    memset(e, 0, sizeof *e);
    e->saved_self = R->frame->self;
    e->saved_other = R->frame->other;
    Obj *first = NULL;
    if (target.k == V_STRUCT) {
        e->struct_target = target.o;
        obj_retain(target.o);
        e->count = 1;
        first = target.o;
    } else {
        int32_t n = v_to_int(target);
        if (n == IT_SELF || n == IT_OTHER || n == IT_GLOBAL) {
            Scope s = scope_from_value(target, false);
            first = s.obj;
            if (first) {
                e->struct_target = first;
                obj_retain(first);
                e->count = 1;
            }
        } else {
            e->count = resolve_all(target, &e->targets);
            for (int32_t i = 0; i < e->count; i++) {
                Instance *in = instance_find_id(e->targets[i]);
                if (in && !in->destroyed) { first = &in->base; e->pos = i; break; }
            }
        }
    }
    if (!first) {
        free(e->targets);
        if (e->struct_target) obj_release(e->struct_target);
        return false;
    }
    R->nenvs++;
    R->frame->other = R->frame->self;
    R->frame->self = first;
    return true;
}

/* Advances to the next target; returns false when finished (env popped). */
static bool env_next(bool force_end) {
    if (R->nenvs <= 0) return false;
    WithEnv *e = &R->envs[R->nenvs - 1];
    if (!force_end && !e->struct_target) {
        for (int32_t i = e->pos + 1; i < e->count; i++) {
            Instance *in = instance_find_id(e->targets[i]);
            if (in && !in->destroyed) {
                e->pos = i;
                R->frame->self = &in->base;
                return true;
            }
        }
    }
    R->frame->self = e->saved_self;
    R->frame->other = e->saved_other;
    free(e->targets);
    if (e->struct_target) obj_release(e->struct_target);
    R->nenvs--;
    return false;
}

/* ------------------------------------------------------------------ */
/* the interpreter loop                                                */
/* ------------------------------------------------------------------ */

static inline int32_t branch_offset(uint32_t w) {
    int32_t off = (int32_t)(w & 0x7FFFFF);
    if (off & 0x400000) off |= ~0x7FFFFF; /* sign extend 23 bits */
    return off;
}

static Value make_func_value(int32_t func) {
    const FuncEntry *fe = &R->dw.funcs[func];
    if (fe->code >= 0) return v_method(fe->code, v_undef());
    if (fe->builtin >= 0) return v_method(-fe->builtin - 1, v_undef());
    return v_real(func);
}

static Value do_call(int32_t func, int32_t argc) {
    Value argbuf[16];
    Value *args = argc > 16 ? d3_malloc((size_t)argc * sizeof(Value)) : argbuf;
    for (int32_t i = 0; i < argc; i++) args[i] = pop();
    Value r = vm_call_function_index(func, R->frame->self, R->frame->other, argc, args);
    for (int32_t i = 0; i < argc; i++) v_release(args[i]);
    if (args != argbuf) free(args);
    return r;
}

static Value vm_exec(Frame *f) {
    const CodeEntry *ce = &R->dw.code[f->code];
    const uint32_t *pc = ce->start;
    const uint32_t *end = ce->end;
    const DataWin *dw = &R->dw;
    int32_t base_sp = R->sp;
    int32_t base_env = R->nenvs;
    Value ret = v_undef();

    while (pc < end) {
        uint32_t w = pc[0];
        uint8_t op = (uint8_t)(w >> 24);
        uint8_t t1 = (uint8_t)((w >> 16) & 0xF);
        uint8_t t2 = (uint8_t)((w >> 20) & 0xF);

        switch (op) {
        case OP_CONV: {
            Value v = pop();
            push(convert(v, t2));
            pc++;
            break;
        }
        case OP_MUL: case OP_DIV: case OP_REM: case OP_MOD: case OP_ADD: case OP_SUB:
        case OP_AND: case OP_OR: case OP_XOR: case OP_SHL: case OP_SHR: {
            Value b = pop(), a = pop();
            push(arith(op, a, b));
            v_release(a);
            v_release(b);
            pc++;
            break;
        }
        case OP_NEG: {
            Value a = pop();
            if (a.k == V_INT32) push(v_int(-a.i));
            else if (a.k == V_INT64) push(v_int64(-a.l));
            else push(v_real(-v_to_real(a)));
            v_release(a);
            pc++;
            break;
        }
        case OP_NOT: {
            Value a = pop();
            if (t1 == DT_BOOL || a.k == V_BOOL) push(v_bool(!v_to_bool(a)));
            else if (a.k == V_INT64) push(v_int64(~a.l));
            else push(v_int(~v_to_int(a)));
            v_release(a);
            pc++;
            break;
        }
        case OP_CMP: {
            Value b = pop(), a = pop();
            push(v_bool(compare((uint8_t)((w >> 8) & 0xFF), a, b)));
            v_release(a);
            v_release(b);
            pc++;
            break;
        }
        case OP_POP: {
            if (t1 == DT_INT16) {
                /* pop swap: move the top value below the next (n) entries */
                int32_t n = (int32_t)(w & 0xFFFF) - 3;
                if (n < 1) n = 1;
                if (R->sp - base_sp > n) {
                    Value top = R->stack[R->sp - 1];
                    memmove(&R->stack[R->sp - n], &R->stack[R->sp - n - 1], (size_t)n * sizeof(Value));
                    R->stack[R->sp - n - 1] = top;
                }
                pc++;
                break;
            }
            uint32_t ref = pc[1];
            uint8_t rt = (uint8_t)((ref >> 24) & 0xF8);
            const VarEntry *ve = &dw->vars[ref & 0x07FFFFFF];
            int32_t it = (int16_t)(w & 0xFFFF);
            if (rt == VR_ARRAY) {
                Value idx = pop(), inst = pop(), val = pop();
                int32_t index = v_to_int(idx);
                if (!arg_write(ve, index, true, val)) {
                    Scope s = (it == IT_LOCAL || it == IT_STATIC || it == IT_GLOBAL)
                                  ? scope_from_type(it, true) : scope_from_value(inst, true);
                    scope_write(&s, ve, index, true, val);
                }
                v_release(idx);
                v_release(inst);
            } else if (rt == VR_STACKTOP) {
                Value inst = pop(), val = pop();
                Scope s = scope_from_value(inst, true);
                scope_write(&s, ve, 0, false, val);
                v_release(inst);
            } else if (rt == VR_INSTANCE) {
                Value val = pop();
                Scope s = scope_from_value(v_int(it + INSTANCE_ID_BASE), true);
                scope_write(&s, ve, 0, false, val);
            } else {
                Value val = pop();
                if (!arg_write(ve, 0, false, val)) {
                    Scope s = scope_from_type(it, true);
                    scope_write(&s, ve, 0, false, val);
                }
            }
            pc += 2;
            break;
        }
        case OP_DUP: {
            int32_t n = (int32_t)(w & 0xFF);
            int32_t n2 = (int32_t)((w >> 8) & 0xFF);
            if (n2 != 0 && t1 != DT_INT16) {
                /* "dup swap" form (GMS 2.3 struct/array assignments):
                 * move the top n items below the following n2 items. */
                if (R->sp - base_sp >= n + n2 && n > 0) {
                    Value tmp[16];
                    if (n > 16) n = 16;
                    memcpy(tmp, &R->stack[R->sp - n], (size_t)n * sizeof(Value));
                    memmove(&R->stack[R->sp - n2], &R->stack[R->sp - n - n2], (size_t)n2 * sizeof(Value));
                    memcpy(&R->stack[R->sp - n - n2], tmp, (size_t)n * sizeof(Value));
                }
            } else {
                int32_t count = n + 1;
                if (R->sp - base_sp < count) count = R->sp - base_sp;
                int32_t start = R->sp - count;
                for (int32_t i = 0; i < count; i++) push(v_copy(R->stack[start + i]));
            }
            pc++;
            break;
        }
        case OP_RET: {
            ret = pop();
            goto done;
        }
        case OP_EXIT:
            goto done;
        case OP_POPZ: {
            Value v = pop();
            v_release(v);
            pc++;
            break;
        }
        case OP_B:
            pc += branch_offset(w);
            break;
        case OP_BT: {
            Value v = pop();
            bool b = v_to_bool(v);
            v_release(v);
            pc += b ? branch_offset(w) : 1;
            break;
        }
        case OP_BF: {
            Value v = pop();
            bool b = v_to_bool(v);
            v_release(v);
            pc += b ? 1 : branch_offset(w);
            break;
        }
        case OP_PUSHENV: {
            Value target = pop();
            bool any = env_push(target);
            v_release(target);
            if (any) pc++;
            else {
                pc += branch_offset(w);
                /* if the jump lands on the matching popenv, skip it too */
                if (pc < end && (uint8_t)(pc[0] >> 24) == OP_POPENV) pc++;
            }
            break;
        }
        case OP_POPENV: {
            if ((w & 0xFFFFFF) == 0xF00000) { /* break out of a with (before exit/return) */
                env_next(true);
                pc++;
                break;
            }
            if (env_next(false)) pc += branch_offset(w);
            else pc++;
            break;
        }
        case OP_PUSHI: {
            push(v_int((int16_t)(w & 0xFFFF)));
            pc++;
            break;
        }
        case OP_PUSH: case OP_PUSHLOC: case OP_PUSHGLB: case OP_PUSHBLTN: {
            uint8_t t = (uint8_t)((w >> 16) & 0xFF);
            if (op == OP_PUSH && t != DT_VAR) {
                switch (t) {
                case DT_DOUBLE: push(v_real(rd_f64((const uint8_t *)(pc + 1)))); pc += 3; break;
                case DT_FLOAT: push(v_real(rd_f32((const uint8_t *)(pc + 1)))); pc += 2; break;
                case DT_INT32: push(v_int((int32_t)pc[1])); pc += 2; break;
                case DT_INT64: push(v_int64(rd_i64((const uint8_t *)(pc + 1)))); pc += 3; break;
                case DT_BOOL: push(v_bool(pc[1] != 0)); pc += 2; break;
                case DT_STRING: {
                    uint32_t si = pc[1];
                    if (si < (uint32_t)dw->nstrings) push(v_str_own(dw->strings[si]));
                    else push(v_str(""));
                    pc += 2;
                    break;
                }
                case DT_FUNCREF: push(make_func_value((int32_t)pc[1])); pc += 2; break;
                case DT_INT16: push(v_int((int16_t)(w & 0xFFFF))); pc++; break;
                default:
                    vm_runtime_error("unknown push type %u", t);
                    pc++;
                    break;
                }
                break;
            }
            uint32_t ref = pc[1];
            uint8_t rt = (uint8_t)((ref >> 24) & 0xF8);
            const VarEntry *ve = &dw->vars[ref & 0x07FFFFFF];
            int32_t it = (int16_t)(w & 0xFFFF);
            if (op == OP_PUSHLOC) it = IT_LOCAL;
            else if (op == OP_PUSHGLB) it = IT_GLOBAL;
            Value out;
            if (rt == VR_ARRAY || rt == VR_PUSHAF || rt == VR_POPAF) {
                Value idx = pop(), inst = pop();
                int32_t index = v_to_int(idx);
                if (rt == VR_POPAF) {
                    /* get (or create) the nested array for a following popaf */
                    Scope s = (it == IT_LOCAL || it == IT_STATIC || it == IT_GLOBAL)
                                  ? scope_from_type(it, false) : scope_from_value(inst, false);
                    Value *slot = scope_slot(&s, ve, true);
                    if (slot && index >= 0) {
                        GMArray *a = writable_array(slot);
                        Value *e = arr_at(a, index, true);
                        if (e->k != V_ARRAY) { v_release(*e); *e = v_array_new(0); }
                        out = v_copy(*e);
                    } else out = v_array_new(0);
                } else if (!arg_read(ve, index, true, &out)) {
                    Scope s = (it == IT_LOCAL || it == IT_STATIC || it == IT_GLOBAL)
                                  ? scope_from_type(it, false) : scope_from_value(inst, false);
                    out = scope_read(&s, ve, index, true);
                }
                v_release(idx);
                v_release(inst);
            } else if (rt == VR_STACKTOP) {
                Value inst = pop();
                Scope s = scope_from_value(inst, false);
                out = scope_read(&s, ve, 0, false);
                v_release(inst);
            } else if (rt == VR_INSTANCE) {
                Scope s = scope_from_value(v_int(it + INSTANCE_ID_BASE), false);
                out = scope_read(&s, ve, 0, false);
            } else {
                if (!arg_read(ve, 0, false, &out)) {
                    Scope s = scope_from_type(it, false);
                    out = scope_read(&s, ve, 0, false);
                }
            }
            push(out);
            pc += 2;
            break;
        }
        case OP_CALL: {
            int32_t argc = (int32_t)(w & 0xFFFF);
            Value r = do_call((int32_t)pc[1], argc);
            push(r);
            pc += 2;
            break;
        }
        case OP_CALLV: {
            int32_t argc = (int32_t)(w & 0xFF);
            Value fn = pop();
            Value selfv = pop();
            Value argbuf[16];
            Value *args = argc > 16 ? d3_malloc((size_t)argc * sizeof(Value)) : argbuf;
            for (int32_t i = 0; i < argc; i++) args[i] = pop();
            Scope s = scope_from_value(selfv, false);
            Obj *self = s.kind == SC_OBJ && s.obj ? s.obj : R->frame->self;
            Value r = vm_call_method(fn, self, R->frame->self, argc, args);
            for (int32_t i = 0; i < argc; i++) v_release(args[i]);
            if (args != argbuf) free(args);
            v_release(fn);
            v_release(selfv);
            push(r);
            pc++;
            break;
        }
        case OP_BREAK: {
            int16_t sub = (int16_t)(w & 0xFFFF);
            uint8_t t = (uint8_t)((w >> 16) & 0xFF);
            int words = (t == DT_INT32 || t == DT_FUNCREF) ? 2 : 1;
            switch (sub) {
            case -1: /* chkindex */
                break;
            case -2: { /* pushaf: arr[idx] */
                Value idx = pop(), arr = pop();
                Value out = v_undef();
                if (arr.k == V_ARRAY) {
                    Value *e = arr_at(arr.a, v_to_int(idx), false);
                    if (e) out = v_copy(*e);
                }
                push(out);
                v_release(idx);
                v_release(arr);
                break;
            }
            case -3: { /* popaf: arr[idx] = value */
                Value idx = pop(), arr = pop(), val = pop();
                int32_t i = v_to_int(idx);
                if (arr.k == V_ARRAY && i >= 0) {
                    Value *e = arr_at(arr.a, i, true);
                    Value old = *e;
                    *e = val;
                    v_release(old);
                } else v_release(val);
                v_release(idx);
                v_release(arr);
                break;
            }
            case -4: { /* pushac: get nested array (creating it) */
                Value idx = pop(), arr = pop();
                Value out = v_array_new(0);
                int32_t i = v_to_int(idx);
                if (arr.k == V_ARRAY && i >= 0) {
                    Value *e = arr_at(arr.a, i, true);
                    if (e->k != V_ARRAY) { v_release(*e); *e = v_array_new(0); }
                    v_release(out);
                    out = v_copy(*e);
                }
                push(out);
                v_release(idx);
                v_release(arr);
                break;
            }
            case -5: { /* setowner */
                Value v = pop();
                R->array_owner = v_to_int64(v);
                R->cow_enabled = true;
                v_release(v);
                break;
            }
            case -6: /* isstaticok */
                push(v_bool(R->statics_init[f->code]));
                break;
            case -7: /* setstatic */
                R->statics_init[f->code] = true;
                break;
            case -8: { /* savearef: remember array + index for a compound assignment */
                v_release(R->saved_aref[0]);
                v_release(R->saved_aref[1]);
                R->saved_aref[1] = pop();
                R->saved_aref[0] = pop();
                break;
            }
            case -9: /* restorearef */
                push(R->saved_aref[0]);
                push(R->saved_aref[1]);
                R->saved_aref[0] = v_undef();
                R->saved_aref[1] = v_undef();
                break;
            case -10: { /* chknullish: push whether top is undefined (keeps value) */
                Value *top = peek(0);
                push(v_bool(top->k == V_UNDEF));
                break;
            }
            case -11: { /* pushref: asset reference */
                int32_t arg = (int32_t)pc[1];
                if (t == DT_FUNCREF) push(make_func_value(arg));
                else push(v_real(arg & 0xFFFFFF));
                break;
            }
            default:
                if (warn_once_check(0x60000000u ^ (uint16_t)sub))
                    vm_runtime_error("unknown break opcode %d", sub);
                break;
            }
            pc += words;
            break;
        }
        default:
            vm_runtime_error("unknown opcode 0x%02X at word %ld", op, (long)(pc - ce->start));
            goto done;
        }
    }

done:
    /* unwind with-environments and leftover stack values */
    while (R->nenvs > base_env) env_next(true);
    while (R->sp > base_sp) v_release(R->stack[--R->sp]);
    return ret;
}
