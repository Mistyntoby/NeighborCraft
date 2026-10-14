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

## Empty-hand pickup, carrying arm, crawl at doors again (2026-10-10, late)
Evidence: the 20:44 session (`UE4SS.log`, recorder `session-20261010-204419`), `tools/handtest.py` runs.
- Forced crawl at Door_inward_2092 (4 times at 20:44-20:46, 45 forced ticks): boxes inside Steve's standing box at the
  door plane the tick before each. Cause: the clearance keeps moving collision out of Steve by removing those cells
  from `g_dynBits`, and the static probe masked moving parts with that same map; a swinging leaf (its blocks are
  re-probed every tick it moves) was found in exactly the cleared cells and baked into the static world there.
  Now the probe and the floor rays mask with `g_dynMask`: every moving part's cells, never cleared around Steve, half
  a sub-cell wider (a static sub-cell counts geometry in its middle 6.5 cm, a moving one its centre only).
- Pickup needs an empty hand: Minecraft reports `held` with `empty`; HN starts a pickup only then (a Minecraft item in
  the hand is used instead), and `Carry.give` refuses unless the selected slot is empty ("hand not empty", HN shows
  "Empty your hand first"); the token goes into the selected slot. On success only: the server swings the hand and
  plays ITEM_PICKUP, once (`handtest`: 4 cases as expected, one "into the hand" log line per pickup).
- Not carried: anything HN's OnIsActionComponent says yes to, and TV/Switch/Tumbler/Lamp classes (Act 1's TV is a
  loose physics object).
- Carrying arm: Minecraft drew no arm while the token (empty model) was in the hand. `FirstPersonCarryMixin` draws
  the player's own arm (renderPlayerArm: skin, swing, equip motion) in a carrying pose (raised 0.16, 0.10 inward),
  and `CarryHand` sends the fist every frame (`{"t":"hand"}`): the arm cube's end (-1, 10, 0) px after the arm part
  transform (pivot (-5, 2, 0), zRot 0.1). Measured: the hand space looks along +z with +x to the left; the fist goes
  through the hand projection (hudFov) to a screen point, then out of the world camera (FOV without the overscan)
  at the distance where sizes match. `handtest`: fist 0.66 right, 0.28 down, 1.15 ahead of the camera = screen
  (0.46, -0.35), where the hand layer screenshot shows it. HN rests the object's visual bounds on that point along the
  camera's up (85% of its half height above), scaled to at most 18% of the fist distance in half size.
