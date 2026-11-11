/*
 * Nintendo 3DS platform layer (libctru + citro2d + ndsp + tremor).
 *
 * Top screen: the game, scaled to fit 400x240 (a 640x480 game is shown at
 * exactly half size, 320x240, which is pixel-perfect for games drawn at 2x).
 * Bottom screen: text console with status and log output.
 *
 * Build variants (see Makefile):
 *   D3DS_NEW3DS defined  -> New 3DS: 804 MHz + L2 cache, RGBA8 textures,
 *                           bigger texture cache, audio decoding on core 2.
 *   otherwise            -> Old 3DS: RGBA4 textures (half the memory),
 *                           smaller cache, audio decoding on core 1.
 */
#ifdef __3DS__
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <tremor/ivorbisfile.h>
#include "platform.h"
#include "../core/common.h"
#include <sys/stat.h>

#ifdef D3DS_NEW3DS
#define TEX_BUDGET (40u << 20)
#define AUDIO_CORE 2
#else
#define TEX_BUDGET (14u << 20)
#define AUDIO_CORE 1
#endif

/* Linear heap (textures + audio). The rest of app memory is the normal heap
 * that holds game data. */
#ifdef D3DS_NEW3DS
u32 __ctru_linear_heap_size = 56u << 20;
#else
u32 __ctru_linear_heap_size = 22u << 20;
#endif
/* Deep GML recursion needs a larger main thread stack. */
u32 __stacksize__ = 512 * 1024;

#define SCREEN_W 400
#define SCREEN_H 240

static C3D_RenderTarget *g_top;
static float g_scale = 0.5f, g_offx = 40, g_offy = 0;
static int g_win_w = 640, g_win_h = 480;
static float g_depth;
static bool g_new3ds;
static bool g_quit;
static u64 g_quit_hold_start;
static char g_save_dir[512];
static PrintConsole g_console;

/* ------------------------------------------------------------------ */
/* system                                                              */
/* ------------------------------------------------------------------ */

bool plat_init(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, &g_console);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    g_new3ds = n3ds;
#ifdef D3DS_NEW3DS
    if (g_new3ds) osSetSpeedupEnable(true);
    else printf("\x1b[33mWarning: this is the New 3DS build running on an Old 3DS.\x1b[0m\n");
#else
    APT_SetAppCpuTimeLimit(30);
#endif
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
    C2D_Prepare();
    C2D_SetTintMode(C2D_TintMult);
    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    snprintf(g_save_dir, sizeof g_save_dir, "%ssaves/", plat_data_root());
    mkdir("sdmc:/3ds", 0777);
    mkdir(plat_data_root(), 0777);
    mkdir(g_save_dir, 0777);
    return true;
}

void plat_shutdown(void) {
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

bool plat_should_quit(void) { return g_quit || !aptMainLoop(); }

void plat_log_line(const char *tag, const char *msg) {
    const char *col = tag[0] == 'E' ? "\x1b[31m" : tag[0] == 'W' ? "\x1b[33m" : "";
    printf("%s%s\x1b[0m\n", col, msg);
    static FILE *logf;
    if (!logf) {
        char p[256];
        snprintf(p, sizeof p, "%sdelta3ds.log", plat_data_root());
        logf = fopen(p, "w");
    }
    if (logf) {
        fprintf(logf, "[%s] %s\n", tag, msg);
        fflush(logf);
    }
}

void plat_fatal(const char *msg) {
    printf("\x1b[31m\nFATAL: %s\x1b[0m\n\nPress START to exit.\n", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    plat_shutdown();
    exit(1);
}

void *plat_alloc_big(size_t n) { return malloc(n ? n : 1); }
void plat_free_big(void *p) { free(p); }

void plat_loading_progress(const char *what, float frac) {
    static int last = -1;
    int pct = (int)(frac * 100);
    if (pct / 5 == last / 5) return;
    last = pct;
    printf("\r%s... %3d%%", what, pct);
    if (pct >= 100) printf("\n");
    gfxFlushBuffers();
    gfxSwapBuffers();
}

uint64_t plat_time_us(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000); }
void plat_sleep_us(uint64_t us) { svcSleepThread((s64)us * 1000); }
const char *plat_data_root(void) { return "sdmc:/3ds/Delta3DS/"; }
const char *plat_save_dir(void) { return g_save_dir; }
void plat_show_message(const char *msg) {
    printf("\n\x1b[36m%s\x1b[0m\n(press A)\n", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_A) break;
        gspWaitForVBlank();
    }
}

