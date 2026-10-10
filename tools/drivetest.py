"""Tests Minecraft's drive mode without Hello Neighbor: acts as the host over the link, gives Minecraft a floor of
HN-style host blocks with a gentle ramp in it (floor heights rising 0.8 cm per 1/8 block, like the one the collision
recorder caught after a curb, z 0..4) and stairs (11 cm risers, 25 cm treads, z 6..8, then a drop), puts Steve on it
in drive mode and sprints him across, then reports his speed per stretch and per section, and whether Minecraft kept
answering.

    python tools/drivetest.py [seconds] [x z]

x z: where in Minecraft's world to build it (default next to spawn); far from spawn, in chunks that don't exist yet,
the way HN's levels are (Act 1's road is at about -75 -112).

Start Minecraft first (tools/start_minecraft.bat, or gradlew runClient in mc/) with Hello Neighbor closed.
"""
import asyncio
import json
import math
import random
import subprocess
import sys
import time

import websockets

PORT = 25620
Y = 140  # floor blocks (Minecraft y); Steve stands on y 141 + the ramp
OX, OZ = 0, 0  # where it is built
BURST = False  # --burst: also HN's start-of-level burst of detailed shapes
LATE = 0.0  # --late S: the server is kept busy ~S seconds when Steve's floor is sent (it is placed late)


def floor_block(heights):
    """A host block with no box cells and the given 64 floor heights (blocks, 0..1)."""
    return "0" * 128, ",".join(str(int(round(h * 1000))) for h in heights)


