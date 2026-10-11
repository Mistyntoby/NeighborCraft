"""Tests Minecraft's side of items without Hello Neighbor: acts as the host over the link, stands Steve on a floor in
drive mode, and checks

  1. Q drops one item of the selected stack, Ctrl+Q ("stack": true) the whole stack, an empty slot drops nothing,
     and a dropped stack is picked up again (walking over it) with its full count;
  2. the icons of Hello Neighbor's objects: two tokens of different classes get different fallback icons, and a
     picture from HN ({"t":"icon"}, a run-length encoded Radiance file as UE writes them) replaces one class's
     fallback; a screenshot of the hotbar goes to mc/run/screenshots/items-hotbar.png.

    python tools/itemtest.py

Start Minecraft first (gradlew runClient in mc/) with Hello Neighbor closed.
"""
import asyncio
import json
import math
import os
import struct
import subprocess
import sys
import time

import websockets

PORT = 25620
Y = 140
OX, OZ = 80, 80


def radiance(path, size=64):
    """A test picture: a red ring on black, RLE scanlines (as Unreal's HDR writer makes them)."""
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (size, size))
        for y in range(size):
            planes = [[], [], [], []]
            for x in range(size):
                r = math.hypot(x - size / 2, y - size / 2)
                on = size * 0.25 < r < size * 0.42
                rgbe = (200, 30, 30, 128) if on else (0, 0, 0, 0)
                for i in range(4):
                    planes[i].append(rgbe[i])
            f.write(bytes([2, 2, size >> 8, size & 255]))
            for plane in planes:
                i = 0
                while i < size:  # literal runs of up to 128
                    n = min(128, size - i)
                    f.write(bytes([n]) + bytes(plane[i:i + n]))
                    i += n


