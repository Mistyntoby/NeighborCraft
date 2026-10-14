# NeighborCraft – Handoff

State after the downhill/slope/crawl commit on top of `f79f348` (2026-10-10, evening). Read this first, then `MODLOG.md` (chronological log, gotchas). Repository
evidence beats anything said in old conversations: every status below says how it is known.

Status legend: **VERIFIED** (seen in game, a log, a recording or an automated test, as cited) · **REPORTED**
(the user said it works, no recorded evidence) · **BUILT** (compiles and is installed, never exercised) ·
**OPEN** (known broken or not done).

---

## 1. Purpose

Real Minecraft Java runs next to Hello Neighbor 1 (Steam) and is composited into it: you play as Steve inside HN's
world. Minecraft owns movement, physics, camera, inventory and combat. HN's player is a puppet placed where Steve is,
and HN's own doors, cabinets, switches and physics objects keep working. Minecraft's picture (world, hand, HUD) is
depth-composited into HN's frame and relit. HN's geometry reaches Minecraft as invisible collision blocks.

Rules from the owner (still in force):
- Don't ship Mojang or tinyBuild assets; read them from the installs.
- Don't touch the user's normal Minecraft profile. HN saves and files are backed up in the lab (`backups/`).
- Private repo `Mistyntoby/NeighborCraft`. Push each working milestone. Git identity Mistyntoby /
  `147459291+Mistyntoby@users.noreply.github.com`.
- The owner's name must never appear in the repo. `.githooks/pre-commit` checks this against patterns kept outside
  the repo in `.git/info/personal-patterns`; commit with `git -c core.hooksPath=.githooks commit`.
- Never commit game files, extracted assets, UE4SS/ReShade binaries, build output or save backups (`.gitignore`).
- Work from evidence (logs, recorder, dumps), not guesses. Don't claim success without in-game evidence.

## 2. Environment, build, run

| Part | Tech | Where |
|---|---|---|
| HN side | C++20 UE4SS C++ mod (RE-UE4SS experimental `6eb3d9bc`, MSVC 14.44, Ninja, Rust for patternsleuth) | `hn/NeighborCraftMod/src` |
| Rendering | ReShade 6.8 add-on API (registered from the same DLL) + shader | `hn/NeighborCraftMod/src/compositor.cpp`, `hn/shaders/NeighborCraft.fx` |
| MC side | Fabric mod for Minecraft **26.3** (Loom, unobfuscated names), JDK 25 | `mc/` |
| Game | Hello Neighbor 1, UE 4.20.2 (tinyBuild custom build), D3D11, Steam app 521890 | Steam install |

The lab lives outside the repo, in `%USERPROFILE%\NeighborCraft-lab`. It holds:
- `deps/`: jdk25, RE-UE4SS sources, ReShade headers;
- `build/hn`: the CMake build;
- `pyenv/`: Python with `websockets`, `minidump`, `PIL`;
- `backups/`, `dumps/`, `collision-report/`;
- `mc-client.log`: the Minecraft log of the last HN-launched session;
- `patch_*.py`: one-off edit scripts. They're history, not needed.

- **Build HN:** `cmd /c hn\build.bat` (vcvars + cmake/ninja → `build/hn/NeighborCraftMod/NeighborCraftMod.dll`).
- **Install HN:** `bash hn/install.sh`. It copies the DLL to `...\Win64\ue4ss\Mods\NeighborCraftMod\dlls\main.dll`
  plus the shader, preset and launch file; `--remove` undoes it.
  - **HN must be closed** first: the DLL stays locked a few seconds after HN closes.
  - **Verify the install** with `md5sum` of the built and installed DLL. A queued install once "succeeded" while the
    copy failed (`Device or resource busy`); a pipe to `tail` hid the error.
