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
#include <algorithm>
#include <chrono>
#include <vector>
#include "simde/x86/fma.h"

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "RENDER/Hdr.h"   // fds::hdrf — the per-target HDR radiance element
#include "Base/CameraContext.h"   // fds::CameraContext (light-list builders)

namespace meka { struct GBuffer; }
struct Scene;

// SSR (--env_ssr) previous-frame color copy. renderFrame memcpys VPage into
// this at the FIRST deferred pass of each main frame (before that pass's
// raster overwrites VPage, so it holds last frame's FINAL image); the env
// compose's SSR march (EnvSpecComposeScalar) samples it — 1-frame-stale, since
// sampling live VPage races the concurrent tile writes. Sized XRes*YRes*4
// (BGRA dwords, == PageSize). g_ssrPrevW/H are the dims it was sized for; the
// march only runs when ctx.xres/yres match (skips mismatched offscreen bakes).
extern std::vector<uint8_t> g_ssrPrevColor;
extern int g_ssrPrevW, g_ssrPrevH;

// Does anything in this process configuration READ the lightmapMF/ST G-buffer
// planes? Two independent gates must both be open (0b466b7, and the
// --shadow_lightmap flag row):
//   1. shadow_lightmap()  — the ALLOCATION gate. EngineGBuffer_Resize consults
//      it at BOOT for the main G-buffer; the OFFSCREEN G-buffer builders (the
//      mirror RTT slot, the shard bake's serial + per-worker buffers) consult
//      it whenever they lazily build, which for greets is AFTER
//      GreetsApplyRunDefaults has turned it on — so they, unlike the main
//      buffer, really did allocate.
//   2. lmKernelEnabled = !shadow_dynamic() || shadow_lm_dynamic() — the
//      per-pixel SAMPLE gate (DeferredSurfaceKernel.cpp, resolveCubeAtten).
// greets ships with shadow_dynamic ON and shadow_lm_dynamic OFF, so gate 2 is
// shut and every texel those offscreen planes cost — 6 B/px of store plus
// Mekalele's per-pixel writes into them — was unreadable. Allocating on gate 1
// alone is what made that dead weight; the offscreen builders use THIS, so
// they cannot drift from the kernel. `--shadow_lightmap --shadow_lm_dynamic`
// (the only arm that can sample the atlas) still opens both and still gets its
// planes, bit-for-bit.
inline bool DeferredLightmapPlanesReadable() {
	return fds::FeatureFlags::shadow_lightmap()
	    && (!fds::FeatureFlags::shadow_dynamic()
	        || fds::FeatureFlags::shadow_lm_dynamic());
}

