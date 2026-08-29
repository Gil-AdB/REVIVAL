// ─── REFERENCE RELIEF RENDERER for the greets stone (--greets_displace_ref) ──
//
// WHY THIS EXISTS.  A month of tessellated-displacement fixes was measured
// against an invented reference.  This file renders the DEFINITION of the
// displaced surface, per pixel, with NO tessellation, so that (a) the model
// itself can be judged by eye at his poses and (b) the tessellated bake can be
// measured against it per pixel.  It is an INDEPENDENT SECOND IMPLEMENTATION:
// it does not call, read or resemble DisplaceStoneSubdiv, and it runs on the
// AUTHORED (undisplaced) faces — i.e. with --greets_displace OFF.
//
// THE MODEL — docs/DISPLACEMENT_RESEARCH_II.md §REF has the full write-up and
// every place the definition was ambiguous plus what was chosen;
// docs/DISPLACEMENT_LITERATURE.md is the survey it is built against.
//
//   * Each authored planar face is an offset SLAB: the points whose signed
//     distance from the face's own AUTHORED PLANE lies in [-back, d(u,v)],
//     with d(u,v) = amp * (h(u,v) - mipMean), h bilinear at texel centres with
//     toroidal wrap at the bake mip.  The displacement DIRECTION is the face's
//     plane normal and nothing else: OpenSubdiv states that displacing along a
//     normal that is discontinuous at a crease "will likely tear apart the
//     surface along the crease", so a smoothed vertex normal has no correct
//     version here (survey §B, §F/1).
//
//   * A slab is EXTENDED past each authored SHARED edge by hAbsMax·cot(φ/2),
//     φ = the angle between the two face normals (survey §G's correction — a
//     margin of h alone is exact only at a right angle and under-extends on
//     shallow bends), mitre-limited, and TRIMMED at the mitre point across a
//     convex edge (survey §B: "at a convex material corner the offset faces
//     must be extended to reach the miter point (spike risk)").
//
//   * A FREE edge — used by exactly one face, with no face of ANY material
//     sharing it or lying position-coincident across it — gets NO extension,
//     so its exposed lateral face IS the skirt.
//
//   * Where two slabs meet the surface is partitioned by the NEAREST BASE
//     PLANE (the bisector), which castellates with a step wherever the two
//     faces' heights differ.  --greets_displace_ref_partition=0 switches to a
//     plain union; --greets_displace_ref_shared_edge instead forces both sides
//     to read ONE dominant height along the crease, which is what the
//     literature actually does and which makes the step vanish on its own.
//
//   * The shading normal is n ∝ N − ∇ₛh in the plane's own frame, EXACT on a
//     plane (Mikkelsen's surface-gradient form, survey §C), with N the face
//     normal at a crease and the crease-aware interpolated authored normal
//     below the threshold.
//
// THE MARCH is a per-texel DDA with an EXACT quadratic root inside each
// bilinear cell plus a conservative per-cell max-height skip.  Linear search
// plus binary refinement (Policarpo 2005 / POM) is NOT conservative — it steps
// over thin features — and this is a reference, so it is not used anywhere.
// The march always starts OUTSIDE the solid, which is what makes min-over-slabs
// exact (survey §G).
//
// EVERY boundary of the "is this point in the solid" predicate is enumerated as
// an exact EVENT (slab crossings from the DDA, bisector crossings, extended-
// polygon boundaries, band boundaries).  The first interval whose interior is
// inside the solid starts at the hit.  That is what makes the castellation
// STEP an exact planar hit rather than something a step size might miss.
//
// COST is deliberately not a consideration: this runs at snapshot time, takes
// seconds, and is default OFF so the shipping arm is byte-identical.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <semaphore>
#include <string>
#include <unordered_map>
#include <vector>

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "Base/Compiler.h"
#include "Base/FrameState.h"
#include "Base/Scene.h"
#include "Base/TriMesh.h"
#include "Base/Material.h"
#include "Base/Texture.h"
#include "FILLERS/Mekalele.h"
#include "Threads.h"

namespace renderns { extern std::counting_semaphore<INT_MAX> tileDone; }

