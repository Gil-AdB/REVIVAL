#include <StdIO.H>
#include <StdLIB.H>
#include <String.H>

#include "../FDS_Vars.H"
#include "../FDS_Decs.H"
#include "../FDS_Defs.H"

float FOVX,FOVY;

// Advanced Frustrum Elite (Nonhomeginous / Triangle / Universal)
// Clip-vertex interpolators. Out = A*R + B*L, where BOTH weights are handed in, each from
// its OWN division (R = -dB/(dA-dB), L = dA/(dA-dB), d = signed distance to the clip plane).
// They are NOT R = 1-L: with a 24-bit mantissa (FPU_LPrecision pins PC=single) a weight of
// 1-1.5e-6 rounds to 1.0f, so `1-L` collapsed to 0 and the clip vertex snapped onto the far
// endpoint. That is what ate the left edge of the city water: a near-clipped vertex has
// PX = TPos.X/NZP ~ 2e8, so L was 1-1.6e-6 and the X=0 clip point landed up to 32px inside
// the screen. Two direct divisions keep full relative precision in BOTH weights, so
// A*R + B*L lands on the plane to ~1e-4 px no matter how far off-screen A is.
// The callers then SNAP the clipped axis to its exact plane value (PX=0, PX=XClip, PY=0,
// PY=YClip): interpolating a coordinate we already know can only round it. That matters for
// the one case two accurate weights can't save - an edge whose BOTH endpoints are near-clip
// vertices at +-2e8, where reaching 0 means cancelling two ~1e8 floats and ulp(1e8)=8px.
void (*Frust_2DI)(Vertex *A,Vertex *B,Vertex *Out,float L,float R);
void (*Frust_3DI)(Vertex *A,Vertex *B,Vertex *Out,float L,float R);
void (*Rasterizer)(Vertex *A,Vertex *B,Vertex *C);

// Avatar is back...
Vertex F_VB[12];  // Vertex Buffer - for clipper generated vertices.

// Clipper fan-out census: faces entering the screen-edge cascade vs triangles leaving it.
// UFE1..UFE4 are triangle-in/triangles-out, so one face crossing two screen edges can leave
// as four, each paying its own filler setup and carrying an internal diagonal. The ratio is
// the ceiling on what an n-gon walker can save. File scope, not inside an #ifdef -- a counter
// that only exists in one build configuration silently reads as zero in the other.
extern "C" { long g_ClipFaces = 0; long g_ClipTris = 0; long g_DirectTris = 0;
             long g_ScreenPolys = 0; }   // g_ScreenPolys: entries to the SCREEN-edge clip,
// which separates the screen fan from the near/far fan already counted in g_ClipTris.
void Vertex_Interpolate3D_Flat(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  // Near-plane clip vertex. TPos is HOMOGENEOUS (screenX*Z); screen PX = TPos.X/Z. At the
  // near plane Z=C_NZP, so PX = interp(TPos.X)*C_rNZP and RZ = C_rNZP.
  // (When the plane was hard-coded 1.0 this reduced to PX=TPos.X, RZ=1 - the original form;
  // moving the plane without this scale pulled every clipped vertex toward the origin =
  // the "missing / weird" clipped faces.) The plane is PER SCENE now: PX scales with 1/NZP,
  // and PX beyond ~1e8 makes the float32 screen-edge clip below lose whole pixels.
  float rn = C_rNZP;
  Out->PX = (A->TPos.X*R + B->TPos.X*L)*rn;
  Out->PY = (A->TPos.Y*R + B->TPos.Y*L)*rn;
#ifdef FDS_ZFLIP
  Out->RZ = rn;                    // 1/Z at the near plane (feeds the z-buffer code)
  Out->LR = A->LR*R + B->LR*L;     // Gouraud light is screen-linear: NOT scaled by rn
#endif
}

void Vertex_Interpolate2D_Flat(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;
#ifdef FDS_ZFLIP
  Out->RZ = A->RZ*R + B->RZ*L;   // 1/w is screen-linear: plain lerp is exact
  Out->LR = A->LR*R + B->LR*L;
#endif
}

void Frust_Set_Flat()
{
  Frust_2DI = &Vertex_Interpolate2D_Flat;
  Frust_3DI = &Vertex_Interpolate3D_Flat;
}

void Vertex_Interpolate3D_L(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  float rn = C_rNZP;
  Out->PX = (A->TPos.X*R + B->TPos.X*L)*rn;
  Out->PY = (A->TPos.Y*R + B->TPos.Y*L)*rn;
  Out->LR = A->LR*R + B->LR*L;     // light: screen-linear, unscaled
  Out->LG = A->LG*R + B->LG*L;
  Out->LB = A->LB*R + B->LB*L;
#ifdef FDS_ZFLIP
  Out->RZ = rn;
#endif
}

