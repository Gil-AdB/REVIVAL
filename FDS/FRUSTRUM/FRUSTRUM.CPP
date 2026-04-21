#include <stdio.h>
#include <stdlib.h>

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FDS_DEFS.H>

#include <Base/Scene.h>

#include "FRUSTRUM.H"
#include <atomic>

#define ClipAccurate
#define COUNT_POLYS
#define NO_MIPMAPS

float FOVX,FOVY;
float C_FZP,C_rFZP, C_NZP, C_rNZP;

std::atomic<dword> g_renderedPolys;

///////////////////////////////////////////////////////////////////////////////
// Third Generation Triangle -> Convex counterclockwise NGon Frustrum clipper.
// Features:
// Based on the usual Hodgeman-Sutherland polygon clipping algorithm.
// Cuts polygons in parts so texture-mappers can assume input polygons to have 
// a constant mipmapping level across them. this also allevates for affine gouraud
// interpolation errors.
// Version 1.0
// Log
// 30.03.03 project opened
//
// Notes
// (Twosided support): Vertices may not be in counterclockwise order at first.
// Order swapping is executed after Z-clipping.
// The CW/CCW check before vertex order swapping can be removed if 
// backface culling test will store its result in the face flags.

#define ENABLE_PIXELCOUNT


#define LOGTABLEPREC 8
#define EXPTABLEPREC 8
#define EXPTABLEMASK 0x000000FF

thread_local static float LogTable[1<<LOGTABLEPREC];
thread_local static float ExpTable[(1<<EXPTABLEPREC)+1];

float fastLog2(float x)
{
//	static const float b2 = 1.0 / log(2.0);
//	return log(x)*b2;
	sdword ix = *(sdword *)&x;
	return LogTable[(ix & 0x7f8000) >> (23-LOGTABLEPREC)] + ((ix  >> 23) - 127);
}

float fastPow2(float x)
{
//	static const float b2 = log(2.0);
//	return exp(b2*x);
	constexpr float ETP = (1 << EXPTABLEPREC);
	// generate pow2 LUT
	int32_t i = Fist(x * ETP);
	union {
		float e;
		int32_t le;
	};
	e = ExpTable[i & EXPTABLEMASK];

	// 3.5 cycles faster on P-4!!!!!!!!
	//le += (i & (~((1<<EXPTABLEPREC)-1)) ) << (23-EXPTABLEPREC);

	le += (i >> EXPTABLEPREC) << 23;
	return e;
}

static float invTable[32] = {0.0,
	2.0/1.0,  2.0/2.0,  2.0/3.0,  2.0/4.0,
	2.0/5.0,  2.0/6.0,  2.0/7.0,  2.0/8.0,
	2.0/9.0,  2.0/10.0, 2.0/11.0, 2.0/12.0,
	2.0/13.0, 2.0/14.0, 2.0/15.0, 2.0/16.0,
	2.0/17.0, 2.0/18.0, 2.0/19.0, 2.0/20.0,
	2.0/21.0, 2.0/22.0, 2.0/23.0, 2.0/24.0,
	2.0/25.0, 2.0/26.0, 2.0/27.0, 2.0/28.0,
	2.0/29.0, 2.0/30.0, 2.0/31.0, 
};

void FrustumClipper::Init()
{
	dword i;
	const float rl2 = 1.0f / logf(2.0f);

	memset(C_Verts, 0, sizeof(C_Verts));

	C_Prim = C_Ptr[0];
	C_Scnd = C_Ptr[1];
	C_Tetr = C_Ptr[2];

	union
	{
		float x;
		dword ix;
	};
	// generate log2 LUT
	for (i = 0; i < 1 << LOGTABLEPREC; i++)
	{
		ix = (127 << 23) + (i << (23-LOGTABLEPREC));
		LogTable[i] = logf(x) * rl2;
	}

	const float b2 = logf(2.0f);
	const float rETP = 1.0f / (1 << EXPTABLEPREC);
	// generate pow2 LUT
	for (i = 0; i <= 1 << EXPTABLEPREC; i++)
	{
		x = i * rETP;
		ExpTable[i] = expf(b2*x);
	}
	
	//for(i=0;i<10; i++)
	//{
	//	x = RAND_15() / 391.241;

	//	float l_acc = log(x) * rl2;
	//	float l_tbl = fastLog2(x);

	//	l_acc = exp(x * b2);
	//	l_tbl = fastPow2(x);
	//}
}

