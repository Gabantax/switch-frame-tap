#!/usr/bin/env python3
"""Switch Frame Tap demo video: 1920x1080, 60 fps, H.264 + AAC.

Segments:
  title card, the phone clip (Switch on the laptop -> the TV), a native
  recording framed in the middle, the phone's stats-overlay shot, end card.
"""
import sys
import av
import numpy as np
from fractions import Fraction
from PIL import Image, ImageDraw, ImageFilter, ImageFont
import hlg   # HLG (HDR) phone video -> SDR

W, H, FPS = 1920, 1080, 60
SR = 48000
FONT_B = "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf"
FONT_M = "/usr/share/fonts/truetype/ubuntu/Ubuntu-M.ttf"
FONT_R = "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf"
BG = (14, 16, 22)
ACCENT = (64, 196, 255)

PHONE_CONNECT = "/home/tomas/Desktop/VID20261015220845.mp4"   # 4K: Switch on the laptop, then the TV
PHONE_STATS = "/home/tomas/Desktop/VID20261015221127.mp4"     # 4K: overlay, the stats at 60 fps 1080p
REC = "/home/tomas/Videos/Switch Frame Tap/switch_2026-10-15_22-31-07.mp4"   # 1080p60 recording: MK8 menu + race


def font(path, size):
    return ImageFont.truetype(path, size)


def text_block(draw, lines, y, fill=(255, 255, 255)):
    """lines: [(text, font)] centred, drawn from y down; returns the next y"""
    for t, f in lines:
        w = draw.textlength(t, font=f)
        # a soft shadow for legibility over video
        draw.text(((W - w) / 2 + 3, y + 3), t, font=f, fill=(0, 0, 0))
        draw.text(((W - w) / 2, y), t, font=f, fill=fill)
        y += int(f.size * 1.25)
    return y


