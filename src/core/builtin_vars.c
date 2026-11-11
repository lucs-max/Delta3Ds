/*
 * Built-in variables (x, y, sprite_index, room, view_xview, ...).
 */
#include "runtime.h"
#include "../platform/platform.h"
#include <time.h>

typedef enum {
    /* instance variables */
    BV_X, BV_Y, BV_XPREVIOUS, BV_YPREVIOUS, BV_XSTART, BV_YSTART, BV_HSPEED, BV_VSPEED, BV_SPEED,
    BV_DIRECTION, BV_FRICTION, BV_GRAVITY, BV_GRAVITY_DIRECTION, BV_SPRITE_INDEX, BV_MASK_INDEX,
    BV_IMAGE_INDEX, BV_IMAGE_SPEED, BV_IMAGE_XSCALE, BV_IMAGE_YSCALE, BV_IMAGE_ANGLE, BV_IMAGE_ALPHA,
    BV_IMAGE_BLEND, BV_IMAGE_NUMBER, BV_DEPTH, BV_LAYER, BV_VISIBLE, BV_SOLID, BV_PERSISTENT,
    BV_OBJECT_INDEX, BV_ID, BV_ALARM, BV_SPRITE_WIDTH, BV_SPRITE_HEIGHT, BV_SPRITE_XOFFSET,
    BV_SPRITE_YOFFSET, BV_BBOX_LEFT, BV_BBOX_RIGHT, BV_BBOX_TOP, BV_BBOX_BOTTOM, BV_IMAGE_SINGLE,
    /* globals */
    BV_ROOM, BV_ROOM_SPEED, BV_ROOM_WIDTH, BV_ROOM_HEIGHT, BV_ROOM_FIRST, BV_ROOM_LAST,
    BV_ROOM_PERSISTENT, BV_FPS, BV_FPS_REAL, BV_CURRENT_TIME, BV_DELTA_TIME, BV_INSTANCE_COUNT,
    BV_KEYBOARD_KEY, BV_KEYBOARD_LASTKEY, BV_VIEW_ENABLED, BV_VIEW_CURRENT, BV_VIEW_VISIBLE,
    BV_VIEW_XVIEW, BV_VIEW_YVIEW, BV_VIEW_WVIEW, BV_VIEW_HVIEW, BV_VIEW_XPORT, BV_VIEW_YPORT,
    BV_VIEW_WPORT, BV_VIEW_HPORT, BV_VIEW_ANGLE, BV_VIEW_CAMERA, BV_VIEW_OBJECT, BV_BACKGROUND_COLOR,
    BV_BACKGROUND_SHOWCOLOR, BV_OS_TYPE, BV_OS_DEVICE, BV_OS_BROWSER, BV_WORKING_DIRECTORY,
    BV_PROGRAM_DIRECTORY, BV_GAME_SAVE_ID, BV_GAME_DISPLAY_NAME, BV_SCORE, BV_LIVES, BV_HEALTH,
    BV_DEBUG_MODE, BV_MOUSE_X, BV_MOUSE_Y, BV_APPLICATION_SURFACE, BV_CURRENT_YEAR,
    BV_CURRENT_MONTH, BV_CURRENT_DAY, BV_CURRENT_HOUR, BV_CURRENT_MINUTE, BV_CURRENT_SECOND,
    BV_CURRENT_WEEKDAY, BV_TEMP_DIRECTORY, BV_EVENT_TYPE, BV_EVENT_NUMBER, BV_EVENT_OBJECT,
    BV_ASYNC_LOAD, BV_OS_VERSION, BV_GAMEPAD_COUNT, BV_COUNT
} BVar;

typedef struct { const char *name; BVar id; bool array; } BVarDef;

