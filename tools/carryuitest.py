"""Minecraft's side of the carry UI without Hello Neighbor (this script is the host):

  1. the progress bar in the experience bar's place: a pickup value shows, a throw value replaces it (never both),
     ending one kind leaves the other, a value that stops coming runs out (400 ms), and in the screenshots the green
     of the bar grows with the value (bar-*.png);
  2. icons: one of HN's real captures (a chair: the object a speck in the middle) is framed to the common size
     (the log line, and items-framed.png);
  3. the fist: holding a token, every "hand" message names that token (HN shows an object on its own fist only).

    python tools/carryuitest.py

Start Minecraft first (gradlew runClient in mc/) with Hello Neighbor closed.
"""
import asyncio
import json
import os
import re
import subprocess
import sys
import time

import websockets

PORT = 25620
Y = 140
OX, OZ = 160, 160
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SHOTS = os.path.join(ROOT, "mc", "run", "screenshots")
LOG = os.path.join(ROOT, "mc", "run", "logs", "latest.log")
CHAIR = r"C:\Program Files (x86)\Steam\steamapps\common\Hello Neighbor\HelloNeighbor\Binaries\Win64\ue4ss\Mods\NeighborCraftMod\dlls\icons\BP_Chair_apartments_C.hdr"


def green_in_bar(path):
    """Green pixels of the experience bar's progress in the bottom part of a screenshot (its middle 60%)."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    n = 0
    for y in range(int(h * 0.82), h):
        for x in range(int(w * 0.2), int(w * 0.8)):
            r, g, b = im.getpixel((x, y))
            if g > 150 and g > r + 40 and g > b + 60:
                n += 1
    return n


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
        cells = [c for x in range(-4, 5) for z in range(-4, 5) for c in (x + OX, Y, z + OZ, 1)]
        await ws.send(json.dumps({"t": "shape", "c": cells, "m": [], "f": [], "g": []}))
        await asyncio.sleep(1.0)
        await ws.send(json.dumps({"t": "tp", "p": [OX + 0.5, Y + 1.01, OZ + 0.5]}))
        await ws.send(json.dumps({"t": "cmd", "c": "clear @a"}))
        await ws.send(json.dumps({"t": "cmd", "c": "xp set @a 0 levels"}))
        await ws.send(json.dumps({"t": "cmd", "c": "xp set @a 0 points"}))
        msgs = []
        state = {"frame": 0, "bar": None}

        async def host():
            while True:
                state["frame"] += 1
                await ws.send(json.dumps({"t": "cam", "drive": True, "f": state["frame"], "r": [0.0, 0.0, 0.0], "fov": 70.0}))
                await ws.send(json.dumps({"t": "move", "m": 0}))
                if state["bar"] and state["frame"] % 3 == 0:  # as HN: every ~50 ms while it runs
                    await ws.send(json.dumps({"t": "bar", "k": state["bar"][0], "v": state["bar"][1]}))
                try:
                    while True:
                        m = json.loads(await asyncio.wait_for(ws.recv(), 0.001))
                        if m.get("t") in ("barq", "hand", "inv", "held"):
                            msgs.append(m)
                except (asyncio.TimeoutError, json.JSONDecodeError):
                    pass
                await asyncio.sleep(1 / 60)

        task = asyncio.create_task(host())

        async def barq():
            msgs.clear()
            await ws.send(json.dumps({"t": "barq"}))
            for _ in range(40):
                await asyncio.sleep(0.03)
                got = [m for m in msgs if m.get("t") == "barq"]
                if got:
                    return got[-1]["k"], round(got[-1]["v"], 2)
            return None

        async def shot(name):
            await ws.send(json.dumps({"t": "shot", "n": name}))
            path = os.path.join(SHOTS, name)
            for _ in range(60):
                await asyncio.sleep(0.1)
                if os.path.exists(path) and time.time() - os.path.getmtime(path) < 10:
                    await asyncio.sleep(0.2)
                    return path
            return None

        ok = True
        await asyncio.sleep(1.5)
        print("1. the bar")
        for f in ("bar-none.png", "bar-pickup-25.png", "bar-throw-75.png"):
            if os.path.exists(os.path.join(SHOTS, f)):
                os.remove(os.path.join(SHOTS, f))
        none_shot = await shot("bar-none.png")
        state["bar"] = ("pickup", 0.25)
        await asyncio.sleep(0.4)
        q = await barq()
        print("   pickup 25%:", q)
        ok &= q == ("pickup", 0.25)
        p25 = await shot("bar-pickup-25.png")
        state["bar"] = ("throw", 0.75)
        await asyncio.sleep(0.4)
        q = await barq()
        print("   then throw 75%:", q, "(the throw replaces the pickup)")
        ok &= q == ("throw", 0.75)
        t75 = await shot("bar-throw-75.png")
        await ws.send(json.dumps({"t": "bar", "k": "pickup", "v": -1}))
        await asyncio.sleep(0.1)
        q = await barq()
        print("   ending 'pickup' while the throw shows:", q, "(the throw stays)")
        ok &= q == ("throw", 0.75)
        state["bar"] = None
        await asyncio.sleep(0.7)
        q = await barq()
        print("   no more values for 0.7 s:", q, "(ran out)")
        ok &= q == ("", 0.75) or (q is not None and q[0] == "")
        state["bar"] = ("throw", 0.5)
        await asyncio.sleep(0.3)
        state["bar"] = None
        await ws.send(json.dumps({"t": "bar", "k": "throw", "v": -1}))
        await asyncio.sleep(0.1)
        q = await barq()
        print("   ended at once:", q)
        ok &= q is not None and q[0] == ""
        if none_shot and p25 and t75:
            g0, g25, g75 = green_in_bar(none_shot), green_in_bar(p25), green_in_bar(t75)
            print(f"   green of the bar in the screenshots: none {g0}, 25% {g25}, 75% {g75} (expected 0 < 25% < 75%, about 1:3)")
            ok &= g0 < g25 < g75 and 2.0 < g75 / max(g25, 1) < 4.5
        else:
            print("   no screenshots")
            ok = False

        print("2. icon framing")
        if os.path.exists(CHAIR):
            log_before = os.path.getsize(LOG) if os.path.exists(LOG) else 0
            await ws.send(json.dumps({"t": "slot", "n": 0}))
            await ws.send(json.dumps({"t": "pickup", "id": "u1", "name": "Chair", "cls": "/Game/Test/BP_ChairT.BP_ChairT_C", "kg": 50}))
            await asyncio.sleep(0.6)
            await ws.send(json.dumps({"t": "icon", "key": "/Game/Test/BP_ChairT.BP_ChairT_C", "file": CHAIR.replace("\\", "/")}))
            await asyncio.sleep(0.8)
            await ws.send(json.dumps({"t": "slot", "n": 3}))
            await asyncio.sleep(0.4)
            await shot("items-framed.png")
            text = open(LOG, encoding="utf-8", errors="replace").read()[log_before:] if os.path.exists(LOG) else ""
            m = re.search(r"icon for /Game/Test/BP_ChairT\.BP_ChairT_C framed: the object was (\d+)x(\d+) of (\d+)x\d+ px, scaled ([\d.]+)", text)
            if m:
                bw, bh, side, scale = int(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4))
                fill = max(bw, bh) * scale / side
                print(f"   the chair was {bw}x{bh} of {side} px; scaled {scale}: it fills {fill * 100:.0f}% of the side (expected 87%)")
                ok &= abs(fill - 0.875) < 0.03
            else:
                print("   no framing line in the log")
                ok = False
        else:
            print("   (no chair capture on this machine: skipped)")

        print("3. the fist names its token")
        await ws.send(json.dumps({"t": "slot", "n": 0}))
        await asyncio.sleep(1.0)
        msgs.clear()
        await asyncio.sleep(0.8)
        hands = [m for m in msgs if m.get("t") == "hand"]
        ids = {m.get("id") for m in hands}
        print(f"   {len(hands)} fist messages holding u1, ids {ids}")
        ok &= len(hands) > 10 and ids == {"u1"}
        await ws.send(json.dumps({"t": "consume", "id": "u1"}))
        await asyncio.sleep(0.5)
        task.cancel()
        print()
        print("results:", "OK" if ok else "UNEXPECTED", "(screenshots in mc/run/screenshots: bar-*.png, items-framed.png)")


if __name__ == "__main__":
    running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HelloNeighbor-Win64-Shipping.exe"], capture_output=True, text=True).stdout
    if "HelloNeighbor" in running:
        sys.exit("Hello Neighbor is running: close it first (this test would drive the game's own Minecraft)")
    asyncio.run(main())
