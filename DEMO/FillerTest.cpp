#include "FillerTest.h"
#include "Base/FDS_VARS.H"
#define MEASURE_ZSTATS
#include "Base/Scene.h"
#include "Gradient.h"
#include "FRUSTRUM.H"
#include "Clipper.h"
#include "Threads.h"
#include <FILLERS/TheOtherBarry.h>
#include <FILLERS/Mekalele.h>

#include <VESA/Vesa.h>

#include <thread>

struct IXVertex
{
	union
	{
		struct
		{
			float x, RZ, UZ, VZ; // 16 bytes
		};
		F4Vec XZUV;
	};
	union
	{
		struct
		{
			float R, G, B, z;	 // 16 bytes
		};
		F4Vec RGBZ;
	};	
	float y;			 // 4 bytes
	dword _filler1[3];	 // 12 bytes
						 // Sum - 48 Bytes
};


/////////////////////////////////
// static filler variables (SFV)
struct 
{
	float x, RZ, UZ, VZ;
	float dX, dRZ, dUZ, dVZ;
	dword R, G, B, z;
	sdword dR, dG, dB, dZ;
	float Height; // Section Height
	dword 
		ScanLines, // Scan-lines left to draw
		Index; // Current Vertex index
} Left;

struct
{
	float x, dX;
	dword R, G, B, z;
	sdword dR, dG, dB, dZ;
	float Height, rHeight; // Section Height
	dword 
		ScanLines, // Scan-lines left to draw
		Index;  // Current Vertex index
} Right;

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

thread_local static sword	dRdx;
thread_local static sword	dGdx;
thread_local static sword	dBdx;
thread_local static sword	dZdx;

//dword zReject, zPass;


void CalcRightSection (IXVertex *V1, IXVertex *V2)
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
	if (Right.ScanLines > 0)
	{
		FPRevHeight = Fist(rHeight * 65536.0);
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
	Right.R = V1->R * 65536.0 + prestep * Right.dR;//+ (((Right.dR>>8) * iprestep)>>8);
	Right.G = V1->G * 65536.0 + prestep * Right.dG;//+ (((Right.dG>>8) * iprestep)>>8);
	Right.B = V1->B * 65536.0 + prestep * Right.dB;//+ (((Right.dB>>8) * iprestep)>>8);
	Right.z = V1->z * 65536.0 + prestep * Right.dZ;//+ (((Right.dZ>>8) * iprestep)>>8);
}


void CalcLeftSection (IXVertex *V1, IXVertex *V2)
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
		
//	Left.R = V1->R;
//	Left.G = V1->G;
//	Left.B = V1->B;
//	Left.z = V1->z;
	
	// Calculate deltas for Gouraud values
	sdword FPRevHeight;
	if (Left.ScanLines > 0)
	{
		FPRevHeight = Fist(RevHeight*65536.0);
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
	Left.UZ = V1->UZ + Left.dUZ * prestep;
	Left.VZ = V1->VZ + Left.dVZ * prestep;
	Left.RZ = V1->RZ + Left.dRZ * prestep;

	//int32_t iprestep = Fist(65536.0 * prestep);
	Left.R = V1->R * 65536.0 + prestep * Left.dR;//+ (((Left.dR>>8) * iprestep)>>8);
	Left.G = V1->G * 65536.0 + prestep * Left.dG;//+ (((Left.dG>>8) * iprestep)>>8);
	Left.B = V1->B * 65536.0 + prestep * Left.dB;//+ (((Left.dB>>8) * iprestep)>>8);
	Left.z = V1->z * 65536.0 + prestep * Left.dZ;//+ (((Left.dZ>>8) * iprestep)>>8);
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
			}

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
		}

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
			dword r,g,b, tex;

			// ZBuffer test			
			if (_Z0 > *ZSpanPtr)
			{
				*ZSpanPtr = _Z0;

				tex = ((dword *)IX_Texture)[((_u0& ((0x100<<IX_L2X) - 0x100) )>> 8) + (((_v0&((0x100<<IX_L2Y) - 0x100))>>8) << IX_L2X)];
				//tex = 0xFFFFFF;

//				*SpanPtr = 0x7F7F7F;
				//tex = 0x00ffffff;

				// *SpanPtr = tex;				
			} else {
				int yadda = 1;
			}
			ZSpanPtr++;
			SpanPtr++;
			_u0 += _du;
			_v0 += _dv;
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

