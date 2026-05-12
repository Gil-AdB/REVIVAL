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
#include "SimdHelpers.h"
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
//	FillerPixelcount += 0.5*fabs((_A->PX - _C->PX)*(_B->PY - _C->PY) - (_A->PY - _C->PY)*(_B->PX - _C->PX));
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
	FillerPixelcount += 0.5*fabs((_A->PX - _C->PX)*(_B->PY - _C->PY) - (_A->PY - _C->PY)*(_B->PX - _C->PX));
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
	FillerPixelcount += 0.5*fabs((_A->PX - _C->PX)*(_B->PY - _C->PY) - (_A->PY - _C->PY)*(_B->PX - _C->PX));
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
	FillerPixelcount += 0.5*fabs((_A->PX - _C->PX)*(_B->PY - _C->PY) - (_A->PY - _C->PY)*(_B->PX - _C->PX));
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
		FillerPixelcount += 0.5*fabs((A->PX - C->PX)*(B->PY - C->PY) - (A->PY - C->PY)*(B->PX - C->PX));
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
		FillerPixelcount += 0.5*fabs((A->PX - C->PX)*(B->PY - C->PY) - (A->PY - C->PY)*(B->PX - C->PX));
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


template <int TextureRes = 32, bool EnableZTest = false>
void Spriter(float x, float y, float width, float height, void* txtr, void* vpage, dword Color, dword Z, float spriteClipX1, float spriteClipY1, float spriteClipX2, float spriteClipY2) {
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
			Vec32uc dst;
			dst.load_a(span);
			texture_samples = add_saturated(Vec32uc(texture_samples), dst);

			_mm256_maskstore_ps((float*)span, *(__m256i*)(&mask), *(__m256*)(&texture_samples));

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

void The_MMX_Scalar(Face* F, Vertex **V, dword numVerts, dword miplevel)
{
//	int32_t Size = ImageSize*_A->RZ;
	float Size = ImageSize*_A->RZ*View->PerspX;

	// flare image size is written as a floating pt.
	Size *= F->FlareSize;

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
//		FillerPixelcount += edgeLen*edgeLen;
#endif
		dword Col = 0xFFFFFF;
		if (CurScene->Flags & Scn_Fogged)
		{
			float FogRate = 1.0 - _A->TPos.z * C_rFZP;
			int32_t iFog = Fist(255.0 * FogRate);
			Col = iFog + (iFog<<8) +(iFog<<16);
		}
		// support no far clipping on omnilights
		dword Z;
		if (_A->TPos.z < C_FZP)
			Z = 0xFF80 - Fist(g_zscale * _A->TPos.z);
		else
			Z = 1;

		// Z compare on center only.
		if (F->Flags & Face_PointZTest)
		{
			int32_t x = _A->PX;
			if (x<0) x=0;
			if (x>XRes-1) x=XRes-1;
			int32_t y = _A->PY;
			if (y<0) y=0;
			if (y>YRes-1) y=YRes-1;
			if (Z <= ZPage16[XRes*y+x]) return;
		}
		Spriter<256, true>(_A->PX, _A->PY, edgeLen, edgeLen, F->Txtr->Txtr->Data, VPage, Col, Z, 0, 0, XRes, YRes);
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
	FillerPixelcount = FillerPixelcount + Height * LineWidth;
	__m64 mm4 { (int32_t)Color };
	__m64 mm1 = {};
	
	for(int i=0; i<Height; i++)
	{

		dword u = iu;
		uintptr_t ofs = ((v >> 11) & 0x3E0);
		for(int j = 0;j<LineWidth; j++)
		{
			__m64 mm0 = { 0 };
#ifdef __clang__
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
		dword Z = 0xFF80 - Fist(g_zscale * _A->TPos.z);
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

void InsertSpanToTBR(Face *F, dword tile, float renderZ)
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
	const float renderZ = F->A->TPos.z;
	for (int i = y1; i <= y2; i++)
	{
		InsertSpanToTBR(F, i, renderZ);
	}
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
	// Screen-Y bounds from projected vertex coords (set by
	// Transform_Objects).
	float y_min = F->A->PY;
	float y_max = F->A->PY;
	if (F->B->PY < y_min) y_min = F->B->PY;
	if (F->B->PY > y_max) y_max = F->B->PY;
	if (F->C->PY < y_min) y_min = F->C->PY;
	if (F->C->PY > y_max) y_max = F->C->PY;

	if (y_min >= YRes || y_max < 0) return;

	int32_t numTiles = CurScene->NumTiles;
	int s_lo = Fist(y_min * rTILESIZE - 0.999999f);
	int s_hi = Fist(y_max * rTILESIZE);
	if (s_lo < 0) s_lo = 0;
	if (s_hi >= numTiles) s_hi = numTiles - 1;
	if (s_lo > s_hi) return;

	// Per-face front-facing test (matches the rest of the xpar pipeline).
	const float ex = F->B->TPos.x - F->A->TPos.x;
	const float ey = F->B->TPos.y - F->A->TPos.y;
	const float ez = F->B->TPos.z - F->A->TPos.z;
	const float fx = F->C->TPos.x - F->A->TPos.x;
	const float fy = F->C->TPos.y - F->A->TPos.y;
	const float fz = F->C->TPos.z - F->A->TPos.z;
	const float nx = ey * fz - ez * fy;
	const float ny = ez * fx - ex * fz;
	const float nz = ex * fy - ey * fx;
	const float vd = nx * F->A->TPos.x + ny * F->A->TPos.y + nz * F->A->TPos.z;
	const bool front = vd < 0.0f;

	float renderZ;
	if (front) {
		// Near surface — min vertex Z. Sorts later (= rendered LATER
		// = on top of farther stuff).
		renderZ = F->A->TPos.z;
		if (F->B->TPos.z < renderZ) renderZ = F->B->TPos.z;
		if (F->C->TPos.z < renderZ) renderZ = F->C->TPos.z;
	} else {
		// Far surface — max vertex Z. Sorts earlier (= rendered FIRST
		// = the back-most surface in the per-strip back-to-front walk).
		renderZ = F->A->TPos.z;
		if (F->B->TPos.z > renderZ) renderZ = F->B->TPos.z;
		if (F->C->TPos.z > renderZ) renderZ = F->C->TPos.z;
	}

	for (int s = s_lo; s <= s_hi; s++) {
		InsertSpanToTBR(F, s, renderZ);
	}
}

void TBR_Sprite(Face* F, Vertex **V, dword numVerts, dword miplevel)
{
	float Size = ImageSize*_A->RZ*View->PerspX;

	// flare image size is written as a floating pt.
	Size *= F->FlareSize;

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

namespace tbr {
	std::mutex					tileCounterMutex;
	std::atomic<int>			tileCounter;
	std::condition_variable		condition;
}

// Execute/Clear sprite buffer
//
// Per-strip walk: each Y-strip (8 rows) is processed by one worker
// thread. The linked list at SBufferHead[i] holds all sprite entries
// that overlap this strip (and, once unified, transparent face
// fragments too). At flush time we collect entries into a scratch
// array, sort descending by renderZ (back-to-front), and walk the
// sorted array dispatching each item.
void TBR_Render(Scene *Sc)
{
	dword numTiles = Sc->NumTiles;

	tbr::tileCounter = 0;

	for (mword i = 0; i < numTiles; i++)
	{

		ThreadPool::instance().enqueue([i, Sc]() {
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
			// Descending: largest renderZ (farthest) first.
			std::stable_sort(scratch.begin(), scratch.end(),
				[](const TBREntry& a, const TBREntry& b) {
					return a.renderZ > b.renderZ;
				});

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

			auto computeFront = [](Face* F) -> bool {
				const float ex = F->B->TPos.x - F->A->TPos.x;
				const float ey = F->B->TPos.y - F->A->TPos.y;
				const float ez = F->B->TPos.z - F->A->TPos.z;
				const float fx = F->C->TPos.x - F->A->TPos.x;
				const float fy = F->C->TPos.y - F->A->TPos.y;
				const float fz = F->C->TPos.z - F->A->TPos.z;
				const float nx = ey * fz - ez * fy;
				const float ny = ez * fx - ex * fz;
				const float nz = ex * fy - ey * fx;
				const float vd = nx * F->A->TPos.x + ny * F->A->TPos.y + nz * F->A->TPos.z;
				return vd < 0.0f;
			};
			auto flushXparClump = [&]() {
				if (xparClump.empty()) return;
				RenderXparClumpInStrip(xparClump.data(),
				                       int(xparClump.size()),
				                       clumpFront, strip_y_i, strip_h);
				xparClump.clear();
			};

			for (const TBREntry& e : scratch) {
				Face*   F = e.F;
				const bool isSprite = (F->A == F->B);

				if (isSprite) {
					// Flush any open transparent clump first so it
					// composites BEFORE this (closer) sprite.
					flushXparClump();

					Vertex* V = F->A;
					float edgeLen = ImageSize * V->RZ * View->PerspX * F->FlareSize * 2.0f;
					dword R = V->LR, G = V->LG, B = V->LB;
					dword Color = (R << 16) + (G << 8) + B;
					dword Z = 0xFF80 - Fist(g_zscale * V->TPos.z);
					Spriter<32, true>(V->PX, V->PY, edgeLen, edgeLen,
						F->Txtr->Txtr->Data, VPage, Color, Z,
						0, spriteClipY1, XRes, spriteClipY2);
					continue;
				}

				// Transparent face. Open / extend / flush+restart a
				// clump based on (ParentTri, frontFacing).
				TriMesh*  meshHere = F->ParentTri;
				const bool frontHere = computeFront(F);
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

			std::unique_lock<std::mutex> lock(tbr::tileCounterMutex);
			++tbr::tileCounter;
			tbr::condition.notify_one();
		});
	}

	{
		std::unique_lock<std::mutex> lock(tbr::tileCounterMutex);
		tbr::condition.wait(lock, [numTiles] {return tbr::tileCounter == numTiles; });
	}

	Sc->SBufferCur = 0;
}


