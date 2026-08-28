#!/usr/bin/env python3
"""changed-px census between two PPMs: count, %, max|d|, mean|d| on changed."""
import sys, numpy as np
from PIL import Image
a=np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.int16)
b=np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(np.int16)
if a.shape!=b.shape: print("SIZE MISMATCH",a.shape,b.shape); sys.exit(1)
d=np.abs(a-b)
pixmask=d.max(axis=2)>0
n=int(pixmask.sum()); tot=pixmask.size
mx=int(d.max())
mean_on_changed=float(d[pixmask].mean()) if n else 0.0
mean_all=float(d.mean())
hist={}
for lvl in (1,2,3,4,8,12):
    hist[lvl]=int((d.max(axis=2)>=lvl).sum())
print(f"{sys.argv[3] if len(sys.argv)>3 else ''} changed={n} ({100.0*n/tot:.4f}%) max|d|={mx} mean|d|(changed ch)={mean_on_changed:.4f} mean|d|(all)={mean_all:.5f} px>=1:{hist[1]} >=2:{hist[2]} >=3:{hist[3]} >=4:{hist[4]} >=8:{hist[8]} >=12:{hist[12]}")
