#!/bin/bash
# Installs the Hello Neighbor side into the game: the UE4SS C++ mod (which is also the ReShade add-on) and the
# NeighborCraft.fx effect, and switches the ReShade preset to it (the old preset is kept as .neighborcraft-bak).
#   install.sh           install / update (Hello Neighbor must be closed: the DLL is locked while it runs)
#   install.sh --remove  remove exactly what this adds, and restore the preset
set -e
HN=${HN_DIR:-"/c/Program Files (x86)/Steam/steamapps/common/Hello Neighbor"}
WIN64="$HN/HelloNeighbor/Binaries/Win64"
MOD="$WIN64/ue4ss/Mods/NeighborCraftMod"
SHADERS="$WIN64/reshade-shaders/Shaders"
PRESET="$WIN64/ReShadePreset.ini"
BUILD=${BUILD:-"$USERPROFILE/NeighborCraft-lab/build/hn"}
HERE=$(cd "$(dirname "$0")" && pwd)

[ -d "$WIN64/ue4ss" ] || { echo "UE4SS isn't installed in $WIN64"; exit 1; }
[ -d "$SHADERS" ] || { echo "ReShade (with its shaders) isn't installed in $WIN64"; exit 1; }

if [ "$1" = "--remove" ]; then
	rm -rf "$MOD"
	rm -f "$SHADERS/NeighborCraft.fx"
	[ -f "$PRESET.neighborcraft-bak" ] && mv -f "$PRESET.neighborcraft-bak" "$PRESET"
	echo "removed"
	exit 0
fi

mkdir -p "$MOD/dlls"
cp "$BUILD/NeighborCraftMod/NeighborCraftMod.dll" "$MOD/dlls/main.dll"
# how HN's objects sit in the hand (edit the installed copy to tune; reinstalling resets it)
cp "$HERE/NeighborCraft_grips.txt" "$MOD/dlls/NeighborCraft_grips.txt"
touch "$MOD/enabled.txt"
# how the mod starts Minecraft when Hello Neighbor starts (and Minecraft isn't running yet)
echo "cmd.exe /c \"$(cygpath -w "$HERE/../tools/start_minecraft.bat")\"" > "$MOD/minecraft_launch.txt"
cp "$HERE/shaders/NeighborCraft.fx" "$SHADERS/NeighborCraft.fx"
if [ -f "$PRESET" ] && ! grep -q '^Techniques=NeighborCraft@NeighborCraft.fx' "$PRESET"; then
	[ -f "$PRESET.neighborcraft-bak" ] || cp "$PRESET" "$PRESET.neighborcraft-bak"
	sed -i 's/^Techniques=.*/Techniques=NeighborCraft@NeighborCraft.fx/' "$PRESET"
fi
echo "installed:"; ls -la "$MOD/dlls" "$SHADERS/NeighborCraft.fx"; grep '^Techniques' "$PRESET"
