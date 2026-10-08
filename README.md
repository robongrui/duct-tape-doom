# Duct-Tape DOOM

A hacked-together-for-fun port of id Software's 1997 DOOM source release to
modern macOS, Linux and Windows. It keeps the original game simulation, and adds an optional
GPU renderer that builds a 3D scene from the WAD at runtime. Every effect is
derived from the unmodified game data: nothing is replaced, converted or
downloaded, and the result is meant to still feel like DOOM.

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

DOOM is a trademark of id Software. This project is not affiliated with or
endorsed by id Software, ZeniMax or Bethesda.
