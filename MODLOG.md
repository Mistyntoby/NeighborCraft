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

### 1b. RHI confirmed live (2026-10-07)
- ReShade.log: game calls `D3D11CreateDeviceAndSwapChain` / `IDXGIFactory::CreateSwapChain` -> **D3D11**.
  Back buffer is not RGBA8 (sRGB-sampling warnings), exclusive fullscreen via `SetFullscreenState(TRUE)`.

### 2. UE4SS (2026-10-07): injects
- PS scan found EngineVersion 4.20, Shipping, GUObjectArray, GMalloc, FName::ToString/ctor,
  StaticConstructObject_Internal, FUObjectHashTables::Get, GNatives, GameEngineTick, ProcessConsoleExec.
  HashTables self test passed (1950 classes). CheatManagerEnabler: CheatManager already exists.
- **Failed: ConsoleManagerSingleton** (2 candidates: 7FF7288DE6B0, 7FF729151470). Only matters for CVars via
  IConsoleManager; fix with `ue4ss/UE4SS_Signatures/ConsoleManager.lua` if we need it.
- In-game `~` console: pending user confirmation.

### 3. ReShade depth (in progress)
- With `RESHADE_DEPTH_INPUT_IS_REVERSED=0` DisplayDepth was almost all black (consistent with reversed-Z).
  Set REVERSED=1, far plane 1000 in ReShade.ini + preset; awaiting visual check and Generic Depth buffer info.

### Lab / backups
- Backup 1: `%USERPROFILE%\NeighborCraft-lab\backups\20261006-220736` (outside the repo): `Saved\` (saves + user
  config), game `Binaries\` and `Config\`, SHA-256 of root files and binaries, list of installed Mods.
- Restore: copy `Saved\` back to `%LOCALAPPDATA%\HelloNeighbor\`; delete any added loader files from
  `Binaries\Win64` and copy `Binaries\` back; or Steam "Verify integrity" for game files.
- Normal Minecraft profile is untouched; the mod will use its own Fabric profile + game dir.

### Lab setup (2026-10-06)
- Installed **UE4SS experimental-latest** (`UE4SS_v3.0.1-1161-g6eb3d9bc.zip`, asset dated 2026-10-06) into
  `HelloNeighbor\Binaries\Win64\`: `dwmapi.dll` + `ue4ss\`. Settings: `ConsoleEnabled/GuiConsoleEnabled/
  GuiConsoleVisible = 1`, `GraphicsAPI = opengl` (kept off D3D11 so it never touches the swapchain ReShade hooks).
- Downloaded **ReShade 6.8.0 Addon** installer to `%USERPROFILE%\NeighborCraft-lab\downloads\`; the user runs it
  (target `HelloNeighbor-Win64-Shipping.exe`, DirectX 10/11/12, standard effects).
- Moved all 10 Mod Kit mods (Bomb, FoxyReignited, FredbearRunaway, GM, HelloGuest, HelloMultiplayer,
  Hello_Neighbor_Alpha_1_Remake, Into the Mind, roblox, vgnUltimateMod) to
  `%USERPROFILE%\NeighborCraft-lab\hn-mods-disabled\`. Move back to `HelloNeighbor\Mods\` to restore.
- Uninstall: delete `dwmapi.dll`, `ue4ss\` (and later `dxgi.dll`, `ReShade.ini`, `reshade-shaders\`) from `Win64`.
