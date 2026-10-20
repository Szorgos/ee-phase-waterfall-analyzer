"""Aqulify ikon generálása (aether.ico, több méretben). Futtatás: python make_icon.py"""
import math
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # rajzolás nagy felbontáson, majd kicsinyítés

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
# Háttér: lekerekített négyzet, mélykék -> lila átmenet
bg = Image.new("RGBA", (S, S))
px = bg.load()
c1, c2 = (14, 22, 40, 255), (58, 34, 92, 255)
for y in range(S):
    for x in range(S):
        px[x, y] = lerp(c1, c2, (x * 0.4 + y * 0.6) / S)
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([40, 40, S - 40, S - 40], radius=210, fill=255)
img.paste(bg, (0, 0), mask)

# "Vízesés": egymás mögötti spektrum-hegyvonulatok, elöl ciánkék, hátul narancs/vörös
d = ImageDraw.Draw(img)
layers = 6
for li in range(layers):
    t = li / (layers - 1)
    base = 760 - li * 70
    amp = 330 - li * 40
    col = lerp((255, 120, 0, 255), (0, 240, 255, 255), t)
    pts = []
    for i in range(0, 101):
        x = 150 + i * 7.24
        f = i / 100
        y = base - amp * (0.55 * math.exp(-((f - 0.22) / 0.10) ** 2) + 0.35 * math.exp(-((f - 0.62) / 0.06) ** 2)
                          + 0.18 * math.exp(-((f - 0.85) / 0.05) ** 2)) * (0.6 + 0.4 * t)
        pts.append((x, y))
    poly = pts + [(874, base + 30), (150, base + 30)]
    shade = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shade).polygon(poly, fill=col[:3] + (90 + int(120 * t),))
    img = Image.alpha_composite(img, shade)
    d = ImageDraw.Draw(img)
    d.line(pts, fill=col, width=18 if li == layers - 1 else 10, joint="curve")

# Finom ragyogás az élen
glow = img.filter(ImageFilter.GaussianBlur(6))
img = Image.alpha_composite(glow, img)
img.putalpha(Image.composite(img.getchannel("A"), Image.new("L", (S, S), 0), mask))

sizes = [16, 24, 32, 48, 64, 128, 256]
img.resize((256, 256), Image.LANCZOS).save("aether.ico", sizes=[(s, s) for s in sizes])
img.resize((256, 256), Image.LANCZOS).save("aether_256.png")
print("aether.ico kész")
