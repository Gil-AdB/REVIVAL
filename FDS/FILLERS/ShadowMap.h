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
