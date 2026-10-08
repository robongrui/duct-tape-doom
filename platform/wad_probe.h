#ifndef DOOM_WAD_PROBE_H
#define DOOM_WAD_PROBE_H

typedef struct {
    int is_iwad;
    int episode_maps;
    int numbered_maps;
    int full_game;
} doom_wad_info;

/* Check the directory before offering a file in the launcher. */
#ifdef __cplusplus
extern "C" {
#endif
int DOOM_ProbeWad(const char *path, doom_wad_info *info);
#ifdef __cplusplus
}
#endif
#endif
