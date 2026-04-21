#include <StdIO.H>
#include <String.H>
#include <Math.H>
#include <StdLib.H>
#include <Memory.H>
#include <String.H>
#include <ConIO.H>
#include <i86.H>
#include <DOS.H>
#include <Time.H>


#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"
#include "..\FDS_Defs.H"

struct Four_C
{
  char R,G,B,A;
};
union FCDW
{
  Four_C C;
  DWord DW;
};

DWord TblFlags = 0;
char *ModTbl;

/// Table Handlers

// Modulation table - takes up 64Kb. It is used for fast lookup for
// the simple operation m(I,J) = I*J>>8. This takes several cycles
// to compute and is found inside several inner loops requiring modulation
// (mainly for Illumination of textures)
void Make_Modulation()
{
  long I,J;
  char *W;
  if (TblFlags&TblMod_Made) return;
  TblFlags|=TblMod_Made;
  // Generate a Multiplicative (Modulation) table
  ModTbl = new char[65536]; // should have been aligned.
  W = ModTbl;
  for(J=0;J<256;J++)
    for(I=0;I<256;I++)
      *W++ = I*J>>8;
}

// Mip-maps a given Image by simple Texel averaging
void MipmapXY(Image *Img)
{
  long X,Y;
  DWord *Mip = new DWord[(Img->X+1>>1)*(Img->Y+1>>1)];
  char *Trg = (char *)Mip;
  char *Src = (char *)Img->Data;
  char *Tex;
  long X4 = Img->X<<2,Y4 = Img->Y<<2;
  long X4_4 = X4+4;

  for(Y=0;Y<Img->Y>>1;Y++)
  {
    for(X=0;X<Img->X>>1;X++)
    {
      Tex = Src+((X+Y*Img->X)<<3);
      *Trg++ = (*Tex)+(*(Tex+4))+(*(Tex+X4))+(*(Tex+X4_4))>>2; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))+(*(Tex+X4))+(*(Tex+X4_4))>>2; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))+(*(Tex+X4))+(*(Tex+X4_4))>>2; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))+(*(Tex+X4))+(*(Tex+X4_4))>>2; Tex++;
    }
    // copy last texel as a two-texel average
    if (Img->X&1)
    {
      Tex = Src+(X4-4+Y*X4);
      *Trg++ = (*Tex)+(*(Tex+X4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+X4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+X4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+X4))>>1; Tex++;
    }
  }
  if (Img->Y&1)
  {
    Y = Img->Y-1;
    // copy last row as two-texel average
    for(X=0;X<Img->X>>1;X++)
    {
      Tex = Src+((X+Y*Img->X)<<2);
      *Trg++ = (*Tex)+(*(Tex+4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))>>1; Tex++;
    }
    if (Img->X&1) // copy last texel as it is
      *((DWord *)Trg) = *((DWord *)Tex);
  }
  delete Img->Data;
  Img->X = (Img->X+1)>>1;
  Img->Y = (Img->Y+1)>>1;
  Img->Data = Mip;
}

// Mip-maps a given Image by simple Texel averaging, works on 2x1 blocks
void MipmapX(Image *Img)
{
  long X,Y;
  DWord *Mip = new DWord[(Img->X+1>>1)*Img->Y];
  char *Trg = (char *)Mip;
  char *Src = (char *)Img->Data;
  char *Tex;
  long X4 = Img->X<<2,Y4 = Img->Y<<2;
  long X4_4 = X4+4;

  for(Y=0;Y<Img->Y;Y++)
  {
    for(X=0;X<Img->X>>1;X++)
    {
      Tex = Src+(((X<<1)+Y*Img->X)<<2);
      *Trg++ = (*Tex)+(*(Tex+4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+4))>>1; Tex++;
    }
    // copy last texel as it is
    if (Img->X&1)
    {
      Tex = Src+(X4-4+Y*X4);
      *((DWord *)Trg) = *((DWord *)Tex);
    }
  }
  delete Img->Data;
  Img->X = (Img->X+1)>>1;
  Img->Data = Mip;
}

