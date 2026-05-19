#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
//#include <conio.h>
#include <Base/Vector.h>
#include <memory>
#include <vector>
#include <algorithm>
#include <map>
#include <limits>
#include <chrono>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "simde/x86/fma.h"

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "RenderPipeline.h"
#include "VESA/Vesa.h"
#include "FRUSTRUM.H"

// Defined in Transform.cpp — used by the deferred transparent path
// (RenderXparClumpInStrip) and by the legacy xpar peel.
bool IsFrontFacingInViewSpace(const Face* F);

// Defined in DeferredLighting.cpp.
void Render_DeferredLighting();
void Render_DeferredFogPass();
void Render_VolumetricCones();
void Render_OmniHalos();
void Render_DeferredVolumetric();

// Resolve the unified-pass preference tri-state:
//   CLI/env explicit (FDS_VOLUMETRIC_UNIFIED set) → flag wins
//   otherwise → scene's PreferVolumetricUnified
static bool useUnifiedVolumetric() {
	if (fds::FeatureFlags::isSet(fds::FeatureFlags::BoolId::volumetric_unified))
		return fds::FeatureFlags::volumetric_unified();
	return CurScene && CurScene->PreferVolumetricUnified != 0;
}
void renderDeferredTransparentTile_Front(int tileIdx, int x1, int y1, int x2, int y2);
void renderDeferredTransparentTile_Back (int tileIdx, int x1, int y1, int x2, int y2);

// Defined in RenderInner.cpp.
void RenderInner(float x1, float y1, float x2, float y2);
void RenderInnerMekalele(float x1, float y1, float x2, float y2);
void RenderInnerDeferredTransparent(float x1, float y1, float x2, float y2,
                                    int firstIdx, int count, struct TriMesh* parentFilter,
                                    XparFaceSel faceSel = XparFaceSel::Both);

//#define TRACE_OBJECTS

thread_local FrustumClipper clipper;


float frand() {
	return static_cast <float> (RAND_15()) / static_cast <float> (RAND_15_MAX);
}

// Front-to-back sorting used together with ZBuffer.
#define FRONT_TO_BACK_SORTING

//struct SceneContext {
//	std::unique_ptr<Scene>  CurScene;
//	std::vector<Face *>     FList;
//	std::vector<Face *>     SList;
//	Face                    *DoFace;
//	int32_t CPolys,COmnies,CPcls,CAll,Polys;
//	char Cam_HeadLight;
//	float CurFrame;
//	float g_zscale;
//	float g_zscale256;
//	int32_t g_FrameTime;
//	float Ambient_Factor;
//	float Diffusive_Factor;
//	float Specular_Factor;
//	float Range_Factor;
//};


Scene *CurScene;
// FList / SList / CPolys / COmnies / CPcls / CAll / Polys / g_zscale /
// g_zscale256 / FOVX / FOVY / CntrX / CntrY / CntrEX / CntrEY / C_FZP /
// C_rFZP / C_NZP / C_rNZP are now fields of fds::g_mainFrame (see
// FDS/Base/FrameState.h); the legacy names are reference aliases
// defined in FrameState.cpp.
Material Default_Mat;
unsigned int OldFPUCW, FPUCW;

dword OldMXCSR, CurMXCSR;
char **MsgStr;
int32_t *MsgClock;
DWord *MsgID;
char Cam_HeadLight;
float CurFrame;


int32_t g_FrameTime;

////////////////
// LIGHTING
float Ambient_Factor;
float Diffusive_Factor;
float Specular_Factor;
float Range_Factor;

int32_t Frames;
#ifndef __clang__
extern float dTime;
#endif

Texture Phong_Mapping;
Font *Font1;
char G_String[256];

VESA_Surface Layer2;
// Backing storage for Layer2.Data. Owned here so RAII handles cleanup;
// Layer2.Data is just a non-owning view set up in Render() below.
static std::vector<byte> s_Layer2Storage;

#include "GENERAL.H"
#include "SORTS.H"
#include <Threads.h>
#include <FILLERS/TheOtherBarry.h>
#include <FILLERS/Mekalele.h>
#include <FILLERS/ShadowMap.h>

// RULEZ
void DO_NOP()
{
}

Vector Transform_Vec(Vector Pos, Vector Source, Matrix Mat) {
	Vector U, V, S;

	// postrioric Offset Vector.
	Vector_Sub(&Pos, &Source, &U);
	MatrixXVector(Mat, &U, &S);
	V.x = CntrEX * S.z + FOVX * S.x;
	V.y = CntrEY * S.z - FOVY * S.y;
	V.z = S.z;

	//MatrixXVector(Mat, &V, &AP);
	//Vector_SelfAdd(&S, &AP);
	V.x /= V.z;
	V.y /= V.z;

	return V;
}

