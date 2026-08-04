// DOSRECOVER.CPP — definitions that were present in the original 1998 build
// but are NOT in the preserved Original/dos-rev snapshot (they survived only
// compiled, in FDS.LIB/REV.LIB, and were later culled from the modern port by
// the Tier-1 cleanup, commit 3b09fcb, under _C_WATCOM / #if 0). Reconstructed
// here to complete the from-source Open Watcom link. Pattern matches the base
// definitions still in FILLERS.CPP / IMGGENR.CPP.

#include <StdIO.H>
#include <StdLib.H>
#include <String.H>
#include <Math.H>

#include "Source/FDS_Defs.H"
#include "Source/FDS_Vars.H"
#include "Source/FDS_Decs.H"

// ---- demo globals (extern-declared in REVIVAL/FDS_VARS.H; storage TU missing) ----
float Ambient_Factor;
float Diffusive_Factor;
float ImageSize;
float C_FZP;
float C_rFZP;
float C_NZP;
float C_rNZP;
float CParticle_ISize;

// ---- WobPointsHeight: WOBTR.ASM does `EXTRN _WobPointsHeight : DWORD` (=YRes/8).
// Set from YRes at grid-init time; provide storage here. ----
extern "C" DWord WobPointsHeight;
#pragma aux WobPointsHeight "_*";
DWord WobPointsHeight;

// ---- base spriter ASM (as declared in FILLERS.CPP) ----
extern "C" void Seven_UP(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux Seven_UP parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi];