/* ------------------------------------------------------------------ */
/* textures                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int16_t atlas;
    uint16_t x, y, w, h;   /* rect in the atlas */
    uint16_t ow, oh;       /* original source size of the item */
} TpagMap;

typedef struct {
    bool loaded, failed;
    C3D_Tex tex;
    uint16_t w, h;
    uint32_t bytes;
    uint64_t last_used;
} Atlas;

static TpagMap *g_map;
static int g_nmap;
static Atlas *g_atlases;
static int g_natlas;
static char g_tex_dir[512];
static uint32_t g_tex_bytes;
static uint64_t g_frame_no;

static void atlas_unload(Atlas *a) {
    if (!a->loaded) return;
    C3D_TexDelete(&a->tex);
    g_tex_bytes -= a->bytes;
    a->loaded = false;
}

static void evict_until(uint32_t need) {
    while (g_tex_bytes + need > TEX_BUDGET) {
        Atlas *lru = NULL;
        for (int i = 0; i < g_natlas; i++) {
            Atlas *a = &g_atlases[i];
            if (a->loaded && a->last_used < g_frame_no && (!lru || a->last_used < lru->last_used)) lru = a;
        }
        if (!lru) return;
        atlas_unload(lru);
    }
}

/* Atlas file: "D3TX", u16 w, u16 h, u32 format (0 = RGBA8), then w*h*4 bytes
 * already in 3DS tiled order (Morton 8x8, bottom-up), bytes A,B,G,R. */
static bool atlas_load(int idx) {
    Atlas *a = &g_atlases[idx];
    if (a->loaded) return true;
    if (a->failed) return false;
    char path[600];
    snprintf(path, sizeof path, "%satlas_%03d.bin", g_tex_dir, idx);
    FILE *f = fopen(path, "rb");
    if (!f) { a->failed = true; LOGW("missing %s", path); return false; }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "D3TX", 4) != 0) { fclose(f); a->failed = true; return false; }
    uint16_t w = rd_u16(hdr + 4), h = rd_u16(hdr + 6);
#ifdef D3DS_NEW3DS
    GPU_TEXCOLOR fmt = GPU_RGBA8;
    uint32_t bpp = 4;
#else
    GPU_TEXCOLOR fmt = GPU_RGBA4;
    uint32_t bpp = 2;
#endif
    uint32_t bytes = (uint32_t)w * h * bpp;
    evict_until(bytes);
    if (!C3D_TexInit(&a->tex, w, h, fmt)) {
        /* out of linear memory: evict everything not used this frame and retry */
        evict_until(TEX_BUDGET);
        if (!C3D_TexInit(&a->tex, w, h, fmt)) { fclose(f); a->failed = true; LOGE("no memory for texture %d", idx); return false; }
    }
    uint32_t rgba_bytes = (uint32_t)w * h * 4;
#ifdef D3DS_NEW3DS
    size_t got = fread(a->tex.data, 1, rgba_bytes, f);
#else
    /* read in chunks and pack ABGR8888 -> RGBA4444 */
    uint16_t *dst = (uint16_t *)a->tex.data;
    uint8_t buf[4096];
    size_t got = 0;
    while (got < rgba_bytes) {
        size_t n = fread(buf, 1, sizeof buf, f);
        if (!n) break;
        for (size_t i = 0; i + 3 < n; i += 4) {
            uint8_t A = buf[i], B = buf[i + 1], G = buf[i + 2], Rr = buf[i + 3];
            dst[(got + i) / 4] = (uint16_t)(((Rr >> 4) << 12) | ((G >> 4) << 8) | ((B >> 4) << 4) | (A >> 4));
        }
        got += n;
    }
#endif
    fclose(f);
    if (got != rgba_bytes) LOGW("short texture file %s", path);
    C3D_TexFlush(&a->tex);
    C3D_TexSetFilter(&a->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&a->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    a->w = w;
    a->h = h;
    a->bytes = bytes;
    a->loaded = true;
    g_tex_bytes += bytes;
    return true;
}

