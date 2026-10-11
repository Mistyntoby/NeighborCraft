"""Minecraft's shadow boxes (ShadowExport.java) without Hello Neighbor (this script is the host): Steve stands on a floor
in drive mode, and the "shadows" messages are checked:

  1. standing: Steve's model gives at least 6 boxes (head, body, arms, legs), the highest box near his head height
     (~1.5-1.9 blocks above his feet), the lowest at his feet; the hand frame comes with them;
  2. sneaking: his highest box comes down (the model's own sneak pose);
  3. a dropped item: one more box, near where it lies;
  4. particles (a /particle burst): more boxes;
  5. a held sword: its sprite's silhouette, several boxes (one box made every item's shadow a square).

    python tools/shadowtest.py

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
OX, OZ = 120, 120


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
        await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=item]"}))
        state = {"frame": 0, "move": 0, "last": None}

        async def host():
            while True:
                state["frame"] += 1
                await ws.send(json.dumps({"t": "cam", "drive": True, "f": state["frame"], "r": [0.0, 0.0, 0.0], "fov": 70.0}))
                await ws.send(json.dumps({"t": "move", "m": state["move"]}))
                try:
                    while True:
                        m = json.loads(await asyncio.wait_for(ws.recv(), 0.001))
                        if m.get("t") == "shadows":
                            state["last"] = m
                except (asyncio.TimeoutError, json.JSONDecodeError):
                    pass
                await asyncio.sleep(1 / 60)

        task = asyncio.create_task(host())

        def boxes():
            m = state["last"] or {"b": []}
            b = m["b"]
            return [b[i:i + 12] for i in range(0, len(b) - 11, 12)], m.get("hand")

        def near_steve(bs):
            return [x for x in bs if abs(x[0] - (OX + 0.5)) < 1.0 and abs(x[2] - (OZ + 0.5)) < 1.0]

        def top(bs):
            # the highest point of each box: centre + |half-axes| along y
            return max(x[1] + abs(x[4]) + abs(x[7]) + abs(x[10]) for x in bs)

        def bottom(bs):
            return min(x[1] - abs(x[4]) - abs(x[7]) - abs(x[10]) for x in bs)

        ok = True
        await asyncio.sleep(2.5)
        bs, hand = boxes()
        st = near_steve(bs)
        feet = Y + 1.0
        print(f"1. standing: {len(bs)} boxes, {len(st)} at Steve; top {top(st) - feet:.2f} above his feet, bottom {bottom(st) - feet:+.2f}; hand frame {'yes' if hand else 'no'}")
        ok &= len(st) >= 6 and 1.5 < top(st) - feet < 1.95 and abs(bottom(st) - feet) < 0.15 and hand is not None
        standing_top = top(st)
        state["move"] = 32  # sneak
        await asyncio.sleep(1.0)
        bs, _ = boxes()
        st = near_steve(bs)
        print(f"2. sneaking: top {top(st) - feet:.2f} above his feet (standing {standing_top - feet:.2f})")
        ok &= top(st) < standing_top - 0.1
        state["move"] = 0
        await asyncio.sleep(0.8)
        n0 = len(boxes()[0])
        await ws.send(json.dumps({"t": "cmd", "c": f"summon item {OX + 2.5} {Y + 1.2} {OZ + 2.5} {{Item:{{id:\"minecraft:stone\",count:1}},PickupDelay:32767}}"}))
        await asyncio.sleep(1.5)
        bs, _ = boxes()
        item = [x for x in bs if abs(x[0] - (OX + 2.5)) < 0.5 and abs(x[2] - (OZ + 2.5)) < 0.5]
        print(f"3. a dropped item: {len(bs)} boxes (before {n0}); at it: {len(item)}, its centre {item[0][1] - feet:.2f} above the floor" if item else f"3. a dropped item: no box at it ({len(bs)} boxes)")
        ok &= len(item) == 1 and 0.0 < item[0][1] - feet < 0.6
        n1 = len(bs)
        await ws.send(json.dumps({"t": "cmd", "c": f"particle minecraft:cloud {OX + 0.5} {Y + 2.5} {OZ - 2.5} 0.5 0.5 0.5 0 40 force"}))
        await asyncio.sleep(0.4)
        bs, _ = boxes()
        print(f"4. particles: {len(bs)} boxes (before {n1})")
        ok &= len(bs) > n1 + 10
        await asyncio.sleep(1.5)
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a weapon.mainhand with minecraft:diamond_sword"}))
        await asyncio.sleep(1.0)
        with_sword = len(near_steve(boxes()[0]))
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a weapon.mainhand with minecraft:air"}))
        await asyncio.sleep(1.0)
        bare = len(near_steve(boxes()[0]))
        print(f"5. a held sword: {with_sword - bare} boxes for it ({with_sword} at Steve, {bare} bare-handed)")
        ok &= with_sword - bare >= 4
        task.cancel()
        print()
        print("results:", "OK" if ok else "UNEXPECTED")


if __name__ == "__main__":
    running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HelloNeighbor-Win64-Shipping.exe"], capture_output=True, text=True).stdout
    if "HelloNeighbor" in running:
        sys.exit("Hello Neighbor is running: close it first (this test would drive the game's own Minecraft)")
    asyncio.run(main())
