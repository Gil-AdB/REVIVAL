#pragma once

#include "Base/Vector.h"
#include "Base/Matrix.h"
#include "Base/Camera.h"
#include "Base/Face.h"
#include "Base/FeatureFlags.h"
#include <atomic>
#include <vector>
#include <cstdint>

// Per-shadow-casting-light occluder buffer + the light-view transform used
// to compute it. The lighting kernel reconstructs each pixel's
// world-space position from the deferred G-buffer, transforms it into
// this light's view space using `lightViewMat` + `lightISource`, projects
// using `perspX`/`perspY`, samples the plane, and compares against the
// pixel's view-space z. If pixel.z > sampled.z + bias, the pixel is in
// shadow for this light.
//
// The map's resolution is independent of the framebuffer (typically
// 512×512). Encoded Z matches Mekalele's: `enc = 0xFF80 - round(z * zScale)`
// where `zScale = 0xFF00 / (fzp * 1.1f)`. Higher enc = closer.
//
// ─── One shadow texel = ONE 32-bit word ──────────────────────────────────
// Low half  = encoded Z of the closest occluder (`enc = 0xFF80 - z*zScale`,
//             higher = closer to the light; 0 = unwritten).
// High half = that occluder's 16-bit ShadowMatID (0 = "no occluder"; the
//             engine matID is stored +1 so 0 stays a free sentinel).
//
// WHY THEY ARE ONE WORD AND NOT TWO ARRAYS. A PolyId cube tap needs a
// texel's id AND its z, for the static pair and the dynamic pair. As four
// separate std::vector<uint16_t> those sit ~512 KB apart at greets' 512²,
// so 32 bytes of useful data was gathered from 4 base pointers over 2 PCF
// rows = up to 8 cache lines (~512 B of line traffic) per tap. Interleaved,
// a texel's id+z is one 32-bit load: 8 lines → 4, and the static-lightmap
// composite path's dynamic-only tap 4 → 2. Total bytes resident are
// unchanged (4 × u16 either way); only the grouping changed. Measured in
// docs/OPTIMIZATION_BACKLOG.md.
// ─── 8×8 PolyId uniformity pyramid ───────────────────────────────────────
// Block edge is a compile-time 8 (kShadowUniShift = 3): the census in
// 43ac3456 swept the coherence of the tap verdict against block size and 8×8
// was where the uniform share stopped growing fast enough to pay for a finer
// pyramid. kShadowUniMixed cannot collide with a real ShadowMatID because
// those are 16-bit and this sentinel sets the high half.
inline constexpr int      kShadowUniShift = 3;
inline constexpr int      kShadowUniSize  = 1 << kShadowUniShift;
inline constexpr uint32_t kShadowUniMixed = 0xFFFFFFFFu;

// Rebuild one plane's uniformity pyramid from the linear plane. dynPlane
// selects packDyn (true) or packSD (false). Cheap enough to run inside the
// bake's own parallel-over-maps dispatch — one sequential pass over a plane
// that the raster just left hot.
void ShadowMap_BuildUniformity(struct ShadowMap &sm, bool dynPlane);

// Per-thread accumulator for the dirty box above: MekaleleShadowDepth ORs each
// clipped polygon's clamped screen bbox in, the phase-B tile task resets it
// before its face loop and reads it after. Thread-local rather than atomic
// because the raster runs thousands of polygons per tile and four CAS loops
// each would price the bake instead of the build. [0..3] = x0, y0, x1, y1.
extern thread_local int g_shadowRasterBox[4];
inline void ShadowRasterBoxReset() {
	g_shadowRasterBox[0] = g_shadowRasterBox[1] = 0x7FFFFFFF;
	g_shadowRasterBox[2] = g_shadowRasterBox[3] = -1;
}

#if FDS_SHADOW_TAP_CENSUS
// [INSTRUMENT, compiled out unless -DFDS_SHADOW_TAP_CENSUS=ON] Where every
// PolyId cube tap went. Per-thread counters (no atomics in the tap — a shared
// cache line under 12 workers would price the thing it is measuring); summed
// on demand by ShadowPyrCensusTotals from a process-wide registry, and printed
// as extra rows of the existing --shadow_tap_census report so the two share
// one frame accounting.
struct ShadowPyrCensus {
    uint64_t reached = 0;   // PolyId taps that got as far as the pyramid check
    uint64_t noPyr   = 0;   // ... of those, map had no pyramid built
    uint64_t fastLit = 0;   // ... resolved uniform-LIT, no texel read
    uint64_t fastOcc = 0;   // ... resolved uniform-OCCLUDING, no texel read
    uint64_t mixed   = 0;   // ... fell through to the real 2x2 tap
    uint64_t dynOnly = 0;   // of `reached`, the lightmap-composite (dyn-only) form
};
ShadowPyrCensus &ShadowPyrCensusTLS();
void ShadowPyrCensusTotals(ShadowPyrCensus &out);
#define FDS_PYR_CENSUS(field) (++ShadowPyrCensusTLS().field)
#else
#define FDS_PYR_CENSUS(field) ((void)0)
#endif

