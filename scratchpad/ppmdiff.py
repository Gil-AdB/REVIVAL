#!/usr/bin/env python3
"""ppmdiff.py A.ppm B.ppm — pixel delta report for a judge call."""
import sys
def read(p):
    d=open(p,'rb').read()
    # P6\n<w> <h>\n255\n
    i=0; f=[]
    while len(f)<4:
        while d[i:i+1].isspace(): i+=1
        if d[i:i+1]==b'#':
            while d[i:i+1]!=b'\n': i+=1
            continue
        j=i
        while not d[j:j+1].isspace(): j+=1
        f.append(d[i:j]); i=j
    i+=1
    w,h=int(f[1]),int(f[2])
    return w,h,d[i:i+w*h*3]
wa,ha,A=read(sys.argv[1]); wb,hb,B=read(sys.argv[2])
assert (wa,ha)==(wb,hb), "size mismatch"
n=wa*ha
diff=0; maxd=0; hist={}
for k in range(n):
    o=k*3
    d0=abs(A[o]-B[o]); d1=abs(A[o+1]-B[o+1]); d2=abs(A[o+2]-B[o+2])
    m=max(d0,d1,d2)
    if m:
        diff+=1; maxd=max(maxd,m); hist[m]=hist.get(m,0)+1
print("%s vs %s: %dx%d=%d px" % (sys.argv[1].split('/')[-1], sys.argv[2].split('/')[-1], wa,ha,n))
print("  changed: %d px (%.4f%%)   max |delta| = %d/255" % (diff, 100.0*diff/n, maxd))
if hist:
    print("  histogram: " + ", ".join("|d|=%d:%d"%(k,v) for k,v in sorted(hist.items())[:8]))
