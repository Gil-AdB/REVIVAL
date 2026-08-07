#include <stdio.h>
#include <stdlib.h>

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FDS_DEFS.H>

#include <Base/Scene.h>
#include <Base/CameraContext.h>
#include <Base/FeatureFlags.h>
#include <Base/RenderStats.h>

#include "FRUSTRUM.H"
#include "ClipperTileRect.h"
#include <atomic>

// Trilinear albedo filtering: export the continuous mip FRACTION to the
// Mekalele rasterizer (which can't receive it through the fixed RasterFunc
// signature) via a thread_local. Defined in Mekalele.cpp; ∈ [0,1] toward
// mip+1. Reset to 0 on non-mip / unknown filler paths.
namespace meka { extern thread_local float g_tlsMipFrac; }
#include <climits>
#include <mutex>

// 4-lane SIMD for FInterpolator's contiguous Vertex-field lerps.
// Vertex is #pragma pack(1) so the relevant 4-float blocks
// (PX/PY/UZ/VZ at offset 4 and TTangent.x/.y/.z/EUZ at offset 84) are
// dense 16-byte regions safe to load unaligned.
#include "simde/x86/sse.h"
#include <simd/vectorclass.h>

#define ClipAccurate
#define COUNT_POLYS
// NO_MIPMAPS: was force-disabling all mip selection (g_MipLevel=0 at
// every decision site) — the resulting unfiltered minification is a
// full-screen texel shimmer under camera motion (measured: ~10%-of-scale
// mean per-frame diff across entire facades). Re-enabled behind the
// mip_bias/mip_hysteresis flags; the historically-buggy split path is
// still exercised — watch for traversal artifacts.
//#define NO_MIPMAPS

// FOVX, FOVY, C_FZP, C_rFZP, C_NZP, C_rNZP now live as fields of
// fds::g_mainFrame (see FrameState.h). Legacy names defined as
// reference aliases in FrameState.cpp.

std::atomic<dword> g_renderedPolys;
std::atomic<bool> g_HideInnerXpar{false};
std::atomic<bool> g_HideOuterXpar{false};

// Per-thread clipper-tile ownership rect — see ClipperTileRect.h.
// Defaults to "no clamp" so rasterizer callers outside a clipper
// Render() (none today, but defensive) don't lose pixels.
thread_local fds::ClipperTileRect fds::g_clipperTileRect = {
    INT_MIN, INT_MAX, INT_MIN, INT_MAX
};

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

// Read-only across the whole process — shared by all worker threads
// so each core's L1 can warm a single copy. Initialized once via
// std::call_once from FrustumClipper::Init().
static float LogTable[1<<LOGTABLEPREC];
static float ExpTable[(1<<EXPTABLEPREC)+1];

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
	memset(C_Verts, 0, sizeof(C_Verts));

	C_Prim = C_Ptr[0];
	C_Scnd = C_Ptr[1];
	C_Tetr = C_Ptr[2];

	// LogTable / ExpTable are read-only across the whole process. Init
	// once via std::call_once — all worker threads share one copy, so
	// each core's L1 warms the same lines and we avoid per-thread TLS
	// duplication + lookup cost. Without this gate, every per-tile
	// FrustumClipper rebuilt the tables, racking up ~60k logf/expf
	// calls per frame in greets (5 lights × 24 shadow tiles × 513
	// transcendentals).
	static std::once_flag sLutOnce;
	std::call_once(sLutOnce, []{
		dword i;
		const float rl2 = 1.0f / logf(2.0f);
		union { float x; dword ix; };
		for (i = 0; i < 1 << LOGTABLEPREC; i++)
		{
			ix = (127 << 23) + (i << (23-LOGTABLEPREC));
			LogTable[i] = logf(x) * rl2;
		}
		const float b2 = logf(2.0f);
		const float rETP = 1.0f / (1 << EXPTABLEPREC);
		for (i = 0; i <= 1 << EXPTABLEPREC; i++)
		{
			x = i * rETP;
			ExpTable[i] = expf(b2*x);
		}
	});
	
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

void FrustumClipper::publishClipperTileRect()
{
	constexpr int TS = 8;
	fds::g_clipperTileRect = {
		int(C_VP.ClipX1) / TS,
		(int(C_VP.ClipX2) - 1) / TS,
		int(C_VP.ClipY1) / TS,
		(int(C_VP.ClipY2) - 1) / TS,
	};
}

FrustumClipper::~FrustumClipper()
{
	// Reset to "no clamp" so later same-thread rasterizer callers
	// outside a clipper Render() (sky cube, sprites, particles, …)
	// don't inherit this worker's rect.
	fds::g_clipperTileRect = { INT_MIN, INT_MAX, INT_MIN, INT_MAX };
}

void FrustumClipper::SetClippingExtents(float x1, float y1, float x2, float y2)
{
	C_VP.ClipX1 = x1;
	C_VP.ClipY1 = y1;
	C_VP.ClipX2 = x2;
	C_VP.ClipY2 = y2;
	publishClipperTileRect();
}

