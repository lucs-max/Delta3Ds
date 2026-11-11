/*
 * Desktop test runner:  delta3ds_host <data.win> [frames] [keyscript]
 *
 * keyscript: comma separated "frame:key:down" entries, e.g. "10:90:1,12:90:0"
 * presses key 90 ('Z') on frame 10 and releases it on frame 12.
 */
#ifndef __3DS__
#include "../core/runtime.h"
#include "../platform/platform.h"

void host_set_key(int key, bool down);
uint64_t host_draw_calls(void);

typedef struct { int frame, key, down; } KeyEvent;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <data.win> [frames] [keyscript]\n", argv[0]);
        return 2;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 60;
    KeyEvent ev[256];
    int nev = 0;
    if (argc > 3) {
        const char *p = argv[3];
        while (*p && nev < 256) {
            KeyEvent e;
            if (sscanf(p, "%d:%d:%d", &e.frame, &e.key, &e.down) == 3) ev[nev++] = e;
            const char *c = strchr(p, ',');
            if (!c) break;
            p = c + 1;
        }
    }
    plat_init();
    char err[512];
    if (!runtime_init(argv[1], err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    int f;
    for (f = 0; f < frames; f++) {
        for (int i = 0; i < nev; i++)
            if (ev[i].frame == f) host_set_key(ev[i].key, ev[i].down != 0);
        if (!runtime_frame()) break;
    }
    printf("frames=%d room=%d instances=%d draw_calls=%llu\n", f, R->room, R->ninsts,
           (unsigned long long)host_draw_calls());
    runtime_shutdown();
    plat_shutdown();
    return 0;
}
#endif