void Vertex_Interpolate2D_L(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;

  Out->LR = A->LR*R + B->LR*L;
  Out->LG = A->LG*R + B->LG*L;
  Out->LB = A->LB*R + B->LB*L;
#ifdef FDS_ZFLIP
  Out->RZ = A->RZ*R + B->RZ*L;
#endif
}

void Frust_Set_L()
{
  Frust_2DI = &Vertex_Interpolate2D_L;
  Frust_3DI = &Vertex_Interpolate3D_L;
}


void Vertex_Interpolate3D_Env(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  float rn = C_rNZP;
  Out->PX = (A->TPos.X*R + B->TPos.X*L)*rn;
  Out->PY = (A->TPos.Y*R + B->TPos.Y*L)*rn;
  Out->RZ = rn;
  Out->REU = (A->EU*R + B->EU*L)*rn;   // env coord carried as U*RZ, like RU below
  Out->REV = (A->EV*R + B->EV*L)*rn;
}

void Vertex_Interpolate2D_Env(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;

  Out->REU = A->REU*R + B->REU*L;
  Out->REV = A->REV*R + B->REV*L;
  Out->RZ = A->RZ*R + B->RZ*L;
}

void Frust_Set_Env()
{
  Frust_2DI = &Vertex_Interpolate2D_Env;
  Frust_3DI = &Vertex_Interpolate3D_Env;
}


void Vertex_Interpolate3D_UV(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  float rn = C_rNZP;
  Out->PX = (A->TPos.X*R + B->TPos.X*L)*rn;
  Out->PY = (A->TPos.Y*R + B->TPos.Y*L)*rn;
  Out->RZ = rn;
  Out->RU = (A->U*R + B->U*L)*rn;   // RU is U*RZ; rasterizer does U = RU/RZ per pixel
  Out->RV = (A->V*R + B->V*L)*rn;
}

void Vertex_Interpolate2D_UV(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;

  Out->RU = A->RU*R + B->RU*L;
  Out->RV = A->RV*R + B->RV*L;
  Out->RZ = A->RZ*R + B->RZ*L;
}

void Frust_Set_UV()
{
  Frust_2DI = &Vertex_Interpolate2D_UV;
  Frust_3DI = &Vertex_Interpolate3D_UV;
}

void Vertex_Interpolate3D_UVL(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  float rn = C_rNZP;
  Out->PX = (A->TPos.X*R + B->TPos.X*L)*rn;
  Out->PY = (A->TPos.Y*R + B->TPos.Y*L)*rn;
  Out->RZ = rn;
  Out->RU = (A->U*R + B->U*L)*rn;
  Out->RV = (A->V*R + B->V*L)*rn;
  Out->LR = A->LR*R + B->LR*L;     // light: unscaled
  Out->LG = A->LG*R + B->LG*L;
  Out->LB = A->LB*R + B->LB*L;
}

void Vertex_Interpolate2D_UVL(Vertex *A,Vertex *B,Vertex *Out,float L,float R)
{
  Out->PX = A->PX*R + B->PX*L;
  Out->PY = A->PY*R + B->PY*L;

  Out->RU = A->RU*R + B->RU*L;
  Out->RV = A->RV*R + B->RV*L;
  Out->RZ = A->RZ*R + B->RZ*L;
  Out->LR = A->LR*R + B->LR*L;
  Out->LG = A->LG*R + B->LG*L;
  Out->LB = A->LB*R + B->LB*L;
}

void Frust_Set_UVL()
{
  Frust_2DI = &Vertex_Interpolate2D_UVL;
  Frust_3DI = &Vertex_Interpolate3D_UVL;
}

// One reciprocal, two multiplies -> both weights, each accurate to full relative precision.
// dA/dB are the signed distances of A/B to the plane being clipped (dA and dB have opposite
// signs at the call sites, so dA-dB never cancels).
#define CLIPI2(a,b,out,dA,dB) do{ float _da=(dA),_db=(dB),_id=1.0f/(_da-_db); \
                                  Frust_2DI((a),(b),(out),_da*_id,-_db*_id); }while(0)
#define CLIPI3(a,b,out,dA,dB) do{ float _da=(dA),_db=(dB),_id=1.0f/(_da-_db); \
                                  Frust_3DI((a),(b),(out),_da*_id,-_db*_id); }while(0)

// Maps a 3-vertex filler to its n-gon twin, or 0 when that filler has no n-gon entry
// yet - fillers convert one at a time and the cascade stays the fallback for the rest.
extern "C" void *NGon_FillerFor(void *triFiller);

