// Per-frame geometry pipeline stages — extracted from RENDER.CPP.
//
//   Animate_Objects(Sc, skipCameraAnim)
//     Walks scene splines / keyframes and updates Pos/Rot/Scale on each
//     Object. Builds RotMat per TriMesh. Runs once before transform.
//
//   Vertex_Loop1   (dead code; legacy MMX-era helper kept for reference)
//
//   calcVisibilityFlags(Sc, vtx)
//     Frustum-test classification for a single vertex.
//
//   addParticleTrail(Sc, FListInsertPtr, particle)
//     Emit per-particle Face entries with trail-segment vertices.
//
//   IsFrontFacingInViewSpace(F)
//     View-space facing test; also used by RenderXparClumpInStrip in
//     the deferred-transparent path so it stays non-static.
//
//   QuadAwareMaxViewZ(F, T)
//     Quad-sibling-extended max view-z for back-to-front sort keys.
//
//   Transform_Objects(Sc, xresOverride, yresOverride)
//     The big one. Walks ObjectHead, per-object: skips invisible/culled,
//     transforms vertices into view space, runs particles, builds FList,
//     stamps SortZ/VisibilityFlags/Filler, populates CPolys/COmnies/CPcls.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <Base/Vector.h>
#include <memory>
#include <algorithm>

// 4-wide SIMD for the per-vertex transform inner loops below. Loads/stores
// are on the fly via vectorclass Vec4f; the per-vertex matrix-vector is
// rewritten as 3 broadcast-FMAs against column-major matrices instead of
// 9 scalar muls + 9 scalar adds. See Transform_Objects for the staging.
#include "simde/x86/sse.h"
#include <simd/vectorclass.h>

// Scalar polynomial atan2/asin approximations for the reflective-face
// equirectangular EU/EV per-vertex stamp (used heavily by city windows).
#include "FILLERS/SimdHelpers.h"

// SoA Vertex refactor — Phase 1: per-mesh SoA companion of the AoS
// transformed-vertex output fields. See docs/SOA_VERTEX_REFACTOR.md.
#include "Base/VertexFrame.h"
#include <chrono>
#include <cstdint>
// fds::g_offAxisFrustumCull — off-axis projection support for the
// mirror RTT's offscreen Transform (see FrameState.h).
#include "Base/FrameState.h"

// Research instruments (--xfrm_pass_prof, --mirror_cull_census): compile-time
// gated, see the FDS_VIS_CENSUS block further down for WHY a runtime flag is
// not enough here. 0 = the shipping build, textually un-instrumented.
#ifndef FDS_VIS_CENSUS
#define FDS_VIS_CENSUS 0
#endif
#if FDS_VIS_CENSUS
#include <atomic>
// fds::MirrorCloneSubSpheres — the per-source-mesh sub-spheres of a mirror
// clone, read ONLY by the --mirror_cull_census diagnostic below.
#include "RENDER/GreetsMirror.h"
#endif

// Front-to-back face sort. Closer faces dispatch first so subsequent
// farther faces fail Z and skip the rasterizer's per-pixel work — a
// pure perf optimization. The original RENDER.CPP defined this at
// line 38; the 415fd16 extraction to Transform.cpp accidentally
// dropped it, silently selecting the back-to-front branch. That
// surfaced as a deferred-reflective-window regression: the order
// reversal interacted badly with TheOtherBarry's forward filler
// running inside Mekalele's tile pass — Mekalele wrote wall mat32
// before TheOtherBarry overdrew the window, and the lighting kernel
// then re-shaded the wall over the window's reflection. That
// correctness issue is fixed in the rasterizer (TheOtherBarry now
// stamps the mat32 sentinel for every pixel it writes in deferred
// mode); this define stays in place purely for the perf win.
#define FRONT_TO_BACK_SORTING

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "RENDER/EnvCube.h"   // env_cube: trig-free per-triangle face select
#include "RENDER/EnvBake.h"   // --env_live_water: animated water in env lookups
#include "RENDER/ChunkOcclusion.h"  // Phase B: conservative main-view chunk occlusion cull
#include "Base/Scene.h"
#include "Base/TriMesh.h"
#include "Base/Vertex.h"
#include "Base/Face.h"
#include "Base/Omni.h"
#include "Base/Camera.h"
#include "Base/Material.h"
#include "Base/Object.h"
#include "Base/Spline.h"
#include "Base/CameraContext.h"
#include "Base/FaceListContext.h"
#include "Base/VertexScratch.h"
#include "FILLERS/ShadowMap.h"
#include "Threads.h"     // --xfrm_par: mesh-sharded main-view transform
#include <thread>
#include <cstring>

// Defined in Shadows.cpp; the shadow orchestrator sets this to the
// current shadow light before calling Transform_Objects, so the per-mesh
// loop below can read its cone params for the bsphere-vs-cone cull.
extern thread_local Omni* g_currentShadowOmni;

// Defined in Shadows.cpp. Generation stamp for the per-mesh bake cache
// (TriMesh::BakeCacheGen / BakeWsBSphereCtr / BakeIsDynamic); primed
// serially before the parallel per-face tasks, read-only here.
extern uint32_t g_shadowBakeGen;

// ── XFRM front-end profiler (--xfrm_prof=N) ──────────────────────────────
// Fine-grained breakdown of the MAIN-VIEW Transform_Objects call. Off by
// default: `xp` is false, no clock is read, and every accumulate site is a
// predictable not-taken branch. See FeatureFlags.def:xfrm_prof for what the
// buckets mean. Tick-thread only (the main-view Transform_Objects is
// single-threaded and the gate excludes every offscreen/threaded pass), so
// plain scalars — no atomics.
namespace {
enum XProfSect { XP_TOTAL = 0, XP_SETUP, XP_VERT, XP_SOA, XP_FACE, XP_NUM };
// Per-frame samples, not a running mean. The box this runs on is routinely
// shared with other agents' renders (load average 20+ observed), and a mean
// over a window is dominated by whichever frames got descheduled. Keeping the
// per-frame values lets the dump report the MINIMUM (the least-contended
// frame = the best estimator of the uncontended cost) alongside the median.
constexpr int XP_MAXFRAMES = 256;
struct XProfAcc {
	int64_t cur[XP_NUM] = {};                 // frame in flight
	int64_t s[XP_NUM][XP_MAXFRAMES] = {};     // per-frame samples
	int64_t meshes = 0, verts = 0, facesTested = 0, facesPushed = 0;
	int64_t vInside = 0, vAhead = 0, vRegular = 0;  // which per-vertex loop ran
	int     frames = 0;
};
XProfAcc g_xprof;

inline int64_t xpNow() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Ablation bits — see FeatureFlags.def:xfrm_ablate.
enum : int {
	XAB_NO_TN     = 1,
	XAB_NO_PROJ   = 2,
	XAB_NO_SOA    = 4,
	XAB_FACE_CULL = 8,
	XAB_FACE_NOOP = 16,
	XAB_NO_FACE   = 32,
	XAB_NO_BBOX   = 64,
};

// --xfrm_rcp: the per-vertex 1/z used by the projection. Mode 0 reproduces
// today's code EXACTLY (the Ahead/Regular loops write `1.0/z` — a DOUBLE
// divide on a float, then a narrowing store; Inside writes `1.0f/z`).
// Mode 1 makes every site a single-precision divide. Mode 2 uses the NEON
// reciprocal estimate plus one Newton-Raphson step. Modes 1 and 2 CHANGE
// PIXELS (1 changes only where double-rounding differs from direct float
// rounding; 2 is a genuine approximation) - see the divergence numbers in
// docs/SOA_VERTEX_REFACTOR.md before enabling either.
inline float xfrmRcpApprox(float z) {
	__m128 v = _mm_set_ss(z);
	const float r = _mm_cvtss_f32(_mm_rcp_ss(v));
	return r * (2.0f - z * r);   // one Newton step
}
// Sites that today divide in DOUBLE.
inline float xfrmRcpD(float z, int mode) {
	if (mode == 0) return float(1.0 / double(z));
	if (mode == 1) return 1.0f / z;
	return xfrmRcpApprox(z);
}
// Sites that today divide in FLOAT.
inline float xfrmRcpF(float z, int mode) {
	if (mode <= 1) return 1.0f / z;
	return xfrmRcpApprox(z);
}

#if FDS_VIS_CENSUS
#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <Base/Material.h>
// ── VISIBILITY CENSUS BUILD ONLY (-DFDS_VIS_CENSUS=1) ────────────────────
// The per-PASS profiler and the mirror-clone split census below are RESEARCH
// instruments (docs/VISIBILITY_PLAN.md §8). They are compile-time gated, not
// just flag-gated, because this build is -O3 -flto -ffp-contract=fast and
// MEASURED: merely carrying the (never-taken) branches inside
// Transform_Objects changes which expressions the compiler contracts into
// FMAs in the surrounding vertex/face work, moving real pixels with every
// flag OFF — city cold-cache b2af24de -> 850be968 at 1920x1080. A diagnostic
// must not cost pixels, and a runtime `if (0)` cannot promise that here. With
// the macro undefined the preprocessor removes every line, so the shipping
// Transform_Objects is TEXTUALLY identical to the un-instrumented one and
// byte-nullity is guaranteed by construction rather than by measurement.
// Measure with:  cmake -S . -B build-census -G Ninja -DFDS_VIS_CENSUS=ON
// ── Per-PASS front-end census (--xfrm_pass_prof=N) ───────────────────────
// xfrm_prof above is MAIN-VIEW only by construction (no synchronisation).
// Greets runs many more Transform_Objects calls per frame — the mirror RTT
// bakes, the shadow-cube faces, the env/SH probe faces — and the question
// "where does the geometry front-end time actually go across the whole
// frame" needs all of them attributed. This accumulator is therefore
// atomic (shadow bakes + the mirror shard pass run Transform_Objects on
// worker threads) and buckets by pass KIND. Diagnostic only: culls
// nothing, and when the flag is 0 nothing is read or written.
enum XPassKind { XPK_MAIN = 0, XPK_MIRROR_RTT, XPK_SHADOW, XPK_OFFSCREEN, XPK_NUM };
const char *const kXPassName[XPK_NUM] = { "MAIN", "MIRROR-RTT", "SHADOW", "OFFSCREEN" };
struct XPassAcc {
	std::atomic<int64_t> ns{0};
	std::atomic<int64_t> calls{0};
	std::atomic<int64_t> meshesSeen{0}, meshesXformed{0};
	std::atomic<int64_t> vertsSeen{0}, vertsXformed{0};
	std::atomic<int64_t> facesPushed{0};
};
XPassAcc g_xpass[XPK_NUM];
std::atomic<int> g_xpassMainFrames{0};

void xpassDump(int frames) {
	std::fprintf(stderr, "[XFRM-PASS] over %d main-view frames (per-frame averages)\n", frames);
	const double f = double(frames > 0 ? frames : 1);
	double totMs = 0.0;
	for (int k = 0; k < XPK_NUM; ++k) {
		XPassAcc &a = g_xpass[k];
		const double ms = a.ns.load() / 1e6 / f;
		totMs += ms;
		std::fprintf(stderr,
		    "[XFRM-PASS]   %-11s calls %6.2f  %8.3f ms  meshes %7.1f/%7.1f  "
		    "verts %10.1f/%10.1f (cull %5.1f%%)  fPushed %9.1f\n",
		    kXPassName[k], a.calls.load() / f, ms,
		    a.meshesXformed.load() / f, a.meshesSeen.load() / f,
		    a.vertsXformed.load() / f, a.vertsSeen.load() / f,
		    a.vertsSeen.load() > 0
		        ? 100.0 * double(a.vertsSeen.load() - a.vertsXformed.load()) / double(a.vertsSeen.load())
		        : 0.0,
		    a.facesPushed.load() / f);
		a.ns = 0; a.calls = 0; a.meshesSeen = 0; a.meshesXformed = 0;
		a.vertsSeen = 0; a.vertsXformed = 0; a.facesPushed = 0;
	}
	std::fprintf(stderr, "[XFRM-PASS]   %-11s %30.3f ms/frame\n", "TOTAL", totMs);
}

// ── Per-MESH / per-MATERIAL decomposition of the pass census
//    (--xfrm_pass_mesh_prof=N, census build only) ───────────────────────────
// xpassDump answers "which PASS dominates"; this answers "WHAT IS IN IT".
// Needed because the 7.6 M shadow-pass verts were only ever measured as one
// lump, and the decision "flatten the wall casters" depends entirely on the
// walls' share of that lump. Attribution is per mesh (the granularity the
// mesh cull works at) plus a proportional per-MATERIAL split computed at DUMP
// time from each mesh's face->Txtr histogram, so the hot loops stay untouched
// beyond two atomic adds.
//
// Lock-free open-addressed table keyed on the TriMesh pointer: shadow passes
// run Transform_Objects on 12 workers, and a mutex there would serialise them.
// Meshes are created once at scene init, so after the first frame every probe
// is a single acquire-load.
constexpr int XM_CAP = 8192;   // power of two
struct XMeshSlot {
	std::atomic<const void*> key{nullptr};
	const TriMesh *mesh = nullptr;
	const char    *name = nullptr;
	std::atomic<int64_t> seen[XPK_NUM]{};
	std::atomic<int64_t> seenV[XPK_NUM]{};
	std::atomic<int64_t> xf[XPK_NUM]{};
	std::atomic<int64_t> xfV[XPK_NUM]{};
};
XMeshSlot g_xm[XM_CAP];

__attribute__((noinline))
XMeshSlot *xmSlotFor(const TriMesh *T, const char *nm)
{
	size_t h = size_t(reinterpret_cast<uintptr_t>(T) >> 4) * size_t(0x9E3779B97F4A7C15ull);
	h ^= h >> 29;
	for (int i = 0; i < XM_CAP; ++i) {
		XMeshSlot &s = g_xm[(h + size_t(i)) & (XM_CAP - 1)];
		const void *k = s.key.load(std::memory_order_acquire);
		if (k == (const void *)T) return &s;
		if (k == nullptr) {
			const void *expect = nullptr;
			if (s.key.compare_exchange_strong(expect, (const void *)T,
			                                  std::memory_order_acq_rel)) {
				s.mesh = T; s.name = nm;
				return &s;
			}
			if (s.key.load(std::memory_order_acquire) == (const void *)T) return &s;
		}
	}
	return nullptr;
}

// Base name for grouping: "Piramid.lwo:c17" -> "Piramid.lwo", "momy.lwo::body"
// -> "momy.lwo". The chunk / bin splits are an implementation detail of the
// same authored object and the wall/statue question is asked at object level.
void xmBaseName(const char *n, char *out, size_t cap)
{
	if (!n) { std::snprintf(out, cap, "?"); return; }
	std::snprintf(out, cap, "%s", n);
	char *c = std::strstr(out, "::");
	if (c) { *c = 0; return; }
	c = std::strchr(out, ':');
	if (c) *c = 0;
}

struct XMAgg {
	char     name[80] = {0};
	int64_t  seen[XPK_NUM] = {0}, seenV[XPK_NUM] = {0};
	int64_t  xf[XPK_NUM] = {0}, xfV[XPK_NUM] = {0};
	int      meshes = 0;
};

void xmDump(int frames)
{
	const double f = double(frames > 0 ? frames : 1);
	// ── group by base object name ──
	std::vector<XMAgg> agg;
	// ── material histogram, proportional attribution of a mesh's verts to
	//    the materials its faces use (a chunk can straddle rooms/floor/etc.)
	std::unordered_map<std::string, std::array<double, XPK_NUM>> matV, matVseen;
	int64_t tot[XPK_NUM] = {0}, totSeen[XPK_NUM] = {0};

	for (int i = 0; i < XM_CAP; ++i) {
		XMeshSlot &s = g_xm[i];
		if (s.key.load(std::memory_order_acquire) == nullptr) continue;
		char bn[80]; xmBaseName(s.name, bn, sizeof(bn));
		XMAgg *a = nullptr;
		for (XMAgg &e : agg) if (!std::strcmp(e.name, bn)) { a = &e; break; }
		if (!a) { agg.push_back(XMAgg{}); a = &agg.back();
		          std::snprintf(a->name, sizeof(a->name), "%s", bn); }
		++a->meshes;
		for (int k = 0; k < XPK_NUM; ++k) {
			const int64_t sv = s.seenV[k].load(), xv = s.xfV[k].load();
			a->seen[k]  += s.seen[k].load();  a->seenV[k] += sv;
			a->xf[k]    += s.xf[k].load();    a->xfV[k]   += xv;
			tot[k] += xv; totSeen[k] += sv;
		}
		// material split for this mesh, by face count
		const TriMesh *T = s.mesh;
		if (T && T->Faces && T->FIndex) {
			std::unordered_map<std::string, int> fc;
			for (DWord fi = 0; fi < T->FIndex; ++fi) {
				const Material *M = T->Faces[fi].Txtr;
				fc[M && M->Name ? M->Name : "?"] += 1;
			}
			for (auto &kv : fc) {
				const double frac = double(kv.second) / double(T->FIndex);
				auto &row  = matV[kv.first];
				auto &rowS = matVseen[kv.first];
				for (int k = 0; k < XPK_NUM; ++k) {
					row[k]  += frac * double(s.xfV[k].load());
					rowS[k] += frac * double(s.seenV[k].load());
				}
			}
		}
	}

	std::sort(agg.begin(), agg.end(), [](const XMAgg &a, const XMAgg &b) {
		return a.xfV[XPK_SHADOW] > b.xfV[XPK_SHADOW];
	});

	std::fprintf(stderr,
	    "[XFRM-MESH] per-frame TRANSFORMED verts by OBJECT over %d main frames "
	    "(sorted by SHADOW). tot/frame: MAIN %.0f  RTT %.0f  SHADOW %.0f  OFFSCR %.0f\n",
	    frames, tot[XPK_MAIN]/f, tot[XPK_MIRROR_RTT]/f, tot[XPK_SHADOW]/f, tot[XPK_OFFSCREEN]/f);
	std::fprintf(stderr,
	    "[XFRM-MESH] %-26s %5s | %10s %5s | %10s %5s | %10s | %10s\n",
	    "object", "#mesh", "SHADOW-V", "%", "MAIN-V", "%", "OFFSCR-V", "RTT-V");
	for (const XMAgg &a : agg) {
		if (a.xfV[XPK_SHADOW] + a.xfV[XPK_MAIN] + a.xfV[XPK_OFFSCREEN] + a.xfV[XPK_MIRROR_RTT] == 0)
			continue;
		std::fprintf(stderr,
		    "[XFRM-MESH] %-26s %5d | %10.0f %5.1f | %10.0f %5.1f | %10.0f | %10.0f\n",
		    a.name, a.meshes,
		    a.xfV[XPK_SHADOW]/f, tot[XPK_SHADOW] ? 100.0*double(a.xfV[XPK_SHADOW])/double(tot[XPK_SHADOW]) : 0.0,
		    a.xfV[XPK_MAIN]/f,   tot[XPK_MAIN]   ? 100.0*double(a.xfV[XPK_MAIN])  /double(tot[XPK_MAIN])   : 0.0,
		    a.xfV[XPK_OFFSCREEN]/f, a.xfV[XPK_MIRROR_RTT]/f);
	}

	// ── UNGROUPED top meshes, with each mesh's FACE count. The face column is
	//    the point: Transform_Objects has no FIndex==0 early-out, so a mesh
	//    that was "retired" by moving its faces elsewhere (the greets Piramid
	//    chunk split zeroes the parent's FIndex but leaves its 16 596 verts on
	//    the object list) still pays a full per-vertex transform in every pass
	//    and emits nothing.
	struct XMRow { const char *nm; DWord nf; DWord nv; int64_t sh, mn, off; };
	std::vector<XMRow> raw;
	int64_t zeroFaceSh = 0, zeroFaceMn = 0, zeroFaceOff = 0, zeroFaceRtt = 0;
	for (int i = 0; i < XM_CAP; ++i) {
		XMeshSlot &s = g_xm[i];
		if (s.key.load(std::memory_order_acquire) == nullptr) continue;
		const TriMesh *T = s.mesh;
		raw.push_back({ s.name ? s.name : "?", T ? T->FIndex : 0, T ? T->VIndex : 0,
		                s.xfV[XPK_SHADOW].load(), s.xfV[XPK_MAIN].load(),
		                s.xfV[XPK_OFFSCREEN].load() });
		if (T && T->FIndex == 0) {
			zeroFaceSh  += s.xfV[XPK_SHADOW].load();
			zeroFaceMn  += s.xfV[XPK_MAIN].load();
			zeroFaceOff += s.xfV[XPK_OFFSCREEN].load();
			zeroFaceRtt += s.xfV[XPK_MIRROR_RTT].load();
		}
	}
	std::sort(raw.begin(), raw.end(), [](const XMRow &a, const XMRow &b) {
		return (a.sh + a.mn + a.off) > (b.sh + b.mn + b.off);
	});
	std::fprintf(stderr, "[XFRM-TOP]  top individual meshes (verts/frame transformed)\n");
	std::fprintf(stderr, "[XFRM-TOP]  %-28s %7s %7s | %10s | %10s | %10s\n",
	             "mesh", "faces", "verts", "SHADOW-V", "MAIN-V", "OFFSCR-V");
	for (size_t i = 0; i < raw.size() && i < 25; ++i) {
		const XMRow &r = raw[i];
		if (r.sh + r.mn + r.off == 0) break;
		std::fprintf(stderr, "[XFRM-TOP]  %-28s %7u %7u | %10.0f | %10.0f | %10.0f\n",
		             r.nm, unsigned(r.nf), unsigned(r.nv), r.sh/f, r.mn/f, r.off/f);
	}
	std::fprintf(stderr,
	    "[XFRM-ZERO] FACELESS meshes (FIndex==0, transform emits nothing): "
	    "SHADOW %.0f (%.1f%%)  MAIN %.0f (%.1f%%)  OFFSCR %.0f (%.1f%%)  RTT %.0f\n",
	    zeroFaceSh/f,  tot[XPK_SHADOW]    ? 100.0*double(zeroFaceSh)/double(tot[XPK_SHADOW])       : 0.0,
	    zeroFaceMn/f,  tot[XPK_MAIN]      ? 100.0*double(zeroFaceMn)/double(tot[XPK_MAIN])         : 0.0,
	    zeroFaceOff/f, tot[XPK_OFFSCREEN] ? 100.0*double(zeroFaceOff)/double(tot[XPK_OFFSCREEN])   : 0.0,
	    zeroFaceRtt/f);

	std::vector<std::pair<std::string, std::array<double, XPK_NUM>>> mv(matV.begin(), matV.end());
	std::sort(mv.begin(), mv.end(), [](const auto &a, const auto &b) {
		return a.second[XPK_SHADOW] > b.second[XPK_SHADOW];
	});
	std::fprintf(stderr, "[XFRM-MAT]  per-frame TRANSFORMED verts by MATERIAL "
	                     "(proportional face-count split within each mesh)\n");
	std::fprintf(stderr, "[XFRM-MAT]  %-26s | %10s %5s | %10s %5s | %10s\n",
	             "material", "SHADOW-V", "%", "MAIN-V", "%", "OFFSCR-V");
	for (const auto &e : mv) {
		if (e.second[XPK_SHADOW] + e.second[XPK_MAIN] + e.second[XPK_OFFSCREEN] < 1.0) continue;
		std::fprintf(stderr,
		    "[XFRM-MAT]  %-26s | %10.0f %5.1f | %10.0f %5.1f | %10.0f\n",
		    e.first.c_str(),
		    e.second[XPK_SHADOW]/f, tot[XPK_SHADOW] ? 100.0*e.second[XPK_SHADOW]/double(tot[XPK_SHADOW]) : 0.0,
		    e.second[XPK_MAIN]/f,   tot[XPK_MAIN]   ? 100.0*e.second[XPK_MAIN]  /double(tot[XPK_MAIN])   : 0.0,
		    e.second[XPK_OFFSCREEN]/f);
	}

	for (int i = 0; i < XM_CAP; ++i) {
		XMeshSlot &s = g_xm[i];
		if (s.key.load(std::memory_order_acquire) == nullptr) continue;
		for (int k = 0; k < XPK_NUM; ++k) {
			s.seen[k] = 0; s.seenV[k] = 0; s.xf[k] = 0; s.xfV[k] = 0;
		}
	}
}

// ── Mirror-clone split CEILING census (--mirror_cull_census=N) ───────────
// A greets mirror clone is ONE TriMesh holding the whole mirrored room, so
// its single bsphere is room-sized and the mesh-level frustum cull can
// never reject it (the disease the Piramid chunk split cured for the source
// wall). This census answers, WITHOUT changing any geometry, "what would a
// per-source-mesh split of the clone buy?": it runs the same sphere test the
// mesh cull runs, once per clone sub-range, and reports the verts it would
// have rejected. Main-view only, so plain scalars.
struct MCensus {
	int64_t frames = 0;
	int64_t cloneMeshes = 0, cloneVerts = 0;
	int64_t subTotal = 0, subCulled = 0;
	int64_t subVertsTotal = 0, subVertsCulled = 0;
	// Window arm: the clone can only paint inside its mirror's screen
	// footprint, so a sub-sphere outside the WINDOW pyramid is dead
	// weight even when the full-screen frustum keeps it.
	int64_t winCulled = 0, winVertsCulled = 0;
	int64_t winFrames = 0;          // frames where a usable window rect existed
	double  winAreaFrac = 0.0;      // window px / screen px, summed over winFrames
};
MCensus g_mcensus;

// Same math as the symmetric mesh-level test in Transform_Objects (depth
// then left/right then up/down), factored so the census can run it per
// sub-sphere. R2 is already L2-scaled (view-space radius²).
inline bool censusSphereOutside(const Vector &S, float R2,
                                float px, float py, float cex, float cey,
                                float nearZ, float farZ)
{
	float dz = S.z - nearZ;
	if (dz*dz > R2 && dz < 0.0f) return true;
	dz = S.z - farZ;
	if (dz*dz > R2 && dz > 0.0f) return true;
	const float ax = std::fabs(S.x);
	float L1 = px*ax - cex*S.z;
	if (L1*L1 > R2*(px*px + cex*cex) && ax*px > S.z*cex) return true;
	const float ay = std::fabs(S.y);
	L1 = py*ay - cey*S.z;
	if (L1*L1 > R2*(py*py + cey*cey) && ay*py > S.z*cey) return true;
	return false;
}

// Generalized sphere-vs-viewport-rect reject. Setting the rect to the full
// screen (0,0,XRes,YRes) reproduces the existing off-axis test at the mesh
// cull above exactly; the census also calls it with the MIRROR WINDOW rect.
// Outside-positive q = distance·‖n‖; reject iff q > 0 and q² > R²‖n‖².
inline bool censusSphereOutsideRect(const Vector &S, float R2,
                                    float px, float py, float cex, float cey,
                                    float x0, float y0, float x1, float y1)
{
	float n, q;
	n = cex - x0; q = -(px*S.x + n*S.z);                 // screen_x < x0
	if (q > 0.0f && q*q > R2*(px*px + n*n)) return true;
	n = cex - x1; q =  (px*S.x + n*S.z);                 // screen_x > x1
	if (q > 0.0f && q*q > R2*(px*px + n*n)) return true;
	n = cey - y0; q =  (py*S.y - n*S.z);                 // screen_y < y0
	if (q > 0.0f && q*q > R2*(py*py + n*n)) return true;
	n = cey - y1; q = -(py*S.y - n*S.z);                 // screen_y > y1
	if (q > 0.0f && q*q > R2*(py*py + n*n)) return true;
	return false;
}

// One mesh's contribution to the census. Deliberately NOINLINE and out of
// line of Transform_Objects: this build is -O3 -flto -ffp-contract=fast, and
// carrying the body inside the mesh loop measurably changed which expressions
// the compiler contracted into FMAs in the SURROUNDING vertex/face work — 216
// differing bytes (max |delta| 44) on the 1920x1080 city pin with the flag
// OFF. A diagnostic must be byte-null off; keeping it behind a call boundary
// restores that (city 37e62845 exact).
__attribute__((noinline))
void mirrorCensusMesh(const TriMesh *T, const Matrix &IM, const Vector &OS,
                      float L2, const fds::CameraContext &cam,
                      float px, float py, int xr, int yr)
{
	const std::vector<fds::MirrorCloneSubSphere> *subs =
	    fds::MirrorCloneSubSpheres(T);
	if (!subs || subs->empty()) return;
	++g_mcensus.cloneMeshes;
	g_mcensus.cloneVerts += T->VIndex;
	// Screen rect of this mirror's WALL faces = the only region the clone can
	// paint (Mekalele's per-pixel mirrorTag gate). Projected here from world
	// through the SAME camera the mesh cull uses. Any wall vert at/behind the
	// near plane makes the rect unusable -> fall back to the full screen (no
	// window culling that frame), which keeps the census conservative.
	float wx0 = 0.0f, wy0 = 0.0f;
	float wx1 = float(xr), wy1 = float(yr);
	bool winValid = false;
	const std::vector<fds::MirrorCloneWall> *walls = fds::MirrorCloneWalls(T);
	if (walls && !walls->empty()) {
		float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
		bool ok = true;
		for (const fds::MirrorCloneWall &w : *walls) {
			if (!w.face || !w.mesh) { ok = false; break; }
			const Vertex *vs[3] = { w.face->A, w.face->B, w.face->C };
			for (int k = 0; k < 3 && ok; ++k) {
				if (!vs[k]) { ok = false; break; }
				Vector lp = vs[k]->Pos, wp, vp, rel;
				MatrixXVector(w.mesh->RotMat, &lp, &wp);
				wp.x += w.mesh->IPos.x; wp.y += w.mesh->IPos.y;
				wp.z += w.mesh->IPos.z;
				Vector_Sub(&wp, &cam.view->ISource, &rel);
				MatrixXVector(cam.view->Mat, &rel, &vp);
				if (vp.z <= cam.nearZ) { ok = false; break; }
				const float sx = ( px*vp.x + cam.cntrEX*vp.z) / vp.z;
				const float sy = (-py*vp.y + cam.cntrEY*vp.z) / vp.z;
				bx0 = std::min(bx0, sx); bx1 = std::max(bx1, sx);
				by0 = std::min(by0, sy); by1 = std::max(by1, sy);
			}
			if (!ok) break;
		}
		if (ok && bx1 > bx0 && by1 > by0) {
			// Intersect with the screen: the window can extend past the frame
			// edge, and clone geometry outside the screen is dead regardless.
			wx0 = std::max(0.0f, bx0); wy0 = std::max(0.0f, by0);
			wx1 = std::min(float(xr), bx1); wy1 = std::min(float(yr), by1);
			winValid = (wx1 > wx0 && wy1 > wy0);
		}
	}
	if (winValid) {
		++g_mcensus.winFrames;
		g_mcensus.winAreaFrac += double((wx1-wx0)*(wy1-wy0))
		                       / double(std::max(1, xr) * std::max(1, yr));
	}
	for (const fds::MirrorCloneSubSphere &s : *subs) {
		// View-space sub-centre: OS is the mesh bsphere centre in view space,
		// so add the IM-rotated offset between them.
		Vector off = { s.ctr.x - T->BSphereCtr.x,
		               s.ctr.y - T->BSphereCtr.y,
		               s.ctr.z - T->BSphereCtr.z };
		Vector rot;
		MatrixXVector(IM, &off, &rot);
		const Vector Ss = { OS.x + rot.x, OS.y + rot.y, OS.z + rot.z };
		++g_mcensus.subTotal;
		g_mcensus.subVertsTotal += s.vCount;
		const float R2s = L2 * s.radSq;
		const bool outFrustum = censusSphereOutside(
		    Ss, R2s, px, py, cam.cntrEX, cam.cntrEY, cam.nearZ, cam.farZ);
		if (outFrustum) {
			++g_mcensus.subCulled;
			g_mcensus.subVertsCulled += s.vCount;
		}
		// Window arm: frustum reject OR outside the mirror's own screen rect
		// (a superset of the frustum reject, since the rect is clamped to the
		// screen). Depth still uses the camera's near/far.
		if (winValid) {
			const bool outWin = outFrustum
			    || censusSphereOutsideRect(Ss, R2s, px, py,
			            cam.cntrEX, cam.cntrEY, wx0, wy0, wx1, wy1);
			if (outWin) {
				++g_mcensus.winCulled;
				g_mcensus.winVertsCulled += s.vCount;
			}
		}
	}
}

void mcensusDump() {
	MCensus &c = g_mcensus;
	const double f = double(c.frames > 0 ? c.frames : 1);
	std::fprintf(stderr,
	    "[MIRROR-CULL-CENSUS] n=%lld  clone meshes %.2f/frame  clone verts %.0f/frame"
	    "  | sub-spheres %.1f, frustum-culled %.1f (%.1f%%)"
	    "  | sub verts %.0f, FRUSTUM-CULLABLE %.0f (%.1f%%)"
	    "  | WINDOW-CULLABLE %.0f (%.1f%%) over %lld frames, window %.2f%% of screen\n",
	    (long long)c.frames, c.cloneMeshes / f, c.cloneVerts / f,
	    c.subTotal / f, c.subCulled / f,
	    c.subTotal ? 100.0 * double(c.subCulled) / double(c.subTotal) : 0.0,
	    c.subVertsTotal / f, c.subVertsCulled / f,
	    c.subVertsTotal ? 100.0 * double(c.subVertsCulled) / double(c.subVertsTotal) : 0.0,
	    c.winVertsCulled / f,
	    c.subVertsTotal ? 100.0 * double(c.winVertsCulled) / double(c.subVertsTotal) : 0.0,
	    (long long)c.winFrames,
	    c.winFrames ? 100.0 * c.winAreaFrac / double(c.winFrames) : 0.0);
	c = MCensus{};
}

#endif  // FDS_VIS_CENSUS

void xpDump(int interval) {
	XProfAcc &a = g_xprof;
	const int n = a.frames;
	const double f = double(n);
	double mn[XP_NUM], md[XP_NUM];
	for (int k = 0; k < XP_NUM; ++k) {
		std::sort(a.s[k], a.s[k] + n);
		mn[k] = a.s[k][0] / 1e6;
		md[k] = a.s[k][n / 2] / 1e6;
	}
	// OTHER is derived from the same frame's buckets, so recompute it as the
	// min/median of the per-frame residual rather than differencing order
	// statistics (which need not come from the same frame).
	std::fprintf(stderr,
	    "[XFRM-PROF] n=%d  min: TOTAL %7.3f | SETUP %6.3f VERT %6.3f SOA %6.3f FACE %6.3f  "
	    "p50: TOTAL %7.3f | SETUP %6.3f VERT %6.3f SOA %6.3f FACE %6.3f  ms"
	    "  | meshes %.0f verts %.0f (in %.0f ahead %.0f reg %.0f) fTested %.0f fPushed %.0f\n",
	    n,
	    mn[XP_TOTAL], mn[XP_SETUP], mn[XP_VERT], mn[XP_SOA], mn[XP_FACE],
	    md[XP_TOTAL], md[XP_SETUP], md[XP_VERT], md[XP_SOA], md[XP_FACE],
	    a.meshes / f, a.verts / f, a.vInside / f, a.vAhead / f, a.vRegular / f,
	    a.facesTested / f, a.facesPushed / f);
	a = XProfAcc{};
}
}  // namespace

