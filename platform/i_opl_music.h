/* OPL FM music synthesis from the WAD's GENMIDI instruments. */
#ifndef DOOM_I_OPL_MUSIC_H
#define DOOM_I_OPL_MUSIC_H
#include "doomtype.h"

/* All of these run with the audio lock held. */
boolean OPL_InitMusic (int rate);
void OPL_SetMusicVolume (int volume);
boolean OPL_RegisterSong (const void* data, int size);
void OPL_UnRegisterSong (void);
void OPL_PlaySong (boolean loop);
void OPL_StopSong (void);
void OPL_PauseSong (void);
void OPL_ResumeSong (void);
/* Adds the next frames of music to an interleaved stereo buffer. */
void OPL_RenderMusic (float* stereo, int frames);

/* The same, taking the audio lock; defined in i_sdl_sound.c. */
void I_OPL_ShutdownMusic(void);
void I_OPL_SetMusicVolume(int volume);
void I_OPL_PauseSong(void);
void I_OPL_ResumeSong(void);
void I_OPL_StopSong(void);
void I_OPL_PlaySong(int looping);
int I_OPL_RegisterSong(void *data);
#endif
