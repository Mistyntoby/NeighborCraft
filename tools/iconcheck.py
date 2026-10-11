"""How much of each exported HN icon the object fills: the bounding box of its covered pixels (the same rules as
CarryIcons: a PNG with alpha, turned round if its border is opaque (Unreal writes 1 - coverage); or Radiance .hdr,
black is empty), as a share of the picture's side. A tiny share is a tiny icon.

    python tools/iconcheck.py [icons dir]

Default dir: the installed mod's icons/ folder.
"""
import glob
import os
import sys

HN = r"C:\Program Files (x86)\Steam\steamapps\common\Hello Neighbor\HelloNeighbor\Binaries\Win64\ue4ss\Mods\NeighborCraftMod\dlls\icons"


def radiance(path):
    b = open(path, "rb").read()
    i = 0
    width = height = 0
    while True:
        end = b.index(b"\n", i)
        line = b[i:end].decode("ascii", "replace").strip()
        i = end + 1
        if line.startswith("-Y "):
            parts = line.split()
            height, width = int(parts[1]), int(parts[3])
            break
    rows = []
    for _ in range(height):
        scan = bytearray(width * 4)
        if 8 <= width < 32768 and b[i] == 2 and b[i + 1] == 2 and not (b[i + 2] & 0x80):
            i += 4
            for plane in range(4):
                x = 0
                while x < width:
                    count = b[i]
                    i += 1
                    if count > 128:
                        count -= 128
                        v = b[i]
                        i += 1
                        for _ in range(count):
                            scan[x * 4 + plane] = v
                            x += 1
                    else:
                        for _ in range(count):
                            scan[x * 4 + plane] = b[i]
                            i += 1
                            x += 1
        else:
            scan[:] = b[i:i + width * 4]
            i += width * 4
        row = []
        for x in range(width):
            e = scan[x * 4 + 3]
            f = 0.0 if e == 0 else 2.0 ** (e - 136)
            row.append(sum(scan[x * 4 + k] * f for k in range(3)))
        rows.append(row)
    return width, height, rows


def coverage(width, height, rows):
    # the same rule as CarryIcons (tone of r+g+b >= 6 of 255): about 0.0015 linear in total
    xs, ys = [], []
    for y in range(height):
        for x in range(width):
            if rows[y][x] > 0.0015 * 3:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return min(xs), min(ys), max(xs), max(ys)


def png_box(path):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    a = [[im.getpixel((x, y))[3] for x in range(w)] for y in range(h)]
    border = [a[0][x] for x in range(w)] + [a[h - 1][x] for x in range(w)]
    if sum(1 for v in border if v > 127) * 2 > len(border):
        a = [[255 - v for v in row] for row in a]
    xs = [x for y in range(h) for x in range(w) if a[y][x] > 127]
    ys = [y for y in range(h) for x in range(w) if a[y][x] > 127]
    return w, h, ((min(xs), min(ys), max(xs), max(ys)) if xs else None)


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else HN
    files = sorted(glob.glob(os.path.join(folder, "*.hdr")))
    if not files:
        sys.exit("no icons in " + folder)
    for f in files:
        if open(f, "rb").read(4)[1:] == b"PNG":
            w, h, box = png_box(f)
        else:
            w, h, rows = radiance(f)
            box = coverage(w, h, rows)
        name = os.path.basename(f)
        if not box:
            print(f"  {name:50s} empty")
            continue
        side = max(box[2] - box[0] + 1, box[3] - box[1] + 1) / max(w, h)
        print(f"  {name:50s} {w}x{h}  object {box[2] - box[0] + 1}x{box[3] - box[1] + 1} px = {side * 100:5.1f}% of the side{'  TINY' if side < 0.5 else ''}")


if __name__ == "__main__":
    main()