inline constexpr uint16_t ShadowTexZ (uint32_t t) { return uint16_t(t & 0xFFFFu); }
inline constexpr uint16_t ShadowTexId(uint32_t t) { return uint16_t(t >> 16); }
inline constexpr uint32_t ShadowTexPack(uint16_t z, uint16_t id) {
	return uint32_t(z) | (uint32_t(id) << 16);
}

struct ShadowMap {
	// STATIC plane: one packed (z | id<<16) word per texel, row-major,
	// size xres*yres. Written by the once-baked static pass (StaticOnce)
	// and by the legacy per-frame dynamic-omni pass (DynamicOmnisPerFrame).
	// The z half is always written (it is a real z-buffer regardless of
	// render mode); the id half is written under the same z-pass mask, so
	// the closest occluder's ShadowMatID wins. The id is a 16-bit
	// ShadowMatID (widened from the old 8-bit matID so per-Material
	// overrides — e.g. the greets wall split — can exceed 255); it
	// defaults to `Txtr->ID + 1`. Memory: 4 bytes/texel × 6 cube faces ×
	// N omnis, e.g. 10 omnis × 6 × 512² = 60 MB for both planes together —
	// exactly what the four u16 planes cost.
	std::vector<uint32_t> packSD;

	// DYNAMIC plane, same encoding, populated per-frame by the
	// dynamic-objects bake (BakeDynamicForStaticOmnis). Only animated
	// meshes are rendered here. Static-omni lighting takes the
	// closest-by-z of the two planes so moving objects cast moving
	// shadows even though the static bake only fires once. Allocated
	// alongside packSD by ShadowMaps_Rebuild / CubeShadowMaps_Rebuild —
	// always the same size, so `packDyn.empty() == packSD.empty()`.
	std::vector<uint32_t> packDyn;
	// True iff the DynamicMeshesPerFrame bake processed this map THIS frame
	// (a dynamic mesh was visible in the face pyramid). When false the
	// dynamic planes are stale/empty and the dynamicOnly tap short-circuits
	// to lit — which both skips the tap cost for the (common) faces with no
	// moving mesh AND stops a stale last-visible-frame silhouette from
	// shadowing after the mesh leaves the face.
	bool dynBaked = false;

	// [experiment: --shadow-swizzle] 8×8-tiled copies of the two planes.
	// Rebuilt from the linear planes after each bake (ShadowMap_SwizzlePlanes,
	// timed separately so the swizzle overhead is measurable on its own); the
	// linear planes stay the source of truth for every other reader (viz,
	// overlay, lightmap bake, clone taps). Sized tilesPerRow*tileRows*tileSz
	// with zero-filled edge padding (reads there = "unwritten" sentinel).
	// Empty until first swizzle → hot readers fall back to linear when empty.
	std::vector<uint32_t> packSDSw, packDynSw;

	// ─── 8×8 PolyId UNIFORMITY PYRAMID (uniSD / uniDyn) ──────────────────
	// One u32 per 8×8 block of the plane. Value = the single ShadowMatID that
	// EVERY texel of that block's 9×9 APRON carries, or kShadowUniMixed when
	// the apron is not uniform. Built from the linear planes right after each
	// bake writes them (ShadowMap_BuildUniformity), so it is a pure function
	// of the plane contents and never goes stale: the only writers of packSD /
	// packDyn are the per-map clear + raster inside Render_DeferredShadowMaps
	// and the assign() below, and each rebuilds the matching pyramid.
	//
	// WHY A 9×9 APRON AND NOT AN 8×8 BLOCK. A 2×2 PCF footprint anchored at
	// (iX, iY) reads texels (iX..iX+1, iY..iY+1). Summarising the bare 8×8
	// block would leave every footprint anchored on the block's last row or
	// column straddling two blocks — 23 % of anchor positions, and exactly
	// the ones in the interior of a large uniform region, where the fast path
	// is worth the most. Extending the summary one texel past the right and
	// bottom edges makes the block index (iX>>3, iY>>3) sufficient on its own:
	// every footprint anchored inside the block lies inside the apron. No
	// boundary branch at the tap, no straddle case to get wrong. (At the map's
	// right/bottom edge the apron is clamped to xres-1 / yres-1; the tap's own
	// `iX + 1 >= xres` reject already guarantees no footprint reads past it.)
	//
	// WHY IT IS BYTE-NULL. In ShadowMode::PolyId a texel's verdict is the pure
	// id comparison `id != 0 && id != receiverId`. If all four footprint texels
	// carry the same id c, the 2×2 PCF sum is either 0 (c lit) or w00+w10+w01+
	// w11 (c occluding) — the SAME float expression the tap would evaluate, in
	// the same order, so the fast path returns the identical bit pattern. See
	// CubeShadow_Sample below.
	std::vector<uint32_t> uniSD, uniDyn;
	int    uniW = 0;                  // blocks per row  = (xres + 7) >> 3
	int    uniH = 0;                  // block rows      = (yres + 7) >> 3

