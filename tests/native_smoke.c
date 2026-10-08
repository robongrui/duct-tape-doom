/* Exercise an actual level and complete native save/load without desktop input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "g_game.h"
#include "i_system.h"
#include "m_argv.h"
#include "r_state.h"
#include "p_local.h"
#ifdef DOOM_RENDER3D
#include "../platform/i_render3d.h"
#endif
extern void A_PosAttack(mobj_t *actor);
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

static int stage, saved_ammo, saved_health, player_flashes, enemy_flashes;
static void (*renderer_flash)(mobj_t *,int,int);
static void observe_flash(mobj_t *source,int weapon,int projectile)
{
    if(source->player) ++player_flashes; else ++enemy_flashes;
#ifdef DOOM_RENDER3D
    if(getenv("DOOM_CAPTURE_FLASH_TEST") &&
       ((source->player && player_flashes==1)||(!source->player && enemy_flashes==1))) I_Render3DScreenshot();
#endif
    if(renderer_flash) renderer_flash(source,weapon,projectile);
}
#ifdef DOOM_RENDER3D
static unsigned serial_before_change;
static int projectile_capture;
#endif
static void key(evtype_t type, int value)
{
    event_t event = {type, value, 0, 0};
    D_PostEvent(&event);
}
void I_TestFrame(void)
{
    if (gamestate != GS_LEVEL) return;
    if (stage == 0 && gametic >= 5)
    {
#ifdef DOOM_RENDER3D
        if (!R_AcceleratedView || !R_MuzzleFlash) I_Error("Metal smoke test: GPU renderer or flash hook is inactive");
        I_Render3DToggleFlashlight(); /* Exercise beam/bounce during movement and firing. */
        I_Render3DLook(-80);
#endif
        renderer_flash=R_MuzzleFlash;R_MuzzleFlash=observe_flash;
        key(ev_keydown, 'w'); stage = 1;
    }
    else if (stage == 1 && gametic >= 20)
    {
#ifdef DOOM_RENDER3D
        if(getenv("DOOM_CAPTURE_FLASH_TEST")) I_Render3DScreenshot();
#endif
        key(ev_keyup, 'w'); key(ev_keydown, KEY_RCTRL); stage = 2;
    }
    else if (stage == 2 && gametic >= 35)
    {
        mobj_t *player=players[0].mo;
        if (abs(player->momx) > FRACUNIT/8 || abs(player->momy) > FRACUNIT/8)
            I_Error("Smoke test: player still drifts after releasing movement");
        mobj_t *shooter=P_SpawnMobj(player->x,player->y,ONFLOORZ,MT_POSSESSED);
        key(ev_keyup, KEY_RCTRL);
        shooter->target=player;
        A_PosAttack(shooter);
        P_RemoveMobj(shooter);
        if(enemy_flashes!=1) I_Error("Smoke test: enemy shot did not notify flash renderer");
#ifdef DOOM_RENDER3D
        /* Keep the projectile capture clear of the fixture shot's red pain tint. */
        players[0].damagecount=0;
        P_SpawnPlayerMissile(player,MT_BRUISERSHOT);
#endif
        stage = 3;
    }
#ifdef DOOM_RENDER3D
    else if (stage == 3 && gametic >= 45 && !projectile_capture)
    {
        if(getenv("DOOM_CAPTURE_FLASH_TEST")) I_Render3DScreenshot();
        projectile_capture=1;
    }
#endif
    else if (stage == 3 && gametic >= 60)
    {
#ifdef DOOM_RENDER3D
        I_Render3DToggleFlashlight(); /* Exercise the reflected-color fade-out. */
        I_Render3DLook(80);
#endif
        saved_ammo = players[0].ammo[am_clip];
        saved_health = players[0].health;
        if (player_flashes == 0) I_Error("Smoke test: player shots did not notify flash renderer");
        if (saved_ammo >= 50) I_Error("Smoke test: firing did not consume ammo");
        G_SaveGame(5, "Native smoke test"); stage = 4;
    }
    else if (stage == 4 && gametic >= 70 && gameaction == ga_nothing)
    {
        FILE *file = fopen("doomsav5.dsg", "rb");
        if (!file) I_Error("Smoke test: save file was not created");
        fclose(file);
        players[0].ammo[am_clip] = 1;
        G_LoadGame("doomsav5.dsg"); stage = 5;
    }
    else if (stage == 5 && gametic >= 85 && gameaction == ga_nothing)
    {
        if (players[0].ammo[am_clip] != saved_ammo || players[0].health != saved_health)
            I_Error("Smoke test: saved player state was not restored");
        if (!players[0].mo) I_Error("Smoke test: player object was not restored");
        puts("Native gameplay, firing, save, and load passed.");
#ifdef DOOM_RENDER3D
        I_Render3DToggleFlashlight(); /* Keep it on through load and level reset. */
        serial_before_change = r_levelserial;
        G_DeferedInitNew(sk_medium, 1, 2);
        stage = 6;
#else
        I_Quit();
#endif
    }
#ifdef DOOM_RENDER3D
    else if (stage == 6 && gametic >= 110 && gameaction == ga_nothing)
    {
        if (gamemap != 2 || r_levelserial <= serial_before_change || !players[0].mo)
            I_Error("Metal smoke test: level change failed");
        puts("Metal level change passed.");
        I_Quit();
    }
#endif
}
int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    FILE *config = fopen("smoke.cfg", "w");
    if (!config) return 1;
    fputs("key_up 119\nkey_fire 157\nsnd_channels 16\n", config);
    fclose(config);
    char *args[] = {"smoke", "-iwad", argv[1], "-warp", "1", "1",
        "-config", "smoke.cfg", "-nomonsters", "-nomusic"};
    myargc = sizeof(args)/sizeof(args[0]); myargv = args;
    D_DoomMain();
    return 0;
}
