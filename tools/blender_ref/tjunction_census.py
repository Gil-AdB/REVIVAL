# T-junction census of the authored greets stone OBJ (engine coordinates): weld at eps, then every
# vertex lying strictly inside another face's edge (off both ends by > eps, within eps of the line).
import sys, numpy as np
eps=1e-3
V=[]; F=[]; mat=None; FM=[]
for ln in open(sys.argv[1]):
    p=ln.split()
    if not p: continue
    if p[0]=="v": V.append([float(x) for x in p[1:4]])
    elif p[0]=="usemtl": mat=p[1]
    elif p[0]=="f": F.append([int(t.split("/")[0])-1 for t in p[1:4]]); FM.append(mat)
V=np.array(V); n=len(V)
# weld
rep=np.arange(n)
for i in range(n):
    if rep[i]!=i: continue
    d=np.linalg.norm(V-V[i],axis=1); close=np.where((d<eps)&(rep==np.arange(n)))[0]
    for j in close: rep[j]=i
W=sorted(set(rep)); wi={w:k for k,w in enumerate(W)}; P=V[W]
Fw=[[wi[rep[a]] for a in f] for f in F]
print("authored %d verts / %d faces -> %d welded verts"%(n,len(F),len(P)))
# edges (undirected) with owning faces
edges={}
for fi,f in enumerate(Fw):
    for k in range(3):
        a,b=f[k],f[(k+1)%3]
        if a==b: continue
        e=(min(a,b),max(a,b)); edges.setdefault(e,[]).append(fi)
tj=[]
for (a,b),owners in edges.items():
    A,Bp=P[a],P[b]; ab=Bp-A; L=np.linalg.norm(ab)
    if L<eps: continue
    d=ab/L
    for v in range(len(P)):
        if v==a or v==b: continue
        t=np.dot(P[v]-A,d)
        if t<=eps or t>=L-eps: continue
        if np.linalg.norm((P[v]-A)-d*t)>eps: continue
        # is v already a vertex of one of the owner faces? (then it's not a T for that face)
        if all(v in Fw[o] for o in owners): continue
        tj.append((v,a,b,t/L,sorted(set(FM[o] for o in owners))))
print("T-junctions (vertex strictly inside another face's edge): %d"%len(tj))
# group by the host edge's line: direction + a point
lines={}
for v,a,b,s,mats in tj:
    d=P[b]-P[a]; d/=np.linalg.norm(d); d=d if (d[np.argmax(np.abs(d))]>0) else -d
    key=(tuple(np.round(d,2)), tuple(np.round(P[a]-np.dot(P[a],d)*d,1)))
    lines.setdefault(key,[]).append((v,a,b,s,mats))
print("%d host lines"%len(lines))
for key,items in sorted(lines.items(), key=lambda kv:(-len(kv[1]))):
    d,p0=key; vs=[P[i[0]] for i in items]
    lo=np.min(vs,axis=0); hi=np.max(vs,axis=0)
    print("line dir=(%.2f,%.2f,%.2f)  %d T-verts  span x[%.2f..%.2f] y[%.2f..%.2f] z[%.2f..%.2f]  host mats %s"%(d[0],d[1],d[2],len(items),lo[0],hi[0],lo[1],hi[1],lo[2],hi[2],sorted(set(m for i in items for m in i[4]))))
    for v,a,b,s,mats in items:
        print("   T-vert (%.3f,%.3f,%.3f) at %.3f along host edge (%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f)"%(P[v][0],P[v][1],P[v][2],s,P[a][0],P[a][1],P[a][2],P[b][0],P[b][1],P[b][2]))