	// Texel-space bbox of everything the LAST bake rasterised into this map,
	// clamped to the plane; x1 < x0 means "the bake wrote nothing". Set on the
	// tick thread after the raster drain from per-tile-job boxes, so it is
	// order-independent and identical run to run.
	//
	// WHY IT EXISTS. Phase A clears the whole plane, so every texel OUTSIDE
	// this box is known-zero and its pyramid entry is known-zero too — no read
	// required. Without it the per-frame dynamic build has to stream all 14 MB
	// of greets' fourteen 512^2 dynamic planes to rediscover that the mech is
	// the only thing in them, and MEASURED that cost 0.375 ms/frame at the
	// user's pose against a 0.336 ms tap saving: the build ate the win whole.
	int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = -1, dirtyY1 = -1;

	int    xres = 0;
	int    yres = 0;

	// Camera built from the omni. lightViewMat is the 3x3 world→view
	// rotation; lightISource is the world-space light position.
	Matrix lightViewMat;
	Vector lightISource;
	float  perspX = 0.0f;             // = (xres/2) / tan(fovX/2)
	float  perspY = 0.0f;             // similarly
	float  cntrX  = 0.0f;             // map center
	float  cntrY  = 0.0f;
	float  fzp    = 0.0f;             // light range = far plane for the map
	float  rFZP   = 0.0f;
	float  zScale = 0.0f;             // matches g_zscale's formula

	// Source omni — used by the lighting kernel to find this shadow map
	// when iterating its per-pixel light list.
	struct Omni *omni = nullptr;

	// -1 = spot/standalone 2D shadow map; 0-5 = one face of a cube
	// shadow (in CubeShadowRef::faceIdx order +X,-X,+Y,-Y,+Z,-Z).
	// Tells Render_DeferredShadowMaps how to orient the camera for
	// this entry (standalone uses the omni's IDir; cube face uses
	// a fixed ±axis basis).
	int8_t cubeFace = -1;

	// Precomputed per-frame "view-space → light-view-space" affine.
	// Fills the role of:
	//     lightPos = lightViewMat * (mainView^T * pixelViewSpace
	//                                + mainISource - lightISource)
	// after main View is restored. The lighting kernel then just does
	//     lightPos = viewToLight * pixelViewSpace + viewToLightOffset
	// per pixel — one 3×3 matmul + 1 vector add, no per-pixel transpose
	// or world-space round-trip.
	Matrix viewToLight;
	Vector viewToLightOffset;
};

// Depth-only rasterizer. Same RasterFunc signature as Mekalele, but
// reads the active shadow map from the thread-local `g_currentShadowMap`
// pointer set by the orchestrator just before invoking the clipper.
// Writes only the packed (z | id<<16) word to `g_currentShadowMap->packSD`
// (or ->packDyn on the dynamic bake); no color, no G-buffer, no texture.
void MekaleleShadowDepth(Face* F, struct Vertex** V, unsigned int numVerts, unsigned int miplevel,
                          const fds::RenderTarget& rt,
                          const fds::CameraContext& cam);

// Per-thread pointer to the shadow map currently being rasterized.
// Set by the orchestrator around clipper.Render() calls.
extern thread_local ShadowMap *g_currentShadowMap;

// Global container of shadow maps for the current scene, one per
// shadow-casting light. Index is stable within a frame; the lighting
// kernel matches by `ShadowMap::omni == Omni*`.
extern std::vector<ShadowMap> g_shadowMaps;

