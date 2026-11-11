/*
 * Instance, collision, layer and camera functions.
 */
#include "builtins_internal.h"

#define NOONE IT_NOONE

/* ------------------------------------------------------------------ */
/* bounding boxes                                                      */
/* ------------------------------------------------------------------ */

bool instance_bbox(Instance *inst, double x, double y, double *l, double *t, double *r, double *b) {
    int32_t si = inst->mask_index >= 0 ? inst->mask_index : inst->sprite_index;
    if (si < 0 || si >= R->dw.nsprites) return false;
    const Sprite *s = &R->dw.sprites[si];
    double ml = s->margin_l, mr = s->margin_r + 1, mt = s->margin_t, mb = s->margin_b + 1;
    if (mr <= ml || mb <= mt) { ml = 0; mr = s->w; mt = 0; mb = s->h; }
    double x1 = (ml - s->origin_x) * inst->image_xscale, x2 = (mr - s->origin_x) * inst->image_xscale;
    double y1 = (mt - s->origin_y) * inst->image_yscale, y2 = (mb - s->origin_y) * inst->image_yscale;
    if (inst->image_angle != 0) {
        double rad = -inst->image_angle * M_PI / 180.0, c = cos(rad), sn = sin(rad);
        double xs[4] = {x1, x2, x2, x1}, ys[4] = {y1, y1, y2, y2};
        double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
        for (int i = 0; i < 4; i++) {
            double rx = xs[i] * c - ys[i] * sn, ry = xs[i] * sn + ys[i] * c;
            if (rx < minx) minx = rx;
            if (rx > maxx) maxx = rx;
            if (ry < miny) miny = ry;
            if (ry > maxy) maxy = ry;
        }
        x1 = minx; x2 = maxx; y1 = miny; y2 = maxy;
    }
    *l = x + fmin(x1, x2);
    *r = x + fmax(x1, x2);
    *t = y + fmin(y1, y2);
    *b = y + fmax(y1, y2);
    return true;
}

typedef enum { SHAPE_RECT, SHAPE_POINT, SHAPE_LINE, SHAPE_CIRCLE } ShapeKind;
typedef struct { ShapeKind k; double x1, y1, x2, y2; } Shape;

static bool segment_hits_rect(double x1, double y1, double x2, double y2, double l, double t, double r, double b) {
    double t0 = 0, t1 = 1, dx = x2 - x1, dy = y2 - y1;
    double p[4] = {-dx, dx, -dy, dy}, q[4] = {x1 - l, r - x1, y1 - t, b - y1};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
        } else {
            double u = q[i] / p[i];
            if (p[i] < 0) { if (u > t1) return false; if (u > t0) t0 = u; }
            else { if (u < t0) return false; if (u < t1) t1 = u; }
        }
    }
    return true;
}

static bool shape_hits(const Shape *s, Instance *i) {
    double l, t, r, b;
    if (!instance_bbox(i, i->x, i->y, &l, &t, &r, &b)) return false;
    switch (s->k) {
    case SHAPE_RECT: {
        double x1 = fmin(s->x1, s->x2), x2 = fmax(s->x1, s->x2), y1 = fmin(s->y1, s->y2), y2 = fmax(s->y1, s->y2);
        return x1 < r && l <= x2 && y1 < b && t <= y2;
    }
    case SHAPE_POINT: return s->x1 >= l && s->x1 < r && s->y1 >= t && s->y1 < b;
    case SHAPE_LINE: return segment_hits_rect(s->x1, s->y1, s->x2, s->y2, l, t, r, b);
    case SHAPE_CIRCLE: {
        double cx = fmax(l, fmin(s->x1, r)), cy = fmax(t, fmin(s->y1, b));
        double dx = s->x1 - cx, dy = s->y1 - cy;
        return dx * dx + dy * dy <= s->x2 * s->x2;
    }
    }
    return false;
}

/* Returns the first instance matching target that hits the shape. */
static Instance *find_hit(const Shape *s, Value target, Instance *exclude) {
    int32_t *ids;
    int32_t n = resolve_all(target, &ids);
    Instance *hit = NULL;
    for (int32_t k = 0; k < n && !hit; k++) {
        Instance *i = instance_find_id(ids[k]);
        if (!i || i == exclude) continue;
        if (shape_hits(s, i)) hit = i;
    }
    free(ids);
    return hit;
}

