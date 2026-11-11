#include "datawin.h"
#include "../platform/platform.h"

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[5];
    uint32_t off;  /* offset of chunk data (after the 8-byte header) */
    uint32_t len;
} Chunk;

#define MAX_CHUNKS 64

typedef struct {
    DataWin *dw;
    Chunk chunks[MAX_CHUNKS];
    int nchunks;
    char *err;
    size_t errlen;
    bool failed;
} Ctx;

static void fail(Ctx *c, const char *fmt, const char *arg) {
    if (c->failed) return;
    c->failed = true;
    snprintf(c->err, c->errlen, fmt, arg);
}

static const Chunk *find_chunk(Ctx *c, const char *name) {
    for (int i = 0; i < c->nchunks; i++)
        if (memcmp(c->chunks[i].name, name, 4) == 0) return &c->chunks[i];
    return NULL;
}

static inline bool in_buf(const DataWin *dw, uint32_t off, uint32_t n) {
    return off <= dw->buf_len && n <= dw->buf_len - off;
}

static inline uint32_t U32(const DataWin *dw, uint32_t off) {
    return in_buf(dw, off, 4) ? rd_u32(dw->buf + off) : 0;
}
static inline int32_t I32(const DataWin *dw, uint32_t off) { return (int32_t)U32(dw, off); }
static inline float F32(const DataWin *dw, uint32_t off) {
    return in_buf(dw, off, 4) ? rd_f32(dw->buf + off) : 0.0f;
}
static inline uint16_t U16(const DataWin *dw, uint32_t off) {
    return in_buf(dw, off, 2) ? rd_u16(dw->buf + off) : 0;
}

/* A GameMaker string reference points at the characters; the length is the
 * uint32 right before them. */
static const char *STR(const DataWin *dw, uint32_t off) {
    uint32_t p = U32(dw, off);
    if (p < 4 || !in_buf(dw, p, 1)) return "";
    return (const char *)(dw->buf + p);
}

/* Pointer list: count followed by count absolute pointers. */
static uint32_t plist_count(const DataWin *dw, uint32_t off) {
    uint32_t n = U32(dw, off);
    if (!in_buf(dw, off + 4, n * 4)) return 0;
    return n;
}
static uint32_t plist_get(const DataWin *dw, uint32_t off, uint32_t i) { return U32(dw, off + 4 + i * 4); }

/* ------------------------------------------------------------------ */
/* chunk parsers                                                       */
/* ------------------------------------------------------------------ */

static void parse_gen8(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t o = ch->off;
    dw->bytecode = dw->buf[o + 1];
    dw->game_name = STR(dw, o + 40);
    dw->version_major = U32(dw, o + 44);
    dw->window_w = U32(dw, o + 60);
    dw->window_h = U32(dw, o + 64);
    dw->display_name = STR(dw, o + 100);
    uint32_t ro = o + (dw->bytecode >= 14 ? 128 : 124);
    uint32_t n = U32(dw, ro);
    if (n > 100000) n = 0;
    dw->nroom_order = (int32_t)n;
    dw->room_order = d3_calloc(n, sizeof(int32_t));
    for (uint32_t i = 0; i < n; i++) dw->room_order[i] = I32(dw, ro + 4 + i * 4);
    /* GMS2 adds random/info numbers then the FPS as a float. The layout of the
     * random block is fixed: 8 + 4 * 8 bytes. */
    if (dw->version_major >= 2) {
        uint32_t fo = ro + 4 + n * 4 + 8 + 32;
        if (fo + 4 <= ch->off + ch->len) dw->gms2_fps = F32(dw, fo);
    }
    if (!(dw->gms2_fps > 1 && dw->gms2_fps < 1000)) dw->gms2_fps = 30;
}

static void parse_strg(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->nstrings = (int32_t)n;
    dw->strings = d3_calloc(n, sizeof(GMStr *));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        uint32_t len = U32(dw, p);
        if (!in_buf(dw, p + 4, len)) len = 0;
        dw->strings[i] = gmstr_static((const char *)dw->buf + p + 4, len);
    }
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void parse_tpag(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->ntpag = (int32_t)n;
    dw->tpag = d3_calloc(n, sizeof(TPage));
    dw->tpag_ptrs = d3_calloc(n, sizeof(uint32_t));
    bool sorted = true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        dw->tpag_ptrs[i] = p;
        if (i && p <= dw->tpag_ptrs[i - 1]) sorted = false;
        TPage *t = &dw->tpag[i];
        t->sx = U16(dw, p); t->sy = U16(dw, p + 2); t->sw = U16(dw, p + 4); t->sh = U16(dw, p + 6);
        t->tx = U16(dw, p + 8); t->ty = U16(dw, p + 10); t->tw = U16(dw, p + 12); t->th = U16(dw, p + 14);
        t->bw = U16(dw, p + 16); t->bh = U16(dw, p + 18);
        t->page = (int16_t)U16(dw, p + 20);
    }
    if (!sorted) {
        /* Very unusual; keep lookups correct by refusing to guess. */
        LOGW("TPAG pointers are not sorted; texture lookups may fail");
        qsort(dw->tpag_ptrs, n, sizeof(uint32_t), cmp_u32);
    }
}

