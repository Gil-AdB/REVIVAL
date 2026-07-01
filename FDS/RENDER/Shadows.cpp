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
#include <thread>
#include <vector>
#include <array>
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
#include "Base/VertexFrame.h"  // SoA Phase 4: F->frame access in backface skip
#include "Base/Material.h"
#include "FILLERS/ShadowMap.h"
#include "TailProf.h"     // phase-1 barrier instrumentation (FDS_TAIL_PROF)
#include "FILLERS/Mekalele.h"
#include "Base/VertexScratch.h"
#include "FRUSTRUM.H"
#include "Threads.h"

#include <mutex>
#include <condition_variable>
#include <semaphore>
#include <climits>
namespace renderns {
	extern std::counting_semaphore<INT_MAX> tileDone;
	extern std::counting_semaphore<INT_MAX> shadowDone;
	extern std::mutex                tileCounterMutex;
	extern std::atomic<int>          tileCounter;
	extern std::condition_variable   condition;
}

// Set by the shadow orchestrator around each per-light Transform_Objects
// so the mesh-bsphere-vs-cone cull in Transform.cpp can read the active
// light's pose without an explicit parameter. nullptr outside the
// shadow pass.
thread_local Omni* g_currentShadowOmni = nullptr;

// True only inside Render_DeferredShadowMaps_Dynamic's per-frame bake.
// Inverts the Transform_Objects mesh filter (keep animated, skip static)
// and routes MekaleleShadowDepth writes to sm.depth_dynamic / polyId_dynamic
// instead of the static buffers.
thread_local bool g_inDynamicShadowBake = false;

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


// [experiment: --shadow-swizzle] Re-tile one map's freshly-baked planes into
// the 8×8-tiled *Sw copies (see ShadowSwzOffset). dynPlanes selects which pair
// this bake wrote: static (depth/polyId — StaticOnce + DynOmnis) or dynamic
// (depth_dynamic/polyId_dynamic — DynMeshes). Linear planes stay the source
// of truth; this is a derived copy. Row-of-tile copies are 8×u16 = 16 B →
// one 128-bit load/store each; the pass is pure memory traffic, which is
// exactly the cost the experiment wants to weigh against the PCF read gain.
static void ShadowMap_SwizzlePlanes(ShadowMap &sm, bool dynPlanes)
{
	const ShadowSwzShape &shp = ShadowSwzGetShape();
	const int tw = 1 << shp.a, th = 1 << shp.b;
	const int tileSz = tw * th;
	const int tpr = ShadowSwzTilesPerRow(sm.xres, shp);
	const int trs = (sm.yres + shp.maskY) >> shp.b;
	const size_t n = size_t(tpr) * size_t(trs) * size_t(tileSz);
	auto tile = [&](const std::vector<uint16_t> &src, std::vector<uint16_t> &dst) {
		if (src.empty()) { dst.clear(); return; }
		if (dst.size() != n) dst.assign(n, 0);   // zero pad = "unwritten" sentinel
		const uint16_t *s = src.data();
		uint16_t *d = dst.data();
		for (int y = 0; y < sm.yres; ++y) {
			const uint16_t *srow = s + size_t(y) * size_t(sm.xres);
			// row-of-tile runs: tw consecutive texels per tile share y.
			uint16_t *drow = d + (size_t(y >> shp.b) * size_t(tpr)) * size_t(tileSz)
			                   + (size_t(y & shp.maskY) << shp.a);
			int x = 0;
			for (int t = 0; t < tpr; ++t, x += tw)
				std::memcpy(drow + size_t(t) * size_t(tileSz), srow + x,
				            size_t(std::min(tw, sm.xres - x)) * sizeof(uint16_t));
		}
	};
	if (dynPlanes) { tile(sm.depth_dynamic, sm.depthDynSw); tile(sm.polyId_dynamic, sm.polyIdDynSw); }
	else           { tile(sm.depth,         sm.depthSw);    tile(sm.polyId,         sm.polyIdSw);    }
}

