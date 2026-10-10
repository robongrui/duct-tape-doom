/* Platform-neutral 3D scene: map geometry, lights, effects and settings built
   at runtime from the loaded WAD. A GPU backend uploads and draws the result. */
#ifndef DOOM_SCENE3D_H
#define DOOM_SCENE3D_H
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
extern "C" {
#include "doomtype.h"
}
#include "flash_lighting.h"
#include "i_render3d.h"

struct SDL_Window;
// Defined by the backend; the scene only holds and passes references.
struct GpuTexture;
using GpuTextureRef=std::shared_ptr<GpuTexture>;
enum class GpuFormat { R8, RG8, RGBA8, RGBA16Uint };

// Backend hooks used while the scene builds its resources.
// levels>1: pixels holds that many mip levels packed tightly one after another.
GpuTextureRef gpuCreateTexture(GpuFormat format,int width,int height,const void *pixels,int bytesPerRow,int levels=1);
// Replaces rects (x, y, width, height) of a texture with the same rects of
// pixels, an image of the texture's size; the rest keeps its contents.
void gpuUpdateTexture(const GpuTextureRef &texture,GpuFormat format,const std::vector<std::array<int,4>> &rects,const void *pixels,int bytesPerRow);
// Reads an emissive mask image as value*alpha in [0,1]; false if unreadable or the wrong size.
bool platformReadMask(const char *path,int width,int height,std::vector<float> &values);
// The most MSAA samples the GPU offers for the world targets (1: none).
int gpuMaxSamples();