namespace fds {
namespace refrender {

// ═══════════════════════════════════════════════════════════════════════════
// 0.  Small double-precision vector helpers (this file only).
//     Doubles throughout the model build: the whole point of the exercise is
//     that the reference is not the thing under suspicion.
// ═══════════════════════════════════════════════════════════════════════════
struct V3 {
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	explicit V3(const Vector &v) : x(v.x), y(v.y), z(v.z) {}
};
static inline V3 operator+(const V3 &a, const V3 &b) { return V3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline V3 operator-(const V3 &a, const V3 &b) { return V3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline V3 operator*(const V3 &a, double s)    { return V3(a.x*s, a.y*s, a.z*s); }
static inline double dot(const V3 &a, const V3 &b)   { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3 cross(const V3 &a, const V3 &b) {
	return V3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline double len(const V3 &a) { return std::sqrt(dot(a, a)); }
static inline V3 nrm(const V3 &a) { const double l = len(a); return l > 1e-30 ? a * (1.0/l) : V3(0,0,1); }

static inline V3 MatVec(const Matrix M, const V3 &u) {
	return V3(double(M[0][0])*u.x + double(M[0][1])*u.y + double(M[0][2])*u.z,
	          double(M[1][0])*u.x + double(M[1][1])*u.y + double(M[1][2])*u.z,
	          double(M[2][0])*u.x + double(M[2][1])*u.y + double(M[2][2])*u.z);
}

// ═══════════════════════════════════════════════════════════════════════════
// 1.  The height field.
// ═══════════════════════════════════════════════════════════════════════════

// The block-tiled texel address SampleHeight8Bilinear uses.  Byte-for-byte the
// same function as DEMO/MeshOps.cpp:428 (the bake's).  COPIED, not shared, on
// purpose: an independent implementation must not link against the bake.
static inline size_t SwizzledOffsetRef(int x, int y, int bsx, int bsy, int SizeY) {
	const int BX = 1 << bsx, BY = 1 << bsy;
	const int blockRowsPerCol = SizeY >> bsy;
	return (size_t(x >> bsx) * blockRowsPerCol + size_t(y >> bsy)) * size_t(BX * BY)
	       + size_t(y & (BY - 1)) * BX + size_t(x & (BX - 1));
}

struct RefMat {
	Material      *mat  = nullptr;
	const Texture *hm   = nullptr;
	const byte    *data = nullptr;
	int    mip = 0, mw = 0, mh = 0, bsx = 0, bsy = 0;
	double mean = 0.5, amp = 0.3;
	double h0 = 0.0, h1 = 1.0;       // min / max of the mip, 0..1
	double dMax = 0.0, dMin = 0.0;   // amp*(h1-mean) / amp*(h0-mean)
	double dAbs = 0.0;               // max |d| — the offset the mitre is sized on
	uint32_t matID = 0;
	std::vector<uint8_t> cellMax;    // per bilinear CELL, max of its 4 corner texels

	inline int wx(int a) const { const int r = a % mw; return r < 0 ? r + mw : r; }
	inline int wy(int a) const { const int r = a % mh; return r < 0 ? r + mh : r; }
	inline double texel(int x, int y) const {
		return double(data[SwizzledOffsetRef(wx(x), wy(y), bsx, bsy, mh)]);
	}
	inline void corners(int cx, int cy, double &h00, double &h10, double &h01, double &h11) const {
		h00 = texel(cx, cy);     h10 = texel(cx + 1, cy);
		h01 = texel(cx, cy + 1); h11 = texel(cx + 1, cy + 1);
	}
	// h in 0..1 at a normalized UV: texel-centre convention + toroidal wrap.
	inline double sampleH(double u, double v) const {
		const double x = u * mw - 0.5, y = v * mh - 0.5;
		const double fxf = std::floor(x), fyf = std::floor(y);
		const double fx = x - fxf, fy = y - fyf;
		double h00, h10, h01, h11;
		corners(int(fxf), int(fyf), h00, h10, h01, h11);
		const double top = h00 + (h10 - h00) * fx;
		const double bot = h01 + (h11 - h01) * fx;
		return (top + (bot - top) * fy) * (1.0 / 255.0);
	}
	// ∂h/∂u, ∂h/∂v of the SAME bilinear reconstruction, analytic.  The gradient
	// must come from the level the displacement came from or geometry and
	// shading disagree (survey §C, Nießner & Loop 2013).
	inline void gradH(double u, double v, double &hu, double &hv) const {
		const double x = u * mw - 0.5, y = v * mh - 0.5;
		const double fxf = std::floor(x), fyf = std::floor(y);
		const double fx = x - fxf, fy = y - fyf;
		double h00, h10, h01, h11;
		corners(int(fxf), int(fyf), h00, h10, h01, h11);
		const double k = 1.0 / 255.0;
		hu = ((h10 - h00) + (h11 - h01 - h10 + h00) * fy) * k * mw;
		hv = ((h01 - h00) + (h11 - h01 - h10 + h00) * fx) * k * mh;
	}
	inline double cellMaxAt(int cx, int cy) const {
		return double(cellMax[size_t(wy(cy)) * size_t(mw) + size_t(wx(cx))]) * (1.0 / 255.0);
	}
	inline double dOfH(double h) const { return amp * (h - mean); }
};

// ═══════════════════════════════════════════════════════════════════════════
// 2.  Canonical position keys.
//     DiagSplit's precision rule — "ordering control points along an edge by
//     their world space position ensures consistent numerical evaluation" —
//     applies to everything derived from a shared edge here: the dominant
//     owner, the blend parameter, the margin.  So the two endpoints of an edge
//     are ORDERED BY WORLD POSITION before anything is derived from them, and
//     the derivation never sees which face was walked first.
// ═══════════════════════════════════════════════════════════════════════════
struct PKey {
	int64_t x, y, z;
	bool operator==(const PKey &o) const { return x==o.x && y==o.y && z==o.z; }
	bool operator<(const PKey &o) const {
		if (x != o.x) return x < o.x;
		if (y != o.y) return y < o.y;
		return z < o.z;
	}
};
struct PKeyH { size_t operator()(const PKey &k) const {
	return size_t(k.x) * 73856093u ^ size_t(k.y) * 19349663u ^ size_t(k.z) * 83492791u; } };
static constexpr double kPosGrid = 1.0e4;      // 1e-4 world-unit lattice
static inline PKey pkey(const V3 &p) {
	return PKey{ int64_t(std::llround(p.x * kPosGrid)),
	             int64_t(std::llround(p.y * kPosGrid)),
	             int64_t(std::llround(p.z * kPosGrid)) };
}
struct EKey {
	PKey a, b;                                  // ALWAYS a < b (world order)
	bool operator==(const EKey &o) const { return a == o.a && b == o.b; }
};
struct EKeyH { size_t operator()(const EKey &k) const {
	PKeyH h; return h(k.a) * 1000003u ^ h(k.b); } };
static inline EKey ekey(const V3 &p, const V3 &q) {
	PKey a = pkey(p), b = pkey(q);
	if (b < a) std::swap(a, b);                 // world-position order, not walk order
	return EKey{ a, b };
}

// ═══════════════════════════════════════════════════════════════════════════
// 3.  The model.
// ═══════════════════════════════════════════════════════════════════════════
enum : int8_t { EDGE_FREE = 0, EDGE_CONVEX = 1, EDGE_CONCAVE = 2 };

struct RefFace {
	V3     p[3];                 // authored world verts
	V3     vn[3];                // crease-aware corner normals (world)
	double u[3] = {0,0,0}, v[3] = {0,0,0};
	V3     n;   double d = 0;    // authored plane: n unit + outward, n·x = d
	V3     Tu, Tv;               // ∂P/∂u, ∂P/∂v, world units per UV unit
	V3     gu, gv;               // u(p) = u[0] + gu·(p − p[0])
	V3     em[3];  double ec[3] = {0,0,0};   // edge i (p[i]→p[i+1]) outward line
	double margin[3] = {0,0,0};
	int8_t kind[3]   = {EDGE_FREE, EDGE_FREE, EDGE_FREE};
	bool   smoothEdge[3] = {false,false,false};   // φ below the crease threshold
	int    nbrFace[3] = {-1,-1,-1};          // stone neighbour, else -1
	V3     nbrN[3];  double nbrD[3] = {0,0,0};   // ANY neighbour's plane (trim)
	bool   hasNbr[3] = {false,false,false};
	int    domOwner[3] = {-1,-1,-1};         // dominant height owner (stone only)
	V3     ext[3];               // extended-triangle corners (world)
	int    matIdx = 0;
	uint16_t shadowMatID = 0;
	uint8_t  lastMip = 0;
	double worldPerTexel = 0.01;
	bool   anyShared = false;
};

struct RefFaceV {                 // per-frame view-space projection
	V3 p0, n;  double d = 0;
	V3 em[3];  double ec[3] = {0,0,0};
	V3 gu, gv; double u0 = 0, v0 = 0;
	V3 ext[3];
	V3 vnv[3];                    // corner normals, view space
	V3 Tuv, Tvv;                  // tangent frame, view space
	double sxMin = 0, sxMax = 0, syMin = 0, syMax = 0;   // screen bbox
	bool  onScreen = false;
};

struct RefModel {
	bool   built = false;
	Scene *scene = nullptr;
	std::vector<RefMat>   mats;
	std::vector<RefFace>  faces;
	std::vector<RefFaceV> fv;
	// tunables resolved at build
	double back = 0.3, creaseDeg = 30.0, mitreLimit = 4.0, maxExt = 2.0;
	double marginOverride = 0.0, freeTol = 0.05, edgeBandTex = 2.0;
	int    partition = 1, maxCells = 6000;
	bool   sharedEdge = false, mitreTrim = true;
	// build census
	int nFree = 0, nConvex = 0, nConcave = 0, nExtCapped = 0, nBevel = 0;
	int nNonStoneNbr = 0, nSoup = 0, nSmooth = 0;
	int phiHist[9] = {0,0,0,0,0,0,0,0,0};   // 0-10,10-20,...,80+ degrees
	double maxMargin = 0.0;
	// tile bins
	int tilesX = 0, tilesY = 0, tilePx = 32;
	std::vector<std::vector<int32_t>> bins;
};

static RefModel g_m;

// The scene's whole face soup in world space — material-BLIND on purpose: a
// wall and the floor are two planes of one solid, and a face of ANY material
// across an edge is enough to make that edge not free.
// `n` is the AUTHORED outward normal (geometric normal oriented by the engine's
// own F->N).  A winding-dependent normal here is not a cosmetic issue: the mitre
// trim asks which side of the neighbour's plane the material is on, so a flipped
// soup normal silently inverts the trim for that edge.
struct SoupFace { V3 p[3]; V3 n; const Material *mat = nullptr; };

// ═══════════════════════════════════════════════════════════════════════════
// 4.  Build.
// ═══════════════════════════════════════════════════════════════════════════

static inline V3 WorldPos(const TriMesh *T, const Vertex *vx) {
	const V3 r = MatVec(T->RotMat, V3(vx->Pos));
	return V3(r.x + T->IPos.x, r.y + T->IPos.y, r.z + T->IPos.z);
}

static bool IsStoneName(const char *nm) {
	return nm && (!std::strcmp(nm, "rooms") || !std::strcmp(nm, "floor"));
}

// Uniform spatial grid over the soup, for the free-edge probe.
struct SoupGrid {
	double cell = 0.5;
	V3 lo, hi;
	int nx = 1, ny = 1, nz = 1;
	std::vector<std::vector<int32_t>> b;
	inline int idx(int x, int y, int z) const { return (z * ny + y) * nx + x; }
	void build(const std::vector<SoupFace> &s, double c) {
		cell = c;
		lo = V3(1e30, 1e30, 1e30); hi = V3(-1e30, -1e30, -1e30);
		for (const SoupFace &f : s) for (int k = 0; k < 3; ++k) {
			lo.x = std::min(lo.x, f.p[k].x); hi.x = std::max(hi.x, f.p[k].x);
			lo.y = std::min(lo.y, f.p[k].y); hi.y = std::max(hi.y, f.p[k].y);
			lo.z = std::min(lo.z, f.p[k].z); hi.z = std::max(hi.z, f.p[k].z);
		}
		if (s.empty()) { lo = V3(0,0,0); hi = V3(1,1,1); }
		nx = std::max(1, std::min(256, int((hi.x - lo.x) / cell) + 1));
		ny = std::max(1, std::min(256, int((hi.y - lo.y) / cell) + 1));
		nz = std::max(1, std::min(256, int((hi.z - lo.z) / cell) + 1));
		b.assign(size_t(nx) * ny * nz, {});
		for (size_t i = 0; i < s.size(); ++i) {
			V3 a = s[i].p[0], c2 = s[i].p[0];
			for (int k = 1; k < 3; ++k) {
				a.x = std::min(a.x, s[i].p[k].x); c2.x = std::max(c2.x, s[i].p[k].x);
				a.y = std::min(a.y, s[i].p[k].y); c2.y = std::max(c2.y, s[i].p[k].y);
				a.z = std::min(a.z, s[i].p[k].z); c2.z = std::max(c2.z, s[i].p[k].z);
			}
			const int x0 = cx(a.x), x1 = cx(c2.x), y0 = cy(a.y), y1 = cy(c2.y),
			          z0 = cz(a.z), z1 = cz(c2.z);
			for (int z = z0; z <= z1; ++z) for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x)
				b[size_t(idx(x, y, z))].push_back(int32_t(i));
		}
	}
	inline int cx(double x) const { return std::max(0, std::min(nx-1, int((x - lo.x) / cell))); }
	inline int cy(double y) const { return std::max(0, std::min(ny-1, int((y - lo.y) / cell))); }
	inline int cz(double z) const { return std::max(0, std::min(nz-1, int((z - lo.z) / cell))); }
};

// Squared distance from a point to a triangle (standard clamped-barycentric).
static double PointTriDist2(const V3 &p, const V3 &a, const V3 &b, const V3 &c) {
	const V3 ab = b - a, ac = c - a, ap = p - a;
	const double d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return dot(ap, ap);
	const V3 bp = p - b;
	const double d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return dot(bp, bp);
	const double vc = d1*d4 - d3*d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) { const double t = d1/(d1-d3); const V3 q = a + ab*t - p; return dot(q,q); }
	const V3 cp = p - c;
	const double d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return dot(cp, cp);
	const double vb = d5*d2 - d1*d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) { const double t = d2/(d2-d6); const V3 q = a + ac*t - p; return dot(q,q); }
	const double va = d3*d6 - d5*d4;
	if (va <= 0 && (d4-d3) >= 0 && (d5-d6) >= 0) {
		const double t = (d4-d3)/((d4-d3)+(d5-d6)); const V3 q = b + (c-b)*t - p; return dot(q,q);
	}
	const double den = 1.0/(va+vb+vc), vv = vb*den, ww = vc*den;
	const V3 q = a + ab*vv + ac*ww - p; return dot(q, q);
}

FDS_NOINLINE static bool BuildModel(Scene *Sc)
{
	using FF = fds::FeatureFlags;
	g_m = RefModel();
	g_m.scene       = Sc;
	g_m.creaseDeg   = double(FF::greets_displace_ref_crease());
	g_m.mitreLimit  = double(FF::greets_displace_ref_mitre());
	g_m.maxExt      = double(FF::greets_displace_ref_max_ext());
	g_m.marginOverride = double(FF::greets_displace_ref_margin());
	g_m.freeTol     = double(FF::greets_displace_ref_free_tol());
	g_m.edgeBandTex = double(FF::greets_displace_ref_edge_band());
	g_m.partition   = FF::greets_displace_ref_partition();
	g_m.maxCells    = std::max(16, FF::greets_displace_ref_steps());
	g_m.sharedEdge  = FF::greets_displace_ref_shared_edge();
	g_m.mitreTrim   = FF::greets_displace_ref_mitre_trim();

	const double amp = (FF::greets_displace_ref_amp() > 0.0f)
	                 ? double(FF::greets_displace_ref_amp()) : double(FF::greets_displace_amp());
	const int mipReq = (FF::greets_displace_ref_mip() >= 0)
	                 ? FF::greets_displace_ref_mip() : FF::greets_displace_mip();
	const int verbose = FF::greets_displace_ref_stats();

	// ── materials ──────────────────────────────────────────────────────────
	MatTable mt = Scene_GetMatTable(Sc);
	for (Material *M = MatLib; M; M = M->Next) {
		if (M->RelScene != Sc || !IsStoneName(M->Name) || !M->HeightMap) continue;
		const Texture *hm = M->HeightMap;
		if (hm->BPP != 8 || !hm->numMipmaps) {
			std::fprintf(stderr, "[REFRENDER] '%s': height map is not 8-bit tiled — skipped\n", M->Name);
			continue;
		}
		RefMat rm;
		rm.mat = M; rm.hm = hm; rm.amp = amp;
		int mip = std::min(mipReq, int(hm->numMipmaps) - 1); if (mip < 0) mip = 0;
		while (mip > 0 && ((std::max(1, int(hm->SizeX) >> mip) < (1 << hm->blockSizeX)) ||
		                   (std::max(1, int(hm->SizeY) >> mip) < (1 << hm->blockSizeY)))) --mip;
		rm.mip = mip;
		rm.mw = std::max(1, int(hm->SizeX) >> mip);
		rm.mh = std::max(1, int(hm->SizeY) >> mip);
		rm.bsx = hm->blockSizeX; rm.bsy = hm->blockSizeY;
		rm.data = hm->Mipmap[mip];
		if (!rm.data) continue;
		// mipMean exactly as the bake computes it: the plain mean of the mip's
		// bytes (the swizzle is a permutation, so the mean is the same).
		{
			const size_t n = size_t(rm.mw) * size_t(rm.mh);
			uint64_t sum = 0; byte lo = 255, hi = 0;
			for (size_t i = 0; i < n; ++i) { const byte b = rm.data[i];
				sum += b; if (b < lo) lo = b; if (b > hi) hi = b; }
			rm.mean = double(sum) / double(n) / 255.0;
			rm.h0 = double(lo) / 255.0; rm.h1 = double(hi) / 255.0;
		}
		rm.dMax = rm.amp * (rm.h1 - rm.mean);
		rm.dMin = rm.amp * (rm.h0 - rm.mean);
		rm.dAbs = std::max(std::fabs(rm.dMax), std::fabs(rm.dMin));
		rm.matID = M->ID;
		// conservative per-CELL maximum (Tevs et al. max-mipmap, one level).
		rm.cellMax.assign(size_t(rm.mw) * size_t(rm.mh), 0);
		for (int y = 0; y < rm.mh; ++y) for (int x = 0; x < rm.mw; ++x) {
			double a, b, c, d;
			rm.corners(x, y, a, b, c, d);
			rm.cellMax[size_t(y) * rm.mw + x] = uint8_t(std::max(std::max(a, b), std::max(c, d)));
		}
		g_m.mats.push_back(std::move(rm));
	}
	if (g_m.mats.empty()) {
		std::fprintf(stderr, "[REFRENDER] no stone material with a height map — pass is inert\n");
		g_m.built = true;
		return false;
	}
	g_m.back = (FF::greets_displace_ref_back() > 0.0f)
	         ? double(FF::greets_displace_ref_back()) : amp;

	auto matIndexOf = [&](const Material *M) -> int {
		for (size_t i = 0; i < g_m.mats.size(); ++i) if (g_m.mats[i].mat == M) return int(i);
		return -1;
	};

	// ── the soup + the stone faces ─────────────────────────────────────────
	std::vector<SoupFace> soup;
	std::vector<std::pair<const TriMesh*, const Face*>> stoneSrc;
	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
		if (!T->Faces || !T->Verts) continue;
		for (dword i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C) continue;
			SoupFace sf;
			sf.p[0] = WorldPos(T, F.A); sf.p[1] = WorldPos(T, F.B); sf.p[2] = WorldPos(T, F.C);
			sf.n = nrm(cross(sf.p[1] - sf.p[0], sf.p[2] - sf.p[0]));
			{	const V3 eN = nrm(MatVec(T->RotMat, V3(F.N)));
				if (dot(sf.n, eN) < 0.0) sf.n = sf.n * -1.0; }
			sf.mat = F.Txtr;
			if (len(cross(sf.p[1] - sf.p[0], sf.p[2] - sf.p[0])) < 1e-12) continue;   // degenerate
			soup.push_back(sf);
			if (F.Txtr && IsStoneName(F.Txtr->Name) && matIndexOf(F.Txtr) >= 0)
				stoneSrc.emplace_back(T, &F);
		}
	}
	g_m.nSoup = int(soup.size());

