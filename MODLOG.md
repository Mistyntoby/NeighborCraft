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

## Interactions, physics, terrain, stability (2026-10-10)

Evidence-driven round: collision recorder sessions, HN/Minecraft logs, minidumps, and an automated drive test
(`tools/drivetest.py`, Minecraft without HN; refuses to run while HN is open).

### Fixed (verified)
- Inventory left-click: Minecraft 26 numbers mouse buttons 1 left / 2 middle / 3 right (SDL); the link's 0/1/2 went
  through unmapped (left did nothing, right acted as left). Mapped, with Minecraft's double-click.
- Right click on HN furniture: cabinets, wardrobes, fridges, TV stands are FlatActors; their blueprints answer
  `OnIsActionComponent(part)`. The right-click ray asks that, then presses HN's action key (E). Cabinets open
  (confirmed in game). The ray is Minecraft's (eyes + look) and no longer skips loose physics objects (the TV was skipped).
- Shutdown crash (0xc0000409 in the CRT's DLL shutdown at every HN close): the link's std::thread was still joinable
  when the DLL's globals went; detached in WsClient's destructor (no crash dump since).
- Freeze: the game thread waited forever inside UE4SS's `Output::send` (watchdog dump: game thread in Output.hpp
  under our log(), ~20 engine workers parked in UE4SS). Logging now goes through our own queue and thread. A watchdog
  writes `NeighborCraft_freeze_*.dmp` if the game thread stops for 5 s while driving.
- Cutscenes: `Human::IsPlayCutScene` gates gameplay (Minecraft's HUD/kit/driving waited for nothing before).
- Gentle slopes (0.8 cm per 1/8 column after a curb): Minecraft's step-up takes the lowest helping step, one column
  per tick; a lift onto the highest <=1/16 ledge in the path, with that tick moving horizontally only. Drive test:
  2.2-2.9 -> 5.66 m/s on the ramp. Stairs keep their speed when a step-up only grazes the next riser.
- Falling through Act 1's road: HN had measured the road (rescue diagnostics); Minecraft's server was seconds behind
  ("Can't keep up! ... 12590ms") and the client didn't have the ground yet. Causes and fixes:
  - shape building (Shapes.or per box, ~14.5 ms a block) ran on the server thread: now built on its own thread, as a
    voxel bitset + pairwise unoptimized joins (~0.3 ms a block; identical shapes, checked on 300);
  - Steve holds still while HN's ground under him is unplaced on the server or not yet on the client (logged; 5 s
    failsafe); view distance 16 -> 8;
  - floor columns remember the heights they were probed for (a road falling 12 blocks left the lower part unprobed).

### Changed, not yet confirmed in game
- HN's player capsule is Steve's hitbox while Minecraft drives (51 cm radius vs 30 pushed physics doors open).
- Moving parts never become collision inside Steve (door leaves / falling objects forced the crawl pose).
- Pushes by walking: from the movement keys (Steve is stopped at an object, his speed was 0), impact + capped force.
- Punches from Minecraft's own attack hit (startAttack on a host block): velocity change on HN's mass scale,
  weapon damage, charge, sprint; hinged parts (doors, drawers) excluded.
- Collision recorder always on (F12 adds the debug view and the interaction target readout).

### Gotchas
10. Minecraft 26 mouse buttons are SDL's (1, 2, 3), not GLFW's (0, 1, 2).
11. HN masses are 150-400 kg for small props: impulses must be velocity changes.
12. UE4SS logging from the game thread can deadlock: log from another thread.
13. A test Minecraft on the link port talks to the player's game: never run tools while HN is open.

