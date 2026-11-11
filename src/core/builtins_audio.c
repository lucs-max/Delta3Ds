/*
 * Audio functions. Sounds come from the data file (AUDO chunk),
 * audiogroupN.dat files, or external files (streams such as the ogg files in mus/).
 */
#include "builtins_internal.h"
#include "../platform/platform.h"

#define MAX_SOUND_INSTANCES 64
#define SOUND_INSTANCE_BASE 200000
#define STREAM_BASE 300000
#define MAX_STREAMS 64
#define MAX_GROUPS 16

typedef struct {
    bool used;
    int32_t asset;  /* sound index or STREAM_BASE + n */
    int voice;
    float gain, target_gain, gain_step; /* fades */
    int32_t fade_frames;
    float pitch;
    bool paused;
} SoundInst;

typedef struct {
    bool used;
    char path[512];
    int buffer;
} Stream;

typedef struct {
    char path[512];
    int32_t naudio;
    AudioEntry *audio;
} AudioGroup;

static SoundInst g_inst[MAX_SOUND_INSTANCES];
static Stream g_streams[MAX_STREAMS];
static int *g_buffers;      /* sound index -> platform buffer (-2 = not loaded, -1 = failed) */
static int32_t g_nbuffers;
static AudioGroup g_groups[MAX_GROUPS];
static float g_master_gain = 1;
static float *g_asset_gain; /* per sound asset gain (audio_sound_gain on an asset) */

void audio_init_groups(void) {
    DataWin *dw = &R->dw;
    g_nbuffers = dw->nsounds;
    g_buffers = d3_malloc((size_t)(g_nbuffers ? g_nbuffers : 1) * sizeof(int));
    g_asset_gain = d3_malloc((size_t)(g_nbuffers ? g_nbuffers : 1) * sizeof(float));
    for (int32_t i = 0; i < g_nbuffers; i++) { g_buffers[i] = -2; g_asset_gain[i] = 1; }
    /* audiogroupN.dat files: FORM files whose AUDO chunk we index */
    for (int32_t gi = 1; gi < dw->naudio_groups && gi < MAX_GROUPS; gi++) {
        AudioGroup *g = &g_groups[gi];
        snprintf(g->path, sizeof g->path, "%saudiogroup%d.dat", dw->dir, (int)gi);
        FILE *f = fopen(g->path, "rb");
        if (!f) continue;
        uint8_t hdr[8];
        uint32_t pos = 8;
        while (fseek(f, (long)pos, SEEK_SET) == 0 && fread(hdr, 1, 8, f) == 8) {
            uint32_t len = rd_u32(hdr + 4);
            if (!memcmp(hdr, "AUDO", 4)) {
                uint8_t nb[4];
                if (fread(nb, 1, 4, f) == 4) {
                    uint32_t n = rd_u32(nb);
                    if (n < 100000) {
                        uint8_t *ptrs = d3_malloc(n * 4 + 4);
                        if (fread(ptrs, 1, n * 4, f) == n * 4) {
                            g->naudio = (int32_t)n;
                            g->audio = d3_calloc(n, sizeof(AudioEntry));
                            for (uint32_t i = 0; i < n; i++) {
                                uint32_t p = rd_u32(ptrs + i * 4);
                                fseek(f, (long)p, SEEK_SET);
                                if (fread(nb, 1, 4, f) == 4) {
                                    g->audio[i].offset = p + 4;
                                    g->audio[i].length = rd_u32(nb);
                                }
                            }
                        }
                        free(ptrs);
                    }
                }
                break;
            }
            pos += 8 + len;
        }
        fclose(f);
        LOGI("audio group %d: %d sounds", (int)gi, (int)g->naudio);
    }
}

void audio_shutdown(void) {
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++)
        if (g_inst[i].used) plat_audio_stop(g_inst[i].voice);
    memset(g_inst, 0, sizeof g_inst);
    for (int32_t i = 0; i < g_nbuffers; i++)
        if (g_buffers[i] >= 0) plat_audio_unload(g_buffers[i]);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].used && g_streams[i].buffer >= 0) plat_audio_unload(g_streams[i].buffer);
    memset(g_streams, 0, sizeof g_streams);
    for (int i = 0; i < MAX_GROUPS; i++) { free(g_groups[i].audio); g_groups[i].audio = NULL; g_groups[i].naudio = 0; }
    free(g_buffers);
    free(g_asset_gain);
    g_buffers = NULL;
    g_asset_gain = NULL;
    g_nbuffers = 0;
}