// Mip-maps a given Image by simple Texel averaging, works on 1x2 blocks
void MipmapY(Image *Img)
{
  long X,Y;
  DWord *Mip = new DWord[Img->X*(Img->Y+1>>1)];
  char *Trg = (char *)Mip;
  char *Src = (char *)Img->Data;
  char *Tex;
  long X4 = Img->X<<2,Y4 = Img->Y<<2;
  long X4_4 = X4+4;

  for(Y=0;Y<Img->Y>>1;Y++)
  {
    for(X=0;X<Img->X;X++)
    {
      Tex = Src+((X+(Y<<1)*Img->X)<<2);
      *Trg++ = (*Tex)+(*(Tex+X4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+X4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+X4))>>1; Tex++;
      *Trg++ = (*Tex)+(*(Tex+X4))>>1; Tex++;
    }
  }
  if (Img->Y&1)
  {
    Y = Img->Y-1;
    // copy last row as it is
    memcpy(Trg,Src+((Y*Img->X)<<2),X4);
  }
  delete Img->Data;
  Img->Y = (Img->Y+1)>>1;
  Img->Data = Mip;
}

// Converts a 256x256 Texture to the Digital Iamge format.
void Convert_Texture2Image(Texture *Tx,Image *Img)
{
  Texture *TT = new Texture;
  TT->BPP = Tx->BPP;
  Img->X = 256;
  Img->Y = 256;

  if (TT->BPP!=32)
  {
    // make a new texture, convert it, and hand over the data
    TT->Data = new char[65536*(TT->BPP+1>>3)];
    memcpy(TT->Data,Tx->Data,65536*(TT->BPP+1>>3));
    BPPConvert_Texture(TT,32);
    Img->Data = (DWord *)TT->Data;
  } else {
    // copy txtr as it is
    Img->Data = new DWord[65536];
    memcpy(Img->Data,Tx->Data,262144);
  }
  delete TT;
}

// Converts Digital Image to a usable Texture.
void Convert_Image2Texture(Image *Img,Texture *Tx)
{
  Image *TI = new Image;
  long I,J,X4,X4_4;
  float X,Y,dX,dY,iX,iY,lX,lY,rX,rY,rXrY,rXlY,lXrY,lXlY;
  char *W,*R,*RY,*RO;
  // copy original Image to TI
  TI->X = Img->X;
  TI->Y = Img->Y;
  TI->Data = new DWord[TI->X*TI->Y];
  memcpy(TI->Data,Img->Data,TI->X*TI->Y<<2);

  // Mipmap image until it's smaller than Texture size (256x256)
  while (TI->X>256||TI->Y>256)
  {
    if (TI->X>256&&TI->Y>256) {MipmapXY(TI); continue;}
    if (TI->X>256) {MipmapX(TI); continue;}
    if (TI->Y>256) {MipmapY(TI); continue;}
  }
  // Bilinear Filtering
  Tx->Data = new char [262144];
  if (TI->X<256||TI->Y<256)
  {
    R = (char *)TI->Data;
    W = (char *)Tx->Data;
    X4 = TI->X<<2;
    X4_4 = X4+4;
    dX = (float)(TI->X-1.0)/256.0;
    dY = (float)(TI->Y-1.0)/256.0;
    Y = 0.0;
    for(J=0;J<256;J++)
    {
      X = 0.0;
      iY = floor(Y);
      lY = Y-iY;
      rY = 1.0-lY;
      RY = R+((((int)iY)*TI->X)<<2);
      for(I=0;I<256;I++)
      {
        iX = floor(X);
        lX = X-iX;
        rX = 1.0-lX;
        RO = RY+((int)iX<<2);
        rXrY = rX*rY; lXrY=lX*rY; rXlY = rX*lY; lXlY = lX*lY;

        *W++ = rXrY*(*RO)+lXrY*(*(RO+4))+rXlY*(*(RO+X4))+lXlY*(*(RO+X4_4)); RO++;
        *W++ = rXrY*(*RO)+lXrY*(*(RO+4))+rXlY*(*(RO+X4))+lXlY*(*(RO+X4_4)); RO++;
        *W++ = rXrY*(*RO)+lXrY*(*(RO+4))+rXlY*(*(RO+X4))+lXlY*(*(RO+X4_4)); RO++;
        *W++ = rXrY*(*RO)+lXrY*(*(RO+4))+rXlY*(*(RO+X4))+lXlY*(*(RO+X4_4));
        X+=dX;
      }
      Y+=dY;
    }
  } else memcpy(Tx->Data,TI->Data,262144);
  delete TI->Data;
  delete TI;

  // Convert to requested bpp
  I = Tx->BPP;
  Tx->BPP=32;
  BPPConvert_Texture(Tx,I);
}

