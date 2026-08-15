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
#include <stdlib.h>
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
#include "Base/RenderContext.h"
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
#include "FaceTileBin.h"

#include <semaphore>
#include <climits>

// Shared view-space front/back classifier (defined in Transform.cpp). Reads
// the SoA frame; the xpar batch loop in RENDER.CPP uses the same helper, so
// the peel rasterizer below must too (see RenderInnerDeferredTransparent).
bool IsFrontFacingInViewSpace(const Face* F);

namespace renderns {
	extern std::counting_semaphore<INT_MAX> tileDone;
	extern std::mutex                tileCounterMutex;
	extern std::atomic<int>          tileCounter;
	extern std::condition_variable   condition;
}

void RenderInner(const fds::RenderContext& ctx, float x1, float y1, float x2, float y2,
                 Face* const* binFaces, int32_t binCount) {
	// Forward raster of ctx's face list into ctx's target — all per-pass
	// state from ctx, no globals, so a worker can run this on its own
	// surface/face-list concurrently (RenderContext migration; the parallel
	// shard/shadow/RTT bakes are the consumers). renderFrame passes the
	// primary context (= the old globals → byte-identical).
	FrustumClipper clipper;
	// Near/far from ctx.camera, NOT ctx.scene: the scene's NZP/FZP are
	// shared mutable state, so concurrent offscreen workers (Slice 6 shard
	// pass) can't each stamp a different mirror-plane near-Z into it. The
	// CameraContext overload reads cam.nearZ/farZ + reciprocals, which are
	// byte-identical to Sc->NZP/FZP for the main frame and the RTT path
	// (SetCurrentScene stamps C_NZP=Sc->NZP and C_rNZP=1.0f/C_NZP, the same
	// float expression viewportInit uses). The clip extents InitViewport
	// would set are overwritten by SetClippingExtents below regardless.
	clipper.InitViewport(ctx.camera);
	clipper.SetClippingExtents(x1, y1, x2, y2);
	auto rt  = ctx.target;
	// Forward dispatcher: the deferred lighting kernel won't read the
	// G-buffer this frame, so we don't want TheOtherBarry stamping its
	// mat32 sentinel into it. Also relevant during the cube-map bake:
	// rt.xres there reflects TmpSurf (1024) but g_gbuffer is sized to
	// the engine framebuffer (1920×…), so a stamp would write at the
	// wrong row stride and corrupt the next deferred frame's mat32.
	rt.gbuffer                = nullptr;
	rt.gbufferTransparent     = nullptr;
	rt.gbufferTransparentBack = nullptr;
	const auto& cam = ctx.camera;

	int32_t I = binFaces ? binCount : ctx.faces.cAll;
	fds::FListEntry* FLS = ctx.faces.fList;
	Vertex* A, * B, * C;

	Vertex* V[4];

	// S2 / B5 tile pre-reject (see RenderInnerMekalele for the rationale).
	// When the caller handed us a face->tile bin (--face_tile_bin) the reject
	// has already been done once for the whole frame and this tile's run is
	// dense: read Face* straight out of it, in the same order the walk gave.
	const bool bboxCull = !binFaces && fds::FeatureFlags::tile_bbox_cull();
	const int tx1 = int(x1), ty1 = int(y1), tx2 = int(x2), ty2 = int(y2);

	while (I--) {
		Face* F;
		if (binFaces) {
			F = *binFaces++;
		} else {
			const fds::FListEntry* ep = FLS++;
			if (bboxCull && (ep->bbMaxX < tx1 || ep->bbMinX >= tx2 ||
			                 ep->bbMaxY < ty1 || ep->bbMinY >= ty2)) continue;
			F = ep->face;
		}

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
			// Untextured faces are unrenderable here — every forward filler
			// (TheOtherBarry, the reflective env path) dereferences
			// F->Txtr->Txtr. The deferred path (RenderInnerMekalele) already
			// skips these; match it so a face deliberately blanked by
			// nulling its Txtr (e.g. the shattered greets screen) doesn't
			// crash the offscreen forward passes (RTT, env bake).
			if (!F->Txtr || !F->Txtr->Txtr) continue;
			RasterFunc filler;
			if (F->Flags & Face_Reflective) {
				// Clone env faces (the disco ball's mirror image):
				// the forward env filler has no per-pixel mirror-mask
				// commit gate (that's Mekalele's), so without this
				// check a mirrored ball renders floating in the void
				// outside the room whenever its mirror is active.
				// Same centroid-footprint gate as the clone-flare one
				// in The_MMX_Scalar.
				if (F->mirrorMaskTag != 0) {
					if (!rt.gbuffer || rt.gbuffer->mirrorMask.empty())
						continue;
					int32_t mx = int32_t((F->A->PX + F->B->PX + F->C->PX) * (1.0f / 3.0f));
					int32_t my = int32_t((F->A->PY + F->B->PY + F->C->PY) * (1.0f / 3.0f));
					if (mx < 0) mx = 0; else if (mx > rt.xres - 1) mx = rt.xres - 1;
					if (my < 0) my = 0; else if (my > rt.yres - 1) my = rt.yres - 1;
					if (rt.gbuffer->mirrorMask[size_t(my) * size_t(rt.xres) + size_t(mx)]
					    != F->mirrorMaskTag)
						continue;
				}
				clipper.Render(F, TheOtherBarry<barry::TBlendMode::OVERWRITE, barry::TTextureMode::TEXTURETEXTURE>, false, rt, cam);
			} else {
				clipper.Render(F, F->Filler, false, rt, cam);
			}
			//if (F->Flags & Face_Reflective) {
			//	clipper.Render(F, IX_Prefiller_Reflective, true);
			//}
		}
	}

	// One permit per completed tile. Lock-free; drained by the
	// orchestrator's `for(i<N) tileDone.acquire()` loop. Replaces the
	// prior lock+increment+notify pattern (see RENDER.CPP renderns).
	renderns::tileDone.release();
}

