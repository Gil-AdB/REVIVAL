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
#include "Base/Compiler.h"   // FDS_POPCOUNT
#include "Base/Scene.h"
#include "Base/Omni.h"
#include "Base/Camera.h"
#include "FILLERS/Mekalele.h"
#include "FILLERS/ShadowMap.h"
#include "RENDER/DeferredCommon.h"
#include "RENDER/Hdr.h"  // HDR overlay reorg — cones/halos composite into g_hdrBuf
#include "TailProf.h"     // phase-1 barrier-tail instrumentation (FDS_TAIL_PROF)
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

// Cone-cost diagnostic scaffold: counts the (8px-batch x spot) iteration
// volume of the vec path and how much of it dies in the scalar prologue.
// COMPILE-TIME gated so the shipping build pays literally nothing — this
// sits in the hottest loop in the codebase (37.5% of all running CPU on
// city t=1961), where even a well-predicted branch is not free.
// Re-derive the census with:  cmake -B build -DCMAKE_CXX_FLAGS=-DFDS_CONE_DIAG=1
// then run with FDS_CONE_ATTR=1.  See docs/HW_PROFILING.md sections 9-10 for
// the numbers this produced (the solve is 63.6% of the pass) and for the
// xctrace recipe that found the pass in the first place.
#ifndef FDS_CONE_DIAG
#define FDS_CONE_DIAG 0
#endif
// Compiled-out record of a measured-and-rejected early-out in the 8-wide
// cone solve — see the comment at its site for the numbers.
#ifndef FDS_CONE_SOLVE_EARLYOUT
#define FDS_CONE_SOLVE_EARLYOUT 0
#endif
// Compiled-out record of the OTHER measured-and-rejected variant of the
// 8-wide cone solve: its three reciprocals and two square roots done with
// the NEON estimate instructions instead of true IEEE div/sqrt. Numbers and
// verdict at the RCP/SQRTV definitions below.
#ifndef FDS_CONE_SOLVE_APPROX
#define FDS_CONE_SOLVE_APPROX 0
#endif
// Compiled-out record of round 7's SECOND probe: W² and D·W inside the
// 8-segment hybrid's coneAtten evaluated in CLOSED FORM from uV/VP/DV/PP
// instead of rebuilding W = z·V − P per segment. Numbers and verdict at the
// coneAttenAt lambda. Short version: it removes real ops but they are not on
// the pass's critical path in enough pairs to pay, and it is a
// re-association, so it stays off.
#ifndef FDS_CONE_SEG_CLOSEDFORM
#define FDS_CONE_SEG_CLOSEDFORM 0
#endif
// Ablation ladder for the SECOND round of the cone-cost campaign (the first
// round's ladder was ad-hoc and not committed; this one is, because the split
// has to be re-derived every time the pass changes shape). COMPILE-TIME only:
// build the TU with -DFDS_CONE_ABLATE=n and the per-spot loop `continue`s at
// staged depth n, so the Ginstr/f difference between two builds prices exactly
// one stage. n=0 (shipping) emits literally nothing.
//
//   1  cut at the top of the per-spot loop      -> per-batch floor
//   2  keep the per-spot scalar prologue
//   3  keep + the 8-wide cone-interval solve
//   4  keep + the scalar per-lane dz/fade loop
//   5  keep + the broadcasts and the integration body (cut before accumulate)
//   0  full pass
//
// Each cut sinks the values it retains into a per-TILE vector accumulator
// (one volatile store per tile call, so no false sharing and no perturbation
// of the composite's `accB<=0` skip) — otherwise the retained work is dead and
// the compiler deletes exactly what we are trying to price. Sink cost is 2-7
// instructions per (batch x spot); quantified where it matters.
#ifndef FDS_CONE_ABLATE
#define FDS_CONE_ABLATE 0
#endif
#if FDS_CONE_ABLATE
static volatile float g_ablSink = 0.0f;
// NB: no do/while(0) wrapper — the `continue` has to reach the per-spot
// `for`, and a do/while would swallow it.
#define CONE_ABL_CUT(stage, expr) \
    if constexpr ((FDS_CONE_ABLATE) == (stage)) { \
        ablSinkV = _mm256_add_ps(ablSinkV, (expr)); continue; }
#define CONE_ABL_CUT_BARE(stage) \
    if constexpr ((FDS_CONE_ABLATE) == (stage)) { continue; }
#else
#define CONE_ABL_CUT(stage, expr)  ((void)0)
#define CONE_ABL_CUT_BARE(stage)   ((void)0)
#endif

// ─── Compile-time ARM FOLDING — the companion instrument to FDS_CONE_ABLATE ──
// The ablation ladder prices a STAGE. This prices an ARM. A runtime-flagged
// function carries every arm in one register allocation, so its disassembly
// and its register pressure are the union of paths no single frame takes;
// round 2 needed a build with the flags folded to constants to see the
// shipping path alone, built one ad hoc, and could only describe it in prose.
// Committed because round 3's whole question turned out to be "where does the
// register allocator spend its slots", which a two-arm histogram cannot answer.
//
//   0  runtime flags (shipping — folds to the plain flag reads, byte-null)
//   1  vol_cone_solve_vec and vol_cone_lane_vec folded to compile-time TRUE,
//      so the scalar fallbacks dead-code away entirely
#ifndef FDS_CONE_FORCE
#define FDS_CONE_FORCE 0
#endif

// ─── FDS_CONE_HOTONLY — prices the arms a scene never executes ──────────────
// DIAGNOSTIC ONLY, and it is not a correct renderer: it deletes the segmented
// hybrid, the ray-march fallback and the midpoint shadow tap outright, leaving
// only the branch city's wide headlights actually take. City is byte-identical
// under it (it never reaches the deleted arms), which is what makes it a clean
// measurement of how much of the hot path's cost is INTERFERENCE from code that
// never runs. Answer, city t=1961: 0.248 Ginstr/f, 10.5% of the pass — and
// 0.000 Gcyc. See docs/HW_PROFILING.md section 11.
#ifndef FDS_CONE_HOTONLY
#define FDS_CONE_HOTONLY 0
#endif

// ─── FDS_CONE_W4 — round 4's native-width solve: BUILT, MEASURED, REJECTED ──
// Spells the 8-wide cone-interval solve as TWO 4-wide passes, so that a live
// value costs ONE of the 32 NEON registers instead of two. It is bit-exact
// (city 3cbe42b1 3/3) and it does relieve pressure — and it buys NOTHING,
// which is the result: at =1 (unrolled, what the compiler does to a trip count
// of 2) instructions fall 3.7% and cycles move +0.7%; at =2 (half loop kept
// ROLLED, which is the only spelling that actually halves the live set: stack
// `str q` 89/79 -> 72/70) cycles go +7.5% and wall +8.5%. __m256 on arm64 is
// not two wasted registers, it is two INDEPENDENT NEON chains — free 2x
// unroll-and-jam — and taking them apart costs more than the spills it saves.
// Kept compiled out rather than behind a FeatureFlag because round 3 priced a
// live second arm in this kernel at +2.0 to +3.5% instructions, and a measured
// -0.1% is not worth that. docs/HW_PROFILING.md section 12.
//   0 = not compiled  1 = W4, half loop unrolled  2 = W4, half loop rolled
#ifndef FDS_CONE_W4
#define FDS_CONE_W4 0
#endif

// ─── FDS_CONE_NEONMINMAX — every min/max in the cone kernel at ONE op ───────
// See fmin_x8/fmax_x8 for why the shipping binary emits zero fmin.4s/fmax.4s
// today. 1 = use the NEON instruction; 0 = the byte-exact cmp+blend the port
// was built with. NOT a FeatureFlag: round 3 priced a live second arm in this
// kernel at +2.0 to +3.5% instructions, and 19 sites cannot be switched at
// runtime without putting a branch on each of them.
// DEFAULT 1 — it ships. city -4.5% cycles / -3.8% wall, greets -3.2% / -3.5%,
// -3.2 to -4.7% cycles at every pose of the city t-sweep, and it turned out to
// be BIT-EXACT in practice: city 3cbe42b1, greets 778fa6ac, fountain 8db68ccb
// all 3/3, render_gate ALL FOUR rows PASS including conetest. Build with
// -DFDS_CONE_NEONMINMAX=0 to get the cmp+blend spelling back.
#ifndef FDS_CONE_NEONMINMAX
#define FDS_CONE_NEONMINMAX 1
#endif

// ─── FDS_CONE_QUADEARLYOUT — the early-out at the RIGHT cut point ───────────
// Round 1 built a range-sphere early-out (FDS_CONE_SOLVE_EARLYOUT, still
// compiled out below) and it cost +2.0% instructions on city, because a pair
// is only skipped when all EIGHT lanes miss the range SPHERE and city loses
// only 7.7% of its pairs there. Chase loses ZERO percent there (DIAG
// sphdead=0.0% at every pose): its spots are long-range beams whose range
// sphere covers the whole visible depth, so every dead lane dies on the CONE
// quadratic instead — 30 lines further down. This cut sits after the
// a-sign/discriminant mask, which is the first point at which the pair's fate
// is decided, and skips the range-sphere clamp (a sqrt + a divide), the
// apex-plane cut (a second divide) and the three stores.
// BYTE-NULL BY CONSTRUCTION: zLoArr/zHiArr/aliveLane are zero-initialised per
// pair and `spotAlive` stays false, which is exactly the state the fall-through
// would leave them in before `if (!spotAlive) continue;`.
#ifndef FDS_CONE_QUADEARLYOUT
#define FDS_CONE_QUADEARLYOUT 1
#endif

static constexpr bool g_coneDiag = (FDS_CONE_DIAG != 0);
static std::atomic<long long> g_dPairs{0};   // batch x spot pairs entering the scalar solve
static std::atomic<long long> g_dSegPair{0}; // ...of which take the 8-segment hybrid
static std::atomic<long long> g_dDead{0};    // ...of which had ZERO alive lanes (wasted prologue)
static std::atomic<long long> g_dLanes{0};   // alive (lane x spot) pairs reaching integration
static std::atomic<long long> g_dSphDead{0}; // ...pairs whose EIGHT lanes all miss the range sphere
static std::atomic<long long> g_dQuadDead{0};// ...pairs whose EIGHT lanes are dead right after the
                                             //    a-sign/discriminant mask, i.e. BEFORE the sphere
                                             //    clamp, the apex-plane cut and the three stores.
                                             //    THIS is the cut point round 1's sphere early-out
                                             //    (FDS_CONE_SOLVE_EARLYOUT) missed.


// The ONLY fused multiply-add the 8-wide cone solve is allowed to use.
// simde lowers _mm256_fmadd_ps to vfmaq_f32 on arm64 (a true FMA), but
// lowers _mm256_fmsub_ps / _mm256_fnmadd_ps to sub(mul(a,b),c) — which
// under the tree-wide -ffp-contract=fast hands the "which product gets
// rounded" decision back to the compiler, and that decision is exactly
// what the bit-exact port has to pin down. Every `a*b - c` in the solve
// is therefore written fma_x8(a, b, NEG(c)).
static inline __m256 fma_x8(const __m256 &a, const __m256 &b, const __m256 &c) {
    return _mm256_fmadd_ps(a, b, c);
}
// Same barrier at the machine's NATIVE width (--vol_cone_solve_w4). arm64 has
// no 256-bit unit: simde lowers every __m256 op to two 128-bit NEON ops, so an
// __m256 value costs TWO of the 32 v-registers. Spelling the hot chain 4 wide
// issues the identical NEON ops and halves the register cost of every live
// value; fma_x4 pins the contraction the same way fma_x8 does.
static inline __m128 fma_x4(const __m128 &a, const __m128 &b, const __m128 &c) {
    return _mm_fmadd_ps(a, b, c);
}

// ─── min/max at ONE instruction instead of two ──────────────────────────────
// The cone kernel contains ~19 min/max sites and the shipping binary emits
// ZERO `fmin.4s` / `fmax.4s` for them. Two separate reasons, same cost:
//   * the solve and the dz/fade loop spell std::min/std::max by hand as
//     cmp+blend, because 7e34645 found NEON FMIN resolves NaN and -0 the
//     opposite way from the scalar FCSEL and the port had to be bit-exact;
//   * every `_mm256_max_ps` already in the kernel ALSO lowers to cmp+blend,
//     because SIMDE_FAST_NANS is not defined in this build and simde's
//     NaN-correct fallback is `m = a<b; (a&m)|(b&~m)`, which LLVM folds to
//     fcmgt+bsl. So the intrinsic that looks like one op is two.
// Round 4 measured the pass at ~81% of this core's 4-wide NEON ALU issue
// ceiling (the solve retires ~205 vector-ALU ops in ~63 cycles per
// (batch x spot)), which makes VECTOR-ALU OP COUNT the metric that moves
// cycles -- so a systematic 2-ops-to-1 is worth having. vmaxq_f32 is FMAX,
// not FMAXNM: NaN propagates (matching the simde fallback, which also
// returns the NaN operand), and only the +0/-0 tie-break differs.
#if defined(__ARM_NEON) || defined(__aarch64__)
static inline __m256 fmax_x8(const __m256 &a, const __m256 &b) {
    simde__m256_private ap = simde__m256_to_private(a),
                        bp = simde__m256_to_private(b), rp;
    rp.m128_private[0].neon_f32 = vmaxq_f32(ap.m128_private[0].neon_f32,
                                            bp.m128_private[0].neon_f32);
    rp.m128_private[1].neon_f32 = vmaxq_f32(ap.m128_private[1].neon_f32,
                                            bp.m128_private[1].neon_f32);
    return simde__m256_from_private(rp);
}
static inline __m256 fmin_x8(const __m256 &a, const __m256 &b) {
    simde__m256_private ap = simde__m256_to_private(a),
                        bp = simde__m256_to_private(b), rp;
    rp.m128_private[0].neon_f32 = vminq_f32(ap.m128_private[0].neon_f32,
                                            bp.m128_private[0].neon_f32);
    rp.m128_private[1].neon_f32 = vminq_f32(ap.m128_private[1].neon_f32,
                                            bp.m128_private[1].neon_f32);
    return simde__m256_from_private(rp);
}
#else
static inline __m256 fmax_x8(const __m256 &a, const __m256 &b) { return _mm256_max_ps(a, b); }
static inline __m256 fmin_x8(const __m256 &a, const __m256 &b) { return _mm256_min_ps(a, b); }
#endif

// OR of the mirror-footprint presence bits (ctx.tileMirrorPresence,
// LIGHTING-tile geometry: 8-rounded X over the 12x8 grid) across every
// lighting tile overlapping the given pixel rect. Used by the cone and
// halo tile binning to skip (tile × cloneLight) pairs whose footprint
// bit is clear — the per-pixel gate (mmask[pi] == mirrorId) rejects
// every pixel there anyway, so the cull is exactly conservative. The
// volumetric passes' own tile grids (12x8 fine cones / 6x4 coarse
// cones and halos) don't share the lighting grid's rounding, hence
// rect-based instead of an index scale.
static inline uint32_t mirrorPresenceForRect(const uint32_t *grid,
                                             int x1, int y1, int x2, int y2,
                                             int xres, int yres)
{
    const int rawTileX = (xres + DEFERRED_NUM_TILES_X - 1) / DEFERRED_NUM_TILES_X;
    const int tsX = (rawTileX + 7) & ~7;
    const int tsY = (yres + DEFERRED_NUM_TILES_Y - 1) / DEFERRED_NUM_TILES_Y;
    const int iLo = std::max(0, x1 / tsX);
    const int iHi = std::min(DEFERRED_NUM_TILES_X - 1, (x2 - 1) / tsX);
    const int jLo = std::max(0, y1 / tsY);
    const int jHi = std::min(DEFERRED_NUM_TILES_Y - 1, (y2 - 1) / tsY);
    uint32_t bits = 0;
    for (int j = jLo; j <= jHi; ++j)
        for (int i = iLo; i <= iHi; ++i)
            bits |= grid[j * DEFERRED_NUM_TILES_X + i];
    return bits;
}

// HDR overlay reorg: additively composite a float scatter contribution (the
// cone/halo in-scatter accumulated for this pixel) at pixel index i. In HDR
// mode (g_hdrActive) it accumulates UNCAPPED into the float radiance buffer so
// god-ray shafts + omni halos bloom and roll off at the tonemap instead of
// clipping at 255; otherwise it's the legacy 8-bit add-and-saturate onto VPage.
// Channel order B,G,R matches both buffers. The LDR branch is byte-identical to
// the inline code it replaces (render gate covers it via conetest/halotest).
// --hdr-cone-softknee: soft-knee the cone/halo in-scatter (max-channel scaled,
// hue-preserving) — same gain curve g=(2x-1)/x² as the fog in-scatter knee.
// Rolls hot shaft cores off toward `glowMax` so they bloom and tonemap through
// ACES instead of running hot, replacing the flat hdr_glow_scale fudge.
static inline void ConeGlowSoftKnee(float &r, float &g, float &b, float glowMax) {
    if (glowMax <= 0.0f) return;
    const float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const float k = glowMax * 0.5f;
    if (m > k) {
        const float e = m - k;
        const float s = (k + e / (1.0f + e / k)) / m;
        r *= s; g *= s; b *= s;
    }
}