void UFE1(Vertex *A,Vertex *B,Vertex *C);
void UFE2(Vertex *A,Vertex *B,Vertex *C);
void UFE3(Vertex *A,Vertex *B,Vertex *C);
void UFE4(Vertex *A,Vertex *B,Vertex *C);

// ---------------------------------------------------------------------------------------
// FDS_NGON: screen clip as ONE Sutherland-Hodgman pass over a vertex list.
//
// UFE1..UFE4 are triangle-in/triangles-out: each stage that cuts a corner off produces a
// quad and immediately splits it back into two triangles, which then re-enter the remaining
// stages. A face crossing two screen edges leaves as four triangles - four filler setups,
// and three interior diagonals that were never part of the model. Measured on city: 1.65
// triangles out per clipped face, and the extra 0.65 is 10.8% of every triangle the frame
// draws (21% on the water-heavy frames). Those extras are also the worst ones to pay for:
// clip slivers, where the per-section FPU setup dwarfs the inner loop.
//
// The same four planes over a vertex list give one convex polygon and one filler call.
// Convex in -> convex out, and each plane adds at most one vertex, so 3 in -> 7 out.
// Same CLIPI2 weights and the same exact-plane snap as the cascade, so clip points are
// bit-identical to what UFE1..UFE4 produced - this changes the topology, not the geometry.
// 6 planes x at most 2 crossings each = 12 generated vertices from a triangle; 16 for slack.
Vertex P_VB[16];                    // clip-generated vertices for the polygon path
long   P_VB_Plane[16];              // which plane produced each (FDS_NGONLOG probe)
void (*RasterizerN)(Vertex **V,long n);

#ifdef FDS_ZLINEAR
static void FarClipVtx(Vertex *A,Vertex *B,Vertex *Out);
#endif

// Signed distance to each clip plane, >= 0 == inside. Same inside tests as the cascade.
// Planes 0..1 are the FAR and NEAR planes in view space, 2..5 the screen edges. Measuring
// the two stages separately showed the near/far fan (1.57x) is nearly three times the
// screen-edge fan (1.13x), so a polygon path that stopped at the screen edges would have
// left 74% of the duplicated triangles in place.
static float PlaneDist(Vertex *V,long plane)
{
  switch (plane)
  {
    case 0:  return C_FZP - V->TPos.Z;
    case 1:  return V->TPos.Z - C_NZP;
    case 2:  return V->PX;
    case 3:  return XRes_1 - V->PX;
    case 4:  return V->PY;
    default: return YRes_1 - V->PY;
  }
}

static void PlaneSnap(Vertex *V,long plane)
{
  switch (plane)
  {
    case 2:  V->PX = 0.0f;    break;
    case 3:  V->PX = XRes_1;  break;
    case 4:  V->PY = 0.0f;    break;
    default: V->PY = YRes_1;  break;
  }
}

// One SH pass. Emitting cur BEFORE the crossing vertex preserves winding, which the
// chain builder in FILLERS.CPP relies on to tell the left edge from the right one.
static long SH_Clip(Vertex **in,long nIn,Vertex **out,long *poolUsed,long plane)
{
  long i,nOut = 0;
  for (i = 0; i < nIn; i++)
  {
    Vertex *cur = in[i];
    Vertex *nxt = in[(i+1==nIn) ? 0 : (i+1)];
    float dc = PlaneDist(cur,plane);
    float dn = PlaneDist(nxt,plane);
    if (dc >= 0.0f) out[nOut++] = cur;
    if ((dc >= 0.0f) != (dn >= 0.0f))
    {
      Vertex *o;
      if (*poolUsed >= 16)          // convex input cannot reach this; if it ever does the
      {                             // polygon is malformed and silently truncating it would
        fprintf(LogFile,"NGON: clip pool exhausted (plane %ld, nIn %ld)\n",plane,nIn);
        fflush(LogFile);            // hide the real bug behind a wrong-looking face.
        return 0;
      }
      o = P_VB + *poolUsed;
      P_VB_Plane[*poolUsed] = plane;
      (*poolUsed)++;
      // Each plane keeps the interpolator the cascade used for it, so the clip vertices are
      // bit-identical to what UFE_NearClip / the far stage produced.
#ifdef FDS_ZLINEAR
      if (plane == 0) FarClipVtx(cur,nxt,o);          // snaps TPos.Z to C_FZP itself
      else
#endif
      if (plane == 1) CLIPI3(cur,nxt,o,dc,dn);        // 3D interpolator: lands PX/PY at NZP
      else { CLIPI2(cur,nxt,o,dc,dn); PlaneSnap(o,plane); }
      out[nOut++] = o;
    }
  }
  return nOut;
}