// Return true when the world-space sphere (center C, radius r) lies
// fully outside the spot cone (apex P, normalized axis D, outer cosine
// cosOuter, max range). Conservative — false-negatives (keeps a sphere
// it could safely cull) are fine; false-positives (culls a sphere that
// should contribute) cause missing shadows, so the test is widened by
// the sphere radius along the cone surface (r/cosOuter at any depth)
// and clamped near the apex.
static inline bool sphereOutsideSpotCone(const Vector& C, float r,
                                          const Vector& P, const Vector& D,
                                          float cosOuter, float maxRange)
{
	const float vx = C.x - P.x;
	const float vy = C.y - P.y;
	const float vz = C.z - P.z;
	const float v2 = vx*vx + vy*vy + vz*vz;

	// Range: a sphere centre farther than (range + r) from the apex
	// can't fall inside the cone within `maxRange` of the apex.
	const float rMax = maxRange + r;
	if (v2 > rMax * rMax) return true;

	// Project onto axis. If the sphere is entirely behind the apex by
	// more than its radius, it can't reach the cone.
	const float distAlongAxis = vx*D.x + vy*D.y + vz*D.z;
	if (distAlongAxis < -r) return true;

	// Cone-vs-sphere at the sphere's depth. Defensively skip the cull
	// for very wide cones (cosOuter near 0) — the math goes wonky and
	// the cull buys little there anyway.
	if (cosOuter < 1e-3f) return false;
	const float sinOuter = std::sqrt(std::max(0.0f, 1.0f - cosOuter * cosOuter));
	const float tanOuter = sinOuter / cosOuter;

	const float depth = distAlongAxis > 0.0f ? distAlongAxis : 0.0f;
	const float coneRadiusAtDepth = depth * tanOuter + r / cosOuter;
	const float perpSq = v2 - distAlongAxis * distAlongAxis;
	return perpSq > coneRadiusAtDepth * coneRadiusAtDepth;
}

// Specialized test for the 90°-FOV cube-face pyramid's circumscribed
// cone (cos = 1/√3 = 0.5774). Called per mesh per cube face per frame
// — at greets's chunked Piramid this is ~10k+ ops, so we sqrt+div
// inline once-deduplicate the trig out. Same conservative shape as
// sphereOutsideSpotCone with cosOuter=0.577; just constant-folded.
static inline bool sphereOutsidePyramidCone(const Vector& C, float r,
                                             const Vector& P, const Vector& D,
                                             float maxRange)
{
	constexpr float kTanOuter = 1.4142135623730951f;  // √2
	constexpr float kInvCos   = 1.7320508075688772f;  // √3 = 1 / (1/√3)
	const float vx = C.x - P.x, vy = C.y - P.y, vz = C.z - P.z;
	const float v2 = vx*vx + vy*vy + vz*vz;
	const float rMax = maxRange + r;
	if (v2 > rMax * rMax) return true;
	const float distAlongAxis = vx*D.x + vy*D.y + vz*D.z;
	if (distAlongAxis < -r) return true;
	const float depth = distAlongAxis > 0.0f ? distAlongAxis : 0.0f;
	const float coneRAtDepth = depth * kTanOuter + r * kInvCos;
	const float perpSq = v2 - distAlongAxis * distAlongAxis;
	return perpSq > coneRAtDepth * coneRAtDepth;
}

// Defined in RENDER.CPP. Forward-declare to avoid pulling the rest of
// RENDER.CPP's prelude in here.
float frand();

