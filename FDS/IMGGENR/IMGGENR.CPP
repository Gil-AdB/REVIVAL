// Image Generation Routines

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/Omni.h"
#include <memory>
#include <simd/vectorclass.h>
#include <FILLERS/SimdHelpers.h>

dword WobNumOfHorizontalBlocks;
dword WobNumOfVerticalBlocks;
dword WobBlockJump;
dword WobBPSL;

dword MMXWobNumOfHorizontalBlocks;
dword MMXWobNumOfVerticalBlocks;
dword MMXWobBlockJump;
dword MMXWobBPSL;

DWord WOBPOINTSHEIGHT;
Material Flare;
char NoiseTblRdy = 0;
float *NoiseTbl;      // Range [0,+2PI] acc 0.01

DWord Colors32[256]; // a 32bit 256 color palette. For use with intensity
// map effects.

Vector *WPDT = NULL;
float WRFOV;

void New_Image(Image *Img, int32_t X, int32_t Y)
{
	Img->x = X; Img->y = Y; Img->Data = (DWord *)_aligned_malloc(sizeof(DWord)*X*Y, 16);
}

// Return Pseudorandom value between -1 and 1. Requires a polynomial
// seed given at x.

float frand1d(int32_t x)
{
	int32_t s = 71 * x; s = s * 8192 ^ s;
	return 1.0 - ((s*(s*s*15731+789221)+1376312589)& 0x7fffffff)/1073741824.0;
}

float Noise1D(float x)
{
	int32_t i;
	float f,n0,n1;
	
	i = floor(x);
	f = x - i;
	n0 = frand1d(i);
	n1 = frand1d(i+1);
	
	return n0 + (n1-n0)*f;
}

void Make_Noise()
{
	int32_t I;
	float f;
	if (!NoiseTblRdy)
	{
		NoiseTblRdy = 1;
		NoiseTbl = new float[6284];
	}
	for(I=0;I<=6283;I++)
	{
		f = I/100.0f;
		NoiseTbl[I] = Noise1D(f);
	}
}


// Generates a Lens Flare at the target Image, the main color is at [RGB].
// All additional variables are considered constant.
void Generate_Flare_Image_Test(Image *Img,float FR,float FG,float FB,float blah,float XDec,float YDec)
{
	int32_t X, Y;
	char  Col;
	float D, R, G, B, F, V ,A ,Bump;
	float Y2;
	float Linear = 0.03;
	float Gauss  = 0.006;
	float Mix    = 0.5;
	float NumLines = 0.1;
	float NoiseFreq = 100.0*blah;
	float NoiseMod = 2;
	float Falloff = 0.02;
	float RMid = 27;
	float RWid = 1.6;
	float Blaze = 0.2;
	float Ring,RMin,RMax,rRW;
	
	char *Pen = (char *)Img->Data;
	float Rx,Ry;
	
	if (!NoiseTblRdy) Make_Noise();
	
	// ring position and width
	
	Rx = Img->x>>1;
	Ry = -(Img->y>>1);
	for (Y=-Ry; Y>Ry; Y--)
	{
		Y2=(Y/*-YDec*/)*(Y/*-YDec*/);
		for (X=-Rx; X<Rx; X++)
		{
			// Get distance from center of flare
			D = ((X/*-XDec*/)*(X/*-XDec*/) + Y2);
			
			// Creates random lines out from the center.
			V = atan2(X-XDec, Y-YDec)+PI; //Angle
			V = (fmod(V/PI*NumLines + 1.0 + NoiseTbl[(int)(V*NoiseFreq)]
				//Noise1D(V*NoiseFreq)
				, 1.0) - 0.5)*NoiseMod;
			V = fabs(V);
			//      V *= V*V*V*V;
			//      V = pow(fabs(V), 1.5);
			
			// Add lines and fade out over distance.
			A = Blaze*V / (1.0 + D*Falloff*0.04);
			
			// Clip to maximum value
			if (A>1) A = 1;
			Col = 255.0 * A;
			*Pen++ = Col;
			*Pen++ = Col;
			*Pen++ = Col;
			Pen++; //Alpha channel
		}
	}
}


// Generates a Lens Flare at the target Image, the main color is at [RGB].
// All additional variables are considered constant.
void Generate_Flare_Image(Image *Img,float FR,float FG,float FB)
{
    //memset(Img->Data, Img->x * Img->y * 4, 0);
    //return;
	int32_t X, Y;
	float D, R, G, B, F, V ,A ,Bump;
	float Y2;
	float Linear = 2.0;
	float Gauss3  = 4.0;
	float Gauss   = Gauss3*Gauss3*Gauss3;
	float Mix    = 0.5;
	float NumLines = 0.2;
	float NoiseFreq = 500.0;
	float NoiseMod = 2;
	float Falloff = 15.0;
	float RMid = 0.5;
	float RWid = 0.02;
	float Blaze = 0.3;
	float Ring,RMin,RMax,rRW;
	
	char *Pen = (char *)Img->Data;
	float Rx,Ry;
	
	if (!NoiseTblRdy) Make_Noise();
	
	// ring position and width
	RMax = RMid + RWid;
	RMin = RMid - RWid;
	rRW = 1.0/RWid;
	
	Rx = Img->x>>1;
	Ry = -(Img->y>>1);

	float factor = 1.0/(float)Rx;

	for (Y=-Ry; Y>Ry; Y--)
	{
		Y2=Y*Y;
		for (X=-Rx; X<Rx; X++)
		{
			// Get distance from center of flare
			D = sqrt(X*X + Y2)*factor;
			Bump = 3.0*exp(-D*Linear)*(1-Mix)-0.2;
			if (Bump<0) Bump=0.0;
			A = -D*D*D*Gauss;
			if (A>-7.0)
			{
				A = 1.4*exp(A)*Mix;
				R = A+Bump*FR;
				G = A+Bump*FG;
				B = A+Bump*FB;
			} else {
				R = Bump*FR;
				G = Bump*FG;
				B = Bump*FB;
			}
			if (R>=1.0&&G>=1.0&&B>=1.0) {*(uint32_t *)Pen = 0x00FFFFFF; Pen+=4; continue;}
			
			if (D<RMin || D>RMax)
				Ring = 0;
			else
			{
				Ring = fabs(D-RMid)*rRW;
				Ring = 1.0 - Ring*Ring*(3 - 2*Ring); //Polynom shit
				Ring *= 0.20;
			}
			
			R += Ring*FR;
			G += Ring*FG;
			B += Ring*FB;
			
			// Creates random lines out from the center.
			V = atan2((float)X, (float)Y)+PI; //Angle
			V = (fmod(V/PI*NumLines + 1.0 + NoiseTbl[(int)(V*NoiseFreq)]
				//Noise1D(V*NoiseFreq)
				, 1.0) - 0.5)*NoiseMod;
			V *= V;
			//V = pow(fabs(V), 1.5);
			//V = fabs(V);
			
			// Add lines and fade out over distance.
			A = Blaze*V / (1.0 + D*D*Falloff);
			R+=A;
			G+=A;
			B+=A;
			
			// Clip to maximum value
			if (R>1) R = 1;
			if (G>1) G = 1;
			if (B>1) B = 1;
			
			*Pen++ = 255.0 * B;
			*Pen++ = 255.0 * G;
			*Pen++ = 255.0 * R;
			Pen++; //Alpha channel
		}
	}
}