// A Mipmapping, Bi-linear scaler.
void Scale_Image(Image *Img,long NX,long NY)
{
  Image *TI = new Image,*BI;
  long I,J,X4,X4_4;
  float X,Y,dX,dY,iX,iY,lX,lY,rX,rY,rXrY,rXlY,lXrY,lXlY;
  char *W,*R,*RY,*RO;
  // copy original Image to TI
  TI->X = Img->X;
  TI->Y = Img->Y;
  TI->Data = new DWord[TI->X*TI->Y];
  memcpy(TI->Data,Img->Data,TI->X*TI->Y<<2);

  // Mipmap image until it's smaller than specified size
  while (TI->X>NX||TI->Y>NY)
  {
    if (TI->X>NX&&TI->Y>NY) {MipmapXY(TI); continue;}
    if (TI->X>NX) {MipmapX(TI); continue;}
    if (TI->Y>NY) {MipmapY(TI); continue;}
  }
  // Bilinear Filtering
  delete Img->Data;
  Img->Data = new DWord[NX*NY];
  Img->X = NX;
  Img->Y = NY;
  if (TI->X<NX||TI->Y<NY)
  {
    R = (char *)TI->Data;
    W = (char *)Img->Data;
    X4 = TI->X<<2;
    X4_4 = X4+4;
    dX = (float)(TI->X-1.0)/NX;
    dY = (float)(TI->Y-1.0)/NY;
    Y = 0.0;
    for(J=0;J<NY;J++)
    {
      X = 0.0;
      iY = floor(Y);
      lY = Y-iY;
      rY = 1.0-lY;
      RY = R+((((int)iY)*TI->X)<<2);
      for(I=0;I<NX;I++)
      {
        iX = floor(X);
        lX = X-iX;
        rX = 1.0-lX;
        RO = RY+((int)iX<<2);
        rXrY = rX*rY; lXrY=lX*rY; rXlY = rX*lY; lXlY = lX*lY;

        *W++ = rXrY*(*RO)+lXrY*(*(RO+4))+rXlY*(*(RO+X4))+lXlY*(*(RO+X4_4)); RO++;
        *W++ = rXrY*(*RO)+lXrY*(*(RO+4))+rXlY*(*(RO+X4))+lXlY*(*(RO+X4_4)); RO++;
        *W++ = rXrY*(*RO)+lXrY*(*(RO+4))+rXlY*(*(RO+X4))+lXlY*(*(RO+X4_4)); RO++;
        *W++ = rXrY*(*RO)+lXrY*(*(RO+4))+rXlY*(*(RO+X4))+lXlY*(*(RO+X4_4));
        X+=dX;
      }
      Y+=dY;
    }
  } else memcpy(Img->Data,TI->Data,4*NX*NY);
  delete TI->Data;
  delete TI;
}

// Performs Gamma correction on the Image. Values between 0 and 1 darken it,
// while values greater than 1 may require clipping and reduce image detail.
void Gamma_Correction(Image *Img,float Gamma)
{
  long I,J,K;
  FCDW FCD;
  DWord *P = Img->Data;
  if (Gamma<0) return;
  if (Gamma<=1.0)
  {
    for(J=0;J<Img->Y;J++)
      for(I=0;I<Img->X;I++)
      {
        FCD.DW = *P;
        FCD.C.R*=Gamma;
        FCD.C.G*=Gamma;
        FCD.C.B*=Gamma;
        *P++ = FCD.DW;
      }
  } else {
    for(J=0;J<Img->Y;J++)
      for(I=0;I<Img->X;I++)
      {
        FCD.DW = *P;
        K = FCD.C.R*Gamma;
        if (K>255) FCD.C.R=255; else FCD.C.R=K;
        K = FCD.C.G*Gamma;
        if (K>255) FCD.C.G=255; else FCD.C.G=K;
        K = FCD.C.B*Gamma;
        if (K>255) FCD.C.B=255; else FCD.C.B=K;
        *P++ = FCD.DW;
      }
  }
}