void viewportInit(Viewport& vp, const Scene* Sc) {
	vp.NearZ = Sc->NZP;
	vp.FarZ = Sc->FZP;
	vp.iNearZ = 1.0f / Sc->NZP;
	vp.iFarZ = 1.0f / Sc->FZP;

	// scene should have a flag for modifyable clipping bounds.
	vp.ClipX1 = 0.0;
	vp.ClipX2 = XRes;
	vp.ClipY1 = 0.0;
	vp.ClipY2 = YRes;
}

void viewportCalcFlags(const Viewport& vp, Vertex* v) {
	if (v->PX < vp.ClipX1) v->Flags = Vtx_VisLeft; else v->Flags = 0;
	if (v->PX > vp.ClipX2) v->Flags |= Vtx_VisRight;
	if (v->PY < vp.ClipY1) v->Flags |= Vtx_VisUp;
	if (v->PY > vp.ClipY2) v->Flags |= Vtx_VisDown;
}

void viewportCalcYFlags(const Viewport& vp, Vertex* v) {
	if (v->PY < vp.ClipY1) v->Flags = Vtx_VisUp; else v->Flags = 0;
	if (v->PY > vp.ClipY2) v->Flags |= Vtx_VisDown;
}

void FrustumClipper::InitViewport(Scene *Sc)
{
	viewportInit(C_VP, Sc);
}

void FrustumClipper::Calc_Flags(Vertex *V)
{
	viewportCalcFlags(C_VP, V);
	C_Flags |= V->Flags;
}

void FrustumClipper::Calc_YFlags(Vertex *V)
{
	viewportCalcYFlags(C_VP, V);
	C_Flags |= V->Flags;
}

void FrustumClipper::Swap()
{
	C_Scnd[C_numVerts] = C_Scnd[0];

	Vertex **Swap = C_Prim;
	C_Prim = C_Scnd;
	C_Scnd = Swap;
}