namespace doom3d {
constexpr float doomPi=3.14159265358979323846f;
inline float units(int value) { return value/65536.0f; }
struct Point { float x,y; };
// sunU/sunV: texel in the wall bake atlas, or -1 for surfaces without one.
// statics: on sprites with bake-only lights, the static light's direction
// (x, y, z as unorm bytes) and how much of it comes from that direction (w).
// lampTint: the sector light's color from the lamps around a sprite (lampTint
// in baked_lighting.h), unorm bytes; white for surfaces, which compute their own.
struct Vertex { float x,y,z,u,v,light; unsigned mode; float red=0,green=0,blue=0; unsigned lightMask[2]={}; float sunU=-1,sunV=0; unsigned statics=0; unsigned lampTint=0xFFFFFFFFu; };
// fx: exposure, sky light tint; fx2: render-target pixels per Doom pixel,
// sky light level (0: off); texFilter: sharp bilinear edge softness in texels, palette mipmaps (0: off); ripple: liquid rings (x, y, radius, strength);
// flicker: the current light of the baked flicker groups, group 0 steady.
struct Uniforms { float eye[4],right[4],forward[4],up[4],projection[4],effects[4],materials[4],flashlightTint[4],fog[4],map[4],water[4],sun[4],bake[4],fx[4],fx2[4],texFilter[4],ripple[4][4],flicker[4][4]; };
static_assert(sizeof(Vertex)==64&&sizeof(Uniforms)==384,"Shader buffer layout");
using Flash=doom_flash_t;
// 63 lights keep the FlashSet uniform block under 4 KB and fit the 64-bit light masks.
constexpr unsigned maxLights=63;
struct FlashSet { unsigned count=0,reserved[3]={}; Flash lights[maxLights]={}; };
using LightBlocker=doom_light_blocker_t;
static_assert(sizeof(Flash)==64&&sizeof(FlashSet)==4048&&sizeof(LightBlocker)==32,"Flash shader layout");
struct FogLights { unsigned count=0,indices[4]={},padding[3]={}; };
static_assert(sizeof(FogLights)==32,"Fog shader layout");
struct Settings {
    int accelerated=1, widescreen=1, crosshair=1, look=1, retro=0, fps=1, scale=100;
    float msaa=0; // Geometry edge anti-aliasing: 0 off, then 2x, 4x and 8x MSAA (capped by gpuMaxSamples).
    // Walls and floors: 0 crisp pixels, 1 smooth (bilinear), 2 sharp bilinear
    // (flat texels, edges blended over sharpSoftness texels plus a pixel).
    int filter=0;
    float sharpSoftness=0;
    int paletteMips=0; // Palette-snapped mip levels for distant walls and floors when smooth or sharp.
    float fov=90;
    float flashlightTintGain=1;
    int emissive=1, fog=1, palette=0;
    int detail=1; // Derived normal maps and palette gloss.
    int variedHighlights=1; // Gloss highlights tighten on smooth artwork and spread on busy artwork, per palette ramp.
    int softLight=1; // Light seam blending and edge contact shading.
    int reflections=1; // Mirrored scene on water, nukage, slime and blood.
    int retroReflections=1; // Eighth resolution, palette colors, stepped wobble.
    int spriteFilter=0; // 0: crisp, 1: bilinear, 2: xBR pixel-art reconstruction.
    int sun=1; // Sun shadows baked at level load from the sky texture.
    int bakedLights=1; // Static lights baked at level load; nearby dynamic copies add flicker and detail.
    int bounce=1; // Light bounced from sunlit and lit surfaces, baked at level load.
    int caustics=1; // Rippling light from liquids on nearby walls.
    // Experimental effects, each separately switchable.
    int blood=1; // Splats on floors and walls from hitscan hits; pools under corpses.
    int bloodShine=1; // Fresh blood glints under dynamic lights, then dries matte.
    int flashlightShadows=1; // Things in the flashlight beam cast their silhouette on the wall behind.
    int softSprites=1; // Explosions and puffs stop slicing into walls; dithered toward floors.
    int heatHaze=0; // Stepped raster shimmer above lava, hot floors and flames.
    int eyeAdaptation=1; // Brief stepped over/underexposure when the light changes.
    int splashes=1; // Droplets and ripple rings where things land or walk in liquids.
    int dust=1; // Specks glinting in sunbeams and the flashlight beam.
    int playerShadow=1; // The player's own sprite shadow from the sun or a strong light.
    int doorLight=1; // Light from a bright room spills through a door as it opens.
    int movingRelight=1; // Baked lamp light and sun re-bake around doors, lifts and lowering walls as they move.
    int texelLight=1; // Baked light, sun and bounce blend per texture pixel instead of hard map cells.
    int skyLight=1; // Cool fill from the visible sky, baked at level load; dark under overhangs.
    int bakedAO=1; // Corners, ledges and alcoves darken, baked at level load.
    int thingShadows=1; // Decorations (columns, trees, hanging bodies) cast baked shadows.
    int lightFlow=1; // Brighter sectors light their neighbors through openings, flicker included.
    int ceilingCaustics=1; // Liquid caustics also ripple on the ceilings above.
    int causticsComputed=1; // Caustics are the light the liquid flat's waves gather, sharpening with distance from the water.
    int causticsGrow=1; // Caustic shapes grow from 2 to 4 units per texel with distance from the water.
    int causticsAngle=1; // Flashlight caustics follow how much the water reflects at the beam's angle.
    int causticsSway=1; // Caustics sway with the wave slope, more the farther the light travels.
    int causticsSprites=1; // Monsters and things near liquids catch the caustics too.
    int causticsShots=0; // Muzzle flashes and projectiles over liquids throw caustics, not just the flashlight.
    int dampShores=1; // Walls and banks just above liquids turn darker and damp up to a ragged line.
    int sunDisc=1; // A faint sun in the sky where the baked sunlight comes from.
    int sunShafts=1; // Soft sunbeams slanting down through ceiling holes and windows, with the dust motes in them.
    int sunScatter=1; // Sunbeams glow brighter seen toward the sun and fainter from behind it.
    int glossyScreens=1; // Monitor glass found in computer textures bulges, refracts the screen behind it and catches light.
    int weaponLighting=1; // The weapon's painted sheen and light come out at load; lights around the player relight it.
    // Performance: cheaper stand-ins for per-frame light work.
    int bakeOnlyLights=0; // Static lights only in the bake, with its light direction and flicker groups; pools as area lights.
    int gridSpriteLight=0; // Things take static light and their shadow light from a grid baked at level load.
    int unoccludedSurfaceLights=0; // Glowing textures' dynamic light skips wall tests; its gloss can show through walls.
    int fewerSurfaceLights=0; // Only the 8 nearest glowing-texture lights stay dynamic, not 24.
};
// glassFrame: on textures with monitor screens, where each glass pixel sits
// on its screen (see screen_glass.h); null otherwise.
struct Image { bool opaque=true; std::vector<byte> pixels; GpuTextureRef texture,glassFrame; int width=0,height=0,left=0,top=0; std::array<float,3> glow={1,1,1}; float glowWeight=0; std::array<float,3> emissionColor={}; float emissionWeight=0,emissionCoverage=0,emissionU=0,emissionV=0; std::array<float,3> average={}; bool averaged=false; };

// Frame profiling for the performance smoke test (I_Render3DProfile).
struct Profile { bool enabled=false; unsigned skip=0; const char *screenshotPath=nullptr; I_Render3DFrameProfile frame={}; };
extern Profile profile;

extern Settings settings;
extern SDL_Window *gameWindow;
extern FlashSet flashes;
// What the GPU gets: flashes with first/count into lightWords, the blockers
// and their per-direction slices (buildLightWords in scene3d.cpp).
extern FlashSet gpuFlashes;
extern std::vector<std::array<uint32_t,4>> lightWords;
extern FogLights fogLights;
extern std::vector<LightBlocker> lightBlockers;
/* Texture keys: wall index >=0, WAD lump = -1-lump. */
extern std::unordered_map<int,Image> images;
extern std::map<int,std::vector<Vertex>> batches,shadowBatches,decalBatches;
extern std::vector<Vertex> mistVertices,particleVertices,heatVertices;
// Sunbeam ribbons, drawn additively in the mist pass.
extern std::vector<Vertex> shaftVertices;
// Sky ceilings and sky walls, drawn into depth with the sky shader.
extern std::vector<Vertex> skyVertices;
extern GpuTextureRef seamTexture,contactTexture,cloudTexture,wallBakeTexture,flatLightTexture,causticTexture,causticPattern,shoreTexture;
// Bake-only lights: where static light comes from, per wall atlas texel and
// floor/ceiling cell (see bakeLights).
extern GpuTextureRef wallDirectionTexture,flatDirectionTexture;
extern float reflectionPlane;
extern bool reflectionActive;
extern bool worldPending,screenshotPending,flashlightOn;
extern float pitch;

// Screen layout of the world view and the 4:3 HUD in render-target pixels.
struct FrameView {
    Uniforms camera;
    float uiX,uiY,uiWidth,uiHeight,worldX,worldY,worldW,worldH;
};
struct SurfaceBinding { const Image *image,*next; float blend; };
struct SpriteDraw { const Image *image; std::vector<Vertex> vertices; };

// Lifecycle: init reads options and graphics.cfg, the backend may override
// them, then applySettings installs the engine hooks.
void sceneInit(SDL_Window *window);
void sceneShutdown();
void applySettings();
void settingsChanged();
void buildCloudTexture();
Image &wallImage(int index);
Image &lumpImage(int lump,bool flat);
// Builds the camera, layout and (when the world is visible) all geometry and lights.
FrameView prepareFrame(int width,int height);
std::vector<unsigned> hudPixels(const unsigned *pixels);
std::vector<std::array<float,4>> sectorInfo();
SurfaceBinding surfaceBinding(int key);
std::vector<SpriteDraw> weaponDraws(const Uniforms &camera);
std::vector<Vertex> crosshairVertices(float worldW,float worldH);
void screenQuad(std::vector<Vertex> &out,float x,float y,float w,float h,float u0,float v0,float u1,float v1,float light=1,unsigned mode=1);
// 32³ nearest-palette lookup, BGRA, indexed (b*32+g)*32+r.
std::vector<unsigned> paletteLUTColors(const unsigned *palette);
bool renderCheckDue();
void saveScreenshot(const void *bgra,int width,int height,int stride);
void updateTitle(int width,int height,const char *renderer);
} // namespace doom3d
#endif
