/*
 * Platform layer: everything the core needs from the console.
 * Implemented by platform_3ds.c (real hardware) and platform_host.c
 * (headless desktop build used by the test suite).
 *
 * Coordinates given to the draw functions are in "game window" pixels
 * (normally 640x480 for GameMaker games); the platform scales them to the
 * screen.
 */
#ifndef D3_PLATFORM_H
#define D3_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- lifecycle / system ---- */
bool plat_init(void);
void plat_shutdown(void);
bool plat_should_quit(void);
void plat_log_line(const char *tag, const char *msg);
void plat_fatal(const char *msg);
void *plat_alloc_big(size_t n);
void plat_free_big(void *p);
void plat_loading_progress(const char *what, float frac);
uint64_t plat_time_us(void);
void plat_sleep_us(uint64_t us);
const char *plat_data_root(void); /* e.g. "sdmc:/3ds/Delta3DS/" */
const char *plat_save_dir(void);  /* writable save directory, trailing slash */
void plat_room_changed(void);     /* hint: a good moment to evict caches */

/* ---- video ---- */
void plat_set_window_size(int w, int h);
void plat_frame_begin(uint32_t clear_abgr);
void plat_frame_end(void);
/* Opens the converted texture atlases for a game directory. */
bool plat_textures_open(const char *game_dir, int ntpag);
void plat_textures_close(void);
/*
 * Draws part of a texture page item.
 *  tpag: TPAG index. (sx, sy, sw, sh): source rect relative to the item's
 *  source rect on its page. (x, y): window position of the pivot.
 *  (w, h): destination size, negative to mirror. (px, py): pivot inside the
 *  destination rect, in destination units from its top-left corner.
 *  angle: radians, clockwise on screen. color: ABGR (A = alpha).
 */
void plat_draw_image(int tpag, float sx, float sy, float sw, float sh, float x, float y, float w, float h,
                     float px, float py, float angle, uint32_t abgr);
void plat_draw_rect(float x, float y, float w, float h, uint32_t abgr);
void plat_draw_rect_gradient(float x, float y, float w, float h, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4);
void plat_draw_triangle(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t abgr);
void plat_draw_line(float x1, float y1, float x2, float y2, float width, uint32_t abgr);
/* GameMaker blend modes: 0 normal, 1 add, 2 max, 3 subtract */
void plat_set_blend_mode(int mode);
/* Clip rect in window pixels (w <= 0 disables). */
void plat_set_clip(float x, float y, float w, float h);

/* ---- input ---- */
/* Fills keys[0..255] (GameMaker virtual key codes) with 1 = held. */
void plat_input_state(uint8_t keys[256]);

/* ---- audio ---- */
/* Loads a sound from a file (or a byte range of it: length 0 = whole file).
 * Streams (music) are decoded on the fly. Returns a buffer id or -1. */
int plat_audio_load(const char *path, uint32_t offset, uint32_t length, bool stream);
void plat_audio_unload(int buffer);
/* Starts playing a buffer; returns a voice id or -1. */
int plat_audio_play(int buffer, bool loop, float gain, float pitch);
void plat_audio_stop(int voice);
bool plat_audio_is_playing(int voice);
void plat_audio_set_gain(int voice, float gain);
void plat_audio_set_pitch(int voice, float pitch);
void plat_audio_pause(int voice, bool paused);
float plat_audio_get_position(int voice); /* seconds */
void plat_audio_set_position(int voice, float seconds);
float plat_audio_length(int buffer); /* seconds */

/* ---- misc ---- */
/* Shows a message (show_message) and waits for the user to confirm. */
void plat_show_message(const char *msg);

#endif