int32_t datawin_tpag_index(const DataWin *dw, uint32_t ptr) {
    int32_t lo = 0, hi = dw->ntpag - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (dw->tpag_ptrs[mid] == ptr) return mid;
        if (dw->tpag_ptrs[mid] < ptr) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static void parse_sprt(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->nsprites = (int32_t)n;
    dw->sprites = d3_calloc(n, sizeof(Sprite));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        Sprite *s = &dw->sprites[i];
        if (!p) { s->name = ""; continue; }
        s->name = STR(dw, p);
        s->w = I32(dw, p + 4); s->h = I32(dw, p + 8);
        s->margin_l = I32(dw, p + 12); s->margin_r = I32(dw, p + 16);
        s->margin_b = I32(dw, p + 20); s->margin_t = I32(dw, p + 24);
        s->bbox_mode = I32(dw, p + 40); s->sep_masks = I32(dw, p + 44);
        s->origin_x = I32(dw, p + 48); s->origin_y = I32(dw, p + 52);
        s->speed = 1.0f;
        s->speed_type = 1;
        uint32_t q = p + 56;
        if (I32(dw, q) == -1) {
            uint32_t sversion = U32(dw, q + 4);
            uint32_t stype = U32(dw, q + 8);
            q += 12;
            if (dw->version_major >= 2) {
                s->speed = F32(dw, q);
                s->speed_type = (int32_t)U32(dw, q + 4);
                q += 8;
                if (sversion >= 2) q += 4; /* sequence offset */
                if (sversion >= 3) q += 4; /* nine-slice offset */
            }
            if (stype != 0) {
                /* SWF / Spine / vector sprites are not supported on 3DS. */
                LOGW("sprite %s has unsupported type %u", s->name, (unsigned)stype);
                continue;
            }
        }
        uint32_t nf = U32(dw, q);
        if (nf > 10000) nf = 0;
        s->nframes = (int32_t)nf;
        s->frames = d3_calloc(nf, sizeof(int32_t));
        for (uint32_t f = 0; f < nf; f++) s->frames[f] = datawin_tpag_index(dw, U32(dw, q + 4 + f * 4));
    }
}

static void parse_bgnd(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->nbackgrounds = (int32_t)n;
    dw->backgrounds = d3_calloc(n, sizeof(Background));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        Background *b = &dw->backgrounds[i];
        if (!p) { b->name = ""; continue; }
        b->name = STR(dw, p);
        b->tpag = datawin_tpag_index(dw, U32(dw, p + 16));
        if (dw->version_major < 2) continue;
        uint32_t q = p + 20;
        /* q+0 tileset version, q+4 tile w, q+8 tile h, then optionally two
         * separation fields (2024.14.1+), border x/y, columns, items, count.
         * "items per tile" is practically always 1, which tells the layouts apart. */
        b->tile_w = U32(dw, q + 4);
        b->tile_h = U32(dw, q + 8);
        uint32_t base = q + 12;
        if (U32(dw, base + 12) != 1 && U32(dw, base + 20) == 1) base += 8;
        b->border_x = U32(dw, base);
        b->border_y = U32(dw, base + 4);
        b->columns = U32(dw, base + 8);
        b->tile_count = U32(dw, base + 16);
        if (!b->columns) b->columns = 1;
    }
}

static void build_font_glyphs(DataWin *dw, Font *f, uint32_t list) {
    uint32_t n = plist_count(dw, list);
    f->nglyphs = (int32_t)n;
    f->glyphs = d3_calloc(n, sizeof(Glyph));
    for (uint32_t g = 0; g < n; g++) {
        uint32_t p = plist_get(dw, list, g);
        Glyph *gl = &f->glyphs[g];
        gl->ch = U16(dw, p); gl->x = U16(dw, p + 2); gl->y = U16(dw, p + 4);
        gl->w = U16(dw, p + 6); gl->h = U16(dw, p + 8);
        gl->shift = (int16_t)U16(dw, p + 10); gl->offset = (int16_t)U16(dw, p + 12);
    }
}

