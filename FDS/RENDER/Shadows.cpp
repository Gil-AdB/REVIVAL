// Per-frame shadow-map orchestration — extracted from RENDER.CPP.
//
// Render_DeferredShadowMaps(Sc) walks Omni_CastsShadow lights, builds a
// per-light camera, swaps the engine's view globals to it, re-runs
// Transform_Objects to project geometry into the light's POV, and
// rasterizes depth + polyId into the matching ShadowMap. Globals are
// saved and restored — callers see no side effects on the main camera /
// perspective / clipper.
//
// Compile-time and runtime gates:
//   --shadows / FDS_SHADOWS              master enable
//   --shadow_polyid / FDS_SHADOW_POLYID  initial g_shadowMode (PolyId vs
//                                        Depth comparison; F3 in greets
//                                        toggles at runtime)
//   g_inShadowPass  per-thread flag set during the depth pre-pass so
//                   the rasterizer / culler can take a depth-only path.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <Base/Vector.h>
#include <atomic>
#include <vector>
#include <chrono>

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "Base/Camera.h"
#include "Base/Omni.h"
#include "Base/SpotLight.h"
#include "Base/TriMesh.h"
#include "Base/Material.h"
#include "FILLERS/ShadowMap.h"
#include "FILLERS/Mekalele.h"
#include "FRUSTRUM.H"
#include "Threads.h"

#include <mutex>
#include <condition_variable>
namespace renderns {
	extern std::mutex                tileCounterMutex;
	extern std::atomic<int>          tileCounter;
	extern std::condition_variable   condition;
}

// Per-frame depth pre-pass over every Omni_CastsShadow light. For each
// such light we build a temporary Camera (look down IDir, FOV = spot
// outer cone), swap the engine's view globals to it, re-run
// Transform_Objects to project every visible face into the light's
// view+screen space, then walk FList and rasterize depth-only into the
// light's ShadowMap. Globals are saved/restored around the whole
// thing — when this returns the main view + perspective + clipper are
// untouched.
//
// Re-uses the main FList buffer. Transform_Objects overwrites it
// with this light's CAll faces; the caller (Render()) re-runs
// Transform_Objects with the main camera after this returns to
// repopulate FList for the main pass. (One extra Transform_Objects per
// shadow light per frame — acceptable until we profile.)
//
// Off by default while wiring; flip via --shadows.
static bool shadowsEnabled() {
	return fds::FeatureFlags::shadows();
}

thread_local bool g_inShadowPass = false;

// PolyId is the production default. F3 still toggles at runtime.
std::atomic<ShadowMode> g_shadowMode{
	fds::FeatureFlags::shadow_polyid() ? ShadowMode::PolyId : ShadowMode::Depth};


