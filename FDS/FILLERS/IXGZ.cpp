#include "IX.h"
#include "Base/Scene.h"

/////////////////////////////////
// static filler variables (SFV)
struct GZLeft
{
	float x, RZ;
	float dX, dRZ;
	dword R, G, B, z;
	sdword dR, dG, dB, dZ;
	float Height; // Section Height
	dword 
		ScanLines, // Scan-lines left to draw
		Index; // Current Vertex index
};

thread_local static GZLeft Left;

struct GZRight
{
	float x, dX;
	dword R, G, B, z;
	sdword dR, dG, dB, dZ;
	float Height, rHeight; // Section Height
	dword 
		ScanLines, // Scan-lines left to draw
		Index;  // Current Vertex index
};

thread_local static GZRight Right;

thread_local static void *IX_Texture;
thread_local static void *IX_Page;
thread_local static word *IX_ZBuffer;
thread_local static dword IX_L2X, IX_L2Y;


union deltas
{
	struct
	{
		float dRZdx;
	};
	F4Vec UVZ;
};

#define L2SPANSIZE 4
#define SPANSIZE 16
#define fSPANSIZE 16.0f
thread_local static deltas ddx;
thread_local static deltas ddx32;

thread_local static sword	dRdx;
thread_local static sword	dGdx;
thread_local static sword	dBdx;
thread_local static sword	dZdx;

static void CalcRightSection (IXVertexG *V1, IXVertexG *V2)
{
	
	// Calculate number of scanlines
	dword iy1, iy2;
	Fist(iy1,V1->y);
	Fist(iy2,V2->y);
	Right.ScanLines = iy2 - iy1;
	
	if (Right.ScanLines == 0)
	{
		return;
	}
	
	// Calculate delta
	Right.Height = (V2->y - V1->y);
	float rHeight = 1.0f/Right.Height;		
	Right.dX = (V2->x - V1->x) * rHeight;
	sdword FPRevHeight;
	if (Right.ScanLines > 0)
	{
		FPRevHeight = Fist(rHeight * 65536.0f);
		Right.dR = (V2->R - V1->R) * FPRevHeight;
		Right.dG = (V2->G - V1->G) * FPRevHeight;
		Right.dB = (V2->B - V1->B) * FPRevHeight;
		Right.dZ = (V2->z - V1->z) * FPRevHeight;
//		Left.dR = (V2->R - V1->R) * RevHeight;
//		Left.dG = (V2->G - V1->G) * RevHeight;
//		Left.dB = (V2->B - V1->B) * RevHeight;
//		Left.dZ = (V2->z - V1->z) * RevHeight;
	} else {
		Right.dR = 0;
		Right.dG = 0;
		Right.dB = 0;
		Right.dZ = 0;
	}
	
	// Sub pixeling
	float prestep;
	prestep = (float)iy1 - V1->y;
	Right.x = V1->x + Right.dX * prestep;
	Right.R = V1->R * 65536.0f + prestep * Right.dR;//+ (((Right.dR>>8) * iprestep)>>8);
	Right.G = V1->G * 65536.0f + prestep * Right.dG;//+ (((Right.dG>>8) * iprestep)>>8);
	Right.B = V1->B * 65536.0f + prestep * Right.dB;//+ (((Right.dB>>8) * iprestep)>>8);
	Right.z = V1->z * 65536.0f + prestep * Right.dZ;//+ (((Right.dZ>>8) * iprestep)>>8);
}