static Instance *place_hit(Instance *self, double x, double y, Value target) {
    double l, t, r, b;
    if (!self || !instance_bbox(self, x, y, &l, &t, &r, &b)) return NULL;
    int32_t *ids;
    int32_t n = resolve_all(target, &ids);
    Instance *hit = NULL;
    for (int32_t k = 0; k < n && !hit; k++) {
        Instance *i = instance_find_id(ids[k]);
        if (!i || i == self) continue;
        double l2, t2, r2, b2;
        if (!instance_bbox(i, i->x, i->y, &l2, &t2, &r2, &b2)) continue;
        if (l < r2 && l2 < r && t < b2 && t2 < b) hit = i;
    }
    free(ids);
    return hit;
}

static Value id_or_noone(Instance *i) { return v_real(i ? i->id : NOONE); }

static Value bi_place_meeting(int32_t argc, Value *a) {
    Instance *self = as_instance(cur_self());
    return v_bool(argc > 2 && place_hit(self, v_to_real(a[0]), v_to_real(a[1]), a[2]) != NULL);
}
static Value bi_instance_place(int32_t argc, Value *a) {
    Instance *self = as_instance(cur_self());
    return id_or_noone(argc > 2 ? place_hit(self, v_to_real(a[0]), v_to_real(a[1]), a[2]) : NULL);
}
static Value bi_place_free(int32_t argc, Value *a) {
    Instance *self = as_instance(cur_self());
    double l, t, r, b;
    if (!self || !instance_bbox(self, ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), &l, &t, &r, &b)) return v_bool(true);
    for (int32_t k = 0; k < R->ninsts; k++) {
        Instance *i = R->insts[k];
        if (i == self || i->destroyed || !i->active || !i->solid) continue;
        double l2, t2, r2, b2;
        if (instance_bbox(i, i->x, i->y, &l2, &t2, &r2, &b2) && l < r2 && l2 < r && t < b2 && t2 < b)
            return v_bool(false);
    }
    return v_bool(true);
}
static Value bi_place_empty(int32_t argc, Value *a) {
    Instance *self = as_instance(cur_self());
    Value all = v_real(IT_ALL);
    return v_bool(!place_hit(self, ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), argc > 2 ? a[2] : all));
}
static Value bi_position_meeting(int32_t argc, Value *a) {
    Shape s = {SHAPE_POINT, ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), 0, 0};
    return v_bool(argc > 2 && find_hit(&s, a[2], NULL));
}
static Value bi_instance_position(int32_t argc, Value *a) {
    Shape s = {SHAPE_POINT, ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), 0, 0};
    return id_or_noone(argc > 2 ? find_hit(&s, a[2], NULL) : NULL);
}
static Value collision_impl(Shape *s, int32_t argc, Value *a, int obj_arg) {
    if (argc <= obj_arg) return v_real(NOONE);
    bool notme = argc > obj_arg + 2 && v_to_bool(a[obj_arg + 2]);
    Instance *self = as_instance(cur_self());
    return id_or_noone(find_hit(s, a[obj_arg], notme ? self : NULL));
}
static Value bi_collision_rectangle(int32_t argc, Value *a) {
    Shape s = {SHAPE_RECT, ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0)};
    return collision_impl(&s, argc, a, 4);
}
static Value bi_collision_point(int32_t argc, Value *a) {
    Shape s = {SHAPE_POINT, ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), 0, 0};
    return collision_impl(&s, argc, a, 2);
}
static Value bi_collision_line(int32_t argc, Value *a) {
    Shape s = {SHAPE_LINE, ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0)};
    return collision_impl(&s, argc, a, 4);
}
static Value bi_collision_circle(int32_t argc, Value *a) {
    Shape s = {SHAPE_CIRCLE, ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGR(argc, a, 2, 0), 0};
    return collision_impl(&s, argc, a, 3);
}

/* ------------------------------------------------------------------ */
/* instances                                                           */
/* ------------------------------------------------------------------ */

static Value bi_instance_create_depth(int32_t argc, Value *a) {
    Instance *i = instance_create(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGI(argc, a, 3, -1), ARGR(argc, a, 2, 0), true);
    return id_or_noone(i);
}

static int32_t layer_index_by_value(Value v) {
    if (v.k == V_STRING) {
        for (int32_t i = 0; i < R->nlayers; i++)
            if (!strcmp(R->layers[i].name, v.s->chars)) return i;
        return -1;
    }
    int32_t id = v_to_int(v);
    for (int32_t i = 0; i < R->nlayers; i++)
        if (R->layers[i].id == id) return i;
    return -1;
}

