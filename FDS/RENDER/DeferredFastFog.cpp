// Fast analytic + froxel fog and the unified Beer-Lambert volumetric
// pass — split out of DeferredLighting.cpp (verbatim function moves;
// see DeferredCommon.h for the split layout).
//
// Entry points (called from RENDER.CPP / scene drivers):
//   Render_DeferredFastFog()    — analytic / froxel fog composite
//   Render_DeferredVolumetric() — unified fog+cones+halos pass
//   Render_ScreenSpaceRain()    — screen-space rain streaks
//   FastFog_SetReflectionZ() / FastFog_BeginFrame() — city pass-2 hook
// The transparent peel (DeferredSurfaceKernel.cpp) samples fog at xpar
// depth through the non-static FastFog_XparActive/SampleGrid/SSActive/
// SSSample hooks declared in DeferredCommon.h.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <algorithm>
#include <limits>
#include <vector>
#include <chrono>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "simde/x86/fma.h"

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/Scene.h"
#include "Base/Omni.h"
#include "Base/Camera.h"
#include "FILLERS/Mekalele.h"
#include "FILLERS/ShadowMap.h"
#include "RENDER/DeferredCommon.h"
#include "RENDER/Hdr.h"
#include "Threads.h"

// FRUSTRUM.CPP — file-scope; forward declare for libm-pow fallback.
extern float fastLog2(float x);
extern float fastPow2(float x);

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

// ─── Fast analytic fog (iquilezles model) ───────────────────────────────
//
// A cheap closed-form atmospheric fog post-pass that composes with the
// analytic cone/halo passes (it runs after them, so god-ray shafts fog
// out with distance like everything else). Unlike Render_DeferredFogPass
// — which is a sqrt distance ramp toward black, gated on Scn_Fogged —
// this is gated on the `fast_fog` flag alone so it can be toggled on
// clear scenes, and uses Beer-Lambert transmittance toward the scene's
// Ambient color (the atmosphere tint).
//
// Per pixel, view-space ray V=(X,Y,1), surface at view depth z=zMax:
//   optical depth  τ = σ·|V|·e^{-k·camY}·∫₀^{zMax} e^{-k·gY·z} dz
//   fog amount       = 1 - e^{-τ}        (clamp 0..1)
//   out              = lerp(surf, fogColor, fogAmount)
// where σ = density/FZP (per true-distance extinction), k = height
// falloff (denser low), gY = world-Y change per unit view-z =
// viewToWorld[1]·(X,Y,1), |V| = sqrt(X²+Y²+1) maps view-z to true
// distance. k=0 collapses to uniform Beer-Lambert τ = σ·(true distance).
// fastPow2 (FRUSTRUM.CPP) builds the result by shifting the integer part
// straight into the float exponent field with no saturation, so a large
// |argument| yields garbage rather than 0/∞. These helpers keep the
// exponent in a safe band: e^{±50} is already far past visual relevance
// (e^{-50}≈2e-22 → fully transparent; e^{50}≈5e21 → fully opaque once
// scaled), so clamping there avoids NaN pixels without changing the look.
static inline float fastExpNeg(float x) {
	constexpr float kLog2e = 1.4426950408889634f;
	if (x > 50.0f)  x = 50.0f;
	if (x < -50.0f) x = -50.0f;
	return fastPow2(-x * kLog2e);
}
static inline float fastExpPos(float x) {
	constexpr float kLog2e = 1.4426950408889634f;
	if (x > 50.0f) x = 50.0f;
	if (x < 0.0f)  x = 0.0f;
	return fastPow2(x * kLog2e);
}

// ∫₀^z e^{-m·z'} dz'. The Taylor series for small |m·z| dodges the
// 1-e^{-mz} catastrophic cancellation (fastPow2 is only an 8-bit LUT, so
// e^{-mz}≈1 can't be resolved near the horizon); closed form otherwise,
// with fastExpPos clamping the growth on descending rays (m<0).
static inline float fogAntiderivG(float z, float m) {
	const float mz = m * z;
	if (mz > 0.1f)  return (1.0f - fastExpNeg(mz)) / m;
	if (mz < -0.1f) return (fastExpPos(-mz) - 1.0f) / (-m);
	return z * (1.0f - mz*(0.5f - mz*(1.0f/6.0f - mz*(1.0f/24.0f))));
}

struct FastFogParams {
	float invFOVX, invFOVY, invZScale;
	float sigma, fogFar, kHeight, heightBase;
	// viewToWorld rows (rotation): world ray dir = w·(X,Y,1).
	float w00, w01, w02, w10, w11, w12, w20, w21, w22;
	float camX, camY, camZ;
	float slabY0, slabY1;
	float fogR, fogG, fogB;
	bool  blobs;
	float cell, invCell, jitter;
	bool  worley;          // froxel blob field: inverted Worley F1 instead of value noise
	float worleyThresh;    // density hits 0 at F1 = (1-thresh) cells from a feature point
	float worleyInvT;      // 1/(1-thresh), precomputed remap gain
	float blobOverlap;     // >0: additive metaball field, blob radius in cell units
	float glowMax;         // >0: soft-knee cap on per-slice in-scatter radiance
	bool  hdr;             // HDR: glowMax forced 0 (raw radiance); composite -> g_hdrBuf
	int   glowGridDiv;     // froxel glow on a /div coarse XY grid (1 = per-column)
	int   taps;            // density samples per froxel per frame (1 or 2)
	float invRf;   // distance-falloff rate: density *= exp(-z·invRf)
	// In-scatter glow: scene lights lighting the fog medium.
	const ViewLightsSoA *lights;
	int   numLights;
	float inscatter;   // 0 = off
	int   inscatterSamples;   // sample count for the sampled/fallback integral (>=1)
	bool  inscatterAnalytic;  // closed-form arctan integral for unshadowed lights
	bool  inscatterJitter;    // Bayer per-pixel sample-offset (breaks shadow terraces); off = centered samples
	bool  shadowEarlyOut;     // skip the per-sample integral where the segment probes fully lit
	bool  shadowAnalytic;     // bisection + per-interval analytic lit integral instead of the sample loop
	int   shadowPcf;          // PCF radius (texels) for the sampled shadow tap; 0 = single tap
	// Downsample: coarseStep px between computed samples (2 = half-res).
	// adaptThresh > 0 enables the adaptive refine (recompute edges).
	int   coarseStep;
	float adaptThresh;
	float ditherAmp;   // triangular dither (levels) to break 8-bit banding
	float feather;     // slab Y-edge feather (world units); 0 = hard cutoff
	float invFeather;  // 1/feather, precomputed for the froxel density
	// Animation: rigid wind drift + a low-frequency curl (swirl) of the noise
	// field, so the fog flows and churns instead of sitting frozen. driftX/Y/Z
	// is this frame's advection in world units (= wind·time); the curl warps the
	// sample position rotationally. Froxel path applies both (fogWarp); the
	// screen-space DDA applies only the rigid drift (a straight-ray DDA can't
	// follow a curved warp). All 0 = static field.
	float driftX, driftY, driftZ;
	float swirlAmp;    // curl warp amplitude (world units); 0 = drift only
	float swirlFreq;   // curl spatial frequency (1/world-units)
	float swirlPhase;  // curl temporal phase (radians; advances with scene time)
};

// Hash a 3D integer cell index → 32 random bits. One call yields all
// three jitter offsets (10 bits each) to keep the per-cell DDA cost low.
static inline uint32_t cellHash(int ix, int iy, int iz) {
	// Nonzero seed + rotate-between-terms. A plain XOR of per-axis products
	// degenerates at the world origin (hash(0,0,0)==0, which survives the whole
	// finalizer) and on the coordinate planes (a zero coord drops its term, so
	// the lattice gains a fixed structural feature aligned to the axes). Anchored
	// at world (0,0,0) that feature lands exactly where origin-centered scene
	// content sits (e.g. a spot at the origin) and reads as a deliberate disc in
	// the blob fog rather than honest noise. The seed kills the 0->0 fixed point
	// and the rotations make the three axis contributions non-commuting so no
	// coordinate plane collapses the entropy.
	uint32_t h = 0x9E3779B9u;
	h ^= uint32_t(ix) * 0x8DA6B343u; h = (h << 13) | (h >> 19);
	h ^= uint32_t(iy) * 0xD8163841u; h = (h << 13) | (h >> 19);
	h ^= uint32_t(iz) * 0xCB1AB31Fu; h = (h << 13) | (h >> 19);
	h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
	h *= 0x297A2D39u; h ^= h >> 15;
	return h;
}

// ── Fog animation: sin/cos LUT ─────────────────────────────────────────────
// The curl warp (fogWarp) evaluates sin/cos per dense froxel across the whole
// screen, so libm trig would dominate. A 1024-entry table — built once at
// static-init time, before any worker thread runs — with linear interpolation
// is plenty for a low-frequency domain warp. The extra guard entry [1024]==[0]
// lets the lerp read i+1 without a wrap branch.
static const struct FogTrigLUT {
	float s[1025];
	FogTrigLUT() {
		for (int i = 0; i <= 1024; ++i)
			s[i] = float(std::sin(double(i) * (6.283185307179586 / 1024.0)));
	}
} g_fogTrig;
static inline float fogSin(float x) {
	float r = x * (1.0f / 6.2831853f);
	r -= std::floor(r);                       // wrap to [0,1)
	float f = r * 1024.0f;
	int   i = int(f);                         // 0..1023
	return g_fogTrig.s[i] + (g_fogTrig.s[i + 1] - g_fogTrig.s[i]) * (f - float(i));
}
static inline float fogCos(float x) { return fogSin(x + 1.5707963f); }

// Domain-warp a world sample position so the fog field animates: a rigid wind
// drift plus a low-frequency curl that rotates the flow (the field churns, not
// just slides). Driven by P.swirlPhase, which advances with scene time. Called
// at the noise-lookup entry so every variant (value/worley/metaball) and every
// octave warps identically. The z term uses the pre-warp x to avoid feedback.
static inline void fogWarp(const FastFogParams& P, float& wx, float& wy, float& wz) {
	wx += P.driftX; wy += P.driftY; wz += P.driftZ;
	if (P.swirlAmp > 0.0f) {
		const float f = P.swirlFreq, ph = P.swirlPhase;
		const float ox = wx;                              // pre-warp x
		wx += P.swirlAmp * fogSin(wz * f + ph);
		wz += P.swirlAmp * fogCos(ox * f + ph);
		wy += P.swirlAmp * 0.5f * fogSin((ox + wz) * f * 0.7f + ph * 1.3f);
	}
}

// Optical depth of the procedural fog field along the world ray
// O + t·D over t ∈ [tA,tB]. The density is trilinear VALUE NOISE on the
// world grid: each lattice corner gets a hashed random value, and the
// density at a point is the trilinear blend of its 8 cell corners. Because
// neighbouring cells SHARE corner values, the field is continuous across
// cell walls — no cube slices and no hard cell-boundary rectangles (the
// failure modes of per-cell owned spheres). A 3D-DDA walks the cells the
// ray crosses; within each cell the density along the ray is a cubic in t
// (three linear interpolants multiplied), so 2-point Gauss integrates the
// cell's z-segment EXACTLY. Front-to-back with a Beer-Lambert early-out.
static float blobFieldTau(const FastFogParams& P,
                          float Ox, float Oy, float Oz,
                          float Dx, float Dy, float Dz,
                          float /*a*/, float Vlen, float tA, float tB)
{
	const float cell = P.cell, invCell = P.invCell;

	// Wind drift: rigid translation of the field, which keeps this straight-ray
	// DDA valid. The curl swirl is froxel-path only (see fogWarp) — a domain
	// warp would bend the ray and break the cell traversal.
	Ox += P.driftX; Oy += P.driftY; Oz += P.driftZ;

	const float ex = Ox + tA*Dx, ey = Oy + tA*Dy, ez = Oz + tA*Dz;
	int cx = int(std::floor(ex * invCell));
	int cy = int(std::floor(ey * invCell));
	int cz = int(std::floor(ez * invCell));

	auto setup = [&](float d, float o, int c, int& step, float& tMax, float& tDelta){
		if (d > 1e-12f || d < -1e-12f) {
			step = d > 0.0f ? 1 : -1;
			const float boundary = (d > 0.0f ? float(c + 1) : float(c)) * cell;
			tMax   = (boundary - o) / d;
			tDelta = cell / (d > 0.0f ? d : -d);
		} else { step = 0; tMax = 1e30f; tDelta = 1e30f; }
	};
	int sx, sy, sz; float tMaxX, tMaxY, tMaxZ, tDx, tDy, tDz;
	setup(Dx, Ox, cx, sx, tMaxX, tDx);
	setup(Dy, Oy, cy, sy, tMaxY, tDy);
	setup(Dz, Oz, cz, sz, tMaxZ, tDz);

	auto h01 = [](int x, int y, int z){ return float(cellHash(x, y, z)) * (1.0f/4294967296.0f); };

	float t = tA, tau = 0.0f;
	for (int guard = 0; t < tB && guard < 96; ++guard) {
		const float tNext  = std::min(tMaxX, std::min(tMaxY, tMaxZ));
		const float segEnd = std::min(tNext, tB);

		if (segEnd > t) {
			// 8 shared lattice-corner randoms for this cell.
			const float c000 = h01(cx,   cy,   cz  ), c100 = h01(cx+1, cy,   cz  );
			const float c010 = h01(cx,   cy+1, cz  ), c110 = h01(cx+1, cy+1, cz  );
			const float c001 = h01(cx,   cy,   cz+1), c101 = h01(cx+1, cy,   cz+1);
			const float c011 = h01(cx,   cy+1, cz+1), c111 = h01(cx+1, cy+1, cz+1);

			// Empty-cell skip: trilinear interpolation is bounded by its 8
			// corners, so if every corner is below the gap threshold the
			// density is 0 throughout the cell — skip the Gauss trilerps
			// (still hop the cell; the traversal itself is unavoidable).
			constexpr float kGap = 0.45f;
			const float cmax = std::max(std::max(std::max(c000,c100), std::max(c010,c110)),
			                            std::max(std::max(c001,c101), std::max(c011,c111)));
			if (cmax < kGap) {
				if (tMaxX <= tMaxY && tMaxX <= tMaxZ)      { cx += sx; t = tMaxX; tMaxX += tDx; }
				else if (tMaxY <= tMaxZ)                   { cy += sy; t = tMaxY; tMaxY += tDy; }
				else                                       { cz += sz; t = tMaxZ; tMaxZ += tDz; }
				continue;
			}

			const float cmx = float(cx)*cell, cmy = float(cy)*cell, cmz = float(cz)*cell;

			auto sample = [&](float s) -> float {
				float u = (Ox + s*Dx - cmx) * invCell;
				float v = (Oy + s*Dy - cmy) * invCell;
				float w = (Oz + s*Dz - cmz) * invCell;
				u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
				v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
				w = w < 0.f ? 0.f : (w > 1.f ? 1.f : w);
				// Quintic fade (Perlin): C2-continuous interpolation weights so
				// the field has no slope kinks at cell faces. Plain trilinear is
				// only C0 — its per-face kinks line up along the world-aligned
				// lattice into visible facet/chevron bands once amplified by the
				// in-scatter glow. The quintic curves them away.
				u = u*u*u*(u*(u*6.f - 15.f) + 10.f);
				v = v*v*v*(v*(v*6.f - 15.f) + 10.f);
				w = w*w*w*(w*(w*6.f - 15.f) + 10.f);
				const float x00 = c000 + (c100-c000)*u, x01 = c001 + (c101-c001)*u;
				const float x10 = c010 + (c110-c010)*u, x11 = c011 + (c111-c011)*u;
				const float y0  = x00 + (x10-x00)*v,     y1  = x01 + (x11-x01)*v;
				const float val = y0 + (y1-y0)*w;
				// Remap to carve gaps (clear air) and dense cores so the
				// field reads as discrete masses, not uniform haze.
				const float d = (val - 0.45f) * 1.8f;
				return d > 0.0f ? (d > 1.0f ? 1.0f : d) : 0.0f;
			};

			// 2-point Gauss over [t, segEnd]. Exact for trilinear (cubic along
			// the ray); the quintic fade in sample() lifts the degree, but the
			// per-cell segment is short enough that 2-point matches a 16-point
			// composite midpoint to <1 level (verified) — keep it cheap.
			const float mid = 0.5f*(t + segEnd), hlen = 0.5f*(segEnd - t);
			constexpr float gq = 0.5773502692f;
			float dens = hlen * (sample(mid - hlen*gq) + sample(mid + hlen*gq));
			if (dens > 0.0f) {
				if (P.invRf > 0.0f) dens *= fastExpNeg(mid * P.invRf);
				tau += P.sigma * Vlen * dens;
				if (tau > 12.0f) return tau;   // opaque — stop walking
			}
		}

		if (tMaxX <= tMaxY && tMaxX <= tMaxZ)      { cx += sx; t = tMaxX; tMaxX += tDx; }
		else if (tMaxY <= tMaxZ)                   { cy += sy; t = tMaxY; tMaxY += tDy; }
		else                                       { cz += sz; t = tMaxZ; tMaxZ += tDz; }
	}
	return tau;
}

// Shadow test for a view-space point against a spot's shadow map. Returns
// visibility in [0,1] (1 lit, 0 occluded). Constant bias only (a volume point
// has no surface normal for slope bias). pcf=0 → single binary tap; pcf>0 → a
// (2·pcf+1)² PCF box, returning FRACTIONAL visibility. The fraction is what
// kills the along-ray terracing cheaply: with a binary tap, neighbouring
// in-scatter samples flip 0→1 at the shadow edge in one step (→ 1/ns bands),
// so you need huge ns to hide it; PCF spreads the edge over a soft penumbra
// so a handful of samples already read smooth.
static inline float volSpotShadow(int smIdx, float x, float y, float z, int pcf) {
	if (smIdx < 0 || size_t(smIdx) >= g_shadowMaps.size()) return 1.0f;
	const ShadowMap& sm = g_shadowMaps[smIdx];
	const float lx = sm.viewToLight[0][0]*x + sm.viewToLight[0][1]*y + sm.viewToLight[0][2]*z + sm.viewToLightOffset.x;
	const float ly = sm.viewToLight[1][0]*x + sm.viewToLight[1][1]*y + sm.viewToLight[1][2]*z + sm.viewToLightOffset.y;
	const float lz = sm.viewToLight[2][0]*x + sm.viewToLight[2][1]*y + sm.viewToLight[2][2]*z + sm.viewToLightOffset.z;
	if (lz <= 0.0f) return 1.0f;
	const float invLZ = 1.0f / lz;
	const int cX = int(sm.cntrX + sm.perspX * lx * invLZ);
	const int cY = int(sm.cntrY - sm.perspY * ly * invLZ);
	if (cX < 0 || cX >= sm.xres || cY < 0 || cY >= sm.yres) return 1.0f;
	const int pixZ = (0xFF80 - int(lz * sm.zScale)) + 80;   // +kVolShadowBias
	const bool hasDyn = !sm.depth_dynamic.empty();
	if (pcf <= 0) {
		const size_t idx = size_t(cY) * size_t(sm.xres) + size_t(cX);
		uint16_t occ = sm.depth[idx];
		if (hasDyn) occ = std::max(occ, sm.depth_dynamic[idx]);
		return (pixZ < int(occ)) ? 0.0f : 1.0f;
	}
	int lit = 0, total = 0;
	for (int dy = -pcf; dy <= pcf; ++dy) {
		const int sy = cY + dy;
		if (sy < 0 || sy >= sm.yres) continue;
		const size_t row = size_t(sy) * size_t(sm.xres);
		for (int dx = -pcf; dx <= pcf; ++dx) {
			const int sx = cX + dx;
			if (sx < 0 || sx >= sm.xres) continue;
			const size_t idx = row + size_t(sx);
			uint16_t occ = sm.depth[idx];
			if (hasDyn) occ = std::max(occ, sm.depth_dynamic[idx]);
			lit += (pixZ < int(occ)) ? 0 : 1;
			++total;
		}
	}
	return total ? float(lit) / float(total) : 1.0f;
}