// Intensity Correction
void Intns_Histogram_Correction(Image *Img)
{
  long Hist[444];
  float Conv[444];
  long I,J,K;
  float F;
  memset(Hist,0,444*4);
  DWord *P = Img->Data;
  float rt = 444.0/(float)(Img->X*Img->Y);
  FCDW FCD;

  for(J=0;J<Img->Y;J++)
    for(I=0;I<Img->X;I++)
    {
      FCD.DW = *P++;
      Hist[(int)sqrt(FCD.C.R*FCD.C.R+FCD.C.G*FCD.C.G+FCD.C.B*FCD.C.B)]++;
    }
  for(I=1;I<444;I++)
    Hist[I]+=Hist[I-1];
  //now we got addative statistics...
  //Convertion rates
  for(I=0;I<444;I++)
    Conv[I]=(float)(Hist[I]*rt)/(float)I;
  P = Img->Data;
  for(J=0;J<Img->Y;J++)
    for(I=0;I<Img->X;I++)
    {
      FCD.DW = *P;
      F = Conv[(int)sqrt(FCD.C.R*FCD.C.R+FCD.C.G*FCD.C.G+FCD.C.B*FCD.C.B)];
      K = FCD.C.R*F;
      if (K>255) FCD.C.R=255; else FCD.C.R=K;
      K = FCD.C.G*F;
      if (K>255) FCD.C.G=255; else FCD.C.G=K;
      K = FCD.C.B*F;
      if (K>255) FCD.C.B=255; else FCD.C.B=K;
      *P++ = FCD.DW;
    }
}

// Saves the (R,G,B) intensity inside the Alpha channel.
void Intensity_Alpha(Image *Img)
{
  DWord *Data = Img->Data,*DE = Data + Img->X*Img->Y;
  DWord C;
  long R,G,B,A;

  for(;Data<DE;Data++)
  {
    C = *Data;
    R = (C&0x00FF0000)>>16;
    G = (C&0x0000FF00)>>8;
    B = C&0x000000FF;
    A = ((long)((sqrt(R*R+G*G+B*B)*255.0)/443.5))<<24;

    // Apply intensity value to alpha channel:
    *Data &= 0x00FFFFFF;
    *Data += A;
  }
}