static Value bi_instance_create_layer(int32_t argc, Value *a) {
    int32_t li = argc > 2 ? layer_index_by_value(a[2]) : -1;
    double depth = li >= 0 ? R->layers[li].depth : 0;
    Instance *i = instance_create(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGI(argc, a, 3, -1), depth, true);
    if (i && li >= 0) i->layer = R->layers[li].id;
    return id_or_noone(i);
}
static Value bi_instance_create(int32_t argc, Value *a) {
    Instance *i = instance_create(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGI(argc, a, 2, -1), 0, false);
    return id_or_noone(i);
}
static Value bi_instance_destroy(int32_t argc, Value *a) {
    if (argc == 0) {
        Instance *self = as_instance(cur_self());
        if (self) instance_destroy(self, true);
        return v_undef();
    }
    bool run = argc > 1 ? v_to_bool(a[1]) : true;
    int32_t *ids;
    int32_t n = resolve_all(a[0], &ids);
    for (int32_t k = 0; k < n; k++) instance_destroy(instance_find_id(ids[k]), run);
    free(ids);
    return v_undef();
}
static Value bi_instance_exists(int32_t argc, Value *a) {
    if (!argc) return v_bool(false);
    if (a[0].k == V_STRUCT) return v_bool(true);
    int32_t t = v_to_int(a[0]);
    if (t >= INSTANCE_ID_BASE) {
        Instance *i = instance_find_id(t);
        return v_bool(i && i->active);
    }
    if (t == IT_SELF || t == IT_OTHER) return v_bool(resolve_single(a[0]) != NULL);
    return v_bool(instance_count_of(t) > 0);
}
static Value bi_instance_number(int32_t argc, Value *a) { return v_real(instance_count_of(ARGI(argc, a, 0, -1))); }
static Value bi_instance_find(int32_t argc, Value *a) {
    if (!argc) return v_real(NOONE);
    int32_t *ids;
    int32_t n = resolve_all(a[0], &ids), k = ARGI(argc, a, 1, 0);
    Value r = v_real(k >= 0 && k < n ? ids[k] : NOONE);
    free(ids);
    return r;
}
static Value nearest_impl(int32_t argc, Value *a, bool furthest) {
    if (argc < 3) return v_real(NOONE);
    double x = v_to_real(a[0]), y = v_to_real(a[1]);
    int32_t *ids;
    int32_t n = resolve_all(a[2], &ids);
    int32_t best = NOONE;
    double bd = furthest ? -1 : 1e300;
    Instance *self = as_instance(cur_self());
    for (int32_t k = 0; k < n; k++) {
        Instance *i = instance_find_id(ids[k]);
        if (!i || (!furthest && i == self && n > 1 && false)) continue;
        double d = (i->x - x) * (i->x - x) + (i->y - y) * (i->y - y);
        if (furthest ? d > bd : d < bd) { bd = d; best = i->id; }
    }
    free(ids);
    return v_real(best);
}
static Value bi_instance_nearest(int32_t argc, Value *a) { return nearest_impl(argc, a, false); }
static Value bi_instance_furthest(int32_t argc, Value *a) { return nearest_impl(argc, a, true); }
static Value bi_instance_change(int32_t argc, Value *a) {
    Instance *self = as_instance(cur_self());
    int32_t obj = ARGI(argc, a, 0, -1);
    if (!self || obj < 0 || obj >= R->dw.nobjects) return v_undef();
    bool perf = argc > 1 && v_to_bool(a[1]);
    if (perf) event_run(self, EV_DESTROY, 0, NULL);
    self->object_index = obj;
    const Object *o = &R->dw.objects[obj];
    self->sprite_index = o->sprite;
    self->mask_index = o->mask;
    if (perf) event_run(self, EV_CREATE, 0, NULL);
    return v_undef();
}
static Value bi_instance_copy(int32_t argc, Value *a) {
    Instance *self = as_instance(cur_self());
    if (!self) return v_real(NOONE);
    Instance *c = instance_create(self->x, self->y, self->object_index, self->depth, true);
    (void)argc; (void)a;
    return id_or_noone(c);
}
static void set_active_matching(Value target, bool active, bool notme) {
    Instance *self = as_instance(cur_self());
    for (int32_t k = 0; k < R->ninsts; k++) {
        Instance *i = R->insts[k];
        if (i->destroyed || (notme && i == self)) continue;
        int32_t t = v_to_int(target);
        bool match = t == IT_ALL || (t >= INSTANCE_ID_BASE ? i->id == t : object_is_ancestor(i->object_index, t));
        if (match) i->active = active;
    }
}
static Value bi_instance_activate_all(int32_t argc, Value *a) { (void)argc; (void)a; set_active_matching(v_real(IT_ALL), true, false); return v_undef(); }
static Value bi_instance_deactivate_all(int32_t argc, Value *a) {
    set_active_matching(v_real(IT_ALL), false, argc > 0 && v_to_bool(a[0]));
    return v_undef();
}
static Value bi_instance_activate_object(int32_t argc, Value *a) { if (argc) set_active_matching(a[0], true, false); return v_undef(); }
static Value bi_instance_deactivate_object(int32_t argc, Value *a) { if (argc) set_active_matching(a[0], false, false); return v_undef(); }
static Value bi_instance_activate_region(int32_t argc, Value *a) { (void)argc; (void)a; set_active_matching(v_real(IT_ALL), true, false); return v_undef(); }
static Value bi_instance_deactivate_region(int32_t argc, Value *a) { (void)argc; (void)a; return v_undef(); }