static inline void VolCompositeAdd(dword* out, size_t i, float aB, float aG, float aR) {
    if (fds::g_hdrActive) {
        // Physical roll-off of the cone/halo glow before it enters the linear
        // HDR buffer (cached FeatureFlags reads — the sanctioned hot-loop toggle).
        if (fds::FeatureFlags::hdr_cone_softknee()) {
            float gm = fds::FeatureFlags::cone_glow_max();
            if (gm <= 0.0f) gm = 200.0f;
            ConeGlowSoftKnee(aR, aG, aB, gm);
        }
        fds::hdrf* h = fds::g_hdrBuf.data() + i * 4;
        h[0] = fds::HdrClamp(float(h[0]) + aB); h[1] = fds::HdrClamp(float(h[1]) + aG); h[2] = fds::HdrClamp(float(h[2]) + aR);
        return;
    }
    const dword pix = out[i];
    int newR = int((pix >> 16) & 0xFF) + int(aR);
    int newG = int((pix >>  8) & 0xFF) + int(aG);
    int newB = int( pix        & 0xFF) + int(aB);
    if (newR > 255) newR = 255;
    if (newG > 255) newG = 255;
    if (newB > 255) newB = 255;
    out[i] = (dword(newR) << 16) | (dword(newG) << 8) | dword(newB) | 0xFF000000u;
}

// ─── Cone turbulence (cone_turbulence / cone_swirl flags) ───────────────
//
// Animated internal structure for the volumetric beams: per-sample (ray-
// march) / per-segment (analytic hybrid) density modulation by a cheap
// self-contained 3D value noise, evaluated in WORLD space so a moving or
// rotating beam sweeps through a static drifting medium (the "flashlight
// through smoke" read; cone-space sampling would glue the pattern to the
// beam, which looks like a gobo, not smoke). Optional swirl pre-rotates
// each sample around the beam axis by an angle proportional to the axial
// distance + scene time, which shears the world field into helical
// streaks that ride the beam.
//
// Determinism: every term derives from g_FrameTime (scene clock, pinned
// in snapshots) and pure position hashes — no rand()/wall time. All
// per-frame constants are computed once per Render_VolumetricCones call
// (orchestrator side) and passed down by value; tiles never write shared
// state.
struct ConeTurb {
	bool  on = false;
	float amp = 0.0f;         // modulation amplitude (flag × 2.2 contrast)
	float invCell = 1.0f;     // 1 / cone_turb_scale (world units → cells)
	float driftX = 0.0f, driftY = 0.0f, driftZ = 0.0f;  // cell offset (×t)
	float swirl = 0.0f;       // cone_swirl (0 = no axis rotation)
	float twistK = 0.0f;      // rad per world unit of axial distance
	float phase = 0.0f;       // rad, time term of the swirl angle
	int   octaves = 1;
	// view→world copied from the ctx (world = M·view + cam).
	float M[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
	float camX = 0.0f, camY = 0.0f, camZ = 0.0f;
};

// Integer-lattice hash → [0,1). Same avalanching recipe as the pixel
// jitter hash above; pure function of the lattice coords.
static inline float ConeTurbHash3(int x, int y, int z) {
	uint32_t h = uint32_t(x) * 0x8DA6B343u
	           + uint32_t(y) * 0xD8163841u
	           + uint32_t(z) * 0xCB1AB31Fu;
	h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
	return float(h & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

// Trilinear value noise, smoothstep-faded, → [0,1).
static inline float ConeTurbNoise3(float x, float y, float z) {
	const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
	const int ix = int(fx), iy = int(fy), iz = int(fz);
	float tx = x - fx, ty = y - fy, tz = z - fz;
	tx = tx * tx * (3.0f - 2.0f * tx);
	ty = ty * ty * (3.0f - 2.0f * ty);
	tz = tz * tz * (3.0f - 2.0f * tz);
	const float c000 = ConeTurbHash3(ix,     iy,     iz    );
	const float c100 = ConeTurbHash3(ix + 1, iy,     iz    );
	const float c010 = ConeTurbHash3(ix,     iy + 1, iz    );
	const float c110 = ConeTurbHash3(ix + 1, iy + 1, iz    );
	const float c001 = ConeTurbHash3(ix,     iy,     iz + 1);
	const float c101 = ConeTurbHash3(ix + 1, iy,     iz + 1);
	const float c011 = ConeTurbHash3(ix,     iy + 1, iz + 1);
	const float c111 = ConeTurbHash3(ix + 1, iy + 1, iz + 1);
	const float x00 = c000 + (c100 - c000) * tx;
	const float x10 = c010 + (c110 - c010) * tx;
	const float x01 = c001 + (c101 - c001) * tx;
	const float x11 = c011 + (c111 - c011) * tx;
	const float y0  = x00 + (x10 - x00) * ty;
	const float y1  = x01 + (x11 - x01) * ty;
	return y0 + (y1 - y0) * tz;
}

// Density modulation factor at a view-space sample point (sx,sy,sz) inside
// the cone (apex P, unit axis D, both view space). Mean ≈ 1 so overall
// beam brightness is preserved; the noise redistributes it into wisps.
static inline float ConeTurbFactor(const ConeTurb &tp,
                                   float sx, float sy, float sz,
                                   float Px, float Py, float Pz,
                                   float Dx, float Dy, float Dz)
{
	if (tp.swirl != 0.0f) {
		// Rotate the sample around the beam axis by φ(axialDist, t):
		// r' = r·cosφ + (D×r)·sinφ (r ⊥ D so Rodrigues collapses).
		const float Wx = sx - Px, Wy = sy - Py, Wz = sz - Pz;
		const float ta = Dx*Wx + Dy*Wy + Dz*Wz;
		const float rx = Wx - ta*Dx, ry = Wy - ta*Dy, rz = Wz - ta*Dz;
		const float phi = tp.swirl * (tp.twistK * ta) + tp.phase;
		const float cs = std::cos(phi), sn = std::sin(phi);
		const float cxv = Dy*rz - Dz*ry;
		const float cyv = Dz*rx - Dx*rz;
		const float czv = Dx*ry - Dy*rx;
		sx = Px + ta*Dx + rx*cs + cxv*sn;
		sy = Py + ta*Dy + ry*cs + cyv*sn;
		sz = Pz + ta*Dz + rz*cs + czv*sn;
	}
	const float wx = tp.M[0][0]*sx + tp.M[0][1]*sy + tp.M[0][2]*sz + tp.camX;
	const float wy = tp.M[1][0]*sx + tp.M[1][1]*sy + tp.M[1][2]*sz + tp.camY;
	const float wz = tp.M[2][0]*sx + tp.M[2][1]*sy + tp.M[2][2]*sz + tp.camZ;
	const float nx = wx * tp.invCell + tp.driftX;
	const float ny = wy * tp.invCell + tp.driftY;
	const float nz = wz * tp.invCell + tp.driftZ;
	float n = ConeTurbNoise3(nx, ny, nz);
	if (tp.octaves >= 2)
		n = 0.65f * n + 0.35f * ConeTurbNoise3(nx * 2.03f + 37.2f,
		                                       ny * 2.03f + 11.7f,
		                                       nz * 2.03f +  5.9f);
	const float m = 1.0f + tp.amp * (2.0f * n - 1.0f);
	return m > 0.0f ? m : 0.0f;
}

// ─── 8-wide turbulence (lanes = 8 pixels, same layout as the cone
// integration). A scalar-per-lane version of the factor cost the greets
// cone pass +35 ms/frame (measured); this vector version keeps it in
// the low single digits. Math mirrors the scalar helper (same lattice
// hash / fade / octave weights); the swirl sin/cos uses the standard
// parabola approx (~0.1% err — invisible at density-modulation gain).
static inline __m256 ConeTurbAbs_x8(__m256 x) {
	return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), x);
}
// sin over [-π,π]: y = B·r + C·r·|r|, refined y += 0.225(y·|y| − y).
static inline __m256 ConeTurbSin_x8(__m256 r) {
	const __m256 B = _mm256_set1_ps(1.27323954f);
	const __m256 C = _mm256_set1_ps(-0.405284735f);
	__m256 y = _mm256_fmadd_ps(_mm256_mul_ps(C, r), ConeTurbAbs_x8(r),
	                           _mm256_mul_ps(B, r));
	y = _mm256_fmadd_ps(_mm256_set1_ps(0.225f),
	                    _mm256_fmsub_ps(y, ConeTurbAbs_x8(y), y), y);
	return y;
}
// Wrap x to [-π,π].
static inline __m256 ConeTurbWrapPi_x8(__m256 x) {
	const __m256 inv2pi = _mm256_set1_ps(0.15915494f);
	const __m256 twopi  = _mm256_set1_ps(6.2831853f);
	const __m256 n = _mm256_round_ps(_mm256_mul_ps(x, inv2pi), 0 + 8);
	return _mm256_fnmadd_ps(n, twopi, x);
}
// One octave of 8-wide value noise — the vector twin of ConeTurbNoise3
// (identical lattice hash constants and avalanche).
static inline __m256 ConeTurbNoise3_x8(__m256 x, __m256 y, __m256 z) {
	const __m256 fx = _mm256_floor_ps(x);
	const __m256 fy = _mm256_floor_ps(y);
	const __m256 fz = _mm256_floor_ps(z);
	__m256 tx = _mm256_sub_ps(x, fx);
	__m256 ty = _mm256_sub_ps(y, fy);
	__m256 tz = _mm256_sub_ps(z, fz);
	const __m256 three = _mm256_set1_ps(3.0f), two = _mm256_set1_ps(2.0f);
	tx = _mm256_mul_ps(_mm256_mul_ps(tx, tx), _mm256_fnmadd_ps(two, tx, three));
	ty = _mm256_mul_ps(_mm256_mul_ps(ty, ty), _mm256_fnmadd_ps(two, ty, three));
	tz = _mm256_mul_ps(_mm256_mul_ps(tz, tz), _mm256_fnmadd_ps(two, tz, three));
	const __m256i K1 = _mm256_set1_epi32((int)0x8DA6B343u);
	const __m256i K2 = _mm256_set1_epi32((int)0xD8163841u);
	const __m256i K3 = _mm256_set1_epi32((int)0xCB1AB31Fu);
	const __m256i ix = _mm256_cvttps_epi32(fx);
	const __m256i iy = _mm256_cvttps_epi32(fy);
	const __m256i iz = _mm256_cvttps_epi32(fz);
	// base = ix·K1 + iy·K2 + iz·K3; the 8 corners are base + subset sums
	// of {K1,K2,K3} — adds only, 3 mullo total.
	const __m256i base = _mm256_add_epi32(
	    _mm256_add_epi32(_mm256_mullo_epi32(ix, K1), _mm256_mullo_epi32(iy, K2)),
	    _mm256_mullo_epi32(iz, K3));
	const __m256i k12  = _mm256_add_epi32(K1, K2);
	// Avalanche + 24-bit → float [0,1): h^=h>>13; h*=K; h^=h>>16.
	const __m256i AV = _mm256_set1_epi32((int)0xC2B2AE35u);
	const __m256i M24 = _mm256_set1_epi32(0x00FFFFFF);
	const __m256 inv24 = _mm256_set1_ps(1.0f / 16777216.0f);
	auto corner = [&](__m256i h) -> __m256 {
		h = _mm256_xor_si256(h, _mm256_srli_epi32(h, 13));
		h = _mm256_mullo_epi32(h, AV);
		h = _mm256_xor_si256(h, _mm256_srli_epi32(h, 16));
		return _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_and_si256(h, M24)), inv24);
	};
	const __m256 c000 = corner(base);
	const __m256 c100 = corner(_mm256_add_epi32(base, K1));
	const __m256 c010 = corner(_mm256_add_epi32(base, K2));
	const __m256 c110 = corner(_mm256_add_epi32(base, k12));
	const __m256i baseZ = _mm256_add_epi32(base, K3);
	const __m256 c001 = corner(baseZ);
	const __m256 c101 = corner(_mm256_add_epi32(baseZ, K1));
	const __m256 c011 = corner(_mm256_add_epi32(baseZ, K2));
	const __m256 c111 = corner(_mm256_add_epi32(baseZ, k12));
	auto lerp = [&](__m256 a, __m256 b, __m256 t) -> __m256 {
		return _mm256_fmadd_ps(_mm256_sub_ps(b, a), t, a);
	};
	const __m256 x00 = lerp(c000, c100, tx);
	const __m256 x10 = lerp(c010, c110, tx);
	const __m256 x01 = lerp(c001, c101, tx);
	const __m256 x11 = lerp(c011, c111, tx);
	const __m256 y0  = lerp(x00, x10, ty);
	const __m256 y1  = lerp(x01, x11, ty);
	return lerp(y0, y1, tz);
}