/* Checks whether off looks like a pointer list whose first element starts
 * right after the list (how GameMaker lays out font glyphs). */
static bool looks_like_glyph_list(const DataWin *dw, uint32_t off, uint32_t lo, uint32_t hi) {
    uint32_t n = U32(dw, off);
    if (n == 0) return true;
    if (n > 70000) return false;
    uint32_t first = U32(dw, off + 4);
    if (first != off + 4 + n * 4) return false;
    return first >= lo && first < hi;
}

static void parse_font(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->nfonts = (int32_t)n;
    dw->fonts = d3_calloc(n, sizeof(Font));
    uint32_t lo = ch->off, hi = ch->off + ch->len;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        Font *f = &dw->fonts[i];
        if (!p) { f->name = ""; continue; }
        f->name = STR(dw, p);
        uint32_t em = U32(dw, p + 8);
        if (em & 0x80000000u) { float fl; uint32_t u = em & 0x7FFFFFFFu; memcpy(&fl, &u, 4); f->em_size = fl; }
        else f->em_size = (float)em;
        f->bold = U32(dw, p + 12) != 0;
        f->italic = U32(dw, p + 16) != 0;
        f->tpag = datawin_tpag_index(dw, U32(dw, p + 28));
        f->scale_x = F32(dw, p + 32);
        f->scale_y = F32(dw, p + 36);
        /* 0..4 optional version-dependent uint32 fields follow before the glyph list */
        uint32_t list = 0;
        for (uint32_t extra = 0; extra <= 5; extra++) {
            if (looks_like_glyph_list(dw, p + 40 + extra * 4, lo, hi)) { list = p + 40 + extra * 4; break; }
        }
        if (!list) { LOGW("font %s: glyph list not found", f->name); continue; }
        if (list - (p + 40) >= 16) f->line_height = (int32_t)U32(dw, list - 4);
        build_font_glyphs(dw, f, list);
    }
}

static void parse_sond(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->nsounds = (int32_t)n;
    dw->sounds = d3_calloc(n, sizeof(Sound));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        Sound *s = &dw->sounds[i];
        if (!p) { s->name = ""; s->audio_id = -1; continue; }
        s->name = STR(dw, p);
        s->flags = U32(dw, p + 4);
        s->type = STR(dw, p + 8);
        s->file = STR(dw, p + 12);
        s->volume = F32(dw, p + 20);
        s->pitch = F32(dw, p + 24);
        s->group = I32(dw, p + 28);
        s->audio_id = I32(dw, p + 32);
    }
}

static void parse_agrp(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->naudio_groups = (int32_t)n;
    dw->audio_groups = d3_calloc(n, sizeof(char *));
    for (uint32_t i = 0; i < n; i++) dw->audio_groups[i] = STR(dw, plist_get(dw, ch->off, i));
}

/* Object entry parse. Returns false if the guessed layout is inconsistent. */
static bool parse_object_entry(DataWin *dw, uint32_t p, bool managed, Object *o, const Chunk *ch) {
    uint32_t q = p;
    o->name = STR(dw, q); q += 4;
    o->sprite = I32(dw, q); q += 4;
    o->visible = U32(dw, q) != 0; q += 4;
    if (managed) q += 4;
    o->solid = U32(dw, q) != 0; q += 4;
    o->depth = I32(dw, q); q += 4;
    o->persistent = U32(dw, q) != 0; q += 4;
    int32_t parent = I32(dw, q); q += 4;
    o->parent = parent == -100 ? -1 : parent;
    o->mask = I32(dw, q); q += 4;
    q += 4 * 3;          /* uses physics, sensor, shape */
    q += 4 * 5;          /* density, restitution, group, linear/angular damping */
    uint32_t nverts = U32(dw, q); q += 4;
    q += 4 * 3;          /* friction, awake, kinematic */
    if (nverts > 100000) return false;
    q += nverts * 8;
    /* event lists: pointer list of EV_COUNT pointer lists */
    uint32_t ntypes = U32(dw, q);
    if (ntypes < 12 || ntypes > 20) return false;
    uint32_t first = U32(dw, q + 4);
    if (first < ch->off || first >= ch->off + ch->len) return false;
    for (uint32_t t = 0; t < ntypes && t < EV_COUNT; t++) {
        uint32_t el = U32(dw, q + 4 + t * 4);
        uint32_t ne = plist_count(dw, el);
        o->nevents[t] = (int32_t)ne;
        o->events[t] = ne ? d3_calloc(ne, sizeof(Event)) : NULL;
        for (uint32_t e = 0; e < ne; e++) {
            uint32_t ep = plist_get(dw, el, e);
            o->events[t][e].subtype = U32(dw, ep);
            uint32_t actions = ep + 4;
            o->events[t][e].code = -1;
            if (plist_count(dw, actions) > 0) {
                uint32_t ap = plist_get(dw, actions, 0);
                o->events[t][e].code = I32(dw, ap + 32);
            }
        }
    }
    return true;
}