constexpr int DEFERRED_MAX_LIGHTS = 128;
// Scene-wide light capacity (ViewLightsSoA + the halo/volumetric index
// scratch), decoupled from the per-tile cap above. Mirror-cloned omnis
// multiply the scene total (greets: 15 source × (1 + #mirrors)) but the
// per-tile mirror-footprint cull keeps each TILE's list small — so the
// scene array must hold them all while TileLights stays at 128.
// ViewLightsSoA is 41 arrays of 4 bytes → 256 entries ≈ 41 KB, trivial.
// (Said "~33 arrays ≈ 34 KB" until --mem_census counted them.)
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
	// Clone lights whose source is a CUBE omni: the source's cube-ref
	// index (-1 = none). Same reflect-and-sample-source idea as
	// srcShadowMapIdx, but a cube tap at reflect(P) instead of a 2D map.
	alignas(32) int32_t srcCubeShadowIdx[DEFERRED_MAX_VIEW_LIGHTS];
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
	// Per-spot volumetric-cone density multiplier (haloDensityMul's cone
	// sibling). Filled from Omni::VolBeamGain in the SoA build (0 = unset
	// → 1.0), consumed by Render_VolumetricCones_Tile — scalar and vec
	// integration paths both multiply the accumulated integral by it.
	alignas(32) float    coneGain      [DEFERRED_MAX_VIEW_LIGHTS];
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
// Memory: this said "24 tiles × 8 arrays × 128 floats × 4 bytes = 96 KiB
// total" for a long time and was stale by ~25 arrays and 72 tiles — the number
// it quoted is why nobody looked. MEASURED by --mem_census: sizeof(TileLights)
// is 16 928 B (33 arrays × DEFERRED_MAX_LIGHTS=128 × 4 B), so
// s_tileLights[DEFERRED_NUM_TILES=96] is 1.55 MiB and
// g_stripLights[DEFERRED_MAX_STRIPS=512] (DeferredLightLists.cpp) is 8.27 MiB.
// Both are BSS sized by the CAP, not by the scene: at 1080p only 135 of the 512
// strips are ever written, so most of the 8.27 MiB stays untouched (address
// space, not RSS) — but it is 8.27 MiB of the binary's BSS either way, and a
// per-tile slice is no longer the "easily fits in L1" object this claimed.
// Keep this figure honest: run `--mem_census` if you change the array count.
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
	// Clone lights whose source is a CUBE omni: source's cube-ref index
	// (mirrored cube tap; see ViewLightsSoA::srcCubeShadowIdx).
	alignas(32) int32_t srcCubeShadowIdx[DEFERRED_MAX_LIGHTS];
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
	float           zMax;           // view-space z of farthest OPAQUE surface
	                                // pixel in tile (+inf / -inf when the tile
	                                // has no geometry). SKY / untouched pixels
	                                // are NOT counted — see hasSky.
	bool            hasSky;         // ≥1 pixel had no geometry (zEnc==0). Such
	                                // pixels' rays run to the fog cutoff, so the
	                                // volumetric cone cull must extend its far
	                                // bound past zMax for these tiles (else it
	                                // clips beams glowing in the sky part → a
	                                // rectangular per-tile seam).
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
	// Mirror-footprint presence bits per LIGHTING tile (see
	// computeMirrorPresenceGrid) for the volumetric passes' clone-light
	// tile cull. Copied BY VALUE: the kernel's grid is a file-static
	// that an offscreen bake (shard/RTT) can recompute between the
	// lighting call and the cone/halo passes. hasMirrorPresence=false →
	// keep all clone lights (no mirrors this frame / offscreen ctx).
	uint32_t             tileMirrorPresence[DEFERRED_NUM_TILES];
	bool                 hasMirrorPresence;
	// GEOMETRY OF THE GRID tileLights[] IS INDEXED ON. Needed because the
	// transparent composite is dispatched on renderFrame's 6x5 FRAME tiler, not
	// on this 12x8 lighting grid, so it cannot use its dispatch ordinal as a
	// tileLights[] subscript — it has to resolve the light tile from the pixel.
	// Populated once per pass in Render_DeferredLighting (offscreen targets get
	// their own clamped grid, so these must be read from the ctx, not from
	// DEFERRED_NUM_TILES_X/Y).
	int                  ltNumX, ltNumY;     // light-grid tile counts
	int                  ltSizeX, ltSizeY;   // light-grid tile size in pixels
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
	// Per-matID bitmask of Shadow_MaterialSkipsCasting(matTable.data[matID])
	// — "this material was excluded from the shadow BAKE", which under
	// --shadow_noncaster_depth resolves the receiver to the -1 (force-Depth)
	// sentinel. The predicate depends ONLY on the Material*, and matID selects
	// that from matTable, so it is constant for the whole frame; it was being
	// re-evaluated once per shaded PIXEL through an out-of-line call with a
	// function-local atomic cache (measured 3.44 % of all steady-state samples
	// on greets t=5743 — 5.5 % of the lighting stage). Filled once per frame in
	// Render_DeferredLighting; matID is 8-bit so 256 bits covers the table.
	// Bit set == skips casting. Byte-identical: same predicate, same Material*.
	uint64_t             shadowSkipMask[4];
	// "Some material in matTable carries a NormalMap." Same once-per-frame
	// table scan as shadowSkipMask above, and for the same reason: the OuterVec
	// kernel ran an 8-iteration per-LANE loop that re-derives matID, re-resolves
	// Material* and tests Mat->NormalMap on EVERY 8-pixel group, wrapped in a
	// store/reload round-trip of the three normal registers. When this is false
	// the loop provably writes nothing and the reload is the identity, so both
	// are skipped (byte-null by construction). CITY: 138 materials, ZERO normal
	// maps (--deferred_gloss_stats). greets does carry them and takes the loop.
	bool                 anyNormalMap;
	// True when the tile kernels run INLINE on the calling thread (an
	// offscreen bake: DeferredOverride::inlineDispatch). Then the kernel must
	// NOT release renderns::tileDone and the dispatch loop must not acquire
	// it: the permit would go straight back to the thread that posted it, and
	// that shared semaphore is the one every pool thread uses. MEASURED: 12
	// threads round-tripping ONE std::counting_semaphore cost 3.4-4.0 us per
	// release+acquire pair in CORE time against 34 ns uncontended, and the
	// mirror-shard bake was paying 96 tiles x 238 shards = 22 848 of them per
	// shatter frame. Pixel values do not depend on this flag.
	bool                 inlineDispatch = false;
	// HDR radiance target for THIS pass (B,G,R,coverage ×4 per pixel, xres×yres),
	// or nullptr when this pass writes no HDR. Replaces the kernels' old
	// `Hdr_WritableFor(ctx.xres, ctx.yres)` test, which used "g_hdrBuf happens to
	// be sized like me" as a de-facto "am I the main pass?" — so any offscreen
	// bake at another resolution silently fell through to the LDR combine and
	// ended up on a DIFFERENT TRANSFER FUNCTION from the frame it feeds. Main
	// frame: g_hdrBuf.data() under exactly the old predicate (byte-identical).
	// Offscreen: DeferredOverride::hdr, which for the shard bake is the calling
	// worker's OWN buffer — the bakes run N-concurrent, so this cannot be a
	// global the way the serial mirror RTT's Hdr_BeginFramePass is.
	fds::hdrf           *hdrBuf = nullptr;
	// TRUE only when this pass is guaranteed to end in Render_TonemapToVPage,
	// i.e. every 8-bit VPage byte the kernel writes will be overwritten from
	// hdrBuf before anybody looks at it (docs/GRAPHICS_PIPELINE.md's "#1 HDR
	// gotcha", stated as a promise instead of an assumption). Read by
	// --deferred_fill_ldr_skip. `hdrBuf != nullptr` is NOT the same predicate
	// and using it would be a bug: renderFrame's tonemap sits inside
	// `if (!skipVolumetric)`, and CITY.CPP's water-reflection underlay renders
	// with skipVolumetric=true into the MAIN VPage at the MAIN resolution —
	// so hdrBuf is live, no tonemap runs, and that pass's VPage IS its product
	// (CITY.CPP reads it straight back to displace the reflection). Also false
	// for every offscreen/override bake: the shard bake and the greets mirror
	// RTT do tonemap inline, but GreetsMirror.cpp's own comment claims wave-2
	// fill pixels reach g_hdrBuf only via the VPage lift, and this flag is not
	// the place to adjudicate that.
	bool                 ldrDiscarded = false;
};


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
    // Per-target HDR radiance buffer (xres*yres*4 hdrf), or null for an LDR
    // bake. Per WORKER, not per scene: N shard bakes run concurrently, which is
    // why the mirror RTT's serial Hdr_BeginFramePass(w,h) shape does not port.
    fds::hdrf            *hdr        = nullptr;
};