void Animate_Objects(Scene *Sc, Camera *cam)
{
	// `cam` is the explicit camera to animate (replaces the old global
	// `View` read + SkipCameraAnimation bool). nullptr = no camera ops,
	// safe to call from off-render-loop threads (e.g. Initialize_X bake
	// prep) without touching globals.
	const bool SkipCameraAnimation = (cam == nullptr);
	Camera *const View = cam;  // alias so the legacy body still reads `View`
	TriMesh *T;
	Omni *Om;
	Object *Obj;
	Vector U,*W,Z;
	Matrix M, PathMat, tmp;
	FILE *F;
    Vector ZeroVector(0.0f, 0.0f, 0.0f);


    //  F = fopen("Matrix.txt","at");
	for (T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&Tri_Possessed) continue;
		Spline_Calc_3D(&T->Pos,CurFrame,&T->IPos);
		Spline_Calc_3D(&T->Scale,CurFrame,&T->IScale);
		//    Vector_Form(&T->IScale,1,1,1); // until i get it right
		// Editor per-object scale knob (TriMesh.h): 0 = unset → authored
		// scale, guarded so the default path stays byte-identical. Applied
		// to the spline RESULT, so it pivots on the object pivot (IPos is
		// offset by Rot·Pivot in the hierarchy pass below) and composes
		// into children through the parent→child matrix chain.
		if (T->EditorScale > 0.0f) Vector_SelfScale(&T->IScale, T->EditorScale);
		
		if (T->Flags&Tri_Euler)
		{
			Spline_Calc_3D(&T->Rotate,CurFrame,&U);
			Euler_Angles(T->RotMat,U.x,U.y,U.z);
		} else {
			Spline_Calc_4D_Alt(&T->Rotate,CurFrame,&T->IRot);
			//Spline_Subdivide_Bezier(&T->Rotate,CurFrame,&T->IRot);
			//    Spline_Calc_4D(&T->Rotate,CurFrame,&T->IRot);
			
			Convert_Quat2Mat(&T->IRot,T->RotMat);
			//    fprintf(F,"%d:((%1.3f,%1.3f,%1.3f),(%1.3f,%1.3f,%1.3f),(%1.3f,%1.3f,%1.3f))\n\n",(int32_t)CurFrame,T->RotMat[0][0],T->RotMat[0][1],T->RotMat[0][2],T->RotMat[1][0],T->RotMat[1][1],T->RotMat[1][2],T->RotMat[2][0],T->RotMat[2][1],T->RotMat[2][2]);
		}
		
		if (T->Flags&Tri_AlignToPath) 
		{
			const float Aheadfactor = 1.0f;
			Spline_Calc_3D(&T->Pos,CurFrame+Aheadfactor,&Z);
			Vector_Sub(&Z, &T->IPos,&U);
			
			float l = Vector_Length(&U);
			
			// replace constant by 
			if (l > Sc->PathingMinVelocity)
			{
				Vector_Copy(&T->Heading, &U);
			}
			
			Kick_Camera(&ZeroVector, &T->Heading, 0.0, PathMat);
			Matrix_Transpose(PathMat);

			Spline_Calc_3D(&T->Rotate, CurFrame, &U);
			Euler_Angles(T->RotMat, 0, 0, U.z);


			MatrixXMatrix(PathMat, T->RotMat, tmp);
			Matrix_Copy(T->RotMat,tmp);
		}
		
		memcpy(T->UnscaledRotMat, T->RotMat, sizeof(T->RotMat));
		W = (Vector *)T->RotMat;
		Vector_SelfScale(W,T->IScale.x); W++;
		Vector_SelfScale(W,T->IScale.y); W++;
		Vector_SelfScale(W,T->IScale.z); W++;
//AfterScale:;
	/*if (T->Status)
	{
		if (CurFrame>T->CurStat->Frame)
		T->CurStat=T->CurStat->Next;
		T->Flags&=0xFFFFFFFF-HTrack_Visible;
		T->Flags|=T->CurStat->Stat;
	} else*/ 	
		// automatic resetting to VISIBLE removed @ 06.04.02 to enable manual modification
		// of hiding state
		//T->Flags|=HTrack_Visible;
	}
	//  fclose(F);
	for(Om=Sc->OmniHead;Om;Om=Om->Next)
	{
		Spline_Calc_3D(&Om->Pos,CurFrame,&Om->IPos);
		if (Om->Size.NumKeys)
			Spline_Calc_1D(&Om->Size,CurFrame,&Om->ISize);
		else
			Om->ISize = 1.0f;
		Spline_Calc_1D(&Om->Range,CurFrame,&Om->IRange);
		Om->rRange = 1.0f/Om->IRange;
		//		Om->IRange*=Om->IRange;
	}
	if (View!=&FC && !SkipCameraAnimation)
	{
		Spline_Calc_3D(&View->Source,CurFrame,&View->ISource);
		Spline_Calc_3D(&View->Target,CurFrame,&View->ITarget);
		Spline_Calc_1D(&View->Roll,CurFrame,&View->IRoll);
		Spline_Calc_1D(&View->FOV,CurFrame,&View->IFOV);
		if (View->Flags & Cam_Euler) {
			//Euler_Angles(View->Mat,View->ITarget.x,View->ITarget.y,View->ITarget.z);
			Euler_Angles(View->Mat, View->ITarget.y, View->ITarget.x, -View->ITarget.z);
		} else {
			Kick_Camera(&View->ISource, &View->ITarget, View->IRoll, View->Mat);
		}
	}

	// Skip the global-View ops entirely when SkipCameraAnimation is set.
	// Callers like Initialize_Greets's bake-prep run on the t1 init thread
	// before any scene's render loop sets the global `View` — touching it
	// here would either crash on null or stomp on whichever scene happens
	// to have set it last. The bake doesn't need PerspX/PerspY or the
	// FOVX/FOVY globals; those are render-loop concerns.
	if (!SkipCameraAnimation && View) {
		CalcPersp(View);
		FOVX = View->PerspX;
		FOVY = View->PerspY;
	}
	
	
	// lalala, HARARCHIA , Ver 3, it now rulati
	for (Obj=Sc->ObjectHead;Obj;Obj=Obj->Next)
	{
		if (Obj->Type == Obj_Omni) {
			Matrix_Identity(*Obj->Rot);
		}
		//if (Obj->Pivot.is_zero()) {
		MatrixXVector(*Obj->Rot, &Obj->Pivot, &U);
//			U.x = U.y = U.z = 0.0f;
		//} else {
			//MatrixXVector(*Obj->Rot, &Obj->Pivot, &U);
		//}
		//printf("%s U: \n", Obj->Name);
		//U.print();
		//printf("\n");
		Vector_SelfSub(Obj->Pos,&U);
		if (Obj->Parent)
		{
			MatrixXVector(*Obj->Parent->Rot,Obj->Pos,&U);
			Vector_Add(Obj->Parent->Pos,&U,Obj->Pos);
			// Skip the rotation compose for omnis. Omni Objects alias
			// `Obj->Rot` to the SHARED global identity matrix `Mat_ID`
			// (FLD_CONV.CPP:114), so `Matrix_Copy(*Obj->Rot, M)` here
			// would write parent.Rot into Mat_ID — corrupting every
			// other reader of the global identity matrix until the
			// next omni iteration's line-224 reset. Omnis have no
			// meaningful rotation anyway; the position composition
			// above is the only piece they need.
			if (Obj->Type != Obj_Omni) {
				MatrixXMatrix(*Obj->Parent->Rot,*Obj->Rot,M);
				Matrix_Copy(*Obj->Rot,M);
			} else {
				// ... except authored SPOT omnis (FLD "LightType 2",
				// ConvertOmni sets Type=Light_SpotLight): their aim is
				// the wrapper-local +Z, so compose it through the
				// parent's rotation each frame — IDir = parentRot·(0,0,1)
				// = parent Rot column 2, normalized to strip the scale
				// the RotMat rows carry (uniform-scale parents; same
				// math as the retired code-created city headlights'
				// UnscaledRotMat read). Unparented spots keep their
				// conversion-time IDir; point omnis have no aim at all.
				Omni *SpotOm = (Omni *)Obj->Data;
				if (SpotOm && SpotOm->Type == Light_SpotLight) {
					Vector d;
					d.x = (*Obj->Parent->Rot)[0][2];
					d.y = (*Obj->Parent->Rot)[1][2];
					d.z = (*Obj->Parent->Rot)[2][2];
					const float l2 = d.x*d.x + d.y*d.y + d.z*d.z;
					if (l2 > 1e-12f) {
						const float il = 1.0f / sqrtf(l2);
						d.x *= il; d.y *= il; d.z *= il;
						SpotOm->IDir = d;
					}
				}
			}
		}
	}
}

void Vertex_Loop1(Vertex *Vert,Vertex *VEnd,Matrix M,Vector *V)
{
	Vertex *Vtx;
	float *f = (float *)M;
	Vector U;
	for (Vtx=Vert;Vtx<VEnd;Vtx++)
	{
		//    if (!Vtx->FRem) continue;
		//    MatrixXVector(M,&Vtx->Pos,&U);
		//    Vector_Add(&U,V,&Vtx->TPos_AOS);
		//    Vtx->TPos_AOS.x = (*f++)*Vtx->Pos.x+(*f++)*Vtx->Pos.y+(*f++)*Vtx->Pos.z+V->x;
		//    Vtx->TPos_AOS.y = (*f++)*Vtx->Pos.x+(*f++)*Vtx->Pos.y+(*f++)*Vtx->Pos.z+V->y;
		//    Vtx->TPos_AOS.z = (*f++)*Vtx->Pos.x+(*f++)*Vtx->Pos.y+(*f)*Vtx->Pos.z+V->z;
		//    f-=8;
		MatrixXVector(M,&Vtx->Pos,&Vtx->TPos_AOS);
		Vector_SelfAdd(&Vtx->TPos_AOS,V);
		
		
		Vtx->RZ=1.0/Vtx->TPos_AOS.z;
		Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
		Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
		Vtx->UZ=Vtx->U*Vtx->RZ;
		Vtx->VZ=Vtx->V*Vtx->RZ;
		Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
		if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
		if (Vtx->PX>=XRes) Vtx->Flags|=Vtx_VisRight;
		if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
		if (Vtx->PY>=YRes) Vtx->Flags|=Vtx_VisDown;
	}
}

void calcVisibilityFlags(Scene* Sc, Vertex* Vtx, fds::CameraContext &cam) {
	Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
	//      if (*(int32_t *)(&Vtx->TPos_AOS.z)>0x3F800000) // 1.0 in floating point rep.
	if (Vtx->TPos_AOS.z > cam.nearZ) {
		Vtx->RZ = 1.0 / Vtx->TPos_AOS.z;
		Vtx->PX = Vtx->TPos_AOS.x * Vtx->RZ;
		Vtx->PY = Vtx->TPos_AOS.y * Vtx->RZ;
		//          Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
		//          Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
		Vtx->UZ = Vtx->U * Vtx->RZ;
		Vtx->VZ = Vtx->V * Vtx->RZ;
		if (Vtx->PX < 0) Vtx->Flags |= Vtx_VisLeft;
		if (Vtx->PX >= XRes) Vtx->Flags |= Vtx_VisRight;
		if (Vtx->PY < 0) Vtx->Flags |= Vtx_VisUp;
		if (Vtx->PY >= YRes) Vtx->Flags |= Vtx_VisDown;
		if (Vtx->TPos_AOS.z > cam.farZ) Vtx->Flags |= Vtx_VisFar;
	} else Vtx->Flags |= Vtx_VisNear;
}

void addParticleTrail(Scene* Sc, fds::FListEntry*& Ins, Particle& p, fds::CameraContext &cam) {
	Vector V;

	Vector VelDir = p.Vel;
	Vector_Norm(&VelDir);

	Vector src = p.V.Pos - p.TrailLength * VelDir;
	Vector targ = p.V.Pos;

	Vector d1 = (src - cam.view->ISource).cross(targ - cam.view->ISource);
	Vector_Norm(&d1);

	int quad_uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	Vector* centerPoints[2] = { &src, &targ };
	Vertex* quad = p.TrailV;
	for (const auto& uv : quad_uvs) {
		float u = float(uv[0]);
		int v = uv[1];
		// interpolate along middle of raindrop quad

		const Vector& tmp = *(centerPoints[v]);
		Vertex& A = *quad;
		V = tmp + d1 * (u - 0.5f) * p.TrailWidth - cam.view->ISource;
		MatrixXVector(cam.view->Mat, &V, &A.TPos_AOS);

		A.TPos_AOS.x = A.TPos_AOS.z * cam.cntrX + A.TPos_AOS.x * cam.fovX;
		A.TPos_AOS.y = A.TPos_AOS.z * cam.cntrY - A.TPos_AOS.y * cam.fovY;
		A.RZ = 1.0f / A.TPos_AOS.z;
		A.PX = A.TPos_AOS.x * A.RZ;
		A.PY = A.TPos_AOS.y * A.RZ;

		A.LA = A.LR = A.LG = A.LB = 255.0;

		calcVisibilityFlags(Sc, &A, cam);
		++quad;
	}

	for (size_t i = 0; i != 2; ++i) {
#ifdef FRONT_TO_BACK_SORTING
		p.TrailF[i].SortZ.F = 2 * cam.farZ - p.V.TPos_AOS.z;
#else
		p.TrailF[i].SortZ.F = cam.farZ - p.V.TPos_AOS.z;
#endif
	}
	*Ins++ = { p.TrailF[0].SortZ.DW, &p.TrailF[0] };
	*Ins++ = { p.TrailF[1].SortZ.DW, &p.TrailF[1] };
}

#define DEBUG_PARTICLES 0

// Per-face front-facing test using view-space vertex positions. Computes
// the triangle's face normal via edge cross-product, then sign-tests against
// vertex A's view-space position. Same test the deferred transparent
// classifier uses (see RenderInnerDeferredTransparent) — face-level, not
// per-vertex, so coplanar triangles of the same quad classify identically.
bool IsFrontFacingInViewSpace(const Face* F)
{
	// SoA Phase 4: read TPos_AOS via F->frame's SoA arrays. Same value as
	// the AoS reads during the dual-write period; will diverge only
	// when Phase 5 drops the AoS TPos_AOS field. F->frame stamped during
	// FList build in Transform_Objects (main: T->frame; shadow:
	// per-clone scratch frame).
	const float *tpos_x = F->frame->TPos_x;
	const float *tpos_y = F->frame->TPos_y;
	const float *tpos_z = F->frame->TPos_z;
	const uint32_t ai = F->A_idx, bi = F->B_idx, ci = F->C_idx;
	const float Ax = tpos_x[ai], Ay = tpos_y[ai], Az = tpos_z[ai];
	const float ex = tpos_x[bi] - Ax;
	const float ey = tpos_y[bi] - Ay;
	const float ez = tpos_z[bi] - Az;
	const float fx = tpos_x[ci] - Ax;
	const float fy = tpos_y[ci] - Ay;
	const float fz = tpos_z[ci] - Az;
	const float nx = ey * fz - ez * fy;
	const float ny = ez * fx - ex * fz;
	const float nz = ex * fy - ey * fx;
	const float vd = nx * Ax + ny * Ay + nz * Az;
	return vd < 0.0f;
}

// View-space max-Z for a transparent triangle, extended to include its quad
// sibling's unshared vertex when present. Without this, the two coplanar
// triangles of a quad get sort keys based on different vertex subsets,
// splitting them across the back-to-front render order so other faces draw
// between them. On a convex transparent mesh (a glass cube) this caused the
// bottom face's two tris to render at positions 4 and 12 in a 12-triangle
// sort, with front walls drawn over the first half and back walls overdrawing
// the second.
//
// "Sibling" = adjacent Face in the same TriMesh with the same Face normal
// sharing ≥2 vertex pointers. Quads built via appendQuad-style code always
// emit their two tris adjacently, so we only need to check F-1 and F+1.
// `facesBase` is the array F was iterated from — which is the per-pass
// CLONE array (tFaces) when VertexScratch is in use, NOT T->Faces. Using
// T->Faces here was a heap-buffer-overflow waiting for ASan: clone arrays
// are separate allocations with different bounds. ASan caught it within
// the first transparent frame of greets.
static float QuadAwareMaxViewZ(const Face* F, const Face* facesBase, DWord facesCount)
{
	// SoA Phase 4: dz read via F->frame's SoA arrays. Sibling search
	// still uses Vertex* identity (the only thing it needs is to
	// detect shared/unshared vertices); when the unshared vert is
	// found, its index is computed via pointer offset off F->A, then
	// looked up in the same frame (sibling is in the same mesh as F
	// by construction — facesBase is one mesh's faces).
	const float *tpos_z = F->frame->TPos_z;
	float dz = tpos_z[F->A_idx];
	if (tpos_z[F->B_idx] > dz) dz = tpos_z[F->B_idx];
	if (tpos_z[F->C_idx] > dz) dz = tpos_z[F->C_idx];

	if (fds::FeatureFlags::no_quad_sort()) return dz;

	auto sameNormal = [](const Face* x, const Face* y) {
		return x->N.x == y->N.x && x->N.y == y->N.y && x->N.z == y->N.z;
	};
	auto sharedCount = [](const Face* x, const Face* y) {
		int n = 0;
		if (y->A == x->A || y->A == x->B || y->A == x->C) ++n;
		if (y->B == x->A || y->B == x->B || y->B == x->C) ++n;
		if (y->C == x->A || y->C == x->B || y->C == x->C) ++n;
		return n;
	};
	auto extendWithSibling = [&](const Face* sib) {
		Vertex* unshared = nullptr;
		if      (sib->A != F->A && sib->A != F->B && sib->A != F->C) unshared = sib->A;
		else if (sib->B != F->A && sib->B != F->B && sib->B != F->C) unshared = sib->B;
		else if (sib->C != F->A && sib->C != F->B && sib->C != F->C) unshared = sib->C;
		if (unshared) {
			// Convert the unshared Vertex* into an index by offset
			// from F->A in the same vertex array. Sibling shares F's
			// mesh (facesBase scope), so same SoA frame applies.
			const uint32_t u_idx = F->A_idx + uint32_t(unshared - F->A);
			if (tpos_z[u_idx] > dz) dz = tpos_z[u_idx];
		}
	};

	const Face* prev = (F > facesBase)                  ? F - 1 : nullptr;
	const Face* next = (F + 1 < facesBase + facesCount) ? F + 1 : nullptr;
	if      (prev && sameNormal(F, prev) && sharedCount(F, prev) >= 2) extendWithSibling(prev);
	else if (next && sameNormal(F, next) && sharedCount(F, next) >= 2) extendWithSibling(next);

	return dz;
}

// Defined in EnvBake.cpp: while an env-reflection panorama bake renders its
// cube faces, moving meshes are excluded (see the inStaticBake predicate)
// and the baked surface's own FACES are excluded (face-level — greets
// merges momy + the whole room into single TriMeshes, so the old whole-mesh
// skip emptied the room out of the probe).
namespace fds { extern int  g_offscreenViewDepth;   // >0 inside a mirror RTT /
                    // env-probe / disco / shatter offscreen render (OffscreenView.cpp)
                extern bool g_envBakeSkipDynamic;
                extern bool g_envBakeSkipAnimatedForce;   // probe rides a mover:
                    // keep the movers out of ITS static capture regardless of
                    // --env_bake_include_animated (EnvBake.cpp's global comment)
                extern bool g_envBakeSkipMirrorClones;
                void EnvBake_NoteMoverInStaticCapture(const char* name, int faces);
                extern bool g_envOverlayDynamicOnly;   // ENVDYN A3: invert the
                    // static-only env bake — render DYNAMIC meshes only, static
                    // skipped (mirrors g_inDynamicShadowBake for the env overlay)
                bool EnvBake_HasSkipFaces();
                bool EnvBake_FaceExcluded(const Face* F, TriMesh* T);
                bool EnvBake_LegacyMeshExcluded(TriMesh* T);
                bool EnvBake_IsMirrorCloneObj(const Object* O);
#if FDS_SHARD_BAKE_LAB
                void ReflFaceCull_Mark(const TriMesh* T, const Face* tFaces,
                                       const Vertex* tVerts, const Vector& AP,
                                       bool offscreenPass, const uint8_t*& keepOut);
#endif
                }

// ── --xfrm_par: mesh-sharded main-view Transform_Objects ─────────────────
// The whole reason this exists: the per-vertex loop is pinned at ONE core's
// streaming ceiling (~64 GB/s measured, docs/SOA_VERTEX_REFACTOR.md
// 2026-08-06 (b)) and the main-view call runs on the tick thread only, so 11
// cores idle through it. Per-mesh work is already independent — the only
// shared mutable state in the mesh loop is the FList append cursor, which is
// why the shard carries its own PRE-RESERVED segment instead of bumping a
// shared cursor: FList order then depends on MESH ORDER, not on which worker
// finishes first, and the radix sort's input is bit-identical to serial.
//
//   kind == Meshes  : process mesh sequence indices [lo, hi), append starting
//                     at fList + insOff, record how many were pushed in
//                     `out`, and RETURN before the omni/particle epilogue.
//   kind == Epilogue: process NO meshes (lo == hi == 0), append the omni
//                     flares + particles starting at fList + insOff (which
//                     the driver sets to the compacted poly count) and set
//                     cAll / cOmnies / cPcls exactly as the serial path does.
//                     cPolys is set by the driver, not here.
//
// Serial callers (and every shadow / mirror-RTT / env-probe re-entry) see
// g_xfrmShard == nullptr and take lo=0 / hi=INT_MAX / insOff=0, i.e. the
// original code path with an unconditional counter+compare in the mesh-loop
// preamble. Deliberately NOT a flag-predicated branch: a never-taken
// `if (flag)` inside this -ffp-contract=fast function is not byte-null
// (docs/VISIBILITY_PLAN.md 8a — 216 bytes on city, max |delta| 44).
namespace {
struct XfrmShard {
	int      lo   = 0;         // first mesh sequence index (inclusive)
	int      hi   = 0;         // last  mesh sequence index (exclusive); 0 => epilogue-only
	int32_t  insOff = 0;       // FList slot this shard starts appending at
	int32_t  out  = 0;         // [out] entries this shard actually pushed
};
thread_local const XfrmShard *g_xfrmShard = nullptr;
}  // namespace

static void Transform_Objects_Sharded(Scene *Sc, fds::CameraContext &cam,
                                     fds::FaceListContext &faces, int nShards);