void Draw_Line_32Bit_ForKids(Vertex* V1, Vertex* V2);
void Draw_Line_32Bit(Vertex* V1, Vertex* V2);

void Bivex_Frustrum_Perspective(Vertex* A, Vertex* B, void (*Filler)(Vertex* V1, Vertex* V2));

void DrawSpline(Spline Pos, Vector CamISource, int EndFrame) {
	Vertex Last, Cur;
	for (auto frame = 0; frame < EndFrame; ++frame) {
		Vector IPos;
		Spline_Calc_3D(&Pos, frame, &IPos);
		IPos = Transform_Vec(IPos, CamISource, View->Mat);
		//auto pos = ((Fist(IPos.x * 4) + Fist(IPos.y) * VESA_BPSL));
		//if (pos < PageSize) {
		//	*(dword*)(VPage + pos) = 0xffffffff;
		//}
		Cur.PX = IPos.x;
		Cur.PY = IPos.y;
		if (frame != 0) {
			if (Last.PX < XRes-1 && Last.PY < YRes-1 && Last.PX > 0 && Last.PY > 0 &&
				Cur.PX < XRes-1 && Cur.PY < YRes-1 && Cur.PX > 0 && Cur.PY > 0) {

				Draw_Line_32Bit_ForKids(&Last, &Cur);
			}
		}
		Last.PX = Cur.PX;
		Last.PY = Cur.PY;
	}
}



// Forward (per-vertex) lighting pass now lives in Lighting.cpp:
//   Vector_CosAngleFAST, LightingOld, StaticLighting, Lighting.




void Restore_Splines(Scene *Sc)
{
	TriMesh *T;
	Camera *Cm;
	Omni *Om;
	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		T->Pos.CurKey = 0;
		T->Rotate.CurKey = 0;
		T->Scale.CurKey = 0;
		T->CurStat = T->Status;
	}
	for(Om=Sc->OmniHead;Om;Om=Om->Next)
	{
		Om->Pos.CurKey = 0;
		//    Om->Col.CurKey = 0;
	}
	for(Cm=Sc->CameraHead;Cm;Cm=Cm->Next)
	{
		Cm->Source.CurKey = 0;
		Cm->Target.CurKey = 0;
		Cm->Roll.CurKey = 0;
		Cm->FOV.CurKey = 0;
	}
}

namespace renderns {
	std::mutex					tileCounterMutex;
	std::atomic<int>			tileCounter;
	std::condition_variable		condition;
};

// output is unused on the deferred path.
bool deferredEnabled() {
	return fds::FeatureFlags::deferred();
}

// Per-scene matID for the water material — when this matches the
// G-buffer mat32 at a pixel, the deferred lighting compositor blends
// the lit water texel over the existing VPage value (which holds the
// dispMap-distorted reflection from the pass-1 mirrored-world draw),
// matching forward's TheOtherBarry<TRANSPARENT> filler:
//   pixel = saturate(lit_water + existing/2)
// Set to -1 (default) to disable the water-blend special case.
int g_deferredWaterMatID = -1;

void fds::RenderPipeline::setDeferredWaterMatID(int id) {
	waterMatID_ = id;
	g_deferredWaterMatID = id;
}

// Phase A deferred lighting pass. Replaces the per-vertex Lighting() +
// per-pixel colorize() pipeline with one per-pixel pass: decode mat32 →
// Material*, sample texture, decode oct normal, reconstruct view-space
// position from Z, ambient + Lambertian + linear distance falloff
// per omni, modulate texel by light, write final pixel.
//
// Matches the forward Lighting() model 1:1 — same Lambert × linear
// falloff, same 250-saturation, same Diffuse/Luminosity/BaseCol use.
// Differences vs forward:
//   - Per-pixel material (forward takes Mat[0] per mesh — incidental
//     improvement, not a divergence).
//   - Per-pixel position/normal (forward Gouraud-interpolates per vertex).
//   - View-space lighting (forward transforms each omni into the current
//     mesh's object space; we transform once per frame into view space
//     and reconstruct pixel positions there).
//
// Parallelism: same 6×4 tile-job model the rasterizer uses. Per-frame
// setup (matTable, view-space omni list, screen-projection constants)
// happens once on the demo thread; per-tile work is enqueued onto the
// ThreadPool. Tiles are independent (each writes a disjoint slice of
// VPage). Per-pixel work is scalar; SIMD is a future tighter pass.
// View-space omni storage. SoA layout so the per-pixel inner loop can
// load 8 omnis at a time into Vec8f for the diffuse accumulation.
// Padded entries (index >= numLights, up to MAX_LIGHTS) are zero so
// they fail both the dot >= 0 and len2 <= range2 tests, contributing
// zero — no need to round numLights up to a multiple of 8 explicitly.


