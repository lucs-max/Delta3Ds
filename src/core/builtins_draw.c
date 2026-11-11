/*
 * Drawing: sprites, text, primitives, room layers and the per-frame draw pass.
 * All positions are transformed from room space to game-window space here;
 * the platform then maps the window to the physical screen.
 */
#include "builtins_internal.h"
#include "../platform/platform.h"

/* current transform: window = (room - view) * k + port */
static double g_vx, g_vy, g_kx = 1, g_ky = 1, g_px, g_py;
static int g_surface_depth; /* > 0 while drawing to a (unsupported) surface */

static inline uint32_t abgr(uint32_t bgr, double alpha) {
    if (alpha < 0) alpha = 0;
    if (alpha > 1) alpha = 1;
    return (bgr & 0xFFFFFF) | ((uint32_t)(alpha * 255.0 + 0.5) << 24);
}
static inline double tx_x(double x) { return (x - g_vx) * g_kx + g_px; }
static inline double tx_y(double y) { return (y - g_vy) * g_ky + g_py; }

void draw_init_state(void) {
    R->draw_color = 0xFFFFFF;
    R->draw_alpha = 1;
    R->draw_font = -1;
    R->halign = 0;
    R->valign = 0;
}

void draw_set_view_transform(int32_t v) {
    View *vw = &R->views[v];
    g_vx = vw->x;
    g_vy = vw->y;
    g_kx = vw->w != 0 ? vw->port_w / vw->w : 1;
    g_ky = vw->h != 0 ? vw->port_h / vw->h : 1;
    g_px = vw->port_x;
    g_py = vw->port_y;
}

void draw_set_gui_transform(void) {
    double gw = R->gui_w > 0 ? R->gui_w : R->window_w, gh = R->gui_h > 0 ? R->gui_h : R->window_h;
    g_vx = g_vy = 0;
    g_kx = R->window_w / gw;
    g_ky = R->window_h / gh;
    g_px = g_py = 0;
}

/* Draws a rectangle of a TPAG item. (lx, ly) is the offset of the item's
 * top-left from the anchor (x, y) before scaling/rotation, in room pixels. */
static void draw_tpag(int32_t tpag, double sx, double sy, double sw, double sh, double x, double y, double lx, double ly,
                      double dw, double dh, double xs, double ys, double angle_deg, uint32_t color) {
    if (g_surface_depth > 0 || tpag < 0) return;
    double w = dw * xs * g_kx, h = dh * ys * g_ky;
    double px = -lx * xs * g_kx, py = -ly * ys * g_ky;
    plat_draw_image(tpag, (float)sx, (float)sy, (float)sw, (float)sh, (float)tx_x(x), (float)tx_y(y), (float)w, (float)h,
                    (float)px, (float)py, (float)(-angle_deg * M_PI / 180.0), color);
}

void draw_sprite_frame(int32_t sprite, double frame, double x, double y, double xs, double ys, double angle,
                       uint32_t color, double alpha) {
    if (sprite < 0 || sprite >= R->dw.nsprites) return;
    const Sprite *s = &R->dw.sprites[sprite];
    if (s->nframes <= 0) return;
    int32_t f = (int32_t)floor(frame);
    f %= s->nframes;
    if (f < 0) f += s->nframes;
    int32_t ti = s->frames[f];
    if (ti < 0) return;
    const TPage *t = &R->dw.tpag[ti];
    double scale_x = t->tw ? (double)t->tw / t->sw : 1, scale_y = t->th ? (double)t->th / t->sh : 1;
    (void)scale_x; (void)scale_y;
    draw_tpag(ti, 0, 0, t->sw, t->sh, x, y, t->tx - s->origin_x, t->ty - s->origin_y, t->tw, t->th, xs, ys, angle,
              abgr(color, alpha));
}

/* Part of a sprite frame: (l, t, w, h) in frame pixels. */
static void draw_sprite_part_impl(int32_t sprite, double frame, double l, double t, double w, double h, double x, double y,
                                  double xs, double ys, uint32_t color, double alpha) {
    if (sprite < 0 || sprite >= R->dw.nsprites) return;
    const Sprite *s = &R->dw.sprites[sprite];
    if (s->nframes <= 0) return;
    int32_t f = (int32_t)floor(frame) % s->nframes;
    if (f < 0) f += s->nframes;
    int32_t ti = s->frames[f];
    if (ti < 0) return;
    const TPage *tp = &R->dw.tpag[ti];
    /* intersect the requested rect with the trimmed item rect */
    double il = fmax(l, tp->tx), it = fmax(t, tp->ty);
    double ir = fmin(l + w, tp->tx + tp->tw), ib = fmin(t + h, tp->ty + tp->th);
    if (ir <= il || ib <= it) return;
    draw_tpag(ti, il - tp->tx, it - tp->ty, ir - il, ib - it, x, y, il - l, it - t, ir - il, ib - it, xs, ys, 0,
              abgr(color, alpha));
}

/* ------------------------------------------------------------------ */
/* sprite functions                                                    */
/* ------------------------------------------------------------------ */

