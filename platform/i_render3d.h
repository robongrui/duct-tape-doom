/* Accelerated 3D renderer: i_gpu3d.cpp draws the scene from scene3d.cpp with
   SDL_gpu (Metal on macOS, Vulkan elsewhere). */
#ifndef DOOM_RENDER3D_H
#define DOOM_RENDER3D_H
#ifdef __cplusplus
extern "C" {
#endif
struct SDL_Window;
int I_Render3DInit(struct SDL_Window *window);
void I_Render3DShutdown(void);
void I_Render3DStartFrame(void);
void I_Render3DPresent(const unsigned *pixels, const unsigned *palette);
void I_Render3DLook(float delta);
void I_Render3DSettings(void);
/* The in-game F4 panel (settings_menu.cpp): the only one on Windows and
   Linux, and on the Mac with -ingamesettings. It draws a 640x400 image that
   the renderer scales by whole pixels; clicks use that image's coordinates
   (button 0: hover, 1: left, 3: right). I_Render3DSettingsMouse takes window
   coordinates. */
#define DOOM_SETTINGS_WIDTH 640
#define DOOM_SETTINGS_HEIGHT 400
void I_Render3DSettingsMenu(void);
int I_Render3DSettingsOpen(void);
void I_Render3DSettingsKey(int scancode, int shift);
void I_Render3DSettingsClick(int x, int y, int button);
void I_Render3DSettingsDraw(unsigned *pixels);
void I_Render3DSettingsMouse(float x, float y, int button);
void I_Render3DScreenshot(void);
void I_Render3DToggleFlashlight(void);
int I_Render3DIsAccelerated(void);
#ifdef __cplusplus
}
#endif
#endif