// Single-threaded forward render of a region — no threadpool dispatch or
// barrier. For tiny offscreen targets (the mirror-shard 64² reflections),
// the per-Render() enqueue + semaphore-barrier overhead dwarfs the actual
// raster (profiled: workers 97% idle, the dispatch is the cost). Renders
// inline on the caller's thread, then drains the permit RenderInner
// released so the shared tileDone semaphore stays net-zero for the next
// threadpool Render(). Preconditions match Render(ForceForward): FList/CAll
// populated, CurScene + surface globals (VPage/ZPage16/XRes) set.
void RenderForwardRegionInline(const fds::RenderContext& ctx,
                               float x1, float y1, float x2, float y2) {
	RenderInner(ctx, x1, y1, x2, y2, nullptr, 0);
	renderns::tileDone.acquire();   // drain RenderInner's release (non-blocking)
}

// Deferred G-buffer fill of ctx's face list into ctx.target's G-buffer —
// single-threaded, inline, no tileDone traffic. The deferred sibling of
// RenderForwardRegionInline: the deferred shard bake (MirrorShatter) and the
// deferred mirror RTT (GreetsMirror) call this to fill their own G-buffer,
// then run Render_DeferredLighting with a per-pass DeferredOverride to shade it.
// Same face routing as RenderInnerMekalele (the main deferred opaque pass) so
// an offscreen view of the full room renders correctly: opaque → Mekalele
// (G-buffer); reflective → forward env filler straight to vpage (the deferred
// kernel leaves those mat32-sentinel pixels alone); additive → forward filler;
// transparent → skipped (no xpar pass offscreen). ctx.target.gbuffer MUST be
// set (unlike the forward path, which nulls it).
void MekaleleFillRegionInline(const fds::RenderContext& ctx,
                              float x1, float y1, float x2, float y2) {
	FrustumClipper clipper;
	clipper.InitViewport(ctx.camera);
	clipper.SetClippingExtents(x1, y1, x2, y2);
	const auto rt  = ctx.target;     // G-buffer KEPT (deferred fill)
	const auto& cam = ctx.camera;
	int32_t I = ctx.faces.cAll;
	fds::FListEntry* FLS = ctx.faces.fList;
	while (I--) {
		Face* F = (FLS++)->face;
		Vertex* A = F->A; Vertex* B = F->B;
		if (A == B) continue;
		Vertex* C = F->C;
		if ((A->Flags & B->Flags & C->Flags) & Vtx_Visible) continue;
		if (!F->Txtr || !F->Txtr->Txtr) continue;
		const dword txtrFlags = F->Txtr->Flags;
		if (txtrFlags & Mat_Transparent) continue;   // no offscreen xpar pass
		if (F->Flags & Face_Reflective) {
			// Clone env faces gate on the mirror mask (which an offscreen
			// G-buffer doesn't carry) — skip them; regular reflective faces
			// (disco ball, windows) render the env map straight to vpage.
			if (F->mirrorMaskTag != 0) continue;
			clipper.Render(F, TheOtherBarry<barry::TBlendMode::OVERWRITE,
			               barry::TTextureMode::TEXTURETEXTURE>, false, rt, cam);
		} else if (txtrFlags & Mat_Additive) {
			clipper.Render(F, F->Filler, false, rt, cam);   // TheOtherBarry<ADDITIVE>
		} else {
			clipper.Render(F, Mekalele, false, rt, cam);
		}
	}
}


