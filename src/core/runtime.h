/*
 * Game runtime: instances, rooms, events, views and the interpreter state.
 */
#ifndef D3_RUNTIME_H
#define D3_RUNTIME_H

#include "common.h"
#include "value.h"
#include "datawin.h"

#define MAX_ALARMS 12
#define MAX_VIEWS 8
#define INSTANCE_ID_BASE 100000

/* Instance types used by the bytecode */
enum {
    IT_SELF = -1, IT_OTHER = -2, IT_ALL = -3, IT_NOONE = -4, IT_GLOBAL = -5,
    IT_BUILTIN = -6, IT_LOCAL = -7, IT_STACKTOP = -9, IT_ARG = -15, IT_STATIC = -16,
};

typedef struct Instance {
    Obj base; /* kind == OBJ_INSTANCE; must be first */
    int32_t id;
    int32_t object_index;
    bool destroyed, active, visible, solid, persistent;
    bool created; /* create event finished */
    double x, y, xprevious, yprevious, xstart, ystart;
    double hspeed, vspeed, speed, direction, friction, gravity, gravity_direction;
    int32_t sprite_index, mask_index;
    double image_index, image_speed, image_xscale, image_yscale, image_angle, image_alpha;
    uint32_t image_blend;
    double depth;
    int32_t layer;
    double alarm[MAX_ALARMS];
    uint32_t create_order;
} Instance;

typedef struct {
    bool enabled, visible;
    double x, y, w, h;           /* camera view rect (room space) */
    double port_x, port_y, port_w, port_h; /* where it goes in the window */
    double angle;
    int32_t follow;              /* object index or instance id to follow */
    double border_x, border_y, speed_x, speed_y;
} View;

/* A run-time room layer (from room data or created by layer_create). */
typedef struct {
    int32_t id;
    char name[64];
    int32_t depth;
    bool visible;
    float x, y, hspeed, vspeed;
    uint32_t type;
    const RoomLayer *src; /* NULL for layers created at run time */
    /* background state, can be changed by layer_background_* */
    int32_t bg_sprite;
    uint32_t bg_color;
    float bg_index, bg_speed, bg_xscale, bg_yscale;
    bool bg_visible, bg_htiled, bg_vtiled, bg_stretch;
} Layer;

/* The VM call frame. */
typedef struct Frame {
    int32_t code;          /* code entry index */
    Obj *self, *other;
    Value *args;
    Value *args_alloc; /* heap buffer owned by the frame, or NULL */
    int32_t argc, args_cap;
    VarMap locals;
    struct Frame *parent;
} Frame;

/* Environment stack for `with` */
typedef struct {
    Obj *saved_self, *saved_other;
    int32_t *targets; /* instance ids, or -1 for struct target */
    Obj *struct_target;
    int32_t count, pos;
} WithEnv;

typedef struct Runtime {
    DataWin dw;
    Obj global;

    /* instances */
    Instance **insts;
    int32_t ninsts, cap_insts;
    int32_t next_instance_id;
    uint32_t create_counter;
    /* id -> instance hash (open addressing) */
    int32_t *id_keys;
    Instance **id_vals;
    uint32_t id_cap, id_count;

    /* object hierarchy: ancestors[obj] = list ending in -1 */
    int32_t **ancestors;

    /* room state */
    int32_t room, next_room;
    bool room_change_pending, game_restart_pending, game_end_pending;
    bool first_room_started;
    double room_speed;
    uint32_t room_width, room_height;
    uint32_t bg_color;
    bool draw_bg_color;
    View views[MAX_VIEWS];
    bool views_enabled;
    int32_t view_current;
    Layer *layers;
    int32_t nlayers, cap_layers, next_layer_id;

    /* window / gui */
    int32_t window_w, window_h;
    double gui_w, gui_h; /* 0 = window size */

    /* draw state */
    uint32_t draw_color;
    double draw_alpha;
    int32_t draw_font;
    int32_t halign, valign;
    int32_t blend_mode;
    bool in_gui;
    double cur_view_x, cur_view_y, cur_view_sx, cur_view_sy, cur_port_x, cur_port_y;

    /* input */
    uint8_t keys[256], keys_prev[256];
    int32_t last_key;
    bool any_key, any_key_prev;

    /* timing */
    uint64_t start_us;
    double fps, fps_real;
    uint64_t frame_count;
    double delta_time;

    /* static variables per code entry (lazily allocated) */
    VarMap **statics;
    bool *statics_init;

    /* interpreter */
    Value *stack;
    int32_t sp, stack_cap;
    Frame *frame;
    WithEnv *envs;
    int32_t nenvs, cap_envs;
    int64_t array_owner;
    bool cow_enabled;
    int32_t call_depth;
    Value saved_aref[2];
    /* current event, for event_inherited / event_perform */
    int32_t ev_type, ev_sub, ev_object;
    int32_t code_by_script_name_ready;

    /* builtin function lookup tables */
    int32_t *script_code_by_name; /* intern id -> code index, -1 */
    int32_t script_code_by_name_cap;

    /* misc */
    double random_seed;
    char error[512];
} Runtime;

