#include <string.h>
#include <stdio.h>
#include <stdlib.h>
//#include <conio.h>
#include <math.h>
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
#pragma pack(1)

typedef struct
{
	float FX,FY;
	int   x, y;
	float z, U, V;
} Yosi_Triangle;

#ifdef _C_WATCOM
#define CRGBGouraud _RGBGouraud
#define CRGBGouraudMMX _RGBGouraudMMX
#define CRGBTGouraud _RGBTGouraud
#define CRGBTGouraudMMX _RGBTGouraudMMX
#define CP_Texture_8 _P_Texture_8
#define CP_Texture_16 _P_Texture_16
#define CP_Texture_32 _P_Texture_32
#define CPG_Texture_32 _PG_Texture_32
#define CPT_Texture_32 _PT_Texture_32

extern "C" void _RGBGouraud(Bi_Triangle * BT,unsigned int u);
#pragma aux _RGBGouraud parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void _RGBGouraudMMX(Bi_Triangle * BT,unsigned int u);
#pragma aux _RGBGouraudMMX parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void _RGBTGouraud(Bi_Triangle *BT,unsigned int u);
#pragma aux _RGBTGouraud parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void _RGBTGouraudMMX(Bi_Triangle * BT,unsigned int u);
#pragma aux _RGBTGouraudMMX parm [ebx] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void _P_Texture_8(Yosi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux _P_Texture_8 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void _P_Texture_16(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux _P_Texture_16 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void _P_Texture_32(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux _P_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void _PG_Texture_32(Bi_GTriangle * BT, char * Bitmap,unsigned int u);
#pragma aux _PG_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void _PT_Texture_32(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux _PT_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];
#endif*/
#ifdef WIN32
/*
#define CRGBGouraud RGBGouraudV
#define CRGBGouraudMMX RGBGouraudMMXV
#define CRGBTGouraud RGBTGouraudV
#define CRGBTGouraudMMX RGBTGouraudMMXV
#define CP_Texture_8 P_Texture_8V
#define CP_Texture_16 P_Texture_16V
#define CP_Texture_32 P_Texture_32V
#define CPG_Texture_32 PG_Texture_32V
#define CPT_Texture_32 PT_Texture_32V

extern "C" void __cdecl RGBGouraud_(void);
void RGBGouraudV(Bi_Triangle * BTM,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov ecx, [u]
		call	RGBGouraud_
		popad
	}
}

extern "C" void __cdecl RGBGouraudMMX_(void);
void RGBGouraudMMXV(Bi_Triangle * BTM,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov ecx, [u]
		call	RGBGouraudMMX_
		popad
	}
}

extern "C" void __cdecl RGBTGouraud_(void);
void RGBTGouraudV(Bi_Triangle * BTM,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov ecx, [u]
		call	RGBTGouraud_
		popad
	}
}



extern "C" void __cdecl RGBTGouraudMMX_(void);
void RGBTGouraudMMXV(Bi_Triangle * BTM,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov ecx, [u]
		call	RGBTGouraudMMX_
		popad
	}
}

extern "C" void __cdecl P_Texture_8_(void);
void P_Texture_8V(Yosi_Triangle * BTM, char * Bitmap,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov eax, [Bitmap]
		mov ecx, [u]
		call	P_Texture_8_
		popad
	}
}

extern "C" void __cdecl P_Texture_16_(void);
void P_Texture_16V(Bi_Triangle * BTM, char * Bitmap,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov eax, [Bitmap]
		mov ecx, [u]
		call	P_Texture_16_
		popad
	}
}

extern "C" void __cdecl P_Texture_32_(void);
void P_Texture_32V(Bi_Triangle * BTM, char * Bitmap,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov eax, [Bitmap]
		mov ecx, [u]
		call	P_Texture_32_
		popad
	}
}

extern "C" void __cdecl PG_Texture_32_(void);
void PG_Texture_32V(Bi_GTriangle * BTM, char * Bitmap,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov eax, [Bitmap]
		mov ecx, [u]
		call	PG_Texture_32_
		popad
	}
}

extern "C" void __cdecl PT_Texture_32_(void);
void PT_Texture_32V(Bi_Triangle * BTM, char * Bitmap,unsigned int u)
{
	__asm
	{
		pushad
		mov ebx, [BTM]
		mov eax, [Bitmap]
		mov ecx, [u]
		call	PT_Texture_32_
		popad
	}
}
*/
#endif
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

