"""HLG (BT.2100, BT.2020) 10-bit 4K phone video -> SDR BT.709 RGB at 1920x1080."""
import numpy as np
from PIL import Image

A, B, C = 0.17883277, 0.28466892, 0.55991073
M2020_709 = np.array([[1.6605, -0.5876, -0.0728],
                      [-0.1246, 1.1329, -0.0083],
                      [-0.0182, -0.1006, 1.1187]], np.float32)


def plane(f, i, h, w):
    p = f.planes[i]
    a = np.frombuffer(p, np.uint16).reshape(-1, p.line_size // 2)
    return a[:h, :w].astype(np.float32)


def to_sdr(f, exposure=1.0):
    w, h = f.width, f.height
    y = plane(f, 0, h, w)
    y = (y[0::2, 0::2] + y[1::2, 0::2] + y[0::2, 1::2] + y[1::2, 1::2]) * 0.25   # 2x2 down: chroma's size
    u = plane(f, 1, h // 2, w // 2)
    v = plane(f, 2, h // 2, w // 2)
    yp = (y - 64.0) / 876.0
    cb = (u - 512.0) / 896.0
    cr = (v - 512.0) / 896.0
    rgb = np.stack([yp + 1.4746 * cr, yp - 0.16455 * cb - 0.57135 * cr, yp + 1.8814 * cb], -1)
    rgb = np.clip(rgb, 0.0, 1.0)
    # HLG inverse OETF -> scene light (0..1)
    lin = np.where(rgb <= 0.5, rgb * rgb / 3.0, (np.exp((rgb - C) / A) + B) / 12.0)
    # HLG reference white (75 % signal, ~0.265) -> SDR white
    lin = lin * (exposure / 0.265)
    lin = lin @ M2020_709.T
    lin = np.clip(lin, 0.0, None)
    # soft highlight roll-off above 0.75
    k = 0.75
    over = lin > k
    lin = np.where(over, k + (1 - k) * (1 - np.exp(-(lin - k) / (1 - k))), lin)
    srgb = np.where(lin <= 0.0031308, 12.92 * lin, 1.055 * np.power(lin, 1 / 2.4) - 0.055)
    return Image.fromarray((np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8))
