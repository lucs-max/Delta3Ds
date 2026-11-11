#include "runtime.h"
#include "../platform/platform.h"

Runtime *R;

/* ------------------------------------------------------------------ */
/* instance id map                                                     */
/* ------------------------------------------------------------------ */

static void idmap_grow(void) {
    uint32_t ncap = R->id_cap ? R->id_cap * 2 : 1024;
    int32_t *nk = d3_malloc(ncap * sizeof(int32_t));
    Instance **nv = d3_malloc(ncap * sizeof(Instance *));
    for (uint32_t i = 0; i < ncap; i++) nk[i] = -1;
    for (uint32_t i = 0; i < R->id_cap; i++) {
        if (R->id_keys[i] < 0) continue;
        uint32_t j = ((uint32_t)R->id_keys[i] * 2654435761u) & (ncap - 1);
        while (nk[j] >= 0) j = (j + 1) & (ncap - 1);
        nk[j] = R->id_keys[i];
        nv[j] = R->id_vals[i];
    }
    free(R->id_keys);
    free(R->id_vals);
    R->id_keys = nk;
    R->id_vals = nv;
    R->id_cap = ncap;
}

static void idmap_put(int32_t id, Instance *inst) {
    if ((R->id_count + 1) * 2 >= R->id_cap) idmap_grow();
    uint32_t j = ((uint32_t)id * 2654435761u) & (R->id_cap - 1);
    while (R->id_keys[j] >= 0 && R->id_keys[j] != id) j = (j + 1) & (R->id_cap - 1);
    if (R->id_keys[j] < 0) R->id_count++;
    R->id_keys[j] = id;
    R->id_vals[j] = inst;
}

static void idmap_remove(int32_t id) {
    if (!R->id_cap) return;
    uint32_t j = ((uint32_t)id * 2654435761u) & (R->id_cap - 1);
    while (R->id_keys[j] >= 0) {
        if (R->id_keys[j] == id) {
            /* backward-shift deletion keeps probe chains valid */
            uint32_t k = j;
            for (;;) {
                R->id_keys[k] = -1;
                uint32_t n = k;
                for (;;) {
                    n = (n + 1) & (R->id_cap - 1);
                    if (R->id_keys[n] < 0) { R->id_count--; return; }
                    uint32_t home = ((uint32_t)R->id_keys[n] * 2654435761u) & (R->id_cap - 1);
                    bool between = (k <= n) ? (k < home && home <= n) : (k < home || home <= n);
                    if (!between) break;
                }
                R->id_keys[k] = R->id_keys[n];
                R->id_vals[k] = R->id_vals[n];
                k = n;
            }
        }
        j = (j + 1) & (R->id_cap - 1);
    }
}