#ifdef _C_WATCOM

#define CSeven_UP Seven_UP
#define CSeven_UP_64 Seven_UP_64
#define CSeven_UP_MMX Seven_UP_MMX
#define CSeven_UP_MMX_32 Seven_UP_MMX_32
#define CZScale_thatfucksBTB ZScale_thatfucksBTB

extern "C" void _Seven_UP(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux _Seven_UP parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

extern "C" void Seven_UP_64(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux Seven_UP_64 parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

extern "C" void Seven_UP_MMX(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux Seven_UP_MMX parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

extern "C" void Seven_UP_MMX_32(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux Seven_UP_MMX_32 parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

extern "C" void ZScale_thatfucksBTB(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux ZScale_thatfucksBTB parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi]

#else 
#if 0
#define CSeven_UP Seven_UPV
#define CSeven_UP_MMX Seven_UP_MMXV
#define CSeven_UP_MMX_32 Seven_UP_MMX_32V
#define CZScale_thatfucksBTB ZScale_thatfucksBTBV

extern "C" void Grid_Texture_Mapper_TG_(void); // this is here because it's easier this way

extern "C" void Seven_UP_(void);
void Seven_UPV(int X,int Y,int SizeX,int SizeY,byte *Txtr,byte *Page)
{
	__asm
	{
		pushad
		mov eax, [X]
		mov ebx, [Y]
		mov ecx, [SizeX]
		mov edx, [SizeY]
		mov esi, [Txtr]
		mov edi, [Page]
		call Seven_UP_
		popad
	}
}

extern "C" void Seven_UP_MMX_(void);
void Seven_UP_MMXV(int X,int Y,int SizeX,int SizeY, byte *Txtr, byte *Page, dword Color, dword Z)
{
	__asm
	{
		mov eax, [X]
		mov ebx, [Y]
		mov ecx, [SizeX]
		mov edx, [SizeY]
		mov esi, [Txtr]
		mov edi, [Page]
		pushad
		push Z
		push Color
		call Seven_UP_MMX_
		pop eax
		pop eax
		popad
	}
}

extern "C" void Seven_UP_MMX_32_(void);
void Seven_UP_MMX_32V(int X,int Y,int SizeX,int SizeY, byte *Txtr, byte *Page, dword Color, dword Z)
{
	__asm
	{
		mov eax, [X]
		mov ebx, [Y]
		mov ecx, [SizeX]
		mov edx, [SizeY]
		mov esi, [Txtr]
		mov edi, [Page]
		pushad
		push Z
		push Color	
		call Seven_UP_MMX_32_		
		pop eax
		pop eax
		popad
	}
}

extern "C" void ZScale_thatfucksBTB_(void);
void ZScale_thatfucksBTBV(int X,int Y,int SizeX,int SizeY, byte *Txtr, byte *Page)
{
	__asm
	{
		pushad
		mov eax, [X]
		mov ebx, [Y]
		mov ecx, [SizeX]
		mov edx, [SizeY]
		mov esi, [Txtr]
		mov edi, [Page]
		call ZScale_thatfucksBTB_
		popad
	}
}
#endif
#endif

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
	auto zscanline = (((byte*)vpage) + PageSize + iy1 * XRes * 2);

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
			if (Z <= ((word *)VPage) [(PageSize>>1)+XRes*y+x]) return;
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
#if defined(PORTABLE_CODE) && defined(PIG_VOMIT)
	__m64 mm4 { (int32_t)Color };
	__m64 mm1 = {};
#else
	__asm
	{
		movd mm4, [Color]
		pxor mm1,mm1
		punpcklbw mm4,mm1
	}
#endif
	
	for(int i=0; i<Height; i++)
	{

		dword u = iu;
#ifndef PORTABLE_CODE
		dword *texScanLine = &tex[((v>>11)&0x3E0)];
		__asm
		{
			push ebp

			; uploading virus
			mov		ecx,	[LineWidth]
			mov		esi,	[texScanLine]
			mov		edi,	[ScanLine]
			mov		eax,	[u]
			mov		edx,	[idu]
			mov		ebp,	edi			

			; next scanline prefetch (bad impl - should prefetch entire tile in TBR renderer)
;			mov		ebx,	[VESA_BPSL]
;			prefetcht0		byte ptr [edi+ebp]

			; align to 8 bytes
			and		ebp,	4
			jz		AfterLeftAlign

			mov		ebx,	eax
			shr		ebx,	14
			and		ebx,	0fffffffch
			
			movd mm1, [edi]
			punpcklbw mm0, [esi+ebx]
			psrlw mm0, 8
			pmullw mm0, mm4
			psrlw mm0, 8
			packuswb mm0, mm0			
			paddusb mm0, mm1				
			movd [edi], mm0			
		
			add		eax,	edx
			add		edi,	4
			sub		ecx,	1
			jz		EndScanline
AfterLeftAlign:
			mov		ebp,	ecx
			and		ebp,	1			
			and		ecx,	0fffffffeh
			jz		BeforeRightAlign

			; read two pixels from screen
			movq mm1, [edi]

			; read first pixel from texture
			mov		ebx,	eax
			shr		ebx,	14
			and		ebx,	0fffffffch
			
			punpcklbw mm0, [esi+ebx]

			add		eax,	edx

			; read second pixel from texture
			mov			ebx,	eax
			shr			ebx,	14
			and			ebx,	0fffffffch
			
			punpcklbw	mm2,	[esi+ebx]

			pmulhuw		mm0,	mm4
			pmulhuw		mm2,	mm4

			packuswb	mm0,	mm2
			paddusb		mm0,	mm1

			movq		[edi],	mm0

			add		eax,	edx
			add		edi,	8

			sub		ecx,	2

			cmp		ebp,	ebp
			jz		AfterLeftAlign

BeforeRightAlign:
			mov		ebx,	eax
			shr		ebx,	14
			and		ebx,	0fffffffch
			
			movd mm1, [edi]
			punpcklbw mm0, [esi+ebx]
			psrlw mm0, 8
			pmullw mm0, mm4
			psrlw mm0, 8
			packuswb mm0, mm0			
			paddusb mm0, mm1				
			movd [edi], mm0			
			
EndScanline:
			pop ebp
		}

#else
		uintptr_t ofs = ((v >> 11) & 0x3E0);
		//__m64 x = { 0 };
		for(int j = 0;j<LineWidth; j++)
		{
#ifdef PIG_VOMIT


			//dword t = tex[(u >> 16) + ofs];

////			tmm.m64_u32[0] = t;
//			*(dword *)&tmm = t;

			//mm0 = _m_pxor(mm0, mm0);
//			mm0 = _m_punpcklbw(mm0, *(__m64*)&tex[(u >> 16) + ((v >> 11) & 0x3E0)]);
			__m64 mm0 = { 0 };
			//*x = mm0.m64_u32[0];
#ifdef __clang__
			((dword*)ScanLine)[j] = (dword)mm0[0];
#else
			((dword*)ScanLine)[j] = mm0.m64_u32[0];
#endif
#else
			dword t = tex[(u >> 16) + ofs];
			dword x = ((dword*)ScanLine)[j] ;
			__asm
			{
				pxor mm0, mm0
				punpcklbw mm0, [t]
				psrlw mm0, 8
				pmullw mm0, mm4
				psrlw mm0, 8
				packuswb mm0, mm0
				movd mm1, [x]
				paddusb mm0, mm1				
				movd [x], mm0
			}
			((dword *)ScanLine)[j] = x;
#endif // PIG_VOMIT

			u += idu;
		}
#endif
		ScanLine += VESA_BPSL;
		v += idv;
	}
#ifdef PORTABLE_CODE
#else
	__asm
	{
		emms
	}
#endif
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
		if (Z > ((word *)VPage) [(PageSize>>1)+XRes*y+x])
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

void InsertSpanToTBR(Face *F, dword tile)
{
	sdword h = CurScene->SBufferHead[tile];
	mword  n = CurScene->SBufferCur;
	CurScene->SBuffer[n].F = F;
	CurScene->SBuffer[n].next = h;
	CurScene->SBufferHead[tile] = n;
	CurScene->SBufferCur++;
}


void InsertSpriteToTBR(Face *F, float y, float Height)
{
	int y1, y2;
	static int32_t numTiles = CurScene->NumTiles;

	y1 = Fist(((y - Height) * rTILESIZE) - 0.999999);
	y2 = Fist((y + Height) * rTILESIZE);

	if (y1 >= numTiles || y2 < 0)
		return;

	if (y1 < 0) y1=0;
	if (y2 >= numTiles) y2 = numTiles-1;

	for (int i = y1; i <= y2; i++)
	{
		InsertSpanToTBR(F, i);
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

		dword R,G,B;
		R = _A->LR;
		G = _A->LG;
		B = _A->LB;

		dword Color = (R<<16)+(G<<8)+B;

		// get color from vertex _A
		dword Z = 0xFF80 - Fist(g_zscale * _A->TPos.z);
		// Z compare on center only.
		int32_t x = _A->PX;
		if (x<0) x=0;
		if (x>XRes-1) x=XRes-1;
		int32_t y = _A->PY;
		if (y<0) y=0;
		if (y>YRes-1) y=YRes-1;
		if (
			(Z > ((word *)VPage) [(PageSize>>1)+XRes*y+x]) && // depth test
			((_A->PX + edgeLen > 0) && (_A->PX - edgeLen < XRes)) // x-clipping
			)
		{
			// Insertion routine - Add 'spans' to TBR data structure
//			spriteClipY1 = 0;
//			spriteClipY2 = YRes;
//			Spriter32(_A->PX,_A->PY,edgeLen,edgeLen,F->Txtr->Txtr->Data,VPage, Color);
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
void TBR_Render(Scene *Sc)
{
	dword numTiles = Sc->NumTiles;

	tbr::tileCounter = 0;

	for (mword i = 0; i < numTiles; i++)
	{

		ThreadPool::instance().enqueue([i, Sc]() {
			Face* F;
			Vertex* V;

			float spriteClipY1 = i << TILELOG;
			float spriteClipY2 = (i + 1) << TILELOG;
			if (spriteClipY2 > YRes) spriteClipY2 = YRes;
			sdword n = Sc->SBufferHead[i];
			while (n != -1) {
				F = Sc->SBuffer[n].F;
				V = F->A;
				float edgeLen = ImageSize * V->RZ * View->PerspX * F->FlareSize * 2.0;
				dword R, G, B;
				R = V->LR;
				G = V->LG;
				B = V->LB;
				dword Color = (R << 16) + (G << 8) + B;

				Spriter(V->PX,
					V->PY,
					edgeLen,
					edgeLen,
					F->Txtr->Txtr->Data,
					VPage,
					Color, 
					0,
					0,
					spriteClipY1,
					XRes,
					spriteClipY2);
				n = Sc->SBuffer[n].next;
			}
			Sc->SBufferHead[i] = -1;

			std::unique_lock<std::mutex> lock(tbr::tileCounterMutex);
			++tbr::tileCounter;
			tbr::condition.notify_one();
		});
		// LEAAAAAAT LEAAAAAAAT
//		Flip(MainSurf);
	}

	{
		std::unique_lock<std::mutex> lock(tbr::tileCounterMutex);
		tbr::condition.wait(lock, [numTiles] {return tbr::tileCounter == numTiles; });
	}

	Sc->SBufferCur = 0;	
}


#ifdef WIN32

extern "C" 
{
	extern dword RGBGouraud_Size;
	extern dword RGBGouraudMMX_Size;
    extern dword RGBTGouraud_Size;
	extern dword RGBTGouraudMMX_Size;
	extern dword P_Texture_8_Size;
	extern dword P_Texture_16_Size;
	extern dword P_Texture_32_Size;
	extern dword P_Texture_32_CodeStart;
	extern dword PG_Texture_32_Size;
	extern dword PT_Texture_32_Size;
	dword Seven_UP_MMX_Size;
	extern dword Seven_UP_MMX_32_Size;
	extern dword WobMMX_Size;

	//extern dword IX_TGZM_Code_Start;
	//extern dword IX_TGZM_Code_Size;
	//extern dword IX_TGZTM_Code_Start;
	//extern dword IX_TGZTM_Code_Size;
	//extern dword IX_TGZTAM_Code_Start;
	//extern dword IX_TGZTAM_Code_Size;
	//extern dword IX_TGZSAM_Code_Start;
	//extern dword IX_TGZSAM_Code_Size;
}

void InitFillers (VPFunc VirtualProtectFunc, dword Protection)
{
	uint32_t	OldProtection;

//	VirtualProtectFunc(InitFillers, 1000, Protection, &OldProtection);
//	((char *)InitFillers)[0] = 0;

//	VirtualProtectFunc(Grid_Texture_Mapper_TG_, 
//					   WobMMX_Size,
//					   Protection,
//					   &OldProtection);

	//VirtualProtectFunc(RGBGouraud_, 
	//				   RGBGouraud_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(RGBGouraudMMX_, 
	//				   RGBGouraudMMX_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(RGBTGouraud_, 
	//				   RGBTGouraud_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(RGBTGouraudMMX_, 
	//				   RGBTGouraudMMX_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(P_Texture_8_, 
	//				   P_Texture_8_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(P_Texture_16_, 
	//				   P_Texture_16_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc((void *)P_Texture_32_CodeStart, 
	//				   P_Texture_32_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(PG_Texture_32_, 
	//				   PG_Texture_32_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(PT_Texture_32_, 
	//				   PT_Texture_32_Size,
	//				   Protection,
	//				   &OldProtection);

#ifdef NON_PORTABE_CODE

	VirtualProtectFunc(reinterpret_cast<void *>(Seven_UP_MMX_), 
					   Seven_UP_MMX_Size,
					   Protection,
					   &OldProtection);

#endif // NON_PORTABE_CODE

#if 0
	VirtualProtectFunc(reinterpret_cast<void *>(Seven_UP_MMX_32_),
					   Seven_UP_MMX_32_Size,
					   Protection,
					   &OldProtection);
#endif

	//VirtualProtectFunc(reinterpret_cast<void *>(IX_TGZM_Code_Start),
	//				   IX_TGZM_Code_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(reinterpret_cast<void *>(IX_TGZTM_Code_Start),
	//				   IX_TGZTM_Code_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(reinterpret_cast<void *>(IX_TGZTAM_Code_Start),
	//				   IX_TGZTAM_Code_Size,
	//				   Protection,
	//				   &OldProtection);

	//VirtualProtectFunc(reinterpret_cast<void*>(IX_TGZSAM_Code_Start),
	//	IX_TGZSAM_Code_Size,
	//	Protection,
	//	&OldProtection);
}
#endif
