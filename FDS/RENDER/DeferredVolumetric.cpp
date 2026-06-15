// Deferred volumetric passes — split out of DeferredLighting.cpp
// (verbatim function moves; see DeferredCommon.h for the split layout).
//
// Post-lighting passes over the finished VPage:
//   Render_DeferredFogPass()   — legacy Scn_Fogged sqrt distance ramp
//   Render_VolumetricCones()   — spot god-ray cones (analytic + hybrid)
//   Render_OmniHalos()         — omni halo spheres (analytic atan integral)
//   Render_DeferredSkybox()    — deferred sky composite
//   VolProf_Tick()             — per-interval cone/halo profiling dump
// All entry points are called from RENDER.CPP's renderFrame.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <algorithm>
#include <limits>
#include <vector>
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
#include "Threads.h"

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

// Cone-tile path counters: incremented once per (tile × spot × 8-pixel
// batch). Reset and reported by VolProf_Tick when vol_prof is on. Sit
// at file scope so the cone tile fn (which is above the VolProf struct)
// can reach them.
static std::atomic<int> g_coneAnalyticHits{0};
static std::atomic<int> g_coneRaymarchHits{0};

// Full-screen distance fog over opaque pixels. Runs after
// Render_DeferredLighting writes finished colors to VPage; before the
// transparent peel composites. 1998 used `sqrt(1 - z/FZP)` per VERTEX;
// we keep the sqrt curve (the linear curve makes mid-range too thick)
// but compute it per-pixel via approx rsqrt: `sqrt(t) = t * rsqrt(t)`.
// On arm64 this maps to vrsqrteq_f32 (4-5 cycles) and on x86 to
// _mm256_rsqrt_ps. With the simde arm64 m256 rsqrt patch, both lanes
// hit native NEON / AVX2 directly.
//
// Skipped pixels:
//   mat32 == 0xFFFFFFFF — forward filler wrote here (reflective windows,
//     additive fountain vortex, sky-cube). Those have their own
//     per-vertex fog from TheOtherBarry and would double-fog otherwise.
//   zEnc == 0 — no opaque pixel (sky-cube background, or empty Z).
//     Sky already represents infinite distance; don't fog it.
static void Render_DeferredFogPass_Tile(int x1, int y1, int x2, int y2,
                                         float invFZP)
{
	// Render-target state from the per-frame ctx (populated by
	// Render_DeferredLighting, which runs before any volumetric pass — see
	// renderFrame order). Locals shadow the globals so the body is untouched.
	// (g_deferredCtx is still file-scope; param-threading lands with renderFrame.)
	const DeferredLightingCtx &ctx = g_deferredCtx;
	const int XRes = ctx.xres;
	byte *const VPage = ctx.vpage;
	word *const ZPage16 = ctx.zpage16;
	const float CntrEX = ctx.cntrEX, CntrEY = ctx.cntrEY;
	const meka::GBuffer *const g_gbuffer = ctx.gb;
	const float g_zscale = ctx.zscale;
	dword *out = reinterpret_cast<dword*>(VPage);
	const uint32_t *mat = g_gbuffer->txtr.data();
	const uint16_t *zEnc = ZPage16;
	const float invZScale = 1.0f / float(g_zscale);
	const Vec8f vInvZScale(invZScale);
	const Vec8f vInvFZP(invFZP);
	const Vec8f vOne(1.0f);
	const Vec8f vZero(0.0f);
	const Vec8i vSentinel(int(0xFFFFFFFF));
	const Vec8i vZEncZero(0);
	const Vec8f vZBase(float(0xFF80));
	const Vec8i vFFu(0xFF);
	const Vec8i vTen(10);
	const Vec8i vAlpha(int(0xFF000000));

	for (int py = y1; py < y2; ++py) {
		int px = x1;
		const size_t row = size_t(py) * XRes;
		// 8-pixel SIMD body.
		for (; px + 8 <= x2; px += 8) {
			const size_t i = row + px;

			Vec8i v8;
			v8.load(out + i);
			Vec8i m8;
			m8.load(mat + i);
			// 8x u16 → 8x i32 (no sign-extension needed: zEnc is unsigned).
			alignas(16) uint16_t z16buf[8];
			std::memcpy(z16buf, zEnc + i, 16);
			alignas(32) int32_t z32buf[8];
			for (int k = 0; k < 8; ++k) z32buf[k] = int32_t(z16buf[k]);
			Vec8i z8;
			z8.load_a(z32buf);

			// Pixel-valid mask: mat != sentinel AND zEnc != 0.
			Vec8ib maskValid = (m8 != vSentinel) & (z8 != vZEncZero);

			// Decode z, fog rate, sqrt-via-rsqrt.
			Vec8f zf  = (vZBase - to_float(z8)) * vInvZScale;
			Vec8f t   = max(vZero, vOne - zf * vInvFZP);
			Vec8f rs  = approx_rsqrt(t);
			Vec8f fog = t * rs;          // sqrt(t)
			// At t==0, rsqrt yields garbage; mask the result.
			Vec8fb tPositive = t > vZero;
			fog = select(tPositive, fog, vZero);
			fog = min(fog, vOne);

			// Channel split, fog-multiply, floor-at-10, re-pack.
			Vec8i bB = v8        & vFFu;
			Vec8i bG = (v8 >>  8) & vFFu;
			Vec8i bR = (v8 >> 16) & vFFu;
			Vec8i nB = max(vTen, truncatei(to_float(bB) * fog));
			Vec8i nG = max(vTen, truncatei(to_float(bG) * fog));
			Vec8i nR = max(vTen, truncatei(to_float(bR) * fog));
			Vec8i packed = nB | (nG << 8) | (nR << 16) | vAlpha;
			Vec8i result = select(Vec8ib(maskValid), packed, v8);
			result.store(out + i);
		}
		// Scalar tail (≤7 pixels).
		for (; px < x2; ++px) {
			const size_t i = row + px;
			if (mat[i] == 0xFFFFFFFFu) continue;
			const word z16 = zEnc[i];
			if (z16 == 0) continue;
			const float z = float(0xFF80 - z16) * invZScale;
			const float t = 1.0f - z * invFZP;
			const float fog = t > 0.0f ? t * fast_rsqrt(t) : 0.0f;
			const dword v = out[i];
			int B = std::max(10, int(((v      ) & 0xFF) * fog));
			int G = std::max(10, int(((v >>  8) & 0xFF) * fog));
			int R = std::max(10, int(((v >> 16) & 0xFF) * fog));
			out[i] = dword(B) | (dword(G) << 8) | (dword(R) << 16) | 0xFF000000u;
		}
	}
}