- **Minecraft:** HN starts it automatically (`tools/start_minecraft.bat` → `gradlew runClient -PexitWithHost=true`,
  log in the lab's `mc-client.log`). It **compiles from source at each launch**, so MC changes need no install.
  Compile check: `cd mc && JAVA_HOME=<lab>/deps/jdk25 ./gradlew compileClientJava`.
- **Run:** start HN from Steam, then load Act 1. Minecraft launches hidden, connects on ws `127.0.0.1:25620`, makes a
  fresh void world, and takes over movement once HN's world near the player is known.
- **Automated test without HN:** `python tools/drivetest.py [seconds] [x z] [--burst] [--late S]`.
  - Start Minecraft first with `gradlew runClient` in `mc/`, **without** HN.
  - It builds a test floor with a gentle ramp and stairs, drives Steve across, and reports speed per section.
  - It **refuses to run while HN is open**: it would drive the player's own Minecraft.
  - `--burst` sends an HN-sized burst of shapes; `--late` keeps the server busy so the floor is placed late.

## 3. Architecture map

### Link (`hn/.../ws.cpp` ↔ `mc/.../client/HostLink.java`)
JSON over websocket. HN is the client, Minecraft the server on port 25620.

HN → MC:
- `cam` (drive flag, rotation, fov)
- `move` (key bits)
- `key` (inventory, drop, use, attack…)
- `shape` (HN collision blocks)
- `tp`, `clear`, `blocksync`
- `pause`, `screen` mouse/click/wheel
- `punched`, `pickup`, `consume`
- `debug`, `cmd`

MC → HN:
- `hello`, `ready`
- `mcpos` (eyes, camera, look, fov, **feet + hitbox size**)
- `blocks` (player-placed blocks)
- `screen` (open/close)
- `punch`
- `held`, `picked`, `throwreq`

Frames go through shared memory `Local\NeighborCraftFrame`: 4 layers (world, depth, GUI, hand) written by
`FrameExporter.java` and read by `compositor.cpp`.

### HN side (`bridge.cpp`, ~3900 lines; key functions)
- **Tick:** `tick()` runs on UE4SS's engine tick.
  - Each tick: player controller → `in_gameplay()` (menu, pause, **cutscene via `Human::IsPlayCutScene`**) → level
    change and `relevel()` → `doors_tick()`, `physics_tick()`, `physics_punch()`, `carry_tick()`, `dynamic_tick()`
    → `sample_world()` → drive branch (pawn placement, `steve_capsule()`, `minecraft_view()`) → `minecraft_controls()`.
- **World probe (HN → MC collision):**
  - `sample_world()`, nearest first, ≤400 traces per tick:
    - `probe_column()`: 8×8 down-rays per block column for floors and 4×4 up-rays for ceilings. Columns remember the
      heights they covered (`g_columns`: lo, hi) and are probed with a 4-block margin below.
    - `probe_block()`: an octree of `BoxTraceSingleByProfile("Pawn")` down to 1/8 block.
  - `block_shape()`:
    - floor levelling (≤5 cm);
    - step rule (≤0.6 block: heightfield only);
    - ceilings;
    - ORs in `g_dynBits`;
    - sends the result as `shape`.
- **Moving parts (doors, cabinet doors, drawers, loose objects):** `watch_interactives()` + `dynamic_tick()` +
  `voxelise()`.
  - Each tick, collision comes from each part's real mesh bounds × transform.
  - The static probe **ignores** moving parts: `in_moving_part()`, plus box cells masked with `g_dynBits`.
  - Blocks whose moving collision changed are re-probed.
  - `doors_tick()` invalidates a door's area when its bounds change.
  - Moving collision is never placed inside Steve's hitbox.
- **Interaction:** `right_click()` → carry first (place, or start a pickup hold) → `looking_at_interactive()`.
  - The ray is Minecraft's: eyes + look, 3 m, only the held item ignored.
  - HN furniture decides via `flat_actionable()` → blueprint `OnIsActionComponent(part)`.
  - `interactive_actor()` (InteractiveComponent or Door) → the injected HN action key **E** (`InputAction`/
    `InputPickUp` in HN's `DefaultInput.ini`).
  - Fallback: inside a **door's** bounds (+2 cm).
  - HN's camera component is moved to Steve's eyes so HN's own E trace matches.
- **Physics:**
  - `physics_tick()`: walk/sprint pushes from the *intended* movement keys (Steve is stopped at objects, so his own
    speed is 0); an impact on first contact, then a capped force, at the point of the object nearest his waist.
  - `physics_punch()`: from Minecraft's attack hit (`MinecraftAttackMixin`, once per `startAttack`), a velocity change
    `1800/(1+m/500)` cm/s × weapon × charge × sprint. HN masses are 150–400 kg for small props.
  - `hinged_part()` excludes doors and drawers from both.
- **Carry (new):** `pickup_target()`, `complete_pickup()`, `carry_answer()`, `carried_for()`, `carry_place()`,
  `carry_throw()`, `release()`, `carry_tick()`. See §9.
- **Player body:** while driving, `steve_capsule()` sizes HN's capsule to Steve's hitbox and places it on his feet
  (HN's own is 51.4 cm radius × 216 cm; it used to shove physics doors). `hn_capsule()` restores it.
- **Stability:**
  - `log()` only queues lines; `log_writer()` hands them to UE4SS Output from its own thread (UE4SS Output once
    deadlocked the game thread).
  - `watchdog()` writes `NeighborCraft_freeze_*.dmp` next to the DLL if the game thread stops for 5 s while driving.
  - `WsClient::~WsClient` detaches its thread (this fixed a CRT abort on every HN exit).
- **Diagnostics in HN:**
  - `frame_stats()` logs frame times and the probe's share every 5 s.
  - F12: debug view, plus the interaction target readout on the action bar.
  - F11: collision map.
  - F9: object dump.
  - F8: re-level and resend the world.
  - F10: toggles collision mode.

### MC side (`mc/src`)
- `main/.../Passthrough.java`: registers `neighborcraft:host` (the invisible HN collision block) and
  `neighborcraft:hn_object` (the carried-object token, stack 1); server tick hooks.
- `HostBlock.java`:
  - `SHAPES` (shared server/client map) and `fromMask()`: a voxel bitset plus pairwise slab joins, ~0.3 ms per
    block; it was 14.5 ms with `Shapes.or`.
  - `shape()`: **no entry = empty**.
- `WorldBridge.java`:
  - `shapes()`: built on the `NeighborCraft shapes` thread, placed on the server; `pending` tracking; timing stats in
    the log every 5 s.
  - `arrived()`: whether the ground under Steve is on the client yet.
  - `clearSolid()` goes through the same queue as shapes.
  - `sync`, `command`.
- `Carry.java`: token give/consume (server side), and re-adds tokens that were dropped as item entities.
- Client:
  - `HostLink`: messages.
  - `ClientInput`: keys, mouse, inventory clicks (link buttons 0/1/2 → Minecraft/SDL 1/3/2), sounds.
  - `PlayerSync`, `HostState`: drive lockstep.
  - `FrameExporter`: frame layers.
  - `CollisionRecorder`: always on.
  - `PassthroughClient`: options (view distance 8, no auto-jump, no music); fresh world per run; kit; reports the held
    token.
- Client mixins:
  - `CameraMixin`: camera, bob, FOV, sends `mcpos`.
  - `EntityMoveMixin`:
    - micro-ledge lift (≤1/16 block, no gravity that tick);
    - speed kept on stepped or grazing collisions;
    - **holds Steve while his ground hasn't arrived** (5 s failsafe).
  - `MinecraftAttackMixin`: punches.
  - `GameRendererMixin`, `LevelRendererMixin`, `LocalPlayerMixin`, …

## 4. Status by feature

| Feature | Status | Evidence |
|---|---|---|
| Compositing under HN UI, pause sync, hidden MC window | VERIFIED (earlier sessions, user) | MODLOG |
| MC camera/FOV/bob, F5, keybinds, survival, no auto-jump | REPORTED fixed | user, earlier |
| Door collision follows the leaf (moving collision) | VERIFIED by the user | user confirmed; preserve it |
| Door remnants (static probe baked mid-swing leaf/handle) | BUILT, 2nd cause fixed | user still saw a remnant at 20:31: the closed leaf was in the static world until re-probed; now cleared on the first move (MODLOG night section); **not yet run** |
| Collision leeway | BUILT | moving parts their own size, static sub-cells need geometry in their middle; **not yet run** |
| Doors stuck open | BUILT, likely fixed | cause: HN capsule (51 cm) inside the leaf + our nudges on the 747 kg physics leaf; both fixed; no recent report of stuck doors |
| Forced crawl at doors / objects on head | BUILT (invariant) | 4 causes so far (MODLOG); now nothing is sent inside Steve's current hitbox above 0.15 m; 21:30 session still crawled 9 times before it; **not yet run in game** |
| Stops running/jumping downhill | VERIFIED in drive test (13 -> 0 mid-air stops), cause proven from the 21:30 recorder | the short-wait clamp held him in the air; **not yet run in game**; recorder event MIDAIR_STOP will catch any left |
| Slope smoothness (micro-steps) | BUILT, VERIFIED in the drive test | step-down + camera height filter; downhill 4.87 -> 5.6 m/s, worst camera jolt 19.8 -> 7.2 cm; **feel not yet judged by the user** |
| Cabinets / wardrobes / TV-stand drawers open | VERIFIED | log `OnIsActionComponent(Door1) yes`, user |
| Right-click targeting (nearest hit) | VERIFIED partly | ray now Minecraft's; **bug seen**: carpet in a doorway activated the door via the bounds fallback (log 02:01) |
| TV / light switches | OPEN, unverified | the TV in Act 1 (`BP_TV_apartments`) is a loose physics object: now a carry target, not an "on" toggle; switches (`BP_Switcher`, Tumbler) untested |
| Walk/sprint pushes | VERIFIED working (log impacts 30–40k kg·cm/s) | user didn't complain since |
| Punch/sword push + hit sound | VERIFIED (log: chair 6–8 m/s, mug 12 m/s) | sound unconfirmed by the user |
| Gentle-slope/seam slowdown | VERIFIED in the drive test | ramp 2.2–2.9 → 5.66 m/s |
| Stairs speed | VERIFIED in the drive test | ~5.1 m/s sprint, no resets (~11% cost: one step per tick) |
| Falling through Act 1 road | VERIFIED fixed in game (no falls since the hold); later stutter fixed | logs: holds 50–150 ms, server keeps up |
| Stutter downhill | VERIFIED | frame stats: 60 fps, worst ≈ 23–38 ms, 0 frames > 50 ms |
| Freeze (UE4SS log deadlock) | VERIFIED cause (watchdog dump); fix built | no freeze since |
| Shutdown crash | VERIFIED fixed | no CrashDumps since |
| Cutscene glitch (MC kit/HUD/drive during cutscene) | VERIFIED gate fires (`cutscene: Minecraft waits` / `cutscene over`) | the user hasn't re-judged the item glitch |
| Inventory left-click | VERIFIED (log 02:01: MC button 1 = left picks the full stack) | |
| Carry: pickup/hold/place/throw | in game: pickups VERIFIED, attachment continuous (360 check, 22:41); fist frame bug found and fixed (turntest STEADY, 3.9 deg max); TVs get no HN action (BUILT); **object on the fist in game not yet seen after the fix** | MODLOG night 5 |
| Per-item inventory icons | HN capture VERIFIED in game (3 exports); path and alpha fixed; the real exports VERIFIED in the hotbar (test Minecraft screenshot) | MODLOG night 4 |
| See-through cutout blocks / background-tinted light | cutout alpha fixed (night 4, measured); light from the picture behind replaced by light from near surfaces (emulated 58% -> 0%); **neither seen in game; check ReShade.log compiles NeighborCraft.fx** | MODLOG nights 4-5 |
| Ctrl+Q stack drop | VERIFIED in itemtest (counts, empty slot, pickup again); not yet in game | MODLOG night 3 |
| Lighting/shadows, transparency, GPU sharing, macOS, block mining in HN geometry, Neighbor combat | OPEN | not started |

## 5. Verified fixes (with the evidence that proved the cause)

All of these are in MODLOG §"Interactions, physics, terrain, stability":
- Inventory buttons (SDL numbering).
- FlatActor interactions.
- Shutdown crash (exit minidumps → `dll_dllmain.cpp`).
- Freeze (watchdog dump: game thread in `Output.hpp`).
- Ramp lift (recorder + drive test).
- Road fall: rescue diagnostics showed HN had the floors; the server was 2.9–12.6 s behind; the shape builder was
  measured at 8.7 s for a burst.
- Stutter (frame stats).
- Door capsule.

## 6. Outstanding issues (priority order)

000000000. **Scene-capture receivers (prototype, 2026-10-13 evening)**: goal: receivers invisible to the player while
          the compositor still reads HN's light on them. Off unless `dlls/neighborcraft_capture_test.txt` exists ("show"
          or "hide", optional second word = ESceneCaptureSource; re-read every 5 s). Rollback: delete the file; full
          rollback: tag `receivers-visible-baseline`.
          - APIs (4.20 header dump): APlayerController.HiddenActors (engine TArray, grown with GMalloc), SceneCapture2D
            + CaptureComponent2D (FOVAngle 0x2D4, TextureTarget 0x2E0, CaptureSource 0x2E8, bCaptureEveryFrame 0x280;
            the name lookup failed for FOVAngle once: fields by name, else by offset), HideActorComponents,
            KismetRenderingLibrary CreateRenderTarget2D / ExportRenderTarget (param "FileName"; float targets only:
            .hdr) / ReadRenderTargetRawPixel (useless on a float target).
          - [game] VERIFIED: a receiver in the player controller's HiddenActors is gone from HN's own picture (ReShade
            off, screenshots hide2_off/hide0_off) and still in the capture, lit by HN (SceneColorHDR export: bluish in a
            house's shade; BaseColor: grey 128; normals right). Camera alignment of the capture matches the screen.
          - [game] SCS_FinalColorLDR comes out black near the camera (the capture's own auto exposure): use HDR.
          - [game] no frame-time change (60 fps vsync-bound, 16.7 ms with and without; GPU time not measured).
          - NOT settled: whether a player-hidden receiver still casts its shadow in the player's view (the
            sideways-lit test lost the cube from view). Design so it doesn't matter: keep the hidden shadow caster
            (proven) next to the player-hidden receiver.
          - Next: (1) the compositor reads the capture: the ReShade add-on finds the 480x270 RGBA16F texture by its
            description at init_resource, makes a view and binds it to the effect (update_texture_bindings); the shader
            takes the received light from it (HDR ratio to a sunlit-grey reference, to be measured); (2) receivers as
            separate actors in HiddenActors, casters unchanged; (3) GPU cost of a second scene render.
          - Found on the way: UE4SS's FWeakObjectPtr gives OTHER objects in this build (the object array doesn't line up
            with the indices): no weak pointers or root-set anywhere (lights now per-level plain pointers).

00000000. **Play-test 2026-10-13** (P0): blocks and Steve in a dark room (the nightmare corridor) should no longer be
         daylight bright (shader surround_cap, MODLOG 2026-10-13); check blocks in shade on dark ground aren't too dark.
         Next: the user's seven-task list (doors, held-item use + Act 1 car keys, performance, lamp, torch light,
         block tops).

0000000. **Open (2026-10-11 night)**:
        1. Shadows on NeighborCraft's own blocks (receiving): blocks are Minecraft's picture; HN's shadows fall on
           HN's surfaces only. The compositor's local light strength already darkens blocks in HN's shade.
        2. Right click didn't place blocks on the lawn in one automated session (unexplained, see MODLOG).
        3. Particles cast as cubes of their size (billboards in Minecraft).
        Testing shadows: ReShade off with End, stand-ins visible with F6; screenshots of the game window.

000000. **Play-test 2026-10-11 afternoon** (P0), in this order, reading UE4SS.log:
        1. Interactions: right click the TV and a lamp (switches; `is electric: HN's action`), tap the radio (switches;
           `a tap on ...`), hold on it (carried), the front door and a switch (one activation per press).
        2. HN's hotbar: right click/hold every chair, stool, box: always the NeighborCraft hotbar; any
           `carry: HN took ...` line names a prop the rule still misses (movable/size are in it).
        3. Mining: hold left click on dirt/stone: breaks; let go midway: stops; tap: one swing.
        4. Throw: tap Q (short), hold Q ~1.2 s (far), `carry: threw ... (charge N%)`; switch slot while charging
           (no throw); Ctrl+Q still drops the stack.
        5. Shadows: `shadows: HN's lights: ...` (are any able to cast dynamic shadows?), `shadows: a stand-in as the
           engine has it: ... bRenderInMainPass 0 ... mobility 2`, `Steve's shadow made of 6 boxes`; look for Steve's
           and stone blocks' shadows near a movable/stationary light. If every light is static, native shadows are
           impossible from them (see MODLOG).

00000. **Play-test 2026-10-11** (P0): right click/hold the trash can, picture frames, TV, a sink, the stove, doors,
       switches: props ours (NeighborCraft hotbar icon), TV nothing, fixtures HN's action; any `carry: HN took ...`
       line names a prop the rule misses (with its movable/parts/size). Held items about twice the fist (tune the
       `scale` column). Place stone/brick blocks near a lamp or window: `shadows: loaded /Engine/BasicShapes/Cube...`
       and `shadows: N of Minecraft's blocks cast HN shadows`, and visible shadows on HN's floor and walls.

0000. **Play-test night 5** (P0): ReShade.log must say NeighborCraft.fx compiled; hold objects and turn full circles,
      look up/down (`carry: hand ok ... fist Minecraft's (~115 ~66 ~-28 cm)`, never "default" for long, never FAR);
      right click the TV (nothing happens: no grab), doors/cabinets/switches (HN's action), props (ours); blocks in
      front of bright and dark backgrounds should no longer change brightness with what is behind them;
      `mesh path:` lines name the materials usable for drawing Minecraft blocks as HN meshes (next stage).

000. **Play-test night 4** (P0): pick up the radio and others; read `carry: class chain of ...` (is the Item/Key rule
     right?), `carry: 360 check of ...` (continuous?) and any `carry: hand FAR`; turn a full circle holding things;
     look at leaves/glass/iron bars/trapdoors placed in HN (no longer see-through; glass stays translucent); hotbar
     icons should be the objects' pictures. If the 360 check says continuous but the object still drifts in game,
     something outside the transform moves it: the FAR lines name where it was.

00. **Play-test night 3** (P0): pick up several props (video recorder, vase, chairs, mugs, anything multi-part): log
    `carry: pickup started` / `carry: what the look meets isn't carried: <reason>` names any that still go to HN; look
    up/down/turn while holding (the object should stay on the fist); stand against a wall holding it (it must not
    vanish); hotbar: `carry: icon for <class> in ...` (HN) and `carry: icon for ... (64x64)` (Minecraft), or `carry: no
    icon for <class>: <why>`; Ctrl+Q a block stack; sprint-jump downhill (no MIDAIR_STOP away from teleports). Tune
    `dlls/NeighborCraft_grips.txt` from what the hand looks like.

0. **Play-test night 2** (P0): hold right click on the video recorder / a chair with an empty hand (our pickup,
   never HN's grab; log `carry: pickup started`), with a sword (refused, no grab), TV/switch (HN's action), place,
   Q-throw; sprint-jump down the road (recorder: no MIDAIR_STOP); doors and pushing chairs while standing in them
   (no POSE_FORCED; log `collision: ... kept out`). Open question: a level start's teleport before HN's floor arrives
   can drop Steve into a block-to-be (seen only in the test tools so far).

1. **Run the newest build once** (P1). Check: sprint and sprint-jump down Act 1's road (no stops; `waiting for HN's
   world` lines may still appear, they must no longer stop Steve), how the slope feels, opening kitchen stands and
   drawers while standing close (no crawl). Carry has never been exercised. Steps: Act 1 → open and close doors
   while standing in the doorway (watch for crawling) → hold right click on a mug or chair → hotbar → right click to
   place → pick it up again → Q to throw.
2. **Targeting: door bounds fallback fires through floors** (P1). Repro: aim at the carpet or floor in a doorway and
   right click; the log shows `carpet_16 ... HN's action on Door_inward_2092 (bounds)`. Likely fix: only use the
   fallback when the hit is on a vertical surface or above knee height, or drop it (the door leaf itself is hit
   directly in practice).
3. **TV power / light switches** (P2). Find out what HN's action does on `BP_TV` and `BP_Switcher` (Tumbler).
   Decide whether the TV is a carry target or a toggle (today it's a carry target). F12 shows
   `OnIsActionComponent` answers live.
4. **Fresh Act 1 item glitch** (P2). The cutscene gate should have fixed it (the kit was being handed out during the
   cutscene). Confirm with the user.
5. **Doors stuck open** (P2). Expected fixed; confirm with repeated open/close while standing close. If not, read
   the door blueprint (`Door_inward`: `EDoorState`, `GetCurrentForce`, `m_fDefaultForce`, a `MeshDoor` hit handler
   with `LaunchCharacter`).
6. **Stairs: ~11% sprint cost** (P3). Minecraft's step-up takes the lowest helping step. Could try a second step-up
   within the tick (`EntityMoveMixin`).
7. **Carry follow-ups:** per-item icons (HN has `/Game/UMG/Inventory/inventory_icon/*`; reading them at runtime
   would need texture export), hand/throw animation, saving across levels (tokens respawn by class if the class is
   loaded).

## 7. Recent commits
- `1a830a4`: crawl fix (`HostBlock.shape` empty fallback), static probe ignores moving parts, carry system.
- `ed066d6`: inventory buttons, FlatActor interactions, shutdown crash, logging deadlock, watchdog, cutscene gate,
  slopes/stairs, road fall, shape builder, drive test, symbolizer.
- `56590a8` and earlier: moving-part collision, recorder, compositing, movement audits (see `git log`).

The working tree was clean at the time of writing. Nothing is running.

## 8. Diagnostics
- **Collision recorder** (`CollisionRecorder.java`):
  - Always on. F12 adds the debug view and records every kind of event; without it only blocked steps, failed
    steps, lost speed or sprint are kept.
  - Output: `mc/run/neighborcraft-collisions/session-*.jsonl`. Each line is an event with the 40 ticks before and 20
    after. A tick holds pos, attempt, resolved, vel, ground, collision flags, pose, input, bb and the boxes around.
  - Report: `python tools/collision_report.py [session] [outdir]` (prints the first 17 events and writes PNGs).
    Small ad-hoc Python over the JSON lines worked best (classify blockers, find pose transitions).
- **Logs:**
  - HN: `...\Win64\ue4ss\UE4SS.log`, overwritten each launch. Useful lines: `right click: …`, `physics: …`,
    `carry: …`, `frames: …`, `cutscene…`, `Steve is under HN's world` (with column diagnostics), `HN's capsule…`.
  - MC: the lab's `mc-client.log`. Useful lines: `HN's world: N blocks …`, `waiting for HN's world…`,
    `Can't keep up!`, `screen click…`, `carry: …`, `shape check`.
- **Dumps:**
  - Crash dumps: `%LOCALAPPDATA%\CrashDumps` and the UE4 `Saved\Crashes`.
  - `python tools/crashinfo.py <dmp>`, then `python tools/symbolize.py build\...\NeighborCraftMod.dll <offsets…>`.
    This needs the PDB of the **same** build.
  - Freeze dumps are written next to the DLL.
- **Other tools:** `tools/cmd.py` (Minecraft commands over the link; HN must be closed or it talks to the game's
  Minecraft), `inventorytest.py`, `linktest.py`.

## 9. Item system (carry) design: BUILT, untested
- **Eligible:** `pickup_target()` accepts a loose physics object that is its own actor's root, simulates physics,
  isn't `hinged_part()`, and isn't `interactive_actor()`. HN story items with an InteractiveComponent (keys…) keep
  HN's own E pickup.
- **Hold:** right click on such an object starts `g_pickup`. After `kPickupHold` = 350 ms with the target still under
  the crosshair it calls `complete_pickup()`; the action bar shows progress. Releasing early or looking away cancels.
- **Transaction:**
  1. HN hides the actor (`SetActorHiddenInGame`, collision off, `SetSimulatePhysics` off) and sends
     `pickup{id,name,cls,kg}`.
  2. `Carry.give` puts a `hn_object` token (`ITEM_NAME` = name; `CUSTOM_DATA` = id, cls, kg) into the inventory and
     answers `picked{ok}`.
  3. On failure (inventory full) HN restores the object.
  4. Place and throw remove the token only after HN put the object back (`consume`).
- **Held:** Minecraft reports the selected token as `held`. `carry_tick()` shows that same actor, scaled to ≤ ~35 cm,
  at the hand position in front of HN's camera (real mesh and HN lighting). The token's item model is `empty` in
  hand contexts, so Minecraft draws nothing there.
- **Place:** right click → `carry_place()`. Trace from the eyes, back off the surface by the object's half size, check
  with `BoxOverlapComponents` (up to 3 lifts of 10 cm), then `release()`. If there's no room: a message, and the item
  is kept.
- **Throw:** Q → `carry_throw()`. Spawned in front of Steve (half his width + its extent), checked for overlap,
  velocity `clamp(1100/(1+m/600), 600, 1100)` cm/s + 200 up. A token dropped from the inventory screen comes back
  and HN throws the object (`throwreq`).
- **Other level:** the hidden actor dies with its level. `carried_for()` spawns a new one from the stored class path
  if that class is loaded; otherwise it's logged and the token stays.

## 10. Known limitations
- Testing needs the user to play: HN can't be driven from here. The only automated test is `tools/drivetest.py`
  (Minecraft side only).
- HN is a custom UE 4.20 build:
  - UE4SS hash lookups are unreliable (`bForceGUObjectArrayForIteration = true`);
  - never cache `UObject*` across levels;
  - no debug drawing in Shipping, so debugging goes through logs and the action-bar readout.
- Minecraft doesn't have HN's models, so carried objects are drawn by HN, not as Minecraft items.
- Block mining *inside* HN geometry (clipping triangles against a block volume) is not started. It would need
  runtime mesh editing in HN; unexplored.

## 11. Next steps (ordered)
1. One play session on HEAD (§6.1). Read `UE4SS.log` (`carry:`, `right click:`, `frames:`) and the newest recorder
   session (POSE_FORCED should be gone).
2. Fix the door-bounds targeting fallback (§6.2).
3. TV and switch behaviour (§6.3) with F12 readouts.
4. Carry polish from the play results: hand position and scale, placement rules, icons.
5. Remaining P3 items, then rendering work.

## 12. Verification status of this handoff
2026-10-11 night: shadowtest (Steve's pose, sneak, dropped item, particles, sword silhouette), minetest, handtest, itemtest, carryuitest pass; in game (driven by me): Steve's and blocks' native shadows with ReShade off, sharper after the cascade change, held object in Steve's hand in third person, no green tint on cobblestone. HN DLL `8814de2e...` installed.

2026-10-11 evening: keys unlock natively and are kept ([game] log); native block and Steve shadows seen with ReShade off ([game] screenshots); carryuitest, minetest (twice), handtest, itemtest, turntest pass; HN DLL `d16625f4...` installed.

2026-10-11 afternoon: minetest (new, fails before the fix, OK after), handtest, itemtest pass; turntest STEADY; HN DLL `f3dd464e...` installed (md5 checked); interactions, hotbar rule, throw charge and shadows not run in game.

2026-10-11: opaque classification, handtest, itemtest, turntest pass; HN DLL `a5f38254...` and grips installed; pickup rule, held size and shadow proxies not run in game.

Night 5: turntest STEADY, relight_check, handtest/itemtest unchanged; HN DLL + shader installed (md5 checked); nothing of night 5 run in game.

Night 4: rendertest before/after, itemtest, handtest, real-icon hotbar screenshot, drive tests pass; HN DLL md5 `baaea859...` installed; held-object attachment, 360 check and radio rule not run in game.

Night 3: itemtest, handtest and drive tests pass on the final code; HN DLL md5 `1c8984e8...` installed; nothing of night 3 run in game.

Night 2: drive tests and handtest pass on the final code; HN DLL md5 `20224a47…` installed; nothing of night 2 run in game.

Late update: `tools/handtest.py` passes (4 pickup cases, fist on screen where drawn); drive tests unchanged; HN DLL md5 `9e7ff2de…` installed, not run in game.

Night update: drive tests of the landing fix (MODLOG); HN DLL md5 `b3ffa30e…` installed, not run in game.

Evening update:
- Drive tests on the new Minecraft code (`tools/drivetest.py --downhill`, with and without `--stream`/`--jump`, and
  the original ramp/stairs course: ramp 5.35, stairs 5.41 m/s) as in MODLOG; MC compiles.
- HN DLL built and installed, md5 `6b85de43…` matches. Not run in game.

Earlier:
- Ran: MC compile of HEAD (0 errors). The HN build of HEAD succeeded earlier (2026-10-10 01:59).
- Installed and checked: installed `main.dll` md5 = built DLL md5 (`bd2d0f90…`), 2026-10-10.
- Not run: any in-game test of `1a830a4` (crawl fix, door-remnant fix, carry).

## 13. Render pipeline (state, night 3)
- Owners: HN renders its world (UE 4.20, D3D11) with HN's camera placed at Minecraft's camera each tick
  (`minecraft_view`); the held HN object is an HN actor in that world (now 25 cm from the eye). Minecraft renders its
  world, its hand and its GUI into four layers (`FrameExporter`: world colour, depth, hand, overlay; GPU->CPU copies
  into shared memory `Local\NeighborCraftFrame`); `compositor.cpp` (ReShade add-on in the same DLL) uploads them and
  `NeighborCraft.fx` composites: world by depth (re-projected onto HN's newer camera), contact shadows, bloom, the
  hand relit with HN's light at its pixels, the overlay on top.
- Done this round: correct depth for the held object (near-eye placement), same-frame camera for it, item icons
  (one GUI path), HN's object pictures (capture/export path, untested in game).
- Next candidates, by evidence needed: (1) see the held object and icons in game and tune grips; (2) the four
  per-frame GPU->CPU readbacks are the main cost: the hand and overlay layers could be skipped when unchanged or
  empty (needs frame timing from `frames:` lines first); (3) GPU sharing (D3D11/GL interop) instead of readbacks.

## 14. Night 4: files changed and sources read
- Changed: `hn/NeighborCraftMod/src/bridge.cpp` (pickup_target: InteractiveComponent props + class-chain log;
  carry_hand_follow on the view camera, carry_detach, carry_sweep, held_in_camera; icon path before narrow()),
  `mc/src/client/java/dev/neighborcraft/client/CarryIcons.java` (coverage-alpha inversion),
  `mc/src/client/java/dev/neighborcraft/client/mixin/ShaderSourceMixin.java` (new: opaque alpha for solid/cutout),
  `mc/src/client/resources/neighborcraft.client.mixins.json`, `tools/rendertest.py` (new), MODLOG, this file.
- The visible block fix is `ShaderSourceMixin` alone (rendertest: cutout 16.2% -> 3.2% low-alpha pixels).
- SkyCraft read: README.md, docs/DESIGN.md, protocol/skycraft_protocol.h, fabric/src/client/java/dev/skycraft/client/
  render/WorldExporter.java (and the file list of skse/src/WorldRender.cpp, FrameExporter.java). MIT; nothing copied.

## 15. Night 5: files changed and sources read
- Changed: `mc/src/client/java/dev/neighborcraft/client/CarryHand.java` (the fist projected on the camera's axes: the
  held-object fix), `hn/NeighborCraftMod/src/bridge.cpp` (kNotCarriedNotGrabbed: loose TVs/lamps get no HN action;
  mesh_path_probe), `hn/shaders/NeighborCraft.fx` (host_light_near; relight uses it; the hand takes the overall light),
  `tools/turntest.py`, `tools/relight_check.py` (new), MODLOG, this file.
- SkyCraft read: README.md, docs/DESIGN.md, protocol/skycraft_protocol.h, fabric/src/client/java/dev/skycraft/client/
  render/WorldExporter.java, skse/src/WorldRender.cpp (PSMain, DrawMesh, RenderPasses, DrawInFrame, RenderWorldHook,
  render-ring cases). MIT; nothing copied.
- Next for rendering: from the `mesh path:` log, pick a masked, lit HN material with a texture parameter; then stage 2:
  Minecraft exports player-placed block sections (as SkyCraft's WorldExporter: Minecraft's own block renderer, per
  vertex cutout/translucent) and its block atlas (PNG); HN loads the atlas (ImportFileAsTexture2D), makes a dynamic
  instance with it, and builds ProceduralMeshComponent sections at the blocks' places (HN's depth, lighting, shadows
  and TAA then apply natively; the picture path stays for the hand, GUI and entities).

## 16. 2026-10-11: files changed
- `hn/NeighborCraftMod/src/bridge.cpp`: pickup_target rule order + PropShape/prop_shape; `carry: HN took ...`
  diagnostic; held size (kHeldHalf, grip scale); shadow stand-ins (shadow_cube, spawn_shadow, g_shadows,
  apply_blocks with `opaque`, destroy_colliders).
- `hn/NeighborCraft_grips.txt`: `scale` column.
- `mc/src/main/java/dev/neighborcraft/WorldBridge.java`: block changes 0/1/2, `opaque` list.
- MODLOG, this file.

## 17. 2026-10-11 afternoon: files changed
- `hn/NeighborCraftMod/src/bridge.cpp`: electric() + hn_action_tap()/release_hn_action() (E as a tap); right click
  on electric devices; tap-activates for carryable devices; pickup rule 5 without the one-part limit (<= 90 cm);
  charged throw (g_throwCharge, carry_throw charge); shadow-only boxes (spawn_shadow_box, SetRenderInMainPass false),
  light_census, Steve's shadow (steve_shadow_tick).
- `mc/.../mixin/MinecraftAttackMixin.java`: hold-to-mine (isMouseGrabbed redirect in handleKeybinds).
- `mc/.../ClientInput.java`: `ungrab` test message.
- `tools/minetest.py`: hold-to-mine test.
- MODLOG, this file.

## 18. 2026-10-11 evening: files changed
- `hn/NeighborCraftMod/src/bridge.cpp`: native_apply / flat_mode / item_use (HN's bound apply method); keys and
  usable items carried (apply_component, item_name with codes); TV/lamp eligibility (m_bPickUp); fist ids and
  shown-on-own-fist; CarryBar progress messages; item life (suspend_activity/resume_activity); icon framing;
  stand-ins movable + hidden + bCastHiddenShadow, F6 toggle, light census, lend_shadows_tick, cleanup
  (shadow_extras_reset).
- `hn/shaders/NeighborCraft.fx`: contact shadow removed.
- `mc/.../CarryBar.java`, `mixin/HudCarryBarMixin.java` (new), `CarryHand.java` + `FirstPersonCarryMixin.java`
  (fist id), `CarryIcons.java` (framing), `ClientInput.java` (bar, barq), mixins json.
- `tools/carryuitest.py`, `tools/iconcheck.py` (new), `tools/minetest.py` (server-side block check).
- Reverse-engineering scripts (not in the repo, the lab's scratch): header dump in `<lab>/sdk/CXXHeaderDump`.

## 19. 2026-10-11 night: files changed
- `mc/.../ShadowExport.java` (new) + accessors `LivingEntityRendererInvoker`, `ModelPartAccessor`,
  `ParticleEngineAccessor`, `ParticleGroupAccessor`, `ParticleAccessor`, `SingleQuadParticleAccessor`,
  `ItemStackRenderStateAccessor`, `LayerRenderStateAccessor`, `SpriteContentsAccessor`; `GameRendererMixin`
  (export after each frame); mixins json.
- `hn/.../bridge.cpp`: "shadows" message, ShadowPool / box_transform / shadow_boxes_tick, Steve's approximation
  steps aside, carry_hand_third_person (+ its diagnostics), the sun's cascades in light_census.
- `hn/shaders/NeighborCraft.fx`: PS_Grey / light_colour (grey-surface light colour, global).
- `tools/shadowtest.py`, `tools/tintcheck.py` (new).