void Generate_RGBFlares()
{
	Image Img;
	Img.x = 256;
	Img.y = 256;
	Flare.Flags = Mat_Virtual;
	Flare.Txtr = new Texture;
	memset(Flare.Txtr,0, sizeof(Texture));
	Flare.Txtr->BPP = BPP;
	Img.Data = new DWord[256*256];
//	Generate_Flare_Image(&Img,1.0,0.2,0.1);
	Generate_Flare_Image(&Img,0.1,0.2,1.0);
	Convert_Image2Texture(&Img,Flare.Txtr);
/*	Flare_IG.Flags = Mat_Virtual;
	Flare_IG.Txtr = new Texture;
	Flare_IG.Txtr->BPP = BPP;
	Generate_Flare_Image(&Img,0.3,1.0,0.3);
	Convert_Image2Texture(&Img,Flare_IG.Txtr);
	Flare_IB.Flags = Mat_Virtual;
	Flare_IB.Txtr = new Texture;
	Flare_IB.Txtr->BPP = BPP;
	Generate_Flare_Image(&Img,0.2,0.4,1.0);
	Convert_Image2Texture(&Img,Flare_IB.Txtr);*/
	delete [] Img.Data;
}

Material *Generate_RGBFlare(unsigned char R,unsigned char G,unsigned char B)
{
	Image Img;
	Material *M = getAlignedType<Material>(16); //(Material *)getAlignedBlock(sizeof(Material), 16);
	//memset(M,0,sizeof(Material));
	Img.x = 256;
	Img.y = 256;
	M->Flags = Mat_Virtual;
	M->Txtr = new Texture;
	memset(M->Txtr,0,sizeof(Texture));
	M->Txtr->BPP = BPP;
	Img.Data = new DWord[256*256];
	Generate_Flare_Image(&Img,R/255.0,G/255.0,B/255.0);
	Convert_Image2Texture(&Img,M->Txtr);
	delete [] Img.Data;
	return M;
}

void Fit_FlareImage(Omni *O)
{
//	if ((O->L.R>=O->L.G)&&(O->L.R>=O->L.B)) O->F.Txtr = &Flare_IR;
//    else if ((O->L.G>=O->L.R)&&(O->L.G>=O->L.B)) O->F.Txtr = &Flare_IG;
//	else O->F.Txtr = &Flare_IB;
}

// Fractal Noise (Colour Plasma) generator

byte *FrcNoise;
float FrcNoise_Grain;
static int32_t FN_X;

int32_t Linear_Rand(int32_t Base, int32_t Limit)
{
	int32_t L = Base-Limit+(RAND_15()*Limit>>14);
	if (L<0) L=0;
	if (L>255) L=255;
	return L;
}

void Fract_Noise(int32_t X1, int32_t Y1, int32_t X2, int32_t Y2)
{
	int32_t X,Y;
	byte C11,C12,C21,C22;
	
	if (X2-X1<2&&Y2-Y1<2) return;
	X = (X1+X2)>>1;
	Y = (Y1+Y2)>>1;
	C11 = FrcNoise[(X1+Y1*FN_X)<<2];
	C12 = FrcNoise[(X2+Y1*FN_X)<<2];
	C21 = FrcNoise[(X1+Y2*FN_X)<<2];
	C22 = FrcNoise[(X2+Y2*FN_X)<<2];
	if (!FrcNoise[(X+Y1*FN_X)<<2]) FrcNoise[(X+Y1*FN_X)<<2] = Linear_Rand((C11+C12)>>1,(X2-X1)*FrcNoise_Grain);
	if (!FrcNoise[(X2+Y*FN_X)<<2]) FrcNoise[(X2+Y*FN_X)<<2] = Linear_Rand((C12+C22)>>1,(Y2-Y1)*FrcNoise_Grain);
	if (!FrcNoise[(X+Y2*FN_X)<<2]) FrcNoise[(X+Y2*FN_X)<<2] = Linear_Rand((C22+C21)>>1,(X2-X1)*FrcNoise_Grain);
	if (!FrcNoise[(X1+Y*FN_X)<<2]) FrcNoise[(X1+Y*FN_X)<<2] = Linear_Rand((C21+C11)>>1,(Y2-Y1)*FrcNoise_Grain);
	if (!FrcNoise[(X+Y *FN_X)<<2]) FrcNoise[(X+Y*FN_X)<<2] = (C11+C12+C21+C22)>>2;//Linear_Rand(C11+C12+C21+C22>>2,(X2-X1)*FrcNoise_Grain*0.5);
	
	Fract_Noise(X1,Y1,X,Y);
	Fract_Noise(X,Y1,X2,Y);
	Fract_Noise(X,Y,X2,Y2);
	Fract_Noise(X1,Y,X,Y2);
}

// note : Also generates noise within alpha channel
void Generate_Fractal_Noise(Image *Img,float Graininess)
{
	int32_t I;
	FrcNoise = (byte *)Img->Data;
	FrcNoise_Grain = Graininess;
	FN_X = Img->x;
	I=4;
	while (I--)
	{
		FrcNoise[0] = Linear_Rand(128,64);
		FrcNoise[(FN_X-1)<<2] = Linear_Rand(128,64);
		FrcNoise[((Img->y-1)*FN_X)<<2] = Linear_Rand(128,64);
		FrcNoise[(Img->y*FN_X-1)<<2] = Linear_Rand(128,64);
		Fract_Noise(0,0,Img->x-1,Img->y-1);
		FrcNoise++;
	}
}

static int32_t xseed;

unsigned char cfunc (short x)
{
	if (x&0xff00)
	{
		if (x<0) return 0; else return 255;
	} else return x;
}

short int oldrand (void)
{
	xseed=0x015a4e35*xseed+1;
	return (xseed>>16);
}

int a[55];
int aj;

short int the_rand (void)
{
	aj=(aj+1)%55;
	a[aj]=(a[(aj+1)%55]^a[(aj+2)%55]);
	return (a[aj]);
}

