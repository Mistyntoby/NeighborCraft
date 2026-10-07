# NeighborCraft MODLOG

Real Minecraft Java running next to Hello Neighbor 1 and composited into it (passthrough, mashup-mods pattern 2),
modelled on universal-modder `examples/minecraft-gta5-passthrough`.

## Step 1: recon (2026-10-06)

### Install
- Hello Neighbor: Steam app 521890, build 4340169, `C:\Program Files (x86)\Steam\steamapps\common\Hello Neighbor`.
- Exe: `HelloNeighbor\Binaries\Win64\HelloNeighbor-Win64-Shipping.exe` (53,029,376 bytes, no version resource).
- Content is shipped as **loose cooked files** (`HelloNeighbor\Content\*.uasset`, 7723 UFS files), with **no paks**.
  Packages are unversioned (`LegacyFileVersion -7`, ue4ver 0), so the version comes from the exe.
- Official Mod Kit plugin `HelloNeighborMod` is enabled; `HelloNeighbor\Mods\` holds 9 user-installed mod-kit mods
  (all `EngineVersion 4.20.0`). These must be disabled or isolated for lab runs.
- Also installed (do not confuse): Hello Neighbor Alpha 4 (1096710, `HelloNeighborReborn`), Alpha 2.
- Saves: `%LOCALAPPDATA%\HelloNeighbor\Saved\SaveGames` (gp, os, sf_0, slot1-3, ws `.sav`).
  Config: `%LOCALAPPDATA%\HelloNeighbor\Saved\Config\WindowsNoEditor` (2560x1080, windowed fullscreen, all Epic).
  Shipping build writes no log.

### 1. Engine version / RHI
- **UE 4.20.2**: exe strings `4.20.2`, `++UE4+Release-4.20-CL-0`, source paths `c:\Work\.git\UE4_20\...`
  (tinyBuild's own engine build, CL-0).
- **D3D11.** D3D12RHI, VulkanRHI and OpenGLDrv are compiled into the exe, but only `GlobalShaderCache-PCD3D_SM5.bin`
  and `-PCD3D_SM4.bin` ship and no `TargetedRHIs`/`DefaultGraphicsRHI` override exists, so the D3D11 SM5 path is
  the one that runs. `-dx12` would have no cooked shaders. To be confirmed live (ReShade reports the API).

### 4. Navigation (static evidence)
- `DefaultEngine.ini`: `RecastNavMesh`, `RuntimeGeneration=DynamicModifiersOnly`, `bForceRebuildOnLoad=True`.
  Agents: `Default` (r=34, h=176) and `SuperMarket` (r=135).
- **Not fully dynamic.** With DynamicModifiersOnly, tiles rebuild only around nav modifiers
  (`UNavModifierComponent`, `ANavModifierVolume`, components with `bDynamicObstacle`), not around newly spawned
  collision geometry. Consequences:
  - Placed blocks: spawn collision actors with `bDynamicObstacle=true` + `NavArea_Null` (or a NavModifierComponent),
    so the Neighbor paths around them.
  - Broken walls: hiding a wall mesh opens **no** new walkable navmesh. Needs either a runtime switch of the level's
    RecastNavMesh to `Dynamic` plus rebuild, or pre-baked nav under walls.
  - The level's RecastNavMesh actor may override the ini default; read it live with UE4SS.
  - Still unverified: whether the Neighbor's AI actually paths via the navmesh (vs. custom movement).

### Pending (needs game launches)
- 2. UE4SS injection + console.
- 3. ReShade depth buffer (expected reversed-Z: UE4 on D3D11 uses inverted depth).

### Lab / backups
- Backup 1: `%USERPROFILE%\NeighborCraft-lab\backups\20261006-220736` (outside the repo): `Saved\` (saves + user
  config), game `Binaries\` and `Config\`, SHA-256 of root files and binaries, list of installed Mods.
- Restore: copy `Saved\` back to `%LOCALAPPDATA%\HelloNeighbor\`; delete any added loader files from
  `Binaries\Win64` and copy `Binaries\` back; or Steam "Verify integrity" for game files.
- Normal Minecraft profile is untouched; the mod will use its own Fabric profile + game dir.