static Value bi_draw_self(int32_t argc, Value *a) {
    (void)argc; (void)a;
    Instance *i = as_instance(cur_self());
    if (i) draw_sprite_frame(i->sprite_index, i->image_index, i->x, i->y, i->image_xscale, i->image_yscale,
                             i->image_angle, i->image_blend, i->image_alpha);
    return v_undef();
}
static double frame_arg(int32_t argc, Value *a, int32_t i) {
    double f = ARGR(argc, a, i, 0);
    if (f < 0) {
        Instance *self = as_instance(cur_self());
        return self ? self->image_index : 0;
    }
    return f;
}
static Value bi_draw_sprite(int32_t argc, Value *a) {
    draw_sprite_frame(ARGI(argc, a, 0, -1), frame_arg(argc, a, 1), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0), 1, 1, 0,
                      0xFFFFFF, R->draw_alpha);
    return v_undef();
}
static Value bi_draw_sprite_ext(int32_t argc, Value *a) {
    draw_sprite_frame(ARGI(argc, a, 0, -1), frame_arg(argc, a, 1), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0),
                      ARGR(argc, a, 4, 1), ARGR(argc, a, 5, 1), ARGR(argc, a, 6, 0), (uint32_t)ARGI(argc, a, 7, 0xFFFFFF),
                      ARGR(argc, a, 8, 1));
    return v_undef();
}
static Value bi_draw_sprite_part(int32_t argc, Value *a) {
    draw_sprite_part_impl(ARGI(argc, a, 0, -1), frame_arg(argc, a, 1), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0),
                          ARGR(argc, a, 4, 0), ARGR(argc, a, 5, 0), ARGR(argc, a, 6, 0), ARGR(argc, a, 7, 0), 1, 1,
                          0xFFFFFF, R->draw_alpha);
    return v_undef();
}
static Value bi_draw_sprite_part_ext(int32_t argc, Value *a) {
    draw_sprite_part_impl(ARGI(argc, a, 0, -1), frame_arg(argc, a, 1), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0),
                          ARGR(argc, a, 4, 0), ARGR(argc, a, 5, 0), ARGR(argc, a, 6, 0), ARGR(argc, a, 7, 0),
                          ARGR(argc, a, 8, 1), ARGR(argc, a, 9, 1), (uint32_t)ARGI(argc, a, 10, 0xFFFFFF),
                          ARGR(argc, a, 11, 1));
    return v_undef();
}
static void stretched(int32_t argc, Value *a, uint32_t color, double alpha) {
    int32_t sp = ARGI(argc, a, 0, -1);
    if (sp < 0 || sp >= R->dw.nsprites) return;
    const Sprite *s = &R->dw.sprites[sp];
    if (s->w <= 0 || s->h <= 0) return;
    double w = ARGR(argc, a, 4, 0), h = ARGR(argc, a, 5, 0);
    double xs = w / s->w, ys = h / s->h;
    /* stretched sprites ignore the origin */
    draw_sprite_frame(sp, frame_arg(argc, a, 1), ARGR(argc, a, 2, 0) + s->origin_x * xs,
                      ARGR(argc, a, 3, 0) + s->origin_y * ys, xs, ys, 0, color, alpha);
}
static Value bi_draw_sprite_stretched(int32_t argc, Value *a) { stretched(argc, a, 0xFFFFFF, R->draw_alpha); return v_undef(); }
static Value bi_draw_sprite_stretched_ext(int32_t argc, Value *a) {
    stretched(argc, a, (uint32_t)ARGI(argc, a, 6, 0xFFFFFF), ARGR(argc, a, 7, 1));
    return v_undef();
}
static void tiled(int32_t sp, double frame, double x, double y, double xs, double ys, uint32_t color, double alpha,
                  bool htile, bool vtile) {
    if (sp < 0 || sp >= R->dw.nsprites) return;
    const Sprite *s = &R->dw.sprites[sp];
    double w = s->w * fabs(xs), h = s->h * fabs(ys);
    if (w < 1 || h < 1) return;
    /* visible region in room space */
    double vl = g_vx, vt = g_vy, vr = g_vx + R->window_w / g_kx, vb = g_vy + R->window_h / g_ky;
    double sx = htile ? x - ceil((x - vl) / w) * w : x, sy = vtile ? y - ceil((y - vt) / h) * h : y;
    double ex = htile ? vr : x + 1, ey = vtile ? vb : y + 1;
    int guard = 0;
    for (double yy = sy; yy < ey && guard < 4096; yy += h)
        for (double xx = sx; xx < ex && guard < 4096; xx += w, guard++)
            draw_sprite_frame(sp, frame, xx + s->origin_x * xs, yy + s->origin_y * ys, xs, ys, 0, color, alpha);
}
static Value bi_draw_sprite_tiled(int32_t argc, Value *a) {
    tiled(ARGI(argc, a, 0, -1), frame_arg(argc, a, 1), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0), 1, 1, 0xFFFFFF,
          R->draw_alpha, true, true);
    return v_undef();
}
static Value bi_draw_sprite_tiled_ext(int32_t argc, Value *a) {
    tiled(ARGI(argc, a, 0, -1), frame_arg(argc, a, 1), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0), ARGR(argc, a, 4, 1),
          ARGR(argc, a, 5, 1), (uint32_t)ARGI(argc, a, 6, 0xFFFFFF), ARGR(argc, a, 7, 1), true, true);
    return v_undef();
}
static Value bi_draw_sprite_general(int32_t argc, Value *a) {
    /* sprite, subimg, left, top, width, height, x, y, xscale, yscale, rot, c1, c2, c3, c4, alpha */
    int32_t sp = ARGI(argc, a, 0, -1);
    if (sp < 0 || sp >= R->dw.nsprites) return v_undef();
    double rot = ARGR(argc, a, 10, 0);
    if (rot == 0) {
        draw_sprite_part_impl(sp, frame_arg(argc, a, 1), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0), ARGR(argc, a, 4, 0),
                              ARGR(argc, a, 5, 0), ARGR(argc, a, 6, 0), ARGR(argc, a, 7, 0), ARGR(argc, a, 8, 1),
                              ARGR(argc, a, 9, 1), (uint32_t)ARGI(argc, a, 11, 0xFFFFFF), ARGR(argc, a, 15, 1));
        return v_undef();
    }
    const Sprite *s = &R->dw.sprites[sp];
    if (s->nframes <= 0) return v_undef();
    int32_t f = (int32_t)floor(frame_arg(argc, a, 1)) % s->nframes;
    if (f < 0) f += s->nframes;
    int32_t ti = s->frames[f];
    if (ti < 0) return v_undef();
    const TPage *tp = &R->dw.tpag[ti];
    double l = ARGR(argc, a, 2, 0), t = ARGR(argc, a, 3, 0), w = ARGR(argc, a, 4, 0), h = ARGR(argc, a, 5, 0);
    double il = fmax(l, tp->tx), it = fmax(t, tp->ty), ir = fmin(l + w, tp->tx + tp->tw), ib = fmin(t + h, tp->ty + tp->th);
    if (ir <= il || ib <= it) return v_undef();
    draw_tpag(ti, il - tp->tx, it - tp->ty, ir - il, ib - it, ARGR(argc, a, 6, 0), ARGR(argc, a, 7, 0), il - l, it - t,
              ir - il, ib - it, ARGR(argc, a, 8, 1), ARGR(argc, a, 9, 1), rot,
              abgr((uint32_t)ARGI(argc, a, 11, 0xFFFFFF), ARGR(argc, a, 15, 1)));
    return v_undef();
}
static Value bi_draw_sprite_pos(int32_t argc, Value *a) {
    /* sprite, subimg, x1,y1 .. x4,y4, alpha: approximated by the bounding rect */
    double minx = fmin(fmin(ARGR(argc, a, 2, 0), ARGR(argc, a, 4, 0)), fmin(ARGR(argc, a, 6, 0), ARGR(argc, a, 8, 0)));
    double maxx = fmax(fmax(ARGR(argc, a, 2, 0), ARGR(argc, a, 4, 0)), fmax(ARGR(argc, a, 6, 0), ARGR(argc, a, 8, 0)));
    double miny = fmin(fmin(ARGR(argc, a, 3, 0), ARGR(argc, a, 5, 0)), fmin(ARGR(argc, a, 7, 0), ARGR(argc, a, 9, 0)));
    double maxy = fmax(fmax(ARGR(argc, a, 3, 0), ARGR(argc, a, 5, 0)), fmax(ARGR(argc, a, 7, 0), ARGR(argc, a, 9, 0)));
    int32_t sp = ARGI(argc, a, 0, -1);
    if (sp < 0 || sp >= R->dw.nsprites) return v_undef();
    const Sprite *s = &R->dw.sprites[sp];
    if (s->w <= 0 || s->h <= 0) return v_undef();
    double xs = (maxx - minx) / s->w, ys = (maxy - miny) / s->h;
    draw_sprite_frame(sp, frame_arg(argc, a, 1), minx + s->origin_x * xs, miny + s->origin_y * ys, xs, ys, 0, 0xFFFFFF,
                      ARGR(argc, a, 10, 1));
    return v_undef();
}