def caption(img, lines, y=60):
    """a dark band at the top with the caption"""
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    band_h = 60 + sum(int(f.size * 1.25) for _, f in lines)
    bw = int(max(d.textlength(t, font=f) for t, f in lines)) + 120
    d.rounded_rectangle(((W - bw) // 2, y - 30, (W + bw) // 2, y - 30 + band_h), radius=36, fill=(10, 12, 18, 200))
    img = Image.alpha_composite(img.convert("RGBA"), over)
    d = ImageDraw.Draw(img)
    text_block(d, lines, y)
    return img.convert("RGB")


def card(lines, sub=None):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    total = sum(int(f.size * 1.25) for _, f in lines)
    y = (H - total) // 2 - 110
    y = text_block(d, lines, y)
    if sub:
        y += 30
        text_block(d, sub, y, fill=(170, 180, 195))
    return img


def frames_of(path, t0, t1):
    """(time from t0, PIL image) of the source between t0 and t1"""
    c = av.open(path)
    v = c.streams.video[0]
    v.thread_type = "AUTO"
    hdr = v.codec_context.color_trc == 18      # ARIB STD-B67 (HLG): the phone's 4K clips
    for f in c.decode(video=0):
        if f.time is None or f.time < t0:
            continue
        if f.time >= t1:
            break
        yield f.time - t0, (hlg.to_sdr(f, 0.33) if hdr else f.to_image())
    c.close()


def retime(src, dur):
    """source frames -> exactly dur*FPS output frames (repeat/skip by time)"""
    n = int(round(dur * FPS))
    it = iter(src)
    cur = None
    nxt = next(it, None)
    for i in range(n):
        t = i / FPS
        while nxt is not None and nxt[0] <= t:
            cur = nxt
            nxt = next(it, None)
        if cur is None:
            cur = nxt
        yield cur[1]


def fill_portrait(img):
    """a frame scaled to fill the picture (cropping the excess)"""
    sw, sh = img.size
    s = max(W / sw, H / sh)
    img = img.resize((int(sw * s + 0.5), int(sh * s + 0.5)), Image.LANCZOS)
    x = (img.width - W) // 2
    y = (img.height - H) // 2
    return img.crop((x, y, x + W, y + H))


def phone(img):
    """the phone clips (4K originals, upright)"""
    return fill_portrait(img)


def badge(img, text="1080p 60fps"):
    """a pill at the top left over the screen recording"""
    f = font(FONT_B, 44)
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    tw = d.textlength(text, font=f)
    d.rounded_rectangle((40, 40, 40 + tw + 56, 40 + 76), radius=38, fill=(10, 12, 18, 210))
    d.ellipse((60, 66, 84, 90), fill=(255, 64, 64, 255))   # a "live" dot
    img = Image.alpha_composite(img.convert("RGBA"), over)
    d = ImageDraw.Draw(img)
    d.text((96, 52), text, font=f, fill=ACCENT)
    return img.convert("RGB")


def framed_landscape(img):
    """a 16:9 frame in the middle, on a blurred, darkened copy of itself"""
    bg = img.resize((W, int(W * img.height / img.width)))
    bg = fill_portrait(bg).filter(ImageFilter.GaussianBlur(40))
    bg = Image.eval(bg, lambda p: int(p * 0.45))
    fw = W - 40
    fh = int(fw * img.height / img.width)
    fg = img.resize((fw, fh), Image.LANCZOS)
    bg.paste(fg, (20, (H - fh) // 2 + 120))
    return bg


def audio_of(path, t0, t1):
    """float32 stereo 48 kHz samples of the source between t0 and t1"""
    try:
        c = av.open(path)
    except Exception:
        return np.zeros((0, 2), np.float32)
    if not c.streams.audio:
        return np.zeros((0, 2), np.float32)
    rs = av.AudioResampler(format="flt", layout="stereo", rate=SR)
    out = []
    for f in c.decode(audio=0):
        if f.time is None or f.time + f.samples / f.sample_rate < t0:
            continue
        if f.time >= t1:
            break
        for g in rs.resample(f):
            out.append(g.to_ndarray().reshape(-1, 2))
    c.close()
    a = np.concatenate(out) if out else np.zeros((0, 2), np.float32)
    return a[: int((t1 - t0) * SR)]


def fit_audio(a, dur, gain=1.0):
    n = int(round(dur * SR))
    if len(a) < n:
        a = np.concatenate([a, np.zeros((n - len(a), 2), np.float32)])
    a = a[:n] * gain
    fade = min(n // 4, int(0.25 * SR))     # short fades between segments
    if fade > 0:
        ramp = np.linspace(0, 1, fade, dtype=np.float32)[:, None]
        a[:fade] *= ramp
        a[-fade:] *= ramp[::-1]
    return a


def main(out_path):
    big = font(FONT_B, 110)
    mid = font(FONT_B, 64)
    small = font(FONT_M, 46)
    tiny = font(FONT_R, 40)

    segments = []

    # 1. title
    t = card([("Switch Frame Tap", big)],
             [("Play your Switch games on your PC.", small), ("No capture card. No dock.", small),
              ("Works on Switch Lite too.", small)])
    segments.append(("still", t, 2.8, None))

    # 2. the phone clip: plug in, then the TV (0-13.6 s of b.mp4)
    segments.append(("clip", PHONE_CONNECT, 0.0, 5.8, phone,
                     [("A USB-C cable", mid), ("from the Switch to your PC", small)], 0.8))
    segments.append(("clip", PHONE_CONNECT, 5.8, 13.6, phone,
                     [("...and play on the big screen", mid), ("works on Switch Lite too", small)], 0.8))

    # 3. the screen recording (R in the viewer): the menu, then a race
    rec_shape = lambda img: badge(fill_portrait(img))
    segments.append(("clip", REC, 0.5, 3.5, rec_shape,
                     [("Recorded on the PC with one key", mid), ("the game's own frames, no capture card", small)], 1.0))
    segments.append(("clip", REC, 26.0, 40.0, rec_shape, None, 1.0))

    # 4. the stats overlay on the TV (a.mp4 ~28.5-34 s)
    segments.append(("clip", PHONE_STATS, 27.0, 31.8, phone,
                     [("Native 1080p, 60 fps", mid), ("USB or Wi-Fi", small)], 0.6))

    # 5. end card
    e = card([("Free and open source", mid)],
             [("github.com/TomasUribe/switch-frame-tap", tiny), ("", tiny),
              ("Windows, Linux and macOS", small), ("On the Homebrew App Store", small)])
    d = ImageDraw.Draw(e)
    d.rectangle((W // 2 - 90, H // 2 - 200, W // 2 + 90, H // 2 - 192), fill=ACCENT)
    segments.append(("still", e, 3.5, None))

    oc = av.open(out_path, "w")
    vs = oc.add_stream("libx264", rate=FPS)
    vs.width, vs.height, vs.pix_fmt = W, H, "yuv420p"
    vs.options = {"crf": "21", "preset": "medium", "profile": "high", "movflags": "+faststart"}
    vs.codec_context.time_base = Fraction(1, FPS)
    as_ = oc.add_stream("aac", rate=SR)
    as_.layout = "stereo"
    as_.bit_rate = 160000

    pts = 0
    audio_parts = []
    for seg in segments:
        if seg[0] == "still":
            _, img, dur, _ = seg
            n = int(round(dur * FPS))
            fr = av.VideoFrame.from_image(img)
            for _ in range(n):
                f = fr.reformat(format="yuv420p")
                f.pts = pts
                pts += 1
                for p in vs.encode(f):
                    oc.mux(p)
            audio_parts.append(fit_audio(np.zeros((0, 2), np.float32), dur))
        else:
            _, path, t0, t1, shape, cap, gain = seg
            dur = t1 - t0
            for img in retime(frames_of(path, t0, t1), dur):
                img = shape(img)
                if cap: img = caption(img, cap)
                f = av.VideoFrame.from_image(img).reformat(format="yuv420p")
                f.pts = pts
                pts += 1
                for p in vs.encode(f):
                    oc.mux(p)
            audio_parts.append(fit_audio(audio_of(path, t0, t1), dur, gain))
        print("segment done, %.1f s so far" % (pts / FPS), flush=True)
    for p in vs.encode():
        oc.mux(p)

    audio = np.clip(np.concatenate(audio_parts), -1, 1).astype(np.float32)
    apts = 0
    step = 1024
    for i in range(0, len(audio), step):
        chunk = np.ascontiguousarray(audio[i:i + step].reshape(1, -1))
        af = av.AudioFrame.from_ndarray(chunk, format="flt", layout="stereo")
        af.sample_rate = SR
        af.pts = apts
        apts += chunk.shape[1] // 2
        for p in as_.encode(af):
            oc.mux(p)
    for p in as_.encode():
        oc.mux(p)
    oc.close()
    print("wrote", out_path, "%.1f s" % (pts / FPS))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "switch-frame-tap-demo.mp4")
