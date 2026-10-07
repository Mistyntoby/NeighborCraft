#!/bin/bash
# Fetches what the Hello Neighbor side builds against but may not be redistributed, into
# %USERPROFILE%/NeighborCraft-lab/deps (outside the repo):
#   reshade/   ReShade's add-on API headers (crosire/reshade at v6.8.0, the version installed in the game)
#   ReShade.fxh, ReShadeUI.fxh   which NeighborCraft.fx includes (crosire/reshade-shaders, slim branch)
# RE-UE4SS itself is cloned separately at the commit of the installed UE4SS build (see MODLOG.md).
set -e
DEPS=${DEPS:-$USERPROFILE/NeighborCraft-lab/deps}
RESHADE=6.8.0
mkdir -p "$DEPS/reshade"
for f in reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp; do
	curl -fsSL "https://raw.githubusercontent.com/crosire/reshade/v$RESHADE/include/$f" -o "$DEPS/reshade/$f"
done
for f in ReShade.fxh ReShadeUI.fxh; do
	curl -fsSL "https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/$f" -o "$DEPS/$f"
done
ls "$DEPS/reshade" "$DEPS"/*.fxh
