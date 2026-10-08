A hacked-together-for-fun DOOM port. Each download includes **Freedoom**, so you can
play straight away; add your own WADs to play DOOM, DOOM II or custom maps.

## Download

| System | File | How to play |
| --- | --- | --- |
| macOS 11 or newer (Apple Silicon and Intel) | `DuctTapeDOOM-mac.zip` | Unzip and open **Play DOOM.app**. Choose your WAD folder or click **Play Freedoom**. |
| Windows 10/11, 64-bit | `DuctTapeDOOM-windows.zip` | Unzip, drop your `.wad` files into the `wads` folder and run `doom.exe`. |
| Linux, x86-64 | `DuctTapeDOOM-linux.tar.gz` | Unpack, drop your `.wad` files into `wads` and run `./doom`. |

With no WADs in the `wads` folder, Freedoom starts. With a base game (such as
`doom.wad` or `doom2.wad`) and a custom map WAD, the map is loaded on top of the
game. Controls and options are in `PLAY.md`.

**Windows:** the program is not code-signed, so SmartScreen may say "Windows
protected your PC". Click **More info**, then **Run anyway**.

**Linux:** you need a Vulkan driver for the 3D renderer; without one the game
falls back to the classic renderer.

## Before you play

This is a pre-release. The Mac version is the most tested. The Windows and Linux
versions are built automatically but have had little real play, so please
[report problems](https://github.com/robongrui/duct-tape-doom/issues).

Supported maps: vanilla and limit-removing, including DEHACKED patches. Boom,
MBF21 and GZDoom maps are not supported. Multiplayer is not implemented.
