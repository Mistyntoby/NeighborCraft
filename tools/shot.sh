#!/bin/bash
# Screenshot of the Hello Neighbor window (Windows Graphics Capture through ffmpeg's gfxcapture), for checking the
# composite without touching the game: tools/shot.sh [name]  ->  %USERPROFILE%/NeighborCraft-lab/shots/<name>.png
set -e
OUT="${SHOTS:-$USERPROFILE/NeighborCraft-lab/shots}"
mkdir -p "$OUT"
NAME=${1:-shot-$(date +%H%M%S)}
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "gfxcapture=window_exe=HelloNeighbor-Win64-Shipping\.exe:capture_cursor=0:max_framerate=30,hwdownload,format=bgra" \
	-frames:v 1 "$OUT/$NAME.png"
echo "$OUT/$NAME.png"