/* sprite info */
static const Sprite *sprite_arg(int32_t argc, Value *a) {
    int32_t s = ARGI(argc, a, 0, -1);
    return (s >= 0 && s < R->dw.nsprites) ? &R->dw.sprites[s] : NULL;
}
static Value bi_sprite_exists(int32_t argc, Value *a) { return v_bool(sprite_arg(argc, a) != NULL); }
static Value bi_sprite_get_number(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->nframes : 0); }
static Value bi_sprite_get_width(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->w : 0); }
static Value bi_sprite_get_height(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->h : 0); }
static Value bi_sprite_get_xoffset(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->origin_x : 0); }
static Value bi_sprite_get_yoffset(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->origin_y : 0); }
static Value bi_sprite_get_bbox_left(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->margin_l : 0); }
static Value bi_sprite_get_bbox_right(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->margin_r : 0); }
static Value bi_sprite_get_bbox_top(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->margin_t : 0); }
static Value bi_sprite_get_bbox_bottom(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->margin_b : 0); }
static Value bi_sprite_get_name(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_str(s ? s->name : "<undefined>"); }
static Value bi_sprite_get_speed(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->speed : 0); }
static Value bi_sprite_get_speed_type(int32_t argc, Value *a) { const Sprite *s = sprite_arg(argc, a); return v_real(s ? s->speed_type : 0); }
static Value bi_sprite_set_offset(int32_t argc, Value *a) {
    int32_t si = ARGI(argc, a, 0, -1);
    if (si >= 0 && si < R->dw.nsprites) {
        R->dw.sprites[si].origin_x = ARGI(argc, a, 1, 0);
        R->dw.sprites[si].origin_y = ARGI(argc, a, 2, 0);
    }
    return v_undef();
}

/* ------------------------------------------------------------------ */
/* draw state                                                          */
/* ------------------------------------------------------------------ */

static Value bi_draw_set_color(int32_t argc, Value *a) { R->draw_color = (uint32_t)ARGI(argc, a, 0, 0) & 0xFFFFFF; return v_undef(); }
static Value bi_draw_get_color(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->draw_color); }
static Value bi_draw_set_alpha(int32_t argc, Value *a) { R->draw_alpha = ARGR(argc, a, 0, 1); return v_undef(); }
static Value bi_draw_get_alpha(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->draw_alpha); }
static Value bi_draw_set_font(int32_t argc, Value *a) { R->draw_font = ARGI(argc, a, 0, -1); return v_undef(); }
static Value bi_draw_get_font(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->draw_font); }
static Value bi_draw_set_halign(int32_t argc, Value *a) { R->halign = ARGI(argc, a, 0, 0); return v_undef(); }
static Value bi_draw_set_valign(int32_t argc, Value *a) { R->valign = ARGI(argc, a, 0, 0); return v_undef(); }
static Value bi_draw_get_halign(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->halign); }
static Value bi_draw_get_valign(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->valign); }
static Value bi_gpu_set_blendmode(int32_t argc, Value *a) {
    R->blend_mode = ARGI(argc, a, 0, 0);
    if (g_surface_depth == 0) plat_set_blend_mode(R->blend_mode);
    return v_undef();
}
static Value bi_gpu_set_blendmode_ext(int32_t argc, Value *a) {
    /* bm_src_alpha (5) / bm_one (2) -> additive, everything else -> normal */
    int src = ARGI(argc, a, 0, 5), dst = ARGI(argc, a, 1, 6);
    R->blend_mode = (dst == 2) ? 1 : 0;
    (void)src;
    if (g_surface_depth == 0) plat_set_blend_mode(R->blend_mode);
    return v_undef();
}
static Value bi_gpu_get_blendmode(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(R->blend_mode); }

/* ------------------------------------------------------------------ */
/* primitives                                                          */
/* ------------------------------------------------------------------ */

