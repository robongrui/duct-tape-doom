#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomdef.h"
#include "d_main.h"
#include "m_argv.h"
#include "i_compat.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

static char *selected_wad;
static boolean selection_done;
#ifdef __APPLE__
extern int DOOM_ChooseWads(const char *bundled, char **base, char **addon);
#endif
static void selected(void *userdata, const char * const *files, int filter)
{
    (void)userdata; (void)filter;
    if (files && files[0]) selected_wad = SDL_strdup(files[0]);
    selection_done = true;
}

/* Many releases ship their DEHACKED patch beside the WAD: map.wad + map.deh. */
static char *companion_patch(const char *wad)
{
    static const char *extensions[] = { ".deh", ".DEH", ".bex", ".BEX" };
    size_t length = strlen(wad);
    const char *dot = strrchr(wad, '.');
    const char *slash = strrchr(wad, '/');
    if (dot && (!slash || dot > slash)) length = (size_t)(dot - wad);
    char *path = malloc(length + 5);
    if (!path) return NULL;
    for (int i = 0; i < 4; ++i)
    {
        memcpy(path, wad, length);
        strcpy(path + length, extensions[i]);
        FILE *file = fopen(path, "rb");
        if (file) { fclose(file); return path; }
    }
    free(path);
    return NULL;
}

int main(int argc, char **argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "Could not initialize SDL: %s\n", SDL_GetError());
        return 1;
    }
    const char *base = SDL_GetBasePath();
    char bundled[4096];
    snprintf(bundled, sizeof(bundled), "%sdefault.wad", base ? base : "./");
    int has_iwad = 0;
    char *selected_addon = NULL;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "-iwad")) has_iwad = 1;
#ifdef __APPLE__
    if (argc == 1)
    {
        if (!DOOM_ChooseWads(bundled, &selected_wad, &selected_addon))
        { SDL_Quit(); return 0; }
    }
#endif
    if (!has_iwad && !selected_wad)
    {
        FILE *file = fopen(bundled, "rb");
        if (file) { fclose(file); selected_wad = SDL_strdup(bundled); }
        else
        {
            SDL_DialogFileFilter filter = { "DOOM game data", "wad" };
            SDL_ShowOpenFileDialog(selected, NULL, NULL, &filter, 1, NULL, false);
            while (!selection_done) { SDL_PumpEvents(); SDL_Delay(10); }
            if (!selected_wad) { SDL_Quit(); return 0; }
        }
    }
    char *preferences = SDL_GetPrefPath("Local Games", "DOOM");
    if (!preferences)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DOOM", "Could not create the game save folder.", NULL);
        SDL_Quit(); return 1;
    }
#ifdef _WIN32
    if (_chdir(preferences))
#else
    if (chdir(preferences))
#endif
    { SDL_free(preferences); SDL_Quit(); return 1; }
    SDL_free(preferences);
    if (argc == 1)
    {
        FILE *log = fopen("game.log", "w");
        if (log) fclose(log);
        freopen("game.log", "a", stdout);
        freopen("game.log", "a", stderr);
    }
    FILE *file = fopen("doom.cfg", "r");
    if (file) fclose(file);
    else
    {
        file = fopen("doom.cfg", "w");
        if (file)
        {
            fputs("key_up 119\nkey_down 115\nkey_strafeleft 97\nkey_straferight 100\n"
                  "key_use 32\nkey_fire 157\nmouse_sensitivity 5\n"
                  "sfx_volume 10\nmusic_volume 7\nsnd_channels 16\n", file);
            fclose(file);
        }
    }
    char **args = calloc((size_t)argc + 10, sizeof(char*));
    if (!args) { SDL_Quit(); return 1; }
    int count = 0;
    for (int i = 0; i < argc; ++i) args[count++] = argv[i];
    if (!has_iwad) { args[count++] = "-iwad"; args[count++] = selected_wad; }
    if (selected_addon)
    {
        char *patch = companion_patch(selected_addon);
        args[count++] = "-file"; args[count++] = selected_addon;
        if (patch) { args[count++] = "-deh"; args[count++] = patch; }
    }
    int has_config = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "-config")) has_config = 1;
    if (!has_config) { args[count++] = "-config"; args[count++] = "doom.cfg"; }
    myargc = count;
    myargv = args;
    D_DoomMain();
    return 0;
}
