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
void I_Render3DScreenshot(void);
void I_Render3DToggleFlashlight(void);
int I_Render3DIsAccelerated(void);
#ifdef __cplusplus
}
#endif
#endif
