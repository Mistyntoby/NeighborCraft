"""Tests Minecraft's inventory input without Hello Neighbor: opens the inventory through the link, moves the cursor
over the hotbar and left-clicks the first slot (positions are fractions of the window, as HN sends them), then
screenshots Minecraft's window. Minecraft's log shows each click ("screen click: ...").

    python tools/inventorytest.py [outdir]
"""
import asyncio
import json
import os
import subprocess
import sys

import websockets

PORT = 25620


async def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    async with websockets.connect(f"ws://127.0.0.1:{PORT}") as ws:
        await ws.recv()  # hello
        await ws.send(json.dumps({"t": "key", "k": "inventory", "down": True}))
        await ws.send(json.dumps({"t": "key", "k": "inventory", "down": False}))
        await asyncio.sleep(1.0)
        # survival inventory, 1920x1080, GUI scale 4: the hotbar row sits ~0.663 down; first slot ~0.425 across
        for x, y in [(0.30, 0.30), (0.425, 0.663)]:
            await ws.send(json.dumps({"t": "mouse", "x": x, "y": y}))
            await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "click", "b": 0, "down": True, "x": 0.425, "y": 0.663}))
        await asyncio.sleep(0.1)
        await ws.send(json.dumps({"t": "click", "b": 0, "down": False, "x": 0.425, "y": 0.663}))
        await asyncio.sleep(0.3)
        await ws.send(json.dumps({"t": "mouse", "x": 0.55, "y": 0.45}))
        await asyncio.sleep(0.8)
    out = os.path.join(outdir, "inventory.png")
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                    "ddagrab=output_idx=0:draw_mouse=0:framerate=30,hwdownload,format=bgra", "-frames:v", "1", out], check=False)
    print("screenshot:", out)


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else os.path.expandvars(r"%USERPROFILE%\NeighborCraft-lab\inventorytest")))