void FrustumClipper::InitViewport(Scene *Sc)
{
	viewportInit(C_VP, Sc);
	publishClipperTileRect();
}

void viewportInit(Viewport& vp, const fds::CameraContext& cam) {
	// Per-pass projection — pulls near/far from the active CameraContext
	// instead of Sc->NZP/Sc->FZP. Used by the shadow tile workers so
	// each light's clipper test against its own range without the
	// orchestrator mutating Sc.
	vp.NearZ  = cam.nearZ;
	vp.iNearZ = cam.invNearZ;
	vp.FarZ   = cam.farZ;
	vp.iFarZ  = cam.invFarZ;
	vp.ClipX1 = 0.0f;
	vp.ClipX2 = float(XRes);
	vp.ClipY1 = 0.0f;
	vp.ClipY2 = float(YRes);
}

void FrustumClipper::InitViewport(const fds::CameraContext& cam)
{
	viewportInit(C_VP, cam);
	publishClipperTileRect();
}

// Calc_Flags / Swap / CorrectCWOrder moved to header as standard `inline`
// member functions (FRUSTRUM.H) so the hot Render() body sees their bodies
// and elides the call boundary.

void FrustumClipper::Calc_YFlags(Vertex *V)
{
	viewportCalcYFlags(C_VP, V);
	C_Flags |= V->Flags;
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
		t = (C_VP.NearZ - _IA->TPos_AOS.z) / (_IB->TPos_AOS.z - _IA->TPos_AOS.z);
		
		newVert->TPos_AOS.x = _IA->TPos_AOS.x + t * (_IB->TPos_AOS.x - _IA->TPos_AOS.x);
		newVert->TPos_AOS.y = _IA->TPos_AOS.y + t * (_IB->TPos_AOS.y - _IA->TPos_AOS.y);
		newVert->TPos_AOS.z = C_VP.NearZ;
		newVert->LR = _IA->LR + t * (_IB->LR - _IA->LR);
		newVert->LG = _IA->LG + t * (_IB->LG - _IA->LG);
		newVert->LB = _IA->LB + t * (_IB->LB - _IA->LB);
		newVert->U = _IA->U + t * (_IB->U - _IA->U);
		newVert->V = _IA->V + t * (_IB->V - _IA->V);
		newVert->EU = _IA->EU + t * (_IB->EU - _IA->EU);
		newVert->EV = _IA->EV + t * (_IB->EV - _IA->EV);
		// View-space shading normal + tangent. Linear-in-screen
		// interpolation — nlerp shape: rasterizer renormalizes
		// per-pixel. Object-space N stays untouched on source
		// vertices (read by forward-path Lighting() pre-clip); the
		// clipped n-gon vertices only need the view-space TN/TTangent
		// that the deferred rasterizer reads.
		newVert->TN.x = _IA->TN.x + t * (_IB->TN.x - _IA->TN.x);
		newVert->TN.y = _IA->TN.y + t * (_IB->TN.y - _IA->TN.y);
		newVert->TN.z = _IA->TN.z + t * (_IB->TN.z - _IA->TN.z);
		newVert->TTangent.x = _IA->TTangent.x + t * (_IB->TTangent.x - _IA->TTangent.x);
		newVert->TTangent.y = _IA->TTangent.y + t * (_IB->TTangent.y - _IA->TTangent.y);
		newVert->TTangent.z = _IA->TTangent.z + t * (_IB->TTangent.z - _IA->TTangent.z);
		// Static-shadow lightmap bary on F. Same linear-in-object-space
		// interp as U/V here (t comes from TPos_AOS.z, which is the object-
		// space parameter — Z is linear in world coords along the edge).
		// Linear is correct for object-space attributes; the perspective-
		// correct FInterpolator path used by Left/Right/Up/Down handles
		// screen-affine t separately.
		newVert->OrigBaryB = _IA->OrigBaryB + t * (_IB->OrigBaryB - _IA->OrigBaryB);
		newVert->OrigBaryC = _IA->OrigBaryC + t * (_IB->OrigBaryC - _IA->OrigBaryC);
		// S1b shell entry height (--pom_shell) — another object-space
		// per-vertex attribute, same linear treatment as OrigBary here.
		newVert->ShellH = _IA->ShellH + t * (_IB->ShellH - _IA->ShellH);

		newVert->PX = newVert->TPos_AOS.x * C_VP.iNearZ;
		newVert->PY = newVert->TPos_AOS.y * C_VP.iNearZ;
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
		t = (C_VP.FarZ-_IA->TPos_AOS.z)/(_IB->TPos_AOS.z-_IA->TPos_AOS.z);		
		newVert->PX = C_VP.iFarZ * (_IA->TPos_AOS.x + t * (_IB->TPos_AOS.x-_IA->TPos_AOS.x) );
		newVert->PY = C_VP.iFarZ * (_IA->TPos_AOS.y + t * (_IB->TPos_AOS.y-_IA->TPos_AOS.y) );			
		newVert->LR = _IA->LR + t * (_IB->LR-_IA->LR);
		newVert->LG = _IA->LG + t * (_IB->LG-_IA->LG);
		newVert->LB = _IA->LB + t * (_IB->LB-_IA->LB);
		newVert->TPos_AOS.z = C_VP.FarZ;
		newVert->RZ = C_VP.iFarZ;
		newVert->U = _IA->U + t * (_IB->U-_IA->U);
		newVert->V = _IA->V + t * (_IB->V-_IA->V);
		newVert->UZ = C_VP.iFarZ * newVert->U;
		newVert->VZ = C_VP.iFarZ * newVert->V;
		newVert->EU = _IA->EU + t * (_IB->EU - _IA->EU);
		newVert->EV = _IA->EV + t * (_IB->EV - _IA->EV);
		newVert->EUZ = C_VP.iFarZ * newVert->EU;
		newVert->EVZ = C_VP.iFarZ * newVert->EV;
		newVert->TN.x = _IA->TN.x + t * (_IB->TN.x - _IA->TN.x);
		newVert->TN.y = _IA->TN.y + t * (_IB->TN.y - _IA->TN.y);
		newVert->TN.z = _IA->TN.z + t * (_IB->TN.z - _IA->TN.z);
		newVert->TTangent.x = _IA->TTangent.x + t * (_IB->TTangent.x - _IA->TTangent.x);
		newVert->TTangent.y = _IA->TTangent.y + t * (_IB->TTangent.y - _IA->TTangent.y);
		newVert->TTangent.z = _IA->TTangent.z + t * (_IB->TTangent.z - _IA->TTangent.z);
		// Static-shadow lightmap bary on F (linear-in-object-space; t
		// is the object-space parameter from TPos_AOS.z).
		newVert->OrigBaryB = _IA->OrigBaryB + t * (_IB->OrigBaryB - _IA->OrigBaryB);
		newVert->OrigBaryC = _IA->OrigBaryC + t * (_IB->OrigBaryC - _IA->OrigBaryC);
		newVert->ShellH    = _IA->ShellH + t * (_IB->ShellH - _IA->ShellH);
		Calc_Flags(newVert);
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	Swap();
}

