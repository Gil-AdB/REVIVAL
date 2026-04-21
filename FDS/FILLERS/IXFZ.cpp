#include "IX.h"
#include "Base/Scene.h"

#define MEASURE_POLYSTATS

/////////////////////////////////
// static filler variables (SFV)
struct FZLeft
{
	float x, RZ;
	float dX, dRZ;
	float Height; // Section Height
	dword 
		ScanLines, // Scan-lines left to draw
		Index; // Current Vertex index
};

thread_local static FZLeft Left;

struct FZRight
{
	float x, dX;
	float Height, rHeight; // Section Height
	dword 
		ScanLines, // Scan-lines left to draw
		Index;  // Current Vertex index
};

thread_local static FZRight Right;

thread_local static void *IX_Texture;
thread_local static void *IX_Page;
thread_local static word *IX_ZBuffer;
thread_local static dword IX_L2X, IX_L2Y;
thread_local static dword IX_FlatColor;
thread_local static dword IX_TFlatColor;

struct deltas
{
	float dRZdx;
};

#define L2SPANSIZE 4
#define SPANSIZE 16
#define fSPANSIZE 16.0f
thread_local static deltas ddx;
thread_local static deltas ddx32;

struct PolygonStats
{
	dword numCells;
	dword *areaHist;
	dword *widthHist;
	dword *heightHist;
};

thread_local PolygonStats IX_PolyStats;

void InitPolyStats(int32_t cells)
{
#ifdef MEASURE_POLYSTATS
	IX_PolyStats.numCells = cells;
	IX_PolyStats.areaHist = new dword [cells];
	IX_PolyStats.widthHist = new dword [cells];
	IX_PolyStats.heightHist = new dword [cells];

	memset(IX_PolyStats.areaHist, 0, cells * sizeof(dword));
	memset(IX_PolyStats.widthHist, 0, cells * sizeof(dword));
	memset(IX_PolyStats.heightHist, 0, cells * sizeof(dword));
#endif
}

void SavePolyStats(const char *fileName)
{	
#ifdef MEASURE_POLYSTATS
	FILE *F = fopen(fileName, "wt");

//	fprintf(F, "Area Stats:\n");
	mword i;
	mword cells = IX_PolyStats.numCells;
/*	for(i=0; i<cells; i++)
	{
		float area = (float)i/IX_PolyStats.numCells;
		area *= area * XRes * YRes * 2.0f;
		fprintf(F, "\t%f\t%d\n", area, IX_PolyStats.areaHist[i]);
	}*/
	fprintf(F, "Width Stats:\n");
	for(i=0; i<cells; i++)
	{
		float width = (i+0.5f)*XRes/IX_PolyStats.numCells;		
		fprintf(F, "\t%f\t%d\n", width, IX_PolyStats.widthHist[i]);
	}
	fprintf(F, "Height Stats:\n");
	for(i=0; i<cells; i++)
	{
		float height = (i+0.5f)*YRes/IX_PolyStats.numCells;
		fprintf(F, "\t%f\t%d\n", height, IX_PolyStats.heightHist[i]);
	}

	fclose(F);

	memset(IX_PolyStats.areaHist, 0, cells * sizeof(dword));
	memset(IX_PolyStats.widthHist, 0, cells * sizeof(dword));
	memset(IX_PolyStats.heightHist, 0, cells * sizeof(dword));
#endif
}

static void CalcRightSection (IXVertexG *V1, IXVertexG *V2)
{
	
	// Calculate number of scanlines
	dword iy1, iy2;
	//Fist(iy1,V1->y);
	//Fist(iy2,V2->y);
	iy1 = dword(ceilf(V1->y));
	iy2 = dword(ceilf(V2->y));

	Right.ScanLines = iy2 - iy1;
	
	if (Right.ScanLines == 0)
	{
		return;
	}
	
	// Calculate delta
	Right.Height = (V2->y - V1->y);
	float rHeight = Right.Height;
	rcpss(&rHeight);
	Right.dX = (V2->x - V1->x) * rHeight;
	sdword FPRevHeight;

	// Sub pixeling
	float prestep;
	prestep = (float)iy1 - V1->y;
	Right.x = V1->x + Right.dX * prestep;
}


