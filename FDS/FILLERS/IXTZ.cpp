#include "IX.h"

/////////////////////////////////
// static filler variables (SFV)
struct TZLeft
{
	float x, RZ, UZ, VZ;
	float dX, dRZ, dUZ, dVZ;
	dword R, G, B, z;
	sdword dR, dG, dB, dZ;
	float Height; // Section Height
	dword 
		ScanLines, // Scan-lines left to draw
		Index; // Current Vertex index
};

thread_local static TZLeft Left;

struct TZRight
{
	float x, dX;
	dword R, G, B, z;
	sdword dR, dG, dB, dZ;
	float Height, rHeight; // Section Height
	dword 
		ScanLines, // Scan-lines left to draw
		Index;  // Current Vertex index
};

thread_local static TZRight Right;

thread_local static void *IX_Texture;
thread_local static void *IX_Page;
thread_local static word *IX_ZBuffer;
thread_local static dword IX_L2X, IX_L2Y;


union deltas
{
	struct
	{
		float dUZdx, dVZdx, dRZdx;
	};
	F4Vec UVZ;
};

#define L2SPANSIZE 4
#define SPANSIZE 16
#define fSPANSIZE 16.0
thread_local static deltas ddx;
thread_local static deltas ddx32;

static void CalcRightSection (IXVertexT *V1, IXVertexT *V2)
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
	float rHeight = 1.0/Right.Height;		
	Right.dX = (V2->x - V1->x) * rHeight;
	sdword FPRevHeight;
	
	// Sub pixeling
	float prestep;
	prestep = (float)iy1 - V1->y;
	Right.x = V1->x + Right.dX * prestep;
}


static void CalcLeftSection (IXVertexT *V1, IXVertexT *V2)
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
	Left.dUZ = (V2->UZ - V1->UZ) * RevHeight;
	Left.dVZ = (V2->VZ - V1->VZ) * RevHeight;
	Left.dRZ = (V2->RZ - V1->RZ) * RevHeight;
		
	
	float prestep = ((float)iy1 - V1->y);
	Left.x  = V1->x  + Left.dX  * prestep;
	Left.UZ = V1->UZ + Left.dUZ * prestep;
	Left.VZ = V1->VZ + Left.dVZ * prestep;
	Left.RZ = V1->RZ + Left.dRZ * prestep;
}