// Attenuation of one light at a view-space point Pv (distance + cone falloff,
// same shape as the halo/cone surface passes). Returns 0 outside range / behind
// the cone. No shadow tap (the caller adds it where needed) and no colour.
static inline float lightAttenAt(const ViewLightsSoA *L, int li,
                                 float Px, float Py, float Pz)
{
	const float Wx = Px - L->posX[li], Wy = Py - L->posY[li], Wz = Pz - L->posZ[li];
	const float d2 = Wx*Wx + Wy*Wy + Wz*Wz;
	if (d2 >= L->range2[li] || d2 < 1e-6f) return 0.0f;
	const float dist = std::sqrt(d2);
	const float dr   = dist * L->rRange[li];
	const float cutoff = 1.0f - dr;
	float atten = cutoff * cutoff / (dr*dr + 0.05f);
	if (L->isSpot[li]) {
		const float DW = L->dirX[li]*Wx + L->dirY[li]*Wy + L->dirZ[li]*Wz;
		if (DW <= 0.0f) return 0.0f;
		const float cosT = DW / dist;
		const float cosO = L->cosOuter[li];
		if (cosT < cosO) return 0.0f;
		const float cosI = L->cosInner[li];
		if (cosT < cosI) {
			const float tt = (cosT - cosO) / (cosI - cosO);
			atten *= tt * tt * (3.0f - 2.0f * tt);
		}
	}
	return atten;
}

// Clip a view ray Pv(z)=(z·X,z·Y,z) against one light's support: (range sphere)
// ∩ (spot cone, forward half). [zLo,zHi] comes in as the candidate interval and
// leaves clipped; returns false if the ray never passes through the light.
// uV=|V|², VP=<V,Lpos>, PP=|Lpos|² are the caller's ray/light dot products.
// Shared by the screen-space inscatter and the froxel populate — the integrand
// has compact support here, and BOTH need it sub-sample-exact (see callers).
static inline bool lightRayClip(const ViewLightsSoA* L, int li, float X, float Y,
                                float uV, float VP, float PP,
                                float& zLo, float& zHi)
{
	// Range sphere: |Pv-L|² = r². uV z² - 2 VP z + (PP-r²) = 0.
	const float sphereC    = PP - L->range2[li];
	const float sphereDisc = VP*VP - uV*sphereC;
	if (sphereDisc <= 0.0f) return false;          // ray misses the sphere
	const float sphereSq = std::sqrt(sphereDisc);
	const float zSphLo = (VP - sphereSq) / uV, zSphHi = (VP + sphereSq) / uV;
	if (zLo < zSphLo) zLo = zSphLo;
	if (zHi > zSphHi) zHi = zSphHi;
	if (L->isSpot[li]) {
		// Cone: (D·W)² = cosO²|W|². a z² + b z + cq = 0 in z.
		const float Dx = L->dirX[li], Dy = L->dirY[li], Dz = L->dirZ[li];
		const float Lx = L->posX[li], Ly = L->posY[li], Lz = L->posZ[li];
		const float DV = Dx*X + Dy*Y + Dz, DP = Dx*Lx + Dy*Ly + Dz*Lz;
		const float c2 = L->cosOuter[li] * L->cosOuter[li];
		const float a  = DV*DV - c2*uV;
		const float b  = 2.0f*(c2*VP - DV*DP);
		const float cq = DP*DP - c2*PP;
		if (a < -1e-8f) {                          // ray enters & exits cone
			const float d = b*b - 4.0f*a*cq;
			if (d < 0.0f) return false;
			const float sq = std::sqrt(d), inv2a = 0.5f / a;
			const float r1 = (-b - sq)*inv2a, r2 = (-b + sq)*inv2a;
			if (zLo < std::min(r1,r2)) zLo = std::min(r1,r2);
			if (zHi > std::max(r1,r2)) zHi = std::max(r1,r2);
		} else if (a > 1e-8f) {                    // cone opens the other way
			const float d = b*b - 4.0f*a*cq;
			if (d >= 0.0f) {
				const float sq = std::sqrt(d), inv2a = 0.5f / a;
				const float r1 = std::min((-b-sq)*inv2a,(-b+sq)*inv2a);
				const float r2 = std::max((-b-sq)*inv2a,(-b+sq)*inv2a);
				if (DV > 1e-6f)      { if (zLo < r2) zLo = r2; }
				else if (DV < -1e-6f){ if (zHi > r1) zHi = r1; }
				else return false;
			}
		} else return false;
		// Forward half (D·W ≥ 0): the cone is single-sheeted.
		if (std::fabs(DV) > 1e-6f) {
			const float zFwd = DP / DV;
			if (DV > 0.0f) { if (zLo < zFwd) zLo = zFwd; }
			else           { if (zHi > zFwd) zHi = zFwd; }
		}
	}
	return zHi > zLo;
}

// In-scatter glow integrated over the view-ray fog segment [zA,zB] (ray point
// Pv(z) = (z·X, z·Y, z)): the MEAN attenuation × colour over the segment, summed
// over lights, into gR/gG/gB (the caller scales by fog amount × strength).
//
// The radial kernel is cutoff²/(dr²+0.05) — a near-1/dist² term that spikes in a
// lamp's near field. A single midpoint sample of it prints a bright disc/shell
// wherever the midpoint locus crosses a lamp. We instead INTEGRATE it:
//   • Unshadowed lights (inscatterAnalytic): closed form. dr²+0.05 = αz²+βz+γ,
//     so ∫dz/(αz²+βz+γ) = 2/√disc·Δatan — the same arctan integral the cone/halo
//     analytic passes use. cutoff² and the cone smoothstep (slowly varying) are
//     taken at the segment's closest-approach point z* = clamp(VP/uV, zA, zB),
//     where the integrand peaks. Finite and smooth → no disc.
//   • Shadow-casting spots: the per-point shadow tap can't be integrated in
//     closed form (same reason vol_cone_analytic ray-marches shadowed spots), so
//     fall back to ns stratified samples with the shadow tap per sample.
static inline void fogInscatterSegment(const FastFogParams& P, float X, float Y,
                                       float zA, float zB, float jitter,
                                       float& gR, float& gG, float& gB)
{
	const ViewLightsSoA *L = P.lights;
	const float seg = zB - zA;
	if (seg <= 0.0f) return;
	const float invSeg = 1.0f / seg;
	const float uV = X*X + Y*Y + 1.0f;
	const int   ns = P.inscatterSamples > 0 ? P.inscatterSamples : 1;

	for (int li = 0; li < P.numLights; ++li) {
		if (L->mirrorId[li] != 0) continue;            // clones don't glow

		const float rr  = L->rRange[li], rr2 = rr*rr;
		const float Lx = L->posX[li], Ly = L->posY[li], Lz = L->posZ[li];
		const float VP = X*Lx + Y*Ly + Lz;            // <Pv(z),L>/z linear term
		const float PP = Lx*Lx + Ly*Ly + Lz*Lz;       // |L|²

		// Clip the integration interval [zLo,zHi] to the light's actual passage
		// through (range sphere) ∩ (spot cone, forward half) — the integrand has
		// compact support there. BOTH paths use this: the analytic path needs it
		// so the cone factor isn't over-counted (matches vol_cone_analytic); the
		// SAMPLED path needs it so all ns samples land inside the support instead
		// of being spread over the whole [zA,zB] fog span — otherwise the few
		// samples that happen to hit the ~1/dist² near-field spike quantise it
		// into concentric rings (the "distinct circles" under shadow-cast spots).
		float zLo = zA, zHi = zB;
		if (!lightRayClip(L, li, X, Y, uV, VP, PP, zLo, zHi))
			continue;                                  // segment never in-light

		const bool shadowed = L->shadowMapIdx[li] >= 0;
		if (P.inscatterAnalytic) {
			// ── Brightness: exact analytic integral (no 1/dist² spike noise,
			// no sampling bias). ∫[zLo,zHi] dz/(αz²+βz+γ)=2/√disc·Δatan, with
			// αz²+βz+γ = dr²+0.05.
			const float alpha = rr2 * uV;
			const float beta  = -2.0f * rr2 * VP;
			const float gamma = rr2 * PP + 0.05f;
			const float disc  = 4.0f*alpha*gamma - beta*beta;   // > 0 (the +0.05)
			if (disc <= 0.0f) continue;
			const float invD  = 1.0f / std::sqrt(disc);
			const float twoA  = alpha + alpha;
			const float aHi   = std::atan((twoA*zHi + beta) * invD);
			const float aLo   = std::atan((twoA*zLo + beta) * invD);
			// Normalise by the FULL segment (zB-zA): glow is the mean attenuation
			// over the ray's fog span, with the out-of-light part contributing 0.
			const float meanRadial = (2.0f * invD * (aHi - aLo)) * invSeg;
			if (meanRadial <= 0.0f) continue;

			// cutoff²·cone (slowly varying) at the integrand's peak z* = closest
			// approach VP/uV, clamped into the in-light interval. This is the
			// UNSHADOWED / fully-lit glow.
			float zStar = VP / uV;
			zStar = zStar < zLo ? zLo : (zStar > zHi ? zHi : zStar);
			const float aStar = lightAttenAt(L, li, zStar*X, zStar*Y, zStar);
			if (aStar <= 0.0f) continue;
			const float ddxS = zStar*X - Lx, ddyS = zStar*Y - Ly, ddzS = zStar - Lz;
			const float drS2 = (ddxS*ddxS + ddyS*ddyS + ddzS*ddzS) * rr2;   // dr(z*)²
			float atten = aStar * (drS2 + 0.05f) * meanRadial;

			if (shadowed) {
				const int smi = L->shadowMapIdx[li];
				// Analytic shadow (experimental flag): bisect the shadow map for the
				// lit/shadowed transitions, sum the analytic radial integral over LIT
				// sub-intervals with cutoff²·cone pinned per interval at clamp(z*,a,b).
				//   glow = invSeg · 2·invD · Σ_lit shaping(clamp(z*,a,b))·Δatan
				// >4 transitions → fall through to the robust importance loop.
				if (P.shadowAnalytic) {
					auto visB   = [&](float z){ return volSpotShadow(smi, z*X, z*Y, z, 0) >= 0.5f; };
					auto atanAt = [&](float z){ return std::atan((twoA*z + beta) * invD); };
					auto shapingAt = [&](float z) -> float {
						const float Wx = z*X-Lx, Wy = z*Y-Ly, Wz = z-Lz;
						const float d2 = Wx*Wx + Wy*Wy + Wz*Wz;
						if (d2 >= L->range2[li] || d2 < 1e-6f) return 0.0f;
						const float dist = std::sqrt(d2);
						const float cutoff = 1.0f - dist * L->rRange[li];
						float s = cutoff * cutoff;
						if (L->isSpot[li]) {
							const float DW = L->dirX[li]*Wx + L->dirY[li]*Wy + L->dirZ[li]*Wz;
							if (DW <= 0.0f) return 0.0f;
							const float cosT = DW / dist;
							if (cosT < L->cosOuter[li]) return 0.0f;
							if (cosT < L->cosInner[li]) {
								const float tt = (cosT - L->cosOuter[li]) / (L->cosInner[li] - L->cosOuter[li]);
								s *= tt * tt * (3.0f - 2.0f * tt);
							}
						}
						return s;
					};
					constexpr int M = 8;
					const float dzc = (zHi - zLo) / float(M - 1);
					bool  prevLit = visB(zLo);
					float zRunLo = zLo, aRun = aLo;
					float glowAcc = 0.0f;
					int   trans = 0;
					bool  ok = true;
					float zprev = zLo;
					auto closeLit = [&](float zEnd, float aEnd){
						float zc = zStar < zRunLo ? zRunLo : (zStar > zEnd ? zEnd : zStar);
						glowAcc += shapingAt(zc) * (aEnd - aRun);
					};
					for (int i = 1; i < M; ++i) {
						const float z = (i == M-1) ? zHi : (zLo + dzc*float(i));
						const bool  lit = visB(z);
						if (lit != prevLit) {
							float lo = zprev, hi = z;
							for (int b = 0; b < 6; ++b) {
								const float mid = 0.5f*(lo+hi);
								if (visB(mid) == prevLit) lo = mid; else hi = mid;
							}
							const float zt = 0.5f*(lo+hi);
							if (prevLit) closeLit(zt, atanAt(zt));
							else { zRunLo = zt; aRun = atanAt(zt); }
							prevLit = lit;
							if (++trans > 4) { ok = false; break; }
						}
						zprev = z;
					}
					if (ok) {
						if (prevLit) closeLit(zHi, aHi);
						atten = invSeg * 2.0f * invD * glowAcc;
						if (atten <= 0.0f) continue;
						gR += L->colR[li] * atten;
						gG += L->colG[li] * atten;
						gB += L->colB[li] * atten;
						continue;
					}
				}
				// Optional fully-lit early-out (flag, lit-heavy views): if PCF
				// visibility reads 1 at z* and 5 points across the segment, the
				// ray is unoccluded → the analytic brightness above is exact.
				bool doLoop = true;
				if (P.shadowEarlyOut) {
					bool lit = volSpotShadow(smi, zStar*X, zStar*Y, zStar, P.shadowPcf) >= 1.0f;
					const float seg4 = (zHi - zLo) * 0.25f;
					for (int t = 0; t <= 4 && lit; ++t) {
						const float z = zLo + seg4 * float(t);
						if (volSpotShadow(smi, z*X, z*Y, z, P.shadowPcf) < 1.0f) lit = false;
					}
					doLoop = !lit;
				}
				if (doLoop) {
					// Exact coupled integral invSeg·∫kernel·vis, IMPORTANCE-sampling
					// z by the RADIAL kernel's own CDF (atan invertible: z(u) =
					// (tan(aLo+u·span)/invD − β)/2α). The 1/dist² spike rides the
					// importance distribution, so the summand is shaping(z)·vis(z)
					// — cutoff²·cone (slowly varying) × PCF visibility, both smooth
					// → tiny variance, UNBIASED (matches brute-force coupled), no
					// terraces / jitter pattern at a handful of samples.
					//   glow = invSeg · (∫radial) · mean[ shaping(z_k)·vis(z_k) ]
					// tan of the evenly-spaced angles θ_k = aLo+(k+jitter)·dAng is
					// advanced by the addition formula tan(θ+dAng) = (t+tD)/(1−t·tD),
					// so ns tan() calls collapse to two. shaping is cutoff²·cone
					// computed directly (the radial divide cancels the importance
					// weight — no lightAttenAt divide-then-remultiply).
					const float span  = aHi - aLo;
					const float invNs = 1.0f / float(ns);
					const float dAng  = span * invNs;
					const float Rint  = 2.0f * invD * span;
					const float invDr = 1.0f / invD;          // = √disc
					const float invTwoA = 1.0f / twoA;
					const float tD = std::tan(dAng);
					float t = std::tan(aLo + jitter * dAng);  // tan θ_0
					const float rR = L->rRange[li], range2 = L->range2[li];
					const bool  isSpot = L->isSpot[li];
					const float Dx = L->dirX[li], Dy = L->dirY[li], Dz = L->dirZ[li];
					const float cosO = L->cosOuter[li], cosI = L->cosInner[li];
					float acc = 0.0f;
					for (int k = 0; k < ns; ++k) {
						const float z = (t * invDr - beta) * invTwoA;
						t = (t + tD) / (1.0f - t * tD);        // advance to θ_{k+1}
						const float Wx = z*X - Lx, Wy = z*Y - Ly, Wz = z - Lz;
						const float d2 = Wx*Wx + Wy*Wy + Wz*Wz;
						if (d2 >= range2 || d2 < 1e-6f) continue;
						const float dist = std::sqrt(d2);
						const float cutoff = 1.0f - dist * rR;
						float shaping = cutoff * cutoff;       // = cutoff²·cone
						if (isSpot) {
							const float DW = Dx*Wx + Dy*Wy + Dz*Wz;
							if (DW <= 0.0f) continue;
							const float cosT = DW / dist;
							if (cosT < cosO) continue;
							if (cosT < cosI) {
								const float tt = (cosT - cosO) / (cosI - cosO);
								shaping *= tt * tt * (3.0f - 2.0f * tt);
							}
						}
						acc += shaping * volSpotShadow(smi, z*X, z*Y, z, P.shadowPcf);
					}
					atten = invSeg * Rint * acc * invNs;
				}
				if (atten <= 0.0f) continue;
			}
			gR += L->colR[li] * atten;
			gG += L->colG[li] * atten;
			gB += L->colB[li] * atten;
		} else {
			// Fully-sampled path (--no-fast_fog_inscatter_analytic): kernel×shadow
			// per sample, normalised by the FULL segment. Placed across the CLIPPED
			// [zLo,zHi] so all ns samples land in the support. Jitter decorrelates
			// the binary shadow tap's 1/ns terracing into dither.
			const float dz = (zHi - zLo) / float(ns);
			float acc = 0.0f;
			for (int k = 0; k < ns; ++k) {
				const float z = zLo + (float(k) + jitter) * dz;
				float a = lightAttenAt(L, li, z*X, z*Y, z);
				if (a > 0.0f && shadowed)
					a *= volSpotShadow(L->shadowMapIdx[li], z*X, z*Y, z, P.shadowPcf);
				acc += a;
			}
			const float atten = acc * dz * invSeg;     // ∫[zLo,zHi]/seg
			gR += L->colR[li] * atten;
			gG += L->colG[li] * atten;
			gB += L->colB[li] * atten;
		}
	}
}

