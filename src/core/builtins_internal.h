#ifndef D3_BUILTINS_INTERNAL_H
#define D3_BUILTINS_INTERNAL_H

#include "runtime.h"

void builtins_register_core(void);
void builtins_register_instances(void);
void builtins_register_draw(void);
void builtins_register_audio(void);
void builtins_register_ds(void);
void builtins_register_files(void);

int utf8_decode(const char *s, int32_t len, int32_t *pos);
int utf8_encode(uint32_t c, char *out);
int32_t utf8_length(const char *s, int32_t len);
int32_t utf8_offset(const char *s, int32_t len, int32_t index);
double rng_next(void);

/* game_change() target, consumed by the launcher (empty if none) */
extern char g_game_change_request[512];

/* file path helpers (builtins_file.c) */
/* Resolves a path for reading: save directory first, then the game directory. */
void file_resolve_read(const char *name, char *out, size_t n);
void file_resolve_write(const char *name, char *out, size_t n);

/* ds_map / json helpers shared with files */
int32_t dsmap_create(void);
Value json_to_dsmap(const char *text);

#endif
