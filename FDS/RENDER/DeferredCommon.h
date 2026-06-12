#ifndef FDS_RENDER_DEFERRED_COMMON_H_INCLUDED
#define FDS_RENDER_DEFERRED_COMMON_H_INCLUDED

// DeferredCommon.h — types, constants and inline helpers shared by the
// deferred-lighting translation units (split out of the former
// monolithic DeferredLighting.cpp):
//   DeferredLighting.cpp      — per-frame orchestration
//   DeferredLightLists.cpp    — per-tile / per-strip light culling
//   DeferredSurfaceKernel.cpp — per-pixel lighting kernels
//   DeferredVolumetric.cpp    — cones / halos / fog tile passes
//   DeferredFastFog.cpp       — analytic + froxel fog
// Everything here is moved VERBATIM from DeferredLighting.cpp; see that
// file's header comment for the pipeline overview.

#include <cmath>
#include <cstdint>
#include <atomic>
#include <chrono>
#include "simde/x86/fma.h"

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"

namespace meka { struct GBuffer; }
struct Scene;

constexpr int DEFERRED_MAX_LIGHTS = 128;
// Scene-wide light capacity (ViewLightsSoA + the halo/volumetric index
// scratch), decoupled from the per-tile cap above. Mirror-cloned omnis
// multiply the scene total (greets: 15 source × (1 + #mirrors)) but the
// per-tile mirror-footprint cull keeps each TILE's list small — so the
// scene array must hold them all while TileLights stays at 128.
// ViewLightsSoA is ~33 arrays of 4 bytes → 256 entries ≈ 34 KB, trivial.
constexpr int DEFERRED_MAX_VIEW_LIGHTS = 256;
constexpr int DEFERRED_NUM_TILES_X = 12;
constexpr int DEFERRED_NUM_TILES_Y = 8;
constexpr int DEFERRED_NUM_TILES   = DEFERRED_NUM_TILES_X * DEFERRED_NUM_TILES_Y;