// Debug overlay state. SDL2 V_Flip calls ShadowMap_Overlay(VS) at end
// of frame, which paints the indexed shadow map as a thumbnail in the
// top-left of the framebuffer when index >= 0. -1 = hidden. REV.CPP's
// SDL_KEYDOWN handler cycles the index on each 'V' press: -1 -> 0 ->
// 1 -> ... -> N-1 -> -1.
extern int g_shadowViewIdx;
// Set by scene tick when the user has the full-screen shadow viz on
// (greets M key). When true, ShadowMap_Overlay skips the thumbnail —
// the full-screen viz already shows the map, and V just cycles which
// map is displayed instead of stacking a thumbnail on top.
extern std::atomic<bool> g_shadowFullscreenView;
// pitchBytes is the surface's BPSL (may exceed xres*4 on locked SDL
// textures with alignment padding — caller passes VS->BPSL).
void ShadowMap_Overlay(byte *vpage, int xres, int yres, int pitchBytes);
void ShadowMap_ViewCycle();  // advances g_shadowViewIdx, prints status

// Resize / re-build the shadow-map collection. Called at scene init
// (after omnis are flagged with Omni_CastsShadow) and on engine
// resize. `res` is the square edge length (e.g. 512).
void ShadowMaps_Rebuild(struct Scene *Sc, int res);

// One-shot shadow-map bake for Omni_StaticShadow lights. Renders
// each static shadow map exactly once into g_shadowMaps. Called
// from scene init (after ShadowMaps_Rebuild + CubeShadowMaps_Rebuild)
// so the per-frame Render_DeferredShadowMaps can skip these lights
// entirely. Hides in the existing scene-init bake window (city's
// Glato cube-map bake, etc.) so it doesn't delay the demo start.
//
// Dynamic lights (no Omni_StaticShadow flag) are unaffected — they
// continue to rebake every frame via Render_DeferredShadowMaps.
//
// forceEnable (default false): bypass the global FeatureFlags::shadows()
// gate for this bake. Needed by greets, which turns --shadows on only at
// RUN time (after this init bake) but force-bakes its static-shadow
// lightmap here — the lightmap reads these occluder maps, so they must be
// populated even when shadows() is still off at init.
void ShadowMaps_BakeStatic(struct Scene *Sc, bool forceEnable = false);

// ─── Cube shadow maps (for Light_Omni shadow casters) ────────────────
//
// An omnidirectional light radiates in all directions, so a single
// view-frustum shadow map can't cover it. Implementation: 6 ordinary
// ShadowMap entries (one per ±X/±Y/±Z face) in the existing
// g_shadowMaps vector, plus a CubeShadowRef that groups them. The
// shadow render pass treats each face as a 90°-FOV "spotlight" and
// rasterizes it with the existing infrastructure.
//
// Face order (matches sampling axis-select code):
//   0 = +X    1 = -X    2 = +Y    3 = -Y    4 = +Z    5 = -Z
//
// Memory: 6 × res² × 4 B per plane (static + dynamic) per shadow-casting
// omni. At 256² per face: ~3MB/omni. Fine for handful of static omnis;
// don't enable for many dynamic omnis (fountain particles).
//
// Per-pixel sampling (in deferred kernel / volumetric pass): take
// world-space point P, compute D = P - lightISource, find dominant
// axis (max |D.x|, |D.y|, |D.z|), index into the cube ref to get
// the right face's ShadowMap, then use the existing 2D shadow sample
// code on that face.
struct CubeShadowRef {
    // Indices into g_shadowMaps of the 6 face maps (in face-order above).
    int32_t faceIdx[6];
    Vector  lightISource;     // omni world position, shared by all faces
    struct Omni *omni = nullptr;
};

// Parallel to g_shadowMaps but grouped by omni. The 6 entries
// `g_shadowMaps[faceIdx[k]]` belong to this cube.
extern std::vector<CubeShadowRef> g_cubeShadowRefs;

// Append 6 ShadowMap entries to g_shadowMaps and a CubeShadowRef to
// g_cubeShadowRefs for any omni whose flags include Omni_CastsShadow
// AND whose Type is Light_Omni. Called by ShadowMaps_Rebuild after
// handling spotlights. `res` is per-face edge length.
void CubeShadowMaps_Rebuild(struct Scene *Sc, int res);