// ---- filler variants (culled) ----
void The_Seven_UP_HALFSIZE(Vertex *A,Vertex *B,Vertex *C)
{
  long Size = 5.0*A->RZ;
#ifdef FDS_ZFLIP
  // was an unconditional blit into VPage -> these fountain particles drew over every face
  // regardless of depth. Same z-tested additive sprite the omni flares use.
  // g_PclNoZ (Z key) restores the original no-depth behaviour for A/B comparison.
  Sprite_ZTested(A,Size,(int)g_PclNoZ);
  return;
#endif
  Seven_UP(A->PX,A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
}

void The_Seven_UP_64(long X,long Y,long SX,long SY,DWord *Bitmap,char *Page)
{
  Seven_UP(X,Y,SX,SY,(char *)Bitmap,Page);
}

// ---- base grid mapper ASM (Grid_Texture_MapASM_ lives in WOBTR.ASM, 320-wide) ----
extern "C" void Grid_Texture_MapASM(char *Texture,GridPoint *Points,char *VirScr);
#pragma aux Grid_Texture_MapASM parm [ecx] [esi] [edi] modify exact [eax ebx ecx edx esi edi];
#ifdef FDS_MMXWOB
// WOBTRG.ASM: fused texture+gouraud, integer addressing + MMX modulate interleaved.
extern "C" void Grid_Texture_MapG_MMX(char *Texture,GridPoint *Points,char *VirScr);
#pragma aux Grid_Texture_MapG_MMX parm [ecx] [esi] [edi] modify exact [eax ebx ecx edx esi edi];
// Same routine, tiled (8-wide vertical strip) texel addressing; needs a swizzled texture.
extern "C" void Grid_Texture_MapG_MMX_T(char *Texture,GridPoint *Points,char *VirScr);
#pragma aux Grid_Texture_MapG_MMX_T parm [ecx] [esi] [edi] modify exact [eax ebx ecx edx esi edi];
#endif

#ifdef FDS_WOBCHK
// Runtime verification of the tiled plane mapper, against an EXACT C reference: row-major
// addressing (the definition) plus WOBTRG.ASM's precise MMX light semantics, so the compare
// is byte-for-byte with no rounding slack. GLAT hands us the row-major copy of the plane
// texture; the ASM gets the swizzled one.
//
// Note the shipped packed-byte routine does NOT match this reference on ~0.7% of pixels: its
// carry chain keeps U's fraction in bits 24-31 of the same register as V's 16-bit value plus
// a sign-extension lane at bits 16-23, and on runs with dv<=0 that lane's carry leaks into
// U's fraction, walking U up to a texel off. Modelled on the host: 0.60-0.73% of pixels per
// frame across 48 grids. That is a 1998 bug the tiled form simply doesn't have.
DWord *g_WobRowTex = 0;
static DWord *WobChkBuf = 0;
static long   WobChkN = 0;

static long WobPack(long b,long g,long r)   // packuswb: saturate signed words to bytes
{
  if (b<0) b=0; if (b>255) b=255;
  if (g<0) g=0; if (g>255) g=255;
  if (r<0) r=0; if (r>255) r=255;
  return b | (g<<8) | (r<<16);
}
static long WobMulH(long t,long light)      // pmulhw: signed 16x16, keep the high word
{
  return (long)(short)((((long)(short)(t*2) * (long)(short)light) >> 16) & 0xFFFF);
}
static void Grid_Texture_MapG_Ref(DWord *tex, GridPoint *P, DWord *dst, long rows)
{
  DWord *block = dst, *scan;
  GridPoint *p0=P, *p1=P+1, *p2=P+41, *p3=P+42;
  long i,j,x,y;
  for (j=0;j<rows;j++)
  {
    for (i=0;i<40;i++)
    {
      long lu=p0->U, lv=p0->V, ru=p1->U, rv=p1->V;
      long dlu=(p2->U-lu)>>3, dlv=(p2->V-lv)>>3, dru=(p3->U-ru)>>3, drv=(p3->V-rv)>>3;
      long lr=p0->R, lg=p0->G, lb=p0->B, rr=p1->R, rg=p1->G, rb=p1->B;
      long dlr=(p2->R-lr)>>3, dlg=(p2->G-lg)>>3, dlb=(p2->B-lb)>>3;
      long drr=(p3->R-rr)>>3, drg=(p3->G-rg)>>3, drb=(p3->B-rb)>>3;
      scan = block;
      for (y=0;y<8;y++)
      {
        long u=lu, v=lv, du=(ru-lu)>>3, dv=(rv-lv)>>3;
        long wb=(short)(lb>>1), wg=(short)(lg>>1), wr=(short)(lr>>1);      // mm0
        long sb=(short)((rb-lb)>>4), sg=(short)((rg-lg)>>4), sr=(short)((rr-lr)>>4);  // mm1
        for (x=0;x<8;x++)
        {
          DWord t = tex[((u&0xFF00)>>8)+(v&0xFF00)];
          scan[x] = (DWord)WobPack(WobMulH((long)(t&0xFF),wb),
                                   WobMulH((long)((t>>8)&0xFF),wg),
                                   WobMulH((long)((t>>16)&0xFF),wr));
          u+=du; v+=dv;
          wb=(short)(wb+sb); wg=(short)(wg+sg); wr=(short)(wr+sr);          // paddw wraps
        }
        scan += 320;
        lu+=dlu; lv+=dlv; ru+=dru; rv+=drv;
        lr+=dlr; lg+=dlg; lb+=dlb; rr+=drr; rg+=drg; rb+=drb;
      }
      block += 8;
      p0++; p1++; p2++; p3++;
    }
    block += 7*320;
    p0++; p1++; p2++; p3++;
  }
}
#endif

// ---- mapper variants (culled). The base ASM is already 320-wide, so _320 maps
// straight to it; _MMX falls back to the base until WOBTRMMX.ASM is recovered
// from its .LST (visually identical, just no MMX acceleration). ----
void Grid_Texture_Mapper_320(GridPoint *GP,Image *Img,DWord *DAC)
{
  WobPointsHeight = YRes >> 3;    // ScreenHeight/8 — the ASM's row-loop count
#ifdef FDS_CWOB
  Grid_Texture_MapC((DWord *)Img->Data,GP,DAC,(long)WobPointsHeight);
#else
  Grid_Texture_MapASM((char *)Img->Data,GP,(char *)DAC);
#endif
}

void Grid_Texture_Mapper_MMX(GridPoint *GP,Image *Img,DWord *DAC)
{
  WobPointsHeight = YRes >> 3;
#ifdef FDS_CWOB
  Grid_Texture_MapC((DWord *)Img->Data,GP,DAC,(long)WobPointsHeight);
#else
  Grid_Texture_MapASM((char *)Img->Data,GP,(char *)DAC);
#endif
}

// The base ASM only maps U,V — it ignores the per-vertex R,G,B (distance color the
// original applied to the wobbler plane). This second pass Gouraud-modulates the
// rendered texture by the interpolated R,G,B (bilinear over each 8x8 cell), so the
// plane fades to dark with distance the way the shipped demo did. GridPoint.R holds
// r*254 (r=1..255, brighter=closer) so R>>8 is the ~0..252 modulation weight.
void Grid_Texture_Mapper_Gouraud(GridPoint *GP,Image *Img,DWord *DAC)
{
  WobPointsHeight = YRes >> 3;
#ifdef FDS_MMXWOB
  // MMX-scheduled fused wobbler: integer pipe addresses/fetches, MMX pipe modulates.
#ifdef FDS_WOBTILE
  Grid_Texture_MapG_MMX_T((char *)Img->Data,GP,(char *)DAC);   // tiled fetch (swizzled texture)
#ifdef FDS_WOBCHK
  if (g_WobRowTex && WobChkN < 8)
  {
    long px = 320L*(long)WobPointsHeight*8L, i, bad = 0, first = -1, badPk = 0;
    if (!WobChkBuf) WobChkBuf = new DWord[2*320*256];
    memset(WobChkBuf,0,2*px*sizeof(DWord));
    Grid_Texture_MapG_Ref(g_WobRowTex,GP,WobChkBuf,(long)WobPointsHeight);   // exact C
    Grid_Texture_MapG_MMX((char *)g_WobRowTex,GP,(char *)(WobChkBuf+px));    // shipped ASM
    for (i=0;i<px;i++)
    {
      if (((DWord *)DAC)[i] != WobChkBuf[i]) { if (first<0) first = i; bad++; }
      if (WobChkBuf[px+i] != WobChkBuf[i]) badPk++;
    }
    fprintf(LogFile,"WOBCHK n=%ld px=%ld tiledVsRef=%ld packedVsRef=%ld first=%ld (%ld,%ld) "
                    "tiled=%08lx ref=%08lx\n",
            WobChkN,px,bad,badPk,first,first<0?-1:first%320,first<0?-1:first/320,
            first<0?0:(unsigned long)((DWord *)DAC)[first],
            first<0?0:(unsigned long)WobChkBuf[first]);
    fflush(LogFile);
    WobChkN++;
  }
#endif
#else
  Grid_Texture_MapG_MMX((char *)Img->Data,GP,(char *)DAC);
#endif
  return;
#endif
#ifdef FDS_CWOB
  // fused: texture + gouraud modulate per 8-pixel run while L1-hot (one pass)
  Grid_Texture_MapC_G((DWord *)Img->Data,GP,DAC,(long)WobPointsHeight);
  return;
#endif
  Grid_Texture_MapASM((char *)Img->Data,GP,(char *)DAC);   // texture first

  long cols = 40;                    // PointsWidth (ScreenWidth/8) cells per row
  long rows = (long)WobPointsHeight; // YRes/8 rows
  long stride = 41;                  // row stride = 40 cells + 1 (matches WOBTR.ASM)
  for (long ry=0; ry<rows; ry++)
    for (long cx=0; cx<cols; cx++)
    {
      GridPoint *P00=GP+ry*stride+cx, *P01=P00+1, *P10=P00+stride, *P11=P10+1;
      for (long py=0; py<8; py++)
      {
        long lR=P00->R+(P10->R-P00->R)*py/8, lG=P00->G+(P10->G-P00->G)*py/8, lB=P00->B+(P10->B-P00->B)*py/8;
        long rR=P01->R+(P11->R-P01->R)*py/8, rG=P01->G+(P11->G-P01->G)*py/8, rB=P01->B+(P11->B-P01->B)*py/8;
        long R=lR,G=lG,B=lB, dR=(rR-lR)/8,dG=(rG-lG)/8,dB=(rB-lB)/8;
        DWord *o = DAC + (ry*8+py)*(long)XRes + cx*8;
        for (long px=0; px<8; px++)
        {
          long cr=R>>8, cg=G>>8, cb=B>>8;
          if(cr<0)cr=0; else if(cr>255)cr=255;
          if(cg<0)cg=0; else if(cg>255)cg=255;
          if(cb<0)cb=0; else if(cb>255)cb=255;
          DWord t=o[px];
          o[px] = (((t&0xFF)*cb)>>8) | ((((t>>8)&0xFF)*cg)>>8<<8) | ((((t>>16)&0xFF)*cr)>>8<<16);
          R+=dR; G+=dG; B+=dB;
        }
      }
    }
}

// ---- Interactive controls, shared by every scene (call once per frame, just before Flip) ----
//   P : toggle pause. Timer is HELD at its entry value while paused, so scene time does not
//       advance (city/fountain derive CurFrame from Timer; greets steps CurFrame per rendered
//       frame, and simply not looping freezes it).
//   C : dump camera source/target + CurFrame + Timer to RUNTIME.LOG and show it on screen.
//       If a Scene is passed, ALSO dump every visible mesh's view-space Z range and how many
//       of its vertices are behind the near plane / beyond the far plane - i.e. exactly what
//       is needed to tell "this mesh was culled correctly" from "this mesh should be visible".
// Both keys are edge-triggered, so holding them does not repeat.
// A/B toggle for the particle depth test (Z key). 0 = z-tested (default, the fix),
// 1 = draw unconditionally the way the original did. Affects the PARTICLE fillers only -
// the omni flares keep their z-test either way.
long g_PclNoZ = 0;

// Particle group map, published by the scene that owns the particle system (fountain).
long g_PclGroupOff[5] = {0,0,0,0,0};
long g_PclGroupN[5]   = {0,0,0,0,0};

static char DbgInfo1[40] = "";
static char DbgInfo2[40] = "";
static char DbgInfo3[40] = "";
static long DbgInfoTicks = 0;

static void Demo_DumpState(const char *SceneName,float CurFrm,Scene *Sc)
{
  Camera *C = View;
  sprintf(DbgInfo1,"%s f%.0f T%ld",SceneName,(double)CurFrm,(long)Timer);
  if (C)
  {
    sprintf(DbgInfo2,"S %.0f %.0f %.0f",C->ISource.X,C->ISource.Y,C->ISource.Z);
    sprintf(DbgInfo3,"T %.0f %.0f %.0f",C->ITarget.X,C->ITarget.Y,C->ITarget.Z);
  }
  else { DbgInfo2[0]=0; DbgInfo3[0]=0; }
  DbgInfoTicks = 150;

  fprintf(LogFile,"CAMINFO %s CurFrame=%.2f Timer=%ld",SceneName,(double)CurFrm,(long)Timer);
  if (C)
    fprintf(LogFile," src=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f) FOV=%.2f",
      C->ISource.X,C->ISource.Y,C->ISource.Z,
      C->ITarget.X,C->ITarget.Y,C->ITarget.Z,C->IFOV);
  fprintf(LogFile,"\n");
  if (Sc)
  {
    Object *DO;
    for(DO=Sc->ObjectHead;DO;DO=DO->Next)
    {
      TriMesh *DT;
      Vertex *dv,*dve;
      float zmin,zmax;
      long nNear,nFar,orflag,andflag;
      float pxmn,pxmx,pymn,pymx,rl0,rl1,rl2;
      Vector *rw;
      if (DO->Type!=Obj_TriMesh || !DO->Data) continue;
      DT=(TriMesh *)DO->Data;
      if (!DT->VIndex) continue;
      // RotMat rows carry IScale (Animate_Objects scales them), so |row| != 1 means the
      // backface test's MatrixTXVector(RotMat,...) transpose-as-inverse is INVALID.
      rw=(Vector *)DT->RotMat;
      rl0=(float)sqrt(rw[0].X*rw[0].X+rw[0].Y*rw[0].Y+rw[0].Z*rw[0].Z);
      rl1=(float)sqrt(rw[1].X*rw[1].X+rw[1].Y*rw[1].Y+rw[1].Z*rw[1].Z);
      rl2=(float)sqrt(rw[2].X*rw[2].X+rw[2].Y*rw[2].Y+rw[2].Z*rw[2].Z);
      zmin=1e30f; zmax=-1e30f; nNear=0; nFar=0;
      pxmn=1e30f; pxmx=-1e30f; pymn=1e30f; pymx=-1e30f; orflag=0; andflag=0x3F;
      dve=DT->Verts+DT->VIndex;
      for(dv=DT->Verts;dv<dve;dv++)
      {
        if (dv->TPos.Z<zmin) zmin=dv->TPos.Z;
        if (dv->TPos.Z>zmax) zmax=dv->TPos.Z;
        if (dv->TPos.Z<C_NZP) nNear++;
        if (dv->TPos.Z>C_FZP) nFar++;
        if (dv->TPos.Z>C_NZP)
        {
          if (dv->PX<pxmn) pxmn=dv->PX;
          if (dv->PX>pxmx) pxmx=dv->PX;
          if (dv->PY<pymn) pymn=dv->PY;
          if (dv->PY>pymx) pymx=dv->PY;
        }
        orflag  |= (dv->Flags&0x3F);
        andflag &= (dv->Flags&0x3F);
      }
      fprintf(LogFile,"  CAMMESH '%s' verts=%ld Z[%.2f,%.2f] behindNear=%ld beyondFar=%ld "
                      "invisible=%d | faces=%ld trivialrej=%ld backface=%ld drawn=%ld"
                      " | PX[%.0f,%.0f] PY[%.0f,%.0f] or=%02lx and=%02lx (screen %ldx%ld)"
                      " | IScale=(%.3f,%.3f,%.3f) rowlen=(%.3f,%.3f,%.3f)\n",
        DO->Name?DO->Name:"?",(long)DT->VIndex,zmin,zmax,nNear,nFar,
        (int)((DT->Flags&Tri_Invisible)!=0),
        (long)DT->DbgFaces,(long)DT->DbgRej,(long)DT->DbgBack,(long)DT->DbgIns,
        pxmn,pxmx,pymn,pymx,orflag,andflag,(long)XRes,(long)YRes,
        DT->IScale.X,DT->IScale.Y,DT->IScale.Z,rl0,rl1,rl2);
      // If the row lengths do not match IScale, the rotation part is not orthonormal and the
      // backface test (which inverts RotMat by transposing) is invalid. Dump the matrix.
      if (rl0 < DT->IScale.X*0.99f || rl0 > DT->IScale.X*1.01f ||
          rl1 < DT->IScale.Y*0.99f || rl1 > DT->IScale.Y*1.01f)
        fprintf(LogFile,"    CAMROT '%s' flags=%lx IRot=(W%.4f,%.4f,%.4f,%.4f) |q|2=%.4f\n"
                        "      M=[%.4f %.4f %.4f][%.4f %.4f %.4f][%.4f %.4f %.4f]\n",
          DO->Name?DO->Name:"?",(unsigned long)DT->Flags,
          DT->IRot.W,DT->IRot.X,DT->IRot.Y,DT->IRot.Z,
          (double)(DT->IRot.W*DT->IRot.W+DT->IRot.X*DT->IRot.X+
                   DT->IRot.Y*DT->IRot.Y+DT->IRot.Z*DT->IRot.Z),
          DT->RotMat[0][0],DT->RotMat[0][1],DT->RotMat[0][2],
          DT->RotMat[1][0],DT->RotMat[1][1],DT->RotMat[1][2],
          DT->RotMat[2][0],DT->RotMat[2][1],DT->RotMat[2][2]);
    }
  }
  if (Sc && Sc->Pcl && Sc->NumOfParticles)
  {
    // live particle count per group, so a starving emitter is obvious. Group layout is the
    // fountain's: [outer][inner0][inner1][inner2][spiral]; g_PclGroup* is filled by the scene.
    long g;
    for (g=0; g<5 && g_PclGroupN[g]; g++)
    {
      long i,live=0;
      for (i=g_PclGroupOff[g]; i<g_PclGroupOff[g]+g_PclGroupN[g]; i++)
        if (Sc->Pcl[i].Flags) live++;
      fprintf(LogFile,"  CAMPCL group%ld [%ld..%ld) live=%ld of %ld\n",
        g,g_PclGroupOff[g],g_PclGroupOff[g]+g_PclGroupN[g],live,g_PclGroupN[g]);
      // Sample a few live particles: their depth code vs what is already in the z-buffer at
      // their pixel. "zbuf > zc" = something NEARER is there (occluded, correctly or not);
      // comparing the two codes says whether the occluder is at a plausible depth.
      { long shown=0;
        for (i=g_PclGroupOff[g]; i<g_PclGroupOff[g]+g_PclGroupN[g] && shown<5; i++)
        {
          Particle *P=Sc->Pcl+i;
          long px,py; unsigned short zc,zb; float zcf;
          if (!P->Flags) continue;
          if (P->V.RZ<=0.0f) continue;
          px=(long)P->V.PX; py=(long)P->V.PY;
          if (px<0||px>=XRes||py<0||py>=YRes) continue;
          zcf = 65408.0f - ZLinScale*(1.0f/P->V.RZ);
          if (zcf<0.0f) zcf=0.0f;
          zc = (zcf>65408.0f) ? (unsigned short)65408 : (unsigned short)zcf;
          zb = ZBuf[py*(long)XRes+px];
          fprintf(LogFile,"    CAMPCLZ g%ld i%ld Z=%.1f P(%ld,%ld) zc=%u zbuf=%u %s\n",
            g,i,(double)(1.0f/P->V.RZ),px,py,(unsigned)zc,(unsigned)zb,
            (zc<zb)?"OCCLUDED":"drawn");
          shown++;
        }
      }
    }
  }
  fflush(LogFile);
}

void Demo_Controls(const char *SceneName,float CurFrm,Scene *Sc)
{
  static int pPrev=0,cPrev=0;
  int pNow,cNow;
#ifdef FDS_CLIPCHK
  // headless capture: same dump the C key produces, on a fixed cadence
  { static long _callN=0; if ((_callN++ % 100)==60) Demo_DumpState(SceneName,CurFrm,Sc); }
#endif

  cNow = (Keyboard[ScC]!=0);
  if (cNow && !cPrev) Demo_DumpState(SceneName,CurFrm,Sc);
  cPrev = cNow;

  // Z: flip the particle depth test live, so both looks are comparable in one run.
  { static int zPrev=0; int zNow=(Keyboard[ScZ]!=0);
    if (zNow && !zPrev)
    {
      g_PclNoZ = !g_PclNoZ;
      fprintf(LogFile,"PCLZTEST %s (Z key)\n",g_PclNoZ?"OFF - particles ignore depth":"ON");
      fflush(LogFile);
      DbgInfoTicks = 150;
    }
    zPrev=zNow; }
  if (g_PclNoZ) OutTextXY(VPage,4,YRes-10,"pcl Z-TEST OFF (Z)",120);

  pNow = (Keyboard[ScP]!=0);
  if (pNow && !pPrev)
  {
    long held = Timer;
    int released = 0;
    Demo_DumpState(SceneName,CurFrm,Sc);
    while (!Keyboard[ScESC])
    {
      Timer = held;                       // hold scene time while paused
      OutTextXY(VPage,4,14,"PAUSED P=resume C=info Z=pclz",120);
      if (DbgInfo1[0]) OutTextXY(VPage,4,24,DbgInfo1,120);
      if (DbgInfo2[0]) OutTextXY(VPage,4,34,DbgInfo2,120);
      if (DbgInfo3[0]) OutTextXY(VPage,4,44,DbgInfo3,120);
      Flip(Screen);
      cNow = (Keyboard[ScC]!=0);
      if (cNow && !cPrev) Demo_DumpState(SceneName,CurFrm,Sc);
      cPrev = cNow;
      if (!Keyboard[ScP]) released = 1;
      else if (released) break;           // released then pressed again -> resume
    }
    while (Keyboard[ScP]) Timer = held;   // swallow the release so we do not re-pause
    Timer = held;
    pNow = 0;
  }
  pPrev = pNow;

  // keep the readout on screen for a moment after a C press
  if (DbgInfoTicks>0)
  {
    DbgInfoTicks--;
    if (DbgInfo1[0]) OutTextXY(VPage,4,14,DbgInfo1,120);
    if (DbgInfo2[0]) OutTextXY(VPage,4,24,DbgInfo2,120);
    if (DbgInfo3[0]) OutTextXY(VPage,4,34,DbgInfo3,120);
  }
}

// Simple on-screen FPS counter: draw the last ~1s frame rate onto VPage.
// Call once per frame after Render(), before Flip(). (Timer is centiseconds.)
void Show_FPS(void)
{
  // Measure over a ~1s window using the ACTUAL timer rate (Timer_Freq), not a hard-coded 100.
  // Report one decimal so slow scenes (6-7 fps) don't quantise to integers. fps = frames*Freq/dt.
  static long fcount = 0, lastT = 0, fps10 = 0;
  char buf[24];
  long dt = Timer - lastT;
  // Scene transitions reset Timer to 1, leaving lastT far in the "future": dt goes
  // negative and the window never closes, freezing the display at the previous scene's
  // last value for most of the new scene. Resync on any backwards jump.
  if (dt < 0) { lastT = Timer; fcount = 0; dt = 0; }
  fcount++;
  if (dt >= Timer_Freq) { fps10 = (fcount*Timer_Freq*10)/dt; fcount = 0; lastT = Timer; }
  sprintf(buf,"%ld.%ld FPS", fps10/10, fps10%10);
  OutTextXY(VPage,4,4,buf,120);
}
