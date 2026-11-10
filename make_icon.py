#!/usr/bin/env python3
"""FreezeMark icon: glass-blue rounded square + coral annotation frame
with corner handles + white snowflake (freeze) in the middle.
Drawn per-size so small sizes stay readable (snowflake dropped <=32px)."""
import math
from PIL import Image, ImageDraw

SS = 4  # supersample


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def draw_icon(size):
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # --- glass rounded-square background with vertical gradient ---
    inset = int(s * 0.045)
    rad = int(s * 0.24)
    grad = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    top, bot = (0x5C, 0xAC, 0xFF), (0x1E, 0x55, 0xC8)
    for y in range(s):
        gd.line([(0, y), (s, y)], fill=lerp(top, bot, y / s) + (255,))
    mask = Image.new("L", (s, s), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle([inset, inset, s - inset, s - inset], radius=rad, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # gloss: soft white blob top-left, clipped by the same mask
    gloss = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    gl = ImageDraw.Draw(gloss)
    gl.ellipse([-s * 0.25, -s * 0.55, s * 0.75, s * 0.28], fill=(255, 255, 255, 90))
    img.paste(Image.alpha_composite(img, gloss), (0, 0), mask)
    d = ImageDraw.Draw(img)
    # rim
    d.rounded_rectangle([inset, inset, s - inset - 1, s - inset - 1],
                        radius=rad, outline=(255, 255, 255, 190),
                        width=max(1, int(s * 0.012)))

    # --- coral annotation frame ---
    x0, y0, x1, y1 = s * 0.235, s * 0.26, s * 0.765, s * 0.76
    lw = max(2, int(s * 0.052))
    coral = (0xFF, 0x51, 0x47, 255)
    d.rounded_rectangle([x0, y0, x1, y1], radius=int(s * 0.045),
                        outline=coral, width=lw)

    # --- snowflake in the middle (freeze), large sizes only ---
    if size >= 48:
        cx, cy = s / 2, (y0 + y1) / 2
        R = s * 0.115
        sw = max(2, int(s * 0.035))
        white = (255, 255, 255, 245)
        for k in range(3):
            ang = math.radians(90 + k * 60)
            dx, dy = math.cos(ang) * R, math.sin(ang) * R
            d.line([(cx - dx, cy - dy), (cx + dx, cy + dy)], fill=white, width=sw)
            for sgn in (1, -1):
                ex, ey = cx + dx * sgn, cy + dy * sgn
                for ba in (ang + math.radians(150), ang - math.radians(150)):
                    bx = ex - math.cos(ba) * R * 0.42 * sgn
                    by = ey - math.sin(ba) * R * 0.42 * sgn
                    d.line([(ex, ey), (bx, by)], fill=white, width=sw)

    # --- corner handles (selection points) ---
    hr = max(2.0, s * 0.052)
    for hx, hy in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
        d.ellipse([hx - hr, hy - hr, hx + hr, hy + hr],
                  fill=(255, 255, 255, 255), outline=coral,
                  width=max(1, int(s * 0.02)))

    return img.resize((size, size), Image.LANCZOS)


sizes = [256, 128, 64, 48, 32, 24, 16]
frames = [draw_icon(sz) for sz in sizes]
frames[0].save("/home/hatch/workspace/FreezeMark/app.ico", format="ICO",
               sizes=[(sz, sz) for sz in sizes],
               append_images=frames[1:])
draw_icon(256).save("/home/hatch/workspace/FreezeMark/icon_preview.png")
draw_icon(32).save("/home/hatch/workspace/FreezeMark/icon_preview_32.png")
print("icon written")
