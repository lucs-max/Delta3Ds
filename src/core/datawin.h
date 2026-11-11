/*
 * data.win (GameMaker IFF "FORM") reader.
 *
 * The file is read from the start up to the end of the last chunk the runner
 * needs. Texture pages (TXTR) and embedded audio (AUDO) are normally the last
 * chunks; they are not kept in RAM. Textures come from the converted atlases
 * made by tools/d3ds_convert.py, audio is streamed from the file on demand.
 *
 * All pointers inside a data.win are absolute file offsets, so the loaded
 * buffer is indexed directly by file offset.
 */
#ifndef D3_DATAWIN_H
#define D3_DATAWIN_H

#include "common.h"
#include "value.h"

#define EV_COUNT 15
enum {
    EV_CREATE = 0, EV_DESTROY = 1, EV_ALARM = 2, EV_STEP = 3, EV_COLLISION = 4,
    EV_KEYBOARD = 5, EV_MOUSE = 6, EV_OTHER = 7, EV_DRAW = 8, EV_KEYPRESS = 9,
    EV_KEYRELEASE = 10, EV_TRIGGER = 11, EV_CLEANUP = 12, EV_GESTURE = 13, EV_PRECREATE = 14,
};
/* Step subtypes */
enum { STEP_NORMAL = 0, STEP_BEGIN = 1, STEP_END = 2 };
/* Draw subtypes */
enum {
    DRAW_NORMAL = 0, DRAW_GUI = 64, DRAW_RESIZE = 65, DRAW_BEGIN = 72, DRAW_END = 73,
    DRAW_GUI_BEGIN = 74, DRAW_GUI_END = 75, DRAW_PRE = 76, DRAW_POST = 77,
};
/* Other subtypes */
enum {
    OTHER_OUTSIDE = 0, OTHER_BOUNDARY = 1, OTHER_GAME_START = 2, OTHER_GAME_END = 3,
    OTHER_ROOM_START = 4, OTHER_ROOM_END = 5, OTHER_ANIMATION_END = 7, OTHER_END_OF_PATH = 8,
    OTHER_USER0 = 10, OTHER_ASYNC_AUDIO_PLAYBACK = 74,
};

typedef struct {
    uint16_t sx, sy, sw, sh;  /* source rect on the texture page */
    uint16_t tx, ty, tw, th;  /* where the source rect goes inside the sprite frame */
    uint16_t bw, bh;          /* full frame size */
    int16_t page;
} TPage;

typedef struct {
    const char *name;
    int32_t w, h;
    int32_t margin_l, margin_r, margin_b, margin_t;
    int32_t bbox_mode, sep_masks;
    int32_t origin_x, origin_y;
    int32_t nframes;
    int32_t *frames; /* TPAG indices */
    float speed;
    int32_t speed_type; /* 0 = frames per second, 1 = frames per game frame */
} Sprite;

typedef struct {
    const char *name;
    int32_t tpag;
    uint32_t tile_w, tile_h, border_x, border_y, columns, tile_count;
} Background;

typedef struct {
    uint16_t ch, x, y, w, h;
    int16_t shift, offset;
} Glyph;

typedef struct {
    const char *name;
    float em_size;
    bool bold, italic;
    int32_t tpag;
    float scale_x, scale_y;
    int32_t nglyphs;
    Glyph *glyphs;
    uint16_t *glyph_lut; /* ch (0..0xFFFF) -> glyph index + 1, built lazily */
    int32_t line_height;
} Font;

typedef struct {
    const char *name;
    uint32_t flags;
    const char *type, *file;
    float volume, pitch;
    int32_t group;
    int32_t audio_id;
} Sound;

typedef struct {
    uint32_t subtype;
    int32_t code; /* code entry index, -1 if none */
} Event;

typedef struct {
    const char *name;
    int32_t sprite, mask;
    bool visible, solid, persistent;
    int32_t depth, parent;
    int32_t nevents[EV_COUNT];
    Event *events[EV_COUNT];
} Object;

typedef struct {
    int32_t x, y, obj;
    uint32_t id;
    int32_t creation_code;
    float scale_x, scale_y, image_speed;
    int32_t image_index;
    uint32_t color;
    float rotation;
    int32_t pre_create_code;
} RoomInst;

typedef struct {
    bool enabled;
    int32_t vx, vy, vw, vh, px, py, pw, ph;
    uint32_t border_x, border_y;
    int32_t speed_x, speed_y, follow;
} RoomView;

