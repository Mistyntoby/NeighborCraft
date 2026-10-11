"""(History: the screen-based estimate this compares was replaced, 2026-10-11 night, by Hello Neighbor's sun colour
passed in by the add-on: any estimate from the picture changes with where one looks. Kept as the record of why.)

The compositor's light colour for Minecraft blocks (NeighborCraft.fx relight: the near-light tint and the grade),
old against new, emulated on synthetic Hello Neighbor pictures:

  A. midday, a lawn filling most of the screen, a grey road below: white light. A grey stone block must stay grey.
  B. the same at sunset: orange light on everything (the lawn, the road). The block must turn orange.

Old: the light's colour = the average colour around the block / of the whole picture (a lawn's green counted as
light). New: from near-grey pixels only (PS_Grey / light_colour). Printed: the block's colour after the tint and the
grade (stone albedo 0.5 grey), and its hue error against the true light.

    python tools/tintcheck.py
"""
import colorsys

LIGHT_TINT, GRADE_MATCH = 0.55, 0.55


def luma(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def picture(light):
    """(colour, share of the screen): lit surfaces = albedo * light."""
    lawn, road, wall = (0.25, 0.55, 0.15), (0.45, 0.45, 0.45), (0.6, 0.58, 0.55)
    lit = lambda a: tuple(min(1.0, a[i] * light[i]) for i in range(3))
    return [(lit(lawn), 0.7), (lit(road), 0.2), (lit(wall), 0.1)]


def average(px, weight=lambda c: 1.0):
    s, w = [0.0, 0.0, 0.0], 0.0
    for c, share in px:
        k = share * weight(c)
        w += k
        for i in range(3):
            s[i] += c[i] * k
    return tuple(v / max(w, 1e-9) for v in s), w


def grey_weight(c):
    hi, lo = max(c), min(c)
    sat = (hi - lo) / hi if hi > 1e-3 else 1.0
    import math
    return math.exp(-sat / 0.15) * max(0.0, min(1.0, hi * 8.0)) * max(0.0, min(1.0, (0.98 - hi) * 20.0))


def normalised(c):
    l = max(luma(c), 1e-3)
    return tuple(v / l for v in c)


def block(colour_of_light):
    """A grey stone block (albedo 0.5) after the tint (LightTint) and the grade (0.6, GradeMatch)."""
    tint = tuple(min(1.5, max(0.55, 1.0 + (v - 1.0) * LIGHT_TINT)) for v in colour_of_light)
    grade = tuple(min(1.6, max(0.6, 1.0 + (v - 1.0) * 0.6)) for v in colour_of_light)
    c = tuple(0.5 * t for t in tint)
    return tuple(c[i] * (1.0 + (grade[i] - 1.0) * GRADE_MATCH) for i in range(3))


def hue_error(c, light):
    h1, s1, _ = colorsys.rgb_to_hsv(*c)
    h2, s2, _ = colorsys.rgb_to_hsv(*[v / max(light) for v in light])
    if s2 < 0.05:
        return s1  # white light: any saturation is error
    d = abs(h1 - h2)
    return min(d, 1 - d) + abs(s1 - s2 * 0.5)


ok = True
for name, light in (("A. midday (white light)", (1.0, 1.0, 1.0)), ("B. sunset (orange light)", (1.0, 0.7, 0.45))):
    px = picture(light)
    old = block(normalised(average(px)[0]))
    new = block(normalised(average(px, grey_weight)[0]))
    eo, en = hue_error(old, light), hue_error(new, light)
    print(f"{name}: block old {tuple(round(v, 3) for v in old)} (error {eo:.3f}), new {tuple(round(v, 3) for v in new)} (error {en:.3f})")
    ok &= en < eo or en < 0.02
    if name.startswith("A"):
        ok &= abs(new[0] - new[1]) < 0.02 and abs(new[1] - new[2]) < 0.02  # grey stays grey (the test wall is slightly warm)
    else:
        ok &= new[0] > new[1] > new[2] and en < eo  # orange stays orange
print("results:", "OK" if ok else "UNEXPECTED")