// Deferred opaque lighting pass. ov=nullptr → main frame (engine globals,
// pool-tiled). ov!=nullptr → offscreen bake into ov's target (see above).
void Render_DeferredLighting(DeferredLightingCtx &ctx, const DeferredOverride *ov = nullptr);

// TRUE when this frame's opaque lighting kernel writes linear radiance into the
// HDR buffer (scalar wave-1 kernel + its checkerboard fill). FALSE on a
// PreferOuterVec scene, where the outer-vec kernel writes 8-bit VPage only and
// leaves the coverage lane 0 so a later lift (froxel composite / Hdr_ActivateNoFog)
// seeds g_hdrBuf FROM VPage. `Hdr_WritableFor()` answers "is the buffer sized for
// me", which is NOT the same question — see the definition's comment.
bool Deferred_KernelWritesHdrRadiance();

// Volumetric spotlight cones (disco beams etc.). Reads its render target from
// ctx (xres/vpage/zpage16/invFOVX…), so after a per-worker deferred bake the
// shard reflection can run it to draw the beams. inlineDispatch=true runs the
// tiles on the calling thread (offscreen bake); false pool-tiles (main frame).
void Render_VolumetricCones(const DeferredLightingCtx &ctx, bool inlineDispatch = false);

// Screen-space bounding rect of a view-space sphere — the shared first
// step of every light-list / volumetric tile binning (was copied in 4
// places; a cull added to one copy rotted in the others). Returns:
//   false        → sphere entirely behind the camera; skip the light.
//   true, full=1 → sphere straddles the near plane; bin conservatively
//                  into every tile (the rect fields are not written).
//   true, full=0 → rect is the clamped pixel bbox; callers reject the
//                  light when the rect is empty ONLY if they cull that
//                  axis (the strip builder ignores X, so an off-screen-
//                  in-X light must still reach its Y strips).
struct LightScreenRect {
	int  x0, x1, y0, y1;   // inclusive pixel bounds, clamped to screen
	bool full;             // near-plane straddle → tag every tile
};
inline bool lightSphereScreenRect(float vx, float vy, float vz, float r,
                                  float fovX, float fovY,
                                  float cntrEX, float cntrEY,
                                  int xres, int yres,
                                  LightScreenRect &out)
{
	if (vz + r < 0.0f) return false;      // entirely behind camera
	if (vz - r < 1.0f) { out.full = true; return true; }
	out.full = false;
	// Pinhole projection of the bounding sphere — small-angle
	// approximation. Slightly over-estimates near the FOV edges, which
	// just bins tiles that the per-pixel cull rejects; no correctness
	// impact.
	const float invZ = 1.0f / vz;
	const float cx   = cntrEX + vx * fovX * invZ;
	const float cy   = cntrEY - vy * fovY * invZ;
	const float rx   = r * fovX * invZ;
	const float ry   = r * fovY * invZ;
	out.x0 = std::max(0,        int(std::floor(cx - rx)));
	out.x1 = std::min(xres - 1, int(std::ceil (cx + rx)));
	out.y0 = std::max(0,        int(std::floor(cy - ry)));
	out.y1 = std::min(yres - 1, int(std::ceil (cy + ry)));
	return true;
}

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

