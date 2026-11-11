/*
 * GML runtime values: reals, strings, arrays, structs, methods.
 * Heap values are reference counted. Constant strings from the STRG chunk
 * are "immortal" (refcount < 0) and never freed.
 */
#ifndef D3_VALUE_H
#define D3_VALUE_H

#include "common.h"

typedef enum {
    V_UNDEF = 0,
    V_REAL,
    V_INT32,
    V_INT64,
    V_BOOL,
    V_STRING,
    V_ARRAY,
    V_STRUCT, /* struct or instance used as a value (instances are normally ids) */
    V_METHOD,
    V_PTR,
} VKind;

typedef struct GMStr GMStr;
typedef struct GMArray GMArray;
typedef struct Obj Obj;
typedef struct GMMethod GMMethod;

typedef struct Value {
    uint8_t k;
    union {
        double r;
        int32_t i;
        int64_t l;
        GMStr *s;
        GMArray *a;
        Obj *o;
        GMMethod *m;
        void *p;
    };
} Value;

struct GMStr {
    int32_t rc; /* < 0: immortal */
    int32_t len;
    const char *chars; /* NUL terminated */
    char inline_data[];
};

struct GMArray {
    int32_t rc;
    int32_t len, cap;
    int32_t owner; /* copy-on-write owner token */
    Value *items;
};

/* Variable storage: name id -> value. Open addressing hash map. */
typedef struct VarSlot { int32_t key; Value v; } VarSlot;
typedef struct VarMap {
    VarSlot *slots;
    uint32_t cap, count;
} VarMap;

/* Common header of instances and structs: both can be "self". */
typedef enum { OBJ_STRUCT = 0, OBJ_INSTANCE = 1, OBJ_GLOBAL = 2 } ObjKind;
struct Obj {
    int32_t rc;
    uint8_t kind;
    VarMap vars;
};

struct GMMethod {
    int32_t rc;
    int32_t func;  /* >= 0: code entry index, < 0: -(builtin index) - 1 */
    Value bound;   /* bound self (instance id real, struct) or undefined */
};

/* ---- constructors ---- */
static inline Value v_undef(void) { Value v; v.k = V_UNDEF; v.l = 0; return v; }
static inline Value v_real(double d) { Value v; v.k = V_REAL; v.r = d; return v; }
static inline Value v_int(int32_t i) { Value v; v.k = V_INT32; v.l = 0; v.i = i; return v; }
static inline Value v_int64(int64_t l) { Value v; v.k = V_INT64; v.l = l; return v; }
static inline Value v_bool(bool b) { Value v; v.k = V_BOOL; v.l = 0; v.i = b ? 1 : 0; return v; }

Value v_str(const char *s);           /* copies */
Value v_strn(const char *s, size_t n); /* copies */
Value v_str_own(GMStr *s);            /* takes ownership of one reference */
GMStr *gmstr_new(const char *s, size_t n);
GMStr *gmstr_static(const char *s, size_t n); /* immortal, does not copy */
Value v_array_new(int32_t len);
Value v_struct_new(void);
Value v_method(int32_t func, Value bound);

/* ---- refcounting ---- */
void v_retain(Value v);
void v_release(Value v);
static inline Value v_copy(Value v) { v_retain(v); return v; }

/* ---- conversion ---- */
double v_to_real(Value v);
int32_t v_to_int(Value v);
int64_t v_to_int64(Value v);
bool v_to_bool(Value v);
/* Converts to a new string value (always returns a V_STRING with its own ref). */
Value v_to_string(Value v);
/* Writes the GML string form into buf. */
void v_format(Value v, char *buf, size_t n);
static inline bool v_is_number(Value v) {
    return v.k == V_REAL || v.k == V_INT32 || v.k == V_INT64 || v.k == V_BOOL;
}
const char *v_kind_name(Value v);

/* Equality / ordering the way GML compares (numbers with epsilon, strings lexically). */
bool v_equal(Value a, Value b);
int v_compare(Value a, Value b); /* <0, 0, >0 */
extern double g_math_epsilon;

/* ---- arrays ---- */
Value *arr_at(GMArray *a, int32_t idx, bool grow); /* NULL if out of range and !grow */
void arr_resize(GMArray *a, int32_t len);
void arr_push(GMArray *a, Value v); /* takes ownership */

/* ---- variable maps ---- */
void varmap_init(VarMap *m);
void varmap_free(VarMap *m);
Value *varmap_get(VarMap *m, int32_t key);              /* NULL if absent */
Value *varmap_get_or_add(VarMap *m, int32_t key);       /* creates undefined */
void varmap_set(VarMap *m, int32_t key, Value v);       /* takes ownership */
bool varmap_remove(VarMap *m, int32_t key);

/* ---- objects ---- */
Obj *obj_new_struct(void);
void obj_retain(Obj *o);
void obj_release(Obj *o);

#endif