static void rect_window(double x1, double y1, double x2, double y2, uint32_t c) {
    if (g_surface_depth > 0) return;
    double l = fmin(x1, x2), t = fmin(y1, y2), r = fmax(x1, x2), b = fmax(y1, y2);
    plat_draw_rect((float)tx_x(l), (float)tx_y(t), (float)((r - l) * g_kx), (float)((b - t) * g_ky), c);
}
static void line_room(double x1, double y1, double x2, double y2, double w, uint32_t c) {
    if (g_surface_depth > 0) return;
    plat_draw_line((float)tx_x(x1), (float)tx_y(y1), (float)tx_x(x2), (float)tx_y(y2), (float)(w * g_kx), c);
}
static void draw_rect_impl(double x1, double y1, double x2, double y2, bool outline, uint32_t c) {
    if (!outline) {
        /* GameMaker rectangles include the bottom-right pixel */
        rect_window(x1, y1, x2 + 1, y2 + 1, c);
        return;
    }
    double l = fmin(x1, x2), t = fmin(y1, y2), r = fmax(x1, x2), b = fmax(y1, y2);
    rect_window(l, t, r + 1, t + 1, c);
    rect_window(l, b, r + 1, b + 1, c);
    rect_window(l, t, l + 1, b + 1, c);
    rect_window(r, t, r + 1, b + 1, c);
}
static Value bi_draw_rectangle(int32_t argc, Value *a) {
    draw_rect_impl(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0),
                   argc > 4 && v_to_bool(a[4]), abgr(R->draw_color, R->draw_alpha));
    return v_undef();
}
static Value bi_draw_rectangle_color(int32_t argc, Value *a) {
    bool outline = argc > 8 && v_to_bool(a[8]);
    uint32_t c1 = abgr((uint32_t)ARGI(argc, a, 4, 0), R->draw_alpha), c2 = abgr((uint32_t)ARGI(argc, a, 5, 0), R->draw_alpha);
    uint32_t c3 = abgr((uint32_t)ARGI(argc, a, 6, 0), R->draw_alpha), c4 = abgr((uint32_t)ARGI(argc, a, 7, 0), R->draw_alpha);
    double x1 = ARGR(argc, a, 0, 0), y1 = ARGR(argc, a, 1, 0), x2 = ARGR(argc, a, 2, 0), y2 = ARGR(argc, a, 3, 0);
    if (outline || g_surface_depth > 0) { draw_rect_impl(x1, y1, x2, y2, outline, c1); return v_undef(); }
    double l = fmin(x1, x2), t = fmin(y1, y2), r = fmax(x1, x2) + 1, b = fmax(y1, y2) + 1;
    plat_draw_rect_gradient((float)tx_x(l), (float)tx_y(t), (float)((r - l) * g_kx), (float)((b - t) * g_ky), c1, c2, c3, c4);
    return v_undef();
}
static Value bi_draw_line(int32_t argc, Value *a) {
    line_room(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0), 1,
              abgr(R->draw_color, R->draw_alpha));
    return v_undef();
}
static Value bi_draw_line_width(int32_t argc, Value *a) {
    line_room(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0), ARGR(argc, a, 4, 1),
              abgr(R->draw_color, R->draw_alpha));
    return v_undef();
}
static Value bi_draw_line_color(int32_t argc, Value *a) {
    line_room(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0), 1,
              abgr((uint32_t)ARGI(argc, a, 4, 0), R->draw_alpha));
    return v_undef();
}
static Value bi_draw_line_width_color(int32_t argc, Value *a) {
    line_room(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), ARGR(argc, a, 2, 0), ARGR(argc, a, 3, 0), ARGR(argc, a, 4, 1),
              abgr((uint32_t)ARGI(argc, a, 5, 0), R->draw_alpha));
    return v_undef();
}
static Value bi_draw_point(int32_t argc, Value *a) {
    double x = ARGR(argc, a, 0, 0), y = ARGR(argc, a, 1, 0);
    rect_window(x, y, x + 1, y + 1, abgr(R->draw_color, R->draw_alpha));
    return v_undef();
}
static void ellipse(double cx, double cy, double rx, double ry, bool outline, uint32_t c) {
    if (g_surface_depth > 0) return;
    const int seg = 24;
    for (int i = 0; i < seg; i++) {
        double a0 = i * 2 * M_PI / seg, a1 = (i + 1) * 2 * M_PI / seg;
        double x0 = cx + cos(a0) * rx, y0 = cy + sin(a0) * ry, x1 = cx + cos(a1) * rx, y1 = cy + sin(a1) * ry;
        if (outline) line_room(x0, y0, x1, y1, 1, c);
        else plat_draw_triangle((float)tx_x(cx), (float)tx_y(cy), (float)tx_x(x0), (float)tx_y(y0), (float)tx_x(x1),
                                (float)tx_y(y1), c);
    }
}
static Value bi_draw_circle(int32_t argc, Value *a) {
    double r = ARGR(argc, a, 2, 0);
    ellipse(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), r, r, argc > 3 && v_to_bool(a[3]), abgr(R->draw_color, R->draw_alpha));
    return v_undef();
}
static Value bi_draw_circle_color(int32_t argc, Value *a) {
    double r = ARGR(argc, a, 2, 0);
    ellipse(ARGR(argc, a, 0, 0), ARGR(argc, a, 1, 0), r, r, argc > 5 && v_to_bool(a[5]),
            abgr((uint32_t)ARGI(argc, a, 4, 0), R->draw_alpha));
    return v_undef();
}
static Value bi_draw_ellipse(int32_t argc, Value *a) {
    double x1 = ARGR(argc, a, 0, 0), y1 = ARGR(argc, a, 1, 0), x2 = ARGR(argc, a, 2, 0), y2 = ARGR(argc, a, 3, 0);
    ellipse((x1 + x2) / 2, (y1 + y2) / 2, fabs(x2 - x1) / 2, fabs(y2 - y1) / 2, argc > 4 && v_to_bool(a[4]),
            abgr(R->draw_color, R->draw_alpha));
    return v_undef();
}
static Value bi_draw_triangle(int32_t argc, Value *a) {
    double p[6];
    for (int i = 0; i < 6; i++) p[i] = ARGR(argc, a, i, 0);
    uint32_t c = abgr(R->draw_color, R->draw_alpha);
    if (argc > 6 && v_to_bool(a[6])) {
        line_room(p[0], p[1], p[2], p[3], 1, c);
        line_room(p[2], p[3], p[4], p[5], 1, c);
        line_room(p[4], p[5], p[0], p[1], 1, c);
    } else if (g_surface_depth == 0) {
        plat_draw_triangle((float)tx_x(p[0]), (float)tx_y(p[1]), (float)tx_x(p[2]), (float)tx_y(p[3]), (float)tx_x(p[4]),
                           (float)tx_y(p[5]), c);
    }
    return v_undef();
}
static Value bi_draw_clear(int32_t argc, Value *a) {
    if (g_surface_depth == 0) plat_draw_rect(0, 0, (float)R->window_w, (float)R->window_h, abgr((uint32_t)ARGI(argc, a, 0, 0), 1));
    return v_undef();
}
static Value bi_draw_clear_alpha(int32_t argc, Value *a) {
    if (g_surface_depth == 0)
        plat_draw_rect(0, 0, (float)R->window_w, (float)R->window_h, abgr((uint32_t)ARGI(argc, a, 0, 0), ARGR(argc, a, 1, 1)));
    return v_undef();
}

/* ------------------------------------------------------------------ */
/* text                                                                */
/* ------------------------------------------------------------------ */

static const Font *cur_font(void) {
    int32_t f = R->draw_font;
    if (f < 0 || f >= R->dw.nfonts) f = 0;
    if (f >= R->dw.nfonts) return NULL;
    return &R->dw.fonts[f];
}

static const Glyph *font_glyph(Font *f, int ch) {
    if (!f->glyph_lut) {
        f->glyph_lut = d3_calloc(65536, sizeof(uint16_t));
        for (int32_t g = 0; g < f->nglyphs && g < 65535; g++) f->glyph_lut[f->glyphs[g].ch] = (uint16_t)(g + 1);
    }
    if (ch < 0 || ch > 0xFFFF) return NULL;
    uint16_t i = f->glyph_lut[ch];
    return i ? &f->glyphs[i - 1] : NULL;
}