static void parse_objt(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->nobjects = (int32_t)n;
    dw->objects = d3_calloc(n, sizeof(Object));
    int managed = -1; /* unknown until the first object tells us */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        Object *o = &dw->objects[i];
        o->parent = -1;
        if (!p) { o->name = ""; continue; }
        bool ok;
        if (managed < 0) {
            ok = parse_object_entry(dw, p, false, o, ch);
            if (ok) managed = 0;
            else {
                memset(o, 0, sizeof *o);
                ok = parse_object_entry(dw, p, true, o, ch);
                if (ok) managed = 1;
            }
        } else {
            ok = parse_object_entry(dw, p, managed == 1, o, ch);
        }
        if (!ok) LOGW("object %u: could not parse event lists", (unsigned)i);
    }
    dw->v2022_5 = managed == 1;
}

static void parse_room_layers(DataWin *dw, Room *r, uint32_t list, bool *tiles_compressed) {
    uint32_t n = plist_count(dw, list);
    r->nlayers = (int32_t)n;
    r->layers = d3_calloc(n, sizeof(RoomLayer));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, list, i);
        uint32_t next = (i + 1 < n) ? plist_get(dw, list, i + 1) : 0;
        RoomLayer *L = &r->layers[i];
        L->name = STR(dw, p);
        L->id = U32(dw, p + 4);
        L->type = U32(dw, p + 8);
        L->depth = I32(dw, p + 12);
        L->x_offset = F32(dw, p + 16); L->y_offset = F32(dw, p + 20);
        L->h_speed = F32(dw, p + 24); L->v_speed = F32(dw, p + 28);
        L->visible = U32(dw, p + 32) != 0;
        uint32_t q = p + 36;
        if (dw->v2022_1) {
            q += 8; /* effect enabled, effect type */
            uint32_t np = U32(dw, q);
            q += 4 + np * 12;
        }
        L->tile_bg = -1;
        switch (L->type) {
        case LAYER_INSTANCES: {
            uint32_t ni = U32(dw, q);
            if (ni > 100000) ni = 0;
            L->ninst = (int32_t)ni;
            L->inst_ids = d3_calloc(ni, sizeof(uint32_t));
            for (uint32_t k = 0; k < ni; k++) L->inst_ids[k] = U32(dw, q + 4 + k * 4);
            break;
        }
        case LAYER_TILES: {
            L->tile_bg = I32(dw, q);
            L->tiles_x = U32(dw, q + 4);
            L->tiles_y = U32(dw, q + 8);
            uint32_t total = L->tiles_x * L->tiles_y;
            if (L->tiles_x > 10000 || L->tiles_y > 10000) { L->tiles_x = L->tiles_y = 0; break; }
            L->tile_data = d3_calloc(total, sizeof(uint32_t));
            uint32_t d = q + 12;
            /* Uncompressed data is exactly 4 bytes per tile. When the next
             * layer starts somewhere else, the data must be RLE compressed
             * (GameMaker 2024.2+). */
            if (next && next != d + total * 4) *tiles_compressed = true;
            else if (next) *tiles_compressed = false;
            if (!*tiles_compressed) {
                for (uint32_t k = 0; k < total; k++) L->tile_data[k] = U32(dw, d + k * 4);
            } else {
                uint32_t k = 0;
                while (k < total && in_buf(dw, d, 1)) {
                    uint8_t len = dw->buf[d++];
                    if (len >= 128) {
                        uint32_t run = (uint32_t)(len & 0x7F) + 1, tile = U32(dw, d);
                        d += 4;
                        for (uint32_t j = 0; j < run && k < total; j++) L->tile_data[k++] = tile;
                    } else {
                        for (uint32_t j = 0; j < len && k < total; j++, d += 4) L->tile_data[k++] = U32(dw, d);
                    }
                }
            }
            break;
        }
        case LAYER_BACKGROUND: {
            LayerBackground *b = &L->bg;
            b->visible = U32(dw, q) != 0; b->foreground = U32(dw, q + 4) != 0;
            b->sprite = I32(dw, q + 8);
            b->tile_h = U32(dw, q + 12) != 0; b->tile_v = U32(dw, q + 16) != 0;
            b->stretch = U32(dw, q + 20) != 0;
            b->color = U32(dw, q + 24);
            b->first_frame = F32(dw, q + 28); b->anim_speed = F32(dw, q + 32);
            b->anim_type = U32(dw, q + 36);
            break;
        }
        case LAYER_ASSETS: {
            uint32_t sprites = U32(dw, q + 4);
            uint32_t ns = plist_count(dw, sprites);
            L->nsprites = (int32_t)ns;
            L->sprites = d3_calloc(ns, sizeof(LayerSprite));
            for (uint32_t k = 0; k < ns; k++) {
                uint32_t sp = plist_get(dw, sprites, k);
                LayerSprite *s = &L->sprites[k];
                s->name = STR(dw, sp);
                s->sprite = I32(dw, sp + 4);
                s->x = I32(dw, sp + 8); s->y = I32(dw, sp + 12);
                s->scale_x = F32(dw, sp + 16); s->scale_y = F32(dw, sp + 20);
                s->color = U32(dw, sp + 24);
                s->anim_speed = F32(dw, sp + 28); s->anim_type = U32(dw, sp + 32);
                s->frame = F32(dw, sp + 36); s->rotation = F32(dw, sp + 40);
            }
            break;
        }
        default: break;
        }
    }
}

