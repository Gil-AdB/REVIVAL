#include <string.h>
#include <stdio.h>
#include <stdlib.h>
//#include <conio.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <Base/Omni.h>
#include <Base/Scene.h>

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FDS_DEFS.H"
#include "Base/RenderStats.h"
#include "Base/VertexFrame.h"  // SoA Phase 4: F->frame access in TBR helpers
#include "SimdHelpers.h"
#include "Mekalele.h"  // g_gbuffer — flare filler's mirror-footprint gate
#include "RENDER/Hdr.h"  // HDR overlay reorg — sprites accumulate into g_hdrBuf
#include "RENDER/GreetsMirror.h"  // MirrorMaskScreenBBox — --mirror_flare_bbox
#include "Base/FeatureFlags.h"
#include <Threads.h>
// #define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/mmx.h"

float CParticle_ISize;
float ImageSize = 1000.0;

std::atomic<double> FillerPixelcount;

#define ENABLE_PIXELCOUNT
#define _A V[0]
#define _B V[1]
#define _C V[2]

void Draw_Line_8Bit(Vertex *V1,Vertex *V2)
{
  char C=64;
  int32_t D;
  int32_t Dx,Dy;
  int32_t Ai,Bi,Xi,Yi;
  int32_t Offs;
  int32_t X1,Y1,X2,Y2;
  byte *Ptr = VPage;
  byte *End;

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
    Ai = (Dy-Dx)<<1;
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
    Ai = (Dx-Dy)<<1;
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
  int32_t D;
  int32_t Dx,Dy;
  int32_t Ai,Bi,Xi,Yi;
	int32_t Offs;
  int32_t X1,Y1,X2,Y2;
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
    Ai = (Dy-Dx)<<1;
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
    Ai = (Dx-Dy)<<1;
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
  int32_t D;
  int32_t Dx,Dy;
  int32_t Ai,Bi,Xi,Yi;
  int32_t Offs;
  int32_t X1,Y1,X2,Y2;
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
    Ai = (Dy-Dx)<<1;
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
    Ai = (Dx-Dy)<<1;
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
//void Flat_Poly_8Bit(Vertex **V, dword numVerts)
//{
//	int32_t X1,Y1,X2,Y2,X3,Y3; //Pixelled projected vertex position
//	int32_t dX12,dX13,dX23;    //Fixed point Slopes. (16:16)
//	int32_t dx12,dx13,dx23;    //X-deltas.
//	int32_t dy12,dy13,dy23;    //Y-deltas.
//	int32_t X,Y;               //Loop and generic variables.
//	int32_t x1,x2;
//  int32_t EX1,EX2;           //Edge interpolation rate.
//  int32_t dX1,dX2;           //Edge interpolation differencial.
//  byte *Pen;              //Viewport Scanning Pointer
//  char Col=(DoFace->A->LR+DoFace->B->LR+DoFace->C->LR)*0.32; //Average Color.
//  char EndAtMiddle=0;     //Flag: End drawing in the middle(ignore Y2->y3).
//  int32_t Horiz_Span = XRes<<16; //Horizon Span. Used to Calculate Jmp.
//
//  X1 = _A->PX+0.5; Y1 = _A->PY+0.5;
//  X2 = _B->PX+0.5; Y2 = _B->PY+0.5;
//  X3 = _C->PX+0.5; Y3 = _C->PY+0.5;
//
//  dx12 = X2-X1; dy12=Y2-Y1;
//  dx13 = X3-X1; dy13=Y3-Y1;
//  dx23 = X3-X2; dy23=Y3-Y2;
//
//  if (Y1==Y3)
//  {
//    if (X1<=X2)
//		{
//      Pen = VPage+YOffs[Y1]+X1;
//      memset(Pen,Col,dx12+1);
//      if (X3<X1) memset(Pen+dx13,Col,1-dx13); else if (X2<X3) memset(Pen+dx12,Col,dx23+1);
//    } else {
//      Pen = VPage+YOffs[Y1]+X2;
//      memset(Pen,Col,1-dx12);
//      Pen-=dx12;
//      if (X3<X2) memset(Pen+dx13,Col,1-dx13); else if (X1<X3) memset(Pen-dx12,Col,dx13+1);
//    }
//    goto END_POLY;
//  }
//  dX13 = (dx13<<16)/dy13;
//  if (Y1==Y2)
//  {
//    if (X1<=X2)
//    {
//      Pen = VPage+YOffs[Y1]+X1;
//      X = dx12+1;
//			memset(Pen,Col,X);
//
//      EX1 = X1<<16;
//      EX2 = X2<<16;
//      Pen+=XRes-X1;
//      goto MIDDLE_POLY;
//    } else {
//      Pen = VPage+YOffs[Y1]+X2;
//      X = 1-dx12;
//      memset(Pen,Col,X);
//
//      EX1 = X2<<16;
//      EX2 = X1<<16;
//      Pen+=XRes-X2;
//      goto MIDDLE_POLY;
//    }
//  }
//  dX12 = (dx12<<16)/dy12;
//  if (Y2==Y3) EndAtMiddle=1;
//	// Scan conv Poly BEGINS.
//  // Initialization...
//  EX1 = EX2 = X1<<16;
//  EX2 += 65536;
//
//  if (dX12<=dX13)
//  {
//    dX1 = dX12;
//    dX2 = dX13;
//  } else {
//    dX1 = dX13;
//    dX2 = dX12;
//  }
//
//  Y = dy12;
//  Pen = VPage+YOffs[Y1];
//  EX1-=dX1;
//  EX2-=dX2;
//  do
//	{
//    EX1+=dX1;
//    EX2+=dX2;
//    x1=EX1>>16;
//    x2=EX2>>16;
///*    if (x2==XRes_1)
//    {
//      Pen=NULL;
//      return;
//    }*/
//    memset(Pen+x1,Col,x2-x1+1);
//    Pen+=XRes;
//  } while (Y--);
//  MIDDLE_POLY: if (EndAtMiddle) goto END_POLY;
//  dX23 = (dx23<<16)/dy23;
//  if (dX13<=dX23)
//  {
//    dX1 = dX23;
//    dX2 = dX13;
//	} else {
//    dX1 = dX13;
//    dX2 = dX23;
//  }
//  Y = dy23-1;
//  do
//  {
//    EX1+=dX1;
//    EX2+=dX2;
//    x1 = EX1>>16;
//    x2 = EX2>>16;
///*    if (x2==XRes_1)
//    {
//      Pen=NULL;
//      return;
//    }*/
//    memset(Pen+x1,Col,x2-x1+1);
//    Pen+=XRes;
//  } while (Y--);
//	END_POLY:;
//}


//void Flat_Poly_ManyBits(Vertex **V, dword numVerts)
//{	
//#ifdef ENABLE_PIXELCOUNT
//	FDS_STATS_ADD_PIXELS(0.5*fabs((_A->PX - _C->PX)*(_B->PY - _C->PY) - (_A->PY - _C->PY)*(_B->PX - _C->PX)));
//#endif
//
//	int32_t X1,Y1,X2,Y2,X3,Y3; //Pixelled projected vertex position
//	int32_t dX12,dX13,dX23;    //Fixed point Slopes. (16:16)
//	int32_t dx12,dx13,dx23;    //X-deltas.
//	int32_t dy12,dy13,dy23;    //Y-deltas.
//	int32_t X,Y;               //Loop and generic variables.
//	int32_t x1,x2;
//	int32_t EX1,EX2;           //Edge interpolation rate.
//	int32_t dX1,dX2;           //Edge interpolation differencial.
//	byte *Pen;              //Viewport Scanning Pointer
//	dword 
//		r=(DoFace->A->LR+DoFace->B->LR+DoFace->C->LR)*0.32, //Average Color.
//		g=(DoFace->A->LG+DoFace->B->LG+DoFace->C->LG)*0.32, //Average Color.
//		b=(DoFace->A->LB+DoFace->B->LB+DoFace->C->LB)*0.32; //Average Color.  
//	char EndAtMiddle=0;     //Flag: End drawing in the middle(ignore Y2->y3).
//	int32_t Horiz_Span = XRes<<16; //Horizon Span. Used to Calculate Jmp.
//	char CPP = (BPP+1)>>3;
//	dword Col, C16;
//	
//	switch (BPP)
//	{
//	case 8:
//		Col = (r<<24)+(r<<16)+(r<<8)+r;
//		break;
//	case 15:
//		C16 = ((r>>3)<<10)+((g>>3)<<5)+(b>>3);
//		Col = (C16<<16)+C16;
//		break;
//	case 16:
//		C16 = ((r>>3)<<11)+((g>>2)<<5)+(b>>3);
//		Col = (C16<<16)+C16;
//		break;
//	case 24:
//	case 32:
//		Col = (r<<16)+(g<<8)+b;
//	}
//	
//	
//	X1 = _A->PX+0.5; Y1 = _A->PY+0.5;
//	X2 = _B->PX+0.5; Y2 = _B->PY+0.5;
//	X3 = _C->PX+0.5; Y3 = _C->PY+0.5;
//	
//	dx12 = X2-X1; dy12=Y2-Y1;
//	dx13 = X3-X1; dy13=Y3-Y1;
//	dx23 = X3-X2; dy23=Y3-Y2;
//	
//	if (Y1==Y3)
//	{
//		if (X1<=X2)
//		{
//			Pen = VPage+(YOffs[Y1]+X1)*CPP;
//			//      memset(Pen,Col,CPP*(dx12+1));
//			//      if (X3<X1) memset(Pen+dx13*CPP,Col,CPP*(1-dx13)); else if (X2<X3) memset(Pen+dx12*CPP,Col,CPP*(dx23+1));
//		} else {
//			Pen = VPage+(YOffs[Y1]+X2)*CPP;
//			//    memset(Pen,Col,CPP*(1-dx12));
//			Pen-=dx12;
//			//      if (X3<X2) memset(Pen+dx13*CPP,Col,CPP*(1-dx13)); else if (X1<X3) memset(Pen-dx12*CPP,Col,CPP*(dx13+1));
//		}
//		goto END_POLY;
//	}
//	dX13 = (dx13<<16)/dy13;
//	if (Y1==Y2)
//	{
//		if (X1<=X2)
//		{
//			Pen = VPage+(YOffs[Y1]+X1)*CPP;
//			X = dx12+1;
//			//      memset(Pen,Col,CPP*X);
//			
//			EX1 = X1<<16;
//			EX2 = X2<<16;
//			Pen+=(XRes-X1)*CPP;
//			goto MIDDLE_POLY;
//		} else {
//			Pen = VPage+(YOffs[Y1]+X2)*CPP;
//			X = 1-dx12;
//			//      memset(Pen,Col,CPP*X);
//			
//			EX1 = X2<<16;
//			EX2 = X1<<16;
//			Pen+=(XRes-X2)*CPP;
//			goto MIDDLE_POLY;
//		}
//	}
//	dX12 = (dx12<<16)/dy12;
//	if (Y2==Y3) EndAtMiddle=1;
//	// Scan conv Poly BEGINS.
//	// Initialization...
//	EX1 = EX2 = X1<<16;
//	//EX2 += 65536;
//	
//	if (dX12<=dX13)
//	{
//		dX1 = dX12;
//		dX2 = dX13;
//	} else {
//		dX1 = dX13;
//		dX2 = dX12;
//	}
//	
//	Y = dy12;
//	Pen = VPage+YOffs[Y1]*CPP;
//	EX1-=dX1;
//	EX2-=dX2;
//	do
//	{
//		EX1+=dX1;
//		EX2+=dX2;
//		x1=EX1>>16;
//		x2=EX2>>16;
//		/*    if (x2==XRes_1)
//		{
//		Pen=NULL;
//		return;
//    }*/
//		if (x2>=x1) 
//			//			memset(Pen+CPP*x1,Col,CPP*(x2-x1));
//			FastWrite(Pen+CPP*x1,Col,x2-x1);
//		Pen+=CPP*XRes;
//	} while (Y--);
//	MIDDLE_POLY: 
//	
//	if (EndAtMiddle) goto END_POLY;
//	dX23 = (dx23<<16)/dy23;
//	if (dX13<=dX23)
//	{
//		dX1 = dX23;
//		dX2 = dX13;
//	} else {
//		dX1 = dX13;
//		dX2 = dX23;
//	}
//	Y = dy23-1;
//	do
//	{
//		EX1+=dX1;
//		EX2+=dX2;
//		x1 = EX1>>16;
//		x2 = EX2>>16;
//		if (x2>=x1) 
//			FastWrite(Pen+CPP*x1,Col,x2-x1);
//		Pen+=CPP*XRes;
//	} 
//	while (Y--);
//
//END_POLY:;
//}

/*
void FlatV2_Poly_8Bit(Vertex **V, dword numVerts)
{
  char Col=(DoFace->A->LR+DoFace->B->LR+DoFace->C->LR)*0.32;
	int32_t Ex1,Ex2,x1,x2;
  int32_t X1,X2,X3,Y1,Y2,Y3;
  int32_t T;
  int32_t dX12,dX13,dX23;
  int32_t X,Y;

  byte *Ptr,*Line,*PtrEnd;

  X1 = _A->PX+0.5; X2 = _B->PX+0.5; X3 = _C->PX+0.5;
  Y1 = _A->PY+0.5; Y2 = _B->PY+0.5; Y3 = _C->PY+0.5;

  if (Y1<Y3) {
    dX13=((X3-X1)<<16)/(Y3-Y1);

    if (Y1<Y2) {
      dX12=((X2-X1)<<16)/(Y2-Y1);
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
      dX23=((X3-X2)<<16)/(Y3-Y2);
      goto middle;
    }
    if (Y2<Y3) dX23=((X3-X2)<<16)/(Y3-Y2);
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
  middle: Y=Y3-Y2;  //After Y1->y2
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
	end:; //After Y2->y3
}
*/
// Calls THE Bilinear mapper
/*
extern void __cdecl Adv_PTexture_8(Bi_Floating * vtx, char * bitmap,unsigned int u);

void The_Specular_Gouraud(Vertex **V, dword numVerts)
{
	Vertex *A = V[0], *B, *C;
	for(mword i=2; i<numVerts; i++)
	{
		if (V[i-1]->PY < V[i]->PY)
		{
			B = V[i-1];
			C = V[i];
		} else {
			B = V[i];
			C = V[i-1];
		}

		Bi_Triangle BT[3];
		BT[0].x=A->PX;
		BT[0].y=A->PY;
		BT[0].z=A->LR;
		BT[0].U=A->LG;
		BT[0].V=A->LB;
		
		BT[1].x=B->PX;
		BT[1].y=B->PY;
		BT[1].z=B->LR;
		BT[1].U=B->LG;
		BT[1].V=B->LB;
		
		BT[2].x=C->PX;
		BT[2].y=C->PY;
		BT[2].z=C->LR;
		BT[2].U=C->LG;
		BT[2].V=C->LB;
		CRGBGouraud(BT,(uint32_t)VPage);
	}
}

void The_MMX_Gouraud(Vertex **V, dword numVerts)
{
	Bi_Triangle BT[3];
	Vertex *A = V[0], *B, *C;
	for(mword i=2; i<numVerts; i++)
	{
		if (V[i-1]->PY < V[i]->PY)
		{
			B = V[i-1];
			C = V[i];
		} else {
			B = V[i];
			C = V[i-1];
		}
		
		BT[0].x=A->PX;
		BT[0].y=A->PY;
		BT[0].z=A->LR;
		BT[0].U=A->LG;
		BT[0].V=A->LB;
		
		BT[1].x=B->PX;
		BT[1].y=B->PY;
		BT[1].z=B->LR;
		BT[1].U=B->LG;
		BT[1].V=B->LB;
		
		BT[2].x=C->PX;
		BT[2].y=C->PY;
		BT[2].z=C->LR;
		BT[2].U=C->LG;
		BT[2].V=C->LB;
		
		CRGBGouraud(BT,(uint32_t)VPage);
	}
}


void The_Specular_TGouraud(Vertex **V, dword numVerts)
{
  Bi_Triangle BT[3];
	BT[0].x=_A->PX;
	BT[0].y=_A->PY;
  BT[0].z=_A->LB;
  BT[0].U=_A->LG;
  BT[0].V=_A->LR;

  BT[1].x=_B->PX;
  BT[1].y=_B->PY;
  BT[1].z=_B->LB;
  BT[1].U=_B->LG;
  BT[1].V=_B->LR;

  BT[2].x=_C->PX;
  BT[2].y=_C->PY;
  BT[2].z=_C->LB;
  BT[2].U=_C->LG;
  BT[2].V=_C->LR;

  CRGBTGouraudMMX(BT,(uint32_t)VPage);

}

void The_Bilinear(Vertex **V, dword numVerts)
{
	Yosi_Triangle BT[3];

	BT[0].FX=_A->PX;
	BT[0].FY=_A->PY;
	BT[0].z=_A->RZ;
	BT[0].U=_A->UZ;
	BT[0].V=_A->VZ;

	BT[1].FX=_B->PX;
	BT[1].FY=_B->PY;
	BT[1].z=_B->RZ;
	BT[1].U=_B->UZ;
	BT[1].V=_B->VZ;

	BT[2].FX=_C->PX;
	BT[2].FY=_C->PY;
	BT[2].z=_C->RZ;
	BT[2].U=_C->UZ;
	BT[2].V=_C->VZ;

	CP_Texture_8(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);
//  Adv_PTexture_8(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);
}

void The_BiHi(Vertex **V, dword numVerts)
{
  Bi_Triangle BT[3];
  BT[0].x=_A->PX;
  BT[0].y=_A->PY;
  BT[0].z=_A->RZ;
  BT[0].U=_A->UZ;
  BT[0].V=_A->VZ;

  BT[1].x=_B->PX;
  BT[1].y=_B->PY;
  BT[1].z=_B->RZ;
  BT[1].U=_B->UZ;
	BT[1].V=_B->VZ;

  BT[2].x=_C->PX;
  BT[2].y=_C->PY;
  BT[2].z=_C->RZ;
  BT[2].U=_C->UZ;
  BT[2].V=_C->VZ;

  CP_Texture_16(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);
}

void The_BiTrue(Vertex **V, dword numVerts)
{
#ifdef ENABLE_PIXELCOUNT
	FDS_STATS_ADD_PIXELS(0.5*fabs((_A->PX - _C->PX)*(_B->PY - _C->PY) - (_A->PY - _C->PY)*(_B->PX - _C->PX)));
#endif
  Bi_Triangle BT[3];
  BT[0].x=_A->PX;
  BT[0].y=_A->PY;
  BT[0].z=_A->RZ;
	BT[0].U=_A->UZ;
  BT[0].V=_A->VZ;

  BT[1].x=_B->PX;
  BT[1].y=_B->PY;
  BT[1].z=_B->RZ;
  BT[1].U=_B->UZ;
  BT[1].V=_B->VZ;

  BT[2].x=_C->PX;
  BT[2].y=_C->PY;
  BT[2].z=_C->RZ;
  BT[2].U=_C->UZ;
  BT[2].V=_C->VZ;

  CP_Texture_32(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);
//	Draw_Line_32Bit_ForKids(_A,_B);
//	Draw_Line_32Bit_ForKids(_B,_C);
//	Draw_Line_32Bit_ForKids(_C,_A);
}

void The_BiTTrue(Vertex **V, dword numVerts)
{
#ifdef ENABLE_PIXELCOUNT
	FDS_STATS_ADD_PIXELS(0.5*fabs((_A->PX - _C->PX)*(_B->PY - _C->PY) - (_A->PY - _C->PY)*(_B->PX - _C->PX)));
#endif
	Bi_Triangle BT[3];
	BT[0].x=_A->PX;
	BT[0].y=_A->PY;
	BT[0].z=_A->RZ;
	BT[0].U=_A->UZ;
	BT[0].V=_A->VZ;
	
	BT[1].x=_B->PX;
	BT[1].y=_B->PY;
	BT[1].z=_B->RZ;
	BT[1].U=_B->UZ;
	BT[1].V=_B->VZ;
	
	BT[2].x=_C->PX;
	BT[2].y=_C->PY;
	BT[2].z=_C->RZ;
	BT[2].U=_C->UZ;
	BT[2].V=_C->VZ;
	
	CPT_Texture_32(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);
}

void The_BiPNTrue(Vertex **V, dword numVerts)
{
	Bi_Triangle BT[3];
	BT[0].x=_A->PX;
	BT[0].y=_A->PY;
	BT[0].z=_A->RZ;
//	BT[0].U=_A->REU;
//	BT[0].V=_A->REV;

	BT[1].x=_B->PX;
	BT[1].y=_B->PY;
	BT[1].z=_B->RZ;
//	BT[1].U=_B->REU;
//	BT[1].V=_B->REV;

	BT[2].x=_C->PX;
	BT[2].y=_C->PY;
	BT[2].z=_C->RZ;
//	BT[2].U=_C->REU;
//	BT[2].V=_C->REV;

//	CP_Texture_32(BT,Phong_Mapping.Data,(uint32_t)VPage); //this will be addative
}

void The_BiPTrue(Vertex **V, dword numVerts)
{
	Bi_Triangle BT[3];
	BT[0].x=_A->PX;
	BT[0].y=_A->PY;
	BT[0].z=_A->RZ;
	BT[0].U=_A->UZ;
	BT[0].V=_A->VZ;
	
	BT[1].x=_B->PX;
	BT[1].y=_B->PY;
	BT[1].z=_B->RZ;
	BT[1].U=_B->UZ;
	BT[1].V=_B->VZ;
	
	BT[2].x=_C->PX;
	BT[2].y=_C->PY;
	BT[2].z=_C->RZ;
	BT[2].U=_C->UZ;
	BT[2].V=_C->VZ;
	
	CP_Texture_32(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);
	
//	BT[0].U=_A->REU;
//	BT[0].V=_A->REV;

//	BT[1].U=_B->REU;
//	BT[1].V=_B->REV;
	
//	BT[2].U=_C->REU;
//	BT[2].V=_C->REV;

//	CPT_Texture_32(BT,Phong_Mapping.Data,(uint32_t)VPage); //this will be addative
}

void The_BiPTTrue(Vertex **V, dword numVerts)
{
	Bi_Triangle BT[3];
	BT[0].x=_A->PX;
	BT[0].y=_A->PY;
	BT[0].z=_A->RZ;
	BT[0].U=_A->UZ;
	BT[0].V=_A->VZ;
	
	BT[1].x=_B->PX;
	BT[1].y=_B->PY;
	BT[1].z=_B->RZ;
	BT[1].U=_B->UZ;
	BT[1].V=_B->VZ;
	
	BT[2].x=_C->PX;
	BT[2].y=_C->PY;
	BT[2].z=_C->RZ;
	BT[2].U=_C->UZ;
	BT[2].V=_C->VZ;
	
	CPT_Texture_32(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);
	
//	BT[0].U=_A->REU;
//	BT[0].V=_A->REV;
	
//	BT[1].U=_B->REU;
//	BT[1].V=_B->REV;
	
//	BT[2].U=_C->REU;
//	BT[2].V=_C->REV;
	
//	CPT_Texture_32(BT,Phong_Mapping.Data,(uint32_t)VPage); //this will be addative
}

void The_BiGTrue(Vertex **V, dword numVerts)
{
	Bi_GTriangle BT[3];
	float pow;
//pow = _A->RZ*75.0f;
//if (pow>0.5f) pow=0.5f;
	pow = 0.5f;
	BT[0].x=_A->PX;
	BT[0].y=_A->PY;
	BT[0].z=_A->RZ;
	BT[0].U=_A->UZ;
	BT[0].V=_A->VZ;
	BT[0].R=_A->LB*pow;
	BT[0].G=_A->LG*pow;
	BT[0].B=_A->LR*pow;

	//pow = _B->RZ*75.0f;
	//if (pow>0.5f) pow=0.5f;
	//pow = 0.5f;
	BT[1].x=_B->PX;
	BT[1].y=_B->PY;
	BT[1].z=_B->RZ;
	BT[1].U=_B->UZ;
	BT[1].V=_B->VZ;
	BT[1].R=_B->LB*pow;
	BT[1].G=_B->LG*pow;
	BT[1].B=_B->LR*pow;

	//pow = _C->RZ*75.0f;
	//if (pow>0.5f) pow=0.5f;
	BT[2].x=_C->PX;
	BT[2].y=_C->PY;
	BT[2].z=_C->RZ;
	BT[2].U=_C->UZ;
	BT[2].V=_C->VZ;
	BT[2].R=_C->LB*pow;
	BT[2].G=_C->LG*pow;
	BT[2].B=_C->LR*pow;

	CPG_Texture_32(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);

//  Bi_Triangle BT[3];
//  BT[0].x=_A->PX;
//  BT[0].y=_A->PY;
//  BT[0].z=_A->RZ;
//  BT[0].U=_A->UZ;
//  BT[0].V=_A->VZ;
//
//  BT[1].x=_B->PX;
//  BT[1].y=_B->PY;
//  BT[1].z=_B->RZ;
//  BT[1].U=_B->UZ;
//  BT[1].V=_B->VZ;

//  BT[2].x=_C->PX;
//  BT[2].y=_C->PY;
//  BT[2].z=_C->RZ;
//  BT[2].U=_C->UZ;
//  BT[2].V=_C->VZ;

//	P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(uint32_t)VPage);

  //BT[0].z=_A->LB;
  //BT[0].U=_A->LG;
  //BT[0].V=_A->LR;

  //BT[1].z=_B->LB;
  //BT[1].U=_B->LG;
  //BT[1].V=_B->LR;

  //BT[2].z=_C->LB;
  //BT[2].U=_C->LG;
  //BT[2].V=_C->LR;

  //RGBGouraud(BT,(uint32_t)(Layer2.Data));
}

void The_BiGMMXTrue(Vertex **V, dword numVerts)
{
#ifdef ENABLE_PIXELCOUNT
	FDS_STATS_ADD_PIXELS(0.5*fabs((_A->PX - _C->PX)*(_B->PY - _C->PY) - (_A->PY - _C->PY)*(_B->PX - _C->PX)));
#endif
	Bi_Triangle BT[3];
	BT[0].x=_A->PX;
	BT[0].y=_A->PY;
	BT[0].z=_A->RZ;
	BT[0].U=_A->UZ;
	BT[0].V=_A->VZ;
	
	BT[1].x=_B->PX;
	BT[1].y=_B->PY;
	BT[1].z=_B->RZ;
	BT[1].U=_B->UZ;
	BT[1].V=_B->VZ;
	
	BT[2].x=_C->PX;
	BT[2].y=_C->PY;
	BT[2].z=_C->RZ;
	BT[2].U=_C->UZ;
	BT[2].V=_C->VZ;
	
	CP_Texture_32(BT,DoFace->Txtr->Txtr->Data,(DWord)VPage);
	
	BT[0].z=_A->LR;
	BT[0].U=_A->LG;
	BT[0].V=_A->LB;
	
	BT[1].z=_B->LR;
	BT[1].U=_B->LG;
	BT[1].V=_B->LB;
	
	BT[2].z=_C->LR;
	BT[2].U=_C->LG;
	BT[2].V=_C->LB;
	
	CRGBTGouraudMMX(BT,(DWord)VPage);
}

void The_BiGMMXTrue_Fogged(Vertex **V, dword numVerts)
{
	
	Vertex *A = V[0], *B, *C;
	for(mword i=2; i<numVerts; i++)
	{
		if (V[i-1]->PY < V[i]->PY)
		{
			B = V[i-1];
			C = V[i];
		} else {
			B = V[i];
			C = V[i-1];
		}
#ifdef ENABLE_PIXELCOUNT
		FDS_STATS_ADD_PIXELS(0.5*fabs((A->PX - C->PX)*(B->PY - C->PY) - (A->PY - C->PY)*(B->PX - C->PX)));
#endif
		
		Bi_Triangle BT[3];
		BT[0].x=A->PX;
		BT[0].y=A->PY;
		BT[0].z=A->RZ;
		BT[0].U=A->UZ;
		BT[0].V=A->VZ;
		
		BT[1].x=B->PX;
		BT[1].y=B->PY;
		BT[1].z=B->RZ;
		BT[1].U=B->UZ;
		BT[1].V=B->VZ;
		
		BT[2].x=C->PX;
		BT[2].y=C->PY;
		BT[2].z=C->RZ;
		BT[2].U=C->UZ;
		BT[2].V=C->VZ;
		
		CP_Texture_32(BT,DoFace->Txtr->Txtr->Data,(DWord)VPage);
		
		float fogRate;
		fogRate = 1.0 - C_rFZP / A->RZ;
		BT[0].z=A->LR * fogRate;
		BT[0].U=A->LG * fogRate;
		BT[0].V=A->LB * fogRate;
		
		fogRate = 1.0 - C_rFZP / B->RZ;
		BT[1].z=B->LR * fogRate;
		BT[1].U=B->LG * fogRate;
		BT[1].V=B->LB * fogRate;
		
		fogRate = 1.0 - C_rFZP / C->RZ;
		BT[2].z=C->LR * fogRate;
		BT[2].U=C->LG * fogRate;
		BT[2].V=C->LB * fogRate;
		
		CRGBTGouraudMMX(BT,(DWord)VPage);
	}
}

void The_BiMMXTTrue_Fogged(Vertex **V, dword numVerts)
{
	Vertex *A = V[0], *B, *C;
	for(mword i=2; i<numVerts; i++)
	{
		if (V[i-1]->PY < V[i]->PY)
		{
			B = V[i-1];
			C = V[i];
		} else {
			B = V[i];
			C = V[i-1];
		}
#ifdef ENABLE_PIXELCOUNT
		FDS_STATS_ADD_PIXELS(0.5*fabs((A->PX - C->PX)*(B->PY - C->PY) - (A->PY - C->PY)*(B->PX - C->PX)));
#endif
		Bi_Triangle BT[3];
		BT[0].x=A->PX;

		BT[0].y=A->PY;
		BT[0].z=A->RZ;
		BT[0].U=A->UZ;
		BT[0].V=A->VZ;
		
		BT[1].x=B->PX;
		BT[1].y=B->PY;
		BT[1].z=B->RZ;
		BT[1].U=B->UZ;
		BT[1].V=B->VZ;
		
		BT[2].x=C->PX;
		BT[2].y=C->PY;
		BT[2].z=C->RZ;
		BT[2].U=C->UZ;
		BT[2].V=C->VZ;
		
		CPT_Texture_32(BT,DoFace->Txtr->Txtr->Data,(DWord)VPage);

		// fog should be applied BEFORE texture is blended into screen (now it's too late)
		float fogRate;
		fogRate = 1.0 - C_rFZP / A->RZ;
		BT[0].z=255.0 * fogRate;
		BT[0].U=255.0 * fogRate;
		BT[0].V=255.0 * fogRate;
		
		fogRate = 1.0 - C_rFZP / B->RZ;
		BT[1].z=255.0 * fogRate;
		BT[1].U=255.0 * fogRate;
		BT[1].V=255.0 * fogRate;
		
		fogRate = 1.0 - C_rFZP / C->RZ;
		BT[2].z=255.0 * fogRate;
		BT[2].U=255.0 * fogRate;
		BT[2].V=255.0 * fogRate;
		
		CRGBTGouraudMMX(BT,(DWord)VPage);
	}
}
*/

// Only one of them usable, but all three transferred due to Filler
// compatability requirements.
/*void Dark_Sprite(Vertex **V, dword numVerts)
{
  int32_t Cx,Cy;
  int32_t X1,Y1,X2,Y2;
  Cx = _A->PX+0.5; Cy = _A->PY+0.5;
  if (Cx>=0&&Cy>=0&&Cx<=XRes_1&&Cy<=YRes_1) VPage[Cx+YOffs[Cy]]=64;
}*/
/*
void Dark_Sprite(Vertex **V, dword numVerts)
{
  byte *OmniTxtr = DoFace->Txtr->Txtr->Data;
  int CntX = _A->PX;
  int CntY = _A->PY;
  int32_t Scale = 5.0*_A->RZ;

  int32_t int BX1,BY1,BX2,BY2;
  int32_t int EDX1,EDY1,ECX1,ECY1,CY1,OX;
  int32_t int X,Y,PO,CX,PG,Jmp;
  byte *OT2;
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
}
*/
// Zol,this shit does *NOTHING* , just to make filler pointers shut
// that bouzenki up
void Spirit_16(Vertex **V, dword numVerts)
{
}


#if 0
void The_Seven_UP(Vertex **V, dword numVerts)
{
	int32_t Size = ImageSize*_A->RZ;
	// look for flares to match doface
	Omni *O;
	for(O = CurScene->OmniHead;O;O=O->Next)
		if (DoFace == &O->F)
			break;
	if (O) Size *= O->ISize; else Size *= CParticle_ISize;
	if (Size<0) return;
	if (DoFace->Txtr&&DoFace->Txtr->Txtr)
	{
		CSeven_UP(_A->PX,_A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
	}
}

void The_Seven_UP_HALFSIZE(Vertex **V, dword numVerts)
{
	int32_t Size = 2;
	CSeven_UP(_A->PX,_A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
}
#endif


#define CSeven_UP_64 Seven_UP_64V
extern "C" void Seven_UP_64_(void);
void Seven_UP_64V(int X, int Y, int SizeX, int SizeY, byte* Txtr, byte* Page)
{
}

void The_Seven_UP_64(int32_t X, int32_t Y, int32_t SX, int32_t SY, DWord* Bitmap, byte* Page)
{
	CSeven_UP_64(X, Y, SX, SY, (byte*)Bitmap, VPage);
}

#define CSeven_UP_MMX Seven_UP_MMXV

extern "C" void Seven_UP_MMX_(void);
void Seven_UP_MMXV(int X,int Y,int SizeX,int SizeY, byte *Txtr, byte *Page, dword Color, dword Z)
{
}


constexpr uint32_t clog2(uint32_t n)
{
	return ((n < 2) ? 0 : 1 + clog2(n / 2));
}


// MaskMirror: per-pixel planar-mirror clip for MIRROR-CLONE flares
// (--mirror_flare_bbox). A clone flare may only paint pixels the mask pre-pass
// stamped with its own mirror's tag; every other lane is killed. This replaces
// the all-or-nothing CENTRE-pixel gate the clone flares used to take, which
// deleted an entire sprite the moment its centre crossed the mirror edge even
// though most of the sprite was still over the mirror. Default false → the
// instantiation, and its output, are exactly what they were.
template <int TextureRes = 32, bool EnableZTest = false, bool HDRAccum = false,
          bool MaskMirror = false>
void Spriter(float x, float y, float width, float height, void* txtr, void* vpage, dword Color, dword Z, float spriteClipX1, float spriteClipY1, float spriteClipX2, float spriteClipY2,
             const uint8_t* mmask = nullptr, uint32_t mtag = 0) {
	float x1, y1, x2, y2;
	float du, dv;
	float u0 = 0, v0 = 0;
	int32_t  iu, iv, idu, idv;
	int32_t  LineWidth;
	int32_t  Height;

	x1 = (x - width);
	x2 = (x + width);
	y1 = (y - height);
	y2 = (y + height);

	if (x2 <= spriteClipX1 || x1 >= spriteClipX2 || y2 <= spriteClipY1 || y1 >= spriteClipY2) return;

	du = float(TextureRes) / 2.0f / width;
	dv = float(TextureRes) / 2.0f / height;

	if (x1 < 0.0) {
		u0 = du * (-x1);
		x1 = 0.0;
	}

	if (x1 < spriteClipX1) {
		u0 = du * (spriteClipX1 - x1);
		x1 = spriteClipX1;
	}

	if (y1 < spriteClipY1) {
		v0 = dv * (spriteClipY1 - y1);
		y1 = spriteClipY1;
	}

	if (x2 > spriteClipX2) x2 = spriteClipX2;
	if (y2 > spriteClipY2) y2 = spriteClipY2;

	const constexpr int32_t TILE_SIZE = 8;
	int32_t ix1 = Fist(x1);
	int32_t ix2 = Fist(x2);
	int32_t iy1 = Fist(y1);
	int32_t iy2 = Fist(y2);
	int32_t tile_x1 = ix1 / TILE_SIZE;
	int32_t tile_x2 = (ix2 + TILE_SIZE - 1) / TILE_SIZE;

	Vec8i v_u0 = roundi(v8_from_arith_seq(u0 + (tile_x1 * TILE_SIZE - x1) * du, du) * 65536.0f);
	Vec8i v_du = roundi(Vec8f(du * TILE_SIZE * 65536.0f));

	Vec8ui v_v = Vec8ui(roundi(Vec8f((v0 + float(iy1 - y1) * dv) * 65536.0f)));
	Vec8ui v_dv = Vec8ui(roundi(Vec8f(dv * 65536.0f)));
	Vec32us blend_color = Vec32us(Vec32sFromVec4s({ int16_t((Color & 0xff)), int16_t((Color & 0xff00) >> 8), int16_t((Color & 0xff00000) >> 16), int16_t((Color & 0xff000000) >> 24) }));

	auto z_candidate = Vec8ui(Z);

	Vec8i v_x0 = v8_from_arith_seq(tile_x1 * TILE_SIZE, 1);
	dword* scanline = (dword*)(((byte*)vpage) + iy1 * VESA_BPSL);
	// Z-buffer is in its own allocation now (ZPage16 global), no longer
	// contiguous with vpage. iy1 * XRes is in u16 units.
	auto zscanline = (byte*)(ZPage16 + iy1 * XRes);

	for (int32_t y = iy1; y != iy2; ++y) {
		Vec8i v_x = v_x0;
		Vec8i v_u = v_u0;
		Vec8i v_v_ofs = ((v_v >> 16) & (TextureRes - 1)) << clog2(TextureRes);
		dword* span = scanline + tile_x1 * TILE_SIZE;
		auto zspan = ((uint16_t*)zscanline) + tile_x1 * TILE_SIZE;
		for (int32_t tile_x = tile_x1; tile_x != tile_x2; ++tile_x) {
			Vec8ib mask = (ix1 <= v_x) & (v_x < ix2);
			if constexpr (MaskMirror) {
				// 8 mask bytes for this tile row. Byte load, not a vector
				// load: the last tile of a span can reach past XRes (the
				// out-of-span lanes are already masked off, but the READ
				// would still walk off the final row), so copy only the
				// bytes that exist and leave the rest 0 (= no mirror).
				uint8_t mb[8] = {0,0,0,0,0,0,0,0};
				const int32_t bx = tile_x * TILE_SIZE;
				const int32_t navail = (bx + 8 <= int32_t(XRes)) ? 8 : (int32_t(XRes) - bx);
				if (navail > 0)
					std::memcpy(mb, mmask + size_t(y) * size_t(XRes) + size_t(bx),
					            size_t(navail));
				const Vec8i ids(mb[0], mb[1], mb[2], mb[3], mb[4], mb[5], mb[6], mb[7]);
				mask &= (ids == Vec8i(int32_t(mtag)));
			}
			if constexpr (EnableZTest) {
				Vec8us z_existing_c;
				z_existing_c.load_a(zspan);
				auto z_existing = extend(z_existing_c);

				auto zmask = z_candidate > z_existing;
				mask &= zmask;
			}

			auto p_offset = ((v_u >> 16) & (TextureRes - 1)) | v_v_ofs;
			auto texture_samples = gather(Vec8ui(p_offset), txtr, mask);
			texture_samples = colorize(Vec32uc(texture_samples), blend_color);
			if constexpr (HDRAccum) {
				// HDR overlay reorg: additively accumulate the source
				// contribution into the float radiance buffer instead of the
				// 8-bit add_saturated store, so overlapping/over-flash sprites
				// bloom and roll off at the tonemap rather than clipping at
				// 255. Scalar over the masked lanes — sprites are sparse, a
				// SIMD float-RMW gather/scatter is a later optimization.
				alignas(32) uint8_t src[32];
				Vec32uc(texture_samples).store(src);
				int marr[8];
				select(mask, Vec8i(-1), Vec8i(0)).store(marr);
				const int baseX = tile_x * TILE_SIZE;
				fds::hdrf* hbuf = fds::g_hdrBuf.data();
				for (int k = 0; k < 8; ++k) {
					if (!marr[k]) continue;
					fds::hdrf* h = hbuf + (size_t(y) * size_t(XRes) + size_t(baseX + k)) * 4;
					h[0] = fds::HdrClamp(float(h[0]) + float(src[k * 4 + 0]));   // B
					h[1] = fds::HdrClamp(float(h[1]) + float(src[k * 4 + 1]));   // G
					h[2] = fds::HdrClamp(float(h[2]) + float(src[k * 4 + 2]));   // R
				}
			} else {
				Vec32uc dst;
				dst.load_a(span);
				texture_samples = add_saturated(Vec32uc(texture_samples), dst);

				_mm256_maskstore_ps((float*)span, *(__m256i*)(&mask), *(__m256*)(&texture_samples));
			}

			v_x += 8u;
			v_u += v_du;
			span += TILE_SIZE;
			zspan += TILE_SIZE;
		}
		scanline += VESA_BPSL / 4;
		zscanline += XRes * 2;
		v_v += v_dv;
	}
}

// HDR overlay reorg: pick the HDRAccum Spriter instantiation at runtime.
// g_hdrActive is set once the froxel composite has populated g_hdrBuf this
// frame (HDR mode); until then — LDR scenes, --hdr off — sprites take the
// 8-bit add_saturated path and the result is byte-identical to before.
template <int Res>
static inline void SpriterRT(float x, float y, float w, float h, void* txtr,
                             void* vpage, dword Color, dword Z,
                             float cx1, float cy1, float cx2, float cy2,
                             const uint8_t* mmask = nullptr, uint32_t mtag = 0) {
	// mmask != nullptr ONLY for mirror-clone flares under --mirror_flare_bbox
	// (see SpriteMirrorMask). Every other caller takes the identical two
	// instantiations it always took.
	if (mmask) {
		if (fds::g_hdrActive)
			Spriter<Res, true, true,  true>(x, y, w, h, txtr, vpage, Color, Z, cx1, cy1, cx2, cy2, mmask, mtag);
		else
			Spriter<Res, true, false, true>(x, y, w, h, txtr, vpage, Color, Z, cx1, cy1, cx2, cy2, mmask, mtag);
		return;
	}
	if (fds::g_hdrActive)
		Spriter<Res, true, true >(x, y, w, h, txtr, vpage, Color, Z, cx1, cy1, cx2, cy2);
	else
		Spriter<Res, true, false>(x, y, w, h, txtr, vpage, Color, Z, cx1, cy1, cx2, cy2);
}

// ── --mirror_flare_bbox: the clone-flare mirror gate ─────────────────────────
//
// A mirror-clone flare is a SPRITE with real screen extent (greets' lamps and
// fountain's `ImageSize 10.0` sprites are tens to hundreds of pixels across).
// The legacy gate sampled the mask at the sprite's CENTRE pixel and dropped the
// whole sprite on a mismatch, so a flare drifting toward a mirror's edge
// vanished in one frame while most of its disc was still over the mirror —
// "flares rendering in mirrors should be decided by their bbox, no center
// point". The replacement is two-part and strictly tighter than a plain widen:
//   * ADMISSION  — the sprite's screen bbox vs the mirror's stamped footprint
//     bbox (fds::MirrorMaskScreenBBox, built by StampMirrorMasks from the same
//     clipped wall polygons). O(1), and only a reject-early optimisation.
//   * THE DECISION — per pixel, in Spriter's MaskMirror lane test. This is what
//     keeps the original defects dead: a clone whose reflected world position
//     happens to pass a plain z-test inside ANOTHER mirror's footprint, or out
//     in the open room, still paints nothing, because those pixels do not carry
//     its tag. Widening the gate WITHOUT the per-pixel clip would resurrect
//     "omni shows twice in the mirror" / "flare through walls"; that is why the
//     two go in together.
// Returns the mask plane for a clone flare when the flag is on, else nullptr
// (which takes SpriterRT's original instantiation → byte-identical).
static inline const uint8_t* SpriteMirrorMask(const Face* F) {
	if (!F || F->mirrorMaskTag == 0) return nullptr;
	if (!fds::FeatureFlags::mirror_flare_bbox()) return nullptr;
	if (!g_gbuffer || g_gbuffer->mirrorMask.empty()) return nullptr;
	return g_gbuffer->mirrorMask.data();
}

// Admission test for a clone flare of half-extent `halfExt` centred at (px,py).
// True = "may reach this mirror's footprint"; the per-pixel clip decides the
// rest. Also the gate that keeps a mirror which did not stamp this frame
// (inactive, viewer behind the plane) from drawing its flares at all.
static inline bool SpriteTouchesMirror(const Face* F, float px, float py, float halfExt) {
	const fds::MirrorMaskBBox &bb = fds::MirrorMaskScreenBBox(F->mirrorMaskTag);
	if (!bb.valid) return false;
	return !(px + halfExt < float(bb.x0) || px - halfExt > float(bb.x1) ||
	         py + halfExt < float(bb.y0) || py - halfExt > float(bb.y1));
}

void TBR_Sprite(Face* F, Vertex **V, dword numVerts, dword miplevel,
                const fds::RenderTarget& rt, const fds::CameraContext& cam);

void The_MMX_Scalar(Face* F, Vertex **V, dword numVerts, dword miplevel,
                    const fds::RenderTarget& rt,
                    const fds::CameraContext& cam)
{
	// TBR scenes: flares JOIN the unified per-strip back-to-front order
	// (sprites + transparent clumps sorted together by renderZ) instead of
	// drawing immediately — otherwise the particle TBR and the xpar clumps
	// composite over the lamp glows (fountain vortex over the purple omni),
	// and an immediate draw can only be fully-before or fully-after them.
	// TBR_MatchesTarget: offscreen passes (mirror RTT) draw immediate.
	if ((CurScene->Flags & Scn_SpriteTBR) && TBR_MatchesTarget(CurScene)) {
		TBR_Sprite(F, V, numVerts, miplevel, rt, cam);
		return;
	}
//	int32_t Size = ImageSize*_A->RZ;
	float Size = ImageSize*_A->RZ*View->PerspX;

	// flare image size is written as a floating pt.
	Size *= F->FlareSize;

	// Mirror-clone flare: drawable only inside its OWN mirror's stamped
	// footprint. Depth alone is not enough — every mirror clones every
	// omni, so other mirrors' clones sit at arbitrary reflected world
	// positions and pass plain z tests inside OTHER mirrors' footprints
	// ("omni shows twice in the mirror") or in open rooms ("flare
	// through walls"). Center-point test, consistent with the sprite's
	// Face_PointZTest behavior.
	// --mirror_flare_bbox: bbox admission + a per-pixel clip in the blit
	// instead of the centre sample. See SpriteMirrorMask above.
	const uint8_t* const mflMask = SpriteMirrorMask(F);
	if (F->mirrorMaskTag != 0) {
		if (!g_gbuffer || g_gbuffer->mirrorMask.empty()) return;
		if (mflMask) {
			if (!SpriteTouchesMirror(F, _A->PX, _A->PY, 2.0f * Size)) return;
		} else {
			int32_t mx = int32_t(_A->PX);
			int32_t my = int32_t(_A->PY);
			if (mx < 0) mx = 0; else if (mx > XRes - 1) mx = XRes - 1;
			if (my < 0) my = 0; else if (my > YRes - 1) my = YRes - 1;
			if (g_gbuffer->mirrorMask[size_t(my) * size_t(XRes) + size_t(mx)]
			    != F->mirrorMaskTag) return;
		}
	}

	if (Size<=0) return;
	if (F->Txtr&&F->Txtr->Txtr)
	{
		int edgeLen;
		if (Size==0)
			edgeLen = 1;
		else
			edgeLen = 2.0*Size;

		// this size should undergo 2d clipping before being added.
#ifdef ENABLE_PIXELCOUNT
//		FDS_STATS_ADD_PIXELS(edgeLen*edgeLen);
#endif
		dword Col = 0xFFFFFF;
		if (CurScene->Flags & Scn_Fogged)
		{
			float FogRate = 1.0 - _A->TPos_AOS.z * C_rFZP;
			int32_t iFog = Fist(255.0 * FogRate);
			Col = iFog + (iFog<<8) +(iFog<<16);
		}
		// support no far clipping on omnilights
		dword Z;
		if (_A->TPos_AOS.z < C_FZP)
			Z = 0xFF80 - Fist(g_zscale * _A->TPos_AOS.z);
		else
			Z = 1;

		// Z compare on center only.
		// Skipped for a mirror-clone flare under --mirror_flare_bbox for the
		// same reason the centre MASK sample is: it is a whole-sprite verdict
		// read off one pixel, and at a mirror edge that pixel is the WALL
		// beside the mirror, which occludes and kills a sprite that is mostly
		// over the (farther) reflection. Spriter's per-pixel z test is exact
		// and already runs (EnableZTest=true), so nothing is lost but the
		// early-out. Non-clone flares are untouched.
		if ((F->Flags & Face_PointZTest) && !mflMask)
		{
			int32_t x = _A->PX;
			if (x<0) x=0;
			if (x>XRes-1) x=XRes-1;
			int32_t y = _A->PY;
			if (y<0) y=0;
			if (y>YRes-1) y=YRes-1;
			if (Z <= ZPage16[XRes*y+x]) return;
		}
		SpriterRT<256>(_A->PX, _A->PY, edgeLen, edgeLen, F->Txtr->Txtr->Data, VPage, Col, Z, 0, 0, XRes, YRes,
		               mflMask, F->mirrorMaskTag);
		//CSeven_UP_MMX(_A->PX,_A->PY,edgeLen,edgeLen,F->Txtr->Txtr->Data,VPage, Col, Z);
		//CSeven_UP(_A->PX,_A->PY,Size<<1,Size<<1,F->Txtr->Txtr->Data,VPage);
	}


}

#define PIG_VOMIT
//void The_MMX_Spriter_32(Vertex **V, dword numVerts)

 
void Spriter32(float x, float y,float width,float height,void *txtr,void *VPage, dword Color, float spriteClipY1, float spriteClipY2)
{
	float x1, y1, x2, y2;
	float du, dv;
	float u0 = 0 , v0 = 0;
	int32_t  iu, iv, idu, idv;
	int32_t  LineWidth;
	int32_t  Height;

	x1 = (x - width);
	x2 = (x + width);
	y1 = (y - height);
	y2 = (y + height);


	if (y2<=spriteClipY1 || y1>=spriteClipY2) return;

	du = 16.0 / width;
	dv = 16.0 / height;
	
	if (x1 < 0.0) 
	{
		u0 = du * (-x1);
		x1 = 0.0;
	}
	if (y1 < spriteClipY1)
	{
		v0 = dv * (spriteClipY1-y1);
		y1 = spriteClipY1;
	}

	if (x2 > XRes) x2 = XRes;
	if (y2 > spriteClipY2) y2 = spriteClipY2;

	int32_t ix1, ix2;
	ix1 = Fist(x1);
	ix2 = Fist(x2);
	LineWidth = ix2 - ix1;
	u0 += du * (ix1 - x1);

	int32_t iy1, iy2;
	iy1 = Fist(y1);
	iy2 = Fist(y2);
	Height = iy2 - iy1;
	v0 += dv * (iy1 - y1);

	//dword *ScanLine  = (dword *)((dword)VPage + VESA_BPSL * iy1) + ix1;
	uintptr_t ScanLine  = (uintptr_t)VPage + VESA_BPSL * iy1 + ix1*4;

	iu = Fist(u0 * 65536.0);
	iv = Fist(v0 * 65536.0);
	idu = Fist(du * 65536.0);
	idv = Fist(dv * 65536.0);
	
	dword *tex = (dword *)txtr;
	dword v = iv;

	if (Height <= 0 || LineWidth <= 0) return;
	FDS_STATS_ADD_PIXELS(Height * LineWidth);
	__m64 mm4 { (int32_t)Color };
	__m64 mm1 = {};
	
	for(int i=0; i<Height; i++)
	{

		dword u = iu;
		uintptr_t ofs = ((v >> 11) & 0x3E0);
		for(int j = 0;j<LineWidth; j++)
		{
			__m64 mm0 = { 0 };
			// __m64 is a GCC-style vector type on clang, clang-cl AND gcc
			// (subscriptable); `.m64_u32` is the MSVC union spelling and
			// exists only there. __GNUC__ is the exact discriminator —
			// clang defines it, real MSVC does not.
#ifdef __GNUC__
			((dword*)ScanLine)[j] = (dword)mm0[0];
#else
			((dword*)ScanLine)[j] = mm0.m64_u32[0];
#endif

			u += idu;
		}
		ScanLine += VESA_BPSL;
		v += idv;
	}
}

#if 0
void The_MMX_Scalar_32(Vertex **V, dword numVerts)
{
	float Size = ImageSize*_A->RZ*View->PerspX;

	// flare image size is written as a floating pt.
	Size *= DoFace->FlareSize;

	if (Size<=0) return;
	if (DoFace->Txtr&&DoFace->Txtr->Txtr)
	{
		float edgeLen=2.0*Size;

		// get color from vertex _A
		dword R,G,B;
		R = _A->LR;
		G = _A->LG;
		B = _A->LB;
		dword Color = (R<<16)+(G<<8)+B;
		dword Z = 0xFF80 - Fist(g_zscale * _A->TPos_AOS.z);
		// Z compare on center only.
		int32_t x = _A->PX;
		if (x<0) x=0;
		if (x>XRes-1) x=XRes-1;
		int32_t y = _A->PY;
		if (y<0) y=0;
		if (y>YRes-1) y=YRes-1;
		if (Z > ZPage16[XRes*y+x])
			CSeven_UP_MMX_32(_A->PX,_A->PY,edgeLen,edgeLen,DoFace->Txtr->Txtr->Data,VPage, Color, Z);
		//Spriter32(_A->PX,_A->PY,edgeLen,edgeLen,DoFace->Txtr->Txtr->Data,VPage, Color, Z);
		//CSeven_UP(_A->PX,_A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
	}
}


void The_ZScale_Shit(Vertex **V, dword numVerts)
{
  int32_t Size = ImageSize*_A->RZ;
  CZScale_thatfucksBTB(_A->PX,_A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
}
#endif


void Particle_Sprite(Vertex **V, dword numVerts)
{
	int32_t Cx,Cy;
	int32_t X1,Y1,X2,Y2;
	Cx = _A->PX; Cy = _A->PY;
	int32_t K;
	//  if (BPP!=8) return;
	if (Cx>=0&&Cx<=XRes-2&&Cy>=0&&Cy<=YRes-2)
		VPage[(Cx+YOffs[Cy])*((BPP+1)>>3)]=0xFF;
}


////////////////////////////////////////
// Tile-based rendering
#define TILELOG 3
#define TILESIZE (1 << (TILELOG))
#define rTILESIZE 1.0/(float(TILESIZE))

void TBR_Init(Scene *Sc, mword size)
{
	Sc->SBuffer = new TBREntry [size];
	Sc->SBufferSize = size;
	Sc->SBufferCur = 0;

	mword numTiles = (YRes+TILESIZE-1)>>TILELOG;
	Sc->NumTiles = numTiles;
	Sc->SBufferHead = new sdword [numTiles];
	for(mword i=0; i<numTiles; i++)
		Sc->SBufferHead[i] = -1;
}

// (Re)initialize the scene's TBR when it's missing or sized for a stale
// YRes (mid-scene resize reallocates the framebuffer but scenes without
// an explicit on_resize never re-ran TBR_Init — inserts past NumTiles
// would corrupt). Cheap no-op when current; safe to call every tick.
// entriesPerStrip sizes the span pool; InsertSpanToTBR drops overflow
// gracefully, so this is a budget, not a hard bound.
void TBR_EnsureInit(Scene *Sc, mword entriesPerStrip)
{
	const mword numTiles = (YRes+TILESIZE-1)>>TILELOG;
	if (Sc->SBufferHead && Sc->NumTiles == numTiles) return;
	delete[] Sc->SBuffer;
	delete[] Sc->SBufferHead;
	TBR_Init(Sc, numTiles * entriesPerStrip);
}

// True when the scene's TBR strips match the CURRENT render target.
// Every TBR consumer (sprite divert, xpar routing, TBR_Render) must
// check this, not just Scn_SpriteTBR: an offscreen pass (mirror RTT
// via OffscreenViewScope, cube bakes) swaps the YRes global while
// CurScene keeps its main-sized strip arrays — inserting there would
// index strips with offscreen-space Y coords, and TBR_Render would
// draw main-frame entries onto the small target. Offscreen passes
// fall back to immediate sprites / the legacy peel.
bool TBR_MatchesTarget(const Scene *Sc)
{
	return Sc->SBufferHead != nullptr &&
	       Sc->NumTiles == mword((YRes+TILESIZE-1)>>TILELOG);
}

void InsertSpanToTBR(Face *F, dword tile, float renderZ, uint8_t rank)
{
	// SBufferSize is sized from typicalSpans = YRes * 30/1536, an estimate
	// of the *average* tile-coverage per particle. Particles near the
	// camera with large FlareSize can easily cover 5-10× that, so once we
	// hit the cap we drop excess spans rather than write past the buffer
	// end (heap corruption used to surface as an SBuffer[n].next bogus
	// index, then OOB in TBR_Render's lambda chasing it).
	if (CurScene->SBufferCur >= CurScene->SBufferSize) return;
	sdword h = CurScene->SBufferHead[tile];
	mword  n = CurScene->SBufferCur;
	CurScene->SBuffer[n].F = F;
	CurScene->SBuffer[n].next = h;
	CurScene->SBuffer[n].renderZ = renderZ;
	CurScene->SBuffer[n].rank = rank;
	CurScene->SBufferHead[tile] = n;
	CurScene->SBufferCur++;
}


void InsertSpriteToTBR(Face *F, float y, float Height)
{
	int y1, y2;
	// Read fresh every call — `static` here used to cache the value, but
	// rebuildSizedBuffers (engine resize) reallocates SBufferHead at a
	// different size. A stale cached numTiles would let us index past the
	// new (smaller) SBufferHead end on HD→SD transitions.
	int32_t numTiles = CurScene->NumTiles;

	y1 = Fist(((y - Height) * rTILESIZE) - 0.999999);
	y2 = Fist((y + Height) * rTILESIZE);

	if (y1 >= numTiles || y2 < 0)
		return;

	if (y1 < 0) y1=0;
	if (y2 >= numTiles) y2 = numTiles-1;

	// Sprite renderZ: the particle's view-space Z. All strips this sprite
	// inserts into get the same renderZ.
	// NOTE: kept on the AoS path (F->A->TPos_AOS.z) deliberately. Particle
	// Faces (Sc->Pcl[I].F) live outside any TriMesh, so they don't get
	// the F->frame stamping that Compute_FaceVertexIndices /
	// Transform_Objects do for mesh faces — F->frame is null here.
	// SoA migration of this single read would gain nothing and would
	// need a per-particle VertexFrame, which isn't worth the bookkeeping.
	const float renderZ = F->A->TPos_AOS.z;
	for (int i = y1; i <= y2; i++)
	{
		InsertSpanToTBR(F, i, renderZ, /*rank=*/1);   // sprite
	}
}

// Deterministic, camera-INDEPENDENT ordering for the per-strip transparent
// sort (TBR_Render and the glass-layered scheduler). The primary key is
// renderZ descending — farthest-first, back-to-front — exactly as before, so
// every pair with DISTINCT renderZ keeps its former order (byte-identical for
// well-separated transparents: city beacons, fountain orbs, greets panels away
// from coplanarity). The addition is the TIE-BREAK: when two fragments land at
// the SAME renderZ (near-coplanar surfaces — a thin glass panel's front/back
// faces, stacked decals), std::stable_sort otherwise falls back to the FList
// insertion order, which is itself a function of the view (the Transform_Objects
// radix key), so a coplanar front/back pair could swap composite order under a
// sub-unit camera move. Instead we impose a fixed total order on the tie: back
// surface before front (preserving the peel's far→near layering, matching the
// max-Z / min-Z renderZ convention), then MeshFaceIdx — both camera-independent,
// so a coplanar tie composites identically in every frame. The rank helper
// derives facing from the same view-space winding the rest of the peel uses.
static inline bool tbrXparOrderLess(const TBREntry &a, const TBREntry &b)
{
	if (a.renderZ != b.renderZ) return a.renderZ > b.renderZ;   // far → near
	// Tie-break on the PRECOMPUTED entry rank (back 0 < sprite 1 < front 2),
	// then MeshFaceIdx (load-time constant). No live TPos/frame reads here:
	// the comparator runs inside per-strip sorts that overlap OTHER strips'
	// raster/composite work — re-deriving facing from shared vertex data
	// mid-walk was nondeterministic (greets mirror screens flashed pink,
	// ~1-in-8 frames at peel>=2; see TBREntry::rank).
	if (a.rank != b.rank) return a.rank < b.rank;
	return a.F->MeshFaceIdx < b.F->MeshFaceIdx;
}

// Insert a transparent face fragment into every Y-strip its screen
// bounding box overlaps. Each strip's linked list (Sc->SBufferHead[i])
// is the unified back-to-front list TBR_Render walks. Entry renderZ
// is the tri's far surface for back-facing (max vertex Z) and the near
// surface for front-facing (min vertex Z) — sorted descending at flush
// time, this naturally interleaves with sprites whose renderZ is the
// particle's view-space Z, producing the correct per-strip back-to-
// front order across both types.
void InsertTransparentToTBR(Face *F)
{
	// SoA Phase 4: all transformed-vertex reads via F->frame's SoA
	// arrays. Same indices as the original Vertex* loads.
	const VertexFrame *ff  = F->frame;
	const float *PY        = ff->PY;
	const float *tpos_x    = ff->TPos_x;
	const float *tpos_y    = ff->TPos_y;
	const float *tpos_z    = ff->TPos_z;
	const uint32_t ai = F->A_idx, bi = F->B_idx, ci = F->C_idx;

	// A vertex at/behind the near plane never went through projection, so
	// its PY is stale/meaningless. A camera-straddling face (chase's giant
	// un-tessellated water quad is the canonical case) would compute its
	// strip span from that garbage and land in the wrong strips — the water
	// inserted only ABOVE the horizon and vanished from the deferred frame.
	// Span from PY is only trustworthy when all three verts project;
	// otherwise insert into every strip and let the per-strip clipper
	// raster trim exactly. (soa-vertex 9902349 — the chase water dark band.)
	const float tzA = tpos_z[ai], tzB = tpos_z[bi], tzC = tpos_z[ci];
	const bool straddlesNear = (tzA < 1.0f) || (tzB < 1.0f) || (tzC < 1.0f);

	int32_t numTiles = CurScene->NumTiles;
	int s_lo, s_hi;
	if (straddlesNear) {
		s_lo = 0;
		s_hi = numTiles - 1;
	} else {
		// Screen-Y bounds from projected vertex coords.
		const float pyA = PY[ai], pyB = PY[bi], pyC = PY[ci];
		float y_min = pyA;
		float y_max = pyA;
		if (pyB < y_min) y_min = pyB;
		if (pyB > y_max) y_max = pyB;
		if (pyC < y_min) y_min = pyC;
		if (pyC > y_max) y_max = pyC;

		if (y_min >= YRes || y_max < 0) return;

		s_lo = Fist(y_min * rTILESIZE - 0.999999f);
		s_hi = Fist(y_max * rTILESIZE);
		if (s_lo < 0) s_lo = 0;
		if (s_hi >= numTiles) s_hi = numTiles - 1;
		if (s_lo > s_hi) return;
	}

	// Per-face front-facing test (matches the rest of the xpar pipeline).
	const float Ax = tpos_x[ai], Ay = tpos_y[ai], Az = tpos_z[ai];
	const float Bx = tpos_x[bi], By = tpos_y[bi], Bz = tpos_z[bi];
	const float Cx = tpos_x[ci], Cy = tpos_y[ci], Cz = tpos_z[ci];
	const float ex = Bx - Ax, ey = By - Ay, ez = Bz - Az;
	const float fx = Cx - Ax, fy = Cy - Ay, fz = Cz - Az;
	const float nx = ey * fz - ez * fy;
	const float ny = ez * fx - ex * fz;
	const float nz = ex * fy - ey * fx;
	const float vd = nx * Ax + ny * Ay + nz * Az;
	const bool front = vd < 0.0f;

	float renderZ;
	if (straddlesNear) {
		// A camera-straddling face spans the whole depth range, so a
		// painter's-order key from its near surface (negative/near-0 Z)
		// would sort it on top of every sprite in the strip — chase's
		// water would composite over the explosion flares forward draws
		// on top of it. Use the far surface: the face renders first and
		// sprites/flares layer over it, matching the forward Z-sort.
		renderZ = Az;
		if (Bz > renderZ) renderZ = Bz;
		if (Cz > renderZ) renderZ = Cz;
	} else if (front) {
		// Near surface — min vertex Z. Sorts later (= rendered LATER
		// = on top of farther stuff).
		renderZ = Az;
		if (Bz < renderZ) renderZ = Bz;
		if (Cz < renderZ) renderZ = Cz;
	} else {
		// Far surface — max vertex Z. Sorts earlier (= rendered FIRST
		// = the back-most surface in the per-strip back-to-front walk).
		renderZ = Az;
		if (Bz > renderZ) renderZ = Bz;
		if (Cz > renderZ) renderZ = Cz;
	}

	for (int s = s_lo; s <= s_hi; s++) {
		InsertSpanToTBR(F, s, renderZ, front ? uint8_t(2) : uint8_t(0));
	}
}

void TBR_Sprite(Face* F, Vertex **V, dword numVerts, dword miplevel,
                const fds::RenderTarget& /*rt*/,
                const fds::CameraContext& /*cam*/)
{
	float Size = ImageSize*_A->RZ*View->PerspX;

	// flare image size is written as a floating pt.
	Size *= F->FlareSize;

	// Mirror-clone flare: drawable only inside its OWN mirror's stamped
	// footprint (center-point test — same gate as The_MMX_Scalar's
	// immediate path; see the comment there for why depth alone isn't
	// enough). The mask is stamped before the FList sprite loop that
	// inserts here, so it's valid at insert time. Without this, a TBR
	// scene with mirrors (greets) re-grows the "clone flare through
	// walls / in other mirrors" family the immediate path already fixed.
	// --mirror_flare_bbox: admission by the sprite's screen bbox instead of
	// its centre pixel; the per-pixel clip happens at blit time in TBR_Render
	// / drawSprite (which re-derive the mask from the Face).
	if (F->mirrorMaskTag != 0) {
		if (!g_gbuffer || g_gbuffer->mirrorMask.empty()) return;
		if (SpriteMirrorMask(F)) {
			if (!SpriteTouchesMirror(F, _A->PX, _A->PY, 2.0f * Size)) return;
		} else {
			int32_t mx = int32_t(_A->PX);
			int32_t my = int32_t(_A->PY);
			if (mx < 0) mx = 0; else if (mx > XRes - 1) mx = XRes - 1;
			if (my < 0) my = 0; else if (my > YRes - 1) my = YRes - 1;
			if (g_gbuffer->mirrorMask[size_t(my) * size_t(XRes) + size_t(mx)]
			    != F->mirrorMaskTag) return;
		}
	}

	if (Size<=0) return;
	if (F->Txtr&&F->Txtr->Txtr)
	{
		float edgeLen=2.0*Size;

		// X-clipping early-out: reject sprites that don't cross the
		// screen at all. Per-pixel Z occlusion happens later in TBR_Render
		// (Spriter<true>) — we don't want a coarse per-CENTER Z test
		// here because it incorrectly rejects partially-occluded sprites
		// (e.g. fountain particles behind near building edges).
		if (_A->PX + edgeLen > 0 && _A->PX - edgeLen < XRes) {
			InsertSpriteToTBR(F, _A->PY, edgeLen);
		}
	}
}

// TBR barrier primitives used by the glass scheduler's drain() (see
// TBR_Render_GlassLayered). fog-wt's default TBR_Render uses a local
// counting_semaphore instead, so these are only referenced by the glass path.
namespace tbr {
	std::mutex					tileCounterMutex;
	std::atomic<int>			tileCounter;
	std::condition_variable		condition;
}

// ── Barrier-per-layer glass scheduler (--glass_refract) ──────────────────────
//
// The default per-strip TBR walk composites each strip's clumps back-to-front
// on its OWN worker with NO cross-strip coordination. Glass refraction there had
// to snapshot only the strip's own row band and CLAMP the vertical refracted
// fetch to that band (else it would read a neighbour strip mid-composite) → an
// 8-px vertical stair-step on every refracted surface.
//
// Glass is a WHOLE-SCREEN effect: a refracted fetch offsets up to glass_refract_max
// pixels in ANY direction, so it needs a coherent full-frame background. This
// scheduler hoists the per-strip layer sequencing to a GLOBAL barrier per depth
// band (same model the legacy per-mesh peel uses — tiled-parallel within a layer,
// sequenced + snapshotted between layers):
//
//   1. PLAN  (parallel per strip): collect + sort the strip's items back-to-front
//      and segment them into steps (a sprite, or a clump of consecutive
//      same-(mesh,front) transparent faces — identical to the legacy walk). Glass
//      steps (refracting transparents) contribute their depth to the band set.
//   2. BAND thresholds (main thread): the distinct glass depths (deduped within an
//      epsilon, capped) become the barrier depths T[0] > T[1] > …
//   3. EXECUTE rounds r = 0..m: every strip composites, IN PARALLEL, all its steps
//      with renderZ > T[r] (whole steps, never split — a clump's band key is its
//      farthest face's renderZ). BARRIER (all strips done) → full-frame snapshot of
//      the now-coherent g_hdrBuf/VPage into g_glassRefr{Hdr,Ldr} → next round.
//
// Glass in round r composites AFTER the snapshot taken at the end of round r-1,
// which holds everything with renderZ > T[r-1] = everything strictly behind it
// (opaque + fog + every farther transparent/glass layer). It refracts that
// full-frame snapshot (glassBandOn stays false → the kernel sampler reads
// g_glassRefr*), so the fetch is whole-screen coherent (no per-strip clamp → NO
// stair-step) AND correctly layered (inner glass shows through outer). Non-glass
// steps (sprites / water / mirror clones) compose in their exact back-to-front
// slot, so their output is byte-identical to the plain walk; only glass pixels
// change. The snapshot is a main-thread memcpy taken while all strips are drained
// → race-free and frame-to-frame deterministic (no flicker).
//
// FUTURE perf lever (NOT implemented): a per-strip max-displacement cap so strips
// whose glass can't reach a neighbour's band skip that barrier, and merging
// screen-disjoint glass layers so non-overlapping objects share one snapshot.
namespace tbrglass {

struct XparStep {
	bool               isSprite = false;
	Face*              spriteF  = nullptr;   // sprite path
	std::vector<Face*> faces;                // clump path
	bool               front    = false;
	bool               isGlass  = false;     // refracting transparent → band depth
	float              renderZ  = 0.0f;      // band key = farthest entry's renderZ
};
struct XparPlan {
	std::vector<XparStep> steps;
	size_t                cursor = 0;
	size_t                roundStart = 0;    // first step index of the current round
	int                   strip_y = 0, strip_h = 0;
	float                 clipY1 = 0.0f, clipY2 = 0.0f;
};
static std::vector<XparPlan> g_plans;

// Full-frame snapshot of the accumulated background into the stable buffers the
// refraction sampler reads. Main thread, between round barriers → race-free.
static void glassSnapshotFull() {
	const size_t np = size_t(XRes) * size_t(YRes), n4 = np * 4;
	if (fds::g_hdrActive && fds::g_hdrBuf.size() >= n4) {
		if (fds::g_glassRefrHdr.size() < n4) fds::g_glassRefrHdr.resize(n4);
		memcpy(fds::g_glassRefrHdr.data(), fds::g_hdrBuf.data(), n4 * sizeof(fds::hdrf));
	}
	if (VPage) {
		if (fds::g_glassRefrLdr.size() < np) fds::g_glassRefrLdr.resize(np);
		memcpy(fds::g_glassRefrLdr.data(), VPage, np * 4);
	}
}

static void drawSprite(Face* F, float clipY1, float clipY2) {
	Vertex* V = F->A;
	float edgeLen = ImageSize * V->RZ * View->PerspX * F->FlareSize * 2.0f;
	dword Z = 0xFF80 - Fist(g_zscale * V->TPos_AOS.z);
	// nullptr for everything but a mirror-clone flare under --mirror_flare_bbox.
	const uint8_t* const mm = SpriteMirrorMask(F);
	if (F->Txtr->Txtr->SizeX == 256) {
		SpriterRT<256>(V->PX, V->PY, edgeLen, edgeLen, F->Txtr->Txtr->Data, VPage,
		               0xFFFFFF, Z, 0, clipY1, XRes, clipY2, mm, F->mirrorMaskTag);
	} else {
		dword R = V->LR, G = V->LG, B = V->LB;
		dword Color = (R << 16) + (G << 8) + B;
		SpriterRT<32>(V->PX, V->PY, edgeLen, edgeLen, F->Txtr->Txtr->Data, VPage,
		              Color, Z, 0, clipY1, XRes, clipY2, mm, F->mirrorMaskTag);
	}
}

// dctx is threaded from TBR_Render (fog-wt's TBR refactor passes a
// DeferredLightingCtx to the strip xpar dispatcher). It may be null on a
// non-deferred TBR frame, in which case no transparent clumps were routed —
// only sprites compose.
static inline void composeStep(const DeferredLightingCtx *dctx, XparPlan& p, XparStep& s) {
	if (s.isSprite) drawSprite(s.spriteF, p.clipY1, p.clipY2);
	else if (dctx) RenderXparClumpInStrip(*dctx, s.faces.data(), int(s.faces.size()), s.front,
	                            p.strip_y, p.strip_h, /*bandSnapshot=*/false);
}

} // namespace tbrglass

static void TBR_Render_GlassLayered(Scene *Sc, const DeferredLightingCtx *dctx)
{
	extern void Xtrace_FrameBegin();   // FDS_XPAR_TRACE dev diagnostic
	Xtrace_FrameBegin();
	// Whatever the previous frame / the legacy peel left in the xpar layer
	// slices is unknown to the per-strip column tracker — declare it all dirty
	// so the first clump in each strip clears the full width.
	XparStripSlices_MarkAllDirty();
	using namespace tbrglass;
	const dword numTiles = Sc->NumTiles;
	if (g_plans.size() < numTiles) g_plans.resize(numTiles);

	auto drain = [&](dword n) {
		std::unique_lock<std::mutex> lk(tbr::tileCounterMutex);
		tbr::condition.wait(lk, [n] { return tbr::tileCounter == (int)n; });
	};

	// ── Phase 1: plan (parallel per strip) ──
	tbr::tileCounter = 0;
	for (mword i = 0; i < numTiles; i++) {
		ThreadPool::instance().enqueue([i, Sc]() {
			XparPlan& plan = g_plans[i];
			plan.steps.clear();
			plan.cursor = 0;
			float clipY1 = i << TILELOG, clipY2 = (i + 1) << TILELOG;
			if (clipY2 > YRes) clipY2 = YRes;
			plan.clipY1 = clipY1; plan.clipY2 = clipY2;
			plan.strip_y = int(i) << TILELOG;
			plan.strip_h = int(clipY2) - plan.strip_y;

			thread_local std::vector<TBREntry> scratch;
			scratch.clear();
			sdword n = Sc->SBufferHead[i];
			while (n != -1) { scratch.push_back(Sc->SBuffer[n]); n = Sc->SBuffer[n].next; }
			std::stable_sort(scratch.begin(), scratch.end(), tbrXparOrderLess);

			TriMesh* clumpMesh = nullptr; bool clumpFront = false; int openIdx = -1;
			for (const TBREntry& e : scratch) {
				Face* F = e.F;
				if (F->A == F->B) {                 // sprite: closes any open clump
					openIdx = -1;
					XparStep s; s.isSprite = true; s.spriteF = F; s.renderZ = e.renderZ;
					plan.steps.push_back(std::move(s));
					continue;
				}
				TriMesh* meshHere = F->ParentTri;
				const bool frontHere = (e.rank == 2);   // precomputed at insert (see TBREntry::rank)
				if (openIdx >= 0 && (meshHere != clumpMesh || frontHere != clumpFront))
					openIdx = -1;
				if (openIdx < 0) {
					clumpMesh = meshHere; clumpFront = frontHere;
					XparStep s; s.isSprite = false; s.front = frontHere; s.renderZ = e.renderZ;
					// Refracting glass = a transparent OPT-IN-marked Mat_Refractive
					// that is NOT a mirror clone (mirrorMaskTag). Only marked glass
					// (fountain 'mizraka glass' + 'f_sphere' orb shells, greets
					// 'amudim' panels) becomes a band step that adds a snapshot
					// barrier; every other transparent (the fiery 'f in shpere' orb
					// core, additive glows, mirror-mask walls) keeps its exact
					// composite slot and no barrier — it blends/glows normally,
					// layered by the peel. F->Txtr is the same Material* the kernel
					// reads as Mat, so the Mat_Refractive bit matches there.
				s.isGlass = (F->Txtr && F->mirrorMaskTag == 0 &&
					             (F->Txtr->Flags & Mat_Refractive) != 0);
					plan.steps.push_back(std::move(s));
					openIdx = int(plan.steps.size()) - 1;
				}
				plan.steps[openIdx].faces.push_back(F);
			}
			Sc->SBufferHead[i] = -1;
			std::unique_lock<std::mutex> lk(tbr::tileCounterMutex);
			++tbr::tileCounter; tbr::condition.notify_one();
		});
	}
	drain(numTiles);

	// ── Phase 2: gather glass depths → dedupe/cap → barrier thresholds ──
	static std::vector<float> depths; depths.clear();
	for (mword i = 0; i < numTiles; i++)
		for (auto& s : g_plans[i].steps)
			if (s.isGlass) depths.push_back(s.renderZ);
	static std::vector<float> T; T.clear();
	if (!depths.empty()) {
		std::sort(depths.begin(), depths.end(), [](float a, float b) { return a > b; });
		const int maxBands = 16;
		float eps = 0.5f;
		const float range = depths.front() - depths.back();
		if (range > 0.0f) eps = std::max(eps, range / float(maxBands));
		for (float z : depths) if (T.empty() || (T.back() - z) > eps) T.push_back(z);
		if ((int)T.size() > maxBands) {   // safety coarsen (rare)
			std::vector<float> T2;
			const int stride = ((int)T.size() + maxBands - 1) / maxBands;
			for (size_t k = 0; k < T.size(); k += stride) T2.push_back(T[k]);
			T.swap(T2);
		}
	}

	// ── Phase 3: execute rounds, TWO-PHASE within each round ──
	// Within a round, composite NON-GLASS steps (opaque-behind transparents:
	// additive glow, plain blends) FIRST, snapshot the now-coherent frame, THEN
	// composite the round's GLASS steps so each refracts a snapshot that already
	// contains the same-round transparents behind it. Without this, a glass shell
	// refracts a snapshot taken before the concentric/co-equatorial additive
	// near-hemisphere composited, so the glass OVERWRITES that glow with the
	// refracted opaque box → the additive glow over the box vanishes through the
	// glass. (Works now that the refractive flag classifies the additive sphere
	// as NON-glass; when it was wrongly a glass step this split was a no-op.)
	const size_t m = T.size();
	for (size_t r = 0; r <= m; r++) {
		const float low = (r < m) ? T[r] : -3.0e38f;
		// Phase A: this round's NON-glass steps.
		tbr::tileCounter = 0;
		for (mword i = 0; i < numTiles; i++) {
			ThreadPool::instance().enqueue([i, low, dctx]() {
				XparPlan& plan = g_plans[i];
				plan.roundStart = plan.cursor;
				while (plan.cursor < plan.steps.size() &&
				       plan.steps[plan.cursor].renderZ > low) {
					if (!plan.steps[plan.cursor].isGlass)
						composeStep(dctx, plan, plan.steps[plan.cursor]);
					plan.cursor++;
				}
				std::unique_lock<std::mutex> lk(tbr::tileCounterMutex);
				++tbr::tileCounter; tbr::condition.notify_one();
			});
		}
		drain(numTiles);
		// Coherent background (opaque + fog + all farther rounds + this round's
		// non-glass) for the round's glass to refract.
		glassSnapshotFull();
		// Phase B1: this round's BACK-facing glass, refracting the fresh snapshot.
		// Split by side: a thin glass panel's front and back faces routinely land
		// in the SAME band round (their clump depths dedupe within the band eps —
		// which varies with camera range, so whether they split used to FLIP with
		// sub-unit camera moves). With a single Phase B both faces refracted the
		// pre-round snapshot, so the FRONT face's refracted background was missing
		// its own BACK face (the panel's tint + rear text vanished — the greets
		// screen "face pop"). Composite back glass, re-snapshot, THEN front glass:
		// the front face always refracts a background containing its back face,
		// regardless of how the bands fell.
		tbr::tileCounter = 0;
		for (mword i = 0; i < numTiles; i++) {
			ThreadPool::instance().enqueue([i, dctx]() {
				XparPlan& plan = g_plans[i];
				for (size_t c = plan.roundStart; c < plan.cursor; c++)
					if (plan.steps[c].isGlass && !plan.steps[c].front)
						composeStep(dctx, plan, plan.steps[c]);
				std::unique_lock<std::mutex> lk(tbr::tileCounterMutex);
				++tbr::tileCounter; tbr::condition.notify_one();
			});
		}
		drain(numTiles);
		// Coherent background now includes the round's back glass.
		glassSnapshotFull();
		// Phase B2: this round's FRONT-facing glass.
		tbr::tileCounter = 0;
		for (mword i = 0; i < numTiles; i++) {
			ThreadPool::instance().enqueue([i, dctx]() {
				XparPlan& plan = g_plans[i];
				for (size_t c = plan.roundStart; c < plan.cursor; c++)
					if (plan.steps[c].isGlass && plan.steps[c].front)
						composeStep(dctx, plan, plan.steps[c]);
				std::unique_lock<std::mutex> lk(tbr::tileCounterMutex);
				++tbr::tileCounter; tbr::condition.notify_one();
			});
		}
		drain(numTiles);
	}

	Sc->SBufferCur = 0;
}

// Execute/Clear sprite buffer
//
// Per-strip walk: each Y-strip (8 rows) is processed by one worker
// thread (work-stealing dispatchIndexed — a task per strip cost one
// enqueue mutex+notify per strip). The linked list at SBufferHead[i]
// holds all sprite entries that overlap this strip (and, once
// unified, transparent face fragments too). At flush time we collect
// entries into a scratch array, sort descending by renderZ
// (back-to-front), and walk the sorted array dispatching each item.
// FDS_TBR_STATS=1: per-call wall vs summed worker-busy time + strip load
// stats. The TailProf "serial TBR-render" scope is the orchestrator WALL
// around this parallel dispatch (and sums both city passes per frame) —
// these counters say how much actual work hides behind that wall and how
// well it balances (busy/wall = effective parallelism; maxStrip = the
// critical path).
static const bool g_tbrStats = std::getenv("FDS_TBR_STATS") != nullptr;
static std::atomic<uint64_t> g_tbrBusyNs{0}, g_tbrMaxStripNs{0};
static std::atomic<uint64_t> g_tbrEntries{0}, g_tbrFlushes{0};
// Per-material entry census (pointer-keyed open table; names printed).
static std::atomic<const char*> g_tbrMatName[16];
static std::atomic<uint64_t>    g_tbrMatCount[16];
static inline void tbrCensus(const char* name)
{
	if (!name) name = "<null>";
	for (int s = 0; s < 16; ++s) {
		const char* cur = g_tbrMatName[s].load(std::memory_order_relaxed);
		if (cur == name) { g_tbrMatCount[s].fetch_add(1, std::memory_order_relaxed); return; }
		if (!cur) {
			const char* expect = nullptr;
			if (g_tbrMatName[s].compare_exchange_strong(expect, name)) {
				g_tbrMatCount[s].fetch_add(1, std::memory_order_relaxed); return;
			}
			if (expect == name) { g_tbrMatCount[s].fetch_add(1, std::memory_order_relaxed); return; }
		}
	}
}

void TBR_Render(Scene *Sc, const DeferredLightingCtx *dctx)
{
	// Glass refraction on → route through the barrier-per-layer scheduler for
	// whole-screen-coherent (non-blocky) layered glass. Flag off → the plain
	// per-strip walk below runs unchanged (byte-identical).
	if (fds::FeatureFlags::glass_refract() > 0.0f) {
		TBR_Render_GlassLayered(Sc, dctx);
		XparExtentCensus_Report();
		return;
	}

	extern void Xtrace_FrameBegin();   // FDS_XPAR_TRACE dev diagnostic
	Xtrace_FrameBegin();
	XparStripSlices_MarkAllDirty();    // see the same call in TBR_Render_GlassLayered

	const int numTiles = int(Sc->NumTiles);
	std::counting_semaphore<INT_MAX> stripsDone{0};

	const auto tbrWall0 = std::chrono::steady_clock::now();
	if (g_tbrStats) {
		g_tbrBusyNs.store(0, std::memory_order_relaxed);
		g_tbrMaxStripNs.store(0, std::memory_order_relaxed);
		g_tbrEntries.store(0, std::memory_order_relaxed);
		g_tbrFlushes.store(0, std::memory_order_relaxed);
	}

	dispatchIndexed(numTiles, &stripsDone, [Sc, dctx](int ii) {
			const auto tStrip0 = g_tbrStats
				? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point{};
			const mword i = mword(ii);
			float spriteClipY1 = i << TILELOG;
			float spriteClipY2 = (i + 1) << TILELOG;
			if (spriteClipY2 > YRes) spriteClipY2 = YRes;

			// Collect the strip's linked-list entries into a scratch
			// array so we can sort them. Each entry holds Face* + renderZ
			// + next pointer (set by InsertSpanToTBR at insertion time).
			thread_local std::vector<TBREntry> scratch;
			scratch.clear();
			sdword n = Sc->SBufferHead[i];
			while (n != -1) {
				scratch.push_back(Sc->SBuffer[n]);
				n = Sc->SBuffer[n].next;
			}
			// Descending: largest renderZ (farthest) first, then the
			// deterministic camera-independent tie-break (see tbrXparOrderLess).
			std::stable_sort(scratch.begin(), scratch.end(), tbrXparOrderLess);

			// Walk the sorted strip items in back-to-front order. Two
			// item types share the list:
			//   sprite (F->A == F->B): rendered directly via
			//                          Spriter<32, true> (per-pixel Z).
			//   transparent: accumulated into a clump of consecutive
			//                same-(ParentTri, frontFacing) entries.
			//                On a bucket change (or end-of-strip), the
			//                clump flushes via RenderXparClumpInStrip
			//                which clears the strip's xpar layer slice,
			//                rasters the clump's faces, and composites
			//                that layer onto VPage strip rows.
			thread_local std::vector<Face*> xparClump;
			xparClump.clear();
			TriMesh* clumpMesh   = nullptr;
			bool     clumpFront  = false;
			const int strip_y_i  = int(i) << TILELOG;
			const int strip_h    = int(spriteClipY2) - strip_y_i;

			auto flushXparClump = [&]() {
				if (xparClump.empty()) return;
				if (g_tbrStats) g_tbrFlushes.fetch_add(1, std::memory_order_relaxed);
				// dctx is null only on a non-deferred TBR frame (forward
				// fountain fallback) — no transparent faces were routed
				// into the strips there, so the clump is always empty.
				if (dctx) {
					RenderXparClumpInStrip(*dctx, xparClump.data(),
					                       int(xparClump.size()),
					                       clumpFront, strip_y_i, strip_h);
				}
				xparClump.clear();
			};

			for (const TBREntry& e : scratch) {
				Face*   F = e.F;
				const bool isSprite = (F->A == F->B);
				if (g_tbrStats)
					tbrCensus(isSprite ? "<sprite>"
					          : (F->Txtr ? F->Txtr->Name : nullptr));

				if (isSprite) {
					// Flush any open transparent clump first so it
					// composites BEFORE this (closer) sprite.
					flushXparClump();

					Vertex* V = F->A;
					float edgeLen = ImageSize * V->RZ * View->PerspX * F->FlareSize * 2.0f;
					// Far-plane clamp matches The_MMX_Scalar's immediate path
					// ("support no far clipping on omnilights"): beyond FZP the
					// raw encoding wraps negative and the sprite would WIN every
					// per-pixel z-test instead of losing them (city's distant
					// beacons drew over their towers).
					dword Z;
					if (V->TPos_AOS.z < C_FZP)
						Z = 0xFF80 - Fist(g_zscale * V->TPos_AOS.z);
					else
						Z = 1;
					// Texture-res dispatch: particles are 32×32 and tinted by
					// their vertex lighting; omni FLARES (diverted here by
					// The_MMX_Scalar for TBR scenes) carry 256×256 textures
					// whose color is baked in — colorize with white, matching
					// the immediate-draw path (the omni V's LR/LG/LB are not
					// maintained and would tint the flare black).
					if (F->Txtr->Txtr->SizeX == 256) {
						// Fogged scene: dim the flare by depth — same colorize
						// the immediate path applies (city beacons at distance;
						// without this the TBR flares pop full-brightness).
						dword Col = 0xFFFFFF;
						if (CurScene->Flags & Scn_Fogged) {
							const float fogRate = 1.0f - V->TPos_AOS.z * C_rFZP;
							const int32_t iFog = Fist(255.0f * fogRate);
							Col = iFog + (iFog << 8) + (iFog << 16);
						}
						// Center-point occlusion reject (Face_PointZTest), as
						// in the immediate path. Skipped for a mirror-clone
						// flare under --mirror_flare_bbox — see the note on
						// the same test in The_MMX_Scalar.
						const uint8_t* const mm = SpriteMirrorMask(F);
						if ((F->Flags & Face_PointZTest) && !mm) {
							int32_t cx = int32_t(V->PX);
							if (cx < 0) cx = 0; else if (cx > XRes-1) cx = XRes-1;
							int32_t cy = int32_t(V->PY);
							if (cy < 0) cy = 0; else if (cy > YRes-1) cy = YRes-1;
							if (Z <= ZPage16[XRes*cy + cx]) continue;
						}
						SpriterRT<256>(V->PX, V->PY, edgeLen, edgeLen,
							F->Txtr->Txtr->Data, VPage, Col, Z,
							0, spriteClipY1, XRes, spriteClipY2,
							mm, F->mirrorMaskTag);
					} else {
						dword R = V->LR, G = V->LG, B = V->LB;
						dword Color = (R << 16) + (G << 8) + B;
						SpriterRT<32>(V->PX, V->PY, edgeLen, edgeLen,
							F->Txtr->Txtr->Data, VPage, Color, Z,
							0, spriteClipY1, XRes, spriteClipY2,
							SpriteMirrorMask(F), F->mirrorMaskTag);
					}
					continue;
				}

				// Transparent face. Open / extend / flush+restart a
				// clump based on (ParentTri, frontFacing).
				TriMesh*  meshHere = F->ParentTri;
				const bool frontHere = (e.rank == 2);   // precomputed at insert (see TBREntry::rank)
				if (!xparClump.empty() &&
				    (meshHere != clumpMesh || frontHere != clumpFront)) {
					flushXparClump();
				}
				if (xparClump.empty()) {
					clumpMesh  = meshHere;
					clumpFront = frontHere;
				}
				xparClump.push_back(F);
			}
			// Trailing flush.
			flushXparClump();
			Sc->SBufferHead[i] = -1;
			if (g_tbrStats) {
				const uint64_t ns = uint64_t(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - tStrip0).count());
				g_tbrBusyNs.fetch_add(ns, std::memory_order_relaxed);
				g_tbrEntries.fetch_add(scratch.size(), std::memory_order_relaxed);
				uint64_t cur = g_tbrMaxStripNs.load(std::memory_order_relaxed);
				while (ns > cur &&
				       !g_tbrMaxStripNs.compare_exchange_weak(cur, ns)) {}
			}
	});

	for (int k = 0; k < numTiles; ++k) stripsDone.acquire();

	if (g_tbrStats) {
		static int sCall = 0;
		const double wallMs = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - tbrWall0).count();
		if ((++sCall & 31) < 2) {  // two consecutive calls (= both city passes)
			std::fprintf(stderr, "[TBR-STATS] call=%d strips=%d wall=%.2fms "
			    "busy=%.2fms (par=%.1fx) maxStrip=%.2fms entries=%llu flushes=%llu\n",
			    sCall, numTiles, wallMs,
			    double(g_tbrBusyNs.load()) / 1e6,
			    wallMs > 0 ? (double(g_tbrBusyNs.load()) / 1e6) / wallMs : 0.0,
			    double(g_tbrMaxStripNs.load()) / 1e6,
			    (unsigned long long)g_tbrEntries.load(),
			    (unsigned long long)g_tbrFlushes.load());
			for (int s2 = 0; s2 < 16; ++s2) {
				const char* nm = g_tbrMatName[s2].load(std::memory_order_relaxed);
				const uint64_t c = g_tbrMatCount[s2].exchange(0, std::memory_order_relaxed);
				if (nm && c)
					std::fprintf(stderr, "[TBR-STATS]   %8llu  %s\n",
					    (unsigned long long)c, nm);
			}
		} else {
			for (int s2 = 0; s2 < 16; ++s2)
				g_tbrMatCount[s2].store(0, std::memory_order_relaxed);
		}
	}

	XparExtentCensus_Report();
	Sc->SBufferCur = 0;
}