void fds::RenderPipeline::renderFrame(RenderPath path)
{
	constexpr auto		numTilesX = 6;
	constexpr auto		numTilesY = 4;
	const auto			tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const auto			tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;

	renderns::tileCounter = 0;
	// Deferred only if env var set AND the G-buffer is sized for the
	// current framebuffer. The cube-map bake in Initialize_City swaps
	// MainSurf to a 1024x1024 TmpSurf — XRes/YRes change without our
	// engine-sized G-buffer being resized — so we'd write past the
	// vector. Safer to fall back to forward in that case (and any
	// other ctx where storage hasn't been provisioned).
	//
	// RenderPath::ForceForward bypasses the deferred dispatch — used by
	// RenderSkyCube and the City cube-map bake, whose lighting models
	// are unsuitable for the deferred per-pixel pass.
	// RenderPath::ForceDeferred takes the deferred path regardless of
	// the FDS_DEFERRED env var (which is read-once-and-cached) so
	// snapshot harnesses can render both modes in a single process.
	bool deferred;
	if (path == RenderPath::ForceForward)       deferred = false;
	else if (path == RenderPath::ForceDeferred) deferred = true;
	else                                        deferred = deferredEnabled();
	if (deferred && (!g_gbuffer || g_gbuffer->normal.size() < size_t(XRes) * size_t(YRes))) {
		deferred = false;
	}

	// Clear G-buffer mat32 to a sentinel pattern before each deferred
	// raster pass. Mekalele writes mat32 only for the pixels it covers;
	// any pixel rendered by a forward filler (sky cube, City reflective
	// windows) leaves mat32 untouched. The deferred lighting pass then
	// skips matID >= count via the existing range check, leaving those
	// forward-rendered pixels alone.
	if (deferred && g_gbuffer) {
		const size_t numPixels = size_t(XRes) * size_t(YRes);
		parallel_memset(g_gbuffer->txtr.data(), 0xFF, numPixels * sizeof(uint32_t));
		// Transparent G-buffer: mat32=0xFFFFFFFF sentinel ("no transparent
		// front face here"); xpr-Z=0 ("empty, anything passes Z-test").
		if (g_gbufferTransparent) {
			parallel_memset(g_gbufferTransparent->txtr.data(), 0xFF,
			                numPixels * sizeof(uint32_t));
		}
		if (g_xparZ) {
			parallel_memset(g_xparZ, 0, numPixels * sizeof(uint16_t));
		}
		// Back layer (depth-peel slot for back-facing transparents).
		if (g_gbufferTransparentBack) {
			parallel_memset(g_gbufferTransparentBack->txtr.data(), 0xFF,
			                numPixels * sizeof(uint32_t));
		}
		if (g_xparZBack) {
			parallel_memset(g_xparZBack, 0, numPixels * sizeof(uint16_t));
		}
	}

	for (auto j = 0; j < numTilesY; ++j) {
		auto y1 = tileSizeY * j;
		auto y2 = std::min(y1 + tileSizeY, YRes);
		for (auto i = 0; i < numTilesX; ++i) {
			auto x1 = tileSizeX * i;
			auto x2 = std::min(x1 + tileSizeX, XRes);

			if (deferred) {
				ThreadPool::instance().enqueue([x1, y1, x2, y2]() { RenderInnerMekalele(x1, y1, x2, y2); });
			} else {
				ThreadPool::instance().enqueue([x1, y1, x2, y2]() { RenderInner(x1, y1, x2, y2); });
			}
			// RenderInner(x1, y1, x2, y2);
		}
	}

	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::condition.wait(lock, [] {return renderns::tileCounter == numTilesX * numTilesY; });
	}

	if (deferred) {
		// Phase A deferred lighting pass: per-pixel ambient + Lambertian
		// + linear falloff against the scene's omnis, modulated by the
		// G-buffer's per-pixel texture sample. Replaces the per-vertex
		// Lighting() (skipped on the deferred path) plus the rasterizer's
		// colorize step in one go.
		Render_DeferredLighting();

		// Full-screen linear distance fog over deferred-lit opaque pixels.
		// Runs BEFORE the transparent peel so xpar surfaces composite onto
		// already-fogged opaque dst. Skipped on non-Scn_Fogged scenes.
		// Disabled when the unified volumetric pass is on (folds fog in).
		if (!useUnifiedVolumetric()) {
			Render_DeferredFogPass();
		}

		// Per-object peeled transparent rendering. We process each
		// transparent TriMesh independently — clear the xpar G-buffers,
		// render that mesh's transparent faces (back-facing → back
		// layer, front-facing → front layer), light the back layer
		// onto VPage, light the front layer onto VPage — before moving
		// on to the next mesh. Object order is each mesh's FIRST
		// appearance in the sorted FList, which is back-to-front by
		// the object-grouping sort key, so outer/farther meshes
		// composite before inner/closer ones.
		//
		// This is what makes nested transparent meshes work (fountain
		// outer + inner spire spheres). With a single shared pass the
		// inner mesh's back-facing tris would Z-compete with the
		// outer mesh's back-facing tris in the back layer and one of
		// them would lose. Per-object peeling gives each mesh a fresh
		// xpar G-buffer.

		const bool unifiedTbr = deferredUnifiedTbrEnabled();

		// Unified-TBR path (task #9): insert every transparent face into
		// the per-strip TBR linked lists. Sprites are still inserted in
		// the standalone post-loop further below — TBR_Render at the
		// end of Render() flushes everything together per-strip in
		// back-to-front render-Z order. The 2-deep xpar G-buffer is
		// kept (used in strip-scoped form by RenderXparClumpInStrip per
		// clump of consecutive same-(mesh, frontFacing) entries).
		if (unifiedTbr) {
			for (int32_t k = 0; k < CAll; ++k) {
				Face* F = FList[k].face;
				if (!F || !F->Txtr) continue;
				if (F->A == F->B) continue;  // sprite, handled by post-loop
				if (!(F->Txtr->Flags & Mat_Transparent)) continue;
				if (F->Flags & Face_Reflective) continue;  // forward handles
				if (!F->ParentTri) continue;
				InsertTransparentToTBR(F);
			}
			// Skip the legacy per-mesh xpar peel below.
			goto xpar_done;
		}

		{
		auto runTilePass = [&](auto workFn) {
			renderns::tileCounter = 0;
			for (auto j = 0; j < numTilesY; ++j) {
				auto y1 = tileSizeY * j;
				auto y2 = std::min(y1 + tileSizeY, YRes);
				for (auto i = 0; i < numTilesX; ++i) {
					auto x1 = tileSizeX * i;
					auto x2 = std::min(x1 + tileSizeX, XRes);
					const int tileIdx = j * numTilesX + i;
					ThreadPool::instance().enqueue([x1, y1, x2, y2, tileIdx, workFn]() {
						workFn(x1, y1, x2, y2, tileIdx);
					});
				}
			}
			std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
			renderns::condition.wait(lock, []{ return renderns::tileCounter == numTilesX * numTilesY; });
		};

		const size_t numPixels = size_t(XRes) * size_t(YRes);
		auto clearBack = [&]() {
			if (g_gbufferTransparentBack) {
				parallel_memset(g_gbufferTransparentBack->txtr.data(), 0xFF,
				                numPixels * sizeof(uint32_t));
			}
			if (g_xparZBack) parallel_memset(g_xparZBack, 0, numPixels * sizeof(uint16_t));
		};
		auto clearFront = [&]() {
			if (g_gbufferTransparent) {
				parallel_memset(g_gbufferTransparent->txtr.data(), 0xFF,
				                numPixels * sizeof(uint32_t));
			}
			if (g_xparZ) parallel_memset(g_xparZ, 0, numPixels * sizeof(uint16_t));
		};

		// One walk through FList in sort order. The xpar sort key (built
		// in Transform_Objects) already encodes the correct back-to-front
		// order for concentric meshes via a per-object extent bias —
		// outer.back, inner.back, inner.front, outer.front for a nested
		// pair. We just need to render each face in that order without
		// regrouping by mesh.
		//
		// To keep the 2-deep G-buffer's pixel budget (one back-layer
		// slot + one front-layer slot per screen pixel), batch
		// consecutive faces that share BOTH (mesh, side). When the
		// bucket changes, flush: clear the side's layer, raster the
		// batch with parentFilter + faceSel, composite the side.
		// Consecutive same-(mesh,side) faces naturally co-cluster in
		// the sorted list because the sort key has a high-bit partition
		// per side and a per-mesh score within.
		TriMesh* batchMesh    = nullptr;
		bool     batchFront   = false;
		bool     batchOpen    = false;
		int      batchFirst   = 0;
		int      batchLastEnd = 0;
		auto flushBatch = [&]() {
			if (!batchOpen) return;
			TriMesh*    T   = batchMesh;
			XparFaceSel sel = batchFront ? XparFaceSel::FrontOnly
			                              : XparFaceSel::BackOnly;
			const int first = batchFirst;
			const int count = batchLastEnd - batchFirst;
			if (batchFront) clearFront();
			else            clearBack();
			runTilePass([first, count, T, sel](int x1, int y1, int x2, int y2, int /*tileIdx*/) {
				RenderInnerDeferredTransparent(float(x1), float(y1),
				                                float(x2), float(y2),
				                                first, count, T, sel);
			});
			if (batchFront) {
				runTilePass([](int x1, int y1, int x2, int y2, int tileIdx) {
					renderDeferredTransparentTile_Front(tileIdx, x1, y1, x2, y2);
				});
			} else {
				runTilePass([](int x1, int y1, int x2, int y2, int tileIdx) {
					renderDeferredTransparentTile_Back(tileIdx, x1, y1, x2, y2);
				});
			}
			batchOpen = false;
		};

		for (int32_t k = 0; k < CAll; ++k) {
			Face* F = FList[k].face;
			if (!F || !F->Txtr) continue;
			if (!(F->Txtr->Flags & Mat_Transparent)) continue;
			if (F->Flags & Face_Reflective) continue;  // forward handles
			TriMesh* T = F->ParentTri;
			if (!T) continue;

			const bool front = IsFrontFacingInViewSpace(F);
			if (!batchOpen || T != batchMesh || front != batchFront) {
				flushBatch();
				batchMesh    = T;
				batchFront   = front;
				batchFirst   = k;
				batchOpen    = true;
			}
			batchLastEnd = k + 1;
		}
		flushBatch();
		}  // close unifiedTbr=false legacy block
xpar_done: ;
	}

	int32_t I = CAll;
	fds::FListEntry* FLS = FList;//+CAll-1;
	Vertex *A,*B;

	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;

	while (I--)
	{
		Face* F = (FLS++)->face;

		// Get Mapping Coordinates from the rendered face.
		A=F->A; B=F->B;

		// TODO: change this into a flag check
		if (A == B)
		{
			// Particle/Sprite - no far-Z clipping
			if (A->TPos.z < C_NZP) continue;
			F->Filler(F, &A, 1, 0, rt, cam);
		}
	}

	if (CurScene->Flags & Scn_SpriteTBR)
	{
		TBR_Render(CurScene);
	}

	// Volumetric pass(es). Tri-state dispatch:
	//   useUnifiedVolumetric()=true: one Beer-Lambert pass that does
	//     fog + cones + halos in one ray-march. Used by scenes whose
	//     aesthetic benefits from physically-coupled atmospheric depth
	//     (fountain, greets). Render_DeferredFogPass earlier in the
	//     pipeline was skipped when this is on.
	//   useUnifiedVolumetric()=false (default): three separate additive
	//     passes — the earlier Render_DeferredFogPass for surface
	//     attenuation, plus Render_VolumetricCones + Render_OmniHalos
	//     here as the final additive composites. Each is independently
	//     gated (draw_cones / omni_halo_strength); scenes can keep
	//     bright-clear look (city) while still getting cone shafts
	//     or halos if enabled.
	if (useUnifiedVolumetric()) {
		Render_DeferredVolumetric();
	} else {
		Render_VolumetricCones();
		Render_OmniHalos();
	}