static void parse_room(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->nrooms = (int32_t)n;
    dw->rooms = d3_calloc(n, sizeof(Room));
    bool tiles_compressed = false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        Room *r = &dw->rooms[i];
        if (!p) { r->name = ""; continue; }
        r->name = STR(dw, p);
        r->caption = STR(dw, p + 4);
        r->width = U32(dw, p + 8); r->height = U32(dw, p + 12);
        r->speed = U32(dw, p + 16);
        r->persistent = U32(dw, p + 20) != 0;
        r->bg_color = U32(dw, p + 24);
        r->draw_bg_color = U32(dw, p + 28) != 0;
        r->creation_code = I32(dw, p + 32);
        r->flags = U32(dw, p + 36);
        uint32_t views = U32(dw, p + 44);
        uint32_t objs = U32(dw, p + 48);
        uint32_t q = p + 56;
        if (dw->v2024_13) q += 4; /* instance creation order list */
        q += 4 * 5 + 4 * 3;       /* world, top/left/right/bottom, gravity xy, meters/pixel */
        uint32_t layers = dw->version_major >= 2 ? U32(dw, q) : 0;

        uint32_t nv = plist_count(dw, views);
        if (nv > 8) nv = 8;
        r->nviews = (int32_t)nv;
        for (uint32_t v = 0; v < nv; v++) {
            uint32_t vp = plist_get(dw, views, v);
            RoomView *rv = &r->views[v];
            rv->enabled = U32(dw, vp) != 0;
            rv->vx = I32(dw, vp + 4); rv->vy = I32(dw, vp + 8); rv->vw = I32(dw, vp + 12); rv->vh = I32(dw, vp + 16);
            rv->px = I32(dw, vp + 20); rv->py = I32(dw, vp + 24); rv->pw = I32(dw, vp + 28); rv->ph = I32(dw, vp + 32);
            rv->border_x = U32(dw, vp + 36); rv->border_y = U32(dw, vp + 40);
            rv->speed_x = I32(dw, vp + 44); rv->speed_y = I32(dw, vp + 48);
            rv->follow = I32(dw, vp + 52);
        }

        uint32_t ni = plist_count(dw, objs);
        r->ninst = (int32_t)ni;
        r->inst = d3_calloc(ni, sizeof(RoomInst));
        for (uint32_t k = 0; k < ni; k++) {
            uint32_t ip = plist_get(dw, objs, k);
            RoomInst *ri = &r->inst[k];
            ri->x = I32(dw, ip); ri->y = I32(dw, ip + 4);
            ri->obj = I32(dw, ip + 8);
            ri->id = U32(dw, ip + 12);
            ri->creation_code = I32(dw, ip + 16);
            ri->scale_x = F32(dw, ip + 20); ri->scale_y = F32(dw, ip + 24);
            uint32_t o = ip + 28;
            if (dw->version_major >= 2) {
                ri->image_speed = F32(dw, o); ri->image_index = I32(dw, o + 4);
                o += 8;
            } else {
                ri->image_speed = 1;
            }
            ri->color = U32(dw, o);
            ri->rotation = F32(dw, o + 4);
            ri->pre_create_code = dw->bytecode >= 16 ? I32(dw, o + 8) : -1;
        }
        if (layers) parse_room_layers(dw, r, layers, &tiles_compressed);
    }
    dw->v2024_2 = tiles_compressed;
}

