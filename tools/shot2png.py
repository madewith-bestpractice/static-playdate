"""Convert autopilot screen grabs (Data/shot_NN.bin: 240 rows x 52 bytes, 1 bit
per pixel, MSB first, 1 = white) to PNGs at 2x. Usage: shot2png.py <dir>"""
import sys, glob, os
from PIL import Image
for path in sorted(glob.glob(os.path.join(sys.argv[1], "shot_*.bin"))):
    b = open(path, "rb").read()
    img = Image.new("1", (400, 240))
    px = img.load()
    for y in range(240):
        row = b[y * 52:(y + 1) * 52]
        for x in range(400):
            px[x, y] = (row[x >> 3] >> (7 - (x & 7))) & 1
    img.resize((800, 480), Image.NEAREST).save(path[:-4] + ".png")
print("ok")