#ifdef TRACE_OBJECTS
	//printf("frame: %f\n\t", CurFrame);
	Camera* Cam = CurScene->CameraHead;

	for (Object* O = CurScene->ObjectHead; O; O = O->Next)
	{
		if (O->Type != Obj_TriMesh) continue;
		TriMesh* T = (TriMesh*)O->Data;
		if (!(T->Flags & Tri_Invisible) && T->Flags & Tri_Ahead) {
			if (T->BSphereScreenPos.x < XRes && T->BSphereScreenPos.y < YRes && T->BSphereScreenPos.x > 0 && T->BSphereScreenPos.y > 0) {
				char text[255] = { 0 };
				snprintf(text, 254, "%s (%d)", O->Name, O->Number);
				OutTextXY(VPage, T->BSphereScreenPos.x, T->BSphereScreenPos.y, text, 255);
			}

			DrawSpline(T->Pos, View->ISource, Fist(CurScene->EndFrame));
		}
	}
#endif

	//test for edges stuff
/*	for(Tri=CurScene->TriMeshHead;Tri;Tri=Tri->Next)
	{
		if (Tri->Flags&Tri_Invisible) continue;
		for(I=0;I<Tri->EIndex;I++)
		if (Tri->Edges[I].Flags&Edge_Concave)
		Bivex_Frustrum_Perspective(Tri->Edges[I].A,Tri->Edges[I].B,&Draw_Line_32Bit_ForKids);
		else
		Bivex_Frustrum_Perspective(Tri->Edges[I].A,Tri->Edges[I].B,&Draw_Line_32Bit);
    }
	*/