void Render_DeferredShadowMaps(Scene *Sc)
{
	if (!shadowsEnabled()) return;
	if (!Sc || g_shadowMaps.empty()) return;

	// Per-shadow-pass timing breakdown of Transform vs Rasterize cost,
	// averaged over a rolling window. Tells us where to focus optimization
	// — if Transform >> Raster, cross-light parallelism is moot.
	const bool sProfShadow = fds::FeatureFlags::shadow_prof();
	using clk = std::chrono::high_resolution_clock;
	static thread_local int sFrameIdx = 0;
	static thread_local double sXformAcc = 0.0;
	static thread_local double sRasterAcc = 0.0;
	static thread_local int sLightCount = 0;

	// Save engine state.
	Camera *savedView   = View;
	const float savedFOVX  = FOVX;
	const float savedFOVY  = FOVY;
	const float savedCntrEX = CntrEX;
	const float savedCntrEY = CntrEY;

	Camera lightCam = {};

	for (ShadowMap& sm : g_shadowMaps) {
		Omni *O = sm.omni;
		if (!O) continue;
		if (!(O->Flags & Omni_Active)) continue;
		if (O->Type != Light_SpotLight) continue;  // omni cube maps deferred

		// Light camera. Look from O->IPos toward IPos+IDir, world-up=Y.
		// Roll=0. FOV = 2 * acos(cosOuter) so the outer cone fits in
		// the shadow map. Pad a bit so silhouettes near the cone edge
		// have a few pixels of context (otherwise floating-point in
		// the lighting kernel's cone test lands pixels just past the
		// shadow map edge that read as "lit").
		// Kick_Camera has a singularity at IDir=(0,±1,0) — it builds the
		// horizontal basis as N=(V.z,0,-V.x) and normalizes, which goes
		// 0/0 when the look direction is exactly world-up. Nudge by a
		// tiny X bias if the caller handed us a perfectly-vertical IDir.
		Vector idir = O->IDir;
		if (std::fabs(idir.x) < 1e-4f && std::fabs(idir.z) < 1e-4f) {
			idir.x = 0.01f;
		}
		Vector targ = O->IPos;
		targ.x += idir.x;
		targ.y += idir.y;
		targ.z += idir.z;
		Kick_Camera(&O->IPos, &targ, 0.0f, lightCam.Mat);
		lightCam.ISource = O->IPos;
		const float cosOuter = std::max(0.01f, O->FallOff);
		const float fovHalfRad = std::acos(cosOuter) * 1.10f;  // 10% pad
		const float perspXY = (float(sm.xres) * 0.5f) / std::tan(fovHalfRad);
		lightCam.PerspX = perspXY;
		lightCam.PerspY = perspXY;
		lightCam.IFOV   = (fovHalfRad * 2.0f) * (180.0f / PI);

		// Stash the light camera's pose onto the ShadowMap so the
		// lighting kernel can sample later without rebuilding it.
		Matrix_Copy(sm.lightViewMat, lightCam.Mat);
		sm.lightISource = O->IPos;
		sm.perspX = perspXY;
		sm.perspY = perspXY;
		sm.cntrX  = float(sm.xres) * 0.5f - 0.5f;
		sm.cntrY  = float(sm.yres) * 0.5f - 0.5f;

		// Swap globals to the shadow camera. XRes/YRes are passed via
		// Transform_Objects(Sc, xres, yres) override instead of mutating
		// the globals — Transform_Objects's visibility-flag math (incl.
		// the F->VisibilityFlagsAll face filter) needs to use the shadow
		// rect, not the main screen, otherwise hex tiles near the cone
		// edge get all-3-verts-VisDown and the face filter drops them.
		View   = &lightCam;
		FOVX   = perspXY;
		FOVY   = perspXY;
		CntrEX = sm.cntrX;
		CntrEY = sm.cntrY;

		// Patch the scene's FZP for the duration so Transform_Objects
		// clips against the light's range, not the camera's. We restore
		// it inside the loop because the next shadow light may have a
		// different range. Final restore happens via SetCurrentScene at
		// the bottom.
		const float savedSceneFZP = Sc->FZP;
		Sc->FZP = sm.fzp;

		// Refresh sm.zScale to match what the rasterizer expects.
		sm.zScale = float(0xFF00) / (sm.fzp * 1.1f);

		// Clear shadow map.
		std::fill(sm.depth.begin(), sm.depth.end(), uint16_t(0));
		std::fill(sm.polyId.begin(), sm.polyId.end(), uint8_t(0));

		// Transform from the light's POV. Overwrites FList + per-vertex
		// PX/PY/RZ — main pass re-transforms after we return.
		// Skip Animate_Objects: the scene driver already animated this
		// frame's object poses before calling us. We just re-project
		// those poses through the light camera; calling Animate_Objects
		// again would re-tick per-frame state (object splines,
		// particles, anything its TriMesh-walk touches) and diverge
		// the main pass.
		const auto tXformStart = clk::now();
		g_inShadowPass = true;
		Transform_Objects(Sc, sm.xres, sm.yres);
		g_inShadowPass = false;
		const auto tXformEnd = clk::now();
		if (sProfShadow) {
			sXformAcc += std::chrono::duration<double, std::milli>(tXformEnd - tXformStart).count();
		}

		const auto tRasterStart = clk::now();
		// Depth-only raster every visible face into sm.depth + sm.polyId.
		// Tile-parallel: split the shadow map into 6×4 tiles, dispatch
		// one job per tile to the engine threadpool. Each worker runs
		// its own FrustumClipper bounded to its tile rect — the clipper
		// trims each face down to the tile, and tiles never overlap, so
		// there's no contention on sm.depth/polyId between workers.
		// Same dispatch pattern as RenderInnerMekalele for the main pass.
		{
			std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
			renderns::tileCounter = 0;
		}
		const int numTilesX = 6;
		const int numTilesY = 4;
		const int totalTiles = numTilesX * numTilesY;
		const int tileSizeX = (sm.xres + numTilesX - 1) / numTilesX;
		const int tileSizeY = (sm.yres + numTilesY - 1) / numTilesY;
		ShadowMap * const smPtr = &sm;
		for (int ty = 0; ty < numTilesY; ++ty) {
			const float y1f = float(ty * tileSizeY);
			const float y2f = float(std::min((ty + 1) * tileSizeY, sm.yres));
			for (int tx = 0; tx < numTilesX; ++tx) {
				const float x1f = float(tx * tileSizeX);
				const float x2f = float(std::min((tx + 1) * tileSizeX, sm.xres));
				ThreadPool::instance().enqueue(
					[Sc, smPtr, x1f, y1f, x2f, y2f]() {
						g_currentShadowMap = smPtr;
						FrustumClipper clipper;
						clipper.InitViewport(Sc);
						clipper.SetClippingExtents(x1f, y1f, x2f, y2f);
						for (int i = 0; i < CAll; ++i) {
							Face *F = FList[i];
							if (!F) continue;
							if (!F->Txtr) continue;
							if (F->A == F->B) continue;
							if (F->A->TPos.z <= 0.0f &&
							    F->B->TPos.z <= 0.0f &&
							    F->C->TPos.z <= 0.0f) {
								continue;
							}
							clipper.Render(F, MekaleleShadowDepth, false,
							               /*skipMipLevel=*/true);
						}
						g_currentShadowMap = nullptr;
						std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
						++renderns::tileCounter;
						renderns::condition.notify_one();
					});
			}
		}
		{
			std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
			renderns::condition.wait(lock,
				[totalTiles]{ return renderns::tileCounter >= totalTiles; });
		}
		const auto tRasterEnd = clk::now();
		if (sProfShadow) {
			sRasterAcc += std::chrono::duration<double, std::milli>(tRasterEnd - tRasterStart).count();
			++sLightCount;
		}

		Sc->FZP = savedSceneFZP;
	}

	if (sProfShadow) {
		++sFrameIdx;
		// Print every 60 frames (~2s at 30 fps).
		if (sFrameIdx % 60 == 0) {
			const double xformAvg  = sXformAcc  / 60.0;
			const double rasterAvg = sRasterAcc / 60.0;
			const double perLightX = sLightCount ? sXformAcc / sLightCount : 0.0;
			const double perLightR = sLightCount ? sRasterAcc / sLightCount : 0.0;
			std::fprintf(stderr,
				"[SHADOW-PROF] last 60 frames: total/frame: xform=%.2fms raster=%.2fms "
				"sum=%.2fms  per-light: xform=%.2fms raster=%.2fms  "
				"(N=%d lights/frame)\n",
				xformAvg, rasterAvg, xformAvg + rasterAvg,
				perLightX, perLightR,
				int(sLightCount / 60));
			std::fflush(stderr);
			sXformAcc = 0.0;
			sRasterAcc = 0.0;
			sLightCount = 0;
		}
	}

	// Restore engine state. SetCurrentScene refreshes C_FZP / g_zscale
	// from the un-patched scene FZP so the main pass uses the right
	// depth-encoding constants again.
	View   = savedView;
	FOVX   = savedFOVX;
	FOVY   = savedFOVY;
	CntrEX = savedCntrEX;
	CntrEY = savedCntrEY;
	SetCurrentScene(Sc);

	// Each shadow pass overwrote per-vertex PX/PY/RZ + FList/CAll with
	// the light-camera projection. Re-run Transform_Objects with the
	// main camera so the subsequent Render() sees a consistent scene
	// state. Cost: one extra Transform_Objects per frame on scenes
	// that have any shadow-casting lights. Acceptable until we
	// rewrite Transform_Objects to write into a thread-local Vertex
	// scratch. Animate_Objects not re-run (see comment inside the
	// per-light loop above).
	Transform_Objects(Sc);

	// Precompute the per-shadow-map "view-space → light-view-space"
	// affine, with the main camera now restored on View. Derivation:
	//     pixelWorld     = mainView->Mat^T * pixelView + mainView->ISource
	//     pixelLightView = sm.lightViewMat * (pixelWorld - sm.lightISource)
	//   ⇒ pixelLightView = (sm.lightViewMat * mainView->Mat^T) * pixelView
	//                      + sm.lightViewMat * (mainView->ISource - sm.lightISource)
	// The kernel reads `viewToLight` (3×3) and `viewToLightOffset` (vec)
	// per pixel — no per-pixel world-space round-trip.
	for (ShadowMap& sm : g_shadowMaps) {
		if (!sm.omni) continue;
		// Build sm.lightViewMat * mainView->Mat^T. MatrixXMatrix(A, B, R)
		// gives R = A * B. Need the transpose of mainView->Mat first.
		Matrix mvT;
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				mvT[r][c] = View->Mat[c][r];
		MatrixXMatrix(sm.lightViewMat, mvT, sm.viewToLight);

		// Offset: sm.lightViewMat * (mainView->ISource - sm.lightISource).
		Vector d;
		Vector_Sub(&View->ISource, &sm.lightISource, &d);
		MatrixXVector(sm.lightViewMat, &d, &sm.viewToLightOffset);
	}

	// FDS_DUMP_SHADOWMAP=1: write each shadow map as a .pgm under
	// /tmp/shadowmap_<N>.pgm. One-shot per process — only dumps on
	// the first call.
	static bool dumpedShadowMap = false;
	if (!dumpedShadowMap && fds::FeatureFlags::dump_shadowmap()) {
		dumpedShadowMap = true;
		for (size_t i = 0; i < g_shadowMaps.size(); ++i) {
			const ShadowMap& sm = g_shadowMaps[i];
			char path[256];
			std::snprintf(path, sizeof(path), "/tmp/shadowmap_%zu.pgm", i);
			FILE *f = std::fopen(path, "wb");
			if (!f) continue;
			std::fprintf(f, "P5\n%d %d\n255\n", sm.xres, sm.yres);
			for (uint16_t e : sm.depth) {
				// e in [0..0xFFFF]; 0 = empty, 0xFFFF = closest. Scale
				// to 0..255 for the PGM viewer. Empty/background pixels
				// render as black.
				uint8_t b = uint8_t(e >> 8);
				std::fwrite(&b, 1, 1, f);
			}
			std::fclose(f);
			std::fprintf(stderr, "[SHADOWMAP] dumped %s\n", path);
		}
	}
}