void initrand (void)
{
	aj=0;
	for (int x=0; x<55; x++ )
	{
		a[x]=oldrand();
	}
}

char gb (byte *buf, int32_t x)
{
	//  cout << hex << x << '\n';
	return buf[x<<2];
}

void Generate_Plasma(Image *Img, short p, int32_t seed)
{
	byte *buf = (byte *)Img->Data;

	dword p_input = p;
	dword i=3;
	while (i--)
	{
		xseed=seed;
		initrand();
		buf[0]=the_rand()&255;
		
		uint32_t mx2=0x100;
		uint32_t my2=0x10000l;
		uint32_t x,y;
		
		do {
			uint32_t mx=mx2>>1;
			uint32_t my=my2>>1;
			for (y=0; y<0x10000l; y+=my2)
				for (x=0; x<0x100; x+=mx2)
				{
					short c=2+
						gb(buf,x+y)+
						gb(buf,x+((y+my2)&0xffffl))+
						gb(buf,((x+mx2)&255)+y)+
						gb(buf,((x+mx2)&255)+((y+my2)&0xffffl))+
						((the_rand()*p*3)>>16);
					buf[(x+mx+y+my)<<2]=cfunc (c>>2);
				}
				for (y=0; y<0x10000l; y+=my2)
					for (x=0; x<0x100; x+=mx2)
					{
						short c=2+
							gb(buf, (((x-mx)&255)+y+my))+
							gb(buf, (x+y))+
							gb(buf, (x+mx+y+my))+
							gb(buf, (x+((y+my2)&0xffffl)));
						// ((the_rand()*p)>>16);
						buf[(x+y+my)<<2]=cfunc (c>>2);
					}
					for (y=0; y<0x10000l; y+=my2)
						for (x=0; x<0x100; x+=mx2)
						{
							short c=2+
								gb(buf,x+y)+
								gb(buf,x+mx+((y-my)&0xffffl))+
								gb(buf,((x+mx2)&255)+y)+
								gb(buf,x+mx+y+my);
							// ((the_rand()*p)>>16);
							buf[(x+mx+y)<<2]=cfunc(c>>2);
						}
						mx2=mx;
						my2=my;
						p>>=1;
		} while (mx2>=2);
		p = p_input;
		seed+=2127;
		buf++;
	}
}



// Grid effect generator. Must receive a 256x256 image.

// Interpolation accuracy limit : 8x8 Pixel Granularity.
#define Grid_LOGSubsamp 3
#define Grid_Subsamp 8

int32_t Grid_X,Grid_Y,Grid_XY;
int32_t AGrid_X,AGrid_Y,AGrid_XY;

void Init_PlanarGrid(GridPoint **GP)
{
	Grid_X = (XRes>>Grid_LOGSubsamp)+1;
	Grid_Y = (YRes>>Grid_LOGSubsamp)+1;
	Grid_XY = Grid_X*Grid_Y;
	if (WPDT) delete WPDT;
	WPDT = new Vector[Grid_XY];
	(*GP) = new GridPoint[Grid_X*Grid_Y];
}

// Maps the given effect from GP and Img as a texture, into the DAC
// (Truecolor 32) Surface directly referred as DAC.
/*void Grid_Texture_Mapper(GridPoint *GP,Image *Img,DWord *DAC)
{
long I,J,X,Y;
GridPoint *P11 = GP,*P12,*P21,*P22;
long dU1,dV1,dU2,dV2;
long U1,V1,U2,V2;
long dU,dV,iU,iV;
DWord *XO,*YO = DAC;
DWord *Ptr,*Data = Img->Data;
long J1 = XRes-Grid_Subsamp,J2 = YOffs[Grid_Subsamp];

  for(J=0;J<YRes;J+=Grid_Subsamp)
  {
  XO = YO;
  for(I=0;I<XRes;I+=Grid_Subsamp)
  {
  //P11 holds the currently rendered Grid point
  P12 = P11+1;
  P21 = P11+Grid_X;
  P22 = P21+1;
  
	//Forward differencing (Linear Interpolation rates)
	U1 = P11->U; V1 = P11->V;
	U2 = P12->U; V2 = P12->V;
	
      dU1 = (P21->U-U1)>>Grid_LOGSubsamp;
      dV1 = (P21->V-V1)>>Grid_LOGSubsamp;
      dU2 = (P22->U-U2)>>Grid_LOGSubsamp;
      dV2 = (P22->V-V2)>>Grid_LOGSubsamp;
	  
		Ptr = XO;
		for(Y=0;Y<Grid_Subsamp;Y++)
		{
        // Inner linear interpolation rates
        iU = U1;
        iV = V1;
        dU = U2-iU>>Grid_LOGSubsamp;
        dV = V2-iV>>Grid_LOGSubsamp;
        for(X=0;X<Grid_Subsamp;X++)
        {
		// Inner Rasterization loop (24:8 texture)
		*Ptr++ = Data[(iV&0xFF00)+((iU&0xFF00)>>8)];
		iU+=dU; iV+=dV;
        }
        Ptr += J1;
        U1 += dU1;
        V1 += dV1;
        U2 += dU2;
        V2 += dV2;
		}
		XO += Grid_Subsamp;
		P11++;
		}
		P11++;
		YO+=J2;
		}
} */

// texture grid renderer.
void GridRendererT(GridPointT *GP,Image *Img,dword *page, dword xres, dword yres)
{
	// hopefully, paragraph aligned
//	mmreg c0, c1, dc0dy, dc1dy, c, dcdx, t;

	mword i, j, x, y, gx = (xres / Grid_Subsamp)+1;
	dword *tex = Img->Data;
	
	GridPointT *p[4];
//	GridPointT *p00, *p01, *p10, *p11;		

	p[0] = GP; 
	p[1] = GP + 1;
	p[2] = GP + gx; 
	p[3] = GP + gx + 1;

	dword *block = page, *scanline;

	int32_t YLimit = yres-Grid_Subsamp;
	for(j=0; j<=YLimit; j += Grid_Subsamp)
	{
		for(i=0; i<xres; i += Grid_Subsamp)
		{
			int32_t u0 = p[0]->u;
			int32_t v0 = p[0]->v;
			int32_t u1 = p[1]->u;
			int32_t v1 = p[1]->v;

			int32_t du0dy = (p[2]->u - u0) >> Grid_LOGSubsamp;
			int32_t dv0dy = (p[2]->v - v0) >> Grid_LOGSubsamp;			
			int32_t du1dy = (p[3]->u - u1) >> Grid_LOGSubsamp;
			int32_t dv1dy = (p[3]->v - v1) >> Grid_LOGSubsamp;

			scanline = block;
			for(y=0; y<Grid_Subsamp; y++)
			{
				int32_t u = u0, v = v0;
				int32_t dudx = (u1-u0)>>Grid_LOGSubsamp;
				int32_t dvdx = (v1-v0)>>Grid_LOGSubsamp;
				for(x=0; x<Grid_Subsamp; x++)
				{
					scanline[x] = tex[((u&0xFF00)>>8) + (v&0xFF00)];
					
					u += dudx;
					v += dvdx;
				}
				scanline += xres;
				u0 += du0dy;
				v0 += dv0dy;
				u1 += du1dy;
				v1 += dv1dy;
			}
			block += Grid_Subsamp;
			p[0]++;
			p[1]++;
			p[2]++;
			p[3]++;
		}
		block += (Grid_Subsamp-1) * xres;
		p[0]++;
		p[1]++;
		p[2]++;
		p[3]++;
	}
}