/*  for(EP = ColEdge_Stack;EP<CES;EP++)
    Bivex_Frustrum_Perspective(EP[0]->A,EP[0]->B,&Draw_Line_32Bit_ForKids);*/
	
	//test for vertex stuff
	/*  DWord *VP = (DWord *)VPage;
	for(Tri=CurScene->TriMeshHead;Tri;Tri=Tri->Next)
	{
    if (Tri->Flags&Tri_Invisible) continue;
    for(I=0;I<Tri->VIndex;I++)
	if (Tri->Verts[I].Flags==Vtx_Spike)
	VP[YOffs[(int)Tri->Verts[I].PY]+(int)Tri->Verts[I].PX] = 0xFFFFFFFF;
}*/

	// FDS_DEBUG_XPAR_IDS=1 overlays each transparent face's render-order
	// index and SortZ at its screen centroid. Lets you read the actual sort
	// values that drove rendering — useful for diagnosing back-to-front
	// ordering on real scenes.
	if (fds::FeatureFlags::debug_xpar_ids()) {
		// Min screen area threshold (px²) — skips tiny particles.
		const float setArea = fds::FeatureFlags::debug_xpar_min_area();
		const float minArea = setArea > 0.0f ? setArea : 500.0f;
		std::fprintf(stderr, "[XPAR-IDS] (all transparent faces in render order)\n");
		for (int32_t i = 0; i < CAll; ++i) {
			Face* F = FList[i].face;
			if (!F || !F->Txtr) continue;
			if (!(F->Txtr->Flags & Mat_Transparent)) continue;
			const float ax = F->A->PX, ay = F->A->PY;
			const float bx = F->B->PX, by = F->B->PY;
			const float cxv = F->C->PX, cyv = F->C->PY;
			const float area = 0.5f * std::fabs(ax*(by-cyv) + bx*(cyv-ay) + cxv*(ay-by));
			const float cx = (ax + bx + cxv) * (1.0f/3.0f);
			const float cy = (ay + by + cyv) * (1.0f/3.0f);
			const float azv = F->A->TPos.z, bzv = F->B->TPos.z, czv = F->C->TPos.z;
			std::fprintf(stderr,
				"  [%4d] sortZ=%8.2f  tri-zs=(%.2f,%.2f,%.2f)  area=%6.0f  "
				"centroid=(%.0f,%.0f)  mat=%s\n",
				int(i), F->SortZ.F, azv, bzv, czv, area, cx, cy,
				(F->Txtr->Name ? F->Txtr->Name : "?"));
			if (area < minArea) continue;
			if (cx < 0 || cy < 0 || cx >= XRes || cy >= YRes) continue;
			char text[64];
			std::snprintf(text, sizeof(text), "%d z=%.1f", int(i), F->SortZ.F);
			OutTextXY(VPage, int(cx), int(cy), text, 255);
		}
	}
}