// Fog amount [0,1] for one pixel's ray integrated to an EXPLICIT depth zMax,
// plus the in-scatter glow RGB (premultiplied by fog amount). The depth-
// agnostic core shared by the opaque pass (fogAtPixel reads the Z-buffer)
// and the transparent peel (which fogs each xpar pixel to ITS OWN depth).
static inline float fogAtDepth(const FastFogParams& P, int px, int py,
                               float zMax, float& glowR, float& glowG, float& glowB)
{
	glowR = glowG = glowB = 0.0f;
	if (zMax <= 0.0f) return 0.0f;
	const float Y  = (CntrEY - float(py)) * P.invFOVY;
	const float X  = (float(px) - CntrEX) * P.invFOVX;
	const float uV = X*X + Y*Y + 1.0f;
	const float Vlen = std::sqrt(uV);
	const float gY = P.w10 * X + P.w11 * Y + P.w12;

	// Clamp integration to the ray's segment inside the slab [slabY0,slabY1].
	float zA = 0.0f, zB = zMax;
	if (gY > 1e-9f || gY < -1e-9f) {
		float za = (P.slabY0 - P.camY) / gY;
		float zb = (P.slabY1 - P.camY) / gY;
		if (za > zb) { const float t = za; za = zb; zb = t; }
		zA = za > 0.0f ? za : 0.0f;
		zB = zb < zMax ? zb : zMax;
	} else if (P.camY < P.slabY0 || P.camY > P.slabY1) {
		return 0.0f;   // level ray entirely outside the slab
	}
	if (zB <= zA) return 0.0f;

	float tau;
	if (P.blobs) {
		const float Dx = P.w00*X + P.w01*Y + P.w02;
		const float Dz = P.w20*X + P.w21*Y + P.w22;
		tau = blobFieldTau(P, P.camX, P.camY, P.camZ, Dx, gY, Dz, uV, Vlen, zA, zB);
	} else {
		// Height falloff exp(-(k·gY)z) and distance falloff exp(-z·invRf)
		// are both exponentials in z → one rate m, one closed form.
		float dens;
		if (P.kHeight != 0.0f || P.invRf > 0.0f) {
			const float m = P.kHeight * gY + P.invRf;
			dens = P.heightBase * (fogAntiderivG(zB, m) - fogAntiderivG(zA, m));
		} else {
			dens = zB - zA;
		}
		tau = P.sigma * Vlen * dens;
	}
	if (tau <= 0.0f) return 0.0f;

	// Feather the slab's hard Y-cutoff: smoothstep ramp over `feather` world
	// units at the top and bottom so the boundary isn't a razor edge at
	// grazing angles. Evaluated at the slab segment's world-Y midpoint (for
	// grazing rays Y is ~constant along the segment, so this is accurate
	// where it matters). Unbounded slab → feather huge → profile ~1, no-op.
	if (P.feather > 0.0f) {
		const float wy = P.camY + gY * (0.5f * (zA + zB));
		const float invF = 1.0f / P.feather;
		float lo = (wy - P.slabY0) * invF; lo = lo < 0.f ? 0.f : (lo > 1.f ? 1.f : lo);
		float hi = (P.slabY1 - wy) * invF; hi = hi < 0.f ? 0.f : (hi > 1.f ? 1.f : hi);
		const float prof = lo*lo*(3.0f-2.0f*lo) * hi*hi*(3.0f-2.0f*hi);
		tau *= prof;
		if (tau <= 0.0f) return 0.0f;
	}
	if (tau > 50.0f) tau = 50.0f;
	float amt = 1.0f - fastExpNeg(tau);
	if (amt < 0.0f) amt = 0.0f;
	if (amt > 1.0f) amt = 1.0f;

	// In-scatter glow: lights reaching the fog segment, scaled by how much fog
	// is along the ray. Premultiplied so the compositor just adds it. The
	// segment integrator returns the mean attenuation over [zA,zB] (closed-form
	// arctan for unshadowed lights, sampled for shadowed) — integrating the
	// 1/dist² near-field instead of point-sampling it dissolves the bright
	// disc/shell a single midpoint sample produces.
	if (P.inscatter > 0.0f && P.lights && amt > 0.0f) {
		float gR = 0.0f, gG = 0.0f, gB = 0.0f;
		// Per-pixel sample-offset in [0,1) for the sampled in-scatter path,
		// breaking up the binary shadow tap's 1/ns terracing. Interleaved
		// Gradient Noise (Jimenez): a low-discrepancy hash that spreads offsets
		// like an ordered dither (so the shadow edge reconstructs smoothly) but
		// WITHOUT a Bayer tile's visible regular crosshatch — and far less
		// grainy than a white-noise hash. Degrades gracefully under the half-res
		// upsample where a Bayer tile would alias into a coarse weave.
		float ign = 0.06711056f * float(px) + 0.00583715f * float(py);
		ign = 52.9829189f * (ign - std::floor(ign));
		ign = ign - std::floor(ign);                       // [0,1)
		const float jitter = P.inscatterJitter ? ign
		    : 0.5f;   // off → centered (k+0.5) samples (terraces unless PCF/high ns)
		fogInscatterSegment(P, X, Y, zA, zB, jitter, gR, gG, gB);
		// Soft-knee glow compressor — same as the froxel populate (linear
		// below glowMax/2, asymptote at glowMax), applied to the glow
		// radiance before the fog-amount premultiply. The ambient term is
		// composited separately here (fogColor·amt) and sits well below the
		// knee, so compressing just the glow matches the froxel result.
		gR *= P.inscatter; gG *= P.inscatter; gB *= P.inscatter;
		if (P.glowMax > 0.0f) {
			const float m = gR > gG ? (gR > gB ? gR : gB) : (gG > gB ? gG : gB);
			const float k = P.glowMax * 0.5f;
			if (m > k) {
				const float e = m - k;
				const float s = (k + e / (1.0f + e / k)) / m;
				gR *= s; gG *= s; gB *= s;
			}
		}
		glowR = gR * amt; glowG = gG * amt; glowB = gB * amt;
	}
	return amt;
}

// Fog amount [0,1] for one pixel, plus the surface distance used (for the
// half-res bilateral upsample) and the in-scatter glow RGB (premultiplied by
// fog amount). Returns 0 amount where the ray doesn't fog.
static inline float fogAtPixel(const FastFogParams& P, int px, int py,
                               float& outZ, float& glowR, float& glowG, float& glowB)
{
	const uint16_t *zEnc = ZPage16;
	const size_t i = size_t(py) * size_t(XRes) + size_t(px);
	// Sky (no surface) fogs at the far plane so the horizon fades into the
	// fog color; opaque surfaces clamp to FZP so fog saturates.
	const float zSurf = float(0xFF80 - int(zEnc[i])) * P.invZScale;
	const float zMax  = (zSurf <= 0.0f) ? P.fogFar : std::min(zSurf, P.fogFar);
	outZ = zMax;
	return fogAtDepth(P, px, py, zMax, glowR, glowG, glowB);
}

// ── Screen-space fog hook for the transparent peel ──────────────────────
// Mirror of the froxel-grid hook (FastFog_SampleGrid): when the SCREEN-SPACE
// fast_fog ran this frame, the peel fogs each transparent pixel's lit color
// to its own depth with the same model the opaque composite used:
// T = 1−amt, acc = fogColor·amt + glow. Exact (full per-pixel evaluation,
// including the blob DDA march when blobs are on) — transparent coverage is
// the cost bound; the froxel path's grid fetch is the cheap variant.
namespace {
	FastFogParams gSSFogP;          // this frame's screen-space fog params
	bool          gSSFogActive = false;
}
bool FastFog_SSActive() { return gSSFogActive; }
void FastFog_SSSample(int px, int py, float z,
                             float& aR, float& aG, float& aB, float& T)
{
	float gR, gG, gB;
	const float zMax = z < gSSFogP.fogFar ? z : gSSFogP.fogFar;
	const float amt = fogAtDepth(gSSFogP, px, py, zMax, gR, gG, gB);
	T  = 1.0f - amt;
	aR = gSSFogP.fogR * amt + gR;
	aG = gSSFogP.fogG * amt + gG;
	aB = gSSFogP.fogB * amt + gB;
}

// Composite a fog amount (mix toward fog color) plus additive in-scatter
// glow onto VPage pixel i.
static inline void fogComposite(const FastFogParams& P, size_t i, float amt,
                                float gR, float gG, float gB)
{
	if (amt <= 0.0f && gR <= 0.0f && gG <= 0.0f && gB <= 0.0f) return;
	dword *out = reinterpret_cast<dword*>(VPage);
	const float keep = 1.0f - amt;
	const dword pix = out[i];
	// Triangular (TPDF) dither added before the 8-bit truncate, to dissolve the
	// contour banding that quantizing a smooth low-contrast fog gradient produces.
	// Stable per screen pixel (hash of i) so it doesn't shimmer. INDEPENDENT
	// offset per channel: a single shared offset only dithers luminance and
	// leaves chroma bands in the colored in-scatter glow (every channel's
	// quantization contour lands at the same pixel); decorrelating the three
	// breaks the colored rings too. Grey fog is unaffected (the means cancel).
	float dR = 0.0f, dG = 0.0f, dB = 0.0f;
	if (P.ditherAmp > 0.0f) {
		// One TPDF sample per channel from a hash; each channel gets its own
		// seed so the three offsets are uncorrelated.
		auto tpdf = [&](uint32_t seed) -> float {
			uint32_t h = seed * 0x9E3779B9u; h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13;
			const float r1 = float( h        & 0xFFFFu) * (1.0f/65536.0f);
			const float r2 = float((h >> 16) & 0xFFFFu) * (1.0f/65536.0f);
			return (r1 + r2 - 1.0f) * P.ditherAmp;   // triangular, [-amp, +amp]
		};
		const uint32_t s = uint32_t(i);
		dR = tpdf(s);
		dG = tpdf(s ^ 0x68E31DA4u);
		dB = tpdf(s ^ 0xB5297A4Du);
	}
	int nR = int(float((pix >> 16) & 0xFFu) * keep + P.fogR * amt + gR + dR);
	int nG = int(float((pix >>  8) & 0xFFu) * keep + P.fogG * amt + gG + dG);
	int nB = int(float( pix        & 0xFFu) * keep + P.fogB * amt + gB + dB);
	if (nR > 255) nR = 255; if (nR < 0) nR = 0;
	if (nG > 255) nG = 255; if (nG < 0) nG = 0;
	if (nB > 255) nB = 255; if (nB < 0) nB = 0;
	out[i] = (dword(nR) << 16) | (dword(nG) << 8) | dword(nB) | 0xFF000000u;
}

// Full-res path: compute and composite per pixel.
static void Render_DeferredFastFog_Tile(int x1, int y1, int x2, int y2,
                                        const FastFogParams& P)
{
	for (int py = y1; py < y2; ++py) {
		const size_t row = size_t(py) * size_t(XRes);
		for (int px = x1; px < x2; ++px) {
			float z, gR, gG, gB;
			const float amt = fogAtPixel(P, px, py, z, gR, gG, gB);
			fogComposite(P, row + size_t(px), amt, gR, gG, gB);
		}
	}
}

// Half-res producer: fog amount, surface distance, and in-scatter glow RGB
// for each half-res texel (sampling the full-res pixel at 2·hx, 2·hy). Buffers
// are sized to hw×hh by the dispatcher before this runs.
namespace {
	std::vector<float> gFogAmt, gFogZ, gFogGR, gFogGG, gFogGB;
	int gFogHW = 0, gFogHH = 0;
}
static void Render_DeferredFastFog_HalfTile(int hx1, int hy1, int hx2, int hy2,
                                            const FastFogParams& P)
{
	const int S = P.coarseStep;
	for (int hy = hy1; hy < hy2; ++hy) {
		const int py = std::min(S * hy, YRes - 1);
		const size_t base = size_t(hy) * gFogHW;
		for (int hx = hx1; hx < hx2; ++hx) {
			const int px = std::min(S * hx, XRes - 1);
			float z, gR, gG, gB;
			gFogAmt[base + hx] = fogAtPixel(P, px, py, z, gR, gG, gB);
			gFogZ [base + hx] = z;
			gFogGR[base + hx] = gR; gFogGG[base + hx] = gG; gFogGB[base + hx] = gB;
		}
	}
}

// Adaptive refine compositor: per full-res pixel, gather the 2x2 coarse
// samples; if they agree (fog-amount spread and depth both small) bilinearly
// interpolate, otherwise recompute the pixel exactly. Cheap in smooth regions,
// full-res-sharp at occluder/shadow/depth edges.
static void Render_DeferredFastFog_RefineTile(int x1, int y1, int x2, int y2,
                                              const FastFogParams& P)
{
	const uint16_t *zEnc = ZPage16;
	const int cw = gFogHW, ch = gFogHH, S = P.coarseStep;
	const float invS = 1.0f / float(S);
	for (int py = y1; py < y2; ++py) {
		const size_t row = size_t(py) * size_t(XRes);
		const int cy0 = std::min(py / S, ch - 1);
		const int cy1 = std::min(cy0 + 1, ch - 1);
		const float fy = float(py - cy0 * S) * invS;
		for (int px = x1; px < x2; ++px) {
			const size_t i = row + size_t(px);
			const int cx0 = std::min(px / S, cw - 1);
			const int cx1 = std::min(cx0 + 1, cw - 1);
			const float fx = float(px - cx0 * S) * invS;

			const size_t h00 = size_t(cy0)*cw + cx0, h10 = size_t(cy0)*cw + cx1;
			const size_t h01 = size_t(cy1)*cw + cx0, h11 = size_t(cy1)*cw + cx1;
			const float a00 = gFogAmt[h00], a10 = gFogAmt[h10],
			            a01 = gFogAmt[h01], a11 = gFogAmt[h11];
			const float aMin = std::min(std::min(a00,a10), std::min(a01,a11));
			const float aMax = std::max(std::max(a00,a10), std::max(a01,a11));

			const float zf = float(0xFF80 - int(zEnc[i])) * P.invZScale;
			const float zSurf = (zf <= 0.0f) ? P.fogFar : std::min(zf, P.fogFar);
			const float zMin = std::min(std::min(gFogZ[h00],gFogZ[h10]), std::min(gFogZ[h01],gFogZ[h11]));
			const float zMax = std::max(std::max(gFogZ[h00],gFogZ[h10]), std::max(gFogZ[h01],gFogZ[h11]));
			const bool depthOK = (zMax - zMin) < 0.05f * (zSurf + 1.0f)
			                   && std::min(std::abs(zSurf - zMin), std::abs(zSurf - zMax)) < 0.10f * (zSurf + 1.0f);

			if ((aMax - aMin) <= P.adaptThresh && depthOK) {
				// Smooth region: bilinear interpolate amt + glow.
				const float w00=(1-fx)*(1-fy), w10=fx*(1-fy), w01=(1-fx)*fy, w11=fx*fy;
				const float amt = a00*w00 + a10*w10 + a01*w01 + a11*w11;
				const float gR = gFogGR[h00]*w00 + gFogGR[h10]*w10 + gFogGR[h01]*w01 + gFogGR[h11]*w11;
				const float gG = gFogGG[h00]*w00 + gFogGG[h10]*w10 + gFogGG[h01]*w01 + gFogGG[h11]*w11;
				const float gB = gFogGB[h00]*w00 + gFogGB[h10]*w10 + gFogGB[h01]*w01 + gFogGB[h11]*w11;
				fogComposite(P, i, amt, gR, gG, gB);
			} else {
				// Edge: recompute this pixel exactly.
				float z, gR, gG, gB;
				const float amt = fogAtPixel(P, px, py, z, gR, gG, gB);
				fogComposite(P, i, amt, gR, gG, gB);
			}
		}
	}
}