#pragma pack(push, 1)
struct alignas(16) uint128 {
	uint64_t low, high;
};
#pragma pack(pop)


// texture/gouraud grid renderer.
void GridRendererTG(GridPointTG *GP,Image *Img,dword *page, dword XRes, dword YRes)
{
	const auto UScaleFactor = 256.0f;
	const auto VScaleFactor = 256.0f;
	int32_t t0_umask = (1 << 8) - 1;
	int32_t t0_vmask = (1 << 8) - 1;
	int32_t t0_umask_swizzled = swizzle_umask(8, t0_umask);


	// hopefully, paragraph aligned

//	Vec8ui c0, c1, dc0dy, dc1dy, c, dcdx, t;
	dword i, j, x, y, gx = (XRes / Grid_Subsamp) + 1;
	dword* tex = Img->Data;
	GridPointTG* p[4];
	//	GridPointTG *p00, *p01, *p10, *p11;		

	p[0] = GP;
	p[1] = GP + 1;
	p[2] = GP + gx;
	p[3] = GP + gx + 1;

	dword* block = page, * scanline;

	int32_t YLimit = YRes - Grid_Subsamp;
	for (j = 0; j <= YLimit; j += Grid_Subsamp) {
		for (i = 0; i < XRes; i += Grid_Subsamp) {
			auto c0 = p[0]->BGRA;
			auto c1 = p[1]->BGRA;
			auto dc0dy = ((p[2]->BGRA) - c0) >> Grid_LOGSubsamp;
			auto dc1dy = ((p[3]->BGRA) - c1) >> Grid_LOGSubsamp;


			auto u0 = p[0]->u;
			auto v0 = p[0]->v;
			auto u1 = p[1]->u;
			auto v1 = p[1]->v;

			auto du0dy = (p[2]->u - u0) / Grid_Subsamp;
			auto dv0dy = (p[2]->v - v0) / Grid_Subsamp;
			auto du1dy = (p[3]->u - u1) / Grid_Subsamp;
			auto dv1dy = (p[3]->v - v1) / Grid_Subsamp;

			scanline = block;
			for (y = 0; y < Grid_Subsamp; y++) {
				auto dudx = (u1 - u0) / Grid_Subsamp;
				auto dvdx = (v1 - v0) / Grid_Subsamp;
				Vec8f p_u = v8_from_arith_seq(u0, dudx);
				Vec8f p_v = v8_from_arith_seq(v0, dvdx);

				Vec8i u = roundi(p_u);
				Vec8i v = roundi(p_v);

				auto c = c0;
				auto dcdx = (c1 - c0) >> Grid_LOGSubsamp;
				auto color = Vec32us(v32_from_arith_seq(c0, dcdx));


				Vec8i tu = packed_tile_u(u, 8, t0_umask_swizzled);
				Vec8i tv = packed_tile_v(v, t0_vmask);

				auto p_offset = tu + tv;
				const auto texture_samples = colorize<6>(Vec32uc(gather(Vec8ui(p_offset), tex)), color);

				texture_samples.store(scanline);

				scanline += XRes;

				u0 += du0dy;
				v0 += dv0dy;
				u1 += du1dy;
				v1 += dv1dy;
				c0 += dc0dy;
				c1 += dc1dy;
			}
			block += Grid_Subsamp;
			p[0]++;
			p[1]++;
			p[2]++;
			p[3]++;
		}
		block += (Grid_Subsamp - 1) * XRes;
		p[0]++;
		p[1]++;
		p[2]++;
		p[3]++;
	}

}


#define CGrid_Texture_MapASM		Grid_Texture_MapASMV
#define CGrid_Texture_MapASM_320	Grid_Texture_MapASM_320V
#define CGrid_Texture_MapASM_XXX	Grid_Texture_MapASM_XXXV
#define CGrid_Texture_MapASMMX		Grid_Texture_MapASMMXV
#define CGrid_Texture_Mapper_TG		GridRendererTG // Grid_Texture_Mapper_TGV
#define CGrid_Texture_Mapper_T		Grid_Texture_Mapper_TV

//extern "C" void Grid_Texture_MapASM_(void);
//void Grid_Texture_MapASMV(char *Texture,GridPoint *Points,char *VirScr)
//{
//	__asm
//	{
//		mov ecx, [Texture]
//			mov esi, [Points]
//			mov edi, [VirScr]
//			pushad
//			call Grid_Texture_MapASM_
//			popad
//	}
//}
//
//extern "C" void Grid_Texture_MapASM_320_(void);
//void Grid_Texture_MapASM_320V(char *Texture,GridPoint *Points,char *VirScr)
//{
//	__asm
//	{
//		mov ecx, [Texture]
//			mov esi, [Points]
//			mov edi, [VirScr]
//			pushad
//			call Grid_Texture_MapASM_320_
//			popad
//	}
//}
//
//extern "C" void Grid_Texture_MapASM_XXX_(void);
//void Grid_Texture_MapASM_XXXV(char *Texture,GridPoint *Points,char *VirScr)
//{
//	__asm
//	{
//			mov ecx, [Texture]
//			mov esi, [Points]
//			mov edi, [VirScr]
//			pushad
//			call Grid_Texture_MapASM_XXX_
//			popad
//	}
//}
//
//extern "C" void Grid_Texture_MapASMMX_(void);
//extern "C" void Grid_Texture_MapASMMXV(char *Texture,GridPoint *Points,char *VirScr)
//{
//	__asm
//	{
//			mov ecx, [Texture]
//			mov esi, [Points]
//			mov edi, [VirScr]
//			pushad
//;			Inoperative routine (12/04/02)
//;			call Grid_Texture_MapASMMX_
//			popad
//	}
//}