// Vector twin of ConeTurbFactor: 8 view-space sample points at once.
static inline __m256 ConeTurbFactor_x8(const ConeTurb &tp,
                                       __m256 sx, __m256 sy, __m256 sz,
                                       float Px, float Py, float Pz,
                                       float Dx, float Dy, float Dz)
{
	if (tp.swirl != 0.0f) {
		const __m256 vPx = _mm256_set1_ps(Px), vPy = _mm256_set1_ps(Py),
		             vPz = _mm256_set1_ps(Pz);
		const __m256 vDx = _mm256_set1_ps(Dx), vDy = _mm256_set1_ps(Dy),
		             vDz = _mm256_set1_ps(Dz);
		const __m256 Wx = _mm256_sub_ps(sx, vPx);
		const __m256 Wy = _mm256_sub_ps(sy, vPy);
		const __m256 Wz = _mm256_sub_ps(sz, vPz);
		const __m256 ta = _mm256_fmadd_ps(vDx, Wx,
		                  _mm256_fmadd_ps(vDy, Wy, _mm256_mul_ps(vDz, Wz)));
		const __m256 rx = _mm256_fnmadd_ps(ta, vDx, Wx);
		const __m256 ry = _mm256_fnmadd_ps(ta, vDy, Wy);
		const __m256 rz = _mm256_fnmadd_ps(ta, vDz, Wz);
		const __m256 phi = _mm256_fmadd_ps(
		    _mm256_set1_ps(tp.swirl * tp.twistK), ta,
		    _mm256_set1_ps(tp.phase));
		const __m256 rphi = ConeTurbWrapPi_x8(phi);
		const __m256 sn = ConeTurbSin_x8(rphi);
		const __m256 cs = ConeTurbSin_x8(ConeTurbWrapPi_x8(
		    _mm256_add_ps(rphi, _mm256_set1_ps(1.57079633f))));
		const __m256 cxv = _mm256_fmsub_ps(vDy, rz, _mm256_mul_ps(vDz, ry));
		const __m256 cyv = _mm256_fmsub_ps(vDz, rx, _mm256_mul_ps(vDx, rz));
		const __m256 czv = _mm256_fmsub_ps(vDx, ry, _mm256_mul_ps(vDy, rx));
		sx = _mm256_add_ps(_mm256_fmadd_ps(ta, vDx, vPx),
		     _mm256_fmadd_ps(rx, cs, _mm256_mul_ps(cxv, sn)));
		sy = _mm256_add_ps(_mm256_fmadd_ps(ta, vDy, vPy),
		     _mm256_fmadd_ps(ry, cs, _mm256_mul_ps(cyv, sn)));
		sz = _mm256_add_ps(_mm256_fmadd_ps(ta, vDz, vPz),
		     _mm256_fmadd_ps(rz, cs, _mm256_mul_ps(czv, sn)));
	}
	const __m256 wx = _mm256_fmadd_ps(_mm256_set1_ps(tp.M[0][0]), sx,
	                  _mm256_fmadd_ps(_mm256_set1_ps(tp.M[0][1]), sy,
	                  _mm256_fmadd_ps(_mm256_set1_ps(tp.M[0][2]), sz,
	                                  _mm256_set1_ps(tp.camX))));
	const __m256 wy = _mm256_fmadd_ps(_mm256_set1_ps(tp.M[1][0]), sx,
	                  _mm256_fmadd_ps(_mm256_set1_ps(tp.M[1][1]), sy,
	                  _mm256_fmadd_ps(_mm256_set1_ps(tp.M[1][2]), sz,
	                                  _mm256_set1_ps(tp.camY))));
	const __m256 wz = _mm256_fmadd_ps(_mm256_set1_ps(tp.M[2][0]), sx,
	                  _mm256_fmadd_ps(_mm256_set1_ps(tp.M[2][1]), sy,
	                  _mm256_fmadd_ps(_mm256_set1_ps(tp.M[2][2]), sz,
	                                  _mm256_set1_ps(tp.camZ))));
	const __m256 nx = _mm256_fmadd_ps(wx, _mm256_set1_ps(tp.invCell),
	                                  _mm256_set1_ps(tp.driftX));
	const __m256 ny = _mm256_fmadd_ps(wy, _mm256_set1_ps(tp.invCell),
	                                  _mm256_set1_ps(tp.driftY));
	const __m256 nz = _mm256_fmadd_ps(wz, _mm256_set1_ps(tp.invCell),
	                                  _mm256_set1_ps(tp.driftZ));
	__m256 n = ConeTurbNoise3_x8(nx, ny, nz);
	if (tp.octaves >= 2) {
		const __m256 s2 = _mm256_set1_ps(2.03f);
		n = _mm256_fmadd_ps(n, _mm256_set1_ps(0.65f),
		    _mm256_mul_ps(_mm256_set1_ps(0.35f),
		    ConeTurbNoise3_x8(
		        _mm256_fmadd_ps(nx, s2, _mm256_set1_ps(37.2f)),
		        _mm256_fmadd_ps(ny, s2, _mm256_set1_ps(11.7f)),
		        _mm256_fmadd_ps(nz, s2, _mm256_set1_ps( 5.9f)))));
	}
	const __m256 m = _mm256_fmadd_ps(
	    _mm256_set1_ps(tp.amp),
	    _mm256_fmsub_ps(_mm256_set1_ps(2.0f), n, _mm256_set1_ps(1.0f)),
	    _mm256_set1_ps(1.0f));
	return _mm256_max_ps(m, _mm256_setzero_ps());
}

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
static void Render_DeferredFogPass_Tile(const DeferredLightingCtx &ctx,
                                         int x1, int y1, int x2, int y2,
                                         float invFZP)
{
	// Render-target state from the per-frame ctx (populated by
	// Render_DeferredLighting, which runs before any volumetric pass — see
	// renderFrame order). Locals shadow the globals so the body is untouched.
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

// Per-tile spot-list cap — see the tile filter in Render_VolumetricCones.
// File scope because the per-(tile × spot) precompute below sizes on it.
constexpr int CONE_TILE_SPOT_CAP = 64;

// ─── Per-(tile × spot) INVARIANTS, computed ONCE per tile ────────────────
// The spot loop is the innermost of three (row → 8-px batch → spot), so
// every value that depends only on the LIGHT was being recomputed for every
// (batch × spot) pair: twelve scattered SoA loads, two three-term dot
// products, a square, four ternaries and a DIVIDE. Round 6's ablation ladder
// priced exactly that block at 8.3 % of the whole cone pass on chase and
// 7.2 % of its cycles — 104 instructions per pair (f1ffc925,
// docs/HW_PROFILING.md §14.7).
//
// Hoisting it to the tile is BIT-EXACT BY CONSTRUCTION: identical
// expressions on identical operands, evaluated once instead of ~10 000 times
// per tile (a coarse 6×4 tile at 1920×1080 is 40 batches × 270 rows). Every
// field below is a VERBATIM move of the line it replaces — no term is
// re-associated and no product is respelled — so the compiler's contraction
// map (docs/HW_PROFILING.md §13) travels with the value.
//
// `sphereC`, `cq` and the three pre-scaled broadcast constants are the same
// idea one stage further down: the solve recomputed them per pair from these
// same per-spot scalars. `c2+c2`, `DP+DP` and `cq*-4` are exact operations
// (power-of-two scaling and negation), so folding them here rounds nothing
// that was not already rounded.
struct ConeSpotPre {
	float Px, Py_l, Pz;             // apex, view space
	float Dx, Dy, Dz;               // axis, view space
	float cosO, cosI;
	float r2, rr;
	float DP, PP, c2;
	float inv_cosI_minus_cosO;      // THE divide
	float inv_nSamp;
	float sphereC;                  // PP - r2
	float cq;                       // fma(DP, DP, -(c2*PP))
	float c2x2;                     // c2 + c2          (exact)
	float negDPx2;                  // -(DP + DP)       (exact)
	float cqm4;                     // cq * -4          (exact)
	float hsNx, hsNy, hsNz, hsD;    // bounce half-space plane, view space
	uint32_t omid;                  // mirror-clone id (0 = real spot)
	int32_t  li;                    // index back into the light SoA
	int32_t  nSamp;
	bool     segPath;
	bool     bounce;
};

static void Render_VolumetricCones_Tile(const DeferredLightingCtx &ctx,
                                         int x1, int y1, int x2, int y2,
                                         const ViewLightsSoA *lights,
                                         const int *spotIdx, int spotCount,
                                         float invFOVX, float invFOVY,
                                         float invZScale, float density,
                                         float fogZ, float invFogZ,
                                         const ConeTurb &turb) {
    // Render state from the threaded ctx, not globals. Local refs/aliases
    // shadow the file-scope names so the (large) body is untouched.
    const DeferredLightingCtx &dc = ctx;
    const meka::GBuffer *const g_gbuffer = ctx.gb;
    const int XRes = ctx.xres;
    byte *const VPage = ctx.vpage;
    word *const ZPage16 = ctx.zpage16;
    const float CntrEX = ctx.cntrEX, CntrEY = ctx.cntrEY;
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
#if FDS_CONE_ABLATE
    // Per-tile ablation sink (see the ladder note at FDS_CONE_ABLATE).
    __m256 ablSinkV = _mm256_setzero_ps();
#endif
    // 8-wide cone-interval solve (see the transcript note at the loop).
    // Read once per tile call — the branch never enters the per-spot loop.
    const bool coneSolveVec = (FDS_CONE_FORCE != 0)
                            || fds::FeatureFlags::vol_cone_solve_vec();
    // The two leftover SCALAR per-lane loops (dz/fade window, colour
    // accumulate) 8 lanes wide. Read once per tile, same as above.
    const bool laneVec = (FDS_CONE_FORCE != 0)
                       || fds::FeatureFlags::vol_cone_lane_vec();
    // The same solve at the machine's NATIVE 128-bit width, run twice over the
    // 8-pixel batch (see FDS_CONE_W4). Compile-time only: at the default 0 the
    // arm below emits literally nothing and the shipping kernel disassembles to
    // the parent's histogram.
    constexpr bool solveW4 = (FDS_CONE_W4 != 0);
    // min/max, one NEON instruction under FDS_CONE_NEONMINMAX and the
    // byte-exact cmp+blend transcript otherwise. MAXT/MINT reproduce the
    // hand-written spelling in the solve and the dz/fade loop
    // (max = (a<b)?b:a, min = (b<a)?b:a); VMAX/VMIN stand in for the
    // _mm256_max_ps/_mm256_min_ps calls already in the body, which lower to
    // the same two ops because SIMDE_FAST_NANS is not defined here.
    auto MAXT = [](const __m256 &a, const __m256 &b) {
#if FDS_CONE_NEONMINMAX
        return fmax_x8(a, b);
#else
        return _mm256_blendv_ps(a, b, _mm256_cmp_ps(a, b, _CMP_LT_OQ));
#endif
    };
    auto MINT = [](const __m256 &a, const __m256 &b) {
#if FDS_CONE_NEONMINMAX
        return fmin_x8(a, b);
#else
        return _mm256_blendv_ps(a, b, _mm256_cmp_ps(b, a, _CMP_LT_OQ));
#endif
    };
    auto VMAX = [](const __m256 &a, const __m256 &b) {
#if FDS_CONE_NEONMINMAX
        return fmax_x8(a, b);
#else
        return _mm256_max_ps(a, b);
#endif
    };
    auto VMIN = [](const __m256 &a, const __m256 &b) {
#if FDS_CONE_NEONMINMAX
        return fmin_x8(a, b);
#else
        return _mm256_min_ps(a, b);
#endif
    };
    const __m256 vDensity_v = _mm256_set1_ps(density);
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
    if (fds::FeatureFlags::vol_prof()) {
        (useAnalytic ? g_coneAnalyticHits
                     : g_coneRaymarchHits)
            .fetch_add(1, std::memory_order_relaxed);
    }

    // Half vertical rate: compute even rows, write each result to the
    // row and its lower neighbor. Beams are soft gradients — visually
    // free except at narrow-beam edges. (Vec path only; the scalar path
    // is the --no-vol_vec fallback.)
    //
    // Tie the cone rate to the deferred fill mode: when the surface kernel
    // runs reduced-rate (quarter or checkerboard), halve the cone vertical
    // rate too — one rate knob for the whole reduced-rate frame. The
    // standalone --vol-cone-half-y still forces it independently.
    //
    // No true 2D quarter for cones (my call, per the half-y A/B): this loop
    // is pixel-major SIMD, 8 lanes across X, so X-subsampling wastes lanes
    // and fights the kernel; and the narrow disco beams (2.6°/7°) already
    // show thin one-row edge artifacts at half-Y — quartering both axes
    // doubles that for only ~2 ms on a ~15 ms pass. Row-doubling is the
    // sweet spot; the surface kernel keeps its own 2D quarter/checker.
    const bool coneReduced = fds::FeatureFlags::vol_cone_half_y()
                          || fds::FeatureFlags::deferred_quarter()
                          || fds::FeatureFlags::deferred_checkerboard();
    const int yStep = (vecPath && coneReduced) ? 2 : 1;

    // ─── the per-(tile × spot) precompute (see ConeSpotPre) ─────────────
    // Built once here, consumed by the SIMD spot loop below. Spots the old
    // prologue `continue`d out of are simply not appended, so the compacted
    // list reproduces the old iteration exactly — both `continue`s sat
    // BEFORE the first diag counter, so the census is unaffected too.
    ConeSpotPre conePre[CONE_TILE_SPOT_CAP];
    int conePreCount = 0;
    if (vecPath) {
        const int nPre = std::min(spotCount, CONE_TILE_SPOT_CAP);
        for (int s = 0; s < nPre; ++s) {
            const int li = spotIdx[s];
            const float Px = lights->posX[li], Py_l = lights->posY[li], Pz = lights->posZ[li];
            const float Dx = lights->dirX[li], Dy = lights->dirY[li], Dz = lights->dirZ[li];
            const float cosO = lights->cosOuter[li];
            const float cosI = lights->cosInner[li];
            // Narrow cones (half-angle < ~10°, the disco beams) take the
            // per-segment hybrid inside the analytic branch; nSamp only
            // matters when the analytic flag is off. Turbulence needs
            // per-segment density taps, so turbulent wide cones route
            // through the segmented hybrid too — turb.on=false →
            // segPath==narrowCone, the legacy branch structure.
            const bool  narrowCone = cosO > 0.985f;
            const int   nSamp      = narrowCone ? 16 : N_SAMPLES;
            const float inv_nSamp  = narrowCone ? (1.0f / 16.0f) : inv_N;
            const bool  segPath    = narrowCone || turb.on;
            // Clone spot: beam reflection, confined to its mirror's
            // footprint by the per-pixel gate in the solve.
            const uint32_t omid = lights->mirrorId[li];
            if (omid != 0 && (!mmask || !mmz)) continue;
            // Bounce spot: chord clamped to the camera side of its mirror
            // plane (apex behind the glass). Plane → view space:
            // n_v = viewMatᵀ·N, d_v = N·camW + D.
            const bool bounce = lights->bounceClamp[li] != 0;
            float hsNx=0, hsNy=0, hsNz=0, hsD=0;
            if (bounce) {
                const float Nx = lights->mirNX[li];
                const float Ny = lights->mirNY[li];
                const float Nz = lights->mirNZ[li];
                const auto &VW = dc.viewToWorld;
                hsNx = VW[0][0]*Nx + VW[1][0]*Ny + VW[2][0]*Nz;
                hsNy = VW[0][1]*Nx + VW[1][1]*Ny + VW[2][1]*Nz;
                hsNz = VW[0][2]*Nx + VW[1][2]*Ny + VW[2][2]*Nz;
                hsD  = Nx*dc.cameraWorldX +
                       Ny*dc.cameraWorldY +
                       Nz*dc.cameraWorldZ +
                       lights->mirD[li];
                if (hsD == 0.0f) continue;  // camera on the glass
            }
            const float r2   = lights->range2[li];
            const float rr   = lights->rRange[li];
            const float DP   = Dx*Px + Dy*Py_l + Dz*Pz;
            const float PP   = Px*Px + Py_l*Py_l + Pz*Pz;
            const float c2   = cosO * cosO;
            const float inv_cosI_minus_cosO = 1.0f / (cosI - cosO);
            const float cq   = std::fma(DP, DP, -(c2 * PP));
            ConeSpotPre &p = conePre[conePreCount++];
            p.Px = Px; p.Py_l = Py_l; p.Pz = Pz;
            p.Dx = Dx; p.Dy = Dy;    p.Dz = Dz;
            p.cosO = cosO; p.cosI = cosI;
            p.r2 = r2; p.rr = rr;
            p.DP = DP; p.PP = PP; p.c2 = c2;
            p.inv_cosI_minus_cosO = inv_cosI_minus_cosO;
            p.inv_nSamp = inv_nSamp;
            p.sphereC = PP - r2;
            p.cq      = cq;
            p.c2x2    = c2 + c2;
            p.negDPx2 = -(DP + DP);
            p.cqm4    = cq * -4.0f;
            p.hsNx = hsNx; p.hsNy = hsNy; p.hsNz = hsNz; p.hsD = hsD;
            p.omid = omid;
            p.li = li;
            p.nSamp = nSamp;
            p.segPath = segPath;
            p.bounce = bounce;
        }
    }

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
                // 8-wide colour accumulators — see the accumulate note at
                // the bottom of the per-spot loop. Live in REGISTERS across
                // the whole spot loop under --vol_cone_lane_vec; drained to
                // accB/accG/accR once per batch, so the composite loop
                // below is untouched.
                __m256 vAccB = _mm256_setzero_ps();
                __m256 vAccG = _mm256_setzero_ps();
                __m256 vAccR = _mm256_setzero_ps();

                for (int s = 0; s < conePreCount; ++s) {
                    CONE_ABL_CUT_BARE(1);   // ablation: per-batch floor
                    // Per-batch rect cull: skip if the 8-pixel batch
                    // (per-batch rect-cull experiment was reverted —
                    // per-tile cull at dispatcher already does screen-
                    // rect check; per-batch overhead didn't pay across
                    // the city sweep.)
                    //
                    // EVERYTHING here used to be COMPUTED at this point,
                    // once per (batch × spot) — the 104-instruction
                    // prologue round 6 priced at 8.3 % of the pass. It is
                    // now a straight read of the per-tile ConeSpotPre
                    // record built before the row loop; see the struct's
                    // note for why that is bit-exact rather than merely
                    // equivalent.
                    const ConeSpotPre &P_ = conePre[s];
                    const int   li   = P_.li;
                    const float Px   = P_.Px, Py_l = P_.Py_l, Pz = P_.Pz;
                    const float Dx   = P_.Dx, Dy   = P_.Dy,   Dz = P_.Dz;
                    const float cosO = P_.cosO, cosI = P_.cosI;
                    const int   nSamp     = P_.nSamp;
                    const float inv_nSamp = P_.inv_nSamp;
                    const bool  segPath   = P_.segPath;
                    const uint32_t omid   = P_.omid;
                    const bool  bounce    = P_.bounce;
                    const float hsNx = P_.hsNx, hsNy = P_.hsNy,
                                hsNz = P_.hsNz, hsD  = P_.hsD;
                    const float r2   = P_.r2;
                    const float rr   = P_.rr;
                    const float DP   = P_.DP;
                    const float PP   = P_.PP;
                    const float c2   = P_.c2;
                    const float inv_cosI_minus_cosO = P_.inv_cosI_minus_cosO;
                    // ablation: keep the per-spot scalar prologue only.
                    CONE_ABL_CUT(2, _mm256_set1_ps(DP + PP + c2 +
                                                   inv_cosI_minus_cosO +
                                                   r2 + rr + inv_nSamp));

                    alignas(32) float zLoArr[8] = {};
                    alignas(32) float zHiArr[8] = {};
                    alignas(32) float aliveLane[8] = {};
                    bool spotAlive = false;
                    if (g_coneDiag) {
                        g_dPairs.fetch_add(1, std::memory_order_relaxed);
                        if (segPath) g_dSegPair.fetch_add(1, std::memory_order_relaxed);
                    }
                    // ─── 8-wide cone-interval solve ──────────────────
                    // The scalar loop below is 63.6% of this whole pass
                    // (2.681 of 4.217 Ginstr/f in city; 25.6 M scalar
                    // solves/frame at ~105 instructions each — commit
                    // a16567b). It sits inside the per-spot loop feeding
                    // a body that is already 8-wide, so the lever is to
                    // widen it. Everything here is a BIT-EXACT transcript
                    // of the scalar arm, read off the arm64 disassembly
                    // of the shipping binary rather than off the source —
                    // under the tree-wide -ffp-contract=fast the compiler
                    // picks WHICH product of each multiply-add pair to
                    // fuse, and the choice is not what the source order
                    // suggests. The map it chose (all verified against
                    // objdump, `fmadd`/`fnmsub` operand by operand):
                    //   VP  = fl( Pz + fma(Px, X, fl(Y*Py)) )
                    //   DV  = fl( Dz + fma(Dx, X, fl(Y*Dy)) )
                    //   sD  = fma(VP, VP, -fl(sphereC*uV))
                    //   a   = fma(DV, DV, -fl(c2*uV))
                    //   b   = t+t,  t = fma(c2, VP, -fl(DP*DV))
                    //   disc= fma(b, b, fl(cq * fl(a * -4)))
                    //   cq  = fma(DP, DP, -fl(c2*PP))
                    // i.e. in every pair the SECOND product is the one
                    // that gets rounded. `Y*Py` and `Y*Dy` are rounded
                    // scalars the scalar loop's LICM already hoisted out
                    // per (row × spot) — kept scalar here for the same
                    // reason and broadcast. `1/uV`, `1/(2a)` and `DP/DV`
                    // stay true IEEE divides and the two sqrts stay true
                    // sqrts: no rcp/rsqrt+NR shortcut, that family is a
                    // known image-changer in this codebase (rsqrt_nr_x8).
                    // fma_x8 is the only fused op used, because simde's
                    // _mm256_fmadd_ps lowers to vfmaq_f32 while
                    // _mm256_fmsub_ps lowers to sub(mul(...)) and would
                    // leave the fusion choice back with the compiler —
                    // a*b-c is written fma(a,b,neg(c)) so the rounded
                    // operand is pinned by construction.
                    // Mirror-clone (omid) and bounce spots keep the
                    // scalar arm: both need per-lane gathers/branches
                    // that aren't worth widening at their frequency.
                    // SEGMENTED cones (narrow disco beams, and any
                    // turbulent cone) USED TO be excluded here as well, on
                    // round 1's greets measurement — ungated at the greets
                    // pin pose the wide arm read instructions −3.8% but
                    // cycles +3.6% and wall +1.4% (7.63 → 7.73 ms), because
                    // it computes both a-sign branches, both roots and the
                    // whole tail for all eight lanes where the scalar arm's
                    // dead lanes bail early, and greets' 8-segment body made
                    // the solve a minor share.
                    // THE GATE IS GONE, and that is a measurement too.
                    // Round 6 profiled CHASE, which was never in the
                    // campaign: its cone population is 34 spots of which 32
                    // are narrow (DIAG `narrow(seg8)=32`), so 91 % of its
                    // (batch × spot) pairs were on the scalar arm this gate
                    // forced — and the round-2 ablation ladder, re-run on
                    // chase, put 53.2 % of the whole pass (1.487 of 2.797
                    // Ginstr/f) in that one scalar solve. What changed since
                    // round 1 is the KERNEL, not the argument: NEONMINMAX,
                    // the round-4b/5 algebra folds and the lane_vec loops
                    // all landed after the gate was set. Re-measured against
                    // a real parent binary, interleaved min-of-6:
                    //   chase t=800    cones 18.30 → 14.93 ms, 2.796 →
                    //                  2.192 Ginstr/f, 0.622 → 0.500 Gcyc/f
                    //   greets t=1588  cones 7.53 → 7.41 ms  (its own beams
                    //                  are segmented, and they IMPROVE)
                    //   city t=1961    0.540 → 0.537 Gcyc/f (all wide; the
                    //                  term's removal only perturbs codegen)
                    // Both figures include FDS_CONE_QUADEARLYOUT, which is
                    // what keeps city whole — see its note at the top.
                    // NOT byte-null, unlike round 1's version of this arm:
                    // the wide arm's VP/DV fold (round 5) rounds Y·Py + Pz
                    // once where the scalar arm rounds Y·Py first, so the
                    // segmented cones now inherit that judge call.
                    const bool solveVec = coneSolveVec && omid == 0 && !bounce;
                    // ─── the same solve at NATIVE 128-bit width ──────
                    // Two 4-lane passes over the same 8-pixel batch. The
                    // NEON op count is identical (simde already emits two
                    // 128-bit ops per __m256 op); what changes is that a
                    // live value now costs ONE v-register instead of two,
                    // so the ~30-value live set stops overflowing a
                    // 32-register file. Aimed at CYCLES, not instructions:
                    // the round-4 cycle ablation puts this stage at 0.250
                    // of the pass's 0.585 Gcyc/f (42.7%), and round 3
                    // established the pass is dependency-bound.
                    // Transcribed op for op from the 8-wide arm below —
                    // same contraction map, same cmp+blend spelling of
                    // std::min/max, same unordered predicates. Xarr/uVarr/
                    // zMaxArr are alignas(32), so +4 floats is still a
                    // 16-byte-aligned address.
                    if (solveVec && solveW4) {
                        const __m128 qZero = _mm_setzero_ps();
                        const __m128 qOne  = _mm_set1_ps(1.0f);
                        const __m128 qZMin = _mm_set1_ps(0.05f);
                        const __m128 qAll  = _mm_cmp_ps(qZero, qZero, _CMP_EQ_OQ);
                        auto NEG = [](const __m128 &v) {
                            return _mm_xor_ps(v, _mm_set1_ps(-0.0f));
                        };
                        auto NOT = [&](const __m128 &v) {
                            return _mm_xor_ps(v, qAll);
                        };
                        auto RCP   = [&](const __m128 &v) {
                            return _mm_div_ps(qOne, v);
                        };
                        auto SQRTV = [](const __m128 &v) {
                            return _mm_sqrt_ps(v);
                        };
                        // Scalars the 8-wide arm also computes once per
                        // (batch × spot); hoisted out of the half loop so
                        // the halves share them rather than recompute.
                        const float  YPy     = Y * Py_l;
                        const float  YDy     = Y * Dy;
                        const float  sphereC = P_.sphereC;   // per-tile
                        const float  cq      = P_.cq;        // per-tile
                        int aliveBits = 0;
#if FDS_CONE_W4 == 2
                        // Control build: keep the half loop ROLLED. Left to
                        // itself the compiler fully unrolls a trip count of 2
                        // and schedules both halves together, which restores
                        // exactly the 8-wide live set — so the unrolled W4 arm
                        // is not a test of register pressure at all. This is.
                        #pragma clang loop unroll(disable)
#endif
                        for (int h = 0; h < 8; h += 4) {
                            const __m128 sX    = _mm_load_ps(Xarr + h);
                            const __m128 sUV   = _mm_load_ps(uVarr + h);
                            const __m128 sZMax = _mm_load_ps(zMaxArr + h);

                            __m128 mAlive = _mm_cmp_ps(sZMax, qZero, _CMP_NLE_UQ);

                            const __m128 sVP = _mm_add_ps(_mm_set1_ps(Pz),
                                fma_x4(_mm_set1_ps(Px), sX, _mm_set1_ps(YPy)));
                            const __m128 sDV = _mm_add_ps(_mm_set1_ps(Dz),
                                fma_x4(_mm_set1_ps(Dx), sX, _mm_set1_ps(YDy)));

                            const __m128 sC2   = _mm_set1_ps(c2);
                            const __m128 sSphD = fma_x4(sVP, sVP,
                                NEG(_mm_mul_ps(_mm_set1_ps(sphereC), sUV)));
                            mAlive = _mm_and_ps(mAlive,
                                _mm_cmp_ps(sSphD, qZero, _CMP_NLT_UQ));

                            const __m128 sA  = fma_x4(sDV, sDV,
                                NEG(_mm_mul_ps(sC2, sUV)));
                            const __m128 sBh = fma_x4(sC2, sVP,
                                NEG(_mm_mul_ps(_mm_set1_ps(DP), sDV)));
                            const __m128 sB  = _mm_add_ps(sBh, sBh);
                            const __m128 sDisc = fma_x4(sB, sB,
                                _mm_mul_ps(_mm_set1_ps(cq),
                                    _mm_mul_ps(sA, _mm_set1_ps(-4.0f))));

                            const __m128 sSq    = SQRTV(sDisc);
                            const __m128 sInv2a = RCP(_mm_add_ps(sA, sA));
                            const __m128 sR1 = _mm_mul_ps(sInv2a,
                                _mm_sub_ps(NEG(sB), sSq));
                            const __m128 sR2 = _mm_mul_ps(sInv2a,
                                _mm_sub_ps(sSq, sB));
                            const __m128 rMin = _mm_blendv_ps(sR1, sR2,
                                _mm_cmp_ps(sR2, sR1, _CMP_LT_OQ));
                            const __m128 rMax = _mm_blendv_ps(sR1, sR2,
                                _mm_cmp_ps(sR1, sR2, _CMP_LT_OQ));

                            const __m128 mDisc = _mm_cmp_ps(sDisc, qZero, _CMP_NLT_UQ);
                            const __m128 mANeg = _mm_cmp_ps(sA,
                                _mm_set1_ps(-1e-8f), _CMP_LT_OQ);
                            const __m128 mAPos = _mm_cmp_ps(sA,
                                _mm_set1_ps(1e-8f), _CMP_GT_OQ);

                            const __m128 zLoHit = _mm_blendv_ps(rMax, qZMin,
                                _mm_cmp_ps(rMax, qZMin, _CMP_LT_OQ));
                            const __m128 zHiHit = _mm_blendv_ps(rMin, sZMax,
                                _mm_cmp_ps(sZMax, rMin, _CMP_LT_OQ));
                            const __m128 mFwd = _mm_cmp_ps(sDV,
                                _mm_set1_ps(1e-6f), _CMP_GT_OQ);
                            const __m128 mBwd = _mm_cmp_ps(sDV,
                                _mm_set1_ps(-1e-6f), _CMP_LT_OQ);
                            const __m128 zLoRt = _mm_blendv_ps(qZMin, zLoHit, mFwd);
                            const __m128 zHiRt = _mm_blendv_ps(zHiHit, sZMax, mFwd);
                            const __m128 mPosOk = _mm_or_ps(NOT(mDisc),
                                _mm_and_ps(_mm_or_ps(mFwd, mBwd),
                                    _mm_cmp_ps(zHiRt, zLoRt, _CMP_GT_OQ)));
                            const __m128 zLoP = _mm_blendv_ps(qZMin, zLoRt, mDisc);
                            const __m128 zHiP = _mm_blendv_ps(sZMax, zHiRt, mDisc);

                            __m128 zLo = _mm_blendv_ps(zLoP, rMin, mANeg);
                            __m128 zHi = _mm_blendv_ps(zHiP, rMax, mANeg);
                            mAlive = _mm_and_ps(mAlive,
                                _mm_or_ps(_mm_and_ps(mANeg, mDisc),
                                          _mm_and_ps(mAPos, mPosOk)));

                            const __m128 sSphSq = SQRTV(sSphD);
                            const __m128 sInvUV = RCP(sUV);
                            const __m128 zSphLo = _mm_mul_ps(sInvUV,
                                _mm_sub_ps(sVP, sSphSq));
                            const __m128 zSphHi = _mm_mul_ps(sInvUV,
                                _mm_add_ps(sVP, sSphSq));
                            zLo = _mm_blendv_ps(zLo, zSphLo,
                                _mm_cmp_ps(zLo, zSphLo, _CMP_LT_OQ));
                            zHi = _mm_blendv_ps(zHi, zSphHi,
                                _mm_cmp_ps(zHi, zSphHi, _CMP_GT_OQ));
                            zLo = _mm_blendv_ps(zLo, qZMin,
                                _mm_cmp_ps(zLo, qZMin, _CMP_LT_OQ));
                            mAlive = _mm_and_ps(mAlive,
                                _mm_cmp_ps(zHi, zLo, _CMP_GT_OQ));
                            mAlive = _mm_and_ps(mAlive,
                                _mm_cmp_ps(zLo, sZMax, _CMP_LT_OQ));
                            zHi = _mm_blendv_ps(zHi, sZMax,
                                _mm_cmp_ps(zHi, sZMax, _CMP_GT_OQ));

                            const __m128 mDVBig = _mm_cmp_ps(
                                _mm_andnot_ps(_mm_set1_ps(-0.0f), sDV),
                                _mm_set1_ps(1e-6f), _CMP_GT_OQ);
                            const __m128 zFwd = _mm_div_ps(
                                _mm_set1_ps(DP), sDV);
                            const __m128 mDVPos = _mm_cmp_ps(sDV, qZero, _CMP_GT_OQ);
                            zLo = _mm_blendv_ps(zLo, zFwd,
                                _mm_and_ps(_mm_and_ps(mDVBig, mDVPos),
                                    _mm_cmp_ps(zLo, zFwd, _CMP_LT_OQ)));
                            zHi = _mm_blendv_ps(zHi, zFwd,
                                _mm_and_ps(_mm_andnot_ps(mDVPos, mDVBig),
                                    _mm_cmp_ps(zHi, zFwd, _CMP_GT_OQ)));
                            mAlive = _mm_and_ps(mAlive,
                                _mm_or_ps(NOT(mDVBig),
                                    _mm_cmp_ps(zLo, zHi, _CMP_LT_OQ)));

                            _mm_store_ps(zLoArr + h,    _mm_and_ps(zLo, mAlive));
                            _mm_store_ps(zHiArr + h,    _mm_and_ps(zHi, mAlive));
                            _mm_store_ps(aliveLane + h, _mm_and_ps(qOne, mAlive));
                            aliveBits |= _mm_movemask_ps(mAlive) << h;
                        }
                        spotAlive = aliveBits != 0;
                        if (g_coneDiag) {
                            g_dLanes.fetch_add(FDS_POPCOUNT(unsigned(aliveBits)),
                                               std::memory_order_relaxed);
                            if (!spotAlive)
                                g_dDead.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (solveVec) {
                        const __m256 sX    = _mm256_load_ps(Xarr);
                        const __m256 sUV   = _mm256_load_ps(uVarr);
                        const __m256 sZMax = _mm256_load_ps(zMaxArr);
                        const __m256 sZero = _mm256_setzero_ps();
                        const __m256 sOne  = _mm256_set1_ps(1.0f);
                        const __m256 sZMin = _mm256_set1_ps(0.05f);
                        const __m256 sAll  = _mm256_cmp_ps(sZero, sZero, _CMP_EQ_OQ);
                        auto NEG = [](const __m256 &v) {
                            return _mm256_xor_ps(v, _mm256_set1_ps(-0.0f));
                        };
                        auto NOT = [&](const __m256 &v) {
                            return _mm256_xor_ps(v, sAll);
                        };
                        // Reciprocal and square root. THE APPROXIMATE ARM
                        // WAS BUILT AND MEASURED AND IS NOT USED — see the
                        // numbers in the commit message. The reasoning that
                        // sent it there was that vdivq/vsqrtq are the only
                        // non-pipelined ops in the loop (3 divides + 2
                        // sqrts per 8px-batch × spot); the reasoning that
                        // killed it is that this loop is instruction-BOUND,
                        // not latency-bound (IPC 3.90 measured), and one
                        // vrecpe/vrsqrte is only an 8-bit estimate on NEON
                        // — half the 12 bits the x86 intrinsic name
                        // implies — so any usable accuracy needs
                        // Newton-Raphson steps that cost MORE instructions
                        // than the divide they replaced. Measured: raw
                        // estimates, the fastest thing this family can be,
                        // are SLOWER than exact div/sqrt on both counters.
                        // So the vec_ggx precedent never had to be argued
                        // here (the user's point stands that a volumetric
                        // integrand tolerates approximation better than a
                        // reflect() direction does) — approximation simply
                        // does not pay in this loop, and exact div/sqrt
                        // keeps the byte gate for free.
#if FDS_CONE_SOLVE_APPROX
                        auto RCP   = [](const __m256 &v) {
                            return _mm256_rcp_ps(v);
                        };
                        auto SQRTV = [](const __m256 &v) {
                            return _mm256_mul_ps(v, _mm256_rsqrt_ps(v));
                        };
#else
                        auto RCP   = [&](const __m256 &v) {
                            return _mm256_div_ps(sOne, v);
                        };
                        auto SQRTV = [](const __m256 &v) {
                            return _mm256_sqrt_ps(v);
                        };
#endif
                        // Lanes past laneCount have zMaxArr == 0 and die
                        // on this same test, exactly as they do scalar.
                        // NLE_UQ, not GT_OQ: the scalar test is
                        // `if (zMax <= 0) continue`, and the unordered
                        // predicate is its exact negation, so a NaN would
                        // keep the lane on both arms rather than on one.
                        __m256 mAlive = _mm256_cmp_ps(sZMax, sZero, _CMP_NLE_UQ);

                        // VP = X·Px + Y·Py + Pz, DV = X·Dx + Y·Dy + Dz.
                        // Y·Py + Pz and Y·Dy + Dz are BOTH per-(row × spot)
                        // scalars, so the whole tail of each dot product folds
                        // into ONE broadcast: 4 vector-ALU ops per pair
                        // (dup + copy + two fmla) where the transcript spent 7
                        // (dup + copy + two fmla + a second dup + two fadd).
                        // This is the round-4b fold applied to the head of the
                        // chain, and unlike 4b it is NOT bit-exact — the
                        // scalar arm rounds Y·Py before adding Pz, this rounds
                        // the fused Y·Py + Pz once — so it is a judge call on
                        // the numbers below, not a transcript.
                        const float  VPk = std::fma(Y, Py_l, Pz);
                        const float  DVk = std::fma(Y, Dy,   Dz);
                        const __m256 sVP =
                            fma_x8(_mm256_set1_ps(Px), sX, _mm256_set1_ps(VPk));
                        const __m256 sDV =
                            fma_x8(_mm256_set1_ps(Dx), sX, _mm256_set1_ps(DVk));

                        // sphereC / cq / the three pre-scaled broadcast
                        // constants below are per-SPOT invariants, lifted
                        // to the per-tile ConeSpotPre record — the same
                        // values, computed once (see the struct's note).
                        const float  sphereC = P_.sphereC;
                        const __m256 sSphD   = fma_x8(sVP, sVP,
                            _mm256_mul_ps(_mm256_set1_ps(-sphereC), sUV));
                        // `if (sphereDisc < 0) continue` — NLT_UQ is the
                        // exact negation (a NaN keeps the lane, as scalar).
                        mAlive = _mm256_and_ps(mAlive,
                            _mm256_cmp_ps(sSphD, sZero, _CMP_NLT_UQ));
                        // DIAG-only: how many (batch x spot) pairs lose ALL
                        // eight lanes at the range sphere? That is the ceiling
                        // on any cull placed before the solve.
                        if (g_coneDiag && _mm256_movemask_ps(mAlive) == 0)
                            g_dSphDead.fetch_add(1, std::memory_order_relaxed);
#if FDS_CONE_SOLVE_EARLYOUT
                        // Range-sphere early-out — MEASURED AND NOT KEPT
                        // (kept compiled-out as the record of the test).
                        // The reasoning was that 39.9% of (8px-batch ×
                        // spot) pairs have zero alive lanes (a16567b) and
                        // the scalar arm gets to `continue` out of them at
                        // this exact point, before the divides. It does not
                        // pay: matched three-arm A/B, city t=1961, cones
                        // Ginstr/f 2.884 with this branch against 2.874
                        // without it. The premise is what is wrong — a pair
                        // is only skipped when ALL EIGHT lanes miss the
                        // range sphere, and most dead pairs lose their
                        // lanes later (a-sign, or zHi<=zLo in the tail), so
                        // the branch fires too rarely to pay for itself.
                        // Byte-null either way: when no lane survives, all
                        // the outputs below are masked to the zero the
                        // arrays already hold.
                        if (_mm256_movemask_ps(mAlive) == 0) {
                            if (g_coneDiag)
                                g_dDead.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
#endif

                        const __m256 sA  = fma_x8(sDV, sDV,
                            _mm256_mul_ps(_mm256_set1_ps(-c2), sUV));
                        // b = 2·(c2·VP − DP·DV). Scaling by two is exact in
                        // IEEE, so pushing it into both broadcasts rounds the
                        // identical real product once and deletes the doubling
                        // add — the same argument round 4b used for cq·(a·−4).
                        // Bit-exact by construction.
                        const __m256 sB  = fma_x8(_mm256_set1_ps(P_.c2x2), sVP,
                            _mm256_mul_ps(_mm256_set1_ps(P_.negDPx2), sDV));
                        // fl(cq * fl(a*-4)) == fl(fl(cq*-4) * a): BOTH inner
                        // products are exact (scaling by a power of two), so
                        // either spelling rounds the same real product once.
                        // Folding it into the scalar deletes a whole __m256 mul.
                        const __m256 sDisc = fma_x8(sB, sB,
                            _mm256_mul_ps(_mm256_set1_ps(P_.cqm4), sA));

                        const __m256 sSq    = SQRTV(sDisc);
                        const __m256 sInv2a = RCP(_mm256_add_ps(sA, sA));
                        const __m256 sR1 = _mm256_mul_ps(sInv2a,
                            _mm256_sub_ps(NEG(sB), sSq));
                        const __m256 sR2 = _mm256_mul_ps(sInv2a,
                            _mm256_sub_ps(sSq, sB));
                        // std::min/std::max transcribed as cmp+select so
                        // the NaN/±0 tie-breaks match fcsel rather than
                        // NEON's FMIN (which resolves NaN the other way).
                        const __m256 rMin = MINT(sR1, sR2);
                        const __m256 rMax = MAXT(sR1, sR2);

                        const __m256 mDisc = _mm256_cmp_ps(sDisc, sZero, _CMP_NLT_UQ);
                        const __m256 mANeg = _mm256_cmp_ps(sA,
                            _mm256_set1_ps(-1e-8f), _CMP_LT_OQ);
                        const __m256 mAPos = _mm256_cmp_ps(sA,
                            _mm256_set1_ps(1e-8f), _CMP_GT_OQ);

                        // a > +1e-8: forward/backward half-space pick,
                        // and the disc<0 case that opens the whole ray.
                        const __m256 zLoHit = MAXT(rMax, sZMin);
                        const __m256 zHiHit = MINT(rMin, sZMax);
                        const __m256 mFwd = _mm256_cmp_ps(sDV,
                            _mm256_set1_ps(1e-6f), _CMP_GT_OQ);
                        // |DV| > 1e-6 IS (DV > 1e-6) | (DV < -1e-6), for every
                        // input including NaN (all three compares false) and
                        // +-0 -- so the backward compare and the or below are
                        // the same mask the apex-plane cut needs later, and one
                        // fabs+cmp replaces two cmps and an or.
                        const __m256 mDVBig = _mm256_cmp_ps(
                            _mm256_andnot_ps(_mm256_set1_ps(-0.0f), sDV),
                            _mm256_set1_ps(1e-6f), _CMP_GT_OQ);
                        const __m256 zLoRt = _mm256_blendv_ps(sZMin, zLoHit, mFwd);
                        const __m256 zHiRt = _mm256_blendv_ps(zHiHit, sZMax, mFwd);
                        const __m256 mPosOk = _mm256_or_ps(NOT(mDisc),
                            _mm256_and_ps(mDVBig,
                                _mm256_cmp_ps(zHiRt, zLoRt, _CMP_GT_OQ)));
                        const __m256 zLoP = _mm256_blendv_ps(sZMin, zLoRt, mDisc);
                        const __m256 zHiP = _mm256_blendv_ps(sZMax, zHiRt, mDisc);

                        __m256 zLo = _mm256_blendv_ps(zLoP, rMin, mANeg);
                        __m256 zHi = _mm256_blendv_ps(zHiP, rMax, mANeg);
                        mAlive = _mm256_and_ps(mAlive,
                            _mm256_or_ps(_mm256_and_ps(mANeg, mDisc),
                                         _mm256_and_ps(mAPos, mPosOk)));
                        // DIAG-only: pairs already fully dead HERE. Round 1's
                        // early-out sat 30 lines earlier, at the range sphere,
                        // and fired on 7.7% of city's pairs; chase's sphere
                        // rejects 0.0%, so the question is what this later cut
                        // catches instead.
                        if (g_coneDiag && _mm256_movemask_ps(mAlive) == 0)
                            g_dQuadDead.fetch_add(1, std::memory_order_relaxed);
#if FDS_CONE_QUADEARLYOUT
                        if (_mm256_movemask_ps(mAlive) == 0) {
                            if (g_coneDiag)
                                g_dDead.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
#endif

                        // Range-sphere clamp + the shared tail.
                        const __m256 sSphSq = SQRTV(sSphD);
                        const __m256 sInvUV = RCP(sUV);
                        const __m256 zSphLo = _mm256_mul_ps(sInvUV,
                            _mm256_sub_ps(sVP, sSphSq));
                        const __m256 zSphHi = _mm256_mul_ps(sInvUV,
                            _mm256_add_ps(sVP, sSphSq));
                        zLo = MAXT(zLo, zSphLo);
                        zHi = MINT(zHi, zSphHi);
                        zLo = MAXT(zLo, sZMin);
                        mAlive = _mm256_and_ps(mAlive,
                            _mm256_cmp_ps(zHi, zLo, _CMP_GT_OQ));
                        mAlive = _mm256_and_ps(mAlive,
                            _mm256_cmp_ps(zLo, sZMax, _CMP_LT_OQ));
                        zHi = MINT(zHi, sZMax);

                        // Forward-cone cut at the apex plane z = DP/DV.
#if FDS_CONE_SOLVE_APPROX
                        const __m256 zFwd = _mm256_mul_ps(
                            _mm256_set1_ps(DP), _mm256_rcp_ps(sDV));
#else
                        const __m256 zFwd = _mm256_div_ps(
                            _mm256_set1_ps(DP), sDV);
#endif
                        const __m256 mDVPos = _mm256_cmp_ps(sDV, sZero, _CMP_GT_OQ);
                        zLo = _mm256_blendv_ps(zLo, zFwd,
                            _mm256_and_ps(_mm256_and_ps(mDVBig, mDVPos),
                                _mm256_cmp_ps(zLo, zFwd, _CMP_LT_OQ)));
                        zHi = _mm256_blendv_ps(zHi, zFwd,
                            _mm256_and_ps(_mm256_andnot_ps(mDVPos, mDVBig),
                                _mm256_cmp_ps(zHi, zFwd, _CMP_GT_OQ)));
                        mAlive = _mm256_and_ps(mAlive,
                            _mm256_or_ps(NOT(mDVBig),
                                _mm256_cmp_ps(zLo, zHi, _CMP_LT_OQ)));

                        _mm256_store_ps(zLoArr,    _mm256_and_ps(zLo, mAlive));
                        _mm256_store_ps(zHiArr,    _mm256_and_ps(zHi, mAlive));
                        _mm256_store_ps(aliveLane, _mm256_and_ps(sOne, mAlive));
                        const int aliveBits = _mm256_movemask_ps(mAlive);
                        spotAlive = aliveBits != 0;
                        if (g_coneDiag) {
                            g_dLanes.fetch_add(FDS_POPCOUNT(unsigned(aliveBits)),
                                               std::memory_order_relaxed);
                            if (!spotAlive)
                                g_dDead.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
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
                        if (g_coneDiag) g_dLanes.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (g_coneDiag && !spotAlive)
                        g_dDead.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!spotAlive) continue;
                    // ablation: keep the 8-wide cone-interval solve.
                    CONE_ABL_CUT(3, _mm256_add_ps(_mm256_load_ps(zLoArr),
                                    _mm256_add_ps(_mm256_load_ps(zHiArr),
                                                  _mm256_load_ps(aliveLane))));

                    // Clone AND bounce spots have no own map (smIdx=-1)
                    // but carry the SOURCE's map: visibility of a sample
                    // equals the source's visibility of the sample
                    // reflected across the mirror plane (same identity as
                    // the surface kernel's mirrored tap). The reflection is
                    // applied in VIEW space: v' = v − 2(n_v·v + d_v)n_v
                    // with n_v = viewMatᵀ·N_world, d_v = N·camW + D.
                    // Bounce spots are mirrorId=0 (they light the real
                    // room, not a reflected RTT), so gate on the source-map
                    // index alone — it is only ever set for clones/bounces.
                    int32_t smIdx = lights->shadowMapIdx[li];
                    bool  smMirror = false;
                    float nvx=0, nvy=0, nvz=0, dv_pl=0;
                    if (smIdx < 0 &&
                        lights->srcShadowMapIdx[li] >= 0) {
                        smIdx = lights->srcShadowMapIdx[li];
                        smMirror = true;
                        const float Nx = lights->mirNX[li];
                        const float Ny = lights->mirNY[li];
                        const float Nz = lights->mirNZ[li];
                        const auto &VW = dc.viewToWorld;
                        nvx = VW[0][0]*Nx + VW[1][0]*Ny + VW[2][0]*Nz;
                        nvy = VW[0][1]*Nx + VW[1][1]*Ny + VW[2][1]*Nz;
                        nvz = VW[0][2]*Nx + VW[1][2]*Ny + VW[2][2]*Nz;
                        dv_pl = Nx*dc.cameraWorldX +
                                Ny*dc.cameraWorldY +
                                Nz*dc.cameraWorldZ +
                                lights->mirD[li];
                    }
                    const ShadowMap *sm = (smIdx >= 0 && size_t(smIdx) < g_shadowMaps.size())
                                          ? &g_shadowMaps[smIdx] : nullptr;
                    float sm_m00=0, sm_m01=0, sm_m02=0, sm_ox=0;
                    float sm_m10=0, sm_m11=0, sm_m12=0, sm_oy=0;
                    float sm_m20=0, sm_m21=0, sm_m22=0, sm_oz=0;
                    float sm_cntrX=0, sm_cntrY=0, sm_perspX=0, sm_perspY=0;
                    float sm_zScale=0;
                    const uint32_t *sm_pack = nullptr;
                    int sm_xres=0, sm_yres=0;
                    if (sm) {
                        sm_m00=sm->viewToLight[0][0]; sm_m01=sm->viewToLight[0][1]; sm_m02=sm->viewToLight[0][2]; sm_ox=sm->viewToLightOffset.x;
                        sm_m10=sm->viewToLight[1][0]; sm_m11=sm->viewToLight[1][1]; sm_m12=sm->viewToLight[1][2]; sm_oy=sm->viewToLightOffset.y;
                        sm_m20=sm->viewToLight[2][0]; sm_m21=sm->viewToLight[2][1]; sm_m22=sm->viewToLight[2][2]; sm_oz=sm->viewToLightOffset.z;
                        sm_cntrX=sm->cntrX; sm_cntrY=sm->cntrY;
                        sm_perspX=sm->perspX; sm_perspY=sm->perspY;
                        sm_zScale=sm->zScale;
                        sm_pack=sm->packSD.data();
                        sm_xres=sm->xres; sm_yres=sm->yres;
                    }

                    alignas(32) float dzArr[8] = {}, invDzArr[8] = {}, fadeStartArr[8] = {};
                    // ─── dz / surface-fade window, 8 lanes at a time ──
                    // The scalar loop below is 0.270 Ginstr/f of this
                    // pass — 9.4% — measured by the round-2 ablation
                    // ladder (FDS_CONE_ABLATE). It is the SAME shape of
                    // defect the 8-wide solve fixed in 7e34645: a scalar
                    // per-lane loop sandwiched between an 8-wide producer
                    // (the solve, which just wrote zLoArr/zHiArr) and an
                    // 8-wide consumer (the integration body, which loads
                    // dzArr/invDzArr/fadeStartArr straight back as
                    // __m256). Every lane therefore pays a store and a
                    // reload it never needed: the disassembly of the
                    // shipping kernel is 894 ldr + 583 str out of 4475
                    // instructions (33%), and this loop is one of the two
                    // places that traffic comes from.
                    // BIT-EXACT, and cheaply so — there is no fma in it,
                    // so no contraction map to reproduce. The only two
                    // spellings that matter: std::max(d, fwMin) is
                    // (d < fwMin) ? fwMin : d transcribed as cmp+blend,
                    // NOT _mm256_max_ps (NEON FMAX resolves NaN and -0
                    // the opposite way from the scalar FCSEL — the trap
                    // 7e34645 documents); and `if (alive == 0) continue`
                    // negates to the UNORDERED _CMP_NEQ_UQ. 12.0f*invZScale
                    // is loop-invariant, so hoisting it is the identical
                    // product, not a re-association.
                    if (laneVec) {
                        const __m256 vZeroL  = _mm256_setzero_ps();
                        const __m256 mLive   = _mm256_cmp_ps(
                                               _mm256_load_ps(aliveLane),
                                               vZeroL, _CMP_NEQ_UQ);
                        const __m256 vD      = _mm256_mul_ps(
                                               _mm256_sub_ps(_mm256_load_ps(zHiArr),
                                                             _mm256_load_ps(zLoArr)),
                                               _mm256_set1_ps(inv_nSamp));
                        const __m256 vFwMin  = _mm256_set1_ps(12.0f * invZScale);
                        const __m256 vFadeW  = MAXT(vD, vFwMin);
                        const __m256 vInvFw  = _mm256_div_ps(_mm256_set1_ps(1.0f), vFadeW);
                        const __m256 vFadeS  = _mm256_sub_ps(_mm256_load_ps(zMaxArr), vFadeW);
                        _mm256_store_ps(dzArr,        _mm256_blendv_ps(vZeroL, vD,     mLive));
                        _mm256_store_ps(invDzArr,     _mm256_blendv_ps(vZeroL, vInvFw, mLive));
                        _mm256_store_ps(fadeStartArr, _mm256_blendv_ps(vZeroL, vFadeS, mLive));
                    } else {
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
                    }

                    // ablation: keep the scalar per-lane dz/fade loop.
                    CONE_ABL_CUT(4, _mm256_add_ps(_mm256_load_ps(dzArr),
                                    _mm256_add_ps(_mm256_load_ps(invDzArr),
                                                  _mm256_load_ps(fadeStartArr))));

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
#if FDS_CONE_HOTONLY
                    if (true) {
#else
                    if (useAnalytic) {
#endif
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
                        // The step is one FRSQRTS (see rsqrt_step_x8).
                        __m256 vInvD          = _mm256_rsqrt_ps(vSafeDisc);
                        vInvD = rsqrt_step_x8(vSafeDisc, vInvD);

                        const __m256 vTwoA    = _mm256_add_ps(vAlpha, vAlpha);
                        const __m256 vZHi_v   = _mm256_load_ps(zHiArr);
                        const __m256 vArgHi   = _mm256_mul_ps(vInvD,
                                                _mm256_fmadd_ps(vTwoA, vZHi_v, vBeta));
                        const __m256 vArgLo   = _mm256_mul_ps(vInvD,
                                                _mm256_fmadd_ps(vTwoA, vZLo_v, vBeta));
                        // ablation: keep through vArgHi/vArgLo (pre-atan).
                        CONE_ABL_CUT(6, _mm256_add_ps(vInvD,
                                        _mm256_add_ps(vArgHi, vArgLo)));
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
#if FDS_CONE_SEG_CLOSEDFORM
                        // ROUND 7'S SECOND PROBE — MEASURED AND NOT KEPT
                        // (compiled out in place as the record of the test).
                        // W = z·V − P with V = (X, Y, 1), so both dot products
                        // the smoothstep needs are quadratics in z whose
                        // coefficients the solve already produced:
                        //   W·W = z²(V·V) − 2z(V·P) + P·P = z(z·uV − 2VP) + PP
                        //   D·W = z(D·V) − D·P            = z·DV − DP
                        // 3 vector ops per segment against the 11 the explicit
                        // W spelling costs — 64 __m256 ops saved per alive pair
                        // for 4 of setup, which is what §14.7 parked.
                        // MEASURED, two binaries in one worktree, interleaved
                        // min-of-6 with round 0 discarded, load 5.4–9.7:
                        //   cones Ginstr/f  chase t800 1.993 → 1.995 (+0.1 %)
                        //                   chase t400 2.690 → 2.696 (+0.2 %)
                        //                   greets t1588 0.952 → 0.959 (+0.7 %)
                        //   cones Gcyc/f    −1.7 % / −1.6 % / −2.8 %
                        // The INSTRUCTION column is a small LOSS on all three,
                        // and the arithmetic says why: the segment loop runs
                        // only on ALIVE pairs, 8.1 % of chase's at t=800, so
                        // the whole block is ~0.9 % of the pass GROSS before
                        // the replacement's own cost. What is left is a
                        // shorter dependency chain (IPC 4.30 → 4.39), worth
                        // 1.6–2.8 % of cycles — under §14.7's 2 % bar, and it
                        // is a RE-ASSOCIATION, so buying it means moving
                        // bytes. Not worth a judge call at that size.
                        const __m256 vDVc_v = _mm256_fmadd_ps(vX_v, vDx_v,
                                              _mm256_fmadd_ps(vY_v, vDy_v, vDz_dir_v));
                        const __m256 vN2VP_v = _mm256_mul_ps(vVP_v,
                                               _mm256_set1_ps(-2.0f));
                        const __m256 vNDP_v  = _mm256_set1_ps(-DP);
#endif
                        // coneAtten (smoothstep cosO→cosI) at a given z.
                        auto coneAttenAt = [&](const __m256 &z) -> __m256 {
#if FDS_CONE_SEG_CLOSEDFORM
                            const __m256 W2 = _mm256_fmadd_ps(z,
                                              _mm256_fmadd_ps(z, vUv_v, vN2VP_v),
                                              vPP_v);
                            const __m256 DW = _mm256_fmadd_ps(z, vDVc_v, vNDP_v);
#else
                            const __m256 Wx = _mm256_sub_ps(_mm256_mul_ps(z, vX_v), vPx_v);
                            const __m256 Wy = _mm256_sub_ps(_mm256_mul_ps(z, vY_v), vPy_v);
                            const __m256 Wz = _mm256_sub_ps(z, vPz_v);
                            const __m256 W2 = _mm256_fmadd_ps(Wx, Wx,
                                              _mm256_fmadd_ps(Wy, Wy,
                                               _mm256_mul_ps(Wz, Wz)));
                            const __m256 DW = _mm256_fmadd_ps(vDx_v, Wx,
                                              _mm256_fmadd_ps(vDy_v, Wy,
                                               _mm256_mul_ps(vDz_dir_v, Wz)));
#endif
                            const __m256 safeW2 = VMAX(W2, _mm256_set1_ps(1e-12f));
                            const __m256 cosT = _mm256_mul_ps(DW, rsqrt_nr_x8(safeW2));
                            __m256 t = _mm256_mul_ps(_mm256_sub_ps(cosT, vCosO_v), vInvCIO_v);
                            t = VMAX(vZero_v, VMIN(vOne_v, t));
                            const __m256 sm = _mm256_mul_ps(_mm256_mul_ps(t, t),
                                              _mm256_sub_ps(vThree_v, _mm256_mul_ps(vTwo_v, t)));
                            const __m256 mIn = _mm256_cmp_ps(cosT, vCosI_v, _CMP_GE_OQ);
                            return _mm256_blendv_ps(sm, vOne_v, mIn);
                        };
                        __m256 vIntegral;
#if FDS_CONE_HOTONLY
                        if (true) {
#else
                        if (!segPath) {
#endif
                            // Wide cones: single closed form. coneAtten
                            // is applied at the midpoint further below.
                            vIntegral = _mm256_mul_ps(
                                        _mm256_add_ps(vInvD, vInvD),
                                        atanDiff(vArgHi, vArgLo));
#if FDS_CONE_HOTONLY
                        } else if (false) {
#else
                        } else {
#endif
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
                                sf = VMAX(vZero_v,
                                     VMIN(vOne_v, sf));
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
                                            ShadowTexZ(sm->packSD[o2]),
                                            ShadowTexZ(sm->packDyn[o2]));
                                        int pixZ = 0xFF80 - int(lz * sm_zScale);
                                        if (pixZ < 0) pixZ = 0;
                                        if (pixZ + 128 < int(zS)) shA[ln] = 0.0f;
                                    }
                                    shv = _mm256_load_ps(shA);
                                }
                                // Per-segment turbulent density tap —
                                // 8-wide across lanes (a scalar-per-lane
                                // version cost +35 ms/frame on greets).
                                // Dead lanes compute on zm≈0 — finite,
                                // masked by mAlive downstream. Branch
                                // (not a ×1.0 blend): the off path must
                                // keep the EXACT legacy expression —
                                // simde maps these intrinsics to plain
                                // vector ops on arm64, and reshaping the
                                // chain shifts fma contraction (flipped
                                // the greets pin by a few ulps).
                                if (turb.on) {
                                    const __m256 tbv = ConeTurbFactor_x8(
                                        turb,
                                        _mm256_mul_ps(zm, vX_v),
                                        _mm256_mul_ps(zm, vY_v), zm,
                                        Px, Py_l, Pz, Dx, Dy, Dz);
                                    vSum = _mm256_fmadd_ps(atanDiff(uk, uPrev),
                                           _mm256_mul_ps(_mm256_mul_ps(coneAttenAt(zm), sf),
                                                         _mm256_mul_ps(shv, tbv)), vSum);
                                } else {
                                    vSum = _mm256_fmadd_ps(atanDiff(uk, uPrev),
                                           _mm256_mul_ps(_mm256_mul_ps(coneAttenAt(zm), sf),
                                                         shv), vSum);
                                }
                                uPrev = uk;
                            }
                            vIntegral = _mm256_mul_ps(
                                        _mm256_add_ps(vInvD, vInvD), vSum);
                        }

                        // ablation: keep through vIntegral (atanDiff done).
                        CONE_ABL_CUT(7, vIntegral);
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
                        softEdge_m = VMAX(vZero_v, softEdge_m);
                        softEdge_m = _mm256_mul_ps(softEdge_m, softEdge_m);
                        // coneAtten at midpoint: smoothstep(cosO→cosI).
                        __m256 t_m = _mm256_mul_ps(_mm256_sub_ps(cosT_m, vCosO_v), vInvCIO_v);
                        t_m = VMAX(vZero_v, VMIN(vOne_v, t_m));
                        const __m256 smooth_m = _mm256_mul_ps(
                                                _mm256_mul_ps(t_m, t_m),
                                                _mm256_sub_ps(vThree_v,
                                                  _mm256_mul_ps(vTwo_v, t_m)));
                        const __m256 mInner_m = _mm256_cmp_ps(cosT_m, vCosI_v, _CMP_GE_OQ);
                        // Segmented cones already folded coneAtten in per
                        // segment — don't apply the midpoint one again.
                        const __m256 coneAtten_m = segPath
                            ? vOne_v
                            : _mm256_blendv_ps(smooth_m, vOne_v, mInner_m);
                        // surfaceFade at midpoint.
                        const __m256 mFade_m  = _mm256_cmp_ps(vZMid, vFadeStart_v, _CMP_GT_OQ);
                        const __m256 fadeVal_m = _mm256_mul_ps(
                                                 _mm256_sub_ps(vZMax_v, vZMid), vInvDz_v);
                        // Segmented cones folded the fade per segment.
                        const __m256 surfaceFade_m = segPath
                            ? vOne_v
                            : _mm256_blendv_ps(vOne_v, fadeVal_m, mFade_m);
                        // ablation: keep through the midpoint block.
                        CONE_ABL_CUT(8, _mm256_add_ps(coneAtten_m,
                                        _mm256_add_ps(surfaceFade_m, softEdge_m)));

                        // Match ray-march brightness scaling: N × mean.
                        const __m256 vIntervalLen = _mm256_sub_ps(vZHi_v, vZLo_v);
                        const __m256 vSafeLen  = _mm256_blendv_ps(vOne_v, vIntervalLen, mAlive);
                        const __m256 vN        = _mm256_set1_ps(float(N_SAMPLES));
                        // mean per lane: integral / interval; final
                        // contribution per "sample-unit": mean × N ×
                        // coneAtten_mid × surfaceFade_mid.
                        // rcp refined for the same reason as invD.
                        // The step is one FRECPS (see rcp_step_x8).
                        __m256 vRcpLen = _mm256_rcp_ps(vSafeLen);
                        vRcpLen = rcp_step_x8(vSafeLen, vRcpLen);
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
                            fog_m = VMAX(vZero_v, fog_m);
                            fog_m = _mm256_mul_ps(fog_m, fog_m);
                            vAcc = _mm256_mul_ps(vAcc, fog_m);
                        }

                        // Per-pixel multiplicative noise: replicates the
                        // visual texture of the ray-march path (whose
                        // stochastic sample offsets produce inter-pixel
                        // variation) without sacrificing the analytic
                        // smoothness. Hash from existing pxHashArr.
                        // ablation: keep through vAcc incl. fog (pre-noise).
                        CONE_ABL_CUT(9, vAcc);
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
                        // ablation: keep through the noise multiply.
                        CONE_ABL_CUT(10, vAcc);
                        const __m256 mAng = _mm256_cmp_ps(cosT_m, vCosO_v, _CMP_GE_OQ);
                        __m256 m          = _mm256_and_ps(mAlive,
                                            _mm256_and_ps(mDisc, mAng));

                        // Midpoint shadow tap — one sample at z=zMid
                        // gates the whole segment. Stair-steps at
                        // shadow boundaries; tolerated because halos
                        // are diffuse. Mirrors the in-loop sm path
                        // above but uses zMid instead of per-sample z.
                        if (!FDS_CONE_HOTONLY && sm && !segPath) {
                            // (segmented cones fold shadow per segment
                            // in the hybrid above)
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
                                const uint16_t shadowZ = ShadowTexZ(
                                    sm_pack[size_t(iY) * size_t(sm_xres) + size_t(iX)]);
                                if (biased < int(shadowZ)) shadowMul_s[lane] = 0.0f;
                            }
                            const __m256 vShad_s = _mm256_load_ps(shadowMul_s);
                            m = _mm256_and_ps(m,
                                _mm256_cmp_ps(vShad_s, _mm256_set1_ps(0.5f), _CMP_GT_OQ));
                        }

                        accV = _mm256_and_ps(vAcc, m);
#if FDS_CONE_HOTONLY
                    } else if (false) {
#else
                    } else {
#endif
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
                        t_v = VMAX(vZero_v, VMIN(vOne_v, t_v));
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
                            fogAtten = VMAX(vZero_v, fogAtten);
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
                                const uint16_t shadowZ = ShadowTexZ(
                                    sm_pack[size_t(iY) * size_t(sm_xres) + size_t(iX)]);
                                if (biased < int(shadowZ)) shadowMul[lane] = 0.0f;
                            }
                            const __m256 vShad = _mm256_load_ps(shadowMul);
                            mask = _mm256_and_ps(mask,
                                _mm256_cmp_ps(vShad, _mm256_set1_ps(0.5f), _CMP_GT_OQ));
                        }

                        __m256 contrib = _mm256_mul_ps(
                            _mm256_mul_ps(coneAtten, distAtten),
                            _mm256_mul_ps(fogAtten, surfaceFade));
                        // Per-sample turbulent density — 8-wide across
                        // lanes (masked-out lanes compute on harmless
                        // finite values; the and-mask drops them).
                        if (turb.on) {
                            contrib = _mm256_mul_ps(contrib,
                                ConeTurbFactor_x8(turb,
                                    _mm256_mul_ps(vZ, vX_v),
                                    _mm256_mul_ps(vZ, vY_v), vZ,
                                    Px, Py_l, Pz, Dx, Dy, Dz));
                        }
                        contrib = _mm256_and_ps(contrib, mask);
                        accV = _mm256_add_ps(accV, contrib);
                    }
                    }
                    // ablation: keep the broadcasts + the integration body,
                    // cut before the per-lane colour accumulate.
                    CONE_ABL_CUT(5, accV);

                    alignas(32) float accArr[8];
                    if (!laneVec) _mm256_store_ps(accArr, accV);
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
                    // Per-spot beam gain (authored VolumetricLightIntensity;
                    // unset → 1.0, exact ×1.0 keeps legacy content bit-
                    // identical). Applied as a trailing factor so gain-free
                    // lights evaluate the identical expression as before.
                    const float coneGain = lights->coneGain[li];
                    // ─── colour accumulate, 8 lanes at a time ─────────
                    // The scalar loop below is 0.301 Ginstr/f — 10.5% of
                    // the pass (round-2 ablation ladder). It is the OTHER
                    // end of the same defect as the dz/fade loop: accV is
                    // already an __m256 and gets spilled to accArr purely
                    // so eight scalar iterations can read it back, each
                    // then doing a load-modify-store on three more stack
                    // arrays — 8x(1+3 loads + 3 stores) per (batch x spot)
                    // where three register FMAs would do. Holding the
                    // accumulators in __m256 across the spot loop deletes
                    // that traffic entirely; they are drained to
                    // accB/accG/accR once per BATCH, so the composite loop
                    // that follows never learns the difference.
                    // BIT-EXACT. Two things to get right and both are in
                    // 7e34645's list: `if (acc <= 0) continue` negates to
                    // the UNORDERED _CMP_NLE_UQ (an ordered predicate
                    // would silently drop a NaN lane the scalar arm
                    // propagates); and the multiply ORDER is preserved
                    // exactly as ((acc*density)*nNorm)*coneGain rather
                    // than folded into one scalar factor, which would be a
                    // re-association and would move the pin. The trailing
                    // `accB += w*colB` is spelled as an explicit FMA
                    // because that is what the compiler contracted it to
                    // under the tree-wide -ffp-contract=fast (verified
                    // against the city pin, which does not move).
                    if (laneVec) {
                        const __m256 mPos = _mm256_cmp_ps(accV, _mm256_setzero_ps(),
                                                          _CMP_NLE_UQ);
                        __m256 w = _mm256_mul_ps(accV, vDensity_v);
                        w = _mm256_mul_ps(w, _mm256_set1_ps(nNorm));
                        w = _mm256_mul_ps(w, _mm256_set1_ps(coneGain));
                        w = _mm256_and_ps(w, mPos);
                        vAccB = _mm256_fmadd_ps(w, _mm256_set1_ps(colB), vAccB);
                        vAccG = _mm256_fmadd_ps(w, _mm256_set1_ps(colG), vAccG);
                        vAccR = _mm256_fmadd_ps(w, _mm256_set1_ps(colR), vAccR);
                    } else {
                    for (int lane = 0; lane < 8; ++lane) {
                        if (accArr[lane] <= 0.0f) continue;
                        const float w = accArr[lane] * density * nNorm * coneGain;
                        accB[lane] += w * colB;
                        accG[lane] += w * colG;
                        accR[lane] += w * colR;
                    }
                    }
                }
                if (laneVec) {
                    _mm256_store_ps(accB, vAccB);
                    _mm256_store_ps(accG, vAccG);
                    _mm256_store_ps(accR, vAccR);
                }

                for (int lane = 0; lane < laneCount; ++lane) {
                    if (accB[lane] <= 0.0f && accG[lane] <= 0.0f && accR[lane] <= 0.0f) continue;
                    const int px = pxBase + lane;
                    const size_t i = row + size_t(px);
                    VolCompositeAdd(out, i, accB[lane], accG[lane], accR[lane]);
                    if (dupRow)
                        VolCompositeAdd(out, i + size_t(XRes), accB[lane], accG[lane], accR[lane]);
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
                    const auto &VW = dc.viewToWorld;
                    hsNx_s = VW[0][0]*Nx + VW[1][0]*Ny + VW[2][0]*Nz;
                    hsNy_s = VW[0][1]*Nx + VW[1][1]*Ny + VW[2][1]*Nz;
                    hsNz_s = VW[0][2]*Nx + VW[1][2]*Ny + VW[2][2]*Nz;
                    hsD_s  = Nx*dc.cameraWorldX +
                             Ny*dc.cameraWorldY +
                             Nz*dc.cameraWorldZ +
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
                const uint32_t *sm_pack = nullptr;
                int sm_xres=0, sm_yres=0;
                if (sm) {
                    sm_m00=sm->viewToLight[0][0]; sm_m01=sm->viewToLight[0][1]; sm_m02=sm->viewToLight[0][2]; sm_ox=sm->viewToLightOffset.x;
                    sm_m10=sm->viewToLight[1][0]; sm_m11=sm->viewToLight[1][1]; sm_m12=sm->viewToLight[1][2]; sm_oy=sm->viewToLightOffset.y;
                    sm_m20=sm->viewToLight[2][0]; sm_m21=sm->viewToLight[2][1]; sm_m22=sm->viewToLight[2][2]; sm_oz=sm->viewToLightOffset.z;
                    sm_cntrX=sm->cntrX; sm_cntrY=sm->cntrY;
                    sm_perspX=sm->perspX; sm_perspY=sm->perspY;
                    sm_zScale=sm->zScale;
                    sm_pack=sm->packSD.data();
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
                                const uint16_t shadowZ = ShadowTexZ(
                                    sm_pack[size_t(iY) * size_t(sm_xres) + size_t(iX)]);
                                if (biased < int(shadowZ)) continue;  // shadowed
                            }
                        }
                    }
                    // Turbulent density modulation (world-space noise ×
                    // optional axis swirl) — scalar path.
                    float turbF = 1.0f;
                    if (turb.on)
                        turbF = ConeTurbFactor(turb, z*X, z*Y, z,
                                               Px, Py, Pz, Dx, Dy, Dz);
                    acc += coneAtten * distAtten * fogAtten * surfaceFade * turbF;
                }
                if (acc <= 0.0f) continue;
                // No dz scaling — the path-integral form (acc × dz) gave
                // shallow-angle rays through far cones much brighter results
                // than close cones (where each pixel's ray-cone segment is
                // short). Using per-sample-sum (acc only) combined with the
                // inverse-square distAtten above gives roughly position-
                // invariant brightness, biased toward close cones — matches
                // the "flashlight in fog" mental model.
                // Trailing per-spot beam gain: unset → 1.0 (exact), same as
                // the vec path.
                const float w = acc * density * lights->coneGain[li];
                accB += w * lights->colB[li];
                accG += w * lights->colG[li];
                accR += w * lights->colR[li];
            }
            if (accB <= 0.0f && accG <= 0.0f && accR <= 0.0f) continue;
            const size_t i = row + size_t(px);
            VolCompositeAdd(out, i, accB, accG, accR);
        }
        }
    }