static const BVarDef g_bvars[] = {
    {"x", BV_X, 0}, {"y", BV_Y, 0}, {"xprevious", BV_XPREVIOUS, 0}, {"yprevious", BV_YPREVIOUS, 0},
    {"xstart", BV_XSTART, 0}, {"ystart", BV_YSTART, 0}, {"hspeed", BV_HSPEED, 0}, {"vspeed", BV_VSPEED, 0},
    {"speed", BV_SPEED, 0}, {"direction", BV_DIRECTION, 0}, {"friction", BV_FRICTION, 0},
    {"gravity", BV_GRAVITY, 0}, {"gravity_direction", BV_GRAVITY_DIRECTION, 0},
    {"sprite_index", BV_SPRITE_INDEX, 0}, {"mask_index", BV_MASK_INDEX, 0}, {"image_index", BV_IMAGE_INDEX, 0},
    {"image_speed", BV_IMAGE_SPEED, 0}, {"image_xscale", BV_IMAGE_XSCALE, 0}, {"image_yscale", BV_IMAGE_YSCALE, 0},
    {"image_angle", BV_IMAGE_ANGLE, 0}, {"image_alpha", BV_IMAGE_ALPHA, 0}, {"image_blend", BV_IMAGE_BLEND, 0},
    {"image_number", BV_IMAGE_NUMBER, 0}, {"image_single", BV_IMAGE_SINGLE, 0}, {"depth", BV_DEPTH, 0},
    {"layer", BV_LAYER, 0}, {"visible", BV_VISIBLE, 0}, {"solid", BV_SOLID, 0}, {"persistent", BV_PERSISTENT, 0},
    {"object_index", BV_OBJECT_INDEX, 0}, {"id", BV_ID, 0}, {"alarm", BV_ALARM, 1},
    {"sprite_width", BV_SPRITE_WIDTH, 0}, {"sprite_height", BV_SPRITE_HEIGHT, 0},
    {"sprite_xoffset", BV_SPRITE_XOFFSET, 0}, {"sprite_yoffset", BV_SPRITE_YOFFSET, 0},
    {"bbox_left", BV_BBOX_LEFT, 0}, {"bbox_right", BV_BBOX_RIGHT, 0}, {"bbox_top", BV_BBOX_TOP, 0},
    {"bbox_bottom", BV_BBOX_BOTTOM, 0},
    {"room", BV_ROOM, 0}, {"room_speed", BV_ROOM_SPEED, 0}, {"room_width", BV_ROOM_WIDTH, 0},
    {"room_height", BV_ROOM_HEIGHT, 0}, {"room_first", BV_ROOM_FIRST, 0}, {"room_last", BV_ROOM_LAST, 0},
    {"room_persistent", BV_ROOM_PERSISTENT, 0}, {"fps", BV_FPS, 0}, {"fps_real", BV_FPS_REAL, 0},
    {"current_time", BV_CURRENT_TIME, 0}, {"delta_time", BV_DELTA_TIME, 0},
    {"instance_count", BV_INSTANCE_COUNT, 0}, {"keyboard_key", BV_KEYBOARD_KEY, 0},
    {"keyboard_lastkey", BV_KEYBOARD_LASTKEY, 0}, {"view_enabled", BV_VIEW_ENABLED, 0},
    {"view_current", BV_VIEW_CURRENT, 0}, {"view_visible", BV_VIEW_VISIBLE, 1},
    {"view_xview", BV_VIEW_XVIEW, 1}, {"view_yview", BV_VIEW_YVIEW, 1}, {"view_wview", BV_VIEW_WVIEW, 1},
    {"view_hview", BV_VIEW_HVIEW, 1}, {"view_xport", BV_VIEW_XPORT, 1}, {"view_yport", BV_VIEW_YPORT, 1},
    {"view_wport", BV_VIEW_WPORT, 1}, {"view_hport", BV_VIEW_HPORT, 1}, {"view_angle", BV_VIEW_ANGLE, 1},
    {"view_camera", BV_VIEW_CAMERA, 1}, {"view_object", BV_VIEW_OBJECT, 1},
    {"background_color", BV_BACKGROUND_COLOR, 0}, {"background_colour", BV_BACKGROUND_COLOR, 0},
    {"background_showcolor", BV_BACKGROUND_SHOWCOLOR, 0}, {"background_showcolour", BV_BACKGROUND_SHOWCOLOR, 0},
    {"os_type", BV_OS_TYPE, 0}, {"os_device", BV_OS_DEVICE, 0}, {"os_browser", BV_OS_BROWSER, 0},
    {"os_version", BV_OS_VERSION, 0},
    {"working_directory", BV_WORKING_DIRECTORY, 0}, {"program_directory", BV_PROGRAM_DIRECTORY, 0},
    {"temp_directory", BV_TEMP_DIRECTORY, 0}, {"game_save_id", BV_GAME_SAVE_ID, 0},
    {"game_display_name", BV_GAME_DISPLAY_NAME, 0}, {"score", BV_SCORE, 0}, {"lives", BV_LIVES, 0},
    {"health", BV_HEALTH, 0}, {"debug_mode", BV_DEBUG_MODE, 0}, {"mouse_x", BV_MOUSE_X, 0},
    {"mouse_y", BV_MOUSE_Y, 0}, {"application_surface", BV_APPLICATION_SURFACE, 0},
    {"current_year", BV_CURRENT_YEAR, 0}, {"current_month", BV_CURRENT_MONTH, 0},
    {"current_day", BV_CURRENT_DAY, 0}, {"current_hour", BV_CURRENT_HOUR, 0},
    {"current_minute", BV_CURRENT_MINUTE, 0}, {"current_second", BV_CURRENT_SECOND, 0},
    {"current_weekday", BV_CURRENT_WEEKDAY, 0}, {"event_type", BV_EVENT_TYPE, 0},
    {"event_number", BV_EVENT_NUMBER, 0}, {"event_object", BV_EVENT_OBJECT, 0},
    {"async_load", BV_ASYNC_LOAD, 0}, {"gamepad_count", BV_GAMEPAD_COUNT, 0},
};