static double font_line_height(Font *f) {
    if (!f) return 16;
    const Glyph *g = font_glyph(f, 'M');
    if (g && g->h) return g->h;
    double m = 0;
    for (int32_t i = 0; i < f->nglyphs; i++) if (f->glyphs[i].h > m) m = f->glyphs[i].h;
    return m > 0 ? m : f->em_size;
}

static double text_line_width(Font *f, const char *s, int32_t len) {
    double w = 0;
    int32_t p = 0;
    while (p < len) {
        int c = utf8_decode(s, len, &p);
        const Glyph *g = f ? font_glyph(f, c) : NULL;
        w += g ? g->shift : (f ? f->em_size / 2 : 8);
    }
    return w;
}

/* Splits text into lines on '\n' (and wraps at max_w if > 0). Calls fn for
 * each line with its start/length. */
typedef void (*LineFn)(void *ctx, const char *s, int32_t len, int32_t line_index);
static int32_t text_lines(Font *f, const char *s, int32_t len, double max_w, LineFn fn, void *ctx) {
    int32_t line = 0, start = 0;
    while (start <= len) {
        int32_t end = start;
        while (end < len && s[end] != '\n' && !(s[end] == '\r')) end++;
        /* wrap */
        if (max_w > 0) {
            int32_t ls = start;
            while (ls < end) {
                int32_t last_space = -1, p = ls;
                double w = 0;
                while (p < end) {
                    int32_t q = p;
                    int c = utf8_decode(s, end, &q);
                    const Glyph *g = f ? font_glyph(f, c) : NULL;
                    double cw = g ? g->shift : 8;
                    if (w + cw > max_w && p > ls) break;
                    if (c == ' ') last_space = p;
                    w += cw;
                    p = q;
                }
                int32_t brk = p;
                if (p < end && last_space > ls) brk = last_space;
                if (fn) fn(ctx, s + ls, brk - ls, line);
                line++;
                ls = brk;
                if (ls < end && s[ls] == ' ') ls++;
            }
            if (start == end) { if (fn) fn(ctx, s + start, 0, line); line++; }
        } else {
            if (fn) fn(ctx, s + start, end - start, line);
            line++;
        }
        if (end >= len) break;
        start = end + 1;
        if (s[end] == '\r' && start < len && s[start] == '\n') start++;
    }
    return line;
}

typedef struct {
    Font *font;
    double x, y, xs, ys, angle, sep, total_h;
    uint32_t c_top, c_bottom;
    double alpha;
    double max_w;
} TextCtx;

static void draw_text_line(void *vctx, const char *s, int32_t len, int32_t line) {
    TextCtx *t = vctx;
    Font *f = t->font;
    if (!f) return;
    double lw = text_line_width(f, s, len);
    double lx = R->halign == 1 ? -lw / 2 : R->halign == 2 ? -lw : 0;
    double ly = line * t->sep;
    if (R->valign == 1) ly -= t->total_h / 2;
    else if (R->valign == 2) ly -= t->total_h;
    double rad = t->angle * M_PI / 180.0, c = cos(rad), sn = sin(rad);
    int32_t p = 0;
    uint32_t col = abgr(t->c_top, t->alpha);
    while (p < len) {
        int ch = utf8_decode(s, len, &p);
        const Glyph *g = font_glyph(f, ch);
        if (!g) { lx += f->em_size / 2; continue; }
        if (g->w && g->h) {
            /* glyph top-left in text-local space, then rotate around (x, y) */
            double gx = (lx + g->offset) * t->xs, gy = ly * t->ys;
            double wx = t->x + gx * c + gy * sn, wy = t->y - gx * sn + gy * c;
            draw_tpag(f->tpag, g->x, g->y, g->w, g->h, wx, wy, 0, 0, g->w, g->h, t->xs, t->ys, t->angle, col);
        }
        lx += g->shift;
    }
}

static void text_impl(double x, double y, Value sv, double sep, double max_w, double xs, double ys, double angle,
                      uint32_t c1, double alpha) {
    if (g_surface_depth > 0) return;
    Font *f = (Font *)cur_font();
    if (!f) return;
    Value s = v_to_string(sv);
    TextCtx t = {f, x, y, xs, ys, angle, sep < 0 ? font_line_height(f) : sep, 0, c1, c1, alpha, max_w};
    int32_t lines = text_lines(f, s.s->chars, s.s->len, max_w, NULL, NULL);
    t.total_h = lines * t.sep;
    text_lines(f, s.s->chars, s.s->len, max_w, draw_text_line, &t);
    v_release(s);
}

static Value bi_draw_text(int32_t argc, Value *a) {
    if (argc > 2) text_impl(v_to_real(a[0]), v_to_real(a[1]), a[2], -1, -1, 1, 1, 0, R->draw_color, R->draw_alpha);
    return v_undef();
}
static Value bi_draw_text_ext(int32_t argc, Value *a) {
    if (argc > 2) text_impl(v_to_real(a[0]), v_to_real(a[1]), a[2], ARGR(argc, a, 3, -1), ARGR(argc, a, 4, -1), 1, 1, 0,
                            R->draw_color, R->draw_alpha);
    return v_undef();
}
static Value bi_draw_text_color(int32_t argc, Value *a) {
    if (argc > 2) text_impl(v_to_real(a[0]), v_to_real(a[1]), a[2], -1, -1, 1, 1, 0, (uint32_t)ARGI(argc, a, 3, 0xFFFFFF),
                            ARGR(argc, a, 7, 1));
    return v_undef();
}
static Value bi_draw_text_transformed(int32_t argc, Value *a) {
    if (argc > 2) text_impl(v_to_real(a[0]), v_to_real(a[1]), a[2], -1, -1, ARGR(argc, a, 3, 1), ARGR(argc, a, 4, 1),
                            ARGR(argc, a, 5, 0), R->draw_color, R->draw_alpha);
    return v_undef();
}
static Value bi_draw_text_ext_transformed(int32_t argc, Value *a) {
    if (argc > 2) text_impl(v_to_real(a[0]), v_to_real(a[1]), a[2], ARGR(argc, a, 3, -1), ARGR(argc, a, 4, -1),
                            ARGR(argc, a, 5, 1), ARGR(argc, a, 6, 1), ARGR(argc, a, 7, 0), R->draw_color, R->draw_alpha);
    return v_undef();
}
static Value bi_draw_text_transformed_color(int32_t argc, Value *a) {
    if (argc > 2) text_impl(v_to_real(a[0]), v_to_real(a[1]), a[2], -1, -1, ARGR(argc, a, 3, 1), ARGR(argc, a, 4, 1),
                            ARGR(argc, a, 5, 0), (uint32_t)ARGI(argc, a, 6, 0xFFFFFF), ARGR(argc, a, 10, 1));
    return v_undef();
}
static Value bi_draw_text_ext_color(int32_t argc, Value *a) {
    if (argc > 2) text_impl(v_to_real(a[0]), v_to_real(a[1]), a[2], ARGR(argc, a, 3, -1), ARGR(argc, a, 4, -1), 1, 1, 0,
                            (uint32_t)ARGI(argc, a, 5, 0xFFFFFF), ARGR(argc, a, 9, 1));
    return v_undef();
}
static Value bi_draw_text_ext_transformed_color(int32_t argc, Value *a) {
    if (argc > 2) text_impl(v_to_real(a[0]), v_to_real(a[1]), a[2], ARGR(argc, a, 3, -1), ARGR(argc, a, 4, -1),
                            ARGR(argc, a, 5, 1), ARGR(argc, a, 6, 1), ARGR(argc, a, 7, 0),
                            (uint32_t)ARGI(argc, a, 8, 0xFFFFFF), ARGR(argc, a, 12, 1));
    return v_undef();
}