// ─── Volumetric spotlight cones (screen-space ray-march) ────────────────
//
// Per-pixel: for each spotlight visible in the tile, find the segment of
// the view ray that's both inside the cone and in front of the surface,
// then integrate density × distance falloff × cone falloff along it.
// Uses the same per-tile spot SoA the lighting kernel already builds —
// no separate culling required.
//
// Math (view-space, ray origin = camera):
//   Pixel ray direction: V = (X, Y, 1) where X=(px-CntrEX)*invFOVX,
//                                            Y=(CntrEY-py)*invFOVY.
//   Cone (apex P, axis D, half-angle cosα²=c²): point Q is inside iff
//     (D·(Q-P))² ≥ c²|Q-P|² AND D·(Q-P) ≥ 0.
//   Substituting Q = z_s · V gives a quadratic in z_s with
//     a = (D·V)² - c²(V·V),  b = 2(c²(V·P) - (D·V)(D·P)),
//     c_q = (D·P)² - c²|P|².
//   Real roots → ray crosses cone boundary; clamp interval to
//     [NearZ, min(z_surf, z_at_range)] and integrate.
static void Render_VolumetricCones_Tile(int x1, int y1, int x2, int y2,
                                         const ViewLightsSoA *lights,
                                         const int *spotIdx, int spotCount,
                                         float invFOVX, float invFOVY,
                                         float invZScale, float density,
                                         float fogZ, float invFogZ) {
    if (spotCount == 0) return;
    // fogZ > 0 means scene is fogged: clamp ray to FZP and attenuate each
    // sample by the same (1 - z/FZP) the surface fog pass uses, so the
    // cone fades with depth instead of floating in the cleared backdrop
    // past the fog cutoff. fogZ <= 0 means no fog: no clamp/attenuation.
    // NOTE: iterate the frame-global ViewLightsSoA (not per-tile
    // TileLights). The per-tile lists apply a depth cull that's correct
    // for surface lighting but wrong for volumetric integration — a
    // tile whose surface is past a spot's z-extent is excluded, even
    // though the camera→surface ray can still cross the spot's cone
    // volume. Using the unfiltered list keeps cones consistent across
    // tile boundaries; the per-pixel quadratic test culls per-pixel.

    dword *out = reinterpret_cast<dword*>(VPage);
    const uint16_t *zEnc = ZPage16;
    const int N_SAMPLES = std::max(1, fds::FeatureFlags::vol_n_samples());
    const float inv_N = 1.0f / float(N_SAMPLES);
    const bool vecPath = fds::FeatureFlags::vol_vec();
    const bool analyticCone = fds::FeatureFlags::vol_cone_analytic();
    // Path-counter bump — once per tile call, not per (spot × batch).
    // useAnalytic is constant within a call (depends only on the cone-
    // analytic flag), so a single increment per call is the right
    // granularity. The previous per-batch fetch_add was dragging ~3M
    // atomic ops/frame in city. Fog is handled inside the analytic path
    // via a midpoint (1 - z·invFogZ)² evaluation — same trade as the
    // midpoint coneAtten / shadow tap.
    const bool useAnalytic = analyticCone;
    const float noiseStrength = fds::FeatureFlags::vol_analytic_noise();
    // Mirror gate planes for clone-spot beams (halo-pass precedent):
    // clone beams only paint inside their mirror's stamped footprint,
    // starting at the wall depth. Null when the scene has no mirrors —
    // and then no clone spots are in the list either.
    const meka::u8 *mmask = (g_gbuffer && !g_gbuffer->mirrorMask.empty())
        ? g_gbuffer->mirrorMask.data() : nullptr;
    const uint16_t *mmz = (g_gbuffer && !g_gbuffer->mirrorMaskZ.empty())
        ? g_gbuffer->mirrorMaskZ.data() : nullptr;
    extern DeferredLightingCtx g_deferredCtx;
    if (fds::FeatureFlags::vol_prof()) {
        (useAnalytic ? g_coneAnalyticHits
                     : g_coneRaymarchHits)
            .fetch_add(1, std::memory_order_relaxed);
    }

    // Half vertical rate: compute even rows, write each result to the
    // row and its lower neighbor. Beams are soft gradients — visually
    // free, halves the pass cost. (Vec path only; the scalar path is
    // the --no-vol_vec fallback.)
    const int yStep = (vecPath && fds::FeatureFlags::vol_cone_half_y()) ? 2 : 1;
    for (int py = y1; py < y2; py += yStep) {
        const float Y = (CntrEY - float(py)) * invFOVY;
        const size_t row = size_t(py) * size_t(XRes);
        const bool dupRow = (yStep == 2) && (py + 1 < y2);
        if (vecPath) {
            // ─── Pixel-major SIMD ──────────────────────────────────────
            // 8 lanes = 8 independent rays. Per-pixel setup and per-spot
            // scalar quadratic solve are scalar (the a-sign branching is
            // too hairy to vectorize cleanly); per-sample integration
            // runs 8-wide across pixels. Wins via no wasted lanes at
            // low N, independent dependency chains per lane (better
            // OoO than 8-samples-of-one-ray batching), and per-spot
            // setup amortized across 8 pixels. Shadow lookup stays
            // scalar per-lane — texture gather is too expensive on CPU.
            for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                const int pxEnd     = std::min(pxBase + 8, x2);
                const int laneCount = pxEnd - pxBase;

                alignas(32) float    Xarr[8] = {};
                alignas(32) float    uVarr[8] = {};
                alignas(32) uint32_t pxHashArr[8] = {};
                alignas(32) float    zMaxArr[8] = {};
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
                    const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                    const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
                    float zM = (zSurf > 0.0f) ? zSurf : zSky;
                    if (fogZ > 0.0f && zM > fogZ) zM = fogZ;
                    constexpr float zMin = 0.05f;
                    if (zM > zMin) { zMaxArr[lane] = zM; anyAlive = true; }
                }
                if (!anyAlive) continue;

                alignas(32) float accB[8] = {}, accG[8] = {}, accR[8] = {};

                for (int s = 0; s < spotCount; ++s) {
                    const int li = spotIdx[s];
                    // Per-batch rect cull: skip if the 8-pixel batch
                    // (per-batch rect-cull experiment was reverted —
                    // per-tile cull at dispatcher already does screen-
                    // rect check; per-batch overhead didn't pay across
                    // the city sweep.)
                    const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
                    const float Dx = lights->dirX[li], Dy = lights->dirY[li], Dz = lights->dirZ[li];
                    const float cosO = lights->cosOuter[li];
                    const float cosI = lights->cosInner[li];
                    // Narrow cones (half-angle < ~10°, the disco
                    // beams) take the per-segment hybrid inside the
                    // analytic branch (exact segment integrals ×
                    // per-segment coneAtten — a single midpoint
                    // coneAtten fans into stripes at this gain, and a
                    // uniform-z march loses the 1/d² core spike to a
                    // sample lottery). nSamp only matters when the
                    // analytic flag is off: the fallback march then
                    // uses 16 samples instead of the global N. See
                    // rsqrt_nr_x8 for the 12-bit-rsqrt amplification
                    // that originally made this whole family visible.
                    const bool  narrowCone = cosO > 0.985f;
                    const int   nSamp      = narrowCone ? 16 : N_SAMPLES;
                    const float inv_nSamp  = narrowCone ? (1.0f / 16.0f) : inv_N;
                    // Clone spot: beam reflection, confined to its
                    // mirror's footprint below.
                    const uint32_t omid = lights->mirrorId[li];
                    if (omid != 0 && (!mmask || !mmz)) continue;
                    // Bounce spot: chord clamped to the camera side of
                    // its mirror plane (apex behind the glass). Plane
                    // → view space: n_v = viewMatᵀ·N, d_v = N·camW + D.
                    const bool bounce = lights->bounceClamp[li] != 0;
                    float hsNx=0, hsNy=0, hsNz=0, hsD=0;
                    if (bounce) {
                        const float Nx = lights->mirNX[li];
                        const float Ny = lights->mirNY[li];
                        const float Nz = lights->mirNZ[li];
                        const auto &VW = g_deferredCtx.viewToWorld;
                        hsNx = VW[0][0]*Nx + VW[1][0]*Ny + VW[2][0]*Nz;
                        hsNy = VW[0][1]*Nx + VW[1][1]*Ny + VW[2][1]*Nz;
                        hsNz = VW[0][2]*Nx + VW[1][2]*Ny + VW[2][2]*Nz;
                        hsD  = Nx*g_deferredCtx.cameraWorldX +
                               Ny*g_deferredCtx.cameraWorldY +
                               Nz*g_deferredCtx.cameraWorldZ +
                               lights->mirD[li];
                        if (hsD == 0.0f) continue;  // camera on the glass
                    }
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
                                } else {
                                    continue;
                                }
                                if (zHi <= zLo) continue;
                            }
                        } else {
                            continue;
                        }
                        if (zLo < zSphLo) zLo = zSphLo;
                        if (zHi > zSphHi) zHi = zSphHi;
                        if (zLo < zMin)   zLo = zMin;
                        if (zHi <= zLo)   continue;
                        if (zLo >= zMax)  continue;
                        // Clamp at the surface: without this the
                        // analytic path integrates the chord BEHIND
                        // the floor and leans on the midpoint fade to
                        // approximate the cut — per-pixel z16 noise
                        // then modulates the whole-chord brightness
                        // (the grazing-angle 'fur' on beams).
                        if (zHi > zMax)   zHi = zMax;
                        // Clone beam: only inside its mirror's stamped
                        // footprint, and only BEHIND the glass — clamp
                        // the chord start to the wall depth (halo-pass
                        // precedent).
                        if (omid != 0) {
                            const size_t pi = row + size_t(pxBase + lane);
                            if (uint32_t(mmask[pi]) != omid) continue;
                            const float zWall =
                                float(0xFF80 - int(mmz[pi])) * invZScale;
                            if (zLo < zWall) zLo = zWall;
                            if (zHi <= zLo) continue;
                        }
                        // Bounce beam: keep the chord on the camera
                        // side of the glass. Plane value along the ray
                        // is z·kk + d_v; it changes sign at z* — clamp
                        // zHi there when the ray crosses away from the
                        // camera's side.
                        if (bounce) {
                            const float kk = hsNx*X + hsNy*Y + hsNz;
                            if ((hsD > 0.0f && kk < -1e-9f) ||
                                (hsD < 0.0f && kk >  1e-9f)) {
                                const float zStar = -hsD / kk;
                                if (zHi > zStar) zHi = zStar;
                                if (zHi <= zLo) continue;
                            }
                        }
                        if (std::fabs(DV) > 1e-6f) {
                            const float zFwd = DP / DV;
                            if (DV > 0.0f) { if (zLo < zFwd) zLo = zFwd; }
                            else           { if (zHi > zFwd) zHi = zFwd; }
                            if (zLo >= zHi) continue;
                        }
                        zLoArr[lane]    = zLo;
                        zHiArr[lane]    = zHi;
                        aliveLane[lane] = 1.0f;
                        spotAlive = true;
                    }
                    if (!spotAlive) continue;

                    // Clone spots have no own map (smIdx=-1) but carry
                    // the SOURCE's map: visibility of a sample equals
                    // the source's visibility of the sample reflected
                    // across the mirror plane (same identity as the
                    // surface kernel's mirrored tap). The reflection is
                    // applied in VIEW space: v' = v − 2(n_v·v + d_v)n_v
                    // with n_v = viewMatᵀ·N_world, d_v = N·camW + D.
                    int32_t smIdx = lights->shadowMapIdx[li];
                    bool  smMirror = false;
                    float nvx=0, nvy=0, nvz=0, dv_pl=0;
                    if (smIdx < 0 && omid != 0 &&
                        lights->srcShadowMapIdx[li] >= 0) {
                        smIdx = lights->srcShadowMapIdx[li];
                        smMirror = true;
                        const float Nx = lights->mirNX[li];
                        const float Ny = lights->mirNY[li];
                        const float Nz = lights->mirNZ[li];
                        const auto &VW = g_deferredCtx.viewToWorld;
                        nvx = VW[0][0]*Nx + VW[1][0]*Ny + VW[2][0]*Nz;
                        nvy = VW[0][1]*Nx + VW[1][1]*Ny + VW[2][1]*Nz;
                        nvz = VW[0][2]*Nx + VW[1][2]*Ny + VW[2][2]*Nz;
                        dv_pl = Nx*g_deferredCtx.cameraWorldX +
                                Ny*g_deferredCtx.cameraWorldY +
                                Nz*g_deferredCtx.cameraWorldZ +
                                lights->mirD[li];
                    }
                    const ShadowMap *sm = (smIdx >= 0 && size_t(smIdx) < g_shadowMaps.size())
                                          ? &g_shadowMaps[smIdx] : nullptr;
                    float sm_m00=0, sm_m01=0, sm_m02=0, sm_ox=0;
                    float sm_m10=0, sm_m11=0, sm_m12=0, sm_oy=0;
                    float sm_m20=0, sm_m21=0, sm_m22=0, sm_oz=0;
                    float sm_cntrX=0, sm_cntrY=0, sm_perspX=0, sm_perspY=0;
                    float sm_zScale=0;
                    const uint16_t *sm_depth = nullptr;
                    int sm_xres=0, sm_yres=0;
                    if (sm) {
                        sm_m00=sm->viewToLight[0][0]; sm_m01=sm->viewToLight[0][1]; sm_m02=sm->viewToLight[0][2]; sm_ox=sm->viewToLightOffset.x;
                        sm_m10=sm->viewToLight[1][0]; sm_m11=sm->viewToLight[1][1]; sm_m12=sm->viewToLight[1][2]; sm_oy=sm->viewToLightOffset.y;
                        sm_m20=sm->viewToLight[2][0]; sm_m21=sm->viewToLight[2][1]; sm_m22=sm->viewToLight[2][2]; sm_oz=sm->viewToLightOffset.z;
                        sm_cntrX=sm->cntrX; sm_cntrY=sm->cntrY;
                        sm_perspX=sm->perspX; sm_perspY=sm->perspY;
                        sm_zScale=sm->zScale;
                        sm_depth=sm->depth.data();
                        sm_xres=sm->xres; sm_yres=sm->yres;
                    }

                    alignas(32) float dzArr[8] = {}, invDzArr[8] = {}, fadeStartArr[8] = {};
                    for (int lane = 0; lane < 8; ++lane) {
                        if (aliveLane[lane] == 0.0f) continue;
                        const float d = (zHiArr[lane] - zLoArr[lane]) * inv_nSamp;
                        dzArr[lane]        = d;
                        // Surface-fade window: at least ~12 z-buffer
                        // quanta wide, so the z16 staircase (large in
                        // world units at grazing incidence) jitters
                        // INSIDE the ramp instead of cutting the beam
                        // at a per-column-noisy depth.
                        const float fadeW = std::max(d, 12.0f * invZScale);
                        invDzArr[lane]     = 1.0f / fadeW;
                        fadeStartArr[lane] = zMaxArr[lane] - fadeW;
                    }

                    const __m256 vX_v        = _mm256_load_ps(Xarr);
                    const __m256 vY_v        = _mm256_set1_ps(Y);
                    const __m256 vZMax_v     = _mm256_load_ps(zMaxArr);
                    const __m256 vZLo_v      = _mm256_load_ps(zLoArr);
                    const __m256 vDz_v       = _mm256_load_ps(dzArr);
                    const __m256 vInvDz_v    = _mm256_load_ps(invDzArr);
                    const __m256 vFadeStart_v= _mm256_load_ps(fadeStartArr);
                    const __m256 vAlive_v    = _mm256_load_ps(aliveLane);
                    const __m256 vPx_v       = _mm256_set1_ps(Px);
                    const __m256 vPy_v       = _mm256_set1_ps(Py_l);
                    const __m256 vPz_v       = _mm256_set1_ps(Pz);
                    const __m256 vDx_v       = _mm256_set1_ps(Dx);
                    const __m256 vDy_v       = _mm256_set1_ps(Dy);
                    const __m256 vDz_dir_v   = _mm256_set1_ps(Dz);
                    const __m256 vR2_v       = _mm256_set1_ps(r2);
                    const __m256 vRR_v       = _mm256_set1_ps(rr);
                    const __m256 vCosO_v     = _mm256_set1_ps(cosO);
                    const __m256 vCosI_v     = _mm256_set1_ps(cosI);
                    const __m256 vInvCIO_v   = _mm256_set1_ps(inv_cosI_minus_cosO);
                    const __m256 vInvFogZ_v  = _mm256_set1_ps(invFogZ);
                    const __m256 vZero_v     = _mm256_setzero_ps();
                    const __m256 vOne_v      = _mm256_set1_ps(1.0f);
                    const __m256 vTwo_v      = _mm256_set1_ps(2.0f);
                    const __m256 vThree_v    = _mm256_set1_ps(3.0f);
                    const __m256 vEps_v      = _mm256_set1_ps(1e-6f);
                    const __m256 vPt05_v     = _mm256_set1_ps(0.05f);
                    const __m256 mAlive      = _mm256_cmp_ps(vAlive_v, vZero_v, _CMP_GT_OQ);
                    __m256 accV = vZero_v;

                    // ─── Analytic cone branch ────────────────────────
                    // Closed-form arctan integral of inverse-square dist
                    // attenuation over the cone-clipped segment, with
                    // coneAtten (smoothstep cosO→cosI) approximated at
                    // segment midpoint. Drops the (1-rr·d)² near-edge
                    // cutoff (same trade as omni analytic).
                    //
                    // Approximations vs ray-march:
                    //   (a) coneAtten / surfaceFade / fogAtten —
                    //       evaluated at the segment midpoint.
                    //   (b) shadow occupancy — a single shadow-map tap
                    //       at z=zMid. Whole segment in or out (binary).
                    //       Stair-steps at shadow boundaries; tolerable
                    //       because halos are inherently diffuse.
                    if (useAnalytic) {
                        // α z² + β z + γ = rr²·d²(z) + 0.05
                        const __m256 vRR2_v   = _mm256_mul_ps(vRR_v, vRR_v);
                        // Per-lane uV (= X²+Y²+1, varies per pixel).
                        const __m256 vUv_v    = _mm256_load_ps(uVarr);
                        // VP = X·Px + Y·Py + Pz per lane.
                        const __m256 vVP_v    = _mm256_fmadd_ps(vX_v, vPx_v,
                                                _mm256_fmadd_ps(vY_v, vPy_v, vPz_v));
                        const __m256 vPP_v    = _mm256_set1_ps(PP);
                        const __m256 vAlpha   = _mm256_mul_ps(vRR2_v, vUv_v);
                        const __m256 vBeta    = _mm256_mul_ps(
                                                _mm256_set1_ps(-2.0f),
                                                _mm256_mul_ps(vRR2_v, vVP_v));
                        const __m256 vGamma   = _mm256_fmadd_ps(vRR2_v, vPP_v, vPt05_v);
                        const __m256 vDiscQ   = _mm256_fmsub_ps(
                                                _mm256_mul_ps(_mm256_set1_ps(4.0f), vAlpha), vGamma,
                                                _mm256_mul_ps(vBeta, vBeta));
                        const __m256 mDisc    = _mm256_cmp_ps(vDiscQ, vZero_v, _CMP_GT_OQ);
                        const __m256 vSafeDisc = _mm256_blendv_ps(vOne_v, vDiscQ, mDisc);
                        // rsqrt is a 12-bit table approx; for rays
                        // passing near the light the discriminant is
                        // tiny, invD is huge, and the table's
                        // quantization staircase amplifies into visible
                        // striations across bright narrow cones (the
                        // disco-beam moire). One Newton-Raphson step
                        // (~24-bit) kills it for ~3 fma.
                        __m256 vInvD          = _mm256_rsqrt_ps(vSafeDisc);
                        vInvD = _mm256_mul_ps(vInvD,
                                _mm256_fnmadd_ps(
                                    _mm256_mul_ps(_mm256_set1_ps(0.5f), vSafeDisc),
                                    _mm256_mul_ps(vInvD, vInvD),
                                    _mm256_set1_ps(1.5f)));

                        const __m256 vTwoA    = _mm256_add_ps(vAlpha, vAlpha);
                        const __m256 vZHi_v   = _mm256_load_ps(zHiArr);
                        const __m256 vArgHi   = _mm256_mul_ps(vInvD,
                                                _mm256_fmadd_ps(vTwoA, vZHi_v, vBeta));
                        const __m256 vArgLo   = _mm256_mul_ps(vInvD,
                                                _mm256_fmadd_ps(vTwoA, vZLo_v, vBeta));
                        // atan(u) − atan(v) computed DIRECTLY via the
                        // identity atan((u−v)/(1+uv)) (+π when uv<−1;
                        // u>v always since zHi>zLo): near the ray-
                        // grazes-the-light singularity both arguments
                        // are huge and nearly equal — subtracting two
                        // separately-evaluated atans loses precision,
                        // amplified by 2·invD. The identity feeds ONE
                        // atan a small well-conditioned argument.
                        // (Historical: this was suspected as the
                        // narrow-cone striping cause; the striping was
                        // actually the cosT rsqrt — see rsqrt_nr_x8.)
                        // Stable atan difference via the identity
                        // atan(u)−atan(v) = atan((u−v)/(1+uv)) (+π when
                        // uv<−1) — avoids subtracting two atans of huge
                        // near-equal arguments in the ray-grazes-light
                        // regime.
                        auto atanDiff = [&](const __m256 &u1, const __m256 &u0) -> __m256 {
                            const __m256 num  = _mm256_sub_ps(u1, u0);
                            const __m256 den  = _mm256_fmadd_ps(u1, u0, vOne_v);
                            const __m256 mDen0 = _mm256_cmp_ps(
                                _mm256_andnot_ps(_mm256_set1_ps(-0.0f), den),
                                _mm256_set1_ps(1e-20f), _CMP_LT_OQ);
                            const __m256 safeDen = _mm256_blendv_ps(den, _mm256_set1_ps(1e-20f), mDen0);
                            __m256 at = atan_approx_x8(_mm256_div_ps(num, safeDen));
                            const __m256 mWrap = _mm256_cmp_ps(den, vZero_v, _CMP_LT_OQ);
                            return _mm256_add_ps(at,
                                   _mm256_and_ps(mWrap, _mm256_set1_ps(3.14159265f)));
                        };
                        // coneAtten (smoothstep cosO→cosI) at a given z.
                        auto coneAttenAt = [&](const __m256 &z) -> __m256 {
                            const __m256 Wx = _mm256_sub_ps(_mm256_mul_ps(z, vX_v), vPx_v);
                            const __m256 Wy = _mm256_sub_ps(_mm256_mul_ps(z, vY_v), vPy_v);
                            const __m256 Wz = _mm256_sub_ps(z, vPz_v);
                            const __m256 W2 = _mm256_fmadd_ps(Wx, Wx,
                                              _mm256_fmadd_ps(Wy, Wy,
                                               _mm256_mul_ps(Wz, Wz)));
                            const __m256 DW = _mm256_fmadd_ps(vDx_v, Wx,
                                              _mm256_fmadd_ps(vDy_v, Wy,
                                               _mm256_mul_ps(vDz_dir_v, Wz)));
                            const __m256 safeW2 = _mm256_max_ps(W2, _mm256_set1_ps(1e-12f));
                            const __m256 cosT = _mm256_mul_ps(DW, rsqrt_nr_x8(safeW2));
                            __m256 t = _mm256_mul_ps(_mm256_sub_ps(cosT, vCosO_v), vInvCIO_v);
                            t = _mm256_max_ps(vZero_v, _mm256_min_ps(vOne_v, t));
                            const __m256 sm = _mm256_mul_ps(_mm256_mul_ps(t, t),
                                              _mm256_sub_ps(vThree_v, _mm256_mul_ps(vTwo_v, t)));
                            const __m256 mIn = _mm256_cmp_ps(cosT, vCosI_v, _CMP_GE_OQ);
                            return _mm256_blendv_ps(sm, vOne_v, mIn);
                        };
                        __m256 vIntegral;
                        if (!narrowCone) {
                            // Wide cones: single closed form. coneAtten
                            // is applied at the midpoint further below.
                            vIntegral = _mm256_mul_ps(
                                        _mm256_add_ps(vInvD, vInvD),
                                        atanDiff(vArgHi, vArgLo));
                        } else {
                            // Narrow cones (disco beams): per-segment
                            // hybrid. The pure midpoint coneAtten fans
                            // into stripes (cosT_mid varies violently
                            // across a thin cone), and a uniform-z
                            // ray-march loses the sharp 1/d² spike to a
                            // sample lottery (picket-fence slats). Here
                            // each of 8 segments gets the EXACT distance
                            // integral (stable per-segment atan diff)
                            // weighted by coneAtten at its midpoint —
                            // no lottery, no global midpoint.
                            constexpr int SEG = 8;
                            const __m256 vZMaxFade_v = _mm256_load_ps(zMaxArr);
                            const __m256 vSegDz = _mm256_mul_ps(
                                _mm256_sub_ps(vZHi_v, vZLo_v),
                                _mm256_set1_ps(1.0f / SEG));
                            __m256 vSum  = vZero_v;
                            __m256 uPrev = vArgLo;
                            for (int seg = 1; seg <= SEG; ++seg) {
                                const __m256 zk = _mm256_fmadd_ps(
                                    _mm256_set1_ps(float(seg)), vSegDz, vZLo_v);
                                const __m256 uk = _mm256_mul_ps(vInvD,
                                    _mm256_fmadd_ps(vTwoA, zk, vBeta));
                                const __m256 zm = _mm256_fmadd_ps(
                                    _mm256_set1_ps(float(seg) - 0.5f), vSegDz, vZLo_v);
                                // Surface fade per segment (ramp over
                                // the widened window) — folded here so
                                // the global midpoint fade (skipped for
                                // narrow below) can't reintroduce the
                                // whole-chord z16 sensitivity.
                                __m256 sf = _mm256_mul_ps(
                                    _mm256_sub_ps(vZMaxFade_v, zm), vInvDz_v);
                                sf = _mm256_max_ps(vZero_v,
                                     _mm256_min_ps(vOne_v, sf));
                                // Per-segment shadow tap: the single
                                // whole-chord midpoint tap let beam
                                // segments BEHIND walls glow whenever
                                // the midpoint happened to be lit (the
                                // beams-through-walls report). One
                                // scalar tap per segment×lane, narrow
                                // cones only — the tile cone-cull keeps
                                // the per-frame count small.
                                __m256 shv = vOne_v;
                                if (sm) {
                                    alignas(32) float zmA[8], shA[8] =
                                        {1,1,1,1,1,1,1,1};
                                    _mm256_store_ps(zmA, zm);
                                    for (int ln = 0; ln < laneCount; ++ln) {
                                        if (aliveLane[ln] == 0.0f) continue;
                                        const float zL = zmA[ln];
                                        float zX = zL * Xarr[ln];
                                        float zY = zL * Y, zZ = zL;
                                        if (smMirror) {
                                            const float t2r = 2.0f *
                                                (nvx*zX + nvy*zY + nvz*zZ + dv_pl);
                                            zX -= t2r * nvx;
                                            zY -= t2r * nvy;
                                            zZ -= t2r * nvz;
                                        }
                                        const float lx = sm_m00*zX + sm_m01*zY + sm_m02*zZ + sm_ox;
                                        const float ly = sm_m10*zX + sm_m11*zY + sm_m12*zZ + sm_oy;
                                        const float lz = sm_m20*zX + sm_m21*zY + sm_m22*zZ + sm_oz;
                                        if (lz <= 0.0f) continue;
                                        const float invLZ = 1.0f / lz;
                                        const int iX = int(sm_cntrX + sm_perspX * lx * invLZ);
                                        const int iY = int(sm_cntrY - sm_perspY * ly * invLZ);
                                        if (uint32_t(iX) >= uint32_t(sm_xres) ||
                                            uint32_t(iY) >= uint32_t(sm_yres)) continue;
                                        const size_t o2 = size_t(iY) * size_t(sm_xres) + size_t(iX);
                                        const uint16_t zS = std::max(
                                            sm->depth[o2], sm->depth_dynamic[o2]);
                                        int pixZ = 0xFF80 - int(lz * sm_zScale);
                                        if (pixZ < 0) pixZ = 0;
                                        if (pixZ + 128 < int(zS)) shA[ln] = 0.0f;
                                    }
                                    shv = _mm256_load_ps(shA);
                                }
                                vSum = _mm256_fmadd_ps(atanDiff(uk, uPrev),
                                       _mm256_mul_ps(_mm256_mul_ps(coneAttenAt(zm), sf),
                                                     shv), vSum);
                                uPrev = uk;
                            }
                            vIntegral = _mm256_mul_ps(
                                        _mm256_add_ps(vInvD, vInvD), vSum);
                        }

                        // Midpoint sample: cosT_mid and surfaceFade_mid
                        // approximate the otherwise-z-dependent factors.
                        const __m256 vZMid    = _mm256_mul_ps(
                                                _mm256_add_ps(vZLo_v, vZHi_v),
                                                _mm256_set1_ps(0.5f));
                        const __m256 Wx_m = _mm256_sub_ps(_mm256_mul_ps(vZMid, vX_v), vPx_v);
                        const __m256 Wy_m = _mm256_sub_ps(_mm256_mul_ps(vZMid, vY_v), vPy_v);
                        const __m256 Wz_m = _mm256_sub_ps(vZMid, vPz_v);
                        const __m256 W2_m = _mm256_fmadd_ps(Wx_m, Wx_m,
                                            _mm256_fmadd_ps(Wy_m, Wy_m,
                                             _mm256_mul_ps(Wz_m, Wz_m)));
                        const __m256 DW_m = _mm256_fmadd_ps(vDx_v, Wx_m,
                                            _mm256_fmadd_ps(vDy_v, Wy_m,
                                             _mm256_mul_ps(vDz_dir_v, Wz_m)));
                        const __m256 safeW2_m = _mm256_blendv_ps(vOne_v, W2_m, mAlive);
                        const __m256 invLen_m = rsqrt_nr_x8(safeW2_m);
                        const __m256 cosT_m   = _mm256_mul_ps(DW_m, invLen_m);
                        // Near-edge softness: ray-march multiplies by
                        // (1 - rr·d)² to fade the integrand at the
                        // sphere surface. Reintroduce that as a midpoint
                        // factor so the analytic doesn't show a hard
                        // boundary where the halo ends. dist_mid =
                        // W²·invLen (rsqrt identity).
                        const __m256 dist_m   = _mm256_mul_ps(W2_m, invLen_m);
                        __m256 softEdge_m     = _mm256_sub_ps(vOne_v,
                                                _mm256_mul_ps(vRR_v, dist_m));
                        softEdge_m = _mm256_max_ps(vZero_v, softEdge_m);
                        softEdge_m = _mm256_mul_ps(softEdge_m, softEdge_m);
                        // coneAtten at midpoint: smoothstep(cosO→cosI).
                        __m256 t_m = _mm256_mul_ps(_mm256_sub_ps(cosT_m, vCosO_v), vInvCIO_v);
                        t_m = _mm256_max_ps(vZero_v, _mm256_min_ps(vOne_v, t_m));
                        const __m256 smooth_m = _mm256_mul_ps(
                                                _mm256_mul_ps(t_m, t_m),
                                                _mm256_sub_ps(vThree_v,
                                                  _mm256_mul_ps(vTwo_v, t_m)));
                        const __m256 mInner_m = _mm256_cmp_ps(cosT_m, vCosI_v, _CMP_GE_OQ);
                        // Narrow cones already folded coneAtten in per
                        // segment — don't apply the midpoint one again.
                        const __m256 coneAtten_m = narrowCone
                            ? vOne_v
                            : _mm256_blendv_ps(smooth_m, vOne_v, mInner_m);
                        // surfaceFade at midpoint.
                        const __m256 mFade_m  = _mm256_cmp_ps(vZMid, vFadeStart_v, _CMP_GT_OQ);
                        const __m256 fadeVal_m = _mm256_mul_ps(
                                                 _mm256_sub_ps(vZMax_v, vZMid), vInvDz_v);
                        // Narrow cones folded the fade per segment.
                        const __m256 surfaceFade_m = narrowCone
                            ? vOne_v
                            : _mm256_blendv_ps(vOne_v, fadeVal_m, mFade_m);

                        // Match ray-march brightness scaling: N × mean.
                        const __m256 vIntervalLen = _mm256_sub_ps(vZHi_v, vZLo_v);
                        const __m256 vSafeLen  = _mm256_blendv_ps(vOne_v, vIntervalLen, mAlive);
                        const __m256 vN        = _mm256_set1_ps(float(N_SAMPLES));
                        // mean per lane: integral / interval; final
                        // contribution per "sample-unit": mean × N ×
                        // coneAtten_mid × surfaceFade_mid.
                        // rcp refined for the same reason as invD.
                        __m256 vRcpLen = _mm256_rcp_ps(vSafeLen);
                        vRcpLen = _mm256_mul_ps(vRcpLen,
                                  _mm256_fnmadd_ps(vSafeLen, vRcpLen,
                                                   _mm256_set1_ps(2.0f)));
                        __m256 vAcc = _mm256_mul_ps(vIntegral, vRcpLen);
                        vAcc = _mm256_mul_ps(vAcc, vN);
                        vAcc = _mm256_mul_ps(vAcc, coneAtten_m);
                        vAcc = _mm256_mul_ps(vAcc, surfaceFade_m);
                        vAcc = _mm256_mul_ps(vAcc, softEdge_m);

                        // Midpoint fog: ray-march path uses (1-z·invFogZ)²
                        // per sample; here we sample once at z=zMid. Same
                        // approximation strategy as midpoint cone/shadow.
                        if (invFogZ > 0.0f) {
                            __m256 fog_m = _mm256_sub_ps(vOne_v,
                                            _mm256_mul_ps(vZMid, vInvFogZ_v));
                            fog_m = _mm256_max_ps(vZero_v, fog_m);
                            fog_m = _mm256_mul_ps(fog_m, fog_m);
                            vAcc = _mm256_mul_ps(vAcc, fog_m);
                        }

                        // Per-pixel multiplicative noise: replicates the
                        // visual texture of the ray-march path (whose
                        // stochastic sample offsets produce inter-pixel
                        // variation) without sacrificing the analytic
                        // smoothness. Hash from existing pxHashArr.
                        if (noiseStrength > 0.0f) {
                            alignas(32) float noiseBuf[8];
                            for (int lane = 0; lane < 8; ++lane) {
                                // pxHashArr is already stable per pixel.
                                // Map to [-0.5, +0.5) then scale.
                                const float u =
                                    float(pxHashArr[lane] >> 16) * (1.0f/65536.0f);
                                noiseBuf[lane] = 1.0f + noiseStrength * (u - 0.5f);
                            }
                            vAcc = _mm256_mul_ps(vAcc, _mm256_load_ps(noiseBuf));
                        }
                        // Mask out lanes where: discQ<=0, cone-axis test
                        // fails (cosT<cosO at midpoint), or lane dead.
                        const __m256 mAng = _mm256_cmp_ps(cosT_m, vCosO_v, _CMP_GE_OQ);
                        __m256 m          = _mm256_and_ps(mAlive,
                                            _mm256_and_ps(mDisc, mAng));

                        // Midpoint shadow tap — one sample at z=zMid
                        // gates the whole segment. Stair-steps at
                        // shadow boundaries; tolerated because halos
                        // are diffuse. Mirrors the in-loop sm path
                        // above but uses zMid instead of per-sample z.
                        if (sm && !narrowCone) {
                            // (narrow cones fold shadow per segment in
                            // the hybrid above)
                            alignas(32) float maskArr_s[8], zArr_s[8];
                            _mm256_store_ps(maskArr_s, m);
                            _mm256_store_ps(zArr_s, vZMid);
                            alignas(32) float shadowMul_s[8] =
                                {1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
                            for (int lane = 0; lane < 8; ++lane) {
                                if (maskArr_s[lane] == 0) continue;
                                const float zL = zArr_s[lane];
                                const float Xl = Xarr[lane];
                                float zX = zL * Xl, zY = zL * Y, zZ = zL;
                                if (smMirror) {
                                    // Reflect the sample across the
                                    // mirror plane (view space) — the
                                    // source light's map sees its side.
                                    const float t2r = 2.0f *
                                        (nvx*zX + nvy*zY + nvz*zZ + dv_pl);
                                    zX -= t2r * nvx;
                                    zY -= t2r * nvy;
                                    zZ -= t2r * nvz;
                                }
                                const float lx = sm_m00*zX + sm_m01*zY + sm_m02*zZ + sm_ox;
                                const float ly = sm_m10*zX + sm_m11*zY + sm_m12*zZ + sm_oy;
                                const float lz = sm_m20*zX + sm_m21*zY + sm_m22*zZ + sm_oz;
                                if (lz <= 0.0f) continue;
                                const float invLZ = 1.0f / lz;
                                const float smX = sm_cntrX + sm_perspX * lx * invLZ;
                                const float smY = sm_cntrY - sm_perspY * ly * invLZ;
                                const int iX = int(smX), iY = int(smY);
                                if (uint32_t(iX) >= uint32_t(sm_xres) ||
                                    uint32_t(iY) >= uint32_t(sm_yres)) continue;
                                int pixZ = 0xFF80 - int(lz * sm_zScale);
                                if (pixZ < 0) pixZ = 0;
                                if (pixZ > 0xFFFF) pixZ = 0xFFFF;
                                const int biased = pixZ + 128;
                                const uint16_t shadowZ =
                                    sm_depth[size_t(iY) * size_t(sm_xres) + size_t(iX)];
                                if (biased < int(shadowZ)) shadowMul_s[lane] = 0.0f;
                            }
                            const __m256 vShad_s = _mm256_load_ps(shadowMul_s);
                            m = _mm256_and_ps(m,
                                _mm256_cmp_ps(vShad_s, _mm256_set1_ps(0.5f), _CMP_GT_OQ));
                        }

                        accV = _mm256_and_ps(vAcc, m);
                    } else {
                    for (int k = 0; k < nSamp; ++k) {
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

                        __m256 mask = _mm256_and_ps(mAlive,
                            _mm256_cmp_ps(vZ, vZMax_v, _CMP_LT_OQ));

                        const __m256 mFade   = _mm256_cmp_ps(vZ, vFadeStart_v, _CMP_GT_OQ);
                        const __m256 fadeVal = _mm256_mul_ps(_mm256_sub_ps(vZMax_v, vZ), vInvDz_v);
                        const __m256 surfaceFade = _mm256_blendv_ps(vOne_v, fadeVal, mFade);

                        const __m256 Wx = _mm256_sub_ps(_mm256_mul_ps(vZ, vX_v), vPx_v);
                        const __m256 Wy = _mm256_sub_ps(_mm256_mul_ps(vZ, vY_v), vPy_v);
                        const __m256 Wz = _mm256_sub_ps(vZ, vPz_v);
                        const __m256 W2 = _mm256_fmadd_ps(Wx, Wx,
                                           _mm256_fmadd_ps(Wy, Wy,
                                            _mm256_mul_ps(Wz, Wz)));
                        mask = _mm256_and_ps(mask, _mm256_cmp_ps(W2, vR2_v, _CMP_LE_OQ));
                        mask = _mm256_and_ps(mask, _mm256_cmp_ps(W2, vEps_v, _CMP_GT_OQ));

                        const __m256 DW = _mm256_fmadd_ps(vDx_v, Wx,
                                           _mm256_fmadd_ps(vDy_v, Wy,
                                            _mm256_mul_ps(vDz_dir_v, Wz)));
                        mask = _mm256_and_ps(mask, _mm256_cmp_ps(DW, vZero_v, _CMP_GT_OQ));

                        const __m256 safeW2 = _mm256_blendv_ps(vOne_v, W2, mask);
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

                        __m256 fogAtten = vOne_v;
                        if (invFogZ > 0.0f) {
                            fogAtten = _mm256_sub_ps(vOne_v, _mm256_mul_ps(vZ, vInvFogZ_v));
                            fogAtten = _mm256_max_ps(vZero_v, fogAtten);
                            fogAtten = _mm256_mul_ps(fogAtten, fogAtten);
                        }

                        if (sm) {
                            alignas(32) float maskArr[8], zArr[8];
                            _mm256_store_ps(maskArr, mask);
                            _mm256_store_ps(zArr, vZ);
                            alignas(32) float shadowMul[8] =
                                {1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
                            for (int lane = 0; lane < 8; ++lane) {
                                if (maskArr[lane] == 0) continue;
                                const float zL = zArr[lane];
                                const float Xl = Xarr[lane];
                                float zX = zL * Xl, zY = zL * Y, zZ = zL;
                                if (smMirror) {
                                    const float t2r = 2.0f *
                                        (nvx*zX + nvy*zY + nvz*zZ + dv_pl);
                                    zX -= t2r * nvx;
                                    zY -= t2r * nvy;
                                    zZ -= t2r * nvz;
                                }
                                const float lx = sm_m00*zX + sm_m01*zY + sm_m02*zZ + sm_ox;
                                const float ly = sm_m10*zX + sm_m11*zY + sm_m12*zZ + sm_oy;
                                const float lz = sm_m20*zX + sm_m21*zY + sm_m22*zZ + sm_oz;
                                if (lz <= 0.0f) continue;
                                const float invLZ = 1.0f / lz;
                                const float smX = sm_cntrX + sm_perspX * lx * invLZ;
                                const float smY = sm_cntrY - sm_perspY * ly * invLZ;
                                const int iX = int(smX), iY = int(smY);
                                if (uint32_t(iX) >= uint32_t(sm_xres) ||
                                    uint32_t(iY) >= uint32_t(sm_yres)) continue;
                                int pixZ = 0xFF80 - int(lz * sm_zScale);
                                if (pixZ < 0) pixZ = 0;
                                if (pixZ > 0xFFFF) pixZ = 0xFFFF;
                                const int biased = pixZ + 128;
                                const uint16_t shadowZ =
                                    sm_depth[size_t(iY) * size_t(sm_xres) + size_t(iX)];
                                if (biased < int(shadowZ)) shadowMul[lane] = 0.0f;
                            }
                            const __m256 vShad = _mm256_load_ps(shadowMul);
                            mask = _mm256_and_ps(mask,
                                _mm256_cmp_ps(vShad, _mm256_set1_ps(0.5f), _CMP_GT_OQ));
                        }

                        __m256 contrib = _mm256_mul_ps(
                            _mm256_mul_ps(coneAtten, distAtten),
                            _mm256_mul_ps(fogAtten, surfaceFade));
                        contrib = _mm256_and_ps(contrib, mask);
                        accV = _mm256_add_ps(accV, contrib);
                    }
                    }

                    alignas(32) float accArr[8];
                    _mm256_store_ps(accArr, accV);
                    const float colB = lights->colB[li];
                    const float colG = lights->colG[li];
                    const float colR = lights->colR[li];
                    // March total = nSamp × mean; cone_strength is
                    // calibrated against the global N_SAMPLES, so the
                    // narrow-cone 16-sample march renormalizes — the
                    // extra samples buy smoothness, not brightness.
                    // The analytic/hybrid branch never marched: its
                    // result is already N_SAMPLES × mean (no renorm —
                    // applying it dimmed the hybrid beams 4× into
                    // invisibility).
                    const float nNorm = useAnalytic
                        ? 1.0f : float(N_SAMPLES) / float(nSamp);
                    for (int lane = 0; lane < 8; ++lane) {
                        if (accArr[lane] <= 0.0f) continue;
                        const float w = accArr[lane] * density * nNorm;
                        accB[lane] += w * colB;
                        accG[lane] += w * colG;
                        accR[lane] += w * colR;
                    }
                }

                for (int lane = 0; lane < laneCount; ++lane) {
                    if (accB[lane] <= 0.0f && accG[lane] <= 0.0f && accR[lane] <= 0.0f) continue;
                    const int px = pxBase + lane;
                    const size_t i = row + size_t(px);
                    const dword pix = out[i];
                    int newR = int((pix >> 16) & 0xFF) + int(accR[lane]);
                    int newG = int((pix >>  8) & 0xFF) + int(accG[lane]);
                    int newB = int( pix        & 0xFF) + int(accB[lane]);
                    if (newR > 255) newR = 255;
                    if (newG > 255) newG = 255;
                    if (newB > 255) newB = 255;
                    out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                             |  dword(newB)        | 0xFF000000u;
                    if (dupRow) {
                        const size_t i2 = i + size_t(XRes);
                        const dword p2 = out[i2];
                        int r2 = int((p2 >> 16) & 0xFF) + int(accR[lane]);
                        int g2 = int((p2 >>  8) & 0xFF) + int(accG[lane]);
                        int b2 = int( p2        & 0xFF) + int(accB[lane]);
                        if (r2 > 255) r2 = 255;
                        if (g2 > 255) g2 = 255;
                        if (b2 > 255) b2 = 255;
                        out[i2] = (dword(r2) << 16) | (dword(g2) << 8)
                                  |  dword(b2)        | 0xFF000000u;
                    }
                }
            }
        } else {
        for (int px = x1; px < x2; ++px) {
            const float X = (float(px) - CntrEX) * invFOVX;
            const float uV = X*X + Y*Y + 1.0f;

            // Stratified per-pixel jitter offset, in [0,1). Used inside the
            // per-spot integration to randomize sample positions within
            // each bin so the bright apex region (where distAtten peaks
            // sharply) doesn't alias into visible bands across neighbours.
            // Hash pixel coords for stability frame-to-frame (no flicker).
            // Use a proper avalanching hash (PCG-style multiply + xor-shift):
            // a plain `px*MUL + py*MUL` left adjacent pixels with nearly
            // identical high-16 bits, which manifested as horizontal bands
            // because `frac` (computed from h>>16) was nearly constant in
            // each row.
            uint32_t pxHash = uint32_t(px) * 0x9E3779B9u
                            + uint32_t(py) * 0x85EBCA6Bu
                            + 0xCAFEBABEu;
            pxHash ^= pxHash >> 13;
            pxHash *= 0xC2B2AE35u;
            pxHash ^= pxHash >> 16;

            // Surface depth: 0xFF80 - enc = z*zscale. enc=0 means "sky"
            // (no surface) → cap at fogZ if fogged, else far.
            const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
            const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
            float zMax = (zSurf > 0.0f) ? zSurf : zSky;
            if (fogZ > 0.0f && zMax > fogZ) zMax = fogZ;
            constexpr float zMin = 0.05f;
            if (zMax <= zMin) continue;

            float accB = 0.0f, accG = 0.0f, accR = 0.0f;
            for (int s = 0; s < spotCount; ++s) {
                const int li = spotIdx[s];
                const float Px = lights->posX[li], Py = lights->posY[li], Pz = lights->posZ[li];
                const float Dx = lights->dirX[li], Dy = lights->dirY[li], Dz = lights->dirZ[li];
                const float cosO = lights->cosOuter[li];
                const float cosI = lights->cosInner[li];
                const float r2   = lights->range2[li];
                const float rr   = lights->rRange[li];
                // Clone-beam footprint gate (vec path has the same;
                // scalar fallback keeps correctness for A/B).
                const uint32_t omid_s = lights->mirrorId[li];
                if (omid_s != 0 && (!mmask || !mmz)) continue;
                const bool bounce_s = lights->bounceClamp[li] != 0;
                float hsNx_s=0, hsNy_s=0, hsNz_s=0, hsD_s=0;
                if (bounce_s) {
                    const float Nx = lights->mirNX[li];
                    const float Ny = lights->mirNY[li];
                    const float Nz = lights->mirNZ[li];
                    const auto &VW = g_deferredCtx.viewToWorld;
                    hsNx_s = VW[0][0]*Nx + VW[1][0]*Ny + VW[2][0]*Nz;
                    hsNy_s = VW[0][1]*Nx + VW[1][1]*Ny + VW[2][1]*Nz;
                    hsNz_s = VW[0][2]*Nx + VW[1][2]*Ny + VW[2][2]*Nz;
                    hsD_s  = Nx*g_deferredCtx.cameraWorldX +
                             Ny*g_deferredCtx.cameraWorldY +
                             Nz*g_deferredCtx.cameraWorldZ +
                             lights->mirD[li];
                    if (hsD_s == 0.0f) continue;
                }

                const float DV = Dx*X + Dy*Y + Dz;
                const float DP = Dx*Px + Dy*Py + Dz*Pz;
                const float VP = X*Px + Y*Py + Pz;
                const float PP = Px*Px + Py*Py + Pz*Pz;
                const float c2 = cosO * cosO;

                const float a = DV*DV - c2 * uV;
                const float b = 2.0f * (c2 * VP - DV * DP);
                const float cq = DP*DP - c2 * PP;

                // Solve a*z² + b*z + cq = 0 (the "ray inside cone half-
                // angle" quadratic). The sign of a controls which side
                // of the roots is "inside-cone":
                //   a<0 → ray crosses the cone direction broadside.
                //         Inside-cone is BETWEEN [r1, r2]. (Looking AT
                //         the cone from outside.)
                //   a>0 → ray fits within the cone half-angle (looking
                //         ALONG the cone direction, from any position).
                //         Inside-cone is OUTSIDE [r1, r2] (z ≤ r1 OR
                //         z ≥ r2). Classify by where the visible
                //         interval [zMin, zMax] sits and either
                //         integrate it fully or skip entirely. Per-
                //         sample DW>0 + cosT≥cosO filters cull the
                //         outside-cone middle when visible straddles.
                //   a≈0 → ray exactly parallel to cone wall, no volume.
                //
                // Sphere bounds (the spot's range sphere) also clamp the
                // interval below — and decouple sample positions from
                // the quantized surface depth (zMax fades per-sample).
                const float sphereC = PP - r2;
                const float sphereDisc = VP*VP - uV * sphereC;
                if (sphereDisc < 0.0f) continue;  // ray misses range sphere
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
                        // Q always positive → ray entirely inside cone
                        // (forward half filtered per-sample).
                        zLo = zMin;
                        zHi = zMax;
                    } else {
                        const float sq = std::sqrt(disc);
                        const float inv2a = 1.0f / (2.0f * a);
                        const float root1 = (-b - sq) * inv2a;
                        const float root2 = (-b + sq) * inv2a;
                        const float r1Q = std::min(root1, root2);
                        const float r2Q = std::max(root1, root2);
                        // For a>0, inside-cone is z ≤ r1 OR z ≥ r2.
                        // The forward filter zFwd = DP/DV lies between
                        // r1 and r2 (zFwd is the apex projection along
                        // the ray; for a>0 the apex projection sits in
                        // the outside-cone middle between cone-wall
                        // crossings). So the forward-inside-cone
                        // region is one side:
                        //   DV > 0 → forward is z > zFwd → take z ≥ r2
                        //   DV < 0 → forward is z < zFwd → take z ≤ r1
                        //   DV ≈ 0 → ray perpendicular to cone dir,
                        //            no meaningful forward direction.
                        // This is tighter than integrating [zMin, zMax]
                        // and skipping wrong-side samples per-sample:
                        // sample positions stay entirely inside the
                        // cone, eliminating the cone-wall-sweep stripe
                        // artifact that the wider interval produced.
                        if (DV > 1e-6f) {
                            zLo = std::max(r2Q, zMin);
                            zHi = zMax;
                        } else if (DV < -1e-6f) {
                            zLo = zMin;
                            zHi = std::min(r1Q, zMax);
                        } else {
                            continue;
                        }
                        if (zHi <= zLo) continue;
                    }
                } else {
                    continue;
                }

                // Intersect with sphere bounds (NOT with zMax — that goes
                // into the per-sample fade below). zMin keeps us forward
                // of the near plane.
                if (zLo < zSphLo) zLo = zSphLo;
                if (zHi > zSphHi) zHi = zSphHi;
                if (zLo < zMin)   zLo = zMin;
                if (zHi <= zLo)   continue;
                if (omid_s != 0) {
                    const size_t pi = size_t(py) * size_t(XRes) + size_t(px);
                    if (uint32_t(mmask[pi]) != omid_s) continue;
                    const float zWall =
                        float(0xFF80 - int(mmz[pi])) * invZScale;
                    if (zLo < zWall) zLo = zWall;
                    if (zHi <= zLo) continue;
                }
                if (bounce_s) {
                    const float kk = hsNx_s*X + hsNy_s*Y + hsNz_s;
                    if ((hsD_s > 0.0f && kk < -1e-9f) ||
                        (hsD_s < 0.0f && kk >  1e-9f)) {
                        const float zStar = -hsD_s / kk;
                        if (zHi > zStar) zHi = zStar;
                        if (zHi <= zLo) continue;
                    }
                }
                // Early-out: entire cone interval past the visible surface
                // (zMax is the surface/sky cap; everything past it is fully
                // occluded). Without this we'd still loop N samples for no
                // contribution.
                if (zLo >= zMax)  continue;

                // Forward-cone-half constraint: need D·W ≥ 0 i.e.
                //   z * DV - DP ≥ 0. Resolves to z ≥ DP/DV (if DV>0)
                // or z ≤ DP/DV (if DV<0). Skip if entire segment violates.
                if (std::fabs(DV) > 1e-6f) {
                    const float zFwd = DP / DV;
                    if (DV > 0.0f) { if (zLo < zFwd) zLo = zFwd; }
                    else           { if (zHi > zFwd) zHi = zFwd; }
                    if (zLo >= zHi) continue;
                }

                // Integrate N stratified-jittered samples along [zLo, zHi].
                // Each bin gets one sample placed at a random offset within
                // it — randomization breaks the periodic alignment with
                // the bright apex region that produced visible stripe
                // artifacts at fixed-position sampling. The per-spot salt
                // (s * 0x6F...) avoids correlated noise when multiple spots
                // contribute to the same pixel.
                const float dz = (zHi - zLo) * inv_N;
                const float inv_dz = 1.0f / dz;
                const float zFadeStart = zMax - dz;
                // Hoist per-spot shadow-map state out of the per-sample
                // loop. smIdx is per-light, not per-sample.
                const int32_t smIdx = lights->shadowMapIdx[li];
                const ShadowMap *sm = (smIdx >= 0 && size_t(smIdx) < g_shadowMaps.size())
                                       ? &g_shadowMaps[smIdx] : nullptr;
                // Per-spot precomputed shadow matrix rows (when sm != null).
                // Lets the per-sample shadow code use cached scalars instead
                // of indexing sm->viewToLight[r][c] each sample.
                float sm_m00=0, sm_m01=0, sm_m02=0, sm_ox=0;
                float sm_m10=0, sm_m11=0, sm_m12=0, sm_oy=0;
                float sm_m20=0, sm_m21=0, sm_m22=0, sm_oz=0;
                float sm_cntrX=0, sm_cntrY=0, sm_perspX=0, sm_perspY=0;
                float sm_zScale=0;
                const uint16_t *sm_depth = nullptr;
                int sm_xres=0, sm_yres=0;
                if (sm) {
                    sm_m00=sm->viewToLight[0][0]; sm_m01=sm->viewToLight[0][1]; sm_m02=sm->viewToLight[0][2]; sm_ox=sm->viewToLightOffset.x;
                    sm_m10=sm->viewToLight[1][0]; sm_m11=sm->viewToLight[1][1]; sm_m12=sm->viewToLight[1][2]; sm_oy=sm->viewToLightOffset.y;
                    sm_m20=sm->viewToLight[2][0]; sm_m21=sm->viewToLight[2][1]; sm_m22=sm->viewToLight[2][2]; sm_oz=sm->viewToLightOffset.z;
                    sm_cntrX=sm->cntrX; sm_cntrY=sm->cntrY;
                    sm_perspX=sm->perspX; sm_perspY=sm->perspY;
                    sm_zScale=sm->zScale;
                    sm_depth=sm->depth.data();
                    sm_xres=sm->xres; sm_yres=sm->yres;
                }
                const float inv_cosI_minus_cosO = 1.0f / (cosI - cosO);
                float acc = 0.0f;
                for (int k = 0; k < N_SAMPLES; ++k) {
                    const uint32_t h = pxHash
                        + uint32_t(k) * 0x9E3779B9u
                        + uint32_t(s) * 0x6F4A7531u;
                    const float frac = float(h >> 16) * (1.0f / 65536.0f);
                    const float z = zLo + (float(k) + frac) * dz;
                    if (z >= zMax) break;
                    float surfaceFade = 1.0f;
                    if (z > zFadeStart) {
                        surfaceFade = (zMax - z) * inv_dz;
                    }
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
                        const float t = (cosT - cosO) * inv_cosI_minus_cosO;
                        coneAtten = t * t * (3.0f - 2.0f * t);
                    }
                    const float dr = dist * rr;
                    const float cutoff = 1.0f - dr;
                    const float invSq  = 1.0f / (dr * dr + 0.05f);
                    const float distAtten = cutoff * cutoff * invSq;
                    float fogAtten = 1.0f;
                    if (invFogZ > 0.0f) {
                        fogAtten = 1.0f - z * invFogZ;
                        if (fogAtten < 0.0f) fogAtten = 0.0f;
                        fogAtten *= fogAtten;
                    }
                    // Shadow sample. sm != null fast-checked once per spot;
                    // matrix rows + map metadata cached as scalars above.
                    if (sm) {
                        const float zX = z*X, zY = z*Y;
                        const float lx = sm_m00*zX + sm_m01*zY + sm_m02*z + sm_ox;
                        const float ly = sm_m10*zX + sm_m11*zY + sm_m12*z + sm_oy;
                        const float lz = sm_m20*zX + sm_m21*zY + sm_m22*z + sm_oz;
                        if (lz > 0.0f) {
                            const float invLZ = 1.0f / lz;
                            const float smX = sm_cntrX + sm_perspX * lx * invLZ;
                            const float smY = sm_cntrY - sm_perspY * ly * invLZ;
                            const int iX = int(smX);
                            const int iY = int(smY);
                            if (uint32_t(iX) < uint32_t(sm_xres) &&
                                uint32_t(iY) < uint32_t(sm_yres)) {
                                int pixZ = 0xFF80 - int(lz * sm_zScale);
                                if (pixZ < 0) pixZ = 0;
                                if (pixZ > 0xFFFF) pixZ = 0xFFFF;
                                const int biased = pixZ + 128;
                                const uint16_t shadowZ =
                                    sm_depth[size_t(iY) * size_t(sm_xres) + size_t(iX)];
                                if (biased < int(shadowZ)) continue;  // shadowed
                            }
                        }
                    }
                    acc += coneAtten * distAtten * fogAtten * surfaceFade;
                }
                if (acc <= 0.0f) continue;
                // No dz scaling — the path-integral form (acc × dz) gave
                // shallow-angle rays through far cones much brighter results
                // than close cones (where each pixel's ray-cone segment is
                // short). Using per-sample-sum (acc only) combined with the
                // inverse-square distAtten above gives roughly position-
                // invariant brightness, biased toward close cones — matches
                // the "flashlight in fog" mental model.
                const float w = acc * density;
                accB += w * lights->colB[li];
                accG += w * lights->colG[li];
                accR += w * lights->colR[li];
            }
            if (accB <= 0.0f && accG <= 0.0f && accR <= 0.0f) continue;
            const size_t i = row + size_t(px);
            const dword pix = out[i];
            int newR = int((pix >> 16) & 0xFF) + int(accR);
            int newG = int((pix >>  8) & 0xFF) + int(accG);
            int newB = int( pix        & 0xFF) + int(accB);
            if (newR > 255) newR = 255;
            if (newG > 255) newG = 255;
            if (newB > 255) newB = 255;
            out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                     |  dword(newB)        | 0xFF000000u;
        }
        }
    }
}