static double g_score, g_lives = -1, g_health = 100;

int32_t bvar_lookup(const char *name) {
    static int32_t *ids;
    static int n;
    if (!ids) {
        n = (int)D3_ARRAY_LEN(g_bvars);
        ids = d3_malloc((size_t)n * sizeof(int32_t));
        for (int i = 0; i < n; i++) ids[i] = d3_intern(g_bvars[i].name);
    }
    int id = d3_intern(name);
    for (int i = 0; i < n; i++)
        if (ids[i] == id) return (int32_t)g_bvars[i].id;
    return -1;
}

bool bvar_is_array(int32_t id) {
    for (size_t i = 0; i < D3_ARRAY_LEN(g_bvars); i++)
        if ((int32_t)g_bvars[i].id == id) return g_bvars[i].array;
    return false;
}

static const Sprite *inst_sprite(Instance *i) {
    if (!i || i->sprite_index < 0 || i->sprite_index >= R->dw.nsprites) return NULL;
    return &R->dw.sprites[i->sprite_index];
}

static void sync_speed_from_components(Instance *i) {
    i->speed = sqrt(i->hspeed * i->hspeed + i->vspeed * i->vspeed);
    if (i->hspeed != 0 || i->vspeed != 0) {
        double d = atan2(-i->vspeed, i->hspeed) * 180.0 / M_PI;
        if (d < 0) d += 360;
        i->direction = d;
    }
}

static void sync_components_from_speed(Instance *i) {
    double rad = i->direction * M_PI / 180.0;
    i->hspeed = i->speed * cos(rad);
    i->vspeed = -i->speed * sin(rad);
    if (fabs(i->hspeed) < 1e-10) i->hspeed = 0;
    if (fabs(i->vspeed) < 1e-10) i->vspeed = 0;
}

static View *view_at(int32_t index) {
    if (index < 0) index = 0;
    if (index >= MAX_VIEWS) index = MAX_VIEWS - 1;
    return &R->views[index];
}