void Image_Convulate_3x3(Image *Img,Matrix M)
{
  long I,J;
  DWord *Conv = new DWord[Img->X*Img->Y];
  long X4 = Img->X<<2;
  char *P = (char *)Conv;
  char *R = (char *)Img->Data;
  //Row 0: use middle line values for upper ones
  //Col 0: corner
  *P++=fabs((*R)*(M[0][0]+M[0][1]+M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4))*(M[2][0]+M[2][1])+(*(R+X4+4))*M[2][2]); R++;
  *P++=fabs((*R)*(M[0][0]+M[0][1]+M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4))*(M[2][0]+M[2][1])+(*(R+X4+4))*M[2][2]); R++;
  *P++=fabs((*R)*(M[0][0]+M[0][1]+M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4))*(M[2][0]+M[2][1])+(*(R+X4+4))*M[2][2]); R++;
  *P++=fabs((*R)*(M[0][0]+M[0][1]+M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4))*(M[2][0]+M[2][1])+(*(R+X4+4))*M[2][2]); R++;
  //Col 1 to xr-2: upper row
  for(I=1;I<Img->X-1;I++)
  {
    *P++=fabs((*(R-4))*(M[0][0]+M[0][1])+(*R)*(M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
    *P++=fabs((*(R-4))*(M[0][0]+M[0][1])+(*R)*(M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
    *P++=fabs((*(R-4))*(M[0][0]+M[0][1])+(*R)*(M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
		*P++=fabs((*(R-4))*(M[0][0]+M[0][1])+(*R)*(M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
	}
	//Col xr-1: corner
	*P++=fabs((*(R-4))*(M[0][0]+M[0][1])+(*R)*(M[1][0]+M[1][1]+M[0][2]+M[1][2])+(*(R+X4-4))*M[2][0]+(*(R+X4))*(M[2][1]+M[2][2])); R++;
	*P++=fabs((*(R-4))*(M[0][0]+M[0][1])+(*R)*(M[1][0]+M[1][1]+M[0][2]+M[1][2])+(*(R+X4-4))*M[2][0]+(*(R+X4))*(M[2][1]+M[2][2])); R++;
	*P++=fabs((*(R-4))*(M[0][0]+M[0][1])+(*R)*(M[1][0]+M[1][1]+M[0][2]+M[1][2])+(*(R+X4-4))*M[2][0]+(*(R+X4))*(M[2][1]+M[2][2])); R++;
	*P++=fabs((*(R-4))*(M[0][0]+M[0][1])+(*R)*(M[1][0]+M[1][1]+M[0][2]+M[1][2])+(*(R+X4-4))*M[2][0]+(*(R+X4))*(M[2][1]+M[2][2])); R++;

	for(J=1;J<Img->Y-1;J++)
		for(I=0;I<Img->X;I++)
		{
			//Col 1 to xr-2: main image
			*P++=fabs((*(R-X4-4))*M[0][0]+(*(R-X4))*M[0][1]+(*(R-X4+4))*M[0][2]+(*(R-4))*M[1][0]+(*R)*M[1][1]+(*(R+4))*M[1][2]+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
			*P++=fabs((*(R-X4-4))*M[0][0]+(*(R-X4))*M[0][1]+(*(R-X4+4))*M[0][2]+(*(R-4))*M[1][0]+(*R)*M[1][1]+(*(R+4))*M[1][2]+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
			*P++=fabs((*(R-X4-4))*M[0][0]+(*(R-X4))*M[0][1]+(*(R-X4+4))*M[0][2]+(*(R-4))*M[1][0]+(*R)*M[1][1]+(*(R+4))*M[1][2]+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
			*P++=fabs((*(R-X4-4))*M[0][0]+(*(R-X4))*M[0][1]+(*(R-X4+4))*M[0][2]+(*(R-4))*M[1][0]+(*R)*M[1][1]+(*(R+4))*M[1][2]+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
		}
//  delete Img->Data;
//  Img->Data = Conv;
	memcpy(Img->Data,Conv,Img->X*Img->Y<<2);
	delete Conv;
}

void Image_Laplasian(Image *Img)
{
  Matrix M;
  Matrix_Form(M, 0, 1, 0,
                 1,-4, 1,
                 0, 1, 0);
  Image_Convulate_3x3(Img,M);
}

void Image_LPF(Image *Img)
{
  Matrix M;
  float F=1.0/9.0;
  Matrix_Form(M,F,F,F,
                F,F,F,
                F,F,F);
  Image_Convulate_3x3(Img,M);
}

void Image_HPF(Image *Img)
{
  Matrix M;
  Matrix_Form(M,-1,-1,-1,
                -1, 8,-1,
                -1,-1,-1);
  Matrix_Scale(M,1.0/9.0);
  Image_Convulate_3x3(Img,M);
}

void Image_Enhance(Image *Img)
{
  Matrix M;
  Matrix_Form(M,-1,-1,-1,
                -1,17,-1,
                -1,-1,-1);
  Matrix_Scale(M,1.0/9.0);
  Image_Convulate_3x3(Img,M);
}

////////////////////
// Bump mapping - 2D
//

// Parameters: Prim - contains primery Image to render from and to
// BMap - used as height table. if it is NULL then the Alpha channel of
// Prim is used.
// BTbl - Bump Illumination table. This should be an outcome of the
// Phong map routine. Only the blue color will be used.
// (LX,LY) - Light source coordinates.
// Dist - Distribution of the Light across the rendered image.
// Note: The process is Irreversible. If you want a copy of the original
// Image, create a copy before calling this routine.
void Bump_Image_2D(Image *Prim,Image *BMap,Image *BTbl,long LX,long LY)
{
  long X,Y,CX,CY,mx,my,MX,MY,FX,FY,YOL,YOU,Mod;
  char *Modulation;
  char *D1,*D2,*D3;

  // Several Checks on the passed parameters.
  if (!Prim) return;
  if (BMap)
    if ((BMap->X!=Prim->X)||(BMap->Y!=Prim->Y)) return;   
  if (!BTbl) return;

  // Prepare some variables
  YOL = 3+(Prim->X<<2);
  YOU = 3-(Prim->X<<2);
  D1 = (char *)Prim->Data;
  D3 = (char *)BTbl->Data;
  Make_Modulation(); //whatever, just make it work
  CX = BTbl->X>>1;
  CY = BTbl->Y>>1;
  mx = CX-LX;
  my = CY-LY;
  MX = mx + Prim->X;
  MY = my + Prim->Y;


  if (BMap)
  {
    D2 = (char *)BMap->Data;
  } else {
    // No Bumpmap - use Alpha.
    D1 += (Prim->X<<2);
    for(Y=my+1;Y<MY-1;Y++)
    {
      D1 += 4;
      for(X=mx+1;X<MX-1;X++)
      {
        // current Pixel - D1.
        // location in table should be offsetted by bump
        FX = X - (*(D1+3)) + (*(D1-1));
        FY = Y - (*(D1+YOL)) + (*(D1+YOU));
        if (FX<0) FX = 0; if (FX>=BTbl->X) FX=BTbl->X-1;
        if (FY<0) FY = 0; if (FY>=BTbl->Y) FY=BTbl->Y-1;
        // modulate with table
        Modulation = ModTbl + (D3[(FX+FY*BTbl->X)<<2]<<8);
        *D1 = Modulation[(*D1)];
        D1[1] = Modulation[D1[1]];
        D1[2] = Modulation[D1[2]];
        // modulate w/o table
/*        Mod = D3[(FX+FY*BTbl->X)<<2];
        *D1 = *D1*Mod>>8;
        D1[1] = D1[1]*Mod>>8;
        D1[2] = D1[2]*Mod>>8;*/
        D1+=4;
      }
      D1 += 4;
    }
  }
}

// Solves a Ray-tracing equation: t-1=A*sin(t*sqrt(x*x+y*y)*F+O).
float RippleEq(float A,float F,float O,float X,float Y)
{
  float Mag = sqrt(X*X+Y*Y)*F;
  float t1 = 0.0,t2 = 2.0,f1,f2;
  float t,f;
  long iter = 15;

  f1 = t1-1.0f-A*sin(t1*Mag+O);
  f2 = t2-1.0f-A*sin(t2*Mag+O);
  while (t2-t1>0.001&&iter--)
  {
    if (f1*f2>0) return t1;
    t = (t1*f2-t2*f1)/(f2-f1);
    f = t-1.0f-A*sin(t*Mag+O);
    if (f*f1<0) {t2 = t; f2 = f;}
    else {t1 = t; f1 = f;}
  }
  return t;
}

// Image Precision Ripple wave with shading.
// Parameters: Prim - Primery Image to Render
// HMap - Target Image for Height map
// Amp - Wave Amplitute
// Freq - Wave Frequency
void Image_Ripple(Image *Prim,float Amp,float Freq,float Ofs)
{
  long X,Y,mX,mY,MX,MY;
  long Disp;
  DWord *DD;
  float t;
  DWord *ND = new DWord[Prim->X*Prim->Y],*CD = ND;

  mX = -Prim->X>>1; MX = mX + Prim->X;
  mY = -Prim->Y>>1; MY = mY + Prim->Y;

  Disp = Prim->X*MX+MY;
  DD = Prim->Data + Disp;
  for(Y=mY;Y<MY;Y++)
    for(X=mX;X<MX;X++)
    {
      t = RippleEq(Amp,Freq,Ofs,X,Y);
      *CD++ = DD[(int)(X*t)+(int)(Y*t)*Prim->X];
    }
  delete Prim->Data;
  Prim->Data = ND;
}