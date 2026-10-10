"""Reads a collision-recorder session (F12 in game; mc/run/neighborcraft-collisions/session-*.jsonl) and writes a
report: event counts, and for each event the movement around it and pictures of what Minecraft collided with.

    python tools/collision_report.py [session.jsonl] [outdir]

Pictures (one per event): top-down at the feet (+5 cm) and at the chest (+1 m), and a side view along the attempted
movement. Grey: collision boxes (HN's world as Minecraft has it, and Minecraft blocks); blue: the player's hitbox at
the event; green: the path over the window; red: the attempted movement of the event tick, magnified x20.
"""
import glob
import json
import math
import os
import sys
from collections import Counter

from PIL import Image, ImageDraw

SCALE = 120  # pixels per block
HALF = 2.0   # blocks shown around the player


def newest_session():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mc", "run", "neighborcraft-collisions")
    files = glob.glob(os.path.join(root, "session-*.jsonl"))
    return max(files, key=os.path.getmtime) if files else None


def event_tick(ticks, event):
    """The tick whose labels gave the event (the window ends one second after it)."""
    for t in reversed(ticks):
        if t["labels"] and " | ".join(t["labels"]) == event:
            return t
    return ticks[len(ticks) * 2 // 3]


def draw_top(t, ticks, y):
    img = Image.new("RGB", (int(2 * HALF * SCALE), int(2 * HALF * SCALE)), "white")
    d = ImageDraw.Draw(img)
    cx, cz = t["pos"][0], t["pos"][2]

    def px(x, z):
        return ((x - cx + HALF) * SCALE, (z - cz + HALF) * SCALE)

    for i in range(int(-HALF) - 1, int(HALF) + 2):  # block grid
        gx, _ = px(math.floor(cx) + i, 0)
        _, gz = px(0, math.floor(cz) + i)
        d.line([(gx, 0), (gx, img.height)], fill=(225, 225, 225))
        d.line([(0, gz), (img.width, gz)], fill=(225, 225, 225))
    for b in t["boxes"]:
        if b[1] <= y < b[4]:
            d.rectangle([px(b[0], b[2]), px(b[3], b[5])], fill=(140, 140, 140), outline=(60, 60, 60))
    path = [px(k["pos"][0], k["pos"][2]) for k in ticks]
    if len(path) > 1:
        d.line(path, fill=(0, 160, 0), width=2)
    bb = t["bb"]
    d.rectangle([px(bb[0], bb[2]), px(bb[3], bb[5])], outline=(0, 0, 255), width=3)
    a = t["attempt"]
    d.line([px(cx, cz), px(cx + a[0] * 20, cz + a[2] * 20)], fill=(220, 0, 0), width=3)
    d.text((5, 5), f"top-down at y {y:.2f} (feet {t['pos'][1]:.3f})", fill="black")
    return img


def draw_side(t, ticks):
    """A vertical section along the attempted horizontal direction."""
    a = t["attempt"]
    ln = math.hypot(a[0], a[2])
    ux, uz = (a[0] / ln, a[2] / ln) if ln > 1e-6 else (1.0, 0.0)
    img = Image.new("RGB", (int(2 * HALF * SCALE), int(2.5 * SCALE)), "white")
    d = ImageDraw.Draw(img)
    cx, cy, cz = t["pos"]

    def px(s, y):
        return ((s + HALF) * SCALE, img.height - (y - (cy - 0.5)) * SCALE)

    for b in t["boxes"]:
        # boxes the section line (through the player, along the movement) passes through
        lo, hi = None, None
        for s in [i / 50.0 - HALF for i in range(int(2 * HALF * 50) + 1)]:
            x, z = cx + ux * s, cz + uz * s
            if b[0] <= x <= b[3] and b[2] <= z <= b[5]:
                lo = s if lo is None else lo
                hi = s
        if lo is not None:
            d.rectangle([px(lo, b[4]), px(hi, b[1])], fill=(140, 140, 140), outline=(60, 60, 60))
    bb = t["bb"]
    w = (bb[3] - bb[0]) / 2
    d.rectangle([px(-w, bb[4]), px(w, bb[1])], outline=(0, 0, 255), width=3)
    d.line([px(0, cy), px(ln * 20, cy + a[1] * 20)], fill=(220, 0, 0), width=3)
    d.line([px(-HALF, cy + 0.6), px(HALF, cy + 0.6)], fill=(255, 160, 0))
    d.text((5, 5), "side view along the movement (orange: step height 0.6 above the feet)", fill="black")
    return img


def main(path, outdir):
    if not path:
        print("no session found (press F12 in game to record one)")
        return
    os.makedirs(outdir, exist_ok=True)
    events = [json.loads(line) for line in open(path, encoding="utf-8") if line.strip()]
    counts = Counter()
    lines = [f"session {path}: {len(events)} events"]
    for i, e in enumerate(events):
        for label in e["event"].split(" | "):
            counts[label.split(" ")[0]] += 1
        ticks = e["ticks"]
        t = event_tick(ticks, e["event"])
        before = [k for k in ticks if k["tick"] < t["tick"]][-10:]
        speed_before = max((math.hypot(k["vel"][0], k["vel"][2]) * 20 for k in before), default=0.0)
        speed = math.hypot(t["vel"][0], t["vel"][2]) * 20
        lines.append(f"\n#{i:03d} tick {t['tick']}: {e['event']}")
        lines.append(f"     pos {t['pos']}  ground {t['ground']}  pose {t['pose']}  sprint {t['sprint']}  input {t['input']}  jump {t['jump']}")
        lines.append(f"     speed {speed:.2f} m/s (up to {speed_before:.2f} in the 0.5 s before)  attempt {t['attempt']}  resolved {t['resolved']}")
        lines.append(f"     collision: horizontal {t['hColl']} (minor {t['minor']}), vertical {t['vColl']}; {len(t['boxes'])} boxes around")
        feet = t["pos"][1]
        top = draw_top(t, ticks, feet + 0.05)
        chest = draw_top(t, ticks, feet + 1.0)
        side = draw_side(t, ticks)
        sheet = Image.new("RGB", (top.width * 3 + 20, max(top.height, side.height)), "white")
        sheet.paste(top, (0, 0))
        sheet.paste(chest, (top.width + 10, 0))
        sheet.paste(side, (top.width * 2 + 20, 0))
        sheet.save(os.path.join(outdir, f"event-{i:03d}.png"))
    lines.insert(1, "labels: " + ", ".join(f"{k} x{v}" for k, v in counts.most_common()))
    report = "\n".join(lines)
    open(os.path.join(outdir, "report.txt"), "w", encoding="utf-8").write(report)
    print(report[:6000])
    print(f"\npictures: {outdir}")


if __name__ == "__main__":
    session = sys.argv[1] if len(sys.argv) > 1 else newest_session()
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.expandvars(r"%USERPROFILE%\NeighborCraft-lab\collision-report")
    main(session, out)