typedef struct {
    bool visible, foreground;
    int32_t sprite;
    bool tile_h, tile_v, stretch;
    uint32_t color;
    float first_frame, anim_speed;
    uint32_t anim_type;
} LayerBackground;

typedef struct {
    const char *name;
    int32_t sprite, x, y;
    float scale_x, scale_y;
    uint32_t color;
    float anim_speed;
    uint32_t anim_type;
    float frame, rotation;
} LayerSprite;

enum { LAYER_PATH = 0, LAYER_BACKGROUND = 1, LAYER_INSTANCES = 2, LAYER_ASSETS = 3, LAYER_TILES = 4, LAYER_EFFECT = 6, LAYER_PATH2 = 7 };

typedef struct {
    const char *name;
    uint32_t id;
    uint32_t type;
    int32_t depth;
    float x_offset, y_offset, h_speed, v_speed;
    bool visible;
    /* instances */
    int32_t ninst;
    uint32_t *inst_ids;
    /* tiles */
    int32_t tile_bg;
    uint32_t tiles_x, tiles_y;
    uint32_t *tile_data;
    /* background */
    LayerBackground bg;
    /* assets */
    int32_t nsprites;
    LayerSprite *sprites;
} RoomLayer;

typedef struct {
    const char *name, *caption;
    uint32_t width, height, speed;
    bool persistent;
    uint32_t bg_color;
    bool draw_bg_color;
    int32_t creation_code;
    uint32_t flags;
    int32_t nviews;
    RoomView views[8];
    int32_t ninst;
    RoomInst *inst;
    int32_t nlayers;
    RoomLayer *layers;
} Room;

typedef struct {
    const char *name;
    int32_t name_id;
    uint32_t length;
    uint16_t locals, args;
    const uint32_t *start; /* first instruction */
    const uint32_t *end;   /* end of the shared bytecode blob */
} CodeEntry;

typedef struct {
    const char *name;
    int32_t name_id;
    int32_t inst_type;
    int32_t var_id;
    int32_t builtin; /* builtin variable id or -1 */
} VarEntry;

typedef struct {
    const char *name;
    int32_t builtin; /* builtin function index or -1 */
    int32_t code;    /* code entry index or -1 */
} FuncEntry;

typedef struct {
    const char *name;
    int32_t code;
    bool constructor;
} Script;

typedef struct {
    uint32_t offset; /* absolute offset of the audio bytes in the data file */
    uint32_t length;
} AudioEntry;

typedef struct {
    char path[512];      /* path of data.win */
    char dir[512];       /* directory containing it (with trailing slash) */
    uint8_t *buf;        /* file prefix, indexed by file offset */
    uint32_t buf_len;
    uint32_t file_len;

    uint8_t bytecode;
    const char *game_name, *display_name;
    uint32_t window_w, window_h;
    float gms2_fps;
    uint32_t version_major;
    bool v2022_1, v2022_5, v2023_2, v2024_2, v2024_13; /* detected feature levels */

    int32_t nroom_order;
    int32_t *room_order;

    int32_t nstrings;
    GMStr **strings;

    int32_t ntpag;
    TPage *tpag;
    uint32_t tpag_base, tpag_stride; /* to map TPAG pointers to indices */
    uint32_t *tpag_ptrs;

    int32_t nsprites; Sprite *sprites;
    int32_t nbackgrounds; Background *backgrounds;
    int32_t nfonts; Font *fonts;
    int32_t nsounds; Sound *sounds;
    int32_t nobjects; Object *objects;
    int32_t nrooms; Room *rooms;
    int32_t ncode; CodeEntry *code;
    int32_t nvars; VarEntry *vars;
    int32_t nfuncs; FuncEntry *funcs;
    int32_t nscripts; Script *scripts;
    int32_t nglobal_init; int32_t *global_init;
    int32_t naudio; AudioEntry *audio;
    int32_t naudio_groups; const char **audio_groups;
    int32_t ntextures;
} DataWin;

/* Loads and parses a data.win. Returns false and fills err on failure. */
bool datawin_load(DataWin *dw, const char *path, char *err, size_t errlen);
void datawin_free(DataWin *dw);

/* Looks up a TPAG index from an absolute pointer to a TPAG entry. */
int32_t datawin_tpag_index(const DataWin *dw, uint32_t ptr);

/* Resource lookup by name (linear; used by asset_get_index etc.) */
int32_t datawin_find_code(const DataWin *dw, const char *name);

#endif