// Volumetric-pass timing accumulators — struct + RAII scope live in
// DeferredCommon.h (the unified pass in DeferredFastFog.cpp shares them).
VolProf g_volProf;

// Called once per frame after all volumetric passes complete to maybe
// flush the timing summary. No-op when vol_prof flag is off.
void VolProf_Tick();

static void VolProf_Tick_impl() {
    if (!fds::FeatureFlags::vol_prof()) return;
    if (++g_volProf.framesSeen < g_volProf.interval) return;
    const int N = g_volProf.framesSeen;
    const int cAnalytic = g_coneAnalyticHits.load(std::memory_order_relaxed);
    const int cRaymarch = g_coneRaymarchHits.load(std::memory_order_relaxed);
    std::fprintf(stderr,
        "[VOL-PROF] last %d frame(s) avg per-frame: cones=%.2fms halos=%.2fms "
        "unified=%.2fms sky=%.2fms (calls c=%d h=%d u=%d s=%d) "
        "cone-path: analytic=%d raymarch=%d\n",
        N,
        g_volProf.ms_cones   / N,
        g_volProf.ms_halos   / N,
        g_volProf.ms_unified / N,
        g_volProf.ms_skybox  / N,
        g_volProf.n_cones, g_volProf.n_halos, g_volProf.n_unified,
        g_volProf.n_skybox,
        cAnalytic, cRaymarch);
    std::fflush(stderr);
    g_volProf = VolProf{};
    g_coneAnalyticHits.store(0, std::memory_order_relaxed);
    g_coneRaymarchHits.store(0, std::memory_order_relaxed);
}

