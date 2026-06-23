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
#include <climits>
#include <semaphore>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "simde/x86/fma.h"

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/VertexFrame.h"  // SoA Phase 4: F->frame access in xpar-ids debug path
#include "Base/Scene.h"
#include "Base/RenderContext.h"
#include "DeferredCommon.h"   // DeferredLightingCtx + g_deferredCtx (publish for strip path)
#include "TailProf.h"         // phase-1 barrier-tail instrumentation (FDS_TAIL_PROF)
#include "RenderPipeline.h"
#include "VESA/Vesa.h"
#include "FRUSTRUM.H"

// Defined in Transform.cpp — used by the deferred transparent path
// (RenderXparClumpInStrip) and by the legacy xpar peel.
bool IsFrontFacingInViewSpace(const Face* F);

// Defined in the deferred TUs. The deferred lighting context is built by
// Render_DeferredLighting (fills the passed ctx) and threaded to the
// volumetric/fog orchestrators (RenderContext migration). DeferredLightingCtx
// + Render_DeferredLighting (with its optional DeferredOverride) are declared
// in DeferredCommon.h (included below).
void Render_DeferredFogPass();
// Full-res SSAO post-pass (DeferredSSAO.cpp). Gated on --ssao; no-op otherwise.
void Render_SSAO();
// Render_VolumetricCones declared in DeferredCommon.h (with its inlineDispatch arg).
void Render_OmniHalos(const DeferredLightingCtx& ctx);
void Render_DeferredVolumetric(const DeferredLightingCtx& ctx);
void Render_DeferredFastFog(const DeferredLightingCtx& ctx);
void Render_DeferredFastFogSkyPaint(const DeferredLightingCtx& ctx);
void Render_ScreenSpaceRain();
void Render_LensDrops();
void FastFog_BeginFrame();
void Render_DeferredSkybox();
void VolProf_Tick();

// Defined in LightmapBake.cpp.
namespace fds { void Render_LightmapViz(struct Scene *Sc); }
// HDR prototype (docs/fountain_hdr_plan.md) — defined in Hdr.cpp.
namespace fds { void Hdr_BeginFrame(); void Render_TonemapToVPage(); void Render_BloomPass(); void Render_AnamorphicPass(); void Render_DoFPass(); void Render_LensGhostPass(); void Render_LensPostPass(); void Render_GradeGrainPass(); void Hdr_ActivateNoFog(); extern bool g_hdrDeferTonemap; }

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
void RenderInner(const fds::RenderContext& ctx, float x1, float y1, float x2, float y2);
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

// thread_local: each worker thread calls FPU_LPrecision/NormalState
// independently to set its own FPCR, and they must save/restore their
// own original. Was racing on shared backup via the threadpool init
// (TSan-confirmed). FPCR itself IS per-thread (ARM A64 saves it at
// context switch); the backup needs to track that.
thread_local dword OldMXCSR;
thread_local dword CurMXCSR;
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
	// Per-pass tile-completion signal. Producer workers call
	// tileDone.release() when their tile is finished; the orchestrator
	// drains N permits via tileDone.acquire() loops. Replaces the prior
	// mutex+condvar+counter trio — counting_semaphore provides the same
	// "wait for N tasks" semantics with a lock-free producer and no
	// risk of the lost-wakeup race condition_variable has when the
	// shared variable is modified outside the mutex.
	//
	// LeastMaxValue = INT_MAX so the same semaphore can absorb any
	// dispatch size (per-pass tile counts up to ~30, but the shadow
	// orchestrator may enqueue hundreds of Transform_Objects tasks).
	std::counting_semaphore<INT_MAX> tileDone{0};

	// Dedicated completion signal for the dynamic shadow-map bake
	// (Render_DeferredShadowMaps). Kept separate from tileDone so the bake
	// can run on its own orchestrator thread concurrently with the gbuffer
	// fill (which drains tileDone) without the two stealing each other's
	// permits — pool workers release whichever semaphore matches the task
	// they just ran. See docs/THREADING_OVERLAP_PLAN.md (Stage A).
	std::counting_semaphore<INT_MAX> shadowDone{0};

	// Legacy mutex+condvar trio: still referenced by a few non-hot
	// setup paths (e.g. shadow-orchestrator pre-dispatch counter reset)
	// that haven't been migrated yet. Safe to keep alongside the
	// semaphore — the two systems are independent.
	std::mutex                  tileCounterMutex;
	std::atomic<int>            tileCounter;
	std::condition_variable     condition;
};