// Full-res compositor: bilateral (depth-weighted) upsample of the half-res
// fog amount, so the soft fog reads at full res without bleeding across
// surface silhouettes.
static void Render_DeferredFastFog_CompositeTile(int x1, int y1, int x2, int y2,
                                                 const FastFogParams& P)
{
	const uint16_t *zEnc = ZPage16;
	const int hw = gFogHW, hh = gFogHH;
	for (int py = y1; py < y2; ++py) {
		const size_t row = size_t(py) * size_t(XRes);
		const int hy0 = std::min(py >> 1, hh - 1);
		const int hy1 = std::min(hy0 + 1, hh - 1);
		const float fy = (py & 1) ? 0.5f : 0.0f;
		for (int px = x1; px < x2; ++px) {
			const size_t i = row + size_t(px);
			const float zSurf = float(0xFF80 - int(zEnc[i])) * P.invZScale;
			const float zf = (zSurf <= 0.0f) ? P.fogFar : std::min(zSurf, P.fogFar);

			const int hx0 = std::min(px >> 1, hw - 1);
			const int hx1 = std::min(hx0 + 1, hw - 1);
			const float fx = (px & 1) ? 0.5f : 0.0f;

			// Bilinear weights × depth-similarity (bilateral) weights.
			const float bw[4] = { (1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy };
			const int   hxs[4] = { hx0, hx1, hx0, hx1 };
			const int   hys[4] = { hy0, hy0, hy1, hy1 };
			const float zscale = 1.0f / (zf * 0.10f + 1.0f);
			float wsum = 0.0f, asum = 0.0f, grS = 0.0f, ggS = 0.0f, gbS = 0.0f;
			for (int k = 0; k < 4; ++k) {
				const size_t h = size_t(hys[k]) * hw + hxs[k];
				const float dz = (gFogZ[h] - zf) * zscale;
				const float w  = bw[k] / (1.0f + dz*dz);
				wsum += w;
				asum += w * gFogAmt[h];
				grS  += w * gFogGR[h]; ggS += w * gFogGG[h]; gbS += w * gFogGB[h];
			}
			if (wsum > 0.0f) {
				const float iw = 1.0f / wsum;
				fogComposite(P, i, asum*iw, grS*iw, ggS*iw, gbS*iw);
			}
		}
	}
}

// ─── Froxel volumetric fog (view-frustum 3D grid) ───────────────────────────
// See docs/fast_fog_froxel_plan.md. Replaces the screen-space per-pixel blob
// march: populate density + in-scatter per froxel, integrate front-to-back along
// depth, trilinear-composite. Volumetric by construction, no per-pixel march.
namespace {
	std::vector<float> gFrAccR, gFrAccG, gFrAccB, gFrT;   // integrated cam→slice
	std::vector<float> gFrZb;                             // slice boundaries [nz+1]
	int   gFrX = 0, gFrY = 0, gFrZ = 0;
	float gFrNear = 1.0f, gFrFar = 1.0f;
	// Temporal: per-froxel raw scatter L·σ (rgb) + extinction σ, INTERLEAVED
	// float4 per froxel (one cache line covers a corner-pair in the history
	// fetch), ping-ponged — this frame's blended values are next frame's
	// history. [gFrCur] = current.
	std::vector<float> gFrSct[2];
	int      gFrCur = 0;
	uint32_t gFrFrameIdx = 0;
	bool     gFrHistValid = false;
	bool     gFrTemporal = false;       // this frame: jitter + blend enabled
	float    gFrBlend = 0.8f;
	float    gFrPrevCamX, gFrPrevCamY, gFrPrevCamZ;        // prev frame camera
	float    gFrPrevW[9];               // prev view→world rotation (rows)
	float    gFrPrevA[3];               // Rprevᵀ·(cam − camPrev), per frame
	// Coarse glow grid: per-light in-scatter RADIANCE evaluated on a
	// (nx/div × ny/div × nz) grid — light radiance is low-frequency in XY
	// (city omnis have 5000+ unit ranges) while the per-slice analytic
	// integral keeps depth exact. The fine populate bilinearly fetches it.
	std::vector<float> gGlow;           // gGlX×gGlY columns × nz × RGB
	int   gGlX = 0, gGlY = 0;
	bool  gFrHasShadowedLight = false;  // any light needing exact per-column glow
	bool  gFrHasFlashLight    = false;  // any Omni_FogTransient (added per-frame, not historied)
	// True while THIS renderFrame's froxel grid is valid for sampling
	// (set by the froxel dispatch, cleared at every renderFrame start) —
	// the transparent peel fogs its layers from the grid when set.
	bool     gFrFrameActive = false;
	// Optional reflection-pass depth (encoded uint16, displaced through the
	// scene's wobble like the color): set by the scene between its passes
	// (FastFog_SetReflectionZ), consumed by exactly ONE froxel composite —
	// the main pass's — then reset, so a scene change can't leave the
	// pointer dangling into a freed buffer.
	const uint16_t* gFrReflZ = nullptr;
	float           gFrReflWaterY = 0.0f;   // mirror plane height (world Y)
}

// See gFrReflZ. The city calls this after its dispMap wobble with the
// displaced pass-1 Z so the composite can fog the water's reflections along
// their actual two-leg path instead of uniformly at gFrFar.
void FastFog_SetReflectionZ(const uint16_t* z, float waterY) {
	gFrReflZ = fds::FeatureFlags::fast_fog_refl_depth() ? z : nullptr;
	gFrReflWaterY = waterY;
}

// Called at the top of renderFrame (RENDER.CPP) so a frame whose fog pass
// doesn't run (reflection pass, non-fog scenes) can't sample stale fog state.
void FastFog_BeginFrame() { gFrFrameActive = false; gSSFogActive = false; }
bool FastFog_XparActive() { return gFrFrameActive; }

// Trilinear sample of the integrated froxel grid (in-scatter acc + trans-
// mittance T) at screen pixel (px,py), view depth z. Used by the transparent
// peel: with the background already fully fogged, the EXACT composite under
// the froxel model is out = α·(C·T(z) + acc(z)) + (1−α)·Bg — fog the layer's
// lit color to its own depth, then alpha-blend normally. Linear-in-z slice
// fraction (the opaque composite's exact optical-depth fraction needs the
// per-slice ext, whose buffer has already ping-ponged by peel time; linear
// is visually equivalent on soft transparents).
void FastFog_SampleGrid(int px, int py, float z,
                               float& aR, float& aG, float& aB, float& T)
{
	const int nx = gFrX, ny = gFrY, nz = gFrZ;
	const float fnx = float(nx)/float(XRes), fny = float(ny)/float(YRes);
	const float invLogFN = 1.0f / std::log(gFrFar / gFrNear);
	if (z < gFrNear) z = gFrNear;
	if (z > gFrFar)  z = gFrFar;
	float u = std::log(z / gFrNear) * invLogFN * float(nz);   // boundary coord
	int iz = int(u); if (iz < 0) iz = 0; if (iz > nz-1) iz = nz-1;
	const float fz = u - float(iz);                            // 0 at zb[iz]
	const float fx = (float(px)+0.5f)*fnx - 0.5f;
	const float fy = (float(py)+0.5f)*fny - 0.5f;
	int ix0 = int(std::floor(fx)); float wx = fx - float(ix0);
	int iy0 = int(std::floor(fy)); float wy = fy - float(iy0);
	if (ix0 < 0) { ix0 = 0; wx = 0.0f; } if (ix0 > nx-2) { ix0 = nx>1?nx-2:0; wx = nx>1?1.0f:0.0f; }
	if (iy0 < 0) { iy0 = 0; wy = 0.0f; } if (iy0 > ny-2) { iy0 = ny>1?ny-2:0; wy = ny>1?1.0f:0.0f; }
	const float w00=(1-wx)*(1-wy), w10=wx*(1-wy), w01=(1-wx)*wy, w11=wx*wy;
	float pR=0,pG=0,pB=0,pT=0, cR=0,cG=0,cB=0,cT=0;
	auto add = [&](int ix, int iy, float w) {
		const size_t ic = (size_t(iy)*nx + ix)*nz + iz;
		cR += gFrAccR[ic]*w; cG += gFrAccG[ic]*w; cB += gFrAccB[ic]*w; cT += gFrT[ic]*w;
		if (iz > 0) { const size_t ip = ic-1;
			pR += gFrAccR[ip]*w; pG += gFrAccG[ip]*w; pB += gFrAccB[ip]*w; pT += gFrT[ip]*w;
		} else pT += w;   // before slice 0: acc=0, T=1
	};
	add(ix0,   iy0,   w00); add(ix0+1, iy0,   w10);
	add(ix0,   iy0+1, w01); add(ix0+1, iy0+1, w11);
	aR = pR + (cR-pR)*fz; aG = pG + (cG-pG)*fz; aB = pB + (cB-pB)*fz;
	T  = pT + (cT-pT)*fz;
}

// log2 via exponent bits + a rational mantissa correction (fastapprox-style,
// |err| ~3e-4) — the temporal reprojection needs a per-froxel log for the exp
// slice coordinate and libm logf dominates the loop.
static inline float frFastLog2(float x) {
	union { float f; uint32_t i; } vx; vx.f = x;
	union { uint32_t i; float f; } mx; mx.i = (vx.i & 0x007FFFFFu) | 0x3F000000u;
	const float y = float(vx.i) * 1.1920928955078125e-7f;
	return y - 124.22551499f - 1.498030302f*mx.f - 1.72587999f/(0.3520887068f + mx.f);
}

// Trilinear value-noise density at one world point for a given cell size (one
// sample, NO DDA march). Mirrors blobFieldTau's per-cell density. No slab check.
static inline float blobNoiseAt(float wx, float wy, float wz, float cell, float invCell) {
	const int cx = int(std::floor(wx * invCell));
	const int cy = int(std::floor(wy * invCell));
	const int cz = int(std::floor(wz * invCell));
	auto h01 = [](int x, int y, int z){ return float(cellHash(x, y, z)) * (1.0f/4294967296.0f); };
	const float c000 = h01(cx,cy,cz),     c100 = h01(cx+1,cy,cz);
	const float c010 = h01(cx,cy+1,cz),   c110 = h01(cx+1,cy+1,cz);
	const float c001 = h01(cx,cy,cz+1),   c101 = h01(cx+1,cy,cz+1);
	const float c011 = h01(cx,cy+1,cz+1), c111 = h01(cx+1,cy+1,cz+1);
	float u = (wx - float(cx)*cell) * invCell;
	float v = (wy - float(cy)*cell) * invCell;
	float w = (wz - float(cz)*cell) * invCell;
	u = u*u*u*(u*(u*6.f-15.f)+10.f);          // quintic fade (matches blobFieldTau)
	v = v*v*v*(v*(v*6.f-15.f)+10.f);
	w = w*w*w*(w*(w*6.f-15.f)+10.f);
	const float x00 = c000+(c100-c000)*u, x01 = c001+(c101-c001)*u;
	const float x10 = c010+(c110-c010)*u, x11 = c011+(c111-c011)*u;
	const float y0 = x00+(x10-x00)*v, y1 = x01+(x11-x01)*v;
	const float val = y0 + (y1-y0)*w;
	const float d = (val - 0.45f) * 1.8f;
	return d > 0.0f ? (d > 1.0f ? 1.0f : d) : 0.0f;
}
// Inverted Worley F1: distance (cell units) to the nearest jittered feature
// point over the 3×3×3 neighborhood; density = smoothstep of (1−F1−t)/(1−t),
// so it peaks AT scattered points and falls off radially → round puffy masses
// (value noise's iso-bands read as caustic veins instead). One cellHash per
// cell yields the 3 jitter components (10 bits each).
static inline float worleyNoiseAt(float wx, float wy, float wz, float invCell,
                                  float thresh, float invT)
{
	const float px = wx*invCell, py = wy*invCell, pz = wz*invCell;
	const int cx = int(std::floor(px));
	const int cy = int(std::floor(py));
	const int cz = int(std::floor(pz));
	float best = 1e9f;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx) {
				const int gx = cx+dx, gy = cy+dy, gz = cz+dz;
				const uint32_t h = cellHash(gx, gy, gz);
				const float ddx = float(gx) + float( h        & 1023u)*(1.0f/1024.0f) - px;
				const float ddy = float(gy) + float((h >> 10) & 1023u)*(1.0f/1024.0f) - py;
				const float ddz = float(gz) + float((h >> 20) & 1023u)*(1.0f/1024.0f) - pz;
				const float d2 = ddx*ddx + ddy*ddy + ddz*ddz;
				if (d2 < best) best = d2;
			}
	float d = (1.0f - std::sqrt(best) - thresh) * invT;
	if (d <= 0.0f) return 0.0f;
	if (d > 1.0f) d = 1.0f;
	return d*d*(3.0f - 2.0f*d);                  // rounded core, C1 zero at the edge
}

// Metaball-style ADDITIVE blob field: every neighboring cell's jittered blob
// contributes a C1 falloff (1-(d/R)²)² and overlaps SUM, so density piles up
// where blobs stack — "lots of overlapping big blobs" (clouds). Inverted
// Worley F1 cannot express this: nearest-distance is bounded by the lattice,
// so growing the radius fills space uniformly instead of overlapping. R is
// in cell units (capped 1.5 — the 3×3×3 search horizon); the iso threshold
// (reused fast_fog_worley_thresh) sets where fog begins out of the sum.
static inline float metaballNoiseAt(float wx, float wy, float wz, float invCell,
                                    float radius, float thresh, float invT)
{
	const float px = wx*invCell, py = wy*invCell, pz = wz*invCell;
	const int cx = int(std::floor(px));
	const int cy = int(std::floor(py));
	const int cz = int(std::floor(pz));
	const float invR2 = 1.0f / (radius*radius);
	float sum = 0.0f;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx) {
				const int gx = cx+dx, gy = cy+dy, gz = cz+dz;
				const uint32_t h = cellHash(gx, gy, gz);
				const float ddx = float(gx) + float( h        & 1023u)*(1.0f/1024.0f) - px;
				const float ddy = float(gy) + float((h >> 10) & 1023u)*(1.0f/1024.0f) - py;
				const float ddz = float(gz) + float((h >> 20) & 1023u)*(1.0f/1024.0f) - pz;
				const float t = 1.0f - (ddx*ddx + ddy*ddy + ddz*ddz) * invR2;
				if (t > 0.0f) sum += t*t;
			}
	float d = (sum - thresh) * invT;
	if (d <= 0.0f) return 0.0f;
	return d > 1.0f ? 1.0f : d;
}

// Froxel blob-field sample at one world point for a given octave (cell size).
// Worley/metaball puffs are size-modulated by a large-scale value-noise octave
// (2.7×, non-integer so it doesn't resonate with the feature lattice) — without
// it every cell grows an identical puff and the field reads as a polka-dot grid.
static inline float fogNoiseAt(const FastFogParams& P, float wx, float wy, float wz,
                               float cell, float invCell)
{
	fogWarp(P, wx, wy, wz);   // animate: wind drift + curl swirl (drives every octave)
	float d;
	if (P.blobOverlap > 0.0f)
		d = metaballNoiseAt(wx, wy, wz, invCell, P.blobOverlap,
		                    P.worleyThresh, P.worleyInvT);
	else if (P.worley)
		d = worleyNoiseAt(wx, wy, wz, invCell, P.worleyThresh, P.worleyInvT);
	else
		return blobNoiseAt(wx, wy, wz, cell, invCell);
	if (d <= 0.0f) return 0.0f;
	const float mc = cell * 2.7f;
	d *= 0.35f + blobNoiseAt(wx, wy, wz, mc, 1.0f/mc);
	return d > 1.0f ? 1.0f : d;
}

static inline float froxelDensity(const FastFogParams& P, float wx, float wy, float wz) {
	if (wy < P.slabY0 || wy > P.slabY1) return 0.0f;     // outside the slab
	float d = P.blobs ? fogNoiseAt(P, wx, wy, wz, P.cell, P.invCell) : 1.0f;
	// Feather the slab's Y-cutoff (same smoothstep ramp as the screen-space
	// path, but per sample point): without it the slab top/bottom is a razor
	// edge — a sharp horizontal fog ceiling. fast_fog_feather; auto = 20% of
	// slab thickness.
	if (d > 0.0f) {
		float lo = (wy - P.slabY0) * P.invFeather;
		float hi = (P.slabY1 - wy) * P.invFeather;
		lo = lo < 0.f ? 0.f : (lo > 1.f ? 1.f : lo);
		hi = hi < 0.f ? 0.f : (hi > 1.f ? 1.f : hi);
		d *= lo*lo*(3.0f-2.0f*lo) * hi*hi*(3.0f-2.0f*hi);
	}
	return d;
}

// Fused populate + front-to-back integrate, one pass per froxel column (the
// column is contiguous in memory). Stores accumulated in-scattered radiance
// (cam→slice) and transmittance T per froxel. Energy-conserving slice integral:
// for in-scattered radiance L and extinction σ over slice dz, ∫ L·T dz across
// the slice = L·T_in·(1−e^{−σ·dz}) (single-scatter σ_s=σ_t), then T *= e^{−σ·dz}.
//
// LIGHT GLOW IS INTEGRATED PER SLICE, NOT POINT-SAMPLED. Far slices are thick
// (exp distribution: ~130 units at z≈1400 with nz=64) while a lamp's kernel
// spike is ~0.22·range wide and the cone near its apex narrower still — a
// center point-sample of either gates the slice's whole contribution on/off
// as the grid slides through world space, so the glow re-shaped wildly under
// a 7-unit camera dolly. Instead, per column per light: clip the ray to the
// light's support once (lightRayClip — sphere ∩ cone ∩ forward half, sub-
// froxel-exact), then per overlapped slice take the EXACT radial integral
// 2/√disc·Δatan over slice∩[zLo,zHi] (boundary atans carried — one atan per
// slice). cutoff²·cone and the shadow tap stay point samples at the kernel
// peak clamped into the lit sub-interval (slowly varying / not integrable).
static constexpr int kFrMaxNz = 256;   // per-column stack scratch bound

// Coarse glow pass: the per-light glow loop from Froxel_ColumnTile, run once
// per COARSE column (div× fewer in each of X and Y → div² fewer light loops)
// at full z resolution, storing pure RADIANCE (no density gating — the fine
// populate multiplies by its own extinction, which is 0 in empty froxels).
// This is where the 30-omni city glow cost lives; radiance is low-frequency
// in XY so the bilinear upsample is visually free away from lamp cores.
static void Froxel_GlowTile(int cx0, int cy0, int cx1, int cy1, const FastFogParams& P) {
	const int nz = gFrZ;
	const float invGx = 1.0f/float(gGlX), invGy = 1.0f/float(gGlY);
	const float* zb = gFrZb.data();
	const float invLogR = float(nz) / std::log(gFrFar / gFrNear);
	const float invNear = 1.0f / gFrNear;
	const ViewLightsSoA* L = P.lights;
	for (int cy = cy0; cy < cy1; ++cy) {
		const float sy = (float(cy)+0.5f) * invGy * float(YRes);
		const float Y  = (CntrEY - sy) * P.invFOVY;
		for (int cx = cx0; cx < cx1; ++cx) {
			const float sx = (float(cx)+0.5f) * invGx * float(XRes);
			const float X  = (sx - CntrEX) * P.invFOVX;
			float* out = gGlow.data() + (size_t(cy)*gGlX + cx) * nz * 3;
			std::memset(out, 0, size_t(nz) * 3 * sizeof(float));
			const float uV = X*X + Y*Y + 1.0f;
			for (int li = 0; li < P.numLights; ++li) {
				if (L->mirrorId[li] != 0) continue;        // clones don't glow
				if (L->isFlash[li]) continue;              // transient: added per-frame in pass 2/3, not historied
				// Shadow-casting lights stay EXACT per fine column (pass 2 in
				// Froxel_ColumnTile): the shadow boundary inside the glow is
				// high-frequency and blocks up at coarse XY (conetest A/B).
				if (L->shadowMapIdx[li] >= 0) continue;
				const float Lx = L->posX[li], Ly = L->posY[li], Lz = L->posZ[li];
				const float VP = X*Lx + Y*Ly + Lz;
				const float PP = Lx*Lx + Ly*Ly + Lz*Lz;
				float zLo = zb[0], zHi = zb[nz];
				if (!lightRayClip(L, li, X, Y, uV, VP, PP, zLo, zHi))
					continue;                              // column never in-light
				const float rr2   = L->rRange[li] * L->rRange[li];
				const float alpha = rr2 * uV;
				const float beta  = -2.0f * rr2 * VP;
				const float gamma = rr2 * PP + 0.05f;
				const float disc  = 4.0f*alpha*gamma - beta*beta;
				if (disc <= 0.0f) continue;
				const float invD  = 1.0f / std::sqrt(disc);
				const float twoA  = alpha + alpha;
				const float zStar = VP / uV;
				const int   smi   = L->shadowMapIdx[li];
				int izLo = int(std::log(zLo * invNear) * invLogR) - 1;
				int izHi = int(std::log(zHi * invNear) * invLogR) + 1;
				if (izLo < 0)    izLo = 0;
				if (izHi > nz-1) izHi = nz-1;
				float aPrev = std::atan((twoA*zLo + beta) * invD);
				for (int iz = izLo; iz <= izHi; ++iz) {
					const float a = zb[iz]   > zLo ? zb[iz]   : zLo;
					const float b = zb[iz+1] < zHi ? zb[iz+1] : zHi;
					if (b <= a) continue;
					const float aCur = std::atan((twoA*b + beta) * invD);
					const float dAtan = aCur - aPrev;
					aPrev = aCur;
					float g = 2.0f * invD * dAtan / (zb[iz+1] - zb[iz]);
					if (g <= 0.0f) continue;
					const float zm = zStar < a ? a : (zStar > b ? b : zStar);
					float sShape = lightAttenAt(L, li, X*zm, Y*zm, zm);
					if (sShape <= 0.0f) continue;
					const float ddx = zm*X - Lx, ddy = zm*Y - Ly, ddz = zm - Lz;
					sShape *= (ddx*ddx + ddy*ddy + ddz*ddz) * rr2 + 0.05f;
					if (smi >= 0) {
						const float vis = volSpotShadow(smi, X*zm, Y*zm, zm, P.shadowPcf);
						if (vis <= 0.0f) continue;
						sShape *= vis;
					}
					g *= sShape;
					out[iz*3+0] += L->colR[li] * g;
					out[iz*3+1] += L->colG[li] * g;
					out[iz*3+2] += L->colB[li] * g;
				}
			}
		}
	}
}