// ─── the Newton-Raphson refinement step at ONE instruction ──────────────────
// arm64 has dedicated fused step instructions for exactly these two
// recurrences, and the long-hand spelling below was costing three to five
// vector-ALU ops for one of them.  Round 4 measured the cone pass at ~81% of
// this core's 4-wide NEON ALU issue ceiling, which makes VECTOR-ALU OP COUNT
// the metric that moves cycles (docs/HW_PROFILING.md section 12), so the same
// 2-ops-to-1 argument that shipped FDS_CONE_NEONMINMAX applies here:
//
//   FRECPS  Vd, Vn, Vm  =  2.0 - Vn*Vm          (fused, one rounding)
//   FRSQRTS Vd, Vn, Vm  = (3.0 - Vn*Vm) / 2     (fused, one rounding)
//
// The rcp step is `fnmadd(x, r, 2)` — literally FRECPS.  The rsqrt step is
// `fnmadd(0.5*x, r*r, 1.5)` = (3 - x*r*r)/2 — literally FRSQRTS, one op in
// place of a constant materialisation, a broadcast copy, a multiply and an
// fmls.  Both were checked against the long-hand spelling over 61.4 M inputs
// (every 37th representable positive float plus 4 M log-uniform draws): ZERO
// bit differences for x > 2.4e-38.  Below that the long-hand form loses the
// exponent — `0.5*x` goes subnormal and rounds — and the native step is the
// MORE accurate of the two; the cone kernel's arguments (discriminants of
// O(1e-9..1e4), W² floored at 1e-12, interval lengths) never reach there.
// Build with -DFDS_CONE_NEONSTEP=0 for the long-hand spelling.
#ifndef FDS_CONE_NEONSTEP
#define FDS_CONE_NEONSTEP 1
#endif