typedef struct { Font *f; double w; } WidthCtx;
static void width_line(void *vctx, const char *s, int32_t len, int32_t line) {
    (void)line;
    WidthCtx *c = vctx;
    double w = text_line_width(c->f, s, len);
    if (w > c->w) c->w = w;
}
static Value string_size(int32_t argc, Value *a, bool width, bool ext) {
    Font *f = (Font *)cur_font();
    Value s = argc ? v_to_string(a[0]) : v_str("");
    double sep = ext ? ARGR(argc, a, 1, -1) : -1, max_w = ext ? ARGR(argc, a, 2, -1) : -1;
    if (sep < 0) sep = font_line_height(f);
    double r;
    if (width) {
        WidthCtx c = {f, 0};
        text_lines(f, s.s->chars, s.s->len, max_w, width_line, &c);
        r = c.w;
    } else {
        r = text_lines(f, s.s->chars, s.s->len, max_w, NULL, NULL) * sep;
    }
    v_release(s);
    return v_real(r);
}
static Value bi_string_width(int32_t argc, Value *a) { return string_size(argc, a, true, false); }
static Value bi_string_height(int32_t argc, Value *a) { return string_size(argc, a, false, false); }
static Value bi_string_width_ext(int32_t argc, Value *a) { return string_size(argc, a, true, true); }
static Value bi_string_height_ext(int32_t argc, Value *a) { return string_size(argc, a, false, true); }
static Value bi_font_exists(int32_t argc, Value *a) { int32_t f = ARGI(argc, a, 0, -1); return v_bool(f >= 0 && f < R->dw.nfonts); }
static Value bi_font_get_size(int32_t argc, Value *a) {
    int32_t f = ARGI(argc, a, 0, -1);
    return v_real(f >= 0 && f < R->dw.nfonts ? R->dw.fonts[f].em_size : 0);
}

/* ------------------------------------------------------------------ */
/* surfaces: not supported yet; drawing to them is discarded           */
/* ------------------------------------------------------------------ */

typedef struct { bool used; int32_t w, h; } Surface;
static Surface g_surfaces[64];

static Value bi_surface_create(int32_t argc, Value *a) {
    for (int32_t i = 1; i < 64; i++) {
        if (!g_surfaces[i].used) {
            g_surfaces[i] = (Surface){true, ARGI(argc, a, 0, 1), ARGI(argc, a, 1, 1)};
            return v_real(i);
        }
    }
    return v_real(-1);
}
static Surface *surf_arg(int32_t argc, Value *a) {
    int32_t i = ARGI(argc, a, 0, -1);
    return (i > 0 && i < 64 && g_surfaces[i].used) ? &g_surfaces[i] : NULL;
}
static Value bi_surface_exists(int32_t argc, Value *a) {
    if (ARGI(argc, a, 0, -1) == 0) return v_bool(true); /* application_surface */
    return v_bool(surf_arg(argc, a) != NULL);
}
static Value bi_surface_free(int32_t argc, Value *a) { Surface *s = surf_arg(argc, a); if (s) s->used = false; return v_undef(); }
static Value bi_surface_get_width(int32_t argc, Value *a) {
    if (ARGI(argc, a, 0, -1) == 0) return v_real(R->window_w);
    Surface *s = surf_arg(argc, a);
    return v_real(s ? s->w : 0);
}
static Value bi_surface_get_height(int32_t argc, Value *a) {
    if (ARGI(argc, a, 0, -1) == 0) return v_real(R->window_h);
    Surface *s = surf_arg(argc, a);
    return v_real(s ? s->h : 0);
}
static Value bi_surface_resize(int32_t argc, Value *a) {
    Surface *s = surf_arg(argc, a);
    if (s) { s->w = ARGI(argc, a, 1, s->w); s->h = ARGI(argc, a, 2, s->h); }
    return v_undef();
}
static Value bi_surface_set_target(int32_t argc, Value *a) {
    if (ARGI(argc, a, 0, -1) != 0) g_surface_depth++;
    return v_bool(true);
}
static Value bi_surface_reset_target(int32_t argc, Value *a) {
    (void)argc; (void)a;
    if (g_surface_depth > 0) g_surface_depth--;
    return v_bool(true);
}
static Value bi_surface_getpixel(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(0); }
static Value bi_noop_draw(int32_t argc, Value *a) { (void)argc; (void)a; return v_undef(); }

/* ------------------------------------------------------------------ */
/* room layers and the draw pass                                       */
/* ------------------------------------------------------------------ */

static void draw_background_layer(Layer *L) {
    if (!L->bg_visible) return;
    uint32_t alpha_byte = (L->bg_color >> 24) & 0xFF;
    double alpha = alpha_byte / 255.0;
    if (L->bg_sprite < 0 || L->bg_sprite >= R->dw.nsprites) {
        if (L->src && L->bg_color) {
            rect_window(g_vx, g_vy, g_vx + R->window_w / g_kx, g_vy + R->window_h / g_ky, L->bg_color);
        }
        return;
    }
    const Sprite *s = &R->dw.sprites[L->bg_sprite];
    double xs = L->bg_xscale, ys = L->bg_yscale;
    if (L->bg_stretch && s->w > 0 && s->h > 0) { xs = (double)R->room_width / s->w; ys = (double)R->room_height / s->h; }
    if (L->bg_htiled || L->bg_vtiled)
        tiled(L->bg_sprite, L->bg_index, L->x, L->y, xs, ys, L->bg_color, alpha, L->bg_htiled, L->bg_vtiled);
    else
        draw_sprite_frame(L->bg_sprite, L->bg_index, L->x + s->origin_x * xs, L->y + s->origin_y * ys, xs, ys, 0,
                          L->bg_color, alpha);
}

