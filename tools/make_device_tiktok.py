"""Vertical (1080x1920) cut of a device demo for TikTok/Shorts: the handheld
large in the middle with its crank turning from the take's recorded angles,
a title on top, timed captions under it, credits kept clear of the app's UI.

Usage: make_device_tiktok.py <take dir> <out.mp4> <captions.json>
captions.json: {"title": "...", "subtitle": "...", "credits": "...", "site": "...",
                "captions": [[start_s, end_s, "text"], ...]}"""
import json, math, os, struct, subprocess, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

take, out, capfile = sys.argv[1:4]
C = json.load(open(capfile))
FONT = "/Volumes/GetawayCar/GitHub/static/Oswald-VariableFont_wght.ttf"
W, H, FPS, SS = 1080, 1920, 30, 2
PAPER, INK, MUTED = (245, 243, 239), (40, 38, 34), (110, 106, 98)
BODY, BODY_EDGE = (255, 196, 46), (222, 160, 18)
DARK, LCD_BG, LCD_INK = (52, 50, 46), (178, 176, 168), (49, 47, 40)

t0 = int(open(os.path.join(take, "vid_meta.txt")).read().split()[0])
raw = open(os.path.join(take, "vid_frames.bin"), "rb").read()
assert raw[:4] == b"PDV2"
REC = 8 + 52 * 240
frames = []
for i in range(4, len(raw) - REC + 1, REC):
    t, ang = struct.unpack("<If", raw[i:i + 8])
    frames.append((t - t0, ang, raw[i + 8:i + REC]))
wav = os.path.join(take, "take_mix.wav")
dur = (os.path.getsize(wav) - 44) / (44100 * 2)

SW, SH = 720, 432                            # screen at 1.8x
BX, BY, BW, BH = 70, 330, 840, 840
SX, SY = BX + (BW - SW) // 2, BY + 66
HUB = (BX + BW, SY + SH // 2)
ARM, KNOB = 150, 24

def screen(buf):
    rows = b"".join(bytes(buf[y * 52:y * 52 + 50]) for y in range(240))
    im = Image.frombytes("1", (400, 240), rows)
    rgb = Image.new("RGB", (400, 240), LCD_BG)
    rgb.paste(Image.new("RGB", (400, 240), LCD_INK), mask=im.point(lambda v: 255 - v).convert("L"))
    return rgb.resize((SW * SS, SH * SS), Image.NEAREST)

def font(size, weight):
    f = ImageFont.truetype(FONT, size * SS)
    try: f.set_variation_by_axes([weight])
    except Exception: pass
    return f
s = lambda *v: [x * SS for x in v]

def centered(d, y, text, f, fill):
    w = d.textlength(text, font=f)
    d.text(((W * SS - w) / 2, y * SS), text, font=f, fill=fill)

base = Image.new("RGB", (W * SS, H * SS), PAPER)
d = ImageDraw.Draw(base)
sh = Image.new("L", base.size, 0)
ImageDraw.Draw(sh).rounded_rectangle(s(BX + 16, BY + 26, BX + BW + 16, BY + BH + 26), radius=66 * SS, fill=90)
base.paste(Image.new("RGB", base.size, (205, 200, 190)), mask=sh.filter(ImageFilter.GaussianBlur(26 * SS)))
d.rounded_rectangle(s(BX, BY, BX + BW, BY + BH), radius=66 * SS, fill=BODY, outline=BODY_EDGE, width=4 * SS)
d.rounded_rectangle(s(SX - 20, SY - 20, SX + SW + 20, SY + SH + 20), radius=16 * SS, fill=DARK)
cx, cy, a, b = BX + 200, BY + 680, 54, 20
d.rounded_rectangle(s(cx - a, cy - b, cx + a, cy + b), radius=6 * SS, fill=DARK)
d.rounded_rectangle(s(cx - b, cy - a, cx + b, cy + a), radius=6 * SS, fill=DARK)
for (x, y) in ((BX + 580, BY + 710), (BX + 690, BY + 650)):
    d.ellipse(s(x - 40, y - 40, x + 40, y + 40), fill=DARK)
centered(d, 100, C["title"], font(96, 600), INK)
centered(d, 222, C["subtitle"], font(54, 400), INK)
centered(d, 1405, C["credits"], font(30, 400), MUTED)
centered(d, 1452, C["site"], font(40, 500), INK)

def crank_layer(angle_deg):
    lay = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    hx, hy = HUB
    r = math.radians(angle_deg)
    ox = hx + 30
    kx = ox + 36 + 26 * math.sin(r)
    ky = hy - ARM * math.cos(r)
    near = math.sin(r) >= 0
    knob = KNOB + (3 if near else -3)
    ld.rounded_rectangle(s(hx - 6, hy - 28, ox + 8, hy + 28), radius=10 * SS, fill=BODY_EDGE)
    ld.line(s(ox, hy, kx, ky), fill=DARK, width=20 * SS)
    ld.ellipse(s(ox - 15, hy - 15, ox + 15, hy + 15), fill=DARK)
    ld.ellipse(s(kx - knob, ky - knob, kx + knob, ky + knob), fill=DARK if near else (70, 67, 61))
    ld.ellipse(s(kx - knob + 8, ky - knob + 8, kx + knob - 8, ky + knob - 8), fill=(86, 83, 76))
    return lay

capfont = font(52, 500)
def caption_at(t):
    for a, b, text in C["captions"]:
        if a <= t < b: return text
    return ""

p = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
    "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-", "-i", wav,
    "-vf", f"fade=t=in:st=0:d=0.3,fade=t=out:st={dur - 0.5:.2f}:d=0.5",
    "-af", f"afade=t=in:st=0:d=0.15,afade=t=out:st={dur - 0.5:.2f}:d=0.5",
    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "17", "-preset", "slow",
    "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart", out], stdin=subprocess.PIPE)
k, last, frame = 0, None, None
for n in range(int(dur * FPS)):
    tau = n * 1000 / FPS
    while k + 1 < len(frames) and frames[k + 1][0] <= tau: k += 1
    t, ang, buf = frames[k]
    cap = caption_at(tau / 1000)
    key = (k, round(ang, 1), cap)
    if key != last:
        img = base.copy()
        img.paste(screen(buf), (SX * SS, SY * SS))
        cl = crank_layer(ang)
        img.paste(cl, (0, 0), cl)
        if cap:
            dd = ImageDraw.Draw(img)
            lines, cur = [], ""
            for word in cap.split():
                trial = (cur + " " + word).strip()
                if dd.textlength(trial, font=capfont) > 860 * SS and cur: lines.append(cur); cur = word
                else: cur = trial
            lines.append(cur)
            for i, line in enumerate(lines):
                centered(dd, 1230 + i * 66, line, capfont, INK)
        frame = img.resize((W, H), Image.LANCZOS).tobytes()
        last = key
    p.stdin.write(frame)
p.stdin.close(); p.wait()
print("ok", out, f"{dur:.1f}s")
