#pragma once

#include "Base/Vector.h"
#include "Base/Matrix.h"
#include "Base/Camera.h"
#include "Base/Face.h"
#include "Base/FeatureFlags.h"
#include <atomic>
#include <vector>
#include <cstdint>

// Per-shadow-casting-light depth buffer + the light-view transform used
// to compute it. The lighting kernel reconstructs each pixel's
// world-space position from the deferred G-buffer, transforms it into
// this light's view space using `lightViewMat` + `lightISource`, projects
// using `perspX`/`perspY`, samples `depth`, and compares against the
// pixel's view-space z. If pixel.z > sampled.z + bias, the pixel is in
// shadow for this light.
//
// The map's resolution is independent of the framebuffer (typically
// 512×512). Encoded Z matches Mekalele's: `enc = 0xFF80 - round(z * zScale)`
// where `zScale = 0xFF00 / (fzp * 1.1f)`. Higher enc = closer.
struct ShadowMap {
	// Encoded Z of the closest occluder at each texel (always written by
	// the shadow rasterizer regardless of mode — it's a real z-buffer).
	// Depth mode: lighting kernel compares biased pixel-z against this.
	std::vector<uint16_t> depth;      // size xres*yres; row-major
	// matID + 1 of the closest occluder (8 bits is enough — matID is
	// 0-255 in the engine). Parallel to `depth`, populated by the
	// shadow rasterizer only when running in PolyId mode (zeroed
	// otherwise). 0 = "unwritten / no occluder" sentinel, so the
	// engine-side matID is shifted by +1. Lighting kernel reads this
	// for material equality in PolyId mode; ignored in Depth mode.
	// Widened to uint16_t so per-Material::ShadowMatID overrides (e.g.
	// greets wall split) can exceed the 8-bit cap. 0 = "no occluder";
	// non-zero = ShadowMatID (which itself defaults to `Txtr->ID + 1`
	// when the Material hasn't been assigned a custom group). Memory:
	// 2 bytes/texel × 6 cube faces × N omnis, e.g. 10 omnis × 6 × 512² =
	// 30 MB per buffer pair, still well within budget.
	std::vector<uint16_t> polyId;

	// Parallel buffers populated per-frame by the dynamic-objects bake
	// (BakeDynamicForStaticOmnis). Only animated meshes are rendered
	// here. Static-omni lighting samples min(depth, depth_dynamic) so
	// moving objects cast moving shadows even though the static bake
	// only fires once. Allocated lazily by ShadowMaps_Rebuild +
	// CubeShadowMaps_Rebuild — same size as `depth` / `polyId`.
	std::vector<uint16_t> depth_dynamic;
	std::vector<uint16_t> polyId_dynamic;
	// True iff the DynamicMeshesPerFrame bake processed this map THIS frame
	// (a dynamic mesh was visible in the face pyramid). When false the
	// dynamic planes are stale/empty and the dynamicOnly tap short-circuits
	// to lit — which both skips the tap cost for the (common) faces with no
	// moving mesh AND stops a stale last-visible-frame silhouette from
	// shadowing after the mesh leaves the face.
	bool dynBaked = false;