- `tools/handtest.py`: the pickup rules over the link, and hand-layer screenshots (`shot` with `layer: hand`; the
  hand is cleared from Minecraft's frame once captured, so ordinary screenshots never show it).

### Gotchas
17. FrameExporter clears the hand from Minecraft's frame after copying it: screenshots need the hand layer.
18. Minecraft 26.3 names: `GameRenderer.mainCamera()`, `GameRenderer.mainRenderTarget()`,
    `FirstPersonHandsAndItemsRenderer` (was ItemInHandRenderer).

## Pickup regression, mid-air stops jumping downhill, crawl invariant (2026-10-10, night 2)
Evidence: the 21:30 session (`UE4SS.log`, `mc-client.log`, recorder `session-20261010-213013`), drive tests.
- Pickup regression (mine, previous round): pickup refused anything HN's OnIsActionComponent says yes to, to keep
  TVs and switches out; HN says yes for its loose props too (BP_Video_Recorder, BP_Dining_chair: there "HN's action"
  is HN's own grab), so every right click on them went on to the injected E = HN's native grab (log: "HN's action on
  BP_Video_Recorder_apartments_55 (its OnIsActionComponent)"). HN's own bindings: E for pickup/action, right mouse
  only for put-down/throw: nothing native intercepts the right click. Fix: only TV/Switch/Tumbler/Lamp classes are
  excluded; a carryable object under the crosshair is always ours (with something in the hand: "Empty your hand
  first", never HN's grab). Starts and cancels (release, target lost/out of reach, another object) are logged.
- Mid-air stops jumping down the road: recorder ticks 1846, 1862, 1895, 1942: in the air, falling 0.15-0.45 a tick,
  then 0 with the fall speed zeroed, each at a "waiting for HN's world" (21:31:55-21:32:00, ~50 ms). The short-wait
  rule (first round) forbade every move down; the missing block was a block below him, out of that tick's reach.
  Now only a move into a missing block is cut (to its top; from inside its level, not down), the speed is kept, and
  the check reaches down as far as the tick's fall. New recorder event MIDAIR_STOP (falling in the air, the fall cut
  to under half without a vertical collision). Drive test `--downhill --stream D --jump` (3 runs each): 13 mid-air
  stops before, 0 on the slope after (the only ones left: right after the test's teleports onto ground still
  missing, where holding him is the point); 6.8-7.0 m/s, 0 speed dips, no falls; walking 5.61; ramp/stairs course
  5.92/5.48.
- Forced crawl, 9 episodes in the 21:30 session (door leaf 0.17-0.4 m above his feet at tick 2441, cells at his
  waist pushing chairs at 1337; 3 one-tick ones the recorder couldn't show: it collected boxes only up to 0.4 above
  his crawling box). Invariant when blocks are sent: no collision cell, floor or ceiling inside Steve's current
  hitbox from 0.15 above his feet (3 cm around). Fixed geometry can't be there, so whatever is moved into him; his
  current size keeps a legit low ceiling (it is above his crouched box). Logged as "collision: N pieces inside
  Steve kept out of block ...". The recorder now collects boxes up to standing height in any pose.

### Gotchas
19. HN's OnIsActionComponent says yes for loose props: it is not a "fixed interactable" test.
20. Drive/hand tests teleport, then send the floor: a tick in between can drop Steve into a block-to-be (stuck).

## Pickup for every loose prop, held-object attachment, item icons, Ctrl+Q, near-eye held object (2026-10-10, night 3)
Evidence: the 21:58 session (`UE4SS.log`: 7 pickups through our path, every "in the hand" line "fist not reported";
recorder `session-20261010-215810`: one MIDAIR_STOP), `tools/itemtest.py`, `tools/handtest.py`, drive tests.
- Pickup eligibility: the hit component had to be the actor's root *and* simulate physics; a prop hit on another
  part (a child mesh, a second collider) was refused silently and went on to HN's action = HN's grab. Now: any part of
  an actor whose root simulates physics; refusals of physics objects are logged with their reason
  (`carry: what the look meets isn't carried: ...`). Kept out: doors/drawers, HN's own items (InteractiveComponent:
  keys etc. stay HN's pickup, the story needs them), TV/Switch/Tumbler/Lamp classes.
- Held object: it was placed in world space with the look's yaw only, before HN's camera moved: upright while the hand
  pitched (sliding up and down against it), a frame late. Now `carry_hand_follow` runs right after HN's camera is set,
  from that pose: the fist in the camera's frame (Minecraft sends it, `"l"`: right/up/forward), the object turned with
  the camera's yaw/pitch/roll plus a grip (`NeighborCraft_grips.txt` next to the DLL, installed from
  `hn/NeighborCraft_grips.txt`: per class offsets and turn), its grip point (its mesh's bottom middle) on the fist.
  Rotator/quaternion code checked numerically (round trip exact, forward/up axes as Unreal's).
- Near-eye placement (render pipeline, depth): at the fist's distance (~1.35 m) the object went into walls in front of
  Steve. Fist, grip and size are scaled towards the camera to 25 cm (inside Steve's 30 cm radius, HN's near plane
  ~10 cm): the same picture from the eye, never inside the world. Its shadow is off while held.
- Icons (`CarryIcons`, `GuiItemIconMixin` on `GuiGraphicsExtractor.item(...)`, the one path for hotbar, inventory and
  the cursor stack): per class, a picture from HN when it has one, else a fallback (the class's hue from a mixed hash,
  crate pattern, the name's first letter). HN makes the picture at the first pickup of a class (`capture_icon`:
  SceneCapture2D, show-only the object, SceneColorHDR, 64 px, `ExportRenderTarget` into `dlls/icons/`, then
  `{"t":"icon"}`); Minecraft reads Radiance (RLE or flat) or PNG. itemtest: an RLE .hdr replaces its class's fallback
  in the hotbar (screenshot), empty slots stay empty, consumed tokens leave no icon. **The HN capture itself has not
  run yet.**
- Ctrl+Q: Minecraft's drop asks its own window for Ctrl (never focused: always "no"). HN sends `"stack"` with Q;
  Minecraft calls `gameMode.dropItem(player, stack)` (never with a screen open, never from an empty slot). itemtest:
  Q 64 -> 63, Ctrl+Q -> empty, empty slot: nothing, planks untouched, both drops picked up again: 64. A held HN object:
  Q and Ctrl+Q throw it (one per slot).
- Landing clamp: tick 353 of the 21:58 session: falling 0.445, stopped at y 148.0, the top of the block below that the
  client didn't have yet, while its real (slope) surface was lower: a hang before the drop. When the server has placed
  the shape already (`HostBlock.placedShape`), the clamp uses its real surface under Steve. Drive tests: 0 mid-air
  stops on slopes (the only ones: test teleports onto ground not yet there).
- SkyCraft: the only reference in the repo is MODLOG's "Same split as the SkyCraft (Skyrim) mashup"; no URL in the
  repo, the lab or memory. Not consulted; nothing adapted from it.

## Held object on the camera, the radio, icons for real, see-through cutout blocks (2026-10-10, night 4)
Evidence: the 22:20 session (`UE4SS.log`, `mc-client.log`), the HN icon exports in `dlls/icons/`, `tools/rendertest.py`
(new), `tools/itemtest.py`, `tools/handtest.py`, drive tests, the SkyCraft source (below).
- Held object, broken on one half of the turn (the user: turning east it ended up far away). The world-space chain
  (camera pose -> fist in the camera's frame -> grip -> rotator) was checked numerically over every yaw (-360..720 in 5
  degree steps), pitch -60..60 and three grips: the rotation used for the grip and the one given to Unreal agree to
  1e-15, and nothing in it depends on the yaw; the cause was not found in our math. Instead of patching numbers, the
  world-space step is gone: the object is attached to HN's view camera (`K2_AttachToActor`, KeepRelative) and placed
  by a relative transform only (`K2_SetRelativeLocationAndRotation` on its root: fist + grip, the grip's own turn),
  so the engine's hierarchy composes it with the camera in the same update. Detached (KeepWorld) when put down,
  thrown or swapped out. On every pickup, `carry_sweep` turns the view camera through 360 degrees of yaw (every 15,
  at pitch -60/0/60) within one tick, reads the object back in the camera's frame at each pose and logs the largest
  change (`carry: 360 check of ...: continuous|DISCONTINUOUS`); while held, `carry: hand ok|FAR ...` once a second
  (at once if it is ever more than 1 m from the camera). **Not run in game yet: the 360 check is the evidence to read.**
- Radio: refused as "one of HN's own items (InteractiveComponent)" (22:22:21), so HN's grab took it. HN puts the
  InteractiveComponent on its grabbable props too. Now a loose prop with it is ours unless its class chain names an
  Item, Pickup or Key (HN's inventory items); every such class's chain is logged once (`carry: class chain of ...`)
  to check the rule against the real classes.
- Icons: HN's capture worked in game (vase, ottoman, video recorder: `dlls/icons/*.hdr`), but (1) `narrow()` turned
  every backslash of the path into `_` before my slash swap, so Minecraft found no file; (2) this UE build's
  `ExportRenderTarget` writes PNG (in a .hdr name) with alpha = 1 - coverage. Path: slashes swapped on the wide string
  before `narrow()`. Minecraft: loader detects PNG/Radiance by content and turns alpha round when the border is opaque.
  The three real exports now show in the hotbar (screenshot `items-hotbar-hn.png`).
- See-through blocks. `tools/rendertest.py` reads the world layer from the shared memory (the compositor's own input)
  and measures alpha where Minecraft has geometry. Composite: world.rgb + host * (1 - world.a). Before (same camera,
  no block targeted): solid 0.0% under alpha 250, **cutout 16.2%** (leaves, iron bars, trapdoors, cobwebs: Minecraft's
  cutout pass alpha-tests and does not blend, but writes the texture's mip-filtered alpha; never visible in Minecraft,
  see-through and over-bright in the composite), translucent 98.8% (right). Fix (`ShaderSourceMixin`): Minecraft's own
  `core/terrain.fsh` and `core/block.fsh` are patched as they are read (no Mojang file shipped): alpha 1 after the
  final colour for the solid passes (no ALPHA_CUTOUT) and for what the cutout passes keep (ALPHA_CUTOUT 0.5);
  translucent (ALPHA_CUTOUT 0.1, blended) and OIT paths untouched. After: cutout 3.2%, all of it the glass block,
  which 26.3 draws in the translucent pass (genuinely translucent). The 90 low pixels of the first solid run were the
  targeted block's outline (translucent lines); the test now looks over the wall.
- SkyCraft (https://github.com/chasmlol/SkyCraft, MIT, commit bfcaf17), files read: `README.md` (overview),
  `docs/DESIGN.md` (sec. 9 Rendering: world/hand/GUI layers, depth-tested world, GPU sharing plan),
  `protocol/skycraft_protocol.h` (render ring: RenSection/RenVertex with per-vertex flags `bit0 cutout (alpha test),
  bit1 translucent`, RenAtlas, RenAvatar, RenScene), `fabric/.../render/WorldExporter.java` (block meshes from
  Minecraft's block renderer, `quad.materialInfo().layer().translucent()` to the flag), `FrameExporter.java` (listed).
  Difference that matters: SkyCraft's final design draws Minecraft's blocks as meshes in Skyrim's own frame (occlusion
  and material class native to the host); NeighborCraft composites Minecraft's rendered picture by depth, so material
  class has to be carried by the picture's alpha, which is what the shader patch does. No SkyCraft code was copied.

## The fist's frame, TVs, the light behind blocks, the mesh path's first stage (2026-10-10, night 5)
Evidence: the 22:41 session (`UE4SS.log`: `carry: 360 check` x4 "continuous", 12 `carry: hand ok` lines),
`tools/turntest.py` (new), `tools/relight_check.py` (new), the SkyCraft source.
- Held object: the 360 check proved the attachment continuous (0.00 cm, 0.0000 turn over 72 poses), but the hand lines
  showed Minecraft's fist in the camera's frame anywhere around it at 1.35 m: (35, 81, -101), (20, -116, -66) ...
  instead of (115, 66, -28), and "default" whenever none came. Cause (mine, night 2): the pose Minecraft 26.3 gives
  the first-person arm is the camera's place with the **world's axes** (the camera's rotation is in the view matrix);
  CarryHand read it as camera space, which agrees only facing yaw 0, where it had been tested. turntest (three turns,
  pitch +-35, a teleport): before, the fist's angle = yaw + 24 degrees, none at all between yaw 54 and 258, 112.5
  degrees of swing; after (the fist projected on the camera's right/up/forward): 210 positions, at most 3.9 degrees
  from the middle direction (Minecraft's own hand sway at the start of a fast turn): STEADY. A first guess (the hand
  sway's yBob chasing a yaw jump) was tested, made no difference, and was reverted.
- TVs: `right click: BP_TV_apartments3 ... HN's action` (22:41:53): Act 1's TV is a loose physics prop, and HN's
  action on a loose prop is HN's grab. Rule now: a loose physics body is carried by NeighborCraft, or picked up by HN
  only if it is one of HN's inventory items (class chain names Item/Pickup/Key); anything else loose (TVs, lamps:
  `kNotCarriedNotGrabbed`) gets no HN action at all. Fixed interactables (doors, cabinets, switches) are not loose
  physics and keep HN's action.
- "Transparent or reflective" blocks and held items: the compositor lit every Minecraft pixel, the hand included, with
  Hello Neighbor's picture *at that pixel*, blurred (`PS_Light` copies the back buffer; `relight` sampled it at the
  pixel): that is what lies behind the block or hand, so their colour followed the background. `host_light_near`: the
  light from Hello Neighbor's surfaces near the Minecraft surface in depth (12 taps on a ~0.6 m disc, weighted by depth
  difference), else the picture's overall light; the hand takes the overall light only. relight_check (the shader's
  math in Python, synthetic scene: the same stone block on the same floor, before a bright window and before a dark
  wall): old 0.70 vs 0.30 (58% apart), new 0.57 vs 0.57 (0%). The shader compiles only inside the game (ReShade.log).
- Mesh path, first stage (`mesh_path_probe`, once per level, logs only): the shipping binary has
  ProceduralMeshComponent (CreateMeshSection_LinearColor, UpdateMeshSection), ImportFileAsTexture2D,
  CreateDynamicMaterialInstance, SetTextureParameterValue. The probe logs whether the class is loaded and which loaded
  materials take a texture under the usual parameter names, with their blend mode (masked = cutout) and shading
  model: the one thing a shipped game can't make and the mesh path needs.
- SkyCraft (chasmlol/SkyCraft bfcaf17, MIT) read this round: `skse/src/WorldRender.cpp` (PSMain; DrawMesh;
  RenderPasses; DrawInFrame; RenderWorldHook after Main::RenderWorld; the render ring's kRenAtlas / kRenSection /
  kRenAtlasRegion cases), on top of last round's README.md, docs/DESIGN.md, protocol/skycraft_protocol.h,
  fabric/.../render/WorldExporter.java. What carries over: (1) non-translucent texels written with alpha 1 and cutout
  discarded under 0.5 (PSMain: `if (!(i.flags & 2)) po.color.a = 1.0`) - done for our picture by ShaderSourceMixin;
  (2) lighting not taken from the screen behind the block (its comment: "Not Skyrim's screen-space shadow mask: that
  holds whatever Skyrim surface is behind the block") - done now in host_light_near; (3) blocks drawn in the host's
  own frame with its depth, normals and motion vectors (Skyrim's TAA and post see them as its own) - for HN the native
  route is ProceduralMeshComponent with an HN material, which the probe prepares. Nothing copied.

## Held size, props into HN's hotbar, shadows of Minecraft's blocks (2026-10-11)
Evidence: the 22:57-23:01 session (`UE4SS.log`: `mesh path:` lines, `right click: BP_TV_apartments3 / BP_Picture_set_36 /
BP_Trashcan9 ... HN's action`, no `class chain` lines; ReShade.log: NeighborCraft.fx compiled), tests below.
- Props into HN's own hotbar: the TV, BP_Picture_set_36 and BP_Trashcan9 went to HN's action (HN's take into its
  hotbar). None reached the class-chain or TV rules: they were refused first, silently, as "not a physics object",
  because HN keeps many props asleep (not simulating) until they are touched. pickup_target's order now: (1) part of a
  fixed object -> HN's action; (2) TV/lamp class -> nothing (no carry, no HN action); (3) HN's inventory item ->
  HN's pickup; (4) a simulating body -> ours; (5) a prop HN would take: OnIsActionComponent yes on its root, root
  movable, one primitive component, <= 70 cm half size -> ours (a sink or stove has parts or is bigger: HN's action);
  (6) else HN's action. After every HN action, `carry: HN took <actor> into its hands ...` (0.6 s later, with
  movable/parts/size) names any prop the rule still misses. **Not run in game yet.**
- Held size: objects were only ever shrunk to 18% of the fist's distance in half size, never grown: a mug stayed
  smaller than Minecraft's fist (12.5 cm in half at that distance). Now every object is brought to one visual size,
  19.3% of the fist's distance in half (26 cm at 1.35 m, about twice the fist), times the grip's new `scale` column
  (NeighborCraft_grips.txt), growing small things up to 3x. Scaled before its grip point is placed (it stays on the
  fist), only the copy in the hand (release() restores its scale; collision and mass untouched). Near the eye that is
  ~5 cm in half at 25 cm: its near side stays ~20 cm from the eye, well outside HN's ~10 cm near plane.
- Shadows of Minecraft's blocks in HN's lighting: each full opaque block Minecraft reports (`blocks` now has
  `opaque`: isSolidRender && isCollisionShapeFullBlock) gets a hidden stand-in in HN: a StaticMeshActor with
  Unreal's 1 m cube (/Engine/BasicShapes/Cube, shipped unpacked with HN; found loaded or through the asset registry:
  AssetRegistryHelpers.GetAssetRegistry -> GetAssetByObjectPath -> GetAsset), 98% size, movable, no collision,
  hidden with bCastHiddenShadow: HN's lights and shadow maps shadow it as their own, so direction, softness and
  intensity are HN's. Destroyed with the block or when it stops being opaque, and with the colliders on a level
  change. Held objects cast none (off while held); placed/thrown HN objects have their own. Not shadowed this way:
  glass, slabs, leaves, torches (not full opaque cubes), Minecraft entities and dropped items (only the compositor's
  screen-space contact shadow), and Minecraft blocks *receiving* HN's shadows (they are a picture: only relit).
  Minecraft side tested (`opaque` = stone and bricks; not glass, slab, leaves, torch); **HN side not run in game**.
- SkyCraft (chasmlol/SkyCraft, MIT) for shadows, read: skse/src/WorldRender.cpp DepthVS / ShadowPS (Minecraft's blocks
  drawn depth-only into Skyrim's sun shadow map, cutout texels discarded), SunShadowRenderHook (the vtable hook on
  Skyrim's sun shadow rendering), SkyrimSunShadow (blocks receiving Skyrim's cascades). Same goal, engine-native way
  here: Unreal puts any hidden-shadow-casting primitive into every light's shadow pass, no render hook needed.
- Tests: handtest OK, itemtest OK, turntest STEADY (3.1 deg max; one run had 0 positions because the test switched
  slot and picked up in the same instant, leaving the token in another slot: a test sequencing artefact).

## Interactions, HN's hotbar, mining, charged throw, shadows (2026-10-11, afternoon)
Evidence kinds: [src] source reading, [test] automated test on the final code, [game] seen in Hello Neighbor. Nothing
of this round is [game] yet.
- Mining (fixed, [test]): holding left click made one hit only. Minecraft's handleKeybinds calls
  continueAttack(screen == null && keyAttack.isDown() && mouseHandler.isMouseGrabbed()), and Minecraft's window is
  hidden behind HN's, which owns the mouse: never "grabbed", so block-breaking progress never continued [src: javap of
  Minecraft.handleKeybinds]. Fix: MinecraftAttackMixin redirects that one isMouseGrabbed call to
  `Passthrough.driving || isMouseGrabbed()`. Temporary logs proved it on the running game: "attack held: driving
  true, grabbed false", then continueAttack(true) with a block hit [test]. `tools/minetest.py` (with the new test-only
  `ungrab` message, the window as in the game): tap 0.05 s -> block stays, hold 0.4 s -> stays (progress stops on
  release), hold 1.6 s -> broken, a new block afterwards -> not mined: before the fix the 1.6 s hold failed, after it
  `results: OK` [test]. Menus and cursor are unchanged (only that call is redirected).
- TVs, radios, other electric devices (dispatch [src], not [game]): the TV rule refused everything (no grab and no
  action), and our right click held E down while the button was held, which HN reads as its pick-up, not a press.
  Now: `electric()` = the class chain has Electric or Tumbler (HN's switchable devices: BP_Radio_apartments_C <
  Radio < Tumbler < Electric). Right click on a device we don't carry (TV, lamp) -> one E tap (`hn_action_tap`: key
  down, up 60 ms later) = HN switches it, logged `right click: <actor> is electric: HN's action (switch it)`. A
  carryable electric device (radio): a tap switches it (`a tap on X: HN's action (switch it)`), holding past the
  pickup time carries it. HN's other actions (doors, switches) also use the tap now, never a held E, so one press is
  one activation. release_hn_action() at the start of each right click releases any key still down.
- HN's own hotbar (rule [src], not [game]): a chair still went to HN's hotbar: rule 5 (a prop HN keeps asleep) also
  required a single primitive component, and chairs have several. The broader cause is "anything movable and
  small that HN would take with its action": rule 5 now = OnIsActionComponent on the root, root movable, <= 90 cm
  half size, any number of parts. Fixtures stay HN's (not movable, or bigger). `carry: HN took <actor> ...` remains
  as the detector for anything the rule still misses. HN's inventory isn't disabled (its items/keys keep HN's pickup).
- Charged throw (Q, [src], not [game]): Q down starts a charge (0 -> 100% over 1.2 s, action bar `Throw [####  ]`),
  Q up throws once at base x (0.45 + charge) (a tap ~half of the old throw, full ~1.45x), logged `carry: threw <actor>
  ... (charge N%)`. Cancelled without a throw if the held item changes or a screen opens. Ctrl+Q unchanged.
- Shadows ([src], not [game]): the 12:00 session made a block stand-in and no shadow was seen. Changes: stand-ins are
  no longer hidden actors but visible actors whose mesh has SetRenderInMainPass(false) (Unreal's own "in the shadow
  passes only"), movable, bCastDynamicShadow, bCastHiddenShadow; a readback logs the flags as the engine has them
  (`shadows: a stand-in as the engine has it: ...`). Steve gets a shadow: his model's 6 boxes (Minecraft's player
  model proportions) as the same shadow-only boxes at his feet, facing his way, arms/legs swinging with his walk,
  flat while crawling. `shadows: HN's lights: N (static a, stationary b, movable c), k able to cast dynamic shadows`
  plus the 6 nearest lights: a static light only has baked shadows and gives nothing movable a shadow (HN's own
  props included), so if the house's lights are static no native shadow is possible from them. That census line is
  the evidence to read first. SkyCraft (read last round: skse/src/WorldRender.cpp DepthVS/ShadowPS,
  SunShadowRenderHook, SkyrimSunShadow) draws depth into Skyrim's shadow map by hooking it; Unreal does that for any
  shadow-casting primitive, so no render hook is used. No fake darkening, decals or blobs.
- Tests on the final code: minetest OK, handtest OK, itemtest OK, turntest STEADY (3.9 deg max). HN DLL `f3dd464e...`
  built after the last source edit and installed (md5 of built = installed).

## Keys through HN's native apply, native shadows that show, pickup transition, XP bar, TV, stored items (2026-10-11 evening)
Evidence kinds: [src] source/binary reading, [test] automated test, [game] seen or logged in Hello Neighbor.
- Keys (fixed, [game] 17:31 log). Root causes, from the game binary (UE4SS CXX header dump of the native classes;
  HelloNeighbor-Win64-Shipping.exe disassembled with capstone, scripts in the lab):
  - FlatActor::OnApplyToMe is a blueprint-only event (no native thunk) and no lock blueprint implements it: calling it
    did nothing (the lock stayed "locked").
  - The native unlock is ALockpick's apply method, 0x14051fea0, bound by ALockpick's constructor (0x14050c1c0) as an
    unreflected native delegate in its ApplyComponent at +0xF0. It compares the key's Code with each of the lock's
    m_Codes (case-insensitive); on a match it broadcasts to the character (+0x950), frees the lock from its door
    (attacher, FlatComponent mode free, door's lock list), sets the lock's open byte (+0x440, what IsOpen reads),
    calls OnChange(true), plays the lock's OpenSound, and deletes the key only if bOneOff (mode FM_Delete). It
    returns true only then.
  - The old fallback called OnChange(true) alone (only the lock's look: no sound, HN's state still locked), and the
    old "HN took it into its world" test treated "not hidden" as taken: a held key is never hidden, so the token
    was consumed while the actor stayed attached to the camera (key in hand, gone from the inventory).
  - Now native_apply calls that bound method (after checking the bound object is the item, and the vtable and method
    are in the game's image), once per click; the item's fate is read back (deleted / mode changed -> token goes,
    else kept). 17:31: "lock locked -> open: UNLOCKED", key kept; further clicks on the open door and the car: no
    effect, no error.
- Native shadows ([game] screenshots 17:4x and 18:0x, ReShade off): the stand-ins were placed right (F6 shows them on
  the blocks and on Steve) but with SetRenderInMainPass(false) this UE 4.20 build drops them from the sun's shadow
  too: with F6 (drawn) the shadows appeared, without, none. Now: in the main pass, hidden in game (component
  bHiddenInGame) + bCastHiddenShadow + movable. With ReShade off (Minecraft's picture gone) the hidden stand-ins cast
  the blocks' and Steve's shadows on the road and curb. Earlier the stand-ins were static (SetMobility(2) didn't
  take; now written before FinishSpawning and checked).
- The "silhouette around blocks" was the compositor's screen-space contact shadow (NeighborCraft.fx ContactShadow
  0.5): removed.
- Indoors HN's lights cast no dynamic shadows (census: 120 movable lights, shadows off): the light that lights Steve
  most is given shadows while he is near it (lend_shadows_tick), given back after.
- Pickup transition ([src], [test] fist ids): the first frame after a pickup was placed on a fallback fist (the
  12:43 log: "fist default" on every first line). The fist message now names its token; the object is shown only
  on its own fist ("shown 109 ms after it was selected, on Minecraft's fist for it" in the 13:28 log).
- XP bar ([test] carryuitest: values, replacement, run-out, screenshots): pickup progress and throw charge drawn
  in the experience bar's place with its sprites (CarryBar, HudCarryBarMixin).
- TV ([game]): carried when HN says it can be taken (m_bPickUp) or is loose; a tap still switches it.
- Stored items ([game] 13:24 log): sounds paused, effects stopped, tick off; resumed when held or put back. A
  second store no longer overwrites the remembered tick state.
- Icons ([test] chair 23% -> 87% of the side): framed on the visible meshes and normalised in Minecraft.
- Mining test now asks the server whether the block is there (an inventory count missed drops on reruns).
- ReShade.ini: End toggles effects (KeyEffects=35), for tests with ReShade off.

## Shadows of everything Minecraft draws, held objects in third person, sharper sun shadows, tint (2026-10-11 night)
Tested by driving the game myself (simulated input, screenshots of the game window; ReShade off with End, stand-ins
shown with F6) while the user was away. Evidence: screenshots 18:1x-18:2x, UE4SS.log.
- Shadows from Minecraft's own geometry (ShadowExport.java, [test] shadowtest + [game] screenshots): every frame,
  every entity within 32 blocks through its renderer's own steps: living entities (Steve too, in first person
  where he isn't drawn) by LivingEntityRenderer.submit replayed (render offset, sleeping, scale, setupRotations,
  flip, the renderer's scale, -1.501) and the model's setupAnim, each visible cube of the model; held items as
  ItemInHandLayer places them; dropped items as ItemEntityRenderer does (bob, spin); other entities by their box;
  the nearest particles. Items layer by layer in the layer's own transform; a flat (sprite) item as its opaque
  pixels merged into rectangles (a sword: 19 boxes; one box per item made every item's shadow a square). HN gives
  each box a hidden shadow-casting cube from a pool (shadow_boxes_tick, one SetActorTransform each); the six-box
  approximation of Steve stands aside while poses arrive. [game]: Steve's shadow with the sword in first and third
  person; sneaking lowers it ([test]).
- Sun shadows were blurred blobs: the stationary sun had 1 dynamic cascade over 35 m (log: "dynamic shadow distance
  3500 cm, cascades 1"); now 40 m and 3 cascades: Steve's shadow shows head, shoulders and body with sharp edges
  ([game] screenshots before/after). HN's own movable props get the same.
- Third person: the held HN object went to twice the hand's coordinates (60 m away): its bounds were read with
  colliding components only, and a carried object has collision off (origin 0). Now by its drawn bounds: [game]
  the trash can in Steve's hand, seen from the front.
- Tint: the light's colour now from the whole picture's grey surfaces only (NeighborCraft.fx light_colour; a local
  estimate fell back to grass where nothing grey was near); tools/tintcheck.py (emulation) and [game]: cobblestone
  on a lawn stays grey.
- Not resolved this round: in the second automated session right clicks on the lawn didn't place blocks (HN logged
  "Minecraft use" on the landscape); blocks placed by command were fine. Not investigated.

## Colour that doesn't change with the view (2026-10-11 night, later)
- The user: a cobblestone wall changed colour looking up and down. Screenshots ([game]): green looking at grass,
  beige looking at the sky; the held sword too. Cause: every colour the compositor took from the picture is the
  surfaces' own colour (the light's tint, the grade, and the "no surface near it" fallback, the screen's average).
- Now: the tint is Hello Neighbor's sun colour (the add-on reads the directional light's LightColor once a second:
  HostLightColour; white until set), no grade from the picture, and the fallback is neutral. Brightness stays local
  (HN surfaces near in depth). [game] after the change: the same wall grey looking down, level and up; the sword
  its own colour. The sun-colour part is built, installed at the next restart.

## Faces lit by HN's sun, HN's shadow edges across blocks and Steve (2026-10-11 night, later)
- The user: a wall's sunlit front was as dark as its shaded side; a roof's shadow edge crossed the ground but not
  the blocks or Steve.
- Minecraft's fixed face shading is off (ClientLevelLightingMixin: flat CardinalLighting). The compositor lights
  faces by HN's sun: the add-on reads the sun's direction (GetForwardVector) and intensity, the compositor turns it
  into Minecraft's camera space (SunDirMc), the shader rebuilds each face's normal from Minecraft's depth.
- Receivers: the stand-ins of blocks and of living models' cubes are drawn by HN (hidden before) with HN's own
  opaque base_m and a flat grey texture the mod writes and imports (receiver_material), so HN lights them with its
  sun, shadows and ambient. Where HN's depth equals Minecraft's, the compositor takes that pixel's brightness as the
  light on the block (Minecraft's colour times luma / ReceiverReference; colour from the sun). Measured with ReShade
  off: the grey stand-in is ~0.88 in sun, ~0.5 in shade; ReceiverReference 0.8.
- [game] screenshots: the roof's shadow edge crosses Steve's shoulder and a plank wall exactly as it crosses the
  road; with ReShade off the grey stand-ins show the same edge. Particles and item silhouettes stay shadow-only.
- With ReShade off, blocks and Steve now show as grey boxes (HN draws the receivers).
- Limits: a block face flush with an HN surface may read that surface's brightness; colour comes from the sun only.

## Receivers: crash, flush stand-ins, first person, skin layers, flashes, stutter, freeze, Steam "Stopping" (2026-10-11 late)
- Crash 19:11 (UE crash report: main.dll bridge.cpp:807 stand_in_receives -> SetMaterial): the receiver material and
  its imported texture had no engine reference and were garbage-collected; the next SetMaterial used freed memory.
  Now root-set (UE4SS SetRootSet) while the level lives, released on level change; the material's outer is the
  material library's default object (a rooted object keeps its outer alive). Not yet run in game.
- Freezes (the user): Minecraft's "waiting for HN's world: HN's ground at ... not on the client yet" at the plank
  wall: the world probe found the stand-ins (drawn, flush) as ground inside Minecraft's own blocks. The probe now
  ignores every stand-in; their collision is off three ways (profile NoCollision, all channels ignored, disabled;
  the engine reads back 0).
- Seams: block stand-ins exactly 1 block (0.99 left 1 cm gaps through which the compositor read the ground's light).
- First person: the local player's model boxes only cast (kind 2): Minecraft doesn't draw him there.
- Skin's outer layers (hat, jacket, sleeves, trousers) are in the boxes.
- Grey flash on place: block stand-ins drawn 300 ms after they appear (they cast at once); on break: the client
  tells HN at once (ClientBlockChangeMixin, "unreceive") before the server's change. Stand-in pixels peeking past
  Minecraft's edges take the neighbouring Minecraft pixel (PS_Final).
- First-time stutter: the shadow-box pool grows ahead (4 a tick to 240), the receiver material is made at level start.
- Steam showed the game "Stopping" after the crash: the Gradle daemon started by start_minecraft.bat outlived it;
  now --no-daemon.

## Blocks and Steve too bright in dark rooms (2026-10-13)
- The user (outofplace.png, the nightmare corridor, 2026-10-11 19:27): cobblestone and Steve lit as if in daylight.
  The receivers were active (UE4SS.log 19:26:09); the blocks came out at ~65% of daylight while HN's walls and floor
  beside them read 0.03-0.08.
- [game] measurement 2026-10-13 (ReShade on/off, Act 1, the kid's house's shade): the bare stand-in reads 0.46,
  cobblestone composites to 0.35, the dirt and wall beside it 0.22 and 0.59 (looks right). Worked back from the
  corridor screenshot, the stand-in read ~0.39 there: Unreal lights the movable stand-ins from its coarse indirect
  grid, which in the corridor still carries near-outdoor ambient; HN's baked walls don't get it. (The ^0.6 lift added
  for "Steve too dark in a lit room" adds a little on top; kept.)
- Fix (NeighborCraft.fx): PS_Light also writes HostSurroundTex, HN's brightness with the stand-ins left out (a pixel
  whose HN depth is Minecraft's); surround_cap() averages it over ~1.2 m around a Minecraft surface (near in depth)
  and limits the light to that / SurroundReference (0.25). Both paths: receivers and the fallback relight. Calibration
  from the measurements: outdoor shade cap ~1.2 (no change, gain 0.71), the corridor ~0.2 (cobblestone ~0.1, was 0.31).
- [game] ReShade compiled it live (ReShade.log 18:17:45), composite running. Not yet seen in a dark room.
  Possible side effect: a block in shade on very dark ground could come out darker than before.
- Same day, the user: "out in the sunlight im so dark" (screenshot: Steve's back ~1/4 of its brightness on a sunlit
  road, his long shadow beside him). The average of the surroundings took in his own shadow; now the brightest
  surrounding HN surface sets the limit (SurroundReference 0.3): one lit surface near means light is there. Compiled
  live (ReShade.log 18:21:33); not yet seen in game.
- Same day, the user: a pale line along the top edge of plank blocks seen at eye level (the stand-in's top face). The
  edge fill in PS_Final only took stand-in pixels within +-5 cm of the Minecraft surface beside them; a top face seen
  nearly edge-on recedes up to a block. Now from the surface to 1.05 block behind it. Compiled live (18:24:20).
- Same day, the user: keep the receivers, but make what peeks through look like the block. Each full opaque block's
  receiver is now drawn in its block's colour (Minecraft's map colour, sent as "col" with "opaque" in the blocks
  message; WorldBridge.kind), scaled so its luma as the compositor measures it equals the grey's (sRGB 128: the light
  reading and ReceiverReference are unchanged). One material per colour (bridge.cpp receiver_material(context, rgb),
  a 4x4 PNG written by solid_png, imported), each kept alive by its own keeper stand-in, all forgotten on level change
  like the grey one. Steve's boxes stay grey. [test] Python port: the PNG decodes; planks -> (151,126,77), grass
  (102,144,44), stone/black/white grey, all at luma 0.502. Built; not yet installed or run in game.

## Nightmare crash, Minecraft left running, blotchy Steve, the hand's light, the bare pause screen (2026-10-13 evening)
- Crash 18:34 (UE crash report; symbolized: bridge.cpp lend_shadows_tick, K2_GetComponentLocation on a light): right
  after a nightmare ended. Its lights (Act1Nightmares/...) died with its streamed level, but the light list was keyed
  by the player controller, which a streamed level doesn't change. Now: level_key(pc) (the controller and the world's
  loaded Levels) keys the light list, the sun, the doors and the interactive parts (rebuilt when a level streams in or
  out; logged "levels: N loaded"), and the lights are FWeakObjectPtr (a destroyed one is skipped). Not yet run in game.
- Minecraft played on after the crash (Steve fell into the void): the crash reporter keeps HN's process and its
  connection open, and Minecraft only quit on a closed connection. HN now sends {"t":"beat"} once a second from the
  game thread (menus too); Minecraft pauses after 2 s without one ("Hello Neighbor has stopped answering") and quits
  after 20 s more. Compiles; not yet run.
- Blotchy ("warbled") darkness over Steve in a dark room (warbleddarkness2.png): the light limit took the plain
  brightest of 12 points, which jumped between pixels; now a 4th-power mean over coarser samples, and the received
  light is read slightly blurred over stand-in pixels only (received_luma). Live in the shader (18:40:54).
- The first-person hand took neutral light everywhere; now limited by the room's light (hand_light: HN's surfaces
  within ~3-6 m of the camera, the same soft brightest). Live in the shader.
- Under HN's pause menu the compositor was off, so the stand-ins showed bare (plain coloured boxes; the user took them
  for wrongly textured blocks and Steve). Minecraft's last picture now stays under the pause menu (McPaused: its own
  pause screen and hand left out); not in cutscenes. Built; not yet run.
- Steve's stand-in split from him under the pause menu, and showed beside him while running when Minecraft stuttered
  (the user). Pause: the compositor used the pose a frame back and predicted from the last motion; with no new poses
  under the pause menu that offset stayed for the whole pause. Now no lag and no prediction while paused. Running:
  the stand-ins took the newest "shadows" message, but the picture reaches HN a frame or more later (GPU readback,
  more in a stutter). Shadows messages now carry the exported frame's number ("f", FrameExporter.lastCaptured); HN
  keeps the last 12 and poses from the one of the frame the compositor last uploaded (select_shadow_frame,
  compositor::shown_frame). Builds; not yet run.
- The frame matching made it worse (the user: "a clone of me in white copying my exact moves"): back to the newest
  boxes; HN now logs every 5 s how far the newest boxes run ahead of the picture shown ("shadows: newest boxes of
  Minecraft frame N, the picture shown is frame M (most ahead in 5 s: K)") to find out why before another attempt.
- Model stand-ins in the skin's colours (the user: "change it to match the player skin so if it lags you cannot
  tell"): ShadowExport sends "c", per box the average of the opaque skin pixels its cube's faces use (skin read once
  from the resources; a downloaded skin stays grey); HN draws each model box's receiver in that colour at the grey's
  brightness (receiver_material(context, rgb), as the blocks). Builds; not yet run.
- [test] shadowtest check 6 (new): 13 colours for 13 boxes; Steve's 12 model boxes: 8 coloured (6 distinct: teal
  shirt #06b4b4, trousers #3f3680, hair #513621, ...), 4 cubes the skin leaves fully transparent (empty outer
  layers). Those had been drawn by HN as grey shells around Steve, bigger than him as Minecraft draws him: now sent as
  -2 and not drawn (they still cast). shadowtest: all 6 OK. Installed; not yet run in game.
- Steve's trail on camera turns (the user) and "make sure fps is fixed": Minecraft rendered at 120 fps (framerateLimit
  120, vsync off) against HN's steady 60 (frames: 16.7 ms average), so two Minecraft frames per HN frame; the log
  ("newest boxes of Minecraft frame N, the picture shown is frame M") showed the stand-in poses 2-3 Minecraft frames
  ahead of the picture. Now: HN reports its rate every 5 s ("hostfps"), Minecraft sets its limit to it (60 here);
  the stand-ins are posed from the frame the next present will show (last shown + Minecraft frames per HN frame),
  not the last shown (the earlier attempt, two Minecraft frames behind: the "clone"). Builds; not yet run.
- Verified in game while driving (19:20-19:37): the heartbeat (HN crashed 19:20:57 in the capture prototype; Minecraft
  paused 19:20:59 and quit 19:21:37, no java left), the picture kept under the pause menu (blocks visible behind it),
  "levels: 6 loaded" (the level list for the nightmare-crash fix is readable). The scene capture (480x270, every
  frame) cost nothing measurable: 16.7 ms average with and without it.
- [game] 19:59: UE4SS's FWeakObjectPtr for a stand-in just spawned gave A DIFFERENT OBJECT, and a check of the object
  array's slot (IndexToObject(GetInternalIndex())) didn't hold it either: in this build the object array doesn't line
  up with the objects' indices. The nightmare-crash fix's weak light pointers were therefore wrong; now plain pointers
  dropped whenever level_key changes (a streamed level leaves World.Levels when it unloads, so its lights are dropped
  on the next tick, before any call); the lent light is forgotten on that change, not called. Nothing may rely on
  the object array (weak pointers, root set) in this build.

## Receivers seen only by a scene capture (2026-10-13 night)
- Why: Steve's stand-in trailed him ("a clone of myself chasing my player"): two renderers are never in step, so any
  receiver the player can see shows beside its Minecraft surface when they differ.
- bridge.cpp ReceiverCapture: a SceneCapture2D (482x271 RGBA16F, SCS_SceneColorSceneDepth, player's pawn hidden from
  it) follows HN's camera; once the add-on has the texture, every receiver is in the player controller's HiddenActors
  (rewritten at the end of each tick, emptied before stand-ins are destroyed). compositor.cpp finds the texture at
  init_resource by its size, binds HNRECV, sets RecvActive; NeighborCraft.fx capture_received (capture depth = Minecraft's;
  HDR -> screen by HN's own surfaces in both pictures). Fallback: dlls/neighborcraft_receivers_visible.txt.
- [game] 21:54:40: "scene capture SceneCapture2D_0 -> TextureRenderTarget2D_0", ReShade.log "the receivers' capture
  texture found (482x271, RGBA16F)", "receivers: hidden from the player, their light read from the scene capture".
  Not yet seen: the picture itself (no clone, blocks and Steve still lit/shadowed), the GPU cost. The user needs the
  foreground: the visual check is theirs.
- [game] the user (verybroken.png): no clone any more, but the light on a cobblestone block showed a house's window and
  roof edges and 4-pixel squares. ReShade.log: only one 482x271 texture was ever found (the binding was right). Cause:
  the exposure was estimated per pixel from 12 points around it in both pictures; the two pictures are a hair apart,
  so the ratio jumped at every edge the points crossed. Now one exposure per frame (RecvExposure pass: screen and
  capture brightness where both see the same HN surface, mipped to one value), and the capture's colour read with
  filtering. Live 21:58:33. compositor.cpp also keeps the first matching texture only (built, needs a restart).
- [game] the user: light on blocks and Steve shimmering and smudging, Steve stuttering and slightly pixelated. Research:
  quarter-size lighting buffers are upsampled depth-aware (joint bilateral / nearest-depth: Kopf et al.; the NVIDIA
  nearest-depth filter); UE4 scene captures have no working TAA (forum threads, UE-26778), so their shadow edges crawl.
  Now: capture_received weighs the capture texels around a pixel by depth match and distance (no background through
  an edge, no squares), returns how sure it is and the composite fades by it (no flip between two lights where Steve's
  picture and boxes are a frame apart); the exposure is a log-average, eased over ~12 frames (RecvExpSmooth/Store).
  Live 22:02:45. Not yet seen. Left if it still shimmers: a larger capture (cost) or temporal smoothing of the light.
- [game] the user (trailOrSmudgingEffect.png): copies of Steve and a block when turning. Cause: the re-projection
  marches each pixel's ray up to HN's surface there in 24 steps; with the receivers hidden that surface is the road or
  a house far behind, the steps grew long and the hit snapped between them. Now the march ends at the capture's depth
  (it sees the receivers) and the crossing is bisected (6 steps). Live 22:13:59.
- Looking straight up/down "freaks out" the cameras: the look pitch (HN computes it from raw mouse) is now +-89, not
  +-90. Built; needs a restart.
- [game] the user (screenshots 22:14, 22:23): a block's edge-on side face came apart in vertical streaks. The march's
  end (capture depth) is a quarter-size reading: on an edge-on face it was the front edge's, short of the face. Now
  1.5 blocks past it (the bisection keeps the steps fine). Live 22:26:26.
- Looking straight up/down: the +-89 pitch build was never installed (HN was running); installed at the next close.
- [game] the user: walking around a block, the face turning away from them brightened. Cause: the capture's 4x4
  texels around a pixel were weighed by depth against the pixel's own depth; at a block's edge the next face of the
  same block has nearly that depth, so its light bled onto a face turning edge-on (mostly edge). Now plane-aware: each
  texel is compared with this face's plane at the texel's place (the depth slope per pixel from Minecraft's depth, the
  same samples as the face normal), and the tolerance grows with the slope. Live 22:31:45.

## Roles swapped: receivers in the player's view, the capture shows what is behind them (2026-10-13 night)
- The user: faces still brightening, Steve stuttering, "how was this not a problem earlier?". Earlier the receiver's
  light was read from the very pixel on screen (full size, anti-aliased, same frame); with the receivers only in the
  capture it came from a quarter-size picture without anti-aliasing, a frame apart: every artifact since followed.
- Now: receivers drawn for the player again (light pixel-exact, as before); the capture component's own HiddenActors
  (offset 0x250) leaves them out, so the capture shows what is behind them; NeighborCraft.fx paints that (HDR x this
  frame's exposure) where HN's depth is nearer than the capture's and Minecraft doesn't cover the pixel: a receiver
  peeking out (the trailing clone). The re-projection marches to HN's depth again (bisection kept).
- Steve's judder: Minecraft at exactly HN's 60 fps let the two clocks drift (a frame shown twice or skipped); now twice
  HN's rate (120 here), the stand-ins still posed from the frame HN will show.
- Built (HN, MC compiles, shader); the shader and DLL go in together at the next close.

## Native rendering phase 2 (2026-10-13 night)
- First launch: Minecraft crashed at start ("Mixin transformation of SectionCompiler failed ... Redirector ... Scanned 0
  target(s)"): the redirect of tesselateBlock's call in SectionCompiler.compile found nothing in game. Now an inject at
  the head of ModelBlockRenderer.tesselateBlock (cancelled for native blocks), require = 0: a failing hook leaves
  Minecraft running with all blocks its own.
- 22:55 session: Minecraft got "native n=1" and two blocks were placed, but none became native and no texture was
  written; no log says why (require = 0 hides a hook that didn't apply). Now logged: "Minecraft's block meshing is
  hooked" once, and the first 12 decisions with their reason.
- Steve's clone (the user's screenshot 22:56): his model boxes no longer draw (cast only); his light from HN's surfaces
  around him until native Steve (phase 5). dlls/neighborcraft_model_receivers.txt draws them again.
- 22:58 session: "native blocks: hooked" never appeared: Fabric API's renderer (fabric-renderer-api SectionCompilerMixin
  + fabric-renderer-indigo) meshes every block through Indigo's AltModelBlockRendererImpl.tesselateBlock, never
  Minecraft's ModelBlockRenderer.tesselateBlock (and had rewritten the call the first redirect looked for). Now
  IndigoBlockMixin (@Pseudo, require = 0) hooks Indigo's method; the vanilla hook stays for a game without Indigo.
- Faint coloured silhouette beside Steve when turning (screenshot 23:00): not the stand-ins (now cast only): Minecraft's
  picture is 1-3 frames older than HN's camera, and re-projecting a thin, close, turning model leaves a trace. Only a
  native Steve (phase 5) or a shorter frame path removes it.
- [game] 23:08-23:15: native blocks VERIFIED by the user ("blocks are good now") and by screenshots: cobblestone drawn by
  HN (ReShade off: still there), sharp, lit by HN's sky in shade, no shimmer. Every eligible full cube native (n=4096).
  Seen once, not yet explained: a glowing white square near the neighbour's door in HN's own picture (n1/n1off.png).
- [game] the user: a box of native blocks built around Steve stayed lit inside. HN's sky and bounce light are
  precomputed from its own level: runtime blocks can't darken what they enclose (only direct sun shadows work). Now
  Minecraft's own light levels: each native block is a plane per visible face (against air), each with its texture
  darkened by Minecraft's light curve for the light level in front of it ("<tex>_l<level>", written by mc NativeBlocks,
  resent when the light changes); planes cast shadows two-sided.
- Native Steve (phase 5) built alongside: mc NativeSteve/ShadowExport send the local player's body faces (13 numbers each,
  "nf") and their skin textures ("stex"); HN native_steve_tick places a plane per face each frame; Minecraft hides his
  body (LivingEntityRendererMixin), his held item stays; outer skin layers skipped (opaque material). Plane UV convention
  untested: dlls/neighborcraft_steve_uv.txt "swap flipU flipV" (also used by block faces). Off:
  neighborcraft_native_steve_off.txt.
