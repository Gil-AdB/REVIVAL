#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
//#include <conio.h>

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FDS_DEFS.H"

struct Four_C
{
	uint8_t R,G,B,A;
};
union FCDW
{
	Four_C C;
	DWord DW;
};

DWord TblFlags = 0;
uint8_t*ModTbl;

/// Table Handlers

// Modulation table - takes up 64Kb. It is used for fast lookup for
// the simple operation m(I,J) = I*J>>8. This takes several cycles
// to compute and is found inside several inner loops requiring modulation
// (mainly for Illumination of textures)
void Make_Modulation()
{
	int32_t I,J,K;
	uint8_t *W;
	if (TblFlags&TblMod_Made) return;
	TblFlags|=TblMod_Made;
	// Generate a Multiplicative (Modulation) table
	ModTbl = new uint8_t[65536]; // should have been aligned.
	W = ModTbl;
	for(J=0;J<256;J++)
		for(I=0;I<256;I++)
		{
			K = I*J*3>>9;
			if (K>255) *W++ = 255; else *W++ = uint8_t(K);
		}
}

//void Fast16232(DWord *Dst,Word *Src,int32_t Num)
//{
//	__asm
//	{
//		Mov ECx,Num
//			Mov EDI,Dst
//			Mov ESI,Src
//			LEA EDI,[EDI+ECx*4]
//			LEA ESI,[ESI+ECx*2]
//			Xor ECx,-1
//			Inc ECx
//Inner:
//			mov ax, word ptr [ESI+ECx*2]
//			Shl EAx,3
//			Ror EAx,8
//			Shl Ax,2
//			Shl Ah,3
//			Rol EAx,8
//			Mov dword ptr [EDI+ECx*4],EAx
//			Inc ECx
//			JNZ Inner
//	}
//}
//
//void Fast32216(Word *Dst,DWord *Src,int32_t Num)
//{
//	__asm
//	{
//		Mov ECx,Num
//			Mov EDI,Dst
//			Mov ESI,Src
//			LEA EDI,[EDI+ECx*2]
//			LEA ESI,[ESI+ECx*4]
//			Xor ECx,-1
//			Inc ECx
//Inner:
//			Mov EAx,dword ptr [ESI+ECx*4]
//			Shr Ah,2
//			Shl Ax,2
//			Ror EAx,16 //; Partial stall - 6 Cycles
//			Shr Al,3
//			Rol EAx,11 //; Partial stall - 6 Cycles
//			Mov word ptr [EDI+ECx*2],Ax
//			Inc ECx
//			JNZ Inner
//	}
//}
//
//void T16Conv(Word **Data,int32_t OldX,int32_t OldY,int32_t X,int32_t Y)
//{
//	Image Img;
//	New_Image(&Img,OldX,OldY);
//	Fast16232(Img.Data,*Data,OldX*OldY);
//	delete *Data;
//	Scale_Image(&Img,X,Y);
//	*Data = (Word *)Img.Data;
//	*Data = new Word[X*Y];
//	Fast32216(*Data,Img.Data,X*Y);
//	delete Img.Data;
//}

// Mip-maps a given Image by simple Texel averaging
void MipmapXY(Image *Img)
{
	int32_t X,Y;
	dword *Mip = new dword[((Img->x+1)>>1)*((Img->y+1)>>1)];
	byte *Trg = (byte *)Mip;
	byte *Src = (byte *)Img->Data;
	byte  *Tex;
	int32_t X4 = Img->x<<2,Y4 = Img->y<<2;
	int32_t X4_4 = X4+4;
	
	for(Y=0;Y<Img->y>>1;Y++)
	{
		for(X=0;X<Img->x>>1;X++)
		{
			Tex = Src+((X+Y*Img->x)<<3);
			*Trg++ = ((*Tex)+(*(Tex+4))+(*(Tex+X4))+(*(Tex+X4_4)))>>2; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4))+(*(Tex+X4))+(*(Tex+X4_4)))>>2; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4))+(*(Tex+X4))+(*(Tex+X4_4)))>>2; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4))+(*(Tex+X4))+(*(Tex+X4_4)))>>2; Tex++;
		}
		// copy last texel as a two-texel average
		if (Img->x&1)
		{
			Tex = Src+(X4-4+Y*X4);
			*Trg++ = ((*Tex)+(*(Tex+X4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+X4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+X4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+X4)))>>1; Tex++;
		}
	}
	if (Img->y&1)
	{
		Y = Img->y-1;
		// copy last row as two-texel average
		for(X=0;X<Img->x>>1;X++)
		{
			Tex = Src+((X+Y*Img->x)<<2);
			*Trg++ = ((*Tex)+(*(Tex+4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4)))>>1; Tex++;
		}
		if (Img->x&1) // copy last texel as it is
			*((dword *)Trg) = *((dword *)Tex);
	}
	free(Img->Data);
	Img->x = (Img->x+1)>>1;
	Img->y = (Img->y+1)>>1;
	Img->Data = Mip;
}

