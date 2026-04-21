#include <String.H>
#include <StdIO.H>
#include <StdLib.H>
#include <ConIO.H>
#include <Math.H>


#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"
#include "..\FDS_Defs.H"

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

void Flat_Poly_ManyBits(Vertex *A,Vertex *B,Vertex *C)
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

extern "C" void PG_Texture_32(Bi_GTriangle * BT, char * Bitmap,unsigned int u);
#pragma aux PG_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern "C" void PT_Texture_32(Bi_Triangle * BT, char * Bitmap,unsigned int u);
#pragma aux PT_Texture_32 parm [ebx] [eax] [ecx] modify exact [eax ebx ecx edx esi edi 8087];

extern void Adv_PTexture_8(Bi_Floating * vtx, char * bitmap,unsigned int u);

void The_Specular_Gouraud(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->LR;
  BT[0].U=A->LG;
  BT[0].V=A->LB;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->LR;
  BT[1].U=B->LG;
  BT[1].V=B->LB;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->LR;
  BT[2].U=C->LG;
  BT[2].V=C->LB;
  RGBGouraud(BT,(unsigned long)VPage);
}

void The_MMX_Gouraud(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->LR;
  BT[0].U=A->LG;
  BT[0].V=A->LB;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->LR;
  BT[1].U=B->LG;
  BT[1].V=B->LB;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->LR;
  BT[2].U=C->LG;
  BT[2].V=C->LB;
  RGBGouraud(BT,(unsigned long)VPage);
}


void The_Specular_TGouraud(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->LB;
  BT[0].U=A->LG;
  BT[0].V=A->LR;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->LB;
  BT[1].U=B->LG;
  BT[1].V=B->LR;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->LB;
  BT[2].U=C->LG;
  BT[2].V=C->LR;
  RGBTGouraudMMX(BT,(unsigned long)VPage);
}

void The_Bilinear(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  P_Texture_8(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
//  Adv_PTexture_8(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
}

void The_BiHi(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  P_Texture_16(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
}

void The_BiTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
}

void The_BiTTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  PT_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);
}

void The_BiPTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);

  BT[0].U=A->REU;
  BT[0].V=A->REV;

  BT[1].U=B->REU;
  BT[1].V=B->REV;

  BT[2].U=C->REU;
  BT[2].V=C->REV;

  PT_Texture_32(BT,Phong_Mapping.Data,(unsigned long)VPage); //this will be addative

}

void The_BiPTTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  PT_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);

  BT[0].U=A->REU;
  BT[0].V=A->REV;

  BT[1].U=B->REU;
  BT[1].V=B->REV;

  BT[2].U=C->REU;
  BT[2].V=C->REV;

  PT_Texture_32(BT,Phong_Mapping.Data,(unsigned long)VPage); //this will be addative
}

void The_BiGTrue(Vertex *A,Vertex *B,Vertex *C)
{
  Bi_GTriangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;
  BT[0].R=A->LB*0.5;
  BT[0].G=A->LG*0.5;
  BT[0].B=A->LR*0.5;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;
  BT[1].R=B->LB*0.5;
  BT[1].G=B->LG*0.5;
  BT[1].B=B->LR*0.5;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;
  BT[2].R=C->LB*0.5;
  BT[2].G=C->LG*0.5;
  BT[2].B=C->LR*0.5;

  PG_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);

/*  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(unsigned long)VPage);

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
  Bi_Triangle BT[3];
  BT[0].X=A->PX;
  BT[0].Y=A->PY;
  BT[0].Z=A->RZ;
  BT[0].U=A->RU;
  BT[0].V=A->RV;

  BT[1].X=B->PX;
  BT[1].Y=B->PY;
  BT[1].Z=B->RZ;
  BT[1].U=B->RU;
  BT[1].V=B->RV;

  BT[2].X=C->PX;
  BT[2].Y=C->PY;
  BT[2].Z=C->RZ;
  BT[2].U=C->RU;
  BT[2].V=C->RV;

  P_Texture_32(BT,DoFace->Txtr->Txtr->Data,(DWord)VPage);

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

void The_MMX_Scalar(Vertex *A,Vertex *B,Vertex *C)
{
  long Size = 10000.0*A->RZ;
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