extern Runtime *R;

/* ---- lifecycle ---- */
bool runtime_init(const char *datawin_path, char *err, size_t errlen);
void runtime_shutdown(void);
/* Runs one game frame (logic + draw). Returns false when the game ends. */
bool runtime_frame(void);
void runtime_draw(void);

/* ---- instances ---- */
Instance *instance_create(double x, double y, int32_t obj, double depth, bool run_create);
void instance_destroy(Instance *inst, bool run_event);
Instance *instance_find_id(int32_t id);
bool object_is_ancestor(int32_t obj, int32_t ancestor);
/* Resolves a GML instance designator (id, object index, -1, -2, -3) to the
 * first matching live instance (or struct for struct values). */
Obj *resolve_single(Value target);
/* Iterates all live instances matching an object index / id / keyword.
 * Returns count; ids written to out (caller frees). */
int32_t resolve_all(Value target, int32_t **out);
int32_t instance_count_of(int32_t obj);

/* ---- events ---- */
/* Runs an object's event (searching parents). Returns true if a handler ran. */
bool event_run(Instance *inst, int32_t type, int32_t sub, Obj *other);
bool event_run_inherited(Instance *inst, Obj *other);
int32_t event_find_code(int32_t obj, int32_t type, int32_t sub, int32_t *owner);

/* ---- rooms ---- */
void room_goto(int32_t room);
int32_t room_find_by_order(int32_t room, int32_t delta);

/* ---- vm ---- */
Value vm_call_code(int32_t code, Obj *self, Obj *other, int32_t argc, Value *args);
Value vm_call_method(Value fn, Obj *self, Obj *other, int32_t argc, Value *args);
Value vm_call_function_index(int32_t func, Obj *self, Obj *other, int32_t argc, Value *args);
void vm_runtime_error(const char *fmt, ...);
void vm_link(void); /* resolves FUNC/VARI entries to builtins */
int32_t vm_code_for_script_name(const char *name);

/* ---- variables (used by builtins) ---- */
Value var_get(Obj *target, int32_t name_id);
void var_set(Obj *target, int32_t name_id, Value v); /* takes ownership */
bool var_exists(Obj *target, int32_t name_id);

/* ---- builtin variables / functions (builtins*.c) ---- */
int32_t bvar_lookup(const char *name);
Value bvar_get(int32_t id, Obj *self, int32_t index);
void bvar_set(int32_t id, Obj *self, int32_t index, Value v);
bool bvar_is_array(int32_t id);

typedef Value (*BuiltinFn)(int32_t argc, Value *args);
typedef struct { const char *name; BuiltinFn fn; } BuiltinDef;
int32_t builtin_lookup(const char *name);
BuiltinFn builtin_fn(int32_t index);
const char *builtin_name(int32_t index);
void builtins_register_all(void);
void builtin_register(const char *name, BuiltinFn fn);

/* Convenience for builtins */
static inline Obj *cur_self(void) { return R->frame ? R->frame->self : NULL; }
static inline Obj *cur_other(void) { return R->frame ? R->frame->other : NULL; }
static inline Instance *as_instance(Obj *o) { return (o && o->kind == OBJ_INSTANCE) ? (Instance *)o : NULL; }
static inline double ARGR(int32_t argc, Value *a, int32_t i, double def) { return i < argc ? v_to_real(a[i]) : def; }
static inline int32_t ARGI(int32_t argc, Value *a, int32_t i, int32_t def) { return i < argc ? v_to_int(a[i]) : def; }
const char *ARGS(int32_t argc, Value *a, int32_t i); /* temp string (valid until next call) */

/* Draw helpers (builtins_draw.c) */
void draw_sprite_frame(int32_t sprite, double frame, double x, double y, double xs, double ys, double angle,
                       uint32_t color, double alpha);
void draw_init_state(void);
void draw_layers_and_instances(void);
void draw_set_view_transform(int32_t view);
void draw_set_gui_transform(void);

/* Audio helpers (builtins_audio.c) */
void audio_update(void);
void audio_shutdown(void);
void audio_init_groups(void);

/* Collision helpers (builtins_inst.c) */
bool instance_bbox(Instance *inst, double x, double y, double *l, double *t, double *r, double *b);

#endif
