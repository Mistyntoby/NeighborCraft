"""Turns Steve through three full turns (looking up and down too) while he holds one of Hello Neighbor's objects (without Hello Neighbor: this
script is the host), with a server teleport half way (it writes the player's yaw wrapped, as teleports and position
packets do), and measures where Minecraft's carrying fist is in the camera's frame ({"t":"hand"}, "l": right, up,
forward). That must not change with the way Steve faces: the largest angle between any fist direction and the first one
is printed. The 22:41 session had the fist anywhere around the camera (the hand's sway chasing a yaw jump).

    python tools/turntest.py

Start Minecraft first (gradlew runClient in mc/) with Hello Neighbor closed.
"""
import asyncio
import json
import math
import subprocess
import sys
import time

import websockets

PORT = 25620
Y = 140
OX, OZ = 160, 160


async def main():
    async with websockets.connect(f"ws://127.0.0.1:{PORT}", max_size=None) as ws:
        print("hello:", json.loads(await ws.recv()).get("t"))
        end = time.time() + 180
        while time.time() < end:
            try:
                if json.loads(await asyncio.wait_for(ws.recv(), 5)).get("t") == "ready":
                    break
            except asyncio.TimeoutError:
                pass
        await ws.send(json.dumps({"t": "clear"}))
        cells = [c for x in range(-4, 5) for z in range(-4, 5) for c in (x + OX, Y, z + OZ, 1)]
        await ws.send(json.dumps({"t": "shape", "c": cells, "m": [], "f": [], "g": []}))
        await asyncio.sleep(1.0)
        await ws.send(json.dumps({"t": "tp", "p": [OX + 0.5, Y + 1.01, OZ + 0.5]}))
        await ws.send(json.dumps({"t": "cmd", "c": "clear @a"}))
        hands = []
        state = {"frame": 0, "yaw": 0.0, "pitch": 0.0, "record": True}

        async def frame():
            state["frame"] += 1
            await ws.send(json.dumps({"t": "cam", "drive": True, "f": state["frame"], "r": [state["yaw"], state["pitch"], 0.0], "fov": 70.0}))
            await ws.send(json.dumps({"t": "move", "m": 0}))
            try:
                while True:
                    m = json.loads(await asyncio.wait_for(ws.recv(), 0.001))
                    if m.get("t") == "hand" and "l" in m and state["record"]:
                        hands.append((state["yaw"], m["l"]))
            except (asyncio.TimeoutError, json.JSONDecodeError):
                pass
            await asyncio.sleep(1 / 60)

        for _ in range(60):
            await frame()
        await ws.send(json.dumps({"t": "slot", "n": 0}))
        await ws.send(json.dumps({"t": "pickup", "id": "turn1", "name": "Test", "cls": "/Game/Test/BP_Turn.BP_Turn_C", "kg": 50}))
        for _ in range(90):  # the equip motion settles
            await frame()
        hands.clear()
        teleported = False
        while state["yaw"] < 1080.0:
            state["yaw"] += 6.0  # 360 degrees a second, looking up and down (35 degrees) on the way
            state["pitch"] = 35.0 * math.sin(math.radians(state["yaw"] * 0.75))
            if not teleported and state["yaw"] >= 540.0:
                teleported = True
                await ws.send(json.dumps({"t": "cmd", "c": f"tp @a {OX + 0.5} {Y + 1.01} {OZ + 0.5}"}))
            await frame()
        state["pitch"] = 0.0
        for _ in range(30):
            await frame()
        state["record"] = False  # the token goes next: the arm lowers (the equip motion), not part of the turn
        await ws.send(json.dumps({"t": "consume", "id": "turn1"}))
        for _ in range(20):
            await frame()
        if len(hands) < 10:
            print("too few fist positions:", len(hands))
            return

        def unit(v):
            n = math.sqrt(sum(c * c for c in v))
            return [c / n for c in v]

        # against the middle direction (the hand's own sway lags a few degrees behind a fast turn, as in Minecraft)
        ref = unit([sorted(h[1][i] for h in hands)[len(hands) // 2] for i in range(3)])
        worst, at = 0.0, None
        for yaw, l in hands:
            u = unit(l)
            ang = math.degrees(math.acos(max(-1.0, min(1.0, sum(a * b for a, b in zip(u, ref))))))
            if ang > worst:
                worst, at = ang, (yaw, l)
        print(f"{len(hands)} fist positions over three turns (and a teleport at 540 degrees)")
        print(f"  first: right {hands[0][1][0]:+.2f} up {hands[0][1][1]:+.2f} forward {hands[0][1][2]:+.2f} blocks")
        print(f"  largest change of its direction in the camera's frame (from the middle one): {worst:.1f} degrees (at yaw {at[0]:.0f}: {at[1]})")
        print("result:", "STEADY" if worst < 6.0 else "SWINGS (the hand moves around the camera)")
        if "--trace" in sys.argv:
            for yaw, l in hands[::4]:
                print(f"    yaw {yaw:7.1f}: fist angle around the camera {math.degrees(math.atan2(l[0], l[2])):7.1f}, up {l[1]:+.2f}")


if __name__ == "__main__":
    running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HelloNeighbor-Win64-Shipping.exe"], capture_output=True, text=True).stdout
    if "HelloNeighbor" in running:
        sys.exit("Hello Neighbor is running: close it first (this test would drive the game's own Minecraft)")
    asyncio.run(main())