void VolProf_Tick() { VolProf_Tick_impl(); }

void Render_VolumetricCones() {
    VolProfScope _vp(&g_volProf.ms_cones, &g_volProf.n_cones);
    if (!CurScene || !ZPage16 || !VPage) return;
    const bool allCones = fds::FeatureFlags::draw_cones();
    const float invFOVX = 1.0f / FOVX;
    const float invFOVY = 1.0f / FOVY;
    const float invZScale = 1.0f / float(g_zscale);
    // Density: per-step contribution coefficient. Tunable via existing
    // FDS_CONE_STRENGTH. Empirical: 0.0005-0.002 for City-scale (range
    // in thousands).
    const float density = fds::FeatureFlags::cone_strength() * 0.001f;
    // Zero density = zero contribution: skip the whole per-pixel pass
    // (it previously ran the full integration and multiplied by 0 at
    // the end — --cone-strength=0 benched identical to default).
    if (density <= 0.0f) return;

    // Fog cutoff + per-sample attenuation. Matches Render_DeferredFogPass:
    // cones fade by (1 - z/FZP) so they don't extend past where geometry
    // already fully fogged out. fogZ <= 0 disables (unfogged scenes).
    const float fogZ    = (CurScene->Flags & Scn_Fogged) ? CurScene->FZP : 0.0f;
    const float invFogZ = (fogZ > 0.0f) ? 1.0f / fogZ : 0.0f;

    // Iterate the frame-global ViewLightsSoA built by Render_DeferredLighting
    // (g_deferredCtx.lights / .numLights). The per-tile TileLights apply a
    // surface-z cull that's incorrect for volumetric integration — see the
    // note inside Render_VolumetricCones_Tile.
    extern DeferredLightingCtx g_deferredCtx;
    const ViewLightsSoA *const lights = g_deferredCtx.lights;
    if (!lights) return;
    const int numLights = g_deferredCtx.numLights;

    // Pre-filter spotlight indices once per frame; tiles share the result.
    // Mirror-clone spots ARE admitted (beams show in mirrors): the tile
    // fn gates them per pixel on the mirror footprint mask and clamps
    // the chord to start at the wall depth — same containment the halo
    // pass uses for clone-omni glows. Ungated they'd wash additive glow
    // across pixels IN FRONT of the mirror, which is why they were
    // excluded before the gate existed.
    static int spotIdx[DEFERRED_MAX_VIEW_LIGHTS];
    int spotCount = 0;
    for (int i = 0; i < numLights; ++i) {
        if (lights->isSpot[i] &&
            (allCones || lights->forceCone[i])) spotIdx[spotCount++] = i;
    }
    if (spotCount == 0) return;
    constexpr int numTilesX = 6;
    constexpr int numTilesY = 4;
    constexpr int numTiles  = numTilesX * numTilesY;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;

    // Per-tile spot filtering. Mirror buildTileLightLists's screen-space
    // sphere projection but WITHOUT the z-cull (which caused the
    // tile-stripe artifact fixed in the prior commit). For sparse scenes
    // most tiles will see 0 spots — the per-pixel inner loop short-
    // circuits via spotCount==0.
    static int tileSpotIdx  [numTiles][DEFERRED_MAX_VIEW_LIGHTS];
    static int tileSpotCount[numTiles];
    for (int t = 0; t < numTiles; ++t) tileSpotCount[t] = 0;

    for (int s = 0; s < spotCount; ++s) {
        const int li = spotIdx[s];
        const float vx = lights->posX[li];
        const float vy = lights->posY[li];
        const float vz = lights->posZ[li];
        const float r  = std::sqrt(lights->range2[li]);
        if (vz + r < 0.0f) continue;  // entirely behind camera

        int ti_lo, ti_hi, tj_lo, tj_hi;
        if (vz - r < 1.0f) {
            // Sphere straddles near plane: be conservative, tag every tile.
            ti_lo = 0; ti_hi = numTilesX - 1;
            tj_lo = 0; tj_hi = numTilesY - 1;
        } else {
            const float invZ = 1.0f / vz;
            const float cx = CntrEX + vx * FOVX * invZ;
            const float cy = CntrEY - vy * FOVY * invZ;
            const float rx = r * FOVX * invZ;
            const float ry = r * FOVY * invZ;
            const int sx_min = std::max(0,        int(std::floor(cx - rx)));
            const int sx_max = std::min(XRes - 1, int(std::ceil (cx + rx)));
            const int sy_min = std::max(0,        int(std::floor(cy - ry)));
            const int sy_max = std::min(YRes - 1, int(std::ceil (cy + ry)));
            if (sx_min > sx_max || sy_min > sy_max) continue;
            ti_lo = sx_min / tileSizeX;
            ti_hi = std::min(numTilesX - 1, sx_max / tileSizeX);
            tj_lo = sy_min / tileSizeY;
            tj_hi = std::min(numTilesY - 1, sy_max / tileSizeY);
        }
        // Cone-vs-tile cull: the volumetric ray spans [near, surface],
        // so the relevant chunk is the cone tile's screen rect swept
        // from the near plane to the deepest surface beneath it (max
        // of the 2x2 underlying 12x8 surface tiles' zMax; empty tiles
        // → FZP). Without this every narrow beam pays the per-pixel
        // quadratic + segment integral in nearly every tile.
        const bool coneCull = fds::FeatureFlags::spot_cone_cull() &&
                              g_deferredCtx.tileLights != nullptr;
        const float sinO_cull = lights->sinOuter[li];
        const float fzpFar = CurScene->FZP > 0.0f ? CurScene->FZP : 1e4f;
        for (int j = tj_lo; j <= tj_hi; ++j) {
            for (int i = ti_lo; i <= ti_hi; ++i) {
                const int t = j * numTilesX + i;
                if (coneCull) {
                    float zHiT = -1e30f;
                    for (int sj = 0; sj < 2; ++sj)
                        for (int si = 0; si < 2; ++si) {
                            const int st = (j*2 + sj) * DEFERRED_NUM_TILES_X + (i*2 + si);
                            const float zm = g_deferredCtx.tileLights[st].zMax;
                            zHiT = std::max(zHiT, (zm > 0.0f && zm < 1e30f) ? zm : fzpFar);
                        }
                    const float pad  = r * sinO_cull;
                    const float dirZ = lights->dirZ[li];
                    const float czLo = std::min(vz, vz + dirZ * r) - pad;
                    const float czHi = std::max(vz, vz + dirZ * r) + pad;
                    const float zLoC = std::max(0.05f, czLo);
                    const float zHiC = std::min(zHiT, czHi);
                    if (zHiC < zLoC) continue;  // no z overlap
                    const TileChunkSphere cs = tileChunkSphere(
                        float(i * tileSizeX), float(std::min((i+1) * tileSizeX, int(XRes))),
                        float(j * tileSizeY), float(std::min((j+1) * tileSizeY, int(YRes))),
                        zLoC, zHiC);
                    if (cs.valid &&
                        sphereOutsideCone(cs.cx, cs.cy, cs.cz, cs.R,
                                          vx, vy, vz,
                                          lights->dirX[li], lights->dirY[li],
                                          lights->dirZ[li],
                                          r, lights->cosOuter[li], sinO_cull))
                        continue;
                }
                if (tileSpotCount[t] < DEFERRED_MAX_VIEW_LIGHTS) {
                    tileSpotIdx[t][tileSpotCount[t]++] = li;
                }
            }
        }
    }

    renderns::tileCounter = 0;
    for (int j = 0; j < numTilesY; ++j) {
        const int y1 = tileSizeY * j;
        const int y2 = std::min(y1 + tileSizeY, YRes);
        for (int i = 0; i < numTilesX; ++i) {
            const int x1 = tileSizeX * i;
            const int x2 = std::min(x1 + tileSizeX, XRes);
            const int tileIdx = j * numTilesX + i;
            const int *ts = tileSpotIdx[tileIdx];
            const int  tc = tileSpotCount[tileIdx];
            ThreadPool::instance().enqueue([x1,y1,x2,y2,lights,ts,tc,
                                            invFOVX,invFOVY,invZScale,density,
                                            fogZ,invFogZ]() {
                Render_VolumetricCones_Tile(x1,y1,x2,y2, lights, ts, tc,
                                             invFOVX,invFOVY,invZScale,density,
                                             fogZ,invFogZ);
                // One permit per completed tile (see renderns::tileDone).
                renderns::tileDone.release();
            });
        }
    }
    for (int _i = 0; _i < numTiles; ++_i) {
        renderns::tileDone.acquire();
    }
}

