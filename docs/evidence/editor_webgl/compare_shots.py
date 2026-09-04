#!/usr/bin/env python3
"""Compare two editor screenshots (CPU vs GPU) on the canvas rectangle each page reported:
mean luminance, and the high-pass luminance correlation at unit scale (the blender_ref calibration metric).
usage: compare_shots.py a.png a.log b.png b.log out_pair.jpg"""
import json, re, sys
import numpy as np
from PIL import Image, ImageFilter

def rect(logp):
    for ln in open(logp):
        if ln.startswith("[shoot] {"):
            d = json.loads(ln[len("[shoot] "):]); r = d["rect"]; return [int(round(v)) for v in r], d
    raise SystemExit("no rect in " + logp)

def canvas(png, logp):
    im = Image.open(png).convert("RGB"); (x, y, w, h), d = rect(logp)
    # the screenshot is at device pixel ratio 1 in headless; crop the CSS rect
    return im.crop((x, y, x + w, y + h)), d

def lum(im):
    a = np.asarray(im.convert("L"), dtype=np.float64); return a

def hp(a, sigma=6.0):
    im = Image.fromarray(a.astype(np.uint8)); lo = np.asarray(im.filter(ImageFilter.GaussianBlur(sigma)), dtype=np.float64); return a - lo

def corr(a, b):
    a = a - a.mean(); b = b - b.mean(); d = np.sqrt((a * a).sum() * (b * b).sum()); return float((a * b).sum() / d) if d > 0 else 0.0

A, da = canvas(sys.argv[1], sys.argv[2]); B, db = canvas(sys.argv[3], sys.argv[4])
if A.size != B.size: B = B.resize(A.size, Image.LANCZOS)
la, lb = lum(A), lum(B)
print("canvas", A.size, "gpu flags", da.get("gpu"), db.get("gpu"))
print("mean luminance  A %.1f  B %.1f" % (la.mean(), lb.mean()))
print("black fraction  A %.3f  B %.3f" % ((la < 8).mean(), (lb < 8).mean()))
print("hp correlation  %.3f" % corr(hp(la), hp(lb)))
print("raw correlation %.3f" % corr(la, lb))
pair = Image.new("RGB", (A.width * 2 + 8, A.height), (24, 24, 24)); pair.paste(A, (0, 0)); pair.paste(B, (A.width + 8, 0)); pair.save(sys.argv[5], "JPEG", quality=90)
print("wrote", sys.argv[5])
