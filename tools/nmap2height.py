#!/usr/bin/env python3
"""Recover a tileable height map from a tangent-space normal map via
Frankot-Chellappa FFT integration (periodic boundary == tiled texture).
Sign of the green channel is auto-calibrated against the albedo: mortar
(dark albedo lines) must be LOW, brick faces HIGH.

usage: nmap2height.py normal.png albedo.png out.png
"""
import sys
import numpy as np
from PIL import Image

nrm = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.float64) / 255.0
alb = np.asarray(Image.open(sys.argv[2]).convert("L"), dtype=np.float64) / 255.0
H, W = nrm.shape[:2]

nx = nrm[:, :, 0] * 2.0 - 1.0
ny = nrm[:, :, 1] * 2.0 - 1.0
nz = np.maximum(nrm[:, :, 2] * 2.0 - 1.0, 0.05)


def integrate(gy_sign):
    # surface gradients from normals: dh/dx = -nx/nz, dh/dy = sign * ny/nz
    p = -nx / nz
    q = gy_sign * ny / nz
    fu = np.fft.fftfreq(W)[None, :] * 2 * np.pi
    fv = np.fft.fftfreq(H)[:, None] * 2 * np.pi
    P = np.fft.fft2(p)
    Q = np.fft.fft2(q)
    denom = fu ** 2 + fv ** 2
    denom[0, 0] = 1.0
    Hf = (-1j * fu * P - 1j * fv * Q) / denom
    Hf[0, 0] = 0.0
    return np.real(np.fft.ifft2(Hf))


def mortar_correlation(h):
    # mortar = darkest 15% of albedo; bricks = brightest 50%
    lo = alb < np.quantile(alb, 0.15)
    hi = alb > np.quantile(alb, 0.50)
    return h[hi].mean() - h[lo].mean()  # positive = bricks higher than mortar


best = None
for gy in (+1.0, -1.0):
    h = integrate(gy)
    c = mortar_correlation(h)
    print(f"gy_sign={gy:+.0f}: brick-minus-mortar height = {c:+.4f}")
    if best is None or c > best[1]:
        best = (h, c, gy)

h, c, gy = best
if c <= 0:
    print("WARNING: no sign gives bricks-above-mortar; check assumptions")
# robust normalize (clip 0.5% tails) to 0..255
lo, hi = np.quantile(h, 0.005), np.quantile(h, 0.995)
h8 = np.clip((h - lo) / (hi - lo), 0, 1)
Image.fromarray((h8 * 255.0 + 0.5).astype(np.uint8), mode="L").save(sys.argv[3])
st = (h8.mean(), h8.std())
print(f"chosen gy_sign={gy:+.0f}, corr={c:+.4f}, out mean={st[0]:.3f} std={st[1]:.3f} -> {sys.argv[3]}")