// ─── Omni halos — standalone additive pass for legacy mode ───────────
//
// Same idea as Render_VolumetricCones but for omnidirectional lights:
// ray-march each omni's range sphere, accumulate inverse-square
// in-scatter contribution per sample, composite additively. No fog
// integration (legacy mode keeps fog in Render_DeferredFogPass).
//
// Per pixel:
//   for each omni in tile:
//     [zLo, zHi] = ray ∩ sphere(omni.center, omni.range)
//     clamp by [zMin, zMax=zSurf]
//     ∫ samples: density × 1/(1+(d/R)²) × color
//     add to pixel
//
// Gated by FDS_OMNI_HALO_STRENGTH > 0. Replaces the omni-halo block
// that previously only existed inside the unified pass; called from
// the legacy dispatch (volumetric_unified=0) so City + other scenes
// that stay on legacy passes can still get omni halos.
static void Render_OmniHalos_Tile(
    int x1, int y1, int x2, int y2,
    const ViewLightsSoA *lights,
    const int *omniIdx, int omniCount,
    float invFOVX, float invFOVY,
    float invZScale,
    float fogZ, float invFogZ,
    float density)
{
    if (omniCount == 0) return;
    dword *out = reinterpret_cast<dword*>(VPage);
    const uint16_t *zEnc = ZPage16;
    const int N_SAMPLES = std::max(1, fds::FeatureFlags::vol_n_samples());
    const float inv_N = 1.0f / float(N_SAMPLES);
    const bool vecPath = fds::FeatureFlags::vol_vec();
    const bool analyticHalo = fds::FeatureFlags::vol_halo_analytic();
    const float noiseStrength = fds::FeatureFlags::vol_analytic_noise();
    // Mirror gate planes for clone-omni halos (see Render_OmniHalos's
    // list-build comment). Null when the scene has no mirrors — and
    // then no clone omnis are in the list either.
    const meka::u8 *mmask = (g_gbuffer && !g_gbuffer->mirrorMask.empty())
        ? g_gbuffer->mirrorMask.data() : nullptr;
    const uint16_t *mmz = (g_gbuffer && !g_gbuffer->mirrorMaskZ.empty())
        ? g_gbuffer->mirrorMaskZ.data() : nullptr;

    // ─── Analytic halo path ────────────────────────────────────────────
    // For each pixel/omni, the in-sphere line integral of inverse-square
    // attenuation has a closed form: ∫1/(αz²+βz+γ)dz = (2/D)·arctan
    // ((2αz+β)/D) where D = sqrt(4αγ−β²). We drop the original (1-d/r)²
    // cutoff term (which would require integrating √(quadratic) and has
    // no elementary form) — visually this means a sharper boundary at
    // the omni's range edge instead of a soft fade. For a glow effect
    // it's acceptable; for accurate medium-density attenuation use the
    // ray-march path (--no-vol_halo_analytic).
    //
    // Cost per pixel/omni: ~10 fmuls + 1 atan + 1 sqrt + 1 div instead
    // of N×30 ops. Scalar atan is ~10ns on M-series; that's still <1ns
    // per useful pixel-pass after the per-pixel setup.
    if (analyticHalo && vecPath) {
        // ── Pixel-major SIMD analytic halo ──────────────────────────
        // Outer: per 8-pixel batch. Per-lane scalar sphere intersection
        // (has a sphereDisc<0 reject branch), then 8-wide vec analytic
        // integral via atan_approx_x8.
        for (int py = y1; py < y2; ++py) {
            const float Y = (CntrEY - float(py)) * invFOVY;
            const size_t row = size_t(py) * size_t(XRes);
            for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                const int pxEnd     = std::min(pxBase + 8, x2);
                const int laneCount = pxEnd - pxBase;

                alignas(32) float Xarr[8] = {}, uVarr[8] = {}, zMaxArr[8] = {};
                alignas(32) float noiseBuf[8] = {1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
                bool anyAlive = false;
                for (int lane = 0; lane < laneCount; ++lane) {
                    const int px = pxBase + lane;
                    const float X = (float(px) - CntrEX) * invFOVX;
                    Xarr[lane]  = X;
                    uVarr[lane] = X*X + Y*Y + 1.0f;
                    const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                    const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
                    float zM = (zSurf > 0.0f) ? zSurf : zSky;
                    if (fogZ > 0.0f && zM > fogZ) zM = fogZ;
                    constexpr float zMin = 0.05f;
                    if (zM > zMin) { zMaxArr[lane] = zM; anyAlive = true; }
                    if (noiseStrength > 0.0f) {
                        // Same avalanching hash the ray-march path uses
                        // (PCG-style xor-shift + multiply) so analytic +
                        // ray-march visually agree if you toggle between.
                        uint32_t h = uint32_t(px) * 0x9E3779B9u
                                   + uint32_t(py) * 0x85EBCA6Bu
                                   + 0xCAFEBABEu;
                        h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
                        const float u = float(h >> 16) * (1.0f/65536.0f);
                        noiseBuf[lane] = 1.0f + noiseStrength * (u - 0.5f);
                    }
                }
                if (!anyAlive) continue;
                const __m256 vNoise = _mm256_load_ps(noiseBuf);

                alignas(32) float accB[8] = {}, accG[8] = {}, accR[8] = {};

                for (int o = 0; o < omniCount; ++o) {
                    const int li = omniIdx[o];
                    const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
                    // Halo uses per-omni halo*[] (decoupled from surface
                    // range/rRange). HaloRange=0 in the Omni struct
                    // falls back to IRange — handled at SoA build time.
                    const float r2 = lights->haloRange2[li];
                    const float rr = lights->haloRRange[li];
                    const float perOmniDensity = density * lights->haloDensityMul[li];
                    const float PP = Px*Px + Py_l*Py_l + Pz*Pz;
                    const float rr2 = rr * rr;
                    // Mirror-clone omni: glow only inside the owning
                    // mirror's stamped footprint, and only on the ray
                    // segment BEHIND the wall surface (start the
                    // integral at the wall depth). That is exactly the
                    // reflected image of the source omni's glow between
                    // the wall and the (reflected) scene.
                    const uint32_t omid = lights->mirrorId[li];
                    if (omid != 0 && (!mmask || !mmz)) continue;

                    // Per-lane scalar sphere bounds → zLoArr, zHiArr.
                    alignas(32) float zLoArr[8] = {}, zHiArr[8] = {};
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
                        const float invUV = 1.0f / uV;
                        float zLo = (VP - sphereSq) * invUV;
                        float zHi = (VP + sphereSq) * invUV;
                        if (zLo < zMin) zLo = zMin;
                        if (zHi > zMax) zHi = zMax;
                        if (omid != 0) {
                            const size_t pi = row + size_t(pxBase + lane);
                            if (uint32_t(mmask[pi]) != omid) continue;
                            const float zWall =
                                float(0xFF80 - int(mmz[pi])) * invZScale;
                            if (zLo < zWall) zLo = zWall;
                        }
                        if (zHi <= zLo) continue;
                        zLoArr[lane] = zLo;
                        zHiArr[lane] = zHi;
                        aliveLane[lane] = 1.0f;
                        omniAlive = true;
                    }
                    if (!omniAlive) continue;

                    // 8-wide vec: alpha, beta, gamma → D, invD → arg → atan.
                    const __m256 vY        = _mm256_set1_ps(Y);
                    const __m256 vX_v      = _mm256_load_ps(Xarr);
                    const __m256 vUv       = _mm256_load_ps(uVarr);
                    const __m256 vZLo      = _mm256_load_ps(zLoArr);
                    const __m256 vZHi      = _mm256_load_ps(zHiArr);
                    const __m256 vAlive_v  = _mm256_load_ps(aliveLane);
                    const __m256 vPx       = _mm256_set1_ps(Px);
                    const __m256 vPy       = _mm256_set1_ps(Py_l);
                    const __m256 vPz       = _mm256_set1_ps(Pz);
                    const __m256 vRR2      = _mm256_set1_ps(rr2);
                    const __m256 vPP       = _mm256_set1_ps(PP);
                    const __m256 vZero     = _mm256_setzero_ps();
                    const __m256 vOne      = _mm256_set1_ps(1.0f);
                    const __m256 vNegTwo   = _mm256_set1_ps(-2.0f);
                    const __m256 vFour     = _mm256_set1_ps(4.0f);
                    const __m256 vPt05     = _mm256_set1_ps(0.05f);
                    const __m256 mAlive    = _mm256_cmp_ps(vAlive_v, vZero, _CMP_GT_OQ);

                    // VP = X·Px + Y·Py + Pz, per lane (X varies)
                    const __m256 vVP = _mm256_fmadd_ps(vX_v, vPx,
                                       _mm256_fmadd_ps(vY, vPy, vPz));

                    // α = rr²·uV, β = -2·rr²·VP, γ = rr²·PP + 0.05
                    const __m256 vAlpha = _mm256_mul_ps(vRR2, vUv);
                    const __m256 vBeta  = _mm256_mul_ps(_mm256_mul_ps(vNegTwo, vRR2), vVP);
                    const __m256 vGamma = _mm256_fmadd_ps(vRR2, vPP, vPt05);
                    // discQ = 4αγ − β²
                    const __m256 vDiscQ = _mm256_fmsub_ps(_mm256_mul_ps(vFour, vAlpha), vGamma,
                                                          _mm256_mul_ps(vBeta, vBeta));
                    // Mask out lanes where discQ ≤ 0 (defensive — should be
                    // positive since sphere intersects ray).
                    const __m256 mDisc = _mm256_cmp_ps(vDiscQ, vZero, _CMP_GT_OQ);
                    const __m256 vMask = _mm256_and_ps(mAlive, mDisc);
                    const __m256 vSafeDisc = _mm256_blendv_ps(vOne, vDiscQ, vMask);
                    // rsqrt collapses sqrt + reciprocal into one ~3-cycle
                    // op (12-bit precision; ample for halo brightness vs
                    // div_ps's 10-20 cycles).
                    const __m256 vInvD = _mm256_rsqrt_ps(vSafeDisc);

                    // argHi = (2α·zHi + β) · invD, similarly argLo
                    const __m256 vTwoA = _mm256_add_ps(vAlpha, vAlpha);
                    const __m256 vArgHi = _mm256_mul_ps(vInvD,
                                          _mm256_fmadd_ps(vTwoA, vZHi, vBeta));
                    const __m256 vArgLo = _mm256_mul_ps(vInvD,
                                          _mm256_fmadd_ps(vTwoA, vZLo, vBeta));

                    // integral = 2·invD · (atan(argHi) − atan(argLo))
                    const __m256 vAtanHi = atan_approx_x8(vArgHi);
                    const __m256 vAtanLo = atan_approx_x8(vArgLo);
                    const __m256 vIntegral = _mm256_mul_ps(
                        _mm256_add_ps(vInvD, vInvD),
                        _mm256_sub_ps(vAtanHi, vAtanLo));

                    // Near-edge softness: replicate ray-march's (1-rr·d)²
                    // fade at the midpoint so analytic halos don't show
                    // a hard sphere boundary. Compute W² at z=zMid, then
                    // dist = √W², softEdge = max(0,1-rr·dist)².
                    const __m256 vZMid = _mm256_mul_ps(
                        _mm256_add_ps(vZLo, vZHi), _mm256_set1_ps(0.5f));
                    const __m256 Wx_m = _mm256_sub_ps(_mm256_mul_ps(vZMid, vX_v), vPx);
                    const __m256 Wy_m = _mm256_sub_ps(_mm256_mul_ps(vZMid, vY),  vPy);
                    const __m256 Wz_m = _mm256_sub_ps(vZMid, vPz);
                    const __m256 W2_m = _mm256_fmadd_ps(Wx_m, Wx_m,
                                        _mm256_fmadd_ps(Wy_m, Wy_m,
                                         _mm256_mul_ps(Wz_m, Wz_m)));
                    const __m256 safeW2_m = _mm256_blendv_ps(vOne, W2_m, vMask);
                    const __m256 invLen_m = _mm256_rsqrt_ps(safeW2_m);
                    const __m256 dist_m   = _mm256_mul_ps(W2_m, invLen_m);
                    const __m256 vRR      = _mm256_set1_ps(rr);
                    __m256 softEdge_m     = _mm256_sub_ps(vOne,
                                            _mm256_mul_ps(vRR, dist_m));
                    softEdge_m = _mm256_max_ps(vZero, softEdge_m);
                    softEdge_m = _mm256_mul_ps(softEdge_m, softEdge_m);

                    // w = integral · perOmniDensity · N / interval · softEdge —
                    // rcp is fine for halo (visual effect, not precision-
                    // critical). perOmniDensity folds in HaloIntensity.
                    const __m256 vIntervalLen = _mm256_sub_ps(vZHi, vZLo);
                    const __m256 vDensityN    = _mm256_set1_ps(perOmniDensity * float(N_SAMPLES));
                    const __m256 vSafeLen     = _mm256_blendv_ps(vOne, vIntervalLen, vMask);
                    const __m256 vW = _mm256_mul_ps(
                        _mm256_mul_ps(
                            _mm256_mul_ps(vIntegral, vDensityN),
                            softEdge_m),
                        _mm256_rcp_ps(vSafeLen));
                    // Per-pixel noise (vNoise = 1.0 when noiseStrength=0).
                    const __m256 vWNoised = _mm256_mul_ps(vW, vNoise);
                    const __m256 vWMasked = _mm256_and_ps(vWNoised, vMask);

                    alignas(32) float wArr[8];
                    _mm256_store_ps(wArr, vWMasked);
                    const float colR = lights->colR[li];
                    const float colG = lights->colG[li];
                    const float colB = lights->colB[li];
                    for (int lane = 0; lane < 8; ++lane) {
                        const float w = wArr[lane];
                        if (w <= 0.0f) continue;
                        accR[lane] += w * colR;
                        accG[lane] += w * colG;
                        accB[lane] += w * colB;
                    }
                }

                // Bayer-4x4 dither pattern (in [-0.5, +0.5)) — breaks
                // the visible color-banding that the smooth analytic
                // integral otherwise quantizes into when int-truncating
                // small floating-point contributions to 8-bit channels.
                // 16 stable per-pixel offsets (cheap, deterministic, no
                // flicker). Same pattern reused below for the scalar
                // analytic path.
                static constexpr float kBayer4[16] = {
                    -0.46875f, +0.03125f, -0.34375f, +0.15625f,
                    +0.28125f, -0.21875f, +0.40625f, -0.09375f,
                    -0.28125f, +0.21875f, -0.40625f, +0.09375f,
                    +0.46875f, -0.03125f, +0.34375f, -0.15625f,
                };
                for (int lane = 0; lane < laneCount; ++lane) {
                    if (accR[lane] <= 0.0f && accG[lane] <= 0.0f && accB[lane] <= 0.0f) continue;
                    const int px = pxBase + lane;
                    const float d = kBayer4[(py & 3) * 4 + (px & 3)];
                    const size_t i = row + size_t(px);
                    const dword pix = out[i];
                    int newR = int((pix >> 16) & 0xFF) + int(accR[lane] + 0.5f + d);
                    int newG = int((pix >>  8) & 0xFF) + int(accG[lane] + 0.5f + d);
                    int newB = int( pix        & 0xFF) + int(accB[lane] + 0.5f + d);
                    if (newR > 255) newR = 255;
                    if (newG > 255) newG = 255;
                    if (newB > 255) newB = 255;
                    out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                           |  dword(newB)        | 0xFF000000u;
                }
            }
        }
        return;
    }

    if (analyticHalo) {
        // Scalar fallback (analytic + scalar atan_approx).
        for (int py = y1; py < y2; ++py) {
            const float Y = (CntrEY - float(py)) * invFOVY;
            const size_t row = size_t(py) * size_t(XRes);
            for (int px = x1; px < x2; ++px) {
                const float X = (float(px) - CntrEX) * invFOVX;
                const float uV = X*X + Y*Y + 1.0f;

                const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
                float zMax = (zSurf > 0.0f) ? zSurf : zSky;
                if (fogZ > 0.0f && zMax > fogZ) zMax = fogZ;
                constexpr float zMin = 0.05f;
                if (zMax <= zMin) continue;

                float accR = 0.0f, accG = 0.0f, accB = 0.0f;
                for (int o = 0; o < omniCount; ++o) {
                    const int li = omniIdx[o];
                    const float Px = lights->posX[li], Py = lights->posY[li], Pz = lights->posZ[li];
                    const float r2 = lights->haloRange2[li];
                    const float rr = lights->haloRRange[li];
                    const float perOmniDensity = density * lights->haloDensityMul[li];
                    const float VP = X*Px + Y*Py + Pz;
                    const float PP = Px*Px + Py*Py + Pz*Pz;

                    // Sphere bounds (same as ray-march path).
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

                    // Quadratic d²(z) = (zV - P)·(zV - P) = uV·z² - 2·VP·z + PP.
                    // Inverse-square attenuation: 1/((rr·d)² + 0.05)
                    //   = 1/(rr²·d² + 0.05) = 1/(α·z² + β·z + γ)
                    // with α = rr²·uV, β = -2·rr²·VP, γ = rr²·PP + 0.05.
                    // Discriminant 4αγ − β² simplifies via 4·rr²·(rr²·uV·PP + 0.05·uV − rr²·VP²).
                    // Since the omni range sphere intersects the ray, the
                    // discriminant is positive (else sphereDisc<0 would
                    // have fired above).
                    const float rr2  = rr * rr;
                    const float alpha = rr2 * uV;
                    const float beta  = -2.0f * rr2 * VP;
                    const float gamma = rr2 * PP + 0.05f;
                    const float discQ = 4.0f * alpha * gamma - beta * beta;
                    if (discQ <= 0.0f) continue;
                    // fast_rsqrt computes 1/sqrt(discQ) directly via NEON
                    // frsqrte + 1 NR step (~5 cycles vs ~24 for the
                    // std::sqrt + std::div pair). Recover D from invD by
                    // mul-back, then both atans via the polynomial approx
                    // (~10 ops/call vs ~30 cycles per libm atan).
                    const float invD = fast_rsqrt(discQ);
                    const float D    = discQ * invD;
                    (void)D;  // kept for parity with the legacy comment; argHi/Lo only need invD
                    const float argHi = (2.0f * alpha * zHi + beta) * invD;
                    const float argLo = (2.0f * alpha * zLo + beta) * invD;
                    const float integral = 2.0f * invD * (atan_approx(argHi) - atan_approx(argLo));
                    if (integral <= 0.0f) continue;
                    // Tile fn density is already premultiplied by N_SAMPLES
                    // for the ray-march path's per-sample-sum semantics
                    // (acc ≈ N × mean_distAtten). For the analytic
                    // integral we get the integrated value directly, so
                    // scale by N_SAMPLES to keep the visual intensity
                    // comparable. perOmniDensity folds in HaloIntensity.
                    const float w = integral * perOmniDensity * float(N_SAMPLES);
                    accR += w * lights->colR[li];
                    accG += w * lights->colG[li];
                    accB += w * lights->colB[li];
                }
                if (accR <= 0.0f && accG <= 0.0f && accB <= 0.0f) continue;
                // Bayer-4x4 dither (same pattern as SIMD path above).
                static constexpr float kBayer4[16] = {
                    -0.46875f, +0.03125f, -0.34375f, +0.15625f,
                    +0.28125f, -0.21875f, +0.40625f, -0.09375f,
                    -0.28125f, +0.21875f, -0.40625f, +0.09375f,
                    +0.46875f, -0.03125f, +0.34375f, -0.15625f,
                };
                const float d = kBayer4[(py & 3) * 4 + (px & 3)];
                const size_t i = row + size_t(px);
                const dword pix = out[i];
                int newR = int((pix >> 16) & 0xFF) + int(accR + 0.5f + d);
                int newG = int((pix >>  8) & 0xFF) + int(accG + 0.5f + d);
                int newB = int( pix        & 0xFF) + int(accB + 0.5f + d);
                if (newR > 255) newR = 255;
                if (newG > 255) newG = 255;
                if (newB > 255) newB = 255;
                out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                       |  dword(newB)        | 0xFF000000u;
            }
        }
        return;
    }

    for (int py = y1; py < y2; ++py) {
        const float Y = (CntrEY - float(py)) * invFOVY;
        const size_t row = size_t(py) * size_t(XRes);
        if (vecPath) {
            // Pixel-major SIMD — see Render_VolumetricCones_Tile for
            // rationale. Halo is simpler: only sphere intersection
            // (no cone quadratic) and no shadow lookup.
            for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                const int pxEnd     = std::min(pxBase + 8, x2);
                const int laneCount = pxEnd - pxBase;

                alignas(32) float    Xarr[8] = {};
                alignas(32) float    uVarr[8] = {};
                alignas(32) uint32_t pxHashArr[8] = {};
                alignas(32) float    zMaxArr[8] = {};
                bool anyAlive = false;
                for (int lane = 0; lane < laneCount; ++lane) {
                    const int px = pxBase + lane;
                    const float X = (float(px) - CntrEX) * invFOVX;
                    Xarr[lane]  = X;
                    uVarr[lane] = X*X + Y*Y + 1.0f;
                    uint32_t h = uint32_t(px) * 0x9E3779B9u
                               + uint32_t(py) * 0x85EBCA6Bu
                               + 0xDEC0DE51u;
                    h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
                    pxHashArr[lane] = h;
                    const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
                    const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
                    float zM = (zSurf > 0.0f) ? zSurf : zSky;
                    if (fogZ > 0.0f && zM > fogZ) zM = fogZ;
                    constexpr float zMin = 0.05f;
                    if (zM > zMin) { zMaxArr[lane] = zM; anyAlive = true; }
                }
                if (!anyAlive) continue;

                alignas(32) float accB[8] = {}, accG[8] = {}, accR[8] = {};

                for (int o = 0; o < omniCount; ++o) {
                    const int li = omniIdx[o];
                    const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
                    const float r2 = lights->range2[li];
                    const float rr = lights->rRange[li];
                    const float PP = Px*Px + Py_l*Py_l + Pz*Pz;

                    // Per-lane scalar sphere-bounds solve.
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
                    const __m256 vInvFogZ_v = _mm256_set1_ps(invFogZ);
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

                        __m256 fogAtten = vOne_v;
                        if (invFogZ > 0.0f) {
                            fogAtten = _mm256_sub_ps(vOne_v, _mm256_mul_ps(vZ, vInvFogZ_v));
                            fogAtten = _mm256_max_ps(vZero_v, fogAtten);
                            fogAtten = _mm256_mul_ps(fogAtten, fogAtten);
                        }

                        __m256 contrib = _mm256_mul_ps(distAtten, fogAtten);
                        contrib = _mm256_and_ps(contrib, mask);
                        accV = _mm256_add_ps(accV, contrib);
                    }

                    alignas(32) float accArr[8];
                    _mm256_store_ps(accArr, accV);
                    const float colB = lights->colB[li];
                    const float colG = lights->colG[li];
                    const float colR = lights->colR[li];
                    for (int lane = 0; lane < 8; ++lane) {
                        if (accArr[lane] <= 0.0f) continue;
                        const float w = accArr[lane] * density;
                        accB[lane] += w * colB;
                        accG[lane] += w * colG;
                        accR[lane] += w * colR;
                    }
                }

                for (int lane = 0; lane < laneCount; ++lane) {
                    if (accR[lane] <= 0.0f && accG[lane] <= 0.0f && accB[lane] <= 0.0f) continue;
                    const int px = pxBase + lane;
                    const size_t i = row + size_t(px);
                    const dword pix = out[i];
                    int newR = int((pix >> 16) & 0xFF) + int(accR[lane]);
                    int newG = int((pix >>  8) & 0xFF) + int(accG[lane]);
                    int newB = int( pix        & 0xFF) + int(accB[lane]);
                    if (newR > 255) newR = 255;
                    if (newG > 255) newG = 255;
                    if (newB > 255) newB = 255;
                    out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                           |  dword(newB)        | 0xFF000000u;
                }
            }
        } else {
        for (int px = x1; px < x2; ++px) {
            const float X = (float(px) - CntrEX) * invFOVX;
            const float uV = X*X + Y*Y + 1.0f;

            uint32_t pxHash = uint32_t(px) * 0x9E3779B9u
                            + uint32_t(py) * 0x85EBCA6Bu
                            + 0xDEC0DE51u;  // different salt from cones
            pxHash ^= pxHash >> 13;
            pxHash *= 0xC2B2AE35u;
            pxHash ^= pxHash >> 16;

            const float zSurf = float(0xFF80 - int(zEnc[row + px])) * invZScale;
            const float zSky  = (fogZ > 0.0f) ? fogZ : 1e30f;
            float zMax = (zSurf > 0.0f) ? zSurf : zSky;
            if (fogZ > 0.0f && zMax > fogZ) zMax = fogZ;
            constexpr float zMin = 0.05f;
            if (zMax <= zMin) continue;

            float accR = 0.0f, accG = 0.0f, accB = 0.0f;
            for (int o = 0; o < omniCount; ++o) {
                const int li = omniIdx[o];
                const float Px = lights->posX[li], Py = lights->posY[li], Pz = lights->posZ[li];
                const float r2 = lights->range2[li];
                const float rr = lights->rRange[li];
                const float VP = X*Px + Y*Py + Pz;
                const float PP = Px*Px + Py*Py + Pz*Pz;

                // Ray-sphere intersection bounds the integration.
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
                float acc = 0.0f;
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
                    // Match the legacy fog pass's per-sample squared
                    // attenuation so halos fade consistently with
                    // surface fog in fogged scenes.
                    float fogAtten = 1.0f;
                    if (invFogZ > 0.0f) {
                        fogAtten = 1.0f - z * invFogZ;
                        if (fogAtten < 0.0f) fogAtten = 0.0f;
                        fogAtten *= fogAtten;
                    }
                    acc += distAtten * fogAtten;
                }
                if (acc <= 0.0f) continue;
                const float w = acc * density;
                accR += w * lights->colR[li];
                accG += w * lights->colG[li];
                accB += w * lights->colB[li];
            }
            if (accR <= 0.0f && accG <= 0.0f && accB <= 0.0f) continue;
            const size_t i = row + size_t(px);
            const dword pix = out[i];
            int newR = int((pix >> 16) & 0xFF) + int(accR);
            int newG = int((pix >>  8) & 0xFF) + int(accG);
            int newB = int( pix        & 0xFF) + int(accB);
            if (newR > 255) newR = 255;
            if (newG > 255) newG = 255;
            if (newB > 255) newB = 255;
            out[i] = (dword(newR) << 16) | (dword(newG) << 8)
                   |  dword(newB)        | 0xFF000000u;
        }
        }
    }
}

