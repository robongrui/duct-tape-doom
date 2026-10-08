# Play DOOM on your Mac

Double-click **Play DOOM.app**. No installation is needed, and you can move
the app to Applications. If you have the source code rather than the app,
build it first as described in [BUILDING.md](BUILDING.md).

**On Windows and Linux**, put your `.wad` files into the `wads` folder next to
`doom.exe` (or `doom`) and start the game. Base games are found automatically;
with several, the game asks which one to play, and also offers the included
Freedoom. One custom map WAD in the folder is loaded on top of the base game;
with several, the game asks. A `.deh` or `.bex` patch with the same name as the
map is applied too. With no WADs in the folder, Freedoom starts. The rules below
are the same on the Mac.

At startup, choose **Choose WAD Folder…**, then select the folder directly
containing your `.wad` files. The loader finds base games and custom WADs;
if there are several, it asks which one to play. Put the required DOOM or
DOOM II base game in the same folder as your custom map. Without a base game
in the folder, compatible DOOM episode maps use the included Freedoom Phase 1.
DOOM II maps need a DOOM II base game or Freedoom Phase 2. The loader remembers
the last folder. Cancel quits; **Play Freedoom** launches the bundled game.

Custom maps made for vanilla DOOM or "limit-removing" ports work, including
their DEHACKED patches: a patch inside the WAD is applied automatically, and a
`.deh` or `.bex` file with the same name next to the WAD is loaded with it.
Maps for Boom, MBF21, GZDoom or other extended engines are not supported.

Press **Esc**, choose **New Game**, choose an episode, then choose a difficulty.
Use the arrow keys and Return to select menu items.

| Action | Control |
| --- | --- |
| Move forward/back | W / S |
| Move sideways | A / D |
| Turn | Mouse, or left/right arrows |
| Look up/down | Move the mouse vertically |
| Fire | Left mouse button, or Control |
| Open doors / use switches | Space |
| Run | Hold Shift |
| Select weapon | 1–7 |
| Map | Tab |
| Menu / release mouse | Esc |
| Save / load | F2 / F3, or the menu |
| Fullscreen | F11 |
| Graphics settings | F4 |
| Screenshot | F7 |
| Flashlight (3D renderer) | F |
| Quit | Command-Q |

On keyboards with media function keys, hold **Fn** when using the F keys.
Change sound volume and mouse sensitivity under **Options**.

Movement accelerates and changes direction faster, with less glide after
releasing the keys and reduced camera/weapon bob. Walking and running
retain their original top speeds. Launch with `-classicmovement` to restore the
original feel; demo playback and recording always use the original movement.

The **3D** renderer draws the level at the window's actual pixel
resolution, including Retina pixels. Resize the window to any aspect ratio,
or press F11 to fill your display. It uses 4× antialiasing when supported and
interpolates the camera and moving sprites between the original game ticks.
Enemies have subtle floor shadows made from their animated WAD sprite silhouettes.
Shadows fade and spread beneath airborne enemies and stay within their sector's
floor geometry. Player shots and enemy ranged attacks briefly light nearby
walls, floors, ceilings and sprites with a color taken from the WAD's flash
artwork. Glowing projectiles carry matching colored lights as they travel and
through their bright impact frames: green balls cast green light, for example.
Walls and closed doors block these lights. Nearby enemies also cast a shadow
away from the strongest light. These effects are available in the 3D renderer.

Press **F** to toggle the flashlight in the 3D renderer. Its warm, soft-edged beam
follows mouse look and stops at walls and closed doors. It is held low and to
the right of your view, so wall edges, door frames and ledges cast visible
shadows, as muzzle flashes and projectiles do. The artwork under the
beam supplies a subtle ambient tint: red surfaces warm the view and green
surfaces add a green tint. The color fades smoothly as you aim elsewhere or
switch the flashlight off. It uses one target-color sample at 30 Hz and adds
no reflected world lights. Aiming into sky produces no tint.
Launch with `-flashlight` to start with the light on. `-flashlighttint 0` disables
the tint, `-flashlighttint 1` selects the default, and `-flashlighttint 2` makes it stronger.

Press **F4** for the graphics panel. It opens inside the game as a text-mode
style panel over the paused, dimmed view. Its options are sorted into tabs, the
description of the highlighted option is shown at the bottom, and options that
only refine another one are greyed out while that one is off.