// Returns 1 when the polygon was rasterized here, 0 to let the caller run the cascade.
static long UFE_Poly(Vertex *A,Vertex *B,Vertex *C)
{
  Vertex *bufA[16],*bufB[16];
  Vertex **in = bufA,**out = bufB,**t;
  long n = 3,nPool = 0,p;

  bufA[0] = A; bufA[1] = B; bufA[2] = C;
#ifdef FDS_ZLINEAR
  for (p = 0; p < 6; p++)           // far, near, then the four screen edges
#else
  for (p = 1; p < 6; p++)           // no far plane in this build
#endif
  {
    n = SH_Clip(in,n,out,&nPool,p);
    if (n < 3) return 1;            // fully outside this plane, or a degenerate sliver
    t = in; in = out; out = t;
  }
#ifdef FDS_NGONLOG
  // Probe: is a clip vertex's RU/RV/RZ outside the range the input triangle spans? Every
  // clip vertex is a convex combination of two input vertices, so it MUST be in range --
  // anything else means a field the interpolator never wrote. Reports which plane made it.
  { static long _nlog = 0;
    // Every field the FILLER reads, not just the mapping ones -- the light (LR/LG/LB) is
    // read by the fused gouraud sweep, and a saturated light is exactly what a solid
    // yellow slab looks like.
    #define NF 6
    float lo[NF],hi[NF]; long k,j;
    static const char *nm[NF] = {"RU","RV","RZ","LR","LG","LB"};
    Vertex *src[3]; src[0]=A; src[1]=B; src[2]=C;
    for (j=0;j<NF;j++){ lo[j]=1e30f; hi[j]=-1e30f; }
    for (k=0;k<3;k++)
    { float v[NF]; v[0]=src[k]->RU; v[1]=src[k]->RV; v[2]=src[k]->RZ;
                   v[3]=src[k]->LR; v[4]=src[k]->LG; v[5]=src[k]->LB;
      for (j=0;j<NF;j++){ if(v[j]<lo[j])lo[j]=v[j]; if(v[j]>hi[j])hi[j]=v[j]; } }
    for (k=0;k<n && _nlog<24;k++)
    { float v[NF]; v[0]=in[k]->RU; v[1]=in[k]->RV; v[2]=in[k]->RZ;
                   v[3]=in[k]->LR; v[4]=in[k]->LG; v[5]=in[k]->LB;
      for (j=0;j<NF && _nlog<24;j++)
      { float m=(hi[j]-lo[j])*0.01f+1e-6f;
        if (v[j]<lo[j]-m || v[j]>hi[j]+m)
        { long isPool = (in[k]>=P_VB && in[k]<P_VB+16);
          fprintf(LogFile,"NGONBAD n=%ld v%ld pool=%ld plane=%ld %s=%.4f range[%.4f..%.4f]\n",
            n,k,isPool,isPool?(long)P_VB_Plane[in[k]-P_VB]:-1L,
            nm[j],(double)v[j],(double)lo[j],(double)hi[j]);
          fflush(LogFile); _nlog++; }
      }
    }
    #undef NF
  }
#endif
#ifdef FDS_NGON_NOCLIP
  // Bisect harness: hand the walker ONLY polygons no plane actually cut. If the artifacts
  // survive this, the n-gon walker/chain builder is wrong on plain triangles; if they go
  // away, the clip vertices are.

#endif
  g_ClipTris++;
  RasterizerN(in,n);
  return 1;
}
#define SCREENCLIP(a,b,c) do{ g_ScreenPolys++; UFE1((a),(b),(c)); }while(0)