thread_local static void (*SubInnerPtr)(dword bWidth, dword *SpanPtr, word * ZSpanPtr, float prestep);
/*
static void SubInnerLoopCorrectSlow(dword Width, dword *SpanPtr, word * ZSpanPtr, float prestep)
{
	int32_t i;

	//F4Vec _u, _v, _z;
	int32_t  _u0, _u1;
	int32_t  _v0, _v1;
	word _Z0, _Z1;

	float _z[4];

	short _dZ;
	
	float RZ = Left.RZ + prestep * ddx.dRZdx;
	float UZ = Left.UZ + prestep * ddx.dUZdx;
	float VZ = Left.VZ + prestep * ddx.dVZdx;

//	word R = Left.R;
//	word G = Left.G;
//	word B = Left.B;
//	word Z = Left.z;
	dword R = Fist(Left.R * 256.0 + prestep * dRdx);
	dword G = Fist(Left.G * 256.0 + prestep * dGdx);
	dword B = Fist(Left.B * 256.0 + prestep * dBdx);
	dword Z = Fist(Left.z * 256.0 + prestep * dZdx);

	// number of full sections
	int32_t ns = Width >> L2SPANSIZE;

	// remainder section
	int32_t wrem = Width & (SPANSIZE-1); // % SPANSIZE.

	if (ns >= 4)
	{
		for(i=0; i<4; i++)
		{			
			_z[i] = 256.0 / RZ;
			RZ += ddx32.dRZdx;			
		}
	} else {
		i=0;
		for(; i<ns; i++)
		{			
			_z[i] = 256.0 / RZ;
			RZ += ddx32.dRZdx;
		}
		_z[i] = 256.0 / RZ;
		RZ += ddx.dRZdx * wrem;
		if (i<3) _z[i+1] = 256.0 / RZ;
	}
	
	_u0 = Fist(UZ * _z[0]);
	_v0 = Fist(VZ * _z[0]);
	_Z0 = 0xFF80 - Fist(g_zscale256 * _z[0]);
	
	int32_t _du, _dv;

	while (ns--)
	{

		UZ += ddx32.dUZdx;
		VZ += ddx32.dVZdx;

		_u1 = Fist(UZ * _z[1]);
		_v1 = Fist(VZ * _z[1]);
		_Z1 = 0xFF80 - Fist(g_zscale256 * _z[1]);

		_du = _u1 - _u0 >> L2SPANSIZE;
		_dv = _v1 - _v0 >> L2SPANSIZE;
		_dZ = _Z1 - _Z0 >> L2SPANSIZE;
		
		int32_t SpanWidth = SPANSIZE;
		while (SpanWidth--)
		{
			dword r,g,b, tex;

			// ZBuffer test			
			if (_Z0 > *ZSpanPtr)
			{
				*ZSpanPtr = _Z0;

				tex = ((dword *)IX_Texture)[((_u0& ((0x100<<IX_L2X) - 0x100) )>> 8) + (((_v0&((0x100<<IX_L2Y) - 0x100))>>8) << IX_L2X)];
				r = (tex & 0xff0000) >> 8;
				g = (tex & 0x00ff00);
				b = (tex & 0x0000ff) << 8;

				r *= R >> 8;
				g *= G >> 8;
				b *= B >> 8;

				r = (r & 0xff000000) >> 8;
				g = (g >> 16) & 0x00ff00;
				b >>= 24;

				*SpanPtr = r + g + b; 
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
			_u0 += _du;
			_v0 += _dv;
			_Z0 += _dZ;
			R += dRdx;
			G += dGdx;
			B += dBdx;
			//Z += dZdx;			
		}

		_u0 = _u1;
		_v0 = _v1;
		_Z0 = _Z1;

		_z[1] = _z[2];
		_z[2] = _z[3];
		_z[3] = 256.0f / RZ;

		if (ns == 3)
			RZ += ddx.dRZdx * wrem;
		else
			RZ += ddx32.dRZdx;
	}

	if (!wrem) return;

	UZ += ddx.dUZdx * wrem;
	VZ += ddx.dVZdx * wrem;

	_u1 = Fist(UZ * _z[1]);
	_v1 = Fist(VZ * _z[1]);
	_Z1 = 0xFF80 - Fist(g_zscale256 * _z[1]);

	static float invTable[32] = {
		1.0/1.0,  1.0/2.0,  1.0/3.0,  1.0/4.0,
		1.0/5.0,  1.0/6.0,  1.0/7.0,  1.0/8.0,
		1.0/9.0,  1.0/10.0, 1.0/11.0, 1.0/12.0,
		1.0/13.0, 1.0/14.0, 1.0/15.0, 1.0/16.0,
		1.0/17.0, 1.0/18.0, 1.0/19.0, 1.0/20.0,
		1.0/21.0, 1.0/22.0, 1.0/23.0, 1.0/24.0,
		1.0/25.0, 1.0/26.0, 1.0/27.0, 1.0/28.0,
		1.0/29.0, 1.0/30.0, 1.0/31.0, 1.0/32.0
	};
	float iWidth = invTable[wrem];	

	_du = (_u1 - _u0) * iWidth;
	_dv = (_v1 - _v0) * iWidth;
	_dZ = (_Z1 - _Z0) * iWidth;
	
	while (wrem--)
	{
		dword r,g,b, tex;

		// ZBuffer test			
		if (_Z0 > *ZSpanPtr)
		{
			*ZSpanPtr = _Z0;

			tex = ((dword *)IX_Texture)[((_u0& ((0x100<<IX_L2X) - 0x100) )>> 8) + (((_v0&((0x100<<IX_L2Y) - 0x100))>>8) << IX_L2X)];
			r = (tex & 0xff0000) >> 8;
			g = (tex & 0x00ff00);
			b = (tex & 0x0000ff) << 8;

			r *= R >> 8;
			g *= G >> 8;
			b *= B >> 8;

			r = (r & 0xff000000) >> 8;
			g = (g >> 16) & 0x00ff00;
			b >>= 24;

			*SpanPtr = r + g + b; 
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
		_u0 += _du;
		_v0 += _dv;
		_Z0 += _dZ;
		R += dRdx;
		G += dGdx;
		B += dBdx;
		//Z += dZdx;			
	}
}
*/


