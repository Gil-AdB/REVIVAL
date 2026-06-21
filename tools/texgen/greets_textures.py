#!/usr/bin/env python3
"""Procedural texture generator for the greets (pyramid) scene.

Produces, per surface, a 1024x1024 colour DIFFUSE (.jpg) and a grayscale
HEIGHT map (.png, lossless). The engine bakes a tangent-space NORMAL map from
the height map at load time (BakeNormalMapFromDiffuse reads luminance), so the
height field here drives real relief — mortar grooves and carved glyphs recess
regardless of colour, unlike luminance-from-diffuse.

Also writes a *_normal_preview.png (the engine's exact encoding:
R=(nx+1)*127.5, G=(ny+1)*127.5, B=(nz+1)*127.5, nx=-dH/dx, ny=-dH/dy) purely
so we can eyeball the normal before wiring it in.

Usage: python3 greets_textures.py <surface> <outdir>   # surface: wall|floor|ceiling
"""
import sys, numpy as np
from PIL import Image, ImageDraw

N = 1024
rng = np.random.default_rng

def tileable_value_noise(n, cells, seed):
    """Seamless value noise in [0,1] at `cells`x`cells` lattice, bilerp-upsampled to n."""
    g = rng(seed).random((cells, cells), dtype=np.float64)
    # wrap the lattice so the upsample is seamless
    gx = np.concatenate([g, g[:, :1]], axis=1)
    gg = np.concatenate([gx, gx[:1, :]], axis=0)
    ys = np.linspace(0, cells, n, endpoint=False)
    xs = np.linspace(0, cells, n, endpoint=False)
    y0 = ys.astype(int); x0 = xs.astype(int)
    fy = (ys - y0)[:, None]; fx = (xs - x0)[None, :]
    # smoothstep
    fy = fy*fy*(3-2*fy); fx = fx*fx*(3-2*fx)
    v00 = gg[y0][:, x0]; v01 = gg[y0][:, x0+1]
    v10 = gg[y0+1][:, x0]; v11 = gg[y0+1][:, x0+1]
    return (v00*(1-fx)+v01*fx)*(1-fy) + (v10*(1-fx)+v11*fx)*fy

def fbm(n, base_cells, octaves, seed, persistence=0.5):
    out = np.zeros((n, n)); amp = 1.0; tot = 0.0
    for o in range(octaves):
        out += amp * tileable_value_noise(n, base_cells * (2**o), seed + o*97)
        tot += amp; amp *= persistence
    out /= tot
    return out

def normalize01(a):
    lo, hi = a.min(), a.max()
    return (a - lo) / (hi - lo + 1e-9)