// yeah
char Check_Texture_Memory_Range(Scene *Sc)
{
	TriMesh *T;
	Face *F,*FE;
	Material *M;
	Material *Ptr;
	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		F = T->Faces;
		FE = F + T->FIndex;
		for(;F<FE;F++)
		{
			Ptr = F->Txtr;
			if (!Ptr) continue;
			for(M=MatLib;M;M=M->Next)
				if (M==Ptr)
					break;
				if (!M)
				{
					printf("Address generation Interlock!!\n"); // some ppl say Asyncrontus Gateway Interlock (AGI) delay...one of them is me -Silvatar.
					printf("Ptr = %ph\n", Ptr);
					printf("Material Name = %s\n",Ptr->Name);
					return 0;
				}
		}
	}
	//  for(M=MatLib;M;M=M->Next)
	//    printf("Mat-lib = %xh ==> txtr = %xh\n",(int32_t)M,(int32_t)(M->Txtr));
	return 1;
}

void fds::RenderPipeline::setCurrentScene(Scene *Sc)
{
	clipper.InitViewport(Sc);
	CurScene = Sc;
	g_zscale = (float)0xff00 / (CurScene->FZP * 1.1);
	g_zscale256 = g_zscale / 256.0f;

	C_FZP = Sc->FZP;
	C_rFZP = 1.0f/C_FZP;
	C_NZP = Sc->NZP;
	C_rNZP = 1.0f/C_NZP;

}

