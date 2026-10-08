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

Press **F4** for the graphics panel:

- **Renderer:** accelerated 3D or the classic 320×200 software view.
- **Resolution:** native pixels, 75%, or 50% for a lighter GPU workload.
- **Atmospheric fog:** height fog with soft cloud layers over pits, liquids and damaging floors; enabled by default. Toggle it off for a clearer view or a lighter GPU workload.
- **Doom palette colors:** optional final palette quantization, including colored lighting, fog and bloom; off by default.
- **Flashlight tint:** adjust the subtle target-color tint; the middle is the default, and fully left disables it.
- **Field of view:** 60–120 degrees, measured against the classic 4:3 view.
- **Sprites:** pixel-art upscale (xBR), crisp pixels, or soft bilinear filtering.
- **Widescreen:** show more world at the sides rather than stretching it.
- **Smooth walls and floors:** switch world surfaces between crisp and bilinear filtering.
- **Emissive textures:** glowing lamp strips, computer displays, lava and nukage, with soft bloom and colored light on nearby surfaces; enabled by default.
- **Crosshair, mouse look, retro scanlines and FPS display:** optional toggles.
- **Surface detail:** bump detail and metal gloss derived from the WAD artwork, plus rounded sprite lighting with rim light on backlit enemies; affects dynamic lights only.
- **Soft light seams and contact shading:** sector light blends over a short band across openings, and floors, ceilings and wall bases darken near walls.
- **Liquid reflections:** nearby water, nukage, slime and blood mirror the scene with a gentle wobble; renders the view a second time at half resolution.
- **Retro reflections (PS1 style):** with liquid reflections on, the mirror image uses quarter resolution, hard pixels, Doom palette colors and a stepped wobble. Off by default.

The panel's second column holds **experimental effects**. They are all on by default, and each has its own switch so any of them can be turned off:

- **Blood splats and pools:** hitscan hits leave splats on the floor and on walls close behind the target, and corpses grow a pool in steps. The artwork is generated from the WAD's blood sprite colors.
- **Wet blood shine:** fresh blood on floors glints in a brighter palette red under dynamic lights (flashlight, muzzle flashes, projectiles), then dries matte and darker. Wall splats stay matte. Blood spraying through the air glints the same way on its rounded sprite normals (needs Surface detail).
- **Flashlight silhouettes:** things in the flashlight beam throw a hard-edged sprite silhouette onto the wall behind them. For this effect the light is treated as held low and to the right.
- **Soft effect sprites:** explosions, puffs and teleport fog no longer slice into walls, and they dissolve on an ordered dither where they meet floors and ceilings.
- **Heat haze:** rows of Doom pixels shimmer sideways at the tic rate above lava, red-hot damaging floors and flames.
- **Eye adaptation:** moving between bright and dark areas briefly over- or underexposes the view, stepped in sixteenths like Doom's light levels.
- **Liquid splashes:** droplets and twelve-sided ripple rings appear where things land, explode or wade in liquids, and the rings disturb reflections.
- **Dust motes:** single-pixel specks that glint in indoor sunbeams and the flashlight beam.
- **Player shadow:** your own sprite's shadow, cast from the sun or the strongest light above you.
- **Door light spill:** when a door opens, light from the brighter room spills through it, scaled by how far the door is open.
- **Texel-aligned baked light:** baked lights, sun patches and bounce light blend smoothly between their map cells and step once per texture pixel, so light edges follow the artwork instead of a coarse 4–5 unit grid. Switch it off for the original hard cells.
- **Sky light:** a fill from the visible sky, tinted by the sky texture's upper rows. Indoors, surfaces that see sky through windows and openings brighten toward the outdoor light; outdoors, overhangs and alcoves drop a few light steps. Baked at level load.
- **Baked ambient occlusion:** corners, ledges, stair risers and alcoves darken in light-level steps. Only sector and baked light are affected; dynamic lights are not.
- **Decoration shadows:** columns, trees, hanging bodies and other solid, non-glowing decorations block the sun, baked lamps, bounce and sky light with their sprite's silhouette. Changing it re-bakes the level.
- **Sector light flow:** brighter sectors light the floors and walls beyond their openings over a longer distance than the seam blend, using their current light, so flickering rooms flicker faintly next door. Doors count as closed, as they start.
- **Ceiling caustics:** with liquid caustics on, the ripples also play on the ceilings above liquids, fading in tall rooms.

In `graphics.cfg` these settings are `blood`, `blood_shine`, `flashlight_shadows`, `soft_effects`, `heat_haze`, `eye_adaptation`, `splashes`, `dust_motes`, `player_shadow`, `door_light`, `texel_lighting`, `sky_light`, `baked_occlusion`, `decoration_shadows`, `light_flow` and `ceiling_caustics`.

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
