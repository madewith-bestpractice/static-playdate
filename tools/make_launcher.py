"""Draw the launcher card (350x155) and icon (32x32) as 1-bit PNGs.
Run: python3 tools/make_launcher.py <path to Oswald variable TTF>"""
import sys
from PIL import Image, ImageDraw, ImageFont

font_path = sys.argv[1]

def dial(d, x0, x1, y0, y1, labels, spacing, center):
    d.rectangle([x0, y0, x1, y1], fill=0)
    small = ImageFont.truetype(font_path, 22)
    try: small.set_variation_by_axes([600])
    except Exception: pass
    for i, lab in enumerate(labels):
        x = center + (i - 1.35) * spacing
        w = d.textlength(lab, font=small)
        d.text((x - w / 2, y0 + 6), lab, font=small, fill=1)
    k = 0
    while True:
        x = center + (k * 0.1 - 1.35 - 2) * spacing
        if x > x1: break
        if x >= x0:
            major = k % 10 == 0
            d.line([x, y1 - (10 if major else 4), x, y1], fill=1)
        k += 1
    d.rectangle([center - 1, y0, center + 1, y1], fill=1)

card = Image.new("1", (350, 155), 1)
d = ImageDraw.Draw(card)
big = ImageFont.truetype(font_path, 54)
try: big.set_variation_by_axes([700])
except Exception: pass
title = "STATIC"
w = d.textlength(title, font=big)
d.text(((350 - w) / 2, 6), title, font=big, fill=0)
dial(d, 0, 349, 84, 128, ["NEWS", "LEARN", "MAKE", "WEiRD", "SLEEP"], 92, 175)
d.polygon([(169, 76), (181, 76), (175, 84)], fill=0)
cap = ImageFont.truetype(font_path, 15)
line = "radio made of independent podcasts"
w = d.textlength(line, font=cap)
d.text(((350 - w) / 2, 133), line, font=cap, fill=0)
card.save("Source/launcher/card.png")

icon = Image.new("1", (32, 32), 1)
d = ImageDraw.Draw(icon)
d.rectangle([0, 9, 31, 22], fill=0)
for k in range(0, 32, 3):
    d.line([k, 22 - (5 if k % 9 == 0 else 2), k, 22], fill=1)
d.rectangle([15, 9, 16, 22], fill=1)
d.polygon([(12, 4), (19, 4), (15, 9)], fill=0)
d.polygon([(12, 27), (19, 27), (15, 22)], fill=0)
icon.save("Source/launcher/icon.png")
print("ok")
