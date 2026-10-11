"""Compares the compositor's old and new light estimate for Minecraft surfaces (NeighborCraft.fx relight), emulated in
Python on a synthetic Hello Neighbor scene: a bright window and a dark wall 8 m away, a grey floor from 2 to 4 m, and a
Minecraft block standing on the floor 3 m away, once in front of the window and once in front of the dark wall. The
block is opaque and stands on the same floor in both places, so it should be lit the same; how much its lit colour
changes between the two places is how much the background shows through it.

    python tools/relight_check.py

Not the GPU: the same math (taps, weights, the blur approximated by a box of the mip's footprint).
"""
import math

W, H = 320, 180
LIGHT_REFERENCE, LIGHT_TINT, LIGHT_MATCH, LIGHT_BLUR = 0.42, 0.55, 0.8, 3.2
TAN = math.tan(math.radians(35.0))


def scene():
    col, dep = [[None] * W for _ in range(H)], [[0.0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if y > H * 0.7:  # the floor: 4 m at its far edge, 2 m at the bottom of the screen
                t = (y - H * 0.7) / (H * 0.3)
                col[y][x], dep[y][x] = (0.35, 0.33, 0.30), 4.0 - 2.0 * t
            elif x < W // 2:  # a bright window
                col[y][x], dep[y][x] = (1.0, 0.95, 0.8), 8.0
            else:  # a dark wall
                col[y][x], dep[y][x] = (0.05, 0.05, 0.08), 8.0
    return col, dep


def box(col, x, y, r):
    s, n = [0.0, 0.0, 0.0], 0
    for yy in range(max(0, y - r), min(H, y + r + 1)):
        for xx in range(max(0, x - r), min(W, x + r + 1)):
            c = col[yy][xx]
            s = [s[i] + c[i] for i in range(3)]
            n += 1
    return [v / n for v in s]


def luma(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def old_light(col, dep, x, y, zm):
    # mip LightBlur of a quarter-size copy: a box about 4 * 2^LightBlur pixels wide
    return box(col, x, y, int(2 * 2 ** LIGHT_BLUR))


def new_light(col, dep, x, y, zm):
    glob = box(col, W // 2, H // 2, W)
    r = min(max(0.6 / (max(zm, 0.3) * 2.0 * TAN), 0.01), 0.25)
    s, wsum = [0.0, 0.0, 0.0], 0.0
    for i in range(12):
        a = i * 2.39996 + 0.3
        rr = r * math.sqrt((i + 0.5) / 12.0)
        tx = int(x + math.cos(a) * rr * H)
        ty = int(y + math.sin(a) * rr * H)
        tx, ty = min(max(tx, 0), W - 1), min(max(ty, 0), H - 1)
        w = math.exp(-abs(dep[ty][tx] - zm) / (0.25 + 0.1 * zm))
        c = box(col, tx, ty, 1)
        s = [s[k] + c[k] * w for k in range(3)]
        wsum += w
    near = [v / max(wsum, 1e-4) for v in s]
    k = min(max(wsum / 3.0, 0.0), 1.0)
    return [glob[i] + (near[i] - glob[i]) * k for i in range(3)]


def relit(albedo, light):
    lum = luma(light)
    gain = min(max(lum / LIGHT_REFERENCE, 0.42), 1.3)
    tint = [min(max(1.0 + (light[i] / max(lum, 1e-3) - 1.0) * LIGHT_TINT, 0.55), 1.5) for i in range(3)]
    return [albedo[i] + (albedo[i] * gain * tint[i] - albedo[i]) * LIGHT_MATCH for i in range(3)]


def main():
    col, dep = scene()
    albedo = (0.55, 0.55, 0.55)  # a stone block
    zm = 3.0
    # a two-high pillar of blocks standing on the floor 3 m away: its upper block is seen against the far wall
    y = int(H * 0.62)
    for name, fn in (("old (the picture at the pixel)", old_light), ("new (surfaces near it in depth)", new_light)):
        a = relit(albedo, fn(col, dep, W // 4, y, zm))
        b = relit(albedo, fn(col, dep, 3 * W // 4, y, zm))
        diff = max(abs(a[i] - b[i]) for i in range(3))
        print(f"{name:34s} in front of the window {[round(v, 3) for v in a]}, of the dark wall {[round(v, 3) for v in b]}:"
              f" differs by {diff:.3f} ({diff / max(max(a), max(b)) * 100:.0f}% of the brighter)")


if __name__ == "__main__":
    main()