static void Froxel_ColumnTile(int ix0, int iy0, int ix1, int iy1, const FastFogParams& P) {
	const int nx = gFrX, ny = gFrY, nz = gFrZ;
	const float invNx = 1.0f/float(nx), invNy = 1.0f/float(ny);
	const float* zb = gFrZb.data();                   // exp slice boundaries [nz+1]
	const float invLogR = float(nz) / std::log(gFrFar / gFrNear);   // z → slice idx
	const float invNear = 1.0f / gFrNear;
	const ViewLightsSoA* L = P.lights;
	const bool glowOn = P.inscatter > 0.0f && L && P.numLights > 0;
	// Temporal state (set by the dispatch): sample positions jittered by a
	// sub-froxel Halton offset IN XY ONLY; pass 3 reprojects each froxel's
	// CANONICAL (unjittered) center into the previous frame's grid and blends
	// history. NO z-jitter: far slices are hundreds of units thick, so a
	// ±half-slice offset swings the sampled blob density wildly, and the EMA
	// over a cycling jitter is a limit CYCLE, not a fixed point — ~(1−blend)
	// of that swing survives as permanent per-frame flicker even on a static
	// camera. The XY footprint is tiny (~11 units at z≈1400) so XY jitter —
	// the one that dissolves the grid stairs — leaves negligible ripple.
	const bool  temporal = gFrTemporal && gFrHistValid;
	const float blend = gFrBlend;
	const int   cur = gFrCur, prv = cur ^ 1;
	float*       sct  = gFrSct[cur].data();
	const float* hist = gFrSct[prv].data();
	const float fovX = 1.0f / P.invFOVX, fovY = 1.0f / P.invFOVY;
	const float nxOverXRes = float(nx) / float(XRes);
	const float nyOverYRes = float(ny) / float(YRes);
	const float invLog2R = invLogR * 0.6931472f;   // slice idx per log2 unit
	const float Ax = gFrPrevA[0], Ay = gFrPrevA[1], Az = gFrPrevA[2];
	// Per-COLUMN jitter phase: the global Halton index is offset by a hash of
	// the column, so neighbouring columns sit at different phases of the
	// 8-frame cycle. A GLOBAL jitter makes the whole fog field breathe
	// laterally in lockstep each frame — the EMA damps it to (1−blend) but a
	// coherent 20% shimmer still reads as flicker. Decorrelated phases turn
	// the same residual into fine spatial noise that the bilinear composite
	// and the blend average away; the converged mean is identical.
	static const float h2[8] = {1/2.f,1/4.f,3/4.f,1/8.f,5/8.f,3/8.f,7/8.f,1/16.f};
	static const float h3[8] = {1/3.f,2/3.f,1/9.f,4/9.f,7/9.f,2/9.f,5/9.f,8/9.f};
	float dens[kFrMaxNz];
	float glowR[kFrMaxNz], glowG[kFrMaxNz], glowB[kFrMaxNz];
	float flashGlowR[kFrMaxNz], flashGlowG[kFrMaxNz], flashGlowB[kFrMaxNz];  // transient (not historied)
	for (int iy = iy0; iy < iy1; ++iy) {
		const float syc = (float(iy)+0.5f) * invNy * float(YRes);
		const float Yc  = (CntrEY - syc) * P.invFOVY;
		for (int ix = ix0; ix < ix1; ++ix) {
			const uint32_t colPhase = gFrFrameIdx + cellHash(ix, iy, 0x5EED);
			// Canonical (unjittered) ray for the history reprojection. The
			// reprojected view pos is linear in slice depth: v = A + zc·B,
			// A = Rprevᵀ·(cam−camPrev) (per frame), B = Rprevᵀ·Dc (here).
			const float sxc = (float(ix)+0.5f) * invNx * float(XRes);
			const float Xc  = (sxc - CntrEX) * P.invFOVX;
			const float Dxc = P.w00*Xc + P.w01*Yc + P.w02;
			const float gYc = P.w10*Xc + P.w11*Yc + P.w12;
			const float Dzc = P.w20*Xc + P.w21*Yc + P.w22;
			const float Bx = gFrPrevW[0]*Dxc + gFrPrevW[3]*gYc + gFrPrevW[6]*Dzc;
			const float By = gFrPrevW[1]*Dxc + gFrPrevW[4]*gYc + gFrPrevW[7]*Dzc;
			const float Bz = gFrPrevW[2]*Dxc + gFrPrevW[5]*gYc + gFrPrevW[8]*Dzc;
			const size_t col = (size_t(iy)*nx + ix) * nz;

			// ── pass 1: blob/slab density at each slice center ──────────────
			// Sub-froxel XY jitter (the temporal supersample that dissolves
			// the coarse grid's stairs), with TWO cycle-taming rules learned
			// from the fog-top shimmer (user-bisected: jitter off = no
			// shimmer; finer grid / lower blend / far-z amplitude cap = no
			// change):
			//   • PER-SLICE phase, not per-column. A column-constant offset
			//     shifts every slice of the ray together, so at the grazing
			//     fog-top the whole path integral swings across the iso edge
			//     coherently each frame — the EMA's residual (~(1−blend) of
			//     the swing) reads as blobby horizon shimmer. With iz in the
			//     phase hash each slice cycles independently and the slice
			//     sum averages ~nz independent residuals (~√nz smaller, and
			//     spatially incoherent). The converged mean is identical.
			//   • WORLD-space amplitude = min(froxel footprint, cell/4):
			//     full sub-froxel near (the stairs live there), bounded far
			//     so one sample can never hop the field's whole transition.
			const float fpScale = float(XRes) * invNx * P.invFOVX;  // world units per froxel per z
			const float jcap    = 0.25f * P.cell;
			// 2-tap mode: average two HALF-CYCLE-APART phases per slice each
			// frame (k and k+4) — halves the jitter cycle's amplitude and
			// doubles convergence for ~2× the noise-field cost.
			const int taps = gFrTemporal ? P.taps : 1;
			const float invTaps = taps > 1 ? 0.5f : 1.0f;
			for (int iz = 0; iz < nz; ++iz) {
				const float z  = 0.5f * (zb[iz] + zb[iz+1]);
				const float dz = zb[iz+1] - zb[iz];
				const float fp = z * fpScale;
				const float jamp = fp < jcap ? fp : jcap;
				float d = 0.0f;
				for (int tap = 0; tap < taps; ++tap) {
					float jrx = 0.0f, jry = 0.0f, jrz = 0.0f;
					if (gFrTemporal) {
						// +iz walks the 8-phase Halton cycle along the ray, so
						// a path through ≥8 foggy slices covers ALL offsets
						// within one frame — stratified, not merely
						// decorrelated. (Antithetic sign-flip pairing was
						// tried and measured identical: the iso edge is
						// max(0,·)-clamped, so symmetric pairs don't cancel
						// through it.)
						const uint32_t k = (colPhase + uint32_t(iz) + uint32_t(tap)*4u) & 7u;
						const float jx = h2[k] - 0.5f, jy = h3[k] - 0.5f;
						jrx = jx*P.w00 + jy*P.w01;     // screen-right/up in world
						jry = jx*P.w10 + jy*P.w11;
						jrz = jx*P.w20 + jy*P.w21;
					}
					const float wx = P.camX + z*Dxc + jrx*jamp;
					const float wy = P.camY + z*gYc + jry*jamp;
					const float wz = P.camZ + z*Dzc + jrz*jamp;
					float dt = froxelDensity(P, wx, wy, wz);
					// Distance LOD: a far froxel spans many blob cells but
					// point-samples the cell=180 noise → aliases into bright/
					// dark blocks. Blend toward a COARSER octave (4× cell) by
					// footprint/cell, so distant fog keeps large-scale blob
					// masses and loses only the small-scale aliasing. Only for
					// in-slab blob froxels (the slab/gap zeros must stay zero).
					if (P.blobs && wy >= P.slabY0 && wy <= P.slabY1) {
						const float fpXY = z * (float(XRes)*invNx) * P.invFOVX;
						const float fpL = dz > fpXY ? dz : fpXY;
						float lod = (fpL - P.cell) * (1.0f/P.cell);
						lod = lod < 0.0f ? 0.0f : (lod > 1.0f ? 1.0f : lod);
						if (lod > 0.0f) {
							const float coarse = fogNoiseAt(P, wx, wy, wz, P.cell*4.0f, P.invCell*0.25f);
							dt += (coarse - dt) * lod;
						}
					}
					d += dt;
				}
				dens[iz] = d * invTaps;
			}

			// Coarse-glow-grid mode: bilinear corner pointers/weights for this
			// fine column into the gGlow grid (fetched per slice in pass 3).
			const bool glowGrid = glowOn && P.glowGridDiv > 1;
			const float* gl00 = nullptr; const float* gl10 = nullptr;
			const float* gl01 = nullptr; const float* gl11 = nullptr;
			float glwx = 0.0f, glwy = 0.0f;
			if (glowGrid) {
				const float fx = (float(ix)+0.5f) * float(gGlX) * invNx - 0.5f;
				const float fy = (float(iy)+0.5f) * float(gGlY) * invNy - 0.5f;
				int gx = int(std::floor(fx)); glwx = fx - float(gx);
				int gy = int(std::floor(fy)); glwy = fy - float(gy);
				if (gx < 0) { gx = 0; glwx = 0.0f; }
				if (gy < 0) { gy = 0; glwy = 0.0f; }
				if (gx > gGlX-2) { gx = gGlX > 1 ? gGlX-2 : 0; glwx = gGlX > 1 ? 1.0f : 0.0f; }
				if (gy > gGlY-2) { gy = gGlY > 1 ? gGlY-2 : 0; glwy = gGlY > 1 ? 1.0f : 0.0f; }
				const int gx1 = gx+1 < gGlX ? gx+1 : gx;
				const int gy1 = gy+1 < gGlY ? gy+1 : gy;
				gl00 = gGlow.data() + (size_t(gy )*gGlX + gx )*nz*3;
				gl10 = gGlow.data() + (size_t(gy )*gGlX + gx1)*nz*3;
				gl01 = gGlow.data() + (size_t(gy1)*gGlX + gx )*nz*3;
				gl11 = gGlow.data() + (size_t(gy1)*gGlX + gx1)*nz*3;
			}

			// ── pass 2: per-light glow per slice (clipped, analytic radial) ──
			// In glow-grid mode this still runs for SHADOW-CASTING lights
			// (exact shadow boundaries); unshadowed ones come from the grid.
			const bool pass2 = glowOn && (!glowGrid || gFrHasShadowedLight || gFrHasFlashLight);
			if (pass2) {
				for (int iz = 0; iz < nz; ++iz) {
					glowR[iz] = glowG[iz] = glowB[iz] = 0.0f;
					flashGlowR[iz] = flashGlowG[iz] = flashGlowB[iz] = 0.0f;
				}
				const float X = Xc, Y = Yc;            // glow is smooth — no jitter
				const float uV = X*X + Y*Y + 1.0f;
				for (int li = 0; li < P.numLights; ++li) {
					if (L->mirrorId[li] != 0) continue;        // clones don't glow
					if (glowGrid && L->shadowMapIdx[li] < 0 && !L->isFlash[li]) continue;  // grid covers it (but not the transient flash)
					const float Lx = L->posX[li], Ly = L->posY[li], Lz = L->posZ[li];
					const float VP = X*Lx + Y*Ly + Lz;
					const float PP = Lx*Lx + Ly*Ly + Lz*Lz;
					float zLo = zb[0], zHi = zb[nz];
					if (!lightRayClip(L, li, X, Y, uV, VP, PP, zLo, zHi))
						continue;                              // column never in-light
					const float rr2   = L->rRange[li] * L->rRange[li];
					const float alpha = rr2 * uV;
					const float beta  = -2.0f * rr2 * VP;
					const float gamma = rr2 * PP + 0.05f;
					const float disc  = 4.0f*alpha*gamma - beta*beta;   // > 0 (the +0.05)
					if (disc <= 0.0f) continue;
					const float invD  = 1.0f / std::sqrt(disc);
					const float twoA  = alpha + alpha;
					const float zStar = VP / uV;               // kernel peak (closest approach)
					const int   smi   = L->shadowMapIdx[li];
					// Slices overlapping [zLo,zHi] (log of the exp distribution;
					// widened ±1, the a/b clamp drops strays).
					int izLo = int(std::log(zLo * invNear) * invLogR) - 1;
					int izHi = int(std::log(zHi * invNear) * invLogR) + 1;
					if (izLo < 0)    izLo = 0;
					if (izHi > nz-1) izHi = nz-1;
					float aPrev = std::atan((twoA*zLo + beta) * invD);
					for (int iz = izLo; iz <= izHi; ++iz) {
						const float a = zb[iz]   > zLo ? zb[iz]   : zLo;
						const float b = zb[iz+1] < zHi ? zb[iz+1] : zHi;
						if (b <= a) continue;                  // outside [zLo,zHi]
						const float aCur = std::atan((twoA*b + beta) * invD);
						const float dAtan = aCur - aPrev;
						aPrev = aCur;
						if (dens[iz] <= 0.0f) continue;        // empty froxel
						float g = 2.0f * invD * dAtan / (zb[iz+1] - zb[iz]);
						if (g <= 0.0f) continue;
						// cutoff²·cone at the kernel peak clamped into the lit part
						// (lightAttenAt × (dr²+0.05) strips its radial factor).
						const float zm = zStar < a ? a : (zStar > b ? b : zStar);
						float s = lightAttenAt(L, li, X*zm, Y*zm, zm);
						if (s <= 0.0f) continue;
						const float ddx = zm*X - Lx, ddy = zm*Y - Ly, ddz = zm - Lz;
						s *= (ddx*ddx + ddy*ddy + ddz*ddz) * rr2 + 0.05f;
						if (smi >= 0) {
							const float vis = volSpotShadow(smi, X*zm, Y*zm, zm, P.shadowPcf);
							if (vis <= 0.0f) continue;
							s *= vis;
						}
						g *= s;
						if (L->isFlash[li]) {            // transient → separate (not historied)
							flashGlowR[iz] += L->colR[li] * g;
							flashGlowG[iz] += L->colG[li] * g;
							flashGlowB[iz] += L->colB[li] * g;
						} else {
							glowR[iz] += L->colR[li] * g;
							glowG[iz] += L->colG[li] * g;
							glowB[iz] += L->colB[li] * g;
						}
					}
				}
			}

			// ── pass 3: temporal blend, then front-to-back slice integral ────
			// Raw per-slice values are PREMULTIPLIED scatter (L·σ) + extinction
			// σ — both linear in the medium, so history blends correctly even
			// across empty↔dense froxel edges (L alone is undefined at σ=0).
			float Tc = 1.0f, accR = 0.0f, accG = 0.0f, accB = 0.0f;
			for (int iz = 0; iz < nz; ++iz) {
				const float d = dens[iz];
				float scR = 0.0f, scG = 0.0f, scB = 0.0f, ext = 0.0f;
				if (d > 0.0f) {
					ext = P.sigma * d;
					float Lr = P.fogR, Lg = P.fogG, Lb = P.fogB;   // ambient in-scatter
					if (glowGrid) {
						const int o = iz*3;
						const float w00 = (1-glwx)*(1-glwy), w10 = glwx*(1-glwy);
						const float w01 = (1-glwx)*glwy,     w11 = glwx*glwy;
						Lr += (gl00[o  ]*w00 + gl10[o  ]*w10 + gl01[o  ]*w01 + gl11[o  ]*w11) * P.inscatter;
						Lg += (gl00[o+1]*w00 + gl10[o+1]*w10 + gl01[o+1]*w01 + gl11[o+1]*w11) * P.inscatter;
						Lb += (gl00[o+2]*w00 + gl10[o+2]*w10 + gl01[o+2]*w01 + gl11[o+2]*w11) * P.inscatter;
					}
					if (pass2) {
						Lr += glowR[iz]*P.inscatter;
						Lg += glowG[iz]*P.inscatter;
						Lb += glowB[iz]*P.inscatter;
					}
					// Soft-knee radiance compressor: the lamp kernel peaks
					// ~20× (× inscatter) and clips white near lights. Exactly
					// linear below glowMax/2, asymptote at glowMax; scaled by
					// the max channel so hue is preserved. Density/extinction
					// untouched — this dims only what would clip.
					if (P.glowMax > 0.0f) {
						const float m = Lr > Lg ? (Lr > Lb ? Lr : Lb)
						                        : (Lg > Lb ? Lg : Lb);
						const float k = P.glowMax * 0.5f;
						if (m > k) {
							const float e = m - k;
							const float s = (k + e / (1.0f + e / k)) / m;
							Lr *= s; Lg *= s; Lb *= s;
						}
					}
					scR = Lr*ext; scG = Lg*ext; scB = Lb*ext;
				}
				const float zc  = 0.5f * (zb[iz] + zb[iz+1]);
				const float dzS0 = zb[iz+1] - zb[iz];
				// Skip the history fetch far outside the fog slab: density is
				// 0 there by construction (slab is world-static), so current
				// and history are both 0. Margin = 2 froxel extents so slab-
				// edge froxels (where blending smooths the edge) still blend.
				const float wyc = P.camY + zc*gYc;
				const float fpY = zc * (float(YRes)*invNy) * P.invFOVY;
				const float mar = 2.0f * (dzS0 > fpY ? dzS0 : fpY);
				if (temporal && wyc >= P.slabY0 - mar && wyc <= P.slabY1 + mar) {
					// Reproject the CANONICAL froxel center into the previous
					// frame's grid (v = A + zc·B, see above) and trilinearly
					// blend its history.
					const float vx = Ax + zc*Bx;
					const float vy = Ay + zc*By;
					const float vz = Az + zc*Bz;
					if (vz > 0.0f) {
						const float ivz = 1.0f / vz;
						float fx = (vx*ivz*fovX + CntrEX) * nxOverXRes - 0.5f;
						float fy = (CntrEY - vy*ivz*fovY) * nyOverYRes - 0.5f;
						float fz = frFastLog2(vz * invNear) * invLog2R - 0.5f;
						// Accept the outer HALF-froxel band and CLAMP into
						// the sample range instead of rejecting. A slice's
						// arithmetic-mean center sits past its log-space
						// midpoint (≈ iz+0.52 for this grid), so a hard
						// fz <= nz-1 test rejected slice nz-1 in EVERY
						// column — the last slice never blended history and
						// cycled at full jitter amplitude. Sky pixels
						// integrate through that slice → standing fog-top
						// shimmer at the skyline, immune to the blend weight
						// (the blend never ran there). The same off-by-half
						// on fy was the 1px dashed ripple at the very top
						// screen rows.
						if (fx >= -0.5f && fx <= float(nx)-0.5f &&
						    fy >= -0.5f && fy <= float(ny)-0.5f &&
						    fz >= -0.5f && fz <= float(nz)-0.5f) {
							fx = fx < 0.0f ? 0.0f : (fx > float(nx-1) ? float(nx-1) : fx);
							fy = fy < 0.0f ? 0.0f : (fy > float(ny-1) ? float(ny-1) : fy);
							fz = fz < 0.0f ? 0.0f : (fz > float(nz-1) ? float(nz-1) : fz);
							const int x0 = int(fx), y0 = int(fy), z0 = int(fz);
							const int x1 = x0+1 < nx ? x0+1 : x0;
							const int y1 = y0+1 < ny ? y0+1 : y0;
							const int z1 = z0+1 < nz ? z0+1 : z0;
							const float tx = fx-float(x0), ty = fy-float(y0), tz = fz-float(z0);
							// 8 interleaved float4 corners → 7 component-wise
							// lerps (vectorizes to one 128-bit lane each).
							const float* q000 = hist + ((size_t(y0)*nx + x0)*nz + z0)*4;
							const float* q001 = hist + ((size_t(y0)*nx + x0)*nz + z1)*4;
							const float* q100 = hist + ((size_t(y0)*nx + x1)*nz + z0)*4;
							const float* q101 = hist + ((size_t(y0)*nx + x1)*nz + z1)*4;
							const float* q010 = hist + ((size_t(y1)*nx + x0)*nz + z0)*4;
							const float* q011 = hist + ((size_t(y1)*nx + x0)*nz + z1)*4;
							const float* q110 = hist + ((size_t(y1)*nx + x1)*nz + z0)*4;
							const float* q111 = hist + ((size_t(y1)*nx + x1)*nz + z1)*4;
							float h4[4];
							for (int c = 0; c < 4; ++c) {
								const float a00 = q000[c] + (q001[c]-q000[c])*tz;
								const float a01 = q100[c] + (q101[c]-q100[c])*tz;
								const float a10 = q010[c] + (q011[c]-q010[c])*tz;
								const float a11 = q110[c] + (q111[c]-q110[c])*tz;
								const float a0 = a00 + (a01-a00)*tx;
								const float a1 = a10 + (a11-a10)*tx;
								h4[c] = a0 + (a1-a0)*ty;
							}
							scR += (h4[0] - scR) * blend;
							scG += (h4[1] - scG) * blend;
							scB += (h4[2] - scB) * blend;
							ext += (h4[3] - ext) * blend;
						}
					}
				}
				float* sc4 = sct + (col+iz)*4;
				sc4[0] = scR; sc4[1] = scG; sc4[2] = scB; sc4[3] = ext;
				// Add the TRANSIENT flash in-scatter for THIS frame only — after
				// storing history (so it never lingers / darkens panned edges)
				// and before the integral. Premultiplied by ext like scR.
				if (gFrHasFlashLight && ext > 0.0f) {
					float fLr = flashGlowR[iz]*P.inscatter;
					float fLg = flashGlowG[iz]*P.inscatter;
					float fLb = flashGlowB[iz]*P.inscatter;
					// Same hue-preserving soft-knee the base Lr gets — the split
					// otherwise lets the flash bypass it and white-out the fog.
					if (P.glowMax > 0.0f) {
						const float m = fLr > fLg ? (fLr > fLb ? fLr : fLb)
						                          : (fLg > fLb ? fLg : fLb);
						const float k = P.glowMax * 0.5f;
						if (m > k) {
							const float e = m - k;
							const float s = (k + e / (1.0f + e / k)) / m;
							fLr *= s; fLg *= s; fLb *= s;
						}
					}
					scR += fLr*ext; scG += fLg*ext; scB += fLb*ext;
				}
				// Integrate the BLENDED values: L = scat/σ, so the slice term
				// L·T·(1−e^{−σ·dz}) = scat·T·(1−e^{−σ·dz})/σ → scat·T·dz as σ→0.
				const float dzS = zb[iz+1] - zb[iz];
				if (ext > 1e-6f) {
					const float Topt = fastExpNeg(ext * dzS);
					const float w = Tc * (1.0f - Topt) / ext;
					accR += scR*w; accG += scG*w; accB += scB*w;
					Tc *= Topt;
				} else {
					const float w = Tc * dzS;
					accR += scR*w; accG += scG*w; accB += scB*w;
				}
				gFrAccR[col+iz] = accR; gFrAccG[col+iz] = accG;
				gFrAccB[col+iz] = accB; gFrT[col+iz] = Tc;
			}
		}
	}
}

