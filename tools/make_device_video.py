"""Render a simulator take (autotest "video:...", PDV2 records with the crank
angle) as a stylized yellow handheld whose crank turns exactly as the take's
crank did, with the recorded audio. No Panic logos or wordmarks are drawn.

Usage: make_device_video.py <take dir> <out.mp4> <title> <subtitle> <credit line> [<credit line> ...]
The take dir needs vid_frames.bin, vid_meta.txt and take_mix.wav."""
import math, os, struct, subprocess, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

take, out, title, subtitle, *credits = sys.argv[1:]
FONT = "/Volumes/GetawayCar/GitHub/static/Oswald-VariableFont_wght.ttf"
W, H, FPS, SS = 1920, 1080, 30, 2           # SS: supersampling for smooth edges
PAPER, INK, MUTED = (245, 243, 239), (40, 38, 34), (110, 106, 98)
BODY, BODY_EDGE = (255, 196, 46), (222, 160, 18)
DARK, LCD_BG, LCD_INK = (52, 50, 46), (178, 176, 168), (49, 47, 40)

# --- the take -------------------------------------------------------------
t0 = int(open(os.path.join(take, "vid_meta.txt")).read().split()[0])
raw = open(os.path.join(take, "vid_frames.bin"), "rb").read()
assert raw[:4] == b"PDV2", "needs a take recorded with crank angles (PDV2)"
REC = 8 + 52 * 240
frames = []
for i in range(4, len(raw) - REC + 1, REC):
    t, ang = struct.unpack("<If", raw[i:i + 8])
    frames.append((t - t0, ang, raw[i + 8:i + REC]))
wav = os.path.join(take, "take_mix.wav")
dur = (os.path.getsize(wav) - 44) / (44100 * 2)

def screen(buf, scale):
    rows = b"".join(bytes(buf[y * 52:y * 52 + 50]) for y in range(240))
    im = Image.frombytes("1", (400, 240), rows)
    rgb = Image.new("RGB", (400, 240), LCD_BG)
    rgb.paste(Image.new("RGB", (400, 240), LCD_INK), mask=im.point(lambda v: 255 - v).convert("L"))
    return rgb.resize((400 * scale, 240 * scale), Image.NEAREST)

# --- the static parts, drawn once ----------------------------------------
SCALE = 2
BX, BY, BW, BH = 90, 110, 860, 860          # device body
SX, SY = BX + (BW - 800) // 2, BY + 70       # screen (800x480)
HUB = (BX + BW, SY + 240)                    # crank hub on the right edge
ARM, KNOB = 170, 26

def font(size, weight):
    f = ImageFont.truetype(FONT, size * SS)
    try: f.set_variation_by_axes([weight])
    except Exception: pass
    return f

base = Image.new("RGB", (W * SS, H * SS), PAPER)
d = ImageDraw.Draw(base)
s = lambda *v: [x * SS for x in v]
# soft shadow
sh = Image.new("L", (W * SS, H * SS), 0)
ImageDraw.Draw(sh).rounded_rectangle(s(BX + 18, BY + 28, BX + BW + 18, BY + BH + 28), radius=70 * SS, fill=90)
sh = sh.filter(ImageFilter.GaussianBlur(28 * SS))
base.paste(Image.new("RGB", base.size, (205, 200, 190)), mask=sh)
d.rounded_rectangle(s(BX, BY, BX + BW, BY + BH), radius=70 * SS, fill=BODY, outline=BODY_EDGE, width=4 * SS)
d.rounded_rectangle(s(SX - 22, SY - 22, SX + 822, SY + 502), radius=18 * SS, fill=DARK)   # bezel
# d-pad
cx, cy, a, b = BX + 210, BY + 690, 58, 22
d.rounded_rectangle(s(cx - a, cy - b, cx + a, cy + b), radius=6 * SS, fill=DARK)
d.rounded_rectangle(s(cx - b, cy - a, cx + b, cy + a), radius=6 * SS, fill=DARK)
# A and B
for (x, y) in ((BX + 600, BY + 720), (BX + 720, BY + 660)):
    d.ellipse(s(x - 42, y - 42, x + 42, y + 42), fill=DARK)
# text block
tx = 1180
d.text(s(tx, 290), title, font=font(84, 600), fill=INK)
d.text(s(tx, 410), subtitle, font=font(52, 400), fill=INK)
d.text(s(tx, 560), "On air", font=font(34, 600), fill=MUTED)
for i, line in enumerate(credits):
    d.text(s(tx, 610 + i * 50), line, font=font(36, 400), fill=MUTED)
d.text(s(tx, 880), "staticradio.app", font=font(40, 500), fill=INK)

def crank_layer(angle_deg):
    """The crank seen from the front: it turns in the side plane, so the knob
    travels up and down the right edge (and slightly out toward us and back),
    the way the real one looks when you face the screen."""
    lay = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    hx, hy = HUB
    r = math.radians(angle_deg)
    ox = hx + 34                                   # the folded-out hinge, just off the edge
    kx = ox + 40 + 30 * math.sin(r)                # foreshortened depth
    ky = hy - ARM * math.cos(r)
    near = math.sin(r) >= 0                        # knob toward us: darker and a touch bigger
    knob = KNOB + (3 if near else -3)
    ld.rounded_rectangle(s(hx - 6, hy - 30, ox + 8, hy + 30), radius=10 * SS, fill=BODY_EDGE)   # hinge block
    ld.line(s(ox, hy, kx, ky), fill=DARK, width=22 * SS)
    ld.ellipse(s(ox - 16, hy - 16, ox + 16, hy + 16), fill=DARK)
    ld.ellipse(s(kx - knob, ky - knob, kx + knob, ky + knob), fill=DARK if near else (70, 67, 61))
    ld.ellipse(s(kx - knob + 9, ky - knob + 9, kx + knob - 9, ky + knob - 9), fill=(86, 83, 76))
    return lay

# --- render ---------------------------------------------------------------
p = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
    "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-", "-i", wav,
    "-vf", f"fade=t=in:st=0:d=0.3,fade=t=out:st={dur - 0.5:.2f}:d=0.5",
    "-af", f"afade=t=in:st=0:d=0.15,afade=t=out:st={dur - 0.5:.2f}:d=0.5",
    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16", "-preset", "slow",
    "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart", out], stdin=subprocess.PIPE)
k = 0
last = None
for n in range(int(dur * FPS)):
    tau = n * 1000 / FPS
    while k + 1 < len(frames) and frames[k + 1][0] <= tau: k += 1
    t, ang, buf = frames[k]
    key = (k, round(ang, 1))
    if key != last:
        img = base.copy()
        img.paste(screen(buf, SCALE * SS), (SX * SS, SY * SS))
        cl = crank_layer(ang)
        img.paste(cl, (0, 0), cl)
        frame = img.resize((W, H), Image.LANCZOS).tobytes()
        last = key
    p.stdin.write(frame)
p.stdin.close(); p.wait()
print("ok", out, f"{dur:.1f}s", len(frames), "captured frames")