void Render_OmniHalos() {
    VolProfScope _vp(&g_volProf.ms_halos, &g_volProf.n_halos);
    if (!CurScene || !ZPage16 || !VPage) return;
    if (fds::FeatureFlags::omni_halo_strength() <= 0.0f) return;
    const float invFOVX = 1.0f / FOVX;
    const float invFOVY = 1.0f / FOVY;
    const float invZScale = 1.0f / float(g_zscale);
    const float density = fds::FeatureFlags::omni_halo_strength() * 0.001f;
    const float fogZ    = (CurScene->Flags & Scn_Fogged) ? CurScene->FZP : 0.0f;
    const float invFogZ = (fogZ > 0.0f) ? 1.0f / fogZ : 0.0f;

    extern DeferredLightingCtx g_deferredCtx;
    const ViewLightsSoA *const lights = g_deferredCtx.lights;
    if (!lights) return;
    const int numLights = g_deferredCtx.numLights;

    static int omniIdx[DEFERRED_MAX_VIEW_LIGHTS];
    int omniCount = 0;
    // Mirror-clone halos render only on the analytic+vec path, which
    // gates per pixel on gb.mirrorMask == clone id and starts the
    // integral at the wall depth (mirrorMaskZ) — the reflected glow
    // exists on the virtual segment BEHIND the mirror surface only.
    // The ray-march / scalar fallbacks have no such gate, so clones
    // stay excluded there (15 warm greets clones otherwise bloom a
    // flat additive wash over the reflection AND real geometry).
    const bool cloneHalos =
        fds::FeatureFlags::vol_halo_analytic() && fds::FeatureFlags::vol_vec()
        && g_gbuffer && !g_gbuffer->mirrorMask.empty()
        && g_gbuffer->mirrorMaskZ.size() >= g_gbuffer->mirrorMask.size();
    for (int i = 0; i < numLights; ++i) {
        if (lights->mirrorId[i] != 0 && !cloneHalos) continue;
        if (!lights->isSpot[i]) omniIdx[omniCount++] = i;
    }
    if (omniCount == 0) return;

    constexpr int numTilesX = 6;
    constexpr int numTilesY = 4;
    constexpr int numTiles  = numTilesX * numTilesY;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;

    // Per-tile omni cull — same sphere-projection math as cones, but
    // no z-cull (omni halo is volumetric, surface-z occlusion is per-
    // pixel via zSurf clamp inside the kernel).
    static int tileOmniIdx  [numTiles][DEFERRED_MAX_VIEW_LIGHTS];
    static int tileOmniCount[numTiles];
    for (int t = 0; t < numTiles; ++t) tileOmniCount[t] = 0;

    for (int o = 0; o < omniCount; ++o) {
        const int li = omniIdx[o];
        const float vx = lights->posX[li];
        const float vy = lights->posY[li];
        const float vz = lights->posZ[li];
        // Cull against the *halo* radius, not the surface IRange.
        // omni_halo_force_range / range_mult / per-omni HaloRange can all
        // make the halo extend well past the surface-lit sphere — using
        // range2 here was rejecting tiles where the halo should render,
        // and was also responsible for sharp tile-edge transitions when
        // adjacent tiles disagreed on whether the (small) surface sphere
        // crossed them.
        const float r  = lights->haloRange[li];
        if (vz + r < 0.0f) continue;
        int ti_lo, ti_hi, tj_lo, tj_hi;
        if (vz - r < 1.0f) {
            ti_lo = 0; ti_hi = numTilesX - 1;
            tj_lo = 0; tj_hi = numTilesY - 1;
        } else {
            const float invZ = 1.0f / vz;
            const float cx = CntrEX + vx * FOVX * invZ;
            const float cy = CntrEY - vy * FOVY * invZ;
            const float rx = r * FOVX * invZ;
            const float ry = r * FOVY * invZ;
            const int sx_min = std::max(0,        int(std::floor(cx - rx)));
            const int sx_max = std::min(XRes - 1, int(std::ceil (cx + rx)));
            const int sy_min = std::max(0,        int(std::floor(cy - ry)));
            const int sy_max = std::min(YRes - 1, int(std::ceil (cy + ry)));
            if (sx_min > sx_max || sy_min > sy_max) continue;
            ti_lo = sx_min / tileSizeX;
            ti_hi = std::min(numTilesX - 1, sx_max / tileSizeX);
            tj_lo = sy_min / tileSizeY;
            tj_hi = std::min(numTilesY - 1, sy_max / tileSizeY);
        }
        for (int j = tj_lo; j <= tj_hi; ++j) {
            for (int i = ti_lo; i <= ti_hi; ++i) {
                const int t = j * numTilesX + i;
                if (tileOmniCount[t] < DEFERRED_MAX_VIEW_LIGHTS) {
                    tileOmniIdx[t][tileOmniCount[t]++] = li;
                }
            }
        }
    }

    renderns::tileCounter = 0;
    for (int j = 0; j < numTilesY; ++j) {
        const int y1 = tileSizeY * j;
        const int y2 = std::min(y1 + tileSizeY, YRes);
        for (int i = 0; i < numTilesX; ++i) {
            const int x1 = tileSizeX * i;
            const int x2 = std::min(x1 + tileSizeX, XRes);
            const int tileIdx = j * numTilesX + i;
            const int *ts = tileOmniIdx[tileIdx];
            const int  tc = tileOmniCount[tileIdx];
            ThreadPool::instance().enqueue([x1,y1,x2,y2,lights,ts,tc,
                                            invFOVX,invFOVY,invZScale,
                                            fogZ,invFogZ,density]() {
                Render_OmniHalos_Tile(x1,y1,x2,y2, lights, ts, tc,
                                       invFOVX,invFOVY,invZScale,
                                       fogZ,invFogZ,density);
                // One permit per completed tile (see renderns::tileDone).
                renderns::tileDone.release();
            });
        }
    }
    for (int _i = 0; _i < numTiles; ++_i) {
        renderns::tileDone.acquire();
    }
}