/* motion */
static Value bi_motion_set(int32_t argc, Value *a) {
    Instance *i = as_instance(cur_self());
    if (!i) return v_undef();
    i->direction = ARGR(argc, a, 0, 0);
    i->speed = ARGR(argc, a, 1, 0);
    i->hspeed = i->speed * cos(i->direction * M_PI / 180);
    i->vspeed = -i->speed * sin(i->direction * M_PI / 180);
    return v_undef();
}
static Value bi_motion_add(int32_t argc, Value *a) {
    Instance *i = as_instance(cur_self());
    if (!i) return v_undef();
    double d = ARGR(argc, a, 0, 0), s = ARGR(argc, a, 1, 0);
    i->hspeed += s * cos(d * M_PI / 180);
    i->vspeed -= s * sin(d * M_PI / 180);
    i->speed = sqrt(i->hspeed * i->hspeed + i->vspeed * i->vspeed);
    i->direction = fmod(atan2(-i->vspeed, i->hspeed) * 180 / M_PI + 360, 360);
    return v_undef();
}
static Value bi_move_towards_point(int32_t argc, Value *a) {
    Instance *i = as_instance(cur_self());
    if (!i) return v_undef();
    double dx = ARGR(argc, a, 0, 0) - i->x, dy = ARGR(argc, a, 1, 0) - i->y;
    i->direction = fmod(atan2(-dy, dx) * 180 / M_PI + 360, 360);
    i->speed = ARGR(argc, a, 2, 0);
    i->hspeed = i->speed * cos(i->direction * M_PI / 180);
    i->vspeed = -i->speed * sin(i->direction * M_PI / 180);
    return v_undef();
}
static Value bi_distance_to_point(int32_t argc, Value *a) {
    Instance *i = as_instance(cur_self());
    if (!i) return v_real(0);
    double l, t, r, b;
    double px = ARGR(argc, a, 0, 0), py = ARGR(argc, a, 1, 0);
    if (!instance_bbox(i, i->x, i->y, &l, &t, &r, &b)) { l = r = i->x; t = b = i->y; }
    double dx = px < l ? l - px : px > r ? px - r : 0, dy = py < t ? t - py : py > b ? py - b : 0;
    return v_real(sqrt(dx * dx + dy * dy));
}
static Value bi_distance_to_object(int32_t argc, Value *a) {
    Instance *self = as_instance(cur_self());
    if (!self || !argc) return v_real(1e6);
    int32_t *ids;
    int32_t n = resolve_all(a[0], &ids);
    double best = 1e6;
    double l1, t1, r1, b1;
    bool has = instance_bbox(self, self->x, self->y, &l1, &t1, &r1, &b1);
    if (!has) { l1 = r1 = self->x; t1 = b1 = self->y; }
    for (int32_t k = 0; k < n; k++) {
        Instance *i = instance_find_id(ids[k]);
        if (!i || i == self) continue;
        double l2, t2, r2, b2;
        if (!instance_bbox(i, i->x, i->y, &l2, &t2, &r2, &b2)) { l2 = r2 = i->x; t2 = b2 = i->y; }
        double dx = l2 > r1 ? l2 - r1 : l1 > r2 ? l1 - r2 : 0, dy = t2 > b1 ? t2 - b1 : t1 > b2 ? t1 - b2 : 0;
        double d = sqrt(dx * dx + dy * dy);
        if (d < best) best = d;
    }
    free(ids);
    return v_real(best);
}

