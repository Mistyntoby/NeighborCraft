"""Checks the world layer Minecraft hands to Hello Neighbor (without Hello Neighbor): acts as the host over the link,
stands Steve in front of a row of blocks of one kind at a time (solid, cutout, translucent), and reads the frame from
the shared memory the compositor reads (Local\\NeighborCraftFrame), as the compositor gets it. For each kind: the alpha
of every pixel where Minecraft has geometry (its depth isn't empty). The compositor blends world.rgb + host * (1 -
world.a) (premultiplied), so a solid block must be alpha 255 everywhere it covers; anything less lets Hello Neighbor's
picture show through it. Pictures of the colour and the alpha go to mc/run/rendertest-*.png.

    python tools/rendertest.py

Start Minecraft first (gradlew runClient in mc/) with Hello Neighbor closed.
"""
import asyncio
import json
import mmap
import os
import struct
import subprocess
import sys
import time

import websockets
from PIL import Image

PORT = 25620
Y = 140
OX, OZ = 120, 120
HEADER = 4096
STRIDE = 3840 * 2160 * 4 * 4
KINDS = {
    "solid": ["stone", "oak_planks", "white_wool", "bricks", "dirt"],
    "cutout": ["oak_leaves", "glass", "iron_bars", "oak_trapdoor", "cobweb"],
    "translucent": ["red_stained_glass", "blue_stained_glass", "ice", "slime_block", "honey_block"],
}


def read_frame():
    m = mmap.mmap(-1, HEADER + STRIDE * 3, tagname="Local\\NeighborCraftFrame")
    slot = struct.unpack_from("<i", m, 40)[0]
    if slot < 0:
        return None
    desc = 256 + 128 * slot
    w, h = struct.unpack_from("<ii", m, desc + 24)
    n = w * h * 4
    base = HEADER + STRIDE * slot
    color = bytes(m[base:base + n])
    depth = bytes(m[base + n:base + 2 * n])
    m.close()
    return w, h, color, depth


def analyse(name, frame, outdir):
    w, h, color, depth = frame
    d = struct.unpack("<%df" % (w * h), depth)
    covered = alpha_low = 0
    alphas = {}
    img = Image.frombytes("RGBA", (w, h), color).transpose(Image.FLIP_TOP_BOTTOM)
    amask = Image.new("L", (w, h))
    ap = amask.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            a = color[i * 4 + 3]
            if d[i] > 0.0:  # reversed Z: 0 = nothing drawn
                covered += 1
                bucket = a // 32 * 32
                alphas[bucket] = alphas.get(bucket, 0) + 1
                if a < 250:
                    alpha_low += 1
            ap[x, h - 1 - y] = a
    img.save(os.path.join(outdir, f"rendertest-{name}-color.png"))
    amask.save(os.path.join(outdir, f"rendertest-{name}-alpha.png"))
    share = alpha_low / max(covered, 1)
    print(f"  {name:11s} pixels with Minecraft geometry: {covered:7d}, alpha < 250: {alpha_low:7d} ({share * 100:5.1f}%)"
          f"   alpha histogram (lower edge: count) {dict(sorted(alphas.items()))}")
    return share


async def main():
    outdir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "mc", "run"))
    async with websockets.connect(f"ws://127.0.0.1:{PORT}", max_size=None) as ws:
        print("hello:", json.loads(await ws.recv()).get("t"))
        await ws.send(json.dumps({"t": "view", "w": 640, "h": 360}))
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
        await ws.send(json.dumps({"t": "cmd", "c": "time set noon"}))
        state = {"frame": 0}

        async def host():
            while True:
                state["frame"] += 1
                await ws.send(json.dumps({"t": "cam", "drive": True, "f": state["frame"], "r": [0.0, -15.0, 0.0], "fov": 70.0}))  # looking over the wall: no block targeted (its outline is translucent)
                await ws.send(json.dumps({"t": "move", "m": 0}))
                try:
                    while True:
                        await asyncio.wait_for(ws.recv(), 0.001)
                except asyncio.TimeoutError:
                    pass
                await asyncio.sleep(1 / 60)

        task = asyncio.create_task(host())
        await asyncio.sleep(2.0)
        results = {}
        for kind, blocks in KINDS.items():
            # a wall 3 blocks ahead: the five blocks side by side, two rows
            await ws.send(json.dumps({"t": "cmd", "c": f"fill {OX - 3} {Y + 1} {OZ + 3} {OX + 3} {Y + 3} {OZ + 3} air"}))
            for i, b in enumerate(blocks):
                for yy in (1, 2):
                    await ws.send(json.dumps({"t": "cmd", "c": f"setblock {OX - 2 + i} {Y + yy} {OZ + 3} minecraft:{b}"}))
            await asyncio.sleep(1.5)
            frame = read_frame()
            if not frame:
                print("no frame in the shared memory")
                break
            results[kind] = analyse(kind, frame, outdir)
        task.cancel()
        print()
        if "solid" in results:
            print("solid blocks:", "OPAQUE (alpha 255 wherever they are drawn)" if results["solid"] < 0.001 else
                  "NOT OPAQUE: Hello Neighbor would show through them")


if __name__ == "__main__":
    running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq HelloNeighbor-Win64-Shipping.exe"], capture_output=True, text=True).stdout
    if "HelloNeighbor" in running:
        sys.exit("Hello Neighbor is running: close it first (this test would drive the game's own Minecraft)")
    asyncio.run(main())