void Grid_Texture_Mapper_TV(char *Texture, GridPointT *Points, char *VirScr)
{
	int Delta_Left_U = 0;
	int Delta_Left_V = 0;
	int Left_U = 0;
	int Left_V = 0;
	int Right_U = 0;
	int Right_V = 0;
	int Delta_Right_U = 0;
	int Delta_Right_V = 0;
	int du = 0;
	int dv = 0;
	int lu = 0;
	int lv = 0;
	int ScanLine = 0;
	int PointsList = 0;
	int x = 0;
	int y = 0;
	int t = 0;
	int Counter = 0;

#define u_ofs		0
#define v_ofs		4
#define ElementSize 8
#undef	u_ofs
#undef	v_ofs
#undef	ElementSize
}


extern "C" void Grid_Texture_Mapper_TG_(void);
void Grid_Texture_Mapper_TGV(char *Texture,GridPointTG *Points,char *VirScr)
{
	int Delta_Left_U = 0;
	int Delta_Left_V = 0;
	int Left_U = 0;
	int Left_V = 0;
	int Right_U = 0;
	int Right_V = 0;
	int Delta_Right_U = 0;
	int Delta_Right_V = 0;
	int du = 0;
	int dv = 0;
	int lu = 0;
	int lv = 0;
	int ScanLine = 0;
	int PointsList = 0;
	//int Texture = 0;
	int x = 0;
	int y = 0;
	int Tebp = 0;
	int t = 0;
	int Counter = 0;
	
#define u_ofs		0
#define v_ofs		4
#define RGBA_ofs	8
#define ElementSize 16
#undef	u_ofs
#undef	v_ofs
#undef	RGBA_ofs
#undef	ElementSize
}

void Grid_Texture_Mapper_TGV_XMM(char *Texture, GridPointTG *Points, char *VirScr)
{
	uint128 DULC;
	uint128 DURC;
	uint128 color1;
	uint128 color2;
	int Delta_Left_U = 0;
	int Delta_Left_V = 0;
	int Left_U = 0;
	int Left_V = 0;
	int Right_U = 0;
	int Right_V = 0;
	int Delta_Right_U = 0;
	int Delta_Right_V = 0;
	int du = 0;
	int dv = 0;
	int lu = 0;
	int lv = 0;
	int ScanLine = 0;
	int PointsList = 0;
	int x = 0;
	int y = 0;
	int t = 0;
	int Counter = 0;

#define u_ofs		0
#define v_ofs		4
#define RGBA_ofs	8
#define ElementSize 16
#undef	u_ofs
#undef	v_ofs
#undef	RGBA_ofs
#undef	ElementSize
}


void Setup_Grid_Texture_Mapper_MMX(dword xres, dword yres)
{
	MMXWobNumOfHorizontalBlocks = (xres / Grid_Subsamp);
	MMXWobNumOfVerticalBlocks = (yres / Grid_Subsamp);
	MMXWobBlockJump = 7 * CPP * xres;
	MMXWobBPSL = xres * CPP;
}


/*void Grid_Texture_Mapper(GridPoint *GP,Image *Img,DWord *DAC)
{
	CGrid_Texture_MapASM((char *)Img->Data,GP,(char *)DAC);
}

void Grid_Texture_Mapper_320(GridPoint *GP,Image *Img,DWord *DAC)
{
	CGrid_Texture_MapASM_320((char *)Img->Data,GP,(char *)DAC);
}

void Setup_Grid_Texture_Mapper_MMX(dword xres, dword yres)
{
	MMXWobNumOfHorizontalBlocks  = xres >> 3;
	MMXWobNumOfVerticalBlocks	 = yres >> 3;
	MMXWobBlockJump				 = 7 * CPP * xres;
	MMXWobBPSL					 = xres * CPP;
}

void Setup_Grid_Texture_Mapper_XXX(dword xres, dword yres)
{
	WobNumOfHorizontalBlocks = xres >> 3;
	WobNumOfVerticalBlocks	 = yres >> 3;
	WobBlockJump			 = 7 * CPP * xres;
	WobBPSL					 = xres * CPP;
}

void Grid_Texture_Mapper_XXX(GridPoint *GP,Image *Img,DWord *DAC)
{
	CGrid_Texture_MapASM_XXX((char *)Img->Data,GP,(char *)DAC);
}

void Grid_Texture_MapASMMX(GridPoint *GP,Image *Img,DWord *DAC)
{
	CGrid_Texture_MapASMMX((char *)Img->Data,GP,(char *)DAC);
}
*/
void Grid_Texture_Mapper_TG(GridPointTG *GP,Image *Img,DWord *DAC, dword x, dword y)
{
	GridRendererTG(GP, Img, DAC, x, y);
	//CGrid_Texture_Mapper_TG((char *)Img->Data,GP,(char *)DAC, XRes, YRes);
}

void Grid_Texture_Mapper_T(GridPointT *GP, Image *Img, DWord *DAC)
{
	CGrid_Texture_Mapper_T((char *)Img->Data, GP, (char *)DAC);
}

void GridFX_Test()
{
/*	long I,J;
	GridPoint *GP,*HP;
	Image Img;
	Init_PlanarGrid(&GP);
	HP=GP;
	for(J=0;J<Grid_Y;J++)
		for(I=0;I<Grid_X;I++)
		{
			HP->U = I*I<<6;
			HP->V = J*J<<6;
			HP++;
		}
		Img.x = 256;
		Img.y = 256;
		Img.Data = new DWord[256*256];
		Generate_Flare_Image(&Img,1.0,0.8,0.2);
		Grid_Texture_Mapper(GP,&Img,(DWord *)VPage);*/
}

int32_t WW_Spots,WW_CS;
float WW_Grav,WW_Pulse;
AGridPoint *WW_Grid,*WW_GO;
AGridPoint *WW_SP;




// Initialize the internal particle system for the whirlwind effect.
// This occurs in Texel space. WW_SP will then be used to store main
// Gravity generators. All particles Repulse each other.
void Init_Whirlwind(char Spots,float Grav,float Repulse)
{
	WW_Grid = Init_Antiwobbler();
	WW_GO = Init_Antiwobbler();
	WW_Spots = Spots;
	WW_CS = 0;
	WW_SP = new AGridPoint[Spots];
	WW_Grav = Grav;
	WW_Pulse = Repulse;
}

// Initializes the next Gravity center.
void Init_WWGrav(float U,float V)
{
	if (WW_CS==WW_Spots) return;
	WW_SP[WW_CS].x = U;
	WW_SP[WW_CS].y = V;
	WW_CS++;
}