Instance *instance_find_id(int32_t id) {
    if (!R->id_cap) return NULL;
    uint32_t j = ((uint32_t)id * 2654435761u) & (R->id_cap - 1);
    while (R->id_keys[j] >= 0) {
        if (R->id_keys[j] == id) {
            Instance *i = R->id_vals[j];
            return (i && !i->destroyed) ? i : NULL;
        }
        j = (j + 1) & (R->id_cap - 1);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* object hierarchy                                                    */
/* ------------------------------------------------------------------ */

static void build_ancestors(void) {
    DataWin *dw = &R->dw;
    R->ancestors = d3_calloc((size_t)dw->nobjects, sizeof(int32_t *));
    for (int32_t o = 0; o < dw->nobjects; o++) {
        int32_t chain[64];
        int32_t n = 0, cur = o;
        while (cur >= 0 && cur < dw->nobjects && n < 63) {
            chain[n++] = cur;
            cur = dw->objects[cur].parent;
            if (cur == o) break; /* cycle guard */
        }
        R->ancestors[o] = d3_malloc((size_t)(n + 1) * sizeof(int32_t));
        memcpy(R->ancestors[o], chain, (size_t)n * sizeof(int32_t));
        R->ancestors[o][n] = -1;
    }
}

bool object_is_ancestor(int32_t obj, int32_t ancestor) {
    if (obj < 0 || obj >= R->dw.nobjects) return false;
    for (int32_t *a = R->ancestors[obj]; *a >= 0; a++)
        if (*a == ancestor) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* instance designators                                                */
/* ------------------------------------------------------------------ */

static bool inst_matches(Instance *i, int32_t target) {
    if (i->destroyed || !i->active) return false;
    if (target == IT_ALL) return true;
    if (target >= INSTANCE_ID_BASE) return i->id == target;
    return object_is_ancestor(i->object_index, target);
}

Obj *resolve_single(Value target) {
    if (target.k == V_STRUCT) return target.o;
    int32_t t = v_to_int(target);
    if (t == IT_SELF) return cur_self();
    if (t == IT_OTHER) return cur_other();
    if (t == IT_GLOBAL) return &R->global;
    if (t >= INSTANCE_ID_BASE) {
        Instance *i = instance_find_id(t);
        return i ? &i->base : NULL;
    }
    if (t == IT_ALL || (t >= 0 && t < R->dw.nobjects)) {
        for (int32_t k = 0; k < R->ninsts; k++)
            if (inst_matches(R->insts[k], t)) return &R->insts[k]->base;
    }
    return NULL;
}

int32_t resolve_all(Value target, int32_t **out) {
    *out = NULL;
    if (target.k == V_STRUCT) return 0;
    int32_t t = v_to_int(target);
    if (t == IT_SELF || t == IT_OTHER) {
        Instance *i = as_instance(t == IT_SELF ? cur_self() : cur_other());
        if (!i) return 0;
        *out = d3_malloc(sizeof(int32_t));
        (*out)[0] = i->id;
        return 1;
    }
    if (t >= INSTANCE_ID_BASE) {
        Instance *i = instance_find_id(t);
        if (!i || !i->active) return 0;
        *out = d3_malloc(sizeof(int32_t));
        (*out)[0] = t;
        return 1;
    }
    if (t != IT_ALL && (t < 0 || t >= R->dw.nobjects)) return 0;
    int32_t n = 0;
    int32_t *ids = d3_malloc((size_t)(R->ninsts + 1) * sizeof(int32_t));
    for (int32_t k = 0; k < R->ninsts; k++)
        if (inst_matches(R->insts[k], t)) ids[n++] = R->insts[k]->id;
    *out = ids;
    return n;
}

int32_t instance_count_of(int32_t obj) {
    int32_t n = 0;
    for (int32_t k = 0; k < R->ninsts; k++)
        if (inst_matches(R->insts[k], obj)) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

int32_t event_find_code(int32_t obj, int32_t type, int32_t sub, int32_t *owner) {
    DataWin *dw = &R->dw;
    if (obj < 0 || obj >= dw->nobjects || type < 0 || type >= EV_COUNT) return -1;
    for (int32_t *a = R->ancestors[obj]; *a >= 0; a++) {
        const Object *o = &dw->objects[*a];
        for (int32_t e = 0; e < o->nevents[type]; e++) {
            if ((int32_t)o->events[type][e].subtype == sub) {
                if (owner) *owner = *a;
                return o->events[type][e].code;
            }
        }
    }
    return -1;
}

static bool run_event_from(Instance *inst, int32_t start_obj, int32_t type, int32_t sub, Obj *other) {
    int32_t owner = -1;
    int32_t code = event_find_code(start_obj, type, sub, &owner);
    if (code < 0) return false;
    int32_t st = R->ev_type, ss = R->ev_sub, so = R->ev_object;
    R->ev_type = type;
    R->ev_sub = sub;
    R->ev_object = owner;
    Value r = vm_call_code(code, &inst->base, other ? other : &inst->base, 0, NULL);
    v_release(r);
    R->ev_type = st;
    R->ev_sub = ss;
    R->ev_object = so;
    return true;
}

bool event_run(Instance *inst, int32_t type, int32_t sub, Obj *other) {
    if (!inst || inst->destroyed) return false;
    return run_event_from(inst, inst->object_index, type, sub, other);
}

bool event_run_inherited(Instance *inst, Obj *other) {
    if (!inst || R->ev_object < 0) return false;
    int32_t parent = R->dw.objects[R->ev_object].parent;
    if (parent < 0) return false;
    return run_event_from(inst, parent, R->ev_type, R->ev_sub, other);
}

/* Runs an event on every live instance (snapshot so creation/destruction
 * during the loop is safe). */
static void event_all(int32_t type, int32_t sub) {
    int32_t n = R->ninsts;
    if (!n) return;
    Instance **snap = d3_malloc((size_t)n * sizeof(Instance *));
    memcpy(snap, R->insts, (size_t)n * sizeof(Instance *));
    for (int32_t i = 0; i < n; i++) obj_retain(&snap[i]->base);
    for (int32_t i = 0; i < n; i++)
        if (!snap[i]->destroyed && snap[i]->active) event_run(snap[i], type, sub, NULL);
    for (int32_t i = 0; i < n; i++) obj_release(&snap[i]->base);
    free(snap);
}

/* ------------------------------------------------------------------ */
/* instances                                                           */
/* ------------------------------------------------------------------ */

void runtime_instance_freed(Obj *o) {
    Instance *i = (Instance *)o;
    varmap_free(&i->base.vars);
    free(i);
}

static Instance *instance_alloc(int32_t id, int32_t obj, double x, double y) {
    Instance *i = d3_calloc(1, sizeof *i);
    i->base.rc = 1; /* owned by the instance list */
    i->base.kind = OBJ_INSTANCE;
    varmap_init(&i->base.vars);
    i->id = id;
    i->object_index = obj;
    i->active = true;
    i->x = i->xstart = i->xprevious = x;
    i->y = i->ystart = i->yprevious = y;
    i->image_xscale = i->image_yscale = 1;
    i->image_alpha = 1;
    i->image_blend = 0xFFFFFF;
    i->image_speed = 1;
    i->gravity_direction = 270;
    i->layer = -1;
    for (int k = 0; k < MAX_ALARMS; k++) i->alarm[k] = -1;
    i->create_order = R->create_counter++;
    if (obj >= 0 && obj < R->dw.nobjects) {
        const Object *o = &R->dw.objects[obj];
        i->sprite_index = o->sprite;
        i->mask_index = o->mask;
        i->visible = o->visible;
        i->solid = o->solid;
        i->persistent = o->persistent;
        i->depth = o->depth;
    } else {
        i->sprite_index = -1;
        i->mask_index = -1;
        i->visible = true;
    }
    if (R->ninsts == R->cap_insts) {
        R->cap_insts = R->cap_insts ? R->cap_insts * 2 : 256;
        R->insts = d3_realloc(R->insts, (size_t)R->cap_insts * sizeof(Instance *));
    }
    R->insts[R->ninsts++] = i;
    idmap_put(id, i);
    return i;
}

Instance *instance_create(double x, double y, int32_t obj, double depth, bool use_depth) {
    if (obj < 0 || obj >= R->dw.nobjects) {
        vm_runtime_error("instance_create: object %d does not exist", obj);
        return NULL;
    }
    Instance *i = instance_alloc(R->next_instance_id++, obj, x, y);
    if (use_depth) i->depth = depth;
    obj_retain(&i->base);
    event_run(i, EV_PRECREATE, 0, NULL);
    event_run(i, EV_CREATE, 0, NULL);
    i->created = true;
    obj_release(&i->base);
    return i;
}

void instance_destroy(Instance *inst, bool run_event) {
    if (!inst || inst->destroyed) return;
    if (run_event) event_run(inst, EV_DESTROY, 0, NULL);
    event_run(inst, EV_CLEANUP, 0, NULL);
    inst->destroyed = true;
}

/* Removes destroyed instances from the list. */
static void instances_compact(void) {
    int32_t w = 0;
    for (int32_t r = 0; r < R->ninsts; r++) {
        Instance *i = R->insts[r];
        if (i->destroyed) {
            idmap_remove(i->id);
            obj_release(&i->base);
        } else {
            R->insts[w++] = i;
        }
    }
    R->ninsts = w;
}

/* ------------------------------------------------------------------ */
/* rooms                                                               */
/* ------------------------------------------------------------------ */

int32_t room_find_by_order(int32_t room, int32_t delta) {
    for (int32_t i = 0; i < R->dw.nroom_order; i++) {
        if (R->dw.room_order[i] == room) {
            int32_t j = i + delta;
            if (j < 0 || j >= R->dw.nroom_order) return -1;
            return R->dw.room_order[j];
        }
    }
    return -1;
}

void room_goto(int32_t room) {
    if (room < 0 || room >= R->dw.nrooms) {
        vm_runtime_error("room_goto: room %d does not exist", room);
        return;
    }
    R->next_room = room;
    R->room_change_pending = true;
}

static void setup_views(const Room *rm) {
    memset(R->views, 0, sizeof R->views);
    R->views_enabled = (rm->flags & 1) != 0;
    for (int32_t v = 0; v < MAX_VIEWS; v++) {
        View *vw = &R->views[v];
        vw->follow = -1;
        if (v < rm->nviews) {
            const RoomView *s = &rm->views[v];
            vw->enabled = vw->visible = s->enabled;
            vw->x = s->vx; vw->y = s->vy; vw->w = s->vw; vw->h = s->vh;
            vw->port_x = s->px; vw->port_y = s->py; vw->port_w = s->pw; vw->port_h = s->ph;
            vw->border_x = s->border_x; vw->border_y = s->border_y;
            vw->speed_x = s->speed_x; vw->speed_y = s->speed_y;
            vw->follow = s->follow;
        }
        if (vw->w <= 0) vw->w = R->window_w;
        if (vw->h <= 0) vw->h = R->window_h;
        if (vw->port_w <= 0) vw->port_w = R->window_w;
        if (vw->port_h <= 0) vw->port_h = R->window_h;
    }
}

static void setup_layers(const Room *rm) {
    R->nlayers = 0;
    for (int32_t l = 0; l < rm->nlayers; l++) {
        const RoomLayer *src = &rm->layers[l];
        if (R->nlayers == R->cap_layers) {
            R->cap_layers = R->cap_layers ? R->cap_layers * 2 : 32;
            R->layers = d3_realloc(R->layers, (size_t)R->cap_layers * sizeof(Layer));
        }
        Layer *L = &R->layers[R->nlayers++];
        memset(L, 0, sizeof *L);
        L->id = (int32_t)src->id;
        snprintf(L->name, sizeof L->name, "%s", src->name);
        L->depth = src->depth;
        L->visible = src->visible;
        L->x = src->x_offset; L->y = src->y_offset;
        L->hspeed = src->h_speed; L->vspeed = src->v_speed;
        L->type = src->type;
        L->src = src;
        L->bg_sprite = src->bg.sprite;
        L->bg_color = src->bg.color;
        L->bg_index = src->bg.first_frame;
        L->bg_speed = src->bg.anim_speed;
        L->bg_xscale = L->bg_yscale = 1;
        L->bg_visible = src->bg.visible;
        L->bg_htiled = src->bg.tile_h;
        L->bg_vtiled = src->bg.tile_v;
        L->bg_stretch = src->bg.stretch;
        if (L->id >= R->next_layer_id) R->next_layer_id = L->id + 1;
    }
}

static const Layer *layer_of_room_instance(const Room *rm, uint32_t inst_id, int32_t *index) {
    for (int32_t l = 0; l < rm->nlayers; l++) {
        const RoomLayer *L = &rm->layers[l];
        if (L->type != LAYER_INSTANCES) continue;
        for (int32_t k = 0; k < L->ninst; k++)
            if (L->inst_ids[k] == inst_id) { *index = l; return &R->layers[l]; }
    }
    return NULL;
}

static void room_end_current(void) {
    if (R->room >= 0) {
        event_all(EV_OTHER, OTHER_ROOM_END);
        for (int32_t k = 0; k < R->ninsts; k++) {
            Instance *i = R->insts[k];
            if (!i->persistent && !i->destroyed) {
                event_run(i, EV_CLEANUP, 0, NULL);
                i->destroyed = true;
            }
        }
        instances_compact();
    }
}

static void room_start(int32_t room) {
    DataWin *dw = &R->dw;
    const Room *rm = &dw->rooms[room];
    R->room = room;
    R->room_speed = rm->speed ? rm->speed : 30;
    R->room_width = rm->width;
    R->room_height = rm->height;
    R->bg_color = rm->bg_color;
    R->draw_bg_color = rm->draw_bg_color;
    setup_views(rm);
    setup_layers(rm);
    LOGI("Room %d: %s (%ux%u, %d instances)", room, rm->name, rm->width, rm->height, rm->ninst);

    /* Create all room instances first, then run their events, in data order. */
    int32_t n = rm->ninst;
    Instance **created = d3_calloc((size_t)(n ? n : 1), sizeof(Instance *));
    for (int32_t k = 0; k < n; k++) {
        const RoomInst *ri = &rm->inst[k];
        if (ri->obj < 0 || ri->obj >= dw->nobjects) continue;
        if (instance_find_id((int32_t)ri->id)) continue; /* persistent copy already exists */
        Instance *i = instance_alloc((int32_t)ri->id, ri->obj, ri->x, ri->y);
        i->image_xscale = ri->scale_x;
        i->image_yscale = ri->scale_y;
        i->image_angle = ri->rotation;
        i->image_index = ri->image_index;
        i->image_speed = ri->image_speed;
        i->image_blend = ri->color & 0xFFFFFF;
        i->image_alpha = ((ri->color >> 24) & 0xFF) / 255.0;
        if ((ri->color >> 24) == 0) i->image_alpha = 1;
        int32_t li = -1;
        const Layer *L = layer_of_room_instance(rm, ri->id, &li);
        if (L) { i->depth = L->depth; i->layer = L->id; }
        created[k] = i;
        obj_retain(&i->base);
    }
    for (int32_t k = 0; k < n; k++) {
        Instance *i = created[k];
        if (!i) continue;
        const RoomInst *ri = &rm->inst[k];
        if (ri->pre_create_code >= 0) v_release(vm_call_code(ri->pre_create_code, &i->base, &i->base, 0, NULL));
        event_run(i, EV_PRECREATE, 0, NULL);
        event_run(i, EV_CREATE, 0, NULL);
        if (ri->creation_code >= 0 && !i->destroyed)
            v_release(vm_call_code(ri->creation_code, &i->base, &i->base, 0, NULL));
        i->created = true;
    }
    for (int32_t k = 0; k < n; k++) if (created[k]) obj_release(&created[k]->base);
    free(created);

    if (!R->first_room_started) {
        R->first_room_started = true;
        event_all(EV_OTHER, OTHER_GAME_START);
    }
    if (rm->creation_code >= 0) v_release(vm_call_code(rm->creation_code, &R->global, &R->global, 0, NULL));
    event_all(EV_OTHER, OTHER_ROOM_START);
    instances_compact();
}

static void do_room_change(void) {
    R->room_change_pending = false;
    room_end_current();
    plat_room_changed();
    room_start(R->next_room);
}

/* ------------------------------------------------------------------ */
/* per-frame logic                                                     */
/* ------------------------------------------------------------------ */

static void update_motion(Instance *i) {
    i->xprevious = i->x;
    i->yprevious = i->y;
    if (i->friction != 0 && i->speed != 0) {
        double ns = i->speed > 0 ? fmax(0, i->speed - i->friction) : fmin(0, i->speed + i->friction);
        double rad = i->direction * M_PI / 180.0;
        i->speed = ns;
        i->hspeed = ns * cos(rad);
        i->vspeed = -ns * sin(rad);
    }
    if (i->gravity != 0) {
        double rad = i->gravity_direction * M_PI / 180.0;
        i->hspeed += i->gravity * cos(rad);
        i->vspeed -= i->gravity * sin(rad);
        i->speed = sqrt(i->hspeed * i->hspeed + i->vspeed * i->vspeed);
        if (i->speed != 0) i->direction = fmod(atan2(-i->vspeed, i->hspeed) * 180.0 / M_PI + 360.0, 360.0);
    }
    i->x += i->hspeed;
    i->y += i->vspeed;
}

static void update_animation(Instance *i) {
    if (i->sprite_index < 0 || i->sprite_index >= R->dw.nsprites) return;
    const Sprite *s = &R->dw.sprites[i->sprite_index];
    if (s->nframes <= 0) return;
    double spd = i->image_speed;
    if (s->speed_type == 0) spd *= s->speed / (R->room_speed > 0 ? R->room_speed : 30);
    else spd *= s->speed;
    i->image_index += spd;
    if (i->image_index >= s->nframes) {
        i->image_index -= s->nframes;
        if (i->image_index >= s->nframes) i->image_index = 0;
        event_run(i, EV_OTHER, OTHER_ANIMATION_END, NULL);
    } else if (i->image_index < 0) {
        i->image_index += s->nframes;
        if (i->image_index < 0) i->image_index = 0;
        event_run(i, EV_OTHER, OTHER_ANIMATION_END, NULL);
    }
}

static void update_views(void) {
    for (int v = 0; v < MAX_VIEWS; v++) {
        View *vw = &R->views[v];
        if (!vw->enabled || vw->follow < 0) continue;
        Obj *o = resolve_single(v_real(vw->follow));
        Instance *t = as_instance(o);
        if (!t) continue;
        double bx = vw->border_x, by = vw->border_y;
        if (bx * 2 > vw->w) bx = vw->w / 2;
        if (by * 2 > vw->h) by = vw->h / 2;
        double nx = vw->x, ny = vw->y;
        if (t->x - nx < bx) nx = t->x - bx;
        if (nx + vw->w - t->x < bx) nx = t->x + bx - vw->w;
        if (t->y - ny < by) ny = t->y - by;
        if (ny + vw->h - t->y < by) ny = t->y + by - vw->h;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + vw->w > R->room_width) nx = R->room_width - vw->w;
        if (ny + vw->h > R->room_height) ny = R->room_height - vw->h;
        vw->x = nx;
        vw->y = ny;
    }
}

static void run_collisions(void) {
    DataWin *dw = &R->dw;
    int32_t n = R->ninsts;
    for (int32_t k = 0; k < n && k < R->ninsts; k++) {
        Instance *a = R->insts[k];
        if (a->destroyed) continue;
        /* collect collision events along the parent chain */
        for (int32_t *anc = R->ancestors[a->object_index]; *anc >= 0; anc++) {
            const Object *o = &dw->objects[*anc];
            for (int32_t e = 0; e < o->nevents[EV_COLLISION]; e++) {
                int32_t target = (int32_t)o->events[EV_COLLISION][e].subtype;
                double l1, t1, r1, b1;
                if (!instance_bbox(a, a->x, a->y, &l1, &t1, &r1, &b1)) continue;
                for (int32_t m = 0; m < R->ninsts; m++) {
                    Instance *b = R->insts[m];
                    if (b == a || b->destroyed || !object_is_ancestor(b->object_index, target)) continue;
                    double l2, t2, r2, b2;
                    if (!instance_bbox(b, b->x, b->y, &l2, &t2, &r2, &b2)) continue;
                    if (l1 <= r2 && l2 <= r1 && t1 <= b2 && t2 <= b1) {
                        if (b->solid || a->solid) { a->x = a->xprevious; a->y = a->yprevious; }
                        Value r = vm_call_code(o->events[EV_COLLISION][e].code, &a->base, &b->base, 0, NULL);
                        v_release(r);
                        if (a->destroyed) break;
                    }
                }
            }
        }
    }
}

static void run_keyboard_events(void) {
    /* Only dispatch to objects that actually have keyboard events. */
    for (int key = 1; key < 256; key++) {
        bool down = R->keys[key], prev = R->keys_prev[key];
        if (!down && !prev) continue;
        int32_t n = R->ninsts;
        for (int32_t k = 0; k < n && k < R->ninsts; k++) {
            Instance *i = R->insts[k];
            if (i->destroyed) continue;
            if (down) event_run(i, EV_KEYBOARD, key, NULL);
            if (down && !prev) event_run(i, EV_KEYPRESS, key, NULL);
            if (!down && prev) event_run(i, EV_KEYRELEASE, key, NULL);
        }
    }
}

static void run_alarms(void) {
    int32_t n = R->ninsts;
    for (int32_t k = 0; k < n && k < R->ninsts; k++) {
        Instance *i = R->insts[k];
        for (int a = 0; a < MAX_ALARMS; a++) {
            if (i->destroyed) break;
            if (i->alarm[a] > 0) {
                i->alarm[a] -= 1;
                if (i->alarm[a] <= 0) {
                    i->alarm[a] = -1;
                    event_run(i, EV_ALARM, a, NULL);
                }
            }
        }
    }
}

static void read_input(void) {
    memcpy(R->keys_prev, R->keys, sizeof R->keys);
    plat_input_state(R->keys);
    R->any_key_prev = R->any_key;
    R->any_key = false;
    for (int k = 1; k < 256; k++) {
        if (R->keys[k]) {
            R->any_key = true;
            if (!R->keys_prev[k]) R->last_key = k;
        }
    }
    R->keys[0] = !R->any_key; /* vk_nokey */
    R->keys[1] = R->any_key;  /* vk_anykey */
}

bool runtime_frame(void) {
    uint64_t t0 = plat_time_us();
    read_input();

    event_all(EV_STEP, STEP_BEGIN);
    run_alarms();
    run_keyboard_events();
    event_all(EV_STEP, STEP_NORMAL);
    for (int32_t k = 0; k < R->ninsts; k++)
        if (!R->insts[k]->destroyed) update_motion(R->insts[k]);
    run_collisions();
    event_all(EV_STEP, STEP_END);
    for (int32_t k = 0; k < R->ninsts; k++)
        if (!R->insts[k]->destroyed) update_animation(R->insts[k]);
    instances_compact();
    update_views();
    audio_update();

    runtime_draw();
    instances_compact();

    if (R->game_end_pending) return false;
    if (R->game_restart_pending) {
        R->game_restart_pending = false;
        for (int32_t k = 0; k < R->ninsts; k++) R->insts[k]->destroyed = true;
        instances_compact();
        R->first_room_started = false;
        R->room = -1;
        room_goto(R->dw.room_order[0]);
    }
    if (R->room_change_pending) do_room_change();

    R->frame_count++;
    uint64_t t1 = plat_time_us();
    double dt = (double)(t1 - t0) / 1e6;
    R->fps_real = dt > 0 ? 1.0 / dt : 9999;
    return true;
}

/* ------------------------------------------------------------------ */
/* init / shutdown                                                     */
/* ------------------------------------------------------------------ */

bool runtime_init(const char *path, char *err, size_t errlen) {
    R = d3_calloc(1, sizeof(Runtime));
    if (!datawin_load(&R->dw, path, err, errlen)) return false;
    DataWin *dw = &R->dw;

    R->global.rc = -1; /* never freed */
    R->global.kind = OBJ_GLOBAL;
    varmap_init(&R->global.vars);
    R->statics = d3_calloc((size_t)dw->ncode + 1, sizeof(VarMap *));
    R->statics_init = d3_calloc((size_t)dw->ncode + 1, sizeof(bool));
    R->saved_aref[0] = R->saved_aref[1] = v_undef();
    R->room = -1;
    R->ev_object = -1;
    R->window_w = dw->window_w ? (int32_t)dw->window_w : 640;
    R->window_h = dw->window_h ? (int32_t)dw->window_h : 480;
    R->fps = dw->gms2_fps;
    R->start_us = plat_time_us();
    R->random_seed = 1;

    /* instance ids must not collide with any room instance id */
    int32_t max_id = INSTANCE_ID_BASE;
    for (int32_t r = 0; r < dw->nrooms; r++)
        for (int32_t k = 0; k < dw->rooms[r].ninst; k++)
            if ((int32_t)dw->rooms[r].inst[k].id > max_id) max_id = (int32_t)dw->rooms[r].inst[k].id;
    R->next_instance_id = max_id + 1;

    build_ancestors();
    builtins_register_all();
    vm_link();
    draw_init_state();
    audio_init_groups();
    plat_set_window_size(R->window_w, R->window_h);
    if (!plat_textures_open(dw->dir, dw->ntpag)) LOGW("No converted textures found; drawing placeholders");

    /* global init scripts (2.3 script files: define functions/globals) */
    for (int32_t i = 0; i < dw->nglobal_init; i++)
        v_release(vm_call_code(dw->global_init[i], &R->global, &R->global, 0, NULL));

    if (dw->nroom_order <= 0) { snprintf(err, errlen, "Game has no rooms"); return false; }
    R->next_room = dw->room_order[0];
    do_room_change();
    return true;
}

void runtime_shutdown(void) {
    if (!R) return;
    for (int32_t k = 0; k < R->ninsts; k++) R->insts[k]->destroyed = true;
    instances_compact();
    if (R->statics) audio_shutdown();
    plat_textures_close();
    varmap_free(&R->global.vars);
    for (int32_t i = 0; R->statics && i < R->dw.ncode; i++)
        if (R->statics[i]) { varmap_free(R->statics[i]); free(R->statics[i]); }
    free(R->statics);
    free(R->statics_init);
    for (int32_t o = 0; R->ancestors && o < R->dw.nobjects; o++) free(R->ancestors[o]);
    free(R->ancestors);
    free(R->insts);
    free(R->id_keys);
    free(R->id_vals);
    free(R->layers);
    free(R->stack);
    free(R->envs);
    free(R->script_code_by_name);
    datawin_free(&R->dw);
    free(R);
    R = NULL;
}