// Per-channel TPDF dither (stable per pixel) for the froxel composite.
static inline float frDither(uint32_t s, float amp) {
	uint32_t h = s*0x9E3779B9u; h^=h>>15; h*=0x85EBCA6Bu; h^=h>>13;
	return (float(h&0xFFFFu)*(1.0f/65536.0f) + float((h>>16)&0xFFFFu)*(1.0f/65536.0f) - 1.0f) * amp;
}

// Composite: bilinear in XY, EXACT in depth. Integrating to the pixel's exact
// depth within its slice (not trilinear between slice centers) removes the
// z-slice bands on tilted surfaces. The partial in-slice in-scatter is derived
// from the stored acc difference scaled by the optical-depth fraction
// (1-e^{-σ·partialDz})/(1-e^{-σ·dzSlice}) — no per-slice radiance stored.
static void Froxel_CompositeTile(int x1, int y1, int x2, int y2, const FastFogParams& P) {
	const uint16_t* zEnc = ZPage16;
	dword* out = reinterpret_cast<dword*>(VPage);
	const int nx = gFrX, ny = gFrY, nz = gFrZ;
	const float fnx = float(nx)/float(XRes), fny = float(ny)/float(YRes);
	const float invLogFN = 1.0f / std::log(gFrFar / gFrNear);
	const float invNear  = 1.0f / gFrNear;
	const float* zb = gFrZb.data();
	const float* sctA = gFrSct[gFrCur].data();    // blended scatter+extinction (float4)
	// Read column (ix,iy): accPrev,Tprev (slice iz-1; iz=0 → 0,1), accCur, ext.
	auto col = [&](int ix, int iy, int iz, float* o) {
		const size_t ic = (size_t(iy)*nx + ix)*nz + iz;
		o[4]=gFrAccR[ic]; o[5]=gFrAccG[ic]; o[6]=gFrAccB[ic]; o[7]=sctA[ic*4+3];
		if (iz > 0) { const size_t ip=ic-1; o[0]=gFrAccR[ip];o[1]=gFrAccG[ip];o[2]=gFrAccB[ip];o[3]=gFrT[ip]; }
		else { o[0]=o[1]=o[2]=0.0f; o[3]=1.0f; }
	};
	for (int py = y1; py < y2; ++py) {
		const size_t row = size_t(py) * size_t(XRes);
		const float fy = (float(py)+0.5f)*fny - 0.5f;
		int iy0 = int(std::floor(fy)); float wy = fy - float(iy0);
		if (iy0 < 0) { iy0 = 0; wy = 0.0f; } if (iy0 >= ny-1) { iy0 = ny>1?ny-2:0; wy = ny>1?1.0f:0.0f; }
		const int iy1 = std::min(iy0+1, ny-1);
		for (int px = x1; px < x2; ++px) {
			const size_t i = row + size_t(px);
			const uint16_t ze = zEnc[i];
			// zEnc==0 pixels are sky OR the water's reflection underlay
			// (the transparent peel writes no Z). Default: fog at gFrFar.
			// When the scene provides the reflection pass's (displaced)
			// depth, fog along the reflected path's TWO legs. Sampling
			// the main grid at the mirrored depth is NOT enough: the
			// main (downward) ray exits the slab bottom shortly below
			// the waterline, so its integral saturates and any mirrored
			// depth beyond that reads the same fog. Split instead:
			//   leg 1 camera→water: the normal grid path, at the ray's
			//          analytic water-plane depth z_w;
			//   leg 2 water→building (length z_m − z_w, UP-going, ray's
			//          Y flipped about the plane): closed-form slab
			//          integral (SkyPaint's), folded in after the fetch
			//          as T = T1·T2, acc = acc1 + T1·acc2.
			float z;
			float reflAmt2 = 0.0f;             // leg-2 fog amount (0 = none)
			if (ze == 0) {
				z = gFrFar;
				if (gFrReflZ) {
					const uint16_t zr = gFrReflZ[i];
					if (zr) {
						const float zm = float(0xFF80 - int(zr)) * P.invZScale;
						if (zm > 0.0f && zm < gFrFar) {
							const float Xc = (float(px) - CntrEX) * P.invFOVX;
							const float Yc = (CntrEY - float(py)) * P.invFOVY;
							const float gY = P.w10*Xc + P.w11*Yc + P.w12;
							// Main-ray depth of the water plane.
							float zw = zm;
							if (gY < -1e-6f) {
								const float t = (gFrReflWaterY - P.camY) / gY;
								if (t > 0.0f && t < zm) zw = t;
							}
							z = zw < gFrNear ? gFrNear : zw;
							const float L = zm - zw;
							if (L > 0.0f) {
								const float gYu = -gY;     // up-going leg
								float sB = L;
								if (gYu > 1e-6f) {
									const float sExit =
									    (P.slabY1 - gFrReflWaterY) / gYu;
									if (sExit < sB) sB = sExit;
								}
								if (sB > 0.0f) {
									const float uV = Xc*Xc + Yc*Yc + 1.0f;
									const float m  = P.kHeight*gYu + P.invRf;
									const float hb = (P.kHeight != 0.0f)
									    ? fastExpNeg(P.kHeight * gFrReflWaterY)
									    : 1.0f;
									const float meanD = P.blobs ? 0.22f : 1.0f;
									const float tau = P.sigma * meanD
									    * std::sqrt(uV) * hb
									    * fogAntiderivG(sB, m);
									reflAmt2 = 1.0f - fastExpNeg(tau);
									if (reflAmt2 > 1.0f) reflAmt2 = 1.0f;
									if (reflAmt2 < 0.0f) reflAmt2 = 0.0f;
								}
							}
						}
					}
				}
			} else {
				const float zSurf = float(0xFF80 - int(ze)) * P.invZScale;
				z = zSurf > gFrFar ? gFrFar : zSurf;
			}
			if (z < gFrNear) z = gFrNear;
			const float fx = (float(px)+0.5f)*fnx - 0.5f;
			int ix0 = int(std::floor(fx)); float wx = fx - float(ix0);
			if (ix0 < 0) { ix0 = 0; wx = 0.0f; } if (ix0 >= nx-1) { ix0 = nx>1?nx-2:0; wx = nx>1?1.0f:0.0f; }
			const int ix1 = std::min(ix0+1, nx-1);
			int iz = int(std::log(z * invNear) * invLogFN * float(nz));
			if (iz < 0) iz = 0; if (iz >= nz) iz = nz-1;
			const float zb0 = zb[iz], dzSlice = zb[iz+1] - zb0;
			const float partialDz = z - zb0;
			// bilinear XY: accPrev(0..2), Tprev(3), accCur(4..6), ext(7)
			const float w00=(1-wx)*(1-wy), w10=wx*(1-wy), w01=(1-wx)*wy, w11=wx*wy;
			float acc[8] = {0,0,0,0,0,0,0,0}, c[8];
			col(ix0,iy0,iz,c); for(int k=0;k<8;++k) acc[k]+=c[k]*w00;
			col(ix1,iy0,iz,c); for(int k=0;k<8;++k) acc[k]+=c[k]*w10;
			col(ix0,iy1,iz,c); for(int k=0;k<8;++k) acc[k]+=c[k]*w01;
			col(ix1,iy1,iz,c); for(int k=0;k<8;++k) acc[k]+=c[k]*w11;
			const float ext = acc[7];
			float aR,aG,aB,Tpix;
			if (ext > 1e-8f) {
				const float ToptPart = fastExpNeg(ext * partialDz);
				const float ToptFull = fastExpNeg(ext * dzSlice);
				const float denom = 1.0f - ToptFull;
				const float frac = denom > 1e-6f ? (1.0f - ToptPart) / denom : 0.0f;
				aR = acc[0] + (acc[4]-acc[0])*frac;
				aG = acc[1] + (acc[5]-acc[1])*frac;
				aB = acc[2] + (acc[6]-acc[2])*frac;
				Tpix = acc[3] * ToptPart;
			} else { aR=acc[0]; aG=acc[1]; aB=acc[2]; Tpix=acc[3]; }
			// Reflection leg 2 (water→building): ambient in-scatter +
			// extinction folded behind leg 1's transmittance.
			if (reflAmt2 > 0.0f) {
				aR += Tpix * P.fogR * reflAmt2;
				aG += Tpix * P.fogG * reflAmt2;
				aB += Tpix * P.fogB * reflAmt2;
				Tpix *= 1.0f - reflAmt2;
			}
			const dword pix = out[i];
			if (P.hdr) {
				// HDR: unclamped lit·T + in-scatter → radiance buffer (no dither/
				// clamp; the tonemap rolls off later). g_hdrBuf is B,G,R,(pad)
				// per pixel, contiguous (same i as VPage).
				//
				// HDR B1: take the scene radiance from g_hdrBuf where the deferred
				// kernel wrote it UNCLAMPED (coverage flag h[3] > 0) so bright
				// surfaces bloom; fall back to the 8-bit VPage where it didn't —
				// sky / forward content (skycube, reflective windows, additive
				// vortex) only ever lands in VPage.
				fds::hdrf* h = fds::g_hdrBuf.data() + i*4;
				float scnB, scnG, scnR;
				if (h[3] > 0.0f) { scnB = h[0]; scnG = h[1]; scnR = h[2]; }
				else { scnR = float((pix>>16)&0xFFu); scnG = float((pix>>8)&0xFFu); scnB = float(pix&0xFFu); }
				h[2] = fds::HdrClamp(scnR*Tpix + aR);
				h[1] = fds::HdrClamp(scnG*Tpix + aG);
				h[0] = fds::HdrClamp(scnB*Tpix + aB);
			} else {
			const float da = P.ditherAmp; const uint32_t sd = uint32_t(i);
				int nR = int(float((pix>>16)&0xFFu)*Tpix + aR + frDither(sd, da));
			int nG = int(float((pix>> 8)&0xFFu)*Tpix + aG + frDither(sd^0x68E31DA4u, da));
			int nB = int(float( pix     &0xFFu)*Tpix + aB + frDither(sd^0xB5297A4Du, da));
				if (nR<0)nR=0; if (nG<0)nG=0; if (nB<0)nB=0;
			if (nR>255)nR=255; if (nG>255)nG=255; if (nB>255)nB=255;
			out[i] = (dword(nR)<<16)|(dword(nG)<<8)|dword(nB)|0xFF000000u;
			}
		}
	}
}