static int buffer_for_asset(int32_t asset) {
    if (asset >= STREAM_BASE && asset < STREAM_BASE + MAX_STREAMS) {
        Stream *s = &g_streams[asset - STREAM_BASE];
        if (!s->used) return -1;
        if (s->buffer == -2) s->buffer = plat_audio_load(s->path, 0, 0, true);
        return s->buffer;
    }
    if (asset < 0 || asset >= g_nbuffers) return -1;
    if (g_buffers[asset] != -2) return g_buffers[asset];
    const Sound *snd = &R->dw.sounds[asset];
    int buf = -1;
    bool embedded = (snd->flags & 1) != 0;
    if (embedded && snd->audio_id >= 0) {
        const AudioEntry *e = NULL;
        const char *path = R->dw.path;
        if (snd->group <= 0) {
            if (snd->audio_id < R->dw.naudio) e = &R->dw.audio[snd->audio_id];
        } else if (snd->group < MAX_GROUPS) {
            AudioGroup *g = &g_groups[snd->group];
            if (snd->audio_id < g->naudio) { e = &g->audio[snd->audio_id]; path = g->path; }
        }
        if (e && e->length) buf = plat_audio_load(path, e->offset, e->length, (snd->flags & 2) != 0);
    } else if (snd->file && snd->file[0]) {
        char p[600];
        snprintf(p, sizeof p, "%s%s", R->dw.dir, snd->file);
        if (!strchr(snd->file, '.')) snprintf(p, sizeof p, "%s%s.ogg", R->dw.dir, snd->file);
        buf = plat_audio_load(p, 0, 0, true);
    }
    if (buf < 0) LOGW("could not load sound %s", snd->name);
    g_buffers[asset] = buf;
    return buf;
}

static SoundInst *inst_by_id(int32_t id) {
    int32_t i = id - SOUND_INSTANCE_BASE;
    if (i < 0 || i >= MAX_SOUND_INSTANCES || !g_inst[i].used) return NULL;
    return &g_inst[i];
}

/* Applies f to the instance (if id is an instance) or all instances of an asset. */
static void for_each_target(int32_t id, void (*f)(SoundInst *, void *), void *ctx) {
    SoundInst *si = inst_by_id(id);
    if (si) { f(si, ctx); return; }
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++)
        if (g_inst[i].used && g_inst[i].asset == id) f(&g_inst[i], ctx);
}

static float asset_gain(int32_t asset) {
    if (asset >= 0 && asset < g_nbuffers) return g_asset_gain[asset] * R->dw.sounds[asset].volume;
    return 1;
}

static void apply_gain(SoundInst *s) {
    plat_audio_set_gain(s->voice, s->gain * g_master_gain * asset_gain(s->asset));
}

