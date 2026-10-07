"""Tests the Minecraft side without Hello Neighbor: acts as the host over the link, then reads the frames the mod
exports into shared memory and saves the world colour and depth as PNGs.

    python tools/linktest.py [outdir]

It gives Minecraft a flat floor (barriers at y 62..63 around the origin), places a gold block pillar 4 blocks in
front of the camera, and sends a camera at eye height looking at it. Expect gold in the middle of world.png, a
transparent background (no sky), and the pillar nearer than everything else in depth.png.
"""
import asyncio
import json
import mmap
import os
import struct
import sys
import time

import numpy as np
import websockets
from PIL import Image

PORT = 25620
SHM = "Local\\NeighborCraftFrame"
HEADER = 4096


def read_frame():
    head = mmap.mmap(-1, HEADER, tagname=SHM, access=mmap.ACCESS_READ)
    magic, version, header, slots = struct.unpack_from("<4i", head, 0)
    stride, = struct.unpack_from("<q", head, 16)
    publish, = struct.unpack_from("<q", head, 32)
    latest, = struct.unpack_from("<i", head, 40)
    head.close()
    assert magic == 0x5450434D, hex(magic)
    if latest < 0:
        return None
    m = mmap.mmap(-1, HEADER + stride * slots, tagname=SHM, access=mmap.ACCESS_READ)
    desc = 256 + 128 * latest
    w, h = struct.unpack_from("<2i", m, desc + 24)
    near, far, fov = struct.unpack_from("<3f", m, desc + 32)
    flags, = struct.unpack_from("<i", m, desc + 44)
    base = HEADER + stride * latest
    n = w * h * 4
    color = np.frombuffer(m, np.uint8, n, base).reshape(h, w, 4)[::-1].copy()
    depth = np.frombuffer(m, np.float32, w * h, base + n).reshape(h, w)[::-1].copy()
    m.close()
    return dict(publish=publish, w=w, h=h, near=near, far=far, fov=fov, flags=flags, color=color, depth=depth)


async def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    async with websockets.connect(f"ws://127.0.0.1:{PORT}") as ws:
        print("hello:", await ws.recv())
        await ws.send(json.dumps({"t": "view", "w": 1280, "h": 720}))
        await ws.send(json.dumps({"t": "clear"}))
        cols = []
        for x in range(-8, 9):
            for z in range(-8, 9):
                cols += [x, z, 62, 63]
        await ws.send(json.dumps({"t": "ground", "c": cols}))
        await ws.send(json.dumps({"t": "cmd", "c": "fill 0 64 4 0 66 4 minecraft:gold_block"}))
        # eye at (0.5, 65.62, 0.5) looking +Z (yaw 0) at the pillar
        start = time.time()
        frame = 0
        while time.time() - start < 4.0:
            frame += 1
            await ws.send(json.dumps({"t": "cam", "f": frame, "p": [0.5, 65.62, 0.5], "r": [0.0, 10.0, 0.0], "fov": 60.0, "fp": True}))
            try:
                while True:
                    msg = await asyncio.wait_for(ws.recv(), 0.001)
                    if '"blocks"' in msg:
                        print("event:", msg[:160])
            except asyncio.TimeoutError:
                pass
            await asyncio.sleep(1 / 60)
    f = read_frame()
    if f is None:
        print("no frame exported")
        return 1
    alpha = f["color"][..., 3]
    print(f"frame {f['w']}x{f['h']} publish={f['publish']} near={f['near']} far={f['far']:.0f} fov={f['fov']} flags={f['flags']} "
          f"covered={np.mean(alpha > 0):.3f} centre={f['color'][f['h'] // 2, f['w'] // 2]}")
    Image.fromarray(f["color"], "RGBA").save(os.path.join(outdir, "world.png"))
    d = f["depth"]
    Image.fromarray((np.clip(d, 0, 1) * 255).astype(np.uint8)).save(os.path.join(outdir, "depth.png"))
    print("saved", outdir)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else os.path.expandvars(r"%USERPROFILE%\NeighborCraft-lab\linktest"))))