void Render_DeferredFastFog(const DeferredLightingCtx &ctx) {
	if (!fds::FeatureFlags::fast_fog()) return;
	if (!CurScene || !ZPage16 || !VPage) return;

	// FZP is the fog "far plane" reference for σ. Non-fogged scenes still
	// carry an FZP (the clip plane), so this works on clear scenes too.
	const float fogFar = CurScene->FZP;
	if (fogFar <= 0.0f) return;
	const float kHeight = fds::FeatureFlags::fast_fog_height();

	const DeferredLightingCtx &dc = ctx;
	const float (*w2)[3] = dc.viewToWorld;
	const float camY = dc.cameraWorldY;

	FastFogParams P{};
	P.invFOVX   = 1.0f / FOVX;
	P.invFOVY   = 1.0f / FOVY;
	P.invZScale = 1.0f / float(g_zscale);
	P.sigma     = fds::FeatureFlags::fast_fog_density() / fogFar;
	P.fogFar    = fogFar;
	P.kHeight   = kHeight;
	// exp(-k·y) referenced to world y=0; heightBase folds in the camera term.
	P.heightBase = (kHeight != 0.0f) ? fastExpNeg(kHeight * camY) : 1.0f;
	// viewToWorld rows — world ray dir = w·(X,Y,1). Row 1 (w1*) is gY.
	P.w00 = w2[0][0]; P.w01 = w2[0][1]; P.w02 = w2[0][2];
	P.w10 = w2[1][0]; P.w11 = w2[1][1]; P.w12 = w2[1][2];
	P.w20 = w2[2][0]; P.w21 = w2[2][1]; P.w22 = w2[2][2];
	P.camX = dc.cameraWorldX;
	P.camY = camY;
	P.camZ = dc.cameraWorldZ;
	// Slab bounds in world Y. Defaults (±1e9) → unbounded → plain height fog.
	P.slabY0 = fds::FeatureFlags::fast_fog_bottom();
	P.slabY1 = fds::FeatureFlags::fast_fog_top();
	P.fogR   = float(CurScene->Ambient.R);
	P.fogG   = float(CurScene->Ambient.G);
	P.fogB   = float(CurScene->Ambient.B);
	P.blobs  = fds::FeatureFlags::fast_fog_blobs();
	P.cell   = std::max(1.0f, fds::FeatureFlags::fast_fog_cell());
	P.invCell= 1.0f / P.cell;
	// ── Fog animation (drift + curl swirl) ──────────────────────────────
	// Scene time in seconds (g_FrameTime = centiseconds, 100 Hz; pause-aware,
	// and pinned to Timer in snapshots → deterministic per t). Drift is a
	// gentle horizontal wind expressed in cells/second so it reads the same at
	// any blob scale; the curl is a large-scale rotational warp spanning ~2
	// cells, rotating slowly. Subtle by design — fog creeps, it doesn't churn.
	{
		const float tsec = float(g_FrameTime) * 0.01f;
		P.driftX = (0.35f * P.cell) * tsec;     // ~0.35 cell/s downwind (X)
		P.driftY = 0.0f;                        // no vertical bulk drift
		P.driftZ = (0.15f * P.cell) * tsec;     // cross-flow (Z)
		P.swirlAmp   = 0.45f * P.cell;          // bounded warp (< 1 cell)
		P.swirlFreq  = 1.0f / (2.0f * P.cell);  // curl spans ~2 cells
		P.swirlPhase = 1.8f * tsec;             // ~1.8 rad/s rotation
	}
	P.jitter = fds::FeatureFlags::fast_fog_blob_jitter();
	P.worley = fds::FeatureFlags::fast_fog_worley();
	P.blobOverlap  = std::min(1.5f, fds::FeatureFlags::fast_fog_blob_overlap());
	P.hdr          = fds::FeatureFlags::hdr();
	// --hdr-glow-softknee: run the in-scatter soft-knee in HDR-linear with
	// fast_fog_glow_max as a physical radiance ceiling (auto-tracks the glow
	// level), instead of disabling it and flat-scaling by hdr_glow_scale below.
	// ACES rolls off on top. Falls back to a sensible 300 ceiling if unset.
	const bool hdrSoftKnee = P.hdr && fds::FeatureFlags::hdr_glow_softknee();
	if (hdrSoftKnee) {
		const float gm = fds::FeatureFlags::fast_fog_glow_max();
		P.glowMax = gm > 0.0f ? gm : 300.0f;
	} else {
		P.glowMax = P.hdr ? 0.0f : fds::FeatureFlags::fast_fog_glow_max();  // HDR: no soft-knee; the tonemap rolls off
	}
	P.glowGridDiv  = std::max(1, fds::FeatureFlags::fast_fog_glow_grid_div());
	P.taps         = std::min(2, std::max(1, fds::FeatureFlags::fast_fog_froxel_taps()));
	if (P.blobOverlap > 0.0f) {
		// Metaball sums exceed 1 where blobs stack (that's the point), so the
		// iso threshold ranges [0,3]; fog ramps to full over +0.7 above iso.
		P.worleyThresh = std::min(3.0f, std::max(0.0f, fds::FeatureFlags::fast_fog_worley_thresh()));
		P.worleyInvT   = 1.0f / 0.7f;
	} else {
		P.worleyThresh = std::min(0.9f, std::max(0.0f, fds::FeatureFlags::fast_fog_worley_thresh()));
		P.worleyInvT   = 1.0f / (1.0f - P.worleyThresh);
	}
	// Distance falloff: density *= exp(-z/Rf). 0 = auto (= FZP), so fog
	// thins toward the far plane instead of forming a wall there.
	const float Rf = fds::FeatureFlags::fast_fog_falloff();
	P.invRf  = 1.0f / (Rf > 0.0f ? Rf : fogFar);
	// In-scatter glow reuses the deferred light SoA (view-space positions,
	// colors, ranges, cone params already set up for the frame).
	P.inscatter = fds::FeatureFlags::fast_fog_inscatter();
	// HDR without the soft-knee: glow inputs were tuned against glowMax's cap,
	// which is off here, so flat-scale them. With --hdr-glow-softknee the knee
	// (re-enabled above) does the roll-off in linear instead, so skip the fudge.
	if (P.hdr && !hdrSoftKnee) P.inscatter *= fds::FeatureFlags::hdr_glow_scale();  // glowMax cap off in HDR — compensate
	P.inscatterSamples  = std::max(1, fds::FeatureFlags::fast_fog_inscatter_samples());
	P.inscatterAnalytic = fds::FeatureFlags::fast_fog_inscatter_analytic();
	P.inscatterJitter   = fds::FeatureFlags::fast_fog_inscatter_jitter();
	P.shadowEarlyOut    = fds::FeatureFlags::fast_fog_shadow_earlyout();
	P.shadowAnalytic    = fds::FeatureFlags::fast_fog_shadow_analytic();
	P.shadowPcf         = std::max(0, fds::FeatureFlags::fast_fog_shadow_pcf());
	P.lights    = dc.lights;
	P.numLights = dc.numLights;
	const bool adaptive = fds::FeatureFlags::fast_fog_adaptive();
	P.coarseStep  = adaptive ? std::max(2, fds::FeatureFlags::fast_fog_adaptive_step()) : 2;
	P.adaptThresh = adaptive ? fds::FeatureFlags::fast_fog_adaptive_thresh() : 0.0f;
	P.ditherAmp   = fds::FeatureFlags::fast_fog_dither();
	// Slab edge feather: 0 = auto (20% of slab thickness). Unbounded slab →
	// huge thickness → huge feather → profile stays ~1 (no-op), as intended.
	const float fth = fds::FeatureFlags::fast_fog_feather();
	P.feather = (fth > 0.0f) ? fth : 0.2f * (P.slabY1 - P.slabY0);
	P.invFeather = 1.0f / P.feather;

	constexpr int numTilesX = 6;
	constexpr int numTilesY = 4;

	auto runTiles = [&](int w, int h, auto&& body) {
		const int tsx = (w + numTilesX - 1) / numTilesX;
		const int tsy = (h + numTilesY - 1) / numTilesY;
		renderns::tileCounter = 0;
		constexpr int n = numTilesX * numTilesY;
		dispatchIndexed(n, &renderns::tileDone, [&body, tsx, tsy, w, h](int t) {
			const int j = t / numTilesX, i = t - j * numTilesX;
			const int y1 = tsy * j, y2 = std::min(y1 + tsy, h);
			const int x1 = tsx * i, x2 = std::min(x1 + tsx, w);
			body(x1, y1, x2, y2);
		});
		for (int k = 0; k < n; ++k) renderns::tileDone.acquire();
	};

	if (fds::FeatureFlags::fast_fog_froxel()) {
		// Froxel path: populate+integrate the view-frustum grid, then composite.
		const int nx = std::max(1, fds::FeatureFlags::fast_fog_froxel_x());
		const int ny = std::max(1, fds::FeatureFlags::fast_fog_froxel_y());
		const int nz = std::min(kFrMaxNz, std::max(2, fds::FeatureFlags::fast_fog_froxel_z()));
		if (gFrX != nx || gFrY != ny || gFrZ != nz) {
			gFrX = nx; gFrY = ny; gFrZ = nz;
			const size_t n = size_t(nx) * size_t(ny) * size_t(nz);
			gFrAccR.assign(n, 0.0f); gFrAccG.assign(n, 0.0f);
			gFrAccB.assign(n, 0.0f); gFrT.assign(n, 1.0f);
			gFrZb.assign(size_t(nz)+1, 0.0f);
			gFrSct[0].assign(n*4, 0.0f); gFrSct[1].assign(n*4, 0.0f);
			gFrHistValid = false;          // grid changed → history invalid
		}
		const float newNear = std::max(1.0f, CurScene->NZP);
		if (newNear != gFrNear || fogFar != gFrFar) gFrHistValid = false;
		gFrNear = newNear;
		gFrFar  = fogFar;
		// History is only meaningful within one continuously-viewed scene —
		// across a scene change the world the history sampled no longer
		// exists (it would blend the previous scene's fog into this one's
		// first frames if grid dims/near/far happen to match).
		static const Scene* sceneOfHistory = nullptr;
		if (CurScene != sceneOfHistory) { gFrHistValid = false; sceneOfHistory = CurScene; }
		// Slice boundaries z_b(i) = near·(far/near)^(i/nz) — precomputed so the
		// composite reads them instead of an exp per pixel.
		{
			const float rr = std::pow(gFrFar/gFrNear, 1.0f/float(nz));
			float zbv = gFrNear;
			for (int k = 0; k <= nz; ++k) { gFrZb[k] = zbv; zbv *= rr; }
		}
		// Temporal supersampling: Halton(2,3,5) sub-froxel jitter this frame;
		// after the populate, this frame's camera/rotation + blended grid
		// become the history the NEXT frame reprojects against.
		gFrTemporal = fds::FeatureFlags::fast_fog_froxel_temporal();
		gFrBlend    = std::min(0.95f, std::max(0.0f, fds::FeatureFlags::fast_fog_froxel_blend()));
		// (Per-column XY jitter phases are derived in the column tile from
		// gFrFrameIdx + a column hash — see Froxel_ColumnTile.)
		if (gFrHistValid) {
			// Per-frame constant of the reprojection: A = Rprevᵀ·(cam−camPrev).
			const float dx = P.camX - gFrPrevCamX, dy = P.camY - gFrPrevCamY,
			            dz = P.camZ - gFrPrevCamZ;
			gFrPrevA[0] = gFrPrevW[0]*dx + gFrPrevW[3]*dy + gFrPrevW[6]*dz;
			gFrPrevA[1] = gFrPrevW[1]*dx + gFrPrevW[4]*dy + gFrPrevW[7]*dz;
			gFrPrevA[2] = gFrPrevW[2]*dx + gFrPrevW[5]*dy + gFrPrevW[8]*dz;
		}
		gFrHasShadowedLight = false;
		gFrHasFlashLight    = false;
		if (P.lights)
			for (int li = 0; li < P.numLights; ++li) {
				if (P.lights->shadowMapIdx[li] >= 0 && P.lights->mirrorId[li] == 0)
					gFrHasShadowedLight = true;
				if (P.lights->isFlash[li])
					gFrHasFlashLight = true;
			}
		if (P.glowGridDiv > 1 && P.inscatter > 0.0f && P.numLights > 0) {
			const int gx = (nx + P.glowGridDiv - 1) / P.glowGridDiv;
			const int gy = (ny + P.glowGridDiv - 1) / P.glowGridDiv;
			if (gx != gGlX || gy != gGlY ||
			    gGlow.size() != size_t(gx)*size_t(gy)*size_t(nz)*3) {
				gGlX = gx; gGlY = gy;
				gGlow.assign(size_t(gx)*size_t(gy)*size_t(nz)*3, 0.0f);
			}
			runTiles(gGlX, gGlY, [&](int a,int b,int c,int d){ Froxel_GlowTile(a,b,c,d,P); });
		}
		runTiles(nx, ny, [&](int a,int b,int c,int d){ Froxel_ColumnTile(a,b,c,d,P); });
		runTiles(XRes, YRes, [&](int a,int b,int c,int d){ Froxel_CompositeTile(a,b,c,d,P); });
		// This is the only path that populates g_hdrBuf; mark it so the tonemap
		// runs (and doesn't blacken scenes/frames where the froxel composite
		// never ran — e.g. greets with fog off, which would otherwise tonemap a
		// cleared buffer to black).
		if (P.hdr) fds::g_hdrActive = true;
		gFrReflZ = nullptr;            // consume-once (see FastFog_SetReflectionZ)
		gFrFrameActive = fds::FeatureFlags::fast_fog_xpar();   // peel fogs from this grid
		// This frame becomes next frame's history.
		gFrPrevCamX = P.camX; gFrPrevCamY = P.camY; gFrPrevCamZ = P.camZ;
		gFrPrevW[0] = P.w00; gFrPrevW[1] = P.w01; gFrPrevW[2] = P.w02;
		gFrPrevW[3] = P.w10; gFrPrevW[4] = P.w11; gFrPrevW[5] = P.w12;
		gFrPrevW[6] = P.w20; gFrPrevW[7] = P.w21; gFrPrevW[8] = P.w22;
		gFrHistValid = true;
		gFrCur ^= 1;
		++gFrFrameIdx;
		return;
	}

	// Screen-space path runs: arm the transparent peel's per-pixel fog hook
	// (the froxel branch above returned already; it has its own grid hook).
	gSSFogP = P;
	gSSFogActive = fds::FeatureFlags::fast_fog_xpar();

	if (adaptive || fds::FeatureFlags::fast_fog_halfres()) {
		// Downsampled compute on a coarse grid (step = coarseStep), then
		// either bilateral upsample (half-res) or adaptive refine (recompute
		// only at edges). Fog is low-frequency, so this is near-free quality.
		const int S = P.coarseStep;
		const int cw = (XRes + S - 1) / S + 1, ch = (YRes + S - 1) / S + 1;
		if (gFogHW != cw || gFogHH != ch) {
			gFogHW = cw; gFogHH = ch;
			const size_t n = size_t(cw) * ch;
			gFogAmt.assign(n, 0.0f); gFogZ.assign(n, 0.0f);
			gFogGR.assign(n, 0.0f); gFogGG.assign(n, 0.0f); gFogGB.assign(n, 0.0f);
		}
		runTiles(cw, ch, [&](int a,int b,int c,int d){ Render_DeferredFastFog_HalfTile(a,b,c,d,P); });
		if (adaptive)
			runTiles(XRes, YRes, [&](int a,int b,int c,int d){ Render_DeferredFastFog_RefineTile(a,b,c,d,P); });
		else
			runTiles(XRes, YRes, [&](int a,int b,int c,int d){ Render_DeferredFastFog_CompositeTile(a,b,c,d,P); });
	} else {
		runTiles(XRes, YRes, [&](int a,int b,int c,int d){ Render_DeferredFastFog_Tile(a,b,c,d,P); });
	}
}

// Stand-in for the fast-fog pass on render passes that SKIP the froxel path
// (the city reflection pass): tint every pixel the rasterizer never touched
// (zEnc == 0) toward the fog color by the slab-clipped analytic fog amount
// for that pixel's ray — the same [zA,zB] slab integral the screen-space
// path uses, evaluated with this pass's (mirror) camera. The base under the
// tint is the freshly drawn skybox (the city frame loop clears VPage before
// RenderSkyCube — see CITY.CPP; without that clear the base is STALE
// framebuffer and no tint can hide it). Slab-clipped τ, not the saturated
// far-plane amount: an upward mirrored ray exits the thin slab top quickly —
// painting the full 1−e^{−density} made the whole mirrored sky glare
// ambient-bright, which the water reflected as "white below the waterline".
void Render_DeferredFastFogSkyPaint(const DeferredLightingCtx &ctx) {
	if (!CurScene || !ZPage16 || !VPage) return;
	const float fogFar = CurScene->FZP;
	if (fogFar <= 0.0f) return;
	const DeferredLightingCtx &dc = ctx;
	const float (*w2)[3] = dc.viewToWorld;
	const float camY    = dc.cameraWorldY;
	const float kHeight = fds::FeatureFlags::fast_fog_height();
	const float sigma   = fds::FeatureFlags::fast_fog_density() / fogFar;
	const float slabY0  = fds::FeatureFlags::fast_fog_bottom();
	const float slabY1  = fds::FeatureFlags::fast_fog_top();
	const float Rf      = fds::FeatureFlags::fast_fog_falloff();
	const float invRf   = 1.0f / (Rf > 0.0f ? Rf : fogFar);
	const float fth     = fds::FeatureFlags::fast_fog_feather();
	const float feather = (fth > 0.0f) ? fth : 0.2f * (slabY1 - slabY0);
	const float heightBase = (kHeight != 0.0f) ? fastExpNeg(kHeight * camY) : 1.0f;
	// Blob fields fill the slab only partially — the remapped value noise and
	// the thresholded worley both average ~0.2 of the smooth-slab density.
	// Without this the painted reflection sky carries 4-5x the fog of the
	// real field around it and glares bright. (A point estimate; the real
	// field can't be marched here without paying the full fog pass.)
	const float meanDens = fds::FeatureFlags::fast_fog_blobs() ? 0.22f : 1.0f;
	const float fogR = float(CurScene->Ambient.R);
	const float fogG = float(CurScene->Ambient.G);
	const float fogB = float(CurScene->Ambient.B);
	const float invFOVX = 1.0f / FOVX, invFOVY = 1.0f / FOVY;
	const uint16_t* zEnc = ZPage16;
	dword* out = reinterpret_cast<dword*>(VPage);

	constexpr int numTilesX = 6, numTilesY = 4;
	const int tsx = (XRes + numTilesX - 1) / numTilesX;
	const int tsy = (YRes + numTilesY - 1) / numTilesY;
	{
		constexpr int nJobs = numTilesX * numTilesY;
		dispatchIndexed(nJobs, &renderns::tileDone, [=](int t) {
			const int tj = t / numTilesX, ti = t - tj * numTilesX;
			const int y1 = tsy*tj, y2 = std::min(y1+tsy, (int)YRes);
			const int x1 = tsx*ti, x2 = std::min(x1+tsx, (int)XRes);
			{
				for (int py = y1; py < y2; ++py) {
					const float Y = (CntrEY - float(py)) * invFOVY;
					const size_t row = size_t(py) * size_t(XRes);
					for (int px = x1; px < x2; ++px) {
						const size_t i = row + size_t(px);
						if (zEnc[i] != 0) continue;
						const float X  = (float(px) - CntrEX) * invFOVX;
						const float uV = X*X + Y*Y + 1.0f;
						const float gY = w2[1][0]*X + w2[1][1]*Y + w2[1][2];
						// Slab clip (same as fogAtPixel) on [0, fogFar].
						float zA = 0.0f, zB = fogFar;
						if (gY > 1e-9f || gY < -1e-9f) {
							float za = (slabY0 - camY) / gY;
							float zb = (slabY1 - camY) / gY;
							if (za > zb) { const float t = za; za = zb; zb = t; }
							zA = za > 0.0f ? za : 0.0f;
							zB = zb < fogFar ? zb : fogFar;
						} else if (camY < slabY0 || camY > slabY1) {
							continue;                          // level ray outside slab
						}
						float tau = 0.0f;
						if (zB > zA) {
							const float m = kHeight*gY + invRf;
							const float dens = heightBase *
								(fogAntiderivG(zB, m) - fogAntiderivG(zA, m));
							tau = sigma * meanDens * std::sqrt(uV) * dens;
							if (feather > 0.0f) {
								const float wy = camY + gY * (0.5f*(zA+zB));
								const float invF = 1.0f / feather;
								float lo = (wy-slabY0)*invF; lo = lo<0.f?0.f:(lo>1.f?1.f:lo);
								float hi = (slabY1-wy)*invF; hi = hi<0.f?0.f:(hi>1.f?1.f:hi);
								tau *= lo*lo*(3.f-2.f*lo) * hi*hi*(3.f-2.f*hi);
							}
						}
						if (tau <= 0.0f) continue;
						float amt = 1.0f - fastExpNeg(tau);
						if (amt > 1.0f) amt = 1.0f;
						const float keep = 1.0f - amt;
						const dword pix = out[i];
						int nR = int(float((pix>>16)&0xFFu)*keep + fogR*amt);
						int nG = int(float((pix>> 8)&0xFFu)*keep + fogG*amt);
						int nB = int(float( pix     &0xFFu)*keep + fogB*amt);
						if (nR > 255) nR = 255; if (nG > 255) nG = 255; if (nB > 255) nB = 255;
						out[i] = (dword(nR)<<16)|(dword(nG)<<8)|dword(nB)|0xFF000000u;
					}
				}
			}
		});
		for (int k = 0; k < nJobs; ++k) renderns::tileDone.acquire();
	}
}

