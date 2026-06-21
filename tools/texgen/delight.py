#!/usr/bin/env python3
"""De-light an AI/photo texture into a flat albedo + infer a height/normal map.

The greets HDR-linear path squares the albedo texel (gamma-2.0 decode) then
multiplies by scene light, so any lighting baked into the source darkens twice.
This recovers a flat base colour and moves the relief into a height map the
engine bakes a tangent-space normal from (BakeNormalMapFromDiffuse reads raw
luminance — no gamma — so the height map is authored in linear/raw values).

Outputs (1024², for <name>):
  <name>_albedo.jpg            flat base colour, sRGB
  <name>_h.png                 grayscale height (drives the baked normal)
  <name>_normal_preview.png    engine-encoding normal, for eyeballing only

Method:
  * homomorphic de-light in *linear* space: divide by a large gaussian of
    luminance (the low-freq lighting), renormalise to the mean → kills global
    gradient + per-block shading while keeping albedo detail.
  * height = band-passed luminance of the ORIGINAL (the baked shading IS the
    relief signal): subtract a large blur (drop global light tilt), keep mid/high
    freq (mortar grooves, carved glyphs), normalise.

Usage: python3 delight.py <input.jpg> <outdir> [name] [--delight-sigma F] [--height-sigma F] [--strength F]
"""
import sys, numpy as np
from PIL import Image, ImageFilter

def srgb_to_lin(a): return np.power(np.clip(a,0,1), 2.0)      # gamma-2.0 (matches engine)
def lin_to_srgb(a): return np.power(np.clip(a,0,1), 1/2.0)
def luma(rgb):      return 0.299*rgb[...,0]+0.587*rgb[...,1]+0.114*rgb[...,2]

def gblur(arr, sigma):
    im = Image.fromarray(np.clip(arr*255,0,255).astype(np.uint8))
    return np.asarray(im.filter(ImageFilter.GaussianBlur(sigma)),float)/255.0

def main():
    inp, outdir = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv)>3 and not sys.argv[3].startswith('--') else \
           inp.rsplit('/',1)[-1].rsplit('.',1)[0]
    def opt(flag, d):
        return float(sys.argv[sys.argv.index(flag)+1]) if flag in sys.argv else d
    dl_sigma = opt('--delight-sigma', None)     # default: 1/6 of width
    h_sigma  = opt('--height-sigma', 1.5)
    strength = opt('--strength', 2.5)

    no_delight = '--no-delight' in sys.argv
    img = np.asarray(Image.open(inp).convert('RGB').resize((1024,1024)),float)/255.0
    W = img.shape[0]
    if dl_sigma is None: dl_sigma = W/6.0

    if no_delight:
        # Originals (tiling brick/hex) aren't AI-shaded with a global gradient —
        # keep their look verbatim; we only want the dedicated normal from them.
        albedo = img
    else:
        # --- de-light (homomorphic, linear) ---
        lin = srgb_to_lin(img)
        L   = luma(lin) + 1e-4
        light = gblur(L, dl_sigma)                  # low-freq lighting estimate
        light /= light.mean()
        albedo_lin = lin / light[...,None]
        # gentle highlight knockdown so blown specular doesn't survive as white
        albedo_lin = np.clip(albedo_lin, 0, np.percentile(albedo_lin, 99.5))
        albedo_lin /= albedo_lin.max()
        albedo = lin_to_srgb(albedo_lin)

    # --- height = band-passed original luminance (the baked relief) ---
    Lo   = luma(img)
    base = gblur(Lo, W/8.0)                      # global light tilt to remove
    hp   = (Lo - base)                           # mortar grooves + carvings survive
    h    = gblur(hp, h_sigma)
    h    = (h - h.min())/(h.max()-h.min()+1e-9)

    # --- normal preview (engine encoding) ---
    gx = np.gradient(h,axis=1); gy = np.gradient(h,axis=0)
    nx,ny,nz = -gx*strength, -gy*strength, np.ones_like(h)
    inv = 1.0/np.sqrt(nx*nx+ny*ny+nz*nz); nx*=inv; ny*=inv; nz*=inv
    nrm = np.stack([(nx+1)*127.5,(ny+1)*127.5,(nz+1)*127.5],-1)

    Image.fromarray(np.clip(albedo*255,0,255).astype(np.uint8),'RGB').save(f"{outdir}/{name}_albedo.jpg",quality=92)
    Image.fromarray(np.clip(h*255,0,255).astype(np.uint8),'L').save(f"{outdir}/{name}_h.png")
    Image.fromarray(np.clip(nrm,0,255).astype(np.uint8),'RGB').save(f"{outdir}/{name}_normal_preview.png")
    print(f"{name}: albedo (de-lit, sRGB) + height + normal preview  [delight σ={dl_sigma:.0f}, strength={strength}]")

if __name__ == "__main__":
    main()