/* ------------------------------------------------------------------ */
/* layers                                                              */
/* ------------------------------------------------------------------ */

static Layer *layer_arg(int32_t argc, Value *a, int32_t i) {
    if (i >= argc) return NULL;
    int32_t li = layer_index_by_value(a[i]);
    return li >= 0 ? &R->layers[li] : NULL;
}
static Value bi_layer_get_id(int32_t argc, Value *a) {
    Layer *L = layer_arg(argc, a, 0);
    return v_real(L ? L->id : -1);
}
static Value bi_layer_exists(int32_t argc, Value *a) { return v_bool(layer_arg(argc, a, 0) != NULL); }
static Value bi_layer_get_depth(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); return v_real(L ? L->depth : 0); }
static Value bi_layer_depth(int32_t argc, Value *a) {
    Layer *L = layer_arg(argc, a, 0);
    if (L) {
        int32_t old = L->depth;
        L->depth = ARGI(argc, a, 1, 0);
        for (int32_t k = 0; k < R->ninsts; k++)
            if (R->insts[k]->layer == L->id) R->insts[k]->depth = L->depth;
        (void)old;
    }
    return v_undef();
}
static Value bi_layer_set_visible(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); if (L) L->visible = argc > 1 && v_to_bool(a[1]); return v_undef(); }
static Value bi_layer_get_visible(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); return v_bool(L && L->visible); }
static Value bi_layer_get_name(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); return v_str(L ? L->name : ""); }
static Value bi_layer_x(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); if (L) L->x = (float)ARGR(argc, a, 1, 0); return v_undef(); }
static Value bi_layer_y(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); if (L) L->y = (float)ARGR(argc, a, 1, 0); return v_undef(); }
static Value bi_layer_get_x(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); return v_real(L ? L->x : 0); }
static Value bi_layer_get_y(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); return v_real(L ? L->y : 0); }
static Value bi_layer_hspeed(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); if (L) L->hspeed = (float)ARGR(argc, a, 1, 0); return v_undef(); }
static Value bi_layer_vspeed(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); if (L) L->vspeed = (float)ARGR(argc, a, 1, 0); return v_undef(); }
static Value bi_layer_create(int32_t argc, Value *a) {
    if (R->nlayers == R->cap_layers) {
        R->cap_layers = R->cap_layers ? R->cap_layers * 2 : 32;
        R->layers = d3_realloc(R->layers, (size_t)R->cap_layers * sizeof(Layer));
    }
    Layer *L = &R->layers[R->nlayers++];
    memset(L, 0, sizeof *L);
    L->id = R->next_layer_id++;
    L->depth = ARGI(argc, a, 0, 0);
    L->visible = true;
    L->type = LAYER_INSTANCES;
    L->bg_sprite = -1;
    L->bg_xscale = L->bg_yscale = 1;
    if (argc > 1) snprintf(L->name, sizeof L->name, "%s", ARGS(argc, a, 1));
    else snprintf(L->name, sizeof L->name, "_layer_%d", L->id);
    return v_real(L->id);
}
static Value bi_layer_destroy(int32_t argc, Value *a) {
    int32_t li = argc ? layer_index_by_value(a[0]) : -1;
    if (li >= 0) {
        memmove(&R->layers[li], &R->layers[li + 1], (size_t)(R->nlayers - li - 1) * sizeof(Layer));
        R->nlayers--;
    }
    return v_undef();
}
static Value bi_layer_get_all(int32_t argc, Value *a) {
    (void)argc; (void)a;
    Value arr = v_array_new(0);
    for (int32_t i = 0; i < R->nlayers; i++) arr_push(arr.a, v_real(R->layers[i].id));
    return arr;
}
/* Background elements share the layer's id. */
static Value bi_layer_background_get_id(int32_t argc, Value *a) {
    Layer *L = layer_arg(argc, a, 0);
    return v_real(L ? L->id : -1);
}
static Value bi_layer_background_create(int32_t argc, Value *a) {
    Layer *L = layer_arg(argc, a, 0);
    if (!L) return v_real(-1);
    L->type = LAYER_BACKGROUND;
    L->bg_sprite = ARGI(argc, a, 1, -1);
    L->bg_visible = true;
    L->bg_color = 0xFFFFFFFF;
    return v_real(L->id);
}
#define BG_SETTER(fn, field, conv)                                                                       \
    static Value bi_##fn(int32_t argc, Value *a) {                                                      \
        Layer *L = layer_arg(argc, a, 0);                                                               \
        if (L) L->field = conv;                                                                         \
        return v_undef();                                                                               \
    }