bool plat_textures_open(const char *game_dir, int ntpag) {
    plat_textures_close();
    snprintf(g_tex_dir, sizeof g_tex_dir, "%sd3ds/", game_dir);
    char path[600];
    snprintf(path, sizeof path, "%satlas.idx", g_tex_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "D3AI", 4) != 0) { fclose(f); return false; }
    int n = (int)rd_u32(hdr + 8), na = (int)rd_u32(hdr + 12);
    if (n != ntpag) LOGW("atlas index has %d items, game has %d (re-run the converter?)", n, ntpag);
    g_nmap = n;
    g_map = d3_calloc((size_t)n, sizeof(TpagMap));
    for (int i = 0; i < n; i++) {
        uint8_t e[14];
        if (fread(e, 1, 14, f) != 14) break;
        g_map[i].atlas = (int16_t)rd_u16(e);
        g_map[i].x = rd_u16(e + 2); g_map[i].y = rd_u16(e + 4);
        g_map[i].w = rd_u16(e + 6); g_map[i].h = rd_u16(e + 8);
        g_map[i].ow = rd_u16(e + 10); g_map[i].oh = rd_u16(e + 12);
    }
    fclose(f);
    g_natlas = na;
    g_atlases = d3_calloc((size_t)na, sizeof(Atlas));
    LOGI("%d texture items in %d atlases", n, na);
    return true;
}

void plat_textures_close(void) {
    for (int i = 0; i < g_natlas; i++) atlas_unload(&g_atlases[i]);
    free(g_atlases);
    free(g_map);
    g_atlases = NULL;
    g_map = NULL;
    g_natlas = g_nmap = 0;
}