// Skybox-from-G-buffer pass. Paints sky pixels (zEnc == 0) by
// reconstructing the world-space view direction per pixel and
// sampling the cubemap. Pre-req: the forward RenderSkyCube must be
// suppressed when this pass runs (see SkyCube.cpp early-return),
// otherwise sky pixels would have zEnc != 0 from the forward draw.
//
// Cube face indexing matches SkyCube.cpp's normal convention:
//   0 = +Z (SBBK / "back")
//   1 = +X (SBRT / "right")
//   2 = -Z (SBFT / "front")
//   3 = -X (SBLF / "left")
//   4 = -Y (SBDN / "down")    [Y is up; -Y face is the floor]
//   5 = +Y (SBUP / "up")
// Per-face UV math derived from vertex/UV layout in InitSkyCube — see
// the (face, vertex, UV) table next to that function if extending.
// Deferred sky elapsed-ns accumulator. Always-on (cheap atomic add),
// distinct from the vol_prof flag-gated VolProfScope. Per-scene
// drivers consume this in their PROF_SKY section so the on-screen
// overlay reflects the deferred path the same way it used to
// reflect RenderSkyCube.
namespace {
    std::atomic<std::int64_t> g_deferredSkyNs{0};
}
std::int64_t DeferredSkybox_TakeFrameNs() {
    return g_deferredSkyNs.exchange(0, std::memory_order_relaxed);
}