BG_SETTER(layer_background_sprite, bg_sprite, ARGI(argc, a, 1, -1))
BG_SETTER(layer_background_visible, bg_visible, argc > 1 && v_to_bool(a[1]))
BG_SETTER(layer_background_htiled, bg_htiled, argc > 1 && v_to_bool(a[1]))
BG_SETTER(layer_background_vtiled, bg_vtiled, argc > 1 && v_to_bool(a[1]))
BG_SETTER(layer_background_stretch, bg_stretch, argc > 1 && v_to_bool(a[1]))
BG_SETTER(layer_background_index, bg_index, (float)ARGR(argc, a, 1, 0))
BG_SETTER(layer_background_speed, bg_speed, (float)ARGR(argc, a, 1, 0))
BG_SETTER(layer_background_xscale, bg_xscale, (float)ARGR(argc, a, 1, 1))
BG_SETTER(layer_background_yscale, bg_yscale, (float)ARGR(argc, a, 1, 1))
static Value bi_layer_background_blend(int32_t argc, Value *a) {
    Layer *L = layer_arg(argc, a, 0);
    if (L) L->bg_color = (L->bg_color & 0xFF000000u) | ((uint32_t)ARGI(argc, a, 1, 0xFFFFFF) & 0xFFFFFF);
    return v_undef();
}
static Value bi_layer_background_alpha(int32_t argc, Value *a) {
    Layer *L = layer_arg(argc, a, 0);
    if (L) {
        double al = ARGR(argc, a, 1, 1);
        L->bg_color = (L->bg_color & 0xFFFFFF) | ((uint32_t)(fmax(0, fmin(1, al)) * 255) << 24);
    }
    return v_undef();
}
static Value bi_layer_background_get_sprite(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); return v_real(L ? L->bg_sprite : -1); }
static Value bi_layer_background_get_visible(int32_t argc, Value *a) { Layer *L = layer_arg(argc, a, 0); return v_bool(L && L->bg_visible); }
static Value bi_layer_tilemap_get_id(int32_t argc, Value *a) {
    Layer *L = layer_arg(argc, a, 0);
    return v_real(L && L->type == LAYER_TILES ? L->id : -1);
}

/* ------------------------------------------------------------------ */
/* cameras (camera id == view index)                                   */
/* ------------------------------------------------------------------ */

