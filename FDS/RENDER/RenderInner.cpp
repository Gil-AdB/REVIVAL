// Per-tile rasterizer dispatchers — extracted from RENDER.CPP.
//
// Three small functions, each invoked once per tile from the renderFrame
// orchestrator's tile-job lambda. They walk FList for that tile and
// dispatch each face to its filler:
//
//   RenderInner                 forward path — TheOtherBarry templated
//                               by material flags (REFL / ADDITIVE / TEX).
//   RenderInnerMekalele         opaque deferred — fills the G-buffer;
//                               reflective + additive surfaces still go
//                               through the forward fillers here.
//   RenderInnerDeferredTransparent  transparent deferred — fills the
//                               xpar G-buffer (front / back layer per
//                               FaceSel). Caller composites afterwards.
//
// The renderns counter + condition variable used to synchronise tile
// completion lives in RENDER.CPP; lambdas in the dispatchers ping it
// when the tile finishes.

#include <stdio.h>
#include <string.h>
#include <Base/Vector.h>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "Base/Face.h"
#include "Base/TriMesh.h"
#include "Base/Material.h"
#include "Base/Vertex.h"
#include "FILLERS/FILLERS.H"
#include "FILLERS/Mekalele.h"
#include "FILLERS/TheOtherBarry.h"
#include "FRUSTRUM.H"
#include "Threads.h"
#include "RenderPipeline.h"

namespace renderns {
	extern std::mutex                tileCounterMutex;
	extern std::atomic<int>          tileCounter;
	extern std::condition_variable   condition;
}