static int32_t play(int32_t asset, bool loop, float gain, float pitch, float offset) {
    int buf = buffer_for_asset(asset);
    if (buf < 0) return -1;
    int slot = -1;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        if (g_inst[i].used && !plat_audio_is_playing(g_inst[i].voice) && !g_inst[i].paused) g_inst[i].used = false;
        if (!g_inst[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return -1;
    float eff = gain * g_master_gain * asset_gain(asset);
    int voice = plat_audio_play(buf, loop, eff, pitch);
    if (voice < 0) return -1;
    if (offset > 0) plat_audio_set_position(voice, offset);
    g_inst[slot] = (SoundInst){true, asset, voice, gain, gain, 0, 0, pitch, false};
    return SOUND_INSTANCE_BASE + slot;
}

void audio_update(void) {
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) {
        SoundInst *s = &g_inst[i];
        if (!s->used) continue;
        if (s->fade_frames > 0) {
            s->gain += s->gain_step;
            if (--s->fade_frames == 0) s->gain = s->target_gain;
            apply_gain(s);
        }
        if (!s->paused && !plat_audio_is_playing(s->voice)) s->used = false;
    }
}

static Value bi_audio_play_sound(int32_t argc, Value *a) {
    /* audio_play_sound(index, priority, loop, [gain], [offset], [pitch]) */
    return v_real(play(ARGI(argc, a, 0, -1), argc > 2 && v_to_bool(a[2]), (float)ARGR(argc, a, 3, 1),
                       (float)ARGR(argc, a, 5, 1), (float)ARGR(argc, a, 4, 0)));
}
static Value bi_audio_play_sound_at(int32_t argc, Value *a) {
    /* index, x, y, z, falloff_ref, falloff_max, falloff_factor, loop, priority */
    return v_real(play(ARGI(argc, a, 0, -1), argc > 7 && v_to_bool(a[7]), 1, 1, 0));
}
static void f_stop(SoundInst *s, void *c) { (void)c; plat_audio_stop(s->voice); s->used = false; }
static Value bi_audio_stop_sound(int32_t argc, Value *a) { for_each_target(ARGI(argc, a, 0, -1), f_stop, NULL); return v_undef(); }
static Value bi_audio_stop_all(int32_t argc, Value *a) {
    (void)argc; (void)a;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) if (g_inst[i].used) f_stop(&g_inst[i], NULL);
    return v_undef();
}
static void f_pause(SoundInst *s, void *c) { s->paused = *(bool *)c; plat_audio_pause(s->voice, s->paused); }
static Value bi_audio_pause_sound(int32_t argc, Value *a) { bool p = true; for_each_target(ARGI(argc, a, 0, -1), f_pause, &p); return v_undef(); }
static Value bi_audio_resume_sound(int32_t argc, Value *a) { bool p = false; for_each_target(ARGI(argc, a, 0, -1), f_pause, &p); return v_undef(); }
static Value bi_audio_pause_all(int32_t argc, Value *a) {
    (void)argc; (void)a;
    bool p = true;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) if (g_inst[i].used) f_pause(&g_inst[i], &p);
    return v_undef();
}
static Value bi_audio_resume_all(int32_t argc, Value *a) {
    (void)argc; (void)a;
    bool p = false;
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) if (g_inst[i].used) f_pause(&g_inst[i], &p);
    return v_undef();
}
static Value bi_audio_is_playing(int32_t argc, Value *a) {
    int32_t id = ARGI(argc, a, 0, -1);
    SoundInst *si = inst_by_id(id);
    if (si) return v_bool(plat_audio_is_playing(si->voice) || si->paused);
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++)
        if (g_inst[i].used && g_inst[i].asset == id && plat_audio_is_playing(g_inst[i].voice)) return v_bool(true);
    return v_bool(false);
}
static Value bi_audio_is_paused(int32_t argc, Value *a) {
    SoundInst *si = inst_by_id(ARGI(argc, a, 0, -1));
    return v_bool(si && si->paused);
}
typedef struct { float gain; int32_t frames; } GainCtx;
static void f_gain(SoundInst *s, void *c) {
    GainCtx *g = c;
    if (g->frames <= 0) {
        s->gain = s->target_gain = g->gain;
        s->fade_frames = 0;
        apply_gain(s);
    } else {
        s->target_gain = g->gain;
        s->fade_frames = g->frames;
        s->gain_step = (g->gain - s->gain) / (float)g->frames;
    }
}
static Value bi_audio_sound_gain(int32_t argc, Value *a) {
    int32_t id = ARGI(argc, a, 0, -1);
    double ms = ARGR(argc, a, 2, 0);
    GainCtx g = {(float)ARGR(argc, a, 1, 1), (int32_t)(ms * R->room_speed / 1000.0)};
    if (!inst_by_id(id) && id >= 0 && id < g_nbuffers) {
        /* gain on an asset affects the asset and its playing instances */
        g_asset_gain[id] = g.gain;
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++) if (g_inst[i].used && g_inst[i].asset == id) apply_gain(&g_inst[i]);
        return v_undef();
    }
    for_each_target(id, f_gain, &g);
    return v_undef();
}
static Value bi_audio_sound_get_gain(int32_t argc, Value *a) {
    int32_t id = ARGI(argc, a, 0, -1);
    SoundInst *si = inst_by_id(id);
    if (si) return v_real(si->gain);
    if (id >= 0 && id < g_nbuffers) return v_real(g_asset_gain[id]);
    return v_real(0);
}
static void f_pitch(SoundInst *s, void *c) { s->pitch = *(float *)c; plat_audio_set_pitch(s->voice, s->pitch); }
static Value bi_audio_sound_pitch(int32_t argc, Value *a) {
    float p = (float)ARGR(argc, a, 1, 1);
    for_each_target(ARGI(argc, a, 0, -1), f_pitch, &p);
    return v_undef();
}
static Value bi_audio_sound_get_pitch(int32_t argc, Value *a) {
    SoundInst *si = inst_by_id(ARGI(argc, a, 0, -1));
    return v_real(si ? si->pitch : 1);
}
static Value bi_audio_sound_get_track_position(int32_t argc, Value *a) {
    SoundInst *si = inst_by_id(ARGI(argc, a, 0, -1));
    return v_real(si ? plat_audio_get_position(si->voice) : 0);
}
static Value bi_audio_sound_set_track_position(int32_t argc, Value *a) {
    SoundInst *si = inst_by_id(ARGI(argc, a, 0, -1));
    if (si) plat_audio_set_position(si->voice, (float)ARGR(argc, a, 1, 0));
    return v_undef();
}
static Value bi_audio_sound_length(int32_t argc, Value *a) {
    int32_t id = ARGI(argc, a, 0, -1);
    SoundInst *si = inst_by_id(id);
    int buf = buffer_for_asset(si ? si->asset : id);
    return v_real(buf >= 0 ? plat_audio_length(buf) : 0);
}
static Value bi_audio_master_gain(int32_t argc, Value *a) {
    g_master_gain = (float)ARGR(argc, a, 0, 1);
    for (int i = 0; i < MAX_SOUND_INSTANCES; i++) if (g_inst[i].used) apply_gain(&g_inst[i]);
    return v_undef();
}
static Value bi_audio_create_stream(int32_t argc, Value *a) {
    const char *name = ARGS(argc, a, 0);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!g_streams[i].used) {
            g_streams[i].used = true;
            g_streams[i].buffer = -2;
            file_resolve_read(name, g_streams[i].path, sizeof g_streams[i].path);
            return v_real(STREAM_BASE + i);
        }
    }
    LOGW("audio_create_stream: too many streams");
    return v_real(-1);
}
static Value bi_audio_destroy_stream(int32_t argc, Value *a) {
    int32_t id = ARGI(argc, a, 0, -1);
    if (id >= STREAM_BASE && id < STREAM_BASE + MAX_STREAMS) {
        Stream *s = &g_streams[id - STREAM_BASE];
        for (int i = 0; i < MAX_SOUND_INSTANCES; i++)
            if (g_inst[i].used && g_inst[i].asset == id) f_stop(&g_inst[i], NULL);
        if (s->used && s->buffer >= 0) plat_audio_unload(s->buffer);
        s->used = false;
    }
    return v_undef();
}
static Value bi_audio_exists(int32_t argc, Value *a) {
    int32_t id = ARGI(argc, a, 0, -1);
    return v_bool((id >= 0 && id < g_nbuffers) || inst_by_id(id) ||
                  (id >= STREAM_BASE && id < STREAM_BASE + MAX_STREAMS && g_streams[id - STREAM_BASE].used));
}
static Value bi_true_a(int32_t argc, Value *a) { (void)argc; (void)a; return v_bool(true); }
static Value bi_noop_a(int32_t argc, Value *a) { (void)argc; (void)a; return v_undef(); }
static Value bi_zero_a(int32_t argc, Value *a) { (void)argc; (void)a; return v_real(0); }