//				*SpanPtr = 0x7F7F7F;

//				*SpanPtr += 0x7F7F7F;
				//*SpanPtr = (*SpanPtr & 0xfefefe) + (tex & 0xfefefe) >> 1; 
				//tex = 0x00ffffff;
				//r = (tex & 0xff0000) >> 8;
				//g = (tex & 0x00ff00);
				//b = (tex & 0x0000ff) << 8;

				//r *= R;
				//g *= G;
				//b *= B;

				//r = (r & 0xff000000) >> 8;
				//g = (g >> 16) & 0x00ff00;
				//b >>= 24;

				//*SpanPtr = (*SpanPtr & 0xfefefe) + ((r + g + b) & 0xfefefe) >> 1; 
				//*SpanPtr = tex;
			}
			ZSpanPtr++;
			SpanPtr++;
			_u0 += _du;
			_v0 += _dv;
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


static void IXFiller(IXVertex *Verts, dword numVerts, void *Texture, void *Page, dword logWidth, dword logHeight)
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
			if (Width>1)
			{
				rWidth = //65536/Width;
					Fist(65536.0 / (Right.x - Left.x));
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
thread_local static char l_IXMemBlock[sizeof(IXVertex) * (maximalNgon+1)];
thread_local static IXVertex *l_IXArray = (IXVertex *)( ((uintptr_t)l_IXMemBlock + 0xF) & (~0xF) );
thread_local static Material DummyMat;
thread_local static Texture DummyTex;


#define ENABLE_PIXELCOUNT
static void PrefillerCommon(Face *F, Vertex **V, dword numVerts)
{
	dword i;

//	float area = (B->PX - A->PX) * (C->PY - A->PY) - (C->PX - A->PX) * (B->PY - A->PY);
//#ifdef ENABLE_PIXELCOUNT
//	FillerPixelcount += 0.5*fabs(area);
//#endif	
//	Vertex *V[3];
//
//	if (area < 0.0)
//	{
//		V[0] = A;
//		V[1] = B;
//		V[2] = C;
//	} else {
//		V[0] = A;
//		V[1] = C;
//		V[2] = B;
//	}

	//const float NearZPlane = 1.0;
	//float ZScaleFactor = 0xFFFF * NearZPlane;
	//float ZScaleFactor = 0xFFFF * CurScene->NZP;
	//float ZDenom = (float)0xFFFF * CurScene->FZP / (CurScene->FZP - CurScene->NZP);

	//int32_t MipLevel = 0;
	// [Shirman98]	

	int32_t LogWidth = F->Txtr->Txtr->LSizeX;
	int32_t LogHeight = F->Txtr->Txtr->LSizeY;
	
	uintptr_t TextureAddr = (uintptr_t)F->Txtr->Txtr->Mipmap[0];

//	dword TextureAddr =
		//(dword)l_TestTexture; 
//		(dword)DoFace->Txtr->Txtr->Data;
	//dword TextureSize = sizeof(dword)<<(LogWidth+LogHeight);
	
//	for(i=0; i<MipLevel; i++)
//	{
//		TextureAddr += TextureSize;
//		TextureSize >>= 2;
//	}
	
	float UScaleFactor = (1<<LogWidth);
	float VScaleFactor = (1<<LogHeight);

	if (CurScene->Flags & Scn_Fogged)
	{
		for(i=0; i<numVerts; i++)
		{
			l_IXArray[i].x = V[i]->PX;
			l_IXArray[i].UZ = V[i]->UZ * UScaleFactor;
			l_IXArray[i].VZ = V[i]->VZ * VScaleFactor;
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
			l_IXArray[i].UZ = V[i]->UZ * UScaleFactor;
			l_IXArray[i].VZ = V[i]->VZ * VScaleFactor;
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

	IXFiller(l_IXArray, numVerts, (void *)TextureAddr, VPage, LogWidth, LogHeight);
}

void Prefiller(Face *F, Vertex **V, dword numVerts)
{
	SubInnerPtr = SubInnerLoop;
	PrefillerCommon(F, V, numVerts);
}

void Prefiller_T(Face *F, Vertex **V, dword numVerts)
{
	SubInnerPtr = SubInnerLoopT;
	PrefillerCommon(F, V, numVerts);
}

extern "C"
{
	void IXAsmFiller(IXVertex *Verts, dword numVerts, void *Texture, void *Page, dword logWidth, dword logHeight);
}

std::mutex mut;
std::condition_variable cv;
std::atomic<int> counter;


static void drawPoly(float DT)
{
	Vertex V[4];
	Face F;
	dword i;
	static float T = 0;

	float a = (T + DT) * 0.003;
	float c = cos(a);
	float s = sin(a);

	T += 0.5;

	const auto W = 280;
	const auto H = 250;

	i = 0;
	V[i].PX = 900.1 - W * c - H * s;
	V[i].PY = 400.1 + W * s - H * c;
	//	V[i].PX = 200.0;
	//	V[i].PY = 100.0;
	V[i].TPos.z = 1.0;
	V[i].U =-1.0 / 512.0;
	V[i].V =-1.0 / 512.0;
	V[i].LA = 255;
	V[i].LR = 2;
	V[i].LG = 2;
	V[i].LB = 253;

	i = 1;
	V[i].PX = 900.1 + W * c - H * s;
	V[i].PY = 400.1 - W * s - H * c;
	//	V[i].PX = 130.0;
	//	V[i].PY = 200.0;
	V[i].TPos.z = 1.0;
	V[i].U = 511.0 / 512.0;
	V[i].V =-1.0 / 512.0;
	V[i].LA = 255;
	V[i].LR = 2;
	V[i].LG = 2;
	V[i].LB = 127;

	i = 2;
	V[i].PX = 900.1 + W * c + H * s;
	V[i].PY = 400.1 - W * s + H * c;
	//	V[i].PX = 100.0;
	//	V[i].PY = 100.0;
	V[i].TPos.z = 1.0;
	V[i].U = 511.0 / 512.0;
	V[i].V = 511.0 / 512.0;
	V[i].LA = 255;
	V[i].LR = 253;
	V[i].LG = 2;
	V[i].LB = 2;

	i = 3;
	V[i].PX = 900.1 - W * c + H * s;
	V[i].PY = 400.1 + W * s + H * c;
	V[i].TPos.z = 1.0;
	V[i].U =-1.0 / 512.0;
	V[i].V = 511.0 / 512.0;
	V[i].LA = 255;
	V[i].LR = 255;
	V[i].LG = 2;
	V[i].LB = 127;

	for (i = 0; i != 4; ++i) {
		V[i].LR = V[i].LG = V[i].LB = V[i].LA = 255;
	}

	F.Txtr = &DummyMat;
	DummyMat.Txtr = &DummyTex;
	DummyTex.Data = (byte*)l_TestTexture;
	DummyTex.Mipmap[0] = DummyTex.Data;
	F.Txtr->Txtr->LSizeX = 8;
	F.Txtr->Txtr->LSizeY = 8;
	F.Txtr->ZBufferWrite = 0;
	//F.Filler = IX_Prefiller_TGZSAM;
	// F.Filler = TheOtherBarry<barry::TBlendMode::OVERWRITE>;
	F.Filler = Mekalele;
	//F.Filler = IX_Prefiller_FZ;
	//F.Filler = IX_Prefiller_TGZM;

	Viewport vp;
	vp.ClipX1 = 0;
	vp.ClipX2 = XRes;
	vp.ClipY1 = 0;
	vp.ClipY2 = YRes_1;

	for (i = 0; i < 4; i++) {
		V[i].RZ = 1.0 / V[i].TPos.z;
		V[i].UZ = V[i].U * V[i].RZ;
		V[i].VZ = V[i].V * V[i].RZ;
		viewportCalcFlags(vp, &V[i]);
	}

	counter = 0;
	ThreadPool::instance().enqueue([&F, &vp, V = &V[0]]() {
		F.A = &V[0];
		F.B = &V[1];
		F.C = &V[2];
		_2DClipper::getInstance()->clip(vp, F);

		F.A = &V[0];
		F.B = &V[2];
		F.C = &V[3];
		_2DClipper::getInstance()->clip(vp, F);

		std::unique_lock<std::mutex> lock(mut);
		++counter;
		cv.notify_one();
	});

	{
		std::unique_lock<std::mutex> lock(mut);
		cv.wait(lock, [] {return counter == 1; });
	}

	/*int32_t my=0;
	float minY = V[0].PY;
	for(i=1; i<4; i++)
	{
		if (V[i].PY < minY)
		{
			my = i;
			minY = V[i].PY;
		}
	}
	Vertex *VP[4];
	switch (my)
	{
	case 0:
		VP[0] = &V[0];
		VP[1] = &V[3];
		VP[2] = &V[2];
		VP[3] = &V[1];
		break;
	case 1:
		VP[0] = &V[1];
		VP[1] = &V[0];
		VP[2] = &V[3];
		VP[3] = &V[2];
		break;
	case 2:
		VP[0] = &V[2];
		VP[1] = &V[1];
		VP[2] = &V[0];
		VP[3] = &V[3];
		break;
	case 3:
		VP[0] = &V[3];
		VP[1] = &V[2];
		VP[2] = &V[1];
		VP[3] = &V[0];
		break;
	}
	float fl = 5.3;
	int32_t ifl;
	__asm
	{
		fld dword ptr [fl]
		fistp dword ptr [ifl]
	}
	F.Txtr->ZBufferWrite = 0;
	counter = 0;
	ThreadPool::instance().enqueue([&F, VP=&VP[0]]() {
		IX_Prefiller_TGZSAM(&F, VP, 4, 0);
		std::unique_lock<std::mutex> lock(mut);
		++counter;
		cv.notify_one();
	});
	{
		std::unique_lock<std::mutex> lock(mut);
		cv.wait(lock, [] {return counter == 1; });
	}*/

	//	VPage -= 800;
	//	IX_Prefiller_TGZ(&F, VP, 4);
	//	VPage += 800;*/

	/*	while (!Keyboard[ScSpace])
		{
			continue;
		}*/
}


void FillerTest()
{
	Scene Sc;
	CurScene = &Sc;
	Sc.Flags = 0;
	Sc.NZP = 0.5;
	Sc.FZP = 1000.0;

	meka::GBuffer gbuffer;
	{
		size_t numPixels = XRes * YRes;
		gbuffer.position.resize(numPixels);
		gbuffer.normal.resize(numPixels);
		gbuffer.txtr.resize(numPixels);
	}
	SetGBuffer(&gbuffer);

//	Texture Tx;
//	Tx.FileName = strdup("Textures/PBRK34.JPG");
//	Load_Texture(&Tx);
//	BPPConvert_Texture(&Tx, 32);
	// prepare texture
	//l_TestTexture = new dword [1<<(2*8)];

	std::vector<GradientEndpoint> endpoints;
/*	endpoints.emplace_back(0.0, Color{ 0.0, 0.0, 0.0, 0.0 });
	endpoints.emplace_back(0.1, Color{ 1.0, 0.0, 0.1 });
	endpoints.emplace_back(0.35, Color{ 1.0, 0.4, 0.8 });
	endpoints.emplace_back(0.5, Color{ 1.0, 1.0, 1.0 });
	endpoints.emplace_back(1.0, Color{ 1.0, 1.0, 1.0 });
	//	endpoints.emplace_back(0.4, Color{ 1.0, 0.2, 0.6 });
//	endpoints.emplace_back(1.0, Color{ 1.0, 1.0, 1.0 });
	auto M = Generate_Gradient(endpoints, 256, 0.2);*/

	//endpoints.emplace_back(0, Color{ 0.0, 0.0, 0.0, 0.0 });
	//endpoints.emplace_back(0.5, Color{ 0.3, 0.0, 0.1, 0.20 });
	//endpoints.emplace_back(0.6, Color{ 1.0, 0.0, 0.1, 0.40 });
	//endpoints.emplace_back(0.75, Color{ 1.0, 0.4, 0.8, 0.60 });
	//endpoints.emplace_back(0.8, Color{ 1.0, 1.0, 1.0, 0.80 });
	//endpoints.emplace_back(1.0, Color{ 1.0, 1.0, 1.0, 1.0 });

	//endpoints.emplace_back(0, Color{ 1.0, 1.0, 1.0, 1.0 });
	//endpoints.emplace_back(0.5, Color{ 1.0, 1.0, 1.0, 0.80 });
	//endpoints.emplace_back(0.6, Color{ 1.0, 1.0, 1., 0.60 });
	//endpoints.emplace_back(0.75, Color{ 1.0, 1., 1., 0.40 });
	//endpoints.emplace_back(0.8, Color{ 1.0, 1.0, 1.0, 0.20 });
	//endpoints.emplace_back(1.0, Color{ 1.0, 1.0, 1.0, 0.0 });

	endpoints.emplace_back(0, Color{ 0.0, 0.0, 0.0, 0.2 });
	endpoints.emplace_back(0.5, Color{ 0.3, 0.0, 0.1, 0.2 });
	endpoints.emplace_back(0.6, Color{ 1.0, 0.0, 0.1, 0.2 });
	endpoints.emplace_back(0.75, Color{ 1.0, 0.4, 0.8, 0.2 });
	endpoints.emplace_back(0.8, Color{ 1.0, 1.0, 1.0, 0.2 });
	endpoints.emplace_back(1.0, Color{ 1.0, 1.0, 1.0, 0.2 });

	auto M = Generate_Gradient(endpoints, 256, 0.2, false);

	l_TestTexture = (DWord *)M->Txtr->Data;
//	l_TestTexture = (DWord *)Tx.Data;
	//memset(l_TestTexture, 255, 256 * 256 * 4);

	dword i, j;
	for (j = 0; j < 256; j++)
	{
		for (i = 0; i < 256; i++)
		{
			l_TestTexture[i + (j << 8)] =
				0xFFFFFF;
				//(((i<<3)^(j<<3)) & 0xFF) *0x010101;
				//(i<<16)+(j<<8)+(i^j) * 0x010101;
				//(i ^ j) * 0x010101;
				// (((i ^ j) >> 4) & 1) * 0xffffff;
			//((i>>2)&1)*0xFFFFFF;

		}
	}
	Sachletz(l_TestTexture, 256, 256);
	const int32_t PartTime = 10000;

	M->Txtr->Flags = Txtr_Tiled | Txtr_Nomip;

	int32_t timerStack[20], timerIndex = 0;
	for (int i = 0; i < 20; i++)
		timerStack[i] = Timer;
	char MSGStr[128];


	float TT = Timer;
	byte* TempBuf = new byte[XRes*YRes*4];
	memset(TempBuf, 0, XRes * YRes * 4);
	while (Timer < PartTime)
	{
		dTime = (Timer - TT) / 100.0;
		TT = Timer;

		//memset(VPage, 0, PageSize);
		/*for (int y = 0; y != YRes; ++y) {
			for (int x = 0; x != XRes; ++x) {
				((dword*)VPage)[y * XRes + x] = (x % 64 < 32) ? 0x3f3f3f : 0;
			}
		}*/
		memset(VPage,0,PageSize + XRes*YRes*sizeof(word));
		//memset(VPage, 0, PageSize);
		//for (int y = 0; y != YRes; ++y) {
		//	for (int x = 0; x != XRes; ++x) {
		//		((dword*)VPage)[y * XRes + x] = ((x ^ y) & 8) ? 0x7f7f7f : 0;
		//	}
		//}

		// drawPoly(0);
		drawPoly(500);

		DWord pSrc = 0x80808080;
		DWord pDst = 0x8C8C8C8C;

		//AlphaBlend((byte*)MainSurf->Data, TempBuf, pSrc, pDst, XRes * YRes * 4);
		//memcpy(MainSurf->Data, TempBuf, XRes * YRes * 4);

		timerStack[timerIndex++] = Timer;
		if (timerIndex == 20)
		{
			timerIndex = 0;
			snprintf(MSGStr, sizeof(MSGStr), "%f FPS", 2000.0 / (float)(timerStack[19] - timerStack[timerIndex]));
		}
		else {
			snprintf(MSGStr, sizeof(MSGStr), "%f FPS", 2000.0 / (float)(timerStack[timerIndex - 1] - timerStack[timerIndex]));
		}
		dword scroll = OutTextXY(VPage, 0, 0, MSGStr, 255);

		snprintf(MSGStr, sizeof(MSGStr), "%f frame", CurFrame);
		scroll = OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);

		memcpy(MainSurf->Data, g_gbuffer->txtr.data(), XRes * YRes * 4);

		Flip(MainSurf);

		if (Keyboard[ScESC])
		{
			Timer = PartTime;
			break;
		}
	} Timer -= PartTime;

	delete [] l_TestTexture;
}
