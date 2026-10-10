/* The F4 graphics options; see settings_panel.h. */
extern "C" {
#include "doomtype.h"
}
#include <SDL3/SDL.h>
#include "settings_panel.h"

namespace doom3d {
namespace {
Option check(const char *title,int *flag,const char *tip,OptionCondition enabled=nullptr) {
    Option option{};option.kind=OptionKind::Check;option.title=title;option.tip=tip;option.flag=flag;option.enabled=enabled;return option;
}
Option choice(const char *title,int *flag,std::vector<const char*> items,std::vector<int> values,const char *tip,
              OptionCondition enabled=nullptr) {
    Option option{};option.kind=OptionKind::Choice;option.title=title;option.tip=tip;
    option.flag=flag;option.items=items;option.values=values;option.enabled=enabled;return option;
}
Option slider(const char *title,float *number,double low,double high,double step,const char *tip,
              OptionCondition enabled=nullptr) {
    Option option{};option.kind=OptionKind::Slider;option.title=title;option.tip=tip;
    option.number=number;option.low=low;option.high=high;option.step=step;option.enabled=enabled;
    return option;
}
}

std::vector<OptionTab> optionTabs(Settings &s) {
    return {
        {"Display",{
            {"Renderer",{
                choice("Renderer",&s.accelerated,{"Accelerated 3D","Classic software"},{1,0},
                    "The 3D renderer draws the world at your window's pixel resolution; classic is the original 320x200 view."),
                choice("Resolution",&s.scale,{"Native (100%)","75%","50%"},{100,75,50},
                    "Lower resolutions lighten the GPU workload."),
                slider("Field of view",&s.fov,60,120,5,"Measured against the classic 4:3 view."),
                check("Widescreen",&s.widescreen,"Show more world at the sides rather than stretching it.")}},
            {"View",{
                check("Mouse look",&s.look,"Mouse look tilts the camera; combat keeps DOOM's automatic aiming."),
                check("Crosshair",&s.crosshair,"A small crosshair in the middle of the view."),
                check("Show FPS in window title",&s.fps,"Frames per second and the GPU driver in the window title.")}},
            {"Retro look",{
                check("Retro scanlines",&s.retro,"Dark lines between the rows of the picture, like an old CRT monitor."),
                check("Doom palette colors",&s.palette,"Quantize the final lighting and fog to the current WAD palette.")}}}},
        {"Textures",{
            {"Filtering",{
                choice("Walls and floors",&s.filter,{"Crisp pixels","Smooth (bilinear)","Sharp bilinear"},{0,1,2},
                    "Sharp bilinear keeps every texel flat and crisp and only softens the step between texels, "
                    "by about a screen pixel plus the edge softness below."),
                slider("Edge softness",&s.sharpSoftness,0,1,0.05,
                    "With sharp bilinear walls and floors: how far texel edges blend, in texels. Left keeps only a one-pixel edge; right approaches bilinear.",
                    [](const Settings &d){return d.filter==2;}),
                check("Palette mipmaps",&s.paletteMips,
                    "With smooth or sharp walls and floors: far and grazing surfaces read smaller versions snapped to the WAD palette, "
                    "as software Quake did, instead of shimmering. Crisp pixels stay vanilla.",
                    [](const Settings &d){return d.filter!=0;}),
                choice("Sprites",&s.spriteFilter,{"Crisp pixels","Soft (bilinear)","Pixel-art upscale (xBR)"},{0,1,2},
                    "xBR reconstructs diagonal edges of enlarged sprites from their original colors.")}},
            {"Surface detail",{
                check("Bump detail and metal gloss",&s.detail,
                    "Bump detail and metal gloss derived from the WAD artwork; affects dynamic lights, and static ones with bake-only lights."),
                check("Varied highlights",&s.variedHighlights,
                    "Gloss highlights are tight and bright where the artwork runs smooth and broad and dim where it is busy, "
                    "with slime and blue panels sharper than worn grey metal.",
                    [](const Settings &d){return d.detail!=0;}),
                check("Relit weapon",&s.weaponLighting,
                    "The sheen and light painted into the weapon artwork are taken out, and the lights around you relight it: "
                    "their color, darker rounded edges, glowing rims against lights behind it, and highlights that slide over "
                    "the metal as you look around and the weapon bobs."),
                choice("Detail textures",&s.detailTextures,{"Off","With filtered walls","Always"},{0,1,2},
                    "A fine grain over walls and floors that shows only up close and fades out with distance, picked per texture "
                    "from its colors (stone, metal, wood, flesh). Scale and distance are in graphics.cfg (detail_scale, detail_fade)."),
                slider("Detail strength",&s.detailStrength,0.05,0.6,0.05,"How far the grain lightens and darkens the artwork.",
                    [](const Settings &d){return d.detailTextures!=0;})}},
            {"Glow",{
                check("Emissive textures",&s.emissive,
                    "Lamp strips, computer displays, lava and nukage glow with soft bloom and colored light on nearby surfaces."),
                check("Glossy monitor screens",&s.glossyScreens,
                    "Screens found in computer textures get curved CRT glass: the picture bends and shifts behind it as you move, "
                    "and it catches sharp highlights from lamps, shots and the flashlight.")}}}},
        {"Lighting",{
            {"Baked at level load",{
                check("Baked static lights",&s.bakedLights,
                    "Torches, lamps and glowing textures light the whole level, baked at level load; nearby ones still flicker."),
                check("Sun shadows",&s.sun,"Shadows and sunlit patches baked at level load from the sky texture and map geometry."),
                check("Bounce light",&s.bounce,
                    "Sunlit and lamp-lit surfaces tint and lift what faces them, including ceilings; baked at level load."),
                check("Sky light",&s.skyLight,
                    "A cool fill from the visible sky, tinted by the sky texture: windows glow faintly, overhangs and alcoves outdoors stay darker."),
                check("Ambient occlusion",&s.bakedAO,"Corners, ledges, stair risers and alcoves darken in light-level steps."),
                check("Decoration shadows",&s.thingShadows,
                    "Columns, trees and hanging bodies cast baked sun and lamp shadows shaped like their sprites. Changing it re-bakes the level."),
                check("Texel-aligned baked light",&s.texelLight,
                    "Baked lights, sun patches and bounce blend smoothly but step per texture pixel, instead of in hard 4-5 unit map cells.")}},
            {"Live light",{
                check("Light seams and contact shading",&s.softLight,"Blend sector light across openings and darken floors along walls."),
                check("Sector light flow",&s.lightFlow,
                    "Brighter rooms light the floors and walls beyond their openings; flickering rooms flicker faintly next door."),
                check("Door light spill",&s.doorLight,"Light from a brighter room spills through a door as it opens."),
                check("Moving doors re-light",&s.movingRelight,
                    "Baked lamp light and sunlight follow doors, lifts and lowering walls: as a door rises, the light beyond it "
                    "steps in under the edge, re-baked around it in the background.",
                    [](const Settings &d){return d.bakedLights!=0||d.sun!=0;}),
                check("Eye adaptation",&s.eyeAdaptation,"Brief stepped over- or underexposure when you move between bright and dark areas.")}},
            {"Flashlight and shadows",{
                slider("Flashlight tint",&s.flashlightTintGain,0,2,0.5,"Subtle target-color tint: left is off, middle is the default, right is stronger."),
                check("Flashlight silhouettes",&s.flashlightShadows,"Things caught in the flashlight throw their sprite outline onto the wall behind them."),
                check("Player shadow",&s.playerShadow,"Your own sprite's shadow from the sun or the strongest light above you.")}}}},
        {"Atmosphere",{
            {"Air",{
                check("Atmospheric fog",&s.fog,"Height fog and soft cloud layers derived from pits, liquids and damaging floors."),
                check("Sunbeams",&s.sunShafts,
                    "Soft shafts of sunlight slant down through ceiling holes and windows, where the dust motes glint. Needs sun shadows.",
                    [](const Settings &d){return d.sun!=0;}),
                check("Sunbeam scattering",&s.sunScatter,
                    "Sunbeams glow brighter when you look toward the sun through them and fainter from behind, as dust scatters light onward.",
                    [](const Settings &d){return d.sun!=0&&d.sunShafts!=0;}),
                check("Sun in the sky",&s.sunDisc,"A faint sun disc and soft glow where the baked sunlight comes from, over the sky texture's brightest part."),
                check("Dust motes",&s.dust,"Specks that glint in indoor sunbeams and the flashlight beam."),
                check("Heat haze",&s.heatHaze,"Rows of pixels shimmer above lava, hot damaging floors and flames.")}},
            {"Combat",{
                check("Blood splats and pools",&s.blood,
                    "Hitscan hits leave splats on floors and nearby walls; corpses grow a pool. Colors come from the WAD's blood sprite."),
                check("Wet blood shine",&s.bloodShine,"Fresh blood on the floor glints in a brighter palette red under dynamic lights, then dries matte.",
                    [](const Settings &d){return d.blood!=0;}),
                check("Soft effect sprites",&s.softSprites,"Explosions, puffs and teleport fog stop slicing into walls and dissolve on a dither at floors.")}},
            {"Liquids",{
                check("Reflections",&s.reflections,
                    "Mirror the scene in nearby water, nukage, slime and blood; renders the view a second time at half resolution."),
                check("Retro reflections (PS1 style)",&s.retroReflections,
                    "Eighth resolution, hard pixels, Doom palette colors and a stepped wobble.",
                    [](const Settings &d){return d.reflections!=0;}),
                check("Caustics",&s.caustics,
                    "Rippling light from water, nukage, slime, blood and lava on the walls around them; pattern taken from the level's liquid flat."),
                check("Caustics on ceilings",&s.ceilingCaustics,"The ripples also play on the ceiling above water, nukage, slime and lava.",
                    [](const Settings &d){return d.caustics!=0;}),
                check("Computed caustics",&s.causticsComputed,
                    "The light the liquid's waves actually gather, worked out from its flat at level load: soft near the water, "
                    "sharp lines about 40 units up, broader above. Off shows the flat's own bright streaks.",
                    [](const Settings &d){return d.caustics!=0;}),
                check("Caustics grow with distance",&s.causticsGrow,
                    "The shapes widen from 2 to 4 units per pixel the farther the light travels from the water; off keeps one unit per pixel like the floor.",
                    [](const Settings &d){return d.caustics!=0;}),
                check("Flashlight reflection angle",&s.causticsAngle,
                    "Water reflects more of the beam the flatter it strikes: aiming across a pool lights the walls, aiming straight down barely does.",
                    [](const Settings &d){return d.caustics!=0;}),
                check("Caustic sway",&s.causticsSway,
                    "The pattern sways with the wave slope, more the higher up the wall, like light dancing under a bridge.",
                    [](const Settings &d){return d.caustics!=0;}),
                check("Caustics on monsters and things",&s.causticsSprites,
                    "Monsters, items and decorations near water, nukage, slime and lava catch the ripples too, in their own colors.",
                    [](const Settings &d){return d.caustics!=0;}),
                check("Caustics from shots",&s.causticsShots,
                    "Muzzle flashes and projectiles over liquids throw brief ripples on the walls around them, like the flashlight. Subtle next to their direct light.",
                    [](const Settings &d){return d.caustics!=0;}),
                check("Damp shores",&s.dampShores,
                    "Walls and banks just above water, nukage, slime and blood turn darker and damp up to a ragged line, "
                    "a little stained by the liquid. Lava stays dry."),
                check("Splashes",&s.splashes,"Droplets and ripple rings where things land, explode or wade in liquids; rings disturb reflections.")}}}},
        {"Performance",{
            {"Cheaper light work",{
                check("Bake-only static lights",&s.bakeOnlyLights,
                    "Torches, lamps and glowing textures stop costing per-frame light work: their flicker, bump detail and gloss come from the bake, "
                    "and lava and nukage pools glow as one light each. Shots, explosions and the flashlight stay dynamic. Changing it re-bakes the level.",
                    [](const Settings &d){return d.bakedLights!=0;}),
                check("Grid lighting for monsters",&s.gridSpriteLight,
                    "Monsters and items take static light, and the light their shadow falls from, from a grid baked at level load "
                    "instead of tracing rays every frame.",
                    [](const Settings &d){return d.bakedLights!=0;}),
                check("Unblocked glowing-texture light",&s.unoccludedSurfaceLights,
                    "Light from lamp strips and glowing flats skips the wall tests per pixel. Cheaper near light panels, "
                    "but their highlights can show on shiny surfaces through walls.",
                    [](const Settings &d){return d.emissive!=0&&d.bakeOnlyLights==0;})}}}}};
}
}
