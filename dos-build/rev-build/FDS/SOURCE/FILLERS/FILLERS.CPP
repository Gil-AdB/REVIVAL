#include <String.H>
#include <StdIO.H>
#include <StdLib.H>
#include <ConIO.H>
#include <Math.H>


#include "../FDS_Vars.H"
#include "../FDS_Decs.H"
#include "../FDS_Defs.H"

// fused-mapper triangle census (TEXTGOUR increments; CITY prints + clears per frame)

#if defined(FDS_SUBTEX) && !defined(FDS_FUSED_GOUR)
#error "FDS_SUBTEX requires FDS_FUSED_GOUR: without it The_BiGMMXTrue/The_MMX_Gouraud take their two-pass branch, which hands ONE vertex struct to both a 16.16 mapper and an integer one."
#endif
extern "C" { long g_TriIn = 0; long g_TriDraw = 0; }

// ---------------------------------------------------------------------------------------
// Screen X/Y reach the ASM mappers as INTEGERS (Bi_Triangle/Bi_GTriangle hold `int X,Y`), so
// every vertex snaps to the pixel grid. C's float->int conversion TRUNCATES, which drags each
// vertex a mean half-pixel toward the origin — and since the texture is then correct for the
// SNAPPED triangle rather than the real one, the whole surface shifts with it. Measured with
// MAPTEST A5 (analytic per-pixel expectation over an affine face at fractional positions):
// a systematic +2.36 texel U / +2.70 texel V bias at ~4 texels/pixel, i.e. ~0.6 px. Rounding
// to nearest removes the BIAS; the residual +-0.5px snap is unbiased noise and needs the
// structs widened to 16.16 to fix properly. The +32768/-32768 pair makes the truncation round
// half-up for negative coordinates too, instead of toward zero.
#ifdef FDS_SUBTEX
// Full sub-pixel path: hand the mapper 16.16 fixed-point X/Y so the vertex is never snapped
// at all. The fillers that consume it (BITRUE/BITTRUE/TEXTGOUR) reinterpret the same 32-bit
// fields — no struct or stride change — and prestep every interpolant from the vertex to the
// first scanline and from the exact edge to the first pixel.
// Only BITRUE / BITTRUE / TEXTGOUR were taught to read 16.16 vertices. Every OTHER filler
// still reads X,Y as integer pixels, and handing one a 16.16 Y makes it treat ~12,000,000 as
// a scanline index -> instant wild write. So the format is chosen PER FILLER: SCRFIX for the
// three converted mappers (P_/PT_/PG_Texture_32 and their Lin twins), SCRINT for the rest.
// MEASURED (FDS_XPARVIZ, city water): coordinates reaching the fillers are small (max |PX|
// ~660), so 16.16's +-32767 range is not a constraint and no clamp is warranted.
#define SCRFIX(f)  ((int)((float)(f)*65536.0f))
#define SCRINT(f)  ((int)((float)(f)+32768.5f)-32768)
#define SCRRND(f)  SCRINT(f)
#else
#define SCRFIX(f)  ((int)((float)(f)+32768.5f)-32768)
#define SCRINT(f)  ((int)((float)(f)+32768.5f)-32768)
#define SCRRND(f)  SCRINT(f)
#endif
// ---------------------------------------------------------------------------------------



void Draw_Line_8Bit(Vertex *V1,Vertex *V2)
{
  char C=64;
  long D;
  long Dx,Dy;
  long Ai,Bi,Xi,Yi;
  long Offs;
  long X1,Y1,X2,Y2;
  char *Ptr = VPage;
  char *End;

  X1=V1->PX+0.5; Y1=V1->PY+0.5;
  X2=V2->PX+0.5; Y2=V2->PY+0.5;

  if (X1<=X2)
  {
    Xi = 1;
    Dx = X2-X1;
  } else {
    Xi = -1;
    Dx = X1-X2;
  }

  if (Y1<=Y2) {
    Yi = XRes;
    Dy = Y2-Y1;
  } else {
    Yi = -XRes;
    Dy = Y1-Y2;
  }
  Ptr=VPage+X1+YOffs[Y1];
  End=VPage+X2+YOffs[Y2];
  *Ptr = C;

  if (Dx > Dy) {
    Ai = Dy-Dx<<1;
    Bi = Dy<<1;
    D = Bi-Dx;
    while (Ptr != End) {
      if (D >= 0) {
        Ptr += Yi;
        D += Ai;
      } else D = D + Bi;
      Ptr += Xi;
      *Ptr = C;
    }
  } else {
    Ai = Dx-Dy<<1;
    Bi = Dx<<1;
    D = Bi-Dy;
    while (Ptr != End) {
      if (D >= 0) {
        Ptr += Xi;
        D += Ai;
      } else D += Bi;
      Ptr += Yi;
      *Ptr = C;
    }
  }
}

void Draw_Line_32Bit(Vertex *V1,Vertex *V2)
{
  DWord C=(16<<16)+(16<<8)+16;
  long D;
  long Dx,Dy;
  long Ai,Bi,Xi,Yi;
  long Offs;
  long X1,Y1,X2,Y2;
  DWord *Ptr = (DWord *)VPage;
  DWord *End;

  X1=V1->PX+0.5; Y1=V1->PY+0.5;
  X2=V2->PX+0.5; Y2=V2->PY+0.5;

  if (X1<=X2)
  {
    Xi = 1;
    Dx = X2-X1;
  } else {
    Xi = -1;
    Dx = X1-X2;
  }

  if (Y1<=Y2) {
    Yi = XRes;
    Dy = Y2-Y1;
  } else {
    Yi = -XRes;
    Dy = Y1-Y2;
  }
  End=Ptr+X2+YOffs[Y2];
  Ptr+=X1+YOffs[Y1];

  *Ptr = C;

  if (Dx > Dy) {
    Ai = Dy-Dx<<1;
    Bi = Dy<<1;
    D = Bi-Dx;
    while (Ptr != End) {
      if (D >= 0) {
        Ptr += Yi;
        D += Ai;
      } else D = D + Bi;
      Ptr += Xi;
      *Ptr = C;
    }
  } else {
    Ai = Dx-Dy<<1;
    Bi = Dx<<1;
    D = Bi-Dy;
    while (Ptr != End) {
      if (D >= 0) {
        Ptr += Xi;
        D += Ai;
      } else D += Bi;
      Ptr += Yi;
      *Ptr = C;
    }
  }
}

void Draw_Line_32Bit_ForKids(Vertex *V1,Vertex *V2)
{
  DWord C=0xFFFFFF;
  long D;
  long Dx,Dy;
  long Ai,Bi,Xi,Yi;
  long Offs;
  long X1,Y1,X2,Y2;
  DWord *Ptr = (DWord *)VPage;
  DWord *End;

  X1=V1->PX+0.5; Y1=V1->PY+0.5;
  X2=V2->PX+0.5; Y2=V2->PY+0.5;

  if (X1<=X2)
  {
    Xi = 1;
    Dx = X2-X1;
  } else {
    Xi = -1;
    Dx = X1-X2;
  }

  if (Y1<=Y2) {
    Yi = XRes;
    Dy = Y2-Y1;
  } else {
    Yi = -XRes;
    Dy = Y1-Y2;
  }
  End=Ptr+X2+YOffs[Y2];
  Ptr+=X1+YOffs[Y1];

  *Ptr = C;

  if (Dx > Dy) {
    Ai = Dy-Dx<<1;
    Bi = Dy<<1;
    D = Bi-Dx;
    while (Ptr != End) {
      if (D >= 0) {
        Ptr += Yi;
        D += Ai;
      } else D = D + Bi;
      Ptr += Xi;
      *Ptr = C;
    }
  } else {
    Ai = Dx-Dy<<1;
    Bi = Dx<<1;
    D = Bi-Dy;
    while (Ptr != End) {
      if (D >= 0) {
        Ptr += Xi;
        D += Ai;
      } else D += Bi;
      Ptr += Yi;
      *Ptr = C;
    }
  }
}