// ─── Screen-space rain ────────────────────────────────────────────────────
// Procedural streaks as a tile-parallel post pass — no particles, no face
// list. Three layers at fixed view depths give parallax: each layer is a
// sheared screen-space grid of columns; a column hash decides which cells
// carry a streak this cycle, the in-cell fraction gives the streak's
// vertical alpha ramp (faint head → bright tail, like the 1998 rainmapper).
// Per pixel per layer it's ~a dozen ALU + 2 hashes; streak-covered pixels
// (the rare case) additionally z-test against the surface depth and fetch
// fog transmittance at the LAYER's depth, so distant rain dims into the
// soup instead of punching through it. Scene time (g_FrameTime) drives the
// fall — pause freezes rain mid-air like everything else.
void Render_ScreenSpaceRain() {
	if (!fds::FeatureFlags::rain()) return;
	const float intensity = fds::FeatureFlags::rain_intensity();
	if (intensity <= 0.0f || !ZPage16 || !VPage) return;

	// Timer ticks at 100 Hz (TimerInit(100) in REV.CPP) — centiseconds.
	const float t = float(g_FrameTime) * 0.01f * fds::FeatureFlags::rain_speed();
	// Wind: shared slant (px of x drift per px of y), slow compound gusts.
	const float slant = 0.14f + 0.09f*std::sin(t*0.37f) + 0.05f*std::sin(t*0.83f);
	const float density = intensity > 1.0f ? 1.0f : intensity;  // streak probability
	const float opacity = (intensity > 1.0f ? intensity : 1.0f) * 0.8f; // >1 = heavier look
	const float invZScale = 1.0f / float(g_zscale);
	const float fogFar = CurScene ? CurScene->FZP : 0.0f;
	const bool froxelFog = FastFog_XparActive();
	const bool ssFog     = !froxelFog && FastFog_SSActive();
	const bool legacyFog = !froxelFog && !ssFog && CurScene
	                       && (CurScene->Flags & Scn_Fogged) && fogFar > 0.0f;

	struct Layer {
		float depth;     // assumed view depth (world units) — z-test + fog
		float cellW;     // column pitch, px
		float cellH;     // vertical streak pitch, px
		float lenFrac;   // streak length as fraction of cellH
		float speed;     // cells per second
		float alpha;     // peak opacity
	};
	// Near layer: long sparse bright streaks. Far: short dense faint ones.
	static const Layer L[3] = {
		{  500.0f, 36.0f, 96.0f, 0.55f, 22.0f, 0.55f },
		{ 1400.0f, 22.0f, 64.0f, 0.45f, 16.0f, 0.40f },
		{ 3200.0f, 13.0f, 42.0f, 0.38f, 11.0f, 0.28f },
	};
	// Rain color: cool pale blue, alpha-blended (reads on bright AND dark).
	const float rainR = 165.0f, rainG = 185.0f, rainB = 225.0f;

	const uint16_t* zEnc = ZPage16;
	dword* out = reinterpret_cast<dword*>(VPage);

	constexpr int numTilesX = 6, numTilesY = 4;
	const int tsx = (XRes + numTilesX - 1) / numTilesX;
	const int tsy = (YRes + numTilesY - 1) / numTilesY;
	{
		constexpr int nJobs = numTilesX * numTilesY;
		dispatchIndexed(nJobs, &renderns::tileDone, [=](int tt) {
			const int tj = tt / numTilesX, ti = tt - tj * numTilesX;
			const int y1 = tsy*tj, y2 = std::min(y1+tsy, (int)YRes);
			const int x1 = tsx*ti, x2 = std::min(x1+tsx, (int)XRes);
			{
				// COLUMN-major: streaks are sparse (one core ≤3 px wide per
				// cellW-px column), so iterate the ~tileW/cellW columns per
				// row and touch only each streak's own pixels. ~12× less
				// work than testing every pixel of the tile.
				for (int li = 0; li < 3; ++li) {
					const Layer& l = L[li];
					const float invCellW = 1.0f / l.cellW;
					const float invCellH = 1.0f / l.cellH;
					const float scroll   = t * l.speed;
					const int   salt     = 0x9A1B + li*0x611;
					for (int py = y1; py < y2; ++py) {
						const size_t row = size_t(py) * size_t(XRes);
						const float shear = float(py) * slant;     // px of x at this row
						// Columns whose cores can land in [x1, x2):
						// px = (col + xj)·cellW + shear, xj ∈ [0.15, 0.85].
						const int c0 = int(std::floor((float(x1) - shear) * invCellW)) - 1;
						const int c1 = int(std::floor((float(x2) - shear) * invCellW)) + 1;
						const float vRow = float(py)*invCellH;
						for (int col = c0; col <= c1; ++col) {
							const uint32_t ch = cellHash(col, salt, 0);
							// Column personality: streak x within the column,
							// fall phase, speed wobble.
							const float xj  = 0.15f + float(ch & 0xFFu)*(0.7f/255.0f);
							const float ph  = float((ch >> 8) & 0xFFFu)*(1.0f/4096.0f);
							const float spd = 0.85f + float((ch >> 20) & 0xFFu)*(0.3f/255.0f);
							// MINUS scroll: v grows with py (down-screen), so
							// the streak window must move to LARGER py over
							// time — py = (v0 − vScroll)·cellH would climb;
							// subtracting makes the pattern fall.
							const float v   = vRow - scroll*spd + ph;
							const int   vc  = int(std::floor(v));
							const float fv  = v - float(vc);
							if (fv > l.lenFrac) continue;
							// Cell occupancy: density of streaks this cycle.
							if (float(cellHash(col, vc, salt) & 0xFFFFu)*(1.0f/65536.0f)
							    >= density) continue;
							// Vertical ramp (head faint → tail bright) ×
							// layer/intensity; fog joins at the first pixel.
							float aBase = fv * (1.0f/l.lenFrac) * l.alpha * opacity;
							const float xCore = (float(col) + xj) * l.cellW + shear;
							int pxa = int(xCore - 1.0f); if (pxa < x1) pxa = x1;
							int pxb = int(xCore + 2.0f); if (pxb > x2) pxb = x2;
							bool fogged = false;
							for (int px = pxa; px < pxb; ++px) {
								const float dxp = float(px) - xCore;
								const float lat = 1.5f - (dxp > 0.0f ? dxp : -dxp);
								if (lat <= 0.0f) continue;
								const size_t i = row + size_t(px);
								const uint16_t ze = zEnc[i];
								if (ze != 0) {
									const float zSurf = float(0xFF80 - int(ze)) * invZScale;
									if (zSurf < l.depth) continue;  // behind geometry
								}
								if (!fogged) {
									// Fog T at the LAYER depth — once per
									// streak-row, it can't change across 3 px.
									fogged = true;
									if (froxelFog || ssFog) {
										float aR_, aG_, aB_, T_;
										if (froxelFog) FastFog_SampleGrid(px, py, l.depth, aR_, aG_, aB_, T_);
										else           FastFog_SSSample(px, py, l.depth, aR_, aG_, aB_, T_);
										aBase *= T_;
									} else if (legacyFog) {
										const float k = 1.0f - l.depth / fogFar;
										aBase *= k > 0.0f ? k : 0.0f;
									}
									if (aBase <= 0.003f) break;
								}
								const float a = aBase * lat * (1.0f/1.5f);
								const float keep = 1.0f - a;
								// HDR overlay reorg: rain is scene radiance — in HDR
								// alpha-blend in float into g_hdrBuf (captured by the
								// tonemap below/in the tick); else the 8-bit blend.
								if (fds::g_hdrActive) {
									fds::hdrf* h = fds::g_hdrBuf.data() + i*4;
									h[2] = h[2]*keep + rainR*a;   // R
									h[1] = h[1]*keep + rainG*a;   // G
									h[0] = h[0]*keep + rainB*a;   // B
								} else {
									const dword pix = out[i];
									int nR = int(float((pix>>16)&0xFFu)*keep + rainR*a);
									int nG = int(float((pix>> 8)&0xFFu)*keep + rainG*a);
									int nB = int(float( pix     &0xFFu)*keep + rainB*a);
									out[i] = (dword(nR)<<16)|(dword(nG)<<8)|dword(nB)|0xFF000000u;
								}
							}
						}
					}
				}
			}
		});
		for (int k = 0; k < nJobs; ++k) renderns::tileDone.acquire();
	}
}

// ─── On-camera lens droplets ──────────────────────────────────────────────
// Persistent water drops on the "lens" while it rains: each is a small disc
// that REFRACTS — shows a minified, inverted copy of the scene behind it
// (sampled from a snapshot of its own rect, so reads don't see writes) with
// a darkened rim and a specular glint. Drops spawn at a rate tied to
// rain_intensity, dwell stuck for a while, then trickle downward with a
// wobble, shrinking as they shed mass. All randomness is hash-seeded off a
// spawn counter (deterministic — paused captures stay byte-stable) and all
// motion integrates scene time, so pause freezes the trickle mid-slide.
// Single-threaded: a realistic pool is ~25 drops × ~1.5k px each.
namespace {
struct LensDrop {
	float x, y, r;
	float age, dwell, life;
	float vy, wobPh;
	uint32_t seed;
};
std::vector<LensDrop> gLensDrops;
float    gLensSpawnAcc  = 0.0f;
uint32_t gLensSpawnSeq  = 0x5EED;
int32_t  gLensPrevFT    = -1;
}

void Render_LensDrops() {
	if (!fds::FeatureFlags::rain() || !fds::FeatureFlags::rain_lens()) {
		gLensDrops.clear(); gLensPrevFT = -1;
		return;
	}
	const float intensity = fds::FeatureFlags::rain_intensity();
	if (!VPage) return;

	// Scene-time delta (Timer = centiseconds). Clamped: pause/scrub
	// rollback gives 0 (drops freeze), big forward scrubs cap at 100ms.
	float dt = 0.0f;
	if (gLensPrevFT >= 0) {
		const float d = float(g_FrameTime - gLensPrevFT) * 0.01f;
		dt = d < 0.0f ? 0.0f : (d > 0.1f ? 0.1f : d);
	}
	gLensPrevFT = g_FrameTime;

	auto h01 = [](uint32_t s, int k) {
		return float(cellHash(int(s), k, 0x10F5) & 0xFFFFu) * (1.0f/65536.0f);
	};

	// Spawn: ~5/s at intensity 1, pool-capped.
	gLensSpawnAcc += dt * 11.0f * (intensity < 2.0f ? intensity : 2.0f);
	while (gLensSpawnAcc >= 1.0f && gLensDrops.size() < 80) {
		gLensSpawnAcc -= 1.0f;
		const uint32_t s = gLensSpawnSeq++;
		LensDrop d;
		d.seed  = s;
		d.x     = h01(s, 1) * float(XRes);
		d.y     = h01(s, 2) * float(YRes) * 0.85f;
		d.r     = 4.0f + h01(s, 3) * 9.0f;
		d.age   = 0.0f;
		d.dwell = 0.25f + h01(s, 4) * 1.2f;
		d.life  = d.dwell + 2.0f + h01(s, 5) * 4.0f;
		d.vy    = 0.0f;
		d.wobPh = h01(s, 6) * 6.2832f;
		gLensDrops.push_back(d);
	}

	dword* out = reinterpret_cast<dword*>(VPage);
	static std::vector<dword> rect;       // per-drop source snapshot
	for (size_t di = 0; di < gLensDrops.size(); ) {
		LensDrop& d = gLensDrops[di];
		d.age += dt;
		if (d.age > d.dwell) {
			// Trickle: gravity-ish, wobble, shed mass.
			d.vy += 340.0f * dt;
			if (d.vy > 400.0f) d.vy = 400.0f;
			d.y  += d.vy * dt;
			d.x  += std::sin(d.age * 9.0f + d.wobPh) * d.r * 0.6f * dt;
			d.r  -= d.r * 0.16f * dt;
		}
		if (d.age > d.life || d.r < 2.0f || d.y - d.r > float(YRes)) {
			gLensDrops[di] = gLensDrops.back();
			gLensDrops.pop_back();
			continue;
		}
		// Fade in over the first 150 ms (condensation forming).
		const float fade = d.age < 0.15f ? d.age * (1.0f/0.15f) : 1.0f;
		// Shape: near-circular at rest (real condensation drops are NOT
		// the cartoon teardrop — just a hint of gravity sag), and when
		// sliding the TAIL trails ABOVE the drop (where it came from),
		// narrowing with height, while the leading bottom edge stays round.
		const float stretch = (d.vy * (1.0f/400.0f)) * 0.9f;   // 0..0.9
		const float kTop = 1.05f + stretch;                    // tail above
		const float kBot = 1.08f;                              // gentle sag
		// Refraction FOV: a drop compresses a WIDE view — sample 1.6× the
		// in-drop offset (inverted), so the snapshot rect must extend past
		// the drop by that reach.
		const float sK   = 1.6f;
		const float padX = d.r * (1.0f + sK) + 2.0f;
		const float padT = d.r * kTop * (1.0f + sK) + 2.0f;
		const float padB = d.r * kBot * (1.0f + sK) + 2.0f;
		const int cx  = int(d.x), cy = int(d.y);
		int xa = cx - int(padX), xb = cx + int(padX) + 1;
		int ya = cy - int(padT), yb = cy + int(padB) + 1;
		if (xa < 0) xa = 0; if (ya < 0) ya = 0;
		if (xb > (int)XRes) xb = (int)XRes; if (yb > (int)YRes) yb = (int)YRes;
		if (xa >= xb || ya >= yb) { ++di; continue; }
		const int rw = xb - xa, rh = yb - ya;
		rect.resize(size_t(rw) * size_t(rh));
		for (int y = 0; y < rh; ++y)
			std::memcpy(rect.data() + size_t(y)*rw,
			            out + size_t(ya+y)*XRes + xa,
			            size_t(rw) * sizeof(dword));
		const float invR  = 1.0f / d.r;
		const float invKB = 1.0f / kBot, invKT = 1.0f / kTop;
		// Specular glint sits up-left of center; small and soft.
		const float gx = d.x - d.r*0.30f, gy = d.y - d.r*0.30f;
		const float gr2 = d.r*d.r*0.025f;
		const int pya = cy - int(d.r*kTop) - 1 < ya ? ya : cy - int(d.r*kTop) - 1;
		const int pyb = cy + int(d.r*kBot) + 2 > yb ? yb : cy + int(d.r*kBot) + 2;
		const int pxa = cx - int(d.r) - 1 < xa ? xa : cx - int(d.r) - 1;
		const int pxb = cx + int(d.r) + 2 > xb ? xb : cx + int(d.r) + 2;
		for (int py = pya; py < pyb; ++py) {
			const float dyr = (float(py) - d.y) * invR;          // y in radii
			const float ny  = dyr * (dyr > 0.0f ? invKB : invKT);
			// Sliding tail narrows toward its tip (above); round at rest.
			const float taper = (dyr < 0.0f && stretch > 0.0f)
			    ? 1.0f + 1.2f*stretch*(-dyr*invKT) : 1.0f;
			for (int px = pxa; px < pxb; ++px) {
				const float dx_ = float(px) - d.x;
				const float nx  = dx_ * invR * taper;
				const float t2  = nx*nx + ny*ny;
				if (t2 > 1.0f) continue;
				const float dy_ = float(py) - d.y;
				// Refraction: minified INVERTED wide-angle background —
				// sample the padded snapshot at center − sK·offset.
				int sx = int(d.x - dx_*sK) - xa;
				int sy = int(d.y - dy_*sK) - ya;
				if (sx < 0) sx = 0; if (sx >= rw) sx = rw-1;
				if (sy < 0) sy = 0; if (sy >= rh) sy = rh-1;
				const dword s = rect[size_t(sy)*rw + sx];
				float sR = float((s>>16)&0xFFu);
				float sG = float((s>> 8)&0xFFu);
				float sB = float( s     &0xFFu);
				// Rim darkening (refraction steepens at the edge) + a
				// touch of cool tint so drops read on flat areas.
				if (t2 > 0.70f) {
					const float k = 1.0f - (t2 - 0.70f) * (1.0f/0.30f) * 0.30f;
					sR *= k; sG *= k; sB *= k;
				}
				sB = sB + 14.0f > 255.0f ? 255.0f : sB + 14.0f;
				// Glint.
				const float gdx = float(px)-gx, gdy = float(py)-gy;
				if (gdx*gdx + gdy*gdy < gr2) {
					sR += 55.0f; sG += 55.0f; sB += 55.0f;
					if (sR > 255.0f) sR = 255.0f;
					if (sG > 255.0f) sG = 255.0f;
					if (sB > 255.0f) sB = 255.0f;
				}
				// Edge AA + spawn fade.
				float a = fade;
				if (t2 > 0.82f) a *= (1.0f - t2) * (1.0f/0.18f);
				const size_t i = size_t(py)*XRes + size_t(px);
				const dword pix = out[i];
				const float keep = 1.0f - a;
				const int nR = int(float((pix>>16)&0xFFu)*keep + sR*a);
				const int nG = int(float((pix>> 8)&0xFFu)*keep + sG*a);
				const int nB = int(float( pix     &0xFFu)*keep + sB*a);
				out[i] = (dword(nR)<<16)|(dword(nG)<<8)|dword(nB)|0xFF000000u;
			}
		}
		++di;
	}
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
                            // NR'd: cosT feeds the narrow-cone-gain
                            // smoothstep (see rsqrt_nr_x8) and
                            // Omni_ForceVolCone can route narrow cones
                            // through the unified path too.
                            const __m256 invLen = rsqrt_nr_x8(safeW2);
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

void Render_DeferredVolumetric(const DeferredLightingCtx &ctx) {
    VolProfScope _vp(&g_volProf.ms_unified, &g_volProf.n_unified);
    if (!CurScene || !ZPage16 || !VPage) return;

    const DeferredLightingCtx &dc = ctx;
    const ViewLightsSoA *const lights = dc.lights;
    if (!lights) return;
    const int numLights = dc.numLights;

    // Pre-filter spot vs omni index lists.
    static int spotIdx[DEFERRED_MAX_VIEW_LIGHTS];
    static int omniIdx[DEFERRED_MAX_VIEW_LIGHTS];
    int spotCount = 0, omniCount = 0;
    for (int i = 0; i < numLights; ++i) {
        // Mirror clones don't cast volumetric glow — same additive-wash
        // reasoning as the halo pass (see Render_OmniHalos).
        if (lights->mirrorId[i] != 0) continue;
        if (lights->isSpot[i]) {
            if (fds::FeatureFlags::draw_cones() || lights->forceCone[i])
                spotIdx[spotCount++] = i;
        }
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
    const float coneDens = (spotCount > 0)
        ? fds::FeatureFlags::cone_strength() * 0.001f
          * (fds::FeatureFlags::hdr() ? fds::FeatureFlags::hdr_glow_scale() : 1.0f)  // glowMax cap off in HDR
        : 0.0f;
    const float haloDens = fds::FeatureFlags::omni_halo_strength() * 0.001f;

    constexpr int numTilesX = 6;
    constexpr int numTilesY = 4;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;

    renderns::tileCounter = 0;
    {
        constexpr int nJobs = numTilesX * numTilesY;
        const int *sP = spotIdx; const int sC = spotCount;
        const int *oP = omniIdx; const int oC = omniCount;
        dispatchIndexed(nJobs, &renderns::tileDone, [=](int t) {
            const int j = t / numTilesX, i = t - j * numTilesX;
            const int y1 = tileSizeY * j;
            const int y2 = std::min(y1 + tileSizeY, YRes);
            const int x1 = tileSizeX * i;
            const int x2 = std::min(x1 + tileSizeX, XRes);
            Render_DeferredVolumetric_Tile(
                x1, y1, x2, y2, lights, sP, sC, oP, oC,
                invFOVX, invFOVY, invZScale,
                sigma, fogFar, fogR, fogG, fogB,
                coneDens, haloDens);
        });
        for (int _i = 0; _i < nJobs; ++_i) {
            renderns::tileDone.acquire();
        }
    }
}
