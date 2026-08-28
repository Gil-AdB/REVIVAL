#!/usr/bin/env python3
"""look package: census + before/after/diff crops at the worst 320x240 region."""
import sys, numpy as np
from PIL import Image
a_p,b_p,outbase,label = sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4]
A=Image.open(a_p).convert("RGB"); B=Image.open(b_p).convert("RGB")
a=np.asarray(A).astype(np.int16); b=np.asarray(B).astype(np.int16)
d=np.abs(a-b); dm=d.max(axis=2)
n=int((dm>0).sum()); tot=dm.size
print(f"{label}: changed={n} ({100.0*n/tot:.3f}%) max|d|={int(d.max())} "
      f"mean|d| on changed(ch)={float(d[dm>0].mean()) if n else 0:.4f} "
      f"mean|d| all={float(d.mean()):.5f} px>=2:{int((dm>=2).sum())} px>=4:{int((dm>=4).sum())} px>=8:{int((dm>=8).sum())}")
# worst 320x240 window by summed delta (coarse search on a 40px grid)
H,W=dm.shape; cw,ch=320,240; best=(-1,0,0)
integ=np.cumsum(np.cumsum(dm.astype(np.int64),0),1)
def s(y,x):
    y2,x2=min(y+ch,H)-1,min(x+cw,W)-1
    t=integ[y2,x2]
    if y>0: t-=integ[y-1,x2]
    if x>0: t-=integ[y2,x-1]
    if y>0 and x>0: t+=integ[y-1,x-1]
    return t
for y in range(0,H-ch,40):
    for x in range(0,W-cw,40):
        v=s(y,x)
        if v>best[0]: best=(v,y,x)
_,y,x=best
box=(x,y,min(x+cw,W),min(y+ch,H))
A.crop(box).save(outbase+"_before.png")
B.crop(box).save(outbase+"_after.png")
amp=np.clip(d[y:y+ch,x:x+cw].astype(np.int32)*24,0,255).astype(np.uint8)
Image.fromarray(amp).save(outbase+"_diff24x.png")
print(f"   worst 320x240 window at ({x},{y}); crops -> {outbase}_[before|after|diff24x].png")