static void draw_tile_layer(Layer *L) {
    const RoomLayer *src = L->src;
    if (!src || !src->tile_data || src->tile_bg < 0 || src->tile_bg >= R->dw.nbackgrounds) return;
    const Background *bg = &R->dw.backgrounds[src->tile_bg];
    if (bg->tpag < 0 || !bg->tile_w || !bg->tile_h) return;
    double tw = bg->tile_w, th = bg->tile_h;
    double vl = g_vx - L->x, vt = g_vy - L->y;
    double vr = vl + R->window_w / g_kx, vb = vt + R->window_h / g_ky;
    int32_t c0 = (int32_t)floor(vl / tw), c1 = (int32_t)ceil(vr / tw);
    int32_t r0 = (int32_t)floor(vt / th), r1 = (int32_t)ceil(vb / th);
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 > (int32_t)src->tiles_x) c1 = (int32_t)src->tiles_x;
    if (r1 > (int32_t)src->tiles_y) r1 = (int32_t)src->tiles_y;
    for (int32_t r = r0; r < r1; r++) {
        for (int32_t c = c0; c < c1; c++) {
            uint32_t td = src->tile_data[(uint32_t)r * src->tiles_x + (uint32_t)c];
            uint32_t idx = td & 0x7FFFF;
            if (idx == 0) continue;
            double sx = (idx % bg->columns) * (tw + 2 * bg->border_x) + bg->border_x;
            double sy = (idx / bg->columns) * (th + 2 * bg->border_y) + bg->border_y;
            bool mirror = (td >> 28) & 1, flip = (td >> 29) & 1, rot = (td >> 30) & 1;
            double x = L->x + c * tw + tw / 2, y = L->y + r * th + th / 2;
            draw_tpag(bg->tpag, sx, sy, tw, th, x, y, -tw / 2, -th / 2, tw, th, mirror ? -1 : 1, flip ? -1 : 1,
                      rot ? 270 : 0, 0xFFFFFFFF);
        }
    }
}

static void draw_asset_layer(Layer *L) {
    const RoomLayer *src = L->src;
    if (!src) return;
    for (int32_t i = 0; i < src->nsprites; i++) {
        const LayerSprite *s = &src->sprites[i];
        uint32_t c = s->color;
        draw_sprite_frame(s->sprite, s->frame, s->x + L->x, s->y + L->y, s->scale_x, s->scale_y, s->rotation,
                          c & 0xFFFFFF, ((c >> 24) & 0xFF) / 255.0);
    }
}

static void draw_layer(Layer *L) {
    if (!L->visible) return;
    switch (L->type) {
    case LAYER_BACKGROUND: draw_background_layer(L); break;
    case LAYER_TILES: draw_tile_layer(L); break;
    case LAYER_ASSETS: draw_asset_layer(L); break;
    default: break;
    }
}

typedef struct { double depth; int kind; int32_t order; void *p; } DrawItem;

static int cmp_items(const void *x, const void *y) {
    const DrawItem *a = x, *b = y;
    if (a->depth != b->depth) return a->depth > b->depth ? -1 : 1;
    if (a->kind != b->kind) return a->kind - b->kind;
    return a->order - b->order;
}

static void draw_instance_default(Instance *i) {
    if (!event_run(i, EV_DRAW, DRAW_NORMAL, NULL))
        draw_sprite_frame(i->sprite_index, i->image_index, i->x, i->y, i->image_xscale, i->image_yscale, i->image_angle,
                          i->image_blend, i->image_alpha);
}

static int32_t collect_instances(Instance ***out) {
    int32_t n = R->ninsts;
    Instance **list = d3_malloc((size_t)(n ? n : 1) * sizeof(Instance *));
    int32_t m = 0;
    for (int32_t k = 0; k < n; k++) {
        Instance *i = R->insts[k];
        if (!i->destroyed && i->active && i->visible) { list[m++] = i; obj_retain(&i->base); }
    }
    /* sort by depth (higher first), creation order for ties */
    for (int32_t a = 1; a < m; a++) {
        Instance *x = list[a];
        int32_t b = a - 1;
        while (b >= 0 && (list[b]->depth < x->depth ||
                          (list[b]->depth == x->depth && list[b]->create_order > x->create_order))) {
            list[b + 1] = list[b];
            b--;
        }
        list[b + 1] = x;
    }
    *out = list;
    return m;
}

static void release_instances(Instance **list, int32_t n) {
    for (int32_t k = 0; k < n; k++) obj_release(&list[k]->base);
    free(list);
}

static void event_pass(int32_t sub) {
    Instance **list;
    int32_t n = collect_instances(&list);
    for (int32_t k = 0; k < n; k++)
        if (!list[k]->destroyed) event_run(list[k], EV_DRAW, sub, NULL);
    release_instances(list, n);
}

void draw_layers_and_instances(void) {
    Instance **list;
    int32_t n = collect_instances(&list);
    int32_t total = n + R->nlayers;
    DrawItem *items = d3_malloc((size_t)(total ? total : 1) * sizeof(DrawItem));
    int32_t m = 0;
    for (int32_t l = 0; l < R->nlayers; l++)
        if (R->layers[l].type != LAYER_INSTANCES) items[m++] = (DrawItem){R->layers[l].depth, 0, l, &R->layers[l]};
    for (int32_t k = 0; k < n; k++) items[m++] = (DrawItem){list[k]->depth, 1, k, list[k]};
    qsort(items, (size_t)m, sizeof(DrawItem), cmp_items);
    for (int32_t k = 0; k < m; k++) {
        if (items[k].kind == 0) draw_layer(items[k].p);
        else if (!((Instance *)items[k].p)->destroyed) draw_instance_default(items[k].p);
    }
    free(items);
    release_instances(list, n);
}

static void reset_draw_state_for_pass(void) {
    plat_set_blend_mode(0);
    R->blend_mode = 0;
    g_surface_depth = 0;
}

