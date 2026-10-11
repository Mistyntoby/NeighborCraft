"""Tests carrying Hello Neighbor's objects on Minecraft's side, without Hello Neighbor: acts as the host over the link,
stands Steve on a floor in drive mode, and goes through the pickup rules and the carrying hand:

  1. empty hand: a pickup is taken into the selected slot ("picked" ok, "held" with its id, "hand" fist positions);
  2. holding it: a second pickup is refused ("hand not empty");
  3. a Minecraft item in the hand (stone): refused too;
  4. an empty slot selected again: taken;
  5. "consume" removes the token.

and screenshots of Minecraft's own frame go to mc/run/screenshots/: the empty hand, the pickup swing, the carrying
arm (a still and while walking). Minecraft's log tells the pickups ("into the hand") and refusals.

    python tools/handtest.py

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
OX, OZ = 40, 40


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
        else:
            print("no ready")
            return
        await ws.send(json.dumps({"t": "clear"}))
        cells = []
        for x in range(-4, 5):
            for z in range(-4, 5):
                cells += [x + OX, Y, z + OZ, 1]
        await ws.send(json.dumps({"t": "tp", "p": [OX + 0.5, Y + 1.01, OZ + 0.5]}))
        await asyncio.sleep(0.1)
        await ws.send(json.dumps({"t": "shape", "c": cells, "m": [], "f": [], "g": []}))
        await ws.send(json.dumps({"t": "cmd", "c": "clear @a"}))
        await ws.send(json.dumps({"t": "cmd", "c": "time set noon"}))

        msgs = []
        state = {"frame": 0, "move": 0, "pitch": 10.0}

        async def host():
            # the host's camera and keys, every frame; Minecraft's messages collected
            while True:
                state["frame"] += 1
                await ws.send(json.dumps({"t": "cam", "drive": True, "f": state["frame"], "r": [0.0, 0.0, 0.0], "fov": 70.0}))
                await ws.send(json.dumps({"t": "move", "m": state["move"]}))
                try:
                    while True:
                        m = json.loads(await asyncio.wait_for(ws.recv(), 0.001))
                        if m.get("t") in ("picked", "held", "hand", "mcpos"):
                            msgs.append((time.time(), m))
                except (asyncio.TimeoutError, json.JSONDecodeError):
                    pass
                await asyncio.sleep(1 / 60)

        task = asyncio.create_task(host())

        async def shot(name):
            await ws.send(json.dumps({"t": "shot", "n": name, "layer": "hand"}))
            await asyncio.sleep(0.3)

        def since(t0, kind):
            return [m for t, m in msgs if t >= t0 and m.get("t") == kind]

        async def pickup(oid):
            t0 = time.time()
            await ws.send(json.dumps({"t": "pickup", "id": oid, "name": "Test Mug", "cls": "/Game/Test.Test_C", "kg": 60}))
            await asyncio.sleep(0.15)
            await shot(f"hand-{oid}-swing.png")
            await asyncio.sleep(1.2)
            picked = since(t0, "picked")
            held = since(t0, "held")
            hands = since(t0, "hand")
            ok = picked[-1]["ok"] if picked else None
            print(f"  pickup {oid}: picked {[(p['ok'], p['why']) for p in picked]}, held now {held[-1]['id'] if held else '?'!r}"
                  f" (empty {held[-1].get('empty') if held else '?'}), {len(hands)} fist positions")
            cams = since(t0, "mcpos")
            if hands and cams:
                # the fist from the camera: Minecraft x/z with the look along -z at yaw 0 (the test's camera), y up
                c, p = cams[-1]["cam"], hands[-1]["p"]
                rel = [p[i] - c[i] for i in range(3)]
                print(f"    fist from the camera: right {-rel[0]:+.2f}, up {rel[1]:+.2f}, ahead {rel[2]:+.2f} blocks (distance {hands[-1]['d']:.2f})")
            return ok

        await asyncio.sleep(2.0)
        await ws.send(json.dumps({"t": "slot", "n": 0}))
        await asyncio.sleep(0.5)
        await shot("hand-empty.png")
        print("1. empty hand")
        r1 = await pickup("t1")
        await asyncio.sleep(0.8)
        await shot("hand-carrying.png")
        state["move"] = 1
        await asyncio.sleep(0.6)
        await shot("hand-carrying-walking.png")
        state["move"] = 0
        print("2. holding t1, pickup t2")
        r2 = await pickup("t2")
        print("3. stone in slot 2, pickup t3")
        await ws.send(json.dumps({"t": "slot", "n": 1}))
        await ws.send(json.dumps({"t": "cmd", "c": "item replace entity @a hotbar.1 with minecraft:stone"}))
        await asyncio.sleep(0.6)
        r3 = await pickup("t3")
        print("4. empty slot 3 selected, pickup t4")
        await ws.send(json.dumps({"t": "slot", "n": 2}))
        await asyncio.sleep(0.6)
        r4 = await pickup("t4")
        print("5. consume t1, t4")
        await ws.send(json.dumps({"t": "consume", "id": "t1"}))
        await ws.send(json.dumps({"t": "consume", "id": "t4"}))
        await asyncio.sleep(0.8)
        await ws.send(json.dumps({"t": "slot", "n": 0}))
        await asyncio.sleep(0.6)
        held = [m for _, m in msgs if m.get("t") == "held"]
        print(f"  after consume, slot 1 holds {held[-1]['id']!r} (empty {held[-1].get('empty')})")
        task.cancel()
        print()
        print("results:", "OK" if (r1, r2, r3, r4) == (True, False, False, True) else "UNEXPECTED",
              f"(empty hand {r1}, holding {r2}, stone {r3}, empty slot {r4})")


if __name__ == "__main__":
    running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HelloNeighbor-Win64-Shipping.exe"], capture_output=True, text=True).stdout
    if "HelloNeighbor" in running:
        sys.exit("Hello Neighbor is running: close it first (this test would drive the game's own Minecraft)")
    asyncio.run(main())