// [experiment: --shadow-swizzle] tile-swizzled texel offset. Tile shape is a
// power-of-two W×H selected once via FDS_SHADOW_SWZ_SHAPE="WxH" (default 8x8)
// so the layout family can be swept without rebuilding:
//   8x8 = 128 B/tile (2 lines), rows 16 B apart inside the tile;
//   4x4 =  32 B/tile (two tiles per 64 B line);
//   2x2 =   8 B/tile — an ALIGNED 2×2 PCF quad is exactly one tile;
//   4x2/2x4 = 16 B rectangles biased to x/y-straddling footprints.
// A 2×2 PCF footprint that stays inside one tile lands in ONE cache line vs
// the linear layout's ≥2 (one per row, xres*2 B apart). The experiment weighs
// the extra per-tap int ops + the bake-side swizzle cost against the line-
// traffic saving (see docs/SHADOWMAP_TILING_PLAN.md for measured results).
struct ShadowSwzShape { int a, b, maskX, maskY; };   // tile = 2^a × 2^b texels
inline const ShadowSwzShape& ShadowSwzGetShape()
{
    static const ShadowSwzShape s = [] {
        int w = 8, h = 8;
        if (const char *e = std::getenv("FDS_SHADOW_SWZ_SHAPE"))
            std::sscanf(e, "%dx%d", &w, &h);
        auto lg = [](int v) { int l = 0; while ((1 << l) < v) ++l; return l; };
        const int a = lg(std::min(std::max(w, 2), 64));
        const int b = lg(std::min(std::max(h, 2), 64));
        std::fprintf(stderr, "[SHADOW-SWZ] tile shape %dx%d\n", 1 << a, 1 << b);
        return ShadowSwzShape{a, b, (1 << a) - 1, (1 << b) - 1};
    }();
    return s;
}
inline int ShadowSwzTilesPerRow(int xres, const ShadowSwzShape &s)
{
    return (xres + s.maskX) >> s.a;
}
inline size_t ShadowSwzOffset(int x, int y, int tilesPerRow, const ShadowSwzShape &s)
{
    return (size_t(size_t(y >> s.b) * size_t(tilesPerRow) + size_t(x >> s.a)) << (s.a + s.b))
         + (size_t(y & s.maskY) << s.a) + size_t(x & s.maskX);
}

// Cube face selection: direction → face index in CubeShadowRef::faceIdx.
// Standard "find dominant axis" — ~6 ops, no branches we care about
// (CMOV-friendly). Returns 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
inline int CubeShadow_SelectFace(float dx, float dy, float dz)
{
    const float ax = dx < 0 ? -dx : dx;
    const float ay = dy < 0 ? -dy : dy;
    const float az = dz < 0 ? -dz : dz;
    if (ax >= ay && ax >= az) return dx >= 0 ? 0 : 1;
    if (ay >= az)              return dy >= 0 ? 2 : 3;
    return                            dz >= 0 ? 4 : 5;
}