static Value inst_get(BVar id, Instance *i, int32_t index) {
    const Sprite *s = inst_sprite(i);
    double l, t, r, b;
    switch (id) {
    case BV_X: return v_real(i->x);
    case BV_Y: return v_real(i->y);
    case BV_XPREVIOUS: return v_real(i->xprevious);
    case BV_YPREVIOUS: return v_real(i->yprevious);
    case BV_XSTART: return v_real(i->xstart);
    case BV_YSTART: return v_real(i->ystart);
    case BV_HSPEED: return v_real(i->hspeed);
    case BV_VSPEED: return v_real(i->vspeed);
    case BV_SPEED: return v_real(i->speed);
    case BV_DIRECTION: return v_real(i->direction);
    case BV_FRICTION: return v_real(i->friction);
    case BV_GRAVITY: return v_real(i->gravity);
    case BV_GRAVITY_DIRECTION: return v_real(i->gravity_direction);
    case BV_SPRITE_INDEX: return v_real(i->sprite_index);
    case BV_MASK_INDEX: return v_real(i->mask_index);
    case BV_IMAGE_INDEX: return v_real(i->image_index);
    case BV_IMAGE_SPEED: return v_real(i->image_speed);
    case BV_IMAGE_SINGLE: return v_real(i->image_speed == 0 ? i->image_index : -1);
    case BV_IMAGE_XSCALE: return v_real(i->image_xscale);
    case BV_IMAGE_YSCALE: return v_real(i->image_yscale);
    case BV_IMAGE_ANGLE: return v_real(i->image_angle);
    case BV_IMAGE_ALPHA: return v_real(i->image_alpha);
    case BV_IMAGE_BLEND: return v_real(i->image_blend);
    case BV_IMAGE_NUMBER: return v_real(s ? s->nframes : 0);
    case BV_DEPTH: return v_real(i->depth);
    case BV_LAYER: return v_real(i->layer);
    case BV_VISIBLE: return v_bool(i->visible);
    case BV_SOLID: return v_bool(i->solid);
    case BV_PERSISTENT: return v_bool(i->persistent);
    case BV_OBJECT_INDEX: return v_real(i->object_index);
    case BV_ID: return v_real(i->id);
    case BV_ALARM: return v_real((index >= 0 && index < MAX_ALARMS) ? i->alarm[index] : -1);
    case BV_SPRITE_WIDTH: return v_real(s ? s->w * fabs(i->image_xscale) : 0);
    case BV_SPRITE_HEIGHT: return v_real(s ? s->h * fabs(i->image_yscale) : 0);
    case BV_SPRITE_XOFFSET: return v_real(s ? s->origin_x : 0);
    case BV_SPRITE_YOFFSET: return v_real(s ? s->origin_y : 0);
    case BV_BBOX_LEFT: return v_real(instance_bbox(i, i->x, i->y, &l, &t, &r, &b) ? floor(l) : i->x);
    case BV_BBOX_TOP: return v_real(instance_bbox(i, i->x, i->y, &l, &t, &r, &b) ? floor(t) : i->y);
    case BV_BBOX_RIGHT: return v_real(instance_bbox(i, i->x, i->y, &l, &t, &r, &b) ? ceil(r) - 1 : i->x);
    case BV_BBOX_BOTTOM: return v_real(instance_bbox(i, i->x, i->y, &l, &t, &r, &b) ? ceil(b) - 1 : i->y);
    default: return v_undef();
    }
}

static void inst_set(BVar id, Instance *i, int32_t index, double d) {
    switch (id) {
    case BV_X: i->x = d; break;
    case BV_Y: i->y = d; break;
    case BV_XPREVIOUS: i->xprevious = d; break;
    case BV_YPREVIOUS: i->yprevious = d; break;
    case BV_XSTART: i->xstart = d; break;
    case BV_YSTART: i->ystart = d; break;
    case BV_HSPEED: i->hspeed = d; sync_speed_from_components(i); break;
    case BV_VSPEED: i->vspeed = d; sync_speed_from_components(i); break;
    case BV_SPEED: i->speed = d; sync_components_from_speed(i); break;
    case BV_DIRECTION:
        d = fmod(d, 360.0);
        if (d < 0) d += 360;
        i->direction = d;
        sync_components_from_speed(i);
        break;
    case BV_FRICTION: i->friction = d; break;
    case BV_GRAVITY: i->gravity = d; break;
    case BV_GRAVITY_DIRECTION: i->gravity_direction = d; break;
    case BV_SPRITE_INDEX: i->sprite_index = (int32_t)d; break;
    case BV_MASK_INDEX: i->mask_index = (int32_t)d; break;
    case BV_IMAGE_INDEX: i->image_index = d; break;
    case BV_IMAGE_SPEED: i->image_speed = d; break;
    case BV_IMAGE_SINGLE:
        if (d >= 0) { i->image_index = d; i->image_speed = 0; } else i->image_speed = 1;
        break;
    case BV_IMAGE_XSCALE: i->image_xscale = d; break;
    case BV_IMAGE_YSCALE: i->image_yscale = d; break;
    case BV_IMAGE_ANGLE: i->image_angle = d; break;
    case BV_IMAGE_ALPHA: i->image_alpha = d; break;
    case BV_IMAGE_BLEND: i->image_blend = (uint32_t)(int64_t)d & 0xFFFFFF; break;
    case BV_DEPTH: i->depth = d; break;
    case BV_LAYER: i->layer = (int32_t)d; break;
    case BV_VISIBLE: i->visible = d > 0.5; break;
    case BV_SOLID: i->solid = d > 0.5; break;
    case BV_PERSISTENT: i->persistent = d > 0.5; break;
    case BV_ALARM: if (index >= 0 && index < MAX_ALARMS) i->alarm[index] = d; break;
    default: break; /* read-only */
    }
}