#ifdef FDS_CLIPCHK
void Vertex_Interpolate2D_UV(Vertex *A,Vertex *B,Vertex *Out,float L,float R);
void Vertex_Interpolate2D_UVL(Vertex *A,Vertex *B,Vertex *Out,float L,float R);
// Clipper self-check (diagnostic only, -dFDS_CLIPCHK). Answers "which plane is eating the
// geometry, and is it precision or a real cull?" without needing eyes on a frame:
//   * per-plane cull/clip counts        -> WHICH stage drops faces
//   * per-plane clip-vertex residual    -> the clip point should land exactly ON the plane;
//                                          a residual in PIXELS is the 24-bit-FPU precision loss
//   * max |PX|,|PY| of a near-clip vtx  -> the 1/NZP amplification actually being fed to the
//                                          screen-edge stages
//   * out-of-range rasterized verts     -> clipping that failed to bound the span
struct ClipChkT {
  long nFace,nFarCull,nFarClip,nNearCull,nNearClip;
  long nCullL,nCullR,nCullU,nCullD,nRast,nOOB;
  float resL,resR,resU,resD;
  float maxNearPX,maxNearPY,oobMax;
  // mapper-input range: the fillers turn these into 16.16 fixed point (u*65536 via FISTP),
  // so |u| >= 32768 overflows to the x87 indefinite integer = garbage texel index.
  float maxRZ,maxU,maxV,maxSpanRZ;
  long nUVovf,nBigTri,nUVovfLogged;
};
ClipChkT g_cc;
static void ccRes(float *slot,float v){ if (v<0) v=-v; if (v>*slot) *slot=v; }
void ClipChk_Reset(void){ memset(&g_cc,0,sizeof(g_cc)); }
void ClipChk_Dump(const char *tag)
{
  fprintf(LogFile,
    "CLIPCHK %s: faces=%ld far(cull=%ld clip=%ld) near(cull=%ld clip=%ld) "
    "edgecull(L=%ld R=%ld U=%ld D=%ld) rast=%ld oob=%ld(worst=%.1fpx) "
    "clipvtx-residual-px(L=%.2f R=%.2f U=%.2f D=%.2f) nearclip-max|PX|=%.0f |PY|=%.0f "
    "NZP=%g FZP=%g | mapper-in: maxRZ=%.1f max|u|=%.0f max|v|=%.0f uvOVF=%ld "
    "worst-span-RZratio=%.0f bigTri=%ld\n",
    tag,g_cc.nFace,g_cc.nFarCull,g_cc.nFarClip,g_cc.nNearCull,g_cc.nNearClip,
    g_cc.nCullL,g_cc.nCullR,g_cc.nCullU,g_cc.nCullD,g_cc.nRast,g_cc.nOOB,
    (double)g_cc.oobMax,(double)g_cc.resL,(double)g_cc.resR,(double)g_cc.resU,(double)g_cc.resD,
    (double)g_cc.maxNearPX,(double)g_cc.maxNearPY,(double)C_NZP,(double)C_FZP,
    (double)g_cc.maxRZ,(double)g_cc.maxU,(double)g_cc.maxV,g_cc.nUVovf,
    (double)g_cc.maxSpanRZ,g_cc.nBigTri);
  fflush(LogFile);
}
static void RastChk(Vertex *A,Vertex *B,Vertex *C)
{
  Vertex *v[3]; v[0]=A; v[1]=B; v[2]=C;
  int k; g_cc.nRast++;
  for (k=0;k<3;k++)
  {
    float ex=0.0f;
    if (v[k]->PX < -0.5f)        ex = -0.5f - v[k]->PX;
    else if (v[k]->PX > XRes_1+0.5f) ex = v[k]->PX - (XRes_1+0.5f);
    if (v[k]->PY < -0.5f)      { float e=-0.5f-v[k]->PY; if (e>ex) ex=e; }
    else if (v[k]->PY > YRes_1+0.5f) { float e=v[k]->PY-(YRes_1+0.5f); if (e>ex) ex=e; }
    if (ex>0.0f) { g_cc.nOOB++; if (ex>g_cc.oobMax) g_cc.oobMax=ex; break; }
  }
  // what the FILLER is being handed: RZ (=1/Z) and the recovered u=RU/RZ, v=RV/RZ.
  { float rzmin=1e30f,rzmax=0.0f;
    // RU/RV are only maintained by the UV/UVL interpolators; on a Flat/L/Env clip vertex they
    // are stale from an earlier face, so measuring them there is a false positive.
    int uvValid = (Frust_2DI==&Vertex_Interpolate2D_UV || Frust_2DI==&Vertex_Interpolate2D_UVL);
    for (k=0;k<3;k++)
    {
      float rz=v[k]->RZ;
      if (rz>rzmax) rzmax=rz;
      if (rz<rzmin) rzmin=rz;
      if (rz>g_cc.maxRZ) g_cc.maxRZ=rz;
      if (rz>0.0f && uvValid)
      {
        float u=v[k]->RU/rz, vv=v[k]->RV/rz;
        ccRes(&g_cc.maxU,u); ccRes(&g_cc.maxV,vv);
        if (u<-32768.0f||u>32768.0f||vv<-32768.0f||vv>32768.0f)
        {
          g_cc.nUVovf++;
          // WHOSE face is it? A sprite/flare (Mat_Virtual) carries junk in U/V that its
          // filler never reads, so it would be a false positive. Log the first few per frame
          // with the material flags, the filler, and the SOURCE UVs off the face.
          if (g_cc.nUVovfLogged<3 && DoFace)
          {
            g_cc.nUVovfLogged++;
            fprintf(LogFile,"  UVOVF u=%.0f v=%.0f RZ=%g PX=%.1f PY=%.1f clipvtx=%d "
                            "matFlags=%lx fill=%p srcUV=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
              (double)u,(double)vv,(double)rz,(double)v[k]->PX,(double)v[k]->PY,
              (int)(v[k]>=F_VB && v[k]<F_VB+12),
              (unsigned long)(DoFace->Txtr?DoFace->Txtr->Flags:0),(void*)DoFace->Filler,
              (double)DoFace->U1,(double)DoFace->V1,(double)DoFace->U2,(double)DoFace->V2,
              (double)DoFace->U3,(double)DoFace->V3);
            fflush(LogFile);
          }
        }
      }
    }
    if (rzmin>0.0f) { float r=rzmax/rzmin; if (r>g_cc.maxSpanRZ) g_cc.maxSpanRZ=r; }
  }
  // triangles covering a big chunk of the screen (bbox >= 25% of the viewport) - a
  // near-plane face that survived clipping badly shows up here.
  { float x0=A->PX,x1=A->PX,y0=A->PY,y1=A->PY;
    if (B->PX<x0)x0=B->PX; if (B->PX>x1)x1=B->PX;
    if (C->PX<x0)x0=C->PX; if (C->PX>x1)x1=C->PX;
    if (B->PY<y0)y0=B->PY; if (B->PY>y1)y1=B->PY;
    if (C->PY<y0)y0=C->PY; if (C->PY>y1)y1=C->PY;
    if ((x1-x0)*(y1-y0) >= 0.25f*XRes_1*YRes_1) g_cc.nBigTri++; }
  Rasterizer(A,B,C);
}
#define RAST(a,b,c) do{ g_ClipTris++; RastChk(a,b,c); }while(0)
#define CCNEAR() do{ ccRes(&g_cc.maxNearPX,F_VB[0].PX); ccRes(&g_cc.maxNearPX,F_VB[1].PX); \
                     ccRes(&g_cc.maxNearPY,F_VB[0].PY); ccRes(&g_cc.maxNearPY,F_VB[1].PY); }while(0)