// Repositions the particles in a Grid shape in the [0,256]x[0,256] limit
void Reset_WWGrid()
{
	AW_Staticmap(WW_Grid);
}

void Update_WWGrid()
{
	int32_t I,J;
	//  GridPoint *V,*W;
	//  GridPoint F,U;
	float D;
	const float Pulse_Away = 0.001;
	const float Grav_Away = 0.1;
	
	struct {
		float x, y;
	} *V,*W,F,U;
	
	for(I=0;I<AGrid_XY;I++)
	{
		//Calculate Force
		F.x = 0; F.y = 0;
		// Repulsion
		for(J=0;J<AGrid_XY;J+=4)
		{
			if (J==I) continue;
			U.x = (WW_Grid[J].x - WW_Grid[I].x);
			U.y = (WW_Grid[J].y - WW_Grid[I].y);
			D = WW_Pulse/(U.x*U.x+U.y*U.y+Pulse_Away);
			F.x -= D*U.x;
			F.y -= D*U.y;
		}
		// Grav centers (cities)
		for(J=0;J<WW_Spots;J++)
		{
			U.x = (WW_SP[J].x - WW_Grid[I].x);
			U.y = (WW_SP[J].y - WW_Grid[I].y);
			D = WW_Grav/(U.x*U.x+U.y*U.y+Grav_Away);
			F.x += D*U.x;
			F.y += D*U.y;
		}
		D = 1.0/sqrt(F.x*F.x+F.y*F.y);
		WW_GO[I].x = WW_Grid[I].x + F.x*D;
		WW_GO[I].y = WW_Grid[I].y + F.y*D;
	}
	memcpy(WW_Grid,WW_GO,AGrid_XY*sizeof(GridPoint));
}

void Display_WWGrid(Image *Img)
{
//	AntiGrid_Mapper(WW_Grid,Img,(DWord *)VPage);
}

// Texture Space Grid effect will be Implemented here.
#define Awob_Gran 8
#define Awob_LOGGran 3


AGridPoint * Init_Antiwobbler()
{
	AGridPoint *AGP;
	AGrid_X = (256>>Awob_LOGGran)+1;
	AGrid_Y = (256>>Awob_LOGGran)+1;
	AGrid_XY = AGrid_X*AGrid_Y;
	AGP = new AGridPoint[AGrid_X*AGrid_Y];
	return AGP;
}


#define CP_Texture_32 P_Texture_32V

extern "C" void __cdecl P_Texture_32_(void);
static void P_Texture_32V(Bi_Triangle * BTM, char * Bitmap,unsigned int u)
{
}

void AW_Staticmap(AGridPoint *AGP)
{
	int32_t I,J;
	AGridPoint *GP = AGP;
	float QX=XRes*0.1;
	float QY=YRes*0.1;
	float RX=XRes*0.8;
	float RY=YRes*0.8;
	for(J=0;J<AGrid_Y;J++)
		for(I=0;I<AGrid_X;I++)
		{
			GP->x = QX+I*RX/(float)(AGrid_X-1);
			GP->y = QY+J*RY/(float)(AGrid_Y-1);
			GP++;
		}
}

void AW_Distort(AGridPoint *AGP,float Bang)
{
	int32_t I,J;
	AGridPoint *GP = AGP;
	float ratio = Bang/16384.0;
	for(J=0;J<AGrid_Y;J++)
		for(I=0;I<AGrid_X;I++)
		{
			GP->x += (RAND_15()-16384)*ratio;
			GP->y += (RAND_15()-16384)*ratio;
			GP++;
		}
}

//Reads Grid situation from a Particle array.
//Useful to make them move along more complex paths
void AW_FromParticles(AGridPoint *AGP,Particle *P)
{
	int32_t I,J;
	for(J=0;J<AGrid_Y;J++)
		for(I=0;I<AGrid_X;I++)
		{
			AGP->x = P->V.Pos.x;
			AGP->y = P->V.Pos.y;
			AGP++;
			P++;
		}
}

// Image must be 256x256
/*void AntiGrid_Mapper(AGridPoint *AGP,Image *Img,DWord *DAC)
{
	Bi_Triangle BT[3]; // sent to affine mapper
	long U,V;
	AGridPoint *GP=AGP,*P12=GP+1,*P21=GP+AGrid_X,*P22=P12+AGrid_X;
	char *Bitmap = (char *)Img->Data;
	long Target = (long)DAC;
	
	BT[0].z=BT[1].z=BT[2].z = 1;
	
	for(V=0;V<256;V+=Awob_Gran)
	{
		for(U=0;U<256;U+=Awob_Gran)
		{
			BT[0].x = GP->x;  BT[0].y = GP->y;
			BT[0].U = U;      BT[0].V = V;
			BT[1].x = P12->x; BT[1].y = P12->y;
			BT[1].U = U+Awob_Gran; BT[1].V = V;
			BT[2].x = P22->x; BT[2].y = P22->y;
			BT[2].U = U+Awob_Gran; BT[2].V = V+Awob_Gran;
			CP_Texture_32(BT,Bitmap,Target);
			
			memcpy(BT+1,BT+2,sizeof(Bi_Triangle));
			BT[2].x = P21->x; BT[2].y = P21->y;
			BT[2].U = U;      BT[2].V = V+Awob_Gran;
			CP_Texture_32(BT,Bitmap,Target);
			
			GP++; P12++; P21++; P22++;
		}
		GP++; P12++; P21++; P22++;
	}
	
}*/

// Fractal Generators
void Generate_Julia_Fractal(Image *Img,Complex *C,Complex *R1,Complex *R2)
{
	Complex L,Z,Z2;
	float dX,dY;
	int32_t I,J;
	DWord *Ptr = Img->Data;
	int32_t Orb;
	
	L.y = R2->y;
	dX = (R2->x-R1->x)/(float)Img->x;
	dY = (R1->y-R2->y)/(float)Img->y;
	J = Img->y;
	while(J--)
	{
		I = Img->x;
		L.x = R1->x;
		while (I--)
		{
			Z.x = L.x; Z.y = L.y;
			Orb=32;
			Z2.x = Z.x*Z.x;
			Z2.y = Z.y*Z.y;
			do
			{
				Z.y *= 2.0*Z.x;
				Z.y += C->y;
				Z.x = Z2.x-Z2.y+C->x;
				Z2.x = Z.x*Z.x;
				Z2.y = Z.y*Z.y;
			} while (--Orb&&Z2.x<=4.0&&Z2.y<=4.0);
			*Ptr++ = Colors32[31-Orb];
			L.x += dX;
		}
		L.y += dY;
	}
}