static void parse_code(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->ncode = (int32_t)n;
    dw->code = d3_calloc(n, sizeof(CodeEntry));
    /* map of root bytecode addresses -> blob end */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        CodeEntry *ce = &dw->code[i];
        ce->name = STR(dw, p);
        ce->name_id = d3_intern(ce->name);
        ce->length = U32(dw, p + 4);
        ce->locals = U16(dw, p + 8);
        ce->args = U16(dw, p + 10) & 0x7FFF;
        int32_t rel = I32(dw, p + 12);
        uint32_t blob = (uint32_t)((int32_t)(p + 12) + rel);
        uint32_t offset = U32(dw, p + 16);
        if (!in_buf(dw, blob, ce->length) || (offset & 3)) {
            ce->start = ce->end = NULL;
            continue;
        }
        ce->start = (const uint32_t *)(dw->buf + blob + offset);
        ce->end = (const uint32_t *)(dw->buf + blob + ce->length);
    }
    /* Child entries (2.3 functions) report the parent's length; their end is
     * the parent's end, which the formula above already gives. */
}

static void patch_chain(Ctx *c, uint32_t first, uint32_t occurrences, uint32_t index, bool is_func) {
    DataWin *dw = c->dw;
    uint32_t addr = first;
    for (uint32_t k = 0; k < occurrences; k++) {
        if (!in_buf(dw, addr, 8)) { LOGW("bad reference chain address %u", (unsigned)addr); return; }
        uint32_t w0 = rd_u32(dw->buf + addr);
        uint32_t operand = rd_u32(dw->buf + addr + 4);
        uint32_t next = operand & 0x07FFFFFF;
        uint8_t op = (uint8_t)(w0 >> 24);
        uint32_t patched;
        if (is_func) {
            patched = index;
            /* push.i and pushref.i with a function: tag the instruction so the
             * VM knows the operand is a function index, not a number. */
            if (op == 0xC0 || op == 0xFF) {
                w0 = (w0 & 0xFFF0FFFFu) | (0xCu << 16);
                memcpy(dw->buf + addr, &w0, 4);
            }
        } else {
            patched = (operand & 0xF8000000u) | (index & 0x07FFFFFF);
        }
        memcpy(dw->buf + addr + 4, &patched, 4);
        addr += next;
    }
}

static void parse_vari(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    if (ch->len < 12) return;
    uint32_t o = ch->off + 12;
    uint32_t n = (ch->len - 12) / 20;
    dw->nvars = (int32_t)n;
    dw->vars = d3_calloc(n, sizeof(VarEntry));
    for (uint32_t i = 0; i < n; i++, o += 20) {
        VarEntry *v = &dw->vars[i];
        v->name = STR(dw, o);
        v->name_id = d3_intern(v->name);
        v->inst_type = I32(dw, o + 4);
        v->var_id = I32(dw, o + 8);
        v->builtin = -1;
        uint32_t occ = U32(dw, o + 12);
        int32_t first = I32(dw, o + 16);
        if (occ && first >= 0) patch_chain(c, (uint32_t)first, occ, i, false);
    }
}

static void parse_func(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    if (ch->len < 4) return;
    uint32_t n = U32(dw, ch->off);
    if (n * 12 + 4 > ch->len) { fail(c, "FUNC chunk is malformed%s", ""); return; }
    dw->nfuncs = (int32_t)n;
    dw->funcs = d3_calloc(n, sizeof(FuncEntry));
    uint32_t o = ch->off + 4;
    for (uint32_t i = 0; i < n; i++, o += 12) {
        FuncEntry *f = &dw->funcs[i];
        f->name = STR(dw, o);
        f->builtin = -1;
        f->code = -1;
        uint32_t occ = U32(dw, o + 4);
        int32_t first = I32(dw, o + 8);
        if (occ && first >= 0) patch_chain(c, (uint32_t)first, occ, i, true);
    }
}

