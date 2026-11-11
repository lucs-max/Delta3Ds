/*
 * 3DS entry point and game launcher.
 *
 * Looks for data.win files under sdmc:/3ds/Delta3DS/ (the folder itself and
 * one level of sub-folders, e.g. chapter1/ ... chapter4/), lets the player
 * pick one on the bottom screen, and runs it. game_change() requests from a
 * game (chapter select menus) start the requested data.win next.
 */
#ifdef __3DS__
#include <3ds.h>
#include <citro3d.h>
#include <dirent.h>
#include <sys/stat.h>
#include "core/runtime.h"
#include "core/builtins_internal.h"
#include "platform/platform.h"

void plat_reset_quit(void);
void plat_audio_shutdown(void);

#define MAX_GAMES 32

typedef struct {
    char path[512];
    char label[128];
    bool converted;
} GameEntry;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void add_game(GameEntry *list, int *n, const char *dir, const char *label) {
    if (*n >= MAX_GAMES) return;
    char p[512];
    snprintf(p, sizeof p, "%sdata.win", dir);
    if (!file_exists(p)) return;
    GameEntry *g = &list[(*n)++];
    snprintf(g->path, sizeof g->path, "%s", p);
    snprintf(g->label, sizeof g->label, "%s", label);
    char idx[600];
    snprintf(idx, sizeof idx, "%sd3ds/atlas.idx", dir);
    g->converted = file_exists(idx);
}

static int scan_games(GameEntry *list) {
    int n = 0;
    const char *root = plat_data_root();
    add_game(list, &n, root, "(main folder)");
    DIR *d = opendir(root);
    if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "saves")) continue;
        char sub[512];
        snprintf(sub, sizeof sub, "%s%s/", root, e->d_name);
        add_game(list, &n, sub, e->d_name);
    }
    closedir(d);
    return n;
}

static void print_header(void) {
    consoleClear();
#ifdef D3DS_NEW3DS
    printf("\x1b[36mDelta3DS\x1b[0m - GameMaker runner (New 3DS build)\n");
#else
    printf("\x1b[36mDelta3DS\x1b[0m - GameMaker runner (Old 3DS build)\n");
#endif
    printf("--------------------------------------\n");
}

/* Returns the index picked, or -1 to quit. */
static int pick_game(GameEntry *games, int n) {
    int sel = 0;
    bool redraw = true;
    while (aptMainLoop()) {
        if (redraw) {
            print_header();
            printf("Choose a game folder:\n\n");
            for (int i = 0; i < n; i++)
                printf("%s %-28.28s %s\n", i == sel ? "\x1b[33m>\x1b[0m" : " ", games[i].label,
                       games[i].converted ? "" : "\x1b[31m(not converted)\x1b[0m");
            printf("\nA: start   START: exit\n");
            redraw = false;
        }
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_START) return -1;
        if (k & KEY_DOWN) { sel = (sel + 1) % n; redraw = true; }
        if (k & KEY_UP) { sel = (sel + n - 1) % n; redraw = true; }
        if (k & KEY_A) return sel;
        gspWaitForVBlank();
    }
    return -1;
}

static void wait_start(void) {
    printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

static void run_game(const char *path) {
    char current[512];
    snprintf(current, sizeof current, "%s", path);
    for (;;) {
        print_header();
        printf("Loading %s\n", current);
        g_game_change_request[0] = 0;
        char err[512];
        if (!runtime_init(current, err, sizeof err)) {
            printf("\x1b[31mCould not start the game:\n%s\x1b[0m\n", err);
            runtime_shutdown();
            wait_start();
            return;
        }
        printf("Running. Hold START+SELECT to quit.\n");
        plat_reset_quit();
        double cur_rate = 0;
        while (!plat_should_quit()) {
            if (R->room_speed != cur_rate) {
                cur_rate = R->room_speed > 0 ? R->room_speed : 30;
                C3D_FrameRate((float)cur_rate);
            }
            if (!runtime_frame()) break;
        }
        runtime_shutdown();
        C3D_FrameRate(60.0f);
        if (!g_game_change_request[0]) return;
        snprintf(current, sizeof current, "%s", g_game_change_request);
        if (!file_exists(current)) {
            printf("\x1b[31mgame_change target not found:\n%s\x1b[0m\n", current);
            wait_start();
            return;
        }
    }
}

int main(void) {
    plat_init();
    GameEntry games[MAX_GAMES];
    for (;;) {
        int n = scan_games(games);
        if (n == 0) {
            print_header();
            printf("No data.win found.\n\n"
                   "Copy your game files to:\n  %s\n"
                   "(or one sub-folder per chapter)\n\n"
                   "Then run tools/d3ds_convert.py on your\n"
                   "PC to convert the textures.\n",
                   plat_data_root());
            wait_start();
            break;
        }
        int sel = n == 1 ? 0 : pick_game(games, n);
        if (sel < 0) break;
        if (!games[sel].converted) {
            print_header();
            printf("\x1b[33mTextures for this folder are not converted.\x1b[0m\n"
                   "Run tools/d3ds_convert.py on your PC first.\n"
                   "Sprites will show as placeholders.\n\nA: continue anyway   B: back\n");
            bool go = false;
            while (aptMainLoop()) {
                hidScanInput();
                u32 k = hidKeysDown();
                if (k & KEY_A) { go = true; break; }
                if (k & KEY_B) break;
                gspWaitForVBlank();
            }
            if (!go) { if (n == 1) break; continue; }
        }
        run_game(games[sel].path);
        if (n == 1 || !aptMainLoop()) break;
    }
    plat_audio_shutdown();
    plat_shutdown();
    return 0;
}
#endif