static void SubInnerLoop(dword bWidth, dword *SpanPtr, word * ZSpanPtr, float prestep)
{
	int i = 0;

	//F4Vec _u, _v, _z;
	int   _u0, _v0;
	int   _u1, _v1;
	int   _u2, _v2;
	int   _u3, _v3;

	int _w0, _w1, _w2, _w3;

	float _z0, _z1, _z2, _z3;
	word _Z0, _Z1;
	short _dZ;
	

	int   Width = bWidth;

	float RZ = Left.RZ + prestep * ddx.dRZdx;
	float UZ = Left.UZ + prestep * ddx.dUZdx;
	float VZ = Left.VZ + prestep * ddx.dVZdx;

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
	
	_u0 = Fist(UZ * _z0);
	_v0 = Fist(VZ * _z0);
	_Z0 = 0xFF80 - Fist(g_zscale256 * _z0);
	UZ += ddx32.dUZdx;
	VZ += ddx32.dVZdx;
	
	for(;;)
	{
		int32_t _du, _dv;

		_u1 = Fist(UZ * _z1);
		_v1 = Fist(VZ * _z1);
		_Z1 = 0xFF80 - Fist(g_zscale256 * _z1);

//		if (Width < SPANSIZE)
//		{
//			float iWidth = 1.0 / (float)Width;
//			_du = (_u1 - _u0) * iWidth;
//			_dv = (_v1 - _v0) * iWidth;
//			_dZ = (_Z1 - _Z0) * iWidth;
//		} else {
		_du = (_u1 - _u0) >> L2SPANSIZE;
		_dv = (_v1 - _v0) >> L2SPANSIZE;
		_dZ = (_Z1 - _Z0) >> L2SPANSIZE;
//		}
		
		while (SpanWidth--)
		{

			// ZBuffer test			
			if (_Z0 > *ZSpanPtr)
			{
				*ZSpanPtr = _Z0;
				*SpanPtr = ((dword *)IX_Texture)[((_u0& ((0x100<<IX_L2X) - 0x100) )>> 8) + (((_v0&((0x100<<IX_L2Y) - 0x100))>>8) << IX_L2X)];

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
			_u0 += _du;
			_v0 += _dv;
			_Z0 += _dZ;
		}

		Width -= SPANSIZE;
		if (Width <= 0) return;

		SpanWidth = Width;

		if (Width > SPANSIZE)
		{
			SpanWidth = SPANSIZE;
		}
		
		_u0 = _u1;
		_v0 = _v1;
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
		UZ += ddx32.dUZdx;
		VZ += ddx32.dVZdx;
//		}
	}

}

static void SubInnerLoopT(dword bWidth, dword *SpanPtr, word * ZSpanPtr, float prestep)
{
	int i = 0;

	//F4Vec _u, _v, _z;
	int   _u0, _v0;
	int   _u1, _v1;
	int   _u2, _v2;
	int   _u3, _v3;

	int _w0, _w1, _w2, _w3;

	float _z0, _z1, _z2, _z3;
	word _Z0, _Z1;
	short _dZ;
	

	int   Width = bWidth;

	float RZ = Left.RZ + prestep * ddx.dRZdx;
	float UZ = Left.UZ + prestep * ddx.dUZdx;
	float VZ = Left.VZ + prestep * ddx.dVZdx;

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
	
	_u0 = Fist(UZ * _z0);
	_v0 = Fist(VZ * _z0);
	_Z0 = 0xFF80 - Fist(g_zscale256 * _z0);
	UZ += ddx32.dUZdx;
	VZ += ddx32.dVZdx;
	
	for(;;)
	{
		int32_t _du, _dv;

		_u1 = Fist(UZ * _z1);
		_v1 = Fist(VZ * _z1);
		_Z1 = 0xFF80 - Fist(g_zscale256 * _z1);

//		if (Width < SPANSIZE)
//		{
//			float iWidth = 1.0 / (float)Width;
//			_du = (_u1 - _u0) * iWidth;
//			_dv = (_v1 - _v0) * iWidth;
//			_dZ = (_Z1 - _Z0) * iWidth;
//		} else {
		_du = (_u1 - _u0) >> L2SPANSIZE;
		_dv = (_v1 - _v0) >> L2SPANSIZE;
		_dZ = (_Z1 - _Z0) >> L2SPANSIZE;
//		}
		
		while (SpanWidth--)
		{
			dword r,g,b, tex;

			// ZBuffer test			
			if (_Z0 > *ZSpanPtr)
			{
				// *ZSpanPtr = _Z0;

				tex = ((dword *)IX_Texture)[((_u0& ((0x100<<IX_L2X) - 0x100) )>> 8) + (((_v0&((0x100<<IX_L2Y) - 0x100))>>8) << IX_L2X)];
				dword x = ((*SpanPtr)&0xFEFEFE) >> 1;
				*SpanPtr = x + ((tex&0xFEFEFE)>>1);

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
			_u0 += _du;
			_v0 += _dv;
			_Z0 += _dZ;
			
			//Z += dZdx;			
		}

		Width -= SPANSIZE;
		if (Width <= 0) return;

		SpanWidth = Width;

		if (Width > SPANSIZE)
		{
			SpanWidth = SPANSIZE;
		}
		
		_u0 = _u1;
		_v0 = _v1;
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
		UZ += ddx32.dUZdx;
		VZ += ddx32.dVZdx;
//		}
	}

}


static void IXFiller(IXVertexT *Verts, dword numVerts, void *Texture, void *Page, dword logWidth, dword logHeight)
{
	IX_Texture = Texture;
	IX_Page = Page;
	IX_L2X = logWidth;
	IX_L2Y = logHeight;

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
		float invArea = 1.0 / ((Verts[1].x - Verts[0].x) * dy02 - (Verts[2].x - Verts[0].x) * dy01);
		ddx.dUZdx = invArea * ((Verts[1].UZ - Verts[0].UZ) * dy02 - (Verts[2].UZ - Verts[0].UZ) * dy01);
		ddx.dVZdx = invArea * ((Verts[1].VZ - Verts[0].VZ) * dy02 - (Verts[2].VZ - Verts[0].VZ) * dy01);
		ddx.dRZdx = invArea * ((Verts[1].RZ - Verts[0].RZ) * dy02 - (Verts[2].RZ - Verts[0].RZ) * dy01);
		
		//invArea *= 256.0; 
		//dRdx = Fist(invArea * ((Verts[1].R - Verts[0].R) * dy02 - (Verts[2].R - Verts[0].R) * dy01));
		//dGdx = Fist(invArea * ((Verts[1].G - Verts[0].G) * dy02 - (Verts[2].G - Verts[0].G) * dy01));
		//dBdx = Fist(invArea * ((Verts[1].B - Verts[0].B) * dy02 - (Verts[2].B - Verts[0].B) * dy01));
		//dZdx = Fist(invArea * ((Verts[1].z - Verts[0].z) * dy02 - (Verts[2].z - Verts[0].z) * dy01));
	
	} else {
		// Uses previous delta calculation - currently unused
		float RevLongest = 1.0f / (Verts[Right.Index].x - (Left.x + Right.Height * Left.dX));

		ddx.dUZdx = (Verts[Right.Index].UZ - (Left.UZ + Right.Height * Left.dUZ)) * RevLongest;
		ddx.dVZdx = (Verts[Right.Index].VZ - (Left.VZ + Right.Height * Left.dVZ)) * RevLongest;
		ddx.dRZdx = (Verts[Right.Index].RZ - (Left.RZ + Right.Height * Left.dRZ)) * RevLongest;

		//dRdx = Fist((Verts[Right.Index].R  - (Left.R  + Right.Height * Left.dR )) * RevLongest);
		//dGdx = Fist((Verts[Right.Index].G  - (Left.G  + Right.Height * Left.dG )) * RevLongest);
		//dBdx = Fist((Verts[Right.Index].B  - (Left.B  + Right.Height * Left.dB )) * RevLongest);
		//dZdx = Fist((Verts[Right.Index].z  - (Left.z  + Right.Height * Left.dZ )) * RevLongest);
	}
	ddx32.dUZdx = ddx.dUZdx * fSPANSIZE;
	ddx32.dVZdx = ddx.dVZdx * fSPANSIZE;
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
			
			// Iterate over scan-line
			SubInnerPtr(Width, SpanPtr, ZSpanPtr, (float)lx - Left.x);
//			Scanline[lx] = 0xFFFFFF;
//			Scanline[rx] = 0xFFFFFF;
			//SubInnerLoopCorrectSlow(Width, SpanPtr, ZSpanPtr, (float)lx - Left.x);
			
			// *** End of scan line drawing //
AfterScanConv:
			Scanline += VESA_BPSL >> 2;
			ZScanline += XRes;

			// Advance section components
			Left.x  += Left.dX;
			Left.RZ += Left.dRZ;
			Left.UZ += Left.dUZ;
			Left.VZ += Left.dVZ;
			
			Right.x += Right.dX;
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
thread_local static char l_IXMemBlock[sizeof(IXVertexT) * (maximalNgon+1)];
thread_local static IXVertexT *l_IXArray = (IXVertexT *)( ((uintptr_t)l_IXMemBlock + 0xF) & (~0xF) );
thread_local static Material DummyMat;
thread_local static Texture DummyTex;


static void PrefillerCommon(Face *F, Vertex **V, dword numVerts, dword miplevel)
{
	dword i;

	int32_t LogWidth = F->Txtr->Txtr->LSizeX - miplevel;
	int32_t LogHeight = F->Txtr->Txtr->LSizeY - miplevel;
	
	auto TextureAddr = (uintptr_t)F->Txtr->Txtr->Mipmap[miplevel];

	
	float UScaleFactor = float((1<<LogWidth));
	float VScaleFactor = float((1<<LogHeight));

	for(i=0; i<numVerts; i++)
	{
		l_IXArray[i].x = V[i]->PX;
		l_IXArray[i].UZ = V[i]->UZ * UScaleFactor;
		l_IXArray[i].VZ = V[i]->VZ * VScaleFactor;
		l_IXArray[i].RZ = V[i]->RZ;
		l_IXArray[i].y = V[i]->PY;
	}

	IXFiller(l_IXArray, numVerts, (void *)TextureAddr, VPage, LogWidth, LogHeight);
}

void IX_Prefiller_TZ(Face* F, Vertex **V, dword numVerts, dword miplevel)
{
	SubInnerPtr = SubInnerLoop;
	PrefillerCommon(F, V, numVerts, miplevel);
}

void IX_Prefiller_TAcZ(Face* F, Vertex **V, dword numVerts, dword miplevel)
{
	SubInnerPtr = SubInnerLoopT;
	PrefillerCommon(F, V, numVerts, miplevel);
}