// Sample a cube shadow at the given view-space sample point. Returns
// shadow attenuation in [0, 1] — 1.0 fully lit, 0.0 fully shadowed.
//
// `surfaceMatId` ∈ [-1, 65535] — the receiver's resolved 16-bit
// ShadowMatID for direct comparison against the texel's id half (NO +1
// offset added inside; callers pass the literal value the bake stamped).
//   -1 = legacy Depth mode: compare biased pixel-z against the z half.
//        constBias + slopeBiasInt control the comparison bias.
//   0..65535 = PolyId mode: identity test of ShadowTexId(texel) against
//        surfaceMatId. Caller is responsible for resolving the receiver's
//        ShadowMatID first (Material::ShadowMatID if non-zero, else
//        fallback uint16_t(matID + 1)). Bias parameters ignored.
//
// `dynamicOnly`: when true, only the dynamic plane (sm.packDyn) is
//   sampled — the static plane is ignored. Used by the static-lightmap
//   composite path: the lightmap atlas already encodes the static-cube
//   shadow factor for the pixel, so adding the static plane here would
//   double-count. dynamicOnly layers per-frame dynamic-mesh shadows on
//   top of the static atlas — and, because id and z now travel together,
//   it reads HALF the cache lines the full tap does (2 vs 4).
//
// Caller computes the world-space sample pos once per pixel.
inline float CubeShadow_Sample(int cubeIdx,
                                float worldX, float worldY, float worldZ,
                                float viewX,  float viewY,  float viewZ,
                                int   constBias, int slopeBiasInt,
                                int   surfaceMatId = -1,
                                bool  dynamicOnly  = false)
{
    if (cubeIdx < 0 || size_t(cubeIdx) >= g_cubeShadowRefs.size()) return 1.0f;
    const CubeShadowRef& cr = g_cubeShadowRefs[cubeIdx];
    const float dwx = worldX - cr.lightISource.x;
    const float dwy = worldY - cr.lightISource.y;
    const float dwz = worldZ - cr.lightISource.z;
    const int face = CubeShadow_SelectFace(dwx, dwy, dwz);
    const ShadowMap& sm = g_shadowMaps[cr.faceIdx[face]];
    // dynamicOnly composite tap: nothing was baked into this face's dynamic
    // planes this frame → fully lit, skip the projection + PCF entirely.
    if (dynamicOnly && !sm.dynBaked) return 1.0f;
    // Project view-space sample point into face-view space.
    const float lx = sm.viewToLight[0][0] * viewX + sm.viewToLight[0][1] * viewY +
                     sm.viewToLight[0][2] * viewZ + sm.viewToLightOffset.x;
    const float ly = sm.viewToLight[1][0] * viewX + sm.viewToLight[1][1] * viewY +
                     sm.viewToLight[1][2] * viewZ + sm.viewToLightOffset.y;
    const float lz = sm.viewToLight[2][0] * viewX + sm.viewToLight[2][1] * viewY +
                     sm.viewToLight[2][2] * viewZ + sm.viewToLightOffset.z;
    // lz is the pixel's depth into light-space. lz<=0 = behind light;
    // lz~0 = at-the-light. Both produce a meaningless shadow lookup
    // and the latter explodes invLZ = 1/lz, making smX/smY saturate to
    // INT_MAX and walk off the shadow map. Reject anything closer than
    // 0.05 world units (matches the engine-wide zMin / near-plane).
    if (lz <= 0.05f) return 1.0f;
    // Cube-face frustum check. For a 90°-padded face, valid pixels
    // have |lx|/lz and |ly|/lz ≤ ~1.18 (tan(π/4 · 1.10)). Pixels at
    // the face seam where another face's frustum was the correct one
    // can leak in (SelectFace uses dominant-axis on dwx/dwy/dwz, but
    // post-projection the ratio can exceed 1 in either direction).
    // Reject them — they project off the map anyway, and the correct
    // shadow contribution should come from the adjacent face.
    constexpr float kFaceFrustumRatio = 1.5f;  // a bit of slack past 1.18
    if (lx >  kFaceFrustumRatio * lz || lx < -kFaceFrustumRatio * lz) return 1.0f;
    if (ly >  kFaceFrustumRatio * lz || ly < -kFaceFrustumRatio * lz) return 1.0f;
    const float invLZ = 1.0f / lz;
    const float smX = sm.cntrX + sm.perspX * lx * invLZ;
    const float smY = sm.cntrY - sm.perspY * ly * invLZ;
    // Diagnostic: with the lz>0.05 + face-frustum-ratio rejections
    // above, smX/smY should now always land in roughly [0, xres) /
    // [0, yres). NaN/inf or wildly out-of-range here means the
    // matrix itself is broken (e.g. a per-frame bake desynced).
    // Threshold deliberately loose (16× face size) to ignore face-
    // seam edge cases and only catch genuinely-broken upstream.
    //
    // FDS_DEV ONLY. Its cost is not the ~12 compare/fabs instructions it
    // looks like: the abort branch keeps dwx/dwy/dwz, viewX/Y/Z, all three
    // cr.lightISource components, viewToLight row 0, the offset and lz LIVE
    // across the whole body just to print them, and that register pressure is
    // what forces the 304-byte frame and the 10 stp/ldp callee-save pairs in
    // this function's prologue/epilogue — paid on EVERY tap, ~1.4 M taps per
    // greets frame. It is also redundant for memory safety: the iX/iY range
    // check immediately below rejects anything off the map, and fcvtzs maps
    // NaN to 0, so a broken matrix reads texel 0 rather than walking memory.
    // The ship build drops it; dev builds keep the tripwire.
#if FDS_DEV
    const float kSaneAbs = 16.0f * float(sm.xres > sm.yres ? sm.xres : sm.yres);
    if (!std::isfinite(smX) || !std::isfinite(smY) ||
        std::fabs(smX) > kSaneAbs || std::fabs(smY) > kSaneAbs) {
        static std::atomic<int> sLogged{0};
        if (sLogged.fetch_add(1) < 4) {
            std::fprintf(stderr,
                "[CUBE-SAMPLE-BAD] cubeIdx=%d face=%d  smX=%g smY=%g\n"
                "  worldD=(%g,%g,%g)  view=(%g,%g,%g)\n"
                "  cr.lightISource=(%g,%g,%g)\n"
                "  viewToLight row0=(%g,%g,%g) off=(%g,%g,%g) lz=%g\n",
                cubeIdx, face, smX, smY,
                dwx, dwy, dwz, viewX, viewY, viewZ,
                cr.lightISource.x, cr.lightISource.y, cr.lightISource.z,
                sm.viewToLight[0][0], sm.viewToLight[0][1], sm.viewToLight[0][2],
                sm.viewToLightOffset.x, sm.viewToLightOffset.y, sm.viewToLightOffset.z,
                lz);
            std::fflush(stderr);
        }
        std::abort();
    }
#endif  // FDS_DEV
    const int iX = int(smX);
    const int iY = int(smY);
    if (iX < 0 || iX + 1 >= sm.xres || iY < 0 || iY + 1 >= sm.yres) return 1.0f;
    // ─── PolyId uniformity pyramid: a verdict WITHOUT a tap ───────────────
    // A BRANCH AROUND the tap, not extra live state inside it. Split in two so
    // each fast path pays only what it needs: the block lookup runs FIRST (it
    // needs nothing but iX/iY), and a uniformly-LIT block returns before the
    // weights are even formed. The uniformly-OCCLUDING arm does need them, so
    // it falls through to the shared weight block below and returns there.
    // Either way the 2x2 addressing block — the swizzle test, the row offset,
    // the four texel offsets, the two plane base pointers — is skipped whole.
    //
    // What is skipped on top of that: 4 texels x 2 planes = 8 packed loads over
    // up to 4 cache lines, plus the four id compares. What is paid: one u32
    // from a (xres/8)^2 table — 16 KB for a 512^2 face, which stays resident.
    uint32_t uniC = kShadowUniMixed;
#if FDS_SHADOW_TAP_CENSUS
    if (surfaceMatId >= 0) {
        FDS_PYR_CENSUS(reached);
        if (dynamicOnly) FDS_PYR_CENSUS(dynOnly);
        if (sm.uniSD.empty()) FDS_PYR_CENSUS(noPyr);
    }
#endif
    if (surfaceMatId >= 0 && !sm.uniSD.empty()) {
        const size_t b = size_t(iY >> kShadowUniShift) * size_t(sm.uniW)
                       + size_t(iX >> kShadowUniShift);
        const uint32_t uD = sm.uniDyn[b];
        // dynamicOnly reads ONLY the dynamic plane, so uD is its verdict
        // outright. The full tap takes the closest-by-z of the two ids, and
        // the pyramid can only settle that without looking when the dynamic
        // apron is uniformly EMPTY: with dId == 0 at every texel closestPacked
        // returns the static id verbatim (`if (dId == 0) return sId;`),
        // whatever the z halves hold. Any other dynamic content -> mixed ->
        // the real tap runs below, unchanged.
        uniC = dynamicOnly ? uD : (uD == 0u ? sm.uniSD[b] : kShadowUniMixed);
        if (uniC == kShadowUniMixed) { FDS_PYR_CENSUS(mixed); }
        else {
            // Uniform id over the whole footprint, so the 2x2 PCF is exactly
            // this one texel's verdict, four times.
            if (uniC == 0u || uint16_t(uniC) == uint16_t(surfaceMatId)) {
                FDS_PYR_CENSUS(fastLit);
                return 1.0f;   // occ would never leave its 0.0f initialiser
            }
            FDS_PYR_CENSUS(fastOcc);
            // --shadow_polyid_no_pcf takes the single (00) texel and sets
            // occ = 1.0f outright; mirror that rather than summing weights
            // that can land a few ULP short of 1.0f and return ~6e-8.
            if (fds::FeatureFlags::shadow_polyid_no_pcf()) return 0.0f;
        }
    }
    const float fx = smX - float(iX);
    const float fy = smY - float(iY);
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 =         fx  * (1.0f - fy);
    const float w01 = (1.0f - fx) *         fy;
    const float w11 =         fx  *         fy;
    // Uniform OCCLUDING block: all four `occ += w` fire, in this order, over
    // these same four floats — the identical expression, and therefore the
    // identical bit pattern, the tap below would have produced.
    if (uniC != kShadowUniMixed) {
        float occU = 0.0f;
        occU += w00; occU += w10; occU += w01; occU += w11;
        return (occU >= 1.0f) ? 0.0f : (1.0f - occU);
    }
    // Tap addressing: linear row-major, or 8×8-tiled under --shadow-swizzle
    // (halves the cache lines a 2×2 footprint touches; the tiled copies are
    // derived after each bake). Falls back to linear when a needed tiled
    // copy hasn't been built yet — both layouts hold identical values, so
    // mixing per-map is safe and byte-identical.
    const bool swz = fds::FeatureFlags::shadow_swizzle()
                  && !sm.packSDSw.empty() && !sm.packDynSw.empty();
    size_t o00, o10, o01, o11;
    const uint32_t *psB, *pdB;
    if (swz) {
        const ShadowSwzShape &shp = ShadowSwzGetShape();
        const int tpr = ShadowSwzTilesPerRow(sm.xres, shp);
        o00 = ShadowSwzOffset(iX,     iY,     tpr, shp);
        o10 = ShadowSwzOffset(iX + 1, iY,     tpr, shp);
        o01 = ShadowSwzOffset(iX,     iY + 1, tpr, shp);
        o11 = ShadowSwzOffset(iX + 1, iY + 1, tpr, shp);
        psB = sm.packSDSw.data();  pdB = sm.packDynSw.data();
    } else {
        const size_t rowOfs = size_t(iY) * size_t(sm.xres);
        o00 = rowOfs + size_t(iX);   o10 = o00 + 1;
        o01 = o00 + size_t(sm.xres); o11 = o01 + 1;
        psB = sm.packSD.data();  pdB = sm.packDyn.data();
    }
    float occ = 0.0f;
    if (surfaceMatId >= 0) {
        // PolyId mode: identity test. The ShadowBarry rasterizer writes
        // matID+1 into polyId (0 = unwritten); receiver's matID+1 at a
        // tap = "same material wrote there, not occluded". Anything
        // else nonzero = different-material occluder → occluded.
        //
        // Closest-occluder selection when both static AND dynamic wrote
        // to the tap: pick the polyId whose buffer has the LARGER zEnc
        // (larger = closer to the light, wins as occluder). This mirrors
        // the depth-mode `closest = max(static, dynamic)` logic. The
        // earlier "dynamic always wins if non-zero" shortcut was wrong:
        // it caused dynamic meshes to project spurious shadows onto
        // static surfaces that were actually closer to the light than
        // the dynamic mesh.
        //
        // Reads: ONE 32-bit word per texel per plane. The full tap is 4
        // texels × 2 planes = 32 bytes from 2 base pointers (4 lines);
        // the dynamicOnly composite tap is 16 bytes from 1 (2 lines).
        // Still identity-test semantics (no bias acne).
        // Direct 16-bit comparison: caller already resolved ShadowMatID.
        const uint16_t receiverId = uint16_t(surfaceMatId);
        // dynamicOnly: treat the static word as 0 so closestPacked always
        // returns the dynamic id (or 0 if dynamic is also empty) — and
        // never touches the static plane's cache lines at all.
        auto closestPacked = [&](size_t o) -> uint16_t {
            const uint32_t d = pdB[o];
            const uint32_t s = dynamicOnly ? 0u : psB[o];
            const uint16_t dId = ShadowTexId(d);
            const uint16_t sId = ShadowTexId(s);
            // Empty plane (id == 0) loses regardless of depth, because
            // unwritten texels have zEnc = 0 anyway. Otherwise pick the
            // one whose occluder is closer to the light.
            if (sId == 0) return dId;
            if (dId == 0) return sId;
            return (ShadowTexZ(d) > ShadowTexZ(s)) ? dId : sId;
        };
        if (fds::FeatureFlags::shadow_polyid_no_pcf()) {
            const uint16_t c = closestPacked(o00);
            if (c != 0 && c != receiverId) occ = 1.0f;
        } else {
            const uint16_t c00 = closestPacked(o00);
            const uint16_t c10 = closestPacked(o10);
            const uint16_t c01 = closestPacked(o01);
            const uint16_t c11 = closestPacked(o11);
            if (c00 != 0 && c00 != receiverId) occ += w00;
            if (c10 != 0 && c10 != receiverId) occ += w10;
            if (c01 != 0 && c01 != receiverId) occ += w01;
            if (c11 != 0 && c11 != receiverId) occ += w11;
        }
    } else {
        // Depth mode: legacy biased depth comparison. Dynamic plane is
        // populated only when --shadow-dynamic on; when off it's still
        // allocated but all-zero, so max() falls through to the static
        // value with no behavior change.
        int pixZenc = 0xFF80 - int(lz * sm.zScale);
        if (pixZenc < 0) pixZenc = 0;
        if (pixZenc > 0xFFFF) pixZenc = 0xFFFF;
        const int biased = pixZenc + constBias + slopeBiasInt;
        // dynamicOnly: ignore static plane (its contribution is already
        // baked into the lightmap atlas the caller multiplied us into).
        auto closest = [&](size_t o) -> int {
            const uint16_t a = dynamicOnly ? uint16_t(0) : ShadowTexZ(psB[o]);
            const uint16_t b = ShadowTexZ(pdB[o]);
            return int(a > b ? a : b);
        };
        if (biased < closest(o00)) occ += w00;
        if (biased < closest(o10)) occ += w10;
        if (biased < closest(o01)) occ += w01;
        if (biased < closest(o11)) occ += w11;
    }
    return (occ >= 1.0f) ? 0.0f : (1.0f - occ);
}