// FIXED point Flat poly filler;
// YSorted parameters assumed, Optimized as much as possible by my puny
// Rasterizer coding skills.
// Also used slightly Improved Space optimizations.
// about 100 lines of code.
// not very slow,actually quite ok.
void Flat_Poly_8Bit(Vertex *A,Vertex *B,Vertex *C)
{
  long X1,Y1,X2,Y2,X3,Y3; //Pixelled projected vertex position
  long dX12,dX13,dX23;    //Fixed point Slopes. (16:16)
  long dx12,dx13,dx23;    //X-deltas.
  long dy12,dy13,dy23;    //Y-deltas.
  long X,Y;               //Loop and generic variables.
  long x1,x2;
  long EX1,EX2;           //Edge interpolation rate.
  long dX1,dX2;           //Edge interpolation differencial.
  char *Pen;              //Viewport Scanning Pointer
  char Col=(DoFace->A->LR+DoFace->B->LR+DoFace->C->LR)*0.32; //Average Color.
  char EndAtMiddle=0;     //Flag: End drawing in the middle(ignore Y2->Y3).
  long Horiz_Span = XRes<<16; //Horizon Span. Used to Calculate Jmp.

  X1 = A->PX+0.5; Y1 = A->PY+0.5;
  X2 = B->PX+0.5; Y2 = B->PY+0.5;
  X3 = C->PX+0.5; Y3 = C->PY+0.5;

  dx12 = X2-X1; dy12=Y2-Y1;
  dx13 = X3-X1; dy13=Y3-Y1;
  dx23 = X3-X2; dy23=Y3-Y2;

  if (Y1==Y3)
  {
    if (X1<=X2)
    {
      Pen = VPage+YOffs[Y1]+X1;
      memset(Pen,Col,dx12+1);
      if (X3<X1) memset(Pen+dx13,Col,1-dx13); else if (X2<X3) memset(Pen+dx12,Col,dx23+1);
    } else {
      Pen = VPage+YOffs[Y1]+X2;
      memset(Pen,Col,1-dx12);
      Pen-=dx12;
      if (X3<X2) memset(Pen+dx13,Col,1-dx13); else if (X1<X3) memset(Pen-dx12,Col,dx13+1);
    }
    goto END_POLY;
  }
  dX13 = (dx13<<16)/dy13;
  if (Y1==Y2)
  {
    if (X1<=X2)
    {
      Pen = VPage+YOffs[Y1]+X1;
      X = dx12+1;
      memset(Pen,Col,X);

      EX1 = X1<<16;
      EX2 = X2<<16;
      Pen+=XRes-X1;
      goto MIDDLE_POLY;
    } else {
      Pen = VPage+YOffs[Y1]+X2;
      X = 1-dx12;
      memset(Pen,Col,X);

      EX1 = X2<<16;
      EX2 = X1<<16;
      Pen+=XRes-X2;
      goto MIDDLE_POLY;
    }
  }
  dX12 = (dx12<<16)/dy12;
  if (Y2==Y3) EndAtMiddle=1;
  // Scan conv Poly BEGINS.
  // Initialization...
  EX1 = EX2 = X1<<16;
  EX2 += 65536;

  if (dX12<=dX13)
  {
    dX1 = dX12;
    dX2 = dX13;
  } else {
    dX1 = dX13;
    dX2 = dX12;
  }

  Y = dy12;
  Pen = VPage+YOffs[Y1];
  EX1-=dX1;
  EX2-=dX2;
  do
  {
    EX1+=dX1;
    EX2+=dX2;
    x1=EX1>>16;
    x2=EX2>>16;
/*    if (x2==XRes_1)
    {
      Pen=NULL;
      return;
    }*/
    memset(Pen+x1,Col,x2-x1+1);
    Pen+=XRes;
  } while (Y--);
  MIDDLE_POLY: if (EndAtMiddle) goto END_POLY;
  dX23 = (dx23<<16)/dy23;
  if (dX13<=dX23)
  {
    dX1 = dX23;
    dX2 = dX13;
  } else {
    dX1 = dX13;
    dX2 = dX23;
  }
  Y = dy23-1;
  do
  {
    EX1+=dX1;
    EX2+=dX2;
    x1 = EX1>>16;
    x2 = EX2>>16;
/*    if (x2==XRes_1)
    {
      Pen=NULL;
      return;
    }*/
    memset(Pen+x1,Col,x2-x1+1);
    Pen+=XRes;
  } while (Y--);
  END_POLY:;
}

#ifdef FDS_ZFLIP
// forward declaration: the fused mapper's extern block sits further down this file
extern "C" void PG_Texture_32(Bi_GTriangle * BT, char * Bitmap,unsigned int u);
#pragma aux PG_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
#endif

void Flat_Poly_ManyBits(Vertex *A,Vertex *B,Vertex *C)
{

#ifdef FDS_ZFLIP
  // Depthless 2D fill - route through the fused mapper (gray tile + per-vertex light,
  // which reproduces the flat color: flat-lit faces carry ~equal LR at the vertices)
  // so these faces z-test/write like everything else after the front-to-back flip.
  if (BPP==32)
  {
    Bi_GTriangle GT[3];
    GT[0].X=SCRFIX(A->PX); GT[0].Y=SCRFIX(A->PY); GT[0].Z=A->RZ; GT[0].U=0; GT[0].V=0;
    GT[0].R=A->LR; GT[0].G=A->LR; GT[0].B=A->LR;
    GT[1].X=SCRFIX(B->PX); GT[1].Y=SCRFIX(B->PY); GT[1].Z=B->RZ; GT[1].U=0; GT[1].V=0;
    GT[1].R=B->LR; GT[1].G=B->LR; GT[1].B=B->LR;
    GT[2].X=SCRFIX(C->PX); GT[2].Y=SCRFIX(C->PY); GT[2].Z=C->RZ; GT[2].U=0; GT[2].V=0;
    GT[2].R=C->LR; GT[2].G=C->LR; GT[2].B=C->LR;
#ifdef FDS_PATHVIZ
    PG_Texture_32(GT,(char *)VizTileB,(unsigned long)VPage);
#else
    PG_Texture_32(GT,(char *)GrayTile,(unsigned long)VPage);
#endif
    return;
  }
#endif
  long X1,Y1,X2,Y2,X3,Y3; //Pixelled projected vertex position
  long dX12,dX13,dX23;    //Fixed point Slopes. (16:16)
  long dx12,dx13,dx23;    //X-deltas.
  long dy12,dy13,dy23;    //Y-deltas.
  long X,Y;               //Loop and generic variables.
  long x1,x2;
  long EX1,EX2;           //Edge interpolation rate.
  long dX1,dX2;           //Edge interpolation differencial.
  char *Pen;              //Viewport Scanning Pointer
  char Col=//0xFF;
  (DoFace->A->LR+DoFace->B->LR+DoFace->C->LR)*0.32; //Average Color.
  char EndAtMiddle=0;     //Flag: End drawing in the middle(ignore Y2->Y3).
  long Horiz_Span = XRes<<16; //Horizon Span. Used to Calculate Jmp.
  char CPP = BPP+1>>3;

  X1 = A->PX+0.5; Y1 = A->PY+0.5;
  X2 = B->PX+0.5; Y2 = B->PY+0.5;
  X3 = C->PX+0.5; Y3 = C->PY+0.5;

  dx12 = X2-X1; dy12=Y2-Y1;
  dx13 = X3-X1; dy13=Y3-Y1;
  dx23 = X3-X2; dy23=Y3-Y2;

  if (Y1==Y3)
  {
    if (X1<=X2)
    {
      Pen = VPage+(YOffs[Y1]+X1)*CPP;
      memset(Pen,Col,CPP*(dx12+1));
      if (X3<X1) memset(Pen+dx13*CPP,Col,CPP*(1-dx13)); else if (X2<X3) memset(Pen+dx12*CPP,Col,CPP*(dx23+1));
    } else {
      Pen = VPage+(YOffs[Y1]+X2)*CPP;
      memset(Pen,Col,CPP*(1-dx12));
      Pen-=dx12;
      if (X3<X2) memset(Pen+dx13*CPP,Col,CPP*(1-dx13)); else if (X1<X3) memset(Pen-dx12*CPP,Col,CPP*(dx13+1));
    }
    goto END_POLY;
  }
  dX13 = (dx13<<16)/dy13;
  if (Y1==Y2)
  {
    if (X1<=X2)
    {
      Pen = VPage+(YOffs[Y1]+X1)*CPP;
      X = dx12+1;
      memset(Pen,Col,CPP*X);

      EX1 = X1<<16;
      EX2 = X2<<16;
      Pen+=(XRes-X1)*CPP;
      goto MIDDLE_POLY;
    } else {
      Pen = VPage+(YOffs[Y1]+X2)*CPP;
      X = 1-dx12;
      memset(Pen,Col,CPP*X);

      EX1 = X2<<16;
      EX2 = X1<<16;
      Pen+=(XRes-X2)*CPP;
      goto MIDDLE_POLY;
    }
  }
  dX12 = (dx12<<16)/dy12;
  if (Y2==Y3) EndAtMiddle=1;
  // Scan conv Poly BEGINS.
  // Initialization...
  EX1 = EX2 = X1<<16;
  EX2 += 65536;

  if (dX12<=dX13)
  {
    dX1 = dX12;
    dX2 = dX13;
  } else {
    dX1 = dX13;
    dX2 = dX12;
  }

  Y = dy12;
  Pen = VPage+YOffs[Y1]*CPP;
  EX1-=dX1;
  EX2-=dX2;
  do
  {
    EX1+=dX1;
    EX2+=dX2;
    x1=EX1>>16;
    x2=EX2>>16;
/*    if (x2==XRes_1)
    {
      Pen=NULL;
      return;
    }*/
    memset(Pen+CPP*x1,Col,CPP*(x2-x1+1));
    Pen+=CPP*XRes;
  } while (Y--);
  MIDDLE_POLY: if (EndAtMiddle) goto END_POLY;
  dX23 = (dx23<<16)/dy23;
  if (dX13<=dX23)
  {
    dX1 = dX23;
    dX2 = dX13;
  } else {
    dX1 = dX13;
    dX2 = dX23;
  }
  Y = dy23-1;
  do
  {
    EX1+=dX1;
    EX2+=dX2;
    x1 = EX1>>16;
    x2 = EX2>>16;
/*    if (x2==XRes_1)
    {
      Pen=NULL;
      return;
    }*/
    memset(Pen+CPP*x1,Col,CPP*(x2-x1+1));
    Pen+=CPP*XRes;
  } while (Y--);
  END_POLY:;
}