struct ViewLightsSoA {
	alignas(32) float posX[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float posY[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float posZ[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float colB[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float colG[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float colR[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float range2[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float rRange[DEFERRED_MAX_VIEW_LIGHTS];
	// Spot light cone (Light_SpotLight). dirX/Y/Z is the cone axis in
	// view space (unit vector). cosInner / cosOuter are the cone half
	// angles in cosine form. isSpot=0 means omni (cone params ignored).
	alignas(32) float dirX[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float dirY[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float dirZ[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float cosInner[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float cosOuter[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) uint32_t isSpot[DEFERRED_MAX_VIEW_LIGHTS];
	// Omni_ForceVolCone: per-light volumetric-cone opt-in (renders a
	// cone even when --draw-cones is off scene-wide).
	alignas(32) uint32_t forceCone[DEFERRED_MAX_VIEW_LIGHTS];
	// sin(outer half-angle) for spots (0 for omnis) — precomputed for
	// the tile-vs-cone cull.
	alignas(32) float sinOuter[DEFERRED_MAX_VIEW_LIGHTS];
	// Clone lights: source's 2D shadow map index (-1 = none) + the
	// mirror plane, for mirrored shadow sampling (the clone's
	// visibility of P = the source's visibility of reflect(P)).
	alignas(32) int32_t srcShadowMapIdx[DEFERRED_MAX_VIEW_LIGHTS];
	// Omni_BounceCone: the cone pass clamps this spot's chord to the
	// camera side of its mirror plane (mirN/mirD) — the apex sits
	// behind the glass.
	alignas(32) uint32_t bounceClamp[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float mirNX[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float mirNY[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float mirNZ[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float mirD [DEFERRED_MAX_VIEW_LIGHTS];
	// Index into g_shadowMaps for this light's shadow map, or -1 if
	// not a shadow-caster (most omnis). Filled per frame in
	// Render_DeferredLighting alongside the other per-light fields.
	alignas(32) int32_t  shadowMapIdx[DEFERRED_MAX_VIEW_LIGHTS];
	// World-space position of the light. Required for cube shadow
	// face selection (option 3 in the cube infra design): the per-
	// pixel sample's world position is computed once from the view-
	// space sample, then `D_world = sample_world - omni_world` picks
	// the cube face. Populated for all lights so the kernel doesn't
	// branch on light type to read it; only used by cube-shadow path.
	alignas(32) float posWorldX[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float posWorldY[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float posWorldZ[DEFERRED_MAX_VIEW_LIGHTS];
	// Index into g_cubeShadowRefs for omnis with cube shadow, or -1.
	// Mutually exclusive with shadowMapIdx (which is only meaningful
	// for Light_SpotLight). Populated by Render_DeferredLighting from
	// the omni's Type + CastsShadow flag + cube allocation.
	alignas(32) int32_t  cubeShadowIdx[DEFERRED_MAX_VIEW_LIGHTS];
	// Per-omni halo controls, decoupled from surface lighting. See
	// Omni::HaloIntensity / Omni::HaloRange comments. haloDensityMul[]
	// is the per-omni density multiplier (1.0 for legacy behavior);
	// haloRange2[] / haloRRange[] override range2[] / rRange[] for
	// the halo sphere bounds (falling back to those when HaloRange=0).
	alignas(32) float    haloDensityMul[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float    haloRange     [DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float    haloRange2    [DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float    haloRRange    [DEFERRED_MAX_VIEW_LIGHTS];
	// Per-light mirror id (0 = original world; >0 = clone of mirror
	// with that id). The kernels read gb.mirrorId[pixel] once per
	// pixel and skip any light whose mirrorId disagrees, so original-
	// world surfaces are lit only by original omnis and each mirror's
	// clone surfaces only by that mirror's cloned omnis. Without this
	// filter clone pixels receive the union of both light sets and
	// saturate (greets teleporter mirror went uniformly yellow).
	alignas(32) uint32_t mirrorId      [DEFERRED_MAX_VIEW_LIGHTS];
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
	// Clone lights: source's 2D map + mirror plane (mirrored shadow
	// sampling; see ViewLightsSoA::srcShadowMapIdx).
	alignas(32) int32_t srcShadowMapIdx[DEFERRED_MAX_LIGHTS];
	alignas(32) float mirNX[DEFERRED_MAX_LIGHTS];
	alignas(32) float mirNY[DEFERRED_MAX_LIGHTS];
	alignas(32) float mirNZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float mirD [DEFERRED_MAX_LIGHTS];
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
// per-frame setup cost. Defined in DeferredLighting.cpp.
extern DeferredLightingCtx g_deferredCtx;

// Light-list builders (DeferredLightLists.cpp). Called once per frame
// by the Render_DeferredLighting orchestrator; buildStripLightLists
// fills g_stripLights for the unified-TBR transparent strip path.
void computeTileDepthBounds(TileLights *tileLights, int numTilesX, int numTilesY,
                            int tileSizeX, int tileSizeY, int xres, int yres,
                            float invZScale);
void computeMirrorPresenceGrid(const uint8_t *mask, int w, int h,
                               int regionW, int regionH,
                               int regionsX, int regionsY,
                               uint32_t *out);
void buildTileLightLists(TileLights *tileLights, int numTilesX, int numTilesY,
                         int tileSizeX, int tileSizeY, int xres, int yres,
                         const ViewLightsSoA &lights, int numLights,
                         const uint32_t *tileMirrorPresence);
void buildStripLightLists(int numStrips, int stripHeight, int yres,
                          const ViewLightsSoA &lights, int numLights,
                          const uint32_t *stripMirrorPresence);

// MAX strips at any reasonable display: 4096/8 = 512. We size for
// that ceiling so reallocation isn't needed at runtime resize.
constexpr int DEFERRED_MAX_STRIPS = 512;
// Per-strip light lists (defined in DeferredLightLists.cpp), consumed
// by RenderXparClumpInStrip via a DeferredLightingCtx variant whose
// `tileLights` points at this array.
extern TileLights g_stripLights[DEFERRED_MAX_STRIPS];

// Conservative sphere-vs-spot-cone rejection (cone expanded by the
// sphere radius, capped at `range`). Returns true when the sphere is
// definitely outside the cone volume — safe to drop the light for
// every pixel the sphere bounds. Range-sphere culling alone puts a
// narrow spot (disco beams: 4.5° half-angle, range 38) in nearly
// every tile while its cone intersects almost none of them.
static inline bool sphereOutsideCone(float cx, float cy, float cz, float R,
                                     float ax, float ay, float az,
                                     float dx, float dy, float dz,
                                     float range, float cosO, float sinO)
{
	const float vx = cx - ax, vy = cy - ay, vz = cz - az;
	const float a  = vx*dx + vy*dy + vz*dz;
	if (a > range + R) return true;   // beyond the cap
	if (a < -R)        return true;   // behind the apex
	const float v2 = vx*vx + vy*vy + vz*vz;
	const float q2 = v2 - a*a;
	const float q  = (q2 > 0.0f) ? std::sqrt(q2) : 0.0f;
	return (cosO * q - sinO * a) > R; // outside the expanded cone
}

// Bounding sphere of a tile's view-space frustum chunk: the screen
// rect [x0,x1]×[y0,y1] swept over depth [zLo,zHi].
struct TileChunkSphere { float cx, cy, cz, R; bool valid; };
static inline TileChunkSphere tileChunkSphere(float x0, float x1,
                                              float y0, float y1,
                                              float zLo, float zHi)
{
	TileChunkSphere t{0, 0, 0, 0, false};
	if (!(zHi >= zLo) || zHi <= 0.0f) return t;
	if (zLo < 0.05f) zLo = 0.05f;
	const float xn0 = (x0 - CntrEX) / FOVX, xn1 = (x1 - CntrEX) / FOVX;
	const float yn0 = (CntrEY - y1) / FOVY, yn1 = (CntrEY - y0) / FOVY;
	float px[8], py[8], pz[8];
	int n = 0;
	for (float z : { zLo, zHi })
		for (float xn : { xn0, xn1 })
			for (float yn : { yn0, yn1 }) {
				px[n] = xn * z; py[n] = yn * z; pz[n] = z; ++n;
			}
	float cx = 0, cy = 0, cz = 0;
	for (int i = 0; i < 8; ++i) { cx += px[i]; cy += py[i]; cz += pz[i]; }
	cx *= 0.125f; cy *= 0.125f; cz *= 0.125f;
	float r2 = 0;
	for (int i = 0; i < 8; ++i) {
		const float dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
		const float d2 = dx*dx + dy*dy + dz*dz;
		if (d2 > r2) r2 = d2;
	}
	t.cx = cx; t.cy = cy; t.cz = cz; t.R = std::sqrt(r2); t.valid = true;
	return t;
}

// Volumetric-pass timing accumulators (one set per process; updated
// from the main thread that owns the pass dispatch). Printed every
// FDS_VOL_PROF_INTERVAL frames (default 60) by VolProf_Tick. Shared
// between DeferredVolumetric.cpp (cones/halos/skybox) and
// DeferredFastFog.cpp (unified pass); g_volProf is defined in
// DeferredVolumetric.cpp.
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
extern VolProf g_volProf;

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

// rsqrt + one Newton-Raphson step (~24-bit). The cone passes feed
// cosT = D·W·rsqrt(W²) into smoothstep((cosT−cosO)/(cosI−cosO)):
// for NARROW cones the 1/(cosI−cosO) gain is ~350 (1.5°/4.5°), which
// amplifies the raw 12-bit rsqrt's quantization staircase into ±10%
// attenuation noise — the beam 'fur'/fan-stripe moire family. Wide
// city cones (gain 2-10) never showed it.
static inline __m256 rsqrt_nr_x8(__m256 x) {
    __m256 r = _mm256_rsqrt_ps(x);
    return _mm256_mul_ps(r, _mm256_fnmadd_ps(
        _mm256_mul_ps(_mm256_set1_ps(0.5f), x),
        _mm256_mul_ps(r, r), _mm256_set1_ps(1.5f)));
}

#endif // FDS_RENDER_DEFERRED_COMMON_H_INCLUDED