// Mip-maps a given Image by simple Texel averaging, works on 2x1 blocks
void MipmapX(Image *Img)
{
	int32_t X,Y;
	dword *Mip = new dword[((Img->x+1)>>1)*Img->y];
	byte *Trg = (byte *)Mip;
	byte *Src = (byte *)Img->Data;
	byte *Tex;
	int32_t X4 = Img->x<<2,Y4 = Img->y<<2;
	int32_t X4_4 = X4+4;
	
	for(Y=0;Y<Img->y;Y++)
	{
		for(X=0;X<Img->x>>1;X++)
		{
			Tex = Src+(((X<<1)+Y*Img->x)<<2);
			*Trg++ = ((*Tex)+(*(Tex+4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+4)))>>1; Tex++;
		}
		// copy last texel as it is
		if (Img->x&1)
		{
			Tex = Src+(X4-4+Y*X4);
			*((dword *)Trg) = *((dword *)Tex);
		}
	}
	free(Img->Data);
	Img->x = (Img->x+1)>>1;
	Img->Data = Mip;
}

// Mip-maps a given Image by simple Texel averaging, works on 1x2 blocks
void MipmapY(Image *Img)
{
	int32_t X,Y;
	dword *Mip = new dword[Img->x*((Img->y+1)>>1)];
	byte *Trg = (byte *)Mip;
	byte *Src = (byte *)Img->Data;
	byte *Tex;
	int32_t X4 = Img->x<<2,Y4 = Img->y<<2;
	int32_t X4_4 = X4+4;
	
	for(Y=0;Y<Img->y>>1;Y++)
	{
		for(X=0;X<Img->x;X++)
		{
			Tex = Src+((X+(Y<<1)*Img->x)<<2);
			*Trg++ = ((*Tex)+(*(Tex+X4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+X4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+X4)))>>1; Tex++;
			*Trg++ = ((*Tex)+(*(Tex+X4)))>>1; Tex++;
		}
	}
	if (Img->y&1)
	{
		Y = Img->y-1;
		// copy last row as it is
		memcpy(Trg,Src+((Y*Img->x)<<2),X4);
	}
	free(Img->Data);
	Img->y = (Img->y+1)>>1;
	Img->Data = Mip;
}

// Converts a 256x256 Texture to the Digital Iamge format.
void Convert_Texture2Image(Texture *Tx,Image *Img)
{
	Texture *TT = new Texture;
	TT->BPP = Tx->BPP;
	TT->LSizeX = Tx->LSizeX;
	TT->LSizeY = Tx->LSizeY;
	Img->x = 1 << Tx->LSizeX;
	Img->y = 1 << Tx->LSizeY;
	
	size_t nPixels = size_t{ 1 } << (Tx->LSizeX + Tx->LSizeY);
	if (TT->BPP!=32)
	{
		// make a new texture, convert it, and hand over the data
		TT->Data = new byte[nPixels*((TT->BPP+1)>>3)];
		memcpy(TT->Data,Tx->Data, nPixels*((TT->BPP+1)>>3));
		BPPConvert_Texture(TT,32);
		Img->Data = (DWord *)TT->Data;
	} else {
		// copy txtr as it is
		Img->Data = (dword *)_aligned_malloc(sizeof(dword)*nPixels, 16);
		memcpy(Img->Data,Tx->Data, sizeof(dword)*nPixels);
	}
	delete TT;
}

