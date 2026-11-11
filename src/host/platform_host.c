/*
 * Headless desktop platform used for development and the test suite.
 * It runs the game logic, counts draw calls and can print them, so the
 * interpreter can be tested without a 3DS.
 */
#ifndef __3DS__
#include "../platform/platform.h"
#include "../core/common.h"
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

static bool g_trace_draw;
static uint64_t g_draw_calls;
static uint8_t g_keys[256];
static char g_save_dir[512] = "./d3ds_save/";

/* test hooks */
void host_set_key(int key, bool down) { if (key >= 0 && key < 256) g_keys[key] = down; }
uint64_t host_draw_calls(void) { return g_draw_calls; }
void host_set_save_dir(const char *dir) { snprintf(g_save_dir, sizeof g_save_dir, "%s", dir); }

bool plat_init(void) {
    g_trace_draw = getenv("D3_TRACE_DRAW") != NULL;
    return true;
}
void plat_shutdown(void) {}
bool plat_should_quit(void) { return false; }
void plat_log_line(const char *tag, const char *msg) { fprintf(stderr, "[%s] %s\n", tag, msg); }
void plat_fatal(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}
void *plat_alloc_big(size_t n) { return malloc(n ? n : 1); }
void plat_free_big(void *p) { free(p); }
void plat_loading_progress(const char *what, float frac) { (void)what; (void)frac; }
uint64_t plat_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
void plat_sleep_us(uint64_t us) { (void)us; }
const char *plat_data_root(void) { return "./"; }
const char *plat_save_dir(void) {
    mkdir(g_save_dir, 0777);
    return g_save_dir;
}
void plat_room_changed(void) {}

void plat_set_window_size(int w, int h) { (void)w; (void)h; }
void plat_frame_begin(uint32_t clear) {
    (void)clear;
    if (g_trace_draw) printf("-- frame (clear %08X)\n", clear);
}
void plat_frame_end(void) {}
bool plat_textures_open(const char *dir, int ntpag) { (void)dir; (void)ntpag; return true; }
void plat_textures_close(void) {}

void plat_draw_image(int tpag, float sx, float sy, float sw, float sh, float x, float y, float w, float h, float px,
                     float py, float angle, uint32_t abgr) {
    g_draw_calls++;
    if (g_trace_draw)
        printf("image tpag=%d src=(%.0f,%.0f %.0fx%.0f) at=(%.1f,%.1f) size=(%.1f,%.1f) pivot=(%.1f,%.1f) rot=%.2f col=%08X\n",
               tpag, sx, sy, sw, sh, x, y, w, h, px, py, angle, abgr);
}
void plat_draw_rect(float x, float y, float w, float h, uint32_t abgr) {
    g_draw_calls++;
    if (g_trace_draw) printf("rect (%.1f,%.1f %.1fx%.1f) col=%08X\n", x, y, w, h, abgr);
}
void plat_draw_rect_gradient(float x, float y, float w, float h, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4) {
    (void)c2; (void)c3; (void)c4;
    plat_draw_rect(x, y, w, h, c1);
}
void plat_draw_triangle(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t abgr) {
    g_draw_calls++;
    if (g_trace_draw) printf("tri (%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) col=%08X\n", x1, y1, x2, y2, x3, y3, abgr);
}
void plat_draw_line(float x1, float y1, float x2, float y2, float width, uint32_t abgr) {
    g_draw_calls++;
    if (g_trace_draw) printf("line (%.1f,%.1f)-(%.1f,%.1f) w=%.1f col=%08X\n", x1, y1, x2, y2, width, abgr);
}
void plat_set_blend_mode(int mode) { (void)mode; }
void plat_set_clip(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; }

void plat_input_state(uint8_t keys[256]) { memcpy(keys, g_keys, 256); }

/* Audio: keeps track of "voices" so the game logic sees plausible state. */
typedef struct { bool used; int buffer; uint64_t start; bool loop; } HostVoice;
static HostVoice g_voices[32];
static int g_nbuf;
int plat_audio_load(const char *path, uint32_t offset, uint32_t length, bool stream) {
    (void)offset; (void)length; (void)stream;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fclose(f);
    return g_nbuf++;
}
void plat_audio_unload(int buffer) { (void)buffer; }
int plat_audio_play(int buffer, bool loop, float gain, float pitch) {
    (void)gain; (void)pitch;
    for (int i = 0; i < 32; i++) {
        if (!g_voices[i].used) {
            g_voices[i] = (HostVoice){true, buffer, plat_time_us(), loop};
            return i;
        }
    }
    return -1;
}
void plat_audio_stop(int v) { if (v >= 0 && v < 32) g_voices[v].used = false; }
bool plat_audio_is_playing(int v) { return v >= 0 && v < 32 && g_voices[v].used; }
void plat_audio_set_gain(int v, float g) { (void)v; (void)g; }
void plat_audio_set_pitch(int v, float p) { (void)v; (void)p; }
void plat_audio_pause(int v, bool p) { (void)v; (void)p; }
float plat_audio_get_position(int v) {
    return (v >= 0 && v < 32 && g_voices[v].used) ? (float)(plat_time_us() - g_voices[v].start) / 1e6f : 0;
}
void plat_audio_set_position(int v, float s) { (void)v; (void)s; }
float plat_audio_length(int b) { (void)b; return 1.0f; }

void plat_show_message(const char *msg) { fprintf(stderr, "[message] %s\n", msg); }

#endif