void Render_DeferredShadowMaps(Scene *Sc, ShadowBakeMode mode)
{
	if (!shadowsEnabled()) return;
	if (!Sc || g_shadowMaps.empty()) return;

	// --shadow_bake_time: total wall-clock of the DYNAMIC bake stage (this whole
	// call up to the end of the raster phase). Excludes the init StaticOnce bake
	// — that runs once on a worker thread and isn't the per-frame cost we care
	// about. Coarse single number, complementary to the xform/raster split that
	// --shadow_prof reports. Measured here at entry; reported just after the
	// raster phase ends (tRasterEnd), averaged over FDS_SHADOW_PROF_INTERVAL.
	const bool sBakeTime = fds::FeatureFlags::shadow_bake_time()
	                       && (mode != ShadowBakeMode::StaticOnce);
	const auto tBakeStart = std::chrono::high_resolution_clock::now();

	// Refresh each CubeShadowRef's lightISource from the omni's current
	// IPos. Set once at CubeShadowMaps_Rebuild time; for FLD-animated
	// omnis (e.g. greets robot-following lights) the omni's IPos drifts
	// every frame via Animate_Objects but the ref's copy stayed at t=0,
	// which CubeShadow_Sample uses for cube-face selection. Cheap (one
	// vec copy per omni) and safe (sm.lightISource is also refreshed
	// at line 239 below for the same reason).
	for (auto &cr : g_cubeShadowRefs) {
		if (cr.omni) cr.lightISource = cr.omni->IPos;
	}
	// Convenience predicates over the mode. Three behaviors fold
	// cleanly onto two bools (omni-target × buffer-target):
	//   StaticOnce            wantStatic=true  writeDynamic=false
	//   DynamicOmnisPerFrame  wantStatic=false writeDynamic=false
	//   DynamicMeshesPerFrame wantStatic=true  writeDynamic=true   ← new
	const bool wantStaticOmnis = (mode == ShadowBakeMode::StaticOnce)
	                          || (mode == ShadowBakeMode::DynamicMeshesPerFrame);
	const bool writeDynamicBuf = (mode == ShadowBakeMode::DynamicMeshesPerFrame);
	// One-shot entry log per mode — confirms the dispatch is firing
	// at all. Capped per mode so we don't spam per-frame.
	if (mode == ShadowBakeMode::DynamicMeshesPerFrame) {
		static std::atomic<int> sLogged{0};
		if (sLogged.fetch_add(1) < 4) {
			std::fprintf(stderr,
			    "[SHADOW-PASS] mode=DynamicMeshesPerFrame  "
			    "g_shadowMaps.size=%zu\n", g_shadowMaps.size());
		}
	}

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

	// Per-omni cube-face cull for DynamicMeshesPerFrame mode. The robot
	// (or other dynamic mesh) occupies a small angular region of each
	// static omni's cube — typically only 1-2 of the 6 cube faces have
	// any dynamic mesh visible. Pre-compute a per-shadow-map "any dyn
	// mesh visible" bit; skip phase A + phase B for faces where it's
	// false. Saves ~3 ms on greets (7 static omnis × ~4 culled faces =
	// 28 skipped per-face transforms + raster batches).
	//
	// Only the DynamicMeshesPerFrame path benefits: StaticOnce baked at
	// init (one-shot) and DynamicOmnisPerFrame re-bakes ALL geometry
	// for moving omnis (mech), where there's no "dyn only" subset.
	static thread_local std::vector<bool> hasDynMeshVisible;
	if (mode == ShadowBakeMode::DynamicMeshesPerFrame) {
		hasDynMeshVisible.assign(g_shadowMaps.size(), false);
		// Gather dynamic meshes' world-space bsphere centers + radii.
		// "Dynamic" mirrors Transform_Objects's isDynamicForBake (walks
		// parent chain looking for non-trivial Pos / Rotate spline
		// extents). Cheap — single pass over ObjectHead with O(few)
		// meshes per spline.
		struct DynBSphere { Vector center; float radius; };
		std::vector<DynBSphere> dynMeshes;
		auto isMeshDynamic = [](Object *o) -> bool {
			constexpr float kPosEps = 0.1f, kRotEps = 0.01f;
			for (Object *p = o; p; p = p->Parent) {
				if (p->Type != Obj_TriMesh) continue;
				TriMesh *tm = (TriMesh*)p->Data;
				if (!tm) continue;
				if (tm->Pos.NumKeys > 1 && tm->Pos.Keys) {
					const auto& k0 = tm->Pos.Keys[0].Pos;
					float xmn=k0.x,xmx=k0.x,ymn=k0.y,ymx=k0.y,zmn=k0.z,zmx=k0.z;
					for (DWord i=1;i<tm->Pos.NumKeys;++i) {
						const auto& k = tm->Pos.Keys[i].Pos;
						if (k.x<xmn) xmn=k.x; if (k.x>xmx) xmx=k.x;
						if (k.y<ymn) ymn=k.y; if (k.y>ymx) ymx=k.y;
						if (k.z<zmn) zmn=k.z; if (k.z>zmx) zmx=k.z;
					}
					if ((xmx-xmn)>kPosEps||(ymx-ymn)>kPosEps||(zmx-zmn)>kPosEps) return true;
				}
				if (tm->Rotate.NumKeys > 1 && tm->Rotate.Keys) {
					const auto& q0 = tm->Rotate.Keys[0].Pos;
					for (DWord i=1;i<tm->Rotate.NumKeys;++i) {
						const auto& q = tm->Rotate.Keys[i].Pos;
						const float dx=q.x-q0.x, dy=q.y-q0.y, dz=q.z-q0.z, dw=q.W-q0.W;
						if (dx*dx+dy*dy+dz*dz+dw*dw > kRotEps*kRotEps) return true;
					}
				}
			}
			return false;
		};
		for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next) {
			if (Obj->Type != Obj_TriMesh) continue;
			if (!isMeshDynamic(Obj)) continue;
			TriMesh *T = (TriMesh*)Obj->Data;
			if (!T || T->FIndex == 0) continue;
			Vector wc;
			MatrixXVector(T->RotMat, &T->BSphereCtr, &wc);
			Vector_SelfAdd(&wc, &T->IPos);
			dynMeshes.push_back({wc, T->BSphereRadius});
		}
		// Per shadow map: is any dyn mesh visible from this cube face?
		// Specialized sphere-vs-cone for the 90°-pyramid's CIRCUMSCRIBED
		// cone (half-angle 54.7°, cos = 0.577). All trig derived from
		// cosOuter is constexpr. The perpendicular-vs-cone-radius test
		// compares squared values to avoid the extra sqrt. Only sqrt-free
		// transcendental work in the entire test.
		constexpr float kCosOuter = 0.5773502691896258f;  // 1/√3 = cos(atan(√2))
		constexpr float kTanOuter = 1.4142135623730951f;  // √2 = tan(atan(√2))
		constexpr float kInvCos   = 1.7320508075688772f;  // √3 = 1/kCosOuter
		auto sphereOutsideCone = [&](const Vector& C, float r,
		                              const Vector& P, const Vector& D,
		                              float maxRange) -> bool {
			const float vx = C.x - P.x, vy = C.y - P.y, vz = C.z - P.z;
			const float v2 = vx*vx + vy*vy + vz*vz;
			const float rMax = maxRange + r;
			if (v2 > rMax * rMax) return true;                       // squared
			const float distAlongAxis = vx*D.x + vy*D.y + vz*D.z;
			if (distAlongAxis < -r) return true;
			const float depth = distAlongAxis > 0.0f ? distAlongAxis : 0.0f;
			const float coneRAtDepth = depth * kTanOuter + r * kInvCos;
			const float perpSq = v2 - distAlongAxis * distAlongAxis;
			return perpSq > coneRAtDepth * coneRAtDepth;             // squared
		};
		for (size_t i = 0; i < g_shadowMaps.size(); ++i) {
			const ShadowMap &sm = g_shadowMaps[i];
			Omni *const O = sm.omni;
			if (!O) continue;
			if (!(O->Flags & Omni_StaticShadow)) continue;  // mode filter
			if (sm.cubeFace < 0) continue;  // spot lights handled separately
			Vector faceDir{0,0,0};
			switch (sm.cubeFace) {
				case 0: faceDir.x =  1.0f; break;
				case 1: faceDir.x = -1.0f; break;
				case 2: faceDir.y =  1.0f; break;
				case 3: faceDir.y = -1.0f; break;
				case 4: faceDir.z =  1.0f; break;
				case 5: faceDir.z = -1.0f; break;
			}
			for (const auto& dm : dynMeshes) {
				if (!sphereOutsideCone(dm.center, dm.radius,
				                        O->IPos, faceDir, O->IRange)) {
					hasDynMeshVisible[i] = true;
					break;
				}
			}
		}
	}

	// ─── Phase A: per-light setup + parallel Transform_Objects ──────────
	// Per-light camera, CameraContext, shadow-map stash, depth/polyId
	// clear, FList sizing — all cheap, all touch this light's slot only.
	// Then enqueue Transform_Objects as one threadpool task per light;
	// each task projects the scene into its own FaceListContext +
	// VertexScratch + sets its own thread_local g_inShadowPass /
	// g_currentShadowOmni. No shared writes between tasks.
	//
	// The Phase-A/B tasks pre-assign their tiles and never fetch_add on
	// tileCounter, so this reset is vestigial for the bake — it only kept
	// the shared counter clean for later passes (which reset it themselves
	// before use). When overlapping with the gbuffer fill we must NOT touch
	// the shared counter from this off-thread bake; skip it. (Stage A.)
	if (!fds::FeatureFlags::shadow_gbuffer_overlap()) {
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
		// Mode-driven filter:
		//   StaticOnce / DynamicMeshesPerFrame → only static omnis
		//   DynamicOmnisPerFrame               → only dynamic omnis
		const bool isStatic = (O->Flags & Omni_StaticShadow) != 0;
		if (isStatic != wantStaticOmnis) continue;
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
		// DynamicMeshesPerFrame cube-face cull: skip faces where no
		// dynamic mesh would be visible. Pre-computed above.
		if (mode == ShadowBakeMode::DynamicMeshesPerFrame
		    && sm.cubeFace >= 0
		    && !hasDynMeshVisible[lightIdx]) continue;

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

		// Clear the buffer we're about to write into.
		if (writeDynamicBuf) {
			std::fill(sm.depth_dynamic.begin(),  sm.depth_dynamic.end(),  uint16_t(0));
			std::fill(sm.polyId_dynamic.begin(), sm.polyId_dynamic.end(), uint8_t(0));
		} else {
			std::fill(sm.depth.begin(),  sm.depth.end(),  uint16_t(0));
			std::fill(sm.polyId.begin(), sm.polyId.end(), uint8_t(0));
		}

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
		const bool dynBakeForLambda = writeDynamicBuf;
		ThreadPool::instance().enqueue(
			[ScPtr, smPtr, ctxPtr, facesPtr, scratchPtr, dynBakeForLambda]() {
				g_inShadowPass = true;
				g_inDynamicShadowBake = dynBakeForLambda;
				g_currentShadowOmni = smPtr->omni;
				// Set the active shadow map too — Transform_Objects's
				// per-cube-face bsphere cull needs sm->cubeFace to pick
				// the face axis. (raster lambda already sets this; mirror
				// it here so the xform task has the same context.)
				g_currentShadowMap = smPtr;
				Transform_Objects(ScPtr, *ctxPtr, *facesPtr,
				                  smPtr->xres, smPtr->yres, scratchPtr);
				g_currentShadowMap = nullptr;
				g_currentShadowOmni = nullptr;
				g_inDynamicShadowBake = false;
				g_inShadowPass = false;
				// One permit per completed task (see renderns::shadowDone).
				renderns::shadowDone.release();
			});
	}
	for (int _i = 0; _i < xformsEnqueued; ++_i) {
		renderns::shadowDone.acquire();
	}
	const auto tXformEnd = clk::now();
	if (sProfShadow) {
		sXformAcc += std::chrono::duration<double, std::milli>(tXformEnd - tXformStart).count();
		sLightCount += xformsEnqueued;
	}

	// ─── Phase B: flat (light × tile) tile rasterization ────────────────
	// All active lights' tiles dispatch into a single threadpool batch —
	// the pool load-balances across lights, so a straggler light's tile
	// no longer holds up the next light's transforms or its tile work.
	// Single barrier covers all N × numTilesX*numTilesY tiles.
	//
	// 4×4 chosen so that for a 512² shadow map, tileSizeX = tileSizeY =
	// 128 — both multiples of the rasterizer's 8×8 SIMD tile so adjacent
	// clipper tile workers never share a 16-byte aligned word in
	// apply_exact's blendv RMW. Was 6×4 → 86×128 → race at every column
	// seam, flicker patches in the shadow viz even with input paused
	// (TSan-confirmed).
	constexpr int numTilesX = 4;
	constexpr int numTilesY = 4;
	const auto tRasterStart = clk::now();
	// Vestigial for the bake (Phase-B tiles are pre-assigned); skip it when
	// overlapping so the off-thread bake doesn't touch the shared counter.
	if (!fds::FeatureFlags::shadow_gbuffer_overlap()) {
		std::unique_lock<std::mutex> lock(renderns::tileCounterMutex);
		renderns::tileCounter = 0;
	}
	int tilesEnqueued = 0;
	// [experiment: --shadow-swizzle] maps this bake writes → re-tiled after
	// the raster drain (see below). Filled by the same Phase-B filter.
	const bool sSwz = fds::FeatureFlags::shadow_swizzle();
	std::vector<size_t> swzMaps;
	for (size_t lightIdx = 0; lightIdx < g_shadowMaps.size(); ++lightIdx) {
		ShadowMap& sm = g_shadowMaps[lightIdx];
		Omni *const O = sm.omni;
		if (!O) continue;
		if (!(O->Flags & Omni_Active)) continue;
		// Same mode-driven omni filter as Phase A.
		const bool isStaticB = (O->Flags & Omni_StaticShadow) != 0;
		if (isStaticB != wantStaticOmnis) continue;
		// Same filter as Phase A: spots, and cube-face entries.
		if (O->Type == Light_SpotLight) {
			// ok
		} else if (O->Type == Light_Omni && sm.cubeFace >= 0) {
			// ok — cube face entry
		} else {
			continue;
		}
		// Same cube-face cull as Phase A. Must match — otherwise we
		// enqueue tile workers for a face we skipped in phase A, and
		// they'd read uninitialized perLightFaces / perLightCtx state.
		if (mode == ShadowBakeMode::DynamicMeshesPerFrame
		    && sm.cubeFace >= 0
		    && !hasDynMeshVisible[lightIdx]) continue;

		// Tile size must be a multiple of 8 (see numTilesX comment for
		// why). At shadow res = 4*N*8 (e.g. 128, 256, 512, 1024) this is
		// naturally clean. For arbitrary res, round down to mult-of-8 and
		// let the last tile absorb the remainder — matching the main
		// renderFrame tiler (see RENDER.CPP). The `(ty == numTilesY - 1)`
		// branch is what actually makes the remainder strip render;
		// without it, the last column / row of `sm.xres/yres` would stay
		// at the pre-clear value.
		const int rawTX = (sm.xres + numTilesX - 1) / numTilesX;
		const int rawTY = (sm.yres + numTilesY - 1) / numTilesY;
		const int tileSizeX = rawTX & ~7;
		const int tileSizeY = rawTY & ~7;
		if (sSwz) swzMaps.push_back(lightIdx);
		ShadowMap *const                   smPtr     = &sm;
		const fds::CameraContext *const    camPtr    = &perLightCtx[lightIdx];
		const fds::FaceListContext *const  facesPtr  = &perLightFaces[lightIdx];
		for (int ty = 0; ty < numTilesY; ++ty) {
			const float y1f = float(ty * tileSizeY);
			const float y2f = float((ty == numTilesY - 1) ? sm.yres : ((ty + 1) * tileSizeY));
			for (int tx = 0; tx < numTilesX; ++tx) {
				const float x1f = float(tx * tileSizeX);
				const float x2f = float((tx == numTilesX - 1) ? sm.xres : ((tx + 1) * tileSizeX));
				++tilesEnqueued;
				const bool dynBakeForLambda = writeDynamicBuf;
				ThreadPool::instance().enqueue(
					[smPtr, camPtr, facesPtr, x1f, y1f, x2f, y2f, dynBakeForLambda]() {
						const long long _tp = TailProf::nowNs();
						g_currentShadowMap = smPtr;
						g_inDynamicShadowBake = dynBakeForLambda;
						const auto rt  = fds::MainRenderTargetFromGlobals();
						const auto& cam = *camPtr;
						FrustumClipper clipper;
						clipper.InitViewport(*camPtr);
						clipper.SetClippingExtents(x1f, y1f, x2f, y2f);
						const auto& f = *facesPtr;
						// Per-Material "skip in shadow bake" cache. The FLD
						// doesn't tag lamps / emitters with any flag bit,
						// so we infer from material name (lowercase ASCII
						// substring match) on first encounter and reuse
						// thereafter. Bounded LRU is overkill — scenes
						// have O(50) distinct materials; a flat 256-entry
						// hash-keyed-by-pointer is plenty.
						struct MatShadowCache {
							// Pack (material pointer | skip-bit) into ONE atomic
							// so the pointer-match and the skip flag are read/
							// written together, atomically. Two separate atomics
							// (mat[k] + skip[k]) tore under thread contention: a
							// reader could match mat[k] yet read a STALE skip[k]
							// left by a different material that previously
							// occupied slot k (pointers collide mod 256). That
							// gave a load-dependent wrong skip decision, which
							// dropped/added shadow polygons frame-to-frame — THE
							// shadow tile flicker. TSan never caught it because
							// data races on atomics are not reported. Material is
							// ≥2-aligned, so bit 0 of the pointer is free for the
							// flag.
							std::atomic<uintptr_t> entry[256] = {};
						};
						static MatShadowCache sCache;
						auto looksEmissive = [](const char *n) -> bool {
							if (!n) return false;
							for (const char *p = n; *p; ++p) {
								if ((p[0]=='l'||p[0]=='L') && (p[1]=='a'||p[1]=='A') &&
								    (p[2]=='m'||p[2]=='M') && (p[3]=='p'||p[3]=='P')) return true;
								if ((p[0]=='e'||p[0]=='E') && (p[1]=='m'||p[1]=='M') &&
								    (p[2]=='i'||p[2]=='I')) return true;  // emit/emiter/emitter
							}
							return false;
						};
						auto shouldSkip = [&](Material *m) -> bool {
							if (!m) return true;
							const uintptr_t k = (uintptr_t(m) >> 4) & 255;
							const uintptr_t mbits = uintptr_t(m);  // bit0 = 0 (aligned)
							const uintptr_t e = sCache.entry[k].load(std::memory_order_relaxed);
							if ((e & ~uintptr_t(1)) == mbits) return (e & 1) != 0;
							const bool skip = (m->Flags & (Mat_Transparent | Mat_Additive | Mat_SkipZ))
							                 || looksEmissive(m->Name);
							sCache.entry[k].store(mbits | (skip ? uintptr_t(1) : uintptr_t(0)),
							                      std::memory_order_relaxed);
							return skip;
						};
						int kept = 0, skXpar = 0, skDegen = 0, skBack = 0, skNoTxtr = 0;
						// Material flag census — one-shot dump of (Name,
						// Flags) for the first 64 distinct material
						// addresses we see during any static bake. Used
						// to chase "lamps still cast shadows": shows what
						// the lamp-material bits actually look like, in
						// case we need to widen the skip mask beyond
						// Transparent/Additive/SkipZ.
						{
							static std::atomic<int> sLogged{0};
							static std::atomic<Material*> sSeen[64] = {};
							for (int i = 0; i < f.cAll && sLogged.load() < 64; ++i) {
								Face *const F = f.fList[i].face;
								if (!F || !F->Txtr) continue;
								bool already = false;
								const int seenN = sLogged.load();
								for (int k = 0; k < seenN; ++k) {
									if (sSeen[k].load() == F->Txtr) { already = true; break; }
								}
								if (already) continue;
								const int idx = sLogged.fetch_add(1);
								if (idx < 64) {
									sSeen[idx].store(F->Txtr);
									std::fprintf(stderr,
									    "[SHADOW-MAT] '%s' flags=0x%08x\n",
									    (F->Txtr->Name ? F->Txtr->Name : "?"),
									    unsigned(F->Txtr->Flags));
								}
							}
						}
						for (int i = 0; i < f.cAll; ++i) {
							Face *const F = f.fList[i].face;
							if (!F) continue;
							if (!F->Txtr) { ++skNoTxtr; continue; }
							// Skip materials that don't act as solid occluders.
							// Flag-based (Transparent/Additive/SkipZ) + name-
							// based (lamp/emit/emitter — FLD doesn't flag
							// emissives, so we infer from name). Cached per-
							// Material* so the strstr only runs once.
							if (shouldSkip(F->Txtr)) { ++skXpar; continue; }
							if (F->A == F->B) { ++skDegen; continue; }
							// SoA Phase 4: read TPos_AOS.z via F->frame.
							// F here is from a shadow per-light clone; its
							// frame is the per-clone scratch frame
							// (Transform stamps it during FList build).
							{
								const float *tz = F->frame->TPos_z;
								if (tz[F->A_idx] <= 0.0f &&
								    tz[F->B_idx] <= 0.0f &&
								    tz[F->C_idx] <= 0.0f) {
									++skBack; continue;
								}
							}
							clipper.Render(F, MekaleleShadowDepth, false, rt, cam,
                                          /*skipMipLevel=*/true);
							++kept;
						}
						// One stderr line per "completely empty tile" (the
						// black-map signal) so we can see *why* — first 16
						// per process to keep output bounded.
						if (kept == 0 && f.cAll > 0) {
							static std::atomic<int> sLogged{0};
							if (sLogged.fetch_add(1) < 16) {
								std::fprintf(stderr,
								    "[SHADOW-TILE-EMPTY] cAll=%d  skNoTxtr=%d skXpar=%d skDegen=%d skBack=%d\n",
								    f.cAll, skNoTxtr, skXpar, skDegen, skBack);
							}
						}
						g_currentShadowMap = nullptr;
						g_inDynamicShadowBake = false;
						TailProf::addBusy(_tp);   // before release → race-free
						// One permit per completed task (see renderns::shadowDone).
						renderns::shadowDone.release();
					});
			}
		}
	}
	TailProf::drain(renderns::shadowDone, tilesEnqueued, "shadow-bake");
	// FDS_SHADOW_TILE_PROBE: per-frame 4x4 tile occupancy tracking on
	// the buffer this mode just wrote. Reports a tile flipping between
	// occupied and empty across consecutive frames — the whole-tile
	// shadow flicker signature. Temporary diagnostic.
	if (std::getenv("FDS_SHADOW_TILE_PROBE")) {
		static std::vector<std::array<int,16>> sPrev;
		static int sFrame = 0;
		++sFrame;
		if (sPrev.size() != g_shadowMaps.size())
			sPrev.assign(g_shadowMaps.size(), {});
		for (size_t li = 0; li < g_shadowMaps.size(); ++li) {
			ShadowMap &sm = g_shadowMaps[li];
			Omni *const O = sm.omni;
			if (!O || !(O->Flags & Omni_Active)) continue;
			const bool isStaticP = (O->Flags & Omni_StaticShadow) != 0;
			if (isStaticP != wantStaticOmnis) continue;
			const auto &buf = writeDynamicBuf ? sm.depth_dynamic : sm.depth;
			std::array<int,16> occ{};
			const int tw = sm.xres / 4, th = sm.yres / 4;
			for (int ty = 0; ty < 4; ++ty)
				for (int tx = 0; tx < 4; ++tx) {
					int n = 0;
					for (int y = ty*th; y < (ty+1)*th; y += 4) {
						const uint16_t *row = buf.data() + size_t(y)*sm.xres;
						for (int x = tx*tw; x < (tx+1)*tw; x += 4)
							if (row[x]) ++n;
					}
					occ[ty*4+tx] = n;
				}
			for (int t = 0; t < 16; ++t) {
				const int prev = sPrev[li][t], now = occ[t];
				if ((prev > 50 && now == 0) || (prev == 0 && now > 50)) {
					std::fprintf(stderr,
						"[SHADOW-TILE] f=%d light=%zu res=%d tile=%d,%d %d -> %d\n",
						sFrame, li, sm.xres, t%4, t/4, prev, now);
				}
			}
			sPrev[li] = occ;
			// Full-buffer hash: catches polygon-level nondeterminism the
			// coarse occupancy misses. Under a fixed-frame bench any
			// hash change is a race. FDS_SHADOW_DUMP_LIGHT=<li> dumps
			// prev/now PGMs of the first two changes for texel diffing.
			{
				static std::vector<uint64_t> sHash;
				static std::vector<uint16_t> sPrevBuf;
				if (sHash.size() != g_shadowMaps.size())
					sHash.assign(g_shadowMaps.size(), 0);
				uint64_t h = 0xcbf29ce484222325ull;
				const uint8_t *bp = (const uint8_t*)buf.data();
				for (size_t k = 0; k < buf.size() * 2; k += 7) {
					h ^= bp[k]; h *= 0x100000001b3ull;
				}
				const char *dl = std::getenv("FDS_SHADOW_DUMP_LIGHT");
				const bool isDumpLight = dl && size_t(atoi(dl)) == li;
				if (sFrame > 2 && sHash[li] != 0 && sHash[li] != h) {
					std::fprintf(stderr,
						"[SHADOW-HASH] f=%d light=%zu res=%d %s buffer changed\n",
						sFrame, li, sm.xres,
						writeDynamicBuf ? "dyn" : "static");
					if (isDumpLight && sPrevBuf.size() == buf.size()) {
						static int dumpN = 0;
						if (dumpN < 2) {
							for (int which = 0; which < 2; ++which) {
								const auto &b2 = which ? buf : sPrevBuf;
								char fn[128];
								std::snprintf(fn, sizeof(fn),
								    "/tmp/smdump_l%zu_d%d_%s.bin",
								    li, dumpN, which ? "now" : "prev");
								FILE *fp = std::fopen(fn, "wb");
								if (fp) {
									std::fwrite(b2.data(), 2, b2.size(), fp);
									std::fclose(fp);
								}
							}
							++dumpN;
						}
					}
				}
				sHash[li] = h;
				if (isDumpLight) sPrevBuf = buf;
			}
		}
	}
	const auto tRasterEnd = clk::now();
	if (sProfShadow) {
		sRasterAcc += std::chrono::duration<double, std::milli>(tRasterEnd - tRasterStart).count();
	}

	// [experiment: --shadow-swizzle] Re-tile the planes this bake just wrote,
	// timed SEPARATELY from the bake (its own [SHADOW-BAKE] swizzle line) so
	// the overhead is measurable against the PCF-read gain. Parallel over
	// maps on the pool (memory-bound copies). Moving omnis (DynOmnis) never
	// get a dynamic-plane bake, so also build their (all-zero) dynamic copies
	// once — the cube tap only switches a map to the tiled path when all the
	// planes it needs are tiled.
	if (sSwz && !swzMaps.empty()) {
		const auto tSwzStart = clk::now();
		for (size_t li : swzMaps) {
			ShadowMap *smp = &g_shadowMaps[li];
			const bool dynPl = writeDynamicBuf;
			ThreadPool::instance().enqueue([smp, dynPl]() {
				ShadowMap_SwizzlePlanes(*smp, dynPl);
				if (!dynPl && smp->depthDynSw.empty())
					ShadowMap_SwizzlePlanes(*smp, true);   // one-shot zeroed dyn copy
				renderns::shadowDone.release();
			});
		}
		for (size_t k = 0; k < swzMaps.size(); ++k) renderns::shadowDone.acquire();
		const double swzMs = std::chrono::duration<double, std::milli>(clk::now() - tSwzStart).count();
		if (mode == ShadowBakeMode::StaticOnce) {
			std::fprintf(stderr, "[SHADOW-SWZ] init swizzle: %.2f ms (%zu maps)\n",
			             swzMs, swzMaps.size());
		} else if (sBakeTime) {
			static thread_local double sSwzAcc[3] = {0.0, 0.0, 0.0};
			static thread_local int    sSwzN[3]   = {0, 0, 0};
			static const int sSwzInterval = []() {
				const char *e = std::getenv("FDS_SHADOW_PROF_INTERVAL");
				return (e && *e) ? std::max(1, std::atoi(e)) : 60;
			}();
			const int mi = int(mode);
			sSwzAcc[mi] += swzMs;
			if (++sSwzN[mi] % sSwzInterval == 0) {
				std::fprintf(stderr,
					"[SHADOW-BAKE] swizzle: %.2f ms/frame (avg of %d, %s, %zu maps)\n",
					sSwzAcc[mi] / sSwzInterval, sSwzInterval,
					mode == ShadowBakeMode::DynamicMeshesPerFrame ? "DynMeshes" : "DynOmnis",
					swzMaps.size());
				std::fflush(stderr);
				sSwzAcc[mi] = 0.0;
			}
		}
	}

	// --shadow_bake_time: accumulate this frame's full dynamic-bake wall time
	// and emit one averaged [SHADOW-BAKE] line per interval. Same interval knob
	// as --shadow_prof so the two read together. PER-MODE accumulators: both
	// DynamicOmnisPerFrame and DynamicMeshesPerFrame reach this block every
	// frame, so a shared accumulator would conflate them (and the print would
	// always land on whichever mode hits the interval boundary). Keyed by mode
	// index, each pass reports its own honest per-frame cost.
	if (sBakeTime) {
		static thread_local double sBakeAcc[3]   = {0.0, 0.0, 0.0};
		static thread_local int    sBakeFrames[3] = {0, 0, 0};
		static const int sBakeInterval = []() {
			const char *e = std::getenv("FDS_SHADOW_PROF_INTERVAL");
			return (e && *e) ? std::max(1, std::atoi(e)) : 60;
		}();
		const int mi = int(mode);   // 1=DynOmnis, 2=DynMeshes (0=StaticOnce excluded by sBakeTime)
		sBakeAcc[mi] += std::chrono::duration<double, std::milli>(tRasterEnd - tBakeStart).count();
		if (++sBakeFrames[mi] % sBakeInterval == 0) {
			std::fprintf(stderr,
				"[SHADOW-BAKE] dynamic bake: %.2f ms/frame (avg of %d, %s, %zu maps)\n",
				sBakeAcc[mi] / sBakeInterval, sBakeInterval,
				mode == ShadowBakeMode::DynamicMeshesPerFrame ? "DynMeshes" : "DynOmnis",
				g_shadowMaps.size());
			std::fflush(stderr);
			sBakeAcc[mi] = 0.0;
		}
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
	// Skip the view-space → light-view-space precompute when there's
	// no main camera. Happens during the StaticOnce bake called from
	// Initialize_X (off-render-loop thread, before any scene set the
	// global View). The values would be junk anyway, and every per-
	// frame render call recomputes them against the correct View.
	if (View) {
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
	}

	// FDS_DUMP_SHADOWMAP=1: write each shadow map as a .pgm under
	// /tmp/shadowmap_<N>.pgm. One-shot per process — only dumps on
	// the first call.
#if FDS_DEV
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
#endif
}