void runtime_draw(void) {
    uint32_t clear = R->draw_bg_color ? abgr(R->bg_color, 1) : 0xFF000000u;
    plat_frame_begin(clear);
    reset_draw_state_for_pass();

    int32_t view_list[MAX_VIEWS], nv = 0;
    if (R->views_enabled) {
        for (int32_t v = 0; v < MAX_VIEWS; v++)
            if (R->views[v].enabled && R->views[v].visible) view_list[nv++] = v;
    }
    if (nv == 0) {
        /* no views: one implicit view showing the room at the window size */
        View *vw = &R->views[0];
        if (!R->views_enabled) {
            vw->x = 0; vw->y = 0;
            vw->w = R->window_w; vw->h = R->window_h;
            vw->port_x = 0; vw->port_y = 0;
            vw->port_w = R->window_w; vw->port_h = R->window_h;
        }
        view_list[nv++] = 0;
    }

    R->in_gui = false;
    draw_set_view_transform(view_list[0]);
    event_pass(DRAW_PRE);
    for (int32_t k = 0; k < nv; k++) {
        R->view_current = view_list[k];
        View *vw = &R->views[view_list[k]];
        draw_set_view_transform(view_list[k]);
        plat_set_clip((float)vw->port_x, (float)vw->port_y, (float)vw->port_w, (float)vw->port_h);
        event_pass(DRAW_BEGIN);
        draw_layers_and_instances();
        event_pass(DRAW_END);
    }
    plat_set_clip(0, 0, 0, 0);
    R->view_current = 0;
    draw_set_view_transform(view_list[0]);
    event_pass(DRAW_POST);

    R->in_gui = true;
    draw_set_gui_transform();
    event_pass(DRAW_GUI_BEGIN);
    event_pass(DRAW_GUI);
    event_pass(DRAW_GUI_END);
    R->in_gui = false;
    reset_draw_state_for_pass();
    plat_frame_end();
}

void builtins_register_draw(void) {
#define REG(n) builtin_register(#n, bi_##n)
    REG(draw_self); REG(draw_sprite); REG(draw_sprite_ext); REG(draw_sprite_part); REG(draw_sprite_part_ext);
    REG(draw_sprite_stretched); REG(draw_sprite_stretched_ext); REG(draw_sprite_tiled); REG(draw_sprite_tiled_ext);
    REG(draw_sprite_general); REG(draw_sprite_pos);
    REG(sprite_exists); REG(sprite_get_number); REG(sprite_get_width); REG(sprite_get_height);
    REG(sprite_get_xoffset); REG(sprite_get_yoffset); REG(sprite_get_bbox_left); REG(sprite_get_bbox_right);
    REG(sprite_get_bbox_top); REG(sprite_get_bbox_bottom); REG(sprite_get_name); REG(sprite_get_speed);
    REG(sprite_get_speed_type); REG(sprite_set_offset);
    REG(draw_set_color); REG(draw_get_color); REG(draw_set_alpha); REG(draw_get_alpha); REG(draw_set_font);
    REG(draw_get_font); REG(draw_set_halign); REG(draw_set_valign); REG(draw_get_halign); REG(draw_get_valign);
    builtin_register("draw_set_colour", bi_draw_set_color);
    builtin_register("draw_get_colour", bi_draw_get_color);
    REG(gpu_set_blendmode); REG(gpu_set_blendmode_ext); REG(gpu_get_blendmode);
    builtin_register("draw_set_blend_mode", bi_gpu_set_blendmode);
    builtin_register("draw_set_blend_mode_ext", bi_gpu_set_blendmode_ext);
    builtin_register("gpu_set_blendmode_ext_sepalpha", bi_gpu_set_blendmode_ext);
    REG(draw_rectangle); REG(draw_rectangle_color); REG(draw_line); REG(draw_line_width); REG(draw_line_color);
    REG(draw_line_width_color); REG(draw_point); REG(draw_circle); REG(draw_circle_color); REG(draw_ellipse);
    REG(draw_triangle); REG(draw_clear); REG(draw_clear_alpha);
    builtin_register("draw_rectangle_colour", bi_draw_rectangle_color);
    builtin_register("draw_line_colour", bi_draw_line_color);
    builtin_register("draw_line_width_colour", bi_draw_line_width_color);
    builtin_register("draw_circle_colour", bi_draw_circle_color);
    builtin_register("draw_roundrect", bi_draw_rectangle);
    builtin_register("draw_roundrect_ext", bi_draw_rectangle);
    REG(draw_text); REG(draw_text_ext); REG(draw_text_color); REG(draw_text_transformed);
    REG(draw_text_ext_transformed); REG(draw_text_transformed_color); REG(draw_text_ext_color);
    REG(draw_text_ext_transformed_color);
    builtin_register("draw_text_colour", bi_draw_text_color);
    builtin_register("draw_text_transformed_colour", bi_draw_text_transformed_color);
    builtin_register("draw_text_ext_colour", bi_draw_text_ext_color);
    builtin_register("draw_text_ext_transformed_colour", bi_draw_text_ext_transformed_color);
    REG(string_width); REG(string_height); REG(string_width_ext); REG(string_height_ext); REG(font_exists);
    REG(font_get_size);
    REG(surface_create); REG(surface_exists); REG(surface_free); REG(surface_get_width); REG(surface_get_height);
    REG(surface_resize); REG(surface_set_target); REG(surface_reset_target); REG(surface_getpixel);
    builtin_register("surface_copy", bi_noop_draw);
    builtin_register("surface_copy_part", bi_noop_draw);
    builtin_register("draw_surface", bi_noop_draw);
    builtin_register("draw_surface_ext", bi_noop_draw);
    builtin_register("draw_surface_part", bi_noop_draw);
    builtin_register("draw_surface_part_ext", bi_noop_draw);
    builtin_register("draw_surface_stretched", bi_noop_draw);
    builtin_register("draw_surface_stretched_ext", bi_noop_draw);
    builtin_register("draw_surface_general", bi_noop_draw);
    builtin_register("draw_set_circle_precision", bi_noop_draw);
    builtin_register("draw_primitive_begin", bi_noop_draw);
    builtin_register("draw_primitive_end", bi_noop_draw);
    builtin_register("draw_vertex", bi_noop_draw);
    builtin_register("draw_vertex_color", bi_noop_draw);
    builtin_register("draw_vertex_colour", bi_noop_draw);
    builtin_register("draw_primitive_begin_texture", bi_noop_draw);
    builtin_register("draw_vertex_texture", bi_noop_draw);
    builtin_register("draw_vertex_texture_color", bi_noop_draw);
    builtin_register("draw_vertex_texture_colour", bi_noop_draw);
#undef REG
}