#define CCEDGE(slot,a,b) do{ ccRes(&g_cc.slot,(a)); ccRes(&g_cc.slot,(b)); }while(0)
#else
#define RAST(a,b,c) do{ g_ClipTris++; Rasterizer(a,b,c); }while(0)
#define CCNEAR() ((void)0)
#define CCEDGE(slot,a,b) ((void)0)
#endif

// NEAR-plane clip + screen-edge dispatch for a (sub)triangle already known to be within
// the FAR plane. A,B,C need not be Z-sorted (far-clip sub-triangles aren't). Same logic as
// the original clipper's near stage; extracted so the far stage can feed sub-triangles in.
static void UFE_NearClip(Vertex *A,Vertex *B,Vertex *C)
{
  Vertex *T;
  if (A->TPos.Z>B->TPos.Z) {T=A; A=B; B=T;}
  if (A->TPos.Z>C->TPos.Z) {T=A; A=C; C=T;}
  if (B->TPos.Z>C->TPos.Z) {T=B; B=C; C=T;}

  if (A->TPos.Z>=C_NZP) {SCREENCLIP(A,B,C); return;}
#ifdef FDS_CLIPCHK
  if (C->TPos.Z<C_NZP) { g_cc.nNearCull++; return; }
  g_cc.nNearClip++;
#else
  if (C->TPos.Z<C_NZP) return;
#endif
  if (B->TPos.Z<C_NZP)
  {
    CLIPI3(B,C,F_VB  ,B->TPos.Z-C_NZP,C->TPos.Z-C_NZP);
    CLIPI3(A,C,F_VB+1,A->TPos.Z-C_NZP,C->TPos.Z-C_NZP);
    CCNEAR();
    SCREENCLIP(F_VB,F_VB+1,C);
    return;
  }
  CLIPI3(A,B,F_VB  ,A->TPos.Z-C_NZP,B->TPos.Z-C_NZP);
  CLIPI3(A,C,F_VB+1,A->TPos.Z-C_NZP,C->TPos.Z-C_NZP);
  CCNEAR();
  SCREENCLIP(F_VB,F_VB+1,B);
  SCREENCLIP(F_VB+1,B,C);
}