void FlatV2_Poly_8Bit(Vertex *A,Vertex *B,Vertex *C)
{
  char Col=(DoFace->A->LR+DoFace->B->LR+DoFace->C->LR)*0.32;
  long Ex1,Ex2,x1,x2;
  long X1,X2,X3,Y1,Y2,Y3;
  long T;
  long dX12,dX13,dX23;
  long X,Y;

  char *Ptr,*Line,*PtrEnd;

  X1 = A->PX+0.5; X2 = B->PX+0.5; X3 = C->PX+0.5;
  Y1 = A->PY+0.5; Y2 = B->PY+0.5; Y3 = C->PY+0.5;

  if (Y1<Y3) {
    dX13=(X3-X1<<16)/(Y3-Y1);

    if (Y1<Y2) {
      dX12=(X2-X1<<16)/(Y2-Y1);
      Ex1=Ex2=X1<<16;
    } else {
      if (X1<=X2)
      {
        X=X2-X1+1;
        Ptr=VPage+X1+YOffs[Y1];
        while (X--)
          *Ptr++=Col;
      } else {
        X=X1-X2+1;
        Ptr=VPage+X2+YOffs[Y1];
        while (X--)
          *Ptr++=Col;
      }
      dX23=(X3-X2<<16)/(Y3-Y2);
      goto middle;
    }
    if (Y2<Y3) dX23=(X3-X2<<16)/(Y3-Y2);
  } else {
    if (X2<X1) {T=X1; X1=X2; X2=T;}
    if (X3<X1) {T=X1; X1=X3; X3=T;}
    if (X3<X2) {T=X2; X2=X3; X3=T;}
    X=X3-X1+1;
    Ptr=VPage+X1+YOffs[Y1];
    while (X--)
      *Ptr++=Col;
    goto end;
  }

  //All shit cases handled..begin your chatter here (i.e. do interpolation)
  Y=Y2-Y1+1;
  Line = VPage+YOffs[Y1];

  if (dX12<=dX13)
  {
    while (Y--)
    {
      x1=Ex1>>16;
      x2=Ex2>>16;
      Ptr=Line+x1;
      PtrEnd=Line+x2;

      do
      {
        *Ptr=Col;
      } while (++Ptr<=PtrEnd);
      Line += XRes;
      Ex1+=dX12;
      Ex2+=dX13;
    }
  } else {
    while (Y--)
    {
      x1=Ex1>>16;
      x2=Ex2>>16;
      Ptr=Line+x2;
      PtrEnd=Line+x1;

      do
      {
        *Ptr=Col;
      } while (++Ptr<=PtrEnd);

      Line += XRes;
      Ex1+=dX12;
      Ex2+=dX13;
    }
  }
  middle: Y=Y3-Y2;  //After Y1->Y2
  if (!Y) goto end;
  Line = VPage+YOffs[Y2+1];
  Ex1 = (X2<<16)+dX23;
  Ex2 = (X1<<16)+dX13*(Y2-Y1+1);
  if (Ex1<Ex2)
  {
    while (Y--)
    {
      x1=Ex1>>16;
      x2=Ex2>>16;
      Ptr=Line+x1;
      PtrEnd=Line+x2;

      do
      {
        *Ptr=Col;
      } while (++Ptr<=PtrEnd);
      Line += XRes;
      Ex1+=dX23;
      Ex2+=dX13;
    }
  } else {
    while (Y--)
    {
      x1=Ex1>>16;
      x2=Ex2>>16;
      Ptr=Line+x2;
      PtrEnd=Line+x1;

      do
      {
        *Ptr=Col;
      } while (++Ptr<=PtrEnd);

      Line += XRes;
      Ex1+=dX23;
      Ex2+=dX13;
    }
  }
  end:; //After Y2->Y3
}

// Calls THE Bilinear mapper

extern "C" void RGBGouraud(Bi_Triangle * BT,unsigned int u);
#pragma aux RGBGouraud parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void RGBGouraudMMX(Bi_Triangle * BT,unsigned int u);
#pragma aux RGBGouraudMMX parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void RGBTGouraud(Bi_Triangle *BT,unsigned int u);
#pragma aux RGBTGouraud parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void RGBTGouraudMMX(Bi_Triangle * BT,unsigned int u);
#pragma aux RGBTGouraudMMX parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void P_Texture_8(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux P_Texture_8 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void P_Texture_16(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux P_Texture_16 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void P_Texture_32(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux P_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