// ─── Stage A: shadow bake / gbuffer overlap ───────────────────────────────
// See docs/THREADING_OVERLAP_PLAN.md. The bake is data-isolated from the
// gbuffer fill (per-light VertexScratch clones, its own shadow maps, its own
// renderns::shadowDone semaphore; it does not touch tileCounter when
// overlapping). It must complete before deferred lighting samples the maps, so
// renderFrame calls ShadowBake_JoinPending() right before Render_DeferredLighting.
namespace {
	std::thread       g_shadowBakeThread;
	std::atomic<bool> g_shadowBakePending{false};
}

void ShadowBake_DispatchGreets(Scene *Sc) {
	auto runBakes = [Sc]() {
		Render_DeferredShadowMaps(Sc, ShadowBakeMode::DynamicOmnisPerFrame);
		if (fds::FeatureFlags::shadow_dynamic())
			Render_DeferredShadowMaps(Sc, ShadowBakeMode::DynamicMeshesPerFrame);
	};
	if (fds::FeatureFlags::shadow_gbuffer_overlap()) {
		// Spawn on a dedicated orchestrator thread; the gbuffer fill in the
		// subsequent gg->Render() runs concurrently. Pending is cleared by the
		// first JoinPending() of the frame (the main lighting pass).
		g_shadowBakePending.store(true, std::memory_order_release);
		g_shadowBakeThread = std::thread(runBakes);
	} else {
		runBakes();  // current behavior: synchronous inline bake
	}
}

void ShadowBake_JoinPending() {
	if (g_shadowBakePending.exchange(false, std::memory_order_acquire)) {
		if (g_shadowBakeThread.joinable()) g_shadowBakeThread.join();
	}
}