// 4-wide lerp helper for contiguous Vertex float blocks. Loads/stores
// are unaligned (Vertex is pack(1) and not 16-byte aligned on the
// clipper's C_Verts array). a + t*(b - a) — one FMA-friendly Vec4f
// triple replacing four scalar lerps.
static inline void lerp4_unaligned(float *out, const float *a, const float *b, float t)
{
	const Vec4f va = Vec4f().load(a);
	const Vec4f vb = Vec4f().load(b);
	(va + t * (vb - va)).store(out);
}

// 2-wide sibling. Same `a + t*(b - a)` expression, so a pair lerped here is
// bit-identical to the same pair lerped scalar or inside a Vec4f — only the
// lane count differs. Needed because the Vertex reorder (see Vertex.h) groups
// the hot fields for cache-line span, which leaves the lerpable runs as pairs
// (PX/PY, UZ/VZ, EUZ/EVZ, TTangent.y/.z) rather than quads.
// It is written scalar on purpose: under -ffp-contract=fast each line contracts
// to the same single FMA the Vec4f lane would issue, which is exactly why the
// pre-existing scalar `V->RZ = _IA->RZ + t * (...)` matched the 4-wide PX/PY
// block bit-for-bit.
static inline void lerp2_unaligned(float *out, const float *a, const float *b, float t)
{
	out[0] = a[0] + t * (b[0] - a[0]);
	out[1] = a[1] + t * (b[1] - a[1]);
}