void Generate_Mandelbrot_Fractal(Image *Img,Complex *R1,Complex *R2)
{
	Complex L,Z,Z2;
	float dX,dY;
	int32_t I,J;
	DWord *Ptr = Img->Data;
	int32_t Orb;
	
	L.y = R2->y;
	dX = (R2->x-R1->x)/(float)Img->x;
	dY = (R1->y-R2->y)/(float)Img->y;
	J = Img->y;
	while(J--)
	{
		I = Img->x;
		L.x = R1->x;
		while (I--)
		{
			Z.x = L.x; Z.y = L.y;
			Orb=32;
			Z2.x = Z.x*Z.x;
			Z2.y = Z.y*Z.y;
			do
			{
				Z.y *= 2.0*Z.x;
				Z.y += L.y;
				Z.x = Z2.x-Z2.y+L.x;
				Z2.x = Z.x*Z.x;
				Z2.y = Z.y*Z.y;
			} while (--Orb&&Z2.x<=4.0&&Z2.y<=4.0);
			*Ptr++ = Colors32[31-Orb];
			L.x += dX;
		}
		L.y += dY;
	}
}

// Degree 0 Pheonix, Check this out
void Generate_Phoenix_Fractal(Image *Img,float P1,float P2,Complex *R1,Complex *R2)
{
	Complex L,X,Y,Z,Z2;
	float dX,dY;
	int32_t I,J;
	DWord *Ptr = Img->Data;
	int32_t Orb;
	
	L.y = R2->y;
	dX = (R2->x-R1->x)/(float)Img->x;
	dY = (R1->y-R2->y)/(float)Img->y;
	J = Img->y;
	while(J--)
	{
		I = Img->x;
		L.x = R1->x;
		while (I--)
		{
			Z.x = L.x; Z.y = L.y;
			Orb=32;
			Z2.x = Z.x*Z.x;
			Z2.y = Z.y*Z.y;
			Y.x = 0; Y.y = 0;
			do
			{
				X.x = Z2.x-Z2.y+P1+P2*Y.x;
				X.y = 2.0*Z.x*Z.y+P2*Y.y;
				Y.x = Z.x;
				Y.y = Z.y;
				Z.x = X.x;
				Z.y = X.y;
				Z2.x = Z.x*Z.x;
				Z2.y = Z.y*Z.y;
			} while (--Orb&&Z2.x<=4.0&&Z2.y<=4.0);
			*Ptr++ = Colors32[31-Orb];
			L.x += dX;
		}
		L.y += dY;
	}
}

// Simple Wobbler-8x8 based effects

// Affine transformer -
//   Ang - Rotation angle
//   Zoom - duh
//   XO,YO - (X,Y) Offset from origin
void Affine_Wobbler(GridPoint *Target_GP,float Ang,float Zoom,float XO,float YO)
{
	float Mat[2][2]; // Rotation and scale matrix
	float *Ptr = (float *)Mat;
	float dX,dY,X,Y;
	int32_t I,J;
	GridPoint *GP = Target_GP;
	Mat[1][1] = Mat[0][0] = cos(Ang)*Zoom;
	Mat[0][1] = sin(Ang)*Zoom;
	Mat[1][0] = -Mat[0][1];
	dX = 65536.0/Grid_X;
	dY = 65536.0/Grid_Y;
	XO *= 256;
	YO *= 256;
	Y = -32768;
	for(J=0;J<Grid_Y;J++)
	{
		X = -32768;
		for(I=0;I<Grid_X;I++)
		{
			GP->U = (*(Ptr+1))*X+(*(Ptr+2))*Y+XO;
			GP->V = (*(Ptr+3))*X+(*Ptr)*Y+YO;
			//Ptr -= 3;
			X += dX;
			GP++;
		}
		Y += dY;
	}
}

// Raytracing/Wobbler-8x8 based effects

// Calculates all Ray directions at Pixel density of 1:8, assuming view is
// on the [xy] Plane. The FOV parameter is converted to a perspective
// constant, and is given in Degrees.
void Init_WRaytrace_Table(float FOV)
{
	int32_t I,J;
	float X,Y;
	Vector *V = WPDT;
	
	if (!WPDT) return;
	
	WRFOV = FOV;
	float Persp = tan(FOV*(PI/360.0))/CntrX;
	Y = 0;
	for(J=0;J<Grid_Y;J++)
	{
		X = 0;
		for(I=0;I<Grid_X;I++)
		{
			V->x = (X-CntrX)*Persp;
			V->y = (CntrY-Y)*Persp;
			V->z = 1.0;
			Vector_Norm(V);
			V++;
			X+=Grid_Subsamp;
		}
		Y+=Grid_Subsamp;
	}
}

// Ray-traces an infinite Cylinder at 8x8 subsampling density.
// the Radius Parameter is given Squared.
void WRaytrace_Cylinder(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Radius)
{
	GridPoint *GP = Target_GP,*GPE = GP+Grid_XY;
	Vector *PDT = WPDT;
	Vector D;
	const float Z2U_Conv = 32.0;
	float a,b,c=Origin->x*Origin->x+Origin->y*Origin->y-Radius,d,t;
	const float V_Conv = 32768.0/PI;
	if (c<0) //camera is inside cylinder
	{
		for(;GP<GPE;GP++,PDT++)
		{
			MatrixXVector(Orient,PDT,&D); // Get Direction vector
			a = D.x*D.x+D.y*D.y;
			b = 2*(Origin->x*D.x+Origin->y*D.y);
			d = sqrt(b*b-4*a*c);
			t = (d-b) / (2*a+EPSILON);
			D.x *= t; D.y *= t; D.z *= t;
			D.x += Origin->x; D.y += Origin->y; D.z += Origin->z;
			GP->U = D.z*Z2U_Conv;
			GP->V = fabs(atan2(D.y,D.x))*V_Conv;
		}
	}
	else
	{
		for(;GP<GPE;GP++,PDT++)
		{
			MatrixXVector(Orient,PDT,&D); // Get Direction vector
			a = D.x*D.x+D.y*D.y;
			b = 2*(Origin->x*D.x+Origin->y*D.y);
			if ((d=b*b-4*a*c)<0) continue;
			t = (-sqrt(d)-b) / (2*a+EPSILON);
			D.x *= t; D.y *= t; D.z *= t;
			D.x += Origin->x; D.y += Origin->y; D.z += Origin->z;
			GP->U = D.z*Z2U_Conv;
			GP->V = atan2(D.y,D.x)*V_Conv;
		}
	}
}

