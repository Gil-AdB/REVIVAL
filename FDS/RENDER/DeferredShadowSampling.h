#ifndef FDS_RENDER_DEFERRED_SHADOW_SAMPLING_H_INCLUDED
#define FDS_RENDER_DEFERRED_SHADOW_SAMPLING_H_INCLUDED

// DeferredShadowSampling.h — per-pixel shadow resolve helpers shared by
// the deferred surface + transparent kernels (split out of
// DeferredLighting.cpp; verbatim moves, see DeferredCommon.h for the
// split layout). The lightmap-vs-cube-tap choice and all its debug
// recompute paths live here so every kernel samples shadows the same way.

#include <cstdint>

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/Scene.h"
#include "Base/StaticShadowLightmap.h"
#include "Base/TriMesh.h"
#include "Base/Face.h"
#include "Base/Omni.h"
#include "FILLERS/Mekalele.h"
#include "FILLERS/ShadowMap.h"
#include "RENDER/LightmapBake.h"

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

#endif // FDS_RENDER_DEFERRED_SHADOW_SAMPLING_H_INCLUDED