void FrustumClipper::Near()
{
	dword i,j = 0;
	_IB = C_Prim[0];

	float t;
//	newVert = C_Verts + 0;
	for(i=0; i<C_numVerts;i++)
	{
		_IA = _IB;
		_IB = C_Prim[i+1];
		if  (_IA->Flags & Vtx_VisNear)
		{
			if (_IB->Flags & Vtx_VisNear) continue;
		} else {
			C_Scnd[j++] = _IA;
			if (!(_IB->Flags & Vtx_VisNear)) continue;
		}
		t = (C_VP.NearZ - _IA->TPos.z) / (_IB->TPos.z - _IA->TPos.z);
		
		newVert->TPos.x = _IA->TPos.x + t * (_IB->TPos.x - _IA->TPos.x);
		newVert->TPos.y = _IA->TPos.y + t * (_IB->TPos.y - _IA->TPos.y);
		newVert->TPos.z = C_VP.NearZ;
		newVert->LR = _IA->LR + t * (_IB->LR - _IA->LR);
		newVert->LG = _IA->LG + t * (_IB->LG - _IA->LG);
		newVert->LB = _IA->LB + t * (_IB->LB - _IA->LB);
		newVert->U = _IA->U + t * (_IB->U - _IA->U);
		newVert->V = _IA->V + t * (_IB->V - _IA->V);
		newVert->EU = _IA->EU + t * (_IB->EU - _IA->EU);
		newVert->EV = _IA->EV + t * (_IB->EV - _IA->EV);

		newVert->PX = newVert->TPos.x * C_VP.iNearZ;
		newVert->PY = newVert->TPos.y * C_VP.iNearZ;
		newVert->RZ = C_VP.iNearZ;
		newVert->UZ = newVert->U * C_VP.iNearZ;
		newVert->VZ = newVert->V * C_VP.iNearZ;
		newVert->EUZ = newVert->EU * C_VP.iNearZ;
		newVert->EVZ = newVert->EV * C_VP.iNearZ;

		Calc_Flags(newVert);
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	Swap();
}

void FrustumClipper::Far()
{
	dword i,j = 0;
	_IB = C_Prim[0];

	float t;
	//newVert = C_Verts + 2;
	for(i=0;i<C_numVerts;i++)
	{
		_IA = _IB;
		_IB = C_Prim[i+1];
		if  (_IA->Flags & Vtx_VisFar)
		{
			if (_IB->Flags & Vtx_VisFar) continue;
		} else {
			C_Scnd[j++] = _IA;
			if (!(_IB->Flags & Vtx_VisFar)) continue;
		}
		t = (C_VP.FarZ-_IA->TPos.z)/(_IB->TPos.z-_IA->TPos.z);		
		newVert->PX = C_VP.iFarZ * (_IA->TPos.x + t * (_IB->TPos.x-_IA->TPos.x) );
		newVert->PY = C_VP.iFarZ * (_IA->TPos.y + t * (_IB->TPos.y-_IA->TPos.y) );			
		newVert->LR = _IA->LR + t * (_IB->LR-_IA->LR);
		newVert->LG = _IA->LG + t * (_IB->LG-_IA->LG);
		newVert->LB = _IA->LB + t * (_IB->LB-_IA->LB);
		newVert->TPos.z = C_VP.FarZ;
		newVert->RZ = C_VP.iFarZ;
		newVert->U = _IA->U + t * (_IB->U-_IA->U);
		newVert->V = _IA->V + t * (_IB->V-_IA->V);
		newVert->UZ = C_VP.iFarZ * newVert->U;
		newVert->VZ = C_VP.iFarZ * newVert->V;
		newVert->EU = _IA->EU + t * (_IB->EU - _IA->EU);
		newVert->EV = _IA->EV + t * (_IB->EV - _IA->EV);
		newVert->EUZ = C_VP.iFarZ * newVert->EU;
		newVert->EVZ = C_VP.iFarZ * newVert->EV;
		Calc_Flags(newVert);
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	Swap();
}

// one day, with the appropriate technology, this will use functions generated by code
void FrustumClipper::FInterpolator(Vertex *V, float t)
{
	//if (isnan(t) || t < -0.01 || t > 1.01) {
	//	__asm int 3
	//}
	V->PX = _IA->PX + t * (_IB->PX - _IA->PX);
	V->PY = _IA->PY + t * (_IB->PY - _IA->PY);
	V->LR = _IA->LR + t * (_IB->LR - _IA->LR);
	V->LG = _IA->LG + t * (_IB->LG - _IA->LG);
	V->LB = _IA->LB + t * (_IB->LB - _IA->LB);
	V->RZ = _IA->RZ + t * (_IB->RZ - _IA->RZ);
	V->UZ = _IA->UZ + t * (_IB->UZ - _IA->UZ);
	V->VZ = _IA->VZ + t * (_IB->VZ - _IA->VZ);
	V->EUZ = _IA->EUZ + t * (_IB->EUZ - _IA->EUZ);
	V->EVZ = _IA->EVZ + t * (_IB->EVZ - _IA->EVZ);
	V->i = _IA->i;

	// calculate (U,V)
	V->TPos.z = 1.0 / V->RZ;
	V->U = V->UZ * V->TPos.z;
	V->V = V->VZ * V->TPos.z;
	V->EU = V->EUZ * V->TPos.z;
	V->EV = V->EVZ * V->TPos.z;
}

void FrustumClipper::Left()
{
	dword i, j = 0;
	_IB = C_Prim[0];

//	newVert = C_Verts + 4;
	for(i=0; i<C_numVerts; i++)
	{
		_IA = _IB;
		_IB = C_Prim[i+1];
		if  (_IA->Flags & Vtx_VisLeft)
		{
			if (_IB->Flags & Vtx_VisLeft) continue;
		} else {
			C_Scnd[j++] = _IA;
			if (!(_IB->Flags & Vtx_VisLeft)) continue;
		}
		FInterpolator(newVert, (C_VP.ClipX1-_IA->PX)/(_IB->PX-_IA->PX));
		newVert->PX = C_VP.ClipX1;
		Calc_YFlags(newVert);
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	Swap();
}

void FrustumClipper::Right()
{
	dword i, j = 0;
	_IB = C_Prim[0];

	//newVert = C_Verts + 6;
	for(i=0; i<C_numVerts; i++)
	{
		_IA = _IB;
		_IB = C_Prim[i+1];
		if  (_IA->Flags & Vtx_VisRight)
		{
			if (_IB->Flags & Vtx_VisRight) continue;
		} else {
			C_Scnd[j++] = _IA;
			if (!(_IB->Flags & Vtx_VisRight)) continue;
		}
		FInterpolator(newVert, (C_VP.ClipX2-_IA->PX)/(_IB->PX-_IA->PX));
		newVert->PX = C_VP.ClipX2;
		Calc_YFlags(newVert);
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	Swap();
}

void FrustumClipper::Up()
{
	dword i, j = 0;
	_IB = C_Prim[0];

	// WTFWTFWTF
	//newVert = C_Verts + 8 + 3;
	for(i=0; i<C_numVerts; i++)
	{
		_IA = _IB;
		_IB = C_Prim[i+1];
		if  (_IA->Flags & Vtx_VisUp)
		{
			if (_IB->Flags & Vtx_VisUp) continue;
		} else {
			C_Scnd[j++] = _IA;
			if (!(_IB->Flags & Vtx_VisUp)) continue;
		}
		FInterpolator(newVert, (C_VP.ClipY1-_IA->PY)/(_IB->PY-_IA->PY));
		newVert->Flags = 0;
		newVert->PY = C_VP.ClipY1;
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	Swap();
}

void FrustumClipper::Down()
{
	dword i, j = 0;
	_IB = C_Prim[0];

	//newVert = C_Verts + 10;
	for(i=0; i<C_numVerts; i++)
	{
		_IA = _IB;
		_IB = C_Prim[i+1];
		if  (_IA->Flags & Vtx_VisDown)
		{
			if (_IB->Flags & Vtx_VisDown) continue;
		} else {
			C_Scnd[j++] = _IA;
			if (!(_IB->Flags & Vtx_VisDown)) continue;
		}
		FInterpolator(newVert, (C_VP.ClipY2-_IA->PY)/(_IB->PY-_IA->PY));
		newVert->Flags = 0;
		newVert->PY = C_VP.ClipY2;
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	Swap();
}


void FrustumClipper::CorrectCWOrder()
{
	Vertex *A = C_Prim[0], *B = C_Prim[1], *C = C_Prim[2];
	
	float area = (B->PX - A->PX) * (C->PY - A->PY) - (C->PX - A->PX) * (B->PY - A->PY);

	if (area > 0.0)
	{
		for(mword i=0; i<C_numVerts; i++)
		{
			C_Scnd[i] = C_Prim[C_numVerts-i];
		}
		Swap();
	}
}

void FrustumClipper::YSort(Vertex **Prim, Vertex **Scnd, mword nVerts)
{
	// Find vertex with minimal y-value.
	mword i, j;
	float minY = Prim[0]->PY;
	mword minYIndex = 0;
	for (i=1; i<nVerts; i++)
	{
		float y = Prim[i]->PY;
		if (y < minY)
		{
			minYIndex = i;
			minY = y;
		}
	}
	
	// rotate vertices so that minimal y vertex appears first
	for(i=minYIndex; i<nVerts; i++)
	{
		Scnd[i-minYIndex] = Prim[i];
	}
	
	j = nVerts - minYIndex;
	for(i=0; i<minYIndex; i++)
	{
		Scnd[i+j] = Prim[i];
	}

}

void FrustumClipper::MiplevelClipper(Face *F, RasterFunc filler)
{
	const float mipBias = 0.5;

	Texture *Tx = F->Txtr->Txtr;
	int32_t L = Tx->numMipmaps-1; // maximal level	

	// polygon texel / pixel area
	float pixArea = 0.0f, texArea = 0.0f;

	Vertex *A = C_Prim[0], *B, *C;
	mword i, j;
	for(i=2; i<C_numVerts; i++)
	{
		B = C_Prim[i-1];
		C = C_Prim[i];
	
		pixArea += (C->PX - A->PX) * (B->PY - A->PY) - (B->PX - A->PX) * (C->PY - A->PY);
		texArea += (C->U  - A->U ) * (B->V  - A->V ) - (B->U  - A->U ) * (C->V  - A->V );
	}

#ifdef ENABLE_PIXELCOUNT
	FillerPixelcount = FillerPixelcount + 0.5f * fabs(pixArea);
#endif
	// Simple miplevel calculation
	//if (!Keyboard[ScY])
	static const float MinSize = XRes*YRes * 0.02; // update if XRes and/or YRes changes...

	if ((Tx->Flags & Txtr_Nomip) ||  pixArea < MinSize)
	{
		if (Tx->Flags & Txtr_Nomip) {
			g_MipLevel = 0;
		} else {
			int32_t mip = 0.5 * fastLog2(fabs(texArea * Tx->SizeX * Tx->SizeY / pixArea)) + mipBias;
			if (mip < 0) mip = 0;
			if (mip > L) mip = L;
			g_MipLevel = mip;
		}

#ifdef COUNT_POLYS
		g_renderedPolys++;
#endif
#ifdef NO_MIPMAPS
		g_MipLevel = 0;
#endif
		filler(F, C_Prim, C_numVerts, g_MipLevel);
		return;
	}

	// mipmap level constant
	float q = 0;
	for(i=0; i<C_numVerts; i++)
	{
		q += fastLog2(C_Prim[i]->RZ);
	}
	
	float _C = 0.5 * fastLog2(fabs(texArea * Tx->SizeX * Tx->SizeY / pixArea)) + invTable[C_numVerts] * q + mipBias;

	// calculate range of 1/z values along polygon
	float mrz = 1E+38, Mrz = 0;
	int32_t mlindex = 0;
	for(i=0; i<C_numVerts; i++)
	{
//		level = _C - 2 * log2(rz);
		float rz = C_Prim[i]->RZ;

		if (rz<mrz) 
		{
			mrz = rz;
		}

		if (rz>Mrz)
		{
			Mrz = rz;
			mlindex = i;
		}
	}
	
	// calculate mipmapping level range
	// assuming rounding state = ceil
	float ml = _C - 2 * fastLog2(Mrz);
	float Ml = _C - 2 * fastLog2(mrz);

	int32_t iml = Fist(ml-0.99999);
	int32_t iMl = Fist(Ml-0.99999);

	if (iml < 0) iml = 0;
	if (iml > L) iml = L;
	if (iMl < 0) iMl = 0;
	if (iMl > L) iMl = L;


	// simple case (uniform mipmap across polygon)
	// should also include all polygons with reletively small onscreen area, because
	// polysplitting is REALLY slow.
	if (iml == iMl)
	{
		g_MipLevel = iml;
		if (g_MipLevel < 0) g_MipLevel = 0;
#ifdef COUNT_POLYS
		g_renderedPolys++;
#endif
#ifdef NO_MIPMAPS
		g_MipLevel = 0;
#endif
		filler(F, C_Prim, C_numVerts, g_MipLevel);
		return;
	}

	// scanconversion
	int32_t left, right, limit;

	float t;
	mword numSectionVerts;
//	newVert = C_Verts + 12;


	left = right = mlindex;

	// build initial section	
	mword numRight = 0; // notice: Right vertex #0 is always ignored
	numSectionVerts = 0;

	static const float h_log_2 = 0.5 * log(2.0);
	static const float limrz_mult = 1.0 / sqrt(2.0);
	//float limrz = exp(h_log_2*(_C - iml));
	float limrz = fastPow2(0.5*(_C - iml));

	for(limit = iml+1;limit <= iMl; limit++)
	{
		// calculate crossover 1/z
		limrz *= limrz_mult;

		if (limrz >= Mrz) continue;
		if (limrz <= mrz) break;

		// traverse left
		while(C_Prim[left]->RZ > limrz)
		{
			C_Scnd[numSectionVerts++] = C_Prim[left];
			if (left == C_numVerts) left = 0;
			left++;			
		}

		// traverse right
		while(C_Prim[right]->RZ > limrz)
		{
			C_Tetr[numRight++] = C_Prim[right];
			if (right == 0) right = C_numVerts;
			right--;
		}

		// intersect left
		t = (limrz - C_Prim[left-1]->RZ) / (C_Prim[left]->RZ - C_Prim[left-1]->RZ);

		_IA = C_Prim[left-1];
		_IB = C_Prim[left];
		FInterpolator(newVert, t);
		C_Scnd[numSectionVerts++] = newVert;
		newVert++;
		
		// intersect right
		t = (limrz - C_Prim[right+1]->RZ) / (C_Prim[right]->RZ - C_Prim[right+1]->RZ);

		_IA = C_Prim[right+1];
		_IB = C_Prim[right];
		FInterpolator(newVert, t);
		C_Scnd[numSectionVerts++] = newVert;
		newVert++;

		// write vertices from right edge traversal in reverse order
		for(j = 1; j<numRight; j++)
			C_Scnd[numSectionVerts++] = C_Tetr[numRight-j];

		g_MipLevel = limit-1;
		YSort(C_Scnd, C_Tetr, numSectionVerts);
#ifdef COUNT_POLYS
		g_renderedPolys++;
#endif
#ifdef NO_MIPMAPS
		g_MipLevel = 0;
#endif
		filler(F, C_Tetr, numSectionVerts, g_MipLevel);

		// prepare new section - add last two intersection points
		numRight = 1; // vertex #0 is ignored
		// two earlier intersect points in reverse order
		C_Scnd[0] = newVert - 1;
		C_Scnd[1] = newVert - 2;
		numSectionVerts = 2;
	}

	// generate/draw last polygon section
	g_MipLevel = iMl;

	if (right == 0) right = C_numVerts;
	if (left == mlindex)
	{
#ifdef COUNT_POLYS
		g_renderedPolys++;
#endif
#ifdef NO_MIPMAPS
		g_MipLevel = 0;
#endif
		filler(F, C_Prim, C_numVerts, g_MipLevel);
		return;
	}
	while (left != right)
	{
		C_Scnd[numSectionVerts++] = C_Prim[left];
		if (left == C_numVerts) left = 0;
		left++;			
	}
	C_Scnd[numSectionVerts++] = C_Prim[left];
	YSort(C_Scnd, C_Tetr, numSectionVerts);
#ifdef COUNT_POLYS
	g_renderedPolys++;
#endif
#ifdef NO_MIPMAPS
	g_MipLevel = 0;
#endif
	filler(F, C_Tetr, numSectionVerts, g_MipLevel);
}

float FrustumClipper::PolyArea()
{
	// polygon pixel area
	float pixArea = 0.0f;

	Vertex *A = C_Prim[0], *B, *C;
	mword i, j;
	for(i=2; i<C_numVerts; i++)
	{
		B = C_Prim[i-1];
		C = C_Prim[i];
	
		pixArea += (C->PX - A->PX) * (B->PY - A->PY) - (B->PX - A->PX) * (C->PY - A->PY);
	}

	return fabs(pixArea) * 0.5f;
}


void FrustumClipper::Render(Face *F, RasterFunc filler, bool isEnvCoords)
{
	mword i, j;

	Vertex* A = &C_Verts[0];
	Vertex* B = &C_Verts[1];
	Vertex* C = &C_Verts[2];

	*A = *F->A;
	*B = *F->B;
	*C = *F->C;
	newVert = &C_Verts[3];

	A->U = F->U1; A->V = F->V1;
	B->U = F->U2; B->V = F->V2;
	C->U = F->U3; C->V = F->V3;

	if (F->Flags & Face_Reflective) {
		A->EU = F->EU1; A->EV = F->EV1;
		B->EU = F->EU2; B->EV = F->EV2;
		C->EU = F->EU3; C->EV = F->EV3;
	}

	A->UZ = A->U * A->RZ; A->VZ = A->V * A->RZ;
	B->UZ = B->U * B->RZ; B->VZ = B->V * B->RZ;
	C->UZ = C->U * C->RZ; C->VZ = C->V * C->RZ;
	if (F->Flags & Face_Reflective) {
		A->EUZ = A->EU * A->RZ; A->EVZ = A->EV * A->RZ;
		B->EUZ = B->EU * B->RZ; B->EVZ = B->EV * B->RZ;
		C->EUZ = C->EU * C->RZ; C->EVZ = C->EV * C->RZ;
	}

	auto [Af, Bf, Cf] = std::tuple(A->Flags, B->Flags, C->Flags);
	Calc_Flags(A);
	Calc_Flags(B);
	Calc_Flags(C);
	A->Flags |= Af;
	B->Flags |= Bf;
	C->Flags |= Cf;

	C_Flags = A->Flags | B->Flags | C->Flags;

	// Assign polygon
	C_numVerts = 3;
	C_Prim[0] = A;
	C_Prim[1] = B;
	C_Prim[2] = C;
	C_Prim[3] = A;

	if (C_Flags&Vtx_VisNear)  Near(); 
	if (C_Flags&Vtx_VisFar)   Far();

	// Z-Clipping may have placed the entire polygon outside 2D boundary
	/*j = C_Prim[0]->Flags;
	for(i=1; i<C_numVerts; i++)
		j &= C_Prim[i]->Flags;
	if (j) return;*/

	// Swap vertex order if it isn't CCW.	
	CorrectCWOrder();

	if (C_Flags&Vtx_VisLeft)  Left();
	if (C_Flags&Vtx_VisRight) Right();
	if (C_Flags&Vtx_VisUp)    Up();
	if (C_Flags&Vtx_VisDown)  Down();

	// Clipping process may have eliminated the entire polygon
	if (!C_numVerts) return;

	YSort(C_Prim, C_Scnd, C_numVerts);
	
	// triangulate result NGon and sort triangle vertices by Y
	// to conform to legacy rasterizer convension

	if (F->Txtr->Txtr)
	{
		Swap();
		MiplevelClipper(F, filler);
	} else {
#ifdef ENABLE_PIXELCOUNT
		FillerPixelcount = FillerPixelcount + PolyArea();
#endif
#ifdef COUNT_POLYS
		g_renderedPolys++;
#endif		
		filler(F, C_Scnd, C_numVerts, g_MipLevel);
	}
	/*for(i=2; i<C_numVerts; i++)
	{
		if (C_Scnd[i-1]->PY < C_Scnd[i]->PY)
			filler(C_Scnd[0], C_Scnd[i-1], C_Scnd[i]);
		else
			filler(C_Scnd[0], C_Scnd[i], C_Scnd[i-1]);
	}*/
}
