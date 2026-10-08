#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$project_dir/build/play"}
output_app="$project_dir/Play DOOM.app"
source_app="$build_dir/Play DOOM.app"

if [ ! -x "$source_app/Contents/MacOS/Play DOOM" ]; then
    echo "Build the SDL app first; see BUILDING.md." >&2
    exit 1
fi
rm -rf "$output_app"
ditto "$source_app" "$output_app"
resources="$output_app/Contents/Resources"
mkdir -p "$resources/Licenses"
cp "$project_dir/LICENSE.TXT" "$resources/Licenses/DOOM-GPL-2.txt"
cp "$project_dir/third_party/xbr/LICENSE.txt" "$resources/Licenses/Hyllian-xBR.txt"
cp "$project_dir/PLAY.md" "$resources/PLAY.md"
if [ -f "$build_dir/_deps/nukedopl3-src/LICENSE" ]; then
    cp "$build_dir/_deps/nukedopl3-src/LICENSE" "$resources/Licenses/Nuked-OPL3-LGPL-2.1.txt"
fi
if [ -f "$project_dir/build/dependencies/SDL3-3.4.18/LICENSE.txt" ]; then
    cp "$project_dir/build/dependencies/SDL3-3.4.18/LICENSE.txt" "$resources/Licenses/SDL3.txt"
fi
if [ -f "$project_dir/build/game-data/COPYING.txt" ]; then
    mkdir -p "$resources/Licenses/Freedoom"
    for item in COPYING.txt CREDITS.txt CREDITS-MUSIC.txt README.html; do
        cp "$project_dir/build/game-data/$item" "$resources/Licenses/Freedoom/$item"
    done
fi
codesign --force --sign - "$output_app"
echo "Ready: $output_app"