async def main(seconds):
    async with websockets.connect(f"ws://127.0.0.1:{PORT}", max_size=None) as ws:
        hello = json.loads(await ws.recv())
        print("hello:", hello.get("t"))
        await ws.send(json.dumps({"t": "view", "w": 1280, "h": 720}))
        # wait for the world
        end = time.time() + 180
        while time.time() < end:
            try:
                m = json.loads(await asyncio.wait_for(ws.recv(), 5))
            except asyncio.TimeoutError:
                continue
            if m.get("t") == "ready":
                print("ready")
                break
        else:
            print("no ready")
            return
        await ws.send(json.dumps({"t": "clear"}))
        # floor: full blocks at Y, and at Y+1 a ramp along +z: each 1/8 column 0.8 cm higher (z 0..4)
        cells, masks, floors, ceilings = [], [], [], []
        for x in range(-4, 5):
            for z in range(-6, 12):
                cells += [x + OX, Y, z + OZ, 1]
                heights = None
                if 0 <= z < 4:  # the ramp
                    heights = [0.010 + (z * 8 + sz) * 0.008 for sx in range(8) for sz in range(8)]
                elif 6 <= z < 8:  # stairs: 11 cm risers, 25 cm treads
                    heights = [0.11 * ((z - 6) * 4 + sz // 2 + 1) for sx in range(8) for sz in range(8)]
                if heights:
                    hexmask, fl = floor_block(heights)
                    cells += [x + OX, Y + 1, z + OZ, 2]
                    masks.append(hexmask)
                    floors.append(fl)
                    ceilings.append("")
        # as HN does when a level starts: Steve is teleported there, then the world's shapes follow, and Minecraft
        # drives right away
        await ws.send(json.dumps({"t": "tp", "p": [OX + 0.5, Y + 1.01, OZ - 4.5]}))
        await asyncio.sleep(0.1)
        if LATE:
            # the server busy when the floor comes (as after a level's teleport): heavy work queued ahead of it, so
            # the floor is placed well after it was sent
            for i in range(int(LATE * 10)):
                block = "stone" if i % 2 == 0 else "air"
                await ws.send(json.dumps({"t": "cmd", "c": f"fill {OX - 15} 100 {OZ - 15} {OX + 15} 115 {OZ + 15} minecraft:{block}"}))
        await ws.send(json.dumps({"t": "shape", "c": cells, "m": masks, "f": floors, "g": ceilings}))
        if BURST:
            # what HN sends when a level starts: every block within 7 of Steve, 6 high, each its own detailed shape
            # (here walls of random sub-cells above the floor), in messages of ~300 blocks
            rng = random.Random(1)
            batch = ([], [], [], [])
            for x in range(-7, 8):
                for z in range(-11, 4):
                    for y in range(Y + 2, Y + 8):
                        cells_b, masks_b, floors_b, ceilings_b = batch
                        cells_b += [x + OX, y, z + OZ, 2]
                        masks_b.append("".join(rng.choice("0137f") for _ in range(128)))
                        floors_b.append("")
                        ceilings_b.append("")
                        if len(masks_b) >= 300:
                            await ws.send(json.dumps({"t": "shape", "c": cells_b, "m": masks_b, "f": floors_b, "g": ceilings_b}))
                            batch = ([], [], [], [])
            if batch[1]:
                await ws.send(json.dumps({"t": "shape", "c": batch[0], "m": batch[1], "f": batch[2], "g": batch[3]}))
        await asyncio.sleep(0.2)
        frame, start, last_pos = 0, time.time(), None
        samples = []
        last_seen = time.time()
        while time.time() - start < seconds:
            frame += 1
            await ws.send(json.dumps({"t": "cam", "drive": True, "f": frame, "r": [0.0, 0.0, 0.0], "fov": 70.0}))
            moving = 0.3 < time.time() - start < seconds - 1.0
            await ws.send(json.dumps({"t": "move", "m": (1 | 64) if moving else 0}))
            try:
                while True:
                    m = json.loads(await asyncio.wait_for(ws.recv(), 0.001))
                    if m.get("t") == "mcpos":
                        last_seen = time.time()
                        e = m["eye"]
                        samples.append((time.time(), [e[0] - OX, e[1], e[2] - OZ]))
            except (asyncio.TimeoutError, json.JSONDecodeError):
                pass
            if time.time() - last_seen > 3.0:
                print("Minecraft stopped answering at", round(time.time() - start, 1), "s")
                return
            await asyncio.sleep(1 / 60)
        # speed per 0.25 s, with z (along the run) and height
        print(f"{len(samples)} positions")
        i = 0
        while i < len(samples):
            j = i
            while j < len(samples) and samples[j][0] - samples[i][0] < 0.25:
                j += 1
            if j >= len(samples):
                break
            (t0, a), (t1, b) = samples[i], samples[j]
            v = math.hypot(b[0] - a[0], b[2] - a[2]) / (t1 - t0)
            print(f"  t {t0 - start:5.2f}  z {a[2]:6.2f}  eye y {a[1]:8.3f}  speed {v:5.2f} m/s")
            i = j
        # mean speed per section (sprinting the whole way: Minecraft's sprint is 5.6 m/s)
        for name, z0, z1 in (("flat before", -3.0, -0.5), ("ramp", 0.5, 3.5), ("flat between", 4.5, 5.8), ("stairs", 6.0, 7.8)):
            pts = [(t, p) for t, p in samples if z0 <= p[2] <= z1]
            if len(pts) > 2:
                (ta, pa), (tb, pb) = pts[0], pts[-1]
                print(f"  {name:13s} {math.hypot(pb[0] - pa[0], pb[2] - pa[2]) / max(1e-3, tb - ta):5.2f} m/s")


if __name__ == "__main__":
    # the link port is the game's while Hello Neighbor runs: this would talk to the player's Minecraft
    running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HelloNeighbor-Win64-Shipping.exe"], capture_output=True, text=True).stdout
    if "HelloNeighbor" in running:
        sys.exit("Hello Neighbor is running: close it first (this test would drive the game's own Minecraft)")
    if "--late" in sys.argv:
        i = sys.argv.index("--late")
        LATE = float(sys.argv[i + 1])
        del sys.argv[i:i + 2]
    if "--burst" in sys.argv:
        BURST = True
        sys.argv.remove("--burst")
    if len(sys.argv) > 3:
        OX, OZ = int(sys.argv[2]), int(sys.argv[3])
    asyncio.run(main(float(sys.argv[1]) if len(sys.argv) > 1 else 8.0))