#ifdef FDS_ZLINEAR
// Far-plane clip vertex, ported from native FrustumClipper::Far() (FRUSTRUM.CPP): the clip
// point sits at Z=C_FZP, so its perspective attributes use C_rFZP (1/FZP), NOT 1/NEARZ.
// TPos.X/Y are homogeneous (screenX*Z), so PX = TPos.X*(1/FZP) = screen X at the far plane.
// Computes the superset of attributes (the specialized per-type Frust_3DI can't be reused for
// a different plane); the filler reads whatever it needs. TPos/U/V/EU are kept so a far-clip
// sub-triangle that ALSO crosses the near plane re-clips correctly via UFE_NearClip.
static void FarClipVtx(Vertex *A,Vertex *B,Vertex *Out)
{
  // two-weight form (see Frust_2DI note): d = C_FZP - Z, so wB = dA/(dA-dB), wA = -dB/(dA-dB)
  float dA = C_FZP - A->TPos.Z, dB = C_FZP - B->TPos.Z, id = 1.0f/(dA-dB);
  float t = dA*id, r = -dB*id;
  float rf = C_rFZP;
  Out->TPos.X = A->TPos.X*r + B->TPos.X*t;
  Out->TPos.Y = A->TPos.Y*r + B->TPos.Y*t;
  Out->TPos.Z = C_FZP;
  Out->PX = Out->TPos.X * rf;
  Out->PY = Out->TPos.Y * rf;
  Out->RZ = rf;
  Out->U = A->U*r + B->U*t;
  Out->V = A->V*r + B->V*t;
  Out->RU = Out->U * rf;
  Out->RV = Out->V * rf;
  Out->EU = A->EU*r + B->EU*t;
  Out->EV = A->EV*r + B->EV*t;
  Out->REU = Out->EU * rf;
  Out->REV = Out->EV * rf;
  Out->LR = A->LR*r + B->LR*t;
  Out->LG = A->LG*r + B->LG*t;
  Out->LB = A->LB*r + B->LB*t;
  Out->Flags = 0;
}
#endif

void Universal_Frustrum_Elite()
{
  DoFace->Clipper(); //Initiate global variables with Face clipper's requirements
  g_ClipFaces++;
  Rasterizer = DoFace->Filler;
  RasterizerN = (void (*)(Vertex **,long))NGon_FillerFor((void *)Rasterizer);
  Vertex *A = DoFace->A,*B = DoFace->B,*C = DoFace->C,*T;
#ifdef FDS_CLIPCHK
  g_cc.nFace++;
#endif
  // One SH pass over all six planes, one polygon, one filler call. No Z sort needed: the
  // cascade sorted only to pick which corner its two-triangle split kept.
  if (RasterizerN && UFE_Poly(A,B,C)) return;
  if (A->TPos.Z>B->TPos.Z) {T=A; A=B; B=T;}
  if (A->TPos.Z>C->TPos.Z) {T=A; A=C; C=T;}
  if (B->TPos.Z>C->TPos.Z) {T=B; B=C; C=T;}   // A nearest, C farthest

#ifdef FDS_ZLINEAR
  // FAR-plane clip (C is farthest). Beyond FZP the linear z-code would wrap to a false-near
  // value (fixed at the source here; the mapper clamp stays as a backstop). Fog fades this
  // geometry before the plane, so the hard cutoff is invisible. C_FZP = scene far plane.
  if (C->TPos.Z > C_FZP)
  {
#ifdef FDS_CLIPCHK
    if (A->TPos.Z > C_FZP) { g_cc.nFarCull++; return; }
    g_cc.nFarClip++;
#else
    if (A->TPos.Z > C_FZP) return;              // whole triangle beyond far -> cull
#endif
    if (B->TPos.Z > C_FZP)
    {                                            // only A within: -> triangle A,clipAB,clipAC
      FarClipVtx(A,B,F_VB+10);
      FarClipVtx(A,C,F_VB+11);
      UFE_NearClip(A,F_VB+10,F_VB+11);
      return;
    }
    // A,B within, only C beyond: -> quad A,B,clipBC,clipAC (two triangles)
    FarClipVtx(B,C,F_VB+10);
    FarClipVtx(A,C,F_VB+11);
    UFE_NearClip(A,B,F_VB+10);
    UFE_NearClip(A,F_VB+10,F_VB+11);
    return;
  }
#endif
  UFE_NearClip(A,B,C);
}

// 2D clipping Phase one : X=0.
void UFE1(Vertex *A,Vertex *B,Vertex *C)
{
  Vertex *T;
  if (A->PX>B->PX) {T=A; A=B; B=T;}
  if (A->PX>C->PX) {T=A; A=C; C=T;}
  if (B->PX>C->PX) {T=B; B=C; C=T;}

#ifdef FDS_CLIPCHK
  if (C->PX<0) { g_cc.nCullL++; return; }
#else
  if (C->PX<0) return;
#endif
  if (A->PX>=0) {UFE2(A,B,C); return;}

  if (B->PX<0)
  {
    CLIPI2(A,C,F_VB+2,A->PX,C->PX);
    CLIPI2(B,C,F_VB+3,B->PX,C->PX);
    F_VB[2].PX = 0.0f; F_VB[3].PX = 0.0f;    // exact: this IS the plane
    CCEDGE(resL,F_VB[2].PX,F_VB[3].PX);
    UFE2(F_VB+2,F_VB+3,C);
    return;
  }
  CLIPI2(A,B,F_VB+2,A->PX,B->PX);
  CLIPI2(A,C,F_VB+3,A->PX,C->PX);
  F_VB[2].PX = 0.0f; F_VB[3].PX = 0.0f;
  CCEDGE(resL,F_VB[2].PX,F_VB[3].PX);
  UFE2(F_VB+2,F_VB+3,B);
  UFE2(F_VB+3,B,C);
}