static View *cam_arg(int32_t argc, Value *a) {
    int32_t c = ARGI(argc, a, 0, 0);
    if (c < 0 || c >= MAX_VIEWS) c = 0;
    return &R->views[c];
}
static Value bi_camera_get_view_x(int32_t argc, Value *a) { return v_real(cam_arg(argc, a)->x); }
static Value bi_camera_get_view_y(int32_t argc, Value *a) { return v_real(cam_arg(argc, a)->y); }
static Value bi_camera_get_view_width(int32_t argc, Value *a) { return v_real(cam_arg(argc, a)->w); }
static Value bi_camera_get_view_height(int32_t argc, Value *a) { return v_real(cam_arg(argc, a)->h); }
static Value bi_camera_get_view_angle(int32_t argc, Value *a) { return v_real(cam_arg(argc, a)->angle); }
static Value bi_camera_get_view_target(int32_t argc, Value *a) { return v_real(cam_arg(argc, a)->follow); }
static Value bi_camera_get_view_border_x(int32_t argc, Value *a) { return v_real(cam_arg(argc, a)->border_x); }
static Value bi_camera_get_view_border_y(int32_t argc, Value *a) { return v_real(cam_arg(argc, a)->border_y); }
static Value bi_camera_set_view_pos(int32_t argc, Value *a) {
    View *v = cam_arg(argc, a);
    v->x = ARGR(argc, a, 1, v->x);
    v->y = ARGR(argc, a, 2, v->y);
    return v_undef();
}
static Value bi_camera_set_view_size(int32_t argc, Value *a) {
    View *v = cam_arg(argc, a);
    v->w = ARGR(argc, a, 1, v->w);
    v->h = ARGR(argc, a, 2, v->h);
    return v_undef();
}
static Value bi_camera_set_view_target(int32_t argc, Value *a) { cam_arg(argc, a)->follow = ARGI(argc, a, 1, -1); return v_undef(); }
static Value bi_camera_set_view_angle(int32_t argc, Value *a) { cam_arg(argc, a)->angle = ARGR(argc, a, 1, 0); return v_undef(); }
static Value bi_camera_set_view_border(int32_t argc, Value *a) {
    View *v = cam_arg(argc, a);
    v->border_x = ARGR(argc, a, 1, 0);
    v->border_y = ARGR(argc, a, 2, 0);
    return v_undef();
}
static Value bi_camera_set_view_speed(int32_t argc, Value *a) {
    View *v = cam_arg(argc, a);
    v->speed_x = ARGR(argc, a, 1, -1);
    v->speed_y = ARGR(argc, a, 2, -1);
    return v_undef();
}
static Value bi_camera_create_view(int32_t argc, Value *a) {
    /* Reuse the first view that is not enabled as the new camera. */
    for (int32_t v = 1; v < MAX_VIEWS; v++) {
        if (!R->views[v].enabled) {
            View *vw = &R->views[v];
            vw->x = ARGR(argc, a, 0, 0); vw->y = ARGR(argc, a, 1, 0);
            vw->w = ARGR(argc, a, 2, R->window_w); vw->h = ARGR(argc, a, 3, R->window_h);
            vw->angle = ARGR(argc, a, 4, 0);
            vw->follow = ARGI(argc, a, 5, -1);
            return v_real(v);
        }
    }
    return v_real(0);
}
static Value bi_camera_create(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(0); }
static Value bi_view_get_camera(int32_t argc, Value *a) { return v_real(ARGI(argc, a, 0, 0)); }
static Value bi_view_set_camera(int32_t argc, Value *a) { (void)argc; (void)a; return v_undef(); }
static View *view_arg(int32_t argc, Value *a) {
    int32_t v = ARGI(argc, a, 0, 0);
    if (v < 0 || v >= MAX_VIEWS) v = 0;
    return &R->views[v];
}
static Value bi_view_get_visible(int32_t argc, Value *a) { return v_bool(view_arg(argc, a)->visible); }
static Value bi_view_set_visible(int32_t argc, Value *a) {
    View *v = view_arg(argc, a);
    v->visible = v->enabled = argc > 1 && v_to_bool(a[1]);
    return v_undef();
}
static Value bi_view_get_xport(int32_t argc, Value *a) { return v_real(view_arg(argc, a)->port_x); }
static Value bi_view_get_yport(int32_t argc, Value *a) { return v_real(view_arg(argc, a)->port_y); }
static Value bi_view_get_wport(int32_t argc, Value *a) { return v_real(view_arg(argc, a)->port_w); }
static Value bi_view_get_hport(int32_t argc, Value *a) { return v_real(view_arg(argc, a)->port_h); }
static Value bi_view_set_xport(int32_t argc, Value *a) { view_arg(argc, a)->port_x = ARGR(argc, a, 1, 0); return v_undef(); }
static Value bi_view_set_yport(int32_t argc, Value *a) { view_arg(argc, a)->port_y = ARGR(argc, a, 1, 0); return v_undef(); }
static Value bi_view_set_wport(int32_t argc, Value *a) { view_arg(argc, a)->port_w = ARGR(argc, a, 1, 0); return v_undef(); }
static Value bi_view_set_hport(int32_t argc, Value *a) { view_arg(argc, a)->port_h = ARGR(argc, a, 1, 0); return v_undef(); }