Use **Up/Down** (or the mouse wheel) to choose an option, **Left/Right** or
**Return** to change it, **Tab** (or **1**–**5**) to switch tabs, **R** to
restore the defaults, and **Esc** or **F4** to close the panel and apply the
changes. The mouse works too: click an option to change it (right-click goes
backwards), click a slider to set it, and click a tab to open it. **F7** still
takes a screenshot.

**Display**

- **Renderer:** accelerated 3D or the classic 320×200 software view.
- **Resolution:** native pixels, 75%, or 50% for a lighter GPU workload.
- **Field of view:** 60–120 degrees, measured against the classic 4:3 view.
- **Widescreen:** show more world at the sides rather than stretching it.
- **Mouse look, crosshair and FPS display:** optional toggles.
- **Retro scanlines** and **Doom palette colors:** the palette option quantizes the final image, including colored lighting, fog and bloom, to the WAD palette; off by default.

**Textures**

- **Walls and floors:** crisp pixels, smooth bilinear, or sharp bilinear, which keeps texels flat and only softens the step between them (**Edge softness** sets how far). **Palette mipmaps** keep distant smooth or sharp surfaces from shimmering.
- **Sprites:** pixel-art upscale (xBR), crisp pixels, or soft bilinear filtering.
- **Bump detail and metal gloss:** derived from the WAD artwork, plus rounded sprite lighting with rim light on backlit enemies; affects dynamic lights only.
- **Detail textures:** a fine grain that shows only up close, off by default, with its **strength**.
- **Emissive textures:** glowing lamp strips, computer displays, lava and nukage, with soft bloom and colored light on nearby surfaces.
- **Glossy monitor screens:** screens in computer textures get curved CRT glass that bends the picture and catches highlights.

**Lighting**

- **Baked at level load:** static lights from torches, lamps and glowing textures; sun shadows; bounce light; sky light, a fill from the visible sky tinted by its upper rows; ambient occlusion in corners, ledges and alcoves; decoration shadows from columns, trees and hanging bodies (changing it re-bakes the level); and texel-aligned baked light, which steps once per texture pixel instead of in coarse 4–5 unit cells.
- **Soft light seams and contact shading:** sector light blends over a short band across openings, and floors, ceilings and wall bases darken near walls.
- **Sector light flow:** brighter sectors light the floors and walls beyond their openings, so flickering rooms flicker faintly next door. Doors count as closed, as they start.
- **Door light spill:** when a door opens, light from the brighter room spills through it, scaled by how far the door is open.
- **Eye adaptation:** moving between bright and dark areas briefly over- or underexposes the view, stepped in sixteenths like Doom's light levels.
- **Flashlight tint:** the middle is the default, and fully left disables it.
- **Flashlight silhouettes:** things in the beam throw a hard-edged sprite silhouette onto the wall behind them.
- **Player shadow:** your own sprite's shadow, cast from the sun or the strongest light above you.

**Atmosphere**

- **Atmospheric fog:** height fog with soft cloud layers over pits, liquids and damaging floors. Toggle it off for a clearer view or a lighter GPU workload.
- **Sunbeams** (with sun shadows), **sun in the sky** and **dust motes** that glint in sunbeams and the flashlight beam.
- **Heat haze:** rows of Doom pixels shimmer sideways at the tic rate above lava, red-hot damaging floors and flames.
- **Blood splats and pools:** hitscan hits leave splats on the floor and on walls close behind the target, and corpses grow a pool in steps. **Wet blood shine** makes fresh blood glint under dynamic lights before it dries matte.
- **Soft effect sprites:** explosions, puffs and teleport fog no longer slice into walls, and dissolve on an ordered dither where they meet floors and ceilings.
- **Liquids:** reflections of the scene in nearby water, nukage, slime and blood (rendered a second time at half resolution), optionally in **retro PS1 style** at quarter resolution with palette colors; caustics on the surrounding walls and optionally the ceilings; and splashes with ripple rings where things land or wade.

**Performance** (off by default; needs baked static lights)

- **Bake-only static lights:** torches, lamps and glowing textures stop costing per-frame light work; shots, explosions and the flashlight stay dynamic.
- **Grid lighting for monsters:** monsters and items take static light from a grid baked at level load instead of tracing rays every frame.

All settings are saved in `graphics.cfg`. The effect switches there are named `blood`, `blood_shine`, `flashlight_shadows`, `soft_effects`, `heat_haze`, `eye_adaptation`, `splashes`, `dust_motes`, `player_shadow`, `door_light`, `texel_lighting`, `sky_light`, `baked_occlusion`, `decoration_shadows`, `light_flow` and `ceiling_caustics`.