#ifdef FDS_TEX_SWIZZLE
// P_Texture_32/PT_Texture_32 fetch 8-wide-strip tiled in this build (Data swizzled in
// place at scene-init end; TiledData == Data is the marker). Runtime-generated textures
// (Mat_DynTxtr, e.g. the fountain vortex portal) legitimately stay ROW-MAJOR and are
// dispatched to the *Lin twins (same mapper sources assembled without the swizzle define).
extern "C" void P_TextureLin_32(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux P_TextureLin_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
extern "C" void PT_TextureLin_32(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux PT_TextureLin_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
#define TEX_IS_TILED(T) ((T)->TiledData==(T)->Data)
#endif

extern "C" void PG_Texture_32(Bi_GTriangle * BT, char * Bitmap,unsigned int u);
#pragma aux PG_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void PT_Texture_32(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux PT_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern void Adv_PTexture_8(Bi_Floating * vtx, char * bitmap,unsigned int u);


void The_Specular_Gouraud(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=SCRINT(A->PX);
  BT[0].Y=SCRINT(A->PY);
  BT[0].Z=A->LR;
  BT[0].U=A->LG;
  BT[0].V=A->LB;

  BT[1].X=SCRINT(B->PX);
  BT[1].Y=SCRINT(B->PY);
  BT[1].Z=B->LR;
  BT[1].U=B->LG;
  BT[1].V=B->LB;

  BT[2].X=SCRINT(C->PX);
  BT[2].Y=SCRINT(C->PY);
  BT[2].Z=C->LR;
  BT[2].U=C->LG;
  BT[2].V=C->LB;
  RGBGouraud(BT,(unsigned long)VPage);
}

void The_MMX_Gouraud(Vertex *A,Vertex *B,Vertex *C)
{

#ifdef FDS_ZFLIP
  // GOURMMX carries light in its Z/U/V slots - it has no depth to test. Route through the
  // fused mapper with the constant gray tile instead: t=128 -> modulate returns the light
  // exactly, and the face fully participates in the z-buffer.
#ifdef FDS_PATHVIZ
  { Bi_Triangle VT[3];
    VT[0].X=SCRFIX(A->PX); VT[0].Y=SCRFIX(A->PY); VT[0].Z=A->RZ; VT[0].U=0; VT[0].V=0;
    VT[1].X=SCRFIX(B->PX); VT[1].Y=SCRFIX(B->PY); VT[1].Z=B->RZ; VT[1].U=0; VT[1].V=0;
    VT[2].X=SCRFIX(C->PX); VT[2].Y=SCRFIX(C->PY); VT[2].Z=C->RZ; VT[2].U=0; VT[2].V=0;
    P_Texture_32(VT,(char *)VizTileA,(DWord)VPage);
    return; }
#endif
  Bi_GTriangle GT[3];
  GT[0].X=SCRFIX(A->PX); GT[0].Y=SCRFIX(A->PY); GT[0].Z=A->RZ; GT[0].U=0; GT[0].V=0;
  GT[0].R=A->LR; GT[0].G=A->LG; GT[0].B=A->LB;
  GT[1].X=SCRFIX(B->PX); GT[1].Y=SCRFIX(B->PY); GT[1].Z=B->RZ; GT[1].U=0; GT[1].V=0;
  GT[1].R=B->LR; GT[1].G=B->LG; GT[1].B=B->LB;
  GT[2].X=SCRFIX(C->PX); GT[2].Y=SCRFIX(C->PY); GT[2].Z=C->RZ; GT[2].U=0; GT[2].V=0;
  GT[2].R=C->LR; GT[2].G=C->LG; GT[2].B=C->LB;
  PG_Texture_32(GT,(char *)GrayTile,(unsigned long)VPage);
  return;
#endif
  Bi_Triangle BT[3];
  BT[0].X=SCRINT(A->PX);
  BT[0].Y=SCRINT(A->PY);
  BT[0].Z=A->LR;
  BT[0].U=A->LG;
  BT[0].V=A->LB;

  BT[1].X=SCRINT(B->PX);
  BT[1].Y=SCRINT(B->PY);
  BT[1].Z=B->LR;
  BT[1].U=B->LG;
  BT[1].V=B->LB;

  BT[2].X=SCRINT(C->PX);
  BT[2].Y=SCRINT(C->PY);
  BT[2].Z=C->LR;
  BT[2].U=C->LG;
  BT[2].V=C->LB;
  RGBGouraudMMX(BT,(unsigned long)VPage);   // was RGBGouraud (FPU) — the MMX path was written
                                            // (GOURMMX.ASM) but never called. This is the MMX
                                            // gouraud filler for opaque-lit untextured faces.
}


void The_Specular_TGouraud(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=SCRFIX(A->PX);
  BT[0].Y=SCRFIX(A->PY);
  BT[0].Z=A->LB;
  BT[0].U=A->LG;
  BT[0].V=A->LR;

  BT[1].X=SCRFIX(B->PX);
  BT[1].Y=SCRFIX(B->PY);
  BT[1].Z=B->LB;
  BT[1].U=B->LG;
  BT[1].V=B->LR;

  BT[2].X=SCRFIX(C->PX);
  BT[2].Y=SCRFIX(C->PY);
  BT[2].Z=C->LB;
  BT[2].U=C->LG;
  BT[2].V=C->LR;
  RGBTGouraudMMX(BT,(unsigned long)VPage);
}

void The_Bilinear(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=SCRINT(A->PX);
  BT[0].Y=SCRINT(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=SCRINT(B->PX);
  BT[1].Y=SCRINT(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=SCRINT(C->PX);
  BT[2].Y=SCRINT(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  P_Texture_8(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
//  Adv_PTexture_8(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
}

void The_BiHi(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=SCRINT(A->PX);
  BT[0].Y=SCRINT(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=SCRINT(B->PX);
  BT[1].Y=SCRINT(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=SCRINT(C->PX);
  BT[2].Y=SCRINT(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  P_Texture_16(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
}

void The_BiTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=SCRFIX(A->PX);
  BT[0].Y=SCRFIX(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=SCRFIX(B->PX);
  BT[1].Y=SCRFIX(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=SCRFIX(C->PX);
  BT[2].Y=SCRFIX(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

#ifdef FDS_PATHVIZ
  P_Texture_32(BT,(char *)VizTileC,(unsigned long)VPage);   // solid red
#elif defined(FDS_TEX_SWIZZLE)
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) P_Texture_32(BT,_t->Data,(unsigned long)VPage);
    else P_TextureLin_32(BT,_t->Data,(unsigned long)VPage); }
#else
  P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif
}

void The_BiTTrue(Vertex *A,Vertex *B,Vertex *C)
{

  Bi_Triangle BT[3];
  BT[0].X=SCRFIX(A->PX);
  BT[0].Y=SCRFIX(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=SCRFIX(B->PX);
  BT[1].Y=SCRFIX(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=SCRFIX(C->PX);
  BT[2].Y=SCRFIX(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

#ifdef FDS_PATHVIZ
  PT_Texture_32(BT,(char *)VizTileD,(unsigned long)VPage);  // solid green (50/50 blend)
#elif defined(FDS_TEX_SWIZZLE)
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) PT_Texture_32(BT,_t->Data,(unsigned long)VPage);
    else PT_TextureLin_32(BT,_t->Data,(unsigned long)VPage); }
#else
  PT_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif
}

// Transparent textured Gouraud = 2-pass (there is no single combined filler):
//   pass 1: transparent texture (PT_Texture_32),
//   pass 2: transparent Gouraud light (RGBTGouraudMMX) blended over it.
void The_BiGTTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=SCRFIX(A->PX); BT[0].Y=SCRFIX(A->PY);
  BT[1].X=SCRFIX(B->PX); BT[1].Y=SCRFIX(B->PY);
  BT[2].X=SCRFIX(C->PX); BT[2].Y=SCRFIX(C->PY);

  // Both passes rasterize the SAME triangle and pass 2 MODULATES pass 1's output, so they
  // have to agree on coverage down to the pixel: a column the texture pass paints and the
  // light pass misses keeps its raw texel and reads as a full-brightness seam. GOURMMXT is
  // therefore FDS_SUBTEX-converted alongside BITTRUE and both take the same 16.16 struct.
  // (That mismatch was the bright diagonal across the city water.)
  BT[0].Z=A->RZ; BT[0].U=A->RU; BT[0].V=A->RV;
  BT[1].Z=B->RZ; BT[1].U=B->RU; BT[1].V=B->RV;
  BT[2].Z=C->RZ; BT[2].U=C->RU; BT[2].V=C->RV;
#ifdef FDS_TEX_SWIZZLE
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) PT_Texture_32(BT,_t->Data,(unsigned long)VPage);
    else PT_TextureLin_32(BT,_t->Data,(unsigned long)VPage); }
#else
  PT_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif

  // pass 2 : transparent Gouraud light over it (lighting in Z/U/V = LB/LG/LR),
  // fogged per-pixel by the (valid, clipped) vertex Z = 1/RZ: 1 - Z/FZP, so the
  // far plane goes to 0 and the multiply blacks out the far texture aliasing.
  // (Was hard-coded 2500 = city's old far plane; follow the scene so raising FZP
  // moves the fade with it instead of saturating early.)
  float fa = (A->RZ>0.0f) ? 1.0f-1.0f/(A->RZ*C_FZP) : 0.0f; if(fa<0)fa=0; if(fa>1)fa=1;
  float fb = (B->RZ>0.0f) ? 1.0f-1.0f/(B->RZ*C_FZP) : 0.0f; if(fb<0)fb=0; if(fb>1)fb=1;
  float fc = (C->RZ>0.0f) ? 1.0f-1.0f/(C->RZ*C_FZP) : 0.0f; if(fc<0)fc=0; if(fc>1)fc=1;
  BT[0].Z=A->LB*fa; BT[0].U=A->LG*fa; BT[0].V=A->LR*fa;
  BT[1].Z=B->LB*fb; BT[1].U=B->LG*fb; BT[1].V=B->LR*fb;
  BT[2].Z=C->LB*fc; BT[2].U=C->LG*fc; BT[2].V=C->LR*fc;
  RGBTGouraudMMX(BT,(unsigned long)VPage);
}

void The_BiPTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=SCRFIX(A->PX);
  BT[0].Y=SCRFIX(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=SCRFIX(B->PX);
  BT[1].Y=SCRFIX(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=SCRFIX(C->PX);
  BT[2].Y=SCRFIX(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

#ifdef FDS_TEX_SWIZZLE
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) P_Texture_32(BT,_t->Data,(unsigned long)VPage);
    else P_TextureLin_32(BT,_t->Data,(unsigned long)VPage); }
#else
  P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif

  BT[0].U=A->REU;
  BT[0].V=A->REV;

  BT[1].U=B->REU;
  BT[1].V=B->REV;

  BT[2].U=C->REU;
  BT[2].V=C->REV;

#ifdef FDS_TEX_SWIZZLE
  if (TEX_IS_TILED(&Phong_Mapping)) PT_Texture_32(BT,Phong_Mapping.Data,(unsigned long)VPage); //this will be addative
    else PT_TextureLin_32(BT,Phong_Mapping.Data,(unsigned long)VPage);
#else
  PT_Texture_32(BT,Phong_Mapping.Data,(unsigned long)VPage); //this will be addative
#endif

}

void The_BiPTTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=SCRFIX(A->PX);
  BT[0].Y=SCRFIX(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=SCRFIX(B->PX);
  BT[1].Y=SCRFIX(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=SCRFIX(C->PX);
  BT[2].Y=SCRFIX(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

#ifdef FDS_TEX_SWIZZLE
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) PT_Texture_32(BT,_t->Data,(unsigned long)VPage);
    else PT_TextureLin_32(BT,_t->Data,(unsigned long)VPage); }
#else
  PT_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif

  BT[0].U=A->REU;
  BT[0].V=A->REV;

  BT[1].U=B->REU;
  BT[1].V=B->REV;

  BT[2].U=C->REU;
  BT[2].V=C->REV;

#ifdef FDS_TEX_SWIZZLE
  if (TEX_IS_TILED(&Phong_Mapping)) PT_Texture_32(BT,Phong_Mapping.Data,(unsigned long)VPage); //this will be addative
    else PT_TextureLin_32(BT,Phong_Mapping.Data,(unsigned long)VPage);
#else
  PT_Texture_32(BT,Phong_Mapping.Data,(unsigned long)VPage); //this will be addative
#endif
}

void The_BiGTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_GTriangle BT[3];
  BT[0].X=SCRFIX(A->PX);
  BT[0].Y=SCRFIX(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;
  BT[0].R=A->LB*0.5;
  BT[0].G=A->LG*0.5;
  BT[0].B=A->LR*0.5;

  BT[1].X=SCRFIX(B->PX);
  BT[1].Y=SCRFIX(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;
  BT[1].R=B->LB*0.5;
  BT[1].G=B->LG*0.5;
  BT[1].B=B->LR*0.5;

  BT[2].X=SCRFIX(C->PX);
  BT[2].Y=SCRFIX(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;
  BT[2].R=C->LB*0.5;
  BT[2].G=C->LG*0.5;
  BT[2].B=C->LR*0.5;

  PG_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);

/*  Bi_Triangle BT[3];
  BT[0].X=SCRFIX(A->PX);
  BT[0].Y=SCRFIX(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=SCRFIX(B->PX);
  BT[1].Y=SCRFIX(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=SCRFIX(C->PX);
  BT[2].Y=SCRFIX(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

#ifdef FDS_TEX_SWIZZLE
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) P_Texture_32(BT,_t->Data,(unsigned long)VPage);
    else P_TextureLin_32(BT,_t->Data,(unsigned long)VPage); }
#else
  P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif

  BT[0].Z=A->LB;
  BT[0].U=A->LG;
  BT[0].V=A->LR;

  BT[1].Z=B->LB;
  BT[1].U=B->LG;
  BT[1].V=B->LR;

  BT[2].Z=C->LB;
  BT[2].U=C->LG;
  BT[2].V=C->LR;

  RGBGouraud(BT,(unsigned long)(Layer2.Data));*/
}

void The_BiGMMXTrue(Vertex *A,Vertex *B,Vertex *C)
{
#ifdef FDS_FUSED_GOUR

  // S4: single fused rasterization (PG_Texture_32/TEXTGOUR) — per scanline, texture
  // sub-pass then an L1-hot MMX gouraud sweep. Replaces the texture pass + the full
  // second gouraud rasterization (and its cold VPage read/write) below.
  Bi_GTriangle GT[3];
  GT[0].X=SCRFIX(A->PX); GT[0].Y=SCRFIX(A->PY); GT[0].Z=A->RZ; GT[0].U=A->RU; GT[0].V=A->RV;
  GT[0].R=A->LR; GT[0].G=A->LG; GT[0].B=A->LB;
  GT[1].X=SCRFIX(B->PX); GT[1].Y=SCRFIX(B->PY); GT[1].Z=B->RZ; GT[1].U=B->RU; GT[1].V=B->RV;
  GT[1].R=B->LR; GT[1].G=B->LG; GT[1].B=B->LB;
  GT[2].X=SCRFIX(C->PX); GT[2].Y=SCRFIX(C->PY); GT[2].Z=C->RZ; GT[2].U=C->RU; GT[2].V=C->RV;
  GT[2].R=C->LR; GT[2].G=C->LG; GT[2].B=C->LB;
#ifdef FDS_PATHVIZ
  PG_Texture_32(GT,(char *)VizTileE,(unsigned long)VPage);  // solid yellow, gouraud-lit
  return;
#endif
#ifdef FDS_TEX_SWIZZLE
  // dynamic (row-major) textures fall through to the two-pass path with the Lin mapper
  if (TEX_IS_TILED(DoFace->Txtr->Txtr))
  {
    PG_Texture_32(GT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
    return;
  }
#else
  PG_Texture_32(GT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
  return;
#endif
#endif
  // Reached only for dynamic (row-major) textures in the swizzle build. Both consumers below
  // — P_TextureLin_32 and RGBTGouraudMMX — are FDS_SUBTEX mappers, so this is 16.16 too.
  Bi_Triangle BT[3];
  BT[0].X=SCRFIX(A->PX);
  BT[0].Y=SCRFIX(A->PY);
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=SCRFIX(B->PX);
  BT[1].Y=SCRFIX(B->PY);
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=SCRFIX(C->PX);
  BT[2].Y=SCRFIX(C->PY);
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

#ifdef FDS_TEX_SWIZZLE
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) P_Texture_32(BT,_t->Data,(DWord)VPage);
    else P_TextureLin_32(BT,_t->Data,(DWord)VPage); }
#else
  P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(DWord)VPage);
#endif

  BT[0].Z=A->LR;
  BT[0].U=A->LG;
  BT[0].V=A->LB;

  BT[1].Z=B->LR;
  BT[1].U=B->LG;
  BT[1].V=B->LB;

  BT[2].Z=C->LR;
  BT[2].U=C->LG;
  BT[2].V=C->LB;

  RGBTGouraudMMX(BT,(DWord)VPage);
}

// Only one of them usable, but all three transferred due to Filler
// compatability requirements.
/*void Dark_Sprite(Vertex *A,Vertex *B,Vertex *C)
{
  long Cx,Cy;
  long X1,Y1,X2,Y2;
  Cx = A->PX+0.5; Cy = A->PY+0.5;
  if (Cx>=0&&Cy>=0&&Cx<=XRes_1&&Cy<=YRes_1) VPage[Cx+YOffs[Cy]]=64;
}*/

void Dark_Sprite(Vertex *A,Vertex *B,Vertex *C)
{
  char *OmniTxtr = DoFace->Txtr->Txtr->Data;
  int CntX = A->PX;
  int CntY = A->PY;
  long Scale = 5.0*A->RZ;

  long int BX1,BY1,BX2,BY2;
  long int EDX1,EDY1,ECX1,ECY1,CY1,OX;
  long int X,Y,PO,CX,PG,Jmp;
  char *OT2;
  short Col;

  if (BPP!=8) return;
  if (!Scale) return;
  BX1=CntX-Scale;
  BY1=CntY-Scale;
  BX2=CntX+Scale;
  BY2=CntY+Scale;
  EDX1=EDY1=32640/Scale;       //     EDX1*(2*Scale)=255<<8...
  ECY1=0;
  OX=-EDX1;
  if (BY1>YRes_1||BY2<0||BX1>XRes_1||BX2<0) return;

  if (BY2>YRes_1) BY2=YRes_1;
  if (BY1<0) {
    ECY1 -= BY1*EDY1;
    BY1=0;
  }
  if (BX2>XRes_1) BX2=XRes_1;
  if (BX1<0) {
    OX -= BX1*EDX1;
    BX1=0;
  }
  Y=BY2-BY1+1;           // Y pixels on screen
  CX=BX2-BX1+1;          // X pixels on screen
  PG = BX1+BY1*XRes-1; // Offset on page.
  Jmp = XRes-CX;           // line differencial Jump on page
  while (Y--)            // Y loop
  {
  ECX1=OX;           // effective start,should be OX
  X=CX;              // CX pixels on screen
  CY1=ECY1&0xFFFFFF00; // Y Position on flare by interpolation
  OT2=OmniTxtr+CY1;

  while(X--)         // X loop
    // Target page.Addative map.....Interpolate and shr...Increment offset
      VPage[++PG]=OT2[(ECX1+=EDX1)>>8];

  PG+=Jmp;        // Jump to start of new line
  ECY1+=EDY1;   // Interpolate Y
  }
  end:;
}

// Zol,this shit does *NOTHING* , just to make filler pointers shut
// that bouzenki up
void Spirit_16(Vertex *A,Vertex *B,Vertex *C)
{
}

extern "C" void Seven_UP(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux Seven_UP parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

extern "C" void Seven_UP_MMX(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux Seven_UP_MMX parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

extern "C" void ZScale_thatfucksBTB(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux ZScale_thatfucksBTB parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

void The_Seven_UP(Vertex *A,Vertex *B,Vertex *C)
{
  long Size = 10.0*A->RZ;
  Seven_UP(A->PX,A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
}

// Per-pixel z-TESTED additive sprite (test only, never writes z). The old center-pixel probe
// made whole flares blink as their center crossed an occluder edge, and let a peeking flare
// blast its full quad over nearer walls. Sprites are <=20x20, so exact scalar C is free.
// Shared by the omni flares (The_MMX_Scalar) and the fountain's Seven_UP particles, which
// were blitting straight into VPage with no depth test at all.
#ifdef FDS_ZFLIP
void Sprite_ZTested(Vertex *A,long Size,int noZ)
{
  {
    long W = Size<<1, H = Size<<1;
    long x0 = (long)A->PX - Size, y0 = (long)A->PY - Size;
#ifdef FDS_ZLINEAR
    // linear-Z: enc = 0xFF80 - ZLinScale*Z, Z = 1/RZ (match the ASM mappers' buffer encoding)
    float zcf = 65408.0f - ZLinScale*(1.0f/A->RZ);
    if (zcf<0.0f) zcf=0.0f;
    unsigned short zc = (zcf>65408.0f) ? 65408 : (unsigned short)zcf;
#else
    float zcf = A->RZ*ZScaleF;                    // adaptive scale, clamped like the mappers
    unsigned short zc = (zcf>65534.0f) ? 65534 : (unsigned short)zcf;
#endif
    DWord *tex = (DWord *)DoFace->Txtr->Txtr->Data;
    long sx,sy,tu,tv,du,dv;
    if (W<=0||H<=0) return;
    du = (256<<16)/W; dv = (256<<16)/H;
    tv = 0;
    for (sy=0; sy<H; sy++, tv+=dv)
    {
      long py = y0+sy, trow;
      DWord *dst; unsigned short *zrow;
      if (py<0 || py>=YRes) continue;
      dst  = (DWord *)VPage + py*(long)XRes;
      zrow = ZBuf + py*(long)XRes;
      trow = (tv>>16)<<8;
      tu = 0;
      for (sx=0; sx<W; sx++, tu+=du)
      {
        long px = x0+sx; DWord t,d; long r,g,b;
        if (px<0 || px>=XRes) continue;
        if (!noZ && zc < zrow[px]) continue;   // behind a nearer opaque (noZ = A/B off)
        t = tex[trow + (tu>>16)];
        if (!(t&0x00FFFFFF)) continue;     // black flare texel: nothing to add
        d = dst[px];
        b = (d&0xFF)+(t&0xFF);              if (b>255) b=255;
        g = ((d>>8)&0xFF)+((t>>8)&0xFF);    if (g>255) g=255;
        r = ((d>>16)&0xFF)+((t>>16)&0xFF);  if (r>255) r=255;
        dst[px] = (DWord)b | ((DWord)g<<8) | ((DWord)r<<16);
      }
    }
  }
}
#endif

void The_MMX_Scalar(Vertex *A,Vertex *B,Vertex *C)
{
  long Size = 10000.0*A->RZ;
  if (Size>10) Size=10;   // cap: near omnis were ballooning to a screen-filling sprite
#ifdef FDS_ZFLIP
  Sprite_ZTested(A,Size,0);   // omni flares: always z-tested
  return;
#endif
  Seven_UP_MMX(A->PX,A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
}

void The_ZScale_Shit(Vertex *A,Vertex *B,Vertex *C)
{
  long Size = 100.0*A->RZ;
  ZScale_thatfucksBTB(A->PX,A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
}

void Particle_Sprite(Vertex *A,Vertex *B,Vertex *C)
{
  long Cx,Cy;
  long X1,Y1,X2,Y2;
  Cx = A->PX; Cy = A->PY;
  long K;
//  if (BPP!=8) return;
  if (Cx>=0&&Cx<=XRes-2&&Cy>=0&&Cy<=YRes-2)
    VPage[(Cx+YOffs[Cy])*(BPP+1>>3)]=0xFF;
}

// The Exact Rasterizer Project. (+Avatar Mapper)

#define NO_OVERFLOW

void FlatScanLine(DWord *Ptr,long Length,DWord Col);
#pragma aux FlatScanLine=\
  "REP StoSD",\
  parm [EDI] [ECX] [EAX] modify exact [EDI ECX EAX];


/*

// Sub-Pixelling Flat Color Scan-Converter.
// I'll have to redesign the mapper to be more general
// so it will be easily converted into an NGon
void Adv_Flat_32(Vertex *A,Vertex *B,Vertex *C)
{
  float X1,Y1,X2,Y2,X3,Y3,RY1,RY2;
  long QY1,QY2,QY3;
  float dX12,dX13,dX23,dY12,dY13,dY23,dXdY12,dXdY13,dXdY23;
  float LX,RX,dLX,dRX;
  long QLX,QRX;
  DWord *Page = (DWord *)VPage;
  DWord *Ptr;
  DWord Col = 0xFFFFFFFF;
  long Count;
  long RSide = 0;

  // Load data into local coordinates:
  X1 = A->PX; Y1 = A->PY; RoundToInt(&QY1,Y1); RY1 = QY1+1-Y1;
  X2 = B->PX; Y2 = B->PY; RoundToInt(&QY2,Y2); RY2 = QY2+1-Y2;
  X3 = C->PX; Y3 = C->PY; RoundToInt(&QY3,Y3);

  // Calculate Deltas
  dX12 = X2-X1; dY12 = Y2-Y1;
  dX13 = X3-X1; dY13 = Y3-Y1;
  dX23 = X3-X2; dY23 = Y3-Y2;

  // Calculate Slopes along Edges.
  if (dY13>EPSILON)
  {
    dXdY13 = dX13/dY13;
    if (dY12>EPSILON) dXdY12 = dX12/dY12;
    if (dY23>EPSILON) dXdY23 = dX23/dY23;
  }

  // First Scanline
  Ptr = Page + YOffs[QY1];
  if (QY3==QY1)
  {
    LX = RX = X1;
    // Finds minimum and maximum x values.
    if (X1<X2) {LX = X1; RX = X2;} else {LX = X2; RX = X1;}
    if (X3<LX) LX = X3; if (X3>RX) RX = X3;

    RoundToInt(&QLX,LX);
    RoundToInt(&QRX,RX);

    // Scanline from QLX to QRX+1.
    FlatScanLine(Ptr+QLX,QRX-QLX+1,Col);
    return; //This is the only Scan-line.
  } else if (QY2==QY1)
  {
    if (dX12>=0)
    {
      LX = X1; if (dX13<0) LX += RY1*dXdY13;
      RX = X2; if (dX23>0) RX += RY2*dXdY23;

      RoundToInt(&QLX,LX);
      RoundToInt(&QRX,RX);

      // Scanline from QLX to QRX+1.
      FlatScanLine(Ptr+QLX,QRX-QLX+1,Col);
      goto AfterScanY2;
    } else {
      RX = X1; if (dX13>0) RX += RY1*dXdY13;
      LX = X2; if (dX23<0) LX += RY2*dXdY23;

      RoundToInt(&QLX,LX);
      RoundToInt(&QRX,RX);

      // Scanline from QLX to QRX+1.
      FlatScanLine(Ptr+QLX,QRX-QLX+1,Col);
      goto AfterScanY2;

    }
  }

  // Initial Values.
  Ptr += XRes;

  if (dXdY13<dXdY12)
  {
    LX = X1 + dXdY13 * RY1; dLX = dXdY13;
    RX = X1 + dXdY12 * RY1; dRX = dXdY12;
    RSide = 1;
  } else {
    LX = X1 + dXdY12 * RY1; dLX = dXdY12;
    RX = X1 + dXdY13 * RY1; dRX = dXdY13;
    RSide = 0;
  }

  if (dLX<0) LX+=dLX;
  if (dRX>0) RX+=dRX;

  //Y1====>Y2 regular scanline pass.
  for(Count = QY1+1 ; Count<QY2 ; Count++)
  {
    RoundToInt(&QLX,LX);
    RoundToInt(&QRX,RX);

    // Scanline from QLX to QRX+1.
    FlatScanLine(Ptr+QLX,QRX-QLX+1,Col);

    LX += dLX;
    RX += dRX;
    Ptr += XRes;
  }
  // now We'll do the Y2-Scanline
  if (QY3 == QY2)
  {
    if (RSide)
    {
      RX = X2;
      if (dX12<0) RX = X2 - fmod(Y1,1.0) * dXdY12;
      if (dX23>0) RX = X3;
    } else {
      LX = X2;
      if (dX12>0) LX = X2 - fmod(Y1,1.0) * dXdY12;
      if (dX23<0) LX = X3;
    }
  } else {
    if (RSide)
    {
      RX = X2;
      if (dX12<0) RX = X2 - fmod(Y1,1.0) * dXdY12;
      if (dX23>0) RX = X2 + RY2 * dXdY23;
    } else {
      LX = X2;
      if (dX12>0) LX = X2 - fmod(Y1,1.0) * dXdY12;
      if (dX23<0) LX = X2 + RY2 * dXdY23;
    }
  }
  // Scan line runs from LX to RX
  RoundToInt(&QLX,LX);
  RoundToInt(&QRX,RX);

  // Scanline from QLX to QRX+1.
  FlatScanLine(Ptr+QLX,QRX-QLX+1,Col);
  if (QY2==QY3) return;

  AfterScanY2:
  //Initial values
  Ptr += XRes;

  if (RSide)
  {
    if (dX23>0) {dRX = dXdY23; RX += dRX;}
    else {RX = X2 + dXdY23 * RY2; dRX = dXdY23;}
  } else {
    if (dX23<0) {dLX = dXdY23; LX += dLX;}
    else {LX = X2 + dXdY23 * RY2; dLX = dXdY23;}
  }

  // Y2===>Y3 regular scanline pass
  for(Count = QY2+1 ; Count<QY3 ; Count++)
  {
    RoundToInt(&QLX,LX);
    RoundToInt(&QRX,RX);

    // Scanline from QLX to QRX+1.
    FlatScanLine(Ptr+QLX,QRX-QLX+1,Col);

    LX += dLX;
    RX += dRX;
    Ptr += XRes;
  }

  // Last Scanline
  if (RSide)
  {
    if (dX23>0) RX = X3;
    if (dX13<0) LX = X3;
  } else {
    if (dX13>0) RX = X3;
    if (dX23<0) LX = X3;
  }
  RoundToInt(&QLX,LX);
  RoundToInt(&QRX,RX);

  // Scanline from QLX to QRX+1.
  FlatScanLine(Ptr+QLX,QRX-QLX+1,Col);
}



struct VtxLoop
{
  Vertex *Data;
  VtxLoop *Next,*Prev;
};

void Advanced_Flat_32(Vertex *A,Vertex *B,Vertex *C)
{
  // Vertex Loop.
  VtxLoop *VL = new VtxLoop;
  VL->Data = A;
  VL->Next = new VtxLoop;
  VL->Next->Prev = VL;
  VL=VL->Next;
  VL->Data = B;
  VL->Next = new VtxLoop;
  VL->Next->Prev = VL;
  VL=VL->Next;
  VL->Data = C;
  VL->Next = VL->Prev->Prev;
  VL->Next->Prev = VL;
  VL = VL->Next;
}


// This sux, I really wanna have a CORRECT filler,
// and do it as general as possible.
*/
// =======================================================================================
// N-gon path (FDS_NGON)
//
// The screen clipper (FRUSTRUM.CPP UFE_Poly) hands over one convex polygon instead of the
// 2-4 triangles the UFE1..UFE4 cascade used to fan it into. Measured on city, that fan was
// 1.65 triangles per clipped face -- 10.8% of every triangle the frame drew, and those
// extras are clip slivers where the per-section FPU setup dwarfs the inner loop.
//
// Splitting the work: C++ finds the top/bottom vertices and decides which way round the
// polygon is the left edge; the ASM just walks the chains. The walkers were already index
// driven, so they needed only longer arrays and a flat-section skip -- see the n-gon
// prologue in BITTRUE.ASM, which documents the struct offsets this must match.
#ifdef FDS_NGON

struct NGonChains {      // OFFSETS ARE HARD-CODED IN THE ASM PROLOGUE - keep in sync
  void *Left[10];        //  0   bottom-first, top-last
  void *Right[10];       // 40
  long  NLeft;           // 80   section count = vertices-1
  long  NRight;          // 84
  void *Top;             // 88
};

extern "C" void PT_Texture_32_N(void *chains,char *Bitmap,unsigned int u);
#pragma aux PT_Texture_32_N parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
extern "C" void PT_TextureLin_32_N(void *chains,char *Bitmap,unsigned int u);
#pragma aux PT_TextureLin_32_N parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
extern "C" void P_Texture_32_N(void *chains,char *Bitmap,unsigned int u);
#pragma aux P_Texture_32_N parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
extern "C" void P_TextureLin_32_N(void *chains,char *Bitmap,unsigned int u);
#pragma aux P_TextureLin_32_N parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
extern "C" void PG_Texture_32_N(void *chains,char *Bitmap,unsigned int u);
#pragma aux PG_Texture_32_N parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
extern "C" void RGBTGouraudMMX_N(void *chains,unsigned int u);
#pragma aux RGBTGouraudMMX_N parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

// Returns 0 when the polygon covers no scanlines (zero height, or collinear at the top).
static long NGon_BuildChains(Bi_Triangle *V,long n,NGonChains *pc)
{
  long i,t=0,b=0,f,w,k,nf,nw;
  long fwd[12],bwd[12];
  double ax,ay,bx,by,cross;

  for (i=1;i<n;i++)
  {
    if (V[i].Y < V[t].Y) t=i;
    if (V[i].Y > V[b].Y) b=i;
  }
  if (t==b) return 0;

  // Which way round is the LEFT chain? Take the two edges leaving the top vertex. Screen Y
  // grows DOWNWARD, so the forward edge is the left one exactly when the cross product is
  // negative. In double because 16.16 deltas square past 32 bits. This asks the geometry
  // rather than assuming a winding: the clipper preserves input winding, but backface
  // culling is what fixes it, and hard-coding that coupling would break silently.
  f = (t+1==n) ? 0 : t+1;
  w = (t==0) ? n-1 : t-1;
  ax = (double)V[f].X - (double)V[t].X;  ay = (double)V[f].Y - (double)V[t].Y;
  bx = (double)V[w].X - (double)V[t].X;  by = (double)V[w].Y - (double)V[t].Y;
  cross = ax*by - ay*bx;
  if (cross == 0.0) return 0;

  nf=0; i=t; while (i!=b) { fwd[nf++]=i; i=(i+1==n)?0:i+1; } fwd[nf]=b;
  nw=0; i=t; while (i!=b) { bwd[nw++]=i; i=(i==0)?n-1:i-1; } bwd[nw]=b;

  if (cross < 0.0)
  { for (k=0;k<=nf;k++) pc->Left [nf-k] = &V[fwd[k]];  pc->NLeft  = nf;
    for (k=0;k<=nw;k++) pc->Right[nw-k] = &V[bwd[k]];  pc->NRight = nw; }
  else
  { for (k=0;k<=nw;k++) pc->Left [nw-k] = &V[bwd[k]];  pc->NLeft  = nw;
    for (k=0;k<=nf;k++) pc->Right[nf-k] = &V[fwd[k]];  pc->NRight = nf; }
  pc->Top = (void *)&V[t];
  return 1;
}

void The_BiTTrue_N(Vertex **V,long n)
{
  Bi_Triangle BT[12];
  NGonChains pc;
  long i;
  for (i=0;i<n;i++)
  {
    BT[i].X = SCRFIX(V[i]->PX); BT[i].Y = SCRFIX(V[i]->PY);
    BT[i].Z = V[i]->RZ; BT[i].U = V[i]->RU; BT[i].V = V[i]->RV;
  }
  if (!NGon_BuildChains(BT,n,&pc)) return;
#ifdef FDS_TEX_SWIZZLE
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) PT_Texture_32_N(&pc,_t->Data,(unsigned long)VPage);
    else PT_TextureLin_32_N(&pc,_t->Data,(unsigned long)VPage); }
#else
  PT_Texture_32_N(&pc,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif
}

// Bi_GTriangle twin. Same geometry decisions as NGon_BuildChains -- only the vertex stride
// differs -- so the two must stay in step; any change to one belongs in the other.
static long NGon_BuildChainsG(Bi_GTriangle *V,long n,NGonChains *pc)
{
  long i,t=0,b=0,f,w,k,nf,nw;
  long fwd[12],bwd[12];
  double ax,ay,bx,by,cross;

  for (i=1;i<n;i++)
  {
    if (V[i].Y < V[t].Y) t=i;
    if (V[i].Y > V[b].Y) b=i;
  }
  if (t==b) return 0;
  f = (t+1==n) ? 0 : t+1;
  w = (t==0) ? n-1 : t-1;
  ax = (double)V[f].X - (double)V[t].X;  ay = (double)V[f].Y - (double)V[t].Y;
  bx = (double)V[w].X - (double)V[t].X;  by = (double)V[w].Y - (double)V[t].Y;
  cross = ax*by - ay*bx;
  if (cross == 0.0) return 0;

  nf=0; i=t; while (i!=b) { fwd[nf++]=i; i=(i+1==n)?0:i+1; } fwd[nf]=b;
  nw=0; i=t; while (i!=b) { bwd[nw++]=i; i=(i==0)?n-1:i-1; } bwd[nw]=b;

  if (cross < 0.0)
  { for (k=0;k<=nf;k++) pc->Left [nf-k] = &V[fwd[k]];  pc->NLeft  = nf;
    for (k=0;k<=nw;k++) pc->Right[nw-k] = &V[bwd[k]];  pc->NRight = nw; }
  else
  { for (k=0;k<=nw;k++) pc->Left [nw-k] = &V[bwd[k]];  pc->NLeft  = nw;
    for (k=0;k<=nf;k++) pc->Right[nf-k] = &V[fwd[k]];  pc->NRight = nf; }
  pc->Top = (void *)&V[t];
#ifdef FDS_NGONLOG
  // The walker assumes each chain descends monotonically in Y (true for a convex polygon).
  // If SH ever emits a non-convex or non-monotonic ring, spans go wild -- check it.
  { static long _nm = 0; long q;
    Bi_GTriangle **L=(Bi_GTriangle **)pc->Left,**R=(Bi_GTriangle **)pc->Right;
    for (q=pc->NLeft;q>0 && _nm<20;q--)
      if (L[q]->Y > L[q-1]->Y)
      { fprintf(LogFile,"NGONMONO L n=%ld seg=%ld Y=%ld->%ld\n",n,q,(long)L[q]->Y,(long)L[q-1]->Y);
        fflush(LogFile); _nm++; }
    for (q=pc->NRight;q>0 && _nm<20;q--)
      if (R[q]->Y > R[q-1]->Y)
      { fprintf(LogFile,"NGONMONO R n=%ld seg=%ld Y=%ld->%ld\n",n,q,(long)R[q]->Y,(long)R[q-1]->Y);
        fflush(LogFile); _nm++; }
    // and the chains must start/end on the same vertices
    if (_nm<20 && (L[pc->NLeft]!=R[pc->NRight] || L[0]!=R[0]))
    { fprintf(LogFile,"NGONENDS n=%ld Ltop=%p Rtop=%p Lbot=%p Rbot=%p\n",
        n,(void*)L[pc->NLeft],(void*)R[pc->NRight],(void*)L[0],(void*)R[0]);
      fflush(LogFile); _nm++; }
  }
#endif
  return 1;
}

// The fused mapper: one edge walk, texture sub-pass then an L1-hot gouraud sweep. This is
// city's dominant filler, so it is where the clipper fan actually costs setups.
void The_BiGMMXTrue_N(Vertex **V,long n)
{
  Bi_GTriangle GT[12];
  NGonChains pc;
  long i;
  for (i=0;i<n;i++)
  {
    GT[i].X = SCRFIX(V[i]->PX); GT[i].Y = SCRFIX(V[i]->PY);
    GT[i].Z = V[i]->RZ; GT[i].U = V[i]->RU; GT[i].V = V[i]->RV;
    GT[i].R = V[i]->LR; GT[i].G = V[i]->LG; GT[i].B = V[i]->LB;
  }
  if (!NGon_BuildChainsG(GT,n,&pc)) return;
  PG_Texture_32_N(&pc,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
}

void The_BiTrue_N(Vertex **V,long n)
{
  Bi_Triangle BT[12];
  NGonChains pc;
  long i;
  for (i=0;i<n;i++)
  {
    BT[i].X = SCRFIX(V[i]->PX); BT[i].Y = SCRFIX(V[i]->PY);
    BT[i].Z = V[i]->RZ; BT[i].U = V[i]->RU; BT[i].V = V[i]->RV;
  }
  if (!NGon_BuildChains(BT,n,&pc)) return;
#ifdef FDS_TEX_SWIZZLE
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) P_Texture_32_N(&pc,_t->Data,(unsigned long)VPage);
    else P_TextureLin_32_N(&pc,_t->Data,(unsigned long)VPage); }
#else
  P_Texture_32_N(&pc,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif
}

// Transparent textured Gouraud, the city water. Still two passes, but both now walk the
// polygon once each instead of the cascade's 2-4 triangles. The chains point INTO BT, so the
// light payload can be swapped in place and the same chains reused for pass 2 -- and both
// passes therefore rasterize identical coverage, which is what keeps the seam fixed.
void The_BiGTTrue_N(Vertex **V,long n)
{
  Bi_Triangle BT[12];
  NGonChains pc;
  long i;
  for (i=0;i<n;i++)
  {
    BT[i].X = SCRFIX(V[i]->PX); BT[i].Y = SCRFIX(V[i]->PY);
    BT[i].Z = V[i]->RZ; BT[i].U = V[i]->RU; BT[i].V = V[i]->RV;
  }
  if (!NGon_BuildChains(BT,n,&pc)) return;
#ifdef FDS_TEX_SWIZZLE
  { Texture *_t=DoFace->Txtr->Txtr;
    if (TEX_IS_TILED(_t)) PT_Texture_32_N(&pc,_t->Data,(unsigned long)VPage);
    else PT_TextureLin_32_N(&pc,_t->Data,(unsigned long)VPage); }
#else
  PT_Texture_32_N(&pc,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
#endif
  for (i=0;i<n;i++)
  {
    float f = (V[i]->RZ>0.0f) ? 1.0f-1.0f/(V[i]->RZ*C_FZP) : 0.0f;
    if (f<0) f=0; if (f>1) f=1;
    BT[i].Z = V[i]->LB*f; BT[i].U = V[i]->LG*f; BT[i].V = V[i]->LR*f;
  }
  RGBTGouraudMMX_N(&pc,(unsigned long)VPage);
}

// Asked once per clipped face. 0 means "this filler has no n-gon entry yet" and the
// caller falls back to the UFE1..UFE4 cascade, so fillers convert one at a time.
extern "C" void *NGon_FillerFor(void *triFiller)
{
  if (triFiller == (void *)&The_BiTTrue) return (void *)&The_BiTTrue_N;
  if (triFiller == (void *)&The_BiTrue)  return (void *)&The_BiTrue_N;
  if (triFiller == (void *)&The_BiGTTrue) return (void *)&The_BiGTTrue_N;
#ifdef FDS_FUSED_GOUR
  if (triFiller == (void *)&The_BiGMMXTrue)
  {
#ifdef FDS_TEX_SWIZZLE
    // Dynamic (row-major) textures have no fused mapper at all -- The_BiGMMXTrue falls
    // through to its two-pass path for them, and there is no n-gon twin of that yet.
    // Decline here so the caller uses the cascade; declining is the ONLY correct answer,
    // since returning the n-gon entry and bailing inside it would drop the face.
    if (!TEX_IS_TILED(DoFace->Txtr->Txtr)) return 0;
#endif
    return (void *)&The_BiGMMXTrue_N;
  }
#endif
  return 0;
}

#else
extern "C" void *NGon_FillerFor(void *triFiller) { (void)triFiller; return 0; }
#endif