#if FDS_CONE_NEONSTEP && (defined(__ARM_NEON) || defined(__aarch64__))
// r * (2 - x*r) — one FRECPS per 128-bit half.
static inline __m256 rcp_step_x8(__m256 x, __m256 r) {
    simde__m256_private xp = simde__m256_to_private(x),
                        rp = simde__m256_to_private(r), o;
    for (int h = 0; h < 2; ++h)
        o.m128_private[h].neon_f32 = vmulq_f32(rp.m128_private[h].neon_f32,
            vrecpsq_f32(xp.m128_private[h].neon_f32, rp.m128_private[h].neon_f32));
    return simde__m256_from_private(o);
}
// r * (3 - x*r*r)/2 — one FRSQRTS per 128-bit half (plus the r*r it needs).
static inline __m256 rsqrt_step_x8(__m256 x, __m256 r) {
    simde__m256_private xp = simde__m256_to_private(x),
                        rp = simde__m256_to_private(r), o;
    for (int h = 0; h < 2; ++h) {
        const float32x4_t rr = vmulq_f32(rp.m128_private[h].neon_f32,
                                         rp.m128_private[h].neon_f32);
        o.m128_private[h].neon_f32 = vmulq_f32(rp.m128_private[h].neon_f32,
            vrsqrtsq_f32(xp.m128_private[h].neon_f32, rr));
    }
    return simde__m256_from_private(o);
}
#else
static inline __m256 rcp_step_x8(__m256 x, __m256 r) {
    return _mm256_mul_ps(r, _mm256_fnmadd_ps(x, r, _mm256_set1_ps(2.0f)));
}
static inline __m256 rsqrt_step_x8(__m256 x, __m256 r) {
    return _mm256_mul_ps(r, _mm256_fnmadd_ps(
        _mm256_mul_ps(_mm256_set1_ps(0.5f), x),
        _mm256_mul_ps(r, r), _mm256_set1_ps(1.5f)));
}
#endif

// rsqrt + one Newton-Raphson step (~24-bit). The cone passes feed
// cosT = D·W·rsqrt(W²) into smoothstep((cosT−cosO)/(cosI−cosO)):
// for NARROW cones the 1/(cosI−cosO) gain is ~350 (1.5°/4.5°), which
// amplifies the raw 12-bit rsqrt's quantization staircase into ±10%
// attenuation noise — the beam 'fur'/fan-stripe moire family. Wide
// city cones (gain 2-10) never showed it.
static inline __m256 rsqrt_nr_x8(__m256 x) {
    return rsqrt_step_x8(x, _mm256_rsqrt_ps(x));
}

