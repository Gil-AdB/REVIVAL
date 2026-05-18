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
#include "Base/VertexScratch.h"
#include "FRUSTRUM.H"
#include "Threads.h"

#include <mutex>
#include <condition_variable>
namespace renderns {
	extern std::mutex                tileCounterMutex;
	extern std::atomic<int>          tileCounter;
	extern std::condition_variable   condition;
}

// Set by the shadow orchestrator around each per-light Transform_Objects
// so the mesh-bsphere-vs-cone cull in Transform.cpp can read the active
// light's pose without an explicit parameter. nullptr outside the
// shadow pass.
thread_local Omni* g_currentShadowOmni = nullptr;

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

	// Per-light scratch: one FaceListContext + VertexScratch + Camera +
	// CameraContext per shadow-casting light. Kept across frames so the
	// per-vertex clone arrays + face buffers stay warm —
	// Transform_Objects rewrites the projected fields in place each
	// frame instead of reallocating. Sizing once per frame (in case
	// the shadow-map set grew).
	static thread_local std::vector<fds::FaceListContext> perLightFaces;
	static thread_local std::vector<fds::VertexScratch>   perLightScratch;
	static thread_local std::vector<Camera>               perLightCam;
	static thread_local std::vector<fds::CameraContext>   perLightCtx;
	if (perLightFaces.size() != g_shadowMaps.size()) {
		perLightFaces.resize(g_shadowMaps.size());
		perLightScratch.resize(g_shadowMaps.size());
		perLightCam.resize(g_shadowMaps.size());
		perLightCtx.resize(g_shadowMaps.size());
	}

	// ─── Phase A: per-light setup + parallel Transform_Objects ──────────
	// Per-light camera, CameraContext, shadow-map stash, depth/polyId
	// clear, FList sizing — all cheap, all touch this light's slot only.
	// Then enqueue Transform_Objects as one threadpool task per light;
	// each task projects the scene into its own FaceListContext +
	// VertexScratch + sets its own thread_local g_inShadowPass /
	// g_currentShadowOmni. No shared writes between tasks.
	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::tileCounter = 0;
	}
	int xformsEnqueued = 0;
	const auto tXformStart = clk::now();
	for (size_t lightIdx = 0; lightIdx < g_shadowMaps.size(); ++lightIdx) {
		ShadowMap& sm = g_shadowMaps[lightIdx];
		Omni *const O = sm.omni;
		if (!O) continue;
		if (!(O->Flags & Omni_Active)) continue;
		// Accept Light_SpotLight as before, plus Light_Omni entries that
		// are CubeShadowRef faces (sm.cubeFace >= 0). Plain Light_Omni
		// without cubeFace tagged (=-1) is skipped — those omnis didn't
		// opt into cube shadows.
		if (O->Type == Light_SpotLight) {
			// spot path falls through unchanged
		} else if (O->Type == Light_Omni && sm.cubeFace >= 0) {
			// cube-face path handled below
		} else {
			continue;
		}

		// Build the per-light camera. Spot path: existing IDir-based
		// lookAt + cone-FOV. Cube-face path: fixed axis-aligned camera
		// looking along ±X/±Y/±Z, 90° FOV (each face covers one
		// hemisphere of the omni's volume).
		Camera& lightCam = perLightCam[lightIdx];
		lightCam = Camera{};
		float perspXY;
		if (sm.cubeFace < 0) {
			// Spot path. Look from O->IPos toward IPos+IDir, world-up=Y.
			// FOV = 2 * acos(cosOuter) so the outer cone fits in the
			// shadow map, padded so silhouettes near the cone edge get
			// a few pixels of context.
			// Kick_Camera has a singularity at IDir=(0,±1,0) — nudge
			// by a tiny X bias if the caller handed us a perfectly-
			// vertical IDir.
			Vector idir = O->IDir;
			if (std::fabs(idir.x) < 1e-4f && std::fabs(idir.z) < 1e-4f) {
				idir.x = 0.01f;
			}
			Vector targ{ O->IPos.x + idir.x,
			             O->IPos.y + idir.y,
			             O->IPos.z + idir.z };
			Kick_Camera(&O->IPos, &targ, 0.0f, lightCam.Mat);
			const float cosOuter   = std::max(0.01f, O->FallOff);
			const float fovHalfRad = std::acos(cosOuter) * 1.10f;
			perspXY = (float(sm.xres) * 0.5f) / std::tan(fovHalfRad);
			lightCam.IFOV = (fovHalfRad * 2.0f) * (180.0f / PI);
		} else {
			// Cube-face path. Face order: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
			// Build a look-target one unit along the chosen axis from
			// the omni. World-up for ±Y faces flips to +Z to avoid the
			// Kick_Camera singularity at look ‖ world-up.
			Vector tDir{0,0,0};
			switch (sm.cubeFace) {
				case 0: tDir.x =  1.0f; break;
				case 1: tDir.x = -1.0f; break;
				case 2: tDir.y =  1.0f; break;
				case 3: tDir.y = -1.0f; break;
				case 4: tDir.z =  1.0f; break;
				case 5: tDir.z = -1.0f; break;
			}
			Vector targ{ O->IPos.x + tDir.x,
			             O->IPos.y + tDir.y,
			             O->IPos.z + tDir.z };
			// ±Y faces would hit Kick_Camera's vertical singularity;
			// nudge target slightly horizontally so the basis builds OK.
			if (sm.cubeFace == 2 || sm.cubeFace == 3) {
				targ.x += 0.01f;
			}
			Kick_Camera(&O->IPos, &targ, 0.0f, lightCam.Mat);
			// 90° FOV per face → fovHalfRad = PI/4 → tan = 1. With
			// 10% pad to keep silhouettes a few pixels inside the
			// next face's domain.
			const float fovHalfRad = float(PI) * 0.25f * 1.10f;
			perspXY = (float(sm.xres) * 0.5f) / std::tan(fovHalfRad);
			lightCam.IFOV = 90.0f;
		}
		lightCam.ISource = O->IPos;
		lightCam.PerspX = perspXY;
		lightCam.PerspY = perspXY;

		// Stash the light camera's pose onto the ShadowMap so the
		// lighting kernel can sample later without rebuilding it.
		Matrix_Copy(sm.lightViewMat, lightCam.Mat);
		sm.lightISource = O->IPos;
		sm.perspX = perspXY;
		sm.perspY = perspXY;
		sm.cntrX  = float(sm.xres) * 0.5f - 0.5f;
		sm.cntrY  = float(sm.yres) * 0.5f - 0.5f;
		sm.zScale = float(0xFF00) / (sm.fzp * 1.1f);

		// CameraContext into perLightCtx[lightIdx] — durable across the
		// barrier so the phase-B tile workers can capture a pointer.
		fds::CameraContext& lightCtx = perLightCtx[lightIdx];
		lightCtx            = fds::CameraContext{};
		lightCtx.view       = &lightCam;
		lightCtx.fovX       = perspXY;
		lightCtx.fovY       = perspXY;
		lightCtx.cntrX      = int32_t(sm.cntrX);
		lightCtx.cntrY      = int32_t(sm.cntrY);
		lightCtx.cntrEX     = sm.cntrX;
		lightCtx.cntrEY     = sm.cntrY;
		lightCtx.nearZ      = Sc->NZP;
		lightCtx.invNearZ   = 1.0f / Sc->NZP;
		lightCtx.farZ       = sm.fzp;
		lightCtx.invFarZ    = 1.0f / sm.fzp;
		lightCtx.zScale     = sm.zScale;
		lightCtx.zScale256  = sm.zScale / 256.0f;

		// Clear shadow map.
		std::fill(sm.depth.begin(),  sm.depth.end(),  uint16_t(0));
		std::fill(sm.polyId.begin(), sm.polyId.end(), uint8_t(0));

		// Size this light's FList to match the main-pass capacity. Polys
		// is the worst case (every mesh face + every omni + every
		// particle); shadow geometry is a subset. resize() is a no-op
		// when already sized.
		perLightFaces[lightIdx].resize(static_cast<size_t>(Polys));

		// Enqueue Transform_Objects for this light. Skips Animate_Objects
		// (scene driver already ticked this frame's object poses; re-
		// running would double-tick splines/particles and diverge the
		// main pass).
		Scene *const                 ScPtr      = Sc;
		ShadowMap *const             smPtr      = &sm;
		fds::CameraContext *const    ctxPtr     = &lightCtx;
		fds::FaceListContext *const  facesPtr   = &perLightFaces[lightIdx];
		fds::VertexScratch *const    scratchPtr = &perLightScratch[lightIdx];
		++xformsEnqueued;
		ThreadPool::instance().enqueue(
			[ScPtr, smPtr, ctxPtr, facesPtr, scratchPtr]() {
				g_inShadowPass = true;
				g_currentShadowOmni = smPtr->omni;
				Transform_Objects(ScPtr, *ctxPtr, *facesPtr,
				                  smPtr->xres, smPtr->yres, scratchPtr);
				g_currentShadowOmni = nullptr;
				g_inShadowPass = false;
				std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
				++renderns::tileCounter;
				renderns::condition.notify_one();
			});
	}
	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::condition.wait(lock,
			[xformsEnqueued]{ return renderns::tileCounter >= xformsEnqueued; });
	}
	const auto tXformEnd = clk::now();
	if (sProfShadow) {
		sXformAcc += std::chrono::duration<double, std::milli>(tXformEnd - tXformStart).count();
		sLightCount += xformsEnqueued;
	}

	// ─── Phase B: flat (light × tile) tile rasterization ────────────────
	// All active lights' 6×4 tiles dispatch into a single threadpool
	// batch — the pool load-balances across lights, so a stragglering
	// light's tile no longer holds up the next light's transforms (those
	// already happened in phase A) or its tile work. Single barrier
	// covers all N×24 tiles.
	constexpr int numTilesX = 6;
	constexpr int numTilesY = 4;
	const auto tRasterStart = clk::now();
	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::tileCounter = 0;
	}
	int tilesEnqueued = 0;
	for (size_t lightIdx = 0; lightIdx < g_shadowMaps.size(); ++lightIdx) {
		ShadowMap& sm = g_shadowMaps[lightIdx];
		Omni *const O = sm.omni;
		if (!O) continue;
		if (!(O->Flags & Omni_Active)) continue;
		// Same filter as Phase A: spots, and cube-face entries.
		if (O->Type == Light_SpotLight) {
			// ok
		} else if (O->Type == Light_Omni && sm.cubeFace >= 0) {
			// ok — cube face entry
		} else {
			continue;
		}

		const int tileSizeX = (sm.xres + numTilesX - 1) / numTilesX;
		const int tileSizeY = (sm.yres + numTilesY - 1) / numTilesY;
		ShadowMap *const                   smPtr     = &sm;
		const fds::CameraContext *const    camPtr    = &perLightCtx[lightIdx];
		const fds::FaceListContext *const  facesPtr  = &perLightFaces[lightIdx];
		for (int ty = 0; ty < numTilesY; ++ty) {
			const float y1f = float(ty * tileSizeY);
			const float y2f = float(std::min((ty + 1) * tileSizeY, sm.yres));
			for (int tx = 0; tx < numTilesX; ++tx) {
				const float x1f = float(tx * tileSizeX);
				const float x2f = float(std::min((tx + 1) * tileSizeX, sm.xres));
				++tilesEnqueued;
				ThreadPool::instance().enqueue(
					[smPtr, camPtr, facesPtr, x1f, y1f, x2f, y2f]() {
						g_currentShadowMap = smPtr;
						const auto rt  = fds::MainRenderTargetFromGlobals();
						const auto& cam = *camPtr;
						FrustumClipper clipper;
						clipper.InitViewport(*camPtr);
						clipper.SetClippingExtents(x1f, y1f, x2f, y2f);
						const auto& f = *facesPtr;
						for (int i = 0; i < f.cAll; ++i) {
							Face *const F = f.fList[i].face;
							if (!F) continue;
							if (!F->Txtr) continue;
							if (F->A == F->B) continue;
							if (F->A->TPos.z <= 0.0f &&
							    F->B->TPos.z <= 0.0f &&
							    F->C->TPos.z <= 0.0f) {
								continue;
							}
							clipper.Render(F, MekaleleShadowDepth, false, rt, cam,
                                          /*skipMipLevel=*/true);
						}
						g_currentShadowMap = nullptr;
						std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
						++renderns::tileCounter;
						renderns::condition.notify_one();
					});
			}
		}
	}
	{
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::condition.wait(lock,
			[tilesEnqueued]{ return renderns::tileCounter >= tilesEnqueued; });
	}
	const auto tRasterEnd = clk::now();
	if (sProfShadow) {
		sRasterAcc += std::chrono::duration<double, std::milli>(tRasterEnd - tRasterStart).count();
	}

	if (sProfShadow) {
		++sFrameIdx;
		// Print interval — env override SHADOW_PROF_INTERVAL=N (default 60
		// for live runs to keep stderr quiet; snapshot harness uses N=1
		// since each invocation only renders a handful of frames).
		static const int sInterval = []() {
			const char *e = std::getenv("FDS_SHADOW_PROF_INTERVAL");
			return (e && *e) ? std::max(1, std::atoi(e)) : 60;
		}();
		if (sFrameIdx % sInterval == 0) {
			const double xformAvg  = sXformAcc  / sInterval;
			const double rasterAvg = sRasterAcc / sInterval;
			const double perLightX = sLightCount ? sXformAcc / sLightCount : 0.0;
			const double perLightR = sLightCount ? sRasterAcc / sLightCount : 0.0;
			std::fprintf(stderr,
				"[SHADOW-PROF] last %d frame(s): total/frame: xform=%.2fms raster=%.2fms "
				"sum=%.2fms  per-light: xform=%.2fms raster=%.2fms  "
				"(N=%d lights/frame)\n",
				sInterval, xformAvg, rasterAvg, xformAvg + rasterAvg,
				perLightX, perLightR,
				int(sLightCount / sInterval));
			std::fflush(stderr);
			sXformAcc = 0.0;
			sRasterAcc = 0.0;
			sLightCount = 0;
		}
	}

	// (Previously: re-ran Transform_Objects with the main camera here
	// to restore fds::g_mainFaces + per-vertex PX/PY/RZ that the
	// shadow loop had clobbered. Phase 6 made the shadow loop write
	// into per-light VertexScratch clones instead, so T->Verts /
	// T->Faces / fds::g_mainFaces are untouched and the restore is
	// dead. Saves one full main-camera Transform_Objects per frame.)

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