// one day, with the appropriate technology, this will use functions generated by code
void FrustumClipper::FInterpolator(Vertex *V, float t)
{
	//if (isnan(t) || t < -0.01 || t > 1.01) {
	//	__asm int 3
	//}
	// Field runs follow the Vertex layout in Base/Vertex.h, which is grouped
	// for the two hot loops' cache-line span, not for this function's lane
	// width. Every run below is the SAME `a + t*(b - a)` expression, so the
	// values are bit-identical to the pre-reorder 4-wide blocks — only the
	// grouping changed (PX/PY were quaded with UZ/VZ, TTangent with EUZ).
	//
	// Block 1: PX, PY — the pair at offset 0.
	lerp2_unaligned(&V->PX, &_IA->PX, &_IB->PX, t);
	// Block 2: TN.x, .y, .z, TTangent.x — TN is a 3-float Vector immediately
	// followed by TTangent, so one Vec4f covers four of the six.
	// View-space shading normal + tangent — linear-in-screen interpolation;
	// the rasterizer renormalizes per-pixel (nlerp). Carrying TN/TTangent
	// here means post-clip n-gons get sensible normals at every newly-
	// introduced vertex, just like UVs.
	lerp4_unaligned(&V->TN.x, &_IA->TN.x, &_IB->TN.x, t);
	// Block 3: TTangent.y, .z — the tail of that run. Tangent follows, and it
	// IS persisted from the source vertex on un-clipped paths, so the store
	// must stop here rather than spill into a 4th lane.
	lerp2_unaligned(&V->TTangent.y, &_IA->TTangent.y, &_IB->TTangent.y, t);
	// Block 4/5: UZ, VZ and EUZ, EVZ — two pairs in the untouched tail.
	lerp2_unaligned(&V->UZ,  &_IA->UZ,  &_IB->UZ,  t);
	lerp2_unaligned(&V->EUZ, &_IA->EUZ, &_IB->EUZ, t);
	// Scalar fall-throughs: RZ (bracketed by TPos_AOS and TN, neither of
	// which is interpolated as a block here) and the BGRA bytes, which don't
	// SIMD usefully at 1 byte per lane.
	V->RZ = _IA->RZ + t * (_IB->RZ - _IA->RZ);
	V->LR = _IA->LR + t * (_IB->LR - _IA->LR);
	V->LG = _IA->LG + t * (_IB->LG - _IA->LG);
	V->LB = _IA->LB + t * (_IB->LB - _IA->LB);
	V->i = _IA->i;

	// calculate (U,V)
	V->TPos_AOS.z = 1.0 / V->RZ;
	V->U = V->UZ * V->TPos_AOS.z;
	V->V = V->VZ * V->TPos_AOS.z;
	V->EU = V->EUZ * V->TPos_AOS.z;
	V->EV = V->EVZ * V->TPos_AOS.z;
	// Static-shadow lightmap bary on the original face (A, B, C).
	// Same perspective-correct interpolation as UV: lerp OrigBary*RZ
	// screen-affinely, then divide by the new vertex's RZ at the
	// clip point. Stamped at scene init in LightmapStampOrigBary;
	// read per-pixel by Mekalele for the atlas lookup. Cost: 4 muls
	// + 2 adds + 2 multiplies (uses TPos_AOS.z already computed above).
	{
		const float aBZ = _IA->OrigBaryB * _IA->RZ;
		const float bBZ = _IB->OrigBaryB * _IB->RZ;
		const float aCZ = _IA->OrigBaryC * _IA->RZ;
		const float bCZ = _IB->OrigBaryC * _IB->RZ;
		const float obBZ = aBZ + t * (bBZ - aBZ);
		const float obCZ = aCZ + t * (bCZ - aCZ);
		V->OrigBaryB = float(obBZ * V->TPos_AOS.z);
		V->OrigBaryC = float(obCZ * V->TPos_AOS.z);
	}
	// S1b shell entry height (--pom_shell): perspective-correct exactly like
	// OrigBary/UV — the rasterizer interpolates ShellH*RZ across the tile and
	// divides by per-pixel Z, so the clip vertex must carry the true
	// object-space value at the clip point.
	{
		const float aHZ = _IA->ShellH * _IA->RZ;
		const float bHZ = _IB->ShellH * _IB->RZ;
		const float hZ  = aHZ + t * (bHZ - aHZ);
		V->ShellH = float(hZ * V->TPos_AOS.z);
	}
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


// CorrectCWOrder moved to header (FRUSTRUM.H) as inline member.

// --mip_stats bookkeeping. `level` is the level ACTUALLY handed to the filler
// (i.e. after the --mips gate), `pix` the covered screen area in pixels — the
// caller passes 0.5*|signed fan sum| since the fan sum carries no 0.5 factor.
static inline void MipStat_Record(int32_t level, double pix)
{
#if FDS_RENDER_STATS_ENABLED
	if (level < 0)  level = 0;
	if (level > 15) level = 15;
	fds::PerThreadRenderStats& st = fds::stats_tls();
	st.mipLevelFaces[level]++;
	st.mipLevelPix[level] += pix;
#else
	(void)level; (void)pix;
#endif
}

// Signed fan-sum area of a clipper polygon, *2 (same convention as pixArea in
// MiplevelClipper: no 0.5 factor). Used for the split path's sub-sections,
// whose area is not otherwise computed.
static inline float MipStat_Area2(Vertex **P, mword n)
{
	float a = 0.0f;
	const Vertex *A = P[0];
	for (mword i = 2; i < n; i++)
		a += (P[i]->PX - A->PX) * (P[i-1]->PY - A->PY)
		   - (P[i-1]->PX - A->PX) * (P[i]->PY - A->PY);
	return a;
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

void FrustumClipper::MiplevelClipper(Face *F, RasterFunc filler,
                                      const fds::RenderTarget& rt,
                                      const fds::CameraContext& cam)
{
	FDS_STATS_SCOPE();
	FDS_STATS_INC_S(mipEntered);
	// --mip-bias (default 0.5 = the historical hardcoded constant) and
	// --mip-hysteresis (default 0 = off). Hysteresis holds a face's
	// previous level (F->LastMip) unless the continuous metric crosses
	// the level boundary by the margin — otherwise a boundary-straddling
	// face flips mip every frame, a hard texture-detail flicker with the
	// point-sampled fillers.
	const float mipBias = fds::FeatureFlags::mip_bias();
	const float mipHyst = fds::FeatureFlags::mip_hysteresis();
	// --mip_fix: the split branch's depth ramp is level(rz) = _C - K*log2(rz).
	// K=2 is the historical value and it is WRONG — texel area per pixel goes as
	// z^2, so 0.5*log2(...) gives K=1 (see the flag doc). With K=2 the split
	// branch disagrees with the small-face branch by log2(z) at the same depth,
	// so a face popping across the 2%-screen-area threshold jumps 1-6 levels.
	const bool  mipFix = fds::FeatureFlags::mip_fix();
	const float mipK   = mipFix ? 1.0f : 2.0f;
	// --mip_stats: per-level draw/area histogram. Everything below is inside
	// `if (mipStats)`, so a normal run pays one bool load per face.
	const bool  mipStats = fds::FeatureFlags::mip_stats();
	if (mipStats) fds::RenderStats_MipReportAtExit();
	// --mip_aniso: max-axis LOD instead of the geometric-mean area ratio.
	const bool  mipAniso = fds::FeatureFlags::mip_aniso();

	Texture *Tx = F->Txtr->Txtr;
	int32_t L = Tx->numMipmaps-1; // maximal level	

	// polygon texel / pixel area
	float pixArea = 0.0f, texArea = 0.0f;

	Vertex *A = C_Prim[0], *B, *C;
	mword i, j;
	// --mip_aniso: area-weighted UV->screen Jacobian, accumulated in the same
	// fan sweep. For fan triangle (A,B,C) with screen edges e1=B-A, e2=C-A and
	// texture edges t1, t2, the affine map's derivatives are
	//   d{u,v}/d{x,y} = (numerator) / det,  det = e1x*e2y - e1y*e2x
	// and the numerators below are exactly those, PRE-division. Summing
	// numerators and dets separately over the fan yields the area-weighted mean
	// derivative for the whole (possibly clipped, n-gon) polygon — which is what
	// the area metric already implicitly uses, so this is an apples-to-apples
	// upgrade rather than a different sampling point.
	float nUx = 0.0f, nUy = 0.0f, nVx = 0.0f, nVy = 0.0f, detS = 0.0f;
	for(i=2; i<C_numVerts; i++)
	{
		B = C_Prim[i-1];
		C = C_Prim[i];
	
		pixArea += (C->PX - A->PX) * (B->PY - A->PY) - (B->PX - A->PX) * (C->PY - A->PY);
		texArea += (C->U  - A->U ) * (B->V  - A->V ) - (B->U  - A->U ) * (C->V  - A->V );

		if (mipAniso) {
			const float e1x = B->PX - A->PX, e1y = B->PY - A->PY;
			const float e2x = C->PX - A->PX, e2y = C->PY - A->PY;
			const float t1u = B->U  - A->U , t1v = B->V  - A->V ;
			const float t2u = C->U  - A->U , t2v = C->V  - A->V ;
			detS += e1x*e2y - e1y*e2x;
			nUx  += t1u*e2y - t2u*e1y;
			nUy  += t2u*e1x - t1u*e2x;
			nVx  += t1v*e2y - t2v*e1y;
			nVy  += t2v*e1x - t1v*e2x;
		}
	}

	// The continuous LOD for this polygon, in log2(texels per screen pixel).
	// GEOMETRIC MEAN (legacy): 0.5*log2(texelArea/pixelArea) — the sqrt of the
	// product of the two axis densities. At grazing incidence one axis is
	// compressed far harder than the other (greets' corridor walls run ~16:1),
	// and the geometric mean then sits ~log2(sqrt(16)) = 2 levels SHARPER than
	// the compressed axis needs, which is exactly the regime that aliases.
	// MAX AXIS (--mip_aniso, what GPUs do without aniso filtering): take the
	// longer of the two texel-space axis vectors. Isotropic faces are
	// unaffected (both metrics reduce to log2(s)); anisotropic faces get
	// blurrier by log2(sqrt(aspect)).
	// Max-axis LOD. Returns <0 as "not applicable" so the caller falls back to
	// the legacy area expression VERBATIM — including its original float/double
	// typing, which is load-bearing: _C below positions the subdivision CUT
	// LINES, and those move geometry even when the mip LEVEL is forced to 0, so
	// re-associating this arithmetic is NOT byte-null on the mips-off path.
	// Computed lazily; the Txtr_Nomip exit needs no LOD at all.
	auto AnisoLod = [&]() -> float {
		if (fabs(detS) > 1e-12f) {
			const float rdet = 1.0f / detS;
			const float ux = nUx * rdet * Tx->SizeX, vx = nVx * rdet * Tx->SizeY;
			const float uy = nUy * rdet * Tx->SizeX, vy = nVy * rdet * Tx->SizeY;
			const float lx2 = ux*ux + vx*vx;   // texels^2 per screen-x step
			const float ly2 = uy*uy + vy*vy;   // texels^2 per screen-y step
			const float mx2 = lx2 > ly2 ? lx2 : ly2;
			if (mx2 > 1e-20f) return 0.5f * fastLog2(mx2);
		}
		return -1.0f;
	};

#ifdef ENABLE_PIXELCOUNT
	FDS_STATS_ADD_PIXELS_S(0.5f * fabs(pixArea));
#endif
	// Simple miplevel calculation
	//if (!Keyboard[ScY])
	static const float MinSize = XRes*YRes * 0.02; // update if XRes and/or YRes changes...

	if (mipStats) {
		if (pixArea < 0.0f)              FDS_STATS_INC_S(mipNegArea);
		if (fabs(pixArea) >= MinSize)    FDS_STATS_INC_S(mipBigFace);
		if (Tx->Flags & Txtr_Nomip)      FDS_STATS_INC_S(mipNomip);
	}

	if ((Tx->Flags & Txtr_Nomip) ||  pixArea < MinSize)
	{
		FDS_STATS_INC_S(mipFastUniform);
		if (Tx->Flags & Txtr_Nomip) {
			g_MipLevel = 0;
			meka::g_tlsMipFrac = 0.0f;
		} else {
			const float anis = mipAniso ? AnisoLod() : -1.0f;
			const float mipF = (anis >= 0.0f
			                    ? anis
			                    : 0.5f * fastLog2(fabs(texArea * Tx->SizeX * Tx->SizeY / pixArea)))
			                   + mipBias;
			int32_t mip = int32_t(mipF);
			// Hysteresis: ±1-level transitions must clear the boundary
			// (integer crossings of mipF) by the margin, else hold.
			if (mipHyst > 0.0f && F->LastMip != 0xFF) {
				const int32_t last = F->LastMip;
				if (mip == last + 1 && mipF < float(last + 1) + mipHyst) mip = last;
				else if (mip == last - 1 && mipF > float(last) - mipHyst) mip = last;
			}
			if (mip < 0) mip = 0;
			if (mip > L) mip = L;
			if (g_mipLastMipWrite) F->LastMip = uint8_t(mip);
			g_MipLevel = mip;
			// Trilinear fraction toward mip+1 (int32_t(mipF) truncates, so
			// level L is active for mipF ∈ [L, L+1)). Clamped in the filler.
			meka::g_tlsMipFrac = mipF - float(mip);
		}

#ifdef COUNT_POLYS
		FDS_STATS_INC_POLYS_S();
#endif
		if (!fds::FeatureFlags::mips()) { g_MipLevel = 0; meka::g_tlsMipFrac = 0.0f; }  // was NO_MIPMAPS
		if (mipStats) MipStat_Record(g_MipLevel, 0.5 * fabs(pixArea));
		filler(F, C_Prim, C_numVerts, g_MipLevel, rt, cam);
		return;
	}

	// mipmap level constant
	float q = 0;
	for(i=0; i<C_numVerts; i++)
	{
		q += fastLog2(C_Prim[i]->RZ);
	}
	
	const float anisC = mipAniso ? AnisoLod() : -1.0f;
	// NOTE the `0.5 *` here is DOUBLE in the legacy path and must stay double.
	float _C = (anisC >= 0.0f
	            ? double(anisC)
	            : 0.5 * fastLog2(fabs(texArea * Tx->SizeX * Tx->SizeY / pixArea)))
	           + invTable[C_numVerts] * q + mipBias;

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
	float ml = _C - mipK * fastLog2(Mrz);
	float Ml = _C - mipK * fastLog2(mrz);

	// --mip_fix: match the SMALL-FACE branch's rounding (truncate, so level L is
	// active for the metric in [L, L+1)) instead of Fist(x-0.99999), which lands
	// one level sharper and contradicts the trilinear fraction computed below.
	int32_t iml = mipFix ? int32_t(ml) : int32_t(Fist(ml-0.99999));
	int32_t iMl = mipFix ? int32_t(Ml) : int32_t(Fist(Ml-0.99999));

	if (iml < 0) iml = 0;
	if (iml > L) iml = L;
	if (iMl < 0) iMl = 0;
	if (iMl > L) iMl = L;


	// simple case (uniform mipmap across polygon)
	// should also include all polygons with reletively small onscreen area, because
	// polysplitting is REALLY slow.
	if (iml == iMl)
	{
		FDS_STATS_INC_S(mipFastUniform);
		// Hysteresis (see the small-face branch): level L is active for
		// ml ∈ (L, L+1], so the boundaries sit at integer ml — require a
		// ±1 transition to clear them by the margin, else hold LastMip.
		if (mipHyst > 0.0f && F->LastMip != 0xFF) {
			const int32_t last = F->LastMip;
			if (iml == last + 1 && ml < float(last + 1) + mipHyst) iml = last;
			else if (iml == last - 1 && ml > float(last) - mipHyst) iml = last;
			if (iml < 0) iml = 0;
			if (iml > L) iml = L;
		}
		if (g_mipLastMipWrite) F->LastMip = uint8_t(iml);
		g_MipLevel = iml;
		if (g_MipLevel < 0) g_MipLevel = 0;
		// Trilinear fraction toward mip+1. Level L is active for the
		// continuous metric ml ∈ (L, L+1] (see the hysteresis comment
		// above), so frac = ml - L. Clamped to [0,1] in the filler.
		// With --mip_fix's truncation this is exactly the [0,1) fraction the
		// filler wants; under the legacy rounding it can exceed 1 (the filler
		// clamps). Same expression either way, so the legacy path is untouched.
		meka::g_tlsMipFrac = ml - float(g_MipLevel);
#ifdef COUNT_POLYS
		FDS_STATS_INC_POLYS_S();
#endif
		if (!fds::FeatureFlags::mips()) { g_MipLevel = 0; meka::g_tlsMipFrac = 0.0f; }  // was NO_MIPMAPS
		if (mipStats) MipStat_Record(g_MipLevel, 0.5 * fabs(pixArea));
		filler(F, C_Prim, C_numVerts, g_MipLevel, rt, cam);
		return;
	}

	// Poly-split path: each section rasterizes at a discrete mip; no
	// cross-mip fraction is meaningful, so trilinear falls back to
	// bilinear (frac 0) on these faces.
	meka::g_tlsMipFrac = 0.0f;
	FDS_STATS_INC_S(mipSplit);

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
	// Crossover 1/z for each integer level, solved from the SAME ramp the range
	// used: level = _C - K*log2(rz)  =>  rz = 2^((_C - level)/K). K=2 gives the
	// legacy 0.5 exponent and successive cuts dividing by sqrt2; --mip_fix's K=1
	// gives exponent 1 and successive cuts halving. Solving the cut lines from a
	// different K than the range would put them at the wrong depths.
	const float limrz_mult = mipFix ? 0.5f : float(1.0 / sqrt(2.0));
	//float limrz = exp(h_log_2*(_C - iml));
	float limrz = fastPow2((mipFix ? 1.0f : 0.5f)*(_C - iml));

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
		FDS_STATS_INC_POLYS_S();
#endif
		if (!fds::FeatureFlags::mips()) g_MipLevel = 0;   // was NO_MIPMAPS
		if (mipStats) MipStat_Record(g_MipLevel, 0.5 * fabs(MipStat_Area2(C_Tetr, numSectionVerts)));
		filler(F, C_Tetr, numSectionVerts, g_MipLevel, rt, cam);

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
		FDS_STATS_INC_POLYS_S();
#endif
		if (!fds::FeatureFlags::mips()) g_MipLevel = 0;   // was NO_MIPMAPS
		if (mipStats) MipStat_Record(g_MipLevel, 0.5 * fabs(pixArea));
		filler(F, C_Prim, C_numVerts, g_MipLevel, rt, cam);
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
	FDS_STATS_INC_POLYS_S();
#endif
	if (!fds::FeatureFlags::mips()) g_MipLevel = 0;   // was NO_MIPMAPS
	if (mipStats) MipStat_Record(g_MipLevel, 0.5 * fabs(MipStat_Area2(C_Tetr, numSectionVerts)));
	filler(F, C_Tetr, numSectionVerts, g_MipLevel, rt, cam);
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


void FrustumClipper::Render(Face *F, RasterFunc filler, bool isEnvCoords,
                             const fds::RenderTarget& rt,
                             const fds::CameraContext& cam,
                             bool skipMipLevel)
{
	FDS_STATS_SCOPE();
	FDS_STATS_INC_S(clipperEntered);
	// g_clipperTileRect was published once at InitViewport /
	// SetClippingExtents time and stays valid for the duration of this
	// worker's clipper. The FrustumClipper destructor resets to
	// "no clamp". (Was a per-Render RAII guard — ~45 leaf samples in
	// the profile from repeated thread_local writes.)

	mword i, j;

	Vertex* A = &C_Verts[0];
	Vertex* B = &C_Verts[1];
	Vertex* C = &C_Verts[2];

	*A = *F->A;
	*B = *F->B;
	*C = *F->C;
	newVert = &C_Verts[3];

	// U/V/UZ/VZ stamped + perspective-projected in one block — interleaves
	// the * RZ FMA with its inputs while they're still in registers, and
	// halves the F->Flags load count by merging the two Face_Reflective
	// branches that used to bracket the UZ stamp.
	{
		const float aRZ = A->RZ, bRZ = B->RZ, cRZ = C->RZ;
		A->U = F->U1; A->V = F->V1;
		A->UZ = F->U1 * aRZ; A->VZ = F->V1 * aRZ;
		B->U = F->U2; B->V = F->V2;
		B->UZ = F->U2 * bRZ; B->VZ = F->V2 * bRZ;
		C->U = F->U3; C->V = F->V3;
		C->UZ = F->U3 * cRZ; C->VZ = F->V3 * cRZ;
		if (F->Flags & Face_Reflective) {
			A->EU = F->EU1; A->EV = F->EV1;
			A->EUZ = F->EU1 * aRZ; A->EVZ = F->EV1 * aRZ;
			B->EU = F->EU2; B->EV = F->EV2;
			B->EUZ = F->EU2 * bRZ; B->EVZ = F->EV2 * bRZ;
			C->EU = F->EU3; C->EV = F->EV3;
			C->EUZ = F->EU3 * cRZ; C->EVZ = F->EV3 * cRZ;
		}
	}

	// Preserve only Vtx_VisNear / Vtx_VisFar from Transform_Objects'
	// flags — those are the Z-axis flags the clipper's Calc_Flags
	// doesn't compute (it only handles 2D PX/PY). The 2D flags from
	// Transform_Objects use XRes/YRes globals which only match the
	// clipper's ClipX2/Y2 for the main render. For off-screen
	// rasterization (shadow maps, cube-map bake) the rects differ:
	// e.g. shadow map ClipY2=1024 but YRes=800 means Transform_Objects
	// sets VisDown for any vert with PY≥800, which is fine for the main
	// pass but causes the clipper to extrapolate edges to PY=1024 for
	// verts that are actually inside the shadow map's bounds, producing
	// ghost polygons.
	constexpr dword kZFlags = Vtx_VisNear | Vtx_VisFar;
	const dword Af = A->Flags & kZFlags;
	const dword Bf = B->Flags & kZFlags;
	const dword Cf = C->Flags & kZFlags;
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

	// Combined-flag guards: when no bits in `kZFlags` (resp.
	// `kClipFlags2D`) are set on the OR-of-vertex-flags, the entire Z
	// (resp. 2D) clip block is skipped with one mask test instead of
	// per-direction tests. For the common "fully inside this worker's
	// rect" case, this trims ~3 branches off the per-triangle path; for
	// partial clip the inner tests use a register-cached sub-mask.
	if (C_Flags & kZFlags) {
		FDS_STATS_INC_S(clipNeedZ);
		if (C_Flags & Vtx_VisNear) Near();
		if (C_Flags & Vtx_VisFar)  Far();
	}

	// Z-Clipping may have placed the entire polygon outside 2D boundary
	/*j = C_Prim[0]->Flags;
	for(i=1; i<C_numVerts; i++)
		j &= C_Prim[i]->Flags;
	if (j) return;*/

	// Swap vertex order if it isn't CCW.
	CorrectCWOrder();

	// 2D clipping is a net WIN despite the per-poly cost: rasterizer's
	// tile-loop walks the triangle's bbox, so an unclipped poly that
	// straddles the screen edge iterates off-screen tiles that get
	// clamp-rejected per-pixel — wasted setup work. Clipping produces
	// a smaller on-screen polygon → rasterizer touches fewer tiles.
	// Tested skipping 2D clip 2026-05-31: city went 24.03 → 25.42 ms
	// (-5%), confirming the clipper saves more than it costs.
	// Visually identical (rasterizer handles off-screen verts correctly).
	constexpr dword kClipFlags2D = Vtx_VisLeft | Vtx_VisRight | Vtx_VisUp | Vtx_VisDown;
	if (const dword vis2D = C_Flags & kClipFlags2D) {
		FDS_STATS_INC_S(clipNeed2D);
		if (vis2D & Vtx_VisLeft)  Left();
		if (vis2D & Vtx_VisRight) Right();
		if (vis2D & Vtx_VisUp)    Up();
		if (vis2D & Vtx_VisDown)  Down();
	}

	// Clipping process may have eliminated the entire polygon
	if (!C_numVerts) return;

	YSort(C_Prim, C_Scnd, C_numVerts);
	
	// triangulate result NGon and sort triangle vertices by Y
	// to conform to legacy rasterizer convension

	if (F->Txtr->Txtr && !skipMipLevel)
	{
		Swap();
		MiplevelClipper(F, filler, rt, cam);
	} else {
#ifdef ENABLE_PIXELCOUNT
		FDS_STATS_ADD_PIXELS_S(PolyArea());
#endif
#ifdef COUNT_POLYS
		FDS_STATS_INC_POLYS_S();
#endif
		// skipMipLevel callers (shadow rasterizer) don't care about the
		// mip level — pass 0. The texture wouldn't be sampled anyway.
		// Non-mip textured faces have no cross-mip fraction → trilinear
		// falls back to bilinear (frac 0).
		meka::g_tlsMipFrac = 0.0f;
		filler(F, C_Scnd, C_numVerts, skipMipLevel ? 0u : g_MipLevel, rt, cam);
	}
	/*for(i=2; i<C_numVerts; i++)
	{
		if (C_Scnd[i-1]->PY < C_Scnd[i]->PY)
			filler(C_Scnd[0], C_Scnd[i-1], C_Scnd[i]);
		else
			filler(C_Scnd[0], C_Scnd[i], C_Scnd[i-1]);
	}*/
}