	// The FACE NORMAL that matters is the AUTHORED plane's, and the engine
	// already carries it as F->N / F->NormProd (object space).  Use the
	// geometric normal of the world triangle and orient it with the engine's,
	// so an authored winding flip cannot silently invert the whole slab.
	g_m.faces.reserve(stoneSrc.size());
	for (auto &ps : stoneSrc) {
		const TriMesh *T = ps.first; const Face &F = *ps.second;
		RefFace rf;
		rf.p[0] = WorldPos(T, F.A); rf.p[1] = WorldPos(T, F.B); rf.p[2] = WorldPos(T, F.C);
		const V3 gn = cross(rf.p[1] - rf.p[0], rf.p[2] - rf.p[0]);
		if (len(gn) < 1e-12) continue;
		rf.n = nrm(gn);
		const V3 eN = nrm(MatVec(T->RotMat, V3(F.N)));
		if (dot(rf.n, eN) < 0.0) rf.n = rf.n * -1.0;      // orient to the engine's
		rf.d = dot(rf.n, rf.p[0]);
		rf.u[0] = F.U1; rf.v[0] = F.V1;
		rf.u[1] = F.U2; rf.v[1] = F.V2;
		rf.u[2] = F.U3; rf.v[2] = F.V3;
		rf.matIdx = matIndexOf(F.Txtr);

		rf.shadowMatID = F.ShadowMatID ? F.ShadowMatID
		               : (F.Txtr->ShadowMatID ? F.Txtr->ShadowMatID : uint16_t(F.Txtr->ID + 1));
		rf.lastMip = (F.LastMip == 0xFF) ? 0 : F.LastMip;
		// UV chart: ∂P/∂u, ∂P/∂v from the triangle, and the inverse gradients.
		{
			const double du1 = rf.u[1] - rf.u[0], dv1 = rf.v[1] - rf.v[0];
			const double du2 = rf.u[2] - rf.u[0], dv2 = rf.v[2] - rf.v[0];
			const double det = du1 * dv2 - du2 * dv1;
			const V3 e1 = rf.p[1] - rf.p[0], e2 = rf.p[2] - rf.p[0];
			if (std::fabs(det) < 1e-14) continue;         // degenerate chart
			const double inv = 1.0 / det;
			rf.Tu = (e1 * dv2 - e2 * dv1) * inv;
			rf.Tv = (e2 * du1 - e1 * du2) * inv;
			// u(p) = u0 + gu·(p − p0): the in-plane dual basis of (Tu, Tv).
			const V3 nn = rf.n;
			const double a11 = dot(rf.Tu, rf.Tu), a12 = dot(rf.Tu, rf.Tv), a22 = dot(rf.Tv, rf.Tv);
			const double dt = a11 * a22 - a12 * a12;
			if (std::fabs(dt) < 1e-20) continue;
			const double i11 =  a22 / dt, i12 = -a12 / dt, i22 =  a11 / dt;
			rf.gu = rf.Tu * i11 + rf.Tv * i12;
			rf.gv = rf.Tu * i12 + rf.Tv * i22;
			(void)nn;
			const RefMat &rm = g_m.mats[size_t(rf.matIdx)];
			const double wpt = 0.5 * (len(rf.Tu) / rm.mw + len(rf.Tv) / rm.mh);
			rf.worldPerTexel = (wpt > 1e-9) ? wpt : 0.01;
		}
		// edge lines, in-plane outward
		for (int i = 0; i < 3; ++i) {
			const V3 a = rf.p[i], b = rf.p[(i+1)%3], c = rf.p[(i+2)%3];
			V3 m = cross(rf.n, b - a);
			if (dot(m, c - a) > 0.0) m = m * -1.0;         // point AWAY from the interior
			rf.em[i] = nrm(m);
			rf.ec[i] = dot(rf.em[i], a);
		}
		g_m.faces.push_back(rf);
	}
	if (g_m.faces.empty()) {
		std::fprintf(stderr, "[REFRENDER] no authored stone faces found — pass is inert\n");
		g_m.built = true;
		return false;
	}

	// ── crease-aware corner normals ────────────────────────────────────────
	// Per (face, corner): the area-weighted average of the normals of the
	// stone faces sharing that POSITION whose normal is within the crease
	// threshold of this face's.  Never an average across a crease — at a
	// crease the surface is not differentiable and the question is malformed
	// (survey §C, Max 1999).
	{
		std::unordered_map<PKey, std::vector<int32_t>, PKeyH> byPos;
		for (size_t f = 0; f < g_m.faces.size(); ++f)
			for (int k = 0; k < 3; ++k) byPos[pkey(g_m.faces[f].p[k])].push_back(int32_t(f));
		const double cosT = std::cos(g_m.creaseDeg * M_PI / 180.0);
		for (size_t f = 0; f < g_m.faces.size(); ++f) {
			RefFace &rf = g_m.faces[f];
			const double area = 0.5 * len(cross(rf.p[1]-rf.p[0], rf.p[2]-rf.p[0]));
			(void)area;
			for (int k = 0; k < 3; ++k) {
				V3 acc(0,0,0);
				auto it = byPos.find(pkey(rf.p[k]));
				if (it != byPos.end()) for (int32_t g : it->second) {
					const RefFace &og = g_m.faces[size_t(g)];
					if (dot(og.n, rf.n) < cosT) continue;   // across a crease: excluded
					const double a2 = 0.5 * len(cross(og.p[1]-og.p[0], og.p[2]-og.p[0]));
					acc = acc + og.n * a2;
				}
				rf.vn[k] = (len(acc) > 1e-12) ? nrm(acc) : rf.n;
			}
		}
	}

