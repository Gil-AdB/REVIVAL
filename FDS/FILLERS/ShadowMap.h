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
void MekaleleShadowDepth(Face* F, struct Vertex** V, unsigned int numVerts, unsigned int miplevel);

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
