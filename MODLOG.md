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

### 3. ReShade depth: usable, reversed-Z (2026-10-07)
- DisplayDepth with `RESHADE_DEPTH_INPUT_IS_REVERSED=1`, far plane 1000: near dark -> far white, sky white,
  lines up with the picture (user confirmed). HN set to windowed 1920x1080 (opens borderless).
- UE4: infinite reversed-Z, near plane `NearClipPlane=10` (0.1 m) -> linear depth = 0.1 / d.

### 4b. Navigation, more evidence
- Content has `Sosed/BP_NeighborNavLinkProxy` and `BP_Sosed` (the Neighbor), `Player/BP_Human` (player):
  nav link proxies only matter to navmesh pathing, so the Neighbor almost certainly paths on the navmesh.
- Exe has native names `StunTime`, `ENeighborState`, `CatchPlayer`, `NeighborSpeed`, `m_pNeighbor`.
- Live answer pending: the F9 dump prints every RecastNavMesh's `RuntimeGeneration`.

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

## Build (2026-10-07)
### Toolchain (all outside the repo, under `%USERPROFILE%\NeighborCraft-lab\`)
- JDK 25 (Temurin 25.0.4.1) portable: `deps\jdk25`.
- VS 2022 17.14 + "Desktop development with C++" (MSVC 14.44.35207, SDK 10.0.26100, CMake, Ninja), Rust stable.
- RE-UE4SS at `6eb3d9bc` (= the installed experimental build) in `deps\RE-UE4SS`. Its submodules need HTTPS
  URLs (`git config submodule.<path>.url https://...`) and an Epic-linked GitHub account for UEPseudo.
- ReShade 6.8.0 add-on headers + ReShade.fxh: `hn/fetch_deps.sh` -> `deps\reshade`.
- Python venv for the tools: `pyenv` (websockets, numpy, pillow).

### Minecraft side (`mc/`): working
- Ported from the GTA example, GTA-only systems removed. Port 25620, shm `Local\NeighborCraftFrame`, world
  `neighborcraft` (void, creative). `./gradlew build` / `runClient` with JAVA_HOME=jdk25 (dev client, own game dir).
- `tools/linktest.py` verified: camera drives Minecraft, barriers and commands apply, `blocks` events come back,
  colour + depth exported (1280x720, flags 7), sky transparent.

### Hello Neighbor side (`hn/`): built, first in-game run pending
- `NeighborCraftMod` (UE4SS C++ mod that also registers as a ReShade add-on): engine-tick post callback (game
  thread) sends `cam` (UE->MC axes, horizontal->vertical FOV) and floor probes via
  `KismetSystemLibrary:LineTraceSingle` (ProcessEvent with a parameter block laid out from the UFunction's own
  property offsets). Keys: F6 hands, F7 on/off, F8 re-level, F9 reflection dump.
- `hn/build.bat` -> `build\hn`; `hn/install.sh` (+ `--remove`) installs `ue4ss\Mods\NeighborCraftMod\dlls\main.dll`
  and `reshade-shaders\Shaders\NeighborCraft.fx`, and sets the ReShade preset to the NeighborCraft technique
  (old preset kept as `ReShadePreset.ini.neighborcraft-bak`).

## Integration (2026-10-07 .. 10-09)
### Architecture now
- Minecraft drives the player (default; F10 toggles HN-collision mode with Minecraft speeds). HN's pawn is a puppet
  placed at Steve's eyes (MOVE_None). Same split as the SkyCraft (Skyrim) mashup.
- HN's world in Minecraft: invisible `neighborcraft:host` blocks with per-position VoxelShapes. HN side probes with the
  **Pawn collision profile** (what really blocks a player): `BoxTraceSingleByProfile` octree down to 1/8 block for
  walls/furniture, plus 4x4 `LineTraceSingleByProfile` rays per column for floors (every storey). Message `shape`.
- Minecraft blocks -> HN: one TriggerBox per block, switched to BlockAll 1 m box + `bDynamicObstacle` (navmesh).
- Compositing runs at HN's second back-buffer draw (start of UI) via `render_effects`, D3D11 state saved/restored.
- Handshake: Minecraft sends `ready` after its world loaded; HN sends world/teleports only after it.
- Minecraft pauses (PauseScreen) while HN is in menus/paused (`BPW_MenuPause_C` in viewport; engine pause unused).
- Minecraft is launched by HN (`tools/start_minecraft.bat`), hidden off-screen, quits 20 s after HN; fresh world each run.

### HN gotchas found
1. UE4SS class lookups (FindFirstOf/FindAllOf via hash tables) return unrelated objects in HN's custom 4.20 build:
   set `bForceGUObjectArrayForIteration = true` and verify class chains by name.
2. FUObjectItem slot/flag checks fail for live objects: never cache UObject* across frames; read
   GameEngine -> GameInstance -> LocalPlayers[0] -> PlayerController each tick.
3. Object-type traces hit HN's invisible trigger volumes (phantom blocks): use the Pawn profile.
4. Box overlaps don't see under a landscape heightfield (only its surface): floors by rays.
5. Unreal roll is clockwise-positive, Minecraft's camera roll counter-clockwise: negate.
6. ReShade add-on registered after runtime creation never gets `init_effect_runtime`: take the runtime from per-frame events.
7. `near` is a Windows macro.
8. HN player: native class `Human` (BP_Human_C) with `m_fWalkSpeed`/`m_fRunSpeed`; Neighbor: `Sosed` (BP_Sosed_C)
   with OnStruck/OnHit/SetFreeze/SetState, controlled by SosedAIController (path following, navmesh).
   Navmeshes `Default` and `SuperMarket` are RuntimeGeneration=DynamicModifiersOnly (verified live).
9. Crash dumps: `tools/crashinfo.py` (faulting module + game/UE4SS/mod frames on the stack).

### Open
- Verify in game: floors/standing, pause covering, no crash (latest build untested).
- Rendering: transparency cause, relighting/shadows, GPU texture sharing, native-geometry evaluation.
- Neighbor combat (hooks identified), wall breaking.