static int64_t now_field(int which) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm) return 0;
    switch (which) {
    case 0: return tm->tm_year + 1900;
    case 1: return tm->tm_mon + 1;
    case 2: return tm->tm_mday;
    case 3: return tm->tm_hour;
    case 4: return tm->tm_min;
    case 5: return tm->tm_sec;
    default: return tm->tm_wday;
    }
}

Value bvar_get(int32_t bid, Obj *self, int32_t index) {
    BVar id = (BVar)bid;
    if (id < BV_ROOM) {
        Instance *i = as_instance(self);
        if (!i) {
            /* builtin instance var read on a struct/global: plain member */
            if (self) {
                Value *p = varmap_get(&self->vars, d3_intern(g_bvars[0].name)); /* never used */
                (void)p;
            }
            return v_undef();
        }
        return inst_get(id, i, index);
    }
    int32_t vi = index < 0 ? 0 : index;
    switch (id) {
    case BV_ROOM: return v_real(R->room);
    case BV_ROOM_SPEED: return v_real(R->room_speed);
    case BV_ROOM_WIDTH: return v_real(R->room_width);
    case BV_ROOM_HEIGHT: return v_real(R->room_height);
    case BV_ROOM_FIRST: return v_real(R->dw.nroom_order ? R->dw.room_order[0] : 0);
    case BV_ROOM_LAST: return v_real(R->dw.nroom_order ? R->dw.room_order[R->dw.nroom_order - 1] : 0);
    case BV_ROOM_PERSISTENT: return v_bool(false);
    case BV_FPS: return v_real(R->room_speed);
    case BV_FPS_REAL: return v_real(R->fps_real);
    case BV_CURRENT_TIME: return v_real((double)((plat_time_us() - R->start_us) / 1000));
    case BV_DELTA_TIME: return v_real(1e6 / (R->room_speed > 0 ? R->room_speed : 30));
    case BV_INSTANCE_COUNT: return v_real(R->ninsts);
    case BV_KEYBOARD_KEY: {
        for (int k = 255; k >= 8; k--) if (R->keys[k]) return v_real(k);
        return v_real(0);
    }
    case BV_KEYBOARD_LASTKEY: return v_real(R->last_key);
    case BV_VIEW_ENABLED: return v_bool(R->views_enabled);
    case BV_VIEW_CURRENT: return v_real(R->view_current);
    case BV_VIEW_VISIBLE: return v_bool(view_at(vi)->visible);
    case BV_VIEW_XVIEW: return v_real(view_at(vi)->x);
    case BV_VIEW_YVIEW: return v_real(view_at(vi)->y);
    case BV_VIEW_WVIEW: return v_real(view_at(vi)->w);
    case BV_VIEW_HVIEW: return v_real(view_at(vi)->h);
    case BV_VIEW_XPORT: return v_real(view_at(vi)->port_x);
    case BV_VIEW_YPORT: return v_real(view_at(vi)->port_y);
    case BV_VIEW_WPORT: return v_real(view_at(vi)->port_w);
    case BV_VIEW_HPORT: return v_real(view_at(vi)->port_h);
    case BV_VIEW_ANGLE: return v_real(view_at(vi)->angle);
    case BV_VIEW_CAMERA: return v_real(vi); /* camera id == view index */
    case BV_VIEW_OBJECT: return v_real(view_at(vi)->follow);
    case BV_BACKGROUND_COLOR: return v_real(R->bg_color & 0xFFFFFF);
    case BV_BACKGROUND_SHOWCOLOR: return v_bool(R->draw_bg_color);
    case BV_OS_TYPE: return v_real(6); /* os_linux: closest desktop behaviour */
    case BV_OS_DEVICE: return v_real(0);
    case BV_OS_BROWSER: return v_real(-1);
    case BV_OS_VERSION: return v_real(0);
    case BV_WORKING_DIRECTORY: case BV_PROGRAM_DIRECTORY: return v_str(R->dw.dir);
    case BV_TEMP_DIRECTORY: case BV_GAME_SAVE_ID: return v_str(plat_save_dir());
    case BV_GAME_DISPLAY_NAME: return v_str(R->dw.display_name ? R->dw.display_name : "");
    case BV_SCORE: return v_real(g_score);
    case BV_LIVES: return v_real(g_lives);
    case BV_HEALTH: return v_real(g_health);
    case BV_DEBUG_MODE: return v_bool(false);
    case BV_MOUSE_X: case BV_MOUSE_Y: return v_real(0);
    case BV_APPLICATION_SURFACE: return v_real(0);
    case BV_CURRENT_YEAR: return v_real((double)now_field(0));
    case BV_CURRENT_MONTH: return v_real((double)now_field(1));
    case BV_CURRENT_DAY: return v_real((double)now_field(2));
    case BV_CURRENT_HOUR: return v_real((double)now_field(3));
    case BV_CURRENT_MINUTE: return v_real((double)now_field(4));
    case BV_CURRENT_SECOND: return v_real((double)now_field(5));
    case BV_CURRENT_WEEKDAY: return v_real((double)now_field(6));
    case BV_EVENT_TYPE: return v_real(R->ev_type);
    case BV_EVENT_NUMBER: return v_real(R->ev_sub);
    case BV_EVENT_OBJECT: return v_real(R->ev_object);
    case BV_ASYNC_LOAD: return v_real(-1);
    case BV_GAMEPAD_COUNT: return v_real(1);
    default: return v_undef();
    }
}