#if FDS_CONE_ABLATE
    // Drain the ablation sink once per TILE CALL: keeps the retained work
    // alive against DCE without a per-batch store (which would false-share
    // across the 12 workers and pollute the cycle column).
    {
        alignas(32) float t[8];
        _mm256_store_ps(t, ablSinkV);
        g_ablSink = t[0]+t[1]+t[2]+t[3]+t[4]+t[5]+t[6]+t[7];
    }
#endif
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

void Render_VolumetricCones(const DeferredLightingCtx &ctx, bool inlineDispatch) {
    VolProfScope _vp(&g_volProf.ms_cones, &g_volProf.n_cones);
    // Render-target addressing from the threaded ctx (the cone tile already
    // reads ctx). For the main frame ctx.xres==XRes etc. (Render_DeferredLighting
    // populated it), so this is byte-identical; for an offscreen shard bake
    // (inlineDispatch) ctx points at the worker's 64² target so the disco beams
    // land in the reflection. inlineDispatch runs the tiles on the calling
    // worker thread (no pool enqueue, no tileDone traffic).
    if (!CurScene || !ctx.zpage16 || !ctx.vpage) return;
    const int XRes = ctx.xres;
    const int YRes = ctx.yres;
    const bool allCones = fds::FeatureFlags::draw_cones();
    const float invFOVX = ctx.invFOVX;
    const float invFOVY = ctx.invFOVY;
    const float invZScale = ctx.invZScale;
    // Density: per-step contribution coefficient. Tunable via existing
    // FDS_CONE_STRENGTH. Empirical: 0.0005-0.002 for City-scale (range
    // in thousands).
    float density = fds::FeatureFlags::cone_strength() * 0.001f;
    // HDR: flat-scale by hdr_glow_scale (no cap), UNLESS --hdr-cone-softknee does
    // the roll-off at the VolCompositeAdd HDR composite instead (then pass raw).
    if (fds::FeatureFlags::hdr() && !fds::FeatureFlags::hdr_cone_softknee())
        density *= fds::FeatureFlags::hdr_glow_scale();  // glowMax cap off in HDR
    // Zero density = zero contribution: skip the whole per-pixel pass
    // (it previously ran the full integration and multiplied by 0 at
    // the end — --cone-strength=0 benched identical to default).
    if (density <= 0.0f) return;

    // Fog cutoff + per-sample attenuation. Matches Render_DeferredFogPass:
    // cones fade by (1 - z/FZP) so they don't extend past where geometry
    // already fully fogged out. fogZ <= 0 disables (unfogged scenes).
    const float fogZ    = (CurScene->Flags & Scn_Fogged) ? CurScene->FZP : 0.0f;
    const float invFogZ = (fogZ > 0.0f) ? 1.0f / fogZ : 0.0f;

    // Cone turbulence: all per-frame constants derived ONCE here on the
    // dispatching thread (g_FrameTime is the deterministic scene clock —
    // fixed-t snapshots repeat exactly; tiles get a const ref, no shared
    // mutable state). Local because concurrent shard bakes call this
    // reentrantly with their own ctx (viewToWorld differs per shard).
    ConeTurb turb;
    {
        const float amp = fds::FeatureFlags::cone_turbulence();
        if (amp > 0.0f) {
            turb.on  = true;
            // 2-octave value noise concentrates near 0.5, so the raw
            // flag reads weak; ×2.2 makes flag=1 span ≈[0..2.7]× density
            // (mean stays ≈1 — turbulence redistributes, not dims).
            turb.amp = amp * 2.2f;
            const float cell = std::max(0.05f, fds::FeatureFlags::cone_turb_scale());
            turb.invCell = 1.0f / cell;
            const float tsec = float(g_FrameTime) * 0.01f;
            const float spd  = fds::FeatureFlags::cone_turb_speed();
            // Fixed world drift direction (mostly lateral + a slight
            // rise), in noise cells/s — the fast-fog wind convention.
            turb.driftX = (0.60f * spd) * tsec;
            turb.driftY = (0.25f * spd) * tsec;
            turb.driftZ = (0.35f * spd) * tsec;
            turb.swirl  = fds::FeatureFlags::cone_swirl();
            // swirl=1 → one full twist per 8 noise cells along the axis;
            // the phase rotates the helix over time (rate rides spd).
            turb.twistK = 6.2831853f / (8.0f * cell);
            turb.phase  = 1.2f * spd * tsec;
            turb.octaves = std::max(1, std::min(2,
                fds::FeatureFlags::cone_turb_octaves()));
            std::memcpy(turb.M, ctx.viewToWorld, sizeof(turb.M));
            turb.camX = ctx.cameraWorldX;
            turb.camY = ctx.cameraWorldY;
            turb.camZ = ctx.cameraWorldZ;
        }
    }

    // Iterate the frame-global ViewLightsSoA built by Render_DeferredLighting
    // (dc.lights / .numLights). The per-tile TileLights apply a
    // surface-z cull that's incorrect for volumetric integration — see the
    // note inside Render_VolumetricCones_Tile.
    const ViewLightsSoA *const lights = ctx.lights;
    if (!lights) return;
    const int numLights = ctx.numLights;

    // Pre-filter spotlight indices once per frame; tiles share the result.
    // Mirror-clone spots ARE admitted (beams show in mirrors): the tile
    // fn gates them per pixel on the mirror footprint mask and clamps
    // the chord to start at the wall depth — same containment the halo
    // pass uses for clone-omni glows. Ungated they'd wash additive glow
    // across pixels IN FRONT of the mirror, which is why they were
    // excluded before the gate existed.
    int spotIdx[DEFERRED_MAX_VIEW_LIGHTS];   // local: concurrent shard bakes
    int spotCount = 0;
    // Attribution / ablation debug switches (cone-cost campaign): skip a
    // whole spot category at the prefilter so a window bench isolates its
    // share of the pass. Real spot = neither clone nor bounce.
    static const bool sSkipClone  = [](){ const char *e = std::getenv("FDS_CONE_SKIP_CLONE");  return e && *e == '1'; }();
    static const bool sSkipBounce = [](){ const char *e = std::getenv("FDS_CONE_SKIP_BOUNCE"); return e && *e == '1'; }();
    static const bool sSkipReal   = [](){ const char *e = std::getenv("FDS_CONE_SKIP_REAL");   return e && *e == '1'; }();
    static const bool sConeAttr   = [](){ const char *e = std::getenv("FDS_CONE_ATTR");        return e && *e == '1'; }();
    int nReal = 0, nBounce = 0, nClone = 0;
    for (int i = 0; i < numLights; ++i) {
        if (!lights->isSpot[i] || !(allCones || lights->forceCone[i])) continue;
        const bool isClone  = lights->mirrorId[i] != 0;
        const bool isBounce = lights->bounceClamp[i] != 0;
        if (isClone)       { if (sSkipClone)  continue; ++nClone; }
        else if (isBounce) { if (sSkipBounce) continue; ++nBounce; }
        else               { if (sSkipReal)   continue; ++nReal; }
        spotIdx[spotCount++] = i;
    }
    if (spotCount == 0) return;
    // --cone-fine-tiles: 12x8 (== the lighting grid) instead of the coarse
    // 6x4. Finer tiles spread the cone-heavy region across 4x more tasks for
    // better load balance at the pass barrier; each cone tile then maps 1:1
    // onto a lighting tile (scaleX/Y = 1) for the zMax lookup. Coarse 6x4
    // keeps the legacy 2x2 (scaleX/Y = 2) mapping.
    const bool coneFine = fds::FeatureFlags::cone_fine_tiles();
    const int numTilesX = coneFine ? DEFERRED_NUM_TILES_X : 6;
    const int numTilesY = coneFine ? DEFERRED_NUM_TILES_Y : 4;
    const int numTiles  = numTilesX * numTilesY;
    const int scaleX = DEFERRED_NUM_TILES_X / numTilesX;   // lighting tiles per cone tile
    const int scaleY = DEFERRED_NUM_TILES_Y / numTilesY;
    int tileSizeX, tileSizeY;
    if (coneFine) {
        // Match the lighting tile geometry exactly so cone tile K == lighting
        // tile K (Render_DeferredLighting:3164-3166: 8-rounded X).
        const int rawTileX = (XRes + (numTilesX - 1)) / numTilesX;
        tileSizeX = (rawTileX + 7) & ~7;
        tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;
    } else {
        tileSizeX = (XRes + numTilesX - 1) / numTilesX;
        tileSizeY = (YRes + numTilesY - 1) / numTilesY;
    }

    // Per-tile spot filtering. Mirror buildTileLightLists's screen-space
    // sphere projection but WITHOUT the z-cull (which caused the
    // tile-stripe artifact fixed in the prior commit). For sparse scenes
    // most tiles will see 0 spots — the per-pixel inner loop short-
    // circuits via spotCount==0. Array sized for the 12x8 max; cap 64 is
    // ample (only spots → ~10 disco + 12 bounce on greets, well under 64).
    // CONE_TILE_SPOT_CAP moved to file scope — the per-(tile × spot)
    // precompute in the tile fn sizes its record array on the same cap.
    int tileSpotIdx  [DEFERRED_NUM_TILES][CONE_TILE_SPOT_CAP];   // local: concurrent shard bakes
    int tileSpotCount[DEFERRED_NUM_TILES];
    for (int t = 0; t < numTiles; ++t) tileSpotCount[t] = 0;

    // Clone-spot tile cull: a clone beam contributes only where its
    // mirror's stamped footprint is present (the per-pixel gate below
    // tests mmask[pi] == mirrorId), but its range sphere tags most of
    // the screen — measured 40 clone spots vs 10 real on greets, ~75%
    // of the pass's (tile × spot) iteration volume, ~half its pool
    // time. FDS_NO_CONE_MIRROR_CULL=1 is the A/B escape.
    static const bool sNoMirrorCull = [](){ const char *e = std::getenv("FDS_NO_CONE_MIRROR_CULL"); return e && *e == '1'; }();
    uint32_t tilePresenceBits[DEFERRED_NUM_TILES];
    for (int t = 0; t < numTiles; ++t) {
        if (!ctx.hasMirrorPresence || sNoMirrorCull) { tilePresenceBits[t] = 0xffffffffu; continue; }
        const int j = t / numTilesX, i = t - j * numTilesX;
        tilePresenceBits[t] = mirrorPresenceForRect(
            ctx.tileMirrorPresence,
            i * tileSizeX, j * tileSizeY,
            std::min((i + 1) * tileSizeX, XRes), std::min((j + 1) * tileSizeY, YRes),
            XRes, YRes);
    }

    for (int s = 0; s < spotCount; ++s) {
        const int li = spotIdx[s];
        const float vx = lights->posX[li];
        const float vy = lights->posY[li];
        const float vz = lights->posZ[li];
        const float r  = std::sqrt(lights->range2[li]);
        LightScreenRect sr;
        if (!lightSphereScreenRect(vx, vy, vz, r, FOVX, FOVY, CntrEX, CntrEY,
                                   XRes, YRes, sr)) continue;
        if (!sr.full && (sr.x0 > sr.x1 || sr.y0 > sr.y1)) continue;
        const int ti_lo = sr.full ? 0 : sr.x0 / tileSizeX;
        const int ti_hi = sr.full ? numTilesX - 1
                                  : std::min(numTilesX - 1, sr.x1 / tileSizeX);
        const int tj_lo = sr.full ? 0 : sr.y0 / tileSizeY;
        const int tj_hi = sr.full ? numTilesY - 1
                                  : std::min(numTilesY - 1, sr.y1 / tileSizeY);
        // Cone-vs-tile cull: the volumetric ray spans [near, surface],
        // so the relevant chunk is the cone tile's screen rect swept
        // from the near plane to the deepest surface beneath it (max
        // of the 2x2 underlying 12x8 surface tiles' zMax; empty tiles
        // → FZP). Without this every narrow beam pays the per-pixel
        // quadratic + segment integral in nearly every tile.
        const bool coneCull = fds::FeatureFlags::spot_cone_cull() &&
                              ctx.tileLights != nullptr;
        const float sinO_cull = lights->sinOuter[li];
        const float fzpFar = CurScene->FZP > 0.0f ? CurScene->FZP : 1e4f;
        const uint32_t omidBin = lights->mirrorId[li];
        for (int j = tj_lo; j <= tj_hi; ++j) {
            for (int i = ti_lo; i <= ti_hi; ++i) {
                const int t = j * numTilesX + i;
                // Clone-spot footprint cull (see tilePresenceBits above).
                // Ids ≥ 32 don't fit the bitmask; conservatively kept
                // (grid marks them all-ones), matching the kernel cull.
                if (omidBin != 0 && omidBin < 32 &&
                    !(tilePresenceBits[t] & (1u << omidBin))) {
                    continue;
                }
                if (coneCull) {
                    // Far bound of the tile's volumetric chunk. A cone's
                    // in-scatter reaches a pixel along [near, thatPixel's ray
                    // far]. For an OPAQUE pixel the ray ends at its surface;
                    // for a SKY / background pixel it runs out to the fog
                    // cutoff (where the cone fades to zero). tileLights.zMax
                    // is the farthest *opaque surface* only (sky pixels are
                    // excluded in computeTileDepthBounds), so a tile that
                    // MIXES surface + sky must extend the chunk to the fog
                    // cutoff — otherwise a beam glowing in the tile's sky
                    // portion is clipped away here and reappears in the
                    // adjacent pure-sky tile, drawing a rectangular seam
                    // (the "missing light on the rect" tiled-deferred bug).
                    float zHiT = -1e30f;
                    for (int sj = 0; sj < scaleY; ++sj)
                        for (int si = 0; si < scaleX; ++si) {
                            const int st = (j*scaleY + sj) * DEFERRED_NUM_TILES_X + (i*scaleX + si);
                            const TileLights &stl = ctx.tileLights[st];
                            const float zm = stl.zMax;
                            // hasSky OR no valid surface → the ray reaches the
                            // fog cutoff; else the farthest opaque surface.
                            const float far = (stl.hasSky || !(zm > 0.0f && zm < 1e30f))
                                              ? fzpFar : zm;
                            zHiT = std::max(zHiT, far);
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
                if (tileSpotCount[t] < CONE_TILE_SPOT_CAP) {
                    tileSpotIdx[t][tileSpotCount[t]++] = li;
                }
            }
        }
    }

    if (sConeAttr && !inlineDispatch) {
        // Tile entries per category ≈ the per-pixel loop's iteration mix
        // (each entry = one spot iterated by every alive pixel batch in
        // that tile).
        int eReal = 0, eBounce = 0, eClone = 0;
        for (int t = 0; t < numTiles; ++t)
            for (int s = 0; s < tileSpotCount[t]; ++s) {
                const int li = tileSpotIdx[t][s];
                if (lights->mirrorId[li] != 0)       ++eClone;
                else if (lights->bounceClamp[li] != 0) ++eBounce;
                else                                  ++eReal;
            }
        fprintf(stderr, "[CONE-ATTR] spots real=%d bounce=%d clone=%d | "
                "tile-entries real=%d bounce=%d clone=%d\n",
                nReal, nBounce, nClone, eReal, eBounce, eClone);
    }

    if (g_coneDiag && !inlineDispatch) {
        // Per-spot shape census: which spots take the 8-segment hybrid, and
        // which carry a shadow map (8 scalar taps/lane on that path).
        int nNarrow = 0, nShadow = 0;
        for (int s = 0; s < spotCount; ++s) {
            const int li = spotIdx[s];
            if (lights->cosOuter[li] > 0.985f) ++nNarrow;
            if (lights->shadowMapIdx[li] >= 0 ||
                lights->srcShadowMapIdx[li] >= 0) ++nShadow;
        }
        const long long P = g_dPairs.load(), D = g_dDead.load();
        const long long S = g_dSegPair.load(), L = g_dLanes.load();
        const long long SD = g_dSphDead.load();
        const long long QD = g_dQuadDead.load();
        fprintf(stderr,
            "[CONE-DIAG] spots=%d narrow(seg8)=%d shadowed=%d | "
            "batchxspot=%lld dead=%lld (%.1f%%) seg=%lld (%.1f%%) | "
            "alive lanexspot=%lld (%.2f per pair, %.1f%% of 8) sphdead=%lld (%.1f%%) quaddead=%lld (%.1f%%)\n",
            spotCount, nNarrow, nShadow, P, D,
            P ? 100.0 * double(D) / double(P) : 0.0,
            S, P ? 100.0 * double(S) / double(P) : 0.0,
            L, P ? double(L) / double(P) : 0.0,
            P ? 100.0 * double(L) / (8.0 * double(P)) : 0.0,
            SD, P ? 100.0 * double(SD) / double(P) : 0.0,
            QD, P ? 100.0 * double(QD) / double(P) : 0.0);
        g_dPairs = 0; g_dDead = 0; g_dSegPair = 0; g_dLanes = 0; g_dSphDead = 0;
        g_dQuadDead = 0;
    }

    if (!inlineDispatch) renderns::tileCounter = 0;
    if (inlineDispatch) {
        // Offscreen shard bake: run on the calling worker thread (no
        // pool re-submit, no tileDone traffic).
        for (int t = 0; t < numTiles; ++t) {
            const int j = t / numTilesX, i = t - j * numTilesX;
            const int y1 = tileSizeY * j, y2 = std::min(y1 + tileSizeY, YRes);
            const int x1 = tileSizeX * i, x2 = std::min(x1 + tileSizeX, XRes);
            Render_VolumetricCones_Tile(ctx, x1,y1,x2,y2, lights,
                                         tileSpotIdx[t], tileSpotCount[t],
                                         invFOVX,invFOVY,invZScale,density,
                                         fogZ,invFogZ, turb);
        }
    } else {
        // Work-stealing chunk dispatch via dispatchIndexed (fn releases
        // tileDone itself → done=nullptr; the [&] captures reference this
        // frame's stack, valid until the drain below). Tiles write
        // disjoint rows in any order → byte-identical.
        const TailProf::Stamp _profCones("cones");
        dispatchIndexed(numTiles, nullptr, [&](int t) {
            const int j = t / numTilesX, i = t - j * numTilesX;
            const int y1 = tileSizeY * j, y2 = std::min(y1 + tileSizeY, YRes);
            const int x1 = tileSizeX * i, x2 = std::min(x1 + tileSizeX, XRes);
            const long long _tp = TailProf::nowNs();
            Render_VolumetricCones_Tile(ctx, x1,y1,x2,y2, lights,
                                         tileSpotIdx[t], tileSpotCount[t],
                                         invFOVX,invFOVY,invZScale,density,
                                         fogZ,invFogZ, turb);
            TailProf::addBusy(_tp);   // before release → race-free idle metric
            renderns::tileDone.release();
        });
        TailProf::drain(renderns::tileDone, numTiles, "cones", 2, _profCones);
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
    const DeferredLightingCtx &ctx,
    int x1, int y1, int x2, int y2,
    const ViewLightsSoA *lights,
    const int *omniIdx, int omniCount,
    float invFOVX, float invFOVY,
    float invZScale,
    float fogZ, float invFogZ,
    float density)
{
    // Render state from ctx, not globals (local aliases shadow the globals).
    const meka::GBuffer *const g_gbuffer = ctx.gb;
    const int XRes = ctx.xres;
    byte *const VPage = ctx.vpage;
    word *const ZPage16 = ctx.zpage16;
    const float CntrEX = ctx.cntrEX, CntrEY = ctx.cntrEY;
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
                    // Bayer dither + round bias kept inside the value so the LDR
                    // path stays byte-identical; the <1.0 bias is negligible in
                    // HDR float (no quantization here — the tonemap dithers).
                    VolCompositeAdd(out, i, accB[lane] + 0.5f + d,
                                    accG[lane] + 0.5f + d, accR[lane] + 0.5f + d);
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
                VolCompositeAdd(out, i, accB + 0.5f + d, accG + 0.5f + d, accR + 0.5f + d);
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
                    VolCompositeAdd(out, i, accB[lane], accG[lane], accR[lane]);
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
            VolCompositeAdd(out, i, accB, accG, accR);
        }
        }
    }
}

