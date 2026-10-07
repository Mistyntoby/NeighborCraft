"""Runs Minecraft server commands through the NeighborCraft link (as the "dev" side, next to the game's mod).

    python tools/cmd.py "execute as @p at @s run setblock ^ ^1 ^3 minecraft:gold_block"
    python tools/cmd.py --listen 10      # print what the link sends for 10 s
"""
import asyncio
import json
import sys
import time

import websockets

PORT = 25620


async def main(args):
    async with websockets.connect(f"ws://127.0.0.1:{PORT}") as ws:
        await ws.recv()  # hello
        if args and args[0] == "--listen":
            end = time.time() + (float(args[1]) if len(args) > 1 else 10.0)
            while time.time() < end:
                try:
                    print(await asyncio.wait_for(ws.recv(), max(0.01, end - time.time()))[:300])
                except asyncio.TimeoutError:
                    break
            return
        for command in args:
            await ws.send(json.dumps({"t": "cmd", "c": command}))
            print("sent:", command)
        await asyncio.sleep(0.3)


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1:]))