void bvar_set(int32_t bid, Obj *self, int32_t index, Value v) {
    BVar id = (BVar)bid;
    double d = v_to_real(v);
    v_release(v);
    if (id < BV_ROOM) {
        Instance *i = as_instance(self);
        if (i) inst_set(id, i, index, d);
        return;
    }
    int32_t vi = index < 0 ? 0 : index;
    switch (id) {
    case BV_ROOM: room_goto((int32_t)d); break;
    case BV_ROOM_SPEED: R->room_speed = d; break;
    case BV_ROOM_WIDTH: R->room_width = (uint32_t)d; break;
    case BV_ROOM_HEIGHT: R->room_height = (uint32_t)d; break;
    case BV_VIEW_ENABLED: R->views_enabled = d > 0.5; break;
    case BV_VIEW_VISIBLE: view_at(vi)->visible = view_at(vi)->enabled = d > 0.5; break;
    case BV_VIEW_XVIEW: view_at(vi)->x = d; break;
    case BV_VIEW_YVIEW: view_at(vi)->y = d; break;
    case BV_VIEW_WVIEW: view_at(vi)->w = d; break;
    case BV_VIEW_HVIEW: view_at(vi)->h = d; break;
    case BV_VIEW_XPORT: view_at(vi)->port_x = d; break;
    case BV_VIEW_YPORT: view_at(vi)->port_y = d; break;
    case BV_VIEW_WPORT: view_at(vi)->port_w = d; break;
    case BV_VIEW_HPORT: view_at(vi)->port_h = d; break;
    case BV_VIEW_ANGLE: view_at(vi)->angle = d; break;
    case BV_VIEW_OBJECT: view_at(vi)->follow = (int32_t)d; break;
    case BV_BACKGROUND_COLOR: R->bg_color = (uint32_t)(int64_t)d; break;
    case BV_BACKGROUND_SHOWCOLOR: R->draw_bg_color = d > 0.5; break;
    case BV_SCORE: g_score = d; break;
    case BV_LIVES: g_lives = d; break;
    case BV_HEALTH: g_health = d; break;
    case BV_KEYBOARD_KEY: break;
    case BV_KEYBOARD_LASTKEY: R->last_key = (int32_t)d; break;
    default: break;
    }
}
