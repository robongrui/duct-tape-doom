# Duct-Tape DOOM

A hacked-together-for-fun port of id Software's 1997 DOOM source release to
modern macOS, Linux and Windows. It keeps the original game simulation, and adds an optional
GPU renderer that builds a 3D scene from the WAD at runtime. Every effect is
derived from the unmodified game data: nothing is replaced, converted or
downloaded, and the result is meant to still feel like DOOM.

Development was heavily assisted by Claude. I designed the rendering
approach, constraints and features, tested the results, and iterated on the
implementation; Claude was used extensively for writing and modifying code.

The default settings are how I prefer to play DOOM; everything else can be
switched in the graphics panel (F4).

Most of the effects are here because I liked them in other old games. Few of
them are actually modern; they are fake-modern, cheap tricks that look the
part. A few examples of how they work:

- **Sprite shadows** are not shadow maps. The enemy's current sprite frame is
  squashed flat and painted onto the floor as a soft, translucent black decal,
  so the shadow animates with the monster and fades and spreads as it rises
  off the ground. When a rocket or shot flashes nearby, a second copy is
  stretched away from that light. Shadows never climb walls
  (`addEnemyShadow` in [`platform/scene3d.cpp`](platform/scene3d.cpp)).
- **Muzzle flashes and projectiles** light the room in their own color, and
  that color is read from the sprite: the renderer looks at the bright,
  saturated pixels of the weapon flash or fireball artwork and ignores
  outlines and white cores. A plasma ball glows blue because its sprite is
  blue ([`platform/flash_lighting.h`](platform/flash_lighting.h)).
- **Glowing lamps, lava and computer displays** are found by texture name and
  palette color, not authored. Those pixels get a glow mask, a blurred halo
  and a few approximate point lights placed at the center of each texture
  repeat ([`platform/emissive.h`](platform/emissive.h)).
- **Monitor screens** are flat textures that pretend to be curved CRT glass.
  Dark rectangles framed by lighter housing are detected in the pixels, then
  the shader gives each one a fake dome normal for highlights, a slight lens
  bulge and darker edges. There is no extra geometry
  ([`platform/screen_glass.h`](platform/screen_glass.h)).
- **Light baked at level load** is old-school lightmapping. While the level
  loads, rays walk the 2D map from sector to sector, the way Doom itself sees
  it, and gather light from torches, lamps, glowing floors, sun through sky
  ceilings, one bounce off nearby surfaces, and a sky fill tinted by the sky
  texture. Grates and fences block light where their pixels are solid, and
  decorations such as columns, trees and hanging bodies stand in as their own
  sprite, so their shadows show the outline of the art. The result is
  stored once per texture pixel, so baked light steps along Doom's texel
  grid instead of smearing across it
  ([`platform/baked_lighting.h`](platform/baked_lighting.h)).
- **Fog** is a single closed-form distance haze with a cone for the
  flashlight, not a volumetric raymarch.

- **Accelerated 3D renderer** (SDL_gpu: Metal on macOS, Vulkan on Linux and
  Windows) at native resolution, any aspect ratio, with mouse look,
  interpolated movement and 4× MSAA. The classic 320×200 software renderer
  is one keypress away.
- **Lighting from the WAD itself:** muzzle flashes and projectiles cast
  colored light and shadows, lamps and lava glow with bloom, decorations cast
  light sized to their artwork, and an optional flashlight.
- **Atmosphere:** height fog over pits and liquids, sprite shadows,
  xBR pixel-art upscaling for sprites, surface detail and gloss.
- **Compatibility:** vanilla and limit-removing maps, including DEHACKED
  patches. Boom, MBF21 and GZDoom maps are not supported.
- Headless demo runner and regression tests that need no game data.

Multiplayer is not implemented. The Mac build is the most tested; Linux and
Windows builds compile in CI but have seen little real play.

## Download and play

Ready-to-play builds for macOS, Windows and Linux are on the
[Releases page](https://github.com/robongrui/duct-tape-doom/releases). Each
includes Freedoom, so it plays straight away. On Windows and Linux, drop your
own `.wad` files into the `wads` folder next to the game; on the Mac, choose
your WAD folder when the app starts.

## Game data

This repository contains **no game data**. To build and play from source you
need an IWAD:

- **Freedoom** (free): download `freedoom1.wad` or `freedoom2.wad` from
  [freedoom.github.io](https://freedoom.github.io/).
- **Original DOOM / DOOM II** (commercial): use the `.wad` files from a copy
  you own, e.g. from Steam or GOG.

Please do not commit or upload commercial WAD files to this repository.

## Build

You need CMake 3.21+ and a C11/C++17 compiler (Apple Clang, Clang, GCC or
Visual Studio 2022). SDL3 is downloaded automatically if not installed.

```sh
cmake -S . -B build/play -DCMAKE_BUILD_TYPE=Release -DDOOM_BUILD_SDL=ON \
  -DDOOM_GAME_DATA="/absolute/path/to/freedoom1.wad"
cmake --build build/play --parallel
ctest --test-dir build/play --output-on-failure
```

On macOS this produces `build/play/Play DOOM.app`, with the WAD bundled.
On Linux and Windows run `doom -iwad /path/to/game.wad`.
[BUILDING.md](BUILDING.md) has all build options, platform notes and a
technical description of the renderer.

## Play

Controls, the F4 graphics panel and command-line options are in
[PLAY.md](PLAY.md).

## License

The game code is licensed under the GNU General Public License v2; see
[LICENSE.TXT](LICENSE.TXT). It is based on the DOOM source code released by
id Software in 1997; John Carmack's original release notes are in
[linuxdoom-1.10/README.TXT](linuxdoom-1.10/README.TXT).

Third-party components keep their own licenses:

- [SDL3](https://libsdl.org/) — zlib license (downloaded at build time)
- [Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3) — LGPL-2.1+ (downloaded at build time)
- xBR pixel-art scaling by Hyllian — MIT, see [third_party/xbr/LICENSE.txt](third_party/xbr/LICENSE.txt)
- [Spleen](https://github.com/fcambus/spleen) 8x16 font by Frederic Cambus, used by the in-game settings panel — BSD 2-Clause, see [third_party/spleen/LICENSE.txt](third_party/spleen/LICENSE.txt)

DOOM is a trademark of id Software. This project is not affiliated with or
endorsed by id Software, ZeniMax or Bethesda.