void Render_OmniHalos(const DeferredLightingCtx &ctx) {
    VolProfScope _vp(&g_volProf.ms_halos, &g_volProf.n_halos);
    if (!CurScene || !ZPage16 || !VPage) return;
    if (fds::FeatureFlags::omni_halo_strength() <= 0.0f) return;
    const float invFOVX = 1.0f / FOVX;
    const float invFOVY = 1.0f / FOVY;
    const float invZScale = 1.0f / float(g_zscale);
    const float density = fds::FeatureFlags::omni_halo_strength() * 0.001f;
    const float fogZ    = (CurScene->Flags & Scn_Fogged) ? CurScene->FZP : 0.0f;
    const float invFogZ = (fogZ > 0.0f) ? 1.0f / fogZ : 0.0f;

    const ViewLightsSoA *const lights = ctx.lights;
    if (!lights) return;
    const int numLights = ctx.numLights;

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

    constexpr int numTilesX = 12;
    constexpr int numTilesY = 8;
    constexpr int numTiles  = numTilesX * numTilesY;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;

    // Per-tile omni cull — same sphere-projection math as cones, but
    // no z-cull (omni halo is volumetric, surface-z occlusion is per-
    // pixel via zSurf clamp inside the kernel).
    static int tileOmniIdx  [numTiles][DEFERRED_MAX_VIEW_LIGHTS];
    static int tileOmniCount[numTiles];
    for (int t = 0; t < numTiles; ++t) tileOmniCount[t] = 0;

    // Clone-omni footprint cull — same reasoning as the cone binning's
    // tilePresenceBits (per-pixel gate is mmask[pi] == mirrorId, so a
    // tile without the footprint contributes nothing).
    static const bool sNoMirrorCullH = [](){ const char *e = std::getenv("FDS_NO_CONE_MIRROR_CULL"); return e && *e == '1'; }();
    uint32_t tilePresenceBits[numTiles];
    for (int t = 0; t < numTiles; ++t) {
        if (!ctx.hasMirrorPresence || sNoMirrorCullH) { tilePresenceBits[t] = 0xffffffffu; continue; }
        const int j = t / numTilesX, i = t - j * numTilesX;
        tilePresenceBits[t] = mirrorPresenceForRect(
            ctx.tileMirrorPresence,
            i * tileSizeX, j * tileSizeY,
            std::min((i + 1) * tileSizeX, XRes), std::min((j + 1) * tileSizeY, YRes),
            XRes, YRes);
    }

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
        LightScreenRect sr;
        if (!lightSphereScreenRect(vx, vy, vz, r, FOVX, FOVY, CntrEX, CntrEY,
                                   XRes, YRes, sr)) continue;
        if (!sr.full && (sr.x0 > sr.x1 || sr.y0 > sr.y1)) continue;
        const int ti_lo = sr.full ? 0 : sr.x0 / tileSizeX;
        const int ti_hi = sr.full ? numTilesX - 1
                                  : std::min(numTilesX - 1, sr.x1 / tileSizeX);
        const int tj_lo = sr.full ? 0 : sr.y0 / tileSizeY;
        const int tj_hi = sr.full ? numTilesY - 1
                                  : std::min(numTilesY - 1, sr.y1 / tileSizeY);
        const uint32_t omidBin = lights->mirrorId[li];
        for (int j = tj_lo; j <= tj_hi; ++j) {
            for (int i = ti_lo; i <= ti_hi; ++i) {
                const int t = j * numTilesX + i;
                if (omidBin != 0 && omidBin < 32 &&
                    !(tilePresenceBits[t] & (1u << omidBin))) {
                    continue;
                }
                if (tileOmniCount[t] < DEFERRED_MAX_VIEW_LIGHTS) {
                    tileOmniIdx[t][tileOmniCount[t]++] = li;
                }
            }
        }
    }

    renderns::tileCounter = 0;
    // Work-stealing chunk dispatch (see the cones wave above) — one enqueue
    // per worker instead of per tile. Job data (tileOmniIdx/Count) lives on
    // this frame's stack; valid until the drain below completes.
    {
        const auto *tileOmniIdxP  = &tileOmniIdx[0];
        const int  *tileOmniCntP  = &tileOmniCount[0];
        dispatchIndexed(numTilesX * numTilesY, nullptr,
            [&ctx, lights, invFOVX, invFOVY, invZScale, fogZ, invFogZ, density,
             tileSizeX, tileSizeY, tileOmniIdxP, tileOmniCntP](int t) {
                const int j = t / numTilesX, i = t - j * numTilesX;
                const int y1 = tileSizeY * j, y2 = std::min(y1 + tileSizeY, YRes);
                const int x1 = tileSizeX * i, x2 = std::min(x1 + tileSizeX, XRes);
                Render_OmniHalos_Tile(ctx, x1,y1,x2,y2, lights,
                                       tileOmniIdxP[t], tileOmniCntP[t],
                                       invFOVX,invFOVY,invZScale,
                                       fogZ,invFogZ,density);
                renderns::tileDone.release();
            });
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

// Fold the nearest transparent layer's depth (from the xpar peel) into the
// main Z-buffer. Transparents don't write the opaque Z, so a later additive
// overlay (the fountain bolt) isn't occluded by them; after this, it is.
// Encoding is 0xFF80 - zScale*depth, so NEARER = larger value → take the max.
// No-op outside the deferred xpar path (g_xparZ null). ZPage16 is unused after
// the bolt, so clobbering it here is safe.
void Deferred_FoldXparDepthIntoMainZ() {
    if (!g_xparZ || !ZPage16) return;
    const size_t n = size_t(XRes) * size_t(YRes);
    for (size_t i = 0; i < n; ++i)
        if (g_xparZ[i] > ZPage16[i]) ZPage16[i] = g_xparZ[i];
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

    constexpr int numTilesX = 12;
    constexpr int numTilesY = 8;
    const int tileSizeX = (XRes + numTilesX - 1) / numTilesX;
    const int tileSizeY = (YRes + numTilesY - 1) / numTilesY;
    renderns::tileCounter = 0;
    dispatchIndexed(numTilesX * numTilesY, nullptr, [=](int _t) {
    	const int j = _t / numTilesX, i = _t - j * numTilesX;
    	const int y1 = tileSizeY * j;
    	const int y2 = std::min(y1 + tileSizeY, YRes);
    	const int x1 = tileSizeX * i;
    	const int x2 = std::min(x1 + tileSizeX, XRes);
    	{
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
            }
    });
    for (int _i = 0, n = numTilesX * numTilesY; _i < n; ++_i) {
        renderns::tileDone.acquire();
    }
}

void Render_DeferredFogPass(const DeferredLightingCtx &ctx) {
	if (!CurScene || !(CurScene->Flags & Scn_Fogged)) return;
	// Empty ctx = forward-mode frame (renderFrame only fills dctx on the
	// deferred path) — same no-op contract as the cone/halo passes. An
	// offscreen ForceForward render of a fogged scene reaches here; the
	// tile writes ctx.vpage, so without this it would write through null
	// (the old global-reading code silently fogged the WRONG target).
	if (!ctx.gb || !ctx.vpage || !ctx.zpage16) return;
	const float invFZP = 1.0f / CurScene->FZP;
	constexpr auto numTilesX = 12;
	constexpr auto numTilesY = 8;
	const auto tileSizeX = (XRes + (numTilesX - 1)) / numTilesX;
	const auto tileSizeY = (YRes + (numTilesY - 1)) / numTilesY;
	renderns::tileCounter = 0;
	const TailProf::Stamp _profFog("fog");
	dispatchIndexed(numTilesX * numTilesY, nullptr,
	    [&ctx, tileSizeX, tileSizeY, invFZP](int t) {
		const int j = t / numTilesX, i = t - j * numTilesX;
		const int y1 = tileSizeY * j, y2 = std::min(y1 + tileSizeY, YRes);
		const int x1 = tileSizeX * i, x2 = std::min(x1 + tileSizeX, XRes);
		Render_DeferredFogPass_Tile(ctx, x1, y1, x2, y2, invFZP);
		renderns::tileDone.release();
	});
	TailProf::drain(renderns::tileDone, numTilesX * numTilesY, "fog", 2, _profFog);
}