# ---- block / ashlar masonry layout (seamless) ----
def masonry(n, courses, blocks_per_course, mortar_px, seed):
    """Returns (block_id, mortar_mask, per-pixel block tone offset). Seamless."""
    ch = n // courses
    block_id = np.zeros((n, n), dtype=np.int32)
    mortar = np.zeros((n, n), dtype=bool)
    tone = np.zeros((n, n))
    r = rng(seed)
    bid = 1
    for c in range(courses):
        y0 = c*ch; y1 = y0+ch
        bw = n // blocks_per_course
        offset = (bw//2) if (c % 2) else 0          # running bond, half-offset
        for b in range(blocks_per_course + 1):
            x0 = (b*bw - offset) % n
            x1 = x0 + bw
            t = (r.random()-0.5)*0.18                # per-block tone
            xs = np.arange(x0, x1) % n
            block_id[y0:y1][:, xs] = bid
            tone[y0:y1][:, xs] = t
            bid += 1
        # mortar grooves (horizontal + vertical), with wrap
        mortar[y0:y0+mortar_px, :] = True
        for b in range(blocks_per_course + 1):
            x0 = (b*bw - offset) % n
            for m in range(mortar_px):
                mortar[y0:y1, (x0+m) % n] = True
    return block_id, mortar, tone

# ---- stylized carved hieroglyph frieze (one band) ----
def glyph_band(n, y0, y1, seed):
    """Returns a {0..1} mask of carved glyph strokes within [y0,y1)."""
    img = Image.new("L", (n, n), 0); d = ImageDraw.Draw(img)
    r = rng(seed)
    cell = (y1 - y0) - 24
    step = cell + 28
    x = 14
    while x < n - cell:
        cy = (y0 + y1)//2; s = cell//2
        k = int(r.integers(0, 6))
        w = max(3, cell//10)
        if k == 0:    # eye of horus-ish: circle + tail
            d.ellipse([x, cy-s//2, x+s, cy+s//2], outline=255, width=w)
            d.line([x+s, cy, x+s+s//2, cy+s//2], fill=255, width=w)
        elif k == 1:  # ankh: loop + cross
            d.ellipse([x+s//4, cy-s, x+3*s//4, cy-s//3], outline=255, width=w)
            d.line([x+s//2, cy-s//3, x+s//2, cy+s], fill=255, width=w)
            d.line([x+s//6, cy, x+5*s//6, cy], fill=255, width=w)
        elif k == 2:  # water ripples
            for yy in range(3):
                yb = cy-s//2 + yy*(s//2)
                d.line([(x,yb),(x+s//3,yb-6),(x+2*s//3,yb+6),(x+s,yb)], fill=255, width=w)
        elif k == 3:  # bird-ish chevrons
            d.line([x, cy+s, x+s//2, cy-s], fill=255, width=w)
            d.line([x+s//2, cy-s, x+s, cy+s], fill=255, width=w)
            d.line([x+s//4, cy, x+3*s//4, cy], fill=255, width=w)
        elif k == 4:  # sun disk + rays
            d.ellipse([x+s//4, cy-s//3, x+3*s//4, cy+s//3], fill=255)
        else:         # vertical reed strokes
            for rr in range(3):
                xb = x + s//4 + rr*(s//4)
                d.line([xb, cy-s, xb, cy+s], fill=255, width=w)
        x += step
    return np.asarray(img, dtype=np.float64)/255.0

def save_jpg(arr_rgb, path):
    Image.fromarray(np.clip(arr_rgb,0,255).astype(np.uint8), "RGB").save(path, quality=92)
def save_png_gray(arr, path):
    Image.fromarray(np.clip(arr*255,0,255).astype(np.uint8), "L").save(path)

def normal_preview(height, strength, path):
    gx = np.gradient(height, axis=1); gy = np.gradient(height, axis=0)
    nx = -gx*strength; ny = -gy*strength; nz = np.ones_like(height)
    inv = 1.0/np.sqrt(nx*nx+ny*ny+nz*nz)
    nx*=inv; ny*=inv; nz*=inv
    rgb = np.stack([(nx+1)*127.5,(ny+1)*127.5,(nz+1)*127.5],-1)
    Image.fromarray(np.clip(rgb,0,255).astype(np.uint8),"RGB").save(path)

def gen_wall(outdir):
    base = np.array([196, 170, 126], float)         # warm limestone
    block_id, mortar, tone = masonry(N, courses=7, blocks_per_course=4, mortar_px=10, seed=11)
    grain   = fbm(N, 8, 5, seed=3)                  # fine stone grain
    weather = fbm(N, 3, 4, seed=7)                  # large weathering blotches
    # diffuse colour
    shade = 1.0 + tone + (grain-0.5)*0.22 + (weather-0.5)*0.20
    rgb = base[None,None,:] * shade[...,None]
    rgb[mortar] *= 0.62                             # darker mortar
    # carved hieroglyph frieze on course 2
    ch = N//7
    glyph = glyph_band(N, y0=2*ch+12, y1=3*ch-12, seed=21)
    rgb[glyph>0.5] *= 0.70
    # height: blocks high, mortar low, glyphs carved in, + grain
    h = np.full((N,N), 0.72)
    h[mortar] = 0.18
    h -= glyph*0.28
    h += (grain-0.5)*0.06
    h = normalize01(h)
    save_jpg(rgb, f"{outdir}/greets_wall.jpg")
    save_png_gray(h, f"{outdir}/greets_wall_h.png")
    normal_preview(h, strength=2.5, path=f"{outdir}/greets_wall_normal_preview.png")
    print("wall: greets_wall.jpg + greets_wall_h.png + preview")

if __name__ == "__main__":
    surf = sys.argv[1] if len(sys.argv)>1 else "wall"
    out  = sys.argv[2] if len(sys.argv)>2 else "."
    {"wall": gen_wall}[surf](out)
