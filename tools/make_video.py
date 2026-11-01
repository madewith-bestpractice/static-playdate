"""Build a demo video from a simulator take (autotest "video:..."): the
Playdate screen at 3x on a paper background with a title and credits, plus
the recorded output audio. Usage: make_video.py <take dir> <out.mp4> <title> <credits>"""
import struct, subprocess, sys, os
from PIL import Image, ImageDraw, ImageFont
take, out, title, credits = sys.argv[1:5]
FONT = "/Volumes/GetawayCar/GitHub/static/Oswald-VariableFont_wght.ttf"
W, H, FPS, SCALE = 1920, 1080, 30, 3
PAPER, INK = (245, 243, 239), (40, 38, 34)
LCD_BG, LCD_INK = (178, 176, 168), (49, 47, 40)
t0 = int(open(os.path.join(take, "vid_meta.txt")).read().split()[0])
raw = open(os.path.join(take, "vid_frames.bin"), "rb").read()
REC = 4 + 52 * 240
frames = []
for i in range(0, len(raw) - REC + 1, REC):
    t = struct.unpack("<I", raw[i:i + 4])[0] - t0
    frames.append((t, raw[i + 4:i + REC]))
dur = os.path.getsize(os.path.join(take, "take_mix.wav")) / (44100 * 2)
def screen(buf):
    rows = b"".join(bytes(buf[y * 52:y * 52 + 50]) for y in range(240))   # 52-byte rows, 50 used
    im = Image.frombytes("1", (400, 240), rows)
    rgb = Image.new("RGB", (400, 240), LCD_BG)
    rgb.paste(Image.new("RGB", (400, 240), LCD_INK), mask=im.point(lambda v: 255 - v).convert("L"))
    return rgb.resize((400 * SCALE, 240 * SCALE), Image.NEAREST)
base = Image.new("RGB", (W, H), PAPER)
d = ImageDraw.Draw(base)
ft = ImageFont.truetype(FONT, 64); 
try: ft.set_variation_by_axes([600])
except Exception: pass
fc = ImageFont.truetype(FONT, 30)
sw, sh = 400 * SCALE, 240 * SCALE
sx, sy = (W - sw) // 2, (H - sh) // 2 + 10
tw = d.textlength(title, font=ft); d.text(((W - tw) / 2, sy - 112), title, font=ft, fill=INK)
cw = d.textlength(credits, font=fc); d.text(((W - cw) / 2, sy + sh + 34), credits, font=fc, fill=(110, 106, 98))
d.rectangle([sx - 14, sy - 14, sx + sw + 13, sy + sh + 13], fill=(30, 29, 26))
p = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
    "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
    "-i", os.path.join(take, "take_mix.wav"),
    "-vf", f"fade=t=in:st=0:d=0.3,fade=t=out:st={dur - 0.5:.2f}:d=0.5",
    "-af", f"afade=t=in:st=0:d=0.15,afade=t=out:st={dur - 0.5:.2f}:d=0.5",
    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16", "-preset", "slow",
    "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart", out], stdin=subprocess.PIPE)
k = 0; cache = {}
for n in range(int(dur * FPS)):
    tau = n * 1000 / FPS
    while k + 1 < len(frames) and frames[k + 1][0] <= tau: k += 1
    if k not in cache:
        cache.clear()
        img = base.copy(); img.paste(screen(frames[k][1]), (sx, sy)); cache[k] = img.tobytes()
    p.stdin.write(cache[k])
p.stdin.close(); p.wait()
print("ok", out, f"{dur:.1f}s", len(frames), "captured frames")