	// ── edge classification (survey §E: a 4-tuple, never one enum) ─────────
	// face count is the only topological fact; material identity and
	// coplanarity are ATTRIBUTES of the two faces.  An edge is FREE only when
	// it is used by exactly one face AND no face of any material lies
	// position-coincident across it (ledger 10994f6ef014: vertex coincidence
	// alone freed two walls that meet geometrically and opened a full-height
	// slit).
	{
		// stone-stone index-shared edges
		std::unordered_map<EKey, std::vector<std::pair<int32_t,int8_t>>, EKeyH> shared;
		for (size_t f = 0; f < g_m.faces.size(); ++f)
			for (int i = 0; i < 3; ++i)
				shared[ekey(g_m.faces[f].p[i], g_m.faces[f].p[(i+1)%3])]
					.emplace_back(int32_t(f), int8_t(i));
		// every soup edge, for the "used by more than one face" test
		std::unordered_map<EKey, int, EKeyH> soupCount;
		for (const SoupFace &sf : soup)
			for (int i = 0; i < 3; ++i) ++soupCount[ekey(sf.p[i], sf.p[(i+1)%3])];

		SoupGrid grid;
		grid.build(soup, std::max(0.25, g_m.freeTol * 8.0));
		const double tol2 = g_m.freeTol * g_m.freeTol;

		for (size_t f = 0; f < g_m.faces.size(); ++f) {
			RefFace &rf = g_m.faces[f];
			for (int i = 0; i < 3; ++i) {
				const V3 a = rf.p[i], b = rf.p[(i+1)%3];
				const EKey ek = ekey(a, b);
				int    nbr = -1;
				V3     nbrN;  double nbrD = 0;  bool have = false;

				// (1) a stone face sharing the edge by position
				auto it = shared.find(ek);
				if (it != shared.end()) for (auto &pr : it->second) {
					if (size_t(pr.first) == f) continue;
					nbr = pr.first; nbrN = g_m.faces[size_t(nbr)].n;
					nbrD = g_m.faces[size_t(nbr)].d; have = true; break;
				}
				// (2) any soup face sharing the edge, or lying coincident across it
				if (!have) {
					// Face-soup probe at three interior points of the edge.  Vertex
					// coincidence alone is NOT enough: ledger 10994f6ef014 records two
					// walls meeting geometrically with no shared vertex, both classified
					// open, both freed, opening a full-height slit.
					int bestS = -1; double bestD2 = tol2;
					for (int s = 0; s <= 2; ++s) {
						const V3 q = a + (b - a) * (0.25 + 0.25 * s);
						const int gx = grid.cx(q.x), gy = grid.cy(q.y), gz = grid.cz(q.z);
						for (int dz = -1; dz <= 1; ++dz) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
							const int x = gx+dx, y = gy+dy, z = gz+dz;
							if (x < 0 || y < 0 || z < 0 || x >= grid.nx || y >= grid.ny || z >= grid.nz) continue;
							for (int32_t si : grid.b[size_t(grid.idx(x,y,z))]) {
								const SoupFace &sf = soup[size_t(si)];
								// skip this very face
								if (pkey(sf.p[0]) == pkey(rf.p[0]) && pkey(sf.p[1]) == pkey(rf.p[1])
								    && pkey(sf.p[2]) == pkey(rf.p[2])) continue;
								const double d2 = PointTriDist2(q, sf.p[0], sf.p[1], sf.p[2]);
								if (d2 < bestD2) { bestD2 = d2; bestS = si; }
							}
						}
					}
					if (bestS >= 0) {
						const SoupFace &sf = soup[size_t(bestS)];
						nbrN = sf.n; nbrD = dot(sf.n, sf.p[0]); have = true;
						++g_m.nNonStoneNbr;
					}
				}

				if (!have) { rf.kind[i] = EDGE_FREE; rf.margin[i] = 0.0; ++g_m.nFree; continue; }

				// orient the neighbour plane outward (same side as this face's air)
				if (dot(nbrN, rf.n) < 0.0 && nbr < 0) { /* keep — sign resolved below */ }
				rf.hasNbr[i] = true; rf.nbrFace[i] = nbr; rf.nbrN[i] = nbrN; rf.nbrD[i] = nbrD;

				// convex or concave, from the neighbour's far vertex
				// (the third vertex of THIS face tells which side is material).
				const V3 c = rf.p[(i+2)%3];
				const double sideOwn = dot(nbrN, c) - nbrD;    // <0 ⇒ own face is behind nbr's plane
				rf.kind[i] = (sideOwn < 0.0) ? EDGE_CONVEX : EDGE_CONCAVE;
				if (rf.kind[i] == EDGE_CONVEX) ++g_m.nConvex; else ++g_m.nConcave;
				rf.anyShared = true;

				// ── how far this face may extend past this edge ───────────────
				// TWO QUESTIONS, TWO ANSWERS (survey §F/7: "two-level
				// classification").  A CREASE is a corner: the bisector
				// partitions it, and the extension the bisector needs is
				// hAbsMax·cot(φ/2) (survey §G's correction), while the extension
				// the MITRE needs is hAbsMax·tan(φ/2) — cot dominates up to 90°,
				// tan past it, so the edge needs the max of the two.
				// A SMOOTH seam (φ below the crease threshold) is not a corner at
				// all: the two strips are one surface, the plane bisector of two
				// near-parallel planes lies far away and is meaningless there, and
				// only the mitre requirement tan(φ/2) — tiny — is real.  Sizing a
				// smooth seam by cot(φ/2) is what replaced the curved wall with
				// its tangent planes extended a whole unit: cot(5°) = 11.4, so each
				// strip's slab stood proud of the curve by the sagitta and the two
				// mutually satisfied each other's mitre trim.
				const RefMat &rm = g_m.mats[size_t(rf.matIdx)];
				double c1 = dot(rf.n, nrm(nbrN));
				c1 = std::max(-1.0, std::min(1.0, c1));
				const double phi = std::acos(c1);
				const bool smooth = (phi < g_m.creaseDeg * M_PI / 180.0);
				rf.smoothEdge[i] = smooth;
				if (smooth) ++g_m.nSmooth;
				double margin;
				if (g_m.marginOverride > 0.0) {
					margin = g_m.marginOverride;
				} else if (phi < 1e-4) {
					margin = 2.0 * rf.worldPerTexel;          // coplanar: a token seam
				} else {
					const double half = phi * 0.5;
					const double tanH = std::tan(half);
					const double cotH = 1.0 / std::max(1e-9, tanH);
					const double ratio = 1.0 / std::max(1e-9, std::cos(half));
					const double cap = std::sqrt(std::max(0.0,
					                     g_m.mitreLimit*g_m.mitreLimit - 1.0));
					double k = smooth ? tanH : std::max(cotH, tanH);
					if (k > cap) { k = cap; ++g_m.nBevel; }   // mitre limit → bevel
					margin = rm.dAbs * k + (smooth ? 2.0 * rf.worldPerTexel : 0.0);
				}
				{ const int b = std::min(8, int(phi * 180.0 / M_PI / 10.0)); ++g_m.phiHist[b]; }
				if (margin > g_m.maxExt) { margin = g_m.maxExt; ++g_m.nExtCapped; }
				margin = std::max(margin, 1e-4);
				rf.margin[i] = margin;
				g_m.maxMargin = std::max(g_m.maxMargin, margin);
			}
		}

		// dominant height owner per shared stone edge (Dudash's dominant data).
		// Chosen by a canonical, ORDER-INDEPENDENT key so it does not depend on
		// which face the walk saw first: smaller (material index, centroid key)
		// wins.  With the edge endpoints already ordered by world position,
		// both sides derive the same owner and the same blend parameter.
		for (size_t f = 0; f < g_m.faces.size(); ++f) {
			RefFace &rf = g_m.faces[f];
			for (int i = 0; i < 3; ++i) {
				const int nb = rf.nbrFace[i];
				if (nb < 0) { rf.domOwner[i] = -1; continue; }
				const RefFace &og = g_m.faces[size_t(nb)];
				const PKey ca = pkey((rf.p[0] + rf.p[1] + rf.p[2]) * (1.0/3.0));
				const PKey cb = pkey((og.p[0] + og.p[1] + og.p[2]) * (1.0/3.0));
				bool selfWins = (rf.matIdx != og.matIdx) ? (rf.matIdx < og.matIdx) : (ca < cb);
				rf.domOwner[i] = selfWins ? int(f) : nb;
			}
		}
	}

	// ── extended-triangle corners (offset lines intersected pairwise) ──────
	for (RefFace &rf : g_m.faces) {
		// in-plane basis
		const V3 bx = nrm(rf.p[1] - rf.p[0]);
		const V3 by = nrm(cross(rf.n, bx));
		auto to2 = [&](const V3 &p) { const V3 q = p - rf.p[0]; return std::pair<double,double>(dot(q,bx), dot(q,by)); };
		double lx[3], ly[3], lc[3];
		for (int i = 0; i < 3; ++i) {
			lx[i] = dot(rf.em[i], bx); ly[i] = dot(rf.em[i], by);
			lc[i] = rf.ec[i] - dot(rf.em[i], rf.p[0]) + rf.margin[i];
		}
		for (int k = 0; k < 3; ++k) {
			const int i = (k + 2) % 3, j = k;          // vertex k is on edges k-1 and k
			const double det = lx[i]*ly[j] - lx[j]*ly[i];
			if (std::fabs(det) < 1e-12) { rf.ext[k] = rf.p[k]; continue; }
			const double X = ( lc[i]*ly[j] - lc[j]*ly[i]) / det;
			const double Y = ( lx[i]*lc[j] - lx[j]*lc[i]) / det;
			rf.ext[k] = rf.p[0] + bx*X + by*Y;
		}
		(void)to2;
	}