// output is unused on the deferred path.
bool deferredEnabled() {
	// --hdr / --deferred-quarter / --shard-deferred are meaningless without the
	// deferred kernel (only it populates g_hdrBuf and the quarter/shard paths),
	// so treat them as implying --deferred. Otherwise a cmdline with --hdr but no
	// bare --deferred renders the city FORWARD: no tonemap and the whole HDR post
	// block (bloom/anamorphic/dof/vignette/grade/grain/water-glints) is skipped,
	// while greets still works (its scene forces the deferred path). Flag-OFF is
	// unchanged → render-gate unaffected.
	return fds::FeatureFlags::deferred()
	    || fds::FeatureFlags::hdr()
	    || fds::FeatureFlags::deferred_quarter()
	    || fds::FeatureFlags::shard_deferred();
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


void fds::RenderPipeline::renderFrame(RenderPath path, bool skipVolumetric)
{
	// Tile counts chosen so tile sizes are multiples of 8 (Mekalele's
	// SIMD tile). At 1920×1080: 6×5 → 320×216, both 8-aligned. Otherwise
	// adjacent clipper tile workers race on the 16-byte aligned blendv
	// RMW at apply_exact:459 — TSan-confirmed flicker source. Round
	// tileSize down to mult-of-8; the last tile in each row/col absorbs
	// the remainder.
	constexpr auto		numTilesX = 6;
	constexpr auto		numTilesY = 5;
	const int rawTileX = (XRes + (numTilesX - 1)) / numTilesX;
	const int rawTileY = (YRes + (numTilesY - 1)) / numTilesY;
	const auto			tileSizeX = rawTileX & ~7;
	const auto			tileSizeY = rawTileY & ~7;

	// Tile-completion semaphore needs no reset — by construction every
	// pass enqueues N tasks and drains N permits, so the resting value
	// between passes is always 0.
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
	// New frame for the fast-fog grid: until this frame's fog dispatch runs
	// (if it runs at all), the transparent peel must not sample a stale grid.
	FastFog_BeginFrame();

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
		// Lightmap planes: clear to 0 each frame (sentinel = "no
		// lightmap for this pixel"). Mekalele only writes them for
		// static-mesh faces, so without this clear, robot pixels
		// would inherit the static mesh data from previous frames.
		if (!g_gbuffer->lightmapMF.empty()) {
			parallel_memset(g_gbuffer->lightmapMF.data(), 0,
			                numPixels * sizeof(uint32_t));
		}
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

	// The last tile in each row / column absorbs the rounding remainder so
	// the union covers ALL of [0, XRes) × [0, YRes). Without this, when
	// XRes / numTilesX doesn't divide cleanly into 8, the right strip
	// (XRes - numTilesX*tileSizeX columns) was dropped — no worker ever
	// rasterized it, so it stayed at the pre-clear value. Same on the
	// bottom. Visible as black bars on right/bottom at non-1920×1080
	// resolutions, and as right/bottom-edge discontinuities in the
	// City cube-map bake (which uses the engine XRes/YRes).
	//
	// Safe vs the tile-boundary race: the boundary between tiles N-2 and
	// N-1 stays 8-aligned (tileSizeX is masked & ~7), so the existing
	// ClipperTileRect ownership split works as before. The right/bottom
	// edge of the last tile has no neighbour to race with.
	// Per-pass context for the forward raster path (RenderContext migration):
	// the primary context = the current render globals, so byte-identical.
	// Built once, captured by the tile lambdas (must outlive the tasks — the
	// tileDone drain below joins them before this returns).
	const fds::RenderContext fctx = fds::primaryRenderContext();
	for (auto j = 0; j < numTilesY; ++j) {
		auto y1 = tileSizeY * j;
		auto y2 = (j == numTilesY - 1) ? YRes : (y1 + tileSizeY);
		for (auto i = 0; i < numTilesX; ++i) {
			auto x1 = tileSizeX * i;
			auto x2 = (i == numTilesX - 1) ? XRes : (x1 + tileSizeX);

			if (deferred) {
				ThreadPool::instance().enqueue([x1, y1, x2, y2]() { RenderInnerMekalele(x1, y1, x2, y2); });
			} else {
				ThreadPool::instance().enqueue([&fctx, x1, y1, x2, y2]() { RenderInner(fctx, x1, y1, x2, y2); });
			}
			// RenderInner(x1, y1, x2, y2);
		}
	}

	TailProf::drain(renderns::tileDone, numTilesX * numTilesY, "gbuffer");

	// Per-pass deferred lighting context, owned by renderFrame (RenderContext
	// migration): Render_DeferredLighting fills it; it's threaded to the
	// volumetric/fog orchestrators. Published to the file-scope g_deferredCtx
	// after fill for the one remaining un-threaded reader —
	// RenderXparClumpInStrip on FILLERS' TBR strip path (threading the
	// deferred type through FILLERS is a separate, gate-uncovered step). Empty
	// in forward mode → the volumetric passes no-op (null lights), which is
	// also safer than the old stale-singleton read.
	DeferredLightingCtx dctx{};
	if (deferred) {
		// HDR (prototype): size + clear the radiance buffer BEFORE the deferred
		// lighting pass. In HDR mode the surface kernel writes its unclamped
		// opaque radiance + a coverage flag into g_hdrBuf (HDR B1), the fog
		// composite reads that back, and the tonemap below maps it down. Must
		// precede Render_DeferredLighting (which writes it) and run exactly once
		// per frame (clearing it again afterward would wipe the coverage). Gated
		// on --hdr.
		if (fds::FeatureFlags::hdr()) fds::Hdr_BeginFrame();

		// Stage A overlap (docs/THREADING_OVERLAP_PLAN.md): join the dynamic
		// shadow bake here — as late as possible (after the gbuffer fill, which
		// ran concurrently with it) but before lighting samples the maps. No-op
		// unless a bake was dispatched async this frame (--shadow-gbuffer-overlap,
		// greets only); pending is cleared by this first join so recursive
		// shard/RTT lighting passes don't re-join.
		ShadowBake_JoinPending();

		// Phase A deferred lighting pass: per-pixel ambient + Lambertian
		// + linear falloff against the scene's omnis, modulated by the
		// G-buffer's per-pixel texture sample. Replaces the per-vertex
		// Lighting() (skipped on the deferred path) plus the rasterizer's
		// colorize step in one go.
		Render_DeferredLighting(dctx);
		g_deferredCtx = dctx;

		// Full-res SSAO: darken contact creases over the deferred-lit opaque
		// pixels, BEFORE fog/volumetric so haze layers on top of the occluded
		// surfaces. MAIN pass only (!skipVolumetric) — skip the mirror RTT /
		// water-reflection underlay passes (cost + the static AO scratch buffer
		// isn't re-entrant). No-op unless --ssao.
		if (!skipVolumetric)
			Render_SSAO();

		// Deferred skybox: paint pixels the rasterizer didn't touch
		// (zEnc == 0) by sampling the sky cube directly from the G-
		// buffer. Replaces the forward RenderSkyCube draw and removes
		// its overdraw. Gated on --deferred-skybox; off by default
		// until visually validated.
		if (fds::FeatureFlags::deferred_skybox()) {
			Render_DeferredSkybox();
		}

		// Full-screen distance fog over deferred-lit opaque pixels. Runs
		// BEFORE the transparent peel AND before the cone/halo volumetric
		// scatter, so god-ray shafts add on top of the fogged surfaces
		// and stay bright (mixing them away afterward kills their
		// contrast). Disabled when the unified pass is on (folds fog in).
		//   fast_fog on  → analytic exponential model (iquilezles), any
		//                  scene, mixes toward Ambient.
		//   fast_fog off → legacy sqrt-to-black ramp, Scn_Fogged only.
		if (!useUnifiedVolumetric()) {
			if (fds::FeatureFlags::fast_fog()) {
				// The froxel grid is view-aligned and temporally reprojected
				// against the previous MAIN-camera frame — populating it with
				// the mirror camera would corrupt the history (and pay a whole
				// extra populate). Reflection-pass geometry renders unfogged,
				// but the mirrored SKY must still be painted: those pixels
				// otherwise keep stale VPage content (uninitialized on a
				// scene's first frames) that the water then reflects.
				if (!(skipVolumetric && fds::FeatureFlags::fast_fog_froxel()))
					Render_DeferredFastFog(dctx);
				else
					Render_DeferredFastFogSkyPaint(dctx);
			} else
				Render_DeferredFogPass();
		}

		// Fog-off HDR activation: a no-fog scene (greets) never ran the froxel
		// composite, so g_hdrActive is still false — the transparent peel below
		// and the end-of-pipeline tonemap would both no-op and the view stays LDR.
		// Resolve + activate here, AFTER the deferred kernel and BEFORE the peel.
		// No-op when the composite already activated (fast_fog) or HDR is off.
		// !skipVolumetric: only the MAIN pass activates — the city water reflection
		// pass (skipVolumetric) is an intermediate LDR underlay that must NOT route
		// its transparents into g_hdrBuf (it never tonemaps), same as the RTT.
		if (fds::FeatureFlags::hdr() && !fds::g_hdrActive && !skipVolumetric)
			fds::Hdr_ActivateNoFog();

		// HDR (Phase 2 overlay reorg): the tonemap used to run HERE (right after
		// fog), forcing every overlay below to composite LDR over the tonemapped
		// base. It now walks down the pipeline — the transparent peel + sprites
		// accumulate into g_hdrBuf (gated g_hdrActive) and the tonemap fires
		// after TBR_Render (see below). Each subsequent step (cones/halos/rain/
		// bolt) moves it further until it runs last.

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
		// Last tile in each row/col absorbs the rounding remainder so the
		// union covers [0, XRes) × [0, YRes). Mirrors the opaque tiler
		// (see comment in renderFrame's main tile loop); without this,
		// the right/bottom remainder strip skipped every xpar pass and
		// transparent surfaces (e.g. city water) failed to render in
		// those bands.
		auto runTilePass = [&](auto workFn) {
			for (auto j = 0; j < numTilesY; ++j) {
				auto y1 = tileSizeY * j;
				auto y2 = (j == numTilesY - 1) ? YRes : (y1 + tileSizeY);
				for (auto i = 0; i < numTilesX; ++i) {
					auto x1 = tileSizeX * i;
					auto x2 = (i == numTilesX - 1) ? XRes : (x1 + tileSizeX);
					const int tileIdx = j * numTilesX + i;
					ThreadPool::instance().enqueue([x1, y1, x2, y2, tileIdx, workFn]() {
						workFn(x1, y1, x2, y2, tileIdx);
					});
				}
			}
			TailProf::drain(renderns::tileDone, numTilesX * numTilesY, "postlight");
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
		const int peelPasses = xparPeelPassesEffective();
		auto flushBatch = [&]() {
			if (!batchOpen) return;
			TriMesh*    T   = batchMesh;
			XparFaceSel sel = batchFront ? XparFaceSel::FrontOnly
			                              : XparFaceSel::BackOnly;
			const int first = batchFirst;
			const int count = batchLastEnd - batchFirst;
			const bool reverseMode = (peelPasses > 1);
			auto rasterAndComposite = [&]() {
				runTilePass([first, count, T, sel, reverseMode](int x1, int y1, int x2, int y2, int /*tileIdx*/) {
					// thread_local: set on THIS worker, read by the synchronous
					// raster below (same thread). Opaque ignores it.
					g_xparPeelReverse = reverseMode;
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
			};

			if (peelPasses <= 1) {
				// Legacy single-fragment peel — byte-identical.
				if (batchFront) clearFront();
				else            clearBack();
				rasterAndComposite();
				batchOpen = false;
				return;
			}

			// Reverse depth peel WITHIN this (mesh, side) batch: peel the
			// side's overlapping fragments farthest-first and composite each
			// over the last → far-to-near, which is the same order the
			// facing split + sort key already establish between batches. K
			// passes deep; fixes >2 overlapping transparents sharing a mesh
			// (the fountain glass orbs) that the single keep-nearest layer
			// dropped. The inter-batch order (sort key) is untouched.
			uint16_t*      sideZ  = batchFront ? g_xparZ            : g_xparZBack;
			meka::GBuffer* sideGB = batchFront ? g_gbufferTransparent : g_gbufferTransparentBack;
			for (int pass = 0; pass < peelPasses; ++pass) {
				// Per-pass ceiling: pass 0 accepts everything (0); later passes
				// accept only fragments NEARER than the previous pass's peeled
				// Z (copied before the layer is cleared below).
				if (pass == 0)
					parallel_memset(g_xparPeelFloor, 0, numPixels * sizeof(uint16_t));
				else if (g_xparPeelFloor && sideZ)
					std::memcpy(g_xparPeelFloor, sideZ, numPixels * sizeof(uint16_t));
				// Clear the side layer: Z to 0xFFFF (keep-farthest init),
				// txtr to the empty sentinel.
				if (sideGB) parallel_memset(sideGB->txtr.data(), 0xFF, numPixels * sizeof(uint32_t));
				if (sideZ)  parallel_memset(sideZ, 0xFF, numPixels * sizeof(uint16_t));
				rasterAndComposite();
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

	// NOTE: this loop must run BEFORE TBR_Render — for TBR scenes the
	// sprite fillers (TBR_Sprite, and The_MMX_Scalar's TBR divert) only
	// INSERT into the strip lists; TBR_Render is the draw.
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
			if (A->TPos_AOS.z < C_NZP) continue;
			F->Filler(F, &A, 1, 0, rt, cam);
		}
	}

	if (CurScene->Flags & Scn_SpriteTBR)
	{
		TBR_Render(CurScene);
	}

	// HDR (Phase 2 overlay reorg): the tonemap walks down the pipeline as each
	// overlay is redirected into g_hdrBuf. It now fires after the cone/halo
	// volumetric passes below (was: here, right after TBR_Render). See that call.

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
	if (!skipVolumetric) {
		if (useUnifiedVolumetric()) {
			Render_DeferredVolumetric(dctx);   // NB: not yet HDR-redirected (no scene uses it)
		} else {
			Render_VolumetricCones(dctx);
			Render_OmniHalos(dctx);
		}

		// Screen-space rain LAST among the view-aligned scene passes (streaks
		// overlay fog, beams and halos — near rain is in front of all of
		// them) and never in the reflection pass (skipVolumetric): a
		// screen-space effect re-run from the mirror camera would rain
		// upward in the water. Rain is scene radiance → in HDR it accumulates
		// into g_hdrBuf (gated g_hdrActive) and is captured by the tonemap.
		Render_ScreenSpaceRain();

		// HDR (Phase 2.x): tonemap now that ALL scene-radiance overlays (cones,
		// halos, rain) have accumulated into g_hdrBuf. SKIPPED when
		// g_hdrDeferTonemap — the scene tonemaps later itself (the fountain
		// tonemaps after its bolt, so the bolt blooms too).
		//
		// MUST gate on `deferred`: this volumetric block runs for EVERY
		// renderFrame call, including the forward sky-cube paint
		// (RenderSkyCube → Render(ForceForward), deferred=false). On that path
		// Hdr_BeginFrame never ran, so g_hdrActive is stale-true and g_hdrBuf
		// holds the PREVIOUS frame's scene — tonemapping then overwrites the
		// freshly-painted skybox with last frame's image (grey wash + cityscape
		// smear under camera motion). Snapshots hid it: on a one-frame render
		// g_hdrActive starts false so the sky-cube pass's tonemap no-ops.
		if (deferred && fds::FeatureFlags::hdr() && !fds::g_hdrDeferTonemap) {
			fds::Render_DoFPass();          // depth-of-field bokeh on g_hdrBuf BEFORE bloom (no-op unless --dof)
			fds::Render_AnamorphicPass();   // lens streaks before the tonemap (no-op unless --anamorphic)
			fds::Render_BloomPass();        // glow into g_hdrBuf BEFORE the tonemap (no-op unless --bloom)
			fds::Render_LensGhostPass();    // ghost discs + halo into g_hdrBuf BEFORE the tonemap (no-op unless --lens_ghosts)
			fds::Render_TonemapToVPage();
			fds::Render_LensPostPass();     // CA + vignette on the 8-bit image AFTER the tonemap (no-op unless --chromatic/--vignette)
			fds::Render_GradeGrainPass();   // colour grade + film grain on the 8-bit image (no-op unless --grade/--grain)
		}

		// Lens droplets are on the "camera glass" — a post effect that refracts
		// the FINAL displayed image, so they run AFTER the tonemap (LDR, like
		// UI). The deferred fountain tonemaps in its tick, so it runs lens
		// drops there instead; skip here when the tonemap is deferred.
		if (!(fds::FeatureFlags::hdr() && fds::g_hdrDeferTonemap))
			Render_LensDrops();
	}
	VolProf_Tick();

	// Lightmap debug viz — paints VPage with mesh/face/bary/factor
	// data when --shadow-lightmap-viz != 0. No-op otherwise. Runs last
	// so cones/halos/fog/xpar can't overwrite the diagnostic display.
	fds::Render_LightmapViz(CurScene);



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
#if FDS_DEV
	if (fds::FeatureFlags::debug_xpar_ids()) {
		// Min screen area threshold (px²) — skips tiny particles.
		const float setArea = fds::FeatureFlags::debug_xpar_min_area();
		const float minArea = setArea > 0.0f ? setArea : 500.0f;
		std::fprintf(stderr, "[XPAR-IDS] (all transparent faces in render order)\n");
		for (int32_t i = 0; i < CAll; ++i) {
			Face* F = FList[i].face;
			if (!F || !F->Txtr) continue;
			if (!(F->Txtr->Flags & Mat_Transparent)) continue;
			// SoA Phase 4 (debug-xpar-ids dev path): read via F->frame.
			const VertexFrame *ff = F->frame;
			const uint32_t ai = F->A_idx, bi = F->B_idx, ci = F->C_idx;
			const float ax = ff->PX[ai], ay = ff->PY[ai];
			const float bx = ff->PX[bi], by = ff->PY[bi];
			const float cxv = ff->PX[ci], cyv = ff->PY[ci];
			const float area = 0.5f * std::fabs(ax*(by-cyv) + bx*(cyv-ay) + cxv*(ay-by));
			const float cx = (ax + bx + cxv) * (1.0f/3.0f);
			const float cy = (ay + by + cyv) * (1.0f/3.0f);
			const float azv = ff->TPos_z[ai], bzv = ff->TPos_z[bi], czv = ff->TPos_z[ci];
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
#endif
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
		Animate_Objects(Sc, View);
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

void Render(RenderPath path, bool skipVolumetric) { fds::RenderPipeline::instance().renderFrame(path, skipVolumetric); }
void RunScene(Scene *Sc, float Sec)  { fds::RenderPipeline::instance().runScene(Sc, Sec); }
void SetCurrentScene(Scene *Sc)      { fds::RenderPipeline::instance().setCurrentScene(Sc); }
void SetDeferredWaterMatID(int id)   { fds::RenderPipeline::instance().setDeferredWaterMatID(id); }