void RenderInnerMekalele(float x1, float y1, float x2, float y2,
                         Face* const* binFaces, int32_t binCount) {
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(x1, y1, x2, y2);
	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;

	int32_t I = binFaces ? binCount : CAll;
	fds::FListEntry* FLS = FList;//+CAll-1;
	Vertex* A, * B, * C;

	Vertex* V[4];

	// S2 / B5 tile pre-reject: skip faces whose stamped screen bbox
	// (Transform.cpp FList build) misses this tile rect BEFORE touching the
	// Face — a rejected face costs only the sequential FListEntry read, not
	// the three scattered Vertex loads the visibility test needs. Pure reject
	// (the clipper already clips to the tile), gated for A/B; flag-off faces
	// carry the cover-all sentinel so the test never fires.
	//
	// --face_tile_bin hoists that reject out of the per-tile walk entirely:
	// renderFrame bins the list once per pass and hands each tile a dense
	// Face* run, in the SAME order this walk produces (FaceTileBin.h). When
	// binFaces is non-null the bbox test below is already accounted for.
	const bool bboxCull = !binFaces && fds::FeatureFlags::tile_bbox_cull();
	const int tx1 = int(x1), ty1 = int(y1), tx2 = int(x2), ty2 = int(y2);

	while (I--) {
		Face* F;
		if (binFaces) {
			F = *binFaces++;
		} else {
			const fds::FListEntry* ep = FLS++;
			if (bboxCull && (ep->bbMaxX < tx1 || ep->bbMinX >= tx2 ||
			                 ep->bbMaxY < ty1 || ep->bbMinY >= ty2)) continue;
			F = ep->face;
		}

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
				// Clone env faces (the disco ball's mirror image):
				// the forward env filler has no per-pixel mirror-mask
				// commit gate (that's Mekalele's), so without this
				// check a mirrored ball renders floating in the void
				// outside the room whenever its mirror is active.
				// Same centroid-footprint gate as the clone-flare one
				// in The_MMX_Scalar.
				if (F->mirrorMaskTag != 0) {
					if (!rt.gbuffer || rt.gbuffer->mirrorMask.empty())
						continue;
					int32_t mx = int32_t((F->A->PX + F->B->PX + F->C->PX) * (1.0f / 3.0f));
					int32_t my = int32_t((F->A->PY + F->B->PY + F->C->PY) * (1.0f / 3.0f));
					if (mx < 0) mx = 0; else if (mx > rt.xres - 1) mx = rt.xres - 1;
					if (my < 0) my = 0; else if (my > rt.yres - 1) my = rt.yres - 1;
					if (rt.gbuffer->mirrorMask[size_t(my) * size_t(rt.xres) + size_t(mx)]
					    != F->mirrorMaskTag)
						continue;
				}
				clipper.Render(F, TheOtherBarry<barry::TBlendMode::OVERWRITE, barry::TTextureMode::TEXTURETEXTURE>, false, rt, cam);
			} else if (txtrFlags & Mat_Additive) {
				clipper.Render(F, F->Filler, false, rt, cam);   // TheOtherBarry<ADDITIVE>
			} else {
				clipper.Render(F, Mekalele, false, rt, cam);
			}
		}
	}

	// One permit per completed tile. Lock-free; drained by the
	// orchestrator's `for(i<N) tileDone.acquire()` loop. Replaces the
	// prior lock+increment+notify pattern (see RENDER.CPP renderns).
	renderns::tileDone.release();
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

		// Per-face front/back test: geometric view-space face normal sign,
		// via the SHARED SoA helper. This MUST match the batch-side classifier
		// in RENDER.CPP (which also calls IsFrontFacingInViewSpace) — otherwise
		// the batch flushes one side (faceSel) while we classify the other, and
		// every face is rejected. The old inline test read per-vertex AoS
		// TPos_AOS, which is STALE for SoA-only meshes: the City water's
		// tessellated grid (Phase 6.1) carries garbage AoS positions, so it
		// disagreed with the batch, flushed BackOnly vs. front, and the water
		// never rasterized (only the pass-1 reflection underlay showed; HDR fog
		// then buried it). The helper reads F->frame SoA, same as the batch.
		const bool frontFacing = IsFrontFacingInViewSpace(F);
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

	// One permit per completed tile. Lock-free; drained by the
	// orchestrator's `for(i<N) tileDone.acquire()` loop. Replaces the
	// prior lock+increment+notify pattern (see RENDER.CPP renderns).
	renderns::tileDone.release();
}

// FDS_DEFERRED=1 in env enables the experimental G-buffer path: tiled
// dispatch goes to RenderInnerMekalele instead of RenderInner, then
// Render_DeferredLighting consumes the G-buffer to produce final pixels.
// Lighting() also early-returns when this is set — its per-vertex
