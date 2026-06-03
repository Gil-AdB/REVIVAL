// Deferred per-pixel lighting pipeline — extracted from RENDER.CPP.
//
// All variants of the tile kernel live here (opaque, transparent front/
// back, outer-vec, quarter-rate fill), plus the per-frame view-space
// light list (ViewLightsSoA), per-tile and per-strip light culling
// (TileLights, buildTileLightLists, buildStripLightLists), the post-
// pass fog walk, and the TBR-strip xpar dispatcher RenderXparClumpInStrip.
//
// Public symbols consumed from RENDER.CPP's renderFrame orchestrator:
//   Render_DeferredLighting()        — full deferred pass (tile dispatch)
//   Render_DeferredFogPass()         — post-pass fog walk
//   renderDeferredTransparentTile_Front/Back() — wrappers for the per-tile
//                                       transparent-layer composite that
//                                       renderFrame calls inside a runTilePass
//                                       lambda. The template + g_deferredCtx
//                                       stay in this TU.
//   RenderXparClumpInStrip()         — TBR-strip xpar batch dispatch

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <memory.h>
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

// FRUSTRUM.CPP — file-scope; forward declare for libm-pow fallback.
extern float fastLog2(float x);
extern float fastPow2(float x);
#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "Base/StaticShadowLightmap.h"
#include "Base/TriMesh.h"
#include "Base/Vertex.h"
#include "Base/Face.h"
#include "Base/Omni.h"
#include "Base/Camera.h"
#include "Base/Material.h"
#include "Base/SpotLight.h"
#include "RenderPipeline.h"
#include "RENDER/LightmapBake.h"
#include "FILLERS/Mekalele.h"
#include "FILLERS/ShadowMap.h"
#include "FILLERS/FILLERS.H"
#include "FRUSTRUM.H"
#include "Threads.h"

// Defined in Transform.cpp; used by RenderXparClumpInStrip.
bool IsFrontFacingInViewSpace(const Face* F);
// Defined in RENDER.CPP.
extern int g_deferredWaterMatID;
// Tile-counter synchronisation primitives, defined in RENDER.CPP.
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <semaphore>
#include <climits>
namespace renderns {
	extern std::counting_semaphore<INT_MAX> tileDone;
	extern std::mutex                tileCounterMutex;
	extern std::atomic<int>          tileCounter;
	extern std::condition_variable   condition;
}

// Cone-tile path counters: incremented once per (tile × spot × 8-pixel
// batch). Reset and reported by VolProf_Tick when vol_prof is on. Sit
// at file scope so the cone tile fn (which is above the VolProf struct)
// can reach them.
static std::atomic<int> g_coneAnalyticHits{0};
static std::atomic<int> g_coneRaymarchHits{0};

constexpr int DEFERRED_MAX_LIGHTS = 128;
constexpr int DEFERRED_NUM_TILES_X = 12;
constexpr int DEFERRED_NUM_TILES_Y = 8;
constexpr int DEFERRED_NUM_TILES   = DEFERRED_NUM_TILES_X * DEFERRED_NUM_TILES_Y;

// Cache-line transition stats for shadow-map sampling (gated on
// --shadow_prof_cache). Atomic accumulation across tile workers with
// relaxed ordering; a thread-local tracks the last sample's cache-line
// address so we can count cross-line transitions. Reset and dumped in
// Render_DeferredLighting after the tile barrier.
//
// A "transition" is when the next shadow sample lands on a different
// 64-byte cache line than the previous one on the same thread. Compared
// to total samples this approximates spatial locality: low ratio = most
// samples reuse a recently-touched line; high ratio = lots of misses.
static std::atomic<uint64_t> g_shadowProfSamples{0};
static std::atomic<uint64_t> g_shadowProfLineTransitions{0};
thread_local uintptr_t s_shadowProfLastLine = 0;
struct ViewLightsSoA {
	alignas(32) float posX[DEFERRED_MAX_LIGHTS];
	alignas(32) float posY[DEFERRED_MAX_LIGHTS];
	alignas(32) float posZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float colB[DEFERRED_MAX_LIGHTS];
	alignas(32) float colG[DEFERRED_MAX_LIGHTS];
	alignas(32) float colR[DEFERRED_MAX_LIGHTS];
	alignas(32) float range2[DEFERRED_MAX_LIGHTS];
	alignas(32) float rRange[DEFERRED_MAX_LIGHTS];
	// Spot light cone (Light_SpotLight). dirX/Y/Z is the cone axis in
	// view space (unit vector). cosInner / cosOuter are the cone half
	// angles in cosine form. isSpot=0 means omni (cone params ignored).
	alignas(32) float dirX[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirY[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float cosInner[DEFERRED_MAX_LIGHTS];
	alignas(32) float cosOuter[DEFERRED_MAX_LIGHTS];
	alignas(32) uint32_t isSpot[DEFERRED_MAX_LIGHTS];
	// Index into g_shadowMaps for this light's shadow map, or -1 if
	// not a shadow-caster (most omnis). Filled per frame in
	// Render_DeferredLighting alongside the other per-light fields.
	alignas(32) int32_t  shadowMapIdx[DEFERRED_MAX_LIGHTS];
	// World-space position of the light. Required for cube shadow
	// face selection (option 3 in the cube infra design): the per-
	// pixel sample's world position is computed once from the view-
	// space sample, then `D_world = sample_world - omni_world` picks
	// the cube face. Populated for all lights so the kernel doesn't
	// branch on light type to read it; only used by cube-shadow path.
	alignas(32) float posWorldX[DEFERRED_MAX_LIGHTS];
	alignas(32) float posWorldY[DEFERRED_MAX_LIGHTS];
	alignas(32) float posWorldZ[DEFERRED_MAX_LIGHTS];
	// Index into g_cubeShadowRefs for omnis with cube shadow, or -1.
	// Mutually exclusive with shadowMapIdx (which is only meaningful
	// for Light_SpotLight). Populated by Render_DeferredLighting from
	// the omni's Type + CastsShadow flag + cube allocation.
	alignas(32) int32_t  cubeShadowIdx[DEFERRED_MAX_LIGHTS];
	// Per-omni halo controls, decoupled from surface lighting. See
	// Omni::HaloIntensity / Omni::HaloRange comments. haloDensityMul[]
	// is the per-omni density multiplier (1.0 for legacy behavior);
	// haloRange2[] / haloRRange[] override range2[] / rRange[] for
	// the halo sphere bounds (falling back to those when HaloRange=0).
	alignas(32) float    haloDensityMul[DEFERRED_MAX_LIGHTS];
	alignas(32) float    haloRange     [DEFERRED_MAX_LIGHTS];
	alignas(32) float    haloRange2    [DEFERRED_MAX_LIGHTS];
	alignas(32) float    haloRRange    [DEFERRED_MAX_LIGHTS];
	// Per-light mirror id (0 = original world; >0 = clone of mirror
	// with that id). The kernels read gb.mirrorId[pixel] once per
	// pixel and skip any light whose mirrorId disagrees, so original-
	// world surfaces are lit only by original omnis and each mirror's
	// clone surfaces only by that mirror's cloned omnis. Without this
	// filter clone pixels receive the union of both light sets and
	// saturate (greets teleporter mirror went uniformly yellow).
	alignas(32) uint32_t mirrorId      [DEFERRED_MAX_LIGHTS];
};

// Per-tile light culling. Each tile (a slice of the screen) keeps a
// **compacted** SoA copy of the omnis whose screen-space bounding
// circle overlaps it. Inner loop reads tl.posX[n] etc. directly —
// contiguous, prefetcher-friendly, and — for the vec path — lets a
// single `load_a` pull 8 omnis with one 32-byte aligned read instead
// of building a Vec4f via four `ld1.s {v}[lane]` scalar gathers.
//
// Memory: 24 tiles × 8 arrays × 128 floats × 4 bytes = 96 KiB total.
// Easily fits in L2; per-tile slice stays warm in L1 across pixels.
struct TileLights {
	alignas(32) float posX[DEFERRED_MAX_LIGHTS];
	alignas(32) float posY[DEFERRED_MAX_LIGHTS];
	alignas(32) float posZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float colB[DEFERRED_MAX_LIGHTS];
	alignas(32) float colG[DEFERRED_MAX_LIGHTS];
	alignas(32) float colR[DEFERRED_MAX_LIGHTS];
	alignas(32) float range2[DEFERRED_MAX_LIGHTS];
	alignas(32) float rRange[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirX[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirY[DEFERRED_MAX_LIGHTS];
	alignas(32) float dirZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float cosInner[DEFERRED_MAX_LIGHTS];
	alignas(32) float cosOuter[DEFERRED_MAX_LIGHTS];
	alignas(32) uint32_t isSpot[DEFERRED_MAX_LIGHTS];
	// Shadow-map index in g_shadowMaps for this light's shadow (or -1).
	// Filled by buildTileLightLists when FDS_SHADOWS=1 and the omni
	// has Omni_CastsShadow. Lighting kernel uses it to gate the light's
	// contribution per pixel.
	alignas(32) int32_t shadowMapIdx[DEFERRED_MAX_LIGHTS];
	// World-space position of the light + cube-shadow index. Mirrors
	// the same fields in ViewLightsSoA; see comments there. Per-tile
	// copy so the inner pixel loop has all light state in one SoA.
	alignas(32) float    posWorldX[DEFERRED_MAX_LIGHTS];
	alignas(32) float    posWorldY[DEFERRED_MAX_LIGHTS];
	alignas(32) float    posWorldZ[DEFERRED_MAX_LIGHTS];
	alignas(32) int32_t  cubeShadowIdx[DEFERRED_MAX_LIGHTS];
	// See ViewLightsSoA::mirrorId. Mirrored into the per-tile/per-
	// strip light list so the inner pixel loop reads tl.mirrorId[n].
	alignas(32) uint32_t mirrorId  [DEFERRED_MAX_LIGHTS];
	int             count;          // active entries
	int             paddedCount;    // (count + 7) & ~7, ≤ DEFERRED_MAX_LIGHTS
	float           zMin;           // view-space z of closest pixel in tile
	float           zMax;           // view-space z of farthest pixel in tile
	                                // (+inf / -inf when tile has no geometry)
};

// Per-frame setup shared across all tile jobs. Captured by reference
// in the lambdas; lifetime ends when Render() returns (after the
// tileCounter wait in Render_DeferredLighting).
struct DeferredLightingCtx {
	const meka::GBuffer *gb;
	MatTable             matTable;
	const ViewLightsSoA *lights;
	int                  numLights;
	const TileLights    *tileLights;  // [DEFERRED_NUM_TILES]
	float                invFOVX;
	float                invFOVY;
	float                invZScale;
	Scene               *Sc;
	int                  waterMatID;  // -1 if no water blend
	// view→world transform: world = viewToWorld·viewPos + cameraWorldPos.
	// For a rotation View.Mat, viewToWorld == transpose(View.Mat).
	// Filled once per frame in Render_DeferredLighting from View. Used
	// by per-pixel cube-shadow sampling to compute the sample's world-
	// space position (which then picks the cube face).
	float                viewToWorld[3][3];
	float                cameraWorldX;
	float                cameraWorldY;
	float                cameraWorldZ;
};

// File-scope ctx, populated each frame by Render_DeferredLighting and
// also read by Render_DeferredTransparentLighting_Tile. Both kernels
// share the same per-frame setup (matTable, tileLights, view-space
// projection params, scene); rebuilding it per pass would double the
// per-frame setup cost.
DeferredLightingCtx g_deferredCtx{};

// Runtime mip-level debug knobs. Defined here so FDS doesn't need a
// symbol from DEMO. Declared in Rev.h; toggled by N / Shift+N keys.
std::atomic<int>  g_forceMipLevel{-1};
std::atomic<bool> g_vizMipLevel{false};

// Build per-tile compacted SoA. For each omni: project its view-space
// bounding sphere into screen space, find overlapping tile rects, then
// **append the omni's values** (not its index) into each overlapping
// tile's contiguous SoA arrays. Memory cost is O(omnis × overlapped
// tiles × bytes-per-omni) which for City is ~30 × 4 × 32 ≈ 4 KiB; for
// Greets ~10 × 4 × 32 ≈ 1.3 KiB. Negligible.
//
// The vec inner loop benefits because `tl.posX[slot..slot+8)` is now
// 32-byte aligned and contiguous — one `load_a` instead of four
// `ld1.s {v}[lane]` gathers per Vec4f. Scalar benefits too via
// straight-line prefetchable access.
// Per-tile depth bounds. Scans ZPage16 once per tile (already populated
// by the G-buffer pass), finds the closest and farthest zEnc with
// geometry, and converts back to view-space z. Tiles with no geometry
// get zMin=+inf / zMax=-inf so all lights are culled for them.
//
// Encoding reminder: zEnc = 0xFF80 - g_zscale*z, so larger zEnc means
// closer pixel. zEnc == 0 means the pixel was never touched.
static void computeTileDepthBounds(TileLights *tileLights, int numTilesX, int numTilesY,
                                   int tileSizeX, int tileSizeY, int xres, int yres,
                                   float invZScale)
{
	const uint16_t *zp = reinterpret_cast<const uint16_t*>(ZPage16);
	for (int j = 0; j < numTilesY; ++j) {
		const int y_lo = j * tileSizeY;
		const int y_hi = std::min(y_lo + tileSizeY, yres);
		for (int i = 0; i < numTilesX; ++i) {
			const int x_lo = i * tileSizeX;
			const int x_hi = std::min(x_lo + tileSizeX, xres);
			const int idx = j * numTilesX + i;

			__m128i vMaxZ = _mm_setzero_si128();           // chasing max
			__m128i vMinZ = _mm_set1_epi16(int16_t(0xFFFF)); // chasing min (zeros mapped up)
			uint16_t maxZ = 0;
			uint16_t minZ = 0xFFFF;

			for (int py = y_lo; py < y_hi; ++py) {
				const uint16_t *row = zp + size_t(py) * xres + x_lo;
				const int width = x_hi - x_lo;
				int px = 0;
				for (; px + 8 <= width; px += 8) {
					__m128i v = _mm_loadu_si128((const __m128i*)(row + px));
					// For min-tracking, replace 0 with 0xFFFF so untouched
					// pixels don't pull the minimum down.
					__m128i isZero = _mm_cmpeq_epi16(v, _mm_setzero_si128());
					__m128i vForMin = _mm_or_si128(v, isZero);
					vMaxZ = _mm_max_epu16(vMaxZ, v);
					vMinZ = _mm_min_epu16(vMinZ, vForMin);
				}
				for (; px < width; ++px) {
					uint16_t z = row[px];
					if (z > maxZ) maxZ = z;
					if (z != 0 && z < minZ) minZ = z;
				}
			}
			// Horizontal reduce the SIMD halves.
			alignas(16) uint16_t mx[8], mn[8];
			_mm_store_si128((__m128i*)mx, vMaxZ);
			_mm_store_si128((__m128i*)mn, vMinZ);
			for (int k = 0; k < 8; ++k) {
				if (mx[k] > maxZ) maxZ = mx[k];
				if (mn[k] < minZ) minZ = mn[k];
			}

			if (maxZ == 0) {
				tileLights[idx].zMin = std::numeric_limits<float>::infinity();
				tileLights[idx].zMax = -std::numeric_limits<float>::infinity();
			} else {
				// Larger zEnc → closer, so maxZ-zEnc maps to zMin-view.
				tileLights[idx].zMin = float(0xFF80 - maxZ) * invZScale;
				tileLights[idx].zMax = float(0xFF80 - minZ) * invZScale;
			}
		}
	}
}

static void buildTileLightLists(TileLights *tileLights, int numTilesX, int numTilesY,
                                 int tileSizeX, int tileSizeY, int xres, int yres,
                                 const ViewLightsSoA &lights, int numLights)
{
	const int numTiles = numTilesX * numTilesY;
	for (int t = 0; t < numTiles; ++t) {
		tileLights[t].count = 0;
	}

	// Per-tile depth-bounded light culling. Computed by
	// computeTileDepthBounds (caller fills tileLights[].zMin / zMax);
	// we reject lights whose view-space z extent doesn't overlap the
	// tile's pixel depth range. Default on.
	const bool zCullEnabled = fds::FeatureFlags::deferred_zcull();

	for (int li = 0; li < numLights; ++li) {
		const float vx = lights.posX[li];
		const float vy = lights.posY[li];
		const float vz = lights.posZ[li];
		const float r2 = lights.range2[li];
		const float r  = std::sqrt(r2);
		const float vz_minus_r = vz - r;
		const float vz_plus_r  = vz + r;

		// Sphere entirely behind camera: skip.
		if (vz + r < 0.0f) continue;

		int sx_min, sx_max, sy_min, sy_max;
		if (vz - r < 1.0f) {
			// Sphere straddles or is in front of near plane — be
			// conservative and tag every tile. (This is rare for
			// City; keeps the math simple.)
			sx_min = 0;        sx_max = xres - 1;
			sy_min = 0;        sy_max = yres - 1;
		} else {
			// Pinhole projection of bounding sphere — small-angle
			// approximation. Center: (CntrEX + vx*FOVX/vz, CntrEY -
			// vy*FOVY/vz). Radius on-screen: r * FOVX/vz (use FOVY for
			// vertical). Slightly over-estimates near the edges of
			// the FOV but that just lights tiles that miss the per-
			// pixel cull, no correctness impact.
			const float invZ = 1.0f / vz;
			const float cx   = CntrEX + vx * FOVX * invZ;
			const float cy   = CntrEY - vy * FOVY * invZ;
			const float rx   = r * FOVX * invZ;
			const float ry   = r * FOVY * invZ;
			sx_min = std::max(0,        int(std::floor(cx - rx)));
			sx_max = std::min(xres - 1, int(std::ceil (cx + rx)));
			sy_min = std::max(0,        int(std::floor(cy - ry)));
			sy_max = std::min(yres - 1, int(std::ceil (cy + ry)));
			if (sx_min > sx_max || sy_min > sy_max) continue;
		}

		const int tile_i_lo = sx_min / tileSizeX;
		const int tile_i_hi = std::min(numTilesX - 1, sx_max / tileSizeX);
		const int tile_j_lo = sy_min / tileSizeY;
		const int tile_j_hi = std::min(numTilesY - 1, sy_max / tileSizeY);

		const float Lpx = lights.posX[li];
		const float Lpy = lights.posY[li];
		const float Lpz = lights.posZ[li];
		const float Lcb = lights.colB[li];
		const float Lcg = lights.colG[li];
		const float Lcr = lights.colR[li];
		const float Lr2 = lights.range2[li];
		const float Lrr = lights.rRange[li];
		const float Ldx = lights.dirX[li];
		const float Ldy = lights.dirY[li];
		const float Ldz = lights.dirZ[li];
		const float Lci = lights.cosInner[li];
		const float Lco = lights.cosOuter[li];
		const uint32_t Lis = lights.isSpot[li];
		const int32_t  Lsi = lights.shadowMapIdx[li];
		const float    Lwx = lights.posWorldX[li];
		const float    Lwy = lights.posWorldY[li];
		const float    Lwz = lights.posWorldZ[li];
		const int32_t  Lci2 = lights.cubeShadowIdx[li];
		const uint32_t Lmid = lights.mirrorId[li];

		for (int j = tile_j_lo; j <= tile_j_hi; ++j) {
			for (int i = tile_i_lo; i <= tile_i_hi; ++i) {
				const int idx = j * numTilesX + i;
				TileLights &tl = tileLights[idx];
				// Depth cull: skip if light's z-extent doesn't overlap
				// the tile's pixel depth range. Empty tiles have
				// zMin=+inf and zMax=-inf so this rejects everything.
				if (zCullEnabled &&
				    (vz_plus_r < tl.zMin || vz_minus_r > tl.zMax)) {
					continue;
				}
				if (tl.count < DEFERRED_MAX_LIGHTS) {
					const int s = tl.count++;
					tl.posX[s]   = Lpx;
					tl.posY[s]   = Lpy;
					tl.posZ[s]   = Lpz;
					tl.colB[s]   = Lcb;
					tl.colG[s]   = Lcg;
					tl.colR[s]   = Lcr;
					tl.range2[s] = Lr2;
					tl.rRange[s] = Lrr;
					tl.dirX[s]     = Ldx;
					tl.dirY[s]     = Ldy;
					tl.dirZ[s]     = Ldz;
					tl.cosInner[s] = Lci;
					tl.cosOuter[s] = Lco;
					tl.isSpot[s]   = Lis;
					tl.shadowMapIdx[s] = Lsi;
					tl.posWorldX[s] = Lwx;
					tl.posWorldY[s] = Lwy;
					tl.posWorldZ[s] = Lwz;
					tl.cubeShadowIdx[s] = Lci2;
					tl.mirrorId[s]      = Lmid;
				}
			}
		}
	}

	// Zero the padding slots (count..paddedCount) so the vec loop's
	// over-read produces range2=0 entries that fail the per-pixel
	// `len2 <= range2` mask and contribute nothing.
	for (int t = 0; t < numTiles; ++t) {
		TileLights &tl = tileLights[t];
		const int padded = (tl.count + 7) & ~7;
		const int pad_to = std::min(padded, DEFERRED_MAX_LIGHTS);
		for (int p = tl.count; p < pad_to; ++p) {
			tl.posX[p]   = 0.0f;
			tl.posY[p]   = 0.0f;
			tl.posZ[p]   = 0.0f;
			tl.colB[p]   = 0.0f;
			tl.colG[p]   = 0.0f;
			tl.colR[p]   = 0.0f;
			tl.range2[p] = 0.0f;
			tl.rRange[p] = 0.0f;
			tl.dirX[p]     = 0.0f;
			tl.dirY[p]     = 0.0f;
			tl.dirZ[p]     = 0.0f;
			tl.cosInner[p] = -2.0f;
			tl.cosOuter[p] = -2.0f;
			tl.isSpot[p]   = 0u;
			tl.shadowMapIdx[p] = -1;
			tl.posWorldX[p] = 0.0f;
			tl.posWorldY[p] = 0.0f;
			tl.posWorldZ[p] = 0.0f;
			tl.cubeShadowIdx[p] = -1;
			// 0xffffffff in the padding slot so the per-pixel `==`
			// test against pixelMirrorId (always < 256) is always
			// false; the padded slots contribute nothing whatever
			// the pixel's mirror id.
			tl.mirrorId[p]      = 0xffffffffu;
		}
		tl.paddedCount = pad_to;
	}
}

// Strip-flavored light list builder (1D, Y-only) for the unified TBR
// path's transparent strip rendering. Mirrors buildTileLightLists but
// over 8-row Y-strips matching the TBR tile shape. Each strip's list
// is consumed by RenderXparClumpInStrip via a DeferredLightingCtx
// variant whose `tileLights` points at this array.
//
// MAX strips at any reasonable display: 4096/8 = 512. We size for
// that ceiling so reallocation isn't needed at runtime resize.
constexpr int DEFERRED_MAX_STRIPS = 512;
static TileLights g_stripLights[DEFERRED_MAX_STRIPS];
static int        g_numStripLights = 0;

static void buildStripLightLists(int numStrips, int stripHeight, int yres,
                                  const ViewLightsSoA &lights, int numLights)
{
	if (numStrips > DEFERRED_MAX_STRIPS) numStrips = DEFERRED_MAX_STRIPS;
	for (int s = 0; s < numStrips; ++s) {
		g_stripLights[s].count = 0;
	}

	for (int li = 0; li < numLights; ++li) {
		const float vx = lights.posX[li];
		const float vy = lights.posY[li];
		const float vz = lights.posZ[li];
		const float r  = std::sqrt(lights.range2[li]);

		if (vz + r < 0.0f) continue;

		int sy_min, sy_max;
		if (vz - r < 1.0f) {
			sy_min = 0;        sy_max = yres - 1;
		} else {
			const float invZ = 1.0f / vz;
			const float cy   = CntrEY - vy * FOVY * invZ;
			const float ry   = r * FOVY * invZ;
			sy_min = std::max(0,       int(std::floor(cy - ry)));
			sy_max = std::min(yres - 1, int(std::ceil (cy + ry)));
			if (sy_min > sy_max) continue;
		}

		const int strip_lo = sy_min / stripHeight;
		const int strip_hi = std::min(numStrips - 1, sy_max / stripHeight);

		const float Lpx = lights.posX[li];
		const float Lpy = lights.posY[li];
		const float Lpz = lights.posZ[li];
		const float Lcb = lights.colB[li];
		const float Lcg = lights.colG[li];
		const float Lcr = lights.colR[li];
		const float Lr2 = lights.range2[li];
		const float Lrr = lights.rRange[li];
		const float Ldx = lights.dirX[li];
		const float Ldy = lights.dirY[li];
		const float Ldz = lights.dirZ[li];
		const float Lci = lights.cosInner[li];
		const float Lco = lights.cosOuter[li];
		const uint32_t Lis = lights.isSpot[li];
		const uint32_t Lmid = lights.mirrorId[li];

		for (int s = strip_lo; s <= strip_hi; ++s) {
			TileLights &tl = g_stripLights[s];
			if (tl.count < DEFERRED_MAX_LIGHTS) {
				const int idx = tl.count++;
				tl.posX[idx]   = Lpx;
				tl.posY[idx]   = Lpy;
				tl.posZ[idx]   = Lpz;
				tl.colB[idx]   = Lcb;
				tl.colG[idx]   = Lcg;
				tl.colR[idx]   = Lcr;
				tl.range2[idx] = Lr2;
				tl.rRange[idx] = Lrr;
				tl.dirX[idx]     = Ldx;
				tl.dirY[idx]     = Ldy;
				tl.dirZ[idx]     = Ldz;
				tl.cosInner[idx] = Lci;
				tl.cosOuter[idx] = Lco;
				tl.isSpot[idx]   = Lis;
				tl.mirrorId[idx] = Lmid;
			}
		}
	}

	// Zero the padding slots so the vec loop's overread is benign
	// (range2=0 lanes fail the per-pixel `len2 <= range2` mask).
	for (int s = 0; s < numStrips; ++s) {
		TileLights &tl = g_stripLights[s];
		const int padded = (tl.count + 7) & ~7;
		const int pad_to = std::min(padded, DEFERRED_MAX_LIGHTS);
		for (int p = tl.count; p < pad_to; ++p) {
			tl.posX[p]   = 0.0f;
			tl.posY[p]   = 0.0f;
			tl.posZ[p]   = 0.0f;
			tl.colB[p]   = 0.0f;
			tl.colG[p]   = 0.0f;
			tl.colR[p]   = 0.0f;
			tl.range2[p] = 0.0f;
			tl.rRange[p] = 0.0f;
			tl.dirX[p]     = 0.0f;
			tl.dirY[p]     = 0.0f;
			tl.dirZ[p]     = 0.0f;
			tl.cosInner[p] = -2.0f;
			tl.cosOuter[p] = -2.0f;
			tl.isSpot[p]   = 0u;
			tl.mirrorId[p] = 0xffffffffu;
		}
		tl.paddedCount = pad_to;
		// Strip Z bounds: not used by transparent kernel (depth is
		// already in the xpar G-buffer). Set sentinels.
		tl.zMin = -std::numeric_limits<float>::infinity();
		tl.zMax =  std::numeric_limits<float>::infinity();
	}
	g_numStripLights = numStrips;
}

// Fast scalar 1/sqrt(x) via NEON's frsqrte + one Newton-Raphson step.
// arm64 fsqrt+fdiv is ~24 cycles serial; this is ~5. Accuracy is around
// fast_rsqrt moved to FDS/FILLERS/SimdHelpers.h (shared with Mekalele).

// x^N via binary exponentiation, resolved at compile time. Bit-exact
// vs std::pow for integer N (within float precision; each squaring
// loses ~½ ULP). Optimal op count: 5 squarings + 1 mul for N=48, 6
// squarings for N=64, 7 for N=128, etc. — TMP unrolls the recursion
// into a flat sequence of fmuls. Works for any N ≥ 1; the scalar and
// SIMD overloads of sq()/fmul() let one template body cover both
// float and __m256.
static inline float  sq(float  x) { return x * x; }
static inline __m256 sq(__m256 x) { return _mm256_mul_ps(x, x); }
static inline float  fmul(float  a, float  b) { return a * b; }
static inline __m256 fmul(__m256 a, __m256 b) { return _mm256_mul_ps(a, b); }

template<int N, typename T>
static inline T pow_squaring(T x) {
	static_assert(N >= 1, "pow_squaring: N must be a positive integer");
	if constexpr (N == 1)         return x;
	else if constexpr (N % 2 == 0) return sq(pow_squaring<N/2,   T>(x));
	else                           return fmul(x, pow_squaring<N-1, T>(x));
}

// Runtime dispatch on Glossiness. The TMP template makes adding a new
// case trivial: just list the gloss value here. Falls back to std::pow
// for anything unlisted (FDS_DEFERRED_GLOSS_STATS will flag it).
static inline float pow_glossClass(float x, unsigned gloss) {
	switch (gloss) {
		case 4:   return pow_squaring<4,   float>(x);
		case 8:   return pow_squaring<8,   float>(x);
		case 16:  return pow_squaring<16,  float>(x);
		case 32:  return pow_squaring<32,  float>(x);
		case 48:  return pow_squaring<48,  float>(x);
		case 64:  return pow_squaring<64,  float>(x);
		case 128: return pow_squaring<128, float>(x);
		default:  return std::pow(x, float(gloss));
	}
}

// Vec8f spec accumulator: walks the per-tile light list, computes
// Blinn-Phong half-vector + N·H, raises to the gloss power via
// pow_squaring<Gloss>, accumulates into sB/sG/sR with the same range
// + falloff masks the diffuse loop uses. Called only when wantSpecular
// is true and Mat->Glossiness matches one of our specialized gloss
// constants (the kernel switches on it; default falls back to scalar
// libm pow). Replaces the scalar spec loop the vec path used before.
//
// KNOWN GAP: this vec loop does NOT apply tl.shadowMapIdx[n] / PCF
// shadow attenuation per-light — the scalar lighting body does, but
// the SIMD body short-circuits the per-light shadow tap. The deferred
// vec path is off by default (FDS_DEFERRED_VEC=0, slower than scalar
// on arm64-via-simde anyway), so this never fires in production —
// but if vec is ever turned on for non-bumped scenes, shadowed
// surfaces will leak specular highlights through shadows. Mirror the
// scalar fix at DeferredLighting.cpp:1312 (multiply specStrength by
// shadowAtten) when that day comes.
template<int Gloss>
static inline void run_vec_spec_loop(const TileLights &tl,
                                      float x, float y, float z,
                                      float nx, float ny, float nz,
                                      float vx, float vy, float vz,
                                      float matSpec,
                                      uint32_t pmid,
                                      float &sB, float &sG, float &sR) {
	__m256 vx_v   = _mm256_set1_ps(x);
	__m256 vy_v   = _mm256_set1_ps(y);
	__m256 vz_v   = _mm256_set1_ps(z);
	__m256 vnx_v  = _mm256_set1_ps(nx);
	__m256 vny_v  = _mm256_set1_ps(ny);
	__m256 vnz_v  = _mm256_set1_ps(nz);
	__m256 vvx_v  = _mm256_set1_ps(vx);
	__m256 vvy_v  = _mm256_set1_ps(vy);
	__m256 vvz_v  = _mm256_set1_ps(vz);
	__m256 vSpec  = _mm256_set1_ps(matSpec);
	__m256 vZero  = _mm256_setzero_ps();
	__m256 vOne   = _mm256_set1_ps(1.0f);
	__m256i pmid_v = _mm256_set1_epi32((int)pmid);
	__m256 accB   = _mm256_setzero_ps();
	__m256 accG   = _mm256_setzero_ps();
	__m256 accR   = _mm256_setzero_ps();

	for (int slot = 0; slot < tl.paddedCount; slot += 8) {
		__m256 lpx = _mm256_load_ps(tl.posX   + slot);
		__m256 lpy = _mm256_load_ps(tl.posY   + slot);
		__m256 lpz = _mm256_load_ps(tl.posZ   + slot);
		__m256 lcb = _mm256_load_ps(tl.colB   + slot);
		__m256 lcg = _mm256_load_ps(tl.colG   + slot);
		__m256 lcr = _mm256_load_ps(tl.colR   + slot);
		__m256 lr2 = _mm256_load_ps(tl.range2 + slot);
		__m256 lrr = _mm256_load_ps(tl.rRange + slot);
		// Mirror filter: contrib only lights whose mirrorId matches.
		__m256i lmid = _mm256_load_si256(
			(const __m256i*)(tl.mirrorId + slot));
		__m256 mirrorMask = _mm256_castsi256_ps(
			_mm256_cmpeq_epi32(lmid, pmid_v));

		__m256 wx = _mm256_sub_ps(lpx, vx_v);
		__m256 wy = _mm256_sub_ps(lpy, vy_v);
		__m256 wz = _mm256_sub_ps(lpz, vz_v);
		__m256 dot = _mm256_fmadd_ps(wx, vnx_v,
		              _mm256_fmadd_ps(wy, vny_v,
		               _mm256_mul_ps(wz, vnz_v)));
		__m256 len2 = _mm256_fmadd_ps(wx, wx,
		               _mm256_fmadd_ps(wy, wy,
		                _mm256_mul_ps(wz, wz)));

		__m256 mask_range = _mm256_cmp_ps(len2, lr2,   _CMP_LE_OQ);
		__m256 mask_dot   = _mm256_cmp_ps(dot,  vZero, _CMP_GE_OQ);
		__m256 mask_pos   = _mm256_cmp_ps(len2, vZero, _CMP_GT_OQ);
		__m256 mask       = _mm256_and_ps(mask_range,
		                     _mm256_and_ps(mask_dot,
		                      _mm256_and_ps(mask_pos, mirrorMask)));

		__m256 safe_len2 = _mm256_blendv_ps(vOne, len2, mask);
		__m256 lenInv    = _mm256_rsqrt_ps(safe_len2);
		__m256 dist      = _mm256_mul_ps(safe_len2, lenInv);
		__m256 falloff   = _mm256_sub_ps(vOne, _mm256_mul_ps(dist, lrr));

		// Light direction (normalized w). half = L + V, renormalize.
		__m256 ldx = _mm256_mul_ps(wx, lenInv);
		__m256 ldy = _mm256_mul_ps(wy, lenInv);
		__m256 ldz = _mm256_mul_ps(wz, lenInv);
		__m256 hx  = _mm256_add_ps(ldx, vvx_v);
		__m256 hy  = _mm256_add_ps(ldy, vvy_v);
		__m256 hz  = _mm256_add_ps(ldz, vvz_v);
		__m256 hLen2 = _mm256_fmadd_ps(hx, hx,
		                _mm256_fmadd_ps(hy, hy,
		                 _mm256_mul_ps(hz, hz)));
		__m256 mask_hpos = _mm256_cmp_ps(hLen2, vZero, _CMP_GT_OQ);
		__m256 safe_h2   = _mm256_blendv_ps(vOne, hLen2, mask_hpos);
		__m256 hLenInv   = _mm256_rsqrt_ps(safe_h2);
		// Fold renorm into the N·H dot — 3 fewer fmuls per 8-pixel batch
		// vs scaling H component-wise then dotting. Matches the scalar
		// path's identical fold below.
		__m256 NdotH_raw = _mm256_fmadd_ps(hx, vnx_v,
		                    _mm256_fmadd_ps(hy, vny_v,
		                     _mm256_mul_ps(hz, vnz_v)));
		__m256 NdotH = _mm256_mul_ps(NdotH_raw, hLenInv);
		__m256 mask_nh = _mm256_cmp_ps(NdotH, vZero, _CMP_GT_OQ);
		// Clamp NdotH to [0,1] before squaring so the chain is stable
		// even for masked-out lanes.
		__m256 safeNH = _mm256_max_ps(NdotH, vZero);
		safeNH        = _mm256_min_ps(safeNH, vOne);
		__m256 spec   = pow_squaring<Gloss, __m256>(safeNH);
		__m256 strength = _mm256_mul_ps(_mm256_mul_ps(spec, vSpec), falloff);
		// Apply combined mask (range + dot ≥ 0 + h positive + NdotH > 0)
		__m256 fullMask = _mm256_and_ps(mask, _mm256_and_ps(mask_hpos, mask_nh));
		__m256 contrib  = _mm256_blendv_ps(vZero, strength, fullMask);

		accB = _mm256_fmadd_ps(contrib, lcb, accB);
		accG = _mm256_fmadd_ps(contrib, lcg, accG);
		accR = _mm256_fmadd_ps(contrib, lcr, accR);
	}
	alignas(32) float bufB[8], bufG[8], bufR[8];
	_mm256_store_ps(bufB, accB);
	_mm256_store_ps(bufG, accG);
	_mm256_store_ps(bufR, accR);
	for (int i = 0; i < 8; ++i) {
		sB += bufB[i];
		sG += bufG[i];
		sR += bufR[i];
	}
}

// FDS_DEFERRED_VEC=1 in env switches the per-pixel inner loop from the
// scalar branch-predicted early-out to a Vec8f SoA accumulation. Stays
// available for A/B benchmarking — on arm64-via-simde it's slower than
// scalar (Vec8f decomposes to two 128-bit NEON ops with full per-lane
// work, no early-out), but the scaffolding is in place to revisit on
// AVX2 / native NEON / wider lanes.
static bool deferredLightingVecEnabled() {
	return fds::FeatureFlags::deferred_vec();
}

// FDS_DEFERRED_OUTER_VEC=1 selects the outer-SIMD kernel which
// processes 8 pixels per row simultaneously rather than 8 omnis per
// pixel. The setup work (Z-decode, screen→view position reconstruct,
// oct normal decode, ambient compute) amortizes across a vec lane;
// the per-pixel scalar mat-table chase and texel gather stay scalar
// since arm64-via-simde has no usable vgather. The omni loop becomes
// "1 omni × 8 pixels per iter" — each omni's position/color is a
// broadcast, the per-pixel normal/pos/color stay in vec registers.
// Env override: FDS_DEFERRED_OUTER_VEC=1 forces on, =0 forces off,
// unset = follow the per-scene Scene::PreferOuterVec policy. Each
// Initialize_<scene> sets the flag based on whether the scene is
// dominated by matte materials (city, fountain, crash) or by spec/nmap
// shiny ones (greets — outer kernel pays per-lane nmap + scalar
// fallback for most pixels).
static bool deferredLightingOuterVecEnabled() {
	// Tri-state: when set explicitly (CLI or env), the flag wins; otherwise
	// fall back to the scene's PreferOuterVec policy. Greets sets it off
	// because the outer kernel's per-lane nmap costs dominate most pixels.
	if (fds::FeatureFlags::isSet(fds::FeatureFlags::BoolId::deferred_outer_vec))
		return fds::FeatureFlags::deferred_outer_vec();
	return CurScene && CurScene->PreferOuterVec != 0;
}

// FDS_DEFERRED_CHECKERBOARD=1 enables half-rate lighting: only pixels
// where (px + py) & 1 == 0 (the "even" cells of a checkerboard) get the
// full per-pixel omni evaluation; the odd cells are filled in a second
// wave by averaging two of their already-shaded neighbors when those
// share matID, falling back to full shade when they don't (e.g. across
// material edges). ~50% of pixels skip the omni loop entirely.
static bool deferredLightingCheckerboardEnabled() {
	return fds::FeatureFlags::deferred_checkerboard();
}

// FDS_DEFERRED_QUARTER=1 enables quarter-rate lighting: only pixels
// where (px&1)==0 AND (py&1)==0 (one corner of every 2×2 block) get
// the full kernel; the other 3 of each 2×2 are filled in wave 2 by
// direction-specific interpolation:
//   (odd, even) → horizontal: avg of left+right shaded neighbors
//   (even, odd) → vertical:   avg of top+bottom shaded neighbors
//   (odd,  odd) → diagonal:   avg of 4 corners (top-left, top-right,
//                                              bottom-left, bottom-right)
// matID-mismatch detection at each pattern → fall back to a full
// shade for that pixel. ~75% of pixels skip the omni loop. Mutually
// exclusive with checkerboard (this one wins when both are set).
static bool deferredLightingQuarterEnabled() {
	return fds::FeatureFlags::deferred_quarter();
}

// Per-pixel lightmap address resolved once at top of the per-pixel loop.
// `lm == nullptr` ⇒ legacy path (per-pixel cube shadow tap). Otherwise:
// bilinear sample from the cached lightmap. Two writers populate `lm`:
//   1. Mekalele wrote a non-zero meshLMId for this pixel (static mesh).
//   2. Scene has a populated staticLMTable for that meshLMId.
struct PixelLightmap {
	const StaticShadowLightmap *lm = nullptr;
	TriMesh *mesh = nullptr;  // only set when shadow_lightmap_recompute_at_bary is on
	int     faceIdx = 0;
	uint8_t sB = 0, tB = 0;
};

// One read of the lightmap G-buffer planes + scene table lookup per pixel.
// Returns a zero-filled PixelLightmap when off / dynamic / out-of-range —
// callers test `pl.lm == nullptr` to decide which shadow path runs.
static inline PixelLightmap resolvePixelLightmap(const meka::GBuffer &gb,
                                                  size_t i,
                                                  const Scene *Sc)
{
	PixelLightmap pl;
	if (gb.lightmapMF.empty() || !Sc || !Sc->staticLMTable) return pl;
	const uint32_t mf  = gb.lightmapMF[i];
	const uint16_t mid = uint16_t(mf >> 16);
	if (mid == 0 || size_t(mid) >= Sc->staticLMTable->size()) return pl;
	TriMesh *T = (*Sc->staticLMTable)[mid];
	if (!T || !T->staticShadowLM) return pl;
	const uint16_t fidx = uint16_t(mf & 0xFFFF);
	if (fidx >= T->staticShadowLM->numFaces) return pl;
	pl.lm        = T->staticShadowLM;
	pl.mesh      = T;
	pl.faceIdx   = int(fidx);
	const uint16_t st = gb.lightmapST[i];
	pl.sB = uint8_t(st & 0xFF);
	pl.tB = uint8_t(st >> 8);
	return pl;
}

// Single point of choice for "which cube-shadow path runs this pixel".
// When the pixel has a lightmap address AND we're not in dynamic-shadow
// mode, sample the lightmap. Otherwise run the per-pixel cube tap.
// (Dynamic mode falls back because the lightmap only encodes static
// occluders; the runtime cube tap reads max(static, dynamic) depth.)
// Bundle of cube-tap-related flags. Read ONCE per tile worker and passed
// into resolveCubeAtten so the per-pixel + per-omni hot loop never re-
// queries FeatureFlags::* or g_shadowMode for state that's constant for
// the duration of the tile.
struct CubeAttenFlags {
	bool      shadowDynamicOn;
	bool      lightmapPlanar;
	bool      lightmapNearest;
	bool      lightmapRecomputeBake;
	bool      lightmapRecomputeBary;
	bool      profNoCubeTap;
	ShadowMode shadowMode;
};

static inline float resolveCubeAtten(const PixelLightmap &pl,
                                      int32_t cubeIdx,
                                      bool useLightmap,
                                      const CubeAttenFlags &caFlags,
                                      // cube-tap fallback inputs:
                                      float wx, float wy, float wz, float lenInv,
                                      float nGeoX, float nGeoY, float nGeoZ,
                                      float sampleWorldX, float sampleWorldY, float sampleWorldZ,
                                      float vx, float vy, float vz,
                                      int kShadowBiasG, int kSlopeBiasG,
                                      // Receiver's matID for PolyId cube tap. Pass -1
                                      // to force Depth mode; otherwise caFlags.shadowMode
                                      // selects.
                                      int surfaceMatId)
{
	// Diagnostic: short-circuit to "fully lit" so we can A/B-bench the
	// pure tap cost vs the rest of the kernel. See --prof-no-cube-tap.
	if (caFlags.profNoCubeTap) return 1.0f;
	// Moving omnis (Omni_CastsShadow without Omni_StaticShadow) skip the
	// lightmap path — their cube is re-baked every frame from current
	// IPos, so the t=0 static lightmap is invalid. Fall through to the
	// per-pixel cube tap below, which reads the freshly-baked cube.
	const bool cubeOmniStatic = (cubeIdx >= 0
	    && size_t(cubeIdx) < g_cubeShadowRefs.size()
	    && g_cubeShadowRefs[cubeIdx].omni
	    && (g_cubeShadowRefs[cubeIdx].omni->Flags & Omni_StaticShadow));
	if (useLightmap && pl.lm && cubeIdx >= 0 && cubeIdx < pl.lm->numOmnis && cubeOmniStatic) {
		// Debug: --shadow-lightmap-recompute-bake replaces the atlas
		// bilinear lookup with a fresh per-pixel call to the bake-time
		// sampler (SampleStaticCubeAtWorld). Same flow as the bake, but
		// at the runtime pixel's world position. Isolates bake-function
		// correctness from atlas/bary: if the rendered output matches
		// what cube-tap produces, the bake function is fine; if it
		// matches the broken lightmap path, the bake function itself
		// is wrong (differs from CubeShadow_Sample).
		if (caFlags.lightmapRecomputeBake) {
			const float dotGeoR = wx*nGeoX + wy*nGeoY + wz*nGeoZ;
			const float nDotLR = dotGeoR * lenInv;
			const float invNdotLR = 1.0f / (nDotLR > 0.2f ? nDotLR : 0.2f);
			const int slopeBiasR = int(float(kSlopeBiasG) * (invNdotLR - 1.0f));
			const uint8_t lit = fds::LightmapBake_DebugSampleAtWorld(
			    cubeIdx, sampleWorldX, sampleWorldY, sampleWorldZ,
			    kShadowBiasG, slopeBiasR);
			return float(lit) * (1.0f / 255.0f);
		}
		// Debug: --shadow-lightmap-recompute-at-bary takes the runtime-stored
		// (sB, tB) for this pixel, interpolates face A/B/C world positions
		// using those barys to reconstruct "where the runtime thinks this
		// pixel is on the face", then calls SampleStaticCubeAtWorld at THAT
		// reconstructed point. If the result matches the cube-tap reference,
		// the bary points to the right physical place and the lightmap bug
		// is in atlas resolution. If it diverges, the bary itself is wrong.
		if (caFlags.lightmapRecomputeBary && pl.mesh) {
			TriMesh *T = pl.mesh;
			if (pl.faceIdx >= 0 && DWord(pl.faceIdx) < T->FIndex) {
				const Face &F = T->Faces[pl.faceIdx];
				if (F.A && F.B && F.C) {
					Vector wA, wB, wC;
					MatrixXVector(T->RotMat, &F.A->Pos, &wA);
					MatrixXVector(T->RotMat, &F.B->Pos, &wB);
					MatrixXVector(T->RotMat, &F.C->Pos, &wC);
					wA.x += T->IPos.x; wA.y += T->IPos.y; wA.z += T->IPos.z;
					wB.x += T->IPos.x; wB.y += T->IPos.y; wB.z += T->IPos.z;
					wC.x += T->IPos.x; wC.y += T->IPos.y; wC.z += T->IPos.z;
					const float sBf = float(pl.sB) * (1.0f / 255.0f);
					const float tBf = float(pl.tB) * (1.0f / 255.0f);
					const float wA_w = 1.0f - sBf - tBf;
					const float wpx = wA_w*wA.x + sBf*wB.x + tBf*wC.x;
					const float wpy = wA_w*wA.y + sBf*wB.y + tBf*wC.y;
					const float wpz = wA_w*wA.z + sBf*wB.z + tBf*wC.z;
					const float dotGeoR = wx*nGeoX + wy*nGeoY + wz*nGeoZ;
					const float nDotLR = dotGeoR * lenInv;
					const float invNdotLR = 1.0f / (nDotLR > 0.2f ? nDotLR : 0.2f);
					const int slopeBiasR = int(float(kSlopeBiasG) * (invNdotLR - 1.0f));
					const uint8_t lit = fds::LightmapBake_DebugSampleAtWorld(
					    cubeIdx, wpx, wpy, wpz, kShadowBiasG, slopeBiasR);
					return float(lit) * (1.0f / 255.0f);
				}
			}
		}
		float staticAtten;
		if (caFlags.lightmapPlanar && !pl.lm->planarBases.empty()) {
			staticAtten = pl.lm->sampleBilinearPlanar(pl.faceIdx, cubeIdx,
			                                          sampleWorldX, sampleWorldY, sampleWorldZ);
		} else if (caFlags.lightmapNearest) {
			staticAtten = pl.lm->sampleNearest(pl.faceIdx, cubeIdx, pl.sB, pl.tB);
		} else {
			staticAtten = pl.lm->sampleBilinear(pl.faceIdx, cubeIdx, pl.sB, pl.tB);
		}
		// Composite static × dynamic for --shadow-dynamic. The lightmap
		// atlas only encodes static-occluder shadow factor (baked once at
		// scene init from sm.depth / sm.polyId). To get dynamic mesh
		// shadows on static surfaces, layer a per-pixel cube tap against
		// the DYNAMIC buffers only (sm.depth_dynamic / sm.polyId_dynamic
		// — re-baked each frame by Render_DeferredShadowMaps in
		// DynamicMeshesPerFrame mode). Multiply: the surface must pass
		// both the static and dynamic occlusion tests to receive light.
		// Skip when --shadow-dynamic is off — the dynamic buffers are
		// all-zero and the call would be a no-op multiply by 1.0.
		if (caFlags.shadowDynamicOn) {
			float dynAtten;
			if (caFlags.shadowMode == ShadowMode::PolyId) {
				dynAtten = CubeShadow_Sample(cubeIdx,
				                              sampleWorldX, sampleWorldY, sampleWorldZ,
				                              vx, vy, vz, /*constBias=*/0, /*slopeBias=*/0,
				                              surfaceMatId, /*dynamicOnly=*/true);
			} else {
				const float dotGeo = wx*nGeoX + wy*nGeoY + wz*nGeoZ;
				const float nDotL = dotGeo * lenInv;
				const float invNdotL = 1.0f / (nDotL > 0.2f ? nDotL : 0.2f);
				const int slopeBias = int(float(kSlopeBiasG) * (invNdotL - 1.0f));
				dynAtten = CubeShadow_Sample(cubeIdx,
				                              sampleWorldX, sampleWorldY, sampleWorldZ,
				                              vx, vy, vz, kShadowBiasG, slopeBias,
				                              /*surfaceMatId=*/-1, /*dynamicOnly=*/true);
			}
			return staticAtten * dynAtten;
		}
		return staticAtten;
	}
	// PolyId path skips bias arithmetic entirely (identity test, no
	// depth comparison) — hoist the mode check above the slope-bias
	// math so PolyId mode pays nothing for slope it never uses.
	if (caFlags.shadowMode == ShadowMode::PolyId) {
		return CubeShadow_Sample(cubeIdx,
		                          sampleWorldX, sampleWorldY, sampleWorldZ,
		                          vx, vy, vz, /*constBias=*/0, /*slopeBias=*/0,
		                          surfaceMatId);
	}
	const float dotGeo = wx*nGeoX + wy*nGeoY + wz*nGeoZ;
	const float nDotL = dotGeo * lenInv;
	const float invNdotL = 1.0f / (nDotL > 0.2f ? nDotL : 0.2f);
	const int slopeBias = int(float(kSlopeBiasG) * (invNdotL - 1.0f));
	return CubeShadow_Sample(cubeIdx,
	                          sampleWorldX, sampleWorldY, sampleWorldZ,
	                          vx, vy, vz, kShadowBiasG, slopeBias,
	                          /*surfaceMatId=*/-1);
}

static void Render_DeferredLighting_Tile(const DeferredLightingCtx &ctx,
                                          int tileIndex,
                                          int x1, int y1, int x2, int y2)
{
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	// Hoist mode/global queries once per tile — a `getenv()`-backed
	// cached bool still costs a function call + load + test that the
	// compiler can't see through the std::function/lambda boundary.
	// 2M function calls/frame adds up.
	const bool quarter        = deferredLightingQuarterEnabled();
	const bool checker        = deferredLightingCheckerboardEnabled() && !quarter;
	const bool useVec         = deferredLightingVecEnabled();
	const bool specGlobalOn   = Specular_Factor > 0.0f;
	const bool profShadowCache = fds::FeatureFlags::shadow_prof_cache();

	// Per-stage ablation gates. Set one of these on to short-circuit the
	// stage so a bench harness can measure its cost from the frame-time
	// delta (see `--bench=scene@...`).
	const bool profNoTex    = fds::FeatureFlags::prof_no_tex();
	const bool profNoLights = fds::FeatureFlags::prof_no_lights();
	const bool profNoSpec   = fds::FeatureFlags::prof_no_spec();
	const bool profNoFog    = fds::FeatureFlags::prof_no_fog();

	// Hot-loop flag cache. These flags were being queried per-pixel +
	// per-light + per-PCF-tap, racking up ~1.5% CPU in profile data on
	// greets@t=2500. FeatureFlags::get is a cached load each, but a
	// per-pixel load × 1920×1080 × 4 lights × 4 PCF taps adds up. Hoist
	// to function entry.
#if FDS_DEV
	const bool nmapFromDiffuseG = fds::FeatureFlags::nmap_from_diffuse();
#else
	constexpr bool nmapFromDiffuseG = false;
#endif
	const bool nmapDisabledG    = fds::FeatureFlags::no_nmap();
	const bool deferredNoSpecG  = fds::FeatureFlags::deferred_no_spec();
	const int  kShadowBiasG     = fds::FeatureFlags::shadow_bias();
	const int  kSlopeBiasG      = fds::FeatureFlags::shadow_slope_bias();
	// Lightmap kernel branch is gated off when --shadow-dynamic is on
	// (see resolveCubeAtten's contract above). Hoisted to tile-level
	// so the per-pixel + per-omni hot path doesn't re-query.
	const bool lmKernelEnabled  = !fds::FeatureFlags::shadow_dynamic();
	// Normal-map LOD fade. The texture mip-chain averages cleanly, but
	// averaged normals shorten + rotate toward the surface average, so
	// at distance the bump's perturbation becomes high-frequency lighting
	// noise (hard edges on the floor especially). Fade nmX/nmY linearly
	// toward zero at high mips; nmZ unchanged so perturbed N smoothly
	// converges to the geometric N. Per-pixel cost: one int compare +
	// one float mul + one max — cheap vs the full TBN block.
	const int   nmapFadeStart = fds::FeatureFlags::nmap_lod_fade_start();
	const float nmapFadeStep  = fds::FeatureFlags::nmap_lod_fade_step();
	// Runtime mip-level debug knobs (toggled via N / Shift+N in REV.CPP).
	// Defined here and declared extern in Rev.h. Read once per tile entry
	// to avoid the atomic load on every pixel.
	const int  forceMipLevel = ::g_forceMipLevel.load(std::memory_order_relaxed);
	const bool vizMipLevel   = ::g_vizMipLevel.load(std::memory_order_relaxed);
	// Cube-tap flag bundle. resolveCubeAtten was reading 6 flags + 1
	// atomic per cube tap (1.44M taps/frame at greets t=500). Hoist to
	// tile-level: ~10 micros/frame back across all tile workers, and
	// removes a tail of L1 cold reads from the inner loop.
	const CubeAttenFlags caFlags{
	    /*shadowDynamicOn      */ fds::FeatureFlags::shadow_dynamic(),
	    /*lightmapPlanar       */ fds::FeatureFlags::shadow_lightmap_planar(),
	    /*lightmapNearest      */ fds::FeatureFlags::shadow_lightmap_nearest(),
	    /*lightmapRecomputeBake*/ fds::FeatureFlags::shadow_lightmap_recompute_bake(),
	    /*lightmapRecomputeBary*/ fds::FeatureFlags::shadow_lightmap_recompute_at_bary(),
	    /*profNoCubeTap        */ fds::FeatureFlags::prof_no_cube_tap(),
	    /*shadowMode           */ g_shadowMode.load(std::memory_order_relaxed),
	};

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			// Wave-1 of checkerboard: skip odd cells (filled by the
			// fill-pass after all wave-1 tiles complete).
			if (checker && ((px ^ py) & 1)) continue;
			if (quarter && ((px | py) & 1)) continue;  // shade only (even, even)

			const size_t i = size_t(py) * XRes + px;
			const word zEnc = ZPage16[i];
			if (zEnc == 0) continue;  // pixel not touched by Mekalele

			// Per-pixel mirror id (0 = original world, >0 = mirror N's
			// reflected world). The light-loop filters below skip any
			// omni whose mirrorId disagrees, so clone surfaces only
			// receive light from their own cloned omnis. If the mirror
			// id plane was never allocated (no mirrors in the scene),
			// pmid stays 0 and the filter is a no-op for the originals.
			const uint32_t pmid = gb.mirrorId.empty()
			    ? 0u : uint32_t(gb.mirrorId[i]);

			// Decode mat32 → matID, miplevel, swizzledUV.
			const uint32_t mat32 = gb.txtr[i];
			const uint32_t miplevel    = (mat32 >> 28) & 0xF;
			const uint32_t matID       = (mat32 >> 20) & 0xFF;
			const uint32_t swizzledUV  = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;
			// Force-mip (N key): override the value used for nmap-fade
			// math only. Texture sampling still uses the rasterizer-chosen
			// mip — overriding it there would break swizzledUV (which is
			// encoded against the chosen mip's dimensions) and produce
			// garbage. This way, the user can verify whether the nmap LOD
			// fade is wired correctly by forcing "as if mip 5" and seeing
			// whether the bump effect drops to zero on screen.
			const uint32_t miplevelForFade = (forceMipLevel >= 0)
			    ? uint32_t(forceMipLevel & 7) : miplevel;
			// Mip-level viz (Shift+N): paint each pixel by its raw
			// (rasterizer-chosen) miplevel. Suppresses texturing +
			// lighting; pure color = mip indicator.
			if (vizMipLevel) {
				static constexpr dword kMipPalette[8] = {
				    0xFFFF0000u, // 0 red
				    0xFFFF8000u, // 1 orange
				    0xFFFFFF00u, // 2 yellow
				    0xFF00FF00u, // 3 green
				    0xFF0080FFu, // 4 blue
				    0xFF4B00FFu, // 5 indigo
				    0xFF8000FFu, // 6 violet
				    0xFFFFFFFFu, // 7 white
				};
				out[i] = kMipPalette[miplevel & 7];
				continue;
			}

			// Resolved 16-bit ShadowMatID for the cube polyId path.
			// Source of truth is the per-pixel `gb.shadowMatID` plane
			// stamped by Mekalele (per-face resolution of
			// F->ShadowMatID / F->Txtr->ShadowMatID / Txtr->ID+1).
			// When the plane is empty (non-opaque renderers, or scenes
			// where the plane wasn't allocated), fall back to the
			// legacy uint16_t(matID+1) decoded from `txtr`.
			const int surfaceShadowId = gb.shadowMatID.empty()
			    ? int(matID + 1)
			    : int(gb.shadowMatID[i]);

			// Texture sample: Mekalele's apply_exact already wrote a
			// swizzled offset into mat32, so it's a direct lookup into
			// the mip's tile-major data. Mipmap[k] is byte*; texels are
			// dword (B,G,R,A in low→high bytes).
			// Cached-once-per-frame debug viz switches (FeatureFlags reads
			// env at startup; per-pixel cost is one bool load each).
#if FDS_DEV
			static const bool sVizTangent     = fds::FeatureFlags::viz_tangent();
			static const bool sVizNormal      = fds::FeatureFlags::viz_normal();
			static const bool sVizMatID       = fds::FeatureFlags::viz_matid();
			static const bool sVizPmid        = fds::FeatureFlags::viz_pmid();
			static const bool sNmapAsDiffuse  = fds::FeatureFlags::nmap_as_diffuse();
#else
			constexpr bool sVizTangent    = false;
			constexpr bool sVizNormal     = false;
			constexpr bool sVizMatID      = false;
			constexpr bool sVizPmid       = false;
			constexpr bool sNmapAsDiffuse = false;
#endif
			float texB, texG, texR;
			if (profNoTex) {
				texB = texG = texR = 128.0f;
			} else {
				const Texture *srcTex = Mat->Txtr;
				if (sNmapAsDiffuse && Mat->NormalMap) srcTex = Mat->NormalMap;
				const dword *texData = (const dword *)srcTex->Mipmap[miplevel];
				if (!texData) continue;
				const dword texel = texData[swizzledUV];
				texB = float(texel & 0xFF);
				texG = float((texel >> 8) & 0xFF);
				texR = float((texel >> 16) & 0xFF);
			}

			// Static-shadow lightmap address for this pixel — resolved
			// once, used by all cube-shadow taps in the per-omni loops
			// below via resolveCubeAtten().
			const PixelLightmap pixelLM = resolvePixelLightmap(gb, i, ctx.Sc);
			// Lightmap kernel branch is disabled when the dynamic-mesh
			// shadow pass is on: the lightmap only encodes the static-
			// occluder polyId, but `--shadow-dynamic` puts moving meshes
			// into a parallel buffer that only the runtime cube tap
			// reads. Until we composite both, fall back to cube tap
			// when dynamic is on so robot shadows on the floor still
			// render. (Set once per tile, see lmKernelEnabled below.)

			// Decode normal (view-space, unit-length).
			float nx, ny, nz;
			meka::oct_decode_u16(gb.normal[i], nx, ny, nz);
			// Save the geometric (vertex-interpolated, un-perturbed)
			// normal before the normal-map block below mutates nx/ny/nz.
			// Used by the shadow slope-bias so per-pixel bump-induced
			// N·L variance doesn't pollute the slope decision (which is
			// about the underlying surface geometry, not the apparent
			// micro-roughness from the bump map).
			const float nGeoX = nx, nGeoY = ny, nGeoZ = nz;

			// Object-space normal map: if the material ships one, sample
			// it via the same swizzled UV the diffuse uses, decode the
			// (R,G,B) bytes to a unit vector in world space, and replace
			// the geometric N with it. Static meshes only — for dynamic
			// meshes we'd need to transform from mesh-local to world via
			// a per-mesh RotMat, which we don't have at lighting time.
			//
			// FDS_NMAP_FROM_DIFFUSE=1: re-purposes the diffuse texel as
			// the normal map. Visually wrong but lets us measure the
			// per-pixel cost of the normal-map code path on existing
			// scenes without authoring or baking any new textures.
			Texture* nmTex = nmapDisabledG ? nullptr : Mat->NormalMap;
			if (!nmTex && nmapFromDiffuseG) nmTex = Mat->Txtr;
			if (nmTex) {
				const dword *nmData = (const dword *)nmTex->Mipmap[miplevel];
				if (nmData) {
					const dword nmTexel = nmData[swizzledUV];
					float nmX = (float((nmTexel >> 16) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					float nmY = (float((nmTexel >>  8) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					const float nmZ = (float( nmTexel        & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					// LOD-aware bump fade: scale (nmX, nmY) toward zero at
					// high mip. Uses miplevelForFade so the N-key override
					// can simulate "as if at higher mip" without touching
					// texture sampling. Fade starts AT nmapFadeStart so
					// picking start=0 starts cutting at mip 0 (1-step).
					if (int(miplevelForFade) >= nmapFadeStart) {
						const int   over = int(miplevelForFade) - nmapFadeStart + 1;
						const float fade = 1.0f - float(over) * nmapFadeStep;
						const float s = fade > 0.0f ? fade : 0.0f;
						nmX *= s; nmY *= s;
					}
					// Tier B tangent-space normal map. Reconstruct view-space
					// N from TBN where T is the per-pixel interpolated tangent
					// from the rasterizer's tangent G-buffer (Gram-Schmidt'd
					// vs N at write time; we re-orthogonalize defensively after
					// nlerp + oct-quant round-trip). When the tangent slot is
					// empty (transparent layers, or vertex tangent collapsed)
					// fall back to a Mikkelsen-style on-the-fly tangent so the
					// effect is still visible — visually wrong rotation but
					// the right per-pixel cost.
					float tx = 0, ty = 0, tz = 0;
					bool tangentValid = false;
					if (!gb.tangent.empty()) {
						const meka::u16 packedT = gb.tangent[i];
						if (packedT != 0) {
							meka::oct_decode_u16(packedT, tx, ty, tz);
							// Re-orthogonalize after the lossy oct round-trip.
							const float tDotN = tx*nx + ty*ny + tz*nz;
							tx -= nx * tDotN;
							ty -= ny * tDotN;
							tz -= nz * tDotN;
							const float tLen2 = tx*tx + ty*ty + tz*tz;
							if (tLen2 > 1e-12f) {
								const float invTLen = fast_rsqrt(tLen2);
								tx *= invTLen; ty *= invTLen; tz *= invTLen;
								tangentValid = true;
							}
						}
					}
					if (!tangentValid) {
						// Mikkelsen fallback: pick a reference axis not
						// parallel to N, T = normalize(cross(ref, N)).
						float refx, refy, refz;
						if (std::fabs(ny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
						else                       { refx = 1; refy = 0; refz = 0; }
						tx = refy * nz - refz * ny;
						ty = refz * nx - refx * nz;
						tz = refx * ny - refy * nx;
						const float invTLen = fast_rsqrt(tx*tx + ty*ty + tz*tz);
						tx *= invTLen; ty *= invTLen; tz *= invTLen;
					}
					// B = N × T
					const float bx = ny * tz - nz * ty;
					const float by = nz * tx - nx * tz;
					const float bz = nx * ty - ny * tx;
					// new_N = T * nmX + B * nmY + N * nmZ (view space)
					float vnx = tx * nmX + bx * nmY + nx * nmZ;
					float vny = ty * nmX + by * nmY + ny * nmZ;
					float vnz = tz * nmX + bz * nmY + nz * nmZ;
					float invLen = fast_rsqrt(vnx*vnx + vny*vny + vnz*vnz);
					nx = vnx * invLen;
					ny = vny * invLen;
					nz = vnz * invLen;
				}
			}

			// Reconstruct view-space position. ZPage16 stores
			// 0xFF80 - round(g_zscale * z), so:
			//   z = (0xFF80 - zEnc) / g_zscale
			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

			// Ambient. Same expression as forward Lighting() — except
			// here Mat is per-pixel, not per-mesh-Mat[0].
			float lB, lG, lR;
			if (Mat->Txtr) {
				lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;
			} else {
				lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * ctx.Sc->Ambient.R;
			}

			// View direction (pixel -> camera) in view space. Camera is at
			// the view-space origin, so view_dir = -pos / |pos|. Used by
			// the Blinn-Phong specular term below; cheap to skip for matte
			// materials. Glossiness == 0 + Specular > 0 means the FLD
			// authored a specular value but didn't set a Phong exponent;
			// fall back to a sensible mid-sharpness default so we still
			// see a highlight rather than a no-op.
			//
			// Specular_Factor gates the entire highlight pass, mirroring
			// forward's per-scene tone control (CITY/GREETS set it to 0
			// to suppress specular; FOUNTAIN/CRASH leave it nonzero). The
			// deferred path was previously emitting Blinn-Phong even when
			// the scene had explicitly disabled it, which over-brightened
			// City windows + Greets marble against the forward reference.
			// FDS_DEFERRED_NO_SPEC=1: force-disable all deferred specular,
			// regardless of per-material Mat->Specular. Diagnostic for
			// specular firefly / bump-map speckle isolation.
			const bool wantSpecular = !deferredNoSpecG
				&& specGlobalOn && (Mat->Specular > 0.0f) && !profNoSpec;
			const float gloss = Mat->Glossiness > 0
				? float(Mat->Glossiness)
				: 32.0f;
			float vx = 0, vy = 0, vz = 0;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = fast_rsqrt(vlen2);
				vx = -x * vlenInv;
				vy = -y * vlenInv;
				vz = -z * vlenInv;
			}

			// Specular accumulator — kept separate from diffuse so it can
			// be added AFTER texture modulation (highlights are independent
			// of base-color tint, which is the standard model).
			float sB = 0, sG = 0, sR = 0;

			// Water gets ambient-only (no omni accumulation, no spec).
			// Forward water mesh's BSphereRadius is tiny (Reflective_
			// Surface_Setup tessellates and scales the verts but never
			// recomputes the bsphere) and forward Lighting() skips any
			// omni whose `BSphereRadius + Range < |omni-bsphere_ctr|` —
			// which is basically all of them — so forward water vertex
			// light is just `Mat->Diffuse * Sc->Ambient`. Per-pixel
			// distance tests in the deferred path don't have that cull,
			// so without this skip every nearby city light slams into
			// every water pixel and saturates lB at 250.
			const bool isWater = (int(matID) == ctx.waterMatID);

			// Per-tile filtered light list — only omnis whose screen-
			// space bounding sphere overlaps this tile. For City with
			// ~100 omnis spread across the city, each tile typically
			// sees 10-20.
			const TileLights &tl = ctx.tileLights[tileIndex];

			// Per-pixel normal-map sampling lives only in the scalar
			// path. Force scalar for any pixel whose material has a
			// normal map (or when FDS_NMAP_FROM_DIFFUSE forces it for
			// every textured material). The geometric N from the
			// G-buffer would otherwise drive vec lighting and the
			// normal-map perturbation would be lost.
			const bool hasNormalMap = Mat->NormalMap ||
				(nmapFromDiffuseG && Mat->Txtr);
			const bool useVecHere = useVec && !hasNormalMap;

			// Sample's world-space position. Computed here (outside the
			// vec/scalar split) so both light paths can use it for cube
			// shadow lookup. One 3×3 transform + 3 adds per pixel; cached
			// across all light evaluations for this pixel.
			const float sampleWorldX =
				ctx.viewToWorld[0][0]*x + ctx.viewToWorld[0][1]*y +
				ctx.viewToWorld[0][2]*z + ctx.cameraWorldX;
			const float sampleWorldY =
				ctx.viewToWorld[1][0]*x + ctx.viewToWorld[1][1]*y +
				ctx.viewToWorld[1][2]*z + ctx.cameraWorldY;
			const float sampleWorldZ =
				ctx.viewToWorld[2][0]*x + ctx.viewToWorld[2][1]*y +
				ctx.viewToWorld[2][2]*z + ctx.cameraWorldZ;

			if (!isWater && !profNoLights) {
				if (useVecHere) {
					// 8-wide SIMD inner loop — written directly against
					// simde's _mm256_* intrinsics so we bypass
					// vectorclass's Vec8fb byte-packed mask layout.
					// vectorclass needs 6 NEON instructions per compare
					// to coerce the natural uint32x4_t mask into its
					// portable byte form (xtn → uzp1 → zip2 → ushll →
					// shl → cmlt) plus cross-domain GPR roundtrips for
					// `&`. simde's intrinsics produce masks in the
					// native AVX2 layout (all-1s/all-0s lanes, same as
					// what NEON `vcgeq_f32` returns), so `_mm256_and_ps`
					// is a direct AND with no conversion. simde lowers
					// each intrinsic to native NEON on arm64 / native
					// AVX2 on x86, giving us a single cross-platform
					// hot path.
					__m256 vx_v   = _mm256_set1_ps(x);
					__m256 vy_v   = _mm256_set1_ps(y);
					__m256 vz_v   = _mm256_set1_ps(z);
					__m256 vnx_v  = _mm256_set1_ps(nx);
					__m256 vny_v  = _mm256_set1_ps(ny);
					__m256 vnz_v  = _mm256_set1_ps(nz);
					__m256 vDiff  = _mm256_set1_ps(Mat->Diffuse);
					__m256 vZero  = _mm256_setzero_ps();
					__m256 vOne   = _mm256_set1_ps(1.0f);
					__m256 accB   = _mm256_setzero_ps();
					__m256 accG   = _mm256_setzero_ps();
					__m256 accR   = _mm256_setzero_ps();

					__m256i pmid_v_main = _mm256_set1_epi32((int)pmid);
					for (int slot = 0; slot < tl.paddedCount; slot += 8) {
						__m256 lpx = _mm256_load_ps(tl.posX   + slot);
						__m256 lpy = _mm256_load_ps(tl.posY   + slot);
						__m256 lpz = _mm256_load_ps(tl.posZ   + slot);
						__m256 lcb = _mm256_load_ps(tl.colB   + slot);
						__m256 lcg = _mm256_load_ps(tl.colG   + slot);
						__m256 lcr = _mm256_load_ps(tl.colR   + slot);
						__m256 lr2 = _mm256_load_ps(tl.range2 + slot);
						__m256 lrr = _mm256_load_ps(tl.rRange + slot);
						// Mirror filter: only lights whose mirrorId matches
						// the pixel's contribute. Padded slots carry
						// 0xffffffff so they never match (pmid is < 256).
						__m256i lmid = _mm256_load_si256(
							(const __m256i*)(tl.mirrorId + slot));
						__m256 mirrorMask = _mm256_castsi256_ps(
							_mm256_cmpeq_epi32(lmid, pmid_v_main));

						__m256 wx = _mm256_sub_ps(lpx, vx_v);
						__m256 wy = _mm256_sub_ps(lpy, vy_v);
						__m256 wz = _mm256_sub_ps(lpz, vz_v);
						__m256 dot = _mm256_fmadd_ps(wx, vnx_v,
						              _mm256_fmadd_ps(wy, vny_v,
						               _mm256_mul_ps(wz, vnz_v)));
						__m256 len2 = _mm256_fmadd_ps(wx, wx,
						               _mm256_fmadd_ps(wy, wy,
						                _mm256_mul_ps(wz, wz)));

						__m256 mask_range = _mm256_cmp_ps(len2, lr2,   _CMP_LE_OQ);
						__m256 mask_dot   = _mm256_cmp_ps(dot,  vZero, _CMP_GE_OQ);
						__m256 mask_pos   = _mm256_cmp_ps(len2, vZero, _CMP_GT_OQ);
						__m256 mask = _mm256_and_ps(mask_range,
						               _mm256_and_ps(mask_dot,
						                _mm256_and_ps(mask_pos, mirrorMask)));

						__m256 safe_len2 = _mm256_blendv_ps(vOne, len2, mask);
						__m256 lenInv = _mm256_rsqrt_ps(safe_len2);
						__m256 dist   = _mm256_mul_ps(safe_len2, lenInv);
						__m256 falloff = _mm256_sub_ps(vOne, _mm256_mul_ps(dist, lrr));
						__m256 k = _mm256_mul_ps(_mm256_mul_ps(dot, lenInv), falloff);

						// Spot cone attenuation. For lanes where isSpot==0
						// coneAtten=1 (omni). For spot lanes: smoothstep
						// from cosOuter→cosInner of cosTheta=-dot(Dir,w)/|w|.
						__m256 ldx = _mm256_load_ps(tl.dirX     + slot);
						__m256 ldy = _mm256_load_ps(tl.dirY     + slot);
						__m256 ldz = _mm256_load_ps(tl.dirZ     + slot);
						__m256 lci = _mm256_load_ps(tl.cosInner + slot);
						__m256 lco = _mm256_load_ps(tl.cosOuter + slot);
						__m256i lis = _mm256_load_si256((const __m256i*)(tl.isSpot + slot));
						__m256 dirDotW = _mm256_fmadd_ps(ldx, wx,
						                  _mm256_fmadd_ps(ldy, wy,
						                   _mm256_mul_ps(ldz, wz)));
						__m256 cosTheta = _mm256_mul_ps(
						                    _mm256_sub_ps(vZero, dirDotW),
						                    lenInv);
						__m256 maskInside = _mm256_cmp_ps(cosTheta, lco, _CMP_GT_OQ);
						__m256 rangeRcp = _mm256_div_ps(vOne, _mm256_sub_ps(lci, lco));
						__m256 t = _mm256_mul_ps(_mm256_sub_ps(cosTheta, lco), rangeRcp);
						t = _mm256_min_ps(_mm256_max_ps(t, vZero), vOne);
						// smoothstep(t) = t*t*(3-2t)
						__m256 smooth = _mm256_mul_ps(_mm256_mul_ps(t, t),
						                  _mm256_sub_ps(_mm256_set1_ps(3.0f),
						                                _mm256_mul_ps(_mm256_set1_ps(2.0f), t)));
						// coneAtten = isSpot ? (maskInside ? smooth : 0) : 1
						__m256 spotAtten = _mm256_and_ps(maskInside, smooth);
						__m256 isSpotMask = _mm256_castsi256_ps(
						                     _mm256_cmpgt_epi32(lis, _mm256_setzero_si256()));
						__m256 coneAtten = _mm256_blendv_ps(vOne, spotAtten, isSpotMask);
						k = _mm256_mul_ps(k, coneAtten);

						// Cube shadow attenuation per lane (scalarized).
						// Most lanes have cubeShadowIdx < 0 (no shadow);
						// quick-check the SoA before paying the lookup cost.
						// Per-pixel sampleWorld is already computed for the
						// scalar path; bring it here too via a sibling
						// computation in the vec branch's setup.
						__m256i cubeIdxV = _mm256_load_si256(
							(const __m256i*)(tl.cubeShadowIdx + slot));
						__m256i anyCube = _mm256_cmpgt_epi32(cubeIdxV,
							_mm256_set1_epi32(-1));
						if (_mm256_movemask_epi8(anyCube) != 0) {
							alignas(32) int32_t idxArr[8];
							_mm256_store_si256((__m256i*)idxArr, cubeIdxV);
							alignas(32) float kArr[8];
							_mm256_store_ps(kArr, k);
							alignas(32) float wxArr[8], wyArr[8], wzArr[8], liArr[8];
							_mm256_store_ps(wxArr, wx);
							_mm256_store_ps(wyArr, wy);
							_mm256_store_ps(wzArr, wz);
							_mm256_store_ps(liArr, lenInv);
							const int kSB = kShadowBiasG;
							const int kSL = kSlopeBiasG;
							for (int lane = 0; lane < 8; ++lane) {
								if (idxArr[lane] < 0) continue;
								if (kArr[lane] <= 0.0f) continue;
								const float cubeAtten = resolveCubeAtten(
									pixelLM, idxArr[lane], lmKernelEnabled, caFlags,
									wxArr[lane], wyArr[lane], wzArr[lane], liArr[lane],
									nGeoX, nGeoY, nGeoZ,
									sampleWorldX, sampleWorldY, sampleWorldZ,
									x, y, z, kSB, kSL,
									surfaceShadowId);
								kArr[lane] *= cubeAtten;
							}
							k = _mm256_load_ps(kArr);
						}

						__m256 intensity = _mm256_blendv_ps(vZero,
						                    _mm256_mul_ps(k, vDiff), mask);

						accB = _mm256_fmadd_ps(intensity, lcb, accB);
						accG = _mm256_fmadd_ps(intensity, lcg, accG);
						accR = _mm256_fmadd_ps(intensity, lcr, accR);
					}
					// Horizontal reduction: AVX2 has no scalar reduce;
					// store + scalar add is what vectorclass does
					// internally too. 24 scalar adds total per pixel
					// — negligible vs the omni loop body.
					alignas(32) float bufB[8], bufG[8], bufR[8];
					_mm256_store_ps(bufB, accB);
					_mm256_store_ps(bufG, accG);
					_mm256_store_ps(bufR, accR);
					for (int i = 0; i < 8; ++i) {
						lB += bufB[i];
						lG += bufG[i];
						lR += bufR[i];
					}
					// Vec-spec path: replaces the previous scalar pow loop.
					// Dispatch on Mat->Glossiness so the inner loop uses a
					// constant squaring sequence (bit-exact pow for integer
					// gloss, ~5-7 fmuls vs ~50-80 cycles for libm pow).
					// FDS_DEFERRED_GLOSS_STATS confirms our FLDs only ship
					// gloss ∈ {48, 64}; the other cases here are defensive
					// for spectest's authored values and future FLDs.
					if (wantSpecular) {
						switch (Mat->Glossiness) {
							case 4:
								run_vec_spec_loop<4>  (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, sB,sG,sR); break;
							case 8:
								run_vec_spec_loop<8>  (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, sB,sG,sR); break;
							case 16:
								run_vec_spec_loop<16> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, sB,sG,sR); break;
							case 32:
								run_vec_spec_loop<32> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, sB,sG,sR); break;
							case 48:
								run_vec_spec_loop<48> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, sB,sG,sR); break;
							case 64:
								run_vec_spec_loop<64> (tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, sB,sG,sR); break;
							case 128:
								run_vec_spec_loop<128>(tl, x,y,z, nx,ny,nz, vx,vy,vz, Mat->Specular, pmid, sB,sG,sR); break;
							default:
								// Unknown gloss — defensive scalar libm path
								// (matches old behavior). If this fires on a
								// hot scene, add a `case` for the value.
								for (int n = 0; n < tl.count; ++n) {
									if (tl.mirrorId[n] != pmid) continue;
									const float Lpx = tl.posX[n];
									const float Lpy = tl.posY[n];
									const float Lpz = tl.posZ[n];
									const float wx = Lpx - x;
									const float wy = Lpy - y;
									const float wz = Lpz - z;
									const float dot = wx*nx + wy*ny + wz*nz;
									if (dot < 0.0f) continue;
									const float len2 = wx*wx + wy*wy + wz*wz;
									const float r2 = tl.range2[n];
									if (len2 > r2 || len2 == 0.0f) continue;
									const float lenInv = fast_rsqrt(len2);
									const float dist   = len2 * lenInv;
									const float ldx = wx * lenInv;
									const float ldy = wy * lenInv;
									const float ldz = wz * lenInv;
									const float hx = ldx + vx, hy = ldy + vy, hz = ldz + vz;
									const float hLen2 = hx*hx + hy*hy + hz*hz;
									if (hLen2 <= 0.0f) continue;
									// Fold renorm into the dot (same trick as
									// the nmap path + vec spec loop).
									const float NdotH_raw = nx*hx + ny*hy + nz*hz;
									if (NdotH_raw <= 0.0f) continue;
									const float NdotH = NdotH_raw * fast_rsqrt(hLen2);
									// pow(NdotH, gloss) via LUT-based log2/exp2.
									// std::pow on arm64 libm = ~50-100 cycles
									// per call; fastPow2(gloss*fastLog2(NdotH))
									// is ~10. Used only when Mat->Glossiness
									// falls outside the templated values {4,
									// 8, 16, 32, 48, 64, 128}.
									const float spec = fastPow2(gloss * fastLog2(NdotH));
									const float rRange = tl.rRange[n];
									const float specStrength = spec * Mat->Specular *
										(1.0f - dist * rRange);
									sB += specStrength * tl.colB[n];
									sG += specStrength * tl.colG[n];
									sR += specStrength * tl.colR[n];
								}
								break;
						}
					}
				} else {
					// Scalar path (default). Per-omni early-out on
					// dot/range tests; branch predictor learns the
					// "most filtered omnis still out of range" pattern.
					// `dist = len2 * lenInv` (= sqrt(len2)) avoids a
					// second fdiv vs `1/lenInv` — fdiv on arm64 is
					// ~20 cycles vs fmul's 4. The compiler doesn't do
					// this rewrite under strict FP, so spell it out.
					// sampleWorldX/Y/Z hoisted to the parent scope —
					// used by cube shadow sampling for any omni with
					// cubeShadowIdx >= 0.
					for (int n = 0; n < tl.count; ++n) {
						if (tl.mirrorId[n] != pmid) continue;
						const float Lpx = tl.posX[n];
						const float Lpy = tl.posY[n];
						const float Lpz = tl.posZ[n];
						const float wx = Lpx - x;
						const float wy = Lpy - y;
						const float wz = Lpz - z;
						const float dot = wx * nx + wy * ny + wz * nz;
						if (dot < 0.0f) continue;
						const float len2 = wx*wx + wy*wy + wz*wz;
						const float r2 = tl.range2[n];
						if (len2 > r2) continue;
						const float lenInv = fast_rsqrt(len2);
						const float dist   = len2 * lenInv;
						const float rRange = tl.rRange[n];
						float k = dot * lenInv * (1.0f - dist * rRange);
						// Spot cone: matches the vec body's coneAtten so nmap
						// pixels (which all flow through this scalar path)
						// don't render the robot spotlight as an omni.
						if (tl.isSpot[n]) {
							const float cosTheta = -(tl.dirX[n]*wx + tl.dirY[n]*wy + tl.dirZ[n]*wz) * lenInv;
							if (cosTheta <= tl.cosOuter[n]) continue;
							if (cosTheta < tl.cosInner[n]) {
								const float ct = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
								k *= ct * ct * (3.0f - 2.0f * ct);
							}
						}
						// Shadow test. Project pixel view-space pos to the
						// shadow map's screen+depth; attenuate the light
						// by the occlusion fraction. Cheap branch when
						// smIdx<0 (no shadow).
						//
						// PCF with bilinear-weighted 2×2: sample 4 neighbouring
						// shadow-map texels and blend the per-texel
						// occlusion by fractional (smX, smY) — continuous
						// in light-space position, no integer-snap when
						// the light camera moves a sub-texel between
						// frames. Without bilinear weighting whole
						// polygons that project to a single texel flicker
						// in/out as the texel-grid shifts under them.
						// Cost: 4 loads + 4 compares + 4 muls + 4 adds.
						const int32_t smIdx = tl.shadowMapIdx[n];
						float shadowAtten = 1.0f;
						if (smIdx >= 0 && size_t(smIdx) < g_shadowMaps.size()) {
							const ShadowMap& sm = g_shadowMaps[smIdx];
							const float lx = sm.viewToLight[0][0] * x +
							                 sm.viewToLight[0][1] * y +
							                 sm.viewToLight[0][2] * z +
							                 sm.viewToLightOffset.x;
							const float ly = sm.viewToLight[1][0] * x +
							                 sm.viewToLight[1][1] * y +
							                 sm.viewToLight[1][2] * z +
							                 sm.viewToLightOffset.y;
							const float lz = sm.viewToLight[2][0] * x +
							                 sm.viewToLight[2][1] * y +
							                 sm.viewToLight[2][2] * z +
							                 sm.viewToLightOffset.z;
							if (lz > 0.0f) {
								const float invLZ = 1.0f / lz;
								const float smX = sm.cntrX + sm.perspX * lx * invLZ;
								const float smY = sm.cntrY - sm.perspY * ly * invLZ;
								const int iX = int(smX);
								const int iY = int(smY);
								if (iX >= 0 && iX + 1 < sm.xres &&
								    iY >= 0 && iY + 1 < sm.yres) {
									const size_t rowOfs = size_t(iY) * size_t(sm.xres);
									const uint16_t *zsRow0 = sm.depth.data() + rowOfs;
									const uint16_t *zsRow1 = zsRow0 + sm.xres;
									const uint16_t *zdRow0 = sm.depth_dynamic.data() + rowOfs;
									const uint16_t *zdRow1 = zdRow0 + sm.xres;
									// Per-tap closest-occluder. Static buffer
									// holds the once-baked statics; dynamic
									// holds animated meshes (zero when off).
									// max() wins on whichever caster is closer.
									const uint16_t z00 = std::max(zsRow0[iX  ], zdRow0[iX  ]);
									const uint16_t z10 = std::max(zsRow0[iX+1], zdRow0[iX+1]);
									const uint16_t z01 = std::max(zsRow1[iX  ], zdRow1[iX  ]);
									const uint16_t z11 = std::max(zsRow1[iX+1], zdRow1[iX+1]);
									if (profShadowCache) {
										// One PCF check = one tracked sample.
										// Use the (00) tap's cache-line address
										// — adjacent shadow checks on the same
										// thread that share this line are hits.
										const uintptr_t line =
											reinterpret_cast<uintptr_t>(&zsRow0[iX]) >> 6;
										if (s_shadowProfLastLine != line) {
											g_shadowProfLineTransitions.fetch_add(1, std::memory_order_relaxed);
											s_shadowProfLastLine = line;
										}
										g_shadowProfSamples.fetch_add(1, std::memory_order_relaxed);
									}
									const float fx = smX - float(iX);
									const float fy = smY - float(iY);
									const float w00 = (1.0f - fx) * (1.0f - fy);
									const float w10 =         fx  * (1.0f - fy);
									const float w01 = (1.0f - fx) *         fy;
									const float w11 =         fx  *         fy;
									const ShadowMode mode = g_shadowMode.load(std::memory_order_relaxed);
									float occ = 0.0f;
									if (mode == ShadowMode::PolyId) {
										const uint16_t *idRow0 = sm.polyId.data() +
											size_t(iY) * size_t(sm.xres);
										const uint16_t *idRow1 = idRow0 + sm.xres;
										// Surface matID extracted from gb.txtr's
										// packed (miplevel:4 | matID:8 | swizzledUV:20).
										// Shadow buffer stores matID+1 of the closest
										// occluder; +1 here too so the comparison
										// uses the same offset, and 0 stays as the
										// "no occluder" sentinel.
										// Receiver identity: read the per-pixel
										// `gb.shadowMatID` plane (stamped by
										// Mekalele with the full resolution
										// chain). Falls back to uint16_t(matID+1)
										// when the plane wasn't allocated.
										// Mirrors the scalar surfaceShadowId
										// resolution above.
										const uint16_t surfaceId = uint16_t(surfaceShadowId);
										if (idRow0[iX    ] != surfaceId && idRow0[iX    ] != 0) occ += w00;
										if (idRow0[iX + 1] != surfaceId && idRow0[iX + 1] != 0) occ += w10;
										if (idRow1[iX    ] != surfaceId && idRow1[iX    ] != 0) occ += w01;
										if (idRow1[iX + 1] != surfaceId && idRow1[iX + 1] != 0) occ += w11;
									} else {
										int pixZenc = 0xFF80 - int(lz * sm.zScale);
										if (pixZenc < 0) pixZenc = 0;
										if (pixZenc > 0xFFFF) pixZenc = 0xFFFF;
										// Constant + slope-scale bias. The constant
										// handles front-facing self-shadow acne. The
										// slope term grows as the surface meets the
										// light at shallow angles — a wall almost
										// parallel to the light direction has a steep
										// depth gradient across one shadow texel, so
										// the constant bias alone leaves grazing
										// surfaces with banded "acne" patterns. We
										// scale by 1/(N·L) - 1 so front-facing surfaces
										// get no extra; grazing surfaces get a lot.
										// Tunable via FDS_SHADOW_BIAS / SLOPE_BIAS.
										const int kShadowBias = kShadowBiasG;
										const int kSlopeBias  = kSlopeBiasG;
										// Use the GEOMETRIC normal (saved before normal-
										// map perturbation) for slope calc — the slope
										// is about the underlying surface, not the
										// bumpy micro-detail. Without this, bump-mapped
										// floors show patchy shadow following the bumps,
										// and grazing-angle bumps explode the slope.
										// Clamp at 0.2 so very steep angles plateau
										// rather than blowing past nearby occluders.
										const float dotGeo = wx*nGeoX + wy*nGeoY + wz*nGeoZ;
										const float nDotL = dotGeo * lenInv;
										const float invNdotL = 1.0f / (nDotL > 0.2f ? nDotL : 0.2f);
										const int slopeBias = int(float(kSlopeBias) * (invNdotL - 1.0f));
										const int biased = pixZenc + kShadowBias + slopeBias;
										if (biased < int(z00)) occ += w00;
										if (biased < int(z10)) occ += w10;
										if (biased < int(z01)) occ += w01;
										if (biased < int(z11)) occ += w11;
									}
									if (occ >= 1.0f) continue;       // fully shadowed
									shadowAtten = 1.0f - occ;
								}
							}
						}

						// Cube shadow (omni shadow caster). Two paths:
						//  - Static-mesh pixel + lightmap baked for this
						//    cubeIdx → bilinear sample from the lightmap.
						//  - Otherwise → per-pixel cube tap.
						const int32_t cubeIdx = tl.cubeShadowIdx[n];
						if (cubeIdx >= 0) {
							const float cubeAtten = resolveCubeAtten(
								pixelLM, cubeIdx, lmKernelEnabled, caFlags,
								wx, wy, wz, lenInv,
								nGeoX, nGeoY, nGeoZ,
								sampleWorldX, sampleWorldY, sampleWorldZ,
								x, y, z, kShadowBiasG, kSlopeBiasG,
								surfaceShadowId);
							if (cubeAtten <= 0.0f) continue;
							shadowAtten *= cubeAtten;
						}
						const float intensity = k * Mat->Diffuse * shadowAtten;
						const float Lcb = tl.colB[n];
						const float Lcg = tl.colG[n];
						const float Lcr = tl.colR[n];
						lB += intensity * Lcb;
						lG += intensity * Lcg;
						lR += intensity * Lcr;

						if (wantSpecular) {
							const float ldx = wx * lenInv;
							const float ldy = wy * lenInv;
							const float ldz = wz * lenInv;
							const float hx = ldx + vx;
							const float hy = ldy + vy;
							const float hz = ldz + vz;
							const float hLen2 = hx*hx + hy*hy + hz*hz;
							// dot(N, H_unit) = dot(N, H_raw) * rsqrt(|H_raw|²).
							// Saves 3 muls per lit pixel vs renormalizing H
							// first; positive rsqrt preserves NdotH's sign so
							// the > 0 cull still works.
							const float NdotH_raw = nx*hx + ny*hy + nz*hz;
							if (hLen2 > 0.0f && NdotH_raw > 0.0f) {
								const float NdotH = NdotH_raw * fast_rsqrt(hLen2);
								{
									const float spec = pow_glossClass(NdotH, Mat->Glossiness);
									// Multiply by shadowAtten so shadowed pixels don't
									// leak specular highlights — was a visible bug at
									// bumped-mortar pixels inside shadow regions, where
									// the bumped N satisfies the sharp Gloss=48 lobe
									// while diffuse was correctly killed.
									const float specStrength = spec * Mat->Specular *
										(1.0f - dist * rRange) * shadowAtten;
									sB += specStrength * Lcb;
									sG += specStrength * Lcg;
									sR += specStrength * Lcr;
								}
							}
						}
					}
				}
			}

			// Saturation cap (matches forward at 250).
			if (lB > 250.0f) lB = 250.0f;
			if (lG > 250.0f) lG = 250.0f;
			if (lR > 250.0f) lR = 250.0f;
			if (lB < 0.0f)   lB = 0.0f;
			if (lG < 0.0f)   lG = 0.0f;
			if (lR < 0.0f)   lR = 0.0f;

			// Fog moved to Render_DeferredFogPass — a single post-lighting
			// full-screen pass keeps the lighting kernel SIMD-clean and
			// drops the per-pixel sqrt. Forward TheOtherBarry still
			// applies its own per-vertex fog (TheOtherBarry.h:716-734)
			// for reflective windows / additive fountain vortex; those
			// pixels are skipped by the fog pass via the mat32 sentinel.

			// Diffuse modulation: pixel = texel * light / 256.
			// Reflective faces are dispatched to TheOtherBarry<OVERWRITE,
			// TEXTURETEXTURE> in RenderInnerMekalele and never reach this
			// code path — they're rendered by the forward filler (which
			// does the env+tex/2 composite using forward's per-vertex
			// interpolated eu/ev) and skipped here via the mat32 sentinel.
			float fdB = (texB * lB) * (1.0f / 256.0f);
			float fdG = (texG * lG) * (1.0f / 256.0f);
			float fdR = (texR * lR) * (1.0f / 256.0f);
			int outB = int(fdB) + int(sB);
			int outG = int(fdG) + int(sG);
			int outR = int(fdR) + int(sR);

			// Water-mesh transparent blend. Forward draws the water plane
			// with TheOtherBarry<TRANSPARENT> after a pass-1 mirrored-world
			// draw + dispMap distortion has populated VPage with a wavy
			// reflection of the city. The transparent filler does
			//   pixel = saturate(lit_water_texel + existing_VPage/2)
			// (TheOtherBarry.h:392). We reproduce the same blend here:
			// the existing VPage value at this pixel is the reflection
			// preserved across the inter-pass Z-clear (which lets pass-2
			// deferred shading skip non-water-mesh pixels via zEnc check
			// on the freshly-cleared depth buffer).
			if (isWater) {
				const dword existing = out[i];
				const int rB = int(existing & 0xFF);
				const int rG = int((existing >> 8) & 0xFF);
				const int rR = int((existing >> 16) & 0xFF);
				outB += rB >> 1;
				outG += rG >> 1;
				outR += rR >> 1;
			}

			// FDS_VIZ_NORMAL / FDS_VIZ_TANGENT: stomp final output with
			// a (vec+1)*127.5 visualization. nx/ny/nz here is post-TBN
			// (perturbed by the normal map); per-pixel tangent is decoded
			// fresh from the G-buffer (the in-kernel `tx`/`ty`/`tz` is
			// scoped inside the nmap branch). Use to verify the per-vertex
			// / per-pixel tangent path produces a coherent field.
			if (sVizNormal) {
				outR = int((nx + 1.0f) * 127.5f);
				outG = int((ny + 1.0f) * 127.5f);
				outB = int((nz + 1.0f) * 127.5f);
			}
			if (sVizMatID) {
				// Hash matID to a distinct colour. matID is already in
				// scope from the mat32 unpack above; for matID==0 we
				// keep pure black so untouched/sentinel pixels read
				// distinctly. Multiplication by 3 large primes mixes
				// adjacent ids into clearly different colours.
				const uint32_t h = uint32_t(matID);
				outR = int((h * 73u + 41u) & 0xFFu);
				outG = int((h * 151u + 13u) & 0xFFu);
				outB = int((h * 211u + 97u) & 0xFFu);
			}
			if (sVizPmid) {
				// Hash pmid (gb.mirrorId post-commit, = which mirror
				// context owns this pixel). pmid==0 → black (originals);
				// nonzero → per-mirror colour. Lets us distinguish the
				// reflected floor (pmid > 0) from the original floor
				// (pmid == 0) — matID can't, since clones share Mat.
				const uint32_t h = uint32_t(pmid);
				if (h == 0u) {
					outR = outG = outB = 0;
				} else {
					outR = int((h * 73u + 41u) & 0xFFu);
					outG = int((h * 151u + 13u) & 0xFFu);
					outB = int((h * 211u + 97u) & 0xFFu);
				}
			}
			if (sVizTangent) {
				if (!gb.tangent.empty()) {
					const meka::u16 packedT = gb.tangent[i];
					if (packedT != 0) {
						float vtx, vty, vtz;
						meka::oct_decode_u16(packedT, vtx, vty, vtz);
						outR = int((vtx + 1.0f) * 127.5f);
						outG = int((vty + 1.0f) * 127.5f);
						outB = int((vtz + 1.0f) * 127.5f);
					} else {
						outR = outG = outB = 0;   // degenerate tangent → black
					}
				}
			}

			if (outB > 255) outB = 255;
			if (outG > 255) outG = 255;
			if (outR > 255) outR = 255;
			if (outB < 0)   outB = 0;
			if (outG < 0)   outG = 0;
			if (outR < 0)   outR = 0;

			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}

	// One permit per completed tile (see renderns::tileDone in RENDER.CPP).
	renderns::tileDone.release();
}

// Per-pixel deferred lighting for transparent (front-facing) surfaces.
// Reads the transparent G-buffer (mat32 + normal + xpr-Z, populated by
// MekaleleTransparent in RenderInnerDeferredTransparent for the closest
// front-facing transparent at each pixel). Computes per-pixel ambient
// + Lambertian + fog, modulates the texture sample, alpha-blends 50/50
// onto VPage (matches TheOtherBarry<TRANSPARENT>'s blend rule). Specular
// + checkerboard / quarter-rate / vec paths omitted — transparent
// coverage is small, scalar is fine until profiling says otherwise.
// Templated on which transparent layer to light: Front reads
// g_gbufferTransparent + g_xparZ; Back reads g_gbufferTransparentBack +
// g_xparZBack. Both kernels are identical apart from the buffer pointers
// — kept as one function body so they don't drift.
enum class XparLayer { Front, Back };

template <XparLayer Layer>
static void Render_DeferredTransparentLighting_Tile(const DeferredLightingCtx &ctx,
                                                     int tileIndex,
                                                     int x1, int y1, int x2, int y2)
{
	meka::GBuffer *gbPtr;
	uint16_t      *zPtr;
	if constexpr (Layer == XparLayer::Front) {
		gbPtr = g_gbufferTransparent;
		zPtr  = g_xparZ;
	} else {
		gbPtr = g_gbufferTransparentBack;
		zPtr  = g_xparZBack;
	}
	if (!gbPtr || !zPtr) return;
	const meka::GBuffer &gbX = *gbPtr;
	uint16_t *xparZ = zPtr;
	dword *out = reinterpret_cast<dword *>(VPage);
	// Conservative per-pixel light cull: bypasses the per-tile filter,
	// useful for diagnosing whether observed lighting steps come from
	// culling drift at tile seams.
	const bool lightAll = fds::FeatureFlags::xpar_light_all();
	const TileLights &tlTile = ctx.tileLights[tileIndex];
	const ViewLightsSoA *vlAll = ctx.lights;
	const int allCount = ctx.numLights;

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			const size_t i = size_t(py) * XRes + px;
			const uint32_t mat32 = gbX.txtr[i];
			if (mat32 == 0xFFFFFFFFu) continue;  // no transparent front
			const word zEnc = xparZ[i];
			if (zEnc == 0) continue;
			// Per-pixel mirror id from the transparent layer's plane.
			// Used to gate the light loops below (clone surfaces should
			// see only their own mirror's omnis).
			const uint32_t pmid = gbX.mirrorId.empty()
			    ? 0u : uint32_t(gbX.mirrorId[i]);
			// Opaque-Z occlusion: if opaque ZPage16 has a larger z_candidate
			// at this pixel, the opaque surface is CLOSER to camera than
			// the transparent we wrote into xpr. Reject the transparent —
			// it's hidden behind opaque. MekaleleTransparent doesn't see
			// opaque Z during raster (its zbuffer points at xpr-Z) so we
			// have to gate here. Without this, transparent surfaces show
			// through opaque walls (Greets banding behind marble).
			const word zOpaque = ZPage16[i];
			if (zOpaque > zEnc) continue;

			const uint32_t miplevel   = (mat32 >> 28) & 0xF;
			const uint32_t matID      = (mat32 >> 20) & 0xFF;
			const uint32_t swizzledUV = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;
			// Negative XparBlendAlpha sentinel: skip this pixel
			// entirely so out[i] (= the opaque shading already written
			// by the opaque pass) is preserved. Used by mirror walls
			// — the wall rasterised into xpar only to bound the mask
			// + xparZ, but contributes no colour and must NOT pass
			// through the legacy `litRGB + dst/2` composition (which
			// halves the reflected clones behind it).
			if (Mat->XparBlendAlpha < 0.0f) continue;

			// Resolved 16-bit ShadowMatID — see scalar path comment above.
			// The xpar G-buffer (gbX) does not currently allocate the
			// shadowMatID plane (xpar surfaces don't participate in
			// PolyId shadows much), so this falls through to the
			// matID+1 path unless someone adds the plane later.
			const int surfaceShadowId = gbX.shadowMatID.empty()
			    ? int(matID + 1)
			    : int(gbX.shadowMatID[i]);

			const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
			if (!texData) continue;
			const dword texel = texData[swizzledUV];
			const float texB = float(texel & 0xFF);
			const float texG = float((texel >> 8) & 0xFF);
			const float texR = float((texel >> 16) & 0xFF);

			float nx, ny, nz;
			meka::oct_decode_u16(gbX.normal[i], nx, ny, nz);

			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

			float lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
			float lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
			float lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;

			// Crystal specular for transparent surfaces (e.g. fountain
			// mizraka glass). Same Blinn-Phong as the opaque kernel; gated
			// on Mat->Specular > 0 so non-spec transparents (water, spire
			// orbs) pay zero. Accumulated into sB/sG/sR alongside diffuse
			// and added on top of the texture-modulated lit color (spec
			// highlights aren't tinted by the base texture — standard
			// model).
			const bool wantSpecular = (Mat->Specular > 0.0f);
			const float gloss = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
			float vx_v = 0, vy_v = 0, vz_v = 0;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = fast_rsqrt(vlen2);
				vx_v = -x * vlenInv;
				vy_v = -y * vlenInv;
				vz_v = -z * vlenInv;
			}
			float sB = 0, sG = 0, sR = 0;

			// Water gets ambient-only — forward Lighting() culls every omni
			// (water mesh has a tiny BSphereRadius from Reflective_Surface_
			// Setup's tessellation, so range+radius < |omni-bsphere| for all
			// city omnis). Without skipping the omni loop here, every nearby
			// city light slams into every water pixel and saturates the
			// channel → cyan/turquoise water instead of forward's deep blue.
			// Mirrors the same skip in Render_DeferredLighting_Tile.
			const bool isWater = (int(matID) == ctx.waterMatID);

			// Light loop. Default: per-tile compacted list. Diagnostic
			// mode: full scene omni list (FDS_XPAR_LIGHT_ALL=1).
			if (isWater) {
				// Skip omni accumulation — matches the opaque kernel's
				// isWater guard.
			} else if (lightAll) {
				for (int n = 0; n < allCount; ++n) {
					if (vlAll->mirrorId[n] != pmid) continue;
					const float wx = vlAll->posX[n] - x;
					const float wy = vlAll->posY[n] - y;
					const float wz = vlAll->posZ[n] - z;
					const float dot = wx * nx + wy * ny + wz * nz;
					if (dot < 0.0f) continue;
					const float len2 = wx*wx + wy*wy + wz*wz;
					if (len2 > vlAll->range2[n]) continue;
					const float lenInv = fast_rsqrt(len2);
					const float dist   = len2 * lenInv;
					const float falloff = (1.0f - dist * vlAll->rRange[n]);
					float k = dot * lenInv * falloff;
					if (vlAll->isSpot[n]) {
						const float cosTheta = -(vlAll->dirX[n]*wx + vlAll->dirY[n]*wy + vlAll->dirZ[n]*wz) * lenInv;
						if (cosTheta <= vlAll->cosOuter[n]) continue;
						if (cosTheta < vlAll->cosInner[n]) {
							float t = (cosTheta - vlAll->cosOuter[n]) / (vlAll->cosInner[n] - vlAll->cosOuter[n]);
							k *= t * t * (3.0f - 2.0f * t);
						}
					}
					const float intensity = k * Mat->Diffuse;
					lB += intensity * vlAll->colB[n];
					lG += intensity * vlAll->colG[n];
					lR += intensity * vlAll->colR[n];

					if (wantSpecular) {
						const float ldx = wx * lenInv;
						const float ldy = wy * lenInv;
						const float ldz = wz * lenInv;
						float hx = ldx + vx_v, hy = ldy + vy_v, hz = ldz + vz_v;
						const float hLen2 = hx*hx + hy*hy + hz*hz;
						if (hLen2 <= 0.0f) continue;
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH <= 0.0f) continue;
						const float specStrength = std::pow(NdotH, gloss) *
							Mat->Specular * falloff;
						sB += specStrength * vlAll->colB[n];
						sG += specStrength * vlAll->colG[n];
						sR += specStrength * vlAll->colR[n];
					}
				}
			} else {
				for (int n = 0; n < tlTile.count; ++n) {
					if (tlTile.mirrorId[n] != pmid) continue;
					const float wx = tlTile.posX[n] - x;
					const float wy = tlTile.posY[n] - y;
					const float wz = tlTile.posZ[n] - z;
					const float dot = wx * nx + wy * ny + wz * nz;
					if (dot < 0.0f) continue;
					const float len2 = wx*wx + wy*wy + wz*wz;
					if (len2 > tlTile.range2[n]) continue;
					const float lenInv = fast_rsqrt(len2);
					const float dist   = len2 * lenInv;
					const float falloff = (1.0f - dist * tlTile.rRange[n]);
					float k = dot * lenInv * falloff;
					if (tlTile.isSpot[n]) {
						const float cosTheta = -(tlTile.dirX[n]*wx + tlTile.dirY[n]*wy + tlTile.dirZ[n]*wz) * lenInv;
						if (cosTheta <= tlTile.cosOuter[n]) continue;
						if (cosTheta < tlTile.cosInner[n]) {
							float t = (cosTheta - tlTile.cosOuter[n]) / (tlTile.cosInner[n] - tlTile.cosOuter[n]);
							k *= t * t * (3.0f - 2.0f * t);
						}
					}
					const float intensity = k * Mat->Diffuse;
					lB += intensity * tlTile.colB[n];
					lG += intensity * tlTile.colG[n];
					lR += intensity * tlTile.colR[n];

					if (wantSpecular) {
						const float ldx = wx * lenInv;
						const float ldy = wy * lenInv;
						const float ldz = wz * lenInv;
						float hx = ldx + vx_v, hy = ldy + vy_v, hz = ldz + vz_v;
						const float hLen2 = hx*hx + hy*hy + hz*hz;
						if (hLen2 <= 0.0f) continue;
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH <= 0.0f) continue;
						const float specStrength = std::pow(NdotH, gloss) *
							Mat->Specular * falloff;
						sB += specStrength * tlTile.colB[n];
						sG += specStrength * tlTile.colG[n];
						sR += specStrength * tlTile.colR[n];
					}
				}
			}

			if (lB > 250.0f) lB = 250.0f;
			if (lG > 250.0f) lG = 250.0f;
			if (lR > 250.0f) lR = 250.0f;
			if (lB < 0.0f) lB = 0.0f;
			if (lG < 0.0f) lG = 0.0f;
			if (lR < 0.0f) lR = 0.0f;

			if (ctx.Sc->Flags & Scn_Fogged) {
				// sqrt(t) via fast_rsqrt: sqrt(t) = t * rsqrt(t).
				// Guarded against t<=0 (rsqrt undefined at 0).
				const float t = 1.0f - z * (1.0f / ctx.Sc->FZP);
				const float fogRate = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
				lB = std::min(std::max(lB * fogRate, 10.0f), 253.0f);
				lG = std::min(std::max(lG * fogRate, 10.0f), 253.0f);
				lR = std::min(std::max(lR * fogRate, 10.0f), 253.0f);
			}

			int litB = int((texB * lB) * (1.0f / 256.0f));
			int litG = int((texG * lG) * (1.0f / 256.0f));
			int litR = int((texR * lR) * (1.0f / 256.0f));
			// Specular added on top — independent of base color tint.
			if (wantSpecular) {
				float fogScale = 1.0f;
				if (ctx.Sc->Flags & Scn_Fogged) {
					const float t = 1.0f - z * (1.0f / ctx.Sc->FZP);
					fogScale = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
				}
				litB += int(sB * fogScale);
				litG += int(sG * fogScale);
				litR += int(sR * fogScale);
			}
			if (litB > 255) litB = 255;
			if (litG > 255) litG = 255;
			if (litR > 255) litR = 255;
			if (litB < 0) litB = 0;
			if (litG < 0) litG = 0;
			if (litR < 0) litR = 0;

			// Alpha-blend onto VPage. Default: `litRGB + dst/2`
			// saturated — matches forward TheOtherBarry<TRANSPARENT>
			// (source-full + dest-halved, NOT 50/50).
			// Opt-in per-material (Mat->XparBlendAlpha > 0): linear
			// interpolate `litRGB * α + dst * (1-α)`, no saturation
			// cap. Fountain orb glass uses α=0.4 for a more
			// see-through look; other transparents in the same scene
			// (e.g. fountain spires) keep the legacy formula.
			const dword existing = out[i];
			const int dB = int((existing      ) & 0xFF);
			const int dG = int((existing >>  8) & 0xFF);
			const int dR = int((existing >> 16) & 0xFF);
			int outB, outG, outR;
			if (Mat->XparBlendAlpha > 0.0f) {
				const float a = Mat->XparBlendAlpha;
				const float ia = 1.0f - a;
				outB = int(float(litB) * a + float(dB) * ia);
				outG = int(float(litG) * a + float(dG) * ia);
				outR = int(float(litR) * a + float(dR) * ia);
			} else {
				outB = litB + dB / 2;
				outG = litG + dG / 2;
				outR = litR + dR / 2;
				if (outB > 255) outB = 255;
				if (outG > 255) outG = 255;
				if (outR > 255) outR = 255;
			}
			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}
	// See the Front-layer variant above for why no release() here.
}

// FDS_DEFERRED_UNIFIED_TBR=1 routes the deferred transparent + sprite
// rendering through the unified per-strip TBR walk (task #9). When off
// (default during validation), the existing per-clump xpar peel runs
// alongside the standalone sprite TBR — the legacy behaviour.
//
// Also requires the current scene to have called TBR_Init (so the per-
// strip linked-list storage exists). Scenes without a TBR — city,
// greets — fall back to the legacy peel; the unified path with no
// SBufferHead would silently drop every transparent face, since
// InsertTransparentToTBR bails when NumTiles==0. Only fountain
// currently calls TBR_Init.
bool deferredUnifiedTbrEnabled() {
	if (!fds::FeatureFlags::deferred_unified_tbr()) return false;
	if (!CurScene) return false;
	if (!CurScene->SBufferHead || CurScene->NumTiles == 0) return false;
	return true;
}

// Per-strip xpar render helper, called from TBR_Render's per-strip walk
// for each clump of consecutive same-(mesh, frontFacing) transparent
// faces in the sorted item list. The strip covers rows [strip_y,
// strip_y+strip_h) (strip_h is normally TILESIZE=8, but may be smaller
// for the last strip if YRes isn't a multiple of 8).
//
// One xpar G-buffer layer is dirtied per clump (back layer for back-
// facing tris, front layer for front-facing tris). We clear only the
// strip's slice (61 KB per layer at 1920 wide), raster the clump's
// faces with clipper extents = strip rect, then composite the strip
// rows via Render_DeferredTransparentLighting_Tile<Layer>.
void RenderXparClumpInStrip(Face** faces, int count, bool front,
                             int strip_y, int strip_h)
{
	const size_t rowStart  = size_t(strip_y) * size_t(XRes);
	const size_t rowCount  = size_t(strip_h) * size_t(XRes);

	// Clear strip's slice of the relevant xpar layer.
	if (front) {
		if (g_gbufferTransparent) {
			std::memset(g_gbufferTransparent->txtr.data() + rowStart,
			            0xFF, rowCount * sizeof(uint32_t));
		}
		if (g_xparZ) std::memset(g_xparZ + rowStart, 0,
		                          rowCount * sizeof(uint16_t));
	} else {
		if (g_gbufferTransparentBack) {
			std::memset(g_gbufferTransparentBack->txtr.data() + rowStart,
			            0xFF, rowCount * sizeof(uint32_t));
		}
		if (g_xparZBack) std::memset(g_xparZBack + rowStart, 0,
		                              rowCount * sizeof(uint16_t));
	}

	// Raster the clump's faces into this layer. Clipper extents pin the
	// rasterizer to the strip; faces are routed to MekaleleTransparent
	// (front-facing → front layer) or MekaleleTransparentBack (back-
	// facing → back layer). All faces in a clump share orientation by
	// construction.
	FrustumClipper clipper;
	clipper.InitViewport(CurScene);
	clipper.SetClippingExtents(0.0f, float(strip_y),
	                            float(XRes), float(strip_y + strip_h));
	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;
	// Strip-clamp the rasterizer: a clipped tri whose max PY snapped to
	// strip_y + strip_h would compute tile_My one tile-row past the strip
	// and write pixels that the neighbouring strip's clear (concurrent)
	// then wipes — visible as 1-row dark stripes every TILE_SIZE rows.
	const int stripTileRow = strip_y >> 3;  // TILELOG=3
	const meka::RasterStripClamp savedClamp = meka::g_rasterStripClamp;
	meka::g_rasterStripClamp = { stripTileRow, stripTileRow };
	for (int i = 0; i < count; ++i) {
		Face* F = faces[i];
		if (!F) continue;
		if (front) clipper.Render(F, MekaleleTransparent,     false, rt, cam);
		else       clipper.Render(F, MekaleleTransparentBack, false, rt, cam);
	}
	meka::g_rasterStripClamp = savedClamp;

	// Composite the strip rows onto VPage via the transparent lighting
	// kernel, with a ctx variant whose `tileLights` points at the
	// per-strip light array (built by buildStripLightLists in
	// Render_DeferredLighting). The kernel reads ctx.tileLights[
	// tileIndex] for its per-pixel light loop; with tileLights ->
	// g_stripLights and tileIndex = strip_y/TILESIZE the strip gets
	// exactly the lights overlapping its 8 rows.
	const int stripIdx = strip_y >> 3;  // TILELOG=3
	DeferredLightingCtx stripCtx = g_deferredCtx;
	stripCtx.tileLights = g_stripLights;
	if (front) {
		Render_DeferredTransparentLighting_Tile<XparLayer::Front>(
			stripCtx, stripIdx,
			0, strip_y, XRes, strip_y + strip_h);
	} else {
		Render_DeferredTransparentLighting_Tile<XparLayer::Back>(
			stripCtx, stripIdx,
			0, strip_y, XRes, strip_y + strip_h);
	}
}

// Wave-2 of checkerboard: fills the odd (skipped) cells. For each odd
// pixel: if both horizontal neighbors share the same matID, average
// their already-shaded VPage values (~5 cycles per pixel — 2 loads,
// 3 lane averages, 1 store). If matID disagrees (material edge), fall
// back to full deferred shading for that pixel via the same kernel as
// wave-1. The mismatch rate on City is ~5% per the snapshot pixel
// distribution — most of the frame is large continuous surfaces.
//
// Outer-SIMD diffuse kernel: 8 pixels per row in vec registers. The
// scalar gather (matTable → Material* → Mipmap[mip] → texel + mat
// scalars) happens once per lane and stays scalar — arm64-via-simde
// has no usable vgather. Everything after — texel byte-extract, oct
// normal decode, view-space pos reconstruct, per-omni dot/range/k
// accumulate, saturate, modulate — runs 8-wide. Specular and water
// fall back to scalar tail when needed; checkerboard is handled by
// dropping odd lanes via the alive mask.
static void Render_DeferredLighting_Tile_OuterVec(const DeferredLightingCtx &ctx,
                                                   int tileIndex,
                                                   int x1, int y1, int x2, int y2)
{
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	const bool   quarter      = deferredLightingQuarterEnabled();
	const bool   checker      = deferredLightingCheckerboardEnabled() && !quarter;
	const bool   specGlobalOn = Specular_Factor > 0.0f;
	const TileLights &tl = ctx.tileLights[tileIndex];
	const float  ambB_sc = float(ctx.Sc->Ambient.B);
	const float  ambG_sc = float(ctx.Sc->Ambient.G);
	const float  ambR_sc = float(ctx.Sc->Ambient.R);

	// Pre-extract per-lane data into 8-aligned scratch buffers so we
	// can do gather work scalar-per-lane and load_a back into vec.
	alignas(32) uint32_t lane_mat32[8];
	alignas(32) uint32_t lane_alive[8];
	alignas(32) float    lane_texB[8], lane_texG[8], lane_texR[8];
	alignas(32) float    lane_ambB[8], lane_ambG[8], lane_ambR[8];
	alignas(32) float    lane_diffuse[8];
	alignas(32) float    lane_specular[8];
	alignas(32) float    lane_gloss[8];
	alignas(32) uint32_t lane_wantSpec[8];
	alignas(32) uint32_t lane_isWater[8];
	// Per-lane mirror id, widened uint8→uint32 once per 8-pixel block.
	// The omni loop builds a per-lane mask against broadcast(tl.mirrorId[n]).
	alignas(32) uint32_t lane_mirrorId[8];

	for (int py = y1; py < y2; ++py) {
		// vec body: groups of 8 pixels
		int px = x1;
		// Align start to 8-pixel boundary so loads from gb arrays are
		// naturally aligned (gb.txtr/gb.normal are aligned at frame
		// allocation; ZPage16 aligned by parallel_memset).
		const int x_vec_start = (x1 + 7) & ~7;
		// Scalar lead-in for unaligned pixels at tile start.
		while (px < x_vec_start && px < x2) {
			// Tail: just defer to scalar; rare since tile boundaries
			// are 8-aligned for our 1920x1080 / 12x8 grid (160-wide tiles).
			++px;
		}

		for (; px < x2; px += 8) {
			const size_t i = size_t(py) * XRes + px;
			// Per-lane mirror id snapshot for this 8-pixel block. Used
			// by the omni loop below to mask off lights whose mirrorId
			// disagrees with the lane's. Plane is byte-sized, widened
			// to uint32 here so cmpeq lines up with tl.mirrorId.
			if (gb.mirrorId.empty()) {
				for (int k = 0; k < 8; ++k) lane_mirrorId[k] = 0u;
			} else {
				for (int k = 0; k < 8; ++k)
					lane_mirrorId[k] = uint32_t(gb.mirrorId[i + k]);
			}

			// Load 8 lanes of Z (u16 → s32 zero-extend → float for the
			// cmp; later use the int form too)
			__m128i z16 = _mm_loadu_si128((const __m128i*)(ZPage16 + i));
			__m256i zEncI = _mm256_cvtepu16_epi32(z16);

			// Alive mask: zEnc != 0
			__m256i mask_alive = _mm256_cmpgt_epi32(zEncI, _mm256_setzero_si256());

			// Lane-in-range mask: when this iteration straddles the
			// right tile edge (x2 - px < 8), lanes that fall in the
			// NEXT tile must be masked off so we don't write past x2.
			// For full groups (x2 - px >= 8) every lane is in range
			// and the AND is a no-op. Tile widths at non-multiple-of-8
			// resolutions (e.g., HiDPI 1512/12 = 126) would otherwise
			// leave the trailing 6 columns of each tile-column
			// unrendered — visible as vertical dark bands at every
			// tile-X boundary.
			{
				__m256i lane_idx2 = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i in_range  = _mm256_cmpgt_epi32(
					_mm256_set1_epi32(x2 - px), lane_idx2);
				mask_alive = _mm256_and_si256(mask_alive, in_range);
			}

			// Checkerboard: drop odd cells in wave-1.
			if (checker) {
				// (px+lane + py) & 1 == 0 → keep
				__m256i lane_idx = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i px_lane  = _mm256_add_epi32(_mm256_set1_epi32(px), lane_idx);
				__m256i parity   = _mm256_and_si256(
					_mm256_xor_si256(px_lane, _mm256_set1_epi32(py)),
					_mm256_set1_epi32(1));
				__m256i keep = _mm256_cmpeq_epi32(parity, _mm256_setzero_si256());
				mask_alive = _mm256_and_si256(mask_alive, keep);
			}
			// Quarter-rate: keep only lanes where both px AND py even.
			if (quarter) {
				if (py & 1) continue;  // entire row skipped in wave-1
				__m256i lane_idx = _mm256_setr_epi32(0,1,2,3,4,5,6,7);
				__m256i px_lane  = _mm256_add_epi32(_mm256_set1_epi32(px), lane_idx);
				__m256i parity_x = _mm256_and_si256(px_lane, _mm256_set1_epi32(1));
				__m256i keep = _mm256_cmpeq_epi32(parity_x, _mm256_setzero_si256());
				mask_alive = _mm256_and_si256(mask_alive, keep);
			}

			// Load mat32
			__m256i mat32v = _mm256_loadu_si256((const __m256i*)(gb.txtr.data() + i));
			__m256i matIDv = _mm256_and_si256(_mm256_srli_epi32(mat32v, 20),
			                                   _mm256_set1_epi32(0xFF));
			// Mask: matID < count
			__m256i mask_id = _mm256_cmpgt_epi32(_mm256_set1_epi32(int(ctx.matTable.count)),
			                                     matIDv);
			mask_alive = _mm256_and_si256(mask_alive, mask_id);

			// Stash for scalar lane work
			_mm256_store_si256((__m256i*)lane_mat32, mat32v);
			_mm256_store_si256((__m256i*)lane_alive, mask_alive);

			// Per-lane scalar gather: resolve Material*, gather texel,
			// fill ambient + spec/diffuse scratch.
			bool any_alive = false;
			for (int k = 0; k < 8; ++k) {
				if (!lane_alive[k]) {
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const uint32_t m = lane_mat32[k];
				const uint32_t matID = (m >> 20) & 0xFF;
				const uint32_t mip   = (m >> 28) & 0xF;
				const uint32_t uv    = m & 0xFFFFF;
				Material *Mat = ctx.matTable.data[matID];
				if (!Mat || !Mat->Txtr) {
					lane_alive[k] = 0;
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const dword *texData = (const dword*)Mat->Txtr->Mipmap[mip];
				if (!texData) {
					lane_alive[k] = 0;
					lane_texB[k] = lane_texG[k] = lane_texR[k] = 0;
					lane_ambB[k] = lane_ambG[k] = lane_ambR[k] = 0;
					lane_diffuse[k] = lane_specular[k] = 0;
					lane_gloss[k] = 32.0f;
					lane_wantSpec[k] = 0;
					lane_isWater[k] = 0;
					continue;
				}
				const dword tx = texData[uv];
				lane_texB[k] = float(tx & 0xFF);
				lane_texG[k] = float((tx >> 8) & 0xFF);
				lane_texR[k] = float((tx >> 16) & 0xFF);
				const float Lumin = Mat->Luminosity;
				const float Diff  = Mat->Diffuse;
				lane_ambB[k]    = Lumin * 255.0f + Diff * ambB_sc;
				lane_ambG[k]    = Lumin * 255.0f + Diff * ambG_sc;
				lane_ambR[k]    = Lumin * 255.0f + Diff * ambR_sc;
				lane_diffuse[k] = Diff;
				lane_specular[k]= Mat->Specular;
				lane_gloss[k]   = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
				lane_wantSpec[k]= (Mat->Specular > 0.0f && specGlobalOn) ? 0xFFFFFFFFu : 0u;
				lane_isWater[k] = (int(matID) == ctx.waterMatID) ? 0xFFFFFFFFu : 0u;
				any_alive = true;
			}
			if (!any_alive) continue;
			// Refresh alive mask from scratch (some lanes may have been
			// killed by mip-data null check above).
			__m256i mask_alive_fresh = _mm256_load_si256((const __m256i*)lane_alive);

			// Decode 8 normals in parallel. oct_decode_u16 form:
			//   qx = sign-extend(low byte), qy = sign-extend(high byte)
			//   ox = qx * (1/127), oy = qy * (1/127)
			//   az = 1 - |ox| - |oy|
			//   if (az < 0) fold ...
			//   then normalize.
			__m128i nrm16 = _mm_loadu_si128((const __m128i*)(gb.normal.data() + i));
			// Split into low-byte and high-byte signed expansion.
			__m128i nrm_qx_8 = _mm_and_si128(nrm16, _mm_set1_epi16(0xFF));
			__m128i nrm_qy_8 = _mm_srli_epi16(nrm16, 8);
			// sign-extend 8-bit to 16
			nrm_qx_8 = _mm_slli_epi16(nrm_qx_8, 8);
			nrm_qx_8 = _mm_srai_epi16(nrm_qx_8, 8);
			nrm_qy_8 = _mm_slli_epi16(nrm_qy_8, 8);
			nrm_qy_8 = _mm_srai_epi16(nrm_qy_8, 8);
			__m256i qx32 = _mm256_cvtepi16_epi32(nrm_qx_8);
			__m256i qy32 = _mm256_cvtepi16_epi32(nrm_qy_8);
			__m256 inv127 = _mm256_set1_ps(1.0f / 127.0f);
			__m256 ox = _mm256_mul_ps(_mm256_cvtepi32_ps(qx32), inv127);
			__m256 oy = _mm256_mul_ps(_mm256_cvtepi32_ps(qy32), inv127);
			__m256 absox = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), ox);
			__m256 absoy = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), oy);
			__m256 az = _mm256_sub_ps(_mm256_sub_ps(_mm256_set1_ps(1.0f), absox), absoy);
			// Branchless fold: t = max(-az, 0); ox -= copysign(t, ox); oy -= copysign(t, oy)
			__m256 t = _mm256_max_ps(_mm256_sub_ps(_mm256_setzero_ps(), az),
			                          _mm256_setzero_ps());
			__m256 sign_ox = _mm256_and_ps(ox, _mm256_set1_ps(-0.0f));
			__m256 sign_oy = _mm256_and_ps(oy, _mm256_set1_ps(-0.0f));
			ox = _mm256_sub_ps(ox, _mm256_or_ps(t, sign_ox));
			oy = _mm256_sub_ps(oy, _mm256_or_ps(t, sign_oy));
			// Normalize via approx_rsqrt
			__m256 lenSq = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(ox, ox),
			                                            _mm256_mul_ps(oy, oy)),
			                              _mm256_mul_ps(az, az));
			__m256 invLenN = _mm256_rsqrt_ps(lenSq);
			__m256 nx = _mm256_mul_ps(ox, invLenN);
			__m256 ny = _mm256_mul_ps(oy, invLenN);
			__m256 nz = _mm256_mul_ps(az, invLenN);

			// Per-lane normal-map sampling. Stores the geometric N back
			// to scratch, then re-runs the wave-1 kernel's Tier-B nmap +
			// TBN reconstruction for each lane whose material has a
			// normal map. Loads the perturbed N back to nx/ny/nz so the
			// vec light loop and the scalar fallback both pick it up.
			// Without this, OuterVec rendered nmap surfaces flat, which
			// looked like horizontal banding on greets's hex floor.
			alignas(32) float nx_lane[8], ny_lane[8], nz_lane[8];
			_mm256_store_ps(nx_lane, nx);
			_mm256_store_ps(ny_lane, ny);
			_mm256_store_ps(nz_lane, nz);
			for (int k = 0; k < 8; ++k) {
				if (!lane_alive[k]) continue;
				const uint32_t m = lane_mat32[k];
				const uint32_t mid  = (m >> 20) & 0xFF;
				const uint32_t mipL = (m >> 28) & 0xF;
				const uint32_t uvL  = m & 0xFFFFF;
				Material *MatN = ctx.matTable.data[mid];
				if (!MatN || !MatN->NormalMap) continue;
				const dword *nmData = (const dword*)MatN->NormalMap->Mipmap[mipL];
				if (!nmData) continue;
				const dword nmTexel = nmData[uvL];
				const float nmX = (float((nmTexel >> 16) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
				const float nmY = (float((nmTexel >>  8) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
				const float nmZ = (float( nmTexel        & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
				float lnx = nx_lane[k], lny = ny_lane[k], lnz = nz_lane[k];
				float tx = 0, ty = 0, tz = 0;
				bool tangentValid = false;
				if (!gb.tangent.empty()) {
					const meka::u16 packedT = gb.tangent[i + k];
					if (packedT != 0) {
						meka::oct_decode_u16(packedT, tx, ty, tz);
						const float tDotN = tx*lnx + ty*lny + tz*lnz;
						tx -= lnx * tDotN;
						ty -= lny * tDotN;
						tz -= lnz * tDotN;
						const float tLen2 = tx*tx + ty*ty + tz*tz;
						if (tLen2 > 1e-12f) {
							const float invTLen = fast_rsqrt(tLen2);
							tx *= invTLen; ty *= invTLen; tz *= invTLen;
							tangentValid = true;
						}
					}
				}
				if (!tangentValid) {
					float refx, refy, refz;
					if (std::fabs(lny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
					else                        { refx = 1; refy = 0; refz = 0; }
					tx = refy * lnz - refz * lny;
					ty = refz * lnx - refx * lnz;
					tz = refx * lny - refy * lnx;
					const float invTLen = fast_rsqrt(tx*tx + ty*ty + tz*tz);
					tx *= invTLen; ty *= invTLen; tz *= invTLen;
				}
				const float bx = lny * tz - lnz * ty;
				const float by = lnz * tx - lnx * tz;
				const float bz = lnx * ty - lny * tx;
				float vnx = tx * nmX + bx * nmY + lnx * nmZ;
				float vny = ty * nmX + by * nmY + lny * nmZ;
				float vnz = tz * nmX + bz * nmY + lnz * nmZ;
				float invLen = fast_rsqrt(vnx*vnx + vny*vny + vnz*vnz);
				nx_lane[k] = vnx * invLen;
				ny_lane[k] = vny * invLen;
				nz_lane[k] = vnz * invLen;
			}
			nx = _mm256_load_ps(nx_lane);
			ny = _mm256_load_ps(ny_lane);
			nz = _mm256_load_ps(nz_lane);

			// Reconstruct view-space pos for 8 lanes.
			// z = (0xFF80 - zEnc) * invZScale
			__m256 zEncF = _mm256_cvtepi32_ps(zEncI);
			__m256 zv = _mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(float(0xFF80)), zEncF),
			                           _mm256_set1_ps(ctx.invZScale));
			// x = (px+lane - CntrEX) * z * invFOVX
			__m256 px_lane_f = _mm256_add_ps(_mm256_set1_ps(float(px)),
			                                  _mm256_setr_ps(0,1,2,3,4,5,6,7));
			__m256 xv = _mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(px_lane_f,
			                                                       _mm256_set1_ps(CntrEX)),
			                                         zv),
			                           _mm256_set1_ps(ctx.invFOVX));
			__m256 yv = _mm256_mul_ps(_mm256_mul_ps(_mm256_sub_ps(_mm256_set1_ps(CntrEY),
			                                                       _mm256_set1_ps(float(py))),
			                                         zv),
			                           _mm256_set1_ps(ctx.invFOVY));

			// Texel + ambient as vec
			__m256 texB = _mm256_load_ps(lane_texB);
			__m256 texG = _mm256_load_ps(lane_texG);
			__m256 texR = _mm256_load_ps(lane_texR);
			__m256 lB   = _mm256_load_ps(lane_ambB);
			__m256 lG   = _mm256_load_ps(lane_ambG);
			__m256 lR   = _mm256_load_ps(lane_ambR);
			__m256 vDiff = _mm256_load_ps(lane_diffuse);

			// Water lanes don't accumulate omnis.
			__m256i waterMask = _mm256_load_si256((const __m256i*)lane_isWater);
			// "alive AND not water" lanes accumulate
			__m256i omniMask  = _mm256_andnot_si256(waterMask, mask_alive_fresh);
			__m256  omniMaskF = _mm256_castsi256_ps(omniMask);

			// Early-out optimization: if every alive lane needs the
			// scalar fallback (wantSpec or isWater), the vec omni loop
			// is wasted — the fallback redoes the lighting with full
			// spec accumulation, and vfB/vfG/vfR get overridden lane-by-
			// lane in the pack loop. Skip the body entirely. For greets
			// the floor + walls are mostly spec/nmap, so this fires
			// often and recovers the regression introduced by adding
			// nmap sampling above.
			__m256i wantSpecMask = _mm256_load_si256((const __m256i*)lane_wantSpec);
			__m256i needScalar   = _mm256_or_si256(wantSpecMask, waterMask);
			__m256i needVec      = _mm256_andnot_si256(needScalar, mask_alive_fresh);
			const bool anyVecLane = !_mm256_testz_si256(needVec, needVec);

			// Per-omni accumulate. Each omni broadcast, 8 pixels in vec.
			const bool profNoLights = fds::FeatureFlags::prof_no_lights();
			const int omniLoopN = (profNoLights || !anyVecLane) ? 0 : tl.count;
			__m256i lane_mirror_v = _mm256_load_si256((const __m256i*)lane_mirrorId);
			for (int n = 0; n < omniLoopN; ++n) {
				__m256 wx = _mm256_sub_ps(_mm256_set1_ps(tl.posX[n]), xv);
				__m256 wy = _mm256_sub_ps(_mm256_set1_ps(tl.posY[n]), yv);
				__m256 wz = _mm256_sub_ps(_mm256_set1_ps(tl.posZ[n]), zv);
				__m256 dot = _mm256_fmadd_ps(wx, nx,
				              _mm256_fmadd_ps(wy, ny,
				               _mm256_mul_ps(wz, nz)));
				__m256 len2 = _mm256_fmadd_ps(wx, wx,
				               _mm256_fmadd_ps(wy, wy,
				                _mm256_mul_ps(wz, wz)));
				__m256 mask_dot   = _mm256_cmp_ps(dot,  _mm256_setzero_ps(), _CMP_GE_OQ);
				__m256 mask_range = _mm256_cmp_ps(len2, _mm256_set1_ps(tl.range2[n]), _CMP_LE_OQ);
				__m256 mask_pos   = _mm256_cmp_ps(len2, _mm256_setzero_ps(), _CMP_GT_OQ);
				// Per-lane mirror filter: light's mirrorId must equal
				// the pixel's. tl.mirrorId[n] is the light's id; lane_
				// mirror_v holds the 8 lanes' pixel ids.
				__m256 mirrorMask = _mm256_castsi256_ps(
					_mm256_cmpeq_epi32(lane_mirror_v,
					                    _mm256_set1_epi32((int)tl.mirrorId[n])));
				__m256 omni_lane  = _mm256_and_ps(_mm256_and_ps(mask_dot, mask_range),
				                                   _mm256_and_ps(_mm256_and_ps(mask_pos, omniMaskF),
				                                                  mirrorMask));
				// safe_len2: 1.0 when masked off
				__m256 safe_len2 = _mm256_blendv_ps(_mm256_set1_ps(1.0f), len2, omni_lane);
				__m256 lenInv = _mm256_rsqrt_ps(safe_len2);
				__m256 dist   = _mm256_mul_ps(safe_len2, lenInv);
				__m256 falloff = _mm256_sub_ps(_mm256_set1_ps(1.0f),
				                                _mm256_mul_ps(dist, _mm256_set1_ps(tl.rRange[n])));
				__m256 k = _mm256_mul_ps(_mm256_mul_ps(dot, lenInv), falloff);

				// Spot cone attenuation (only fires when isSpot[n] is set).
				// Matches the standard kernel exactly so omni pixels under
				// greets's robot spotlight get the same cone falloff.
				if (tl.isSpot[n]) {
					__m256 ldx = _mm256_set1_ps(tl.dirX[n]);
					__m256 ldy = _mm256_set1_ps(tl.dirY[n]);
					__m256 ldz = _mm256_set1_ps(tl.dirZ[n]);
					__m256 lci = _mm256_set1_ps(tl.cosInner[n]);
					__m256 lco = _mm256_set1_ps(tl.cosOuter[n]);
					__m256 dirDotW = _mm256_fmadd_ps(ldx, wx,
					                  _mm256_fmadd_ps(ldy, wy,
					                   _mm256_mul_ps(ldz, wz)));
					__m256 cosTheta = _mm256_mul_ps(_mm256_sub_ps(_mm256_setzero_ps(), dirDotW),
					                                 lenInv);
					__m256 maskInside = _mm256_cmp_ps(cosTheta, lco, _CMP_GT_OQ);
					__m256 rangeRcp = _mm256_div_ps(_mm256_set1_ps(1.0f),
					                                 _mm256_sub_ps(lci, lco));
					__m256 t = _mm256_mul_ps(_mm256_sub_ps(cosTheta, lco), rangeRcp);
					t = _mm256_min_ps(_mm256_max_ps(t, _mm256_setzero_ps()),
					                  _mm256_set1_ps(1.0f));
					__m256 smooth = _mm256_mul_ps(_mm256_mul_ps(t, t),
					                  _mm256_sub_ps(_mm256_set1_ps(3.0f),
					                                _mm256_mul_ps(_mm256_set1_ps(2.0f), t)));
					__m256 coneAtten = _mm256_blendv_ps(_mm256_setzero_ps(), smooth, maskInside);
					k = _mm256_mul_ps(k, coneAtten);
				}

				__m256 intensity = _mm256_blendv_ps(_mm256_setzero_ps(),
				                                     _mm256_mul_ps(k, vDiff),
				                                     omni_lane);
				lB = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colB[n]), lB);
				lG = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colG[n]), lG);
				lR = _mm256_fmadd_ps(intensity, _mm256_set1_ps(tl.colR[n]), lR);
			}

			// Saturate to 250
			__m256 sat = _mm256_set1_ps(250.0f);
			__m256 zero = _mm256_setzero_ps();
			lB = _mm256_max_ps(zero, _mm256_min_ps(lB, sat));
			lG = _mm256_max_ps(zero, _mm256_min_ps(lG, sat));
			lR = _mm256_max_ps(zero, _mm256_min_ps(lR, sat));

			// Fog moved to Render_DeferredFogPass (post-lighting).

			// pixel = texel * lit / 256
			__m256 inv256 = _mm256_set1_ps(1.0f / 256.0f);
			__m256 fdB = _mm256_mul_ps(_mm256_mul_ps(texB, lB), inv256);
			__m256 fdG = _mm256_mul_ps(_mm256_mul_ps(texG, lG), inv256);
			__m256 fdR = _mm256_mul_ps(_mm256_mul_ps(texR, lR), inv256);

			// Spec / water blend / pow are not handled in vec — for any
			// alive lane that needs spec or water-mat, fall back to
			// scalar. Detect:
			//   needsScalar = wantSpec_lane | isWater_lane
			alignas(32) uint32_t lane_needs_scalar[8];
			__m256i needsScalar = _mm256_or_si256(
				_mm256_load_si256((const __m256i*)lane_wantSpec),
				_mm256_load_si256((const __m256i*)lane_isWater));
			_mm256_store_si256((__m256i*)lane_needs_scalar, needsScalar);

			alignas(32) float vfB[8], vfG[8], vfR[8];
			_mm256_store_ps(vfB, fdB);
			_mm256_store_ps(vfG, fdG);
			_mm256_store_ps(vfR, fdR);

			alignas(32) int32_t lane_alive_now[8];
			_mm256_store_si256((__m256i*)lane_alive_now, mask_alive_fresh);

			// Pack lanes into pixel dwords; for spec/water lanes, redo
			// scalar shading (rare on most City pixels).
			alignas(32) float ax[8], ay[8], az_lane[8];
			_mm256_store_ps(ax, xv);
			_mm256_store_ps(ay, yv);
			_mm256_store_ps(az_lane, zv);
			alignas(32) float anx[8], any_l[8], anz[8];
			_mm256_store_ps(anx, nx);
			_mm256_store_ps(any_l, ny);
			_mm256_store_ps(anz, nz);

			for (int k = 0; k < 8; ++k) {
				if (!lane_alive_now[k]) continue;
				int outB, outG, outR;
				if (lane_needs_scalar[k]) {
					// Scalar fallback: redo lighting for this pixel
					// including spec / water blend. Reuses tl + cached
					// texel; ambient already in lane_ambB/G/R.
					float lBs = lane_ambB[k];
					float lGs = lane_ambG[k];
					float lRs = lane_ambR[k];
					float sBs = 0, sGs = 0, sRs = 0;
					float xs = ax[k], ys = ay[k], zs = az_lane[k];
					float nxs = anx[k], nys = any_l[k], nzs = anz[k];
					float vxd = 0, vyd = 0, vzd = 0;
					const bool wantSpec = lane_wantSpec[k] != 0;
					if (wantSpec) {
						float vlen2 = xs*xs + ys*ys + zs*zs;
						float vlenInv = fast_rsqrt(vlen2);
						vxd = -xs * vlenInv;
						vyd = -ys * vlenInv;
						vzd = -zs * vlenInv;
					}
					const bool isWater = lane_isWater[k] != 0;
					// Per-lane mirror id: skip lights belonging to a
					// different mirror context than this pixel's.
					const uint32_t pmid_s = gb.mirrorId.empty()
					    ? 0u : uint32_t(gb.mirrorId[i + k]);
					if (!isWater) {
						const float matDiff = lane_diffuse[k];
						const float matSpec = lane_specular[k];
						const float gloss   = lane_gloss[k];
						for (int n = 0; n < tl.count; ++n) {
							if (tl.mirrorId[n] != pmid_s) continue;
							float wxs = tl.posX[n] - xs;
							float wys = tl.posY[n] - ys;
							float wzs = tl.posZ[n] - zs;
							float dots = wxs*nxs + wys*nys + wzs*nzs;
							if (dots < 0.0f) continue;
							float len2s = wxs*wxs + wys*wys + wzs*wzs;
							if (len2s > tl.range2[n] || len2s == 0.0f) continue;
							float lenInvS = fast_rsqrt(len2s);
							float distS   = len2s * lenInvS;
							float ks = dots * lenInvS * (1.0f - distS * tl.rRange[n]);
							// Spot cone (matches std vec body + std scalar fix).
							if (tl.isSpot[n]) {
								float cosTheta = -(tl.dirX[n]*wxs + tl.dirY[n]*wys + tl.dirZ[n]*wzs) * lenInvS;
								if (cosTheta <= tl.cosOuter[n]) continue;
								if (cosTheta < tl.cosInner[n]) {
									float ct = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
									ks *= ct * ct * (3.0f - 2.0f * ct);
								}
							}
							float ints = ks * matDiff;
							lBs += ints * tl.colB[n];
							lGs += ints * tl.colG[n];
							lRs += ints * tl.colR[n];
							if (wantSpec) {
								float ldx = wxs * lenInvS, ldy = wys * lenInvS, ldz = wzs * lenInvS;
								float hx = ldx + vxd, hy = ldy + vyd, hz = ldz + vzd;
								float hLen2 = hx*hx + hy*hy + hz*hz;
								if (hLen2 <= 0.0f) continue;
								float hInv = fast_rsqrt(hLen2);
								hx *= hInv; hy *= hInv; hz *= hInv;
								float NdotH = nxs*hx + nys*hy + nzs*hz;
								if (NdotH <= 0.0f) continue;
								float spec = std::pow(NdotH, gloss);
								float ss = spec * matSpec * (1.0f - distS * tl.rRange[n]);
								sBs += ss * tl.colB[n];
								sGs += ss * tl.colG[n];
								sRs += ss * tl.colR[n];
							}
						}
					}
					if (lBs > 250.0f) lBs = 250.0f; if (lBs < 0) lBs = 0;
					if (lGs > 250.0f) lGs = 250.0f; if (lGs < 0) lGs = 0;
					if (lRs > 250.0f) lRs = 250.0f; if (lRs < 0) lRs = 0;
					float fdBs = lane_texB[k] * lBs * (1.0f / 256.0f);
					float fdGs = lane_texG[k] * lGs * (1.0f / 256.0f);
					float fdRs = lane_texR[k] * lRs * (1.0f / 256.0f);
					outB = int(fdBs) + int(sBs);
					outG = int(fdGs) + int(sGs);
					outR = int(fdRs) + int(sRs);
					if (isWater) {
						const dword existing = out[i + k];
						outB += int(existing & 0xFF) >> 1;
						outG += int((existing >> 8) & 0xFF) >> 1;
						outR += int((existing >> 16) & 0xFF) >> 1;
					}
				} else {
					outB = int(vfB[k]);
					outG = int(vfG[k]);
					outR = int(vfR[k]);
				}
				if (outB > 255) outB = 255; if (outB < 0) outB = 0;
				if (outG > 255) outG = 255; if (outG < 0) outG = 0;
				if (outR > 255) outR = 255; if (outR < 0) outR = 0;
				out[i + k] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
			}
		}

		// Tail: scalar for remaining 1-7 pixels
		for (; px < x2; ++px) {
			// Lazy: just dispatch back to the per-tile inner-loop body.
			// For simplicity we use a no-op since tile widths divide
			// evenly by 8 in our typical 1920×1080 / 12×8 grid.
			// (160-wide tiles → no tail.) If tile widths ever stop
			// being multiples of 8 this needs a real scalar fallback.
			(void)px;
			break;
		}
	}

	// One permit per completed tile (see renderns::tileDone in RENDER.CPP).
	renderns::tileDone.release();
}

// Race-free because wave-2 reads only what wave-1 wrote; tile-job
// dispatch waits on tileCounter between waves. Tile-edge odd pixels
// (px==x1 or px==x2-1) read the next/prev tile's shaded result, which
// is safe because that neighbor tile finished its wave-1 already.
static void Render_DeferredLighting_TileFill(const DeferredLightingCtx &ctx,
                                              int tileIndex,
                                              int x1, int y1, int x2, int y2)
{
	const meka::GBuffer &gb = *ctx.gb;
	dword *out = reinterpret_cast<dword *>(VPage);
	const bool specGlobalOn = Specular_Factor > 0.0f;
	const bool quarter      = deferredLightingQuarterEnabled();
	const bool checker      = deferredLightingCheckerboardEnabled() && !quarter;
	// Normal-similarity threshold for the quarter fill predicate. matID
	// equality alone is too loose — same hull material on a curved
	// surface gives wildly different shading at adjacent pixels, and
	// the average produces the "cartoonish" robot look. Require neighbor
	// normals to be within ~18° (cos > 0.95 default) before averaging.
	// Center normal decoded once per pixel and reused.
	// Wave-2 fill predicate flags. Used by BOTH quarter and checkerboard
	// paths (same adaptive partial averaging shape). Flag name kept as
	// `quarter_normal_cos` for back-compat.
	const float quarterNormalCos = (quarter || checker)
	    ? fds::FeatureFlags::quarter_normal_cos() : 0.0f;
	const bool  quarterNormalCheck = (quarter || checker) && quarterNormalCos > 0.0f;
	// Z-discontinuity threshold. Catches silhouettes / creases that
	// share matID + normal but have an actual depth step (e.g. a hull
	// panel meeting another panel at a sharp angle: same matID, same
	// material normal-map base normal, but the *geometric* surfaces
	// are angled — and at distance even small angle gives a measurable
	// per-pixel Z jump). Without this, those edges blur in quarter.
	const float quarterZJump  = (quarter || checker)
	    ? fds::FeatureFlags::quarter_z_jump() : 0.0f;
	const bool  quarterZCheck = (quarter || checker) && quarterZJump > 0.0f;

	for (int py = y1; py < y2; ++py) {
		for (int px = x1; px < x2; ++px) {
			if (quarter) {
				if (((px | py) & 1) == 0) continue;  // wave-1 shaded
			} else {
				if (((px ^ py) & 1) == 0) continue;
			}

			const size_t i = size_t(py) * XRes + px;
			const word zEnc = ZPage16[i];
			if (zEnc == 0) continue;

			// Per-pixel mirror id — used by the wave-2 fallback shading
			// to filter lights matching the pixel's mirror context.
			const uint32_t pmid = gb.mirrorId.empty()
			    ? 0u : uint32_t(gb.mirrorId[i]);

			const uint32_t mat32  = gb.txtr[i];
			const uint32_t matIDc = (mat32 >> 20) & 0xFF;

			// Center normal decoded once; reused by every fill pattern's
			// neighbor-similarity test below. Cheap enough vs the avoided
			// full shading that we always decode (even if matID fails).
			float ncX = 0, ncY = 0, ncZ = 0;
			if (quarterNormalCheck) {
				meka::oct_decode_u16(gb.normal[i], ncX, ncY, ncZ);
			}
			auto neighborNormalOk = [&](size_t ni) -> bool {
				if (!quarterNormalCheck) return true;
				float nx, ny, nz;
				meka::oct_decode_u16(gb.normal[ni], nx, ny, nz);
				return (ncX*nx + ncY*ny + ncZ*nz) >= quarterNormalCos;
			};
			// Z-discontinuity check: relative depth diff vs center.
			// Center zEnc loaded above (`zEnc`). Compared against neighbor
			// zEnc via a single signed compare and bound. Zero neighbor
			// zEnc (= sky/empty) always fails — averaging in sky pixels
			// would smear edges.
			auto neighborZOk = [&](size_t ni) -> bool {
				if (!quarterZCheck) return true;
				const word zN = ZPage16[ni];
				if (zN == 0) return false;
				const int diff = int(zN) - int(zEnc);
				const int absDiff = diff < 0 ? -diff : diff;
				return float(absDiff) <= quarterZJump * float(zEnc);
			};
			// Combined predicate: same matID + normal-similar + Z-similar.
			// Adaptive partial averaging uses this to pick which neighbors
			// to blend; unmatched neighbors are dropped from the average
			// (instead of falling back to full shading for the whole
			// pixel). Eliminates the per-face-edge outline artifact —
			// previously when 1 of 4 corners failed the cos check, the
			// whole pixel switched to full shading and looked visibly
			// different from surrounding interpolated pixels.
			auto neighborCompatible = [&](size_t ni, uint32_t matIDc_) -> bool {
				const uint32_t mID = (gb.txtr[ni] >> 20) & 0xFF;
				if (mID != matIDc_) return false;
				if (!neighborNormalOk(ni)) return false;
				if (!neighborZOk(ni)) return false;
				return true;
			};

			bool matched = false;
			if (quarter) {
				// Adaptive partial averaging: per-neighbor compatibility
				// test (matID + normal + Z), then average ONLY the passing
				// neighbors. Avoids the per-face-edge outline that the
				// all-or-nothing fallback produces: when 1 of 4 corners
				// (or 1 of 2 sides) fails the test, that pixel previously
				// switched to full shading and looked visibly different
				// from surrounding averaged pixels — that visible step IS
				// the outline. Partial averaging blends smoothly instead.
				//
				// Per-channel sum + division by N. N is 0..4; division by
				// 3 isn't bit-shiftable but at most 25% of pixels hit it
				// and the rest use shift fast paths. Still cheaper by ~5×
				// than the full-shading fallback per pixel.
				const bool odd_x = px & 1;
				const bool odd_y = py & 1;
				size_t nidx[4];
				int    nc = 0;
				if (odd_x && !odd_y) {
					if (px > 0)        nidx[nc++] = i - 1;
					if (px < XRes - 1) nidx[nc++] = i + 1;
				} else if (!odd_x && odd_y) {
					if (py > 0)        nidx[nc++] = i - XRes;
					if (py < YRes - 1) nidx[nc++] = i + XRes;
				} else {
					if (px > 0 && py > 0)               nidx[nc++] = i - XRes - 1;
					if (px < XRes - 1 && py > 0)        nidx[nc++] = i - XRes + 1;
					if (px > 0 && py < YRes - 1)        nidx[nc++] = i + XRes - 1;
					if (px < XRes - 1 && py < YRes - 1) nidx[nc++] = i + XRes + 1;
				}
				int sumR = 0, sumG = 0, sumB = 0;
				int n = 0;
				for (int k = 0; k < nc; ++k) {
					if (!neighborCompatible(nidx[k], matIDc)) continue;
					const dword p = out[nidx[k]];
					sumB += int(p & 0xFF);
					sumG += int((p >> 8) & 0xFF);
					sumR += int((p >> 16) & 0xFF);
					++n;
				}
				if (n > 0) {
					int aR, aG, aB;
					if (n == 1)      { aB = sumB;       aG = sumG;       aR = sumR;       }
					else if (n == 2) { aB = sumB >> 1;  aG = sumG >> 1;  aR = sumR >> 1;  }
					else if (n == 4) { aB = sumB >> 2;  aG = sumG >> 2;  aR = sumR >> 2;  }
					else /* n == 3 */ { aB = sumB / 3;  aG = sumG / 3;   aR = sumR / 3;   }
					out[i] = dword(aB) | (dword(aG) << 8) | (dword(aR) << 16) | 0xFF000000u;
					matched = true;
				}
			} else {
				// Checkerboard: same adaptive partial averaging shape as
				// quarter's horizontal pattern. Neighbors are L and R; each
				// individually tested for matID + normal + Z compatibility.
				// Average passers only; fall back to full shading when both
				// fail. Same fix as quarter for face-edge outlines.
				size_t nidx[2];
				int    nc = 0;
				if (px > 0)        nidx[nc++] = i - 1;
				if (px < XRes - 1) nidx[nc++] = i + 1;
				if (nc == 0) continue;
				int sumR = 0, sumG = 0, sumB = 0;
				int n = 0;
				for (int k = 0; k < nc; ++k) {
					if (!neighborCompatible(nidx[k], matIDc)) continue;
					const dword p = out[nidx[k]];
					sumB += int(p & 0xFF);
					sumG += int((p >> 8) & 0xFF);
					sumR += int((p >> 16) & 0xFF);
					++n;
				}
				if (n > 0) {
					int aR, aG, aB;
					if (n == 1)      { aB = sumB;       aG = sumG;       aR = sumR;       }
					else /* n == 2 */ { aB = sumB >> 1; aG = sumG >> 1;  aR = sumR >> 1; }
					out[i] = dword(aB) | (dword(aG) << 8) | (dword(aR) << 16) | 0xFF000000u;
					matched = true;
				}
			}
			if (matched) continue;

			// Fallback path — replays the wave-1 kernel for this pixel.
			// We don't expect this branch to be hot (most frame area is
			// continuous surface), so the simpler scalar-only fallback
			// keeps the code small even if we're in vec mode otherwise.
			const uint32_t miplevel    = (mat32 >> 28) & 0xF;
			const uint32_t matID       = matIDc;
			const uint32_t swizzledUV  = mat32 & 0xFFFFF;
			if (matID >= ctx.matTable.count) continue;
			Material *Mat = ctx.matTable.data[matID];
			if (!Mat || !Mat->Txtr) continue;
			const dword *texData = (const dword *)Mat->Txtr->Mipmap[miplevel];
			if (!texData) continue;
			const dword texel = texData[swizzledUV];
			const float texB = float(texel & 0xFF);
			const float texG = float((texel >> 8) & 0xFF);
			const float texR = float((texel >> 16) & 0xFF);

			float nx, ny, nz;
			meka::oct_decode_u16(gb.normal[i], nx, ny, nz);

			// Normal map sampling — mirrors the wave-1 kernel exactly so
			// nmap materials that fall through the surface-similar test
			// produce the same result as if they'd been shaded in wave-1.
			// Without this the fallback's geometric N differed from
			// surrounding interpolated pixels and broke quarter mode
			// silently. Tier B per-vertex tangent + Mikkelsen fallback.
			if (Mat->NormalMap) {
				const dword *nmData = (const dword *)Mat->NormalMap->Mipmap[miplevel];
				if (nmData) {
					const dword nmTexel = nmData[swizzledUV];
					const float nmX = (float((nmTexel >> 16) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					const float nmY = (float((nmTexel >>  8) & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					const float nmZ = (float( nmTexel        & 0xFF) * (1.0f/255.0f)) * 2.0f - 1.0f;
					float tx = 0, ty = 0, tz = 0;
					bool tangentValid = false;
					if (!gb.tangent.empty()) {
						const meka::u16 packedT = gb.tangent[i];
						if (packedT != 0) {
							meka::oct_decode_u16(packedT, tx, ty, tz);
							const float tDotN = tx*nx + ty*ny + tz*nz;
							tx -= nx * tDotN; ty -= ny * tDotN; tz -= nz * tDotN;
							const float tLen2 = tx*tx + ty*ty + tz*tz;
							if (tLen2 > 1e-12f) {
								const float invTLen = fast_rsqrt(tLen2);
								tx *= invTLen; ty *= invTLen; tz *= invTLen;
								tangentValid = true;
							}
						}
					}
					if (!tangentValid) {
						float refx, refy, refz;
						if (std::fabs(ny) < 0.9f) { refx = 0; refy = 1; refz = 0; }
						else                       { refx = 1; refy = 0; refz = 0; }
						tx = refy * nz - refz * ny;
						ty = refz * nx - refx * nz;
						tz = refx * ny - refy * nx;
						const float invTLen = fast_rsqrt(tx*tx + ty*ty + tz*tz);
						tx *= invTLen; ty *= invTLen; tz *= invTLen;
					}
					const float bx = ny * tz - nz * ty;
					const float by = nz * tx - nx * tz;
					const float bz = nx * ty - ny * tx;
					float vnx = tx * nmX + bx * nmY + nx * nmZ;
					float vny = ty * nmX + by * nmY + ny * nmZ;
					float vnz = tz * nmX + bz * nmY + nz * nmZ;
					float invLen = fast_rsqrt(vnx*vnx + vny*vny + vnz*vnz);
					nx = vnx * invLen;
					ny = vny * invLen;
					nz = vnz * invLen;
				}
			}

			const float z = float(0xFF80 - zEnc) * ctx.invZScale;
			const float x = (float(px) - CntrEX) * z * ctx.invFOVX;
			const float y = (CntrEY - float(py)) * z * ctx.invFOVY;

			float lB, lG, lR;
			if (Mat->Txtr) {
				lB = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * 255.0f + Mat->Diffuse * ctx.Sc->Ambient.R;
			} else {
				lB = Mat->Luminosity * Mat->BaseCol.B + Mat->Diffuse * ctx.Sc->Ambient.B;
				lG = Mat->Luminosity * Mat->BaseCol.G + Mat->Diffuse * ctx.Sc->Ambient.G;
				lR = Mat->Luminosity * Mat->BaseCol.R + Mat->Diffuse * ctx.Sc->Ambient.R;
			}

			const bool isWater = (int(matID) == ctx.waterMatID);
			const TileLights &tl = ctx.tileLights[tileIndex];

			float sB = 0, sG = 0, sR = 0;
			float vx = 0, vy = 0, vz = 0;
			const bool wantSpecular = specGlobalOn && (Mat->Specular > 0.0f);
			const float gloss = Mat->Glossiness > 0 ? float(Mat->Glossiness) : 32.0f;
			if (wantSpecular) {
				const float vlen2 = x*x + y*y + z*z;
				const float vlenInv = fast_rsqrt(vlen2);
				vx = -x * vlenInv;
				vy = -y * vlenInv;
				vz = -z * vlenInv;
			}

			for (int n = 0; !isWater && n < tl.count; ++n) {
				if (tl.mirrorId[n] != pmid) continue;
				const float wx = tl.posX[n] - x;
				const float wy = tl.posY[n] - y;
				const float wz = tl.posZ[n] - z;
				const float dot = wx*nx + wy*ny + wz*nz;
				if (dot < 0.0f) continue;
				const float len2 = wx*wx + wy*wy + wz*wz;
				if (len2 > tl.range2[n]) continue;
				const float lenInv = fast_rsqrt(len2);
				const float dist   = len2 * lenInv;
				float k = dot * lenInv * (1.0f - dist * tl.rRange[n]);
				// Spot cone gate. coneAtten in [0,1] modulates BOTH diffuse
				// and specular contributions so they stay coherent — the
				// spot just darkens the light past the inner cone, not
				// just its diffuse channel.
				float coneAtten = 1.0f;
				if (tl.isSpot[n]) {
					const float cosTheta = -(tl.dirX[n]*wx + tl.dirY[n]*wy + tl.dirZ[n]*wz) * lenInv;
					if (cosTheta <= tl.cosOuter[n]) continue;
					if (cosTheta < tl.cosInner[n]) {
						float t = (cosTheta - tl.cosOuter[n]) / (tl.cosInner[n] - tl.cosOuter[n]);
						coneAtten = t * t * (3.0f - 2.0f * t);
						k *= coneAtten;
					}
				}
				const float intensity = k * Mat->Diffuse;
				lB += intensity * tl.colB[n];
				lG += intensity * tl.colG[n];
				lR += intensity * tl.colR[n];

				if (wantSpecular) {
					const float ldx = wx * lenInv;
					const float ldy = wy * lenInv;
					const float ldz = wz * lenInv;
					float hx = ldx + vx, hy = ldy + vy, hz = ldz + vz;
					const float hLen2 = hx*hx + hy*hy + hz*hz;
					if (hLen2 > 0.0f) {
						const float hLenInv = fast_rsqrt(hLen2);
						hx *= hLenInv; hy *= hLenInv; hz *= hLenInv;
						const float NdotH = nx*hx + ny*hy + nz*hz;
						if (NdotH > 0.0f) {
							const float spec = std::pow(NdotH, gloss);
							const float specStrength = spec * Mat->Specular * (1.0f - dist * tl.rRange[n]) * coneAtten;
							sB += specStrength * tl.colB[n];
							sG += specStrength * tl.colG[n];
							sR += specStrength * tl.colR[n];
						}
					}
				}
			}

			if (lB > 250.0f) lB = 250.0f;
			if (lG > 250.0f) lG = 250.0f;
			if (lR > 250.0f) lR = 250.0f;
			if (lB < 0.0f) lB = 0.0f;
			if (lG < 0.0f) lG = 0.0f;
			if (lR < 0.0f) lR = 0.0f;

			// Fog moved to Render_DeferredFogPass (post-lighting).

			float fdB = (texB * lB) * (1.0f / 256.0f);
			float fdG = (texG * lG) * (1.0f / 256.0f);
			float fdR = (texR * lR) * (1.0f / 256.0f);
			int outB = int(fdB) + int(sB);
			int outG = int(fdG) + int(sG);
			int outR = int(fdR) + int(sR);
			if (isWater) {
				const dword existing = out[i];
				outB += int(existing & 0xFF) >> 1;
				outG += int((existing >> 8) & 0xFF) >> 1;
				outR += int((existing >> 16) & 0xFF) >> 1;
			}
			if (outB > 255) outB = 255;
			if (outG > 255) outG = 255;
			if (outR > 255) outR = 255;
			if (outB < 0) outB = 0;
			if (outG < 0) outG = 0;
			if (outR < 0) outR = 0;
			out[i] = dword(outB) | (dword(outG) << 8) | (dword(outR) << 16) | 0xFF000000u;
		}
	}

	// One permit per completed tile (see renderns::tileDone in RENDER.CPP).
	renderns::tileDone.release();
}

// Per-frame setup + dispatch tile jobs across the ThreadPool. Same 6×4
// split the rasterizer uses; tiles are independent (each writes a
// disjoint slice of VPage). Reuses the renderns::tileCounter +
// condition variable that Render() already uses for the rasterizer
// pass — fine because we wait synchronously between Render's tile
// dispatch and our own.
void Render_DeferredLighting() {
	if (!g_gbuffer || !ZPage16 || !VPage) return;
	const meka::GBuffer &gb = *g_gbuffer;
	const size_t numPixels = size_t(XRes) * size_t(YRes);
	if (gb.normal.size() < numPixels || gb.txtr.size() < numPixels) return;

	Scene *Sc = CurScene;
	if (!Sc) return;

	MatTable matTable = Scene_GetMatTable(Sc);
	if (!matTable.data || matTable.count == 0) return;

	// FDS_DEFERRED_GLOSS_STATS=1: dump distinct Mat->Glossiness values
	// (and material count per value) for materials with Specular > 0 in
	// the current scene. Run once per scene change; lets us confirm the
	// squaring-dispatch switch in the lighting kernel covers everything
	// in use, and warn if an FLD ships a gloss value we don't specialize.
	if (fds::FeatureFlags::deferred_gloss_stats()) {
		static Scene* lastScene = nullptr;
		if (Sc != lastScene) {
			lastScene = Sc;
			std::map<unsigned short, int> glossHisto;
			int specMats = 0;
			for (size_t i = 0; i < matTable.count; ++i) {
				Material *M = matTable.data[i];
				if (!M || M->Specular <= 0.0f) continue;
				++specMats;
				++glossHisto[M->Glossiness];
			}
			std::fprintf(stderr, "[GLOSS-STATS] scene=%p specMats=%d distinct={",
				(void*)Sc, specMats);
			bool first = true;
			for (const auto &kv : glossHisto) {
				std::fprintf(stderr, "%s%u:%d", first ? "" : ",", kv.first, kv.second);
				first = false;
			}
			std::fprintf(stderr, "}\n");
		}
	}

	// Build view-space omni list once per frame.
	static ViewLightsSoA lights;  // function-static; lifetime spans dispatch + wait
	// Optional Range clamp — if FDS_DEFERRED_MAX_RANGE is set, any
	// per-omni Range above that ceiling is clamped *for culling
	// purposes only* (the falloff math still uses the real Range, so
	// pixel brightness is barely affected — at the clamp boundary
	// contribution is `1 - clamp/Range_real` ≈ 1, dropping to 0 at the
	// clamp edge). Used to estimate the perf upside of dialing in
	// effective ranges scene-wide.
	const float maxRange = fds::FeatureFlags::deferred_max_range();

	std::memset(&lights, 0, sizeof(lights));
	int numLights = 0;
	for (Omni *O = Sc->OmniHead; O && numLights < DEFERRED_MAX_LIGHTS; O = O->Next) {
		if (!(O->Flags & Omni_Active)) continue;
		Vector u, w;
		Vector_Sub(&O->IPos, &View->ISource, &u);
		MatrixXVector(View->Mat, &u, &w);
		float Range = O->IRange;
		if (maxRange > 0.0f && Range > maxRange) Range = maxRange;
		lights.posX[numLights]   = w.x;
		lights.posY[numLights]   = w.y;
		lights.posZ[numLights]   = w.z;
		lights.posWorldX[numLights] = O->IPos.x;
		lights.posWorldY[numLights] = O->IPos.y;
		lights.posWorldZ[numLights] = O->IPos.z;
		lights.colB[numLights]   = O->L.B * O->ISize;
		lights.colG[numLights]   = O->L.G * O->ISize;
		lights.colR[numLights]   = O->L.R * O->ISize;
		lights.range2[numLights] = Range * Range;
		// Note: rRange uses the *real* range so falloff math is
		// preserved. Only the cull radius shrinks.
		const float realRange = O->IRange;
		lights.rRange[numLights] = (realRange > 0.0f) ? 1.0f / realRange : 0.0f;
		// Spot light: rotate the cone axis (world space) into view space
		// using the same View->Mat that placed posX/Y/Z. Pure rotation,
		// no translation — IDir is a direction not a point.
		if (O->Type == Light_SpotLight) {
			Vector dirView;
			MatrixXVector(View->Mat, (Vector*)&O->IDir, &dirView);
			Vector_Norm(&dirView);
			lights.dirX[numLights] = dirView.x;
			lights.dirY[numLights] = dirView.y;
			lights.dirZ[numLights] = dirView.z;
			lights.cosInner[numLights] = O->HotSpot;
			lights.cosOuter[numLights] = O->FallOff;
			lights.isSpot[numLights] = 1u;
		} else {
			lights.dirX[numLights] = 0.0f;
			lights.dirY[numLights] = 0.0f;
			lights.dirZ[numLights] = 0.0f;
			lights.cosInner[numLights] = -2.0f;
			lights.cosOuter[numLights] = -2.0f;
			lights.isSpot[numLights] = 0u;
		}
		// Map this light to its shadow map (or -1). Only set when
		// shadows are enabled — otherwise the kernel must skip the
		// shadow test (and the shadow maps are stale-empty anyway).
		// Spot path: scan g_shadowMaps for a 2D entry (cubeFace<0).
		// Cube-omni path: scan g_cubeShadowRefs for the matching omni.
		int32_t smIdx     = -1;
		int32_t cubeIdx   = -1;
		if (fds::FeatureFlags::shadows() && (O->Flags & Omni_CastsShadow)) {
			if (O->Type == Light_SpotLight) {
				for (size_t i = 0; i < g_shadowMaps.size(); ++i) {
					if (g_shadowMaps[i].omni == O && g_shadowMaps[i].cubeFace < 0) {
						smIdx = int32_t(i);
						break;
					}
				}
			} else if (O->Type == Light_Omni) {
				for (size_t i = 0; i < g_cubeShadowRefs.size(); ++i) {
					if (g_cubeShadowRefs[i].omni == O) {
						cubeIdx = int32_t(i);
						break;
					}
				}
			}
		}
		lights.shadowMapIdx[numLights]  = smIdx;
		lights.cubeShadowIdx[numLights] = cubeIdx;
		// Per-omni halo controls. 0 → "use legacy default":
		//   HaloIntensity = 0 → 1.0 (multiplier no-op)
		//   HaloRange     = 0 → IRange (same as surface lighting)
		// Range resolution chain (later overrides earlier):
		//   IRange  ->  HaloRange  ->  × omni_halo_range_mult
		//                          OR  omni_halo_force_range (hard set)
		const float haloMul    = (O->HaloIntensity > 0.0f) ? O->HaloIntensity : 1.0f;
		const float forceRange = fds::FeatureFlags::omni_halo_force_range();
		float       haloRange;
		if (forceRange > 0.0f) {
			haloRange = forceRange;
		} else {
			const float baseRange = (O->HaloRange > 0.0f) ? O->HaloRange : O->IRange;
			const float rangeMult = fds::FeatureFlags::omni_halo_range_mult();
			haloRange = baseRange * (rangeMult > 0.0f ? rangeMult : 1.0f);
		}
		lights.haloDensityMul[numLights] = haloMul;
		lights.haloRange     [numLights] = haloRange;
		lights.haloRange2    [numLights] = haloRange * haloRange;
		lights.haloRRange    [numLights] = (haloRange > 0.0f) ? 1.0f / haloRange : 0.0f;
		// Mirror id: 0 for originals, 1..N for clones from GreetsMirror.
		// Surface lighting kernels gate per pixel against gb.mirrorId.
		lights.mirrorId      [numLights] = O->mirrorId;
		++numLights;
	}

	constexpr int numTilesX = DEFERRED_NUM_TILES_X;
	constexpr int numTilesY = DEFERRED_NUM_TILES_Y;
	// Round tileSizeX up to the next multiple of 8 so the OuterVec
	// kernel's 8-wide vec loop sees aligned tiles for all-but-the-last
	// tile column. The last tile width can still be unaligned at non-
	// multiple-of-8 XRes (rare on real displays — most are 8-aligned)
	// — handled by the lane-in-range mask inside OuterVec.
	// tileSizeY doesn't need this because the kernels iterate rows one
	// at a time, not in 8-tall groups.
	const int rawTileX  = (XRes + (numTilesX - 1)) / numTilesX;
	const int tileSizeX = (rawTileX + 7) & ~7;
	const int tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;

	// Per-tile light culling: project each omni's bounding sphere
	// into screen space, find which tiles it overlaps, populate
	// indices[] per tile.
	static TileLights tileLights[DEFERRED_NUM_TILES];
	const float invZScale = 1.0f / float(g_zscale);
	computeTileDepthBounds(tileLights, numTilesX, numTilesY,
	                       tileSizeX, tileSizeY, XRes, YRes,
	                       invZScale);
	buildTileLightLists(tileLights, numTilesX, numTilesY,
	                    tileSizeX, tileSizeY, XRes, YRes,
	                    lights, numLights);

	// Per-strip light lists for the unified-TBR transparent path's
	// RenderXparClumpInStrip. 1D Y-strips of TILESIZE rows; built only
	// when the unified path is active to avoid the per-frame cost on
	// the legacy path. (Strip count = ceil(YRes / TILESIZE), capped at
	// DEFERRED_MAX_STRIPS=512.)
	if (deferredUnifiedTbrEnabled()) {
		constexpr int STRIP_H = 1 << 3;  // TILESIZE from FILLERS.CPP
		const int numStrips = (YRes + STRIP_H - 1) >> 3;
		buildStripLightLists(numStrips, STRIP_H, YRes, lights, numLights);
	}
	if (fds::FeatureFlags::deferred_tile_stats()) {
		int total = 0, tmin = INT_MAX, tmax = 0;
		for (int t = 0; t < DEFERRED_NUM_TILES; ++t) {
			total += tileLights[t].count;
			tmin = std::min(tmin, tileLights[t].count);
			tmax = std::max(tmax, tileLights[t].count);
		}
		fprintf(stderr, "[TILE-LIGHTS] numLights=%d tiles avg=%.1f min=%d max=%d\n",
			numLights, total / float(DEFERRED_NUM_TILES), tmin, tmax);
	}

	// File-scope static: also read by the transparent-lighting kernel
	// (Render_DeferredTransparentLighting_Tile) — same per-frame setup,
	// no point rebuilding it. The kernels never run concurrently with
	// each other, so the single instance is safe.
	extern DeferredLightingCtx g_deferredCtx;
	DeferredLightingCtx &ctx = g_deferredCtx;
	ctx.gb         = &gb;
	ctx.matTable   = matTable;
	ctx.lights     = &lights;
	ctx.numLights  = numLights;
	ctx.tileLights = tileLights;
	ctx.invFOVX    = 1.0f / FOVX;
	ctx.invFOVY    = 1.0f / FOVY;
	ctx.invZScale  = 1.0f / float(g_zscale);
	ctx.Sc         = Sc;
	ctx.waterMatID = g_deferredWaterMatID;
	// View → world (transpose of view rotation + camera origin). Used
	// per pixel for cube shadow sampling to convert view-space sample
	// to world for face selection. View.Mat is a pure rotation, so
	// transpose == inverse.
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			ctx.viewToWorld[r][c] = View->Mat[c][r];
	ctx.cameraWorldX = View->ISource.x;
	ctx.cameraWorldY = View->ISource.y;
	ctx.cameraWorldZ = View->ISource.z;

	// Wave 1: shade even cells (full deferred kernel). When checkerboard
	// is off, this is the entire pass and odd-cell skip is a no-op.
	const bool useOuterVec = deferredLightingOuterVecEnabled();
	renderns::tileCounter = 0;
	for (int j = 0; j < numTilesY; ++j) {
		const int y1 = tileSizeY * j;
		const int y2 = std::min(y1 + tileSizeY, YRes);
		for (int i = 0; i < numTilesX; ++i) {
			const int x1 = tileSizeX * i;
			const int x2 = std::min(x1 + tileSizeX, XRes);
			const int tileIndex = j * numTilesX + i;
			if (useOuterVec) {
				ThreadPool::instance().enqueue([tileIndex, x1, y1, x2, y2]() {
					Render_DeferredLighting_Tile_OuterVec(ctx, tileIndex, x1, y1, x2, y2);
				});
			} else {
				ThreadPool::instance().enqueue([tileIndex, x1, y1, x2, y2]() {
					Render_DeferredLighting_Tile(ctx, tileIndex, x1, y1, x2, y2);
				});
			}
		}
	}
	for (int _i = 0, n = numTilesX * numTilesY; _i < n; ++_i) {
		renderns::tileDone.acquire();
	}

	// Wave 2: fill odd cells via 2-tap interpolation (with full-shade
	// fallback at material edges). Skip entirely when checkerboard is
	// off — wave 1 already covered everything.
	if (deferredLightingCheckerboardEnabled() || deferredLightingQuarterEnabled()) {
		renderns::tileCounter = 0;
		for (int j = 0; j < numTilesY; ++j) {
			const int y1 = tileSizeY * j;
			const int y2 = std::min(y1 + tileSizeY, YRes);
			for (int i = 0; i < numTilesX; ++i) {
				const int x1 = tileSizeX * i;
				const int x2 = std::min(x1 + tileSizeX, XRes);
				const int tileIndex = j * numTilesX + i;
				ThreadPool::instance().enqueue([tileIndex, x1, y1, x2, y2]() {
					Render_DeferredLighting_TileFill(ctx, tileIndex, x1, y1, x2, y2);
				});
			}
		}
		for (int _i = 0, n = numTilesX * numTilesY; _i < n; ++_i) {
			renderns::tileDone.acquire();
		}
	}

	// Dump cache-line transition stats accumulated by shadow sampling
	// during this frame's tile work. Reset to zero for the next frame.
	if (fds::FeatureFlags::shadow_prof_cache()) {
		const uint64_t s = g_shadowProfSamples.exchange(0, std::memory_order_relaxed);
		const uint64_t t = g_shadowProfLineTransitions.exchange(0, std::memory_order_relaxed);
		const double pct = s ? 100.0 * double(t) / double(s) : 0.0;
		std::fprintf(stderr,
			"[SHADOW-CACHE] samples=%llu line-transitions=%llu (%.2f%%)\n",
			(unsigned long long)s, (unsigned long long)t, pct);
	}
}

// Full-screen distance fog over opaque pixels. Runs after
// Render_DeferredLighting writes finished colors to VPage; before the
// transparent peel composites. 1998 used `sqrt(1 - z/FZP)` per VERTEX;
// we keep the sqrt curve (the linear curve makes mid-range too thick)
// but compute it per-pixel via approx rsqrt: `sqrt(t) = t * rsqrt(t)`.
// On arm64 this maps to vrsqrteq_f32 (4-5 cycles) and on x86 to
// _mm256_rsqrt_ps. With the simde arm64 m256 rsqrt patch, both lanes
// hit native NEON / AVX2 directly.
//
// Skipped pixels:
//   mat32 == 0xFFFFFFFF — forward filler wrote here (reflective windows,
//     additive fountain vortex, sky-cube). Those have their own
//     per-vertex fog from TheOtherBarry and would double-fog otherwise.
//   zEnc == 0 — no opaque pixel (sky-cube background, or empty Z).
//     Sky already represents infinite distance; don't fog it.
static void Render_DeferredFogPass_Tile(int x1, int y1, int x2, int y2,
                                         float invFZP)
{
	dword *out = reinterpret_cast<dword*>(VPage);
	const uint32_t *mat = g_gbuffer->txtr.data();
	const uint16_t *zEnc = ZPage16;
	const float invZScale = 1.0f / float(g_zscale);
	const Vec8f vInvZScale(invZScale);
	const Vec8f vInvFZP(invFZP);
	const Vec8f vOne(1.0f);
	const Vec8f vZero(0.0f);
	const Vec8i vSentinel(int(0xFFFFFFFF));
	const Vec8i vZEncZero(0);
	const Vec8f vZBase(float(0xFF80));
	const Vec8i vFFu(0xFF);
	const Vec8i vTen(10);
	const Vec8i vAlpha(int(0xFF000000));

	for (int py = y1; py < y2; ++py) {
		int px = x1;
		const size_t row = size_t(py) * XRes;
		// 8-pixel SIMD body.
		for (; px + 8 <= x2; px += 8) {
			const size_t i = row + px;

			Vec8i v8;
			v8.load(out + i);
			Vec8i m8;
			m8.load(mat + i);
			// 8x u16 → 8x i32 (no sign-extension needed: zEnc is unsigned).
			alignas(16) uint16_t z16buf[8];
			std::memcpy(z16buf, zEnc + i, 16);
			alignas(32) int32_t z32buf[8];
			for (int k = 0; k < 8; ++k) z32buf[k] = int32_t(z16buf[k]);
			Vec8i z8;
			z8.load_a(z32buf);

			// Pixel-valid mask: mat != sentinel AND zEnc != 0.
			Vec8ib maskValid = (m8 != vSentinel) & (z8 != vZEncZero);

			// Decode z, fog rate, sqrt-via-rsqrt.
			Vec8f zf  = (vZBase - to_float(z8)) * vInvZScale;
			Vec8f t   = max(vZero, vOne - zf * vInvFZP);
			Vec8f rs  = approx_rsqrt(t);
			Vec8f fog = t * rs;          // sqrt(t)
			// At t==0, rsqrt yields garbage; mask the result.
			Vec8fb tPositive = t > vZero;
			fog = select(tPositive, fog, vZero);
			fog = min(fog, vOne);

			// Channel split, fog-multiply, floor-at-10, re-pack.
			Vec8i bB = v8        & vFFu;
			Vec8i bG = (v8 >>  8) & vFFu;
			Vec8i bR = (v8 >> 16) & vFFu;
			Vec8i nB = max(vTen, truncatei(to_float(bB) * fog));
			Vec8i nG = max(vTen, truncatei(to_float(bG) * fog));
			Vec8i nR = max(vTen, truncatei(to_float(bR) * fog));
			Vec8i packed = nB | (nG << 8) | (nR << 16) | vAlpha;
			Vec8i result = select(Vec8ib(maskValid), packed, v8);
			result.store(out + i);
		}
		// Scalar tail (≤7 pixels).
		for (; px < x2; ++px) {
			const size_t i = row + px;
			if (mat[i] == 0xFFFFFFFFu) continue;
			const word z16 = zEnc[i];
			if (z16 == 0) continue;
			const float z = float(0xFF80 - z16) * invZScale;
			const float t = 1.0f - z * invFZP;
			const float fog = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
			const dword v = out[i];
			int B = std::max(10, int(((v      ) & 0xFF) * fog));
			int G = std::max(10, int(((v >>  8) & 0xFF) * fog));
			int R = std::max(10, int(((v >> 16) & 0xFF) * fog));
			out[i] = dword(B) | (dword(G) << 8) | (dword(R) << 16) | 0xFF000000u;
		}
	}
}

// ─── Volumetric spotlight cones (screen-space ray-march) ────────────────
//
// Per-pixel: for each spotlight visible in the tile, find the segment of
// the view ray that's both inside the cone and in front of the surface,
// then integrate density × distance falloff × cone falloff along it.
// Uses the same per-tile spot SoA the lighting kernel already builds —
// no separate culling required.
//
// Math (view-space, ray origin = camera):
//   Pixel ray direction: V = (X, Y, 1) where X=(px-CntrEX)*invFOVX,
//                                            Y=(CntrEY-py)*invFOVY.
//   Cone (apex P, axis D, half-angle cosα²=c²): point Q is inside iff
//     (D·(Q-P))² ≥ c²|Q-P|² AND D·(Q-P) ≥ 0.
//   Substituting Q = z_s · V gives a quadratic in z_s with
//     a = (D·V)² - c²(V·V),  b = 2(c²(V·P) - (D·V)(D·P)),
//     c_q = (D·P)² - c²|P|².
//   Real roots → ray crosses cone boundary; clamp interval to
//     [NearZ, min(z_surf, z_at_range)] and integrate.
static void Render_VolumetricCones_Tile(int x1, int y1, int x2, int y2,
                                         const ViewLightsSoA *lights,
                                         const int *spotIdx, int spotCount,
                                         float invFOVX, float invFOVY,
                                         float invZScale, float density,
                                         float fogZ, float invFogZ) {
    if (spotCount == 0) return;
    // fogZ > 0 means scene is fogged: clamp ray to FZP and attenuate each
    // sample by the same (1 - z/FZP) the surface fog pass uses, so the
    // cone fades with depth instead of floating in the cleared backdrop
    // past the fog cutoff. fogZ <= 0 means no fog: no clamp/attenuation.
    // NOTE: iterate the frame-global ViewLightsSoA (not per-tile
    // TileLights). The per-tile lists apply a depth cull that's correct
    // for surface lighting but wrong for volumetric integration — a
    // tile whose surface is past a spot's z-extent is excluded, even
    // though the camera→surface ray can still cross the spot's cone
    // volume. Using the unfiltered list keeps cones consistent across
    // tile boundaries; the per-pixel quadratic test culls per-pixel.

    dword *out = reinterpret_cast<dword*>(VPage);
    const uint16_t *zEnc = ZPage16;
    const int N_SAMPLES = std::max(1, fds::FeatureFlags::vol_n_samples());
    const float inv_N = 1.0f / float(N_SAMPLES);
    const bool vecPath = fds::FeatureFlags::vol_vec();
    const bool analyticCone = fds::FeatureFlags::vol_cone_analytic();
    // Path-counter bump — once per tile call, not per (spot × batch).
    // useAnalytic is constant within a call (depends only on the cone-
    // analytic flag), so a single increment per call is the right
    // granularity. The previous per-batch fetch_add was dragging ~3M
    // atomic ops/frame in city. Fog is handled inside the analytic path
    // via a midpoint (1 - z·invFogZ)² evaluation — same trade as the
    // midpoint coneAtten / shadow tap.
    const bool useAnalytic = analyticCone;
    const float noiseStrength = fds::FeatureFlags::vol_analytic_noise();
    if (fds::FeatureFlags::vol_prof()) {
        (useAnalytic ? g_coneAnalyticHits
                     : g_coneRaymarchHits)
            .fetch_add(1, std::memory_order_relaxed);
    }

    for (int py = y1; py < y2; ++py) {
        const float Y = (CntrEY - float(py)) * invFOVY;
        const size_t row = size_t(py) * size_t(XRes);
        if (vecPath) {
            // ─── Pixel-major SIMD ──────────────────────────────────────
            // 8 lanes = 8 independent rays. Per-pixel setup and per-spot
            // scalar quadratic solve are scalar (the a-sign branching is
            // too hairy to vectorize cleanly); per-sample integration
            // runs 8-wide across pixels. Wins via no wasted lanes at
            // low N, independent dependency chains per lane (better
            // OoO than 8-samples-of-one-ray batching), and per-spot
            // setup amortized across 8 pixels. Shadow lookup stays
            // scalar per-lane — texture gather is too expensive on CPU.
            for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                const int pxEnd     = std::min(pxBase + 8, x2);
                const int laneCount = pxEnd - pxBase;

                alignas(32) float    Xarr[8] = {};
                alignas(32) float    uVarr[8] = {};
                alignas(32) uint32_t pxHashArr[8] = {};
                alignas(32) float    zMaxArr[8] = {};
                bool anyAlive = false;
                for (int lane = 0; lane < laneCount; ++lane) {
                    const int px = pxBase + lane;
                    const float X = (float(px) - CntrEX) * invFOVX;
                    Xarr[lane]  = X;
                    uVarr[lane] = X*X + Y*Y + 1.0f;
                    uint32_t h = uint32_t(px) * 0x9E3779B9u
                               + uint32_t(py) * 0x85EBCA6Bu
                               + 0xCAFEBABEu;
                    h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
                    pxHashArr[lane] = h;
                    const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                    const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
                    float zM = (zSurf > 0.0f) ? zSurf : zSky;
                    if (fogZ > 0.0f && zM > fogZ) zM = fogZ;
                    constexpr float zMin = 0.05f;
                    if (zM > zMin) { zMaxArr[lane] = zM; anyAlive = true; }
                }
                if (!anyAlive) continue;

                alignas(32) float accB[8] = {}, accG[8] = {}, accR[8] = {};

                for (int s = 0; s < spotCount; ++s) {
                    const int li = spotIdx[s];
                    // Per-batch rect cull: skip if the 8-pixel batch
                    // (per-batch rect-cull experiment was reverted —
                    // per-tile cull at dispatcher already does screen-
                    // rect check; per-batch overhead didn't pay across
                    // the city sweep.)
                    const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
                    const float Dx = lights->dirX[li], Dy = lights->dirY[li], Dz = lights->dirZ[li];
                    const float cosO = lights->cosOuter[li];
                    const float cosI = lights->cosInner[li];
                    const float r2   = lights->range2[li];
                    const float rr   = lights->rRange[li];
                    const float DP   = Dx*Px + Dy*Py_l + Dz*Pz;
                    const float PP   = Px*Px + Py_l*Py_l + Pz*Pz;
                    const float c2   = cosO * cosO;
                    const float inv_cosI_minus_cosO = 1.0f / (cosI - cosO);

                    alignas(32) float zLoArr[8] = {};
                    alignas(32) float zHiArr[8] = {};
                    alignas(32) float aliveLane[8] = {};
                    bool spotAlive = false;
                    for (int lane = 0; lane < laneCount; ++lane) {
                        if (zMaxArr[lane] <= 0.0f) continue;
                        const float X = Xarr[lane];
                        const float uV = uVarr[lane];
                        const float zMax = zMaxArr[lane];
                        constexpr float zMin = 0.05f;
                        const float DV = Dx*X + Dy*Y + Dz;
                        const float VP = X*Px + Y*Py_l + Pz;
                        const float a  = DV*DV - c2 * uV;
                        const float b  = 2.0f * (c2 * VP - DV * DP);
                        const float cq = DP*DP - c2 * PP;
                        const float sphereC    = PP - r2;
                        const float sphereDisc = VP*VP - uV * sphereC;
                        if (sphereDisc < 0.0f) continue;
                        const float sphereSq = std::sqrt(sphereDisc);
                        const float invUV    = 1.0f / uV;
                        const float zSphLo   = (VP - sphereSq) * invUV;
                        const float zSphHi   = (VP + sphereSq) * invUV;
                        float zLo, zHi;
                        if (a < -1e-8f) {
                            const float disc = b*b - 4.0f*a*cq;
                            if (disc < 0.0f) continue;
                            const float sq = std::sqrt(disc);
                            const float inv2a = 1.0f / (2.0f * a);
                            const float r1 = (-b - sq) * inv2a;
                            const float r2_ = (-b + sq) * inv2a;
                            zLo = std::min(r1, r2_);
                            zHi = std::max(r1, r2_);
                        } else if (a > 1e-8f) {
                            const float disc = b*b - 4.0f*a*cq;
                            if (disc < 0.0f) {
                                zLo = zMin;
                                zHi = zMax;
                            } else {
                                const float sq = std::sqrt(disc);
                                const float inv2a = 1.0f / (2.0f * a);
                                const float root1 = (-b - sq) * inv2a;
                                const float root2 = (-b + sq) * inv2a;
                                const float r1Q = std::min(root1, root2);
                                const float r2Q = std::max(root1, root2);
                                if (DV > 1e-6f) {
                                    zLo = std::max(r2Q, zMin);
                                    zHi = zMax;
                                } else if (DV < -1e-6f) {
                                    zLo = zMin;
                                    zHi = std::min(r1Q, zMax);
                                } else {
                                    continue;
                                }
                                if (zHi <= zLo) continue;
                            }
                        } else {
                            continue;
                        }
                        if (zLo < zSphLo) zLo = zSphLo;
                        if (zHi > zSphHi) zHi = zSphHi;
                        if (zLo < zMin)   zLo = zMin;
                        if (zHi <= zLo)   continue;
                        if (zLo >= zMax)  continue;
                        if (std::fabs(DV) > 1e-6f) {
                            const float zFwd = DP / DV;
                            if (DV > 0.0f) { if (zLo < zFwd) zLo = zFwd; }
                            else           { if (zHi > zFwd) zHi = zFwd; }
                            if (zLo >= zHi) continue;
                        }
                        zLoArr[lane]    = zLo;
                        zHiArr[lane]    = zHi;
                        aliveLane[lane] = 1.0f;
                        spotAlive = true;
                    }
                    if (!spotAlive) continue;

                    const int32_t smIdx = lights->shadowMapIdx[li];
                    const ShadowMap *sm = (smIdx >= 0 && size_t(smIdx) < g_shadowMaps.size())
                                          ? &g_shadowMaps[smIdx] : nullptr;
                    float sm_m00=0, sm_m01=0, sm_m02=0, sm_ox=0;
                    float sm_m10=0, sm_m11=0, sm_m12=0, sm_oy=0;
                    float sm_m20=0, sm_m21=0, sm_m22=0, sm_oz=0;
                    float sm_cntrX=0, sm_cntrY=0, sm_perspX=0, sm_perspY=0;
                    float sm_zScale=0;
                    const uint16_t *sm_depth = nullptr;
                    int sm_xres=0, sm_yres=0;
                    if (sm) {
                        sm_m00=sm->viewToLight[0][0]; sm_m01=sm->viewToLight[0][1]; sm_m02=sm->viewToLight[0][2]; sm_ox=sm->viewToLightOffset.x;
                        sm_m10=sm->viewToLight[1][0]; sm_m11=sm->viewToLight[1][1]; sm_m12=sm->viewToLight[1][2]; sm_oy=sm->viewToLightOffset.y;
                        sm_m20=sm->viewToLight[2][0]; sm_m21=sm->viewToLight[2][1]; sm_m22=sm->viewToLight[2][2]; sm_oz=sm->viewToLightOffset.z;
                        sm_cntrX=sm->cntrX; sm_cntrY=sm->cntrY;
                        sm_perspX=sm->perspX; sm_perspY=sm->perspY;
                        sm_zScale=sm->zScale;
                        sm_depth=sm->depth.data();
                        sm_xres=sm->xres; sm_yres=sm->yres;
                    }

                    alignas(32) float dzArr[8] = {}, invDzArr[8] = {}, fadeStartArr[8] = {};
                    for (int lane = 0; lane < 8; ++lane) {
                        if (aliveLane[lane] == 0.0f) continue;
                        const float d = (zHiArr[lane] - zLoArr[lane]) * inv_N;
                        dzArr[lane]        = d;
                        invDzArr[lane]     = 1.0f / d;
                        fadeStartArr[lane] = zMaxArr[lane] - d;
                    }

                    const __m256 vX_v        = _mm256_load_ps(Xarr);
                    const __m256 vY_v        = _mm256_set1_ps(Y);
                    const __m256 vZMax_v     = _mm256_load_ps(zMaxArr);
                    const __m256 vZLo_v      = _mm256_load_ps(zLoArr);
                    const __m256 vDz_v       = _mm256_load_ps(dzArr);
                    const __m256 vInvDz_v    = _mm256_load_ps(invDzArr);
                    const __m256 vFadeStart_v= _mm256_load_ps(fadeStartArr);
                    const __m256 vAlive_v    = _mm256_load_ps(aliveLane);
                    const __m256 vPx_v       = _mm256_set1_ps(Px);
                    const __m256 vPy_v       = _mm256_set1_ps(Py_l);
                    const __m256 vPz_v       = _mm256_set1_ps(Pz);
                    const __m256 vDx_v       = _mm256_set1_ps(Dx);
                    const __m256 vDy_v       = _mm256_set1_ps(Dy);
                    const __m256 vDz_dir_v   = _mm256_set1_ps(Dz);
                    const __m256 vR2_v       = _mm256_set1_ps(r2);
                    const __m256 vRR_v       = _mm256_set1_ps(rr);
                    const __m256 vCosO_v     = _mm256_set1_ps(cosO);
                    const __m256 vCosI_v     = _mm256_set1_ps(cosI);
                    const __m256 vInvCIO_v   = _mm256_set1_ps(inv_cosI_minus_cosO);
                    const __m256 vInvFogZ_v  = _mm256_set1_ps(invFogZ);
                    const __m256 vZero_v     = _mm256_setzero_ps();
                    const __m256 vOne_v      = _mm256_set1_ps(1.0f);
                    const __m256 vTwo_v      = _mm256_set1_ps(2.0f);
                    const __m256 vThree_v    = _mm256_set1_ps(3.0f);
                    const __m256 vEps_v      = _mm256_set1_ps(1e-6f);
                    const __m256 vPt05_v     = _mm256_set1_ps(0.05f);
                    const __m256 mAlive      = _mm256_cmp_ps(vAlive_v, vZero_v, _CMP_GT_OQ);
                    __m256 accV = vZero_v;

                    // ─── Analytic cone branch ────────────────────────
                    // Closed-form arctan integral of inverse-square dist
                    // attenuation over the cone-clipped segment, with
                    // coneAtten (smoothstep cosO→cosI) approximated at
                    // segment midpoint. Drops the (1-rr·d)² near-edge
                    // cutoff (same trade as omni analytic).
                    //
                    // Approximations vs ray-march:
                    //   (a) coneAtten / surfaceFade / fogAtten —
                    //       evaluated at the segment midpoint.
                    //   (b) shadow occupancy — a single shadow-map tap
                    //       at z=zMid. Whole segment in or out (binary).
                    //       Stair-steps at shadow boundaries; tolerable
                    //       because halos are inherently diffuse.
                    if (useAnalytic) {
                        // α z² + β z + γ = rr²·d²(z) + 0.05
                        const __m256 vRR2_v   = _mm256_mul_ps(vRR_v, vRR_v);
                        // Per-lane uV (= X²+Y²+1, varies per pixel).
                        const __m256 vUv_v    = _mm256_load_ps(uVarr);
                        // VP = X·Px + Y·Py + Pz per lane.
                        const __m256 vVP_v    = _mm256_fmadd_ps(vX_v, vPx_v,
                                                _mm256_fmadd_ps(vY_v, vPy_v, vPz_v));
                        const __m256 vPP_v    = _mm256_set1_ps(PP);
                        const __m256 vAlpha   = _mm256_mul_ps(vRR2_v, vUv_v);
                        const __m256 vBeta    = _mm256_mul_ps(
                                                _mm256_set1_ps(-2.0f),
                                                _mm256_mul_ps(vRR2_v, vVP_v));
                        const __m256 vGamma   = _mm256_fmadd_ps(vRR2_v, vPP_v, vPt05_v);
                        const __m256 vDiscQ   = _mm256_fmsub_ps(
                                                _mm256_mul_ps(_mm256_set1_ps(4.0f), vAlpha), vGamma,
                                                _mm256_mul_ps(vBeta, vBeta));
                        const __m256 mDisc    = _mm256_cmp_ps(vDiscQ, vZero_v, _CMP_GT_OQ);
                        const __m256 vSafeDisc = _mm256_blendv_ps(vOne_v, vDiscQ, mDisc);
                        const __m256 vInvD    = _mm256_rsqrt_ps(vSafeDisc);

                        const __m256 vTwoA    = _mm256_add_ps(vAlpha, vAlpha);
                        const __m256 vZHi_v   = _mm256_load_ps(zHiArr);
                        const __m256 vArgHi   = _mm256_mul_ps(vInvD,
                                                _mm256_fmadd_ps(vTwoA, vZHi_v, vBeta));
                        const __m256 vArgLo   = _mm256_mul_ps(vInvD,
                                                _mm256_fmadd_ps(vTwoA, vZLo_v, vBeta));
                        const __m256 vAtanHi  = atan_approx_x8(vArgHi);
                        const __m256 vAtanLo  = atan_approx_x8(vArgLo);
                        const __m256 vIntegral = _mm256_mul_ps(
                                                 _mm256_add_ps(vInvD, vInvD),
                                                 _mm256_sub_ps(vAtanHi, vAtanLo));

                        // Midpoint sample: cosT_mid and surfaceFade_mid
                        // approximate the otherwise-z-dependent factors.
                        const __m256 vZMid    = _mm256_mul_ps(
                                                _mm256_add_ps(vZLo_v, vZHi_v),
                                                _mm256_set1_ps(0.5f));
                        const __m256 Wx_m = _mm256_sub_ps(_mm256_mul_ps(vZMid, vX_v), vPx_v);
                        const __m256 Wy_m = _mm256_sub_ps(_mm256_mul_ps(vZMid, vY_v), vPy_v);
                        const __m256 Wz_m = _mm256_sub_ps(vZMid, vPz_v);
                        const __m256 W2_m = _mm256_fmadd_ps(Wx_m, Wx_m,
                                            _mm256_fmadd_ps(Wy_m, Wy_m,
                                             _mm256_mul_ps(Wz_m, Wz_m)));
                        const __m256 DW_m = _mm256_fmadd_ps(vDx_v, Wx_m,
                                            _mm256_fmadd_ps(vDy_v, Wy_m,
                                             _mm256_mul_ps(vDz_dir_v, Wz_m)));
                        const __m256 safeW2_m = _mm256_blendv_ps(vOne_v, W2_m, mAlive);
                        const __m256 invLen_m = _mm256_rsqrt_ps(safeW2_m);
                        const __m256 cosT_m   = _mm256_mul_ps(DW_m, invLen_m);
                        // Near-edge softness: ray-march multiplies by
                        // (1 - rr·d)² to fade the integrand at the
                        // sphere surface. Reintroduce that as a midpoint
                        // factor so the analytic doesn't show a hard
                        // boundary where the halo ends. dist_mid =
                        // W²·invLen (rsqrt identity).
                        const __m256 dist_m   = _mm256_mul_ps(W2_m, invLen_m);
                        __m256 softEdge_m     = _mm256_sub_ps(vOne_v,
                                                _mm256_mul_ps(vRR_v, dist_m));
                        softEdge_m = _mm256_max_ps(vZero_v, softEdge_m);
                        softEdge_m = _mm256_mul_ps(softEdge_m, softEdge_m);
                        // coneAtten at midpoint: smoothstep(cosO→cosI).
                        __m256 t_m = _mm256_mul_ps(_mm256_sub_ps(cosT_m, vCosO_v), vInvCIO_v);
                        t_m = _mm256_max_ps(vZero_v, _mm256_min_ps(vOne_v, t_m));
                        const __m256 smooth_m = _mm256_mul_ps(
                                                _mm256_mul_ps(t_m, t_m),
                                                _mm256_sub_ps(vThree_v,
                                                  _mm256_mul_ps(vTwo_v, t_m)));
                        const __m256 mInner_m = _mm256_cmp_ps(cosT_m, vCosI_v, _CMP_GE_OQ);
                        const __m256 coneAtten_m = _mm256_blendv_ps(smooth_m, vOne_v, mInner_m);
                        // surfaceFade at midpoint.
                        const __m256 mFade_m  = _mm256_cmp_ps(vZMid, vFadeStart_v, _CMP_GT_OQ);
                        const __m256 fadeVal_m = _mm256_mul_ps(
                                                 _mm256_sub_ps(vZMax_v, vZMid), vInvDz_v);
                        const __m256 surfaceFade_m = _mm256_blendv_ps(vOne_v, fadeVal_m, mFade_m);

                        // Match ray-march brightness scaling: N × mean.
                        const __m256 vIntervalLen = _mm256_sub_ps(vZHi_v, vZLo_v);
                        const __m256 vSafeLen  = _mm256_blendv_ps(vOne_v, vIntervalLen, mAlive);
                        const __m256 vN        = _mm256_set1_ps(float(N_SAMPLES));
                        // mean per lane: integral / interval; final
                        // contribution per "sample-unit": mean × N ×
                        // coneAtten_mid × surfaceFade_mid.
                        __m256 vAcc = _mm256_mul_ps(vIntegral, _mm256_rcp_ps(vSafeLen));
                        vAcc = _mm256_mul_ps(vAcc, vN);
                        vAcc = _mm256_mul_ps(vAcc, coneAtten_m);
                        vAcc = _mm256_mul_ps(vAcc, surfaceFade_m);
                        vAcc = _mm256_mul_ps(vAcc, softEdge_m);

                        // Midpoint fog: ray-march path uses (1-z·invFogZ)²
                        // per sample; here we sample once at z=zMid. Same
                        // approximation strategy as midpoint cone/shadow.
                        if (invFogZ > 0.0f) {
                            __m256 fog_m = _mm256_sub_ps(vOne_v,
                                            _mm256_mul_ps(vZMid, vInvFogZ_v));
                            fog_m = _mm256_max_ps(vZero_v, fog_m);
                            fog_m = _mm256_mul_ps(fog_m, fog_m);
                            vAcc = _mm256_mul_ps(vAcc, fog_m);
                        }

                        // Per-pixel multiplicative noise: replicates the
                        // visual texture of the ray-march path (whose
                        // stochastic sample offsets produce inter-pixel
                        // variation) without sacrificing the analytic
                        // smoothness. Hash from existing pxHashArr.
                        if (noiseStrength > 0.0f) {
                            alignas(32) float noiseBuf[8];
                            for (int lane = 0; lane < 8; ++lane) {
                                // pxHashArr is already stable per pixel.
                                // Map to [-0.5, +0.5) then scale.
                                const float u =
                                    float(pxHashArr[lane] >> 16) * (1.0f/65536.0f);
                                noiseBuf[lane] = 1.0f + noiseStrength * (u - 0.5f);
                            }
                            vAcc = _mm256_mul_ps(vAcc, _mm256_load_ps(noiseBuf));
                        }
                        // Mask out lanes where: discQ<=0, cone-axis test
                        // fails (cosT<cosO at midpoint), or lane dead.
                        const __m256 mAng = _mm256_cmp_ps(cosT_m, vCosO_v, _CMP_GE_OQ);
                        __m256 m          = _mm256_and_ps(mAlive,
                                            _mm256_and_ps(mDisc, mAng));

                        // Midpoint shadow tap — one sample at z=zMid
                        // gates the whole segment. Stair-steps at
                        // shadow boundaries; tolerated because halos
                        // are diffuse. Mirrors the in-loop sm path
                        // above but uses zMid instead of per-sample z.
                        if (sm) {
                            alignas(32) float maskArr_s[8], zArr_s[8];
                            _mm256_store_ps(maskArr_s, m);
                            _mm256_store_ps(zArr_s, vZMid);
                            alignas(32) float shadowMul_s[8] =
                                {1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
                            for (int lane = 0; lane < 8; ++lane) {
                                if (maskArr_s[lane] == 0) continue;
                                const float zL = zArr_s[lane];
                                const float Xl = Xarr[lane];
                                const float zX = zL * Xl, zY = zL * Y;
                                const float lx = sm_m00*zX + sm_m01*zY + sm_m02*zL + sm_ox;
                                const float ly = sm_m10*zX + sm_m11*zY + sm_m12*zL + sm_oy;
                                const float lz = sm_m20*zX + sm_m21*zY + sm_m22*zL + sm_oz;
                                if (lz <= 0.0f) continue;
                                const float invLZ = 1.0f / lz;
                                const float smX = sm_cntrX + sm_perspX * lx * invLZ;
                                const float smY = sm_cntrY - sm_perspY * ly * invLZ;
                                const int iX = int(smX), iY = int(smY);
                                if (uint32_t(iX) >= uint32_t(sm_xres) ||
                                    uint32_t(iY) >= uint32_t(sm_yres)) continue;
                                int pixZ = 0xFF80 - int(lz * sm_zScale);
                                if (pixZ < 0) pixZ = 0;
                                if (pixZ > 0xFFFF) pixZ = 0xFFFF;
                                const int biased = pixZ + 128;
                                const uint16_t shadowZ =
                                    sm_depth[size_t(iY) * size_t(sm_xres) + size_t(iX)];
                                if (biased < int(shadowZ)) shadowMul_s[lane] = 0.0f;
                            }
                            const __m256 vShad_s = _mm256_load_ps(shadowMul_s);
                            m = _mm256_and_ps(m,
                                _mm256_cmp_ps(vShad_s, _mm256_set1_ps(0.5f), _CMP_GT_OQ));
                        }

                        accV = _mm256_and_ps(vAcc, m);
                    } else {
                    for (int k = 0; k < N_SAMPLES; ++k) {
                        alignas(32) float fracBuf[8];
                        for (int lane = 0; lane < 8; ++lane) {
                            const uint32_t h = pxHashArr[lane]
                                + uint32_t(k) * 0x9E3779B9u
                                + uint32_t(s) * 0x6F4A7531u;
                            fracBuf[lane] = float(h >> 16) * (1.0f / 65536.0f);
                        }
                        const __m256 vFrac = _mm256_load_ps(fracBuf);

                        const __m256 vKf = _mm256_set1_ps(float(k));
                        const __m256 vZ  = _mm256_fmadd_ps(
                            _mm256_add_ps(vKf, vFrac), vDz_v, vZLo_v);

                        __m256 mask = _mm256_and_ps(mAlive,
                            _mm256_cmp_ps(vZ, vZMax_v, _CMP_LT_OQ));

                        const __m256 mFade   = _mm256_cmp_ps(vZ, vFadeStart_v, _CMP_GT_OQ);
                        const __m256 fadeVal = _mm256_mul_ps(_mm256_sub_ps(vZMax_v, vZ), vInvDz_v);
                        const __m256 surfaceFade = _mm256_blendv_ps(vOne_v, fadeVal, mFade);

                        const __m256 Wx = _mm256_sub_ps(_mm256_mul_ps(vZ, vX_v), vPx_v);
                        const __m256 Wy = _mm256_sub_ps(_mm256_mul_ps(vZ, vY_v), vPy_v);
                        const __m256 Wz = _mm256_sub_ps(vZ, vPz_v);
                        const __m256 W2 = _mm256_fmadd_ps(Wx, Wx,
                                           _mm256_fmadd_ps(Wy, Wy,
                                            _mm256_mul_ps(Wz, Wz)));
                        mask = _mm256_and_ps(mask, _mm256_cmp_ps(W2, vR2_v, _CMP_LE_OQ));
                        mask = _mm256_and_ps(mask, _mm256_cmp_ps(W2, vEps_v, _CMP_GT_OQ));

                        const __m256 DW = _mm256_fmadd_ps(vDx_v, Wx,
                                           _mm256_fmadd_ps(vDy_v, Wy,
                                            _mm256_mul_ps(vDz_dir_v, Wz)));
                        mask = _mm256_and_ps(mask, _mm256_cmp_ps(DW, vZero_v, _CMP_GT_OQ));

                        const __m256 safeW2 = _mm256_blendv_ps(vOne_v, W2, mask);
                        const __m256 invLen = _mm256_rsqrt_ps(safeW2);
                        const __m256 dist   = _mm256_mul_ps(W2, invLen);
                        const __m256 cosT   = _mm256_mul_ps(DW, invLen);
                        mask = _mm256_and_ps(mask, _mm256_cmp_ps(cosT, vCosO_v, _CMP_GE_OQ));

                        __m256 t_v = _mm256_mul_ps(_mm256_sub_ps(cosT, vCosO_v), vInvCIO_v);
                        t_v = _mm256_max_ps(vZero_v, _mm256_min_ps(vOne_v, t_v));
                        const __m256 smooth = _mm256_mul_ps(
                            _mm256_mul_ps(t_v, t_v),
                            _mm256_sub_ps(vThree_v, _mm256_mul_ps(vTwo_v, t_v)));
                        const __m256 mInner    = _mm256_cmp_ps(cosT, vCosI_v, _CMP_GE_OQ);
                        const __m256 coneAtten = _mm256_blendv_ps(smooth, vOne_v, mInner);

                        const __m256 dr        = _mm256_mul_ps(dist, vRR_v);
                        const __m256 cutoff    = _mm256_sub_ps(vOne_v, dr);
                        const __m256 invSqDen  = _mm256_fmadd_ps(dr, dr, vPt05_v);
                        const __m256 invSq     = _mm256_rcp_ps(invSqDen);
                        const __m256 distAtten = _mm256_mul_ps(_mm256_mul_ps(cutoff, cutoff), invSq);

                        __m256 fogAtten = vOne_v;
                        if (invFogZ > 0.0f) {
                            fogAtten = _mm256_sub_ps(vOne_v, _mm256_mul_ps(vZ, vInvFogZ_v));
                            fogAtten = _mm256_max_ps(vZero_v, fogAtten);
                            fogAtten = _mm256_mul_ps(fogAtten, fogAtten);
                        }

                        if (sm) {
                            alignas(32) float maskArr[8], zArr[8];
                            _mm256_store_ps(maskArr, mask);
                            _mm256_store_ps(zArr, vZ);
                            alignas(32) float shadowMul[8] =
                                {1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
                            for (int lane = 0; lane < 8; ++lane) {
                                if (maskArr[lane] == 0) continue;
                                const float zL = zArr[lane];
                                const float Xl = Xarr[lane];
                                const float zX = zL * Xl, zY = zL * Y;
                                const float lx = sm_m00*zX + sm_m01*zY + sm_m02*zL + sm_ox;
                                const float ly = sm_m10*zX + sm_m11*zY + sm_m12*zL + sm_oy;
                                const float lz = sm_m20*zX + sm_m21*zY + sm_m22*zL + sm_oz;
                                if (lz <= 0.0f) continue;
                                const float invLZ = 1.0f / lz;
                                const float smX = sm_cntrX + sm_perspX * lx * invLZ;
                                const float smY = sm_cntrY - sm_perspY * ly * invLZ;
                                const int iX = int(smX), iY = int(smY);
                                if (uint32_t(iX) >= uint32_t(sm_xres) ||
                                    uint32_t(iY) >= uint32_t(sm_yres)) continue;
                                int pixZ = 0xFF80 - int(lz * sm_zScale);
                                if (pixZ < 0) pixZ = 0;
                                if (pixZ > 0xFFFF) pixZ = 0xFFFF;
                                const int biased = pixZ + 128;
                                const uint16_t shadowZ =
                                    sm_depth[size_t(iY) * size_t(sm_xres) + size_t(iX)];
                                if (biased < int(shadowZ)) shadowMul[lane] = 0.0f;
                            }
                            const __m256 vShad = _mm256_load_ps(shadowMul);
                            mask = _mm256_and_ps(mask,
                                _mm256_cmp_ps(vShad, _mm256_set1_ps(0.5f), _CMP_GT_OQ));
                        }

                        __m256 contrib = _mm256_mul_ps(
                            _mm256_mul_ps(coneAtten, distAtten),
                            _mm256_mul_ps(fogAtten, surfaceFade));
                        contrib = _mm256_and_ps(contrib, mask);
                        accV = _mm256_add_ps(accV, contrib);
                    }
                    }

                    alignas(32) float accArr[8];
                    _mm256_store_ps(accArr, accV);
                    const float colB = lights->colB[li];
                    const float colG = lights->colG[li];
                    const float colR = lights->colR[li];
                    for (int lane = 0; lane < 8; ++lane) {
                        if (accArr[lane] <= 0.0f) continue;
                        const float w = accArr[lane] * density;
                        accB[lane] += w * colB;
                        accG[lane] += w * colG;
                        accR[lane] += w * colR;
                    }
                }

                for (int lane = 0; lane < laneCount; ++lane) {
                    if (accB[lane] <= 0.0f && accG[lane] <= 0.0f && accR[lane] <= 0.0f) continue;
                    const int px = pxBase + lane;
                    const size_t i = row + size_t(px);
                    const dword pix = out[i];
                    int newR = int((pix >> 16) & 0xFF) + int(accR[lane]);
                    int newG = int((pix >>  8) & 0xFF) + int(accG[lane]);
                    int newB = int( pix        & 0xFF) + int(accB[lane]);
                    if (newR > 255) newR = 255;
                    if (newG > 255) newG = 255;
                    if (newB > 255) newB = 255;
                    out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                             |  dword(newB)        | 0xFF000000u;
                }
            }
        } else {
        for (int px = x1; px < x2; ++px) {
            const float X = (float(px) - CntrEX) * invFOVX;
            const float uV = X*X + Y*Y + 1.0f;

            // Stratified per-pixel jitter offset, in [0,1). Used inside the
            // per-spot integration to randomize sample positions within
            // each bin so the bright apex region (where distAtten peaks
            // sharply) doesn't alias into visible bands across neighbours.
            // Hash pixel coords for stability frame-to-frame (no flicker).
            // Use a proper avalanching hash (PCG-style multiply + xor-shift):
            // a plain `px*MUL + py*MUL` left adjacent pixels with nearly
            // identical high-16 bits, which manifested as horizontal bands
            // because `frac` (computed from h>>16) was nearly constant in
            // each row.
            uint32_t pxHash = uint32_t(px) * 0x9E3779B9u
                            + uint32_t(py) * 0x85EBCA6Bu
                            + 0xCAFEBABEu;
            pxHash ^= pxHash >> 13;
            pxHash *= 0xC2B2AE35u;
            pxHash ^= pxHash >> 16;

            // Surface depth: 0xFF80 - enc = z*zscale. enc=0 means "sky"
            // (no surface) → cap at fogZ if fogged, else far.
            const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
            const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
            float zMax = (zSurf > 0.0f) ? zSurf : zSky;
            if (fogZ > 0.0f && zMax > fogZ) zMax = fogZ;
            constexpr float zMin = 0.05f;
            if (zMax <= zMin) continue;

            float accB = 0.0f, accG = 0.0f, accR = 0.0f;
            for (int s = 0; s < spotCount; ++s) {
                const int li = spotIdx[s];
                const float Px = lights->posX[li], Py = lights->posY[li], Pz = lights->posZ[li];
                const float Dx = lights->dirX[li], Dy = lights->dirY[li], Dz = lights->dirZ[li];
                const float cosO = lights->cosOuter[li];
                const float cosI = lights->cosInner[li];
                const float r2   = lights->range2[li];
                const float rr   = lights->rRange[li];

                const float DV = Dx*X + Dy*Y + Dz;
                const float DP = Dx*Px + Dy*Py + Dz*Pz;
                const float VP = X*Px + Y*Py + Pz;
                const float PP = Px*Px + Py*Py + Pz*Pz;
                const float c2 = cosO * cosO;

                const float a = DV*DV - c2 * uV;
                const float b = 2.0f * (c2 * VP - DV * DP);
                const float cq = DP*DP - c2 * PP;

                // Solve a*z² + b*z + cq = 0 (the "ray inside cone half-
                // angle" quadratic). The sign of a controls which side
                // of the roots is "inside-cone":
                //   a<0 → ray crosses the cone direction broadside.
                //         Inside-cone is BETWEEN [r1, r2]. (Looking AT
                //         the cone from outside.)
                //   a>0 → ray fits within the cone half-angle (looking
                //         ALONG the cone direction, from any position).
                //         Inside-cone is OUTSIDE [r1, r2] (z ≤ r1 OR
                //         z ≥ r2). Classify by where the visible
                //         interval [zMin, zMax] sits and either
                //         integrate it fully or skip entirely. Per-
                //         sample DW>0 + cosT≥cosO filters cull the
                //         outside-cone middle when visible straddles.
                //   a≈0 → ray exactly parallel to cone wall, no volume.
                //
                // Sphere bounds (the spot's range sphere) also clamp the
                // interval below — and decouple sample positions from
                // the quantized surface depth (zMax fades per-sample).
                const float sphereC = PP - r2;
                const float sphereDisc = VP*VP - uV * sphereC;
                if (sphereDisc < 0.0f) continue;  // ray misses range sphere
                const float sphereSq = std::sqrt(sphereDisc);
                const float invUV    = 1.0f / uV;
                const float zSphLo   = (VP - sphereSq) * invUV;
                const float zSphHi   = (VP + sphereSq) * invUV;

                float zLo, zHi;
                if (a < -1e-8f) {
                    const float disc = b*b - 4.0f*a*cq;
                    if (disc < 0.0f) continue;
                    const float sq = std::sqrt(disc);
                    const float inv2a = 1.0f / (2.0f * a);
                    const float r1 = (-b - sq) * inv2a;
                    const float r2_ = (-b + sq) * inv2a;
                    zLo = std::min(r1, r2_);
                    zHi = std::max(r1, r2_);
                } else if (a > 1e-8f) {
                    const float disc = b*b - 4.0f*a*cq;
                    if (disc < 0.0f) {
                        // Q always positive → ray entirely inside cone
                        // (forward half filtered per-sample).
                        zLo = zMin;
                        zHi = zMax;
                    } else {
                        const float sq = std::sqrt(disc);
                        const float inv2a = 1.0f / (2.0f * a);
                        const float root1 = (-b - sq) * inv2a;
                        const float root2 = (-b + sq) * inv2a;
                        const float r1Q = std::min(root1, root2);
                        const float r2Q = std::max(root1, root2);
                        // For a>0, inside-cone is z ≤ r1 OR z ≥ r2.
                        // The forward filter zFwd = DP/DV lies between
                        // r1 and r2 (zFwd is the apex projection along
                        // the ray; for a>0 the apex projection sits in
                        // the outside-cone middle between cone-wall
                        // crossings). So the forward-inside-cone
                        // region is one side:
                        //   DV > 0 → forward is z > zFwd → take z ≥ r2
                        //   DV < 0 → forward is z < zFwd → take z ≤ r1
                        //   DV ≈ 0 → ray perpendicular to cone dir,
                        //            no meaningful forward direction.
                        // This is tighter than integrating [zMin, zMax]
                        // and skipping wrong-side samples per-sample:
                        // sample positions stay entirely inside the
                        // cone, eliminating the cone-wall-sweep stripe
                        // artifact that the wider interval produced.
                        if (DV > 1e-6f) {
                            zLo = std::max(r2Q, zMin);
                            zHi = zMax;
                        } else if (DV < -1e-6f) {
                            zLo = zMin;
                            zHi = std::min(r1Q, zMax);
                        } else {
                            continue;
                        }
                        if (zHi <= zLo) continue;
                    }
                } else {
                    continue;
                }

                // Intersect with sphere bounds (NOT with zMax — that goes
                // into the per-sample fade below). zMin keeps us forward
                // of the near plane.
                if (zLo < zSphLo) zLo = zSphLo;
                if (zHi > zSphHi) zHi = zSphHi;
                if (zLo < zMin)   zLo = zMin;
                if (zHi <= zLo)   continue;
                // Early-out: entire cone interval past the visible surface
                // (zMax is the surface/sky cap; everything past it is fully
                // occluded). Without this we'd still loop N samples for no
                // contribution.
                if (zLo >= zMax)  continue;

                // Forward-cone-half constraint: need D·W ≥ 0 i.e.
                //   z * DV - DP ≥ 0. Resolves to z ≥ DP/DV (if DV>0)
                // or z ≤ DP/DV (if DV<0). Skip if entire segment violates.
                if (std::fabs(DV) > 1e-6f) {
                    const float zFwd = DP / DV;
                    if (DV > 0.0f) { if (zLo < zFwd) zLo = zFwd; }
                    else           { if (zHi > zFwd) zHi = zFwd; }
                    if (zLo >= zHi) continue;
                }

                // Integrate N stratified-jittered samples along [zLo, zHi].
                // Each bin gets one sample placed at a random offset within
                // it — randomization breaks the periodic alignment with
                // the bright apex region that produced visible stripe
                // artifacts at fixed-position sampling. The per-spot salt
                // (s * 0x6F...) avoids correlated noise when multiple spots
                // contribute to the same pixel.
                const float dz = (zHi - zLo) * inv_N;
                const float inv_dz = 1.0f / dz;
                const float zFadeStart = zMax - dz;
                // Hoist per-spot shadow-map state out of the per-sample
                // loop. smIdx is per-light, not per-sample.
                const int32_t smIdx = lights->shadowMapIdx[li];
                const ShadowMap *sm = (smIdx >= 0 && size_t(smIdx) < g_shadowMaps.size())
                                       ? &g_shadowMaps[smIdx] : nullptr;
                // Per-spot precomputed shadow matrix rows (when sm != null).
                // Lets the per-sample shadow code use cached scalars instead
                // of indexing sm->viewToLight[r][c] each sample.
                float sm_m00=0, sm_m01=0, sm_m02=0, sm_ox=0;
                float sm_m10=0, sm_m11=0, sm_m12=0, sm_oy=0;
                float sm_m20=0, sm_m21=0, sm_m22=0, sm_oz=0;
                float sm_cntrX=0, sm_cntrY=0, sm_perspX=0, sm_perspY=0;
                float sm_zScale=0;
                const uint16_t *sm_depth = nullptr;
                int sm_xres=0, sm_yres=0;
                if (sm) {
                    sm_m00=sm->viewToLight[0][0]; sm_m01=sm->viewToLight[0][1]; sm_m02=sm->viewToLight[0][2]; sm_ox=sm->viewToLightOffset.x;
                    sm_m10=sm->viewToLight[1][0]; sm_m11=sm->viewToLight[1][1]; sm_m12=sm->viewToLight[1][2]; sm_oy=sm->viewToLightOffset.y;
                    sm_m20=sm->viewToLight[2][0]; sm_m21=sm->viewToLight[2][1]; sm_m22=sm->viewToLight[2][2]; sm_oz=sm->viewToLightOffset.z;
                    sm_cntrX=sm->cntrX; sm_cntrY=sm->cntrY;
                    sm_perspX=sm->perspX; sm_perspY=sm->perspY;
                    sm_zScale=sm->zScale;
                    sm_depth=sm->depth.data();
                    sm_xres=sm->xres; sm_yres=sm->yres;
                }
                const float inv_cosI_minus_cosO = 1.0f / (cosI - cosO);
                float acc = 0.0f;
                for (int k = 0; k < N_SAMPLES; ++k) {
                    const uint32_t h = pxHash
                        + uint32_t(k) * 0x9E3779B9u
                        + uint32_t(s) * 0x6F4A7531u;
                    const float frac = float(h >> 16) * (1.0f / 65536.0f);
                    const float z = zLo + (float(k) + frac) * dz;
                    if (z >= zMax) break;
                    float surfaceFade = 1.0f;
                    if (z > zFadeStart) {
                        surfaceFade = (zMax - z) * inv_dz;
                    }
                    const float Wx = z*X - Px;
                    const float Wy = z*Y - Py;
                    const float Wz = z    - Pz;
                    const float W2 = Wx*Wx + Wy*Wy + Wz*Wz;
                    if (W2 > r2 || W2 < 1e-6f) continue;
                    const float DW = Dx*Wx + Dy*Wy + Dz*Wz;
                    if (DW <= 0.0f) continue;
                    const float invLen = fast_rsqrt(W2);
                    const float dist = W2 * invLen;
                    const float cosT = DW * invLen;
                    if (cosT < cosO) continue;
                    float coneAtten = 1.0f;
                    if (cosT < cosI) {
                        const float t = (cosT - cosO) * inv_cosI_minus_cosO;
                        coneAtten = t * t * (3.0f - 2.0f * t);
                    }
                    const float dr = dist * rr;
                    const float cutoff = 1.0f - dr;
                    const float invSq  = 1.0f / (dr * dr + 0.05f);
                    const float distAtten = cutoff * cutoff * invSq;
                    float fogAtten = 1.0f;
                    if (invFogZ > 0.0f) {
                        fogAtten = 1.0f - z * invFogZ;
                        if (fogAtten < 0.0f) fogAtten = 0.0f;
                        fogAtten *= fogAtten;
                    }
                    // Shadow sample. sm != null fast-checked once per spot;
                    // matrix rows + map metadata cached as scalars above.
                    if (sm) {
                        const float zX = z*X, zY = z*Y;
                        const float lx = sm_m00*zX + sm_m01*zY + sm_m02*z + sm_ox;
                        const float ly = sm_m10*zX + sm_m11*zY + sm_m12*z + sm_oy;
                        const float lz = sm_m20*zX + sm_m21*zY + sm_m22*z + sm_oz;
                        if (lz > 0.0f) {
                            const float invLZ = 1.0f / lz;
                            const float smX = sm_cntrX + sm_perspX * lx * invLZ;
                            const float smY = sm_cntrY - sm_perspY * ly * invLZ;
                            const int iX = int(smX);
                            const int iY = int(smY);
                            if (uint32_t(iX) < uint32_t(sm_xres) &&
                                uint32_t(iY) < uint32_t(sm_yres)) {
                                int pixZ = 0xFF80 - int(lz * sm_zScale);
                                if (pixZ < 0) pixZ = 0;
                                if (pixZ > 0xFFFF) pixZ = 0xFFFF;
                                const int biased = pixZ + 128;
                                const uint16_t shadowZ =
                                    sm_depth[size_t(iY) * size_t(sm_xres) + size_t(iX)];
                                if (biased < int(shadowZ)) continue;  // shadowed
                            }
                        }
                    }
                    acc += coneAtten * distAtten * fogAtten * surfaceFade;
                }
                if (acc <= 0.0f) continue;
                // No dz scaling — the path-integral form (acc × dz) gave
                // shallow-angle rays through far cones much brighter results
                // than close cones (where each pixel's ray-cone segment is
                // short). Using per-sample-sum (acc only) combined with the
                // inverse-square distAtten above gives roughly position-
                // invariant brightness, biased toward close cones — matches
                // the "flashlight in fog" mental model.
                const float w = acc * density;
                accB += w * lights->colB[li];
                accG += w * lights->colG[li];
                accR += w * lights->colR[li];
            }
            if (accB <= 0.0f && accG <= 0.0f && accR <= 0.0f) continue;
            const size_t i = row + size_t(px);
            const dword pix = out[i];
            int newR = int((pix >> 16) & 0xFF) + int(accR);
            int newG = int((pix >>  8) & 0xFF) + int(accG);
            int newB = int( pix        & 0xFF) + int(accB);
            if (newR > 255) newR = 255;
            if (newG > 255) newG = 255;
            if (newB > 255) newB = 255;
            out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                     |  dword(newB)        | 0xFF000000u;
        }
        }
    }
}

// Volumetric-pass timing accumulators (one set per process; updated
// from the main thread that owns the pass dispatch). Printed every
// FDS_VOL_PROF_INTERVAL frames (default 60) by VolProf_Tick.
namespace {
    using volclk = std::chrono::high_resolution_clock;
    struct VolProf {
        double ms_cones   = 0.0;
        double ms_halos   = 0.0;
        double ms_unified = 0.0;
        double ms_skybox  = 0.0;
        int    n_cones    = 0;
        int    n_halos    = 0;
        int    n_unified  = 0;
        int    n_skybox   = 0;
        int    interval   = 60;
        int    framesSeen = 0;
    };
    VolProf g_volProf;

    struct VolProfScope {
        double *acc;
        int    *cnt;
        volclk::time_point t0;
        VolProfScope(double *a, int *c) : acc(a), cnt(c), t0(volclk::now()) {}
        ~VolProfScope() {
            if (!fds::FeatureFlags::vol_prof()) return;
            *acc += std::chrono::duration<double, std::milli>(volclk::now() - t0).count();
            ++*cnt;
        }
    };
}

// Called once per frame after all volumetric passes complete to maybe
// flush the timing summary. No-op when vol_prof flag is off.
void VolProf_Tick();

static void VolProf_Tick_impl() {
    if (!fds::FeatureFlags::vol_prof()) return;
    if (++g_volProf.framesSeen < g_volProf.interval) return;
    const int N = g_volProf.framesSeen;
    const int cAnalytic = g_coneAnalyticHits.load(std::memory_order_relaxed);
    const int cRaymarch = g_coneRaymarchHits.load(std::memory_order_relaxed);
    std::fprintf(stderr,
        "[VOL-PROF] last %d frame(s) avg per-frame: cones=%.2fms halos=%.2fms "
        "unified=%.2fms sky=%.2fms (calls c=%d h=%d u=%d s=%d) "
        "cone-path: analytic=%d raymarch=%d\n",
        N,
        g_volProf.ms_cones   / N,
        g_volProf.ms_halos   / N,
        g_volProf.ms_unified / N,
        g_volProf.ms_skybox  / N,
        g_volProf.n_cones, g_volProf.n_halos, g_volProf.n_unified,
        g_volProf.n_skybox,
        cAnalytic, cRaymarch);
    std::fflush(stderr);
    g_volProf = VolProf{};
    g_coneAnalyticHits.store(0, std::memory_order_relaxed);
    g_coneRaymarchHits.store(0, std::memory_order_relaxed);
}

void VolProf_Tick() { VolProf_Tick_impl(); }

void Render_VolumetricCones() {
    VolProfScope _vp(&g_volProf.ms_cones, &g_volProf.n_cones);
    if (!CurScene || !ZPage16 || !VPage) return;
    if (!fds::FeatureFlags::draw_cones()) return;
    const float invFOVX = 1.0f / FOVX;
    const float invFOVY = 1.0f / FOVY;
    const float invZScale = 1.0f / float(g_zscale);
    // Density: per-step contribution coefficient. Tunable via existing
    // FDS_CONE_STRENGTH. Empirical: 0.0005-0.002 for City-scale (range
    // in thousands).
    const float density = fds::FeatureFlags::cone_strength() * 0.001f;

    // Fog cutoff + per-sample attenuation. Matches Render_DeferredFogPass:
    // cones fade by (1 - z/FZP) so they don't extend past where geometry
    // already fully fogged out. fogZ <= 0 disables (unfogged scenes).
    const float fogZ    = (CurScene->Flags & Scn_Fogged) ? CurScene->FZP : 0.0f;
    const float invFogZ = (fogZ > 0.0f) ? 1.0f / fogZ : 0.0f;

    // Iterate the frame-global ViewLightsSoA built by Render_DeferredLighting
    // (g_deferredCtx.lights / .numLights). The per-tile TileLights apply a
    // surface-z cull that's incorrect for volumetric integration — see the
    // note inside Render_VolumetricCones_Tile.
    extern DeferredLightingCtx g_deferredCtx;
    const ViewLightsSoA *const lights = g_deferredCtx.lights;
    if (!lights) return;
    const int numLights = g_deferredCtx.numLights;

    // Pre-filter spotlight indices once per frame; tiles share the result.
    // Mirror-clone spots are excluded (additive cone glow would wash
    // across the reflection — see Render_OmniHalos).
    static int spotIdx[DEFERRED_MAX_LIGHTS];
    int spotCount = 0;
    for (int i = 0; i < numLights; ++i) {
        if (lights->isSpot[i] && lights->mirrorId[i] == 0) spotIdx[spotCount++] = i;
    }
    if (spotCount == 0) return;

    constexpr int numTilesX = 6;
    constexpr int numTilesY = 4;
    constexpr int numTiles  = numTilesX * numTilesY;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;

    // Per-tile spot filtering. Mirror buildTileLightLists's screen-space
    // sphere projection but WITHOUT the z-cull (which caused the
    // tile-stripe artifact fixed in the prior commit). For sparse scenes
    // most tiles will see 0 spots — the per-pixel inner loop short-
    // circuits via spotCount==0.
    static int tileSpotIdx  [numTiles][DEFERRED_MAX_LIGHTS];
    static int tileSpotCount[numTiles];
    for (int t = 0; t < numTiles; ++t) tileSpotCount[t] = 0;

    for (int s = 0; s < spotCount; ++s) {
        const int li = spotIdx[s];
        const float vx = lights->posX[li];
        const float vy = lights->posY[li];
        const float vz = lights->posZ[li];
        const float r  = std::sqrt(lights->range2[li]);
        if (vz + r < 0.0f) continue;  // entirely behind camera

        int ti_lo, ti_hi, tj_lo, tj_hi;
        if (vz - r < 1.0f) {
            // Sphere straddles near plane: be conservative, tag every tile.
            ti_lo = 0; ti_hi = numTilesX - 1;
            tj_lo = 0; tj_hi = numTilesY - 1;
        } else {
            const float invZ = 1.0f / vz;
            const float cx = CntrEX + vx * FOVX * invZ;
            const float cy = CntrEY - vy * FOVY * invZ;
            const float rx = r * FOVX * invZ;
            const float ry = r * FOVY * invZ;
            const int sx_min = std::max(0,        int(std::floor(cx - rx)));
            const int sx_max = std::min(XRes - 1, int(std::ceil (cx + rx)));
            const int sy_min = std::max(0,        int(std::floor(cy - ry)));
            const int sy_max = std::min(YRes - 1, int(std::ceil (cy + ry)));
            if (sx_min > sx_max || sy_min > sy_max) continue;
            ti_lo = sx_min / tileSizeX;
            ti_hi = std::min(numTilesX - 1, sx_max / tileSizeX);
            tj_lo = sy_min / tileSizeY;
            tj_hi = std::min(numTilesY - 1, sy_max / tileSizeY);
        }
        for (int j = tj_lo; j <= tj_hi; ++j) {
            for (int i = ti_lo; i <= ti_hi; ++i) {
                const int t = j * numTilesX + i;
                if (tileSpotCount[t] < DEFERRED_MAX_LIGHTS) {
                    tileSpotIdx[t][tileSpotCount[t]++] = li;
                }
            }
        }
    }

    renderns::tileCounter = 0;
    for (int j = 0; j < numTilesY; ++j) {
        const int y1 = tileSizeY * j;
        const int y2 = std::min(y1 + tileSizeY, YRes);
        for (int i = 0; i < numTilesX; ++i) {
            const int x1 = tileSizeX * i;
            const int x2 = std::min(x1 + tileSizeX, XRes);
            const int tileIdx = j * numTilesX + i;
            const int *ts = tileSpotIdx[tileIdx];
            const int  tc = tileSpotCount[tileIdx];
            ThreadPool::instance().enqueue([x1,y1,x2,y2,lights,ts,tc,
                                            invFOVX,invFOVY,invZScale,density,
                                            fogZ,invFogZ]() {
                Render_VolumetricCones_Tile(x1,y1,x2,y2, lights, ts, tc,
                                             invFOVX,invFOVY,invZScale,density,
                                             fogZ,invFogZ);
                // One permit per completed tile (see renderns::tileDone).
                renderns::tileDone.release();
            });
        }
    }
    for (int _i = 0; _i < numTiles; ++_i) {
        renderns::tileDone.acquire();
    }
}

// ─── Omni halos — standalone additive pass for legacy mode ───────────
//
// Same idea as Render_VolumetricCones but for omnidirectional lights:
// ray-march each omni's range sphere, accumulate inverse-square
// in-scatter contribution per sample, composite additively. No fog
// integration (legacy mode keeps fog in Render_DeferredFogPass).
//
// Per pixel:
//   for each omni in tile:
//     [zLo, zHi] = ray ∩ sphere(omni.center, omni.range)
//     clamp by [zMin, zMax=zSurf]
//     ∫ samples: density × 1/(1+(d/R)²) × color
//     add to pixel
//
// Gated by FDS_OMNI_HALO_STRENGTH > 0. Replaces the omni-halo block
// that previously only existed inside the unified pass; called from
// the legacy dispatch (volumetric_unified=0) so City + other scenes
// that stay on legacy passes can still get omni halos.
static void Render_OmniHalos_Tile(
    int x1, int y1, int x2, int y2,
    const ViewLightsSoA *lights,
    const int *omniIdx, int omniCount,
    float invFOVX, float invFOVY,
    float invZScale,
    float fogZ, float invFogZ,
    float density)
{
    if (omniCount == 0) return;
    dword *out = reinterpret_cast<dword*>(VPage);
    const uint16_t *zEnc = ZPage16;
    const int N_SAMPLES = std::max(1, fds::FeatureFlags::vol_n_samples());
    const float inv_N = 1.0f / float(N_SAMPLES);
    const bool vecPath = fds::FeatureFlags::vol_vec();
    const bool analyticHalo = fds::FeatureFlags::vol_halo_analytic();
    const float noiseStrength = fds::FeatureFlags::vol_analytic_noise();

    // ─── Analytic halo path ────────────────────────────────────────────
    // For each pixel/omni, the in-sphere line integral of inverse-square
    // attenuation has a closed form: ∫1/(αz²+βz+γ)dz = (2/D)·arctan
    // ((2αz+β)/D) where D = sqrt(4αγ−β²). We drop the original (1-d/r)²
    // cutoff term (which would require integrating √(quadratic) and has
    // no elementary form) — visually this means a sharper boundary at
    // the omni's range edge instead of a soft fade. For a glow effect
    // it's acceptable; for accurate medium-density attenuation use the
    // ray-march path (--no-vol_halo_analytic).
    //
    // Cost per pixel/omni: ~10 fmuls + 1 atan + 1 sqrt + 1 div instead
    // of N×30 ops. Scalar atan is ~10ns on M-series; that's still <1ns
    // per useful pixel-pass after the per-pixel setup.
    if (analyticHalo && vecPath) {
        // ── Pixel-major SIMD analytic halo ──────────────────────────
        // Outer: per 8-pixel batch. Per-lane scalar sphere intersection
        // (has a sphereDisc<0 reject branch), then 8-wide vec analytic
        // integral via atan_approx_x8.
        for (int py = y1; py < y2; ++py) {
            const float Y = (CntrEY - float(py)) * invFOVY;
            const size_t row = size_t(py) * size_t(XRes);
            for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                const int pxEnd     = std::min(pxBase + 8, x2);
                const int laneCount = pxEnd - pxBase;

                alignas(32) float Xarr[8] = {}, uVarr[8] = {}, zMaxArr[8] = {};
                alignas(32) float noiseBuf[8] = {1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
                bool anyAlive = false;
                for (int lane = 0; lane < laneCount; ++lane) {
                    const int px = pxBase + lane;
                    const float X = (float(px) - CntrEX) * invFOVX;
                    Xarr[lane]  = X;
                    uVarr[lane] = X*X + Y*Y + 1.0f;
                    const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                    const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
                    float zM = (zSurf > 0.0f) ? zSurf : zSky;
                    if (fogZ > 0.0f && zM > fogZ) zM = fogZ;
                    constexpr float zMin = 0.05f;
                    if (zM > zMin) { zMaxArr[lane] = zM; anyAlive = true; }
                    if (noiseStrength > 0.0f) {
                        // Same avalanching hash the ray-march path uses
                        // (PCG-style xor-shift + multiply) so analytic +
                        // ray-march visually agree if you toggle between.
                        uint32_t h = uint32_t(px) * 0x9E3779B9u
                                   + uint32_t(py) * 0x85EBCA6Bu
                                   + 0xCAFEBABEu;
                        h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
                        const float u = float(h >> 16) * (1.0f/65536.0f);
                        noiseBuf[lane] = 1.0f + noiseStrength * (u - 0.5f);
                    }
                }
                if (!anyAlive) continue;
                const __m256 vNoise = _mm256_load_ps(noiseBuf);

                alignas(32) float accB[8] = {}, accG[8] = {}, accR[8] = {};

                for (int o = 0; o < omniCount; ++o) {
                    const int li = omniIdx[o];
                    const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
                    // Halo uses per-omni halo*[] (decoupled from surface
                    // range/rRange). HaloRange=0 in the Omni struct
                    // falls back to IRange — handled at SoA build time.
                    const float r2 = lights->haloRange2[li];
                    const float rr = lights->haloRRange[li];
                    const float perOmniDensity = density * lights->haloDensityMul[li];
                    const float PP = Px*Px + Py_l*Py_l + Pz*Pz;
                    const float rr2 = rr * rr;

                    // Per-lane scalar sphere bounds → zLoArr, zHiArr.
                    alignas(32) float zLoArr[8] = {}, zHiArr[8] = {};
                    alignas(32) float aliveLane[8] = {};
                    bool omniAlive = false;
                    for (int lane = 0; lane < laneCount; ++lane) {
                        if (zMaxArr[lane] <= 0.0f) continue;
                        const float X = Xarr[lane];
                        const float uV = uVarr[lane];
                        const float zMax = zMaxArr[lane];
                        constexpr float zMin = 0.05f;
                        const float VP = X*Px + Y*Py_l + Pz;
                        const float sphereC    = PP - r2;
                        const float sphereDisc = VP*VP - uV * sphereC;
                        if (sphereDisc < 0.0f) continue;
                        const float sphereSq = std::sqrt(sphereDisc);
                        const float invUV = 1.0f / uV;
                        float zLo = (VP - sphereSq) * invUV;
                        float zHi = (VP + sphereSq) * invUV;
                        if (zLo < zMin) zLo = zMin;
                        if (zHi > zMax) zHi = zMax;
                        if (zHi <= zLo) continue;
                        zLoArr[lane] = zLo;
                        zHiArr[lane] = zHi;
                        aliveLane[lane] = 1.0f;
                        omniAlive = true;
                    }
                    if (!omniAlive) continue;

                    // 8-wide vec: alpha, beta, gamma → D, invD → arg → atan.
                    const __m256 vY        = _mm256_set1_ps(Y);
                    const __m256 vX_v      = _mm256_load_ps(Xarr);
                    const __m256 vUv       = _mm256_load_ps(uVarr);
                    const __m256 vZLo      = _mm256_load_ps(zLoArr);
                    const __m256 vZHi      = _mm256_load_ps(zHiArr);
                    const __m256 vAlive_v  = _mm256_load_ps(aliveLane);
                    const __m256 vPx       = _mm256_set1_ps(Px);
                    const __m256 vPy       = _mm256_set1_ps(Py_l);
                    const __m256 vPz       = _mm256_set1_ps(Pz);
                    const __m256 vRR2      = _mm256_set1_ps(rr2);
                    const __m256 vPP       = _mm256_set1_ps(PP);
                    const __m256 vZero     = _mm256_setzero_ps();
                    const __m256 vOne      = _mm256_set1_ps(1.0f);
                    const __m256 vNegTwo   = _mm256_set1_ps(-2.0f);
                    const __m256 vFour     = _mm256_set1_ps(4.0f);
                    const __m256 vPt05     = _mm256_set1_ps(0.05f);
                    const __m256 mAlive    = _mm256_cmp_ps(vAlive_v, vZero, _CMP_GT_OQ);

                    // VP = X·Px + Y·Py + Pz, per lane (X varies)
                    const __m256 vVP = _mm256_fmadd_ps(vX_v, vPx,
                                       _mm256_fmadd_ps(vY, vPy, vPz));

                    // α = rr²·uV, β = -2·rr²·VP, γ = rr²·PP + 0.05
                    const __m256 vAlpha = _mm256_mul_ps(vRR2, vUv);
                    const __m256 vBeta  = _mm256_mul_ps(_mm256_mul_ps(vNegTwo, vRR2), vVP);
                    const __m256 vGamma = _mm256_fmadd_ps(vRR2, vPP, vPt05);
                    // discQ = 4αγ − β²
                    const __m256 vDiscQ = _mm256_fmsub_ps(_mm256_mul_ps(vFour, vAlpha), vGamma,
                                                          _mm256_mul_ps(vBeta, vBeta));
                    // Mask out lanes where discQ ≤ 0 (defensive — should be
                    // positive since sphere intersects ray).
                    const __m256 mDisc = _mm256_cmp_ps(vDiscQ, vZero, _CMP_GT_OQ);
                    const __m256 vMask = _mm256_and_ps(mAlive, mDisc);
                    const __m256 vSafeDisc = _mm256_blendv_ps(vOne, vDiscQ, vMask);
                    // rsqrt collapses sqrt + reciprocal into one ~3-cycle
                    // op (12-bit precision; ample for halo brightness vs
                    // div_ps's 10-20 cycles).
                    const __m256 vInvD = _mm256_rsqrt_ps(vSafeDisc);

                    // argHi = (2α·zHi + β) · invD, similarly argLo
                    const __m256 vTwoA = _mm256_add_ps(vAlpha, vAlpha);
                    const __m256 vArgHi = _mm256_mul_ps(vInvD,
                                          _mm256_fmadd_ps(vTwoA, vZHi, vBeta));
                    const __m256 vArgLo = _mm256_mul_ps(vInvD,
                                          _mm256_fmadd_ps(vTwoA, vZLo, vBeta));

                    // integral = 2·invD · (atan(argHi) − atan(argLo))
                    const __m256 vAtanHi = atan_approx_x8(vArgHi);
                    const __m256 vAtanLo = atan_approx_x8(vArgLo);
                    const __m256 vIntegral = _mm256_mul_ps(
                        _mm256_add_ps(vInvD, vInvD),
                        _mm256_sub_ps(vAtanHi, vAtanLo));

                    // Near-edge softness: replicate ray-march's (1-rr·d)²
                    // fade at the midpoint so analytic halos don't show
                    // a hard sphere boundary. Compute W² at z=zMid, then
                    // dist = √W², softEdge = max(0,1-rr·dist)².
                    const __m256 vZMid = _mm256_mul_ps(
                        _mm256_add_ps(vZLo, vZHi), _mm256_set1_ps(0.5f));
                    const __m256 Wx_m = _mm256_sub_ps(_mm256_mul_ps(vZMid, vX_v), vPx);
                    const __m256 Wy_m = _mm256_sub_ps(_mm256_mul_ps(vZMid, vY),  vPy);
                    const __m256 Wz_m = _mm256_sub_ps(vZMid, vPz);
                    const __m256 W2_m = _mm256_fmadd_ps(Wx_m, Wx_m,
                                        _mm256_fmadd_ps(Wy_m, Wy_m,
                                         _mm256_mul_ps(Wz_m, Wz_m)));
                    const __m256 safeW2_m = _mm256_blendv_ps(vOne, W2_m, vMask);
                    const __m256 invLen_m = _mm256_rsqrt_ps(safeW2_m);
                    const __m256 dist_m   = _mm256_mul_ps(W2_m, invLen_m);
                    const __m256 vRR      = _mm256_set1_ps(rr);
                    __m256 softEdge_m     = _mm256_sub_ps(vOne,
                                            _mm256_mul_ps(vRR, dist_m));
                    softEdge_m = _mm256_max_ps(vZero, softEdge_m);
                    softEdge_m = _mm256_mul_ps(softEdge_m, softEdge_m);

                    // w = integral · perOmniDensity · N / interval · softEdge —
                    // rcp is fine for halo (visual effect, not precision-
                    // critical). perOmniDensity folds in HaloIntensity.
                    const __m256 vIntervalLen = _mm256_sub_ps(vZHi, vZLo);
                    const __m256 vDensityN    = _mm256_set1_ps(perOmniDensity * float(N_SAMPLES));
                    const __m256 vSafeLen     = _mm256_blendv_ps(vOne, vIntervalLen, vMask);
                    const __m256 vW = _mm256_mul_ps(
                        _mm256_mul_ps(
                            _mm256_mul_ps(vIntegral, vDensityN),
                            softEdge_m),
                        _mm256_rcp_ps(vSafeLen));
                    // Per-pixel noise (vNoise = 1.0 when noiseStrength=0).
                    const __m256 vWNoised = _mm256_mul_ps(vW, vNoise);
                    const __m256 vWMasked = _mm256_and_ps(vWNoised, vMask);

                    alignas(32) float wArr[8];
                    _mm256_store_ps(wArr, vWMasked);
                    const float colR = lights->colR[li];
                    const float colG = lights->colG[li];
                    const float colB = lights->colB[li];
                    for (int lane = 0; lane < 8; ++lane) {
                        const float w = wArr[lane];
                        if (w <= 0.0f) continue;
                        accR[lane] += w * colR;
                        accG[lane] += w * colG;
                        accB[lane] += w * colB;
                    }
                }

                // Bayer-4x4 dither pattern (in [-0.5, +0.5)) — breaks
                // the visible color-banding that the smooth analytic
                // integral otherwise quantizes into when int-truncating
                // small floating-point contributions to 8-bit channels.
                // 16 stable per-pixel offsets (cheap, deterministic, no
                // flicker). Same pattern reused below for the scalar
                // analytic path.
                static constexpr float kBayer4[16] = {
                    -0.46875f, +0.03125f, -0.34375f, +0.15625f,
                    +0.28125f, -0.21875f, +0.40625f, -0.09375f,
                    -0.28125f, +0.21875f, -0.40625f, +0.09375f,
                    +0.46875f, -0.03125f, +0.34375f, -0.15625f,
                };
                for (int lane = 0; lane < laneCount; ++lane) {
                    if (accR[lane] <= 0.0f && accG[lane] <= 0.0f && accB[lane] <= 0.0f) continue;
                    const int px = pxBase + lane;
                    const float d = kBayer4[(py & 3) * 4 + (px & 3)];
                    const size_t i = row + size_t(px);
                    const dword pix = out[i];
                    int newR = int((pix >> 16) & 0xFF) + int(accR[lane] + 0.5f + d);
                    int newG = int((pix >>  8) & 0xFF) + int(accG[lane] + 0.5f + d);
                    int newB = int( pix        & 0xFF) + int(accB[lane] + 0.5f + d);
                    if (newR > 255) newR = 255;
                    if (newG > 255) newG = 255;
                    if (newB > 255) newB = 255;
                    out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                           |  dword(newB)        | 0xFF000000u;
                }
            }
        }
        return;
    }

    if (analyticHalo) {
        // Scalar fallback (analytic + scalar atan_approx).
        for (int py = y1; py < y2; ++py) {
            const float Y = (CntrEY - float(py)) * invFOVY;
            const size_t row = size_t(py) * size_t(XRes);
            for (int px = x1; px < x2; ++px) {
                const float X = (float(px) - CntrEX) * invFOVX;
                const float uV = X*X + Y*Y + 1.0f;

                const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
                float zMax = (zSurf > 0.0f) ? zSurf : zSky;
                if (fogZ > 0.0f && zMax > fogZ) zMax = fogZ;
                constexpr float zMin = 0.05f;
                if (zMax <= zMin) continue;

                float accR = 0.0f, accG = 0.0f, accB = 0.0f;
                for (int o = 0; o < omniCount; ++o) {
                    const int li = omniIdx[o];
                    const float Px = lights->posX[li], Py = lights->posY[li], Pz = lights->posZ[li];
                    const float r2 = lights->haloRange2[li];
                    const float rr = lights->haloRRange[li];
                    const float perOmniDensity = density * lights->haloDensityMul[li];
                    const float VP = X*Px + Y*Py + Pz;
                    const float PP = Px*Px + Py*Py + Pz*Pz;

                    // Sphere bounds (same as ray-march path).
                    const float sphereC    = PP - r2;
                    const float sphereDisc = VP*VP - uV * sphereC;
                    if (sphereDisc < 0.0f) continue;
                    const float sphereSq = std::sqrt(sphereDisc);
                    const float invUV    = 1.0f / uV;
                    float zLo = (VP - sphereSq) * invUV;
                    float zHi = (VP + sphereSq) * invUV;
                    if (zLo < zMin) zLo = zMin;
                    if (zHi > zMax) zHi = zMax;
                    if (zHi <= zLo) continue;

                    // Quadratic d²(z) = (zV - P)·(zV - P) = uV·z² - 2·VP·z + PP.
                    // Inverse-square attenuation: 1/((rr·d)² + 0.05)
                    //   = 1/(rr²·d² + 0.05) = 1/(α·z² + β·z + γ)
                    // with α = rr²·uV, β = -2·rr²·VP, γ = rr²·PP + 0.05.
                    // Discriminant 4αγ − β² simplifies via 4·rr²·(rr²·uV·PP + 0.05·uV − rr²·VP²).
                    // Since the omni range sphere intersects the ray, the
                    // discriminant is positive (else sphereDisc<0 would
                    // have fired above).
                    const float rr2  = rr * rr;
                    const float alpha = rr2 * uV;
                    const float beta  = -2.0f * rr2 * VP;
                    const float gamma = rr2 * PP + 0.05f;
                    const float discQ = 4.0f * alpha * gamma - beta * beta;
                    if (discQ <= 0.0f) continue;
                    // fast_rsqrt computes 1/sqrt(discQ) directly via NEON
                    // frsqrte + 1 NR step (~5 cycles vs ~24 for the
                    // std::sqrt + std::div pair). Recover D from invD by
                    // mul-back, then both atans via the polynomial approx
                    // (~10 ops/call vs ~30 cycles per libm atan).
                    const float invD = fast_rsqrt(discQ);
                    const float D    = discQ * invD;
                    (void)D;  // kept for parity with the legacy comment; argHi/Lo only need invD
                    const float argHi = (2.0f * alpha * zHi + beta) * invD;
                    const float argLo = (2.0f * alpha * zLo + beta) * invD;
                    const float integral = 2.0f * invD * (atan_approx(argHi) - atan_approx(argLo));
                    if (integral <= 0.0f) continue;
                    // Tile fn density is already premultiplied by N_SAMPLES
                    // for the ray-march path's per-sample-sum semantics
                    // (acc ≈ N × mean_distAtten). For the analytic
                    // integral we get the integrated value directly, so
                    // scale by N_SAMPLES to keep the visual intensity
                    // comparable. perOmniDensity folds in HaloIntensity.
                    const float w = integral * perOmniDensity * float(N_SAMPLES);
                    accR += w * lights->colR[li];
                    accG += w * lights->colG[li];
                    accB += w * lights->colB[li];
                }
                if (accR <= 0.0f && accG <= 0.0f && accB <= 0.0f) continue;
                // Bayer-4x4 dither (same pattern as SIMD path above).
                static constexpr float kBayer4[16] = {
                    -0.46875f, +0.03125f, -0.34375f, +0.15625f,
                    +0.28125f, -0.21875f, +0.40625f, -0.09375f,
                    -0.28125f, +0.21875f, -0.40625f, +0.09375f,
                    +0.46875f, -0.03125f, +0.34375f, -0.15625f,
                };
                const float d = kBayer4[(py & 3) * 4 + (px & 3)];
                const size_t i = row + size_t(px);
                const dword pix = out[i];
                int newR = int((pix >> 16) & 0xFF) + int(accR + 0.5f + d);
                int newG = int((pix >>  8) & 0xFF) + int(accG + 0.5f + d);
                int newB = int( pix        & 0xFF) + int(accB + 0.5f + d);
                if (newR > 255) newR = 255;
                if (newG > 255) newG = 255;
                if (newB > 255) newB = 255;
                out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                       |  dword(newB)        | 0xFF000000u;
            }
        }
        return;
    }

    for (int py = y1; py < y2; ++py) {
        const float Y = (CntrEY - float(py)) * invFOVY;
        const size_t row = size_t(py) * size_t(XRes);
        if (vecPath) {
            // Pixel-major SIMD — see Render_VolumetricCones_Tile for
            // rationale. Halo is simpler: only sphere intersection
            // (no cone quadratic) and no shadow lookup.
            for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                const int pxEnd     = std::min(pxBase + 8, x2);
                const int laneCount = pxEnd - pxBase;

                alignas(32) float    Xarr[8] = {};
                alignas(32) float    uVarr[8] = {};
                alignas(32) uint32_t pxHashArr[8] = {};
                alignas(32) float    zMaxArr[8] = {};
                bool anyAlive = false;
                for (int lane = 0; lane < laneCount; ++lane) {
                    const int px = pxBase + lane;
                    const float X = (float(px) - CntrEX) * invFOVX;
                    Xarr[lane]  = X;
                    uVarr[lane] = X*X + Y*Y + 1.0f;
                    uint32_t h = uint32_t(px) * 0x9E3779B9u
                               + uint32_t(py) * 0x85EBCA6Bu
                               + 0xDEC0DE51u;
                    h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
                    pxHashArr[lane] = h;
                    const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                    const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
                    float zM = (zSurf > 0.0f) ? zSurf : zSky;
                    if (fogZ > 0.0f && zM > fogZ) zM = fogZ;
                    constexpr float zMin = 0.05f;
                    if (zM > zMin) { zMaxArr[lane] = zM; anyAlive = true; }
                }
                if (!anyAlive) continue;

                alignas(32) float accB[8] = {}, accG[8] = {}, accR[8] = {};

                for (int o = 0; o < omniCount; ++o) {
                    const int li = omniIdx[o];
                    const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
                    const float r2 = lights->range2[li];
                    const float rr = lights->rRange[li];
                    const float PP = Px*Px + Py_l*Py_l + Pz*Pz;

                    // Per-lane scalar sphere-bounds solve.
                    alignas(32) float zLoArr[8] = {};
                    alignas(32) float zHiArr[8] = {};
                    alignas(32) float aliveLane[8] = {};
                    bool omniAlive = false;
                    for (int lane = 0; lane < laneCount; ++lane) {
                        if (zMaxArr[lane] <= 0.0f) continue;
                        const float X = Xarr[lane];
                        const float uV = uVarr[lane];
                        const float zMax = zMaxArr[lane];
                        constexpr float zMin = 0.05f;
                        const float VP = X*Px + Y*Py_l + Pz;
                        const float sphereC    = PP - r2;
                        const float sphereDisc = VP*VP - uV * sphereC;
                        if (sphereDisc < 0.0f) continue;
                        const float sphereSq = std::sqrt(sphereDisc);
                        const float invUV    = 1.0f / uV;
                        float zLo = (VP - sphereSq) * invUV;
                        float zHi = (VP + sphereSq) * invUV;
                        if (zLo < zMin) zLo = zMin;
                        if (zHi > zMax) zHi = zMax;
                        if (zHi <= zLo) continue;
                        zLoArr[lane]    = zLo;
                        zHiArr[lane]    = zHi;
                        aliveLane[lane] = 1.0f;
                        omniAlive = true;
                    }
                    if (!omniAlive) continue;

                    alignas(32) float dzArr[8] = {};
                    for (int lane = 0; lane < 8; ++lane) {
                        if (aliveLane[lane] == 0.0f) continue;
                        dzArr[lane] = (zHiArr[lane] - zLoArr[lane]) * inv_N;
                    }

                    const __m256 vX_v       = _mm256_load_ps(Xarr);
                    const __m256 vY_v       = _mm256_set1_ps(Y);
                    const __m256 vZLo_v     = _mm256_load_ps(zLoArr);
                    const __m256 vDz_v      = _mm256_load_ps(dzArr);
                    const __m256 vAlive_v   = _mm256_load_ps(aliveLane);
                    const __m256 vPx_v      = _mm256_set1_ps(Px);
                    const __m256 vPy_v      = _mm256_set1_ps(Py_l);
                    const __m256 vPz_v      = _mm256_set1_ps(Pz);
                    const __m256 vR2_v      = _mm256_set1_ps(r2);
                    const __m256 vRR_v      = _mm256_set1_ps(rr);
                    const __m256 vInvFogZ_v = _mm256_set1_ps(invFogZ);
                    const __m256 vZero_v    = _mm256_setzero_ps();
                    const __m256 vOne_v     = _mm256_set1_ps(1.0f);
                    const __m256 vEps_v     = _mm256_set1_ps(1e-6f);
                    const __m256 vPt05_v    = _mm256_set1_ps(0.05f);
                    const __m256 mAlive     = _mm256_cmp_ps(vAlive_v, vZero_v, _CMP_GT_OQ);
                    __m256 accV = vZero_v;

                    for (int k = 0; k < N_SAMPLES; ++k) {
                        alignas(32) float fracBuf[8];
                        for (int lane = 0; lane < 8; ++lane) {
                            const uint32_t h = pxHashArr[lane]
                                + uint32_t(k) * 0x9E3779B9u
                                + uint32_t(o) * 0x517CC1B7u;
                            fracBuf[lane] = float(h >> 16) * (1.0f / 65536.0f);
                        }
                        const __m256 vFrac = _mm256_load_ps(fracBuf);

                        const __m256 vKf = _mm256_set1_ps(float(k));
                        const __m256 vZ  = _mm256_fmadd_ps(
                            _mm256_add_ps(vKf, vFrac), vDz_v, vZLo_v);

                        const __m256 Wx = _mm256_sub_ps(_mm256_mul_ps(vZ, vX_v), vPx_v);
                        const __m256 Wy = _mm256_sub_ps(_mm256_mul_ps(vZ, vY_v), vPy_v);
                        const __m256 Wz = _mm256_sub_ps(vZ, vPz_v);
                        const __m256 W2 = _mm256_fmadd_ps(Wx, Wx,
                                           _mm256_fmadd_ps(Wy, Wy,
                                            _mm256_mul_ps(Wz, Wz)));

                        __m256 mask = _mm256_and_ps(mAlive,
                            _mm256_cmp_ps(W2, vR2_v, _CMP_LE_OQ));
                        mask = _mm256_and_ps(mask, _mm256_cmp_ps(W2, vEps_v, _CMP_GT_OQ));

                        const __m256 safeW2 = _mm256_blendv_ps(vOne_v, W2, mask);
                        const __m256 invLen = _mm256_rsqrt_ps(safeW2);
                        const __m256 dist   = _mm256_mul_ps(W2, invLen);

                        const __m256 dr        = _mm256_mul_ps(dist, vRR_v);
                        const __m256 cutoff    = _mm256_sub_ps(vOne_v, dr);
                        const __m256 invSqDen  = _mm256_fmadd_ps(dr, dr, vPt05_v);
                        const __m256 invSq     = _mm256_rcp_ps(invSqDen);
                        const __m256 distAtten = _mm256_mul_ps(_mm256_mul_ps(cutoff, cutoff), invSq);

                        __m256 fogAtten = vOne_v;
                        if (invFogZ > 0.0f) {
                            fogAtten = _mm256_sub_ps(vOne_v, _mm256_mul_ps(vZ, vInvFogZ_v));
                            fogAtten = _mm256_max_ps(vZero_v, fogAtten);
                            fogAtten = _mm256_mul_ps(fogAtten, fogAtten);
                        }

                        __m256 contrib = _mm256_mul_ps(distAtten, fogAtten);
                        contrib = _mm256_and_ps(contrib, mask);
                        accV = _mm256_add_ps(accV, contrib);
                    }

                    alignas(32) float accArr[8];
                    _mm256_store_ps(accArr, accV);
                    const float colB = lights->colB[li];
                    const float colG = lights->colG[li];
                    const float colR = lights->colR[li];
                    for (int lane = 0; lane < 8; ++lane) {
                        if (accArr[lane] <= 0.0f) continue;
                        const float w = accArr[lane] * density;
                        accB[lane] += w * colB;
                        accG[lane] += w * colG;
                        accR[lane] += w * colR;
                    }
                }

                for (int lane = 0; lane < laneCount; ++lane) {
                    if (accR[lane] <= 0.0f && accG[lane] <= 0.0f && accB[lane] <= 0.0f) continue;
                    const int px = pxBase + lane;
                    const size_t i = row + size_t(px);
                    const dword pix = out[i];
                    int newR = int((pix >> 16) & 0xFF) + int(accR[lane]);
                    int newG = int((pix >>  8) & 0xFF) + int(accG[lane]);
                    int newB = int( pix        & 0xFF) + int(accB[lane]);
                    if (newR > 255) newR = 255;
                    if (newG > 255) newG = 255;
                    if (newB > 255) newB = 255;
                    out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                           |  dword(newB)        | 0xFF000000u;
                }
            }
        } else {
        for (int px = x1; px < x2; ++px) {
            const float X = (float(px) - CntrEX) * invFOVX;
            const float uV = X*X + Y*Y + 1.0f;

            uint32_t pxHash = uint32_t(px) * 0x9E3779B9u
                            + uint32_t(py) * 0x85EBCA6Bu
                            + 0xDEC0DE51u;  // different salt from cones
            pxHash ^= pxHash >> 13;
            pxHash *= 0xC2B2AE35u;
            pxHash ^= pxHash >> 16;

            const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
            const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
            float zMax = (zSurf > 0.0f) ? zSurf : zSky;
            if (fogZ > 0.0f && zMax > fogZ) zMax = fogZ;
            constexpr float zMin = 0.05f;
            if (zMax <= zMin) continue;

            float accR = 0.0f, accG = 0.0f, accB = 0.0f;
            for (int o = 0; o < omniCount; ++o) {
                const int li = omniIdx[o];
                const float Px = lights->posX[li], Py = lights->posY[li], Pz = lights->posZ[li];
                const float r2 = lights->range2[li];
                const float rr = lights->rRange[li];
                const float VP = X*Px + Y*Py + Pz;
                const float PP = Px*Px + Py*Py + Pz*Pz;

                // Ray-sphere intersection bounds the integration.
                const float sphereC = PP - r2;
                const float sphereDisc = VP*VP - uV * sphereC;
                if (sphereDisc < 0.0f) continue;
                const float sphereSq = std::sqrt(sphereDisc);
                const float invUV    = 1.0f / uV;
                float zLo = (VP - sphereSq) * invUV;
                float zHi = (VP + sphereSq) * invUV;
                if (zLo < zMin) zLo = zMin;
                if (zHi > zMax) zHi = zMax;
                if (zHi <= zLo) continue;

                const float dz = (zHi - zLo) * inv_N;
                float acc = 0.0f;
                for (int k = 0; k < N_SAMPLES; ++k) {
                    const uint32_t h = pxHash
                        + uint32_t(k) * 0x9E3779B9u
                        + uint32_t(o) * 0x517CC1B7u;
                    const float frac = float(h >> 16) * (1.0f / 65536.0f);
                    const float z = zLo + (float(k) + frac) * dz;
                    const float Wx = z*X - Px;
                    const float Wy = z*Y - Py;
                    const float Wz = z    - Pz;
                    const float W2 = Wx*Wx + Wy*Wy + Wz*Wz;
                    if (W2 > r2 || W2 < 1e-6f) continue;
                    const float invLen = fast_rsqrt(W2);
                    const float dist = W2 * invLen;
                    const float dr = dist * rr;
                    const float cutoff = 1.0f - dr;
                    const float invSq  = 1.0f / (dr * dr + 0.05f);
                    const float distAtten = cutoff * cutoff * invSq;
                    // Match the legacy fog pass's per-sample squared
                    // attenuation so halos fade consistently with
                    // surface fog in fogged scenes.
                    float fogAtten = 1.0f;
                    if (invFogZ > 0.0f) {
                        fogAtten = 1.0f - z * invFogZ;
                        if (fogAtten < 0.0f) fogAtten = 0.0f;
                        fogAtten *= fogAtten;
                    }
                    acc += distAtten * fogAtten;
                }
                if (acc <= 0.0f) continue;
                const float w = acc * density;
                accR += w * lights->colR[li];
                accG += w * lights->colG[li];
                accB += w * lights->colB[li];
            }
            if (accR <= 0.0f && accG <= 0.0f && accB <= 0.0f) continue;
            const size_t i = row + size_t(px);
            const dword pix = out[i];
            int newR = int((pix >> 16) & 0xFF) + int(accR);
            int newG = int((pix >>  8) & 0xFF) + int(accG);
            int newB = int( pix        & 0xFF) + int(accB);
            if (newR > 255) newR = 255;
            if (newG > 255) newG = 255;
            if (newB > 255) newB = 255;
            out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                   |  dword(newB)        | 0xFF000000u;
        }
        }
    }
}

void Render_OmniHalos() {
    VolProfScope _vp(&g_volProf.ms_halos, &g_volProf.n_halos);
    if (!CurScene || !ZPage16 || !VPage) return;
    if (fds::FeatureFlags::omni_halo_strength() <= 0.0f) return;
    const float invFOVX = 1.0f / FOVX;
    const float invFOVY = 1.0f / FOVY;
    const float invZScale = 1.0f / float(g_zscale);
    const float density = fds::FeatureFlags::omni_halo_strength() * 0.001f;
    const float fogZ    = (CurScene->Flags & Scn_Fogged) ? CurScene->FZP : 0.0f;
    const float invFogZ = (fogZ > 0.0f) ? 1.0f / fogZ : 0.0f;

    extern DeferredLightingCtx g_deferredCtx;
    const ViewLightsSoA *const lights = g_deferredCtx.lights;
    if (!lights) return;
    const int numLights = g_deferredCtx.numLights;

    static int omniIdx[DEFERRED_MAX_LIGHTS];
    int omniCount = 0;
    for (int i = 0; i < numLights; ++i) {
        // Skip mirror-clone omnis. Their halo is an ADDITIVE screen-space
        // glow with no per-pixel mirror gating, so 15 cloned warm greets
        // omnis bloom a flat yellow wash over the reflection (and over
        // real geometry). A mirror should show reflected glows only
        // inside its footprint — until the halo kernel gains a per-pixel
        // gb.mirrorId gate, the clean fix is to not glow clones at all.
        if (lights->mirrorId[i] != 0) continue;
        if (!lights->isSpot[i]) omniIdx[omniCount++] = i;
    }
    if (omniCount == 0) return;

    constexpr int numTilesX = 6;
    constexpr int numTilesY = 4;
    constexpr int numTiles  = numTilesX * numTilesY;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;

    // Per-tile omni cull — same sphere-projection math as cones, but
    // no z-cull (omni halo is volumetric, surface-z occlusion is per-
    // pixel via zSurf clamp inside the kernel).
    static int tileOmniIdx  [numTiles][DEFERRED_MAX_LIGHTS];
    static int tileOmniCount[numTiles];
    for (int t = 0; t < numTiles; ++t) tileOmniCount[t] = 0;

    for (int o = 0; o < omniCount; ++o) {
        const int li = omniIdx[o];
        const float vx = lights->posX[li];
        const float vy = lights->posY[li];
        const float vz = lights->posZ[li];
        // Cull against the *halo* radius, not the surface IRange.
        // omni_halo_force_range / range_mult / per-omni HaloRange can all
        // make the halo extend well past the surface-lit sphere — using
        // range2 here was rejecting tiles where the halo should render,
        // and was also responsible for sharp tile-edge transitions when
        // adjacent tiles disagreed on whether the (small) surface sphere
        // crossed them.
        const float r  = lights->haloRange[li];
        if (vz + r < 0.0f) continue;
        int ti_lo, ti_hi, tj_lo, tj_hi;
        if (vz - r < 1.0f) {
            ti_lo = 0; ti_hi = numTilesX - 1;
            tj_lo = 0; tj_hi = numTilesY - 1;
        } else {
            const float invZ = 1.0f / vz;
            const float cx = CntrEX + vx * FOVX * invZ;
            const float cy = CntrEY - vy * FOVY * invZ;
            const float rx = r * FOVX * invZ;
            const float ry = r * FOVY * invZ;
            const int sx_min = std::max(0,        int(std::floor(cx - rx)));
            const int sx_max = std::min(XRes - 1, int(std::ceil (cx + rx)));
            const int sy_min = std::max(0,        int(std::floor(cy - ry)));
            const int sy_max = std::min(YRes - 1, int(std::ceil (cy + ry)));
            if (sx_min > sx_max || sy_min > sy_max) continue;
            ti_lo = sx_min / tileSizeX;
            ti_hi = std::min(numTilesX - 1, sx_max / tileSizeX);
            tj_lo = sy_min / tileSizeY;
            tj_hi = std::min(numTilesY - 1, sy_max / tileSizeY);
        }
        for (int j = tj_lo; j <= tj_hi; ++j) {
            for (int i = ti_lo; i <= ti_hi; ++i) {
                const int t = j * numTilesX + i;
                if (tileOmniCount[t] < DEFERRED_MAX_LIGHTS) {
                    tileOmniIdx[t][tileOmniCount[t]++] = li;
                }
            }
        }
    }

    renderns::tileCounter = 0;
    for (int j = 0; j < numTilesY; ++j) {
        const int y1 = tileSizeY * j;
        const int y2 = std::min(y1 + tileSizeY, YRes);
        for (int i = 0; i < numTilesX; ++i) {
            const int x1 = tileSizeX * i;
            const int x2 = std::min(x1 + tileSizeX, XRes);
            const int tileIdx = j * numTilesX + i;
            const int *ts = tileOmniIdx[tileIdx];
            const int  tc = tileOmniCount[tileIdx];
            ThreadPool::instance().enqueue([x1,y1,x2,y2,lights,ts,tc,
                                            invFOVX,invFOVY,invZScale,
                                            fogZ,invFogZ,density]() {
                Render_OmniHalos_Tile(x1,y1,x2,y2, lights, ts, tc,
                                       invFOVX,invFOVY,invZScale,
                                       fogZ,invFogZ,density);
                // One permit per completed tile (see renderns::tileDone).
                renderns::tileDone.release();
            });
        }
    }
    for (int _i = 0; _i < numTiles; ++_i) {
        renderns::tileDone.acquire();
    }
}

// Skybox-from-G-buffer pass. Paints sky pixels (zEnc == 0) by
// reconstructing the world-space view direction per pixel and
// sampling the cubemap. Pre-req: the forward RenderSkyCube must be
// suppressed when this pass runs (see SkyCube.cpp early-return),
// otherwise sky pixels would have zEnc != 0 from the forward draw.
//
// Cube face indexing matches SkyCube.cpp's normal convention:
//   0 = +Z (SBBK / "back")
//   1 = +X (SBRT / "right")
//   2 = -Z (SBFT / "front")
//   3 = -X (SBLF / "left")
//   4 = -Y (SBDN / "down")    [Y is up; -Y face is the floor]
//   5 = +Y (SBUP / "up")
// Per-face UV math derived from vertex/UV layout in InitSkyCube — see
// the (face, vertex, UV) table next to that function if extending.
// Deferred sky elapsed-ns accumulator. Always-on (cheap atomic add),
// distinct from the vol_prof flag-gated VolProfScope. Per-scene
// drivers consume this in their PROF_SKY section so the on-screen
// overlay reflects the deferred path the same way it used to
// reflect RenderSkyCube.
namespace {
    std::atomic<std::int64_t> g_deferredSkyNs{0};
}
std::int64_t DeferredSkybox_TakeFrameNs() {
    return g_deferredSkyNs.exchange(0, std::memory_order_relaxed);
}

void Render_DeferredSkybox() {
    VolProfScope _vp(&g_volProf.ms_skybox, &g_volProf.n_skybox);
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    struct AccumOnExit {
        clk::time_point t0;
        ~AccumOnExit() {
            const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                clk::now() - t0).count();
            g_deferredSkyNs.fetch_add(dt, std::memory_order_relaxed);
        }
    } _accum{t0};
    if (!CurScene || !VPage || !ZPage16) return;
    extern const dword *SkyCube_GetFaceMip(int, int, int&, int&);
    extern int SkyCube_NumMips();
    const int numMips = SkyCube_NumMips();
    if (numMips <= 0) return;

    // Cache every mip of every face — bounded by 2048² → 12 levels.
    // Per-pixel sampling does 6×kMaxMips=72 entries lookup-free.
    constexpr int kMaxMips = 12;
    const int mipCount = std::min(numMips, kMaxMips);
    const dword *facePix[6][kMaxMips] = {};
    int faceW [6][kMaxMips] = {};
    int faceH [6][kMaxMips] = {};
    for (int f = 0; f < 6; ++f) {
        for (int m = 0; m < mipCount; ++m) {
            facePix[f][m] = SkyCube_GetFaceMip(f, m, faceW[f][m], faceH[f][m]);
            if (!facePix[f][m]) return;
        }
    }

    // View basis. View->Mat is world→view (orthonormal), so view→world
    // for a *direction* is the transpose. Cache the 9 floats so the
    // per-pixel multiply is just nine flops + adds.
    const float m00 = View->Mat[0][0], m01 = View->Mat[0][1], m02 = View->Mat[0][2];
    const float m10 = View->Mat[1][0], m11 = View->Mat[1][1], m12 = View->Mat[1][2];
    const float m20 = View->Mat[2][0], m21 = View->Mat[2][1], m22 = View->Mat[2][2];
    const float invFOVX_l = 1.0f / FOVX;
    const float invFOVY_l = 1.0f / FOVY;
    const float cntrX_l = CntrEX;
    const float cntrY_l = CntrEY;
    // Per-pixel angular size in view space. Used to estimate the
    // cubemap UV change between adjacent pixels for mip selection.
    const float pixelAngle = (invFOVX_l > invFOVY_l) ? invFOVX_l : invFOVY_l;

    constexpr int numTilesX = 6;
    constexpr int numTilesY = 4;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;
    renderns::tileCounter = 0;
    for (int j = 0; j < numTilesY; ++j) {
        const int y1 = tileSizeY * j;
        const int y2 = std::min(y1 + tileSizeY, YRes);
        for (int i = 0; i < numTilesX; ++i) {
            const int x1 = tileSizeX * i;
            const int x2 = std::min(x1 + tileSizeX, XRes);
            ThreadPool::instance().enqueue([=]() {
                dword *out = reinterpret_cast<dword *>(VPage);
                // Hoisted broadcasts for the 8-wide view→world FMAs.
                const __m256 vm00 = _mm256_set1_ps(m00);
                const __m256 vm10 = _mm256_set1_ps(m10);
                const __m256 vm20 = _mm256_set1_ps(m20);
                const __m256 vm01 = _mm256_set1_ps(m01);
                const __m256 vm11 = _mm256_set1_ps(m11);
                const __m256 vm21 = _mm256_set1_ps(m21);
                const __m256 vm02 = _mm256_set1_ps(m02);
                const __m256 vm12 = _mm256_set1_ps(m12);
                const __m256 vm22 = _mm256_set1_ps(m22);
                const __m256 vCntrX = _mm256_set1_ps(cntrX_l);
                const __m256 vInvFOVX = _mm256_set1_ps(invFOVX_l);
                // 0..7 lane offset for vx generation.
                const __m256 vLaneOfs = _mm256_setr_ps(0,1,2,3,4,5,6,7);
                for (int py = y1; py < y2; ++py) {
                    const float vy = -((float(py) - cntrY_l) * invFOVY_l);
                    const __m256 vvy = _mm256_set1_ps(vy);
                    // m1*vy + m2*1 = fmadd(m1, vy, m2). Hoisted per row.
                    const __m256 dxY = _mm256_fmadd_ps(vm10, vvy, vm20);
                    const __m256 dyY = _mm256_fmadd_ps(vm11, vvy, vm21);
                    const __m256 dzY = _mm256_fmadd_ps(vm12, vvy, vm22);
                    const size_t row = size_t(py) * size_t(XRes);
                    for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                        const int pxEnd = std::min(pxBase + 8, x2);
                        const int laneCount = pxEnd - pxBase;
                        // Early-skip batches with no sky pixels — most
                        // tiles in opaque-heavy scenes look like this.
                        // Avoid touching the per-pixel math at all.
                        bool anySky = false;
                        alignas(32) uint32_t skyMask[8] = {};
                        for (int l = 0; l < laneCount; ++l) {
                            if (ZPage16[row + pxBase + l] == 0) {
                                skyMask[l] = 0xFFFFFFFFu; anySky = true;
                            }
                        }
                        if (!anySky) continue;
                        // vx = (pxBase + lane - cntrX) * invFOVX
                        const __m256 vxBase = _mm256_set1_ps(float(pxBase));
                        const __m256 vPx = _mm256_add_ps(vxBase, vLaneOfs);
                        const __m256 vvx = _mm256_mul_ps(
                            _mm256_sub_ps(vPx, vCntrX), vInvFOVX);
                        // D = M^T · v  (vz = 1 folded into dxY/dyY/dzY).
                        const __m256 dxv = _mm256_fmadd_ps(vm00, vvx, dxY);
                        const __m256 dyv = _mm256_fmadd_ps(vm01, vvx, dyY);
                        const __m256 dzv = _mm256_fmadd_ps(vm02, vvx, dzY);
                        alignas(32) float dxArr[8], dyArr[8], dzArr[8];
                        _mm256_store_ps(dxArr, dxv);
                        _mm256_store_ps(dyArr, dyv);
                        _mm256_store_ps(dzArr, dzv);
                        // Per-lane scalar tail: face select + sample.
                        // (Different lanes pick different faces and
                        // textures — vectorizing the sampler would need
                        // gather/scatter; not worth the complexity.)
                        for (int l = 0; l < laneCount; ++l) {
                            if (skyMask[l] == 0) continue;
                            const int px = pxBase + l;
                            const float dx = dxArr[l];
                            const float dy = dyArr[l];
                            const float dz = dzArr[l];
                        const float ax = dx < 0 ? -dx : dx;
                        const float ay = dy < 0 ? -dy : dy;
                        const float az = dz < 0 ? -dz : dz;
                        int face;
                        float u, v, maxAbs;
                        if (az >= ax && az >= ay) {
                            maxAbs = az;
                            const float s = 0.5f / az;
                            if (dz > 0) { face = 0; u = 0.5f - dx*s; v = 0.5f - dy*s; }
                            else        { face = 2; u = 0.5f + dx*s; v = 0.5f - dy*s; }
                        } else if (ax >= ay) {
                            maxAbs = ax;
                            const float s = 0.5f / ax;
                            if (dx > 0) { face = 1; u = 0.5f + dz*s; v = 0.5f - dy*s; }
                            else        { face = 3; u = 0.5f - dz*s; v = 0.5f - dy*s; }
                        } else {
                            maxAbs = ay;
                            const float s = 0.5f / ay;
                            if (dy > 0) { face = 5; u = 0.5f + dx*s; v = 0.5f - dz*s; }
                            else        { face = 4; u = 0.5f + dx*s; v = 0.5f + dz*s; }
                        }
                        // Mip selection. texelStep ≈ pixelAngle·0.5/|dom|·faceW.
                        // Integer log2 via the IEEE-754 exponent — no
                        // call into libm. `(bits>>23)&0xFF` is the
                        // biased exponent; subtract 127 → unbiased.
                        // (negative result = texelStep<1 → mip 0.)
                        const float invMaxAbs = 1.0f / maxAbs;
                        const float texelStep = pixelAngle * invMaxAbs * 0.5f
                                              * float(faceW[face][0]);
                        int mip;
                        if (texelStep < 1.0f) {
                            mip = 0;
                        } else {
                            union { float f; uint32_t i; } u{texelStep};
                            mip = int((u.i >> 23) & 0xFFu) - 127;
                            if (mip < 0) mip = 0;
                            if (mip >= mipCount) mip = mipCount - 1;
                        }
                        const int w = faceW[face][mip];
                        const int h = faceH[face][mip];
                        const dword *tex = facePix[face][mip];

                        // Bilinear sample. fu/fv in [0,1); clamp at edges
                        // so we don't sample across face seams (which
                        // would show up as cube-edge bands).
                        float fx = u * float(w) - 0.5f;
                        float fy = v * float(h) - 0.5f;
                        if (fx < 0) fx = 0; else if (fx > float(w - 1)) fx = float(w - 1);
                        if (fy < 0) fy = 0; else if (fy > float(h - 1)) fy = float(h - 1);
                        const int tx0 = int(fx);
                        const int ty0 = int(fy);
                        const int tx1 = (tx0 + 1 < w) ? tx0 + 1 : tx0;
                        const int ty1 = (ty0 + 1 < h) ? ty0 + 1 : ty0;
                        const float fxr = fx - float(tx0);
                        const float fyr = fy - float(ty0);
                        const dword p00 = tex[size_t(ty0) * size_t(w) + size_t(tx0)];
                        const dword p10 = tex[size_t(ty0) * size_t(w) + size_t(tx1)];
                        const dword p01 = tex[size_t(ty1) * size_t(w) + size_t(tx0)];
                        const dword p11 = tex[size_t(ty1) * size_t(w) + size_t(tx1)];
                        const float w00 = (1.f - fxr) * (1.f - fyr);
                        const float w10 = fxr        * (1.f - fyr);
                        const float w01 = (1.f - fxr) * fyr;
                        const float w11 = fxr        * fyr;
                        // Per-channel blend on ARGB8888.
                        const float bF = float((p00      ) & 0xFFu) * w00
                                       + float((p10      ) & 0xFFu) * w10
                                       + float((p01      ) & 0xFFu) * w01
                                       + float((p11      ) & 0xFFu) * w11;
                        const float gF = float((p00 >>  8) & 0xFFu) * w00
                                       + float((p10 >>  8) & 0xFFu) * w10
                                       + float((p01 >>  8) & 0xFFu) * w01
                                       + float((p11 >>  8) & 0xFFu) * w11;
                        const float rF = float((p00 >> 16) & 0xFFu) * w00
                                       + float((p10 >> 16) & 0xFFu) * w10
                                       + float((p01 >> 16) & 0xFFu) * w01
                                       + float((p11 >> 16) & 0xFFu) * w11;
                        const dword B = dword(bF) & 0xFFu;
                        const dword G = dword(gF) & 0xFFu;
                        const dword R = dword(rF) & 0xFFu;
                        out[row + px] = 0xFF000000u | (R << 16) | (G << 8) | B;
                        }  // per-lane for-l
                    }      // per-batch for-pxBase
                }          // per-row for-py
                // One permit per completed tile (see renderns::tileDone).
                renderns::tileDone.release();
            });
        }
    }
    for (int _i = 0, n = numTilesX * numTilesY; _i < n; ++_i) {
        renderns::tileDone.acquire();
    }
}

void Render_DeferredFogPass() {
	if (!CurScene || !(CurScene->Flags & Scn_Fogged)) return;
	if (!g_gbuffer || !ZPage16 || !VPage) return;
	const float invFZP = 1.0f / CurScene->FZP;
	constexpr auto numTilesX = 6;
	constexpr auto numTilesY = 4;
	const auto tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const auto tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;
	renderns::tileCounter = 0;
	for (int j = 0; j < numTilesY; ++j) {
		const int y1 = tileSizeY * j;
		const int y2 = std::min(y1 + tileSizeY, YRes);
		for (int i = 0; i < numTilesX; ++i) {
			const int x1 = tileSizeX * i;
			const int x2 = std::min(x1 + tileSizeX, XRes);
			ThreadPool::instance().enqueue([x1, y1, x2, y2, invFZP]() {
				Render_DeferredFogPass_Tile(x1, y1, x2, y2, invFZP);
				// One permit per completed tile (see renderns::tileDone).
				renderns::tileDone.release();
			});
		}
	}
	for (int _i = 0, n = numTilesX * numTilesY; _i < n; ++_i) {
		renderns::tileDone.acquire();
	}
}

// ─── Wrappers for the renderFrame orchestrator ───────────────────────────
// renderFrame in RENDER.CPP dispatches transparent-layer composites in a
// tile-job lambda; the template + g_deferredCtx live here, so we expose
// front/back wrappers it can forward into without seeing the template.

void renderDeferredTransparentTile_Front(int tileIdx, int x1, int y1, int x2, int y2) {
	Render_DeferredTransparentLighting_Tile<XparLayer::Front>(
		g_deferredCtx, tileIdx, x1, y1, x2, y2);
	// Release the renderns::tileDone permit on behalf of the inner
	// template — see the comment in that template's body for why the
	// release lives here instead of inside.
	renderns::tileDone.release();
}
void renderDeferredTransparentTile_Back(int tileIdx, int x1, int y1, int x2, int y2) {
	Render_DeferredTransparentLighting_Tile<XparLayer::Back>(
		g_deferredCtx, tileIdx, x1, y1, x2, y2);
	renderns::tileDone.release();
}

// ─── Unified Beer-Lambert volumetric pass ────────────────────────────
//
// Replaces Render_DeferredFogPass + Render_VolumetricCones with one
// physically motivated pass. Per pixel:
//   out = surface × T_fog + fog_emit + light_emit
//
// where T_fog and fog_emit come from an ANALYTIC Beer-Lambert
// formulation (uniform fog σ → closed form, no ray-march needed),
// and light_emit is a ray-marched sum of cone scatter + omni halo
// contributions weighted by the same Beer-Lambert transmittance.
//
// Scene's FZP controls the "far plane" feel via σ = mult/FZP, where
// mult is fog_sigma_mult (default 3). T(z) = exp(-σ·z), so at z=FZP
// we get T = exp(-3) ≈ 0.05 (95% fogged at the far plane).
//
// Sky pixels (zSurf=0, treated as ∞ in clear scenes / zMax=fogFar in
// fogged scenes) get only ambient fog + light scatter; no surface
// contribution.

static void Render_DeferredVolumetric_Tile(
    int x1, int y1, int x2, int y2,
    const ViewLightsSoA *lights,
    const int *spotIdx, int spotCount,
    const int *omniIdx, int omniCount,
    float invFOVX, float invFOVY,
    float invZScale,
    float sigma, float fogFar,
    float fogR, float fogG, float fogB,
    float coneDensity, float omniHaloDensity)
{
    dword *out = reinterpret_cast<dword*>(VPage);
    const uint16_t *zEnc = ZPage16;
    const int N_SAMPLES = std::max(1, fds::FeatureFlags::vol_n_samples());
    const float inv_N = 1.0f / float(N_SAMPLES);
    const bool vecPath = fds::FeatureFlags::vol_vec();

    const bool hasFog  = (sigma > 0.0f);
    // Beer-Lambert transmittance uses exp(-σ·z). LUT-based fastPow2
    // is ~5× faster than std::exp on arm64; per-sample math runs
    // hot enough that this matters. Precompute -σ·log2(e) so the
    // per-sample lookup is one mul + one fastPow2.
    //   exp(-σ·z) = pow2(-σ·z · log2(e))
    constexpr float kLog2e = 1.4426950408889634f;
    const float fogPowK = -sigma * kLog2e;
    const bool hasCone = (spotCount > 0 && coneDensity > 0.0f);
    const bool hasHalo = (omniCount > 0 && omniHaloDensity > 0.0f);

    for (int py = y1; py < y2; ++py) {
        const float Y = (CntrEY - float(py)) * invFOVY;
        const size_t row = size_t(py) * size_t(XRes);
        if (vecPath) {
            // Pixel-major SIMD — see Render_VolumetricCones_Tile for
            // rationale. Unified pass adds analytic Beer-Lambert fog
            // composite. fastPow2 stays scalar per-lane (no SIMD
            // implementation; called once per sample per lane in the
            // hot path).
            for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                const int pxEnd     = std::min(pxBase + 8, x2);
                const int laneCount = pxEnd - pxBase;

                alignas(32) float    Xarr[8] = {};
                alignas(32) float    uVarr[8] = {};
                alignas(32) uint32_t pxHashArr[8] = {};
                alignas(32) float    zMaxArr[8] = {};
                // T_surf defaults to 1.0 (no fog → surface unattenuated);
                // fog_emit defaults to 0. Must initialize before the
                // conditional `if (hasFog)` writes below.
                alignas(32) float    TSurfArr[8] = {1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
                alignas(32) float    fogEmitR_arr[8] = {};
                alignas(32) float    fogEmitG_arr[8] = {};
                alignas(32) float    fogEmitB_arr[8] = {};
                bool anyAlive = false;
                for (int lane = 0; lane < laneCount; ++lane) {
                    const int px = pxBase + lane;
                    const float X = (float(px) - CntrEX) * invFOVX;
                    Xarr[lane]  = X;
                    uVarr[lane] = X*X + Y*Y + 1.0f;
                    uint32_t h = uint32_t(px) * 0x9E3779B9u
                               + uint32_t(py) * 0x85EBCA6Bu
                               + 0xCAFEBABEu;
                    h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
                    pxHashArr[lane] = h;
                    const float zSurfRaw = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                    const bool isSky = (zSurfRaw <= 0.0f);
                    const float zM = isSky
                        ? (hasFog ? fogFar : 1e30f)
                        : (hasFog ? std::min(zSurfRaw, fogFar) : zSurfRaw);
                    constexpr float zMin = 0.05f;
                    if (zM > zMin) {
                        zMaxArr[lane] = zM;
                        anyAlive = true;
                        if (hasFog) {
                            const float TS = fastPow2(fogPowK * zM);
                            const float fogFrac = 1.0f - TS;
                            fogEmitR_arr[lane] = fogR * fogFrac;
                            fogEmitG_arr[lane] = fogG * fogFrac;
                            fogEmitB_arr[lane] = fogB * fogFrac;
                            TSurfArr[lane] = TS;
                        }
                    }
                }
                if (!anyAlive) continue;

                alignas(32) float lightR[8] = {}, lightG[8] = {}, lightB[8] = {};

                if (hasCone) {
                    for (int s = 0; s < spotCount; ++s) {
                        const int li = spotIdx[s];
                        const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
                        const float Dx = lights->dirX[li], Dy = lights->dirY[li], Dz = lights->dirZ[li];
                        const float cosO = lights->cosOuter[li];
                        const float cosI = lights->cosInner[li];
                        const float r2   = lights->range2[li];
                        const float rr   = lights->rRange[li];
                        const float DP   = Dx*Px + Dy*Py_l + Dz*Pz;
                        const float PP   = Px*Px + Py_l*Py_l + Pz*Pz;
                        const float c2   = cosO * cosO;
                        const float inv_cosI_minus_cosO = 1.0f / (cosI - cosO);

                        alignas(32) float zLoArr[8] = {};
                        alignas(32) float zHiArr[8] = {};
                        alignas(32) float aliveLane[8] = {};
                        bool spotAlive = false;
                        for (int lane = 0; lane < laneCount; ++lane) {
                            if (zMaxArr[lane] <= 0.0f) continue;
                            const float X = Xarr[lane];
                            const float uV = uVarr[lane];
                            const float zMax = zMaxArr[lane];
                            constexpr float zMin = 0.05f;
                            const float DV = Dx*X + Dy*Y + Dz;
                            const float VP = X*Px + Y*Py_l + Pz;
                            const float a  = DV*DV - c2 * uV;
                            const float b  = 2.0f * (c2 * VP - DV * DP);
                            const float cq = DP*DP - c2 * PP;
                            const float sphereC    = PP - r2;
                            const float sphereDisc = VP*VP - uV * sphereC;
                            if (sphereDisc < 0.0f) continue;
                            const float sphereSq = std::sqrt(sphereDisc);
                            const float invUV    = 1.0f / uV;
                            const float zSphLo   = (VP - sphereSq) * invUV;
                            const float zSphHi   = (VP + sphereSq) * invUV;
                            float zLo, zHi;
                            if (a < -1e-8f) {
                                const float disc = b*b - 4.0f*a*cq;
                                if (disc < 0.0f) continue;
                                const float sq = std::sqrt(disc);
                                const float inv2a = 1.0f / (2.0f * a);
                                const float r1 = (-b - sq) * inv2a;
                                const float r2_ = (-b + sq) * inv2a;
                                zLo = std::min(r1, r2_);
                                zHi = std::max(r1, r2_);
                            } else if (a > 1e-8f) {
                                const float disc = b*b - 4.0f*a*cq;
                                if (disc < 0.0f) {
                                    zLo = zMin;
                                    zHi = zMax;
                                } else {
                                    const float sq = std::sqrt(disc);
                                    const float inv2a = 1.0f / (2.0f * a);
                                    const float root1 = (-b - sq) * inv2a;
                                    const float root2 = (-b + sq) * inv2a;
                                    const float r1Q = std::min(root1, root2);
                                    const float r2Q = std::max(root1, root2);
                                    if (DV > 1e-6f) {
                                        zLo = std::max(r2Q, zMin);
                                        zHi = zMax;
                                    } else if (DV < -1e-6f) {
                                        zLo = zMin;
                                        zHi = std::min(r1Q, zMax);
                                    } else continue;
                                    if (zHi <= zLo) continue;
                                }
                            } else continue;
                            if (zLo < zSphLo) zLo = zSphLo;
                            if (zHi > zSphHi) zHi = zSphHi;
                            if (zLo < zMin)   zLo = zMin;
                            if (zHi > zMax)   zHi = zMax;
                            if (zHi <= zLo)   continue;
                            zLoArr[lane]    = zLo;
                            zHiArr[lane]    = zHi;
                            aliveLane[lane] = 1.0f;
                            spotAlive = true;
                        }
                        if (!spotAlive) continue;

                        alignas(32) float dzArr[8] = {};
                        for (int lane = 0; lane < 8; ++lane) {
                            if (aliveLane[lane] == 0.0f) continue;
                            dzArr[lane] = (zHiArr[lane] - zLoArr[lane]) * inv_N;
                        }

                        const __m256 vX_v       = _mm256_load_ps(Xarr);
                        const __m256 vY_v       = _mm256_set1_ps(Y);
                        const __m256 vZLo_v     = _mm256_load_ps(zLoArr);
                        const __m256 vDz_v      = _mm256_load_ps(dzArr);
                        const __m256 vAlive_v   = _mm256_load_ps(aliveLane);
                        const __m256 vPx_v      = _mm256_set1_ps(Px);
                        const __m256 vPy_v      = _mm256_set1_ps(Py_l);
                        const __m256 vPz_v      = _mm256_set1_ps(Pz);
                        const __m256 vDx_v      = _mm256_set1_ps(Dx);
                        const __m256 vDy_v      = _mm256_set1_ps(Dy);
                        const __m256 vDz_dir_v  = _mm256_set1_ps(Dz);
                        const __m256 vR2_v      = _mm256_set1_ps(r2);
                        const __m256 vRR_v      = _mm256_set1_ps(rr);
                        const __m256 vCosO_v    = _mm256_set1_ps(cosO);
                        const __m256 vCosI_v    = _mm256_set1_ps(cosI);
                        const __m256 vInvCIO_v  = _mm256_set1_ps(inv_cosI_minus_cosO);
                        const __m256 vZero_v    = _mm256_setzero_ps();
                        const __m256 vOne_v     = _mm256_set1_ps(1.0f);
                        const __m256 vTwo_v     = _mm256_set1_ps(2.0f);
                        const __m256 vThree_v   = _mm256_set1_ps(3.0f);
                        const __m256 vEps_v     = _mm256_set1_ps(1e-6f);
                        const __m256 vPt05_v    = _mm256_set1_ps(0.05f);
                        const __m256 mAlive     = _mm256_cmp_ps(vAlive_v, vZero_v, _CMP_GT_OQ);
                        __m256 accV = vZero_v;

                        for (int k = 0; k < N_SAMPLES; ++k) {
                            alignas(32) float fracBuf[8];
                            for (int lane = 0; lane < 8; ++lane) {
                                const uint32_t h = pxHashArr[lane]
                                    + uint32_t(k) * 0x9E3779B9u
                                    + uint32_t(s) * 0x6F4A7531u;
                                fracBuf[lane] = float(h >> 16) * (1.0f / 65536.0f);
                            }
                            const __m256 vFrac = _mm256_load_ps(fracBuf);
                            const __m256 vKf = _mm256_set1_ps(float(k));
                            const __m256 vZ  = _mm256_fmadd_ps(
                                _mm256_add_ps(vKf, vFrac), vDz_v, vZLo_v);

                            const __m256 Wx = _mm256_sub_ps(_mm256_mul_ps(vZ, vX_v), vPx_v);
                            const __m256 Wy = _mm256_sub_ps(_mm256_mul_ps(vZ, vY_v), vPy_v);
                            const __m256 Wz = _mm256_sub_ps(vZ, vPz_v);
                            const __m256 W2 = _mm256_fmadd_ps(Wx, Wx,
                                               _mm256_fmadd_ps(Wy, Wy,
                                                _mm256_mul_ps(Wz, Wz)));
                            __m256 mask = _mm256_and_ps(mAlive,
                                _mm256_cmp_ps(W2, vR2_v, _CMP_LE_OQ));
                            mask = _mm256_and_ps(mask, _mm256_cmp_ps(W2, vEps_v, _CMP_GT_OQ));

                            const __m256 DW = _mm256_fmadd_ps(vDx_v, Wx,
                                               _mm256_fmadd_ps(vDy_v, Wy,
                                                _mm256_mul_ps(vDz_dir_v, Wz)));
                            mask = _mm256_and_ps(mask, _mm256_cmp_ps(DW, vZero_v, _CMP_GT_OQ));

                            const __m256 safeW2 = _mm256_blendv_ps(vOne_v, W2, mask);
                            const __m256 invLen = _mm256_rsqrt_ps(safeW2);
                            const __m256 dist   = _mm256_mul_ps(W2, invLen);
                            const __m256 cosT   = _mm256_mul_ps(DW, invLen);
                            mask = _mm256_and_ps(mask, _mm256_cmp_ps(cosT, vCosO_v, _CMP_GE_OQ));

                            __m256 t_v = _mm256_mul_ps(_mm256_sub_ps(cosT, vCosO_v), vInvCIO_v);
                            t_v = _mm256_max_ps(vZero_v, _mm256_min_ps(vOne_v, t_v));
                            const __m256 smooth = _mm256_mul_ps(
                                _mm256_mul_ps(t_v, t_v),
                                _mm256_sub_ps(vThree_v, _mm256_mul_ps(vTwo_v, t_v)));
                            const __m256 mInner    = _mm256_cmp_ps(cosT, vCosI_v, _CMP_GE_OQ);
                            const __m256 coneAtten = _mm256_blendv_ps(smooth, vOne_v, mInner);

                            const __m256 dr        = _mm256_mul_ps(dist, vRR_v);
                            const __m256 cutoff    = _mm256_sub_ps(vOne_v, dr);
                            const __m256 invSqDen  = _mm256_fmadd_ps(dr, dr, vPt05_v);
                            const __m256 invSq     = _mm256_rcp_ps(invSqDen);
                            const __m256 distAtten = _mm256_mul_ps(_mm256_mul_ps(cutoff, cutoff), invSq);

                            // Per-lane scalar fastPow2 for T_sample.
                            __m256 vTsample = vOne_v;
                            if (hasFog) {
                                alignas(32) float zArr[8], tsArr[8];
                                _mm256_store_ps(zArr, vZ);
                                for (int lane = 0; lane < 8; ++lane)
                                    tsArr[lane] = fastPow2(fogPowK * zArr[lane]);
                                vTsample = _mm256_load_ps(tsArr);
                            }

                            __m256 contrib = _mm256_mul_ps(
                                _mm256_mul_ps(coneAtten, distAtten), vTsample);
                            contrib = _mm256_and_ps(contrib, mask);
                            accV = _mm256_add_ps(accV, contrib);
                        }

                        alignas(32) float accArr[8];
                        _mm256_store_ps(accArr, accV);
                        const float colR = lights->colR[li];
                        const float colG = lights->colG[li];
                        const float colB = lights->colB[li];
                        for (int lane = 0; lane < 8; ++lane) {
                            if (accArr[lane] <= 0.0f) continue;
                            const float w = accArr[lane] * coneDensity;
                            lightR[lane] += w * colR;
                            lightG[lane] += w * colG;
                            lightB[lane] += w * colB;
                        }
                    }
                }

                if (hasHalo) {
                    for (int o = 0; o < omniCount; ++o) {
                        const int li = omniIdx[o];
                        const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
                        const float r2 = lights->range2[li];
                        const float rr = lights->rRange[li];
                        const float PP = Px*Px + Py_l*Py_l + Pz*Pz;

                        alignas(32) float zLoArr[8] = {};
                        alignas(32) float zHiArr[8] = {};
                        alignas(32) float aliveLane[8] = {};
                        bool omniAlive = false;
                        for (int lane = 0; lane < laneCount; ++lane) {
                            if (zMaxArr[lane] <= 0.0f) continue;
                            const float X = Xarr[lane];
                            const float uV = uVarr[lane];
                            const float zMax = zMaxArr[lane];
                            constexpr float zMin = 0.05f;
                            const float VP = X*Px + Y*Py_l + Pz;
                            const float sphereC    = PP - r2;
                            const float sphereDisc = VP*VP - uV * sphereC;
                            if (sphereDisc < 0.0f) continue;
                            const float sphereSq = std::sqrt(sphereDisc);
                            const float invUV    = 1.0f / uV;
                            float zLo = (VP - sphereSq) * invUV;
                            float zHi = (VP + sphereSq) * invUV;
                            if (zLo < zMin) zLo = zMin;
                            if (zHi > zMax) zHi = zMax;
                            if (zHi <= zLo) continue;
                            zLoArr[lane]    = zLo;
                            zHiArr[lane]    = zHi;
                            aliveLane[lane] = 1.0f;
                            omniAlive = true;
                        }
                        if (!omniAlive) continue;

                        alignas(32) float dzArr[8] = {};
                        for (int lane = 0; lane < 8; ++lane) {
                            if (aliveLane[lane] == 0.0f) continue;
                            dzArr[lane] = (zHiArr[lane] - zLoArr[lane]) * inv_N;
                        }

                        const __m256 vX_v       = _mm256_load_ps(Xarr);
                        const __m256 vY_v       = _mm256_set1_ps(Y);
                        const __m256 vZLo_v     = _mm256_load_ps(zLoArr);
                        const __m256 vDz_v      = _mm256_load_ps(dzArr);
                        const __m256 vAlive_v   = _mm256_load_ps(aliveLane);
                        const __m256 vPx_v      = _mm256_set1_ps(Px);
                        const __m256 vPy_v      = _mm256_set1_ps(Py_l);
                        const __m256 vPz_v      = _mm256_set1_ps(Pz);
                        const __m256 vR2_v      = _mm256_set1_ps(r2);
                        const __m256 vRR_v      = _mm256_set1_ps(rr);
                        const __m256 vZero_v    = _mm256_setzero_ps();
                        const __m256 vOne_v     = _mm256_set1_ps(1.0f);
                        const __m256 vEps_v     = _mm256_set1_ps(1e-6f);
                        const __m256 vPt05_v    = _mm256_set1_ps(0.05f);
                        const __m256 mAlive     = _mm256_cmp_ps(vAlive_v, vZero_v, _CMP_GT_OQ);
                        __m256 accV = vZero_v;

                        for (int k = 0; k < N_SAMPLES; ++k) {
                            alignas(32) float fracBuf[8];
                            for (int lane = 0; lane < 8; ++lane) {
                                const uint32_t h = pxHashArr[lane]
                                    + uint32_t(k) * 0x9E3779B9u
                                    + uint32_t(o) * 0x517CC1B7u;
                                fracBuf[lane] = float(h >> 16) * (1.0f / 65536.0f);
                            }
                            const __m256 vFrac = _mm256_load_ps(fracBuf);
                            const __m256 vKf = _mm256_set1_ps(float(k));
                            const __m256 vZ  = _mm256_fmadd_ps(
                                _mm256_add_ps(vKf, vFrac), vDz_v, vZLo_v);

                            const __m256 Wx = _mm256_sub_ps(_mm256_mul_ps(vZ, vX_v), vPx_v);
                            const __m256 Wy = _mm256_sub_ps(_mm256_mul_ps(vZ, vY_v), vPy_v);
                            const __m256 Wz = _mm256_sub_ps(vZ, vPz_v);
                            const __m256 W2 = _mm256_fmadd_ps(Wx, Wx,
                                               _mm256_fmadd_ps(Wy, Wy,
                                                _mm256_mul_ps(Wz, Wz)));
                            __m256 mask = _mm256_and_ps(mAlive,
                                _mm256_cmp_ps(W2, vR2_v, _CMP_LE_OQ));
                            mask = _mm256_and_ps(mask, _mm256_cmp_ps(W2, vEps_v, _CMP_GT_OQ));

                            const __m256 safeW2 = _mm256_blendv_ps(vOne_v, W2, mask);
                            const __m256 invLen = _mm256_rsqrt_ps(safeW2);
                            const __m256 dist   = _mm256_mul_ps(W2, invLen);

                            const __m256 dr        = _mm256_mul_ps(dist, vRR_v);
                            const __m256 cutoff    = _mm256_sub_ps(vOne_v, dr);
                            const __m256 invSqDen  = _mm256_fmadd_ps(dr, dr, vPt05_v);
                            const __m256 invSq     = _mm256_rcp_ps(invSqDen);
                            const __m256 distAtten = _mm256_mul_ps(_mm256_mul_ps(cutoff, cutoff), invSq);

                            __m256 vTsample = vOne_v;
                            if (hasFog) {
                                alignas(32) float zArr[8], tsArr[8];
                                _mm256_store_ps(zArr, vZ);
                                for (int lane = 0; lane < 8; ++lane)
                                    tsArr[lane] = fastPow2(fogPowK * zArr[lane]);
                                vTsample = _mm256_load_ps(tsArr);
                            }

                            __m256 contrib = _mm256_mul_ps(distAtten, vTsample);
                            contrib = _mm256_and_ps(contrib, mask);
                            accV = _mm256_add_ps(accV, contrib);
                        }

                        alignas(32) float accArr[8];
                        _mm256_store_ps(accArr, accV);
                        const float colR = lights->colR[li];
                        const float colG = lights->colG[li];
                        const float colB = lights->colB[li];
                        for (int lane = 0; lane < 8; ++lane) {
                            if (accArr[lane] <= 0.0f) continue;
                            const float w = accArr[lane] * omniHaloDensity;
                            lightR[lane] += w * colR;
                            lightG[lane] += w * colG;
                            lightB[lane] += w * colB;
                        }
                    }
                }

                // Composite per lane.
                for (int lane = 0; lane < laneCount; ++lane) {
                    if (zMaxArr[lane] <= 0.0f) continue;
                    const int px = pxBase + lane;
                    const size_t i = row + size_t(px);
                    const dword pix = out[i];
                    const float surfR = float((pix >> 16) & 0xFFu);
                    const float surfG = float((pix >>  8) & 0xFFu);
                    const float surfB = float( pix        & 0xFFu);
                    const float TS = TSurfArr[lane];
                    const float newR = surfR * TS + fogEmitR_arr[lane] + lightR[lane];
                    const float newG = surfG * TS + fogEmitG_arr[lane] + lightG[lane];
                    const float newB = surfB * TS + fogEmitB_arr[lane] + lightB[lane];
                    int nR = int(newR), nG = int(newG), nB = int(newB);
                    if (nR > 255) nR = 255;
                    if (nG > 255) nG = 255;
                    if (nB > 255) nB = 255;
                    if (nR <   0) nR =   0;
                    if (nG <   0) nG =   0;
                    if (nB <   0) nB =   0;
                    out[i] = (dword(nR) << 16) | (dword(nG) << 8)
                           |  dword(nB)        | 0xFF000000u;
                }
            }
            continue;
        }
        for (int px = x1; px < x2; ++px) {
            const float X = (float(px) - CntrEX) * invFOVX;
            const float uV = X*X + Y*Y + 1.0f;

            uint32_t pxHash = uint32_t(px) * 0x9E3779B9u
                            + uint32_t(py) * 0x85EBCA6Bu
                            + 0xCAFEBABEu;
            pxHash ^= pxHash >> 13;
            pxHash *= 0xC2B2AE35u;
            pxHash ^= pxHash >> 16;

            const float zSurfRaw = float(0xFF80 - int(zEnc[row + px])) * invZScale;
            const bool isSky = (zSurfRaw <= 0.0f);
            const float zMax = isSky
                ? (hasFog ? fogFar : 1e30f)
                : (hasFog ? std::min(zSurfRaw, fogFar) : zSurfRaw);
            constexpr float zMin = 0.05f;
            if (zMax <= zMin) continue;

            // ─── Analytic ambient fog (Beer-Lambert closed form) ────
            // T_surf = transmittance from surface back to camera.
            // fog_emit = ambient_color × (1 - T_surf) per channel.
            float T_surf = 1.0f;
            float fog_emit_R = 0.0f, fog_emit_G = 0.0f, fog_emit_B = 0.0f;
            if (hasFog) {
                T_surf = fastPow2(fogPowK * zMax);
                const float fogFrac = 1.0f - T_surf;
                fog_emit_R = fogR * fogFrac;
                fog_emit_G = fogG * fogFrac;
                fog_emit_B = fogB * fogFrac;
            }

            // ─── Per-light volumetric scatter ───────────────────────
            float light_R = 0.0f, light_G = 0.0f, light_B = 0.0f;

            // Cones (existing math, kept as-is per spot; each sample's
            // contribution weighted by exp(-σ·z) for proper attenuation).
            if (hasCone) {
                for (int s = 0; s < spotCount; ++s) {
                    const int li = spotIdx[s];
                    const float Px = lights->posX[li], Py = lights->posY[li], Pz = lights->posZ[li];
                    const float Dx = lights->dirX[li], Dy = lights->dirY[li], Dz = lights->dirZ[li];
                    const float cosO = lights->cosOuter[li];
                    const float cosI = lights->cosInner[li];
                    const float r2   = lights->range2[li];
                    const float rr   = lights->rRange[li];

                    const float DV = Dx*X + Dy*Y + Dz;
                    const float DP = Dx*Px + Dy*Py + Dz*Pz;
                    const float VP = X*Px + Y*Py + Pz;
                    const float PP = Px*Px + Py*Py + Pz*Pz;
                    const float c2 = cosO * cosO;

                    // Sphere bounds.
                    const float sphereC = PP - r2;
                    const float sphereDisc = VP*VP - uV * sphereC;
                    if (sphereDisc < 0.0f) continue;
                    const float sphereSq = std::sqrt(sphereDisc);
                    const float invUV    = 1.0f / uV;
                    const float zSphLo   = (VP - sphereSq) * invUV;
                    const float zSphHi   = (VP + sphereSq) * invUV;

                    // Cone quadratic.
                    const float a = DV*DV - c2 * uV;
                    const float b = 2.0f * (c2 * VP - DV * DP);
                    const float cq = DP*DP - c2 * PP;
                    float zLo, zHi;
                    if (a < -1e-8f) {
                        const float disc = b*b - 4.0f*a*cq;
                        if (disc < 0.0f) continue;
                        const float sq = std::sqrt(disc);
                        const float inv2a = 1.0f / (2.0f * a);
                        const float r1 = (-b - sq) * inv2a;
                        const float r2_ = (-b + sq) * inv2a;
                        zLo = std::min(r1, r2_);
                        zHi = std::max(r1, r2_);
                    } else if (a > 1e-8f) {
                        const float disc = b*b - 4.0f*a*cq;
                        if (disc < 0.0f) {
                            zLo = zMin;
                            zHi = zMax;
                        } else {
                            const float sq = std::sqrt(disc);
                            const float inv2a = 1.0f / (2.0f * a);
                            const float root1 = (-b - sq) * inv2a;
                            const float root2 = (-b + sq) * inv2a;
                            const float r1Q = std::min(root1, root2);
                            const float r2Q = std::max(root1, root2);
                            if (DV > 1e-6f) {
                                zLo = std::max(r2Q, zMin);
                                zHi = zMax;
                            } else if (DV < -1e-6f) {
                                zLo = zMin;
                                zHi = std::min(r1Q, zMax);
                            } else continue;
                            if (zHi <= zLo) continue;
                        }
                    } else continue;

                    if (zLo < zSphLo) zLo = zSphLo;
                    if (zHi > zSphHi) zHi = zSphHi;
                    if (zLo < zMin)   zLo = zMin;
                    if (zHi > zMax)   zHi = zMax;
                    if (zHi <= zLo)   continue;

                    const float dz = (zHi - zLo) * inv_N;
                    float acc_attenuated = 0.0f;
                    for (int k = 0; k < N_SAMPLES; ++k) {
                        const uint32_t h = pxHash
                            + uint32_t(k) * 0x9E3779B9u
                            + uint32_t(s) * 0x6F4A7531u;
                        const float frac = float(h >> 16) * (1.0f / 65536.0f);
                        const float z = zLo + (float(k) + frac) * dz;
                        const float Wx = z*X - Px;
                        const float Wy = z*Y - Py;
                        const float Wz = z    - Pz;
                        const float W2 = Wx*Wx + Wy*Wy + Wz*Wz;
                        if (W2 > r2 || W2 < 1e-6f) continue;
                        const float DW = Dx*Wx + Dy*Wy + Dz*Wz;
                        if (DW <= 0.0f) continue;
                        const float invLen = fast_rsqrt(W2);
                        const float dist = W2 * invLen;
                        const float cosT = DW * invLen;
                        if (cosT < cosO) continue;
                        float coneAtten = 1.0f;
                        if (cosT < cosI) {
                            const float t = (cosT - cosO) / (cosI - cosO);
                            coneAtten = t * t * (3.0f - 2.0f * t);
                        }
                        const float dr = dist * rr;
                        const float cutoff = 1.0f - dr;
                        const float invSq  = 1.0f / (dr * dr + 0.05f);
                        const float distAtten = cutoff * cutoff * invSq;
                        // Beer-Lambert: per-sample transmittance from
                        // sample point z to camera = exp(-σ·z).
                        const float T_sample = hasFog ? fastPow2(fogPowK * z) : 1.0f;
                        acc_attenuated += coneAtten * distAtten * T_sample;
                    }
                    if (acc_attenuated <= 0.0f) continue;
                    const float w = acc_attenuated * coneDensity;
                    light_R += w * lights->colR[li];
                    light_G += w * lights->colG[li];
                    light_B += w * lights->colB[li];
                }
            }

            // Omni halos. Sphere-bounded integration, inverse-square
            // intensity falloff from omni center, weighted by Beer-
            // Lambert per sample. Cube shadow attenuation if present.
            if (hasHalo) {
                for (int o = 0; o < omniCount; ++o) {
                    const int li = omniIdx[o];
                    const float Px = lights->posX[li], Py = lights->posY[li], Pz = lights->posZ[li];
                    const float r2 = lights->range2[li];
                    const float rr = lights->rRange[li];
                    const float VP = X*Px + Y*Py + Pz;
                    const float PP = Px*Px + Py*Py + Pz*Pz;

                    const float sphereC = PP - r2;
                    const float sphereDisc = VP*VP - uV * sphereC;
                    if (sphereDisc < 0.0f) continue;
                    const float sphereSq = std::sqrt(sphereDisc);
                    const float invUV    = 1.0f / uV;
                    float zLo = (VP - sphereSq) * invUV;
                    float zHi = (VP + sphereSq) * invUV;
                    if (zLo < zMin) zLo = zMin;
                    if (zHi > zMax) zHi = zMax;
                    if (zHi <= zLo) continue;

                    const float dz = (zHi - zLo) * inv_N;
                    float acc_attenuated = 0.0f;
                    for (int k = 0; k < N_SAMPLES; ++k) {
                        const uint32_t h = pxHash
                            + uint32_t(k) * 0x9E3779B9u
                            + uint32_t(o) * 0x517CC1B7u;
                        const float frac = float(h >> 16) * (1.0f / 65536.0f);
                        const float z = zLo + (float(k) + frac) * dz;
                        const float Wx = z*X - Px;
                        const float Wy = z*Y - Py;
                        const float Wz = z    - Pz;
                        const float W2 = Wx*Wx + Wy*Wy + Wz*Wz;
                        if (W2 > r2 || W2 < 1e-6f) continue;
                        const float invLen = fast_rsqrt(W2);
                        const float dist = W2 * invLen;
                        const float dr = dist * rr;
                        const float cutoff = 1.0f - dr;
                        const float invSq  = 1.0f / (dr * dr + 0.05f);
                        const float distAtten = cutoff * cutoff * invSq;
                        const float T_sample = hasFog ? fastPow2(fogPowK * z) : 1.0f;
                        acc_attenuated += distAtten * T_sample;
                        // (Cube shadow lookup could go here per sample;
                        // for the initial MVP we skip it — halos look
                        // reasonable without per-sample shadow, and
                        // adding it doubles the per-sample cost.)
                    }
                    if (acc_attenuated <= 0.0f) continue;
                    const float w = acc_attenuated * omniHaloDensity;
                    light_R += w * lights->colR[li];
                    light_G += w * lights->colG[li];
                    light_B += w * lights->colB[li];
                }
            }

            // ─── Composite ──────────────────────────────────────────
            // out = surface × T_surf + fog_emit + light_emit
            // Works uniformly for sky and opaque: for sky pixels in
            // fogged scenes, zMax=fogFar so T_surf is small and the
            // sky-cube color fades into the fog ambient — correct
            // horizon behaviour. For non-fogged scenes T_surf=1 and
            // fog_emit=0, so out = surface + light_emit (additive
            // cone/halo with no attenuation).
            const size_t i = row + size_t(px);
            const dword pix = out[i];
            const float surfR = float((pix >> 16) & 0xFFu);
            const float surfG = float((pix >>  8) & 0xFFu);
            const float surfB = float( pix        & 0xFFu);
            const float newR = surfR * T_surf + fog_emit_R + light_R;
            const float newG = surfG * T_surf + fog_emit_G + light_G;
            const float newB = surfB * T_surf + fog_emit_B + light_B;
            (void)isSky;
            int nR = int(newR), nG = int(newG), nB = int(newB);
            if (nR > 255) nR = 255;
            if (nG > 255) nG = 255;
            if (nB > 255) nB = 255;
            if (nR <   0) nR =   0;
            if (nG <   0) nG =   0;
            if (nB <   0) nB =   0;
            out[i] = (dword(nR) << 16) | (dword(nG) << 8)
                   |  dword(nB)        | 0xFF000000u;
        }
    }
}

void Render_DeferredVolumetric() {
    VolProfScope _vp(&g_volProf.ms_unified, &g_volProf.n_unified);
    if (!CurScene || !ZPage16 || !VPage) return;

    extern DeferredLightingCtx g_deferredCtx;
    const ViewLightsSoA *const lights = g_deferredCtx.lights;
    if (!lights) return;
    const int numLights = g_deferredCtx.numLights;

    // Pre-filter spot vs omni index lists.
    static int spotIdx[DEFERRED_MAX_LIGHTS];
    static int omniIdx[DEFERRED_MAX_LIGHTS];
    int spotCount = 0, omniCount = 0;
    for (int i = 0; i < numLights; ++i) {
        // Mirror clones don't cast volumetric glow — same additive-wash
        // reasoning as the halo pass (see Render_OmniHalos).
        if (lights->mirrorId[i] != 0) continue;
        if (lights->isSpot[i]) spotIdx[spotCount++] = i;
        else                   omniIdx[omniCount++] = i;
    }

    const float invFOVX  = 1.0f / FOVX;
    const float invFOVY  = 1.0f / FOVY;
    const float invZScale= 1.0f / float(g_zscale);
    const bool  fogged   = (CurScene->Flags & Scn_Fogged) != 0;
    const float fogFar   = fogged ? CurScene->FZP : 1e30f;
    const float sigma    = fogged
        ? fds::FeatureFlags::fog_sigma_mult() / CurScene->FZP
        : 0.0f;
    const float fogR     = float(CurScene->Ambient.R);
    const float fogG     = float(CurScene->Ambient.G);
    const float fogB     = float(CurScene->Ambient.B);
    const float coneDens = fds::FeatureFlags::draw_cones()
        ? fds::FeatureFlags::cone_strength() * 0.001f
        : 0.0f;
    const float haloDens = fds::FeatureFlags::omni_halo_strength() * 0.001f;

    constexpr int numTilesX = 6;
    constexpr int numTilesY = 4;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;

    renderns::tileCounter = 0;
    for (int j = 0; j < numTilesY; ++j) {
        const int y1 = tileSizeY * j;
        const int y2 = std::min(y1 + tileSizeY, YRes);
        for (int i = 0; i < numTilesX; ++i) {
            const int x1 = tileSizeX * i;
            const int x2 = std::min(x1 + tileSizeX, XRes);
            const int *sP = spotIdx; const int sC = spotCount;
            const int *oP = omniIdx; const int oC = omniCount;
            ThreadPool::instance().enqueue([=]() {
                Render_DeferredVolumetric_Tile(
                    x1, y1, x2, y2, lights, sP, sC, oP, oC,
                    invFOVX, invFOVY, invZScale,
                    sigma, fogFar, fogR, fogG, fogB,
                    coneDens, haloDens);
                // One permit per completed tile (see renderns::tileDone).
                renderns::tileDone.release();
            });
        }
    }
    for (int _i = 0, n = numTilesX * numTilesY; _i < n; ++_i) {
        renderns::tileDone.acquire();
    }
}