static void parse_scpt(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = plist_count(dw, ch->off);
    dw->nscripts = (int32_t)n;
    dw->scripts = d3_calloc(n, sizeof(Script));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = plist_get(dw, ch->off, i);
        Script *s = &dw->scripts[i];
        if (!p) { s->name = ""; s->code = -1; continue; }
        s->name = STR(dw, p);
        int32_t id = I32(dw, p + 4);
        s->constructor = (id & 0x80000000) != 0 && id != -1;
        s->code = id == -1 ? -1 : (id & 0x7FFFFFFF);
    }
}

static void parse_glob(Ctx *c, const Chunk *ch) {
    DataWin *dw = c->dw;
    uint32_t n = U32(dw, ch->off);
    if (n * 4 + 4 > ch->len) n = 0;
    dw->nglobal_init = (int32_t)n;
    dw->global_init = d3_calloc(n, sizeof(int32_t));
    for (uint32_t i = 0; i < n; i++) dw->global_init[i] = I32(dw, ch->off + 4 + i * 4);
}

/* ------------------------------------------------------------------ */

int32_t datawin_find_code(const DataWin *dw, const char *name) {
    int id = d3_intern(name);
    for (int32_t i = 0; i < dw->ncode; i++)
        if (dw->code[i].name_id == id) return i;
    return -1;
}

static void dirname_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash) slash[1] = 0;
    else snprintf(out, n, "./");
}