	// [experiment: --shadow-swizzle] 8×8-tiled copies of the four planes.
	// Rebuilt from the linear planes after each bake (ShadowMap_SwizzlePlanes,
	// timed separately so the swizzle overhead is measurable on its own); the
	// linear planes stay the source of truth for every other reader (viz,
	// overlay, lightmap bake, clone taps). Sized tilesPerRow*tileRows*64 with
	// zero-filled edge padding (reads there = "unwritten" sentinel). Empty
	// until first swizzle → hot readers fall back to linear when empty.
	std::vector<uint16_t> depthSw, polyIdSw, depthDynSw, polyIdDynSw;
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
// Writes only the Z byte to `g_currentShadowMap->depth`; no color, no
// G-buffer, no texture.
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
void ShadowMaps_BakeStatic(struct Scene *Sc);

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
// Memory: 6 × res² × (4 depth + 1 polyId) per shadow-casting omni.
// At 256² per face: ~1.5MB/omni. Fine for handful of static omnis;
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
// ShadowMatID for direct comparison against sm.polyId (NO +1 offset
// added inside; callers pass the literal value the bake stamped).
//   -1 = legacy Depth mode: compare biased pixel-z against sm.depth.
//        constBias + slopeBiasInt control the comparison bias.
//   0..65535 = PolyId mode: identity test of sm.polyId[texel] against
//        surfaceMatId. Caller is responsible for resolving the receiver's
//        ShadowMatID first (Material::ShadowMatID if non-zero, else
//        fallback uint16_t(matID + 1)). Bias parameters ignored.
//
// `dynamicOnly`: when true, only the dynamic buffers (sm.depth_dynamic,
//   sm.polyId_dynamic) are sampled — the static buffers are ignored.
//   Used by the static-lightmap composite path: the lightmap atlas
//   already encodes the static-cube shadow factor for the pixel, so
//   adding the static buffer here would double-count. dynamicOnly
//   layers per-frame dynamic-mesh shadows on top of the static atlas.
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
    const int iX = int(smX);
    const int iY = int(smY);
    if (iX < 0 || iX + 1 >= sm.xres || iY < 0 || iY + 1 >= sm.yres) return 1.0f;
    // Tap addressing: linear row-major, or 8×8-tiled under --shadow-swizzle
    // (halves the cache lines a 2×2 footprint touches; the tiled copies are
    // derived after each bake). Falls back to linear when a needed tiled
    // copy hasn't been built yet — both layouts hold identical values, so
    // mixing per-map is safe and byte-identical.
    const bool swz = fds::FeatureFlags::shadow_swizzle()
                  && !sm.depthSw.empty() && !sm.depthDynSw.empty()
                  && (surfaceMatId < 0 || (!sm.polyIdSw.empty() && !sm.polyIdDynSw.empty()));
    size_t o00, o10, o01, o11;
    const uint16_t *zsB, *zdB;
    const uint16_t *idsB = nullptr, *iddB = nullptr;
    if (swz) {
        const ShadowSwzShape &shp = ShadowSwzGetShape();
        const int tpr = ShadowSwzTilesPerRow(sm.xres, shp);
        o00 = ShadowSwzOffset(iX,     iY,     tpr, shp);
        o10 = ShadowSwzOffset(iX + 1, iY,     tpr, shp);
        o01 = ShadowSwzOffset(iX,     iY + 1, tpr, shp);
        o11 = ShadowSwzOffset(iX + 1, iY + 1, tpr, shp);
        zsB = sm.depthSw.data();  zdB = sm.depthDynSw.data();
        if (surfaceMatId >= 0) { idsB = sm.polyIdSw.data(); iddB = sm.polyIdDynSw.data(); }
    } else {
        const size_t rowOfs = size_t(iY) * size_t(sm.xres);
        o00 = rowOfs + size_t(iX);   o10 = o00 + 1;
        o01 = o00 + size_t(sm.xres); o11 = o01 + 1;
        zsB = sm.depth.data();  zdB = sm.depth_dynamic.data();
        if (surfaceMatId >= 0) { idsB = sm.polyId.data(); iddB = sm.polyId_dynamic.data(); }
    }
    const float fx = smX - float(iX);
    const float fy = smY - float(iY);
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 =         fx  * (1.0f - fy);
    const float w01 = (1.0f - fx) *         fy;
    const float w11 =         fx  *         fy;
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
        // Reads: 4 polyId + 4 depth bytes per buffer (static + dynamic)
        // = 24 bytes/tap × 4 taps = 96 bytes vs depth-only's 64 bytes.
        // Slightly more memory than pure depth, but still identity-test
        // semantics (no bias acne).
        // Direct 16-bit comparison: caller already resolved ShadowMatID.
        const uint16_t receiverId = uint16_t(surfaceMatId);
        auto closestPoly = [&](uint16_t sId, uint16_t dId,
                                uint16_t sZ, uint16_t dZ) -> uint16_t {
            // Empty buffer (id == 0) loses regardless of depth, because
            // unwritten texels have zEnc = 0 anyway. Otherwise pick the
            // one whose occluder is closer to the light.
            if (sId == 0) return dId;
            if (dId == 0) return sId;
            return (dZ > sZ) ? dId : sId;
        };
        // dynamicOnly: zero out the static side so closestPoly always
        // returns the dynamic id (or 0 if dynamic is also empty).
        const uint16_t s00 = dynamicOnly ? uint16_t(0) : idsB[o00];
        const uint16_t s10 = dynamicOnly ? uint16_t(0) : idsB[o10];
        const uint16_t s01 = dynamicOnly ? uint16_t(0) : idsB[o01];
        const uint16_t s11 = dynamicOnly ? uint16_t(0) : idsB[o11];
        const uint16_t zs00 = dynamicOnly ? uint16_t(0) : zsB[o00];
        const uint16_t zs10 = dynamicOnly ? uint16_t(0) : zsB[o10];
        const uint16_t zs01 = dynamicOnly ? uint16_t(0) : zsB[o01];
        const uint16_t zs11 = dynamicOnly ? uint16_t(0) : zsB[o11];
        if (fds::FeatureFlags::shadow_polyid_no_pcf()) {
            const uint16_t c = closestPoly(s00, iddB[o00], zs00, zdB[o00]);
            if (c != 0 && c != receiverId) occ = 1.0f;
        } else {
            const uint16_t c00 = closestPoly(s00, iddB[o00], zs00, zdB[o00]);
            const uint16_t c10 = closestPoly(s10, iddB[o10], zs10, zdB[o10]);
            const uint16_t c01 = closestPoly(s01, iddB[o01], zs01, zdB[o01]);
            const uint16_t c11 = closestPoly(s11, iddB[o11], zs11, zdB[o11]);
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
        // dynamicOnly: ignore static buffer (its contribution is already
        // baked into the lightmap atlas the caller multiplied us into).
        auto closest = [&](uint16_t a, uint16_t b) -> int {
            return int((dynamicOnly ? uint16_t(0) : a) > b
                       ? (dynamicOnly ? uint16_t(0) : a) : b);
        };
        if (biased < closest(zsB[o00], zdB[o00])) occ += w00;
        if (biased < closest(zsB[o10], zdB[o10])) occ += w10;
        if (biased < closest(zsB[o01], zdB[o01])) occ += w01;
        if (biased < closest(zsB[o11], zdB[o11])) occ += w11;
    }
    return (occ >= 1.0f) ? 0.0f : (1.0f - occ);
}