/* GMS1 compatibility helpers (__view_get / __view_set) */
static Value bi_view_get_compat(int32_t argc, Value *a) {
    int32_t e = ARGI(argc, a, 0, 0);
    View *v = &R->views[ARGI(argc, a, 1, 0) & 7];
    switch (e) {
    case 0: return v_real(v->x);
    case 1: return v_real(v->y);
    case 2: return v_real(v->w);
    case 3: return v_real(v->h);
    case 4: return v_real(v->angle);
    case 5: return v_real(v->border_x);
    case 6: return v_real(v->border_y);
    case 7: return v_real(v->speed_x);
    case 8: return v_real(v->speed_y);
    case 9: return v_real(v->follow);
    case 10: return v_bool(v->visible);
    case 11: return v_real(v->port_x);
    case 12: return v_real(v->port_y);
    case 13: return v_real(v->port_w);
    case 14: return v_real(v->port_h);
    case 15: return v_real(ARGI(argc, a, 1, 0) & 7);
    default: return v_real(-1);
    }
}
static Value bi_view_set_compat(int32_t argc, Value *a) {
    int32_t e = ARGI(argc, a, 0, 0);
    View *v = &R->views[ARGI(argc, a, 1, 0) & 7];
    double d = ARGR(argc, a, 2, 0);
    switch (e) {
    case 0: v->x = d; break;
    case 1: v->y = d; break;
    case 2: v->w = d; break;
    case 3: v->h = d; break;
    case 4: v->angle = d; break;
    case 5: v->border_x = d; break;
    case 6: v->border_y = d; break;
    case 7: v->speed_x = d; break;
    case 8: v->speed_y = d; break;
    case 9: v->follow = (int32_t)d; break;
    case 10: v->visible = v->enabled = d > 0.5; break;
    case 11: v->port_x = d; break;
    case 12: v->port_y = d; break;
    case 13: v->port_w = d; break;
    case 14: v->port_h = d; break;
    default: break;
    }
    return v_undef();
}

void builtins_register_instances(void) {
#define REG(n) builtin_register(#n, bi_##n)
    REG(place_meeting); REG(instance_place); REG(place_free); REG(place_empty); REG(position_meeting);
    REG(instance_position); REG(collision_rectangle); REG(collision_point); REG(collision_line);
    REG(collision_circle);
    REG(instance_create_depth); REG(instance_create_layer); REG(instance_create); REG(instance_destroy);
    REG(instance_exists); REG(instance_number); REG(instance_find); REG(instance_nearest);
    REG(instance_furthest); REG(instance_change); REG(instance_copy); REG(instance_activate_all);
    REG(instance_deactivate_all); REG(instance_activate_object); REG(instance_deactivate_object);
    REG(instance_activate_region); REG(instance_deactivate_region);
    builtin_register("instance_deactivate_layer", bi_instance_deactivate_region);
    REG(motion_set); REG(motion_add); REG(move_towards_point); REG(distance_to_point); REG(distance_to_object);
    REG(layer_get_id); REG(layer_exists); REG(layer_get_depth); REG(layer_depth); REG(layer_set_visible);
    REG(layer_get_visible); REG(layer_get_name); REG(layer_x); REG(layer_y); REG(layer_get_x); REG(layer_get_y);
    REG(layer_hspeed); REG(layer_vspeed); REG(layer_create); REG(layer_destroy); REG(layer_get_all);
    REG(layer_background_get_id); REG(layer_background_create); REG(layer_background_sprite);
    REG(layer_background_visible); REG(layer_background_htiled); REG(layer_background_vtiled);
    REG(layer_background_stretch); REG(layer_background_index); REG(layer_background_speed);
    REG(layer_background_xscale); REG(layer_background_yscale); REG(layer_background_blend);
    REG(layer_background_alpha); REG(layer_background_get_sprite); REG(layer_background_get_visible);
    REG(layer_tilemap_get_id);
    REG(camera_get_view_x); REG(camera_get_view_y); REG(camera_get_view_width); REG(camera_get_view_height);
    REG(camera_get_view_angle); REG(camera_get_view_target); REG(camera_get_view_border_x);
    REG(camera_get_view_border_y); REG(camera_set_view_pos); REG(camera_set_view_size);
    REG(camera_set_view_target); REG(camera_set_view_angle); REG(camera_set_view_border);
    REG(camera_set_view_speed); REG(camera_create_view); REG(camera_create);
    REG(view_get_camera); REG(view_set_camera); REG(view_get_visible); REG(view_set_visible);
    REG(view_get_xport); REG(view_get_yport); REG(view_get_wport); REG(view_get_hport);
    REG(view_set_xport); REG(view_set_yport); REG(view_set_wport); REG(view_set_hport);
    builtin_register("__view_get", bi_view_get_compat);
    builtin_register("__view_set", bi_view_set_compat);
#undef REG
}
