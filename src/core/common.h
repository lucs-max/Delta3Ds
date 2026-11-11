/*
 * Delta3DS - a GameMaker runner for Nintendo 3DS.
 * Common includes, logging and small helpers shared by every module.
 */
#ifndef D3_COMMON_H
#define D3_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define D3_UNUSED(x) ((void)(x))
#define D3_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef enum { D3_LOG_DEBUG, D3_LOG_INFO, D3_LOG_WARN, D3_LOG_ERROR } D3LogLevel;

void d3_log(D3LogLevel lvl, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define LOGD(...) d3_log(D3_LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) d3_log(D3_LOG_INFO, __VA_ARGS__)
#define LOGW(...) d3_log(D3_LOG_WARN, __VA_ARGS__)
#define LOGE(...) d3_log(D3_LOG_ERROR, __VA_ARGS__)

/* Allocation wrappers: abort with a message instead of returning NULL. */
void *d3_malloc(size_t n);
void *d3_calloc(size_t count, size_t n);
void *d3_realloc(void *p, size_t n);
char *d3_strdup(const char *s);

/* Fatal error: shows the message on screen (3DS) or stderr (host) and exits. */
void d3_fatal(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2), noreturn))
#endif
    ;

/* Little-endian readers over raw memory. */
static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }
static inline uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int16_t rd_i16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static inline float rd_f32(const uint8_t *p) {
    uint32_t u = rd_u32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static inline double rd_f64(const uint8_t *p) {
    uint64_t u = (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
    double d;
    memcpy(&d, &u, 8);
    return d;
}
static inline int64_t rd_i64(const uint8_t *p) {
    return (int64_t)((uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32));
}

/* Case-sensitive string interning: every variable / member name gets a stable id. */
int d3_intern(const char *s);
int d3_intern_n(const char *s, size_t len);
const char *d3_intern_name(int id);
int d3_intern_count(void);
void d3_intern_reset(void);

#endif