## Crawl fix, door remnants, carrying HN objects (2026-10-10, untested at stop)
- Forced crawl at doors: a host block without a shape was a full cube; the shared shape map drops a shape before the
  client hears the block is gone (recorder: a whole block in Steve's standing box the tick before every crawl).
  No shape = empty; full blocks are stored explicitly.
- Door remnants: the static probe no longer records moving parts (box cells, floor/ceiling ray hits inside them are
  skipped) and blocks whose moving collision changed are re-probed.
- Carrying (built, NOT tested): hold right click 0.35 s on a loose object (not doors/drawers, not HN story items with
  an InteractiveComponent) -> the object is hidden in HN (no collision/physics) and Minecraft gets an `hn_object`
  token (Carry.java; confirmed both ways, inventory full = object stays). Selected in the hotbar, HN shows the real
  object at the hand position; right click places it where Steve looks (overlap-checked), Q throws it; a token
  dropped from the inventory screen is thrown instead. Generic token icon (ours); per-item icons not done.
- Stop point: weekly usage limit reached. Next: test the carry lifecycle and crawl fix in game.

## Downhill stops, slope smoothing, crawl under opening furniture (2026-10-10, evening)
Evidence: the 20:03 play session (first run of `1a830a4`): `mc-client.log`, recorder `session-20261010-200332`.
- Stops running/jumping down Act 1's road: each one (13 on the way down) was the "waiting for HN's world" hold, the
  ground ahead arriving one tick late (47-61 ms). The hold zeroed Steve's velocity, so a sprint or sprint-jump died
  every time. Now a wait under 250 ms keeps his horizontal speed and ground state and only forbids moving down (no
  fall-through); longer waits (level start) still hold him still. Drive test (`--downhill --stream 1.6 --jump`,
  ground streamed just in time as HN does): old 4.0 m/s, 27 speed dips, 3 velocity resets -> new 6.6-6.8 m/s, 0 dips,
  0 resets, with 9 waits logged.
- Slope micro-steps: walking down, Steve left the ground at every 1/8 step and fell onto the next (recorder: airborne
  ticks with falls up to 0.5 the whole way down). Step-down: walking (not jumping) off ground onto ground at most 0.6
  below puts him on it (Minecraft's step-up the other way). Downhill sprint 4.87 -> 5.6 m/s (= flat).
- Camera: the eyes still move in 1/8 stairs (still for frames, then a step in one tick). The camera's height tracks
  them with a critically damped second-order filter (15 rad/s on the ground, 60 in the air; no lag on a steady slope;
  an exponential follow first tried floated 10-25 cm above the eyes). Downhill, worst height jolt per 1/60 s
  19.8 cm (eyes) -> 7.2 cm (camera, view bob included), mean 2.3 -> 1.4 cm.
- Forced crawl, again (ticks 1839-1857): not the shape race fixed before. Right after a right click on
  `BP_Kitchen_stand5`, its opening part was voxelised 2.6 cm into Steve's standing box (he had moved since the feet HN
  knew); Minecraft forced the crawl, and HN's clearance then used the crawling hitbox (0.6), letting the part come down
  to his waist. The clearance is now his standing body (>= 1.8) + 5 cm up and one sub-cell (12.5 cm) around.
- Drive test: `--downhill [--stream D] [--jump]`, a 28-block 1-in-4 slope of 1/8 steps with a flat runout; Minecraft's
  frame times (`tn`); `DRIVETEST_DUMP=file` writes every position.

### Gotchas
14. A drive-test run that ends in the void leaves the next run's Steve falling: keep courses closed (runout).
15. PassthroughClient forces view bob on; the `cam` height in `mcpos` includes it.
16. The HN-launched Minecraft stays up after HN quits to the menu; it exits when HN's process is gone.

## Second play session (20:30): jump landings, door remnant, leeway, carry placement (2026-10-10, night)
Evidence: `mc-client.log`, `UE4SS.log` and recorder `session-20261010-203047` of the 20:30 session.
- "Something stops me jumping down the slope": the recorder shows clean jumps (speed kept, no collisions, ticks
  287-344), so it was the camera. Landing, the eyes stop falling (~9 blocks/s) within a tick, and the height filter
  carried that speed into its ground mode: ~20 cm below the eyes and back after every landing. Landing now drops the
  filter's speed. Drive test `--downhill --jump`: lowest camera - eyes -21 -> -8 cm, worst jolt 32 -> 21 cm, mean
  4.8 -> 3.2 cm. (Ground waits, 11 that session, no longer stopped anyone: 0 velocity resets on the slope.)
- Door remnant after opening: a part becomes "moving" only once it has moved, so the closed leaf had been probed into
  the static world, and stayed there until the doorway was probed again (budgeted). On its first move, its sub-cells at
  rest (`voxelise_at` with the first transform) leave the static box bits and floor heights at once, and are re-probed.
- Leeway ("collision too strict"): moving parts no longer grow by half a sub-cell all round (only parts thinner than a
  sub-cell are widened to one, so door leaves have no gaps); a static 1/8 sub-cell is solid only when HN's geometry
  reaches its middle 6.5 cm (was: touches it anywhere), so walls and furniture stick out up to ~9 cm, not 12.5.
- Carry placing/throwing always "no room": the room check was `BoxOverlapComponents` by object types, which returns
  overlap-only trigger volumes (HN levels are full of them). Now a "Pawn"-profile box trace (what blocks a player),
  ignoring Steve's capsule and the object; a refusal logs what was in the way.
- Carried object not seen in the hand: cause unknown. Logged now: `carry: in the hand: ...` (camera, position,
  bounds, scale, hidden flag) the first time, or `carry: holding ... but ...` when HN can't show it.
- Not run in game yet: all HN changes above (DLL md5 `b3ffa30e…` installed).
