"""Hold-to-mine over the link, without Hello Neighbor (this script is the host): Steve faces a dirt block (a bare hand
breaks it in ~0.75 s); the host sends the attack key as Hello Neighbor does (down on press, up on release).

  tap (0.05 s)            -> the block stays (one swing, no break)
  hold 0.4 s, let go      -> the block stays (breaking stops on release)
  hold 1.6 s              -> the block breaks (its dirt ends in the inventory)
  after letting go        -> a second dirt block put there stays

    python tools/minetest.py

Start Minecraft first (gradlew runClient in mc/) with Hello Neighbor closed.
"""
import asyncio
import json
import subprocess
import sys
import time

import websockets

PORT = 25620
Y = 140
OX, OZ = 240, 240


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
        cells = [c for x in range(-3, 4) for z in range(-3, 6) for c in (x + OX, Y, z + OZ, 1)]
        await ws.send(json.dumps({"t": "shape", "c": cells, "m": [], "f": [], "g": []}))
        await asyncio.sleep(1.0)
        await ws.send(json.dumps({"t": "tp", "p": [OX + 0.5, Y + 1.01, OZ + 0.5]}))
        await ws.send(json.dumps({"t": "cmd", "c": "clear @a"}))
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=item]"}))
        state = {"frame": 0}
        msgs = []

        async def frames(seconds):
            end = time.time() + seconds
            while time.time() < end:
                state["frame"] += 1
                await ws.send(json.dumps({"t": "cam", "drive": True, "f": state["frame"], "r": [0.0, 0.0, 0.0], "fov": 70.0}))
                await ws.send(json.dumps({"t": "move", "m": 0}))
                try:
                    while True:
                        m = json.loads(await asyncio.wait_for(ws.recv(), 0.001))
                        if m.get("t") == "inv":
                            msgs.append(m)
                except (asyncio.TimeoutError, json.JSONDecodeError):
                    pass
                await asyncio.sleep(1 / 60)

        async def dirt_count():
            msgs.clear()
            await ws.send(json.dumps({"t": "inv"}))
            await frames(0.4)
            inv = msgs[-1] if msgs else None
            return sum(s[1] for s in inv["slots"] if s[0] == "minecraft:dirt") if inv else -1

        async def place():
            await ws.send(json.dumps({"t": "cmd", "c": f"setblock {OX} {Y + 2} {OZ + 2} minecraft:dirt"}))
            await frames(0.4)

        async def attack(seconds):
            await ws.send(json.dumps({"t": "key", "k": "attack", "down": True}))
            await frames(seconds)
            await ws.send(json.dumps({"t": "key", "k": "attack", "down": False}))
            await frames(1.5)  # the drop is picked up

        await frames(1.5)
        # as in the game: Minecraft's window is hidden behind Hello Neighbor's and doesn't have the mouse grabbed
        await ws.send(json.dumps({"t": "ungrab"}))
        await frames(0.3)
        ok = True
        await place()
        await attack(0.05)
        n = await dirt_count()
        print(f"  tap:             dirt in the inventory {n} (expected 0: the block stays)")
        ok &= n == 0
        await attack(0.4)
        n = await dirt_count()
        print(f"  hold 0.4 s:      dirt in the inventory {n} (expected 0: breaking stops on release)")
        ok &= n == 0
        await attack(1.6)
        n = await dirt_count()
        print(f"  hold 1.6 s:      dirt in the inventory {n} (expected 1: broken)")
        ok &= n == 1
        await place()
        await frames(1.5)
        n = await dirt_count()
        print(f"  after release:   dirt in the inventory {n} (expected 1: the new block isn't mined)")
        ok &= n == 1
        print("results:", "OK" if ok else "UNEXPECTED")


if __name__ == "__main__":
    running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HelloNeighbor-Win64-Shipping.exe"], capture_output=True, text=True).stdout
    if "HelloNeighbor" in running:
        sys.exit("Hello Neighbor is running: close it first (this test would drive the game's own Minecraft)")
    asyncio.run(main())
