#!/usr/bin/env python3
"""reflmir_hot.py A.ppm B.ppm [CW CH] — top-3 non-overlapping windows by summed |delta|."""
import sys
import numpy as np

def read_ppm(p):
    d=open(p,'rb').read(); i=0; f=[]
    while len(f)<4:
        while d[i:i+1].isspace(): i+=1
        if d[i:i+1]==b'#':
            while d[i:i+1]!=b'\n': i+=1
            continue
        j=i
        while not d[j:j+1].isspace(): j+=1
        f.append(d[i:j]); i=j
    i+=1; w,h=int(f[1]),int(f[2])
    return np.frombuffer(d[i:i+w*h*3],dtype=np.uint8).reshape(h,w,3)

A=read_ppm(sys.argv[1]).astype(np.int16); B=read_ppm(sys.argv[2]).astype(np.int16)
CW=int(sys.argv[3]) if len(sys.argv)>3 else 480
CH=int(sys.argv[4]) if len(sys.argv)>4 else 320
D=np.abs(A-B).max(axis=2).astype(np.int64)
H,W=D.shape
S=np.zeros((H+1,W+1),np.int64); S[1:,1:]=D.cumsum(0).cumsum(1)
def box(y,x): return S[y+CH,x+CW]-S[y,x+CW]-S[y+CH,x]+S[y,x]
step=40
cands=[]
for y in range(0,H-CH+1,step):
    for x in range(0,W-CW+1,step):
        cands.append((box(y,x),x,y))
cands.sort(reverse=True)
picked=[]
for v,x,y in cands:
    if all(abs(x-px)>=CW*0.6 or abs(y-py)>=CH*0.6 for _,px,py in picked):
        picked.append((v,x,y))
    if len(picked)==3: break
for v,x,y in picked:
    sub=D[y:y+CH,x:x+CW]
    print("x=%d y=%d w=%d h=%d  sum|d|=%d  changed=%d  max=%d" % (x,y,CW,CH,v,int((sub>0).sum()),int(sub.max())))
