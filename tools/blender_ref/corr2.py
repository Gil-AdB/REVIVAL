# High-pass luminance correlation of a Blender frame against an FDS frame, at unit scale, best of a +-6 px shift search; whole frame and a crop box
import sys, numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter
def hp(a): return gaussian_filter(a,1.5)-gaussian_filter(a,12)
def load(p): return np.asarray(Image.open(p).convert("L"),dtype=float)
def best(F,Bw):
    b=(-9,0,0)
    for dy in range(-6,7,2):
        for dx in range(-6,7,2):
            c=np.corrcoef(F.ravel(),np.roll(np.roll(Bw,dy,axis=0),dx,axis=1).ravel())[0,1]
            if c>b[0]: b=(c,dx,dy)
    return b
a=hp(load(sys.argv[1])); f=hp(load(sys.argv[2])); box=[int(v) for v in sys.argv[3].split(",")]
whole=(slice(40,1040),slice(40,1880)); crop=(slice(box[1],box[3]),slice(box[0],box[2]))
w=best(f[whole],a[whole]); c=best(f[crop],a[crop])
print("%s vs %s: whole %.3f (dx%+d dy%+d)  crop %.3f (dx%+d dy%+d)"%(sys.argv[1].split("/")[-1],sys.argv[2].split("/")[-2],w[0],w[1],w[2],c[0],c[1],c[2]))
