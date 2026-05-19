#pragma once

#include "Base/Vector.h"
#include "Base/Matrix.h"
#include "Base/Camera.h"
#include "Base/Face.h"
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
	std::vector<uint8_t> polyId;
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
// Bias parameters match the spot-shadow path; slopeBiasFactor accepts
// a precomputed (1/N·L - 1) factor since the caller has the normals
// handy. Caller computes the world-space sample pos once per pixel.
inline float CubeShadow_Sample(int cubeIdx,
                                float worldX, float worldY, float worldZ,
                                float viewX,  float viewY,  float viewZ,
                                int   constBias, int slopeBiasInt)
{
    if (cubeIdx < 0 || size_t(cubeIdx) >= g_cubeShadowRefs.size()) return 1.0f;
    const CubeShadowRef& cr = g_cubeShadowRefs[cubeIdx];
    const float dwx = worldX - cr.lightISource.x;
    const float dwy = worldY - cr.lightISource.y;
    const float dwz = worldZ - cr.lightISource.z;
    const int face = CubeShadow_SelectFace(dwx, dwy, dwz);
    const ShadowMap& sm = g_shadowMaps[cr.faceIdx[face]];
    // Project view-space sample point into face-view space.
    const float lx = sm.viewToLight[0][0] * viewX + sm.viewToLight[0][1] * viewY +
                     sm.viewToLight[0][2] * viewZ + sm.viewToLightOffset.x;
    const float ly = sm.viewToLight[1][0] * viewX + sm.viewToLight[1][1] * viewY +
                     sm.viewToLight[1][2] * viewZ + sm.viewToLightOffset.y;
    const float lz = sm.viewToLight[2][0] * viewX + sm.viewToLight[2][1] * viewY +
                     sm.viewToLight[2][2] * viewZ + sm.viewToLightOffset.z;
    if (lz <= 0.0f) return 1.0f;
    const float invLZ = 1.0f / lz;
    const float smX = sm.cntrX + sm.perspX * lx * invLZ;
    const float smY = sm.cntrY - sm.perspY * ly * invLZ;
    const int iX = int(smX);
    const int iY = int(smY);
    if (iX < 0 || iX + 1 >= sm.xres || iY < 0 || iY + 1 >= sm.yres) return 1.0f;
    const uint16_t *zRow0 = sm.depth.data() + size_t(iY) * size_t(sm.xres);
    const uint16_t *zRow1 = zRow0 + sm.xres;
    const float fx = smX - float(iX);
    const float fy = smY - float(iY);
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 =         fx  * (1.0f - fy);
    const float w01 = (1.0f - fx) *         fy;
    const float w11 =         fx  *         fy;
    int pixZenc = 0xFF80 - int(lz * sm.zScale);
    if (pixZenc < 0) pixZenc = 0;
    if (pixZenc > 0xFFFF) pixZenc = 0xFFFF;
    const int biased = pixZenc + constBias + slopeBiasInt;
    float occ = 0.0f;
    if (biased < int(zRow0[iX    ])) occ += w00;
    if (biased < int(zRow0[iX + 1])) occ += w10;
    if (biased < int(zRow1[iX    ])) occ += w01;
    if (biased < int(zRow1[iX + 1])) occ += w11;
    return (occ >= 1.0f) ? 0.0f : (1.0f - occ);
}