async def main():
    async with websockets.connect(f"ws://127.0.0.1:{PORT}", max_size=None) as ws:
        print("hello:", json.loads(await ws.recv()).get("t"))
        await ws.send(json.dumps({"t": "view", "w": 1280, "h": 720}))
        end = time.time() + 180
        while time.time() < end:
            try:
                if json.loads(await asyncio.wait_for(ws.recv(), 5)).get("t") == "ready":
                    break
            except asyncio.TimeoutError:
                pass
        await ws.send(json.dumps({"t": "clear"}))
        cells = [c for x in range(-6, 7) for z in range(-6, 12) for c in (x + OX, Y, z + OZ, 1)]
        await ws.send(json.dumps({"t": "shape", "c": cells, "m": [], "f": [], "g": []}))
        await asyncio.sleep(1.0)
        await ws.send(json.dumps({"t": "tp", "p": [OX + 0.5, Y + 1.01, OZ + 0.5]}))
        await ws.send(json.dumps({"t": "cmd", "c": "clear @a"}))
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=item]"}))
        state = {"frame": 0, "move": 0}
        msgs = []

        async def host():
            while True:
                state["frame"] += 1
                await ws.send(json.dumps({"t": "cam", "drive": True, "f": state["frame"], "r": [0.0, 0.0, 0.0], "fov": 70.0}))
                await ws.send(json.dumps({"t": "move", "m": state["move"]}))
                try:
                    while True:
                        m = json.loads(await asyncio.wait_for(ws.recv(), 0.001))
                        if m.get("t") in ("inv", "picked", "held"):
                            msgs.append(m)
                except (asyncio.TimeoutError, json.JSONDecodeError):
                    pass
                await asyncio.sleep(1 / 60)

        task = asyncio.create_task(host())

        async def inv():
            msgs.clear()
            await ws.send(json.dumps({"t": "inv"}))
            for _ in range(60):
                await asyncio.sleep(0.05)
                got = [m for m in msgs if m.get("t") == "inv"]
                if got:
                    return got[-1]
            return None

        async def drop(stack):
            await ws.send(json.dumps({"t": "key", "k": "drop", "down": True, "stack": stack}))
            await ws.send(json.dumps({"t": "key", "k": "drop", "down": False, "stack": stack}))
            await asyncio.sleep(0.5)

        ok = True
        await asyncio.sleep(1.0)
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a hotbar.0 with minecraft:cobblestone 64"}))
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a hotbar.1 with minecraft:oak_planks 10"}))
        await ws.send(json.dumps({"t": "slot", "n": 0}))
        await asyncio.sleep(0.6)
        i0 = await inv()
        print("1. start:", i0["slots"][:3])
        await drop(False)
        i1 = await inv()
        print("   Q:", i1["slots"][0], "(expected 63)")
        ok &= i1["slots"][0][1] == 63
        await drop(True)
        i2 = await inv()
        print("   Ctrl+Q:", i2["slots"][0], "(expected empty)")
        ok &= i2["slots"][0][1] == 0
        await drop(True)
        await drop(False)
        i3 = await inv()
        print("   Q / Ctrl+Q on the empty slot:", i3["slots"][:2], "(slot 2 untouched: 10 planks)")
        ok &= i3["slots"][1] == ["minecraft:oak_planks", 10]
        # walk over the dropped stack: Minecraft's pickup delay is 2 s
        await asyncio.sleep(1.8)
        state["move"] = 1
        await asyncio.sleep(1.2)
        state["move"] = 0
        await asyncio.sleep(0.8)
        i4 = await inv()
        total = sum(s[1] for s in i4["slots"] if s[0] == "minecraft:cobblestone")
        print("   picked up again: cobblestone in the hotbar:", total, "(expected 64: the single Q drop and the stack)")
        ok &= total == 64

        print("2. icons")
        await ws.send(json.dumps({"t": "cmd", "c": "clear @a"}))
        await ws.send(json.dumps({"t": "slot", "n": 0}))
        await asyncio.sleep(0.5)
        for n, (oid, cls) in enumerate((("i1", "/Game/Test/BP_Mug.BP_Mug_C"), ("i2", "/Game/Test/BP_Vase.BP_Vase_C"), ("i3", "/Game/Test/BP_Chair.BP_Chair_C"))):
            await ws.send(json.dumps({"t": "slot", "n": n}))
            await asyncio.sleep(0.3)
            await ws.send(json.dumps({"t": "pickup", "id": oid, "name": cls.split(".")[-1], "cls": cls, "kg": 50}))
            await asyncio.sleep(0.6)
        await ws.send(json.dumps({"t": "slot", "n": 4}))
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({"t": "shot", "n": "items-hotbar-fallback.png"}))
        path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "mc", "run", "test-icon.hdr"))
        radiance(path)
        await ws.send(json.dumps({"t": "icon", "key": "/Game/Test/BP_Vase.BP_Vase_C", "file": path.replace("\\", "/")}))
        await asyncio.sleep(0.6)
        await ws.send(json.dumps({"t": "shot", "n": "items-hotbar.png"}))
        await asyncio.sleep(0.6)
        i5 = await inv()
        print("   hotbar:", i5["slots"][:4], "selected", i5["sel"])
        for oid in ("i1", "i2", "i3"):
            await ws.send(json.dumps({"t": "consume", "id": oid}))
        await asyncio.sleep(0.6)
        i6 = await inv()
        print("   after consume:", i6["slots"][:4])
        ok &= all(s[1] == 0 for s in i6["slots"][:3])
        task.cancel()
        print()
        print("results:", "OK" if ok else "UNEXPECTED", "(see mc/run/screenshots/items-hotbar*.png for the icons)")


if __name__ == "__main__":
    running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HelloNeighbor-Win64-Shipping.exe"], capture_output=True, text=True).stdout
    if "HelloNeighbor" in running:
        sys.exit("Hello Neighbor is running: close it first (this test would drive the game's own Minecraft)")
    asyncio.run(main())