static void CalcLeftSection (IXVertexG *V1, IXVertexG *V2)
{
	float RevHeight;
	
	// Calculate number of scanlines
	dword iy1, iy2;
	Fist(iy1,V1->y);
	Fist(iy2,V2->y);

	Left.ScanLines = iy2 - iy1;
	
	if (Left.ScanLines == 0)
	{
		return;
	}
	
	Left.Height = (V2->y - V1->y);
	RevHeight = 1.0f / Left.Height;
		
	// Calculate deltas for texture coordinates
	Left.dX = (V2->x - V1->x) * RevHeight;
	Left.dRZ = (V2->RZ - V1->RZ) * RevHeight;
		
//	Left.R = V1->R;
//	Left.G = V1->G;
//	Left.B = V1->B;
//	Left.z = V1->z;
	
	// Calculate deltas for Gouraud values
	sdword FPRevHeight;
	if (Left.ScanLines > 0)
	{
		FPRevHeight = Fist(RevHeight*65536.0f);
		Left.dR = (V2->R - V1->R) * FPRevHeight;
		Left.dG = (V2->G - V1->G) * FPRevHeight;
		Left.dB = (V2->B - V1->B) * FPRevHeight;
		Left.dZ = (V2->z - V1->z) * FPRevHeight;
//		Left.dR = (V2->R - V1->R) * RevHeight;
//		Left.dG = (V2->G - V1->G) * RevHeight;
//		Left.dB = (V2->B - V1->B) * RevHeight;
//		Left.dZ = (V2->z - V1->z) * RevHeight;
	} else {
		Left.dR = 0;
		Left.dG = 0;
		Left.dB = 0;
		Left.dZ = 0;
	}
	
	float prestep = ((float)iy1 - V1->y);
	Left.x  = V1->x  + Left.dX  * prestep;
	Left.RZ = V1->RZ + Left.dRZ * prestep;

	//int32_t iprestep = Fist(65536.0 * prestep);
	Left.R = V1->R * 65536.0f + prestep * Left.dR;//+ (((Left.dR>>8) * iprestep)>>8);
	Left.G = V1->G * 65536.0f + prestep * Left.dG;//+ (((Left.dG>>8) * iprestep)>>8);
	Left.B = V1->B * 65536.0f + prestep * Left.dB;//+ (((Left.dB>>8) * iprestep)>>8);
	Left.z = V1->z * 65536.0f + prestep * Left.dZ;//+ (((Left.dZ>>8) * iprestep)>>8);
}

thread_local static void (*SubInnerPtr)(dword bWidth, dword *SpanPtr, word * ZSpanPtr, float prestep);


static void SubInnerLoop(dword bWidth, dword *SpanPtr, word * ZSpanPtr, float prestep)
{
	int i = 0;

	float _z0, _z1, _z2, _z3;
	word _Z0, _Z1;
	short _dZ;
	

	int   Width = bWidth;

	float RZ = Left.RZ + prestep * ddx.dRZdx;

	/*union
	{
		word RGBZ[4];
		struct
		{
			word B, G, R, Z;
		};
	};*/
	//static word B, G, R, Z;
	word Col[4];

	Col[0] = Left.B >> 8;
	Col[1] = Left.G >> 8;
	Col[2] = Left.R >> 8;
	Col[3] = Left.z >> 8;

	int SpanWidth = Width;
	if (Width > SPANSIZE) SpanWidth = SPANSIZE;

	_z0 = 256.0 / RZ;
	RZ += ddx32.dRZdx;
	_z1 = 256.0 / RZ;
	RZ += ddx32.dRZdx;
	_z2 = 256.0 / RZ;
	RZ += ddx32.dRZdx;
	_z3 = 256.0 / RZ;
	RZ += ddx32.dRZdx;
	
	_Z0 = 0xFF80 - Fist(g_zscale256 * _z0);
	
	for(;;)
	{
		_Z1 = 0xFF80 - Fist(g_zscale256 * _z1);

//		if (Width < SPANSIZE)
//		{
//			float iWidth = 1.0 / (float)Width;
//			_du = (_u1 - _u0) * iWidth;
//			_dv = (_v1 - _v0) * iWidth;
//			_dZ = (_Z1 - _Z0) * iWidth;
//		} else {
		_dZ = (_Z1 - _Z0) >> L2SPANSIZE;
//		}
		
		while (SpanWidth--)
		{
			// ZBuffer test			
			if (_Z0 > *ZSpanPtr)
			{
				*ZSpanPtr = _Z0;
				*SpanPtr = (Col[0] >> 8) + (Col[1]&0xFF00) + ((Col[2]<<8)&0xFF0000);
#ifdef MEASURE_ZSTATS
				zPass++;
			} else {
				zReject++;
			}
#else
			}
#endif
			ZSpanPtr++;
			SpanPtr++;
			_Z0 += _dZ;
			Col[0] += dBdx;
			Col[1] += dGdx;
			Col[2] += dRdx;
			//Z += dZdx;			
		}

		Width -= SPANSIZE;
		if (Width <= 0) return;

		SpanWidth = Width;

		if (Width > SPANSIZE)
		{
			SpanWidth = SPANSIZE;
		}
		
		_Z0 = _Z1;

		_z1 = _z2;
		_z2 = _z3;
		_z3 = 256.0f / RZ;

//		if (Width < SPANSIZE)
//		{
//			RZ += ddx.dRZdx * Width;
//			UZ += ddx.dUZdx * Width;
//			VZ += ddx.dVZdx * Width;
//		} else {
		RZ += ddx32.dRZdx;
//		}
	}

}