void UFE2(Vertex *A,Vertex *B,Vertex *C)
{
  float XClip = XRes_1;

#ifdef FDS_CLIPCHK
  if (A->PX>XClip) { g_cc.nCullR++; return; }
#else
  if (A->PX>XClip) return;
#endif
  if (C->PX<=XClip) {UFE3(A,B,C); return;}

  if (B->PX>XClip)
  {
    CLIPI2(A,B,F_VB+4,XClip-A->PX,XClip-B->PX);
    CLIPI2(A,C,F_VB+5,XClip-A->PX,XClip-C->PX);
    F_VB[4].PX = XClip; F_VB[5].PX = XClip;
    CCEDGE(resR,F_VB[4].PX-XClip,F_VB[5].PX-XClip);
    UFE3(A,F_VB+4,F_VB+5);
    return;
  }
  CLIPI2(A,C,F_VB+4,XClip-A->PX,XClip-C->PX);
  CLIPI2(B,C,F_VB+5,XClip-B->PX,XClip-C->PX);
  F_VB[4].PX = XClip; F_VB[5].PX = XClip;
  CCEDGE(resR,F_VB[4].PX-XClip,F_VB[5].PX-XClip);
  UFE3(B,F_VB+4,F_VB+5);
  UFE3(A,B,F_VB+4);
}
void UFE3(Vertex *A,Vertex *B,Vertex *C)
{
  Vertex *T;

  if (A->PY>B->PY) {T=A; A=B; B=T;}
  if (A->PY>C->PY) {T=A; A=C; C=T;}
  if (B->PY>C->PY) {T=B; B=C; C=T;}

#ifdef FDS_CLIPCHK
  if (C->PY<0) { g_cc.nCullU++; return; }
#else
  if (C->PY<0) return;
#endif
  if (A->PY>=0) {UFE4(A,B,C); return;}
  if (B->PY<0)
  {
    CLIPI2(A,C,F_VB+6,A->PY,C->PY);
    CLIPI2(B,C,F_VB+7,B->PY,C->PY);
    F_VB[6].PY = 0.0f; F_VB[7].PY = 0.0f;
    CCEDGE(resU,F_VB[6].PY,F_VB[7].PY);
    UFE4(F_VB+6,F_VB+7,C);
    return;
  }
  CLIPI2(A,B,F_VB+6,A->PY,B->PY);
  CLIPI2(A,C,F_VB+7,A->PY,C->PY);
  F_VB[6].PY = 0.0f; F_VB[7].PY = 0.0f;
  CCEDGE(resU,F_VB[6].PY,F_VB[7].PY);
  UFE4(F_VB+6,F_VB+7,B);
  UFE4(F_VB+7,B,C);
}
void UFE4(Vertex *A,Vertex *B,Vertex *C)
{
  float YClip = YRes_1;

  if (C->PY<=YClip) {RAST(A,B,C); return;}
#ifdef FDS_CLIPCHK
  if (A->PY>YClip) { g_cc.nCullD++; return; }
#else
  if (A->PY>YClip) return;
#endif
  if (B->PY>YClip)
  {
    CLIPI2(A,B,F_VB+8,YClip-A->PY,YClip-B->PY);
    CLIPI2(A,C,F_VB+9,YClip-A->PY,YClip-C->PY);
    F_VB[8].PY = YClip; F_VB[9].PY = YClip;
    CCEDGE(resD,F_VB[8].PY-YClip,F_VB[9].PY-YClip);
    RAST(A,F_VB+8,F_VB+9);
    return;
  }
  CLIPI2(A,C,F_VB+8,YClip-A->PY,YClip-C->PY);
  CLIPI2(B,C,F_VB+9,YClip-B->PY,YClip-C->PY);
  F_VB[8].PY = YClip; F_VB[9].PY = YClip;
  CCEDGE(resD,F_VB[8].PY-YClip,F_VB[9].PY-YClip);
  RAST(B,F_VB+8,F_VB+9);
  RAST(A,B,F_VB+8);
}