bool datawin_load(DataWin *dw, const char *path, char *err, size_t errlen) {
    memset(dw, 0, sizeof *dw);
    Ctx c = {.dw = dw, .err = err, .errlen = errlen};
    snprintf(dw->path, sizeof dw->path, "%s", path);
    dirname_of(path, dw->dir, sizeof dw->dir);

    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "Cannot open %s", path); return false; }
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "FORM", 4) != 0) {
        fclose(f);
        snprintf(err, errlen, "%s is not a GameMaker data file", path);
        return false;
    }
    dw->file_len = rd_u32(hdr + 4) + 8;

    /* Walk chunk headers without reading their contents. */
    uint32_t pos = 8;
    while (pos + 8 <= dw->file_len && c.nchunks < MAX_CHUNKS) {
        if (fseek(f, (long)pos, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) break;
        Chunk *k = &c.chunks[c.nchunks++];
        memcpy(k->name, hdr, 4);
        k->name[4] = 0;
        k->off = pos + 8;
        k->len = rd_u32(hdr + 4);
        pos = k->off + k->len;
    }

    /* Load everything up to the end of the last chunk that is not TXTR/AUDO. */
    uint32_t need = 0;
    for (int i = 0; i < c.nchunks; i++) {
        const Chunk *k = &c.chunks[i];
        if (!memcmp(k->name, "TXTR", 4) || !memcmp(k->name, "AUDO", 4)) continue;
        if (k->off + k->len > need) need = k->off + k->len;
    }
    dw->buf_len = need;
    dw->buf = plat_alloc_big(need);
    if (!dw->buf) {
        fclose(f);
        snprintf(err, errlen, "Not enough memory to load %u KB of game data", (unsigned)(need / 1024));
        return false;
    }
    fseek(f, 0, SEEK_SET);
    size_t got = 0;
    while (got < need) {
        size_t r = fread(dw->buf + got, 1, need - got > (1 << 20) ? (1 << 20) : need - got, f);
        if (r == 0) break;
        got += r;
        plat_loading_progress("Loading game data", (float)got / (float)need);
    }
    if (got != need) {
        fclose(f);
        snprintf(err, errlen, "Short read on %s", path);
        return false;
    }

    /* Audio entries are read straight from the file (only their offsets). */
    const Chunk *audo = find_chunk(&c, "AUDO");
    if (audo) {
        uint8_t nb[4];
        fseek(f, (long)audo->off, SEEK_SET);
        if (fread(nb, 1, 4, f) == 4) {
            uint32_t n = rd_u32(nb);
            if (n < 100000) {
                uint8_t *ptrs = d3_malloc(n * 4);
                if (fread(ptrs, 1, n * 4, f) == n * 4) {
                    dw->naudio = (int32_t)n;
                    dw->audio = d3_calloc(n, sizeof(AudioEntry));
                    for (uint32_t i = 0; i < n; i++) {
                        uint32_t p = rd_u32(ptrs + i * 4);
                        fseek(f, (long)p, SEEK_SET);
                        if (fread(nb, 1, 4, f) == 4) {
                            dw->audio[i].offset = p + 4;
                            dw->audio[i].length = rd_u32(nb);
                        }
                    }
                }
                free(ptrs);
            }
        }
    }
    const Chunk *txtr = find_chunk(&c, "TXTR");
    if (txtr) {
        uint8_t nb[4];
        fseek(f, (long)txtr->off, SEEK_SET);
        if (fread(nb, 1, 4, f) == 4) dw->ntextures = (int32_t)rd_u32(nb);
    }
    fclose(f);

    /* Feature-level detection from chunk names. */
    dw->v2022_1 = find_chunk(&c, "FEAT") || find_chunk(&c, "PSEM") || find_chunk(&c, "UILR");
    dw->v2023_2 = find_chunk(&c, "PSEM") != NULL;
    dw->v2024_13 = find_chunk(&c, "UILR") != NULL;

    const Chunk *ch;
    if (!(ch = find_chunk(&c, "GEN8"))) { snprintf(err, errlen, "Missing GEN8 chunk"); return false; }
    parse_gen8(&c, ch);
    if (dw->bytecode < 15) {
        snprintf(err, errlen, "Bytecode version %u is not supported (needs 15+)", dw->bytecode);
        return false;
    }
    if ((ch = find_chunk(&c, "STRG"))) parse_strg(&c, ch);
    if ((ch = find_chunk(&c, "TPAG"))) parse_tpag(&c, ch);
    if ((ch = find_chunk(&c, "SPRT"))) parse_sprt(&c, ch);
    if ((ch = find_chunk(&c, "BGND"))) parse_bgnd(&c, ch);
    if ((ch = find_chunk(&c, "FONT"))) parse_font(&c, ch);
    if ((ch = find_chunk(&c, "SOND"))) parse_sond(&c, ch);
    if ((ch = find_chunk(&c, "AGRP"))) parse_agrp(&c, ch);
    if ((ch = find_chunk(&c, "OBJT"))) parse_objt(&c, ch);
    if ((ch = find_chunk(&c, "ROOM"))) parse_room(&c, ch);
    if ((ch = find_chunk(&c, "CODE"))) parse_code(&c, ch);
    else { snprintf(err, errlen, "No CODE chunk: YYC-compiled games cannot run"); return false; }
    if ((ch = find_chunk(&c, "VARI"))) parse_vari(&c, ch);
    if ((ch = find_chunk(&c, "FUNC"))) parse_func(&c, ch);
    if ((ch = find_chunk(&c, "SCPT"))) parse_scpt(&c, ch);
    if ((ch = find_chunk(&c, "GLOB"))) parse_glob(&c, ch);

    if (c.failed) return false;
    LOGI("Loaded %s: bytecode %u, %d rooms, %d objects, %d code entries, %d sprites",
         dw->display_name, dw->bytecode, dw->nrooms, dw->nobjects, dw->ncode, dw->nsprites);
    return true;
}

void datawin_free(DataWin *dw) {
    for (int32_t i = 0; i < dw->nstrings; i++) free(dw->strings[i]);
    free(dw->strings);
    free(dw->tpag);
    free(dw->tpag_ptrs);
    for (int32_t i = 0; i < dw->nsprites; i++) free(dw->sprites[i].frames);
    free(dw->sprites);
    free(dw->backgrounds);
    for (int32_t i = 0; i < dw->nfonts; i++) { free(dw->fonts[i].glyphs); free(dw->fonts[i].glyph_lut); }
    free(dw->fonts);
    free(dw->sounds);
    free(dw->audio_groups);
    for (int32_t i = 0; i < dw->nobjects; i++)
        for (int t = 0; t < EV_COUNT; t++) free(dw->objects[i].events[t]);
    free(dw->objects);
    for (int32_t i = 0; i < dw->nrooms; i++) {
        Room *r = &dw->rooms[i];
        for (int32_t l = 0; l < r->nlayers; l++) {
            free(r->layers[l].inst_ids);
            free(r->layers[l].tile_data);
            free(r->layers[l].sprites);
        }
        free(r->layers);
        free(r->inst);
    }
    free(dw->rooms);
    free(dw->code);
    free(dw->vars);
    free(dw->funcs);
    free(dw->scripts);
    free(dw->global_init);
    free(dw->audio);
    free(dw->room_order);
    plat_free_big(dw->buf);
    memset(dw, 0, sizeof *dw);
}