static void CalcLeftSection (IXVertexG *V1, IXVertexG *V2)
{
	float RevHeight;
	
	// Calculate number of scanlines
	dword iy1, iy2;
	//Fist(iy1,V1->y);
	//Fist(iy2,V2->y);

	iy1 = dword(ceilf(V1->y));
	iy2 = dword(ceilf(V2->y));


	Left.ScanLines = iy2 - iy1;
	
	if (Left.ScanLines == 0)
	{
		return;
	}
	
	Left.Height = (V2->y - V1->y);
	RevHeight = Left.Height;
	rcpss(&RevHeight);
		
	// Calculate deltas for Z
	Left.dX = (V2->x - V1->x) * RevHeight;
	Left.dRZ = (V2->RZ - V1->RZ) * RevHeight;
	
	float prestep = ((float)iy1 - V1->y);
	Left.x  = V1->x  + Left.dX  * prestep;
	Left.RZ = V1->RZ + Left.dRZ * prestep;
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
			// ZBuffer test			
			if (_Z0 > *ZSpanPtr)
			{
				*ZSpanPtr = _Z0;
				*SpanPtr = IX_FlatColor;
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
			// ZBuffer test			
			if (_Z0 > *ZSpanPtr)
			{
				// *ZSpanPtr = _Z0;

				dword x = ( (*SpanPtr) & 0xFEFEFE) >> 1;
				*SpanPtr = x + IX_TFlatColor;
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
	int r = int(Verts->R),
		g = int(Verts->G),
		b = int(Verts->B);
	IX_FlatColor = (r<<16)+(g<<8)+b;
	IX_TFlatColor = (IX_FlatColor&0xFEFEFEFE)>>1;

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
	// Gradient method - slightly slower

	// use verts (0,1,nVerts-1) or (0,1,2).
	// better precision: use verts (0,1, right) if right != 1.

	float dy01 = Verts[1].y - Verts[0].y;
	float dy02 = Verts[2].y - Verts[0].y;
	float area = ((Verts[1].x - Verts[0].x) * dy02 - (Verts[2].x - Verts[0].x) * dy01);
/*#ifdef MEASURE_POLYSTATS
	IX_PolyStats.areaHist[Fist(sqrt(area/(XRes*YRes*2.0)) * IX_PolyStats.numCells)]++;
#endif*/


	float invArea = 1.0 / area;
	ddx.dRZdx = invArea * ((Verts[1].RZ - Verts[0].RZ) * dy02 - (Verts[2].RZ - Verts[0].RZ) * dy01);
		
	//invArea *= 256.0; 
	//dRdx = Fist(invArea * ((Verts[1].R - Verts[0].R) * dy02 - (Verts[2].R - Verts[0].R) * dy01));
	//dGdx = Fist(invArea * ((Verts[1].G - Verts[0].G) * dy02 - (Verts[2].G - Verts[0].G) * dy01));
	//dBdx = Fist(invArea * ((Verts[1].B - Verts[0].B) * dy02 - (Verts[2].B - Verts[0].B) * dy01));
	//dZdx = Fist(invArea * ((Verts[1].z - Verts[0].z) * dy02 - (Verts[2].z - Verts[0].z) * dy01));
	
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
		// section height stats
#ifdef MEASURE_POLYSTATS
	IX_PolyStats.heightHist[Fist(((float)SectionHeight/YRes) * IX_PolyStats.numCells)]++;
#endif
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
#ifdef MEASURE_POLYSTATS
			IX_PolyStats.widthHist[Fist(((float)Width/XRes) * IX_PolyStats.numCells)]++;
#endif

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
			fogRate = 1.0f - 1.0f * C_rFZP * V[i]->TPos.z;
			if (fogRate < 0.0f)
			{
				fogRate = 0.0f;
			}
			l_IXArray[i].R = V[i]->LR * fogRate;
			l_IXArray[i].G = V[i]->LG * fogRate;
			l_IXArray[i].B = V[i]->LB * fogRate;

			// protect against gouraud interpolation underflows
			if (l_IXArray[i].R < 2.0) l_IXArray[i].R = 2.0f;
			if (l_IXArray[i].G < 2.0) l_IXArray[i].G = 2.0f;
			if (l_IXArray[i].B < 2.0) l_IXArray[i].B = 2.0f;

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

void IX_Prefiller_FZ(Face* F, Vertex **V, dword numVerts, dword miplevel)
{
	SubInnerPtr = SubInnerLoop;
	PrefillerCommon(V, numVerts);
}

void IX_Prefiller_FAcZ(Face* F, Vertex **V, dword numVerts, dword miplevel)
{
	SubInnerPtr = SubInnerLoopT;
	PrefillerCommon(V, numVerts);
}