void builtins_register_audio(void) {
#define REG(n) builtin_register(#n, bi_##n)
    REG(audio_play_sound); REG(audio_play_sound_at); REG(audio_stop_sound); REG(audio_stop_all);
    REG(audio_pause_sound); REG(audio_resume_sound); REG(audio_pause_all); REG(audio_resume_all);
    REG(audio_is_playing); REG(audio_is_paused); REG(audio_sound_gain); REG(audio_sound_get_gain);
    REG(audio_sound_pitch); REG(audio_sound_get_pitch); REG(audio_sound_get_track_position);
    REG(audio_sound_set_track_position); REG(audio_sound_length); REG(audio_master_gain);
    REG(audio_create_stream); REG(audio_destroy_stream); REG(audio_exists);
    builtin_register("audio_group_load", bi_true_a);
    builtin_register("audio_group_unload", bi_true_a);
    builtin_register("audio_group_is_loaded", bi_true_a);
    builtin_register("audio_group_load_progress", bi_zero_a);
    builtin_register("audio_group_set_gain", bi_noop_a);
    builtin_register("audio_group_stop_all", bi_audio_stop_all);
    builtin_register("audio_channel_num", bi_noop_a);
    builtin_register("audio_falloff_set_model", bi_noop_a);
    builtin_register("audio_listener_orientation", bi_noop_a);
    builtin_register("audio_listener_position", bi_noop_a);
    builtin_register("audio_system_is_available", bi_true_a);
    builtin_register("audio_set_master_gain", bi_audio_master_gain);
    builtin_register("audio_sound_loop", bi_noop_a);
    builtin_register("audio_debug", bi_noop_a);
    /* legacy sound_* functions */
    builtin_register("sound_play", bi_audio_play_sound);
    builtin_register("sound_stop", bi_audio_stop_sound);
    builtin_register("sound_stop_all", bi_audio_stop_all);
    builtin_register("sound_isplaying", bi_audio_is_playing);
#undef REG
}