static void SubInnerLoopT(dword bWidth, dword *SpanPtr, word * ZSpanPtr, float prestep)
{
	int i = 0;

	float _z0, _z1, _z2, _z3;
	word _Z0, _Z1;
	short _dZ;

	int   Width = bWidth;

	float RZ = Left.RZ + prestep * ddx.dRZdx;

	word Col[4];
	Col[0] = Left.B >> 8;
	Col[1] = Left.G >> 8;
	Col[2] = Left.R >> 8;
	Col[3] = Left.z >> 8;

	int SpanWidth = Width;
	if (Width > SPANSIZE) SpanWidth = SPANSIZE;

	_z0 = 256.0f / RZ;
	RZ += ddx32.dRZdx;
	_z1 = 256.0f / RZ;
	RZ += ddx32.dRZdx;
	_z2 = 256.0f / RZ;
	RZ += ddx32.dRZdx;
	_z3 = 256.0f / RZ;
	RZ += ddx32.dRZdx;
	
	_Z0 = 0xFF80 - Fist(g_zscale256 * _z0);
	
	for(;;)
	{
		_Z1 = 0xFF80 - Fist(g_zscale256 * _z1);

//		if (Width < SPANSIZE)
//		{
//			float iWidth = 1.0 / (float)Width;
//			_du = (_u1 - _u0) * iWidth;
//			_dv = (_v1 - _v0) * iWidth;
//			_dZ = (_Z1 - _Z0) * iWidth;
//		} else {
		_dZ = (_Z1 - _Z0) >> L2SPANSIZE;
//		}
		
		while (SpanWidth--)
		{
			dword r,g,b, tex;

			// ZBuffer test			
			if (_Z0 > *ZSpanPtr)
			{
				// *ZSpanPtr = _Z0;

				dword x = ( (*SpanPtr) & 0xFEFEFE) >> 1;
				*SpanPtr = x +
					(Col[0] >> 9) + ((Col[1]&0xFE00)>>1) + ((Col[2]<<7)&0xFF0000);

#ifdef MEASURE_ZSTATS
				zPass++;
			} else {
				zReject++;
			}
#else
			}
#endif			
			ZSpanPtr++;
			SpanPtr++;
			_Z0 += _dZ;
			Col[0] += dBdx;			
			Col[1] += dGdx;
			Col[2] += dRdx;
			
			//Z += dZdx;			
		}

		Width -= SPANSIZE;
		if (Width <= 0) return;

		SpanWidth = Width;

		if (Width > SPANSIZE)
		{
			SpanWidth = SPANSIZE;
		}
		
		_Z0 = _Z1;

		_z1 = _z2;
		_z2 = _z3;
		_z3 = 256.0f / RZ;

//		if (Width < SPANSIZE)
//		{
//			RZ += ddx.dRZdx * Width;
//			UZ += ddx.dUZdx * Width;
//			VZ += ddx.dVZdx * Width;
//		} else {
		RZ += ddx32.dRZdx;
//		}
	}

}