void fds::RenderPipeline::runScene(Scene *Sc,float Seconds)
{
	float t;
	int32_t TTrd=0;
	int32_t Par1,Par2,Par3,Par4,Par5,Par6,Tot;
	int32_t RenderedPolys=0;
	int32_t Y;
	int32_t SceneTime = Seconds * Timer_Freq;
	char MotionBlur_Enabled = 0;
	
	int32_t I,J;
	float FPS;
	char *Str = new char[80];
	char AfterFirstFrame = 0,Cur = 0;
	byte *BPage = new byte[PageSize];
	memset(BPage,0,PageSize);
	VESA_Surface Blurred;
	memcpy(&Blurred,Screen,sizeof(VESA_Surface));
	Blurred.Data = BPage;
	Blurred.Flags = VSurf_Noalloc;
	Blurred.Targ = VGAPtr;
	
	s_Layer2Storage.assign(PageSize, 0);
	Layer2.Data = s_Layer2Storage.data();
	Layer2.BPP = 32;
	Layer2.Flags = VSurf_Noalloc;
	Layer2.Targ = VGAPtr;
	
	Material *M;
	Object *Obj;
	SetCurrentScene(Sc);
	CurFrame=Sc->StartFrame;
	View=&FC;
	View->Next=Sc->CameraHead;
	if (Sc->CameraHead)
	{
		Sc->CameraHead->Prev=View;
		View=View->Next;
	}
	
	FList_Allocate(Sc);

	if (!Check_Texture_Memory_Range(Sc)) return;
	
	/*  for(Obj = Sc->ObjectHead;Obj;Obj=Obj->Next)
	printf("Object (%s)'s Pivot is (%f,%f,%f)\n",Obj->Name,Obj->Pivot.x,Obj->Pivot.y,Obj->Pivot.z);*/
	
	Timer=0; Frames=0;
	CurFrame=0;
	
	while (!Keyboard[ScESC])
	{
		if (Timer>SceneTime)
		{
			Frames=0; RenderedPolys=0;
			Timer-=SceneTime;
			TTrd-=SceneTime;
			Restore_Splines(Sc);
			// clear message buffer
			for(I=0;I<10;I++)
			{
				delete MsgStr[I];
				MsgStr[I]=NULL;
			}
		}
		t=float(Timer)/(float)SceneTime;
		
		CurFrame=float((1.0-t)*Sc->StartFrame)+float(t*Sc->EndFrame);
		if (!Keyboard[ScD])
		{
			memset(VPage,0,PageSize);
			//      memset(Layer2.Data,0,PageSize);
		}
		if (AfterFirstFrame) //so lame
		{
			dTime = Timer-TTrd;
			TTrd = Timer;
			if (View==&FC) Dynamic_Camera();
			dTime *= 0.25;
		} else {AfterFirstFrame=1; TTrd = Timer; dTime = 0;}
		Animate_Objects(Sc);
		Transform_Objects(Sc, fds::g_mainCamera, fds::g_mainFaces); Par1 = Timer;
		if (!(Sc->Flags&Scn_Nolighting)) Lighting(Sc); Par4 = Timer;
		
		//if (Sc->PclExec) Sc->PclExec(); Par2 = Timer;
		if (!CAll) goto Away;
		Radix_Sort(FList, SList, CAll);
		//    Quick_Sorting(0,CAll-1);
		if (!Keyboard[ScE]) Render();     Par5 = Timer;
Away:Tot = Timer-TTrd;
	 FPS = 100.0/((float)(Tot));
	 Frames++;
	 RenderedPolys+=CPolys;
	 
	 Y = -15;
	 for(I=0;I<10;I++)
		 if (MsgStr[I])
		 {
			 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,MsgStr[I],64);
			 if (Timer>MsgClock[I]+150)
			 {
				 delete MsgStr[I];
				 MsgStr[I]=NULL;
			 }
		 }
#ifdef RUNTIME_INFO_L1
		 snprintf(Str, 80,"Time = %d , Frame = %5.1f",Timer.load(), CurFrame);
		 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 snprintf(Str, 80,"Inst. FPS = %3.2f (%3.2f)",FPS,100.0*(float)Frames/(float)Timer);
		 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
#endif
#ifdef RUNTIME_INFO_L2
		 //    snprintf(Str, 80,"Active Camera = %s",View->Name);
		 //    Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 //    if (Tot)
		 //		{
		 //			snprintf(Str, 80,"Perf. Distribution: xForm=%d%%,P.Knmtx=%d%%,FList=%d%%,Light=%d%%,\nRender=%d%%",(Par1-TTrd)*100/Tot,(Par2-Par1)*100/Tot,(Par3-Par2)*100/Tot,(Par4-Par3)*100/Tot,(Par5-Par4)*100/Tot);
		 //			Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 //		}
		 //    snprintf(Str, 80,"Dyn. Kinematics: p(%4.2f,%4.2f,%4.2f),v(%1.4f,%1.4f,%1.4f) r(%1.4f,%1.4f,%1.4f)",FC.ISource.x,FC.ISource.y,FC.ISource.z,FV.x,FV.y,FV.z,FT.x,FT.y,FT.z);
		 //    Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 snprintf(Str, 80,"Average Polys/Frame: %5.1f (%3.1f%%)",(float)RenderedPolys/(float)Frames,100.0*(float)CPolys/(float)Polys);
		 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
		 int32_t GTP = 0,GTPP = 0;
		 for(TriMesh *Tri = Sc->TriMeshHead;Tri;Tri = Tri->Next)
			 if (!(Tri->Flags&Tri_Invisible)) {GTP++; GTPP += Tri->FIndex;}
			 snprintf(Str, 80,"%d Triangle Meshes totalling in %d Faces",GTP,GTPP);
			 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
#endif
#ifdef RUNTIME_INFO_L3
			 snprintf(Str, 80,"Rendered %d Polygons,%d Omnilights and %d particles.\n",CPolys,COmnies,CPcls);
			 Y = OutTextXY(VPage,0,Y+15 * g_fontScale,Str,64);
#endif
			 //    DisplayMouse();
			 //		Radiosity(Sc);
			 
			 if (!Keyboard[ScD])
			 {
				 //hope it works ;)
				 if (MotionBlur_Enabled)
				 {
					 if (MMXState)
						 Modulate(Screen,&Blurred,0xb0b0b0,0xb0b0b0, PageSize);
					 else
						 Transparence(Screen,&Blurred);
					 Flip(&Blurred);
				 }
				 //else memcpy(BPage,VPage,PageSize); //v.fast flip emulation
				 else Flip(Screen);
				 
				 /*      if (MMXState)
				 MulModulate(&Layer2,Screen);
				 else*/
				 //    Transparence(&Layer2,Screen);
			 }
			 
			 if (Keyboard[ScP]) { I = Timer; while (Keypressed()) {}; Timer = I; } //Timefreeze
			 if (Keyboard[ScTab]) { if (View->Next) { View = View->Next; } else { View = &FC; } }
			 /*		if (Keyboard[ScR]) {
			 Restore_9();
			 Deinitialize_VESA();
			 printf("Debug me, PLEASE!\n");
			 getchar();
			 getchar();
			 getchar();
			 exit(-1);
		}*/
			 
			 // Free camera ASSUME IDENTITY command.
			 if (Keyboard[ScC]) {FC.ISource = View->ISource; Matrix_Copy(FC.Mat,View->Mat); FC.IFOV=View->IFOV;}
			 if (Keyboard[ScH]) Cam_HeadLight^=1;
			 if (Keyboard[ScM]) MotionBlur_Enabled^=1;
			 if (Keyboard[ScOpenSq]) Sc->FZP+=100;
			 if (Keyboard[ScCloseSq]) Sc->FZP-=100;
			 if (Keyboard[ScY])
			 {
				 FILE *ff = fopen("bearing.","at");
				 fprintf(ff,"POS = (%5.3f,%5.3f,%5.3f), ORIENT = (%5.3f,%5.3f,%5.3f)\n",FC.ISource.x,FC.ISource.y,FC.ISource.z,FC.Mat[2][0],FC.Mat[2][1],FC.Mat[2][2]);
				 fclose(ff);
			 }
	}
	//	printf("FPS == %f\n",100.0*(float)Frames/(float)Timer);
	while (Keyboard[ScESC]) DO_NOP();
	//Exec_FPS = 100.0*(float)Frames/(float)Par5;
	/*  snprintf(Str, 80,"Inst. FPS = %3.2f (%3.2f)",FPGS,100.0*(float)Frames/(float)Par4);
	OutTextXY(VGAPtr,0,0,Str,63);
	while (!Keyboard[ScESC]) DO_NOP();
	while (Keyboard[ScESC]) DO_NOP();*/
	Restore_Splines(Sc);
	// FList / SList storage lives in fds::g_mainFaces; its dtor cleans
	// up (previously a buggy `delete` without `[]` here that raced with
	// the unique_ptr-based scene drivers — see FaceListContext.h).
	// Layer2.Data is a view into s_Layer2Storage, owned at file scope;
	// std::vector dtor cleans it up. (Was: `delete Layer2.Data;` without
	// `[]` — UB on what new[] returned.)
	delete [] Str;
	delete [] BPage;
}

// ─── RenderPipeline singleton + legacy free-function forwarders ────────
// The free functions are still declared in FDS_DECS.H; DEMO scenes and
// snapshot harnesses call them by name. They forward into the singleton
// so internal pipeline state can migrate onto the class over time
// without touching every call site.

fds::RenderPipeline &fds::RenderPipeline::instance() {
	static RenderPipeline s_instance;
	return s_instance;
}

void Render(RenderPath path)         { fds::RenderPipeline::instance().renderFrame(path); }
void RunScene(Scene *Sc, float Sec)  { fds::RenderPipeline::instance().runScene(Sc, Sec); }
void SetCurrentScene(Scene *Sc)      { fds::RenderPipeline::instance().setCurrentScene(Sc); }
void SetDeferredWaterMatID(int id)   { fds::RenderPipeline::instance().setDeferredWaterMatID(id); }