	g_m.built = true;
	if (verbose) {
		for (const RefMat &rm : g_m.mats)
			std::fprintf(stderr,
				"[REFRENDER-MAT] '%s' mip%d %dx%d  mean=%.4f  h=[%.3f..%.3f]  amp=%.3f  "
				"d=[%+.4f..%+.4f] |d|max=%.4f  matID=%u\n",
				rm.mat->Name, rm.mip, rm.mw, rm.mh, rm.mean, rm.h0, rm.h1, rm.amp,
				rm.dMin, rm.dMax, rm.dAbs, rm.matID);
	}
	std::fprintf(stderr,
		"[REFRENDER] model: %zu authored stone faces (soup %d), edges free/convex/concave "
		"%d/%d/%d (smooth %d), non-stone neighbours %d, bevelled %d, ext capped %d, "
		"max margin %.3f u, back %.3f u, crease %.0f deg, "
		"phi hist(10deg bins) %d/%d/%d/%d/%d/%d/%d/%d/%d, partition %s%s%s\n",
		g_m.faces.size(), g_m.nSoup, g_m.nFree, g_m.nConvex, g_m.nConcave, g_m.nSmooth,
		g_m.nNonStoneNbr, g_m.nBevel, g_m.nExtCapped, g_m.maxMargin, g_m.back, g_m.creaseDeg,
		g_m.phiHist[0], g_m.phiHist[1], g_m.phiHist[2], g_m.phiHist[3], g_m.phiHist[4],
		g_m.phiHist[5], g_m.phiHist[6], g_m.phiHist[7], g_m.phiHist[8],
		g_m.partition ? "bisector(steps)" : "union",
		g_m.sharedEdge ? " +dominant-edge" : "",
		g_m.mitreTrim ? " +mitre-trim" : " NO-mitre-trim");
	return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 5.  Per-frame: world → view, and the screen bins.
// ═══════════════════════════════════════════════════════════════════════════
static void PrepareFrame(const fds::CameraContext &cam)
{
	const Camera *V = cam.view;
	const V3 C(V->ISource);
	g_m.fv.assign(g_m.faces.size(), RefFaceV());
	auto toView = [&](const V3 &p) { return MatVec(V->Mat, p - C); };
	auto rotView = [&](const V3 &d) { return MatVec(V->Mat, d); };

	const double nearZ = std::max(0.05, double(cam.nearZ));
	for (size_t i = 0; i < g_m.faces.size(); ++i) {
		const RefFace &rf = g_m.faces[i];
		RefFaceV &fvv = g_m.fv[i];
		fvv.p0 = toView(rf.p[0]);
		fvv.n  = rotView(rf.n);
		fvv.d  = dot(fvv.n, fvv.p0);
		for (int k = 0; k < 3; ++k) {
			fvv.em[k] = rotView(rf.em[k]);
			fvv.ec[k] = dot(fvv.em[k], toView(rf.p[k]));
			fvv.ext[k] = toView(rf.ext[k]);
			fvv.vnv[k] = rotView(rf.vn[k]);
		}
		fvv.gu = rotView(rf.gu); fvv.gv = rotView(rf.gv);
		fvv.u0 = rf.u[0];        fvv.v0 = rf.v[0];
		fvv.Tuv = rotView(rf.Tu); fvv.Tvv = rotView(rf.Tv);

		// screen bbox of the prism (extended triangle × [−back, dMax]).
		const RefMat &rm = g_m.mats[size_t(rf.matIdx)];
		double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
		bool behind = false, any = false;
		for (int k = 0; k < 3; ++k) for (int s = 0; s < 2; ++s) {
			const V3 q = fvv.ext[k] + fvv.n * (s ? rm.dMax : -g_m.back);
			if (q.z <= nearZ) { behind = true; continue; }
			const double sx = cam.cntrEX + (q.x / q.z) * cam.fovX;
			const double sy = cam.cntrEY - (q.y / q.z) * cam.fovY;
			x0 = std::min(x0, sx); x1 = std::max(x1, sx);
			y0 = std::min(y0, sy); y1 = std::max(y1, sy);
			any = true;
		}
		if (behind || !any) { x0 = 0; y0 = 0; x1 = double(XRes); y1 = double(YRes); any = true; }
		fvv.sxMin = x0; fvv.sxMax = x1; fvv.syMin = y0; fvv.syMax = y1;
		fvv.onScreen = any && x1 >= 0 && y1 >= 0 && x0 <= XRes && y0 <= YRes;
	}

	g_m.tilePx = 32;
	g_m.tilesX = (XRes + g_m.tilePx - 1) / g_m.tilePx;
	g_m.tilesY = (YRes + g_m.tilePx - 1) / g_m.tilePx;
	g_m.bins.assign(size_t(g_m.tilesX) * g_m.tilesY, {});
	for (size_t i = 0; i < g_m.fv.size(); ++i) {
		const RefFaceV &fvv = g_m.fv[i];
		if (!fvv.onScreen) continue;
		const int tx0 = std::max(0, int(std::floor(fvv.sxMin)) / g_m.tilePx);
		const int tx1 = std::min(g_m.tilesX - 1, int(std::floor(fvv.sxMax)) / g_m.tilePx);
		const int ty0 = std::max(0, int(std::floor(fvv.syMin)) / g_m.tilePx);
		const int ty1 = std::min(g_m.tilesY - 1, int(std::floor(fvv.syMax)) / g_m.tilePx);
		for (int ty = ty0; ty <= ty1; ++ty) for (int tx = tx0; tx <= tx1; ++tx)
			g_m.bins[size_t(ty) * g_m.tilesX + tx].push_back(int32_t(i));
	}
	// closure: a candidate's neighbours must be present, because the mitre
	// trim and the bisector both read them.
	for (auto &b : g_m.bins) {
		const size_t n0 = b.size();
		for (size_t k = 0; k < n0; ++k) {
			const RefFace &rf = g_m.faces[size_t(b[k])];
			for (int i = 0; i < 3; ++i) if (rf.nbrFace[i] >= 0) {
				const int32_t nb = int32_t(rf.nbrFace[i]);
				if (std::find(b.begin(), b.end(), nb) == b.end()) b.push_back(nb);
			}
		}
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 6.  The per-ray solver.
// ═══════════════════════════════════════════════════════════════════════════
static constexpr int kMaxCand   = 96;
static constexpr int kMaxEvents = 512;

struct RayCand {
	int    fi;
	double sn, s0;          // sd(t)  = sn·t + s0
	double en[3], e0[3];    // ed_i(t)= en[i]·t + e0[i]
	double un, u0c, vn, v0c;// u(t) = un·t + u0c
	double tLo, tHi;        // extended-polygon interval
};

struct RayCtx {
	V3 D;
	RayCand c[kMaxCand];
	int n = 0;
	int slot[kMaxCand];     // face index → slot lookup is linear over n
	int cellsWalked = 0;
	bool budgetHit = false;
};

static inline int SlotOf(const RayCtx &rc, int fi) {
	for (int i = 0; i < rc.n; ++i) if (rc.c[i].fi == fi) return i;
	return -1;
}

// ── direct per-face ray evaluators ─────────────────────────────────────────
// These read g_m.fv[] and the ray direction, so they work for ANY face —
// candidate or not.  That matters: the mitre trim reads the NEIGHBOUR, and a
// neighbour whose own slab band the ray never enters is exactly the case the
// trim exists for.  Resolving it through the candidate table (which drops such
// faces) skipped the trim precisely when it was load-bearing, and the margin
// boundary then stood up as a phantom lateral face.
static inline double SdOfIdx(const RayCtx &rc, int fi, double t) {
	const RefFaceV &f = g_m.fv[size_t(fi)];
	return dot(f.n, rc.D) * t - f.d;
}
static inline void UvOfIdx(const RayCtx &rc, int fi, double t, double &u, double &v) {
	const RefFaceV &f = g_m.fv[size_t(fi)];
	u = f.u0 + dot(f.gu, rc.D) * t - dot(f.gu, f.p0);
	v = f.v0 + dot(f.gv, rc.D) * t - dot(f.gv, f.p0);
}
static inline double EdOfIdx(const RayCtx &rc, int fi, int e, double t) {
	const RefFaceV &f = g_m.fv[size_t(fi)];
	return dot(f.em[e], rc.D) * t - f.ec[e];
}

// d(u,v) for a face at ray parameter t, including the dominant-edge blend.
static double DOfIdx(const RayCtx &rc, int fi, double t)
{
	const RefFace &rf = g_m.faces[size_t(fi)];
	const RefMat  &rm = g_m.mats[size_t(rf.matIdx)];
	double u, v; UvOfIdx(rc, fi, t, u, v);
	double d = rm.dOfH(rm.sampleH(u, v));
	if (!g_m.sharedEdge || !rf.anyShared) return d;

	// Dominant data: within the band of a shared crease BOTH faces read the
	// OWNER's height, weight 1 exactly on the edge, so the two sides agree
	// there and the castellation step cannot exist (survey §A, §G; Dudash's
	// canonical-owner table).  The owner was chosen from an order-independent
	// key over world-ordered edge endpoints, so both sides pick the same one.
	const double band = std::max(1e-6, g_m.edgeBandTex * rf.worldPerTexel);
	int bestE = -1; double bestW = 0.0;
	for (int i = 0; i < 3; ++i) {
		if (rf.domOwner[i] < 0) continue;
		const double ed = EdOfIdx(rc, fi, i, t);          // <0 inside
		if (ed > 0.0) continue;
		const double w = 1.0 - (-ed) / band;
		if (w > bestW) { bestW = w; bestE = i; }
	}
	if (bestE < 0 || bestW <= 0.0) return d;
	const int own = rf.domOwner[bestE];
	if (own == fi) return d;
	const RefFace &of = g_m.faces[size_t(own)];
	const RefMat  &om = g_m.mats[size_t(of.matIdx)];
	double ou, ov; UvOfIdx(rc, own, t, ou, ov);
	const double od = om.dOfH(om.sampleH(ou, ov));
	const double w = std::min(1.0, std::max(0.0, bestW));
	return d * (1.0 - w) + od * w;
}
static inline double DOfFace(const RayCtx &rc, int slot, double t) {
	return DOfIdx(rc, rc.c[slot].fi, t);
}

// signed distance of the ray point at t from the neighbour plane of edge i,
// against that neighbour's own displaced height (0 for a non-stone neighbour).
static bool MitreTrimOk(const RayCtx &rc, int slot, double t)
{
	if (!g_m.mitreTrim) return true;
	const RayCand &c  = rc.c[slot];
	const RefFace &rf = g_m.faces[size_t(c.fi)];
	for (int i = 0; i < 3; ++i) {
		if (!rf.hasNbr[i]) continue;                   // a free edge never trims
		if (c.en[i] * t + c.e0[i] <= 0.0) continue;    // not in the extension
		double sdN, dN;
		if (rf.nbrFace[i] >= 0) {                      // displaced neighbour
			sdN = SdOfIdx(rc, rf.nbrFace[i], t);
			dN  = DOfIdx(rc, rf.nbrFace[i], t);
		} else {                                       // undisplaced neighbour
			const V3 p = rc.D * t;
			sdN = dot(rf.nbrN[i], p) - rf.nbrD[i];     // nbrN is outward (build time)
			dN  = 0.0;
		}
		if (sdN > dN) return false;                    // past the mitre point
	}
	return true;
}

struct Hit {
	bool   hit = false;
	double t = 0;
	int    fi = -1;
	double u = 0, v = 0;
	bool   stepFace = false;
	bool   skirt = false;
};

// Is the ray point at t inside the solid?  Returns the governing face too.
static bool InsideAt(const RayCtx &rc, double t, int &govSlot)
{
	govSlot = -1;
	if (g_m.partition == 0) {                        // plain union of the slabs
		for (int i = 0; i < rc.n; ++i) {
			const RayCand &c = rc.c[i];
			if (t < c.tLo || t > c.tHi) continue;
			const double sd = c.sn * t + c.s0;
			if (sd < -g_m.back) continue;
			if (sd > DOfFace(rc, i, t)) continue;
			if (!MitreTrimOk(rc, i, t)) continue;
			govSlot = i; return true;
		}
		return false;
	}
	// bisector partition: the nearest BASE PLANE governs
	int best = -1; double bestAbs = 1e300, bestSd = 0;
	for (int i = 0; i < rc.n; ++i) {
		const RayCand &c = rc.c[i];
		if (t < c.tLo || t > c.tHi) continue;
		const double sd = c.sn * t + c.s0;
		const double a = std::fabs(sd);
		if (a < bestAbs) { bestAbs = a; best = i; bestSd = sd; }
	}
	if (best < 0) return false;
	if (bestSd < -g_m.back) return false;
	if (bestSd > DOfFace(rc, best, t)) return false;
	if (!MitreTrimOk(rc, best, t)) return false;
	govSlot = best;
	return true;
}

// Clip [lo,hi] by the half-line  a·t + b <= 0.
static inline void ClipHalf(double a, double b, double &lo, double &hi) {
	if (std::fabs(a) < 1e-15) { if (b > 0.0) { lo = 1.0; hi = -1.0; } return; }
	const double t = -b / a;
	if (a > 0.0) hi = std::min(hi, t); else lo = std::max(lo, t);
}

// Per-texel DDA with an EXACT quadratic root inside each bilinear cell.
// Emits every crossing of  sd(t) = d(u(t),v(t))  in [ta,tb] as an event.
static void MarchCandidate(const RayCtx &rc, int slot, double ta, double tb,
                           double *ev, int &nev, int maxEv, int &cells, bool &budget)
{
	if (tb <= ta) return;
	const RayCand &c  = rc.c[slot];
	const RefFace &rf = g_m.faces[size_t(c.fi)];
	const RefMat  &rm = g_m.mats[size_t(rf.matIdx)];
	// blend mode makes d() a blend of two bilinears — no longer a quadratic —
	// so that arm falls back to a conservative bound + bisection.
	const bool exactQuad = !(g_m.sharedEdge && rf.anyShared);

	// texel-space line
	const double ax = c.un * rm.mw, bx = c.u0c * rm.mw - 0.5;
	const double ay = c.vn * rm.mh, by = c.v0c * rm.mh - 0.5;

	double t = ta;
	int    cx = int(std::floor(ax * t + bx));
	int    cy = int(std::floor(ay * t + by));
	const int stepX = (ax > 0) ? 1 : (ax < 0 ? -1 : 0);
	const int stepY = (ay > 0) ? 1 : (ay < 0 ? -1 : 0);

	auto tOfX = [&](int boundary) { return (double(boundary) - bx) / ax; };
	auto tOfY = [&](int boundary) { return (double(boundary) - by) / ay; };

	int guard = 0;
	while (t < tb) {
		if (++guard > g_m.maxCells) { budget = true; break; }
		++cells;
		double tNextX = 1e300, tNextY = 1e300;
		if (stepX) tNextX = tOfX(stepX > 0 ? cx + 1 : cx);
		if (stepY) tNextY = tOfY(stepY > 0 ? cy + 1 : cy);
		double tEnd = std::min(tb, std::min(tNextX, tNextY));
		if (!(tEnd > t)) tEnd = std::min(tb, t + 1e-9);

		// conservative skip: the bilinear is bounded by its corner max.
		double dCellMax;
		if (exactQuad) {
			dCellMax = rm.dOfH(rm.cellMaxAt(cx, cy));
		} else {
			// blend arm: the owner's cell may be taller; be generous.
			dCellMax = rm.dMax;
		}
		const double sdA = c.sn * t + c.s0, sdB = c.sn * tEnd + c.s0;
		if (std::min(sdA, sdB) > dCellMax) {           // entirely above: no root
			// advance
		} else if (exactQuad) {
			double h00, h10, h01, h11;
			rm.corners(cx, cy, h00, h10, h01, h11);
			const double k = 1.0 / 255.0;
			h00 *= k; h10 *= k; h01 *= k; h11 *= k;
			const double K = h00 - h10 - h01 + h11;
			const double sb = bx - double(cx), rb = by - double(cy);
			const double A = h00 + (h10-h00)*sb + (h01-h00)*rb + K*sb*rb;
			const double B = (h10-h00)*ax + (h01-h00)*ay + K*(sb*ay + rb*ax);
			const double C = K * ax * ay;
			// g(t) = sd(t) − amp·(h(t) − mean)
			const double qa = -rm.amp * C;
			const double qb =  c.sn - rm.amp * B;
			const double qc =  c.s0 - rm.amp * (A - rm.mean);
			double roots[2]; int nr = 0;
			if (std::fabs(qa) < 1e-14) {
				if (std::fabs(qb) > 1e-18) roots[nr++] = -qc / qb;
			} else {
				const double disc = qb*qb - 4.0*qa*qc;
				if (disc >= 0.0) {
					const double sq = std::sqrt(disc);
					const double q = -0.5 * (qb + (qb >= 0 ? sq : -sq));
					roots[nr++] = q / qa;
					if (std::fabs(q) > 1e-300) roots[nr++] = qc / q;
				}
			}
			for (int i = 0; i < nr; ++i)
				if (roots[i] > t && roots[i] <= tEnd && nev < maxEv) ev[nev++] = roots[i];
		} else {
			// blend arm: 16 samples + bisection on each sign change.
			const int NS = 16;
			double pt = t, pg = (c.sn*t + c.s0) - DOfFace(rc, slot, t);
			for (int s = 1; s <= NS; ++s) {
				const double q = t + (tEnd - t) * (double(s) / NS);
				const double gq = (c.sn*q + c.s0) - DOfFace(rc, slot, q);
				if ((pg > 0) != (gq > 0)) {
					double a = pt, b = q;
					for (int it = 0; it < 40; ++it) {
						const double m = 0.5 * (a + b);
						const double gm = (c.sn*m + c.s0) - DOfFace(rc, slot, m);
						if ((pg > 0) == (gm > 0)) a = m; else b = m;
					}
					if (nev < maxEv) ev[nev++] = 0.5 * (a + b);
				}
				pt = q; pg = gq;
			}
		}

		if (tEnd >= tb) break;
		if (tNextX <= tNextY) { cx += stepX; t = tNextX; }
		else                  { cy += stepY; t = tNextY; }
	}
}

// Solve one ray.  D is the view-space direction with D.z == 1, so the ray
// parameter t IS the view-space Z, which is what the depth encoding wants.
static Hit SolveRay(const std::vector<int32_t> &bin, const V3 &D,
                    double tNear, double tFar, int &cellsOut, bool &budgetOut)
{
	Hit out;
	RayCtx rc; rc.D = D;

	// build candidates
	for (int32_t fi : bin) {
		if (rc.n >= kMaxCand) break;
		const RefFaceV &fvv = g_m.fv[size_t(fi)];
		const RefFace  &rf  = g_m.faces[size_t(fi)];
		const RefMat   &rm  = g_m.mats[size_t(rf.matIdx)];
		RayCand c;
		c.fi = fi;
		c.sn = dot(fvv.n, D);
		// BACK-FACE CULL, as the rasterizer does per triangle.  A slab is a
		// closed solid, so its BACK plane (sd = -back) is a surface too, and a
		// face whose outward normal points away from the eye presents nothing
		// but that back plane — 'back' units of phantom material standing in
		// front of whatever the eye should have seen.  Measured at cam A before
		// this rule: one far-side wall face minted 203k of the 296k pixels the
		// reference grew, every one of them a non-slab event.  A face cannot
		// show the eye relief it is facing away from.
		if (c.sn >= 0.0) continue;
		c.s0 = -fvv.d;
		double lo = tNear, hi = tFar;
		for (int i = 0; i < 3; ++i) {
			c.en[i] = dot(fvv.em[i], D);
			c.e0[i] = -fvv.ec[i];
			// ed_i(t) <= margin_i
			ClipHalf(c.en[i], c.e0[i] - rf.margin[i], lo, hi);
		}
		if (hi <= lo) continue;
		// band: −back <= sd(t) <= dMax
		ClipHalf( c.sn,  c.s0 - rm.dMax, lo, hi);
		ClipHalf(-c.sn, -c.s0 - g_m.back, lo, hi);
		if (hi <= lo) continue;
		c.un = dot(fvv.gu, D); c.u0c = fvv.u0 - dot(fvv.gu, fvv.p0);
		c.vn = dot(fvv.gv, D); c.v0c = fvv.v0 - dot(fvv.gv, fvv.p0);
		c.tLo = lo; c.tHi = hi;
		rc.c[rc.n++] = c;
	}
	if (rc.n == 0) return out;

	// ── events: every boundary of the inside() predicate, exactly ──────────
	double ev[kMaxEvents]; int nev = 0;
	double gLo = 1e300, gHi = -1e300;
	for (int i = 0; i < rc.n; ++i) { gLo = std::min(gLo, rc.c[i].tLo); gHi = std::max(gHi, rc.c[i].tHi); }
	if (gHi <= gLo) return out;

	for (int i = 0; i < rc.n; ++i) {
		if (nev + 2 < kMaxEvents) { ev[nev++] = rc.c[i].tLo; ev[nev++] = rc.c[i].tHi; }
		// polygon boundary crossings inside the global range (a face entering
		// or leaving candidacy changes the partition)
		const RefFace &rf = g_m.faces[size_t(rc.c[i].fi)];
		for (int k = 0; k < 3; ++k) {
			if (std::fabs(rc.c[i].en[k]) < 1e-15) continue;
			const double t = (rf.margin[k] - rc.c[i].e0[k]) / rc.c[i].en[k];
			if (t > gLo && t < gHi && nev < kMaxEvents) ev[nev++] = t;
		}
		// band boundaries
		const RefMat &rm = g_m.mats[size_t(rf.matIdx)];
		if (std::fabs(rc.c[i].sn) > 1e-15) {
			const double t1 = (rm.dMax  - rc.c[i].s0) / rc.c[i].sn;
			const double t2 = (-g_m.back - rc.c[i].s0) / rc.c[i].sn;
			if (t1 > gLo && t1 < gHi && nev < kMaxEvents) ev[nev++] = t1;
			if (t2 > gLo && t2 < gHi && nev < kMaxEvents) ev[nev++] = t2;
		}
	}
	// bisector crossings (the castellation STEP lives on exactly these)
	if (g_m.partition == 1) {
		for (int i = 0; i < rc.n && nev + 2 < kMaxEvents; ++i)
			for (int j = i + 1; j < rc.n && nev + 2 < kMaxEvents; ++j) {
				const double an = rc.c[i].sn - rc.c[j].sn, ab = rc.c[i].s0 - rc.c[j].s0;
				if (std::fabs(an) > 1e-15) {
					const double t = -ab / an;
					if (t > gLo && t < gHi) ev[nev++] = t;
				}
				const double bn = rc.c[i].sn + rc.c[j].sn, bb = rc.c[i].s0 + rc.c[j].s0;
				if (std::fabs(bn) > 1e-15) {
					const double t = -bb / bn;
					if (t > gLo && t < gHi) ev[nev++] = t;
				}
			}
	}
	// slab crossings, per candidate, by conservative per-texel DDA
	int cells = 0; bool budget = false;
	for (int i = 0; i < rc.n; ++i)
		MarchCandidate(rc, i, rc.c[i].tLo, rc.c[i].tHi, ev, nev, kMaxEvents, cells, budget);
	cellsOut += cells; budgetOut = budgetOut || budget;

	if (nev < 2) return out;
	std::sort(ev, ev + nev);

	// ── walk the intervals; the first one whose interior is inside the solid
	//     starts at the surface.  The march starts OUTSIDE, which is what makes
	//     min-over-slabs exact (survey §G).
	int gov = -1;
	for (int i = 0; i + 1 < nev; ++i) {
		const double a = ev[i], b = ev[i+1];
		if (b <= a || b <= gLo) continue;
		if (a >= gHi) break;
		const double mid = 0.5 * (a + b);
		if (mid <= tNear) continue;
		if (InsideAt(rc, mid, gov)) {
			out.hit = true;
			out.t   = std::max(a, tNear);
			out.fi  = rc.c[gov].fi;
			out.u   = rc.c[gov].un * out.t + rc.c[gov].u0c;
			out.v   = rc.c[gov].vn * out.t + rc.c[gov].v0c;
			// classify the surface we landed on: if at the hit the governing
			// face's own slab boundary is NOT what we crossed, it is a
			// bisector step or a skirt.
			const double sd = rc.c[gov].sn * out.t + rc.c[gov].s0;
			const double dd = DOfFace(rc, gov, out.t);
			if (std::fabs(sd - dd) > 1e-5) {
				const RefFace &rf = g_m.faces[size_t(out.fi)];
				bool nearFree = false;
				for (int k = 0; k < 3; ++k) {
					if (rf.kind[k] != EDGE_FREE) continue;
					if (std::fabs(rc.c[gov].en[k] * out.t + rc.c[gov].e0[k]) < 1e-4) nearFree = true;
				}
				if (nearFree) out.skirt = true; else out.stepFace = true;
			}
			return out;
		}
	}
	return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// 7.  G-buffer write.
// ═══════════════════════════════════════════════════════════════════════════
static inline meka::u32 OctEncodeU32(double nx, double ny, double nz)
{
	// Scalar mirror of meka::oct_encode_u32_x8 (16.16 octahedral).
	const double s = std::fabs(nx) + std::fabs(ny) + std::fabs(nz);
	const double inv = (s > 1e-30) ? 1.0 / s : 0.0;
	double ox = nx * inv, oy = ny * inv;
	if (nz < 0.0) {
		const double fx = (1.0 - std::fabs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
		const double fy = (1.0 - std::fabs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
		ox = fx; oy = fy;
	}
	int qx = int(std::lrint(ox * 32767.0)), qy = int(std::lrint(oy * 32767.0));
	qx = std::max(-32768, std::min(32767, qx));
	qy = std::max(-32768, std::min(32767, qy));
	return meka::u32(qx & 0xFFFF) | (meka::u32(qy & 0xFFFF) << 16);
}
static inline meka::u16 OctEncodeU16(double nx, double ny, double nz)
{
	const double s = std::fabs(nx) + std::fabs(ny) + std::fabs(nz);
	const double inv = (s > 1e-30) ? 1.0 / s : 0.0;
	double ox = nx * inv, oy = ny * inv;
	if (nz < 0.0) {
		const double fx = (1.0 - std::fabs(oy)) * (ox >= 0.0 ? 1.0 : -1.0);
		const double fy = (1.0 - std::fabs(ox)) * (oy >= 0.0 ? 1.0 : -1.0);
		ox = fx; oy = fy;
	}
	int qx = int(std::lrint(ox * 127.0)), qy = int(std::lrint(oy * 127.0));
	qx = std::max(-128, std::min(127, qx));
	qy = std::max(-128, std::min(127, qy));
	return meka::u16((qx & 0xFF) | ((qy & 0xFF) << 8));
}

// The rasterizer's swizzled texel address (SimdHelpers.h, scalar form).
static inline uint32_t TileU(uint32_t u, int32_t vbits, uint32_t swzUmask) {
	return (u & 3u) | ((u << vbits) & swzUmask);
}
static inline uint32_t TileV(uint32_t v, uint32_t vmask) { return (v & vmask) << 2; }
static inline uint32_t SwizzleUmask(int32_t vbits, uint32_t umask) { return (umask >> 2) << (2 + vbits); }

static inline uint32_t BilinearBGRA(const uint32_t *tex, double fu, double fv,
                                    int LogW, int LogH)
{
	const int32_t vmask = (1 << LogH) - 1;
	const uint32_t umaskS = SwizzleUmask(LogH, uint32_t((1 << LogW) - 1));
	const double x = fu - 0.5, y = fv - 0.5;
	const double fx = std::floor(x), fy = std::floor(y);
	const int wu = int((x - fx) * 255.0), wv = int((y - fy) * 255.0);
	const int u0 = int(fx), v0 = int(fy);
	uint32_t s[4];
	s[0] = tex[TileU(uint32_t(u0),   LogH, umaskS) + TileV(uint32_t(v0),   uint32_t(vmask))];
	s[1] = tex[TileU(uint32_t(u0+1), LogH, umaskS) + TileV(uint32_t(v0),   uint32_t(vmask))];
	s[2] = tex[TileU(uint32_t(u0),   LogH, umaskS) + TileV(uint32_t(v0+1), uint32_t(vmask))];
	s[3] = tex[TileU(uint32_t(u0+1), LogH, umaskS) + TileV(uint32_t(v0+1), uint32_t(vmask))];
	uint32_t out = 0;
	for (int ch = 0; ch < 4; ++ch) {
		const int c00 = (s[0] >> (ch*8)) & 0xFF, c10 = (s[1] >> (ch*8)) & 0xFF;
		const int c01 = (s[2] >> (ch*8)) & 0xFF, c11 = (s[3] >> (ch*8)) & 0xFF;
		const int top = (c00 * (255 - wu) + c10 * wu) >> 8;
		const int bot = (c01 * (255 - wu) + c11 * wu) >> 8;
		const int val = (top * (255 - wv) + bot * wv) >> 8;
		out |= uint32_t(val & 0xFF) << (ch*8);
	}
	return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// 8.  The pass.
// ═══════════════════════════════════════════════════════════════════════════
struct RefStats {
	std::atomic<long long> pxCast{0}, pxHit{0}, pxFallback{0}, pxGrow{0};
	std::atomic<long long> pxStep{0}, pxSkirt{0}, pxBudget{0}, cells{0};
};

void Render_GreetsDisplaceRef(Scene *Sc)
{
	using FF = fds::FeatureFlags;
	if (!FF::greets_displace_ref()) return;
	if (!Sc || !g_gbuffer || !ZPage16) return;
	if (int(g_gbuffer->normal.size()) < XRes * YRes) return;

	const auto t0 = std::chrono::steady_clock::now();
	if (!g_m.built || g_m.scene != Sc) {
		if (!BuildModel(Sc)) return;
	}
	if (g_m.faces.empty() || g_m.mats.empty()) return;

	const fds::CameraContext &cam = fds::g_mainCamera;
	if (!cam.view) return;
	PrepareFrame(cam);

	const int verbose = FF::greets_displace_ref_stats();
	const double invFovX = 1.0 / double(cam.fovX), invFovY = 1.0 / double(cam.fovY);
	const double tNear = std::max(0.05, double(cam.nearZ));
	const double tFar  = double(cam.farZ) > 0 ? double(cam.farZ) : 4000.0;
	const double zScale = double(cam.zScale);

	// stone matIDs, for "was this pixel already stone?"
	bool isStoneMat[256] = { false };
	for (const RefMat &rm : g_m.mats) if (rm.matID < 256) isStoneMat[rm.matID] = true;

	const bool wantAlbedo = !g_gbuffer->albedo.empty();
	const bool wantTangent = !g_gbuffer->tangent.empty();
	const bool wantShadowId = !g_gbuffer->shadowMatID.empty();

	// optional raw dump (FDS_REFRENDER_DUMP=path) — matches the FDS_SNAPSHOT_*
	// DUMP convention, which is path-valued env by design (FeatureFlags carries
	// no string type).
	const char *dumpPath = std::getenv("FDS_REFRENDER_DUMP");
	std::vector<float>    dz;
	std::vector<float>    dn;
	std::vector<int32_t>  df;
	std::vector<uint32_t> dfl;
	if (dumpPath && *dumpPath) {
		dz.assign(size_t(XRes) * YRes, 0.0f);
		dn.assign(size_t(XRes) * YRes * 3, 0.0f);
		df.assign(size_t(XRes) * YRes, -1);
		dfl.assign(size_t(XRes) * YRes, 0u);
	}

	RefStats st;

	constexpr int NT = 12;
	const int tsx = (XRes + NT - 1) / NT, tsy = (YRes + NT - 1) / NT;
	dispatchIndexed(NT * NT, nullptr, [&](int tid) {
		const int tj = tid / NT, ti = tid - tj * NT;
		const int y1 = tsy * tj, y2 = std::min(y1 + tsy, int(YRes));
		const int x1 = tsx * ti, x2 = std::min(x1 + tsx, int(XRes));
		long long lCast = 0, lHit = 0, lFall = 0, lGrow = 0, lStep = 0, lSkirt = 0, lBudget = 0, lCells = 0;
		for (int py = y1; py < y2; ++py) {
			const int by = py / g_m.tilePx;
			for (int px = x1; px < x2; ++px) {
				const int bx = px / g_m.tilePx;
				const std::vector<int32_t> &bin = g_m.bins[size_t(by) * g_m.tilesX + bx];
				if (bin.empty()) continue;
				const size_t idx = size_t(py) * XRes + px;
				const meka::u16 zOld = ZPage16[idx];
				const uint32_t txOld = g_gbuffer->txtr[idx];
				const uint32_t matOld = (txOld >> 20) & 0xFF;
				const bool wasStone = (zOld != 0) && isStoneMat[matOld];

				const V3 D((double(px) + 0.5 - double(cam.cntrEX)) * invFovX,
				           (double(cam.cntrEY) - (double(py) + 0.5)) * invFovY, 1.0);
				double tMax = tFar;
				if (zOld != 0 && !wasStone) tMax = double(0xFF80 - int(zOld)) / zScale;   // opaque blocker

				++lCast;
				int cells = 0; bool budget = false;
				Hit h = SolveRay(bin, D, tNear, tFar, cells, budget);
				lCells += cells; if (budget) ++lBudget;

				if (!h.hit) {
					if (wasStone) { ++lFall; if (!dfl.empty()) dfl[idx] |= 2u; }
					continue;
				}
				if (!wasStone && h.t >= tMax) continue;      // hidden behind real geometry
				if (!wasStone) ++lGrow;
				++lHit;
				if (h.stepFace) ++lStep;
				if (h.skirt) ++lSkirt;

				const RefFace  &rf = g_m.faces[size_t(h.fi)];
				const RefFaceV &fvv = g_m.fv[size_t(h.fi)];
				const RefMat   &rm = g_m.mats[size_t(rf.matIdx)];

				// ── shading normal: n ∝ N − ∇ₛh, exact on a plane (survey §C)
				// base N: face normal at a crease, interpolated below it.
				V3 Nb = fvv.n;
				{
					// barycentric of the hit in the face's UV chart
					const double du1 = rf.u[1]-rf.u[0], dv1 = rf.v[1]-rf.v[0];
					const double du2 = rf.u[2]-rf.u[0], dv2 = rf.v[2]-rf.v[0];
					const double det = du1*dv2 - du2*dv1;
					if (std::fabs(det) > 1e-14) {
						const double pu = h.u - rf.u[0], pv = h.v - rf.v[0];
						double b1 = ( pu*dv2 - du2*pv) / det;
						double b2 = ( du1*pv - pu*dv1) / det;
						double b0 = 1.0 - b1 - b2;
						b0 = std::max(0.0, std::min(1.0, b0));
						b1 = std::max(0.0, std::min(1.0, b1));
						b2 = std::max(0.0, std::min(1.0, b2));
						const double s = b0 + b1 + b2;
						if (s > 1e-9) {
							const V3 acc = fvv.vnv[0]*(b0/s) + fvv.vnv[1]*(b1/s) + fvv.vnv[2]*(b2/s);
							if (len(acc) > 1e-9) Nb = nrm(acc);
						}
					}
				}
				double hu = 0, hv = 0;
				rm.gradH(h.u, h.v, hu, hv);
				// ∇ₛh in world-length units: (amp·h_u)/|Tu| along T̂u etc.
				const double lu = len(rf.Tu), lv = len(rf.Tv);
				const V3 Tuh = (lu > 1e-12) ? fvv.Tuv * (1.0/lu) : V3(1,0,0);
				const V3 Tvh = (lv > 1e-12) ? fvv.Tvv * (1.0/lv) : V3(0,1,0);
				const double gU = (lu > 1e-12) ? rm.amp * hu / lu : 0.0;
				const double gV = (lv > 1e-12) ? rm.amp * hv / lv : 0.0;
				V3 nShade = nrm(Nb - Tuh * gU - Tvh * gV);
				if (dot(nShade, fvv.n) < 0.0) nShade = nShade * -1.0;

				// ── depth
				int zEnc = 0xFF80 - int(std::lrint(zScale * h.t));
				if (zEnc < 1) zEnc = 1; if (zEnc > 0xFFFF) zEnc = 0xFFFF;
				ZPage16[idx] = meka::u16(zEnc);

				// ── normal
				g_gbuffer->normal[idx] = OctEncodeU32(nShade.x, nShade.y, nShade.z);

				// ── txtr (mip | matID | swizzled UV) + albedo
				const Material *M = rm.mat;
				const Texture  *TX = M->Txtr;
				if (TX && TX->numMipmaps) {
					int mip = wasStone ? int((txOld >> 28) & 0xF) : int(rf.lastMip);
					if (mip >= int(TX->numMipmaps)) mip = int(TX->numMipmaps) - 1;
					if (mip < 0) mip = 0;
					const int LogW = TX->LSizeX - mip, LogH = TX->LSizeY - mip;
					if (LogW >= 2 && LogH >= 2) {
						const double fu = h.u * double(1 << LogW);
						const double fv = h.v * double(1 << LogH);
						const uint32_t uu = uint32_t(int32_t(std::lrint(fu)));
						const uint32_t vv = uint32_t(int32_t(std::lrint(fv)));
						const uint32_t umaskS = SwizzleUmask(LogH, uint32_t((1 << LogW) - 1));
						const uint32_t suv = TileU(uu, LogH, umaskS)
						                   + TileV(vv, uint32_t((1 << LogH) - 1));
						g_gbuffer->txtr[idx] = (uint32_t(mip) << 28)
						                     | (uint32_t(rm.matID & 0xFF) << 20)
						                     | (suv & 0xFFFFFu);
						if (wantAlbedo && TX->Mipmap[mip])
							g_gbuffer->albedo[idx] = BilinearBGRA(
								reinterpret_cast<const uint32_t*>(TX->Mipmap[mip]), fu, fv, LogW, LogH);
					}
				}
				if (wantTangent) {
					V3 t = Tuh - nShade * dot(nShade, Tuh);
					if (len(t) > 1e-9) t = nrm(t);
					g_gbuffer->tangent[idx] = OctEncodeU16(t.x, t.y, t.z);
				}
				if (wantShadowId) g_gbuffer->shadowMatID[idx] = rf.shadowMatID;
				if (!g_gbuffer->mirrorId.empty() && !wasStone) g_gbuffer->mirrorId[idx] = 0;

				if (!dz.empty()) {
					dz[idx] = float(h.t);
					dn[idx*3+0] = float(nShade.x); dn[idx*3+1] = float(nShade.y); dn[idx*3+2] = float(nShade.z);
					df[idx] = int32_t(h.fi);
					uint32_t fl = 1u;
					if (h.stepFace) fl |= 4u;
					if (h.skirt)    fl |= 8u;
					if (budget)     fl |= 16u;
					if (!wasStone)  fl |= 32u;
					dfl[idx] = fl;
				}
			}
		}
		st.pxCast += lCast; st.pxHit += lHit; st.pxFallback += lFall; st.pxGrow += lGrow;
		st.pxStep += lStep; st.pxSkirt += lSkirt; st.pxBudget += lBudget; st.cells += lCells;
		renderns::tileDone.release();
	});
	for (int k = 0; k < NT * NT; ++k) renderns::tileDone.acquire();

	if (!dz.empty()) {
		if (FILE *f = std::fopen(dumpPath, "wb")) {
			const char magic[8] = { 'R','E','F','R','N','D','0','1' };
			int32_t wh[2] = { int32_t(XRes), int32_t(YRes) };
			std::fwrite(magic, 1, 8, f);
			std::fwrite(wh, sizeof(int32_t), 2, f);
			std::fwrite(dz.data(),  sizeof(float),    dz.size(),  f);
			std::fwrite(dn.data(),  sizeof(float),    dn.size(),  f);
			std::fwrite(df.data(),  sizeof(int32_t),  df.size(),  f);
			std::fwrite(dfl.data(), sizeof(uint32_t), dfl.size(), f);
			std::fclose(f);
			std::fprintf(stderr, "[REFRENDER] dump -> %s (%dx%d, z/n/faceid/flags)\n",
			             dumpPath, int(XRes), int(YRes));
		} else {
			std::fprintf(stderr, "[REFRENDER] could not open FDS_REFRENDER_DUMP='%s'\n", dumpPath);
		}
	}

	const double ms = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
	std::fprintf(stderr,
		"[REFRENDER] %.0f ms  cast %lld  hit %lld (grow %lld, step %lld, skirt %lld)  "
		"fallback %lld  budget-hit %lld  cells %lld (%.1f/px)\n",
		ms, (long long)st.pxCast, (long long)st.pxHit, (long long)st.pxGrow,
		(long long)st.pxStep, (long long)st.pxSkirt, (long long)st.pxFallback,
		(long long)st.pxBudget, (long long)st.cells,
		st.pxCast ? double(st.cells) / double(st.pxCast) : 0.0);
	(void)verbose;
}

}  // namespace refrender

void Render_GreetsDisplaceRef(struct Scene *Sc) { refrender::Render_GreetsDisplaceRef(Sc); }

}  // namespace fds