// ─── movemask on arm64: the systemic simde lowering defect ──────────────────
//
// `_mm256_movemask_ps` / `_mm256_movemask_epi8` are ONE instruction on x86.
// arm64 has no equivalent, so simde builds the sign-bit word by hand, and the
// shipping binary pays ~25 instructions per site: ext.16b, two adrp +
// two constant-table ldr q, ushl.4s, and.16b, ext.16b, orr.8b, then FIVE
// vector->GPR moves and nine scalar lsr/orr to pack the bits. The v->GPR moves
// are ~6-10 cycle latency each and typically sit between a SIMD dependency
// chain and the branch that consumes them, so it costs LATENCY as well as
// issue slots. `docs/HW_PROFILING.md:1128-1152`'s simde audit (round 7, B12)
// checked blendv, the unordered predicates, mask chains, set1, cmp-vs-zero,
// andnot and faddp -- movemask was never in that list.
//
// The overwhelming majority of uses never want the BITS; they want one of two
// predicates. Both have a 3-instruction NEON form, and both are EXACT for
// arbitrary inputs (not merely for the all-ones/all-zeros masks a compare
// produces), so substituting them is control flow only -- BIT-EXACT.
//
//   movemask(m) != 0   <=>  some lane's sign bit is set
//   movemask(m) == -1  <=>  every lane's sign bit is set
//
// WHERE IT PAYS, AND WHERE IT MEASURABLY DOES NOT -- read this before citing
// the helpers as a perf win (city t=1961, his arm, interleaved min-of-11, two
// batteries, quiet box; full account docs/OPTIMIZATION_BACKLOG.md 2026-08-29):
//
//   * volumetric cone kernel (2 sites)  Ginstr -4.4/-4.5%   WALL -5.1/-5.2%
//   * DeferredSurfaceKernel   (5 sites) Ginstr -1.9/-2.0%   Gcyc +0.7/+1.4%
//   * DeferredFastFog         (3 sites) Ginstr -0.3/-0.4%   Gcyc  no effect
//
// Only the cone sites convert to time. `lighting-w1` reproducibly loses ~2% of
// its instructions and pays the SAME cycles -- its IPC goes 4.08 -> 3.94, i.e.
// the slots those 12-instruction sequences occupied were not the constraint,
// so removing them merely exposes the latency chain underneath. That is
// `docs/HW_PROFILING.md:1011-1014` ("everything that reduces instructions on
// the OTHER pipes will keep measuring as zero") landing for the second time.
// The fog sites are simply cold -- they are per-column-block early-outs, not
// per-lane ones. Both are kept anyway: they are bit-exact, they cost nothing,
// and one spelling engine-wide beats two. But they are NOT a time win and
// nothing downstream should be sized as if they were.
//
// NAMING: `Lane` = 32-bit element (movemask_ps), `Byte` = 8-bit element
// (movemask_epi8). They are NOT interchangeable -- movemask_epi8 tests every
// byte, so a 32-bit mask of 0x00000080 makes it nonzero while movemask_ps
// leaves it clear.
// -DFDS_SIMD_ANYLANE=0 rebuilds the exact pre-sweep arm (every site back on
// _mm256_movemask_*) in the SAME worktree, which is the A/B these landed on.
#ifndef FDS_SIMD_ANYLANE
#define FDS_SIMD_ANYLANE 1
#endif
#if FDS_SIMD_ANYLANE && (defined(__ARM_NEON) || defined(__aarch64__))
// == `_mm256_movemask_ps(m) != 0`. The bitwise OR preserves each lane
// position's sign bit; a signed horizontal min is negative iff some lane is.
static inline bool simdAnyLane_ps8(const __m256 &m) {
    simde__m256_private p = simde__m256_to_private(m);
    const int32x4_t o = vorrq_s32(p.m128_private[0].neon_i32,
                                  p.m128_private[1].neon_i32);
    return vminvq_s32(o) < 0;
}
// == `_mm256_movemask_epi8(v) != 0`. Bytewise OR then an unsigned horizontal
// max: >= 0x80 iff some byte has its top bit set.
static inline bool simdAnyByte_epi8(const __m256i &v) {
    simde__m256i_private p = simde__m256i_to_private(v);
    const uint8x16_t o = vorrq_u8(p.m128i_private[0].neon_u8,
                                  p.m128i_private[1].neon_u8);
    return vmaxvq_u8(o) >= 0x80;
}
// == `_mm256_movemask_epi8(v) == -1`. Bytewise AND then an unsigned
// horizontal min: >= 0x80 iff EVERY byte has its top bit set.
static inline bool simdAllBytes_epi8(const __m256i &v) {
    simde__m256i_private p = simde__m256i_to_private(v);
    const uint8x16_t a = vandq_u8(p.m128i_private[0].neon_u8,
                                  p.m128i_private[1].neon_u8);
    return vminvq_u8(a) >= 0x80;
}
#else
static inline bool simdAnyLane_ps8(const __m256 &m)  { return _mm256_movemask_ps(m)   != 0;  }
static inline bool simdAnyByte_epi8(const __m256i &v){ return _mm256_movemask_epi8(v) != 0;  }
static inline bool simdAllBytes_epi8(const __m256i &v){return _mm256_movemask_epi8(v) == -1; }
#endif

#endif // FDS_RENDER_DEFERRED_COMMON_H_INCLUDED