void plat_room_changed(void) {
    /* drop atlases that were not used recently */
    for (int i = 0; i < g_natlas; i++)
        if (g_atlases[i].loaded && g_atlases[i].last_used + 120 < g_frame_no) atlas_unload(&g_atlases[i]);
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

void plat_set_window_size(int w, int h) {
    g_win_w = w > 0 ? w : 640;
    g_win_h = h > 0 ? h : 480;
    float sx = (float)SCREEN_W / g_win_w, sy = (float)SCREEN_H / g_win_h;
    g_scale = sx < sy ? sx : sy;
    g_offx = (SCREEN_W - g_win_w * g_scale) / 2;
    g_offy = (SCREEN_H - g_win_h * g_scale) / 2;
}

static uint32_t to_c2d(uint32_t abgr) { return abgr; /* GameMaker BGR + alpha byte == citro2d ABGR */ }

void plat_frame_begin(uint32_t clear) {
    g_frame_no++;
    g_depth = 0.5f;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(g_top, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(g_top);
    C2D_DrawRectSolid(g_offx, g_offy, 0.5f, g_win_w * g_scale, g_win_h * g_scale, to_c2d(clear | 0xFF000000u));
}

void plat_frame_end(void) {
    /* letterbox bars hide anything drawn outside the game window */
    uint32_t black = C2D_Color32(0, 0, 0, 255);
    plat_set_blend_mode(0);
    if (g_offx > 0) {
        C2D_DrawRectSolid(0, 0, 1.0f, g_offx, SCREEN_H, black);
        C2D_DrawRectSolid(SCREEN_W - g_offx, 0, 1.0f, g_offx, SCREEN_H, black);
    }
    if (g_offy > 0) {
        C2D_DrawRectSolid(0, 0, 1.0f, SCREEN_W, g_offy, black);
        C2D_DrawRectSolid(0, SCREEN_H - g_offy, 1.0f, SCREEN_W, g_offy, black);
    }
    C3D_FrameEnd(0);
}

static inline float sx_(float x) { return g_offx + x * g_scale; }
static inline float sy_(float y) { return g_offy + y * g_scale; }

void plat_draw_image(int tpag, float sx, float sy, float sw, float sh, float x, float y, float w, float h, float px,
                     float py, float angle, uint32_t abgr) {
    if (tpag < 0 || tpag >= g_nmap || g_map[tpag].atlas < 0 || g_map[tpag].atlas >= g_natlas) {
        /* no converted texture: faint placeholder so layouts stay visible */
        C2D_DrawRectSolid(sx_(x - px), sy_(y - py), 0.5f, fabsf(w) * g_scale, fabsf(h) * g_scale,
                          (abgr & 0x00FFFFFF) | 0x40000000);
        return;
    }
    const TpagMap *m = &g_map[tpag];
    Atlas *a = &g_atlases[m->atlas];
    if (!atlas_load(m->atlas)) return;
    a->last_used = g_frame_no;
    float kx = m->ow ? (float)m->w / m->ow : 1, ky = m->oh ? (float)m->h / m->oh : 1;
    float ax = m->x + sx * kx, ay = m->y + sy * ky, aw = sw * kx, ah = sh * ky;
    Tex3DS_SubTexture sub;
    sub.width = (u16)(aw + 0.5f);
    sub.height = (u16)(ah + 0.5f);
    sub.left = ax / a->w;
    sub.right = (ax + aw) / a->w;
    sub.top = 1.0f - ay / a->h;
    sub.bottom = 1.0f - (ay + ah) / a->h;
    /* mirroring: swap texture coordinates, keep a positive size */
    float cx = px, cy = py;
    if (w < 0) { float t = sub.left; sub.left = sub.right; sub.right = t; cx = px - w; w = -w; }
    if (h < 0) { float t = sub.top; sub.top = sub.bottom; sub.bottom = t; cy = py - h; h = -h; }
    C2D_Image img = {&a->tex, &sub};
    C2D_DrawParams p;
    p.pos.x = sx_(x);
    p.pos.y = sy_(y);
    p.pos.w = w * g_scale;
    p.pos.h = h * g_scale;
    p.center.x = cx * g_scale;
    p.center.y = cy * g_scale;
    p.depth = 0.5f;
    p.angle = angle;
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, to_c2d(abgr), 1.0f);
    C2D_DrawImage(img, &p, &tint);
}

void plat_draw_rect(float x, float y, float w, float h, uint32_t abgr) {
    C2D_DrawRectSolid(sx_(x), sy_(y), 0.5f, w * g_scale, h * g_scale, to_c2d(abgr));
}
void plat_draw_rect_gradient(float x, float y, float w, float h, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4) {
    C2D_DrawRectangle(sx_(x), sy_(y), 0.5f, w * g_scale, h * g_scale, to_c2d(c1), to_c2d(c2), to_c2d(c4), to_c2d(c3));
}
void plat_draw_triangle(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t abgr) {
    uint32_t c = to_c2d(abgr);
    C2D_DrawTriangle(sx_(x1), sy_(y1), c, sx_(x2), sy_(y2), c, sx_(x3), sy_(y3), c, 0.5f);
}
void plat_draw_line(float x1, float y1, float x2, float y2, float width, uint32_t abgr) {
    uint32_t c = to_c2d(abgr);
    float t = width * g_scale;
    if (t < 1) t = 1;
    C2D_DrawLine(sx_(x1), sy_(y1), c, sx_(x2), sy_(y2), c, t, 0.5f);
}

void plat_set_blend_mode(int mode) {
    static int cur = -1;
    if (mode == cur) return;
    C2D_Flush();
    cur = mode;
    switch (mode) {
    case 1: /* bm_add */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
        break;
    case 2: /* bm_max */
        C3D_AlphaBlend(GPU_BLEND_MAX, GPU_BLEND_MAX, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        break;
    case 3: /* bm_subtract */
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
        break;
    default:
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE,
                       GPU_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

void plat_set_clip(float x, float y, float w, float h) {
    /* Views in GameMaker games almost always cover the whole window; the
     * letterbox bars handle the rest. */
    (void)x; (void)y; (void)w; (void)h;
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

enum { VK_ENTER = 13, VK_SHIFT = 16, VK_CONTROL = 17, VK_ESCAPE = 27, VK_LEFT = 37, VK_UP = 38, VK_RIGHT = 39,
       VK_DOWN = 40, VK_F1 = 112 };

void plat_input_state(uint8_t keys[256]) {
    hidScanInput();
    u32 held = hidKeysHeld();
    circlePosition cp;
    hidCircleRead(&cp);
    memset(keys, 0, 256);
    const int dead = 40;
    if ((held & KEY_DUP) || cp.dy > dead) keys[VK_UP] = 1;
    if ((held & KEY_DDOWN) || cp.dy < -dead) keys[VK_DOWN] = 1;
    if ((held & KEY_DLEFT) || cp.dx < -dead) keys[VK_LEFT] = 1;
    if ((held & KEY_DRIGHT) || cp.dx > dead) keys[VK_RIGHT] = 1;
    /* A = confirm (Z / Enter), B = cancel / run (X / Shift), X = menu (C / Ctrl),
     * Y = run (Shift), START = menu, SELECT = Escape */
    if (held & KEY_A) { keys['Z'] = 1; keys[VK_ENTER] = 1; }
    if (held & KEY_B) { keys['X'] = 1; keys[VK_SHIFT] = 1; }
    if (held & KEY_X) { keys['C'] = 1; keys[VK_CONTROL] = 1; }
    if (held & KEY_Y) keys[VK_SHIFT] = 1;
    if (held & KEY_START) keys['C'] = 1;
    if (held & KEY_SELECT) keys[VK_ESCAPE] = 1;
    if (held & KEY_L) keys[VK_F1] = 1;
    /* START + SELECT for one second returns to the launcher */
    if ((held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
        if (!g_quit_hold_start) g_quit_hold_start = osGetTime();
        else if (osGetTime() - g_quit_hold_start > 1000) g_quit = true;
    } else {
        g_quit_hold_start = 0;
    }
}

void plat_reset_quit(void) {
    g_quit = false;
    g_quit_hold_start = 0;
}

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */

#define MAX_BUFFERS 512
#define MAX_VOICES 24
#define STREAM_BUFS 4
#define STREAM_SAMPLES 4096

typedef struct {
    bool used, stream;
    char path[512];
    uint32_t offset, length;
    /* static sounds */
    int16_t *pcm;
    uint32_t frames;
    int channels, rate;
} Buffer;

typedef struct {
    FILE *f;
    uint32_t start, length, pos;
} Region;

typedef struct {
    bool used, loop, paused, stream, finished;
    int buffer;
    ndspWaveBuf wb[STREAM_BUFS];
    int16_t *sbuf[STREAM_BUFS];
    OggVorbis_File vf;
    Region region;
    bool vf_open;
    float gain, pitch;
    int channels, rate;
    uint64_t played_frames;
} Voice;

static Buffer g_buf[MAX_BUFFERS];
static Voice g_voice[MAX_VOICES];
static bool g_audio_ok;
static LightLock g_audio_lock;
static Thread g_audio_thread;
static volatile bool g_audio_run;

static size_t rg_read(void *ptr, size_t size, size_t n, void *ds) {
    Region *r = ds;
    size_t want = size * n;
    if (r->pos + want > r->length) want = r->length - r->pos;
    if (!want) return 0;
    fseek(r->f, (long)(r->start + r->pos), SEEK_SET);
    size_t got = fread(ptr, 1, want, r->f);
    r->pos += (uint32_t)got;
    return size ? got / size : 0;
}
static int rg_seek(void *ds, ogg_int64_t off, int whence) {
    Region *r = ds;
    int64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)r->pos + off : (int64_t)r->length + off;
    if (np < 0 || np > r->length) return -1;
    r->pos = (uint32_t)np;
    return 0;
}
static int rg_close(void *ds) {
    Region *r = ds;
    if (r->f) fclose(r->f);
    r->f = NULL;
    return 0;
}
static long rg_tell(void *ds) { return (long)((Region *)ds)->pos; }

static bool region_open(Region *r, const char *path, uint32_t offset, uint32_t length) {
    r->f = fopen(path, "rb");
    if (!r->f) return false;
    if (!length) {
        fseek(r->f, 0, SEEK_END);
        length = (uint32_t)ftell(r->f) - offset;
    }
    r->start = offset;
    r->length = length;
    r->pos = 0;
    return true;
}

static bool open_vorbis(OggVorbis_File *vf, Region *r) {
    ov_callbacks cb = {rg_read, rg_seek, rg_close, rg_tell};
    return ov_open_callbacks(r, vf, NULL, 0, cb) == 0;
}

/* Decodes a WAV (PCM 8/16 bit) into linear memory. */
static bool decode_wav(Buffer *b, const uint8_t *d, uint32_t n) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
    uint32_t p = 12;
    int ch = 1, rate = 44100, bits = 16;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    while (p + 8 <= n) {
        uint32_t cl = rd_u32(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && p + 24 <= n) {
            ch = rd_u16(d + p + 10);
            rate = (int)rd_u32(d + p + 12);
            bits = rd_u16(d + p + 22);
        } else if (!memcmp(d + p, "data", 4)) {
            data = d + p + 8;
            dlen = cl;
            if (p + 8 + dlen > n) dlen = n - p - 8;
            break;
        }
        p += 8 + cl + (cl & 1);
    }
    if (!data || (bits != 8 && bits != 16) || ch < 1 || ch > 2) return false;
    uint32_t frames = dlen / (uint32_t)(ch * bits / 8);
    b->pcm = linearAlloc(frames * (uint32_t)ch * 2 + 4);
    if (!b->pcm) return false;
    for (uint32_t i = 0; i < frames * (uint32_t)ch; i++)
        b->pcm[i] = bits == 16 ? (int16_t)rd_u16(data + i * 2) : (int16_t)(((int)data[i] - 128) << 8);
    b->frames = frames;
    b->channels = ch;
    b->rate = rate;
    DSP_FlushDataCache(b->pcm, frames * (uint32_t)ch * 2);
    return true;
}

static bool decode_ogg_full(Buffer *b) {
    Region r;
    if (!region_open(&r, b->path, b->offset, b->length)) return false;
    OggVorbis_File vf;
    if (!open_vorbis(&vf, &r)) { rg_close(&r); return false; }
    vorbis_info *vi = ov_info(&vf, -1);
    int ch = vi->channels > 2 ? 2 : vi->channels;
    ogg_int64_t total = ov_pcm_total(&vf, -1);
    if (total <= 0 || total > 44100 * 60) { ov_clear(&vf); return false; }
    b->pcm = linearAlloc((size_t)total * (size_t)vi->channels * 2 + 4096);
    if (!b->pcm) { ov_clear(&vf); return false; }
    size_t pos = 0, cap = (size_t)total * (size_t)vi->channels * 2;
    int bs;
    while (pos < cap) {
        long got = ov_read(&vf, (char *)b->pcm + pos, (int)(cap - pos > 4096 ? 4096 : cap - pos), &bs);
        if (got <= 0) break;
        pos += (size_t)got;
    }
    b->frames = (uint32_t)(pos / (size_t)(vi->channels * 2));
    b->channels = ch;
    b->rate = (int)vi->rate;
    ov_clear(&vf);
    DSP_FlushDataCache(b->pcm, (uint32_t)pos);
    return true;
}

static void stream_fill(Voice *v, int i) {
    ndspWaveBuf *wb = &v->wb[i];
    int bs;
    size_t want = STREAM_SAMPLES * (size_t)v->channels * 2, pos = 0;
    char *dst = (char *)v->sbuf[i];
    while (pos < want) {
        long got = ov_read(&v->vf, dst + pos, (int)(want - pos), &bs);
        if (got <= 0) {
            if (v->loop && ov_pcm_seek(&v->vf, 0) == 0) continue;
            break;
        }
        pos += (size_t)got;
    }
    if (!pos) { v->finished = true; return; }
    memset(wb, 0, sizeof *wb);
    wb->data_vaddr = dst;
    wb->nsamples = (u32)(pos / (size_t)(v->channels * 2));
    DSP_FlushDataCache(dst, (u32)pos);
    ndspChnWaveBufAdd(v - g_voice, wb);
}

static void audio_thread(void *arg) {
    (void)arg;
    while (g_audio_run) {
        LightLock_Lock(&g_audio_lock);
        for (int c = 0; c < MAX_VOICES; c++) {
            Voice *v = &g_voice[c];
            if (!v->used || !v->stream || v->finished || v->paused) continue;
            for (int i = 0; i < STREAM_BUFS; i++) {
                if (v->wb[i].status == NDSP_WBUF_DONE || v->wb[i].status == NDSP_WBUF_FREE) {
                    if (v->wb[i].status == NDSP_WBUF_DONE) v->played_frames += v->wb[i].nsamples;
                    stream_fill(v, i);
                    if (v->finished) break;
                }
            }
        }
        LightLock_Unlock(&g_audio_lock);
        svcSleepThread(10 * 1000 * 1000);
    }
}

static void audio_init_once(void) {
    static bool tried;
    if (tried) return;
    tried = true;
    if (R_FAILED(ndspInit())) {
        LOGW("Audio disabled: ndspInit failed (dump DSP firmware with DSP1 to sdmc:/3ds/dspfirm.cdc)");
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    LightLock_Init(&g_audio_lock);
    g_audio_run = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_audio_thread = threadCreate(audio_thread, NULL, 32 * 1024, prio - 1, AUDIO_CORE, false);
    if (!g_audio_thread) g_audio_thread = threadCreate(audio_thread, NULL, 32 * 1024, prio - 1, -2, false);
    g_audio_ok = true;
}

int plat_audio_load(const char *path, uint32_t offset, uint32_t length, bool stream) {
    audio_init_once();
    if (!g_audio_ok) return -1;
    int id = -1;
    for (int i = 0; i < MAX_BUFFERS; i++) if (!g_buf[i].used) { id = i; break; }
    if (id < 0) return -1;
    Buffer *b = &g_buf[id];
    memset(b, 0, sizeof *b);
    snprintf(b->path, sizeof b->path, "%s", path);
    b->offset = offset;
    b->length = length;
    /* sniff the format */
    Region r;
    if (!region_open(&r, path, offset, length)) return -1;
    uint8_t magic[4] = {0};
    rg_read(magic, 1, 4, &r);
    rg_close(&r);
    if (!memcmp(magic, "OggS", 4)) {
        b->stream = true;
        /* short effects are decoded fully so they can overlap cheaply */
        if (!stream && length && length < 256 * 1024 && decode_ogg_full(b)) b->stream = false;
    } else if (!memcmp(magic, "RIFF", 4)) {
        uint8_t *d = malloc(length ? length : 1);
        if (!d) return -1;
        if (!region_open(&r, path, offset, length)) { free(d); return -1; }
        rg_read(d, 1, length, &r);
        rg_close(&r);
        bool ok = decode_wav(b, d, length);
        free(d);
        if (!ok) return -1;
    } else {
        return -1;
    }
    b->used = true;
    return id;
}

static void voice_stop_locked(int c) {
    Voice *v = &g_voice[c];
    if (!v->used) return;
    ndspChnWaveBufClear(c);
    if (v->vf_open) ov_clear(&v->vf);
    for (int i = 0; i < STREAM_BUFS; i++) if (v->sbuf[i]) linearFree(v->sbuf[i]);
    memset(v, 0, sizeof *v);
}

void plat_audio_unload(int id) {
    if (id < 0 || id >= MAX_BUFFERS || !g_buf[id].used) return;
    LightLock_Lock(&g_audio_lock);
    for (int c = 0; c < MAX_VOICES; c++) if (g_voice[c].used && g_voice[c].buffer == id) voice_stop_locked(c);
    LightLock_Unlock(&g_audio_lock);
    if (g_buf[id].pcm) linearFree(g_buf[id].pcm);
    memset(&g_buf[id], 0, sizeof g_buf[id]);
}

static void set_mix(int c, float gain) {
    float mix[12] = {0};
    mix[0] = mix[1] = gain;
    ndspChnSetMix(c, mix);
}

int plat_audio_play(int id, bool loop, float gain, float pitch) {
    if (!g_audio_ok || id < 0 || id >= MAX_BUFFERS || !g_buf[id].used) return -1;
    Buffer *b = &g_buf[id];
    LightLock_Lock(&g_audio_lock);
    int c = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (g_voice[i].used && !g_voice[i].paused && !ndspChnIsPlaying(i) && (!g_voice[i].stream || g_voice[i].finished))
            voice_stop_locked(i);
        if (!g_voice[i].used && c < 0) c = i;
    }
    if (c < 0) { LightLock_Unlock(&g_audio_lock); return -1; }
    Voice *v = &g_voice[c];
    memset(v, 0, sizeof *v);
    v->used = true;
    v->loop = loop;
    v->buffer = id;
    v->gain = gain;
    v->pitch = pitch > 0 ? pitch : 1;
    ndspChnReset(c);
    ndspChnSetInterp(c, NDSP_INTERP_LINEAR);
    if (b->stream) {
        if (!region_open(&v->region, b->path, b->offset, b->length) || !open_vorbis(&v->vf, &v->region)) {
            v->used = false;
            LightLock_Unlock(&g_audio_lock);
            return -1;
        }
        v->vf_open = true;
        v->stream = true;
        vorbis_info *vi = ov_info(&v->vf, -1);
        v->channels = vi->channels > 2 ? 2 : vi->channels;
        v->rate = (int)vi->rate;
        for (int i = 0; i < STREAM_BUFS; i++) v->sbuf[i] = linearAlloc(STREAM_SAMPLES * 2 * 2);
        ndspChnSetFormat(c, v->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        ndspChnSetRate(c, v->rate * v->pitch);
        set_mix(c, gain);
        for (int i = 0; i < STREAM_BUFS && !v->finished; i++) stream_fill(v, i);
    } else {
        v->channels = b->channels;
        v->rate = b->rate;
        ndspChnSetFormat(c, b->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        ndspChnSetRate(c, b->rate * v->pitch);
        set_mix(c, gain);
        memset(&v->wb[0], 0, sizeof v->wb[0]);
        v->wb[0].data_vaddr = b->pcm;
        v->wb[0].nsamples = b->frames;
        v->wb[0].looping = loop;
        ndspChnWaveBufAdd(c, &v->wb[0]);
    }
    LightLock_Unlock(&g_audio_lock);
    return c;
}

void plat_audio_stop(int c) {
    if (!g_audio_ok || c < 0 || c >= MAX_VOICES) return;
    LightLock_Lock(&g_audio_lock);
    voice_stop_locked(c);
    LightLock_Unlock(&g_audio_lock);
}

bool plat_audio_is_playing(int c) {
    if (!g_audio_ok || c < 0 || c >= MAX_VOICES || !g_voice[c].used) return false;
    if (g_voice[c].paused) return true;
    if (g_voice[c].stream) return !g_voice[c].finished || ndspChnIsPlaying(c);
    return ndspChnIsPlaying(c);
}

void plat_audio_set_gain(int c, float g) {
    if (!g_audio_ok || c < 0 || c >= MAX_VOICES || !g_voice[c].used) return;
    g_voice[c].gain = g;
    set_mix(c, g);
}

void plat_audio_set_pitch(int c, float p) {
    if (!g_audio_ok || c < 0 || c >= MAX_VOICES || !g_voice[c].used || p <= 0) return;
    g_voice[c].pitch = p;
    ndspChnSetRate(c, g_voice[c].rate * p);
}

void plat_audio_pause(int c, bool paused) {
    if (!g_audio_ok || c < 0 || c >= MAX_VOICES || !g_voice[c].used) return;
    g_voice[c].paused = paused;
    ndspChnSetPaused(c, paused);
}

float plat_audio_get_position(int c) {
    if (!g_audio_ok || c < 0 || c >= MAX_VOICES || !g_voice[c].used) return 0;
    Voice *v = &g_voice[c];
    if (v->stream) return v->vf_open ? (float)ov_time_tell(&v->vf) / 1000.0f : 0;
    return v->rate ? (float)ndspChnGetSamplePos(c) / (float)v->rate : 0;
}

void plat_audio_set_position(int c, float seconds) {
    if (!g_audio_ok || c < 0 || c >= MAX_VOICES || !g_voice[c].used) return;
    Voice *v = &g_voice[c];
    if (!v->stream) return; /* static sounds restart only */
    LightLock_Lock(&g_audio_lock);
    ndspChnWaveBufClear(c);
    ov_time_seek(&v->vf, (ogg_int64_t)(seconds * 1000));
    v->finished = false;
    for (int i = 0; i < STREAM_BUFS; i++) v->wb[i].status = NDSP_WBUF_FREE;
    LightLock_Unlock(&g_audio_lock);
}

float plat_audio_length(int id) {
    if (id < 0 || id >= MAX_BUFFERS || !g_buf[id].used) return 0;
    Buffer *b = &g_buf[id];
    if (!b->stream) return b->rate ? (float)b->frames / (float)b->rate : 0;
    Region r;
    if (!region_open(&r, b->path, b->offset, b->length)) return 0;
    OggVorbis_File vf;
    if (!open_vorbis(&vf, &r)) { rg_close(&r); return 0; }
    float len = (float)ov_time_total(&vf, -1) / 1000.0f;
    ov_clear(&vf);
    return len;
}

void plat_audio_shutdown(void) {
    if (!g_audio_ok) return;
    g_audio_run = false;
    if (g_audio_thread) { threadJoin(g_audio_thread, U64_MAX); threadFree(g_audio_thread); }
    for (int c = 0; c < MAX_VOICES; c++) voice_stop_locked(c);
    for (int i = 0; i < MAX_BUFFERS; i++) if (g_buf[i].used && g_buf[i].pcm) linearFree(g_buf[i].pcm);
    memset(g_buf, 0, sizeof g_buf);
    ndspExit();
    g_audio_ok = false;
}

#endif /* __3DS__ */