// Converts Digital Image to a usable 256x256 Texture.
void Convert_Image2Texture(Image *Img,Texture *Tx)
{
	Image *TI = new Image;
	int32_t I,J,X4,X4_4;
	float X,Y,dX,dY,iX,iY,lX,lY,rX,rY,rXrY,rXlY,lXrY,lXlY;
	byte *W,*R,*RY,*RO;
	// copy original Image to TI
	TI->x = Img->x;
	TI->y = Img->y;
	TI->Data = new dword[TI->x*TI->y];
	memcpy(TI->Data,Img->Data,TI->x*TI->y<<2);
	
	// Mipmap image until it's smaller than Texture size (256x256)
	while (TI->x>256||TI->y>256)
	{
		if (TI->x>256&&TI->y>256) {MipmapXY(TI); continue;}
		if (TI->x>256) {MipmapX(TI); continue;}
		if (TI->y>256) {MipmapY(TI); continue;}
	}
	// Bilinear Filtering
	if (Tx->Data) delete [] Tx->Data;
	Tx->Data = new byte [262144];
	if (TI->x<256||TI->y<256)
	{
		R = (byte *)TI->Data;
		W = (byte *)Tx->Data;
		X4 = TI->x<<2;
		X4_4 = X4+4;
		dX = (float)(TI->x-1.0)/256.0f;
		dY = (float)(TI->y-1.0)/256.0f;
		Y = 0.0;
		for(J=0;J<256;J++)
		{
			X = 0.0;
			iY = floorf(Y);
			lY = Y-iY;
			rY = 1.0f-lY;
			RY = R+((((int)iY)*TI->x)<<2);
			for(I=0;I<256;I++)
			{
				iX = floorf(X);
				lX = X-iX;
				rX = 1.0f-lX;
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
	delete [] TI->Data;
	delete TI;
	
	// Convert to requested bpp
	I = Tx->BPP;
	Tx->BPP=32;
	Tx->SizeX = 256; Tx->LSizeX = 8;
	Tx->SizeY = 256; Tx->LSizeY = 8;	
	BPPConvert_Texture(Tx,I);
}

// A Mipmapping, Bi-linear scaler.
void Scale_Image(Image *Img,int32_t NX,int32_t NY)
{
	Image *TI = new Image,*BI;
	int32_t I,J,X4,X4_4;
	float X,Y,dX,dY,iX,iY,lX,lY,rX,rY,rXrY,rXlY,lXrY,lXlY;
	byte *W,*R,*RY,*RO;
	// copy original Image to TI
	TI->x = Img->x;
	TI->y = Img->y;
	TI->Data = (DWord *)_aligned_malloc(sizeof(DWord) *TI->x*TI->y, 16);
	memcpy(TI->Data,Img->Data,TI->x*TI->y<<2);
	
	// Mipmap image until it's smaller than specified size
	while (TI->x>NX||TI->y>NY)
	{
		if (TI->x>NX&&TI->y>NY) {MipmapXY(TI); continue;}
		if (TI->x>NX) {MipmapX(TI); continue;}
		if (TI->y>NY) {MipmapY(TI); continue;}
	}
	// Bilinear Filtering
	_aligned_free(Img->Data);
	Img->Data = (DWord *)_aligned_malloc(sizeof(DWord)*NX*NY, 16);
	Img->x = NX;
	Img->y = NY;
	if (TI->x<NX||TI->y<NY)
	{
		R = (byte *)TI->Data;
		W = (byte *)Img->Data;
		X4 = TI->x<<2;
		X4_4 = X4+4;
		dX = (float)(TI->x-1.01)/NX;
		dY = (float)(TI->y-1.01)/NY;
		Y = 0.0;
		for(J=0;J<NY;J++)
		{
			X = 0.0;
			iY = floor(Y);
			lY = Y-iY;
			rY = 1.0-lY;
			RY = R+((((int)iY)*TI->x)<<2);
			for(I=0;I<NX;I++)
			{
				iX = floor(X);
				lX = X-iX;
				rX = 1.0-lX;
				RO = RY+((int)iX<<2);
				rXrY = rX*rY; lXrY=lX*rY; rXlY = rX*lY; lXlY = lX*lY;
				
				*W++ = rXrY*RO[0]+lXrY*RO[4]+rXlY*RO[X4]+lXlY*RO[X4_4]; RO++;
				*W++ = rXrY*RO[0]+lXrY*RO[4]+rXlY*RO[X4]+lXlY*RO[X4_4]; RO++;
				*W++ = rXrY*RO[0]+lXrY*RO[4]+rXlY*RO[X4]+lXlY*RO[X4_4]; RO++;
				*W++ = rXrY*RO[0]+lXrY*RO[4]+rXlY*RO[X4]+lXlY*RO[X4_4];
//				*W++ = *RO++;
//				*W++ = *RO++;
//				*W++ = *RO++;
//				*W++ = *RO++;
				X+=dX;
			}
			Y+=dY;
		}
	} else memcpy(Img->Data,TI->Data,4*NX*NY);
	_aligned_free(TI->Data);
	delete TI;
}



/*void Image_Integral_Scaler(Image *Img,float X,float Y)
{
Image Temp;
int32_t I,J;
float x = 0,dx,y = 0,dy;
int iX,iY;
float R,G,B;

  Temp.x = X;
  Temp.y = Y;
  Temp.Data = new DWord[X*Y];
  
	dx = X/Img->x;
	dy = Y/Img->y;
	y = 0;
	for(J=0;J<Img->y;J++)
	{
	x = 0;
	for(I=0;I<Img->x;I++)
	{
	R = G = B = 0;
	// integral of the color function, on [x,x+dx)x[y,y+dy).
	for(iX=floor(x);iX<=ciel(x+dx);iX++)
	{
	
	  }
	  x += dx;
	  }
	  y += dy;
	  }
}*/

// Performs Gamma correction on the Image. Values between 0 and 1 darken it,
// while values greater than 1 may require clipping and reduce image detail.
void Gamma_Correction(Image *Img,float Gamma)
{
	int32_t I,J,K;
	FCDW FCD;
	DWord *P = Img->Data;
	if (Gamma<0) return;
	if (Gamma<=1.0)
	{
		for(J=0;J<Img->y;J++)
			for(I=0;I<Img->x;I++)
			{
				FCD.DW = *P;
				FCD.C.R*=Gamma;
				FCD.C.G*=Gamma;
				FCD.C.B*=Gamma;
				*P++ = FCD.DW;
			}
	} else {
		for(J=0;J<Img->y;J++)
			for(I=0;I<Img->x;I++)
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
	int32_t Hist[444];
	float Conv[444];
	int32_t I,J,K;
	float F;
	memset(Hist,0,444*4);
	DWord *P = Img->Data;
	float rt = 444.0/(float)(Img->x*Img->y);
	FCDW FCD;
	
	for(J=0;J<Img->y;J++)
		for(I=0;I<Img->x;I++)
		{
			FCD.DW = *P++;
			Hist[(int)sqrt((float)(FCD.C.R*FCD.C.R+FCD.C.G*FCD.C.G+FCD.C.B*FCD.C.B))]++;
		}
		for(I=1;I<444;I++)
			Hist[I]+=Hist[I-1];
		//now we got addative statistics...
		//Convertion rates
		for(I=0;I<444;I++)
			Conv[I]=(float)(Hist[I]*rt)/(float)I;
		P = Img->Data;
		for(J=0;J<Img->y;J++)
			for(I=0;I<Img->x;I++)
			{
				FCD.DW = *P;
				F = Conv[(int)sqrt((float)(FCD.C.R*FCD.C.R+FCD.C.G*FCD.C.G+FCD.C.B*FCD.C.B))];
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
	DWord *Data = Img->Data,*DE = Data + Img->x*Img->y;
	DWord C;
	int32_t R,G,B,A;
	
	for(;Data<DE;Data++)
	{
		C = *Data;
		R = (C&0x00FF0000)>>16;
		G = (C&0x0000FF00)>>8;
		B = C&0x000000FF;
		A = ((int32_t)((sqrt((float)(R*R+G*G+B*B))*255.0)/443.5))<<24;
		
		// Apply intensity value to alpha channel:
		*Data &= 0x00FFFFFF;
		*Data += A;
	}
}

void Image_Convulate_3x3(Image *Img,Matrix M)
{
	int32_t I,J;
	DWord *Conv = new DWord[Img->x*Img->y];
	int32_t X4 = Img->x<<2;
	byte *P = (byte *)Conv;
	byte *R = (byte *)Img->Data;
	//Row 0: use middle line values for upper ones
	//Col 0: corner
	*P++=fabs((*R)*(M[0][0]+M[0][1]+M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4))*(M[2][0]+M[2][1])+(*(R+X4+4))*M[2][2]); R++;
	*P++=fabs((*R)*(M[0][0]+M[0][1]+M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4))*(M[2][0]+M[2][1])+(*(R+X4+4))*M[2][2]); R++;
	*P++=fabs((*R)*(M[0][0]+M[0][1]+M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4))*(M[2][0]+M[2][1])+(*(R+X4+4))*M[2][2]); R++;
	*P++=fabs((*R)*(M[0][0]+M[0][1]+M[1][0]+M[1][1])+(*(R+4))*(M[0][2]+M[1][2])+(*(R+X4))*(M[2][0]+M[2][1])+(*(R+X4+4))*M[2][2]); R++;
	//Col 1 to xr-2: upper row
	for(I=1;I<Img->x-1;I++)
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
	
	for(J=1;J<Img->y-1;J++)
		for(I=0;I<Img->x;I++)
		{
			//Col 1 to xr-2: main image
			*P++=fabs((*(R-X4-4))*M[0][0]+(*(R-X4))*M[0][1]+(*(R-X4+4))*M[0][2]+(*(R-4))*M[1][0]+(*R)*M[1][1]+(*(R+4))*M[1][2]+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
			*P++=fabs((*(R-X4-4))*M[0][0]+(*(R-X4))*M[0][1]+(*(R-X4+4))*M[0][2]+(*(R-4))*M[1][0]+(*R)*M[1][1]+(*(R+4))*M[1][2]+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
			*P++=fabs((*(R-X4-4))*M[0][0]+(*(R-X4))*M[0][1]+(*(R-X4+4))*M[0][2]+(*(R-4))*M[1][0]+(*R)*M[1][1]+(*(R+4))*M[1][2]+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
			*P++=fabs((*(R-X4-4))*M[0][0]+(*(R-X4))*M[0][1]+(*(R-X4+4))*M[0][2]+(*(R-4))*M[1][0]+(*R)*M[1][1]+(*(R+4))*M[1][2]+(*(R+X4-4))*M[2][0]+(*(R+X4))*M[2][1]+(*(R+X4+4))*M[2][2]); R++;
		}
		//  delete Img->Data;
		//  Img->Data = Conv;
		memcpy(Img->Data,Conv,Img->x*Img->y<<2);
		delete [] Conv;
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
void Bump_Image_2D(Image *Prim,Image *BMap,Image *BTbl,int32_t LX,int32_t LY)
{
	int32_t X,Y,CX,CY,mx,my,MX,MY,FX,FY,YOL,YOU,Mod;
	uint8_t *Modulation;
	byte *D1,*D2,*D3;
	
	// Several Checks on the passed parameters.
	if (!Prim) return;
	if (BMap)
		if ((BMap->x!=Prim->x)||(BMap->y!=Prim->y)) return;   
		if (!BTbl) return;
		
		// Prepare some variables
		YOL = 3+(Prim->x<<2);
		YOU = 3-(Prim->x<<2);
		D1 = (byte *)Prim->Data;
		D3 = (byte *)BTbl->Data;
		Make_Modulation(); //whatever, just make it work
		CX = BTbl->x>>1;
		CY = BTbl->y>>1;
		mx = CX-LX;
		my = CY-LY;
		MX = mx + Prim->x;
		MY = my + Prim->y;
		
		
		if (BMap)
		{
			D2 = (byte *)BMap->Data;
		} else {
			// No Bumpmap - use Alpha.
			D1 += (Prim->x<<2);
			for(Y=my+1;Y<MY-1;Y++)
			{
				D1 += 4;
				for(X=mx+1;X<MX-1;X++)
				{
					// current Pixel - D1.
					// location in table should be offsetted by bump
					FX = X - (*(D1+3)) + (*(D1-1));
					FY = Y - (*(D1+YOL)) + (*(D1+YOU));
					if (FX<0) FX = 0; if (FX>=BTbl->x) FX=BTbl->x-1;
					if (FY<0) FY = 0; if (FY>=BTbl->y) FY=BTbl->y-1;
					// modulate with table
					Modulation = ModTbl + (D3[(FX+FY*BTbl->x)<<2]<<8);
					*D1 = Modulation[(*D1)];
					D1[1] = Modulation[D1[1]];
					D1[2] = Modulation[D1[2]];
					// modulate w/o table
					/*        Mod = D3[(FX+FY*BTbl->x)<<2];
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
	int32_t iter = 15;
	
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
/*void Image_Ripple(Image *Prim,float Amp,float Freq,float Ofs)
{
int32_t X,Y,mX,mY,MX,MY;
int32_t Disp;
DWord *DD;
float t;
DWord *ND = new DWord[Prim->x*Prim->y],*CD = ND;

  mX = -Prim->x>>1; MX = mX + Prim->x;
  mY = -Prim->y>>1; MY = mY + Prim->y;
  
	Disp = Prim->x*MX+MY;
	DD = Prim->Data + Disp;
	for(Y=mY;Y<MY;Y++)
	for(X=mX;X<MX;X++)
	{
	t = RippleEq(Amp,Freq,Ofs,X,Y);
	*CD++ = DD[(int)(X*t)+(int)(Y*t)*Prim->x];
	}
	delete Prim->Data;
	Prim->Data = ND;
}*/

DWord *RPLDTBL = NULL;

void Make_RPLTBL(int32_t X,int32_t Y,float Prec)
{
	RPLDTBL = new DWord[X*Y];
	DWord *DW = RPLDTBL;
	int32_t I,J,mX,mY,MX,MY;
	
	mX = -X>>1; MX = mX + X;
	mY = -Y>>1; MY = mY + Y;
	
	for(J=mY;J<MY;J++)
		for(I=mX;I<MX;I++)
			*DW++ = Prec*sqrt((float)(I*I+(J+80)*(J+80)));
}

static float *STbl = NULL;

void Make_STbl(float Freq,int32_t Steps)
{
	float x = 0.0f;
	STbl = new float[Steps];
	float *f = STbl;
	while(Steps--)
	{
		*f++ = sin(x);
		x+=Freq;
	}
}


void Image_Ripple(Image *Prim,float Amp,float Freq,float Ofs)
{
	int32_t X,Y,mX,mY,MX,MY,KX,KY;
	int32_t Disp,Off;
	DWord *DD;
	float t;
	DWord DW;
	DWord *R_T;
	DWord *ND = new DWord[Prim->x*Prim->y],*CD = ND;
	
	mX = -Prim->x>>1; MX = mX + Prim->x;
	mY = -Prim->y>>1; MY = mY + Prim->y;
	
	if (!RPLDTBL)
		Make_RPLTBL(Prim->x,Prim->y,16);
	if (!STbl)
		Make_STbl(Freq,10000);
	
	R_T = RPLDTBL;
	Off = Ofs*16;
	
	Disp = Prim->x*MY+MX;
	DD = Prim->Data + Disp;
	for(Y=mY;Y<MY;Y++)
		for(X=mX;X<MX;X++)
		{
			//displacement function
			t = 1.0 + Amp*(STbl[*R_T++ + Off]-1.0f);
			
			DW = DD[(int)(X*t)+(int)(Y*t)*Prim->x];
			//			if ((DW>>24)>50)
			*CD++ = DW;
			//			else
			//			*CD++ = DD[X+Y*Prim->x];
			//t = RippleEq(Amp,Freq,Ofs,X,Y);
			//*CD++ = DD[(int)(X*t)+(int)(Y*t)*Prim->x];
		}
		delete Prim->Data;
		Prim->Data = ND;
}