static void IXFiller(IXVertexG *Verts, dword numVerts, void *Page)
{
	IX_Page = Page;

	// ZBuffer data starts at the end of framebuffer
	IX_ZBuffer = (word *) ((uintptr_t)Page + PageSize);

	Left.Index = 1;
	Right.Index = numVerts - 1;

	CalcLeftSection (&Verts[0], &Verts[Left.Index]);
	while (Left.ScanLines == 0)
	{
		CalcLeftSection(&Verts[Left.Index], &Verts[Left.Index + 1]);
		Left.Index++;
		if (Left.Index >= numVerts)
		{
			return;
		}
	}
	
	CalcRightSection (&Verts[0], &Verts[Right.Index]);	
	while (Right.ScanLines == 0)
	{
		CalcRightSection (&Verts[Right.Index], &Verts[Right.Index - 1]);
		Right.Index--;
	}

	// Calculate constant deltas for line drawings
	if (1) //Left.Index == Right.Index)
	{
		// Gradient method - slightly slower

		// use verts (0,1,nVerts-1) or (0,1,2).
		// better precision: use verts (0,1, right) if right != 1.

		float dy01 = Verts[1].y - Verts[0].y;
		float dy02 = Verts[2].y - Verts[0].y;
		float invArea = 1.0f / ((Verts[1].x - Verts[0].x) * dy02 - (Verts[2].x - Verts[0].x) * dy01);
		ddx.dRZdx = invArea * ((Verts[1].RZ - Verts[0].RZ) * dy02 - (Verts[2].RZ - Verts[0].RZ) * dy01);
		
		//invArea *= 256.0; 
		//dRdx = Fist(invArea * ((Verts[1].R - Verts[0].R) * dy02 - (Verts[2].R - Verts[0].R) * dy01));
		//dGdx = Fist(invArea * ((Verts[1].G - Verts[0].G) * dy02 - (Verts[2].G - Verts[0].G) * dy01));
		//dBdx = Fist(invArea * ((Verts[1].B - Verts[0].B) * dy02 - (Verts[2].B - Verts[0].B) * dy01));
		//dZdx = Fist(invArea * ((Verts[1].z - Verts[0].z) * dy02 - (Verts[2].z - Verts[0].z) * dy01));
	
	} else {
		// Uses previous delta calculation - currently unused
		float RevLongest = 1.0f / (Verts[Right.Index].x - (Left.x + Right.Height * Left.dX));

		ddx.dRZdx = (Verts[Right.Index].RZ - (Left.RZ + Right.Height * Left.dRZ)) * RevLongest;

		//dRdx = Fist((Verts[Right.Index].R  - (Left.R  + Right.Height * Left.dR )) * RevLongest);
		//dGdx = Fist((Verts[Right.Index].G  - (Left.G  + Right.Height * Left.dG )) * RevLongest);
		//dBdx = Fist((Verts[Right.Index].B  - (Left.B  + Right.Height * Left.dB )) * RevLongest);
		//dZdx = Fist((Verts[Right.Index].z  - (Left.z  + Right.Height * Left.dZ )) * RevLongest);
	}
	ddx32.dRZdx = ddx.dRZdx * fSPANSIZE;

	dword y, SectionHeight;
	y = Fist(Verts[0].y);
	dword *Scanline = (dword *)((uintptr_t)Page + VESA_BPSL * y);
	word *ZScanline = (word *)((uintptr_t)Page + PageSize + sizeof(word) * XRes * y);
	int32_t Width;
	
	// Iterate over sections
	SectionHeight = (Left.ScanLines < Right.ScanLines) ? Left.ScanLines : Right.ScanLines;

	while (1)
	{
		for(y=0; y<SectionHeight; y++)
		{

			// *** Draw scan line *** //
			int32_t lx, rx;
			lx = Fist(Left.x);
			dword *SpanPtr = Scanline + lx;
			word *ZSpanPtr = ZScanline + lx;
			
			// Calculate scan-line width	
			rx = Fist(Right.x);
			Width = rx - lx;
			
			if (Width <= 0)
			{
				goto AfterScanConv;
			}			

			sdword rWidth;
			if (Width>1)
			{
				rWidth = //65536/Width;
					Fist(65536.0f / (Right.x - Left.x));
				sdword delta;
				delta = ((sdword)Right.R - (sdword)Left.R) >> 8;
				dRdx = delta * rWidth >> 16;
				delta = ((sdword)Right.G - (sdword)Left.G) >> 8;
				dGdx = delta * rWidth >> 16;
				delta = ((sdword)Right.B - (sdword)Left.B) >> 8;
				dBdx = delta * rWidth >> 16;
				delta = ((sdword)Right.z - (sdword)Left.z) >> 8;
				dZdx = delta * rWidth >> 16;
			} else {
				dRdx = dGdx = dBdx = 0;
			}
			
			// Iterate over scan-line
			SubInnerPtr(Width, SpanPtr, ZSpanPtr, (float)lx - Left.x);
//			Scanline[lx] = 0xFFFFFF;
//			Scanline[rx] = 0xFFFFFF;
			
			// *** End of scan line drawing //
AfterScanConv:
			Scanline += VESA_BPSL >> 2;
			ZScanline += XRes;

			// Advance section components
			Left.x  += Left.dX;
			Left.RZ += Left.dRZ;
			Left.R  += Left.dR;
			Left.G  += Left.dG;
			Left.B  += Left.dB;
			Left.z  += Left.dZ;
			
			Right.x += Right.dX;
			Right.R += Right.dR;
			Right.G += Right.dG;
			Right.B += Right.dB;
			Right.z += Right.dZ;
		}  // End of outer scan-line loop

		// Reduce section heights and check if they are done
		Left.ScanLines -= SectionHeight;
		while (Left.ScanLines == 0)
		{
			if (Left.Index == Right.Index)
			{
				return; // End mapper!
			}
			
			CalcLeftSection (&Verts[Left.Index], &Verts[Left.Index + 1]);
			Left.Index ++;
		}
		
		Right.ScanLines -= SectionHeight;
		while (Right.ScanLines == 0)
		{
			if (Right.Index == Left.Index)
			{
				return; // End mapper! (shouldn't happen!!!)
			}
			
			CalcRightSection (&Verts[Right.Index], &Verts[Right.Index - 1]);
			Right.Index --;
		}

		SectionHeight = (Left.ScanLines < Right.ScanLines) ? Left.ScanLines : Right.ScanLines;
	}
}