void RenderInner(float x1, float y1, float x2, float y2) {
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(x1, y1, x2, y2);
	auto rt  = fds::MainRenderTargetFromGlobals();
	// Forward dispatcher: the deferred lighting kernel won't read the
	// G-buffer this frame, so we don't want TheOtherBarry stamping its
	// mat32 sentinel into it. Also relevant during the cube-map bake:
	// rt.xres there reflects TmpSurf (1024) but g_gbuffer is sized to
	// the engine framebuffer (1920×…), so a stamp would write at the
	// wrong row stride and corrupt the next deferred frame's mat32.
	rt.gbuffer                = nullptr;
	rt.gbufferTransparent     = nullptr;
	rt.gbufferTransparentBack = nullptr;
	const auto& cam = fds::g_mainCamera;

	int32_t I = CAll;
	fds::FListEntry* FLS = FList;//+CAll-1;
	Vertex* A, * B, * C;

	Vertex* V[4];

	while (I--) {
		Face* F = (FLS++)->face;

		// Get Mapping Coordinates from the rendered face.
		A = F->A; B = F->B;
		if (A == B) {
			continue;
		} else {
			// Polygon - can further check the expression
			// (A->Flags|B->Flags|C->Flags)&Vtx_Visible
			// if it is zero, we can use something lighter than
			// the frustrum clipper.
			C = F->C;

			auto flags = A->Flags & B->Flags & C->Flags;
			if (flags & Vtx_Visible) continue;
			RasterFunc filler;
			if (F->Flags & Face_Reflective) {
				clipper.Render(F, TheOtherBarry<barry::TBlendMode::OVERWRITE, barry::TTextureMode::TEXTURETEXTURE>, false, rt, cam);
			} else {
				clipper.Render(F, F->Filler, false, rt, cam);
			}
			//if (F->Flags & Face_Reflective) {
			//	clipper.Render(F, IX_Prefiller_Reflective, true);
			//}
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}


void RenderInnerMekalele(float x1, float y1, float x2, float y2) {
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(x1, y1, x2, y2);
	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;

	int32_t I = CAll;
	fds::FListEntry* FLS = FList;//+CAll-1;
	Vertex* A, * B, * C;

	Vertex* V[4];

	while (I--) {
		Face* F = (FLS++)->face;

		// Get Mapping Coordinates from the rendered face.
		A = F->A; B = F->B;
		if (A == B) {
			continue;
		} else {
			// Polygon - can further check the expression
			// (A->Flags|B->Flags|C->Flags)&Vtx_Visible
			// if it is zero, we can use something lighter than
			// the frustrum clipper.
			C = F->C;

			auto flags = A->Flags & B->Flags & C->Flags;
			if (flags & Vtx_Visible) continue;
			// Mekalele dereferences F->Txtr->Txtr->LSizeX in its
			// TileRasterizer constructor — untextured Materials would
			// segfault. Skip them for now; the matID/Material dispatch
			// in the future lighting pass needs a sentinel slot for
			// "no texture; use BaseCol", which is the proper fix.
			if (!F->Txtr || !F->Txtr->Txtr) continue;
			// Reflective faces (City windows, etc.) want forward's
			// per-vertex eu/ev interpolation across the panorama —
			// per-pixel reflection-vector reconstruction in the
			// deferred lighting pass produced wrong-scale, jittery
			// reflections that didn't track the world the way
			// Reflective_Mapper_Setup's per-face cv tweak does.
			// Forward TheOtherBarry<OVERWRITE,TEXTURETEXTURE> writes
			// directly to VPage and ZPage16 (skipping the G-buffer);
			// the mat32 sentinel pre-clear ensures the deferred
			// lighting pass treats those pixels as already-shaded
			// and leaves them alone.
			//
			// Material classification:
			//   Mat_Transparent: deferred — render AFTER lighting pass
			//     so the alpha blend reads finished opaque pixels.
			//     (water, glass, etc. — handled by RenderInnerDeferredTransparent.)
			//   Mat_Additive: render NOW via forward filler (TheOtherBarry
			//     <ADDITIVE>) so it Z-tests + writes Z alongside opaque
			//     in the same tile pass. Most-prominent case is the
			//     fountain vortex, which has SortPriorityBias=DrawFirst
			//     (sorted to FList front) — drawing it first writes its
			//     Z to the empty z-buffer; opaque Mekalele then overwrites
			//     where opaque is closer. Render_DeferredLighting's mat32
			//     sentinel skip preserves vortex's color where no opaque
			//     covered. Particles + transparents drawn later Z-test
			//     against vortex Z and get correctly occluded.
			const dword txtrFlags = F->Txtr->Flags;
			if (txtrFlags & Mat_Transparent) {
				continue;
			}
			if (F->Flags & Face_Reflective) {
				clipper.Render(F, TheOtherBarry<barry::TBlendMode::OVERWRITE, barry::TTextureMode::TEXTURETEXTURE>, false, rt, cam);
			} else if (txtrFlags & Mat_Additive) {
				clipper.Render(F, F->Filler, false, rt, cam);   // TheOtherBarry<ADDITIVE>
			} else {
				clipper.Render(F, Mekalele, false, rt, cam);
			}
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

// XparFaceSel selects which face orientation to raster in this pass.
// Concentric transparent meshes need their back-facing tris of OUTER
// composited BEFORE the back-facing tris of INNER, but their front-
// facing tris in the REVERSE order — back-to-front face sequence for
// two nested spheres is:
//   outer_back, inner_back, inner_front, outer_front
// The caller drives this by issuing two phases: phase 1 raster+
// composite of back faces (outer→inner), phase 2 raster+composite of
// front faces (inner→outer). XparFaceSel::Both keeps the legacy single-
// pass behaviour. Defined in RenderPipeline.h.

void RenderInnerDeferredTransparent(float x1, float y1, float x2, float y2,
                                     int firstIdx, int countIdx,
                                     TriMesh* parentFilter,
                                     XparFaceSel faceSel = XparFaceSel::Both) {
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(x1, y1, x2, y2);
	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;

	int32_t I = countIdx;
	fds::FListEntry* FLS = FList + firstIdx;
	Vertex *A, *B, *C;

	while (I--) {
		Face* F = (FLS++)->face;

		// Per-object peeling: when parentFilter is non-null, skip any
		// face that didn't come from that TriMesh. Lets the caller
		// process one transparent object at a time so nested objects
		// (concentric outer+inner) each get their own 2-deep render
		// + composite cycle instead of fighting for the shared back
		// layer.
		if (parentFilter && F->ParentTri != parentFilter) continue;

		A = F->A; B = F->B;
		if (A == B) continue;
		C = F->C;
		auto flags = A->Flags & B->Flags & C->Flags;
		if (flags & Vtx_Visible) continue;
		if (!F->Txtr) continue;
		const dword txtrFlags = F->Txtr->Flags;
		// Mat_Additive (e.g. fountain vortex) renders inside the main
		// Mekalele tile pass via its forward filler, NOT here — that
		// gives it forward-style Z integration with opaque. Only handle
		// Mat_Transparent in this post-lighting pass.
		if (!(txtrFlags & Mat_Transparent)) continue;
		if (F->Flags & Face_Reflective) continue;  // handled in main pass

		// Per-face front/back test: compute the triangle's view-space face
		// normal from its three vertex positions, then sign-test against any
		// point on the face. Using A->TN (a per-vertex normal that the engine
		// smooths across adjacent faces) splits coplanar triangles of the
		// same quad into different code paths when the averaged vertex
		// normals diverge slightly — caused the "panel renders as a frame"
		// bug in xpartest case 3.
		const float ex = B->TPos.x - A->TPos.x;
		const float ey = B->TPos.y - A->TPos.y;
		const float ez = B->TPos.z - A->TPos.z;
		const float fx = C->TPos.x - A->TPos.x;
		const float fy = C->TPos.y - A->TPos.y;
		const float fz = C->TPos.z - A->TPos.z;
		const float nx = ey * fz - ez * fy;
		const float ny = ez * fx - ex * fz;
		const float nz = ex * fy - ey * fx;
		const float vd = nx * A->TPos.x + ny * A->TPos.y + nz * A->TPos.z;
		const bool frontFacing = vd < 0.0f;
		if (frontFacing) {
			if (faceSel == XparFaceSel::BackOnly) continue;
			clipper.Render(F, MekaleleTransparent, false, rt, cam);
		} else {
			if (faceSel == XparFaceSel::FrontOnly) continue;
			// 2-deep transparent G-buffer: back-facing tris of convex
			// transparents (the exit surface along the view ray) go to
			// the back layer. The composite pass lights the back layer
			// onto VPage first, then the front layer on top — so a
			// glass cube shows both its near and far walls.
			clipper.Render(F, MekaleleTransparentBack, false, rt, cam);
		}
	}

	std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
	++renderns::tileCounter;
	renderns::condition.notify_one();
}

// FDS_DEFERRED=1 in env enables the experimental G-buffer path: tiled
// dispatch goes to RenderInnerMekalele instead of RenderInner, then
// Render_DeferredLighting consumes the G-buffer to produce final pixels.
// Lighting() also early-returns when this is set — its per-vertex