// xresOverride / yresOverride: when >= 0, use these instead of the
// global XRes / YRes for vertex visibility flags + face-level
// VisibilityFlagsAll() culling. Lets a caller (e.g. the shadow-map
// orchestrator) project to a different screen rect than the main
// framebuffer without globally mutating XRes/YRes mid-frame. Default
// (-1, -1) preserves legacy behavior.
void Transform_Objects(Scene *Sc, fds::CameraContext &cam, fds::FaceListContext &faces,
                        int xresOverride, int yresOverride,
                        fds::VertexScratch *scratch)
{
	extern thread_local bool g_inShadowPass;
	// Hoist the thread_local load out of the per-vertex hot loops below
	// (Aft / Ahead / Regular paths). macOS TLS access goes through
	// __tls_get_addr or an emutls trampoline — multiple cycles each, and
	// the compiler can't CSE the load across the `MatrixXVector` calls
	// in the body. Caching here turns it into a register read inside the
	// per-vertex `if (!_inShadowPass)` checks at the three sites below.
	const bool _inShadowPass = g_inShadowPass;
	// S1 offscreen proxy predicate: true in the shadow bake AND in any
	// mirror-RTT / env-probe / disco / shatter offscreen render. The greets
	// displaced-stone proxy swap keys on this — offscreen consumers rasterise
	// the flat Tri_OffscreenProxy stand-in and skip the Face_MainOnly displaced
	// detail; the main camera does the reverse. Cheap register read; the flags
	// it gates are only set under --greets_displace, so default scenes never
	// carry either flag and this is inert.
	const bool _offscreenPass = g_inShadowPass || (fds::g_offscreenViewDepth > 0);
	// Phase B chunk occlusion + vis-stats: MAIN VIEW ONLY (off-axis mirror RTT
	// sets g_offAxisFrustumCull; offscreen bakes set _offscreenPass). Cached once
	// so the per-mesh hot loop pays a register read, not a flag/TLS lookup.
	const bool _mainView   = !_offscreenPass && !fds::g_offAxisFrustumCull;
	// --xfrm_par: the MAIN-VIEW call is the one that runs on the tick thread
	// alone. Hand it to the sharded driver (which re-enters this function once
	// per shard with g_xfrmShard set, plus once for the omni/particle
	// epilogue) and return. Excluded: every pass that is ALREADY parallel or
	// already re-entrant (shadow bakes, mirror RTT, env/SH probes — they run
	// on pool workers and would nest dispatches), resolution-overridden probe
	// renders, and scratch (clone) passes.
	const XfrmShard *const _shard = g_xfrmShard;
	if (!_shard && _mainView && !scratch && xresOverride < 0 && yresOverride < 0) {
		const int _parN = fds::FeatureFlags::xfrm_par();
		if (_parN != 0) {
			Transform_Objects_Sharded(Sc, cam, faces, _parN);
			return;
		}
	}
	// Shard window + FList append base. nullptr (serial / shadow / offscreen)
	// => [0, INT_MAX) at offset 0, i.e. every mesh, appending from slot 0.
	const int     _shardLo    = _shard ? _shard->lo     : 0;
	const int     _shardHi    = _shard ? _shard->hi     : INT_MAX;
	const int32_t _shardInsOff= _shard ? _shard->insOff : 0;
	int           _mseq       = 0;   // running mesh sequence index
	const bool _occlCull   = _mainView && fds::g_chunkOcclActive;
	// g_chunkVisStats is a plain (non-atomic) counter block, so the shards
	// must not touch it — the sharded driver re-runs the stats serially is NOT
	// worth it for a debug counter; --vis_stats simply reports nothing under
	// --xfrm_par. Same reasoning for --xfrm_prof / --xfrm_pass_prof below.
	const bool _visStats   = _mainView && fds::g_visStatsActive && !_shard;
	const bool coneCull = g_inShadowPass
		&& fds::FeatureFlags::shadow_cone_cull()
		&& g_currentShadowOmni
		&& g_currentShadowOmni->Type == Light_SpotLight;
	extern thread_local ShadowMap *g_currentShadowMap;
	// Per-cube-face bsphere cull: when xforming geometry into one of the
	// 6 cube faces of an Omni shadow, the face only sees a 90°-FOV pyramid
	// along one cardinal axis. Most meshes are outside that pyramid (an
	// omni's 6 faces partition the world into 6 disjoint hemispheres, so
	// the average mesh is visible to ~1 face out of 6). Skip the per-vertex
	// transform + face submission for meshes whose bsphere is entirely
	// outside this face's pyramid. Reuses sphereOutsideSpotCone with the
	// CIRCUMSCRIBED cone of the 90° pyramid (half-angle ≈ 54.7°, cos ≈
	// 0.577) so we never cull a sphere that could be visible at a corner.
	const bool cubeFaceCull = g_inShadowPass
		&& fds::FeatureFlags::shadow_cube_face_cull()
		&& g_currentShadowOmni
		&& g_currentShadowOmni->Type == Light_Omni
		&& g_currentShadowMap
		&& g_currentShadowMap->cubeFace >= 0;
	Vector cubeFaceDir = { 0.0f, 0.0f, 0.0f };
	Vector cubeFacePos = { 0.0f, 0.0f, 0.0f };
	float  cubeFaceCos = 0.0f;
	float  cubeFaceRange = 0.0f;
	if (cubeFaceCull) {
		// Cube face order matches CubeShadowMaps_Rebuild + the per-face
		// camera setup in Render_DeferredShadowMaps:
		// 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
		switch (g_currentShadowMap->cubeFace) {
			case 0: cubeFaceDir.x =  1.0f; break;
			case 1: cubeFaceDir.x = -1.0f; break;
			case 2: cubeFaceDir.y =  1.0f; break;
			case 3: cubeFaceDir.y = -1.0f; break;
			case 4: cubeFaceDir.z =  1.0f; break;
			case 5: cubeFaceDir.z = -1.0f; break;
		}
		cubeFacePos = g_currentShadowOmni->IPos;
		// Circumscribed cone of the 90° square pyramid: half-angle =
		// atan(sqrt(2)) ≈ 54.7°, so cos ≈ 0.577. Conservative (lets
		// through more spheres than a tight pyramid test); never under-
		// culls. Tight 4-plane pyramid test would be ~2× the math for
		// maybe ~30% tighter cull; not worth it at this granularity.
		cubeFaceCos = 0.577f;
		cubeFaceRange = g_currentShadowOmni->IRange;
	}
	extern thread_local bool g_inDynamicShadowBake;
	// Two shadow-bake mesh filters:
	//   inStaticBake : skip dynamic meshes (their t=0 silhouette would
	//                  freeze in the once-baked static map).
	//   inDynamicBake: skip static meshes (the static bake already has
	//                  them; the per-frame dynamic pass only contains
	//                  the moving parts).
	// Same isDynamicForBake() predicate decides both.
	// Env-reflection bake (EnvBake.cpp): the panorama is a STATIC capture, so
	// moving meshes (the walking mech) must not be frozen into it — they'll
	// have walked away by the time the reflection is seen. Same predicate as
	// the static shadow bake.
	//
	// --env_bake_include_animated (docs/SHADING_CONTRACT.md §11 row E6,
	// default OFF = byte-null) drops ONLY this term. It deliberately does not
	// clear g_envBakeSkipDynamic itself: that global also drives the legacy
	// whole-mesh exclusion (:1549) and the reflector's OWN-FACE skip (:2396),
	// and clearing it lets a reflector's own canopy glass into its own probe
	// (measured on greets' `cockpit`: +Y face 91 % VOID, probe mean 100.31 →
	// 49.11). This is the animated-mesh rule and nothing else.
	//
	// …EXCEPT for the one probe the flag's own justification does not cover:
	// one that RIDES a mover. env_bake_include_animated argues the mech
	// belongs in the mech's own canopy probe because "the hull/barrels/legs do
	// not move relative to it" — true of the pose, false of the DISTANCE once
	// --env_probe_follow_owner glues the capture point onto the canopy itself.
	// From there the owner's own limbs are near-plane-clipped slabs filling
	// every cube face (the user's "random polys"). g_envBakeSkipAnimatedForce
	// is set only for those bakes and only under that flag, and the movers are
	// not lost — --env_dynamic's overlay draws them live at the current pose.
	const bool inStaticBake = (g_inShadowPass
		&& g_currentShadowOmni
		&& (g_currentShadowOmni->Flags & Omni_StaticShadow)
		&& !g_inDynamicShadowBake
		&& fds::FeatureFlags::shadow_skip_animated())
		|| (fds::g_envBakeSkipDynamic
		    && (!fds::FeatureFlags::env_bake_include_animated()
		        || fds::g_envBakeSkipAnimatedForce));
	// inDynamicBake keeps DYNAMIC meshes only, skipping static ones. The
	// shadow dynamic bake sets it via g_inDynamicShadowBake; the ENVDYN env
	// overlay (A3) sets it via g_envOverlayDynamicOnly (not a shadow pass, so
	// gate independently of g_inShadowPass).
	const bool inDynamicBake = (g_inShadowPass && g_inDynamicShadowBake)
		|| fds::g_envOverlayDynamicOnly;
	// Normalize the cone axis once: the shadow lighting kernel does the
	// same for its per-pixel cone test (see StaticLighting), so the
	// authored IDir is not guaranteed unit-length in world space.
	Vector coneDir = { 0.0f, 0.0f, 0.0f };
	Vector conePos = { 0.0f, 0.0f, 0.0f };
	float  coneCosOuter = 0.0f;
	float  coneRange    = 0.0f;
	if (coneCull) {
		coneDir = g_currentShadowOmni->IDir;
		Vector_Norm(&coneDir);
		conePos      = g_currentShadowOmni->IPos;
		coneCosOuter = g_currentShadowOmni->FallOff;
		coneRange    = g_currentShadowOmni->IRange;
	}
	const int32_t xr = (xresOverride >= 0) ? xresOverride : XRes;
	const int32_t yr = (yresOverride >= 0) ? yresOverride : YRes;
	TriMesh *T;
	Omni *O;
	Matrix M,IM;
	float M34[3][4];
	Vector AP,S,U,OS,V,*W=(Vector *)(&M),*W2,*Scl;
	float L1,L2,L3;
	Vertex *Vtx,*VEnd;
	uint32_t vfi;              // running vertex index for the inline SoA store
	Face *F,*FEnd;
	float PX=cam.fovX,PY=cam.fovY,Temp;
	float dz;
	int32_t *pdz = (int32_t *)(&dz);
	int32_t I;
	fds::FListEntry *Ins = faces.fList + _shardInsOff;
	float *f = (float *)(&M);
	float *fv;

	float fzp = cam.farZ;

	// XFRM front-end profiler / ablations (--xfrm_prof, --xfrm_ablate).
	// MAIN-VIEW ONLY: every offscreen pass (shadow bake, mirror RTT, env probe)
	// and every off-axis shard pass is excluded, so the accumulator needs no
	// synchronisation and the numbers describe the frame the user is looking
	// at. `xresOverride < 0` additionally excludes the resolution-overridden
	// probe renders. Timer locals live at FUNCTION scope so the
	// Inside/Ahead/Regular `goto`s never jump over an initialisation.
	const int  _xprofN   = (_mainView && !_shard) ? fds::FeatureFlags::xfrm_prof() : 0;
	const int  _xablate  = _mainView ? fds::FeatureFlags::xfrm_ablate() : 0;
	const bool xp        = (_xprofN > 0) && (xresOverride < 0);
	const bool xab       = (_xablate != 0) && (xresOverride < 0);
	const bool xabNoTN   = xab && (_xablate & XAB_NO_TN);
	const bool xabNoProj = xab && (_xablate & XAB_NO_PROJ);
	const bool xabNoSoa  = xab && (_xablate & XAB_NO_SOA);
	const bool xabNoFace = xab && (_xablate & XAB_NO_FACE);
	const int64_t xpT0   = xp ? xpNow() : 0;   // whole-call
	int64_t xpTM = 0, xpTV = 0, xpTS = 0, xpTF = 0;

#if FDS_VIS_CENSUS
	// Per-PASS census (--xfrm_pass_prof). Unlike xfrm_prof this runs in EVERY
	// pass (mirror RTT, shadow cube faces, env/SH probe faces) so the whole
	// frame's geometry front-end is attributed. Kind is derived from the same
	// three pass predicates the culls key on.
	const int  _xpassN = _shard ? 0 : fds::FeatureFlags::xfrm_pass_prof();
	const bool xpass   = (_xpassN > 0);
	const int  xpKind  = _mainView ? XPK_MAIN
	                   : (fds::g_offAxisFrustumCull ? XPK_MIRROR_RTT
	                   : (_inShadowPass ? XPK_SHADOW : XPK_OFFSCREEN));
	const int64_t xpassT0 = xpass ? xpNow() : 0;
	int64_t xpassMeshSeen = 0, xpassMeshX = 0, xpassVSeen = 0, xpassVX = 0;
	// Per-mesh / per-material decomposition of the same census. `_xms` lives
	// at FUNCTION scope for the same reason the xprof timers do: the mesh loop
	// below contains Inside/Ahead/Regular gotos and must not jump over an
	// initialisation.
	const bool xmesh = xpass && (fds::FeatureFlags::xfrm_pass_mesh_prof() > 0);
	XMeshSlot *_xms = nullptr;

	// Mirror-clone split ceiling census (--mirror_cull_census). Main view only
	// — the RTT bakes HIDE every clone mesh, so a clone is never in an
	// offscreen pass and the ceiling question is entirely a main-view one.
	const int  _mcensusN = (_mainView && !_shard) ? fds::FeatureFlags::mirror_cull_census() : 0;
	const bool _mcensus  = (_mcensusN > 0) && (xresOverride < 0);
#endif  // FDS_VIS_CENSUS

	// SoA Phase 2a — INLINE SoA STORE (--xfrm_soa_inline).
	// Phase 1 shipped the AoS->SoA dual write as a SEPARATE post-pass sweep
	// (VertexFrame_DumpFromAoS at AfterXForm): a second walk over the mesh's
	// whole Vertex array, re-reading 16 of the 136 bytes per vertex that the
	// per-vertex loop had just written. Measured at greets t=5780 with
	// --greets_displace that sweep is ~2.5 ms of a ~8.1 ms main-view
	// Transform_Objects (docs/SOA_VERTEX_REFACTOR.md) — the mesh no longer
	// fits in cache at 958 k verts/frame, so the sweep is a second pass over
	// ~130 MB of cache lines. With this on, each per-vertex loop stores the
	// same four values into the SoA arrays as it goes and the sweep is
	// skipped. BIT-EXACT BY CONSTRUCTION: same values, same source, just
	// stored one loop earlier — VertexFrame contents are identical.
	const bool soaInline = fds::FeatureFlags::xfrm_soa_inline();
	const int  rcpMode   = fds::FeatureFlags::xfrm_rcp();

#if not(DEBUG_PARTICLES)
	Object *Obj; 
//	for (T=Sc->TriMeshHead;T;T=T->Next)
	for(Obj=Sc->ObjectHead; Obj; Obj=Obj->Next)
	{
		
		if (Obj->Type != Obj_TriMesh) continue;
		//if (stricmp(Obj->Name, "water.lwo")) continue;
		// --xfrm_par shard window. The sequence index counts TriMesh objects in
		// OBJECT-LIST order and is incremented before any cull, so the same
		// mesh gets the same index in every shard and in the serial path — that
		// is what makes the FList segment reservation (computed by the driver
		// from the identical walk) line up with what each shard actually
		// appends. Serial: [0, INT_MAX), so this is one add + two compares per
		// mesh, always taken.
		{
			const int _ms = _mseq++;
			if (_ms < _shardLo || _ms >= _shardHi) continue;
		}
		T = (TriMesh *)(Obj->Data);
		if (xp) xpTM = xpNow();   // per-mesh setup starts here

		// FACELESS MESH SKIP. Everything Transform_Objects produces is FList
		// entries, and every FList entry comes from a Face — so a mesh with
		// FIndex == 0 cannot emit one, and its per-vertex transform output is
		// written into arrays nothing will ever read. Not hypothetical: the
		// greets Piramid chunk split "retires" the parent mesh by zeroing
		// FIndex while deliberately keeping its arrays alive (GREETS.CPP
		// ~2540), leaving 16 596 verts on the object list with nothing to
		// draw. Measured at t=5743, 1920x1080, shell arm: that ONE mesh is
		// 365 112 of 540 706 shadow-pass verts (67.5 %), 16 596 of 82 639
		// main-view verts (20.1 %) and 149 364 of 309 740 offscreen verts
		// (48.2 %) — every frame, purely to produce nothing. Unconditional
		// rather than flag-gated on purpose: "no faces => no output" is an
		// invariant of this loop, not a tunable, and a never-taken runtime
		// branch inside this -ffp-contract=fast function is itself not
		// byte-null (docs/VISIBILITY_PLAN.md 8).
		if (T->FIndex == 0) continue;

		// Non-shadow-casting meshes (Tri_NoShadowCast — e.g. the disco ball):
		// excluded from every shadow occluder pass. _inShadowPass covers the
		// static bake, the dynamic per-frame bake, and moving-omni cube re-bakes.
		if (_inShadowPass && (T->Flags & Tri_NoShadowCast)) continue;

		// S1 offscreen proxy: the flat stone stand-in renders ONLY offscreen
		// (shadow / RTT / env). Skip it in the main camera pass so it never
		// double-draws over the displaced detail. Inert unless --greets_displace
		// built one (nothing sets Tri_OffscreenProxy otherwise).
		if (!_offscreenPass && (T->Flags & Tri_OffscreenProxy)) continue;

		uint32_t frustumFlags = 0;  // Tri_Invisible | Tri_Ahead | Tri_Inside, racy
		                            // when T->Flags is shared across N parallel
		                            // shadow-render tasks. Hold locally per call.

		if (!(T->Flags&HTrack_Visible)) {frustumFlags|=Tri_Invisible; continue;}

		// vis-stats: count every main-view mesh that is a real transform
		// candidate (past the proxy/visibility skips), before the frustum +
		// occlusion culls. meshesXformed is stamped after both culls below.
		if (_visStats) {
			++fds::g_chunkVisStats.meshesSeen;
			fds::g_chunkVisStats.vertsSeen += T->VIndex;
		}
#if FDS_VIS_CENSUS
		if (xpass) { ++xpassMeshSeen; xpassVSeen += T->VIndex; }
		_xms = nullptr;
		if (xmesh) {
			_xms = xmSlotFor(T, Obj->Name);
			if (_xms) {
				_xms->seen[xpKind].fetch_add(1, std::memory_order_relaxed);
				_xms->seenV[xpKind].fetch_add(T->VIndex, std::memory_order_relaxed);
			}
		}
#endif

		// Static-bake filter: skip meshes whose *position* animates
		// (Pos spline has more than 1 key). Their t=0 silhouette would
		// otherwise be frozen in the never-rebaked shadow as they move.
		// We deliberately don't check Rotate/Scale — many FLD scenes
		// author no-op rotate/scale envelopes (NumKeys=2 with identical
		// values) on otherwise-static meshes, which would over-filter.
		// A rotating-in-place mesh will have a slightly wrong shadow
		// but won't disappear; pure translation is the bigger artifact.
		// Per-mesh "is dynamic" decision:
		//   1. Own Pos spline extent > ε (mesh translates)
		//   2. Own Rotate spline has more than 1 key (mesh rotates;
		//      we don't compute quaternion extent — just trust NumKeys)
		//   3. Any ancestor moves (parent's IPos changes drive ours)
		// Threshold of 0.1 in scene units handles the (~0.005) noise
		// that FLD often authors as 2-key Pos envelopes for static
		// meshes (e.g. Piramid.lwo in greets).
		auto isDynamicForBake = [](Object *obj) -> bool {
			constexpr float kPosExtentEps = 0.1f;
			constexpr float kRotExtentEps = 0.01f;   // unit-quat delta
			for (Object *o = obj; o; o = o->Parent) {
				if (o->Type != Obj_TriMesh) continue;
				TriMesh *tm = (TriMesh *)o->Data;
				if (!tm) continue;
				if (tm->Pos.NumKeys > 1 && tm->Pos.Keys) {
					const auto& k0 = tm->Pos.Keys[0].Pos;
					float xmin=k0.x, xmax=k0.x, ymin=k0.y, ymax=k0.y, zmin=k0.z, zmax=k0.z;
					for (DWord i = 1; i < tm->Pos.NumKeys; ++i) {
						const auto& k = tm->Pos.Keys[i].Pos;
						if (k.x < xmin) xmin=k.x; if (k.x > xmax) xmax=k.x;
						if (k.y < ymin) ymin=k.y; if (k.y > ymax) ymax=k.y;
						if (k.z < zmin) zmin=k.z; if (k.z > zmax) zmax=k.z;
					}
					if ((xmax-xmin) > kPosExtentEps ||
					    (ymax-ymin) > kPosExtentEps ||
					    (zmax-zmin) > kPosExtentEps) return true;
				}
				// Rotate spline stores a Quaternion in Keys[i].Pos
				// (x,y,z,W). Many FLD scenes author no-op 2-key Rotate
				// envelopes — extent-check on all four components to
				// keep those classified as static.
				if (tm->Rotate.NumKeys > 1 && tm->Rotate.Keys) {
					const auto& q0 = tm->Rotate.Keys[0].Pos;
					float xmin=q0.x, xmax=q0.x, ymin=q0.y, ymax=q0.y;
					float zmin=q0.z, zmax=q0.z, wmin=q0.W, wmax=q0.W;
					for (DWord i = 1; i < tm->Rotate.NumKeys; ++i) {
						const auto& q = tm->Rotate.Keys[i].Pos;
						if (q.x < xmin) xmin=q.x; if (q.x > xmax) xmax=q.x;
						if (q.y < ymin) ymin=q.y; if (q.y > ymax) ymax=q.y;
						if (q.z < zmin) zmin=q.z; if (q.z > zmax) zmax=q.z;
						if (q.W < wmin) wmin=q.W; if (q.W > wmax) wmax=q.W;
					}
					if ((xmax-xmin) > kRotExtentEps ||
					    (ymax-ymin) > kRotExtentEps ||
					    (zmax-zmin) > kRotExtentEps ||
					    (wmax-wmin) > kRotExtentEps) return true;
				}
			}
			return false;
		};
		// Prefer the bake cache over re-walking spline keys: primed once
		// per bake call (Shadows.cpp), same predicate, valid while the
		// generation stamps match. Snapshot/one-off callers that bake
		// without priming fall back to the walk.
		const bool bakeCacheValid = _inShadowPass
		    && T->BakeCacheGen == g_shadowBakeGen;
		auto meshDynForBake = [&]() -> bool {
			return bakeCacheValid ? (T->BakeIsDynamic != 0)
			                      : isDynamicForBake(Obj);
		};
		// Symmetric counterpart for the dynamic per-frame bake: skip
		// meshes whose Pos/Rotate splines are effectively static —
		// those already live in the once-baked static map.
		if (inDynamicBake && !meshDynForBake()) {
			static std::atomic<int> sDynSkip{0};
			if (sDynSkip.fetch_add(1) < 32) {
				std::fprintf(stderr,
				    "[DYN-BAKE-SKIP-MESH] '%s' (static)\n",
				    (Obj->Name ? Obj->Name : "?"));
			}
			continue;
		}
		if (inDynamicBake) {
			static std::atomic<int> sDynKeep{0};
			if (sDynKeep.fetch_add(1) < 32) {
				std::fprintf(stderr,
				    "[DYN-BAKE-KEEP-MESH] '%s' Pos.NumKeys=%u Faces=%u\n",
				    (Obj->Name ? Obj->Name : "?"),
				    unsigned(T->Pos.NumKeys), unsigned(T->FIndex));
			}
		}
		// Env-bake self-exclusion. Under --env_bake_fix it moved to FACE
		// level (the face-submission loop below, EnvBake_FaceExcluded): the
		// probe is captured from the material's centroid — often INSIDE its
		// own object — so the reflector's own faces must not appear in it
		// (classic local-cubemap rule), but skipping the whole MESH nuked
		// every OTHER surface sharing the TriMesh (greets merges momy + the
		// entire room into one mesh → near-black probes, black sibling
		// instances). The LEGACY whole-mesh skip below is kept byte-identical
		// for the default path (city vehicle-glass probes / pinned baseline).
		if (fds::g_envBakeSkipDynamic && fds::EnvBake_LegacyMeshExcluded(T)) continue;
		// --env_bake_fix probe bakes: mirror-clone meshes ("__mirrorClone_*",
		// the reflected copy of the whole room) stay out of the probe
		// STRUCTURALLY. They'd only ever be per-pixel mask-rejected (clone
		// faces need gb.mirrorId == their tag, and the bake neutralizes the
		// mask to 0) — wasted raster at best, committed clone pixels if the
		// mask isn't stamped yet. Flag is bake-scoped (set only while a
		// publishProj bake renders its faces), so the legacy bake path and
		// the disco-ball panorama stay byte-identical.
		if (fds::g_envBakeSkipMirrorClones && fds::EnvBake_IsMirrorCloneObj(Obj)) continue;
		if (inStaticBake && meshDynForBake()) {
			static std::atomic<int> sSkipLogged{0};
			if (sSkipLogged.fetch_add(1) < 32) {
				std::fprintf(stderr,
				    "[STATIC-BAKE-SKIP-MESH] '%s' Pos.NumKeys=%u Rot.NumKeys=%u parent='%s'\n",
				    (Obj->Name ? Obj->Name : "?"),
				    unsigned(T->Pos.NumKeys), unsigned(T->Rotate.NumKeys),
				    (Obj->Parent && Obj->Parent->Name) ? Obj->Parent->Name : "(none)");
			}
			continue;
		}
		if (inStaticBake) {
			static std::atomic<int> sKeepLogged{0};
			if (sKeepLogged.fetch_add(1) < 32) {
				std::fprintf(stderr, "[STATIC-BAKE-KEEP-MESH] '%s' Pos.NumKeys=%u Rot.NumKeys=%u Faces=%u\n",
					(Obj->Name ? Obj->Name : "?"), unsigned(T->Pos.NumKeys),
					unsigned(T->Rotate.NumKeys), unsigned(T->FIndex));
			}
		}
		// THE INVARIANT CENSUS (EnvBake.cpp). Any mesh the dynamic predicate
		// calls a MOVER that survives this far in an ENV static capture is
		// content frozen at bake time. Harmless for a legacy (non-overlaid)
		// probe; a duplicate of the live copy, and the user-visible "some mech
		// parts do not change height", for one --env_dynamic overlays. Counted
		// here rather than asserted, so the bake can NAME what it kept.
		// Env static bakes only — g_envBakeSkipDynamic is false in the shadow
		// bakes and in the dynamic-only overlay.
		if (fds::g_envBakeSkipDynamic && meshDynForBake())
			fds::EnvBake_NoteMoverInStaticCapture(Obj->Name, int(T->FIndex));

		// Mesh-bsphere-vs-cone cull during shadow-pass Transform_Objects.
		// Cheap pre-test that lets us skip the matrix work + vertex
		// transform + face faces.fList submission for any mesh whose bsphere
		// can't contribute to this light's shadow map. Saves both the
		// per-vertex xform and the per-face raster work downstream.
		// The world bsphere comes from the bake cache when primed (see
		// Render_DeferredShadowMaps): identical across every face task of
		// the call, so the per-face MatrixXVector re-derivation is skipped.
		if (coneCull) {
			Vector wsBsphereCtr;
			if (bakeCacheValid) {
				wsBsphereCtr = T->BakeWsBSphereCtr;
			} else {
				MatrixXVector(T->RotMat, &T->BSphereCtr, &wsBsphereCtr);
				Vector_SelfAdd(&wsBsphereCtr, &T->IPos);
			}
			if (sphereOutsideSpotCone(wsBsphereCtr, T->BSphereRadius,
			                          conePos, coneDir, coneCosOuter, coneRange)) {
				frustumFlags |= Tri_Invisible;
				continue;
			}
		}
		// Per-cube-face cull. Same shape of test as the spot-cone cull,
		// but the "cone" is the 90°-FOV pyramid of one of 6 cube faces.
		// No T->Flags write — multiple cube-face xform tasks run in
		// parallel on the same TriMesh; flipping a shared bit would race
		// (each task wants a per-face decision, not a global one).
		if (cubeFaceCull) {
			Vector wsBsphereCtr;
			if (bakeCacheValid) {
				wsBsphereCtr = T->BakeWsBSphereCtr;
			} else {
				MatrixXVector(T->RotMat, &T->BSphereCtr, &wsBsphereCtr);
				Vector_SelfAdd(&wsBsphereCtr, &T->IPos);
			}
			// cubeFaceCos = 0.577 by construction; use the constant-
			// folded variant to skip the sqrt + divide per call.
			if (sphereOutsidePyramidCone(wsBsphereCtr, T->BSphereRadius,
			                             cubeFacePos, cubeFaceDir, cubeFaceRange)) {
				continue;
			}
		}

		// Per-pass clone redirection. With scratch non-null, all
		// reads/writes of this TriMesh's per-vertex projection state
		// (Vertex::PX, PY, RZ, TPos_AOS, TN, TTangent, Flags…)
		// land in scratch's clone instead of T's own Verts. Face
		// pushes also use the clone's Face* (whose A/B/C point into
		// the clone's Verts), so downstream code sees a coherent
		// per-pass snapshot. Nullptr → in-place writes to tVerts
		// (main pass keeps the no-allocation fast path).
		// Fetched AFTER the culls: a culled mesh must not pay the clone
		// lookup — nor the one-time full vertex/face copy cloneOf does on
		// first use, which for a mesh never visible from this light's
		// face was pure wasted memory + bandwidth.
		Vertex *tVerts;
		Face   *tFaces;
		if (scratch) {
			auto& clone = scratch->cloneOf(T);
			tVerts = clone.verts.data();
			tFaces = clone.faces.data();
		} else {
			tVerts = T->Verts;
			tFaces = T->Faces;
		}
#if FDS_SHARD_BAKE_LAB
		const uint8_t *fcKeep = nullptr;
#endif

		// SoA Phase 2a: resolve the VertexFrame here (after the culls, before
		// the per-vertex loops) so the loops can store straight into it. Same
		// resolution the AfterXForm sweep does; when soaInline is off these
		// stay null and the sweep runs exactly as before.
		float *soaX = nullptr, *soaY = nullptr, *soaZ = nullptr, *soaPY = nullptr;
		if (soaInline) {
			VertexFrame *FI_ = nullptr;
			if (scratch) {
				FI_ = &scratch->cloneOf(T).frame;   // cloneOf already ensureSized it
			} else {
				if (!T->frame) T->frame = new VertexFrame();
				T->frame->ensureSized(int(T->VIndex));
				if (T->frame->capacity >= int(T->VIndex)) FI_ = T->frame;
			}
			if (FI_ && FI_->capacity >= int(T->VIndex)) {
				soaX = FI_->TPos_x; soaY = FI_->TPos_y;
				soaZ = FI_->TPos_z; soaPY = FI_->PY;
			}
		}

		MatrixXMatrix(cam.view->Mat,T->RotMat,M);
		Matrix_Copy(IM,M);
		// Advanced Matrix...(watch this)
		Vector_Scale(W,PX,W);
		Vector_Scale(W+1,-PY,W+1);
		Vector_Scale(W+2,cam.cntrEX,&V);
		Vector_SelfAdd(W,&V);
		Vector_Scale(W+2,cam.cntrEY,&V);
		Vector_SelfAdd(W+1,&V);
		// Supermatrix ready.
		
		// postrioric Offset Vector.
		Vector_Sub(&T->IPos,&cam.view->ISource,&U);
		MatrixXVector(cam.view->Mat,&U,&S);

		V.x = cam.cntrEX*S.z+PX*S.x;
		V.y = cam.cntrEY*S.z-PY*S.y;
		V.z = S.z;

		// make a corrected sphere center vector
		MatrixXVector(IM,&T->BSphereCtr,&AP);
		Vector_SelfAdd(&S,&AP);
		// Cache the view-space z and radius for the transparent sort's
		// object-level grouping (used in the Mat_Transparent branch below).
		const float objBSphereViewZ = S.z;
		const float objBSphereRadius = T->BSphereRadius;

		Vector_Copy(&OS, &S);


		//    Vector_Copy(&V,&S);
		// ready
		// 4x3 AFFINE XFORM
		M34[0][0] = M[0][0]; M34[0][1] = M[0][1]; M34[0][2] = M[0][2]; M34[0][3] = V.x;
		M34[1][0] = M[1][0]; M34[1][1] = M[1][1]; M34[1][2] = M[1][2]; M34[1][3] = V.y;
		M34[2][0] = M[2][0]; M34[2][1] = M[2][1]; M34[2][2] = M[2][2]; M34[2][3] = V.z;
		// ready

		// Column-major SIMD staging for the per-vertex M34 * (Pos, 1).
		// Each column is loaded once into a Vec4f; per-vertex compute
		// becomes 3 broadcast-FMAs (vfmaq_n_f32-equivalent) instead of
		// the 9 scalar muls + 9 scalar adds the row-major form requires.
		// 4th lane is unused (kept 0); it falls out when storing TPos_AOS.
		alignas(16) const float m34_col_x_arr[4] = { M[0][0], M[1][0], M[2][0], 0.0f };
		alignas(16) const float m34_col_y_arr[4] = { M[0][1], M[1][1], M[2][1], 0.0f };
		alignas(16) const float m34_col_z_arr[4] = { M[0][2], M[1][2], M[2][2], 0.0f };
		alignas(16) const float m34_col_w_arr[4] = { V.x,     V.y,     V.z,     0.0f };
		const Vec4f m34_col_x = Vec4f().load_a(m34_col_x_arr);
		const Vec4f m34_col_y = Vec4f().load_a(m34_col_y_arr);
		const Vec4f m34_col_z = Vec4f().load_a(m34_col_z_arr);
		const Vec4f m34_col_w = Vec4f().load_a(m34_col_w_arr);
		// IM is 3x3 (no translation). Used by the !_inShadowPass branch to
		// transform N and Tangent into view space.
		alignas(16) const float im_col_x_arr[4] = { IM[0][0], IM[1][0], IM[2][0], 0.0f };
		alignas(16) const float im_col_y_arr[4] = { IM[0][1], IM[1][1], IM[2][1], 0.0f };
		alignas(16) const float im_col_z_arr[4] = { IM[0][2], IM[1][2], IM[2][2], 0.0f };
		const Vec4f im_col_x = Vec4f().load_a(im_col_x_arr);
		const Vec4f im_col_y = Vec4f().load_a(im_col_y_arr);
		const Vec4f im_col_z = Vec4f().load_a(im_col_z_arr);
		
		
		// aprioric Offset Vector.
		MatrixTXVector(T->RotMat,&U,&AP);
		Vector *WP = (Vector *)T->RotMat;
		Vector_SelfScale(&AP, 1.0/Vector_SelfDot(WP));
		// ready
		// Bounding Sphere Elimination test Begins.
		W2 = (Vector *)(&T->RotMat);
		L2 = Dot_Product(W2,W2);
		if ((L1 = Dot_Product(W2+1,W2+1))>L2) L2=L1;
		if ((L1 = Dot_Product(W2+2,W2+2))>L2) L2=L1;
		
		frustumFlags = 0;

		frustumFlags |= Tri_Inside;

		// Out by depth
		dz = S.z - cam.nearZ;
		if (dz*dz>L2*T->BSphereRad)
		{
			if (dz<0.0f)
			{
				frustumFlags |= Tri_Invisible;
				continue;
			}
			frustumFlags |= Tri_Ahead;
		} else {
			frustumFlags &=~Tri_Inside;
		}

		dz = S.z - cam.farZ;
		if (dz*dz>L2*T->BSphereRad)
		{
			if (dz>0.0f)
			{
				frustumFlags |= Tri_Invisible;
				continue;
			}
		} else {
			frustumFlags &=~Tri_Inside;
		}
		// Out by left/right + up/down. The fabs() form below is a
		// SYMMETRIC-frustum test (the view axis must sit inside the
		// viewport). Off-axis passes — the mirror RTT renders through
		// a panel window whose projection center lies far outside the
		// target — set fds::g_offAxisFrustumCull for the duration:
		// the four viewport planes are then tested individually
		// (sphere-vs-plane, outside-positive q = distance·‖n‖; reject
		// iff q > 0 and q² > R²‖n‖²), which is the same math as the
		// folded test but without the mid-viewport-center assumption.
		// Tri_Inside is cleared so survivors take the fully-clipped
		// vertex path (per-vertex PX/PY screen-bound flags are
		// off-axis-correct). Depth classification above stays as-is.
		if (fds::g_offAxisFrustumCull) {
			const float R2 = L2 * T->BSphereRad;   // view-space radius²
			// xr/yr: the override-aware viewport extents — match what
			// the per-vertex flag stamps use.
			const float exR = float(xr) - cam.cntrEX;
			const float eyB = float(yr) - cam.cntrEY;
			// left  (screen_x < 0):    PX·x + cntrEX·z < 0
			float q = -(PX*S.x + cam.cntrEX*S.z);
			if (q > 0.0f && q*q > R2*(PX*PX + cam.cntrEX*cam.cntrEX)) {
				frustumFlags |= Tri_Invisible;
				continue;
			}
			// right (screen_x > XRes): PX·x - (XRes-cntrEX)·z > 0
			q = PX*S.x - exR*S.z;
			if (q > 0.0f && q*q > R2*(PX*PX + exR*exR)) {
				frustumFlags |= Tri_Invisible;
				continue;
			}
			// top   (screen_y < 0):    PY·y - cntrEY·z > 0
			q = PY*S.y - cam.cntrEY*S.z;
			if (q > 0.0f && q*q > R2*(PY*PY + cam.cntrEY*cam.cntrEY)) {
				frustumFlags |= Tri_Invisible;
				continue;
			}
			// bottom (screen_y > YRes): -PY·y - (YRes-cntrEY)·z > 0
			q = -(PY*S.y) - eyB*S.z;
			if (q > 0.0f && q*q > R2*(PY*PY + eyB*eyB)) {
				frustumFlags |= Tri_Invisible;
				continue;
			}
			frustumFlags &= ~Tri_Inside;
		} else {
		// Out by left/right
		S.x=fabs(S.x);
		L1 = PX*S.x - cam.cntrEX*S.z;
		if (L1*L1>L2*T->BSphereRad*(PX*PX+cam.cntrEX*cam.cntrEX))
		{
			if (S.x*PX>S.z*cam.cntrEX)
			{
				frustumFlags |= Tri_Invisible;
				continue;
			}
		} else {
			if (frustumFlags&Tri_Ahead) frustumFlags &=~Tri_Inside;
		}
		// Out by up/down
		S.y = fabs(S.y);
		L1 = PY*S.y - cam.cntrEY*S.z;
		if (L1*L1>L2*T->BSphereRad*(PY*PY+cam.cntrEY*cam.cntrEY))
		{
			if (S.y*PY>S.z*cam.cntrEY)
			{
				frustumFlags |= Tri_Invisible;
				continue;
			}
		} else {
			if (frustumFlags&Tri_Ahead) frustumFlags &=~Tri_Inside;
		}
		}
		// Phase B: conservative chunk occlusion cull (docs/VISIBILITY_PLAN.md
		// §7). Runs AFTER the frustum cull (cheap out-of-view reject first),
		// BEFORE the per-vertex transform + FList build (the work we reclaim).
		// MAIN VIEW ONLY. Skips only meshes whose world AABB is provably fully
		// hidden by the flat occluder hi-Z — byte-identical to not culling
		// (the faces would have z-failed every pixel anyway). Needs a valid
		// world box (chunks carry one from the Phase-A split; other meshes get
		// one from WorldAabb_UpdateScene in ChunkOcclusion_BeginFrame).
		if (_occlCull && T->WorldAabbValid) {
			const float bmn[3] = { T->WorldAabbMin.x, T->WorldAabbMin.y, T->WorldAabbMin.z };
			const float bmx[3] = { T->WorldAabbMax.x, T->WorldAabbMax.y, T->WorldAabbMax.z };
			if (fds::ChunkOcclusion_CullsAabb(bmn, bmx, int(T->VIndex), int(T->FIndex))) {
				frustumFlags |= Tri_Invisible;
				continue;
			}
		}
		if (_visStats) {
			++fds::g_chunkVisStats.meshesXformed;
			fds::g_chunkVisStats.vertsXformed += T->VIndex;
		}
#if FDS_VIS_CENSUS
		if (xpass) { ++xpassMeshX; xpassVX += T->VIndex; }
		if (xmesh && _xms) {
			_xms->xf[xpKind].fetch_add(1, std::memory_order_relaxed);
			_xms->xfV[xpKind].fetch_add(T->VIndex, std::memory_order_relaxed);
		}
		if (_mcensus) mirrorCensusMesh(T, IM, OS, L2, cam, PX, PY, xr, yr);
#endif
		if (xp) {
			// Every mesh past this point runs a per-vertex loop + a face loop.
			xpTV = xpNow();
			g_xprof.cur[XP_SETUP] += xpTV - xpTM;
			++g_xprof.meshes;
			g_xprof.verts += T->VIndex;
		}
		VEnd=tVerts+T->VIndex;

		/*    FEnd=T->Face+T->NumOfFaces;
		for (F=T->Face;F<FEnd;F++)
		if (!(F->Txtr->Flags&Mat_TwoSided))
        F->Flags = (AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z>=F->NormProd);*/
		
		// BSphereScreenPos is the mesh-center projected to screen space,
		// read only by the per-mesh debug-label overlay (RENDER.CPP),
		// which wants the MAIN-camera projection. Skip it for every
		// OFFSCREEN pass — the projection is useless to the overlay and
		// T->BSphereScreenPos is shared across all passes touching this
		// mesh, so writing it from a concurrent pass is a data race:
		//   - shadow bakes: per-light threads (TSan, 2026-06-12)
		//   - mirror-shard reflections: per-worker threads (g_offAxisFrustumCull
		//     set; TSan, 2026-06-16) — the Slice 6 parallel shard pass.
		// (the serial mirror RTT also sets g_offAxisFrustumCull; skipping it
		// there is harmless — the main pass re-projects after.) Only the
		// single main-camera on-axis pass writes it → no race, correct value.
		if (!_inShadowPass && !fds::g_offAxisFrustumCull) {
			Vector *BSC = &T->BSphereScreenPos;

			//BSC->x = M34[0][0] * V.x + M34[0][1] * V.y + M34[0][2] * V.z + M34[0][3];
			//BSC->y = M34[1][0] * V.x + M34[1][1] * V.y + M34[1][2] * V.z + M34[1][3];
			//BSC->z = M34[2][0] * V.x + M34[2][1] * V.y + M34[2][2] * V.z + M34[2][3];
			BSC->x = V.x;
			BSC->y = V.y;
			BSC->z = V.z;

			BSC->x /= BSC->z;
			BSC->y /= BSC->z;
		}
		// Alternate vertex loop for cube-face shadow xform: when the mesh
		// has a pre-computed world-space vertex cache, do a per-vertex
		// pyramid test in world space and skip the view matmul for
		// vertices that fall outside the face frustum. The mesh-level
		// cube cull (cubeFaceCull above) already rejected meshes whose
		// whole bsphere is outside; this is for meshes that straddle a
		// face boundary (e.g. greets's split Piramid chunks). Out-of-
		// pyramid vertices get TPos_AOS = (out-of-frame, out-of-frame, +1):
		// face submission later AND's the per-vertex Vtx_Visible bits,
		// so a face whose 3 vertices all share an out direction culls
		// cleanly. Faces straddling the boundary still process normally.
		if (cubeFaceCull && T->worldVerts
		    && fds::FeatureFlags::shadow_cube_vert_cull()) {
			// Circumscribed cone (matches mesh-level cull): half-angle
			// 54.7°, tan²(54.7°) = 2. So perp² > 2 × axisDist² → outside.
			constexpr float kTan2 = 2.0f;
			const float ckX = cubeFacePos.x, ckY = cubeFacePos.y, ckZ = cubeFacePos.z;
			const float cdX = cubeFaceDir.x, cdY = cubeFaceDir.y, cdZ = cubeFaceDir.z;
			// View transform reads `cam.view->Mat` directly (no perspective
			// pre-mul; we'll apply PX/PY/cntr scaling here, matching what
			// M34 × Pos produces in the legacy path).
			const float (*VM)[3] = cam.view->Mat;
			for (Vtx = tVerts, vfi = 0; Vtx < VEnd; Vtx++, vfi++) {
				const DWord vi = DWord(Vtx - tVerts);
				const Vector &wp = T->worldVerts[vi];
				const float wdx = wp.x - ckX;
				const float wdy = wp.y - ckY;
				const float wdz = wp.z - ckZ;
				const float axisDist = wdx*cdX + wdy*cdY + wdz*cdZ;
				const float d2 = wdx*wdx + wdy*wdy + wdz*wdz;
				const bool outside = (axisDist < 0.0f)
				    || ((d2 - axisDist*axisDist) > kTan2 * axisDist*axisDist);
				if (outside) {
					// Mark all-out so face cull eats the straddler-edge
					// faces whose 3 verts all agreed on being out.
					Vtx->TPos_AOS.x = 0.0f;
					Vtx->TPos_AOS.y = 0.0f;
					Vtx->TPos_AOS.z = 1.0f;
					Vtx->RZ = 1.0f;
					Vtx->PX = -1.0f;
					Vtx->PY = -1.0f;
					Vtx->Flags |= Vtx_Visible;  // all 6 frustum-out bits set
					if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
					continue;
				}
				// In-pyramid: full view xform + perspective scaling.
				// view_* = view.Mat · (worldPos - cam.ISource). Cube-face
				// camera's ISource == omni IPos == cubeFacePos, so wdX/Y/Z
				// already equal world delta from camera.
				const float vx = VM[0][0]*wdx + VM[0][1]*wdy + VM[0][2]*wdz;
				const float vy = VM[1][0]*wdx + VM[1][1]*wdy + VM[1][2]*wdz;
				const float vz = VM[2][0]*wdx + VM[2][1]*wdy + VM[2][2]*wdz;
				Vtx->TPos_AOS.x = PX * vx + cam.cntrEX * vz;
				Vtx->TPos_AOS.y = -PY * vy + cam.cntrEY * vz;
				Vtx->TPos_AOS.z = vz;
				Vtx->Flags &= ~Vtx_Visible;
				Vtx->RZ = 1.0f / vz;
				Vtx->PX = Vtx->TPos_AOS.x * Vtx->RZ;
				Vtx->PY = Vtx->TPos_AOS.y * Vtx->RZ;
				if (Vtx->PX < 0.0f) Vtx->Flags |= Vtx_VisLeft;
				if (Vtx->PX >= float(xr)) Vtx->Flags |= Vtx_VisRight;
				if (Vtx->PY < 0.0f) Vtx->Flags |= Vtx_VisUp;
				if (Vtx->PY >= float(yr)) Vtx->Flags |= Vtx_VisDown;
				if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
			}
			goto AfterXForm;
		}
		// Mirror-shard reflection cull: same per-vertex world-space cone test
		// as the cube-shadow path, but with the shard's narrow off-axis
		// reflection cone (g_reflCone*). The apex == the reflection camera's
		// ISource, so the world delta IS the view delta; in-cone vertices get
		// the off-axis view xform (PX/cntrEX already hold the shard's values),
		// out-of-cone ones are marked all-out so their faces cull. Rejects the
		// bulk of the room before the matmul — the whole point of the pass.
		if (fds::g_reflVertCull && T->worldVerts) {
			const float kTan2 = fds::g_reflConeTan2;
			const float ckX = fds::g_reflConeApex.x, ckY = fds::g_reflConeApex.y, ckZ = fds::g_reflConeApex.z;
			const float cdX = fds::g_reflConeDir.x,  cdY = fds::g_reflConeDir.y,  cdZ = fds::g_reflConeDir.z;
			const float (*VM)[3] = cam.view->Mat;
			for (Vtx = tVerts, vfi = 0; Vtx < VEnd; Vtx++, vfi++) {
				const DWord vi = DWord(Vtx - tVerts);
				const Vector &wp = T->worldVerts[vi];
				const float wdx = wp.x - ckX, wdy = wp.y - ckY, wdz = wp.z - ckZ;
				const float axisDist = wdx*cdX + wdy*cdY + wdz*cdZ;
				const float d2 = wdx*wdx + wdy*wdy + wdz*wdz;
				const bool outside = (axisDist < 0.0f)
					|| ((d2 - axisDist*axisDist) > kTan2 * axisDist*axisDist);
				if (outside) {
					Vtx->TPos_AOS.x = 0.0f; Vtx->TPos_AOS.y = 0.0f; Vtx->TPos_AOS.z = 1.0f;
					Vtx->RZ = 1.0f; Vtx->PX = -1.0f; Vtx->PY = -1.0f;
					Vtx->Flags |= Vtx_Visible;
					if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
					continue;
				}
				const float vx = VM[0][0]*wdx + VM[0][1]*wdy + VM[0][2]*wdz;
				const float vy = VM[1][0]*wdx + VM[1][1]*wdy + VM[1][2]*wdz;
				const float vz = VM[2][0]*wdx + VM[2][1]*wdy + VM[2][2]*wdz;
				Vtx->TPos_AOS.x = PX * vx + cam.cntrEX * vz;
				Vtx->TPos_AOS.y = -PY * vy + cam.cntrEY * vz;
				Vtx->TPos_AOS.z = vz;
				Vtx->Flags &= ~Vtx_Visible;
				Vtx->RZ = 1.0f / vz;
				Vtx->PX = Vtx->TPos_AOS.x * Vtx->RZ;
				Vtx->PY = Vtx->TPos_AOS.y * Vtx->RZ;
				if (Vtx->PX < 0.0f)        Vtx->Flags |= Vtx_VisLeft;
				if (Vtx->PX >= float(xr))  Vtx->Flags |= Vtx_VisRight;
				if (Vtx->PY < 0.0f)        Vtx->Flags |= Vtx_VisUp;
				if (Vtx->PY >= float(yr))  Vtx->Flags |= Vtx_VisDown;
				if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
			}
			goto AfterXForm;
		}
		// NOTE — none of the per-vertex loops below (nor the two world-space
		// cull loops above) stores Vertex::UZ / VZ any more. Those stores were
		// DEAD for every rasterized face: FrustumClipper::Render (FRUSTRUM.CPP,
		// the `U/V/UZ/VZ stamped + perspective-projected in one block` at ~:910)
		// copies A/B/C into its transient C_Verts and then UNCONDITIONALLY
		// overwrites UZ/VZ from `F->U1..V3 * RZ`, and every rasterized mesh face
		// reaches a filler through that entry (RenderInner.cpp 145/147/209/212/
		// 214/316/318/320/394/402, Shadows.cpp:778, DeferredSurfaceKernel.cpp
		// 3700/3701 are the only raster entries; clip-generated verts get UZ/VZ
		// from FInterpolator / Near() / Far()). The other UZ/VZ readers do not
		// see a mesh Vertex written here: `_2DClipper::clip` (FDS/Clipper.cpp)
		// rasterizes f.A/B/C directly, but all its callers hand it locally-built
		// quads that stamp their own UZ/VZ (DEMO/FOUNTAIN.CPP water + particle
		// trails, DEMO/BlasterBolts.cpp; DEMO/FillerTest.cpp is dev-gated); the
		// sprite path (RENDER.CPP:1209, A==B) reads PX/PY/RZ only; RADIO.CPP
		// reuses UZ/VZ as its own ray-coordinate scratch and writes before it
		// reads. Particle-trail verts still get UZ/VZ from calcVisibilityFlags,
		// which is why that function keeps its stores.
		// Removing them drops 2 loads (U,V at offsets 104..111), 2 muls and
		// 2 stores (offsets 12..19) per vertex per pass.
		//    Main vertex loop,in case no restrictions apply.
		if (!(T->Flags&Tri_Phong))
		{
			if (!(frustumFlags&Tri_Inside))
			{
				if (!(frustumFlags&Tri_Ahead))
					goto Regular;
				else goto Ahead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			// SIMD per-vertex transform via column-major matrices (see
			// staging block above the Inside/Ahead/Regular dispatch).
			// Each broadcast-FMA collapses what was 3 scalar muls + 3
			// scalar adds into one Vec4f op; the 4th lane is unused.
			if (xp) g_xprof.vInside += VEnd - tVerts;
			for (Vtx=tVerts,vfi=0;Vtx<VEnd;Vtx++,vfi++)
			{
				const float vpx = Vtx->Pos.x, vpy = Vtx->Pos.y, vpz = Vtx->Pos.z;
				// Explicit mul_add chain so the compiler emits FMLA
				// instead of separate vmul+vadd. clang without
				// -ffp-contract=fast won't fuse `a + b*c` written as
				// a normal expression.
				Vec4f tpos = mul_add(m34_col_x, Vec4f(vpx), m34_col_w);
				tpos       = mul_add(m34_col_y, Vec4f(vpy), tpos);
				tpos       = mul_add(m34_col_z, Vec4f(vpz), tpos);
				alignas(16) float tposArr[4];
				tpos.store_a(tposArr);
				Vtx->TPos_AOS.x = tposArr[0];
				Vtx->TPos_AOS.y = tposArr[1];
				Vtx->TPos_AOS.z = tposArr[2];
				if (!_inShadowPass && !xabNoTN) {
					const float nx = Vtx->N.x, ny = Vtx->N.y, nz = Vtx->N.z;
					Vec4f tn = im_col_x * Vec4f(nx);
					tn       = mul_add(im_col_y, Vec4f(ny), tn);
					tn       = mul_add(im_col_z, Vec4f(nz), tn);
					alignas(16) float tnArr[4];
					tn.store_a(tnArr);
					Vtx->TN.x = tnArr[0]; Vtx->TN.y = tnArr[1]; Vtx->TN.z = tnArr[2];
					const float gx = Vtx->Tangent.x, gy = Vtx->Tangent.y, gz = Vtx->Tangent.z;
					Vec4f tt = im_col_x * Vec4f(gx);
					tt       = mul_add(im_col_y, Vec4f(gy), tt);
					tt       = mul_add(im_col_z, Vec4f(gz), tt);
					alignas(16) float ttArr[4];
					tt.store_a(ttArr);
					Vtx->TTangent.x = ttArr[0]; Vtx->TTangent.y = ttArr[1]; Vtx->TTangent.z = ttArr[2];
				}

				Vtx->Flags &= ~Vtx_Visible;
				if (!xabNoProj) {
					const float rz = xfrmRcpF(tposArr[2], rcpMode);
					Vtx->RZ = rz;
					Vtx->PX = tposArr[0] * rz;
					Vtx->PY = tposArr[1] * rz;
				}
				if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
			}

			goto AfterXForm;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			if (xp) g_xprof.vAhead += VEnd - tVerts;
			for (Vtx=tVerts,vfi=0;Vtx<VEnd;Vtx++,vfi++)
			{
				// SIMD matrix prefix (see Inside path for the column-major
				// staging). Stores TPos_AOS via lane-extracts to avoid touching
				// N.x (next field) with the unused 4th lane.
				const float vpx = Vtx->Pos.x, vpy = Vtx->Pos.y, vpz = Vtx->Pos.z;
				Vec4f tpos = mul_add(m34_col_x, Vec4f(vpx), m34_col_w);
				tpos       = mul_add(m34_col_y, Vec4f(vpy), tpos);
				tpos       = mul_add(m34_col_z, Vec4f(vpz), tpos);
				alignas(16) float tposArr[4];
				tpos.store_a(tposArr);
				Vtx->TPos_AOS.x = tposArr[0];
				Vtx->TPos_AOS.y = tposArr[1];
				Vtx->TPos_AOS.z = tposArr[2];
				if (!_inShadowPass && !xabNoTN) {
					const float nx = Vtx->N.x, ny = Vtx->N.y, nz = Vtx->N.z;
					Vec4f tn = im_col_x * Vec4f(nx);
					tn       = mul_add(im_col_y, Vec4f(ny), tn);
					tn       = mul_add(im_col_z, Vec4f(nz), tn);
					alignas(16) float tnArr[4];
					tn.store_a(tnArr);
					Vtx->TN.x = tnArr[0]; Vtx->TN.y = tnArr[1]; Vtx->TN.z = tnArr[2];
					const float gx = Vtx->Tangent.x, gy = Vtx->Tangent.y, gz = Vtx->Tangent.z;
					Vec4f tt = im_col_x * Vec4f(gx);
					tt       = mul_add(im_col_y, Vec4f(gy), tt);
					tt       = mul_add(im_col_z, Vec4f(gz), tt);
					alignas(16) float ttArr[4];
					tt.store_a(ttArr);
					Vtx->TTangent.x = ttArr[0]; Vtx->TTangent.y = ttArr[1]; Vtx->TTangent.z = ttArr[2];
				}

				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				// Ahead path: BSphere is theoretically fully past NZP, but
				// float precision can let individual verts slip below.
				// Defensively flag those so the clipper's Near() handles
				// them (otherwise RZ would go negative and PX/PY would be
				// flipped, producing ghost polygons at the cone edges).
				if (Vtx->TPos_AOS.z > cam.nearZ) {
					if (!xabNoProj) {
					Vtx->RZ=xfrmRcpD(Vtx->TPos_AOS.z, rcpMode);
					Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
					}
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags|=Vtx_VisDown;
					if (Vtx->TPos_AOS.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
				} else {
					Vtx->Flags|=Vtx_VisNear;
				}
				if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto AfterXForm;
Regular:
			if (xp) g_xprof.vRegular += VEnd - tVerts;
			for (Vtx=tVerts,vfi=0;Vtx<VEnd;Vtx++,vfi++)
			{
				// SIMD matrix prefix (see Inside path for the staging).
				const float vpx = Vtx->Pos.x, vpy = Vtx->Pos.y, vpz = Vtx->Pos.z;
				Vec4f tpos = mul_add(m34_col_x, Vec4f(vpx), m34_col_w);
				tpos       = mul_add(m34_col_y, Vec4f(vpy), tpos);
				tpos       = mul_add(m34_col_z, Vec4f(vpz), tpos);
				alignas(16) float tposArr[4];
				tpos.store_a(tposArr);
				Vtx->TPos_AOS.x = tposArr[0];
				Vtx->TPos_AOS.y = tposArr[1];
				Vtx->TPos_AOS.z = tposArr[2];
				if (!_inShadowPass && !xabNoTN) {
					const float nx = Vtx->N.x, ny = Vtx->N.y, nz = Vtx->N.z;
					Vec4f tn = im_col_x * Vec4f(nx);
					tn       = mul_add(im_col_y, Vec4f(ny), tn);
					tn       = mul_add(im_col_z, Vec4f(nz), tn);
					alignas(16) float tnArr[4];
					tn.store_a(tnArr);
					Vtx->TN.x = tnArr[0]; Vtx->TN.y = tnArr[1]; Vtx->TN.z = tnArr[2];
					const float gx = Vtx->Tangent.x, gy = Vtx->Tangent.y, gz = Vtx->Tangent.z;
					Vec4f tt = im_col_x * Vec4f(gx);
					tt       = mul_add(im_col_y, Vec4f(gy), tt);
					tt       = mul_add(im_col_z, Vec4f(gz), tt);
					alignas(16) float ttArr[4];
					tt.store_a(ttArr);
					Vtx->TTangent.x = ttArr[0]; Vtx->TTangent.y = ttArr[1]; Vtx->TTangent.z = ttArr[2];
				}

				Vtx->Flags&=0xFFFFFFFF-Vtx_Visible;
				//      if (*(int32_t *)(&Vtx->TPos_AOS.z)>0x3F800000) // 1.0 in floating point rep.
				if (Vtx->TPos_AOS.z>cam.nearZ)
				{
					if (!xabNoProj) {
					Vtx->RZ=xfrmRcpD(Vtx->TPos_AOS.z, rcpMode);
					Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->PX=cam.cntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
					//          Vtx->PY=cam.cntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
					}
					if (Vtx->PX<0) Vtx->Flags|=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags|=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags|=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags|=Vtx_VisDown;
					if (Vtx->TPos_AOS.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
				} else Vtx->Flags|=Vtx_VisNear;
				//      printf("Regular shit!\n");
				if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
			}
		} else {
			// instead of all of these complications, I've decided to
			// make the face have void (*Clipper), that will do whatever it needs
			// in one call. the pre-filler will call the asm rasterizers twice
			// if necessary. back to the good old Avatar engine techniques ;)
			// at this section, the code also calculates environment mapping
			// coordinates to (EU,EV) by rotating the v. normals accordingly. slow.
			if (!(frustumFlags&Tri_Inside))
			{
				if (!(frustumFlags&Tri_Ahead))
					goto ERegular;
				else goto EAhead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			for (Vtx=tVerts,vfi=0;Vtx<VEnd;Vtx++,vfi++)
			{
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos_AOS);
				
				Vtx->Flags=0;
				Vtx->RZ=1.0/Vtx->TPos_AOS.z;
				Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=cam.cntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=cam.cntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;
				//if (Vtx->TPos_AOS.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
				if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
			}
			goto AfterXForm;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
EAhead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx=tVerts,vfi=0;Vtx<VEnd;Vtx++,vfi++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos_AOS);
				
				Vtx->RZ=1.0/Vtx->TPos_AOS.z;

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;
				
				Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=cam.cntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=cam.cntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
				if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft; else Vtx->Flags=0;
				if (Vtx->PX>=xr) Vtx->Flags+=Vtx_VisRight;
				if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
				if (Vtx->PY>=yr) Vtx->Flags+=Vtx_VisDown;
				if (Vtx->TPos_AOS.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
				if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
			}
			//    printf("Ahead VGA/Wizard.\n");
			
			goto AfterXForm;
ERegular:
			for (Vtx=tVerts,vfi=0;Vtx<VEnd;Vtx++,vfi++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M,&Vtx->Pos,&U);
				Vector_Add(&U,&V,&Vtx->TPos_AOS);
				
				Vtx->Flags = 0;
				//      if (*(int32_t *)(&Vtx->TPos_AOS.z)>0x3F800000) // 1.0 in floating point rep.

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
				
				if (Vtx->TPos_AOS.z>cam.nearZ)
				{
					Vtx->RZ=xfrmRcpD(Vtx->TPos_AOS.z, rcpMode);
					Vtx->PX=Vtx->TPos_AOS.x*Vtx->RZ;
					Vtx->PY=Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->PX=cam.cntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
					//          Vtx->PY=cam.cntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
//					Vtx->REU=Vtx->EU*Vtx->RZ;
//					Vtx->REV=Vtx->EV*Vtx->RZ;
					if (Vtx->PX<0) Vtx->Flags=Vtx_VisLeft;
					if (Vtx->PX>=xr) Vtx->Flags+=Vtx_VisRight;
					if (Vtx->PY<0) Vtx->Flags+=Vtx_VisUp;
					if (Vtx->PY>=yr) Vtx->Flags+=Vtx_VisDown;
					if (Vtx->TPos_AOS.z>cam.farZ) Vtx->Flags|=Vtx_VisFar;
				} else Vtx->Flags=Vtx_VisNear;
				if (soaX) { soaX[vfi]=Vtx->TPos_AOS.x; soaY[vfi]=Vtx->TPos_AOS.y; soaZ[vfi]=Vtx->TPos_AOS.z; soaPY[vfi]=Vtx->PY; }
				//      printf("Regular shit!\n");
			}
			
		}
AfterXForm:
		if (xp) { xpTS = xpNow(); g_xprof.cur[XP_VERT] += xpTS - xpTV; }
		FEnd=tFaces+T->FIndex;
		// SoA refactor Phase 1+4: dual-write the transformed-vertex
		// outputs into the per-mesh OR per-clone VertexFrame SoA
		// arrays. Main pass writes T->frame; shadow per-light scratch
		// writes clone.frame (so concurrent shadow lights don't race
		// on shared storage — that's why Phase 1 originally skipped
		// the scratch path).
		// One sequential sweep over tVerts; the AoS layout we're
		// reading from is cache-friendly (sequential reads at pack(1)
		// stride). Eventually Transform's per-vert loops will write
		// SoA directly and this sweep goes away.
		// SoA Phase 2a: with --xfrm_soa_inline the per-vertex loops above have
		// already stored TPos_x/y/z + PY, so this whole re-read sweep is dead
		// work. `soaX == nullptr` means the frame couldn't be resolved/sized
		// for this mesh — fall back to the sweep so the SoA never goes stale.
		if (!(soaInline && soaX)) {
			VertexFrame *F_ = nullptr;
			if (scratch) {
				// cloneOf already ensureSized'd clone.frame.
				F_ = &scratch->cloneOf(T).frame;
			} else {
				if (!T->frame) T->frame = new VertexFrame();
				T->frame->ensureSized(int(T->VIndex));
				if (T->frame->capacity >= int(T->VIndex)) F_ = T->frame;
			}
			if (F_) {
				const uint32_t nv = T->VIndex;
				// Ablation 4 skips only the COPY — the frame must still be
				// allocated or F->frame stays null and SortZ null-derefs.
				if (!xabNoSoa) VertexFrame_DumpFromAoS(F_, tVerts, nv);
				// Verification gate — off by default; turn on with
				// --soa-verify during migration to catch any future
				// divergence between AoS and SoA paths bit-for-bit.
				// Checks exactly the field(s) DumpFromAoS populates
				// (TPos_x/y/z + PY); extend both together when a consumer migrates.
				if (fds::FeatureFlags::soa_verify()) {
					for (uint32_t i = 0; i < nv; ++i) {
						if (F_->TPos_x[i] != tVerts[i].TPos_AOS.x ||
						    F_->TPos_y[i] != tVerts[i].TPos_AOS.y ||
						    F_->TPos_z[i] != tVerts[i].TPos_AOS.z ||
						    F_->PY[i]     != tVerts[i].PY) {
							std::fprintf(stderr, "[SOA-VERIFY] mismatch vert %u mesh %p\n", i, (void*)T);
							std::abort();
						}
					}
				}
			}
		}
		// Same bit-for-bit audit for the INLINE path (--xfrm_soa_inline +
		// --soa-verify): the stores happened one loop earlier, so this
		// re-checks them against the AoS the sweep would have copied from.
		if (soaInline && soaX && fds::FeatureFlags::soa_verify()) {
			const uint32_t nv = T->VIndex;
			for (uint32_t i = 0; i < nv; ++i) {
				if (soaX[i]  != tVerts[i].TPos_AOS.x ||
				    soaY[i]  != tVerts[i].TPos_AOS.y ||
				    soaZ[i]  != tVerts[i].TPos_AOS.z ||
				    soaPY[i] != tVerts[i].PY) {
					std::fprintf(stderr, "[SOA-VERIFY] inline mismatch vert %u mesh %p\n", i, (void*)T);
					std::abort();
				}
			}
		}
		if (xp) { xpTF = xpNow(); g_xprof.cur[XP_SOA] += xpTF - xpTS; }
	// Runtime debug: hide specific nested-transparent objects (fountain's
	// f_sphere outer and "f in shpere" inner). Toggled by J / K keys —
	// useful for isolating which face contributes to a rendering bug.
	const bool hideInner = g_HideInnerXpar.load(std::memory_order_relaxed);
	const bool hideOuter = g_HideOuterXpar.load(std::memory_order_relaxed);
	// FDS_XPAR_FORCE_TWOSIDED=1 treats the nested-sphere materials as
	// TwoSided regardless of their actual flag. The fountain meshes ship
	// single-sided, so without this we only see their camera-facing half
	// — which makes "inner is hidden by outer" worse since the inner's
	// far half doesn't even render. Quick experiment to gauge whether
	// the deferred path's missing-inner-objects symptom improves when
	// both halves are present.
	const bool forceXparTwoSided = fds::FeatureFlags::xpar_force_twosided();
	// Per-face xpar sort flags hoisted to per-mesh — the registry read
	// is a memory load + offset and called once per xpar face was ~25
	// leaf samples across greets' xpar walls.
	const bool xparFrontBackDisabled = fds::FeatureFlags::no_xpar_frontback();
	const bool xparObjGroupDisabled  = fds::FeatureFlags::no_xpar_objgroup();
	// Shadow pass: treat all faces as two-sided. Single-quad walls have
	// no back-side polygon to take over when the light's on their "back",
	// yet they still occlude light. Backface-culling for shadow rendering
	// is geometry-dependent (correct for sealed convex meshes, wrong for
	// single-sided walls/sheets) — easier to just skip it.
	extern thread_local bool g_inShadowPass;
	const bool shadowNoBackface = g_inShadowPass && !fds::FeatureFlags::shadow_backface_cull();
	// Env-reflection bake: the baked surface's own faces stay out of its
	// probe (face-level; see the note at the mesh loop). Hoisted bool —
	// false on every non-bake pass, so the per-face cost is one branch.
	const bool envFaceSkip = fds::g_envBakeSkipDynamic && fds::EnvBake_HasSkipFaces();
	// S2 / B5 tile pre-reject (docs/ENVDYN_DISPLACEMENT_PLAN.md): stamp each
	// pushed face's projected screen bbox into its FListEntry so the per-tile
	// rasterizer walk can 4-compare-reject it before the Face deref. Filled
	// only when the flag is on (flag-off leaves the FListEntry's cover-all
	// default -> the walk never rejects -> today's exact path, byte-identical).
	// Read the flag + nearZ once per mesh.
	const bool tileBboxCull = fds::FeatureFlags::tile_bbox_cull();
	const float bboxNearZ   = cam.nearZ;
	auto satI16 = [](float v) -> int16_t {
		if (v < -32768.0f) return -32768;
		if (v >  32767.0f) return  32767;
		return int16_t(v);
	};
#if FDS_SHARD_BAKE_LAB
	// --shard_cone_cull=2: build this mesh's per-face keep mask for the
	// mirror-shard reflection bake, out of line (RENDER/ReflFaceCull.cpp).
	// COMPILE-TIME gated, and the gate is the point: the cull's OUTPUT is
	// byte-identical to no cull, but merely carrying this CALL inside the
	// per-mesh body moved all three scene pins through -ffp-contract=fast, on
	// frames that never shatter a mirror. Bisected — the face-loop branch
	// below is byte-null on its own; the call is not, at either site tried.
	if (fds::g_reflFaceCull)
		fds::ReflFaceCull_Mark(T, tFaces, tVerts, AP, _offscreenPass, fcKeep);
#endif
	// Ablation 32 (--xfrm_ablate=32): skip the per-face loop entirely.
	if (!xabNoFace)
	for (F=tFaces;F<FEnd;F++) {
		// Ablation 16: pure loop + Face-pointer-walk overhead, nothing else.
		if (xab && (_xablate & XAB_FACE_NOOP)) continue;
		if (xp) ++g_xprof.facesTested;
		// S1 offscreen proxy: the displaced stone detail (Face_MainOnly) is
		// main-camera only. In any offscreen/bake pass the flat proxy mesh
		// casts/reflects instead, so drop these faces here. Inert unless
		// --greets_displace tagged them.
		if (_offscreenPass && (F->Flags & Face_MainOnly)) continue;
		if (envFaceSkip && fds::EnvBake_FaceExcluded(F, T)) continue;
#if FDS_SHARD_BAKE_LAB
		// --shard_cone_cull=2: this face's world bounding sphere missed the
		// shard's reflection cone, so it cannot paint a pixel of this bake.
		// Decided in world space by ReflFaceCull_Mark before this loop.
		if (fcKeep && !fcKeep[F - tFaces]) continue;
#endif
		if ((hideInner || hideOuter) && F->Txtr && F->Txtr->Name) {
			const char* mn = F->Txtr->Name;
			if (hideInner && std::strstr(mn, "in shpere")) continue;
			if (hideOuter && std::strcmp(mn, "f_sphere") == 0) continue;
		}
		const bool forceTS = forceXparTwoSided && F->Txtr && F->Txtr->Name &&
			(std::strstr(F->Txtr->Name, "in shpere") ||
			 std::strcmp(F->Txtr->Name, "f_sphere") == 0);
		if ((!F->VisibilityFlagsAll())
			&&(forceTS
			|| shadowNoBackface
			||(F->Txtr->Flags&Mat_TwoSided)
			||(AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z<F->NormProd) // Backface culling
			//||(1) // no backface culling
			))
		{
			// Ablation 8: run the visibility + backface test, then bail —
			// isolates the cull test from the accepted-face work below.
			if (xab && (_xablate & XAB_FACE_CULL)) continue;
			if (0 != (F->Flags & Face_Reflective)) {
				// clobber U1, V1, etc. with the equilateral-whatever coordinates matching
				// the direction from camera to the specific vertex, reflected on the face's plane
				float eu[3];
				float ev[3];
				size_t i = 0;
				Vector wsPos[3];

				for (Vertex* v : { F->A, F->B, F->C }) {
					wsPos[i] = T->RotMat * v->Pos + T->IPos;
					++i;
				}

				// auto cv = (T->BSphereCtr - cam.view->ISource) * 0.9 + cam.view->ISource;
				auto cv = cam.view->ISource;
				float optimalDistFromPlane = fabs(T->BSphereCtr * F->N - F->NormProd);
				float viewDistFromPlane = fabs(AP * F->N - F->NormProd);
				if (viewDistFromPlane > optimalDistFromPlane) {
					float hackDistFromPlane = pow(viewDistFromPlane - optimalDistFromPlane, 0.8) + optimalDistFromPlane;
					Vector bsWorldPos;
					MatrixXVector(T->RotMat, &T->BSphereCtr, &bsWorldPos);
					bsWorldPos += T->IPos;

					auto pullDir = bsWorldPos - cam.view->ISource;
					float step = pullDir * F->N;
					// Skip the cv pull when pullDir is nearly parallel to
					// the face plane (step → 0). Dividing by a tiny step
					// otherwise blows cv out to infinity → wildly wrong
					// reflections that swing with small camera motions
					// (see [[project_cv_pull_instability]]).
					// Threshold: 0.1 × |pullDir| corresponds to ~5.7° tilt
					// between pullDir and the plane. Below that the pull
					// is geometrically ill-conditioned anyway — leaving
					// cv = camera ISource gives a slightly-wider FOV than
					// authored, which is a much less jarring artifact than
					// the swing.
					const float pullLen2 = pullDir * pullDir;
					if (step * step > 0.01f * pullLen2) {
						cv += (hackDistFromPlane - viewDistFromPlane) / step * pullDir;
					}
				}
				auto n = (wsPos[0] - wsPos[1]).cross(wsPos[2] - wsPos[1]);
				Vector_Norm(&n);
				if (fds::FeatureFlags::env_cube() && T->EnvHemiSheets[0]) {
					// env_cube: paraboloid hemisphere sheet bound to the
					// PANEL NORMAL's dominant axis — STATIC for static
					// geometry, so the chart never flips with camera motion
					// (a camera-dependent chart choice pops the frame it
					// changes; that was the close-up "jumping"). Every
					// physically possible reflected ray off this panel lies
					// in the viewer-side normal's hemisphere, which the
					// sheet covers in ONE chart: no fallback, no clamp
					// threshold, no U-wrap. One divide per vertex, no trig.
					// Kept OUT of the equirect else-branch below so that
					// path stays byte-identical when the flag is off.
					//
					// Viewer-side normal: `n` comes from the winding and can
					// point either way; reflected dirs satisfy d·n_view > 0,
					// so flip n to the side the camera is on before the
					// sheet pick (one dot against the first incident ray).
					const Vector w0 = wsPos[0] - cv;
					const float nSide =
						(w0.x*n.x + w0.y*n.y + w0.z*n.z) > 0.0f ? -1.0f : 1.0f;
					const int k = fds::EnvCube_SelectFace(
						nSide * n.x, nSide * n.y, nSide * n.z);
					// Live water (--env_live_water): probe center of THIS
					// building's sheet bake (CITY bakes from IPos +
					// BSphereCtr, no rotation). Inactive = one branch in
					// the helper per vertex.
					const float lwPX = T->IPos.x + T->BSphereCtr.x;
					const float lwPY = T->IPos.y + T->BSphereCtr.y;
					const float lwPZ = T->IPos.z + T->BSphereCtr.z;
					// --env_live_water: the tilt leaves this loop as a
					// per-FACE UV OFFSET, and the mask read moves to the
					// filler, where it is per PIXEL (the sheets carry the
					// coverage in their alpha byte). WHY NOT PERTURB THE
					// VERTEX DIRECTION, which is what 5d28db7/5f1ffa92 did:
					// the rasterizer interpolates the resulting UV affinely,
					// so tilting one corner whose reflection is water drags
					// EVERY pixel of the face with it — including the ones
					// reading the skyline — weighted by that corner's
					// barycentric. A per-vertex mask cannot localize below
					// face granularity, however right the mask is. Measured
					// under the user's preset at the t=1961 pose: per-vertex
					// weights left 38 148 reflected-skyline pixels moving
					// between two wave clocks (mean |Δ| 4.94, max 41) against
					// the deferred per-pixel path's 6 927 / 1.81 / 12.
					// The offset is the three corners' FULL-tilt (w = 1) UV
					// displacement, averaged weighted by each corner's own
					// coverage (see below); the per-pixel coverage is what
					// scales it. Amplitude is therefore face-constant where the old
					// code interpolated it linearly — a difference of degree
					// (both are coarse), against localization, which is the
					// difference between "the water ripples" and "the skyline
					// ripples too".
					float lwDU = 0.0f, lwDV = 0.0f, lwWSum = 0.0f;
					float lwDUf = 0.0f, lwDVf = 0.0f;
					int   lwN  = 0;
					const bool lwLive = T->EnvWaterMask
					                 && T->EnvWaterMaskRes >= 2
					                 && fds::g_envLiveWater.active
					                 && lwPY > fds::g_envLiveWater.waterY;
					for (i = 0; i < 3; ++i) {
						auto d = wsPos[i] - cv;
						d -= (d * n) * 2.0f * n;
						Vector_Norm(&d);
						fds::EnvCube_DirToParaboloidUV(k, d.x, d.y, d.z,
						                               eu[i], ev[i]);
						if (!lwLive || d.y >= -1e-6f) continue;
						// FULL tilt (w = 1); the pixel's own coverage is what
						// scales it down, and it is read with the UNPERTURBED
						// lookup — here, structurally, because the offset is
						// added to the unperturbed UV the texel was fetched
						// at rather than folded into the direction.
						Vector dp = d;
						fds::EnvLiveWater_TiltDir(lwPX, lwPY, lwPZ,
						                          dp.x, dp.y, dp.z, 1.0f);
						float pu, pv;
						fds::EnvCube_DirToParaboloidUV(k, dp.x, dp.y, dp.z,
						                               pu, pv);
						// Weight each corner's displacement by ITS OWN water
						// coverage: on a pane straddling the reflected
						// waterline the water pixels sit near the wet
						// corners, and the tilt magnitude that belongs to
						// them is that corner's (it scales with |dy| and
						// with the slope at a wildly different plane hit).
						// A flat mean pulls the amplitude toward the dry
						// corners and measurably deadens the water. Measured
						// (t=1961, user's preset, Σ|Δ| over the reflected-
						// water region as a share of the pre-fix arm's):
						// flat mean 87.2 %, coverage-weighted 110.6 %, with
						// the reflected-skyline residual 3 375 vs 5 594 px —
						// both far under the deferred path's 6 927 at the
						// same pose, so the amplitude is bought for free.
						const float wv = fds::EnvLiveWater_Weight(
							lwPY, d.x, d.y, d.z,
							T->EnvWaterMask, T->EnvWaterMaskRes);
						lwDUf += pu - eu[i];
						lwDVf += pv - ev[i];
						lwDU  += (pu - eu[i]) * wv;
						lwDV  += (pv - ev[i]) * wv;
						lwWSum += wv;
						++lwN;
					}
					if (lwWSum > 0.0f) {
						const float inv = 1.0f / lwWSum;
						F->LwDU = lwDU * inv;
						F->LwDV = lwDV * inv;
					} else if (lwN) {
						// Below the horizon but no corner reads water — the
						// face's INTERIOR still might (the corners are three
						// samples of a curved footprint), so keep the flat
						// mean and let the per-pixel alpha decide.
						const float inv = 1.0f / float(lwN);
						F->LwDU = lwDUf * inv;
						F->LwDV = lwDVf * inv;
					} else {
						F->LwDU = F->LwDV = 0.0f;
					}
					F->ReflectionTexture = T->EnvHemiSheets[k];
				} else {
				// The legacy equirect panorama carries no coverage mask
				// (nothing baked one), so no live-water tilt — the flag's
				// documented "no mask → no tilt" fallback.
				F->LwDU = F->LwDV = 0.0f;
				i = 0;
				for (Vertex* v : { F->A, F->B, F->C }) {
					auto d = wsPos[i] - cv;
					d -= (d * n) * 2.0f * n;
					Vector_Norm(&d);
					// Equirectangular panorama lookup. The convention
					// matches CalcEquirectangularPanoramaTable's bake:
					//   eu=0    → -z scenery     eu=0.5  → +z scenery
					//   eu=0.25 → +x scenery     eu=0.75 → -x scenery
					//   ev=0    → +y scenery     ev=1    → -y scenery
					// so a reflected ray pointing in world direction d
					// resolves to (eu, ev) where the bake stored that
					// direction's content. Verified with --snapshot=
					// cuberefl (synthetic painter follows the same
					// convention).
					// Polynomial asin / atan2 — libm versions are
					// ~100-200 cycles each. The polynomial pair is
					// ~25 cycles total at ~0.001 rad max error, well
					// below the per-pixel panorama discretization for
					// city windows.
					float lat = asin_approx(d.y);
					float lon = atan2_approx(-d.z, -d.x);
					eu[i] = 0.5 + 0.5 * (lon + PI / 2.0) / PI;
					ev[i] = 0.5 - 0.5 * lat / (PI / 2.0);
					++i;
				}

				// U-wrapping
				if (std::max({ eu[0], eu[1], eu[2] }) - std::min({ eu[0], eu[1], eu[2] }) > 0.8) {
					for (int i = 0; i < 3; ++i) {
						if (eu[i] < 0.5) {
							eu[i] += 1;
						}
					}
				}
				}

				F->EU1 = eu[0];
				F->EV1 = ev[0];
				F->EU2 = eu[1];
				F->EV2 = ev[1];
				F->EU3 = eu[2];
				F->EV3 = ev[2];
			}
			F->ParentTri = T;
			// SoA Phase 4: stamp the active VertexFrame on F so
			// consumers (SortZ / clipper / rasterizer) can read
			// `F->frame->TPos_z[F->A_idx]` etc. without needing to
			// know whether this is the main pass or a per-light
			// shadow scratch. F here is from tFaces (clone-owned in
			// scratch mode, T->Faces in main mode), so stamping is
			// always per-frame-state-isolated.
			F->frame = scratch ? &scratch->cloneOf(T).frame : T->frame;

#ifdef FRONT_TO_BACK_SORTING
			Material *M = F->Txtr;
//			mword sortid = 0;
//			if (M->Txtr)
//				sortid = M->Txtr->ID;

			// front-to-back sorting, also batches polygons that are all using 
			// the same texture.
			if (M->Flags & Mat_Transparent)
			{
				// Pass the array F was iterated from (tFaces — clone when
				// VertexScratch is in use, else T->Faces) so the prev/next
				// bounds check is correct. The CLONE has its own bounds.
				dz = QuadAwareMaxViewZ(F, tFaces, T->FIndex);
				const bool frontFacing = IsFrontFacingInViewSpace(F);

				// Object-level back-to-front grouping. Without this, nested
				// transparent meshes (fountain's f_sphere outer + "f in
				// shpere" inner, both concentric) interleave by per-face
				// max-z. Using the bsphere extent in view-space depth
				// instead:
				//
				//   BACK partition: extent = bsphere_z + radius (farthest
				//     surface). Larger extent → renders first → outer
				//     before inner.
				//   FRONT partition: extent = bsphere_z - radius (nearest
				//     surface). Larger extent → renders first → inner
				//     before outer (inner-front is farther from camera
				//     than outer-front in a concentric pair).
				//
				// Sort key layout (smaller renders first):
				//   bit-high: front-or-back partition (front gets 4*fzp
				//             offset, larger than any object/face score)
				//   bit-mid:  object score (2*fzp - extent, range 0..4*fzp)
				//   bit-low:  face fine sort (in [0, 1.0], used as fraction
				//             so adjacent faces of the same object still
				//             back-to-front)
				if (xparObjGroupDisabled) {
					// Legacy face-only sort, used for A/B comparison.
					if (dz > fzp) F->SortZ.F = fzp;
					else          F->SortZ.F = 2.0f * fzp - dz;
					if (!xparFrontBackDisabled && frontFacing)
						F->SortZ.F += 2.0f * fzp;
				} else {
					const float extent = frontFacing
						? (objBSphereViewZ - objBSphereRadius)
						: (objBSphereViewZ + objBSphereRadius);
					const float objScore = 2.0f * fzp - extent;
					const float faceFine = (dz > 0.0f && fzp > 0.0f)
						? std::max(0.0f, std::min(1.0f, (fzp - dz) / fzp))
						: 0.0f;
					float key = objScore + faceFine;
					if (!xparFrontBackDisabled && frontFacing)
						key += 4.0f * fzp;
					F->SortZ.F = key;
				}

//				F->SortZ.DW >>= 8;
//				F->SortZ.DW += 255 << 24;
			} else {
				// SoA Phase 4: SortZ read migrated. F->frame was
				// stamped on the FList push above (T->frame for main,
				// clone.frame for shadow scratch). Same dz value as the
				// AoS read; will diverge only after Phase 5 removes the
				// AoS TPos_AOS field.
				const float *fzp = F->frame->TPos_z;
				dz = fzp[F->A_idx];
				if (fzp[F->B_idx] > dz) dz = fzp[F->B_idx];
				if (fzp[F->C_idx] > dz) dz = fzp[F->C_idx];
				F->SortZ.F = dz;

//				F->SortZ.DW >>= 8;
//				F->SortZ.DW += sortid << 24;

			}

			static int32_t BiasedSortValues[] = {0, 0, (int32_t)0xFFFFFFFF};
			if (T->SortPriorityBias)
				F->SortZ.F = BiasedSortValues[T->SortPriorityBias];
#else
			// SoA Phase 4: SortZ read migrated. See Phase 4 note above.
			const float *fzpArr = F->frame->TPos_z;
			dz = fzpArr[F->A_idx];
			if (fzpArr[F->B_idx] > dz) dz = fzpArr[F->B_idx];
			if (fzpArr[F->C_idx] > dz) dz = fzpArr[F->C_idx];
			F->SortZ.F = fzp - dz;
#endif

			// Push AFTER SortZ is computed so FListEntry.sortKey
			// captures the final value (legacy layout could push
			// the Face* first since the radix sort dereffed back
			// through it; the new FListEntry has sortKey inline).
			fds::FListEntry* e = Ins++;
			if (xp) ++g_xprof.facesPushed;
			e->sortKey = F->SortZ.DW;
			e->face    = F;
			// S2 tile pre-reject bbox. Read the projected PX/PY straight off
			// the face's own Vertex pointers (A/B/C — exactly what the clipper
			// + rasterizer read), so this is robust even for meshes whose SoA
			// F->*_idx aren't populated (e.g. the conetest giant quad, whose
			// wrong index only perturbs the Z-buffered SortZ harmlessly but
			// would give a degenerate bbox from frame->PX[0]). PX/PY/TPos_AOS
			// all sit in the Vertex's first cache line. Valid only when all
			// three verts are in FRONT of the near plane: behind-near PX/PY are
			// stale (the projection skips the divide there), so those faces get
			// the cover-all box and are never rejected — the near-plane-clipped
			// fragments the clipper would still produce are kept. The box is a
			// conservative superset of the un-clipped triangle (floor/ceil +
			// 1px margin, int16-saturated); the clipper only SHRINKS coverage,
			// so a box that misses the tile means zero output there → the
			// reject is byte-identical to clipping.
			if (tileBboxCull && !(xab && (_xablate & XAB_NO_BBOX))) {
				const Vertex* va = F->A; const Vertex* vb = F->B; const Vertex* vc = F->C;
				const float za = va->TPos_AOS.z, zb = vb->TPos_AOS.z, zc = vc->TPos_AOS.z;
				if (za > bboxNearZ && zb > bboxNearZ && zc > bboxNearZ) {
					const float pxa = va->PX, pxb = vb->PX, pxc = vc->PX;
					const float pya = va->PY, pyb = vb->PY, pyc = vc->PY;
					const float minx = std::min(std::min(pxa, pxb), pxc);
					const float maxx = std::max(std::max(pxa, pxb), pxc);
					const float miny = std::min(std::min(pya, pyb), pyc);
					const float maxy = std::max(std::max(pya, pyb), pyc);
					e->bbMinX = satI16(floorf(minx) - 1.0f);
					e->bbMinY = satI16(floorf(miny) - 1.0f);
					e->bbMaxX = satI16(ceilf (maxx) + 1.0f);
					e->bbMaxY = satI16(ceilf (maxy) + 1.0f);
				} else {
					e->bbMinX = e->bbMinY = -32768;
					e->bbMaxX = e->bbMaxY =  32767;
				}
			} else {
				e->bbMinX = e->bbMinY = -32768;
				e->bbMaxX = e->bbMaxY =  32767;
			}
		}
	}  // close per-face loop body opened above
		if (xp) g_xprof.cur[XP_FACE] += xpNow() - xpTF;
	}
	// --xfrm_par: a MESH shard reports how much of its reserved segment it
	// filled and stops here — the omni/particle epilogue is not per-mesh work
	// and runs once, in the EPILOGUE shard, after the driver has compacted the
	// poly segments and set cPolys. hi == 0 marks that epilogue shard (it
	// matched no mesh above), and it must NOT clobber cPolys.
	if (_shard) {
		if (_shard->hi > 0) {
			const_cast<XfrmShard *>(_shard)->out =
				int32_t(Ins - (faces.fList + _shardInsOff));
			return;
		}
	} else {
	faces.cPolys = Ins-faces.fList;
	}

	// Shadow pass (scratch != nullptr) skips omnis: this loop mutates
	// O->V.TPos_AOS / RZ / PX / PY in place on the source Omni (no clone
	// equivalent exists for omnis). Running it for each light would
	// leave omni screen positions stuck on the last light's camera,
	// breaking the flare draw on the main pass.
	FDW omniFlareSize;
	if (!scratch) for(O=Sc->OmniHead;O;O=O->Next)
	{
		// Mirror-clone omni flares DO draw — the lamp flares must show
		// in reflections ("omnis don't get reflected in mirrors").
		// This used to skip them when clones' F.A/B/C still pointed at
		// the SOURCE omni's V (every clone flare rendered on top of the
		// real lamp = the yellow sprite wall); GreetsMirror now repoints
		// the clone's flare face at its own V, and the per-pixel z test
		// in the flare blit clips the sprite to the mirror footprint
		// (clone-world depth there, nearer real geometry elsewhere).

		Vtx=&O->V;
		Vector_Sub(&O->IPos,&cam.view->ISource,&V);
		//if (O->Flags & Omni_Rand) {
		//	Vector Rand;
		//	Rand.x = (frand() - 0.5) * 2.0f;
		//	Rand.y = (frand() - 0.5) * 2.0f;
		//	Rand.z = (frand() - 0.5) * 2.0f;
		//	Vector_Add(&V, &Rand, &V);
		//}
		MatrixXVector(cam.view->Mat,&V,&Vtx->TPos_AOS);
		if (Vtx->TPos_AOS.z>cam.nearZ&&Vtx->TPos_AOS.z<cam.farZ)
		{
			Vtx->RZ=1.0/Vtx->TPos_AOS.z;
			Vtx->PX=cam.cntrEX+Vtx->TPos_AOS.x*PX*Vtx->RZ;
			Vtx->PY=cam.cntrEY-Vtx->TPos_AOS.y*PY*Vtx->RZ;
			// Insert to List
			dz = Vtx->TPos_AOS.z;
			//dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt(&O->Face.SortZ.DW,dz);

			//omniFlareSize.F = O->ISize;
			//O->F.Flags = omniFlareSize.DW; // overwrite face flags with a pointer to the omnilight object
			// FlareScale: per-omni sprite-size dial (0 = legacy 1.0), so the
			// flare can be sized independently of the lighting intensity.
			O->F.FlareSize = O->ISize * (O->FlareScale > 0.0f ? O->FlareScale : 1.0f);
			if (O->Flags & Omni_Rand) {
				O->F.FlareSize *= 1.0 + ((frand() - 0.5) * 0.2f);
			}
#ifdef FRONT_TO_BACK_SORTING
			O->F.SortZ.F = 2*fzp-dz;
#else
			O->F.SortZ.F = fzp-dz;
#endif			
			*Ins++ = { O->F.SortZ.DW, &O->F };
		}
	}

	faces.cOmnies = (Ins-faces.fList)-faces.cPolys;
#endif
	// Shadow pass skips particles for the same reason as omnis: the
	// projection writes to Sc->Pcl[I].V.* directly, no clone storage.
	if (!scratch) for (I = 0; I < Sc->NumOfParticles; I++) {
		Particle& p = Sc->Pcl[I];

		auto v = p.V.Pos - cam.view->ISource;
		
		p.V.TPos_AOS = cam.view->Mat * v;

		if (p.V.TPos_AOS.z >= cam.nearZ)
		{
			p.V.RZ = 1.0 / p.V.TPos_AOS.z;
			p.V.PX = cam.cntrX + cam.fovX * p.V.TPos_AOS.x * p.V.RZ;
			p.V.PY = cam.cntrY - cam.fovY * p.V.TPos_AOS.y * p.V.RZ;
			p.V.Flags = 0;
		} else {
			p.V.Flags |= Vtx_VisNear;
		}


		if (p.Flags & Particle_Active) {
			if ((dz = p.V.TPos_AOS.z) >= cam.nearZ) {
				if (p.TrailLength == 0) {
					F = &Sc->Pcl[I].F;
#ifdef FRONT_TO_BACK_SORTING
					F->SortZ.F = 2 * fzp - dz;
#else
					F->SortZ.F = fzp - dz;
#endif
					*Ins++ = { F->SortZ.DW, F };
				} else {
					addParticleTrail(Sc, Ins, p, cam);
				}
			}
		}
	}

	faces.cAll = Ins-faces.fList;
	faces.cPcls = faces.cAll-faces.cOmnies-faces.cPolys;

#if FDS_VIS_CENSUS
	if (_mcensus) {
		++g_mcensus.frames;
		if (g_mcensus.frames >= _mcensusN) mcensusDump();
	}

	if (xpass) {
		XPassAcc &a = g_xpass[xpKind];
		a.ns.fetch_add(xpNow() - xpassT0, std::memory_order_relaxed);
		a.calls.fetch_add(1, std::memory_order_relaxed);
		a.meshesSeen.fetch_add(xpassMeshSeen, std::memory_order_relaxed);
		a.meshesXformed.fetch_add(xpassMeshX, std::memory_order_relaxed);
		a.vertsSeen.fetch_add(xpassVSeen, std::memory_order_relaxed);
		a.vertsXformed.fetch_add(xpassVX, std::memory_order_relaxed);
		// faces.cPolys is the poly FList length this call produced — the same
		// number a per-face counter would accumulate, read once instead of
		// incremented inside the hot face loop (an extra store there perturbs
		// the -ffp-contract=fast codegen of the surrounding vertex/face work
		// and measurably moved pixels: 216 bytes / 1920x1080 city, max 44).
		a.facesPushed.fetch_add(faces.cPolys, std::memory_order_relaxed);
		if (xpKind == XPK_MAIN) {
			const int fr = g_xpassMainFrames.fetch_add(1, std::memory_order_relaxed) + 1;
			if (fr >= _xpassN) {
				g_xpassMainFrames.store(0, std::memory_order_relaxed);
				if (xmesh) xmDump(fr);   // before xpassDump: it zeroes its own rows
				xpassDump(fr);
			}
		}
	}
#endif  // FDS_VIS_CENSUS

	if (xp) {
		g_xprof.cur[XP_TOTAL] = xpNow() - xpT0;
		const int fi = g_xprof.frames;
		if (fi < XP_MAXFRAMES) {
			for (int k = 0; k < XP_NUM; ++k) { g_xprof.s[k][fi] = g_xprof.cur[k]; g_xprof.cur[k] = 0; }
			++g_xprof.frames;
		}
		if (g_xprof.frames >= _xprofN || g_xprof.frames >= XP_MAXFRAMES) xpDump(g_xprof.frames);
	}
}

// ─── --xfrm_par driver: mesh-sharded main-view Transform_Objects ──────────
// Runs on the tick thread. Four phases, all timed under --xfrm_prof=N (the
// timers live HERE, in a cold function, never inside Transform_Objects'
// -ffp-contract=fast body):
//
//   PLAN     one walk of the object list -> per-mesh FIndex (an exact UPPER
//            BOUND on what that mesh can push) + a cost estimate, prefix
//            sums, and a partition into W contiguous mesh-index blocks.
//   WORK     dispatch W-1 shards to the pool, run shard 0 on this thread,
//            drain. Each shard appends into its own reserved FList segment.
//   COMPACT  close the gaps between segments, in BLOCK ORDER. This is the
//            step that makes the result deterministic: the surviving entries
//            end up in exactly the order a serial walk would have produced,
//            whatever order the shards finished in.
//   EPILOGUE one more re-entry with an epilogue-only shard for the omni
//            flares + particles (not per-mesh work; mutates Omni::V and
//            Particle::V in place, so it must run exactly once).
namespace {
struct XParAcc {
	static constexpr int MAXF = 4096;
	int frames = 0;
	int64_t cur[5] = {0,0,0,0,0};      // PLAN, WORK, COMPACT, EPILOGUE, TOTAL
	int64_t s[5][MAXF];
	int64_t shards = 0, meshes = 0, reserved = 0, pushed = 0;
	int64_t idleSum = 0;               // sum over frames of (WORK - maxShard)
};
XParAcc g_xpar;
void xparDump(int n) {
	XParAcc &a = g_xpar;
	const double f = double(n);
	double mn[5], md[5];
	for (int k = 0; k < 5; ++k) {
		std::sort(a.s[k], a.s[k] + n);
		mn[k] = a.s[k][0] / 1e6;
		md[k] = a.s[k][n / 2] / 1e6;
	}
	std::fprintf(stderr,
	    "[XFRM-PAR] n=%d shards=%.0f  min: TOTAL %7.3f | PLAN %6.3f WORK %6.3f "
	    "COMPACT %6.3f EPI %6.3f   p50: TOTAL %7.3f | PLAN %6.3f WORK %6.3f "
	    "COMPACT %6.3f EPI %6.3f ms  | meshes %.0f reserved %.0f pushed %.0f\n",
	    n, a.shards / f,
	    mn[4], mn[0], mn[1], mn[2], mn[3],
	    md[4], md[0], md[1], md[2], md[3],
	    a.meshes / f, a.reserved / f, a.pushed / f);
	a = XParAcc{};
}
}  // namespace

static void Transform_Objects_Sharded(Scene *Sc, fds::CameraContext &cam,
                                     fds::FaceListContext &faces, int nShards)
{
	const int  _xprofN = fds::FeatureFlags::xfrm_prof();
	const bool xp      = _xprofN > 0;
	const int64_t t0   = xp ? xpNow() : 0;

	ThreadPool &tp = ThreadPool::instance();
	// BLOCK COUNT, not worker count. These are decoupled on purpose: blocks
	// are work-STOLEN off a shared cursor, so oversubscribing the pool is how
	// the phase stays balanced. It has to be oversubscribed, because the cost
	// model below can only see VIndex/FIndex — it cannot see which meshes the
	// frustum + occlusion culls will throw away, and that is most of them
	// (471 TriMesh objects on the greets object list, 108 survive to a vertex
	// loop). With one block per worker and a static assignment, whichever
	// blocks happen to hold the survivors decide the phase: measured
	// WORK 0.508 ms against an LPT bound of 0.227 at greets t=5780.
	const int nWorkers = int(tp.size()) + 1;   // + this thread
	int W = (nShards < 0) ? nWorkers * 2 : nShards;
	if (W < 1) W = 1;

	// ── PLAN ────────────────────────────────────────────────────────────
	// Same walk, same order, same sequence numbering as the mesh loop's
	// `_mseq` — that identity is what makes the reservation line up.
	struct MeshPlan { int32_t nf; int64_t cost; };
	// thread_local, not plain static: the driver only ever runs on the tick
	// thread today, but a second concurrent main-view caller would otherwise
	// scribble on another's shard array while its workers hold pointers into it.
	static thread_local std::vector<MeshPlan>  plan;
	static thread_local std::vector<int>       blkLo, blkHi;
	static thread_local std::vector<XfrmShard> shards;
	plan.clear();
	int64_t totCost = 0;
	int64_t totFaces = 0;
	for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next) {
		if (Obj->Type != Obj_TriMesh) continue;
		const TriMesh *T = (const TriMesh *)(Obj->Data);
		const int32_t nf = T ? int32_t(T->FIndex) : 0;
		const int32_t nv = T ? int32_t(T->VIndex) : 0;
		// Cost weights from the measured per-item rates at greets t=5780
		// --greets_displace (--xfrm_prof: VERT 1.125 ms / 253 280 verts =
		// 4.44 ns, FACE 0.575 ms / 63 290 faces = 9.09 ns) -> face ~= 2 verts.
		const int64_t c = int64_t(nv) + 2 * int64_t(nf);
		plan.push_back({ nf, c });
		totFaces += nf;
		totCost  += c;
	}
	const int nMesh = int(plan.size());
	if (W > nMesh) W = (nMesh > 0) ? nMesh : 1;

	// One-shot AUDIT (once per Scene pointer, one compare per frame after
	// that): the whole safety argument for sharding by mesh is that no two
	// Objects share a TriMesh — otherwise two shards would write the same
	// Vertex/Face arrays concurrently. The serial path merely gets an
	// order-dependent result in that case; the sharded path gets a race.
	// Shout loudly rather than silently corrupt if a scene ever does it.
	{
		static thread_local const Scene *sAudited = nullptr;
		if (Sc != sAudited) {
			sAudited = Sc;
			std::vector<const void *> seen;
			seen.reserve(size_t(nMesh));
			for (Object *Obj = Sc->ObjectHead; Obj; Obj = Obj->Next)
				if (Obj->Type == Obj_TriMesh) seen.push_back(Obj->Data);
			std::sort(seen.begin(), seen.end());
			const size_t dups = size_t(seen.end() - std::unique(seen.begin(), seen.end()));
			if (dups) {
				std::fprintf(stderr,
				    "[XFRM-PAR] WARNING: %zu TriMesh instance(s) appear on the "
				    "object list more than once — mesh sharding would race on "
				    "their Vertex/Face arrays. Falling back to 1 shard.\n", dups);
				W = 1;
			}
		}
	}

	// FList capacity. The serial path only ever needs the SURVIVING faces,
	// but the reservation needs the per-mesh upper bounds, so a scene whose
	// FList was sized to something tighter than sum(FIndex) has to grow once.
	// (FList_Allocate / SceneDriver::setupFaceLists already size it to
	// sum(FIndex) + omnis + particles, so this is normally a no-op.)
	int32_t nOmni = 0;
	for (Omni *O = Sc->OmniHead; O; O = O->Next) { (void)O; ++nOmni; }
	const size_t need = size_t(totFaces) + size_t(nOmni)
	                  + size_t(Sc->NumOfParticles) * 2 + 16;
	if (faces.fStorage.size() < need) faces.resize(need);

	// Contiguous blocks, balanced on the cost estimate. Contiguous (not
	// strided, not work-stolen) because a block's FList segment is then ONE
	// run in mesh order, so COMPACT is W memmoves rather than a per-mesh
	// gather — and the output order is block order, trivially = mesh order.
	blkLo.clear(); blkHi.clear();
	shards.clear();
	{
		int64_t acc = 0, insOff = 0;
		int b = 0, lo = 0;
		for (int i = 0; i < nMesh; ++i) {
			acc += plan[i].cost;
			const bool last = (i == nMesh - 1);
			// Close the block once its accumulated cost crosses this block's
			// share of the total, but never emit more than W blocks.
			const bool closeIt = last
			    || (b + 1 < W && acc * W >= totCost * int64_t(b + 1));
			if (closeIt) {
				blkLo.push_back(lo);
				blkHi.push_back(i + 1);
				XfrmShard s;
				s.lo = lo; s.hi = i + 1; s.insOff = int32_t(insOff); s.out = 0;
				shards.push_back(s);
				for (int k = lo; k <= i; ++k) insOff += plan[k].nf;
				lo = i + 1;
				++b;
			}
		}
	}
	const int nBlocks = int(shards.size());
	const int64_t tPlan = xp ? xpNow() : 0;

	// ── WORK ────────────────────────────────────────────────────────────
	// WORK-STEALING over the blocks: one enqueue per WORKER (not per block),
	// each pulling block indices off a shared cursor; this thread pulls from
	// the same cursor rather than idling in the drain. Which worker executes
	// which block is therefore scheduling-dependent — and it does not matter,
	// because a block's OUTPUT POSITION was fixed by the reservation above.
	// That separation (execution order free, output order pinned) is the whole
	// design; it is also what lets the block count be tuned for balance
	// without touching determinism.
	{
		XfrmShard *const sbase = shards.data();
		auto cursor    = std::make_shared<std::atomic<int>>(0);
		const int nTasks = std::min(int(tp.size()), nBlocks);
		auto remaining = std::make_shared<std::atomic<int>>(nTasks);
		for (int k = 0; k < nTasks; ++k) {
			tp.enqueue([Sc, &cam, &faces, sbase, nBlocks, cursor, remaining]() {
				// Main-view context on the worker: every pass predicate this
				// function keys on is thread_local, and a pool worker that
				// last ran a shadow / mirror-shard task must not leak that
				// state into a main-view shard.
				extern thread_local bool g_inShadowPass;
				extern thread_local bool g_inDynamicShadowBake;
				extern thread_local ShadowMap *g_currentShadowMap;
				g_inShadowPass = false;
				g_inDynamicShadowBake = false;
				g_currentShadowOmni = nullptr;
				g_currentShadowMap  = nullptr;
				fds::g_offAxisFrustumCull = false;
				int i;
				while ((i = cursor->fetch_add(1, std::memory_order_relaxed)) < nBlocks) {
					g_xfrmShard = sbase + i;
					Transform_Objects(Sc, cam, faces);
					g_xfrmShard = nullptr;
				}
				remaining->fetch_sub(1, std::memory_order_release);
			});
		}
		int i;
		while ((i = cursor->fetch_add(1, std::memory_order_relaxed)) < nBlocks) {
			g_xfrmShard = sbase + i;
			Transform_Objects(Sc, cam, faces);
			g_xfrmShard = nullptr;
		}
		// Spin-then-yield drain. The tick thread has nothing else to do and
		// the blocks are tens of microseconds each, so a condvar round-trip
		// would cost more than the wait itself.
		while (remaining->load(std::memory_order_acquire) > 0) {
			std::this_thread::yield();
		}
	}
	const int64_t tWork = xp ? xpNow() : 0;

	// ── COMPACT ─────────────────────────────────────────────────────────
	// Each shard filled the FRONT of its reserved segment. Slide the runs
	// down in block order; destination never exceeds source, and blocks are
	// processed in increasing order, so this is a safe in-place compaction
	// and the resulting order is exactly the serial insertion order.
	int32_t outN = 0;
	for (int b = 0; b < nBlocks; ++b) {
		const int32_t src = shards[b].insOff;
		const int32_t cnt = shards[b].out;
		if (cnt > 0 && outN != src) {
			std::memmove(faces.fList + outN, faces.fList + src,
			             size_t(cnt) * sizeof(fds::FListEntry));
		}
		outN += cnt;
	}
	faces.cPolys = outN;
	const int64_t tComp = xp ? xpNow() : 0;

	// ── EPILOGUE ────────────────────────────────────────────────────────
	{
		XfrmShard epi;
		epi.lo = 0; epi.hi = 0; epi.insOff = outN; epi.out = 0;
		g_xfrmShard = &epi;
		Transform_Objects(Sc, cam, faces);
		g_xfrmShard = nullptr;
	}

	if (xp) {
		XParAcc &a = g_xpar;
		const int64_t t4 = xpNow();
		a.cur[0] = tPlan - t0;
		a.cur[1] = tWork - tPlan;
		a.cur[2] = tComp - tWork;
		a.cur[3] = t4 - tComp;
		a.cur[4] = t4 - t0;
		const int fi = a.frames;
		if (fi < XParAcc::MAXF) {
			for (int k = 0; k < 5; ++k) { a.s[k][fi] = a.cur[k]; a.cur[k] = 0; }
			++a.frames;
		}
		a.shards += nBlocks;
		a.meshes += nMesh;
		a.reserved += totFaces;
		a.pushed += outN;
		if (a.frames >= _xprofN || a.frames >= XParAcc::MAXF) xparDump(a.frames);
	}
}