// This is a Tweaked version of the Cylinder, and will only work from the
// inside of the tunnel. This doesn't work 100% properly when the camera is
// not positioned on the axis of the Cylinder.
// The radius table is at 1 degree accuracy.
void WRaytrace_CurvedCylinder(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float *Radius)
{
	GridPoint *GP = Target_GP,*GPE = GP+Grid_XY;
	Vector *PDT = WPDT;
	Vector D;
	const float Z2U_Conv = 32.0;
	const float V_Conv = 32768.0/PI;
	const float Rad2Deg = 180/PI;
	float a,b,c,cc=Origin->x*Origin->x+Origin->y*Origin->y,d,t;
	Radius+=179;
	
	for(;GP<GPE;GP++,PDT++)
	{
		MatrixXVector(Orient,PDT,&D); // Get Direction vector
		a = D.x*D.x+D.y*D.y;
		b = 2*(Origin->x*D.x+Origin->y*D.y);
		c = cc-Radius[(int)(atan2(D.x,D.y)*Rad2Deg)];
		d = sqrt(b*b-4*a*c);
		t = (d-b) / (2*a+EPSILON);
		D.x *= t; D.y *= t; D.z *= t;
		D.x += Origin->x; D.y += Origin->y; D.z += Origin->z;
		GP->U = D.z*Z2U_Conv;
		GP->V = fabs(atan2(D.y,D.x))*V_Conv;
	}
}


float Plane_Z2U_Conv = 8.0;
float Plane_X2V_Conv = 8.0;

// Raytraces a Plane, which is always aligned with the [xz] plane. The
// Height parameter measures it's distance
void WRaytrace_Plane(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Height)
{
	GridPoint *GP = Target_GP,*GPE = GP+Grid_XY;
	Vector *PDT = WPDT;
	Vector D;
	float c = Height - Origin->y,t;
	if (c<0) //camera is on the upper side of the plane
	{
		for(;GP<GPE;GP++,PDT++)
		{
			MatrixXVector(Orient,PDT,&D); // Get Direction vector
			if (D.y>0) continue; // Flys away
			t = c/D.y;
			GP->U = (D.z*t+Origin->z)*Plane_Z2U_Conv;
			GP->V = (D.x*t+Origin->x)*Plane_X2V_Conv;
		}
	}
	else
	{
		for(;GP<GPE;GP++,PDT++)
		{
			MatrixXVector(Orient,PDT,&D); // Get Direction vector
			if (D.y<0) continue;
			t = c/D.y;
			GP->U = (D.z*t+Origin->z)*Plane_Z2U_Conv;
			GP->V = (D.x*t+Origin->x)*Plane_X2V_Conv;
		}
	}
}

// Ray-traces an infinite Cylinder at 8x8 subsampling density.
// the Radius is not squared.
// The pole vector of the Sphere is aligned with the Y axis.
void WRaytrace_Sphere(GridPoint *Target_GP,Vector *Origin,Matrix Orient,float Radius)
{
	GridPoint *GP = Target_GP,*GPE = GP+Grid_XY;
	Vector *PDT = WPDT;
	Vector D;
	const float U_Conv = 32768.0/PI;
	float b,c=Origin->x*Origin->x+Origin->y*Origin->y+Origin->z*Origin->z-Radius*Radius,d,t;
	const float V_Conv = 32768.0/PI;
	float rd = 1.0/Radius;
	if (c<0) //camera is inside the Sphere
	{
		for(;GP<GPE;GP++,PDT++)
		{
			MatrixXVector(Orient,PDT,&D); // Get Direction vector
			b = Origin->x*D.x+Origin->y*D.y+Origin->z*D.z;
			d = sqrt(b*b-c);
			t = d-b;
			D.x *= t; D.y *= t; D.z *= t;
			D.x += Origin->x; D.y += Origin->y; D.z += Origin->z;
			GP->U = fabs(asin(D.y*rd))*U_Conv;
			GP->V = fabs(atan2(D.z,D.x))*V_Conv;
		}
	}
	else
	{
		for(;GP<GPE;GP++,PDT++)
		{
			MatrixXVector(Orient,PDT,&D); // Get Direction vector
			b = Origin->x*D.x+Origin->y*D.y+Origin->z*D.z;
			if ((d=b*b-c)<0) continue;
			d = -sqrt(d);
			t = d-b;
			D.x *= t; D.y *= t; D.z *= t;
			D.x += Origin->x; D.y += Origin->y; D.z += Origin->z;
			GP->U = fabs(asin(D.y*rd))*U_Conv;
			GP->V = fabs(atan2(D.z,D.x))*V_Conv;
		}
	}
}

// Calculates a Phong/Specular Illumination map, used with
// an Environment mapper to simulate true Phong shading.
// Power should be 0.5 for normal distribution and higher
// for sharper highlights.
void Phong_Map(Image *Img,float R,float G,float B,float Power)
{
	int32_t I,J;
	float X,Y,Y2,Z,dX,dY;
	DWord *Ptr = Img->Data;
	Y = -1.0;
	dX = 2.0/(float)Img->x;
	dY = 2.0/(float)Img->y;
	for(J=0;J<Img->y;J++)
	{
		Y2 = 1.0-Y*Y;
		X = -1.0;
		for(I=0;I<Img->x;I++)
		{
			Z = Y2-X*X; //so the Magnitude of (X,Y,Z) will be 1
			if (Z<0.0) {*Ptr++ = 0; X += dX; continue;}
			Z = 255.0f*powf(Z,Power);
			*Ptr++ = ((int)(R*Z)<<16)+((int)(G*Z)<<8)+(int)(B*Z);
			X += dX;
		}
		Y += dY;
	}
}

// Hidden Flare effect: Generate a Flare composed of Light rays that are
// blocked by a given Image (the parameter) as if the Lightsource is behind
// it, and hence, only the rays that pass the image are visible. color 0
// is considered a "Block".
// Parameters: Src - the source Image,
// L - Position of the Lightsource, Z value is considered as the depth of the rays.
// (L.z actually does not matter, because it's only an approximation)
void Hidden_Flare(Image *Src,Vector L,Image *Trg)
{
	
}

void Sachletz(dword *data, dword xres, dword yres) {
	auto backbuffer = std::make_unique<dword[]>(xres * yres);
	// right now, there is no support for non-blocked textures,
	// so we need to rearrange TempBuf by 4x4 blocks
	//memcpy(VorSurf, TempBuf, 65536 * 4);
	auto ptr = backbuffer.get();
	memcpy(ptr, data, xres * yres * 4);
	dword* writePtr = data;
	for (int X = 0; X < xres / 4; X++)
		for (int Y = 0; Y < yres / 4;  Y++)
		{
			dword* blockPtr = ptr + ((X + (Y * xres)) << 2);
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++) {
					*writePtr++ = blockPtr[x + (y * xres)];
				}
		}
}