void Render_DeferredSkybox() {
    VolProfScope _vp(&g_volProf.ms_skybox, &g_volProf.n_skybox);
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    struct AccumOnExit {
        clk::time_point t0;
        ~AccumOnExit() {
            const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                clk::now() - t0).count();
            g_deferredSkyNs.fetch_add(dt, std::memory_order_relaxed);
        }
    } _accum{t0};
    if (!CurScene || !VPage || !ZPage16) return;
    extern const dword *SkyCube_GetFaceMip(int, int, int&, int&);
    extern int SkyCube_NumMips();
    const int numMips = SkyCube_NumMips();
    if (numMips <= 0) return;

    // Cache every mip of every face — bounded by 2048² → 12 levels.
    // Per-pixel sampling does 6×kMaxMips=72 entries lookup-free.
    constexpr int kMaxMips = 12;
    const int mipCount = std::min(numMips, kMaxMips);
    const dword *facePix[6][kMaxMips] = {};
    int faceW [6][kMaxMips] = {};
    int faceH [6][kMaxMips] = {};
    for (int f = 0; f < 6; ++f) {
        for (int m = 0; m < mipCount; ++m) {
            facePix[f][m] = SkyCube_GetFaceMip(f, m, faceW[f][m], faceH[f][m]);
            if (!facePix[f][m]) return;
        }
    }

    // View basis. View->Mat is world→view (orthonormal), so view→world
    // for a *direction* is the transpose. Cache the 9 floats so the
    // per-pixel multiply is just nine flops + adds.
    const float m00 = View->Mat[0][0], m01 = View->Mat[0][1], m02 = View->Mat[0][2];
    const float m10 = View->Mat[1][0], m11 = View->Mat[1][1], m12 = View->Mat[1][2];
    const float m20 = View->Mat[2][0], m21 = View->Mat[2][1], m22 = View->Mat[2][2];
    const float invFOVX_l = 1.0f / FOVX;
    const float invFOVY_l = 1.0f / FOVY;
    const float cntrX_l = CntrEX;
    const float cntrY_l = CntrEY;
    // Per-pixel angular size in view space. Used to estimate the
    // cubemap UV change between adjacent pixels for mip selection.
    const float pixelAngle = (invFOVX_l > invFOVY_l) ? invFOVX_l : invFOVY_l;

    constexpr int numTilesX = 6;
    constexpr int numTilesY = 4;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;
    renderns::tileCounter = 0;
    for (int j = 0; j < numTilesY; ++j) {
        const int y1 = tileSizeY * j;
        const int y2 = std::min(y1 + tileSizeY, YRes);
        for (int i = 0; i < numTilesX; ++i) {
            const int x1 = tileSizeX * i;
            const int x2 = std::min(x1 + tileSizeX, XRes);
            ThreadPool::instance().enqueue([=]() {
                dword *out = reinterpret_cast<dword *>(VPage);
                // Hoisted broadcasts for the 8-wide view→world FMAs.
                const __m256 vm00 = _mm256_set1_ps(m00);
                const __m256 vm10 = _mm256_set1_ps(m10);
                const __m256 vm20 = _mm256_set1_ps(m20);
                const __m256 vm01 = _mm256_set1_ps(m01);
                const __m256 vm11 = _mm256_set1_ps(m11);
                const __m256 vm21 = _mm256_set1_ps(m21);
                const __m256 vm02 = _mm256_set1_ps(m02);
                const __m256 vm12 = _mm256_set1_ps(m12);
                const __m256 vm22 = _mm256_set1_ps(m22);
                const __m256 vCntrX = _mm256_set1_ps(cntrX_l);
                const __m256 vInvFOVX = _mm256_set1_ps(invFOVX_l);
                // 0..7 lane offset for vx generation.
                const __m256 vLaneOfs = _mm256_setr_ps(0,1,2,3,4,5,6,7);
                for (int py = y1; py < y2; ++py) {
                    const float vy = -((float(py) - cntrY_l) * invFOVY_l);
                    const __m256 vvy = _mm256_set1_ps(vy);
                    // m1*vy + m2*1 = fmadd(m1, vy, m2). Hoisted per row.
                    const __m256 dxY = _mm256_fmadd_ps(vm10, vvy, vm20);
                    const __m256 dyY = _mm256_fmadd_ps(vm11, vvy, vm21);
                    const __m256 dzY = _mm256_fmadd_ps(vm12, vvy, vm22);
                    const size_t row = size_t(py) * size_t(XRes);
                    for (int pxBase = x1; pxBase < x2; pxBase += 8) {
                        const int pxEnd = std::min(pxBase + 8, x2);
                        const int laneCount = pxEnd - pxBase;
                        // Early-skip batches with no sky pixels — most
                        // tiles in opaque-heavy scenes look like this.
                        // Avoid touching the per-pixel math at all.
                        bool anySky = false;
                        alignas(32) uint32_t skyMask[8] = {};
                        for (int l = 0; l < laneCount; ++l) {
                            if (ZPage16[row + pxBase + l] == 0) {
                                skyMask[l] = 0xFFFFFFFFu; anySky = true;
                            }
                        }
                        if (!anySky) continue;
                        // vx = (pxBase + lane - cntrX) * invFOVX
                        const __m256 vxBase = _mm256_set1_ps(float(pxBase));
                        const __m256 vPx = _mm256_add_ps(vxBase, vLaneOfs);
                        const __m256 vvx = _mm256_mul_ps(
                            _mm256_sub_ps(vPx, vCntrX), vInvFOVX);
                        // D = M^T · v  (vz = 1 folded into dxY/dyY/dzY).
                        const __m256 dxv = _mm256_fmadd_ps(vm00, vvx, dxY);
                        const __m256 dyv = _mm256_fmadd_ps(vm01, vvx, dyY);
                        const __m256 dzv = _mm256_fmadd_ps(vm02, vvx, dzY);
                        alignas(32) float dxArr[8], dyArr[8], dzArr[8];
                        _mm256_store_ps(dxArr, dxv);
                        _mm256_store_ps(dyArr, dyv);
                        _mm256_store_ps(dzArr, dzv);
                        // Per-lane scalar tail: face select + sample.
                        // (Different lanes pick different faces and
                        // textures — vectorizing the sampler would need
                        // gather/scatter; not worth the complexity.)
                        for (int l = 0; l < laneCount; ++l) {
                            if (skyMask[l] == 0) continue;
                            const int px = pxBase + l;
                            const float dx = dxArr[l];
                            const float dy = dyArr[l];
                            const float dz = dzArr[l];
                        const float ax = dx < 0 ? -dx : dx;
                        const float ay = dy < 0 ? -dy : dy;
                        const float az = dz < 0 ? -dz : dz;
                        int face;
                        float u, v, maxAbs;
                        if (az >= ax && az >= ay) {
                            maxAbs = az;
                            const float s = 0.5f / az;
                            if (dz > 0) { face = 0; u = 0.5f - dx*s; v = 0.5f - dy*s; }
                            else        { face = 2; u = 0.5f + dx*s; v = 0.5f - dy*s; }
                        } else if (ax >= ay) {
                            maxAbs = ax;
                            const float s = 0.5f / ax;
                            if (dx > 0) { face = 1; u = 0.5f + dz*s; v = 0.5f - dy*s; }
                            else        { face = 3; u = 0.5f - dz*s; v = 0.5f - dy*s; }
                        } else {
                            maxAbs = ay;
                            const float s = 0.5f / ay;
                            if (dy > 0) { face = 5; u = 0.5f + dx*s; v = 0.5f - dz*s; }
                            else        { face = 4; u = 0.5f + dx*s; v = 0.5f + dz*s; }
                        }
                        // Mip selection. texelStep ≈ pixelAngle·0.5/|dom|·faceW.
                        // Integer log2 via the IEEE-754 exponent — no
                        // call into libm. `(bits>>23)&0xFF` is the
                        // biased exponent; subtract 127 → unbiased.
                        // (negative result = texelStep<1 → mip 0.)
                        const float invMaxAbs = 1.0f / maxAbs;
                        const float texelStep = pixelAngle * invMaxAbs * 0.5f
                                              * float(faceW[face][0]);
                        int mip;
                        if (texelStep < 1.0f) {
                            mip = 0;
                        } else {
                            union { float f; uint32_t i; } u{texelStep};
                            mip = int((u.i >> 23) & 0xFFu) - 127;
                            if (mip < 0) mip = 0;
                            if (mip >= mipCount) mip = mipCount - 1;
                        }
                        const int w = faceW[face][mip];
                        const int h = faceH[face][mip];
                        const dword *tex = facePix[face][mip];

                        // Bilinear sample. fu/fv in [0,1); clamp at edges
                        // so we don't sample across face seams (which
                        // would show up as cube-edge bands).
                        float fx = u * float(w) - 0.5f;
                        float fy = v * float(h) - 0.5f;
                        if (fx < 0) fx = 0; else if (fx > float(w - 1)) fx = float(w - 1);
                        if (fy < 0) fy = 0; else if (fy > float(h - 1)) fy = float(h - 1);
                        const int tx0 = int(fx);
                        const int ty0 = int(fy);
                        const int tx1 = (tx0 + 1 < w) ? tx0 + 1 : tx0;
                        const int ty1 = (ty0 + 1 < h) ? ty0 + 1 : ty0;
                        const float fxr = fx - float(tx0);
                        const float fyr = fy - float(ty0);
                        const dword p00 = tex[size_t(ty0) * size_t(w) + size_t(tx0)];
                        const dword p10 = tex[size_t(ty0) * size_t(w) + size_t(tx1)];
                        const dword p01 = tex[size_t(ty1) * size_t(w) + size_t(tx0)];
                        const dword p11 = tex[size_t(ty1) * size_t(w) + size_t(tx1)];
                        const float w00 = (1.f - fxr) * (1.f - fyr);
                        const float w10 = fxr        * (1.f - fyr);
                        const float w01 = (1.f - fxr) * fyr;
                        const float w11 = fxr        * fyr;
                        // Per-channel blend on ARGB8888.
                        const float bF = float((p00      ) & 0xFFu) * w00
                                       + float((p10      ) & 0xFFu) * w10
                                       + float((p01      ) & 0xFFu) * w01
                                       + float((p11      ) & 0xFFu) * w11;
                        const float gF = float((p00 >>  8) & 0xFFu) * w00
                                       + float((p10 >>  8) & 0xFFu) * w10
                                       + float((p01 >>  8) & 0xFFu) * w01
                                       + float((p11 >>  8) & 0xFFu) * w11;
                        const float rF = float((p00 >> 16) & 0xFFu) * w00
                                       + float((p10 >> 16) & 0xFFu) * w10
                                       + float((p01 >> 16) & 0xFFu) * w01
                                       + float((p11 >> 16) & 0xFFu) * w11;
                        const dword B = dword(bF) & 0xFFu;
                        const dword G = dword(gF) & 0xFFu;
                        const dword R = dword(rF) & 0xFFu;
                        out[row + px] = 0xFF000000u | (R << 16) | (G << 8) | B;
                        }  // per-lane for-l
                    }      // per-batch for-pxBase
                }          // per-row for-py
                // One permit per completed tile (see renderns::tileDone).
                renderns::tileDone.release();
            });
        }
    }
    for (int _i = 0, n = numTilesX * numTilesY; _i < n; ++_i) {
        renderns::tileDone.acquire();
    }
}

void Render_DeferredFogPass() {
	if (!CurScene || !(CurScene->Flags & Scn_Fogged)) return;
	if (!g_gbuffer || !ZPage16 || !VPage) return;
	const float invFZP = 1.0f / CurScene->FZP;
	constexpr auto numTilesX = 6;
	constexpr auto numTilesY = 4;
	const auto tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const auto tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;
	renderns::tileCounter = 0;
	for (int j = 0; j < numTilesY; ++j) {
		const int y1 = tileSizeY * j;
		const int y2 = std::min(y1 + tileSizeY, YRes);
		for (int i = 0; i < numTilesX; ++i) {
			const int x1 = tileSizeX * i;
			const int x2 = std::min(x1 + tileSizeX, XRes);
			ThreadPool::instance().enqueue([x1, y1, x2, y2, invFZP]() {
				Render_DeferredFogPass_Tile(x1, y1, x2, y2, invFZP);
				// One permit per completed tile (see renderns::tileDone).
				renderns::tileDone.release();
			});
		}
	}
	for (int _i = 0, n = numTilesX * numTilesY; _i < n; ++_i) {
		renderns::tileDone.acquire();
	}
}