Animated flats and walls (NUKAGE, FWATER, falls) crossfade between frames,
door and lift movement is interpolated between game tics, and bullets leave
fading marks and a burst of wall-colored debris. Pickups pulse softly and glow
without lighting their surroundings; exploding barrels light the room, throw
sparks and leave a scorch mark on the floor. Torches, candles, lamps and
other glowing decorations cast colored light sized to their artwork; flames
flicker in DOOM's own stepped fire-flicker rhythm. With atmospheric fog on, the
sky hazes toward the horizon.

**Pixel-art upscale (xBR)** is the default for enlarged enemies, items and
weapon sprites. It reconstructs diagonal edges on the GPU using original
sprite colors and transparency, adapting to their size on screen. Smaller
sprites use alpha-aware filtering to reduce shimmer. This option is independent
of wall/floor smoothing. It preserves the original WAD artwork and requires no
asset conversion or download. The classic renderer retains its original sprites.

Emissive textures keep selected pixels bright in dark rooms, using the original
WAD artwork and palette. They add soft bloom around visible glowing pixels and
cast colored light onto nearby walls, floors and sprites. Solid walls and closed
doors block the light. Custom masks can be placed in `Emissive/walls/NAME.png` or
`Emissive/flats/NAME.png` beside `graphics.cfg`. Use uppercase WAD texture names
and the exact original texture dimensions. White pixels emit, black pixels
receive normal lighting, and gray pixels give partial emission; transparent
mask pixels do not emit. Masks override automatic detection. Restart after
changing masks. A black mask disables emission for one texture.

Atmospheric fog uses an analytic exponential height falloff, plus three or four
scrolling cloud layers derived from the map's sector data. Floors at least 24
units below all neighboring floors are tagged as pits. NUKAGE, FWATER, SLIME,
BLOOD and LAVA flats get haze tinted by the flat's average palette color;
damaging sectors get toxic haze. Layers reuse the floor's BSP polygons, follow
moving floors, and fade against walls and sprites using the scene depth.
They are a subtle floor effect and become less visible at grazing angles.
Outdoor sectors get thinner mist; the global fog density drops while the
player is in a sector with a sky ceiling. This global density is a stylistic
approximation, rather than integration through each sector along the ray.
Flashlight and colored-light glows retain their existing inexpensive shadow
approximation. The weapon and HUD stay clear of fog. Launch with `-fog 0` to
disable it.

**Doom palette colors** is a separate F4 toggle. A 32×32×32 RGB lookup quantizes
the final image after fog and bloom to colors in the current WAD palette,
including damage and bonus palette shifts. Launch with `-palette 1` to enable
it or `-palette 0` to disable it. This art pass intentionally changes colored
lights and can produce visible color bands.

Mouse look tilts the camera. Shooting retains DOOM's automatic vertical aiming.
Graphics choices are saved in `graphics.cfg`. F4 and F7 replace the original
sound-menu and end-game shortcuts; those actions are still in the game menu.
Screenshots capture the full display resolution and go into `Screenshots`
inside the save folder below.

A Mac app built with Freedoom bundled (see [BUILDING.md](BUILDING.md)) includes
**Freedoom: Phase 1**, which supplies free levels, graphics, sounds and music
for the DOOM engine. Its credits and license are included inside the app. The
app never contains the commercial DOOM game data.

Saves, settings and the launch log live in:

`~/Library/Application Support/Local Games/DOOM/`

Saves belong to this native build; old DOS/Linux DOOM saves are incompatible.

To use an original DOOM or DOOM II IWAD you own, launch the executable with an
absolute path:

```sh
"./Play DOOM.app/Contents/MacOS/Play DOOM" -iwad "/absolute/path/to/doom.wad"
```

For an exact initial window size, use `-width 1920 -height 1080` (in window
coordinates; Retina displays may supply more physical pixels). You can also
use `-fullscreen`, `-fov 100`, or `-classic`. Sizes between 320×240 and
8192×8192 are accepted; resizing adjusts the render targets automatically.

Launch with `-opl` to hear the music through an emulated OPL3 FM chip with
the WAD's own instrument bank, as on a DOS Sound Blaster, instead of the Mac's
synthesizer. Other options for custom WADs: `-deh file.deh` applies extra
patches, `-nodeh` ignores patches inside WADs, and `-blockmap` rebuilds each
map's collision blockmap.

On macOS the app renders with Metal. The same 3D
renderer runs on Linux and Windows through Vulkan, with OPL music. Multiplayer is
not implemented. Build instructions are in [BUILDING.md](BUILDING.md).