const dword maximalNgon = 16;
thread_local static dword *l_TestTexture = NULL;
thread_local static char l_IXMemBlock[sizeof(IXVertexG) * (maximalNgon+1)];
thread_local static IXVertexG *l_IXArray = (IXVertexG *)( ((uintptr_t)l_IXMemBlock + 0xF) & (~0xF) );
thread_local static Material DummyMat;
thread_local static Texture DummyTex;


#define ENABLE_PIXELCOUNT
static void PrefillerCommon(Vertex **V, dword numVerts)
{
	dword i;

	if (CurScene->Flags & Scn_Fogged)
	{
		for(i=0; i<numVerts; i++)
		{
			l_IXArray[i].x = V[i]->PX;
			l_IXArray[i].RZ = V[i]->RZ;
			//Fist(l_IXArray[i].R, V[i]->LR * 256.0f);
			//Fist(l_IXArray[i].G, V[i]->LG * 256.0f);
			//Fist(l_IXArray[i].B, V[i]->LB * 256.0f);
			float fogRate;
			fogRate = 1.0 - 1.0 * C_rFZP * V[i]->TPos.z;
			if (fogRate < 0.0)
			{
				fogRate = 0.0;
			}
			l_IXArray[i].R = V[i]->LR * fogRate;
			l_IXArray[i].G = V[i]->LG * fogRate;
			l_IXArray[i].B = V[i]->LB * fogRate;

			// protect against gouraud interpolation underflows
			if (l_IXArray[i].R < 2.0) l_IXArray[i].R = 2.0;
			if (l_IXArray[i].G < 2.0) l_IXArray[i].G = 2.0;
			if (l_IXArray[i].B < 2.0) l_IXArray[i].B = 2.0;

			l_IXArray[i].y = V[i]->PY;
		}
	} else {
		for(i=0; i<numVerts; i++)
		{
			l_IXArray[i].x = V[i]->PX;
			l_IXArray[i].RZ = V[i]->RZ;
			//Fist(l_IXArray[i].R, V[i]->LR * 256.0f);
			//Fist(l_IXArray[i].G, V[i]->LG * 256.0f);
			//Fist(l_IXArray[i].B, V[i]->LB * 256.0f);
			l_IXArray[i].R = V[i]->LR;
			l_IXArray[i].G = V[i]->LG;
			l_IXArray[i].B = V[i]->LB;
			l_IXArray[i].y = V[i]->PY;
		}
	}

	IXFiller(l_IXArray, numVerts, VPage);
}

void IX_Prefiller_GZ(Face* F, Vertex **V, dword numVerts, dword miplevel)
{
	SubInnerPtr = SubInnerLoop;
	PrefillerCommon(V, numVerts);
}

void IX_Prefiller_GAcZ(Face* F, Vertex **V, dword numVerts, dword miplevel)
{
	SubInnerPtr = SubInnerLoopT;
	PrefillerCommon(V, numVerts);
}


