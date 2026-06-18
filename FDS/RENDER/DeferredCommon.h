#ifndef FDS_RENDER_DEFERRED_COMMON_H_INCLUDED
#define FDS_RENDER_DEFERRED_COMMON_H_INCLUDED

// DeferredCommon.h — types, constants and inline helpers shared by the
// deferred-lighting translation units (split out of the former
// monolithic DeferredLighting.cpp):
//   DeferredSurfaceKernel.cpp — per-pixel lighting kernels + the
//                               Render_DeferredLighting orchestrator
//   DeferredLightLists.cpp    — per-tile / per-strip light culling
//   DeferredShadowSampling.h  — per-pixel shadow resolve (inline)
//   DeferredVolumetric.cpp    — cones / halos / skybox / legacy fog
//   DeferredFastFog.cpp       — analytic + froxel fog, unified pass
// Everything here was moved VERBATIM from the monolith.

#include <cmath>
#include <cstdint>
#include <atomic>
#include <chrono>
#include "simde/x86/fma.h"

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/CameraContext.h"   // fds::CameraContext (light-list builders)

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
	// Omni_FogTransient: the froxel fog adds this light's in-scatter per-frame
	// on top of the blended base fog (not into history) — for short flashes.
	alignas(32) uint32_t isFlash[DEFERRED_MAX_VIEW_LIGHTS];
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
	// Mirror-bounce window AABB (world space). A bounce spot's apex sits
	// BEHIND the mirror; its light only legitimately reaches a room point
	// P if the apex→P segment passes through the mirror WINDOW rectangle.
	// The surface kernel runs a per-pixel portal test (bouncePortalReject)
	// against this AABB. Non-bounce lights store an INVERTED AABB
	// (winMin > winMax) so the test gate `winMinX <= winMaxX` is false and
	// they pay nothing. Set from Omni::mirrorWinMin/Max in the SoA build.
	alignas(32) float    winMinX[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float    winMinY[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float    winMinZ[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float    winMaxX[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float    winMaxY[DEFERRED_MAX_VIEW_LIGHTS];
	alignas(32) float    winMaxZ[DEFERRED_MAX_VIEW_LIGHTS];
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
	// 1 for bounce spots (Omni_BounceCone): the reflected-source-map tap
	// defaults DARK and lights only inside the source's cone. Clones (0)
	// stay default-lit. See ViewLightsSoA::bounceClamp.
	alignas(32) uint32_t bounceClamp[DEFERRED_MAX_LIGHTS];
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
	// Mirror-bounce window AABB (world). See ViewLightsSoA::winMin/Max.
	// Per-tile copy for the surface kernel's portal test.
	alignas(32) float    winMinX[DEFERRED_MAX_LIGHTS];
	alignas(32) float    winMinY[DEFERRED_MAX_LIGHTS];
	alignas(32) float    winMinZ[DEFERRED_MAX_LIGHTS];
	alignas(32) float    winMaxX[DEFERRED_MAX_LIGHTS];
	alignas(32) float    winMaxY[DEFERRED_MAX_LIGHTS];
	alignas(32) float    winMaxZ[DEFERRED_MAX_LIGHTS];
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
	// Render-target addressing — replaces the per-kernel reads of the
	// XRes/YRes/VPage/ZPage16/CntrE* globals and the transparent G-buffer
	// set, so the tile kernels run off the context (RenderContext migration,
	// docs/RENDER_CONTEXT_PLAN.md). Populated once in Render_DeferredLighting.
	int                  xres;
	int                  yres;
	byte                *vpage;        // 32-bit BGRA framebuffer (== VPage)
	word                *zpage16;      // 16-bit depth (== ZPage16)
	float                cntrEX;
	float                cntrEY;
	float                fovX;         // == FOVX (volumetric passes use it raw)
	float                fovY;         // == FOVY
	float                zscale;       // == g_zscale
	meka::GBuffer       *gbXpar;       // transparent front layer
	word                *xparZ;        // transparent front depth
	word                *xparZBack;    // transparent back depth
};

// File-scope ctx, populated each frame by Render_DeferredLighting and
// also read by Render_DeferredTransparentLighting_Tile. Both kernels
// share the same per-frame setup (matTable, tileLights, view-space
// projection params, scene); rebuilding it per pass would double the
// per-frame setup cost. Defined in DeferredSurfaceKernel.cpp.
extern DeferredLightingCtx g_deferredCtx;

// Per-target override for an OFFSCREEN deferred bake (mirror-shard
// reflections): when passed to Render_DeferredLighting, every render-target /
// camera / scratch-buffer it would read from the engine globals comes from
// here instead, so N bakes run concurrently on the pool (each owns its
// G-buffer + view-space light list + tile-light buffer). nullptr → the engine
// globals (main frame; byte-identical). `inlineDispatch` runs the tile kernels
// on the calling thread (no pool enqueue) for the inter-render model.
struct DeferredOverride {
    meka::GBuffer        *gb         = nullptr;  // opaque G-buffer (this target)
    const fds::CameraContext *cam    = nullptr;  // view + projection
    ViewLightsSoA        *lights     = nullptr;  // per-view light SoA scratch
    TileLights           *tileLights = nullptr;  // per-target tile-light scratch
    byte                 *vpage      = nullptr;  // color framebuffer
    word                 *zpage16    = nullptr;  // depth
    int                   xres       = 0;
    int                   yres       = 0;
    meka::GBuffer        *gbXpar     = nullptr;  // transparent layers (null = opaque only)
    word                 *xparZ      = nullptr;
    word                 *xparZBack  = nullptr;
    bool                  inlineDispatch = false;
};

// Deferred opaque lighting pass. ov=nullptr → main frame (engine globals,
// pool-tiled). ov!=nullptr → offscreen bake into ov's target (see above).
void Render_DeferredLighting(DeferredLightingCtx &ctx, const DeferredOverride *ov = nullptr);

// Volumetric spotlight cones (disco beams etc.). Reads its render target from
// ctx (xres/vpage/zpage16/invFOVX…), so after a per-worker deferred bake the
// shard reflection can run it to draw the beams. inlineDispatch=true runs the
// tiles on the calling thread (offscreen bake); false pool-tiles (main frame).
void Render_VolumetricCones(const DeferredLightingCtx &ctx, bool inlineDispatch = false);

// Light-list builders (DeferredLightLists.cpp). Called once per frame
// by the Render_DeferredLighting orchestrator; buildStripLightLists
// fills g_stripLights for the unified-TBR transparent strip path.
void computeTileDepthBounds(TileLights *tileLights, int numTilesX, int numTilesY,
                            int tileSizeX, int tileSizeY, int xres, int yres,
                            float invZScale, const uint16_t *zpage16);
void computeMirrorPresenceGrid(const uint8_t *mask, int w, int h,
                               int regionW, int regionH,
                               int regionsX, int regionsY,
                               uint32_t *out);
void buildTileLightLists(TileLights *tileLights, int numTilesX, int numTilesY,
                         int tileSizeX, int tileSizeY, int xres, int yres,
                         const ViewLightsSoA &lights, int numLights,
                         const uint32_t *tileMirrorPresence,
                         const fds::CameraContext &cam);
void buildStripLightLists(int numStrips, int stripHeight, int yres,
                          const ViewLightsSoA &lights, int numLights,
                          const uint32_t *stripMirrorPresence,
                          const fds::CameraContext &cam);

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

// Fast-fog hooks for the transparent peel (defined in DeferredFastFog
// .cpp): per-frame validity + a sample of in-scatter acc / transmittance
// T at a pixel's own depth. Froxel variant fetches the grid; screen-
// space variant evaluates the analytic/blob fog for the ray.
bool FastFog_XparActive();
void FastFog_SampleGrid(int px, int py, float z,
                        float& aR, float& aG, float& aB, float& T);
bool FastFog_SSActive();
void FastFog_SSSample(int px, int py, float z,
                      float& aR, float& aG, float& aB, float& T);

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

// Window-portal test for mirror-bounce spots (surface lighting). A
// bounce spot's apex sits behind the mirror plane; light reaches a room
// point P only if the segment apex→P passes through the mirror WINDOW.
// Returns true when the light should be REJECTED for this pixel. The
// caller gates on a valid window (winMinX[n] <= winMaxX[n]) so non-bounce
// lights — which store an inverted AABB — never call this. `L` is duck-
// typed over ViewLightsSoA / TileLights (both carry posWorld*, mirN*,
// mirD, winMin*/winMax*).
//
// COVERAGE: applied in the SCALAR surface kernels — Render_DeferredLighting
// _Tile's scalar path and Render_DeferredLighting_TileFill (the sub-rate
// quarter/checkerboard fill). These are the only paths greets uses by
// default (deferred_vec off, PreferOuterVec off). NOT yet applied in the
// 8-wide vec inner loops (run_vec_spec_loop / the main vec body / OuterVec
// vec) or the transparent peel — so --deferred-vec / --deferred-outer-vec
// on greets would let the bounce leak return. Deferred to the x64 vec-path
// pass; see memory mirror-beam-reflections-design.
template <class L>
static inline bool bouncePortalReject(const L &tl, int n,
                                      float Px, float Py, float Pz)
{
	const float Nx = tl.mirNX[n], Ny = tl.mirNY[n], Nz = tl.mirNZ[n], Nd = tl.mirD[n];
	const float Ax = tl.posWorldX[n], Ay = tl.posWorldY[n], Az = tl.posWorldZ[n];
	const float tA = Nx*Ax + Ny*Ay + Nz*Az + Nd;   // signed dist: apex
	const float tP = Nx*Px + Ny*Py + Nz*Pz + Nd;   // signed dist: sample
	// The light path crosses the mirror plane only when the apex and the
	// lit point straddle it. Same side ⇒ no path through the window
	// (sample behind the glass, or coplanar wall surround at tP≈0).
	if (tA * tP >= 0.0f) return true;
	const float s = tA / (tA - tP);                // crossing ∈ (0,1)
	const float cx = Ax + s * (Px - Ax);
	const float cy = Ay + s * (Py - Ay);
	const float cz = Az + s * (Pz - Az);
	constexpr float pad = 0.05f;                   // edge + thin-axis slack
	return cx < tl.winMinX[n] - pad || cx > tl.winMaxX[n] + pad ||
	       cy < tl.winMinY[n] - pad || cy > tl.winMaxY[n] + pad ||
	       cz < tl.winMinZ[n] - pad || cz > tl.winMaxZ[n] + pad;
}

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
