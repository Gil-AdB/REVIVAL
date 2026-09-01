// ═══════════════════════════════════════════════════════════════════════════
// v4 stone-displacement bake — PHASE 1: stage (a) stitch + half-edge and
// stage (b) chart registry.  CENSUS ONLY — see DEMO/V4Bake.h for the contract.
//
// Everything here is read-only on the scene.  The only outputs are the two
// stderr blocks.  Nothing allocates into the scene, nothing is cached across
// calls, and the entry point is not called at all unless BOTH
// --greets_displace_v4 and --v4_census are on.
//
// The design's citations that this file implements, and where each lands:
//
//   §2a exact-equality stitch          → Stitch(), kWeldStage{Exact,Ulp,Eps}
//   §2a half-edge, boundary = null FACE→ HalfEdge below: every half-edge has a
//                                        twin; a boundary twin's `face` is
//                                        kNullFace (-1).  There is no null twin
//                                        anywhere in the structure, which is
//                                        what makes a boundary walk terminate
//                                        (survey §E, Stanford CS268 notes).
//   §2a material/coplanarity = ATTRS   → EdgeAttr, never consulted by the
//                                        use-count classifier (the bc79e39d
//                                        class of bug cannot be written here).
//   §2a world-position endpoint order  → the vertex ids ARE the world-position
//                                        sort order (Stitch reindexes), so
//                                        (min(id),max(id)) IS DiagSplit's
//                                        canonical world order.  §REF five
//                                        things #5.
//   §2b VSA-ish region growing         → Charts(); see the comment there for
//                                        exactly which acceptance test is used
//                                        and why (it is NOT Lloyd-iterated).
//   §2b two thresholds two questions   → kChartBudget (parameterisation) is a
//                                        flag; kCreaseDeg (shading) is 30° and
//                                        only ever labels an edge.
// ═══════════════════════════════════════════════════════════════════════════

#include "V4Bake.h"
#include "MeshOps.h"

extern void Compute_FaceVertexIndices(TriMesh *T);   // FDS/MISC/PREPROC.CPP

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Scene.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fds {
namespace v4 {
namespace {

// ── small double vector ────────────────────────────────────────────────────
struct V3 {
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	explicit V3(const Vector &v) : x(v.x), y(v.y), z(v.z) {}
};
static inline V3 operator-(const V3 &a, const V3 &b) { return V3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline V3 operator+(const V3 &a, const V3 &b) { return V3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline V3 operator*(const V3 &a, double s)    { return V3(a.x*s, a.y*s, a.z*s); }
static inline double dot(const V3 &a, const V3 &b)   { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3 cross(const V3 &a, const V3 &b) {
	return V3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline double len(const V3 &a) { return std::sqrt(dot(a, a)); }
static inline V3 nrm(const V3 &a) { const double l = len(a); return l > 0 ? a * (1.0/l) : a; }
static inline double angDeg(const V3 &a, const V3 &b) {
	double c = dot(a, b);
	if (c >  1.0) c =  1.0;
	if (c < -1.0) c = -1.0;
	return std::acos(c) * (180.0 / 3.14159265358979323846);
}

// ── thresholds ─────────────────────────────────────────────────────────────
// The crease threshold answers "same tangent plane?" (SHADING).  It is the
// artist convention 30° the whole project already uses (MakeFacesIndependent-
// ByAngle(GreetSc, 30.0f) two lines after the bake, and the reference's
// --greets_displace_ref_crease default).  It is a different question from the
// chart budget and is never used as one — survey §F/7, design §2b.
static constexpr double kCreaseDeg = 30.0;
// "Coplanar" for the edge ATTRIBUTE.  Two triangles of one authored quad agree
// to the bit, so this is a tightness test and not a tolerance dial.
static constexpr double kCoplanarDeg = 0.05;
// ε-weld fallback: ε = kEpsRelShortestEdge · (shortest authored edge).  Never a
// world-unit constant — the old bake's 0.006/0.02 world welds and their
// chaining are refuted (0c2cd2a858e6, 20be1fac463e).  Survey §E.
static constexpr double kEpsRelShortestEdge = 1.0e-3;

static constexpr int32_t kNullFace = -1;   // a boundary half-edge's FACE

// ── gathered face ──────────────────────────────────────────────────────────
struct SFace {
	const TriMesh *mesh   = nullptr;
	int32_t        meshIdx = 0;
	int32_t        faceIdx = 0;
	int32_t        matIdx  = 0;      // index into the material-name table
	V3             p[3];
	int32_t        v[3] = {0,0,0};   // stitched vertex ids
	V3             n;                // geometric normal, oriented by the engine's F.N
	double         area  = 0.0;
	double         plD   = 0.0;      // n·p0
	int32_t        chart = -1;
};

// ── half-edge ──────────────────────────────────────────────────────────────
// h in [0, 3*nFace) is face h/3's corner h%3, running v[h%3] -> v[(h%3+1)%3].
// h in [3*nFace, ...) is a BOUNDARY half-edge: face == kNullFace.  Every
// half-edge has a twin; no twin is ever null.
struct HalfEdge {
	int32_t from = 0, to = 0;
	int32_t face = kNullFace;
	int32_t twin = -1;
	int32_t edge = -1;
};

struct EdgeAttr {
	int32_t a = 0, b = 0;        // world-position-ordered endpoints (a < b as ids)
	int32_t use = 0;             // how many FACE half-edges carry this edge
	int32_t f0 = -1, f1 = -1;
	double  length  = 0.0;
	double  phiDeg  = 0.0;       // dihedral, 0 when not exactly 2 faces
	bool    matSeam = false;     // ATTRIBUTE, never topology
	bool    coplanar = false;
	bool    crease  = false;     // phi >= kCreaseDeg
	int8_t  convex  = 0;         // +1 convex, -1 concave, 0 undecided/smooth
	bool    orientOk = false;    // the two half-edges run in opposite directions
};

// ── union-find ─────────────────────────────────────────────────────────────
struct DSU {
	std::vector<int32_t> p;
	void init(size_t n) { p.resize(n); for (size_t i = 0; i < n; ++i) p[i] = int32_t(i); }
	int32_t find(int32_t a) { while (p[size_t(a)] != a) { p[size_t(a)] = p[size_t(p[size_t(a)])]; a = p[size_t(a)]; } return a; }
	bool join(int32_t a, int32_t b) { a = find(a); b = find(b); if (a == b) return false; if (a < b) p[size_t(b)] = a; else p[size_t(a)] = b; return true; }
};

// ── position key: EXACT equality, with -0.0 normalised to +0.0 ─────────────
// Bitwise on the authored float32s.  Survey §E's "practical recommendation for
// authored geometry: exact equality (bitwise or single-ULP hash) as the primary
// stitch".  -0.0 == +0.0 by value, so folding it here is exactness, not
// tolerance.
static inline uint32_t bitsOf(float f) {
	if (f == 0.0f) f = 0.0f;                       // -0.0 -> +0.0
	uint32_t u; std::memcpy(&u, &f, 4); return u;
}
struct PKey {
	uint32_t x, y, z;
	bool operator==(const PKey &o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PKeyH {
	size_t operator()(const PKey &k) const {
		uint64_t h = 1469598103934665603ull;
		for (uint32_t v : { k.x, k.y, k.z }) { h ^= v; h *= 1099511628211ull; }
		return size_t(h);
	}
};

static inline bool within1Ulp(float a, float b) {
	if (a == b) return true;
	if (std::signbit(a) != std::signbit(b)) return false;   // straddling zero: not 1 ULP
	int32_t ia, ib; std::memcpy(&ia, &a, 4); std::memcpy(&ib, &b, 4);
	return std::abs(ia - ib) <= 1;
}

// ── the FULL scene soup ────────────────────────────────────────────────────
// Every face of every mesh, every material.  Stage (a) classifies a stone edge
// that only ONE stone face uses against THIS, never against the stone set
// alone: an edge whose far side carries a siling / lintel / doorway face is a
// MATERIAL SEAM, not a boundary, and calling it free is exactly the bug the
// design forbids (bc79e39d / 10994f6ef014 — a material-blind misclassification
// freed the floor base and tore it).  The rule and its tolerance are the
// reference renderer's own (DeferredDisplaceRef.cpp, --greets_displace_ref_free_tol):
// a stone face sharing the edge by position, else ANY soup face within
// kFreeTol of three interior points of the edge.
static constexpr double kFreeTol = 0.05;   // world units — the reference's default

struct SoupTri {
	V3 p[3];
	const char *mat = nullptr;
	int32_t meshIdx = 0;
};

struct SoupGrid {
	double cell = 0.4;
	V3 lo, hi;
	int nx = 1, ny = 1, nz = 1;
	std::vector<std::vector<int32_t>> b;
	inline int idx(int x, int y, int z) const { return (z * ny + y) * nx + x; }
	inline int cx(double x) const { return std::max(0, std::min(nx-1, int((x - lo.x) / cell))); }
	inline int cy(double y) const { return std::max(0, std::min(ny-1, int((y - lo.y) / cell))); }
	inline int cz(double z) const { return std::max(0, std::min(nz-1, int((z - lo.z) / cell))); }
	void build(const std::vector<SoupTri> &s, double c) {
		cell = c;
		lo = V3(1e30, 1e30, 1e30); hi = V3(-1e30, -1e30, -1e30);
		for (const SoupTri &f : s) for (int k = 0; k < 3; ++k) {
			lo.x = std::min(lo.x, f.p[k].x); hi.x = std::max(hi.x, f.p[k].x);
			lo.y = std::min(lo.y, f.p[k].y); hi.y = std::max(hi.y, f.p[k].y);
			lo.z = std::min(lo.z, f.p[k].z); hi.z = std::max(hi.z, f.p[k].z);
		}
		if (s.empty()) { lo = V3(0,0,0); hi = V3(1,1,1); }
		nx = std::max(1, std::min(256, int((hi.x - lo.x) / cell) + 1));
		ny = std::max(1, std::min(256, int((hi.y - lo.y) / cell) + 1));
		nz = std::max(1, std::min(256, int((hi.z - lo.z) / cell) + 1));
		b.assign(size_t(nx) * size_t(ny) * size_t(nz), {});
		for (size_t i = 0; i < s.size(); ++i) {
			V3 a = s[i].p[0], c2 = s[i].p[0];
			for (int k = 1; k < 3; ++k) {
				a.x = std::min(a.x, s[i].p[k].x); c2.x = std::max(c2.x, s[i].p[k].x);
				a.y = std::min(a.y, s[i].p[k].y); c2.y = std::max(c2.y, s[i].p[k].y);
				a.z = std::min(a.z, s[i].p[k].z); c2.z = std::max(c2.z, s[i].p[k].z);
			}
			for (int z = cz(a.z); z <= cz(c2.z); ++z)
			for (int y = cy(a.y); y <= cy(c2.y); ++y)
			for (int x = cx(a.x); x <= cx(c2.x); ++x)
				b[size_t(idx(x, y, z))].push_back(int32_t(i));
		}
	}
};

// Ericson's closest point on a triangle, squared distance.
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
		const double t = (d4-d3)/((d4-d3)+(d5-d6));
		const V3 q = b + (c - b)*t - p; return dot(q,q);
	}
	const double den = 1.0/(va+vb+vc), v = vb*den, w = vc*den;
	const V3 q = a + ab*v + ac*w - p; return dot(q,q);
}

// ── material-name table ────────────────────────────────────────────────────
struct MatTab {
	std::vector<std::string> names;
	int32_t idOf(const char *nm) {
		for (size_t i = 0; i < names.size(); ++i) if (names[i] == nm) return int32_t(i);
		names.push_back(nm); return int32_t(names.size()) - 1;
	}
};

// ── mesh placement, READ ONLY ──────────────────────────────────────────────
// At the bake point Animate_Objects has NOT run for this scene: TriMesh::RotMat
// is still {0} and IPos {0,0,0}, so the reference renderer's WorldPos() (which
// reads them) cannot be reused here.  Calling Spline_Calc_* would mutate
// Spline::CurKey, so this reads the KEYS directly instead and reports what it
// finds.  A single-key Pos/Scale/Rotate spline IS the constant placement
// (Spline_Calc_3D returns Keys[0] verbatim for NumKeys == 1, MATH.CPP:1495).
struct Placement {
	uint32_t posKeys = 0, sclKeys = 0, rotKeys = 0;
	Vector   pos{0,0,0}, scl{1,1,1};
	Quaternion rot;
	bool     euler = false;
	bool     hasParentObj = false;
	bool     identity = false;
	bool     animated = false;
};
static Placement PlacementOf(const Scene *Sc, const TriMesh *T) {
	Placement pl;
	pl.posKeys = T->Pos.NumKeys; pl.sclKeys = T->Scale.NumKeys; pl.rotKeys = T->Rotate.NumKeys;
	if (T->Pos.NumKeys   >= 1 && T->Pos.Keys)   { pl.pos.x = T->Pos.Keys[0].Pos.x;   pl.pos.y = T->Pos.Keys[0].Pos.y;   pl.pos.z = T->Pos.Keys[0].Pos.z; }
	if (T->Scale.NumKeys >= 1 && T->Scale.Keys) { pl.scl.x = T->Scale.Keys[0].Pos.x; pl.scl.y = T->Scale.Keys[0].Pos.y; pl.scl.z = T->Scale.Keys[0].Pos.z; }
	if (T->Rotate.NumKeys>= 1 && T->Rotate.Keys){ pl.rot   = T->Rotate.Keys[0].Pos; }
	pl.euler    = (T->Flags & Tri_Euler) != 0;
	pl.animated = (pl.posKeys > 1) || (pl.sclKeys > 1) || (pl.rotKeys > 1);
	for (const Object *O = Sc ? Sc->ObjectHead : nullptr; O; O = O->Next)
		if (O->Type == Obj_TriMesh && O->Data == (const void *)T) { pl.hasParentObj = (O->Parent != nullptr); break; }
	const bool tPos = (pl.pos.x == 0.0f && pl.pos.y == 0.0f && pl.pos.z == 0.0f);
	const bool tScl = (pl.sclKeys == 0) || (pl.scl.x == 1.0f && pl.scl.y == 1.0f && pl.scl.z == 1.0f);
	const bool tRot = pl.euler ? (pl.rot.x == 0.0f && pl.rot.y == 0.0f && pl.rot.z == 0.0f)
	                           : (pl.rot.x == 0.0f && pl.rot.y == 0.0f && pl.rot.z == 0.0f &&
	                              std::fabs(std::fabs(float(pl.rot.W)) - 1.0f) < 1e-6f);
	pl.identity = tPos && tScl && tRot && !pl.animated && !pl.hasParentObj;
	return pl;
}

// ── the old bake's own per-mesh guard, replicated ──────────────────────────
// DisplaceStoneSubdiv (DEMO/MeshOps.cpp:2347-2390) skips a mesh with no target
// face, one whose target material has no 8-bit HeightMap, and one whose height
// mip is (near-)constant.  "Exactly the face set the old bake targets" means
// this guard, so it is reproduced here rather than assumed away.
static bool OldBakeWouldBake(const Material *mat) {
	if (!mat) return false;
	const Texture *hm = mat->HeightMap;
	if (!hm || hm->BPP != 8 || !hm->Mipmap[0]) return false;
	int useMip = FeatureFlags::greets_displace_mip();
	if (useMip >= int(hm->numMipmaps)) useMip = int(hm->numMipmaps) - 1;
	if (useMip < 0) useMip = 0;
	while (useMip > 0 &&
	       ((std::max(1, int(hm->SizeX) >> useMip) < (1 << hm->blockSizeX)) ||
	        (std::max(1, int(hm->SizeY) >> useMip) < (1 << hm->blockSizeY))))
		--useMip;
	const byte *d = hm->Mipmap[useMip];
	if (!d) return false;
	const int mw = std::max(1, int(hm->SizeX) >> useMip);
	const int mh = std::max(1, int(hm->SizeY) >> useMip);
	byte lo = 255, hi = 0;
	const size_t n = size_t(mw) * size_t(mh);
	for (size_t i = 0; i < n; ++i) { const byte b = d[i]; if (b < lo) lo = b; if (b > hi) hi = b; }
	return (hi - lo) >= 2;
}

// ═══════════════════════════════════════════════════════════════════════════
//  THE PASS
// ═══════════════════════════════════════════════════════════════════════════
struct Pass {
	MatTab              mats;
	std::vector<SFace>  faces;
	std::vector<V3>     vpos;          // stitched, world-position ordered
	std::vector<HalfEdge> he;
	std::vector<EdgeAttr> edges;
	std::vector<int32_t>  chartOf;     // per face
	int32_t nCharts = 0;
};

}  // namespace

// ───────────────────────────────────────────────────────────────────────────
void RunP1Census(const Scene *Sc, const char *const *matNames, int nMats)
{
	using clock = std::chrono::steady_clock;
	const auto tBegin = clock::now();
	if (!Sc || !matNames || nMats <= 0) {
		std::fprintf(stderr, "[V4-STITCH] no scene / no materials — inert\n");
		return;
	}
	Pass P;
	for (int i = 0; i < nMats; ++i) P.mats.idOf(matNames[i]);

	std::string matList;
	for (int i = 0; i < nMats; ++i) { if (i) matList += ","; matList += matNames[i]; }
	std::fprintf(stderr, "[V4-STITCH] begin mats=%s crease_deg=%.1f coplanar_deg=%.3f\n",
	             matList.c_str(), kCreaseDeg, kCoplanarDeg);

	// ── mesh walk + placement census ───────────────────────────────────────
	int32_t meshIdx = 0, meshesWithStone = 0, degenerate = 0, skippedByGuard = 0;
	bool allIdentity = true;
	std::vector<int64_t> perMatFaces(size_t(nMats), 0);
	std::vector<SoupTri> soup;
	std::map<std::string, int64_t> matCensus;
	int32_t soupMeshes = 0, soupNonIdentity = 0;
	for (const TriMesh *T = Sc->TriMeshHead; T; T = T->Next, ++meshIdx) {
		if (T->FIndex == 0 || !T->Faces || !T->Verts) continue;
		++soupMeshes;
		if (!PlacementOf(Sc, T).identity) ++soupNonIdentity;
		int32_t stone = 0, guarded = 0;
		for (dword i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C) continue;
			{
				SoupTri st;
				st.p[0] = V3(F.A->Pos); st.p[1] = V3(F.B->Pos); st.p[2] = V3(F.C->Pos);
				if (len(cross(st.p[1] - st.p[0], st.p[2] - st.p[0])) >= 1e-12) {
					st.mat = (F.Txtr && F.Txtr->Name) ? F.Txtr->Name : nullptr;
					st.meshIdx = meshIdx;
					soup.push_back(st);
					matCensus[st.mat ? st.mat : "(none)"]++;
				}
			}
			if (!F.Txtr || !F.Txtr->Name) continue;
			int32_t mi = -1;
			for (int m = 0; m < nMats; ++m)
				if (!std::strcmp(F.Txtr->Name, matNames[m])) { mi = m; break; }
			if (mi < 0) continue;
			++stone;
			if (!OldBakeWouldBake(F.Txtr)) { ++guarded; continue; }
			SFace sf;
			sf.mesh = T; sf.meshIdx = meshIdx; sf.faceIdx = int32_t(i); sf.matIdx = mi;
			sf.p[0] = V3(F.A->Pos); sf.p[1] = V3(F.B->Pos); sf.p[2] = V3(F.C->Pos);
			const V3 gn = cross(sf.p[1] - sf.p[0], sf.p[2] - sf.p[0]);
			const double gl = len(gn);
			if (gl < 1e-12) { ++degenerate; continue; }
			sf.area = 0.5 * gl;
			sf.n = gn * (1.0 / gl);
			// §REF "five things" #4: orient the geometric normal with the
			// ENGINE's (F.N), or a winding flip silently inverts everything
			// derived from the plane.  RotMat is not yet valid here, and this
			// scene's placements are identity (see the placement census), so
			// F.N is already in the same frame as the positions.
			const V3 eN = V3(F.N);
			if (len(eN) > 1e-9 && dot(sf.n, nrm(eN)) < 0.0) sf.n = sf.n * -1.0;
			sf.plD = dot(sf.n, sf.p[0]);
			perMatFaces[size_t(mi)]++;
			P.faces.push_back(sf);
		}
		skippedByGuard += guarded;
		if (stone == 0) continue;
		++meshesWithStone;
		const Placement pl = PlacementOf(Sc, T);
		if (!pl.identity) allIdentity = false;
		const char *nm = "(unnamed)";
		for (const Object *O = Sc->ObjectHead; O; O = O->Next)
			if (O->Type == Obj_TriMesh && O->Data == (const void *)T && O->Name) { nm = O->Name; break; }
		std::fprintf(stderr,
		    "[V4-STITCH] MESH idx=%d name=%s faces=%u stone=%d guard_skipped=%d "
		    "poskeys=%u sclkeys=%u rotkeys=%u pos=%.6f,%.6f,%.6f scl=%.6f,%.6f,%.6f "
		    "rot=%.6f,%.6f,%.6f,%.6f euler=%d parented=%d animated=%d identity=%d "
		    "htrack_visible=%d\n",
		    meshIdx, nm, unsigned(T->FIndex), stone, guarded,
		    pl.posKeys, pl.sclKeys, pl.rotKeys,
		    double(pl.pos.x), double(pl.pos.y), double(pl.pos.z),
		    double(pl.scl.x), double(pl.scl.y), double(pl.scl.z),
		    double(pl.rot.x), double(pl.rot.y), double(pl.rot.z), double(pl.rot.W),
		    int(pl.euler), int(pl.hasParentObj), int(pl.animated), int(pl.identity),
		    int((T->Flags & HTrack_Visible) != 0));
	}
	std::fprintf(stderr, "[V4-STITCH] placement all_identity=%d meshes_with_stone=%d\n",
	             int(allIdentity), meshesWithStone);
	std::fprintf(stderr, "[V4-STITCH] soup faces=%zu meshes=%d non_identity_meshes=%d materials=%zu free_tol=%.4f\n",
	             soup.size(), soupMeshes, soupNonIdentity, matCensus.size(), kFreeTol);
	{
		std::vector<std::pair<int64_t, const std::string *>> mc;
		for (const auto &kv : matCensus) mc.push_back({kv.second, &kv.first});
		std::sort(mc.begin(), mc.end(), [](const auto &x, const auto &y) { return x.first > y.first; });
		for (size_t i = 0; i < mc.size() && i < 24; ++i)
			std::fprintf(stderr, "[V4-STITCH] SOUPMAT rank=%zu name=%s faces=%lld\n",
			             i, mc[i].second->c_str(), (long long)mc[i].first);
	}
	if (!allIdentity)
		std::fprintf(stderr, "[V4-STITCH] WARNING placement is NOT identity on every stone "
		             "mesh — the cross-mesh stitch below is in MODEL space, which is only "
		             "world space when every contributing mesh is placed at the origin. "
		             "P2 must carry the placement explicitly.\n");

	const size_t nF = P.faces.size();
	std::fprintf(stderr, "[V4-STITCH] faces total=%zu", nF);
	for (int m = 0; m < nMats; ++m) std::fprintf(stderr, " %s=%lld", matNames[m], (long long)perMatFaces[size_t(m)]);
	std::fprintf(stderr, " degenerate=%d guard_skipped=%d\n", degenerate, skippedByGuard);
	if (nF == 0) { std::fprintf(stderr, "[V4-STITCH] nothing to stitch — inert\n"); return; }

	// ═══ (a1) EXACT-EQUALITY STITCH ════════════════════════════════════════
	// Primary: bitwise (with -0 folded).  Then a single-ULP pass, then the
	// instrumented ε fallback.  Each stage prints what it merged.
	std::unordered_map<PKey, int32_t, PKeyH> exactMap;
	std::vector<V3>      rawPos;          // one entry per exact class
	std::vector<PKey>    rawKey;
	const size_t nCorner = nF * 3;
	std::vector<int32_t> cornerClass(nCorner, 0);
	for (size_t f = 0; f < nF; ++f) {
		const TriMesh *T = P.faces[f].mesh;
		const Face &F = T->Faces[P.faces[f].faceIdx];
		const Vertex *vv[3] = { F.A, F.B, F.C };
		for (int k = 0; k < 3; ++k) {
			const PKey key { bitsOf(vv[k]->Pos.x), bitsOf(vv[k]->Pos.y), bitsOf(vv[k]->Pos.z) };
			auto it = exactMap.find(key);
			int32_t id;
			if (it == exactMap.end()) {
				id = int32_t(rawPos.size());
				exactMap.emplace(key, id);
				rawPos.push_back(P.faces[f].p[k]);
				rawKey.push_back(key);
			} else id = it->second;
			cornerClass[f*3 + size_t(k)] = id;
		}
	}
	const size_t nExact = rawPos.size();

	// shortest authored edge — ε is relative to it, never a world constant
	double shortestEdge = 1e300;
	for (size_t f = 0; f < nF; ++f)
		for (int k = 0; k < 3; ++k) {
			const double l = len(P.faces[f].p[(k+1)%3] - P.faces[f].p[k]);
			if (l > 0.0 && l < shortestEdge) shortestEdge = l;
		}
	if (shortestEdge > 1e299) shortestEdge = 0.0;
	const double eps = kEpsRelShortestEdge * shortestEdge;

	// order the exact classes by WORLD POSITION (DiagSplit's precision rule,
	// §REF "five things" #5): the sorted order becomes the vertex id, so
	// (min(id), max(id)) IS the canonical world ordering of an edge's endpoints
	// and nothing downstream can depend on which face the walk saw first.
	std::vector<int32_t> order(nExact);
	for (size_t i = 0; i < nExact; ++i) order[i] = int32_t(i);
	std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
		const V3 &A = rawPos[size_t(a)], &B = rawPos[size_t(b)];
		if (A.x != B.x) return A.x < B.x;
		if (A.y != B.y) return A.y < B.y;
		return A.z < B.z;
	});
	std::vector<int32_t> rank(nExact);
	for (size_t i = 0; i < nExact; ++i) rank[size_t(order[i])] = int32_t(i);

	// (a2) single-ULP pass over the world-sorted classes.  Expect 0.
	DSU weld; weld.init(nExact);
	int ulpMerges = 0;
	for (size_t i = 1; i < nExact; ++i) {
		const int32_t a = order[i-1], b = order[i];
		const V3 &A = rawPos[size_t(a)], &B = rawPos[size_t(b)];
		const float ax = float(A.x), ay = float(A.y), az = float(A.z);
		const float bx = float(B.x), by = float(B.y), bz = float(B.z);
		if (within1Ulp(ax,bx) && within1Ulp(ay,by) && within1Ulp(az,bz)) {
			if (weld.join(a, b)) {
				++ulpMerges;
				std::fprintf(stderr, "[V4-STITCH] ULPMERGE a=%.9g,%.9g,%.9g b=%.9g,%.9g,%.9g d=%.3e\n",
				             A.x, A.y, A.z, B.x, B.y, B.z, len(B - A));
			}
		}
	}

	// (a3) ε fallback — INSTRUMENTED, prints every merge.  Grid-bucketed so it
	// is not O(n²); ε is relative to the shortest authored edge.  Expect 0.
	int epsMerges = 0;
	if (eps > 0.0) {
		const double cell = std::max(eps, 1e-9) * 2.0;
		std::unordered_map<uint64_t, std::vector<int32_t>> grid;
		auto cellKey = [&](const V3 &p) -> uint64_t {
			const int64_t xi = int64_t(std::floor(p.x / cell));
			const int64_t yi = int64_t(std::floor(p.y / cell));
			const int64_t zi = int64_t(std::floor(p.z / cell));
			uint64_t h = 1469598103934665603ull;
			for (int64_t v : { xi, yi, zi }) { h ^= uint64_t(v); h *= 1099511628211ull; }
			return h;
		};
		for (size_t i = 0; i < nExact; ++i) grid[cellKey(rawPos[i])].push_back(int32_t(i));
		const double eps2 = eps * eps;
		for (size_t i = 0; i < nExact; ++i) {
			const V3 &A = rawPos[i];
			const int64_t x0 = int64_t(std::floor((A.x - eps) / cell)), x1 = int64_t(std::floor((A.x + eps) / cell));
			const int64_t y0 = int64_t(std::floor((A.y - eps) / cell)), y1 = int64_t(std::floor((A.y + eps) / cell));
			const int64_t z0 = int64_t(std::floor((A.z - eps) / cell)), z1 = int64_t(std::floor((A.z + eps) / cell));
			for (int64_t zz = z0; zz <= z1; ++zz)
			for (int64_t yy = y0; yy <= y1; ++yy)
			for (int64_t xx = x0; xx <= x1; ++xx) {
				uint64_t h = 1469598103934665603ull;
				for (int64_t v : { xx, yy, zz }) { h ^= uint64_t(v); h *= 1099511628211ull; }
				auto it = grid.find(h);
				if (it == grid.end()) continue;
				for (int32_t j : it->second) {
					if (size_t(j) <= i) continue;
					const V3 d = rawPos[size_t(j)] - A;
					if (dot(d, d) >= eps2) continue;
					if (weld.find(int32_t(i)) == weld.find(j)) continue;
					weld.join(int32_t(i), j);
					++epsMerges;
					std::fprintf(stderr, "[V4-STITCH] EPSMERGE a=%.9g,%.9g,%.9g b=%.9g,%.9g,%.9g d=%.3e eps=%.3e\n",
					             A.x, A.y, A.z, rawPos[size_t(j)].x, rawPos[size_t(j)].y, rawPos[size_t(j)].z,
					             len(d), eps);
				}
			}
		}
	}

	// final vertex ids: representative of the weld class, numbered by the
	// world-position rank of that representative.
	std::vector<int32_t> repRank(nExact, -1);
	for (size_t i = 0; i < nExact; ++i) {
		const int32_t r = weld.find(int32_t(i));
		if (repRank[size_t(r)] < 0 || rank[size_t(i)] < repRank[size_t(r)])
			repRank[size_t(r)] = rank[size_t(i)];
	}
	std::vector<std::pair<int32_t,int32_t>> reps;   // (rank, rep)
	for (size_t i = 0; i < nExact; ++i) if (weld.find(int32_t(i)) == int32_t(i)) reps.push_back({repRank[i], int32_t(i)});
	std::sort(reps.begin(), reps.end());
	std::vector<int32_t> vidOfRep(nExact, -1);
	P.vpos.resize(reps.size());
	for (size_t i = 0; i < reps.size(); ++i) { vidOfRep[size_t(reps[i].second)] = int32_t(i); P.vpos[i] = rawPos[size_t(reps[i].second)]; }
	for (size_t f = 0; f < nF; ++f)
		for (int k = 0; k < 3; ++k)
			P.faces[f].v[k] = vidOfRep[size_t(weld.find(cornerClass[f*3 + size_t(k)]))];
	const size_t nV = P.vpos.size();

	std::fprintf(stderr,
	    "[V4-STITCH] weld corners=%zu exact_classes=%zu exact_merges=%zu ulp_merges=%d "
	    "eps_merges=%d vertices=%zu shortest_edge=%.9g eps=%.6e eps_rel=%.1e\n",
	    nCorner, nExact, nCorner - nExact, ulpMerges, epsMerges, nV,
	    shortestEdge, eps, kEpsRelShortestEdge);

	// ═══ (a4) HALF-EDGE ════════════════════════════════════════════════════
	P.he.resize(nF * 3);
	std::map<std::pair<int32_t,int32_t>, std::vector<int32_t>> byEdge;
	for (size_t f = 0; f < nF; ++f)
		for (int k = 0; k < 3; ++k) {
			const int32_t h = int32_t(f*3 + size_t(k));
			P.he[size_t(h)].from = P.faces[f].v[k];
			P.he[size_t(h)].to   = P.faces[f].v[(k+1)%3];
			P.he[size_t(h)].face = int32_t(f);
			const int32_t a = std::min(P.he[size_t(h)].from, P.he[size_t(h)].to);
			const int32_t b = std::max(P.he[size_t(h)].from, P.he[size_t(h)].to);
			byEdge[{a,b}].push_back(h);
		}

	int64_t use1 = 0, use2 = 0, use3plus = 0, selfEdge = 0;
	int64_t matSeamE = 0, coplanarE = 0, creaseE = 0, smoothE = 0, convexE = 0, concaveE = 0;
	int64_t orientOk = 0, orientFlip = 0, convexDisagree = 0;
	double  matSeamLen = 0, creaseLen = 0, smoothLen = 0, freeLen = 0, totalEdgeLen = 0;
	std::vector<int32_t> freeList, nmList;

	P.edges.reserve(byEdge.size());
	for (auto &kv : byEdge) {
		EdgeAttr E;
		E.a = kv.first.first; E.b = kv.first.second;
		E.use = int32_t(kv.second.size());
		E.length = len(P.vpos[size_t(E.b)] - P.vpos[size_t(E.a)]);
		totalEdgeLen += E.length;
		if (E.a == E.b) { ++selfEdge; }
		const int32_t ei = int32_t(P.edges.size());
		for (int32_t h : kv.second) P.he[size_t(h)].edge = ei;

		if (E.use == 1) {
			++use1; freeLen += E.length; freeList.push_back(ei);
			E.f0 = P.he[size_t(kv.second[0])].face;
		} else if (E.use == 2) {
			++use2;
			const int32_t h0 = kv.second[0], h1 = kv.second[1];
			P.he[size_t(h0)].twin = h1; P.he[size_t(h1)].twin = h0;
			E.f0 = P.he[size_t(h0)].face; E.f1 = P.he[size_t(h1)].face;
			E.orientOk = (P.he[size_t(h0)].from == P.he[size_t(h1)].to &&
			              P.he[size_t(h0)].to   == P.he[size_t(h1)].from);
			if (E.orientOk) ++orientOk; else ++orientFlip;
			const SFace &FA = P.faces[size_t(E.f0)], &FB = P.faces[size_t(E.f1)];
			E.phiDeg = angDeg(FA.n, FB.n);
			// ATTRIBUTES.  Neither of these ever reaches the use-count above:
			// an edge with two faces of different materials is a MATERIAL SEAM,
			// not a boundary (survey §E; the bc79e39d class of bug).
			E.matSeam  = (FA.matIdx != FB.matIdx);
			E.coplanar = (E.phiDeg < kCoplanarDeg);
			E.crease   = (E.phiDeg >= kCreaseDeg);
			if (E.matSeam) { ++matSeamE; matSeamLen += E.length; }
			if (E.coplanar) ++coplanarE;
			if (E.crease) { ++creaseE; creaseLen += E.length; } else { ++smoothE; smoothLen += E.length; }
			// convex / concave: does the OTHER face's apex sit behind this
			// face's plane?  Computed from both sides; a disagreement is
			// reported rather than resolved silently.
			if (!E.coplanar) {
				int apexA = 0, apexB = 0;
				for (int k = 0; k < 3; ++k) { if (FA.v[k] != E.a && FA.v[k] != E.b) apexA = k; }
				for (int k = 0; k < 3; ++k) { if (FB.v[k] != E.a && FB.v[k] != E.b) apexB = k; }
				const double sA = dot(FA.n, FB.p[apexB] - FA.p[0]);
				const double sB = dot(FB.n, FA.p[apexA] - FB.p[0]);
				const int cA = (sA < 0) ? 1 : -1, cB = (sB < 0) ? 1 : -1;
				if (cA != cB) ++convexDisagree;
				E.convex = int8_t(cA);
				if (E.crease) { if (cA > 0) ++convexE; else ++concaveE; }
			}
		} else {
			++use3plus; nmList.push_back(ei);
			E.f0 = P.he[size_t(kv.second[0])].face;
		}
		P.edges.push_back(E);
	}

	// Boundary half-edges: one per use-count-1 edge, face == kNullFace, and it
	// IS the twin of the single face half-edge.  Not one null twin exists.
	int32_t boundaryHE = 0;
	for (auto &kv : byEdge) {
		if (kv.second.size() != 1) continue;
		const int32_t h = kv.second[0];
		HalfEdge b;
		b.from = P.he[size_t(h)].to; b.to = P.he[size_t(h)].from;
		b.face = kNullFace;
		b.edge = P.he[size_t(h)].edge;
		b.twin = h;
		P.he.push_back(b);
		P.he[size_t(h)].twin = int32_t(P.he.size()) - 1;
		++boundaryHE;
	}
	int32_t nullTwin = 0;
	for (const HalfEdge &h : P.he) if (h.twin < 0) ++nullTwin;

	std::fprintf(stderr,
	    "[V4-STITCH] edges total=%zu use1=%lld use2=%lld use3plus=%lld self=%lld total_len=%.4f\n",
	    P.edges.size(), (long long)use1, (long long)use2, (long long)use3plus,
	    (long long)selfEdge, totalEdgeLen);
	std::fprintf(stderr,
	    "[V4-STITCH] halfedge face_he=%zu boundary_he=%d null_twin=%d null_face_twins=%d\n",
	    nF * 3, boundaryHE, nullTwin, boundaryHE);
	std::fprintf(stderr,
	    "[V4-STITCH] attrs matseam_edges=%lld matseam_len=%.4f coplanar_edges=%lld "
	    "crease_edges=%lld crease_len=%.4f smooth_edges=%lld smooth_len=%.4f "
	    "convex=%lld concave=%lld orient_ok=%lld orient_flip=%lld convex_disagree=%lld\n",
	    (long long)matSeamE, matSeamLen, (long long)coplanarE,
	    (long long)creaseE, creaseLen, (long long)smoothE, smoothLen,
	    (long long)convexE, (long long)concaveE,
	    (long long)orientOk, (long long)orientFlip, (long long)convexDisagree);

	// ── use-count-1 edges, classified against the FULL SOUP ────────────────
	// USE-COUNT is the only topological fact (survey §E).  A stone edge that
	// only one stone face uses is NOT yet a free edge: the far side may carry a
	// face of another material.  This applies the reference renderer's own rule
	// so the two instruments answer the same question — a soup face sharing the
	// edge by position, else any soup face within kFreeTol of three interior
	// points of it (vertex coincidence alone is refuted, 10994f6ef014).
	std::sort(freeList.begin(), freeList.end(), [&](int32_t a, int32_t b) {
		return P.edges[size_t(a)].length > P.edges[size_t(b)].length;
	});
	{
		SoupGrid grid;
		grid.build(soup, std::max(0.25, kFreeTol * 8.0));
		const double tol2 = kFreeTol * kFreeTol;
		int64_t nSharedSoup = 0, nCoincident = 0, nTrulyFree = 0;
		double  lenSharedSoup = 0, lenCoincident = 0, lenFree = 0;
		std::map<std::string, int64_t> abutMat;
		int64_t crossMesh = 0;
		for (size_t i = 0; i < freeList.size(); ++i) {
			const EdgeAttr &E = P.edges[size_t(freeList[i])];
			const SFace &F = P.faces[size_t(E.f0)];
			const V3 a = P.vpos[size_t(E.a)], b = P.vpos[size_t(E.b)];
			// exclude the edge's OWN face by its position triple
			const PKey own[3] = {
				{ bitsOf(float(F.p[0].x)), bitsOf(float(F.p[0].y)), bitsOf(float(F.p[0].z)) },
				{ bitsOf(float(F.p[1].x)), bitsOf(float(F.p[1].y)), bitsOf(float(F.p[1].z)) },
				{ bitsOf(float(F.p[2].x)), bitsOf(float(F.p[2].y)), bitsOf(float(F.p[2].z)) } };
			int32_t best = -1; double bestD2 = tol2; bool exactShare = false;
			for (int s3 = 0; s3 <= 2 && !exactShare; ++s3) {
				const V3 q = a + (b - a) * (0.25 + 0.25 * s3);
				const int gx = grid.cx(q.x), gy = grid.cy(q.y), gz = grid.cz(q.z);
				for (int dz = -1; dz <= 1; ++dz) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
					const int x = gx+dx, y = gy+dy, z = gz+dz;
					if (x < 0 || y < 0 || z < 0 || x >= grid.nx || y >= grid.ny || z >= grid.nz) continue;
					for (int32_t si : grid.b[size_t(grid.idx(x,y,z))]) {
						const SoupTri &st = soup[size_t(si)];
						int nSame = 0;
						for (int k = 0; k < 3; ++k) {
							const PKey sk { bitsOf(float(st.p[k].x)), bitsOf(float(st.p[k].y)), bitsOf(float(st.p[k].z)) };
							for (int m = 0; m < 3; ++m) if (sk == own[m]) { ++nSame; break; }
						}
						if (nSame == 3) continue;                       // this very face
						const double d2 = PointTriDist2(q, st.p[0], st.p[1], st.p[2]);
						if (d2 < bestD2) { bestD2 = d2; best = si; if (d2 <= 0.0) exactShare = true; }
					}
				}
			}
			const char *cls = (best < 0) ? "free" : (bestD2 <= 1e-18 ? "shared_soup" : "coincident");
			if (best < 0) { ++nTrulyFree; lenFree += E.length; }
			else if (bestD2 <= 1e-18) { ++nSharedSoup; lenSharedSoup += E.length; }
			else { ++nCoincident; lenCoincident += E.length; }
			const char *am = (best >= 0 && soup[size_t(best)].mat) ? soup[size_t(best)].mat : "(none)";
			if (best >= 0) {
				abutMat[am]++;
				if (soup[size_t(best)].meshIdx != F.meshIdx) ++crossMesh;
			}
			std::fprintf(stderr,
			    "[V4-STITCH] USE1 i=%zu class=%s a=%.6f,%.6f,%.6f b=%.6f,%.6f,%.6f len=%.6f "
			    "mat=%s mesh=%d face=%d n=%.4f,%.4f,%.4f abut_mat=%s abut_d=%.6f\n",
			    i, cls, a.x, a.y, a.z, b.x, b.y, b.z, E.length,
			    P.mats.names[size_t(F.matIdx)].c_str(), F.meshIdx, F.faceIdx,
			    F.n.x, F.n.y, F.n.z, am, best >= 0 ? std::sqrt(bestD2) : -1.0);
		}
		std::fprintf(stderr,
		    "[V4-STITCH] use1_class free=%lld free_len=%.4f shared_soup=%lld shared_soup_len=%.4f "
		    "coincident=%lld coincident_len=%.4f cross_mesh_abut=%lld total=%lld total_len=%.4f\n",
		    (long long)nTrulyFree, lenFree, (long long)nSharedSoup, lenSharedSoup,
		    (long long)nCoincident, lenCoincident, (long long)crossMesh,
		    (long long)use1, freeLen);
		for (const auto &kv : abutMat)
			std::fprintf(stderr, "[V4-STITCH] ABUTMAT name=%s edges=%lld\n", kv.first.c_str(), (long long)kv.second);
	}
	for (size_t i = 0; i < nmList.size(); ++i) {
		const EdgeAttr &E = P.edges[size_t(nmList[i])];
		std::fprintf(stderr,
		    "[V4-STITCH] NONMANIFOLD i=%zu a=%.6f,%.6f,%.6f b=%.6f,%.6f,%.6f len=%.6f uses=%d\n",
		    i, P.vpos[size_t(E.a)].x, P.vpos[size_t(E.a)].y, P.vpos[size_t(E.a)].z,
		    P.vpos[size_t(E.b)].x, P.vpos[size_t(E.b)].y, P.vpos[size_t(E.b)].z,
		    E.length, E.use);
	}
	const auto tStitch = clock::now();
	std::fprintf(stderr, "[V4-STITCH] timing ms=%.3f\n",
	    std::chrono::duration<double, std::milli>(tStitch - tBegin).count());

	// ═══ (b) CHART REGISTRY ════════════════════════════════════════════════
	// Region growing under a NORMAL BUDGET, MATERIAL-BLIND, across all meshes.
	//
	// The acceptance test is the DIHEDRAL across the shared edge, not the angle
	// to a running proxy, and that choice is deliberate:
	//   * it is what merges the curved wall — its narrow strips each differ
	//     from their neighbour by a few degrees while the chart as a whole
	//     spans far more than the budget (design §2b, and the trap 60e3e63bed65
	//     that a smooth seam is not a corner);
	//   * it is ORDER-INDEPENDENT (the charts are exactly the connected
	//     components of the graph of sub-budget edges), so the registry is
	//     reproducible run to run and does not depend on the seed order a
	//     Lloyd-iterated VSA would need.
	// The L^{2,1} proxy the survey names is still computed and REPORTED for
	// every chart (area-weighted normal + the max deviation from it), so the
	// number that would refute this choice — a chart the budget cannot fit — is
	// on the census rather than assumed away.
	const double budget = double(FeatureFlags::v4_chart_budget_deg());
	DSU cd; cd.init(nF);
	for (const EdgeAttr &E : P.edges) {
		if (E.use != 2) continue;
		if (E.phiDeg <= budget) cd.join(E.f0, E.f1);
	}
	std::vector<int32_t> chartId(nF, -1);
	int32_t nCharts = 0;
	for (size_t f = 0; f < nF; ++f) {
		const int32_t r = cd.find(int32_t(f));
		if (chartId[size_t(r)] < 0) chartId[size_t(r)] = nCharts++;
	}
	for (size_t f = 0; f < nF; ++f) P.faces[f].chart = chartId[size_t(cd.find(int32_t(f)))];
	P.chartOf.resize(nF);
	for (size_t f = 0; f < nF; ++f) P.chartOf[f] = P.faces[f].chart;
	P.nCharts = nCharts;

	struct Chart {
		int64_t faces = 0; double area = 0; V3 nsum, ctr;
		double maxDevDeg = 0, maxDihedralDeg = 0;
		uint32_t matMask = 0; std::vector<int32_t> meshes;
	};
	std::vector<Chart> ch;
	ch.resize(size_t(nCharts));
	for (size_t f = 0; f < nF; ++f) {
		Chart &c = ch[size_t(P.faces[f].chart)];
		c.faces++; c.area += P.faces[f].area;
		c.nsum = c.nsum + P.faces[f].n * P.faces[f].area;
		c.ctr = c.ctr + (P.faces[f].p[0] + P.faces[f].p[1] + P.faces[f].p[2]) * (P.faces[f].area / 3.0);
		c.matMask |= (1u << P.faces[f].matIdx);
		if (std::find(c.meshes.begin(), c.meshes.end(), P.faces[f].meshIdx) == c.meshes.end())
			c.meshes.push_back(P.faces[f].meshIdx);
	}
	for (Chart &c : ch) { if (c.area > 0) { c.ctr = c.ctr * (1.0 / c.area); } c.nsum = nrm(c.nsum); }
	for (size_t f = 0; f < nF; ++f) {
		Chart &c = ch[size_t(P.faces[f].chart)];
		c.maxDevDeg = std::max(c.maxDevDeg, angDeg(P.faces[f].n, c.nsum));
	}
	for (const EdgeAttr &E : P.edges)
		if (E.use == 2 && P.faces[size_t(E.f0)].chart == P.faces[size_t(E.f1)].chart)
			ch[size_t(P.faces[size_t(E.f0)].chart)].maxDihedralDeg =
			    std::max(ch[size_t(P.faces[size_t(E.f0)].chart)].maxDihedralDeg, E.phiDeg);

	int64_t unassigned = 0, multiChart = 0;
	for (size_t f = 0; f < nF; ++f) if (P.faces[f].chart < 0) ++unassigned;
	int32_t overBudget = 0; double maxDevAll = 0;
	std::vector<int64_t> sizes;
	for (const Chart &c : ch) { sizes.push_back(c.faces); if (c.maxDevDeg > budget) ++overBudget; maxDevAll = std::max(maxDevAll, c.maxDevDeg); }
	std::sort(sizes.begin(), sizes.end());

	std::fprintf(stderr,
	    "[V4-CHARTS] registry budget_deg=%.3f charts=%d faces=%zu unassigned=%lld multi_chart=%lld "
	    "faces_min=%lld faces_p50=%lld faces_max=%lld maxdev_max_deg=%.4f over_budget_charts=%d\n",
	    budget, nCharts, nF, (long long)unassigned, (long long)multiChart,
	    sizes.empty() ? 0LL : (long long)sizes.front(),
	    sizes.empty() ? 0LL : (long long)sizes[sizes.size()/2],
	    sizes.empty() ? 0LL : (long long)sizes.back(), maxDevAll, overBudget);

	// Budget sweep — the number this scene's chart registry is actually
	// sensitive to.  Printed rather than assumed: the design expects the curved
	// wall's strips to merge under the budget, and the sweep is what says
	// whether this scene HAS such strips and at which threshold they join.
	{
		std::string sweep;
		for (double bdg : { 1.0, 5.0, 10.0, 15.0, 20.0, 25.0, 29.0, 30.0, 45.0 }) {
			DSU d2; d2.init(nF);
			for (const EdgeAttr &E : P.edges) if (E.use == 2 && E.phiDeg <= bdg) d2.join(E.f0, E.f1);
			int32_t n = 0;
			for (size_t f = 0; f < nF; ++f) if (d2.find(int32_t(f)) == int32_t(f)) ++n;
			char buf[64]; std::snprintf(buf, sizeof buf, " %.0f:%d", bdg, n);
			sweep += buf;
		}
		std::fprintf(stderr, "[V4-CHARTS] sweep budget_deg:charts%s\n", sweep.c_str());
	}
	// Area concentration — "~24 dominant planes" is a claim about AREA, not
	// about how many charts exist, so both numbers are reported.
	{
		std::vector<double> ar;
		double tot = 0;
		for (const Chart &c : ch) { ar.push_back(c.area); tot += c.area; }
		std::sort(ar.begin(), ar.end(), std::greater<double>());
		double acc = 0; int n50 = 0, n90 = 0, n99 = 0;
		for (size_t i = 0; i < ar.size(); ++i) {
			acc += ar[i];
			if (!n50 && acc >= 0.50 * tot) n50 = int(i) + 1;
			if (!n90 && acc >= 0.90 * tot) n90 = int(i) + 1;
			if (!n99 && acc >= 0.99 * tot) n99 = int(i) + 1;
		}
		std::fprintf(stderr, "[V4-CHARTS] area total=%.4f charts_for_50pct=%d charts_for_90pct=%d charts_for_99pct=%d\n",
		             tot, n50, n90, n99);
	}
	std::vector<int32_t> chOrder;
	chOrder.resize(size_t(nCharts));
	for (int32_t i = 0; i < nCharts; ++i) chOrder[size_t(i)] = i;
	std::sort(chOrder.begin(), chOrder.end(), [&](int32_t a, int32_t b) { return ch[size_t(a)].area > ch[size_t(b)].area; });
	for (size_t i = 0; i < chOrder.size(); ++i) {
		const Chart &c = ch[size_t(chOrder[i])];
		std::string ms;
		for (int m = 0; m < nMats; ++m) if (c.matMask & (1u << m)) { if (!ms.empty()) ms += "+"; ms += matNames[m]; }
		std::fprintf(stderr,
		    "[V4-CHARTS] CHART rank=%zu id=%d faces=%lld area=%.4f n=%.6f,%.6f,%.6f "
		    "maxdev_deg=%.4f maxdihedral_deg=%.4f mats=%s meshes=%zu ctr=%.4f,%.4f,%.4f\n",
		    i, chOrder[i], (long long)c.faces, c.area, c.nsum.x, c.nsum.y, c.nsum.z,
		    c.maxDevDeg, c.maxDihedralDeg, ms.c_str(), c.meshes.size(), c.ctr.x, c.ctr.y, c.ctr.z);
	}

	// ── junction table: every chart pair sharing an edge ───────────────────
	struct Junc {
		int64_t edges = 0; double len = 0, phiLen = 0;
		int64_t convex = 0, concave = 0, smooth = 0, matSeam = 0;
		double  phiMin = 1e9, phiMax = -1e9;
		V3      ctr;
	};
	std::map<std::pair<int32_t,int32_t>, Junc> junc;
	for (const EdgeAttr &E : P.edges) {
		if (E.use != 2) continue;
		const int32_t ca = P.faces[size_t(E.f0)].chart, cb = P.faces[size_t(E.f1)].chart;
		if (ca == cb) continue;
		Junc &J = junc[{std::min(ca,cb), std::max(ca,cb)}];
		J.edges++; J.len += E.length; J.phiLen += E.phiDeg * E.length;
		J.phiMin = std::min(J.phiMin, E.phiDeg); J.phiMax = std::max(J.phiMax, E.phiDeg);
		J.ctr = J.ctr + (P.vpos[size_t(E.a)] + P.vpos[size_t(E.b)]) * (0.5 * E.length);
		if (E.matSeam) J.matSeam++;
		if (!E.crease) J.smooth++; else if (E.convex > 0) J.convex++; else J.concave++;
	}
	double juncLen = 0; int64_t jConvex = 0, jConcave = 0, jSmooth = 0;
	for (auto &kv : junc) {
		juncLen += kv.second.len;
		jConvex += kv.second.convex; jConcave += kv.second.concave; jSmooth += kv.second.smooth;
	}
	std::fprintf(stderr,
	    "[V4-CHARTS] junc_summary junctions=%zu total_len=%.4f edges_convex=%lld edges_concave=%lld edges_smooth=%lld\n",
	    junc.size(), juncLen, (long long)jConvex, (long long)jConcave, (long long)jSmooth);
	{
		std::vector<std::pair<double, const Junc *>> rows;
		std::vector<std::pair<int32_t,int32_t>> keys;
		for (auto &kv : junc) { rows.push_back({kv.second.len, &kv.second}); keys.push_back(kv.first); }
		std::vector<size_t> idx(rows.size());
		for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
		std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return rows[a].first > rows[b].first; });
		for (size_t r = 0; r < idx.size(); ++r) {
			const Junc &J = *rows[idx[r]].second;
			const auto &k = keys[idx[r]];
			const char *cls = (J.convex && !J.concave && !J.smooth) ? "convex"
			                : (J.concave && !J.convex && !J.smooth) ? "concave"
			                : (J.smooth && !J.convex && !J.concave) ? "smooth" : "mixed";
			std::fprintf(stderr,
			    "[V4-CHARTS] JUNC rank=%zu a=%d b=%d edges=%lld len=%.6f phi=%.4f "
			    "phimin=%.4f phimax=%.4f class=%s convex=%lld concave=%lld smooth=%lld "
			    "matseam=%lld ctr=%.4f,%.4f,%.4f\n",
			    r, k.first, k.second, (long long)J.edges, J.len,
			    J.len > 0 ? J.phiLen / J.len : 0.0, J.phiMin, J.phiMax, cls,
			    (long long)J.convex, (long long)J.concave, (long long)J.smooth,
			    (long long)J.matSeam,
			    J.len > 0 ? J.ctr.x / J.len : 0.0,
			    J.len > 0 ? J.ctr.y / J.len : 0.0,
			    J.len > 0 ? J.ctr.z / J.len : 0.0);
		}
	}

	// ── plane-pair reconciliation with the reference's crease census ───────
	// §REF-2's 453 junctions / 7375 u are PLANE-PAIR junctions: the reference
	// clusters face planes within 0.5° and 5 mm and keys each shared edge on
	// the pair of plane ids, INCLUDING the co-planar pairs (a quad's own
	// diagonal keys as (i,i)).  A CHART pair is a different identity — a chart
	// is connected and can span many planes (the curved wall), while two
	// disconnected co-planar patches are one plane but two charts.  So this
	// block reproduces the reference's identity over the same edges, which is
	// what makes the two counts comparable at all.
	{
		std::vector<std::pair<V3,double>> planes;
		auto planeIdOf = [&](const V3 &n, double d) -> int32_t {
			for (size_t i = 0; i < planes.size(); ++i)
				if (dot(planes[i].first, n) > 0.99996 && std::fabs(planes[i].second - d) < 0.005)
					return int32_t(i);
			planes.push_back({n, d});
			return int32_t(planes.size()) - 1;
		};
		std::vector<int32_t> planeOf(nF, -1);
		for (size_t f = 0; f < nF; ++f) planeOf[f] = planeIdOf(P.faces[f].n, P.faces[f].plD);
		std::map<std::pair<int32_t,int32_t>, std::pair<int64_t,double>> pj;
		int64_t coplanarPairs = 0; double coplanarLen = 0;
		for (const EdgeAttr &E : P.edges) {
			if (E.use != 2) continue;
			const int32_t pa = planeOf[size_t(E.f0)], pb = planeOf[size_t(E.f1)];
			auto &acc = pj[{std::min(pa,pb), std::max(pa,pb)}];
			acc.first++; acc.second += E.length;
			if (pa == pb) { ++coplanarPairs; coplanarLen += E.length; }
		}
		int64_t nonCo = 0; double nonCoLen = 0, allLen = 0;
		for (auto &kv : pj) {
			allLen += kv.second.second;
			if (kv.first.first != kv.first.second) { ++nonCo; nonCoLen += kv.second.second; }
		}
		// how many charts each plane hosts (a plane can carry several
		// DISCONNECTED charts; a chart can span several planes)
		std::map<int32_t, std::vector<int32_t>> chartsOfPlane;
		for (size_t f = 0; f < nF; ++f) {
			auto &v = chartsOfPlane[planeOf[f]];
			if (std::find(v.begin(), v.end(), P.faces[f].chart) == v.end()) v.push_back(P.faces[f].chart);
		}
		int32_t multiPlane = 0, maxChartsPerPlane = 0;
		for (auto &kv : chartsOfPlane) {
			if (kv.second.size() > 1) ++multiPlane;
			maxChartsPerPlane = std::max(maxChartsPerPlane, int32_t(kv.second.size()));
		}
		std::fprintf(stderr,
		    "[V4-CHARTS] planepair planes=%zu junctions=%zu total_len=%.4f "
		    "noncoplanar_junctions=%lld noncoplanar_len=%.4f coplanar_edges=%lld coplanar_len=%.4f "
		    "planes_with_multiple_charts=%d max_charts_per_plane=%d\n",
		    planes.size(), pj.size(), allLen, (long long)nonCo, nonCoLen,
		    (long long)coplanarPairs, coplanarLen, multiPlane, maxChartsPerPlane);
	}

	const auto tEnd = clock::now();
	std::fprintf(stderr, "[V4-CHARTS] timing ms=%.3f\n",
	    std::chrono::duration<double, std::milli>(tEnd - tStitch).count());
	std::fprintf(stderr, "[V4-CENSUS] timing total_ms=%.3f\n",
	    std::chrono::duration<double, std::milli>(tEnd - tBegin).count());
	std::fflush(stderr);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PHASE 2 — the UNDISPLACED lattice (design §2c, §6 P2)
//
//  WHY THE TOPOLOGY IS BUILT A SECOND TIME HERE rather than shared with
//  RunP1Census: the census is a DIAGNOSTIC that retires with the old bake
//  (design §5), and it carries a full-scene-soup free-edge classification the
//  lattice does not need (it costs 12.8 of the census's 13 ms — 21975c7a87d5).
//  The lattice is the PRODUCT.  Keeping them separate means (i) P2 cannot
//  regress the P1 gate, and (ii) with --v4_census BOTH blocks print and
//  tools/v4_census.py cross-checks them: two independent builds agreeing on
//  226 faces / 155 vertices / 404 edges / 130 use-1 / 73 charts is stronger
//  evidence than one shared build asserting it.  The stitch here is BITWISE
//  ONLY, because P1 measured the single-ULP pass and the ε fallback merging
//  exactly 0 (bcd934f525da); a divergence in the vertex count is exactly what
//  the cross-check would catch.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// ── P2 topology ────────────────────────────────────────────────────────────
struct P2Face {
	const TriMesh *mesh = nullptr;
	const Face    *src  = nullptr;
	int32_t meshIdx = 0, faceIdx = 0, matIdx = 0;
	int32_t v[3] = {0,0,0};        // stitched vertex ids (world-position order)
	V3      p[3];
	double  uv[3][2] = {{0,0},{0,0},{0,0}};
	V3      n;                     // oriented by the engine's F.N
	double  area = 0.0;
	int32_t chart = -1;
	int32_t edge[3] = {-1,-1,-1};  // edge id of corner k -> k+1
	uint16_t planeOrd = 0;
};
struct P2Edge {
	int32_t a = 0, b = 0;          // a < b: THE canonical world-position order
	int32_t use = 0;
	double  length = 0.0;
	int32_t nSeg = 1;              // R2: the border's own sample count
	int32_t firstVert = -1;        // global id of interior sample 1 (contiguous)
	// P4: the border's INTERIOR sample parameters, in the canonical a→b
	// direction, formed once here so the two faces cannot derive two lists.
	// `tnum[q] >= 0` marks an exact-rational R3 sample (tpar[q] == tnum[q]/nSeg
	// and BorderSample takes the integer path); −1 marks a mortar-crossing
	// sample from --v4_ring_grooves, which has no rational form.
	std::vector<double>  tpar;
	std::vector<int32_t> tnum;
};

struct P2Topo {
	std::vector<P2Face> faces;
	std::vector<V3>     vpos;
	std::vector<const Vertex *> vsrc;      // one authored record per stitched vertex
	std::vector<double> vspacing;          // R2 input: per-vertex target world spacing
	std::vector<P2Edge> edges;
	std::vector<int32_t> vGlobal;          // stitched id -> emitted vertex id
	int32_t nCharts = 0;
	TriMesh *mesh = nullptr;               // the ONE mesh carrying stone
	int32_t meshesWithStone = 0;
	size_t  corners = 0, exactClasses = 0;
	int64_t use1 = 0, use2 = 0, use3plus = 0;
};

// Build (a)+(b) over the target materials.  Returns false when the bake set is
// empty or spans more than one mesh (the cross-mesh case would need the model→
// world placement P1's deviation #3 flagged, and this scene does not have it —
// refusing loudly beats baking in the wrong space).
static bool BuildP2Topo(Scene *Sc, const char *const *matNames, int nMats, P2Topo &T)
{
	int32_t meshIdx = 0;
	for (TriMesh *M = Sc->TriMeshHead; M; M = M->Next, ++meshIdx) {
		if (M->FIndex == 0 || !M->Faces || !M->Verts) continue;
		int32_t stone = 0;
		for (dword i = 0; i < M->FIndex; ++i) {
			const Face &F = M->Faces[i];
			if (!F.A || !F.B || !F.C || !F.Txtr || !F.Txtr->Name) continue;
			int32_t mi = -1;
			for (int m = 0; m < nMats; ++m)
				if (!std::strcmp(F.Txtr->Name, matNames[m])) { mi = m; break; }
			if (mi < 0) continue;
			++stone;
			if (!OldBakeWouldBake(F.Txtr)) continue;      // the old bake's own guard
			P2Face sf;
			sf.mesh = M; sf.src = &F; sf.meshIdx = meshIdx; sf.faceIdx = int32_t(i); sf.matIdx = mi;
			sf.p[0] = V3(F.A->Pos); sf.p[1] = V3(F.B->Pos); sf.p[2] = V3(F.C->Pos);
			sf.uv[0][0] = F.U1; sf.uv[0][1] = F.V1;
			sf.uv[1][0] = F.U2; sf.uv[1][1] = F.V2;
			sf.uv[2][0] = F.U3; sf.uv[2][1] = F.V3;
			const V3 gn = cross(sf.p[1] - sf.p[0], sf.p[2] - sf.p[0]);
			const double gl = len(gn);
			if (gl < 1e-12) continue;
			sf.area = 0.5 * gl;
			sf.n = gn * (1.0 / gl);
			const V3 eN = V3(F.N);
			if (len(eN) > 1e-9 && dot(sf.n, nrm(eN)) < 0.0) sf.n = sf.n * -1.0;
			T.faces.push_back(sf);
		}
		if (stone) { ++T.meshesWithStone; T.mesh = M; }
	}
	if (T.faces.empty() || T.meshesWithStone != 1) return false;

	// ── stitch: BITWISE only, ids = world-position rank (survey §E; P1 measured
	// 0 ULP and 0 ε merges, bcd934f525da) ──────────────────────────────────
	const size_t nF = T.faces.size();
	std::unordered_map<PKey, int32_t, PKeyH> exactMap;
	std::vector<V3> rawPos;
	std::vector<const Vertex *> rawSrc;
	std::vector<int32_t> cornerClass(nF * 3, 0);
	for (size_t f = 0; f < nF; ++f) {
		const Face &F = *T.faces[f].src;
		const Vertex *vv[3] = { F.A, F.B, F.C };
		for (int k = 0; k < 3; ++k) {
			const PKey key { bitsOf(vv[k]->Pos.x), bitsOf(vv[k]->Pos.y), bitsOf(vv[k]->Pos.z) };
			auto it = exactMap.find(key);
			int32_t id;
			if (it == exactMap.end()) {
				id = int32_t(rawPos.size());
				exactMap.emplace(key, id);
				rawPos.push_back(T.faces[f].p[k]);
				rawSrc.push_back(vv[k]);
			} else id = it->second;
			cornerClass[f*3 + size_t(k)] = id;
		}
	}
	T.corners = nF * 3; T.exactClasses = rawPos.size();
	std::vector<int32_t> order(rawPos.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = int32_t(i);
	std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
		const V3 &A = rawPos[size_t(a)], &B = rawPos[size_t(b)];
		if (A.x != B.x) return A.x < B.x;
		if (A.y != B.y) return A.y < B.y;
		return A.z < B.z;
	});
	std::vector<int32_t> rank(rawPos.size());
	for (size_t i = 0; i < order.size(); ++i) rank[size_t(order[i])] = int32_t(i);
	T.vpos.resize(rawPos.size()); T.vsrc.resize(rawPos.size());
	for (size_t i = 0; i < order.size(); ++i) {
		T.vpos[i]  = rawPos[size_t(order[i])];
		T.vsrc[i]  = rawSrc[size_t(order[i])];
	}
	for (size_t f = 0; f < nF; ++f)
		for (int k = 0; k < 3; ++k)
			T.faces[f].v[k] = rank[size_t(cornerClass[f*3 + size_t(k)])];

	// ── edges: (min id, max id) IS the canonical world-position ordering ────
	std::map<std::pair<int32_t,int32_t>, std::vector<std::pair<int32_t,int>>> byEdge;
	for (size_t f = 0; f < nF; ++f)
		for (int k = 0; k < 3; ++k) {
			const int32_t a = std::min(T.faces[f].v[k], T.faces[f].v[(k+1)%3]);
			const int32_t b = std::max(T.faces[f].v[k], T.faces[f].v[(k+1)%3]);
			byEdge[{a,b}].push_back({int32_t(f), k});
		}
	T.edges.reserve(byEdge.size());
	for (auto &kv : byEdge) {
		P2Edge E;
		E.a = kv.first.first; E.b = kv.first.second;
		E.use = int32_t(kv.second.size());
		E.length = len(T.vpos[size_t(E.b)] - T.vpos[size_t(E.a)]);
		const int32_t ei = int32_t(T.edges.size());
		for (auto &fk : kv.second) T.faces[size_t(fk.first)].edge[fk.second] = ei;
		if      (E.use == 1) ++T.use1;
		else if (E.use == 2) ++T.use2;
		else                 ++T.use3plus;
		T.edges.push_back(E);
	}

	// ── charts: connected components of the sub-budget edge graph (P1's own
	// order-independent choice, deviation #1) ──────────────────────────────
	const double budget = double(FeatureFlags::v4_chart_budget_deg());
	DSU cd; cd.init(nF);
	for (auto &kv : byEdge) {
		if (kv.second.size() != 2) continue;
		const int32_t f0 = kv.second[0].first, f1 = kv.second[1].first;
		if (angDeg(T.faces[size_t(f0)].n, T.faces[size_t(f1)].n) <= budget) cd.join(f0, f1);
	}
	std::vector<int32_t> cid(nF, -1);
	int32_t nc = 0;
	for (size_t f = 0; f < nF; ++f) {
		const int32_t r = cd.find(int32_t(f));
		if (cid[size_t(r)] < 0) cid[size_t(r)] = nc++;
	}
	for (size_t f = 0; f < nF; ++f) T.faces[f].chart = cid[size_t(cd.find(int32_t(f)))];
	T.nCharts = nc;
	return true;
}

// ── the per-material map grid (the BREAKLINE source) ───────────────────────
// Everything here comes from the height map, not from the mesh: the mip guard
// and the block pitch are the old bake's own (EstimateBlockPitch, map-relative),
// and the mortar grid is MeshOps_FindStoneGrooveGrid — the finding extracted
// out of DisplaceStoneSubdiv verbatim so both bakes run the same code.  What v4
// does NOT take is the per-line rep heights (StoneLineRep): design §2c keeps the
// finding and drops the rep levels.
struct MatGrid {
	bool ok = false;
	const Texture *hm = nullptr;
	int    useMip = 0, mipW = 1, mipH = 1;
	float  pitchX = 0, pitchY = 0;
	bool   havePitch = false;
	double targetTexX = 0, targetTexY = 0;
	StoneGrooveGrid grid;
	std::vector<StoneRowT> rowTpl;
	std::vector<std::vector<float>> colTpl;
	// Per band, the COLUMN breaklines with the id of the groove run each came
	// from (-1 = none).  Rebuilt here rather than read out of colTpl because the
	// run id is what tells a groove-band interval from a block interior, and
	// colTpl throws it away.  Same construction, same expressions.
	std::vector<std::vector<std::pair<double,int>>> colLine;
	double y0Base = 0.0;                     // rowTpl[0].y0; the template's phase

	// ── the height FIELD (design §1.1, §2d) ────────────────────────────────
	// `fieldOk` is independent of `ok`: a material with no mortar grid has no
	// breaklines but still has a height field, and P3 must not silently bake it
	// flat.  All three fields are ROW-MAJOR at the bake mip — the block swizzle
	// is undone once, here, so one sampler serves all three and the bilinear
	// reconstruction cannot disagree between them.
	//   fBase  the field itself: what a BEVEL node reads, and what every
	//          [V4-RELIEF] r-value is measured against;
	//   fMax   the MAX-pyramid over a `pyrRad`-texel window, what a PLATEAU
	//          node reads (Garland & Heckbert §3.2; Tevs 2008);
	//   fMin   the MIN-pyramid, what a GROOVE node reads.
	bool   fieldOk = false;
	std::vector<uint8_t> fBase, fMax, fMin;
	double mipMean = 0.5;                    // the plain mean of the mip's bytes
	double h0 = 0.0, h1 = 1.0;               // min / max of the mip, 0..1
	double dAbsMax = 0.0;                    // max |amp*(h-mipMean)|
	int    pyrRad = 0;                       // the window radius actually used
};

// The block-tiled texel address of DEMO/MeshOps.cpp:428 — the one
// SampleHeight8Bilinear reads through.  COPIED rather than shared, exactly as
// FDS/RENDER/DeferredDisplaceRef.cpp:131 copies it: the v4 bake reading the
// same bytes through its own address arithmetic is a second implementation, and
// the [V4-RELIEF] census's r-value is only ground truth if it is one.
static inline size_t V4SwizzledOffset(int x, int y, int bsx, int bsy, int SizeY) {
	const int BX = 1 << bsx, BY = 1 << bsy;
	const int blockRowsPerCol = SizeY >> bsy;
	return (size_t(x >> bsx) * blockRowsPerCol + size_t(y >> bsy)) * size_t(BX * BY)
	       + size_t(y & (BY - 1)) * BX + size_t(x & (BX - 1));
}

static inline int WrapI(int a, int m) { const int r = a % m; return r < 0 ? r + m : r; }

// h in 0..1 at a normalized UV: texel-CENTRE convention + toroidal wrap, the
// convention of both SampleHeight8Bilinear and the reference renderer's
// RefMat::sampleH.  Any deviation here would make dz measure a sampling
// difference instead of the mesh (design §1.1).
static double SampleField(const std::vector<uint8_t> &f, int mw, int mh,
                          double u, double v)
{
	if (f.empty()) return 0.0;
	const double x = u * double(mw) - 0.5, y = v * double(mh) - 0.5;
	const double fxf = std::floor(x), fyf = std::floor(y);
	const double fx = x - fxf, fy = y - fyf;
	const int x0 = WrapI(int(fxf), mw), x1 = WrapI(int(fxf) + 1, mw);
	const int y0 = WrapI(int(fyf), mh), y1 = WrapI(int(fyf) + 1, mh);
	const double h00 = double(f[size_t(y0)*size_t(mw) + size_t(x0)]);
	const double h10 = double(f[size_t(y0)*size_t(mw) + size_t(x1)]);
	const double h01 = double(f[size_t(y1)*size_t(mw) + size_t(x0)]);
	const double h11 = double(f[size_t(y1)*size_t(mw) + size_t(x1)]);
	const double top = h00 + (h10 - h00) * fx;
	const double bot = h01 + (h11 - h01) * fx;
	return (top + (bot - top) * fy) * (1.0 / 255.0);
}

// Separable max (WANT_MAX) / min dilation over a (2R+1)² texel window with
// toroidal wrap.  A max over a window IS the max-pyramid evaluated at the level
// that covers that window, without the power-of-two quantisation a literal
// pyramid would impose on a shoulder that is 1.25 texels wide.
static void PyramidFilter(const std::vector<uint8_t> &src, int mw, int mh, int R,
                          bool wantMax, std::vector<uint8_t> &dst)
{
	dst = src;
	if (R <= 0) return;
	std::vector<uint8_t> tmp(src.size());
	for (int y = 0; y < mh; ++y)
		for (int x = 0; x < mw; ++x) {
			int acc = wantMax ? 0 : 255;
			for (int dx = -R; dx <= R; ++dx) {
				const int v = src[size_t(y)*size_t(mw) + size_t(WrapI(x+dx, mw))];
				acc = wantMax ? std::max(acc, v) : std::min(acc, v);
			}
			tmp[size_t(y)*size_t(mw) + size_t(x)] = uint8_t(acc);
		}
	for (int y = 0; y < mh; ++y)
		for (int x = 0; x < mw; ++x) {
			int acc = wantMax ? 0 : 255;
			for (int dy = -R; dy <= R; ++dy) {
				const int v = tmp[size_t(WrapI(y+dy, mh))*size_t(mw) + size_t(x)];
				acc = wantMax ? std::max(acc, v) : std::min(acc, v);
			}
			dst[size_t(y)*size_t(mw) + size_t(x)] = uint8_t(acc);
		}
}

static void BuildMatGrid(const Material *mat, int mipReq, MatGrid &G)
{
	G = MatGrid();
	if (!mat) return;
	const Texture *hm = mat->HeightMap;
	if (!hm || hm->BPP != 8 || !hm->Mipmap[0]) return;
	int useMip = mipReq;
	if (useMip >= int(hm->numMipmaps)) useMip = int(hm->numMipmaps) - 1;
	if (useMip < 0) useMip = 0;
	while (useMip > 0 &&
	       ((std::max(1, int(hm->SizeX) >> useMip) < (1 << hm->blockSizeX)) ||
	        (std::max(1, int(hm->SizeY) >> useMip) < (1 << hm->blockSizeY))))
		--useMip;
	if (!hm->Mipmap[useMip]) return;
	G.hm = hm; G.useMip = useMip;
	G.mipW = std::max(1, int(hm->SizeX) >> useMip);
	G.mipH = std::max(1, int(hm->SizeY) >> useMip);

	// ── the height field, unswizzled once, plus its two pyramids ───────────
	// mipMean is the plain mean of the mip's bytes — the same number the old
	// bake computes (MeshOps.cpp) and the same one the reference renderer
	// computes (DeferredDisplaceRef.cpp), because the swizzle is a permutation
	// and the mean does not see it.  The guard is the old bake's: a
	// (near-)constant map carries no relief and is left alone.
	{
		const byte *d = hm->Mipmap[useMip];
		const size_t n = size_t(G.mipW) * size_t(G.mipH);
		byte lo = 255, hi = 0; uint64_t sum = 0;
		for (size_t i = 0; i < n; ++i) { const byte b = d[i];
			if (b < lo) lo = b; if (b > hi) hi = b; sum += b; }
		if (hi - lo >= 2) {
			G.mipMean = double(sum) / double(n) * (1.0/255.0);
			G.h0 = double(lo) * (1.0/255.0);
			G.h1 = double(hi) * (1.0/255.0);
			G.fBase.assign(n, 0);
			for (int y = 0; y < G.mipH; ++y)
				for (int x = 0; x < G.mipW; ++x)
					G.fBase[size_t(y)*size_t(G.mipW) + size_t(x)] =
						d[V4SwizzledOffset(x, y, hm->blockSizeX, hm->blockSizeY, G.mipH)];
			const double rad = double(FeatureFlags::v4_pyr_radius_tex());
			G.pyrRad = (FeatureFlags::v4_pyramid() && rad > 0.0)
			         ? int(std::ceil(rad - 1e-9)) : 0;
			PyramidFilter(G.fBase, G.mipW, G.mipH, G.pyrRad, true,  G.fMax);
			PyramidFilter(G.fBase, G.mipW, G.mipH, G.pyrRad, false, G.fMin);
			G.fieldOk = true;
		}
	}

	G.havePitch = EstimateBlockPitch(hm, useMip, G.pitchX, G.pitchY);
	// Target cell texel footprint = block pitch / cpb, the old bake's formula
	// with the old bake's fallback (a sixth of a tile) when there is no grid.
	const double invCpb = 1.0 / std::max(0.25f, FeatureFlags::v4_cpb());
	const double fallback = std::max(2.0, double(G.mipW) * (1.0/6.0) * invCpb);
	G.targetTexX = G.havePitch ? std::max(2.0, invCpb * double(G.pitchX)) : fallback;
	G.targetTexY = G.havePitch ? std::max(2.0, invCpb * double(G.pitchY)) : fallback;
	if (!G.havePitch) return;                      // no mortar grid -> no breaklines
	MeshOps_FindStoneGrooveGrid(hm, useMip, G.pitchX, G.pitchY, G.grid, G.rowTpl, G.colTpl);
	if (!G.grid.valid || G.rowTpl.empty()) { G.grid.valid = false; return; }
	G.y0Base = double(G.rowTpl.front().y0);
	G.colLine.resize(G.grid.vPerBand.size());
	for (size_t b = 0; b < G.grid.vPerBand.size(); ++b) {
		int runId = 0;
		for (const StoneGRun &r : G.grid.vPerBand[b]) {
			const float A = r.lo - kStonePadTex, B = r.lo + kStonePadTex;
			const float C = r.hi - kStonePadTex, D = r.hi + kStonePadTex;
			G.colLine[b].push_back({double(A), runId});
			if (C - B >= kStoneMinFloorTex) {
				G.colLine[b].push_back({double(B), runId});
				G.colLine[b].push_back({double(C), runId});
			} else {
				G.colLine[b].push_back({double(0.5f * (r.lo + r.hi)), runId});
			}
			G.colLine[b].push_back({double(D), runId});
			++runId;
		}
		std::sort(G.colLine[b].begin(), G.colLine[b].end());
	}
	G.ok = true;
}

// Is this map coordinate inside a MORTAR BAND (a groove run plus its shoulder
// pads)?  The sliver census splits on it: a thin triangle running ALONG a
// mortar line is the legitimate kind (Dyn, Levin & Rippa 1990, quoted in the
// survey §D — "long and thin triangles, which are traditionally avoided, are
// sometimes very suitable"), while a thin triangle in the middle of a block
// face is a defect.  Reporting one number for both hides which one you have.
static bool InGrooveBand(const MatGrid &G, double u, double v)
{
	if (!G.ok) return false;
	const double pad = double(kStonePadTex);
	auto wrap = [](double x, double p) { double r = std::fmod(x, p); if (r < 0) r += p; return r; };
	const double y = wrap(v * double(G.mipH), double(G.mipH));
	for (const StoneGRun &r : G.grid.h) {
		const double lo = wrap(double(r.lo) - pad, double(G.mipH));
		const double w  = double(r.hi - r.lo) + 2.0*pad;
		if (wrap(y - lo, double(G.mipH)) <= w) return true;
	}
	int band = 0;
	for (size_t b = 0; b < G.grid.bandY.size(); ++b) {
		const double a0 = wrap(double(G.grid.bandY[b].first), double(G.mipH));
		const double w  = double(G.grid.bandY[b].second - G.grid.bandY[b].first);
		if (wrap(y - a0, double(G.mipH)) <= w) { band = int(b); break; }
	}
	if (size_t(band) >= G.grid.vPerBand.size()) return false;
	const double x = wrap(u * double(G.mipW), double(G.mipW));
	for (const StoneGRun &r : G.grid.vPerBand[size_t(band)]) {
		const double lo = wrap(double(r.lo) - pad, double(G.mipW));
		const double w  = double(r.hi - r.lo) + 2.0*pad;
		if (wrap(x - lo, double(G.mipW)) <= w) return true;
	}
	return false;
}

// ── the RELIEF CLASS of a map coordinate (design §2d) ──────────────────────
// The three classes are the mortar grid's own, not a threshold on the field:
//   GROOVE   inside a mortar RUN — the flat floor of the joint;
//   BEVEL    inside the run's shoulder PAD — the ramp the bake mip's blur makes
//            out of what the level-0 map draws as a step;
//   PLATEAU  everything else — the flat face of a block.
// This is exactly the partition InGrooveBand() answers as one bit, split in
// two, and it runs over the identical wrap arithmetic so the two can never
// disagree about where a band is.
enum { kRelGroove = 0, kRelBevel = 1, kRelPlateau = 2 };

static int ReliefClassAt(const MatGrid &G, double u, double v)
{
	if (!G.ok) return kRelPlateau;
	const double pad = double(kStonePadTex);
	auto wrap = [](double x, double p) { double r = std::fmod(x, p); if (r < 0) r += p; return r; };
	// One run's class at a wrapped coordinate.  The half-open convention at the
	// two OUTER pad lines is load-bearing and was measured, not assumed: at
	// cpb=1 the block interior gets no interior line at all (target cell = the
	// block PITCH, so ceil(pitch/target) = 1 split), which means EVERY lattice
	// node in this scene sits exactly on a breakline — the outer pad lines
	// lo−pad / hi+pad and the inner pad lines lo+pad / hi−pad.  Calling the
	// OUTER lines bevel left the census with plateau=0 nodes and the block face
	// spanned shoulder-to-shoulder at the blur's down-slope value: the −0.036 u
	// recession of d8e1d26bfc3e, reproduced exactly.  The outer pad line IS the
	// top of the shoulder, so it is a PLATEAU node and reads the max-pyramid;
	// the inner pad line is the bottom, so it is a GROOVE node and reads the
	// min; the ramp between them is the bevel.  ε is in TEXELS and 6 orders
	// below the 1.25-texel pad, so it only absorbs the fmod's last bit.
	const double eps = 1e-6;
	auto runClass = [&](double c, double lo, double hi, double period) -> int {
		const double a = wrap(lo - pad, period);
		const double w = (hi - lo) + 2.0*pad;
		const double t = wrap(c - a, period);
		if (t < eps || t > w - eps) return kRelPlateau;
		return (t >= pad - eps && t <= w - pad + eps) ? kRelGroove : kRelBevel;
	};
	int cls = kRelPlateau;
	const double y = wrap(v * double(G.mipH), double(G.mipH));
	for (const StoneGRun &r : G.grid.h)
		cls = std::min(cls, runClass(y, double(r.lo), double(r.hi), double(G.mipH)));
	int band = 0;
	for (size_t b = 0; b < G.grid.bandY.size(); ++b) {
		const double a0 = wrap(double(G.grid.bandY[b].first), double(G.mipH));
		const double w  = double(G.grid.bandY[b].second - G.grid.bandY[b].first);
		if (wrap(y - a0, double(G.mipH)) <= w) { band = int(b); break; }
	}
	if (size_t(band) < G.grid.vPerBand.size()) {
		const double x = wrap(u * double(G.mipW), double(G.mipW));
		for (const StoneGRun &r : G.grid.vPerBand[size_t(band)])
			cls = std::min(cls, runClass(x, double(r.lo), double(r.hi), double(G.mipW)));
	}
	return cls;
}

// The FIELD's own height at a map coordinate — the bilinear reconstruction the
// reference renderer marches.  Every r-value of the [V4-RELIEF] census is this.
static inline double FieldH(const MatGrid &G, double u, double v)
{
	return G.fieldOk ? SampleField(G.fBase, G.mipW, G.mipH, u, v) : G.mipMean;
}

// The height a NODE of relief class `cls` takes (design §2d): plateau from the
// max-pyramid, groove from the min-pyramid, bevel from the field itself.  With
// --no-v4_pyramid the two pyramids are copies of the field and this collapses
// to the bilinear rule for every class, which is the isolating arm.
static inline double NodeH(const MatGrid &G, double u, double v, int cls)
{
	if (!G.fieldOk) return G.mipMean;
	if (cls == kRelPlateau) return SampleField(G.fMax, G.mipW, G.mipH, u, v);
	if (cls == kRelGroove)  return SampleField(G.fMin, G.mipW, G.mipH, u, v);
	return SampleField(G.fBase, G.mipW, G.mipH, u, v);
}

// The target cell footprint in WORLD units for one face — the same expression
// FaceLattice sizes its interior density with, factored out so the [V4-RELIEF]
// census can state its CORE band ("more than one cell from an authored edge")
// in the lattice's own unit.  Returns 0 when the face has no usable UV map.
static double FaceCellW(const P2Face &F, const MatGrid &G)
{
	if (!G.ok) return 0.0;
	const double u0 = F.uv[0][0], v0 = F.uv[0][1];
	const double du1 = F.uv[1][0]-u0, dv1 = F.uv[1][1]-v0;
	const double du2 = F.uv[2][0]-u0, dv2 = F.uv[2][1]-v0;
	const double det = du1*dv2 - du2*dv1;
	const double uvScale = std::fabs(du1)+std::fabs(dv1)+std::fabs(du2)+std::fabs(dv2);
	if (!(uvScale > 1e-12) || !(std::fabs(det) > 1e-9 * uvScale * uvScale)) return 0.0;
	const V3 e1 = F.p[1] - F.p[0], e2 = F.p[2] - F.p[0];
	const V3 Tu = (e1 * dv2 - e2 * dv1) * (1.0/det);
	const V3 Bv = (e2 * du1 - e1 * du2) * (1.0/det);
	return std::min(G.targetTexX * len(Tu) / double(G.mipW),
	                G.targetTexY * len(Bv) / double(G.mipH));
}

// ── Delaunay (Bowyer-Watson with adjacency + a straight walk) ──────────────
// The domain is a TRIANGLE, so it is convex and the point set's convex hull is
// exactly the authored face: every consecutive pair of boundary points is a
// hull edge and therefore a Delaunay edge, which is what makes the boundary
// (the edge-owned samples of R1/R2/R3) survive the triangulation untouched.
// Interior grid nodes enter as Steiner points.
struct DTri { int32_t v[3]; int32_t n[3]; bool dead = false; };
struct Delaunay {
	std::vector<double> X, Y;
	std::vector<DTri>   t;
	int32_t last = 0;

	static double orient(double ax,double ay,double bx,double by,double cx,double cy) {
		return (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
	}
	double inCircle(int32_t a,int32_t b,int32_t c,int32_t d) const {
		const double adx = X[size_t(a)]-X[size_t(d)], ady = Y[size_t(a)]-Y[size_t(d)];
		const double bdx = X[size_t(b)]-X[size_t(d)], bdy = Y[size_t(b)]-Y[size_t(d)];
		const double cdx = X[size_t(c)]-X[size_t(d)], cdy = Y[size_t(c)]-Y[size_t(d)];
		const double ad = adx*adx + ady*ady, bd = bdx*bdx + bdy*bdy, cd = cdx*cdx + cdy*cdy;
		return adx*(bdy*cd - bd*cdy) - ady*(bdx*cd - bd*cdx) + ad*(bdx*cdy - bdy*cdx);
	}
	double orient(int32_t a,int32_t b,int32_t c) const {
		return orient(X[size_t(a)],Y[size_t(a)],X[size_t(b)],Y[size_t(b)],X[size_t(c)],Y[size_t(c)]);
	}
	void setNbr(int32_t ti, int32_t e, int32_t other) { if (ti >= 0) t[size_t(ti)].n[e] = other; }
	// edge e of triangle ti runs v[e] -> v[(e+1)%3]
	int32_t edgeOf(int32_t ti, int32_t a, int32_t b) const {
		for (int e = 0; e < 3; ++e)
			if (t[size_t(ti)].v[e] == a && t[size_t(ti)].v[(e+1)%3] == b) return e;
		return -1;
	}
	int32_t locate(int32_t p) {
		if (t.empty()) return -1;
		int32_t ti = last;
		if (t[size_t(ti)].dead) { ti = -1; for (size_t i = 0; i < t.size(); ++i) if (!t[i].dead) { ti = int32_t(i); break; } }
		if (ti < 0) return -1;
		for (int step = 0; step < 4 * int(t.size()) + 64; ++step) {
			const DTri &tr = t[size_t(ti)];
			int32_t next = -1;
			for (int e = 0; e < 3; ++e) {
				if (orient(tr.v[e], tr.v[(e+1)%3], p) < 0.0) { next = tr.n[e]; break; }
			}
			if (next < 0) { last = ti; return ti; }
			if (t[size_t(next)].dead) break;
			ti = next;
		}
		for (size_t i = 0; i < t.size(); ++i) {           // walk failed: brute force
			if (t[i].dead) continue;
			const DTri &tr = t[i];
			if (orient(tr.v[0],tr.v[1],p) >= 0 && orient(tr.v[1],tr.v[2],p) >= 0 &&
			    orient(tr.v[2],tr.v[0],p) >= 0) { last = int32_t(i); return int32_t(i); }
		}
		return -1;
	}
	bool insert(int32_t p) {
		const int32_t seed = locate(p);
		if (seed < 0) return false;
		// cavity = every triangle reachable from `seed` whose circumcircle
		// strictly contains p
		std::vector<int32_t> cav;
		std::vector<int32_t> stack{ seed };
		std::vector<char> inCav(t.size(), 0);
		inCav[size_t(seed)] = 1; cav.push_back(seed);
		while (!stack.empty()) {
			const int32_t ti = stack.back(); stack.pop_back();
			for (int e = 0; e < 3; ++e) {
				const int32_t nb = t[size_t(ti)].n[e];
				if (nb < 0 || inCav[size_t(nb)] || t[size_t(nb)].dead) continue;
				if (inCircle(t[size_t(nb)].v[0], t[size_t(nb)].v[1], t[size_t(nb)].v[2], p) > 0.0) {
					inCav[size_t(nb)] = 1; cav.push_back(nb); stack.push_back(nb);
				}
			}
		}
		// boundary of the cavity, in order-independent form
		struct BE { int32_t a, b, out; };
		std::vector<BE> bnd;
		for (int32_t ti : cav)
			for (int e = 0; e < 3; ++e) {
				const int32_t nb = t[size_t(ti)].n[e];
				if (nb >= 0 && inCav[size_t(nb)]) continue;
				bnd.push_back({ t[size_t(ti)].v[e], t[size_t(ti)].v[(e+1)%3], nb });
			}
		if (bnd.size() < 3) return false;
		for (int32_t ti : cav) t[size_t(ti)].dead = true;
		const size_t base = t.size();
		for (const BE &b : bnd) {
			DTri nt; nt.v[0] = b.a; nt.v[1] = b.b; nt.v[2] = p;
			nt.n[0] = b.out; nt.n[1] = -1; nt.n[2] = -1;
			t.push_back(nt);
		}
		// re-link
		for (size_t i = 0; i < bnd.size(); ++i) {
			const int32_t ti = int32_t(base + i);
			if (bnd[i].out >= 0) {
				const int32_t e = edgeOf(bnd[i].out, bnd[i].b, bnd[i].a);
				if (e >= 0) t[size_t(bnd[i].out)].n[e] = ti;
			}
			for (size_t j = 0; j < bnd.size(); ++j) {
				if (i == j) continue;
				const int32_t tj = int32_t(base + j);
				if (bnd[j].a == bnd[i].b) t[size_t(ti)].n[1] = tj;      // (b,p)
				if (bnd[j].b == bnd[i].a) t[size_t(ti)].n[2] = tj;      // (p,a)
			}
		}
		last = int32_t(base);
		return true;
	}
};

// ── census accumulators ────────────────────────────────────────────────────
struct P2Stats {
	int64_t nodesGen = 0, nodesKept = 0, nodesMargin = 0, nodesOutside = 0, nodesCapped = 0;
	int64_t borderSamples = 0, borderCapped = 0;
	int32_t borderMin = 1 << 30, borderMax = 0;
	double  borderMaxDev = 0.0;              // max |sample - exact line| in world units
	int64_t degenerateUV = 0, delaunayFail = 0, fanFallback = 0;
	int64_t rowCapFaces = 0, nyCap = 0, nxCap = 0;
	double  msRelief = 0;   // AccumRelief, which runs inside the lattice loop
	double  minNodeSpacing = 1e30, maxNodeSpacing = 0;
	int64_t triEmitted = 0;
	double  plateauMinLevel0 = 1e30;         // min level-0 texels from a node to a block edge
	int64_t levelJumpViolations = 0;
	double  msTopo = 0, msGrid = 0, msLattice = 0, msCommit = 0, msCensus = 0;
};

// One face's lattice.  Returns the triangles as index triples into `pt`.
struct LatPt { double x2 = 0, y2 = 0, u = 0, v = 0; V3 p; int32_t gid = -1;
               double d = 0.0; };   // d = the P3 displacement along the chart normal

// Breaklines closer than this many texels at the bake mip are merged (see the
// note at the merge site).  One texel is the field's own resolution.
static constexpr double kLineMergeTex = 1.0;

// ═══ [V4-RELIEF] — design §2d's invariant, measured on the mesh ════════════
// The instrument the design names for this is tools/nspace_relief.py, which
// reads a --refplane_dump raw dump: that flag lives on rev-dispfix and is not
// on this branch, and porting a render-path dump to take a bake number would be
// the wrong trade (trap 2: instrumentation out of the shipping paths).  What
// nspace_relief measures is e−r ALONG THE PLANE NORMAL per relief class, and on
// a v4 face that quantity is available exactly, with no camera and no grazing
// stretch, because the whole face displaces along ONE direction:
//
//   e(x)  the SURFACE's own height — the linear interpolation of the three
//         vertices' displacements over the triangle, which IS the signed
//         distance from the authored plane since all three moved along the same
//         normal.  The recession the −0.036 u finding named lives BETWEEN the
//         nodes (its own per-vertex census read 0), so a per-vertex number
//         would not see it and this one samples the surface.
//   r(x)  the FIELD's own d(u,v) = amp*(h − mipMean) at the same UV, read
//         through this file's own copy of the swizzle and its own bilinear.
//
// Classes and thresholds are nspace_relief's (groove r < −0.03, bevel −0.03..0,
// plateau r ≥ 0) so the numbers are comparable to the ones it produced.
struct ReliefBin {
	std::vector<float> emr;        // e − r over every sample
	std::vector<float> r;          // the field's own d, for the "ref med" column
	std::vector<float> emrCore;    // e − r, samples > one target cell from an
	                               // authored edge: the band P3's height rule
	                               // owns, with P4's pinned rings excluded
};
struct ReliefCensus {
	std::map<uint32_t, std::array<ReliefBin,3>> byPlane;   // (matIdx<<16)|planeOrd
	int64_t samples = 0, coreSamples = 0;
	// r-class (rows) x GEOMETRIC class (cols): where the disagreement lives.
	std::vector<float> cross[3][3];
	struct Worst { float emr, e, r, u, v; int32_t face; uint8_t rc, gc;
	                float tu[3], tv[3], td[3], th[3]; uint8_t tc[3]; };
	std::vector<Worst> worst;
};

// The m² equal-area barycentric sample points of one triangle (the centroids of
// a uniform order-m subdivision: m(m+1)/2 upward plus m(m−1)/2 downward
// sub-triangles, all of area A/m²), so an unweighted percentile over the sample
// set IS the area-weighted percentile over the surface.
static const int kReliefOrder = 3;

static void AccumRelief(const P2Face &F, const MatGrid &G, double amp,
                        const std::vector<LatPt> &pt,
                        const std::vector<std::array<int32_t,3>> &tri,
                        double cellW, ReliefCensus &RC)
{
	if (!G.fieldOk) return;
	const double u0 = F.uv[0][0], v0 = F.uv[0][1];
	const double du1 = F.uv[1][0]-u0, dv1 = F.uv[1][1]-v0;
	const double du2 = F.uv[2][0]-u0, dv2 = F.uv[2][1]-v0;
	const double det = du1*dv2 - du2*dv1;
	if (!(std::fabs(det) > 1e-18)) return;
	const double L12 = len(F.p[2] - F.p[1]);
	const double L20 = len(F.p[0] - F.p[2]);
	const double L01 = len(F.p[1] - F.p[0]);
	const double twoA = 2.0 * F.area;
	const uint32_t key = (uint32_t(F.matIdx) << 16) | uint32_t(F.planeOrd);
	auto &bins = RC.byPlane[key];

	const int m = kReliefOrder;
	const double im = 1.0 / (3.0 * double(m));
	double W[16][2];
	int nW = 0;
	for (int i = 0; i < m; ++i) for (int j = 0; j + i < m; ++j)
		{ W[nW][0] = double(3*i+1)*im; W[nW][1] = double(3*j+1)*im; ++nW; }
	for (int i = 0; i < m-1; ++i) for (int j = 0; j + i < m-1; ++j)
		{ W[nW][0] = double(3*i+2)*im; W[nW][1] = double(3*j+2)*im; ++nW; }

	for (const auto &tr : tri) {
		const LatPt &A = pt[size_t(tr[0])], &B = pt[size_t(tr[1])], &C = pt[size_t(tr[2])];
		for (int q = 0; q < nW; ++q) {
			const double w1 = W[q][0], w2 = W[q][1], w0 = 1.0 - w1 - w2;
			const double e = w0*A.d + w1*B.d + w2*C.d;
			const double u = w0*A.u + w1*B.u + w2*C.u;
			const double v = w0*A.v + w1*B.v + w2*C.v;
			const double r = amp * (FieldH(G, u, v) - G.mipMean);
			// the sample's barycentric in the AUTHORED face -> its distance to
			// the nearest authored edge, in world units
			const double dU = u - u0, dV = v - v0;
			const double fb1 = ( dU*dv2 - dV*du2) / det;
			const double fb2 = ( du1*dV - dv1*dU) / det;
			const double fb0 = 1.0 - fb1 - fb2;
			const double dEdge = std::min(std::min(twoA*fb0/std::max(1e-12,L12),
			                                       twoA*fb1/std::max(1e-12,L20)),
			                              twoA*fb2/std::max(1e-12,L01));
			const int cls = (r < -0.03) ? kRelGroove : (r < 0.0 ? kRelBevel : kRelPlateau);
			const int gcl = ReliefClassAt(G, u, v);
			RC.cross[size_t(cls)][size_t(gcl)].push_back(float(e - r));
			if (std::fabs(e - r) > 0.05)
				RC.worst.push_back({ float(e-r), float(e), float(r), float(u), float(v),
				                     F.faceIdx, uint8_t(cls), uint8_t(gcl),
				                     { float(A.u), float(B.u), float(C.u) },
				                     { float(A.v), float(B.v), float(C.v) },
				                     { float(A.d), float(B.d), float(C.d) },
				                     { float(FieldH(G,A.u,A.v)), float(FieldH(G,B.u,B.v)),
				                       float(FieldH(G,C.u,C.v)) },
				                     { uint8_t(ReliefClassAt(G,A.u,A.v)),
				                       uint8_t(ReliefClassAt(G,B.u,B.v)),
				                       uint8_t(ReliefClassAt(G,C.u,C.v)) } });
			ReliefBin &bn = bins[size_t(cls)];
			bn.emr.push_back(float(e - r));
			bn.r.push_back(float(r));
			++RC.samples;
			if (cellW > 0.0 && dEdge > cellW) {
				bn.emrCore.push_back(float(e - r));
				++RC.coreSamples;
			}
		}
	}
}

static void FaceLattice(const P2Topo &T, const P2Face &F, const MatGrid &G,
                        const std::vector<LatPt> &boundary,
                        const V3 &ex, const V3 &ey,
                        int grooveRefine, bool flat, P2Stats &S,
                        std::vector<LatPt> &pt, std::vector<std::array<int32_t,3>> &tri)
{
	(void)T;
	pt = boundary;
	tri.clear();

	// ── the face's own affine UV map (exact on a triangle) ─────────────────
	// The grid lines below live in the HEIGHT MAP's own texel space, so two
	// faces of one chart see the same breaklines without either of them owning
	// a chart-local frame; each face converts a map coordinate to a position
	// through its own barycentric solve, which keeps every emitted point an
	// exact convex combination of the authored corners (= exactly on the
	// authored plane, which is what "amp = 0 ⇒ the same planes" needs).
	const double u0 = F.uv[0][0], v0 = F.uv[0][1];
	const double du1 = F.uv[1][0]-u0, dv1 = F.uv[1][1]-v0;
	const double du2 = F.uv[2][0]-u0, dv2 = F.uv[2][1]-v0;
	const double det = du1*dv2 - du2*dv1;
	const double uvScale = std::fabs(du1)+std::fabs(dv1)+std::fabs(du2)+std::fabs(dv2);
	const bool   uvOk = !flat && G.ok && uvScale > 1e-12 && std::fabs(det) > 1e-9 * uvScale * uvScale;
	if (!flat && G.ok && !uvOk) ++S.degenerateUV;

	if (uvOk) {
		const V3 e1 = F.p[1] - F.p[0], e2 = F.p[2] - F.p[0];
		const V3 Tu = (e1 * dv2 - e2 * dv1) * (1.0/det);      // world per unit u
		const V3 Bv = (e2 * du1 - e1 * du2) * (1.0/det);      // world per unit v
		const double wTexX = len(Tu) / double(G.mipW);
		const double wTexY = len(Bv) / double(G.mipH);
		const double cellW = std::min(G.targetTexX * wTexX, G.targetTexY * wTexY);
		S.minNodeSpacing = std::min(S.minNodeSpacing, cellW);
		S.maxNodeSpacing = std::max(S.maxNodeSpacing, cellW);
		const double margin = 0.25 * cellW / double(1 << grooveRefine);

		double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
		for (int k = 0; k < 3; ++k) {
			const double x = F.uv[k][0] * double(G.mipW), y = F.uv[k][1] * double(G.mipH);
			xmin = std::min(xmin, x); xmax = std::max(xmax, x);
			ymin = std::min(ymin, y); ymax = std::max(ymax, y);
		}
		const double perH = double(G.mipH), perW = double(G.mipW);

		// ── ROWS: the row template instanced over the face's v footprint, each
		// row split to the interior density (plateau) or by the groove
		// refinement (step/floor).  Rows are lines through the WHOLE face, so a
		// density change between two rows produces no hanging node at all —
		// the restricted-quadtree ≤1-level rule holds by construction, and the
		// census below records any row pair that breaks it anyway. ─────────
		struct RowI { double y0, y1; int type, bandA, bandB; int lvl; };
		std::vector<RowI> rows;
		const long k0 = long(std::floor((ymin - perH - G.y0Base) / perH));
		const long k1 = long(std::ceil ((ymax + perH - G.y0Base) / perH));
		bool capped = false;
		for (long k = k0; k <= k1 && !capped; ++k)
			for (const StoneRowT &r : G.rowTpl) {
				const double a = double(r.y0) + double(k) * perH;
				const double b = double(r.y1) + double(k) * perH;
				if (b <= ymin - perH || a >= ymax + perH) continue;
				int ny;
				if (r.type == 0) ny = std::max(1, int(std::ceil((b - a) / G.targetTexY)));
				else             ny = 1 << grooveRefine;
				if (ny > 64) { ny = 64; ++S.nyCap; }
				int lvl = 0; while ((1 << lvl) < ny) ++lvl;
				for (int i = 0; i < ny; ++i)
					rows.push_back({ a + (b-a)*double(i)/double(ny),
					                 a + (b-a)*double(i+1)/double(ny),
					                 r.type, r.bandA, r.bandB, lvl });
				if (rows.size() > 8192) { capped = true; ++S.rowCapFaces; break; }
			}
		std::sort(rows.begin(), rows.end(), [](const RowI &a, const RowI &b){ return a.y0 < b.y0; });
		for (size_t j = 1; j < rows.size(); ++j)
			if (std::abs(rows[j].lvl - rows[j-1].lvl) > 1) ++S.levelJumpViolations;

		// the mortar RUN edges (not the pad lines) — the block boundaries the
		// plateau-node rule is measured against (design §2c, d8e1d26bfc3e)
		std::vector<double> grooveY;
		for (long k = k0; k <= k1; ++k)
			for (const StoneGRun &r : G.grid.h) {
				grooveY.push_back(double(r.lo) + double(k)*perH);
				grooveY.push_back(double(r.hi) + double(k)*perH);
			}
		std::sort(grooveY.begin(), grooveY.end());

		// ── COLUMNS per row ────────────────────────────────────────────────
		const long j0 = long(std::floor((xmin - perW) / perW));
		const long j1 = long(std::ceil ((xmax + perW) / perW));
		struct ColSet { std::vector<double> lines, runEdge; };
		std::map<std::pair<int,int>, ColSet> colCache;
		const bool allBands = FeatureFlags::v4_band_union();
		const bool blockMid = FeatureFlags::v4_block_mid();
		auto colsFor = [&](int bA, int bB) -> const ColSet & {
			// The cache key is the row's OWN band pair in both arms: under
			// --v4_band_union a foreign band no longer contributes the same
			// thing as an own one (see the CENTRE rule below), so the key may
			// not collapse to one entry per face the way P3's first cut did.
			auto key = std::make_pair(std::min(bA,bB), std::max(bA,bB));
			auto it = colCache.find(key);
			if (it != colCache.end()) return it->second;
			std::vector<std::pair<double,int64_t>> raw;
			ColSet cs;
			int bands[2] = { key.first, key.second };
			const int nb = (key.first == key.second) ? 1 : 2;
			// The LINE set: this row's own two bands, or — under --v4_band_union
			// — every band's.  Measured reason for the option (§P3 log): the
			// mortar of a RUNNING BOND sits at the middle of the band above it,
			// so at cpb=1, where a block interior carries no line of its own,
			// the union ROW at a band change injects the neighbouring band's
			// mortar nodes and the row below has nothing under them.  Delaunay
			// then spans the whole block with a triangle whose three corners are
			// all mortar-deep, and the block face bakes 0.08 u recessed.
			// Unioning the bands gives every block a mid-block line that is
			// classified PLATEAU there and reads the max-pyramid, which is the
			// node the block interior was missing.
			//
			// A FOREIGN band contributes ONE line per run — the run's CENTRE —
			// and never its shoulder profile.  In a running bond that centre IS
			// the centre of this row's block, so it is exactly the plateau node
			// the interior was missing; lo±pad / hi±pad describe a groove that
			// does not exist in this row, and injecting them put THREE lines and
			// two 2.5-texel cells into every block interior instead of one node.
			// Measured cost of the profile form (P3b): 124 716 stone faces and,
			// in the razor-thin mortar strips, twice the near-camera shared
			// edges — where the rasterizer's sub-pixel hairline at an
			// exactly-shared edge leaks single background pixels (the 5 INTERIOR
			// pinholes that reverted the flag in f53dc4d6; a 0.002 u camera
			// jitter moves them, so they are alignment-sensitive leaks at the
			// edge, not a gap in the mesh — the mesh is use-2 everywhere with
			// 0 interior T-vertices in both forms).
			const size_t nbLine = allBands ? G.colLine.size() : size_t(nb);
			for (size_t q = 0; q < nbLine; ++q) {
				const int bb = allBands ? int(q) : bands[q];
				if (bb < 0 || size_t(bb) >= G.colLine.size()) continue;
				const bool own = (bb == bands[0]) || (nb == 2 && bb == bands[1]);
				if (allBands && !own) {
					if (size_t(bb) >= G.grid.vPerBand.size()) continue;
					int runId = 0;
					for (const StoneGRun &r : G.grid.vPerBand[size_t(bb)]) {
						for (long j = j0; j <= j1; ++j) {
							const double x = 0.5*(double(r.lo) + double(r.hi))
							               + double(j) * perW;
							if (x < xmin - perW || x > xmax + perW) continue;
							raw.push_back({ x, (int64_t(bb) << 44)
							                 | (int64_t(runId) << 24)
							                 | int64_t(j + (1 << 23)) });
						}
						++runId;
					}
					continue;
				}
				for (long j = j0; j <= j1; ++j)
					for (const auto &cl : G.colLine[size_t(bb)]) {
						const double x = cl.first + double(j) * perW;
						if (x < xmin - perW || x > xmax + perW) continue;
						const int64_t rk = (int64_t(bb) << 44) | (int64_t(cl.second) << 24)
						                 | int64_t(j + (1 << 23));
						raw.push_back({ x, rk });
					}
			}
			// runEdge stays the ROW's OWN bands: it feeds the plateau-node
			// distance census, which asks how far a node is from the mortar it
			// is actually next to.
			for (int q = 0; q < nb; ++q) {
				const int bb = bands[q];
				if (bb < 0 || size_t(bb) >= G.grid.vPerBand.size()) continue;
				for (long j = j0; j <= j1; ++j)
					for (const StoneGRun &r : G.grid.vPerBand[size_t(bb)]) {
						cs.runEdge.push_back(double(r.lo) + double(j)*perW);
						cs.runEdge.push_back(double(r.hi) + double(j)*perW);
					}
			}
			std::sort(raw.begin(), raw.end());
			std::sort(cs.runEdge.begin(), cs.runEdge.end());
			// Two breaklines closer than ONE TEXEL at the bake mip are the same
			// line as far as the height field can tell, and where two bands of a
			// running bond interleave they land arbitrarily close: keeping both
			// makes hair-thin cells (measured 0.029 u wide against a 1.5 u target
			// cell) whose triangles are the sub-1° family.  Merge them.
			std::vector<std::pair<double,int64_t>> base;
			for (const auto &e : raw)
				if (base.empty() || e.first - base.back().first > kLineMergeTex) base.push_back(e);
			for (size_t i = 0; i < base.size(); ++i) {
				if (i) {
					const double a = base[i-1].first, b = base[i].first;
					const bool sameRun = (base[i-1].second == base[i].second);
					// --v4_block_mid: a span between two DIFFERENT runs is a block
					// interior, and at cpb=1 it takes no line of its own (the target
					// cell IS the block pitch).  Splitting it in two puts the node
					// there, at the midpoint of the host band's own two bounding
					// shoulders — equidistant from the two recesses by construction,
					// which is the property --v4_band_union's borrowed lines do not
					// have on this map (see the flag's doc and the [V4-GRID] census).
					const int minCells = (blockMid && !sameRun && (b - a) > 5.0) ? 2 : 1;
					int nx = sameRun ? (1 << grooveRefine)
					                 : std::max(minCells, int(std::ceil((b - a) / G.targetTexX)));
					if (nx > 64) { nx = 64; ++S.nxCap; }
					for (int q2 = 1; q2 < nx; ++q2)
						cs.lines.push_back(a + (b-a)*double(q2)/double(nx));
				}
				cs.lines.push_back(base[i].first);
			}
			return colCache.emplace(key, std::move(cs)).first->second;
		};

		auto nearest = [](const std::vector<double> &v, double x) -> double {
			if (v.empty()) return 1e300;
			auto it = std::lower_bound(v.begin(), v.end(), x);
			double d = 1e300;
			if (it != v.end())   d = std::min(d, std::fabs(*it - x));
			if (it != v.begin()) d = std::min(d, std::fabs(*(it-1) - x));
			return d;
		};
		auto baryOf = [&](double u, double v, double &b0, double &b1, double &b2) {
			const double dU = u - u0, dV = v - v0;
			b1 = ( dU*dv2 - dV*du2) / det;
			b2 = ( du1*dV - dv1*dU) / det;
			b0 = 1.0 - b1 - b2;
		};
		const double L12 = len(F.p[2] - F.p[1]);
		const double L20 = len(F.p[0] - F.p[2]);
		const double L01 = len(F.p[1] - F.p[0]);
		const double twoA = 2.0 * F.area;
		const double lvl0 = double(1 << G.useMip);
		std::vector<double> merged;
		for (size_t j = 0; j <= rows.size() && !rows.empty(); ++j) {
			const double y = (j == 0) ? rows.front().y0 : rows[j-1].y1;
			if (y < ymin || y > ymax) continue;
			merged.clear();
			const ColSet *csA = nullptr, *csB = nullptr;
			if (j > 0)           { csA = &colsFor(rows[j-1].bandA, rows[j-1].bandB); }
			if (j < rows.size()) { csB = &colsFor(rows[j].bandA,   rows[j].bandB);   }
			if (csA) merged.insert(merged.end(), csA->lines.begin(), csA->lines.end());
			if (csB) merged.insert(merged.end(), csB->lines.begin(), csB->lines.end());
			std::sort(merged.begin(), merged.end());
			const double dYrun = nearest(grooveY, y);
			double lastX = -1e300;
			for (size_t q = 0; q < merged.size(); ++q) {
				if (merged[q] - lastX < kLineMergeTex) continue;
				lastX = merged[q];
				const double x = merged[q];
				if (x < xmin || x > xmax) continue;
				++S.nodesGen;
				if (S.nodesGen > 4000000) { ++S.nodesCapped; continue; }
				double b0, b1, b2;
				baryOf(x / perW, y / perH, b0, b1, b2);
				if (b0 <= 0 || b1 <= 0 || b2 <= 0) { ++S.nodesOutside; continue; }
				if (twoA * b0 < margin * L12 || twoA * b1 < margin * L20 ||
				    twoA * b2 < margin * L01) { ++S.nodesMargin; continue; }
				LatPt P;
				P.p = F.p[0]*b0 + F.p[1]*b1 + F.p[2]*b2;
				P.u = x / perW; P.v = y / perH;
				P.x2 = dot(P.p - F.p[0], ex); P.y2 = dot(P.p - F.p[0], ey);
				pt.push_back(P);
				++S.nodesKept;
				double dXrun = 1e300;
				if (csA) dXrun = std::min(dXrun, nearest(csA->runEdge, x));
				if (csB) dXrun = std::min(dXrun, nearest(csB->runEdge, x));
				const double dRun = std::min(dXrun, dYrun);
				// a PLATEAU node is one outside every mortar band; nodes inside a
				// band are the groove nodes and the rule does not apply to them
				if (dRun > double(kStonePadTex) - 1e-3 && dRun < 1e299)
					S.plateauMinLevel0 = std::min(S.plateauMinLevel0, dRun * lvl0);
			}
		}
	}

	// ── triangulate ────────────────────────────────────────────────────────
	const size_t nB = boundary.size();
	Delaunay D;
	D.X.reserve(pt.size() + 3); D.Y.reserve(pt.size() + 3);
	for (const LatPt &P : pt) { D.X.push_back(P.x2); D.Y.push_back(P.y2); }
	double lo = 1e300, hi = -1e300;
	for (size_t i = 0; i < pt.size(); ++i) {
		lo = std::min(lo, std::min(D.X[i], D.Y[i]));
		hi = std::max(hi, std::max(D.X[i], D.Y[i]));
	}
	const double cc = 0.5*(lo+hi), rr = std::max(1e-3, 20.0*(hi-lo) + 1.0);
	const int32_t s0 = int32_t(D.X.size());
	D.X.push_back(cc - rr);   D.Y.push_back(cc - rr);
	D.X.push_back(cc + 3*rr); D.Y.push_back(cc - rr);
	D.X.push_back(cc - rr);   D.Y.push_back(cc + 3*rr);
	DTri st; st.v[0]=s0; st.v[1]=s0+1; st.v[2]=s0+2; st.n[0]=st.n[1]=st.n[2]=-1;
	D.t.push_back(st);
	bool ok = true;
	for (size_t i = 0; i < pt.size() && ok; ++i) ok = D.insert(int32_t(i));
	if (ok) {
		double areaSum = 0.0;
		for (const DTri &tr : D.t) {
			if (tr.dead) continue;
			if (tr.v[0] >= s0 || tr.v[1] >= s0 || tr.v[2] >= s0) continue;
			const double a = D.orient(tr.v[0], tr.v[1], tr.v[2]);
			if (a <= 0.0) { ok = false; break; }
			areaSum += 0.5 * a;
			tri.push_back({ tr.v[0], tr.v[1], tr.v[2] });
		}
		if (ok && std::fabs(areaSum - F.area) > 1e-6 * std::max(1.0, F.area)) ok = false;
	}
	if (!ok) {
		// Fall back to a CENTROID FAN over the boundary polygon only.  The
		// boundary is what watertightness is about (its samples are the shared
		// ones), so the fallback costs relief resolution, never the seal.  The
		// centroid is used rather than corner 0 because a corner is COLLINEAR
		// with the samples on its own two edges and would emit zero-area
		// triangles there.
		++S.delaunayFail;
		tri.clear();
		pt.resize(nB);
		LatPt C;
		double b0 = 1.0/3.0, b1 = 1.0/3.0, b2 = 1.0/3.0;
		C.p = F.p[0]*b0 + F.p[1]*b1 + F.p[2]*b2;
		C.u = (F.uv[0][0]+F.uv[1][0]+F.uv[2][0])/3.0;
		C.v = (F.uv[0][1]+F.uv[1][1]+F.uv[2][1])/3.0;
		C.x2 = dot(C.p - F.p[0], ex); C.y2 = dot(C.p - F.p[0], ey);
		const int32_t ci = int32_t(pt.size());
		pt.push_back(C);
		for (size_t i = 0; i < nB; ++i)
			tri.push_back({ ci, int32_t(i), int32_t((i+1) % nB) });
	}
	S.triEmitted += int64_t(tri.size());
}

// ═══ P4 (design §2e) — the JUNCTION RINGS ══════════════════════════════════
// P3 pinned every vertex on an authored edge at 0 and said so out loud: §2e's
// offset-plane solve is P4's.  Two of his standing verdicts ARE that pin —
// 7cec6b7a171d (the wall-to-wall bands stay flat) and 98b9eba830ec (no recess
// at a wall boundary) — and the second needs a second thing as well: even
// unpinned, R2's equal-arc border samples put NO vertex where a mortar groove
// crosses a shared edge, so a groove has nothing to end ON.  This block builds
// both halves.
//
// The two rulings of 2026-09-02 shape it:
//   * corner_rule = uniform_dominant (1c196c8c5cea) — the height at EVERY
//     junction is the DOMINANT OWNER's, both sides, no castellation step
//     anywhere.  So d₁ = d₂ at every ring point and the unequal-offset 3×3 the
//     design allows for cannot arise on a two-plane crease; what remains of the
//     3×3 is the chart-CORNER case, three or more planes meeting at a vertex.
//   * membership = partition (1a6e9e61ec89) — in a mesh this is the statement
//     that each face displaces by its OWN plane's height field and nothing
//     else, and that the crease is closed by the ring solve.  It is NOT the
//     reference raycaster's literal bisector partition, which deletes the pier
//     front over a ~90 px band at cam A (trap e43c035dbedf): nothing here is
//     trimmed by a bisector and no material is removed — every authored face
//     keeps its full extent, every ring vertex is shared, so the surface cannot
//     open and cannot lose a face.

// Which block-row band of `G` a map row belongs to.  The same wrap arithmetic
// ReliefClassAt uses, factored out so the two cannot disagree about a band.
static int BandAtY(const MatGrid &G, double yTex)
{
	auto wrap = [](double x, double p) { double r = std::fmod(x, p); if (r < 0) r += p; return r; };
	const double y = wrap(yTex, double(G.mipH));
	for (size_t b = 0; b < G.grid.bandY.size(); ++b) {
		const double a0 = wrap(double(G.grid.bandY[b].first), double(G.mipH));
		const double w  = double(G.grid.bandY[b].second - G.grid.bandY[b].first);
		if (wrap(y - a0, double(G.mipH)) <= w) return int(b);
	}
	return 0;
}

// Where a BREAKLINE of `G` crosses the segment uvA→uvB, as parameters in (0,1).
// The lines are the lattice's own — the row template's boundaries in y, the
// per-band column lines in x — so a border sample placed at one of these is the
// continuation of an interior breakline and not a new line of its own.  A
// column line only counts where the crossing actually lands in ITS band: the
// four bands do not share a column phase on this map (22.0 / 62.5 / 27.0 / 0.5
// texels on a 64-texel pitch, refutation 935f59476311), and taking every band's
// columns everywhere is exactly the mistake --v4_band_union made.
static void EdgeGrooveCrossings(const MatGrid &G, const double uvA[2],
                                const double uvB[2], std::vector<double> &out)
{
	if (!G.ok) return;
	const double perW = double(G.mipW), perH = double(G.mipH);
	const double xA = uvA[0]*perW, xB = uvB[0]*perW;
	const double yA = uvA[1]*perH, yB = uvB[1]*perH;
	const double dx = xB - xA, dy = yB - yA;
	const double tEps = 1e-6;
	if (std::fabs(dy) > 1e-9) {
		const double lo = std::min(yA, yB), hi = std::max(yA, yB);
		const long k0 = long(std::floor((lo - perH - G.y0Base) / perH));
		const long k1 = long(std::ceil ((hi + perH - G.y0Base) / perH));
		if (k1 >= k0 && k1 - k0 < 4096)
			for (long k = k0; k <= k1; ++k)
				for (const StoneRowT &r : G.rowTpl) {
					const double t = (double(r.y0) + double(k)*perH - yA) / dy;
					if (t > tEps && t < 1.0 - tEps) out.push_back(t);
				}
	}
	if (std::fabs(dx) > 1e-9) {
		const double lo = std::min(xA, xB), hi = std::max(xA, xB);
		const long j0 = long(std::floor((lo - perW) / perW));
		const long j1 = long(std::ceil ((hi + perW) / perW));
		if (j1 >= j0 && j1 - j0 < 4096)
			for (size_t b = 0; b < G.colLine.size(); ++b)
				for (long j = j0; j <= j1; ++j)
					for (const auto &cl : G.colLine[b]) {
						const double t = (cl.first + double(j)*perW - xA) / dx;
						if (!(t > tEps && t < 1.0 - tEps)) continue;
						if (BandAtY(G, yA + t*dy) != int(b)) continue;
						out.push_back(t);
					}
	}
}

struct RingStats {
	int64_t cornersSolved = 0, cornersHeldAbut = 0, cornersHeldSoup = 0, cornersNoField = 0;
	int64_t bordersSolved = 0, bordersHeldAbut = 0, bordersHeldSoup = 0;
	int64_t crossSamples = 0, arcSamples = 0, arcDropped = 0, edgesWithCross = 0;
	int64_t planes1 = 0, planes2 = 0, planes3 = 0, planes4 = 0;
	int64_t mitreClamped = 0, singular = 0, illCond = 0;
	// The §2e invariant splits in two once it is measured: a ring vertex with
	// one, two or exactly three independent planes has an EXACT solution and
	// must satisfy every one of them; a vertex with four or more is
	// over-determined and in general has none, so the honest report is the
	// least-squares residual and its own count.
	double  maxResidual = 0.0, maxResidualOver = 0.0;
	double  dMin = 1e300, dMax = -1e300, maxMove = 0.0;
	int64_t soupTris = 0;
	double  msRings = 0.0;
};

// Place a ring point.  `P` lies on every incident authored plane, so plane i's
// OFFSET plane is n_i·X = n_i·P + d, and the solve is for the x with n_i·x = d
// for every i; the vertex is P + x.  Two planes give the design's closed form
// x = d(n₁+n₂)/(1+n₁·n₂) — never a normalised bisector (law f713599ea11d) —
// and three give the 3×3 whose solution is d·(n₁×n₂ + n₂×n₃ + n₃×n₁)/det.
static V3 RingSolve(const V3 &P, const V3 *nIn, int nInCount, double d,
                    double mitreLimit, RingStats &RS)
{
	V3 n[16];
	int m = 0;
	for (int i = 0; i < nInCount && m < 16; ++i) {
		bool dup = false;
		for (int j = 0; j < m; ++j) if (dot(n[j], nIn[i]) > 1.0 - 1e-9) { dup = true; break; }
		if (!dup) n[m++] = nIn[i];
	}
	if (m == 0) return P;
	V3 x(0, 0, 0);
	if (m == 1) { x = n[0] * d; ++RS.planes1; }
	else if (m == 2) {
		++RS.planes2;
		const double c = dot(n[0], n[1]);
		if (1.0 + c < 1e-6) { x = n[0] * d; ++RS.singular; }      // the 180° fold
		else                  x = (n[0] + n[1]) * (d / (1.0 + c));
	} else {
		if (m == 3) ++RS.planes3; else ++RS.planes4;
		// the best-conditioned triple (conditioning ∝ 1/|det|, §2e)
		int bi = 0, bj = 1, bk = 2; double bd = 0.0;
		for (int i = 0; i < m; ++i) for (int j = i+1; j < m; ++j) for (int k = j+1; k < m; ++k) {
			const double dt = std::fabs(dot(n[i], cross(n[j], n[k])));
			if (dt > bd) { bd = dt; bi = i; bj = j; bk = k; }
		}
		if (bd > 1e-4 && m == 3) {
			x = (cross(n[bj], n[bk]) + cross(n[bk], n[bi]) + cross(n[bi], n[bj]))
			    * (d / dot(n[bi], cross(n[bj], n[bk])));
		} else if (bd > 1e-4) {
			// FOUR OR MORE planes: the system is over-determined and in general
			// has NO solution — §2e's "3-plane corner solve" silently assumed
			// three.  Solving three exactly and ignoring the rest leaves the
			// ignored ones off by whatever they happen to be (measured 8.9e-3 u
			// on this scene); the least-squares point (NᵀN)x = d·Nᵀ1 spreads
			// the same error over all of them, which is the right thing when
			// there is no exact answer and the vertex is shared by all of them.
			double A[3][3] = {{0,0,0},{0,0,0},{0,0,0}}, b[3] = {0,0,0};
			for (int i = 0; i < m; ++i) {
				const double nx = n[i].x, ny = n[i].y, nz = n[i].z;
				A[0][0]+=nx*nx; A[0][1]+=nx*ny; A[0][2]+=nx*nz;
				A[1][0]+=ny*nx; A[1][1]+=ny*ny; A[1][2]+=ny*nz;
				A[2][0]+=nz*nx; A[2][1]+=nz*ny; A[2][2]+=nz*nz;
				b[0]+=d*nx; b[1]+=d*ny; b[2]+=d*nz;
			}
			const double det =
			      A[0][0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1])
			    - A[0][1]*(A[1][0]*A[2][2]-A[1][2]*A[2][0])
			    + A[0][2]*(A[1][0]*A[2][1]-A[1][1]*A[2][0]);
			if (std::fabs(det) > 1e-9) {
				const double id = 1.0/det;
				x = V3(( b[0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1])
				        -A[0][1]*(b[1]*A[2][2]-A[1][2]*b[2])
				        +A[0][2]*(b[1]*A[2][1]-A[1][1]*b[2])) * id,
				       ( A[0][0]*(b[1]*A[2][2]-A[1][2]*b[2])
				        -b[0]*(A[1][0]*A[2][2]-A[1][2]*A[2][0])
				        +A[0][2]*(A[1][0]*b[2]-b[1]*A[2][0])) * id,
				       ( A[0][0]*(A[1][1]*b[2]-b[1]*A[2][1])
				        -A[0][1]*(A[1][0]*b[2]-b[1]*A[2][0])
				        +b[0]*(A[1][0]*A[2][1]-A[1][1]*A[2][0])) * id);
			} else {
				x = (cross(n[bj], n[bk]) + cross(n[bk], n[bi]) + cross(n[bi], n[bj]))
				    * (d / dot(n[bi], cross(n[bj], n[bk])));
			}
		} else {
			++RS.illCond;
			// an ill-conditioned triple is three near-coplanar strips; the
			// best-posed problem inside it is the most ORTHOGONAL pair
			int pi = 0, pj = 1; double pc = 2.0;
			for (int i = 0; i < m; ++i) for (int j = i+1; j < m; ++j) {
				const double c2 = std::fabs(dot(n[i], n[j]));
				if (c2 < pc) { pc = c2; pi = i; pj = j; }
			}
			const double c = dot(n[pi], n[pj]);
			if (1.0 + c < 1e-6) { x = n[pi] * d; ++RS.singular; }
			else                  x = (n[pi] + n[pj]) * (d / (1.0 + c));
		}
	}
	// Mitre limit (§2e): the two-plane offset is |d|/cos(θ/2) and diverges as
	// the planes fold back.  The design's alternative past the limit is a bevel
	// QUAD (two vertices); the census reports how often the clamp binds, and a
	// non-zero count is the signal to build the bevel — not to raise this.
	const double ad = std::fabs(d);
	if (ad > 1e-12) {
		const double ratio = len(x) / ad;
		if (ratio > mitreLimit) { x = x * (mitreLimit / ratio); ++RS.mitreClamped; }
	}
	double res = 0.0;
	for (int i = 0; i < m; ++i) res = std::max(res, std::fabs(dot(n[i], x) - d));
	if (m > 3) RS.maxResidualOver = std::max(RS.maxResidualOver, res);
	else       RS.maxResidual     = std::max(RS.maxResidual, res);
	RS.maxMove     = std::max(RS.maxMove, len(x));
	RS.dMin = std::min(RS.dMin, d);
	RS.dMax = std::max(RS.dMax, d);
	return P + x;
}

}  // namespace

// ───────────────────────────────────────────────────────────────────────────
void RunP2Bake(Scene *Sc, const char *const *matNames, int nMats, int mipReq, float ampIn)
{
	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	const bool census = FeatureFlags::v4_census();
	const bool flat   = FeatureFlags::v4_flat();
	// P3 amplitude: --v4_amp when it is set (>= 0), else the caller's
	// --greets_displace_amp, so v4 and the reference renderer carve the same
	// relief by default.  --v4_flat emits the authored triangles and has no
	// interior node to move, so it is amp-blind by construction.
	const double amp = flat ? 0.0
	                 : (FeatureFlags::v4_amp() >= 0.0f ? double(FeatureFlags::v4_amp())
	                                                   : double(ampIn));
	const int  gref   = flat ? 0 : std::max(0, std::min(4, FeatureFlags::v4_groove_refine()));
	const int  capSeg = std::max(1, FeatureFlags::v4_max_border_seg());
	if (!Sc || !matNames || nMats <= 0) return;

	P2Topo T;
	if (!BuildP2Topo(Sc, matNames, nMats, T)) {
		std::fprintf(stderr, "[V4-LATTICE] REFUSED: %d mesh(es) carry the target materials "
		             "and %zu faces survived the guard — v4 bakes exactly one mesh in model "
		             "space (P1 deviation #3).  NOTHING was changed; the old bake did not run "
		             "either, so this arm renders the AUTHORED stone.\n",
		             T.meshesWithStone, T.faces.size());
		return;
	}
	const auto tTopo = clock::now();
	P2Stats S;
	S.msTopo = std::chrono::duration<double, std::milli>(tTopo - t0).count();

	// ── per-material grid (the breaklines) ─────────────────────────────────
	std::vector<MatGrid> grids{ size_t(nMats) };
	for (int m = 0; m < nMats; ++m) {
		const Material *mat = nullptr;
		for (const P2Face &F : T.faces)
			if (F.matIdx == m && F.src->Txtr) { mat = F.src->Txtr; break; }
		BuildMatGrid(mat, mipReq, grids[size_t(m)]);
		MeshOps_StoneParentPlaneReset(matNames[m]);
	}
	const auto tGrid = clock::now();
	S.msGrid = std::chrono::duration<double, std::milli>(tGrid - tTopo).count();

	// ── the CHART PLANE NORMALS (design §2e: the displacement direction, and
	// nothing else, anywhere — law 420fcd4626b8).  The chart's proxy is the
	// AREA-WEIGHTED face normal, which is the optimal L^{2,1} proxy (survey
	// §B), and the max angle any member face makes with it is reported: on this
	// scene P1 measured every chart exactly coplanar (0.0000°), so the choice is
	// free here, but the number is what would refute it on another scene. ──
	std::vector<V3> chartN(size_t(T.nCharts));
	double chartMaxDevDeg = 0.0;
	{
		for (const P2Face &F : T.faces)
			chartN[size_t(F.chart)] = chartN[size_t(F.chart)] + F.n * F.area;
		for (size_t c = 0; c < chartN.size(); ++c) chartN[c] = nrm(chartN[c]);
		for (const P2Face &F : T.faces)
			chartMaxDevDeg = std::max(chartMaxDevDeg, angDeg(chartN[size_t(F.chart)], F.n));
	}

	// ── R2: every vertex's target world spacing, then every border's own
	// sample count.  The spacing is a property of the VERTEX (the finest cell
	// of any stone face touching it), so both sides of a shared edge read the
	// identical number and neither chart's interior decides for the other —
	// DiagSplit's rule, survey §A/R2. ──────────────────────────────────────
	T.vspacing.assign(T.vpos.size(), 1e300);
	for (const P2Face &F : T.faces) {
		const MatGrid &G = grids[size_t(F.matIdx)];
		if (!G.ok) continue;
		const double u0 = F.uv[0][0], v0 = F.uv[0][1];
		const double du1 = F.uv[1][0]-u0, dv1 = F.uv[1][1]-v0;
		const double du2 = F.uv[2][0]-u0, dv2 = F.uv[2][1]-v0;
		const double det = du1*dv2 - du2*dv1;
		if (std::fabs(det) < 1e-18) continue;
		const V3 e1 = F.p[1] - F.p[0], e2 = F.p[2] - F.p[0];
		const V3 Tu = (e1 * dv2 - e2 * dv1) * (1.0/det);
		const V3 Bv = (e2 * du1 - e1 * du2) * (1.0/det);
		const double sw = std::min(G.targetTexX * len(Tu) / double(G.mipW),
		                           G.targetTexY * len(Bv) / double(G.mipH));
		if (!(sw > 0.0)) continue;
		for (int k = 0; k < 3; ++k)
			T.vspacing[size_t(F.v[k])] = std::min(T.vspacing[size_t(F.v[k])], sw);
	}
	int64_t nBorderInterior = 0;
	const bool abutSplit = FeatureFlags::v4_abut_split();
	for (P2Edge &E : T.edges) {
		if (flat) { E.nSeg = 1; continue; }
		E.tpar.clear(); E.tnum.clear();
		// The 130 use-count-1 edges are ABUTMENTS, not boundaries (418c3bf6c9d7),
		// and 74 of them already carry a T-junction in the authored mesh.
		// Splitting one multiplies that pre-existing T-vertex, so by default the
		// abutment keeps its single authored segment: "pinned exactly to the
		// authored edge line", which is what stops P3+ from opening the base
		// junction along it.  §2e (P4) is where the two sides become one ring.
		if (E.use == 1 && !abutSplit) { E.nSeg = 1; continue; }
		const double sa = T.vspacing[size_t(E.a)], sb = T.vspacing[size_t(E.b)];
		if (!(sa < 1e299) || !(sb < 1e299)) { E.nSeg = 1; continue; }
		const double sp = 0.5 * (sa + sb);
		long n = long(std::floor(E.length / sp + 0.5));
		if (n < 1) n = 1;
		if (n > capSeg) { n = capSeg; ++S.borderCapped; }
		E.nSeg = int32_t(n);
		S.borderMin = std::min(S.borderMin, E.nSeg);
		S.borderMax = std::max(S.borderMax, E.nSeg);
	}
	if (flat) { S.borderMin = S.borderMax = 1; }

	// ══ P4 (§2e) — who may move, where to, and where the border samples go ══
	const auto tRing0 = clock::now();
	const bool  rings       = !flat && FeatureFlags::v4_rings();
	const bool  ringGrooves = rings && FeatureFlags::v4_ring_grooves();
	const bool  ringAbut    = FeatureFlags::v4_ring_abut();
	const bool  crossBoth   = FeatureFlags::v4_ring_cross_both();
	const double mitreLimit = std::max(1.0, double(FeatureFlags::v4_mitre_limit()));
	const double minSepFrac = std::max(0.0, std::min(0.9, double(FeatureFlags::v4_ring_min_sep())));
	RingStats RS;

	// edge → its (up to two) faces, in the faces' own mesh order
	std::vector<std::array<int32_t,2>> eFace(T.edges.size(), std::array<int32_t,2>{{-1,-1}});
	for (size_t f = 0; f < T.faces.size(); ++f)
		for (int k = 0; k < 3; ++k) {
			const int32_t ei = T.faces[f].edge[k];
			if (ei < 0) continue;
			auto &e = eFace[size_t(ei)];
			if      (e[0] < 0) e[0] = int32_t(f);
			else if (e[1] < 0) e[1] = int32_t(f);
		}

	// The DOMINANT-OWNER key (his ruling 1c196c8c5cea).  Order-independent by
	// construction — smaller material index, then smaller area-weighted chart
	// centroid — which is the reference renderer's own `domOwner` rule lifted
	// from the face to the chart, so the two implementations pick the same
	// winner and the bake's crease heights are comparable to the reference's.
	struct ChartKey { int32_t mat; double cx, cy, cz; };
	std::vector<ChartKey> ckey(size_t(T.nCharts), ChartKey{ 1 << 30, 0, 0, 0 });
	{
		std::vector<double> aw(size_t(T.nCharts), 0.0);
		std::vector<V3> cc(size_t(T.nCharts));
		for (const P2Face &F : T.faces) {
			const size_t c = size_t(F.chart);
			ckey[c].mat = std::min(ckey[c].mat, F.matIdx);
			cc[c] = cc[c] + (F.p[0] + F.p[1] + F.p[2]) * (F.area / 3.0);
			aw[c] += F.area;
		}
		for (size_t c = 0; c < ckey.size(); ++c) {
			const double w = aw[c] > 0 ? 1.0 / aw[c] : 0.0;
			ckey[c].cx = cc[c].x * w; ckey[c].cy = cc[c].y * w; ckey[c].cz = cc[c].z * w;
		}
	}
	auto chartLess = [&](int32_t a, int32_t b) -> bool {
		if (a == b) return false;
		const ChartKey &A = ckey[size_t(a)], &B = ckey[size_t(b)];
		if (A.mat != B.mat) return A.mat < B.mat;
		if (A.cx  != B.cx)  return A.cx  < B.cx;
		if (A.cy  != B.cy)  return A.cy  < B.cy;
		if (A.cz  != B.cz)  return A.cz  < B.cz;
		return a < b;
	};

	// vertex → the distinct charts meeting there, with one representative face
	// per chart (the first in mesh order, so the pick is deterministic)
	std::vector<std::vector<int32_t>> vChart(T.vpos.size()), vFaceOf(T.vpos.size());
	for (size_t f = 0; f < T.faces.size(); ++f)
		for (int k = 0; k < 3; ++k) {
			const size_t v = size_t(T.faces[f].v[k]);
			const int32_t c = T.faces[f].chart;
			bool have = false;
			for (int32_t q : vChart[v]) if (q == c) { have = true; break; }
			if (!have) { vChart[v].push_back(c); vFaceOf[v].push_back(int32_t(f)); }
		}

	// ── the HOLD set, measured rather than assumed ─────────────────────────
	// bit 0 = the vertex is on a use-count-1 ABUTMENT.  An abutment is a
	// T-junction against another stone face the stitch cannot see (418c3bf6c9d7):
	// the host face has no vertex on the line, so moving this side alone opens
	// the base junction (bc79e39d).  --v4_ring_abut lifts it as a measured arm.
	// bit 1 = the point coincides with a face of the SAME mesh that this bake
	// does NOT touch (siling, lintel, teleporter…).  Those faces stay authored,
	// so a stone vertex that moves away from one tears the seam against it.
	std::vector<uint8_t> vHold(T.vpos.size(), 0), eHold(T.edges.size(), 0);
	std::vector<std::pair<std::string,int64_t>> holdMats;   // who holds them, by material
	for (size_t ei = 0; ei < T.edges.size(); ++ei) {
		const P2Edge &E = T.edges[ei];
		if (E.use != 2) {
			eHold[ei] |= 1;
			vHold[size_t(E.a)] |= 1; vHold[size_t(E.b)] |= 1;
		}
	}
	if (rings) {
		std::vector<uint8_t> baked(size_t(T.mesh->FIndex), 0);
		for (const P2Face &F : T.faces) baked[size_t(F.faceIdx)] = 1;
		std::vector<SoupTri> soup;
		for (int32_t i = 0; i < int32_t(T.mesh->FIndex); ++i) {
			if (baked[size_t(i)]) continue;
			const Face &F = T.mesh->Faces[i];
			if (!F.A || !F.B || !F.C) continue;
			SoupTri s;
			s.p[0] = V3(F.A->Pos); s.p[1] = V3(F.B->Pos); s.p[2] = V3(F.C->Pos);
			s.mat = (F.Txtr && F.Txtr->Name) ? F.Txtr->Name : "?";
			soup.push_back(s);
		}
		RS.soupTris = int64_t(soup.size());
		if (!soup.empty()) {
			SoupGrid SG; SG.build(soup, 0.4);
			const double tol2 = 1e-4 * 1e-4;
			auto touchesSoup = [&](const V3 &p) -> const char * {
				const int x = SG.cx(p.x), y = SG.cy(p.y), z = SG.cz(p.z);
				for (int dz = -1; dz <= 1; ++dz)
				for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx) {
					const int xx = x+dx, yy = y+dy, zz = z+dz;
					if (xx < 0 || yy < 0 || zz < 0 || xx >= SG.nx || yy >= SG.ny || zz >= SG.nz) continue;
					for (int32_t ti : SG.b[size_t(SG.idx(xx, yy, zz))])
						if (PointTriDist2(p, soup[size_t(ti)].p[0], soup[size_t(ti)].p[1],
						                  soup[size_t(ti)].p[2]) <= tol2) return soup[size_t(ti)].mat;
				}
				return nullptr;
			};
			std::map<std::string,int64_t> hm;
			for (size_t v = 0; v < T.vpos.size(); ++v)
				if (const char *nm = touchesSoup(T.vpos[v])) { vHold[v] |= 2; ++hm[nm]; }
			for (size_t ei = 0; ei < T.edges.size(); ++ei) {
				const P2Edge &E = T.edges[ei];
				const V3 mid = (T.vpos[size_t(E.a)] + T.vpos[size_t(E.b)]) * 0.5;
				if (touchesSoup(mid)) eHold[ei] |= 2;
			}
			for (auto &kv : hm) holdMats.push_back({ kv.first, kv.second });
		}
	}
	auto vFree = [&](size_t v) -> bool {
		const uint8_t h = vHold[v];
		if (h & 2) return false;
		if ((h & 1) && !ringAbut) return false;
		return true;
	};
	auto eFree = [&](size_t ei) -> bool {
		const uint8_t h = eHold[ei];
		if (h & 2) return false;
		if ((h & 1) && !ringAbut) return false;
		return true;
	};

	// ── the border sample PARAMETERS, per edge ─────────────────────────────
	// R2's equal-arc i/n samples, plus (--v4_ring_grooves) one where every
	// mortar run boundary of EITHER adjacent face crosses the edge, thinned so
	// nothing lands closer than --v4_ring_min_sep of a nominal segment.
	{
		std::vector<double> cross;
		for (size_t ei = 0; ei < T.edges.size(); ++ei) {
			P2Edge &E = T.edges[ei];
			if (flat) continue;
			cross.clear();
			if (ringGrooves && eFree(ei)) {
				// The crossings come from the DOMINANT OWNER's grid ONLY, and
				// that is not a shortcut — it is the same ruling that decides
				// the heights (1c196c8c5cea).  A crease carries ONE profile,
				// the dominant owner's, so the vertices that profile needs are
				// the ones its own breaklines ask for.  Taking BOTH sides was
				// the first cut and it is REFUTED by the picture: on a
				// wall↔floor edge the floor's flagstone grid crosses the base
				// line at positions that mean nothing in the wall's field, and
				// the extra samples turn the wall's base course into a fan of
				// long facets (docs/img/p4/camA_base_*.png).  The non-dominant
				// side loses nothing it had before: its own interior
				// breaklines still terminate on the border exactly as they did
				// through P2 and P3, which never produced an artifact.
				const int32_t f0 = eFace[ei][0], f1 = eFace[ei][1];
				int32_t fDom = f0;
				if (f0 >= 0 && f1 >= 0 &&
				    chartLess(T.faces[size_t(f1)].chart, T.faces[size_t(f0)].chart)) fDom = f1;
				for (int s = 0; s < (crossBoth ? 2 : 1); ++s) {
					const int32_t fi = crossBoth ? eFace[ei][s] : fDom;
					if (fi < 0) continue;
					const P2Face &F = T.faces[size_t(fi)];
					const MatGrid &G = grids[size_t(F.matIdx)];
					int ka = -1, kb = -1;
					for (int k = 0; k < 3; ++k) {
						if (F.v[k] == E.a) ka = k;
						if (F.v[k] == E.b) kb = k;
					}
					if (ka < 0 || kb < 0) continue;
					const double uvA[2] = { F.uv[ka][0], F.uv[ka][1] };
					const double uvB[2] = { F.uv[kb][0], F.uv[kb][1] };
					EdgeGrooveCrossings(G, uvA, uvB, cross);
				}
			}
			std::sort(cross.begin(), cross.end());
			const double minSep = minSepFrac / double(std::max(1, E.nSeg));
			std::vector<double> keep;
			for (double t : cross) {
				if (t < minSep || t > 1.0 - minSep) continue;
				if (!keep.empty() && t - keep.back() < minSep) continue;
				keep.push_back(t);
			}
			if (!keep.empty()) ++RS.edgesWithCross;
			RS.crossSamples += int64_t(keep.size());
			for (double t : keep) { E.tpar.push_back(t); E.tnum.push_back(-1); }
			for (int q = 1; q < E.nSeg; ++q) {
				const double t = double(q) / double(E.nSeg);
				bool room = true;
				for (double k2 : keep) if (std::fabs(k2 - t) < minSep) { room = false; break; }
				if (!room) { ++RS.arcDropped; continue; }
				E.tpar.push_back(t); E.tnum.push_back(int32_t(q));
				++RS.arcSamples;
			}
			// one sort over both populations, carrying tnum with it
			std::vector<int32_t> ord(E.tpar.size());
			for (size_t q = 0; q < ord.size(); ++q) ord[q] = int32_t(q);
			std::sort(ord.begin(), ord.end(), [&](int32_t a, int32_t b) {
				if (E.tpar[size_t(a)] != E.tpar[size_t(b)]) return E.tpar[size_t(a)] < E.tpar[size_t(b)];
				return a < b;
			});
			std::vector<double> tp; std::vector<int32_t> tn;
			tp.reserve(ord.size()); tn.reserve(ord.size());
			for (int32_t q : ord) { tp.push_back(E.tpar[size_t(q)]); tn.push_back(E.tnum[size_t(q)]); }
			if (int(tp.size()) > capSeg - 1) {
				tp.resize(size_t(std::max(0, capSeg - 1)));
				tn.resize(tp.size());
				++S.borderCapped;
			}
			E.tpar.swap(tp); E.tnum.swap(tn);
			nBorderInterior += int64_t(E.tpar.size());
		}
	}

	// ── the CORNER ring positions ──────────────────────────────────────────
	// Solved here, once per stitched vertex, so every face that references the
	// corner references the SAME moved point.  A corner's incident chart set is
	// every chart meeting there, which is what makes the 3-plane case the
	// chart-corner case rather than a special rule.
	std::vector<V3> vRingPos(T.vpos);
	std::vector<double> vRingD(T.vpos.size(), 0.0);
	if (rings) {
		for (size_t v = 0; v < T.vpos.size(); ++v) {
			if (vChart[v].empty()) continue;
			if (!vFree(v)) {
				if (vHold[v] & 2) ++RS.cornersHeldSoup; else ++RS.cornersHeldAbut;
				continue;
			}
			size_t best = 0;
			for (size_t q = 1; q < vChart[v].size(); ++q)
				if (chartLess(vChart[v][q], vChart[v][best])) best = q;
			const P2Face &FD = T.faces[size_t(vFaceOf[v][best])];
			const MatGrid &G = grids[size_t(FD.matIdx)];
			int kk = -1;
			for (int k = 0; k < 3; ++k) if (FD.v[k] == int32_t(v)) kk = k;
			if (kk < 0 || !G.fieldOk) { ++RS.cornersNoField; continue; }
			const double du = FD.uv[kk][0], dv = FD.uv[kk][1];
			const int    cls = ReliefClassAt(G, du, dv);
			const double d   = amp * (NodeH(G, du, dv, cls) - G.mipMean);
			V3 nn[16]; int m = 0;
			for (size_t q = 0; q < vChart[v].size() && m < 16; ++q)
				nn[m++] = chartN[size_t(vChart[v][q])];
			vRingPos[v] = RingSolve(T.vpos[v], nn, m, d, mitreLimit, RS);
			vRingD[v]   = d;
			++RS.cornersSolved;
		}
	}
	RS.msRings = std::chrono::duration<double, std::milli>(clock::now() - tRing0).count();

	S.borderSamples = nBorderInterior;

	// ── output arrays ──────────────────────────────────────────────────────
	TriMesh *M = T.mesh;
	const uint32_t nOrig = uint32_t(M->VIndex);
	const Vertex *oldV = M->Verts;
	std::vector<Vertex> outV(oldV, oldV + nOrig);
	std::vector<Face>   outF;
	std::vector<std::array<uint32_t,3>> outI;
	outF.reserve(size_t(M->FIndex) + 8);
	outI.reserve(outF.capacity());

	T.vGlobal.assign(T.vpos.size(), -1);
	std::unordered_map<int64_t, int32_t> borderVert;
	std::unordered_map<int64_t, double>  borderD;   // P4: the ring height of each sample
	// per emitted vertex: 0 = authored corner, 1 = sample on a use-1 abutment,
	// 2 = sample on a shared (use-2) border, 3 = interior lattice node.  Used by
	// the T-vertex census to say WHICH population any defect came from.
	std::vector<uint8_t> vClass(size_t(nOrig), 4);   // 4 = an ORIGINAL authored vertex
	auto mkVert = [&](const Vertex *proto, const V3 &pos, const V3 &nn, double u, double v) -> uint32_t {
		Vertex m = *proto;
		m.Pos = Vector{ float(pos.x), float(pos.y), float(pos.z) };
		const double l = len(nn);
		if (l > 1e-12) m.N = Vector{ float(nn.x/l), float(nn.y/l), float(nn.z/l) };
		m.U = float(u); m.V = float(v);
		outV.push_back(m);
		vClass.push_back(3);
		return uint32_t(outV.size()) - 1;
	};
	int64_t nPinnedCorners = 0;
	auto cornerVert = [&](int32_t sv) -> uint32_t {
		if (T.vGlobal[size_t(sv)] >= 0) return uint32_t(T.vGlobal[size_t(sv)]);
		++nPinnedCorners;
		const Vertex *proto = T.vsrc[size_t(sv)];
		Vertex m = *proto;                       // authored record, position included
		// P4 (§2e): the corner's OFFSET-PLANE position, solved once in the ring
		// pre-pass.  Identical to the authored one when the corner is held or
		// when the rings are off, so this line is byte-null in those arms.
		const V3 &rp = vRingPos[size_t(sv)];
		m.Pos = Vector{ float(rp.x), float(rp.y), float(rp.z) };
		outV.push_back(m);
		vClass.push_back(0);
		T.vGlobal[size_t(sv)] = int32_t(outV.size()) - 1;
		return uint32_t(T.vGlobal[size_t(sv)]);
	};

	// which face index in the mesh is stone, and its P2Face
	std::vector<int32_t> faceOfMesh(size_t(M->FIndex), -1);
	for (size_t f = 0; f < T.faces.size(); ++f) faceOfMesh[size_t(T.faces[f].faceIdx)] = int32_t(f);

	// parent-plane ordinals, from the AUTHORED plane (the old bake stamps the
	// same thing, from the emitted faces before they take the displaced plane)
	for (P2Face &F : T.faces)
		F.planeOrd = MeshOps_StoneParentPlaneRegister(matNames[size_t(F.matIdx)],
		                                              F.src->N.x, F.src->N.y, F.src->N.z,
		                                              F.src->NormProd);

	std::vector<LatPt> boundary, pt;
	std::vector<std::array<int32_t,3>> tri;
	std::vector<int64_t> chartTris(size_t(T.nCharts), 0), chartFaces(size_t(T.nCharts), 0);
	int64_t stoneFaces = 0;
	int64_t nodeCls[3] = {0,0,0};                    // groove / bevel / plateau nodes moved
	double  dMinSeen = 1e300, dMaxSeen = -1e300;
	ReliefCensus RC;                                 // [V4-RELIEF]; empty unless asked
	const bool reliefCensus = census && FeatureFlags::v4_relief_census() && amp != 0.0;
	for (int32_t fi = 0; fi < M->FIndex; ++fi) {
		const int32_t si = faceOfMesh[size_t(fi)];
		if (si < 0) {                                    // non-stone: verbatim
			const Face &F = M->Faces[fi];
			outF.push_back(F);
			outI.push_back({ uint32_t(F.A - oldV), uint32_t(F.B - oldV), uint32_t(F.C - oldV) });
			continue;
		}
		const P2Face &F = T.faces[size_t(si)];
		const MatGrid &G = grids[size_t(F.matIdx)];

		// 2D frame: the authored triangle is CCW in it, so any CCW output
		// triangle carries the authored winding
		V3 ex = nrm(F.p[1] - F.p[0]);
		V3 ey = cross(F.n, ex);
		if (dot(cross(F.p[1]-F.p[0], F.p[2]-F.p[0]), cross(ex, ey)) < 0.0) ey = ey * -1.0;

		// boundary: corner k, then the interior samples of edge k, in order
		boundary.clear();
		for (int k = 0; k < 3; ++k) {
			const int32_t vk = F.v[k], vn = F.v[(k+1)%3];
			{
				LatPt P; P.p = F.p[k]; P.u = F.uv[k][0]; P.v = F.uv[k][1];
				P.x2 = dot(P.p - F.p[0], ex); P.y2 = dot(P.p - F.p[0], ey);
				P.d = vRingD[size_t(vk)];
				P.gid = int32_t(cornerVert(vk));
				boundary.push_back(P);
			}
			const int32_t ei = F.edge[k];
			if (ei < 0) continue;
			const P2Edge &E = T.edges[size_t(ei)];
			if (E.tpar.empty()) continue;
			const bool fwd = (vk == E.a);
			const int   uvA = fwd ? k : (k+1)%3;         // corner that IS endpoint a
			const int   uvB = fwd ? (k+1)%3 : k;
			const double A3[3] = { T.vpos[size_t(E.a)].x, T.vpos[size_t(E.a)].y, T.vpos[size_t(E.a)].z };
			const double B3[3] = { T.vpos[size_t(E.b)].x, T.vpos[size_t(E.b)].y, T.vpos[size_t(E.b)].z };
			const V3 nA = V3(T.vsrc[size_t(E.a)]->N), nB = V3(T.vsrc[size_t(E.b)]->N);
			const size_t nPar = E.tpar.size();
			for (size_t q = 0; q < nPar; ++q) {
				// the parameter list is the EDGE's, in its canonical a→b order;
				// a face traversing the edge the other way walks it backwards
				const size_t qi = fwd ? q : (nPar - 1 - q);
				const double t = E.tpar[qi];
				double P3[3];
				if (E.tnum[qi] >= 0) BorderSample(A3, B3, E.tnum[qi], E.nSeg, P3);
				else                 BorderSampleT(A3, B3, t, P3);
				LatPt P;
				P.p = V3(P3[0], P3[1], P3[2]);          // the AUTHORED point on the line
				P.u = F.uv[uvA][0] + (F.uv[uvB][0] - F.uv[uvA][0]) * t;
				P.v = F.uv[uvA][1] + (F.uv[uvB][1] - F.uv[uvA][1]) * t;
				P.x2 = dot(P.p - F.p[0], ex); P.y2 = dot(P.p - F.p[0], ey);
				const int64_t key = (int64_t(ei) << 20) | int64_t(qi);
				auto it = borderVert.find(key);
				auto dit = borderD.find(key);
				if (it == borderVert.end()) {
					// ── P4 (§2e): the sample's OFFSET-PLANE position ──────
					// The height is the DOMINANT OWNER's (his ruling
					// 1c196c8c5cea), read at THIS point in that chart's own UV,
					// and the position is the point that lies on the offset
					// plane of both charts.  Derived from the EDGE, not from
					// the face that happened to reach it first, so the two
					// sides cannot compute two answers — and there is only one
					// vertex anyway, which is what keeps the crease closed.
					V3 pOut = P.p;
					double dRing = 0.0;
					if (rings && eFree(ei)) {
						const int32_t f0 = eFace[size_t(ei)][0], f1 = eFace[size_t(ei)][1];
						int32_t fD = f0;
						if (f1 >= 0 && chartLess(T.faces[size_t(f1)].chart, T.faces[size_t(f0)].chart))
							fD = f1;
						if (fD >= 0) {
							const P2Face &FD = T.faces[size_t(fD)];
							const MatGrid &GD = grids[size_t(FD.matIdx)];
							int ka = -1, kb = -1;
							for (int kx = 0; kx < 3; ++kx) {
								if (FD.v[kx] == E.a) ka = kx;
								if (FD.v[kx] == E.b) kb = kx;
							}
							if (ka >= 0 && kb >= 0 && GD.fieldOk) {
								const double du = FD.uv[ka][0] + (FD.uv[kb][0] - FD.uv[ka][0]) * t;
								const double dv = FD.uv[ka][1] + (FD.uv[kb][1] - FD.uv[ka][1]) * t;
								const int cls = ReliefClassAt(GD, du, dv);
								dRing = amp * (NodeH(GD, du, dv, cls) - GD.mipMean);
								V3 nn2[2]; int m2 = 0;
								nn2[m2++] = chartN[size_t(T.faces[size_t(f0)].chart)];
								if (f1 >= 0) nn2[m2++] = chartN[size_t(T.faces[size_t(f1)].chart)];
								pOut = RingSolve(P.p, nn2, m2, dRing, mitreLimit, RS);
								++RS.bordersSolved;
							}
						}
					} else if (rings) {
						if (eHold[size_t(ei)] & 2) ++RS.bordersHeldSoup;
						else                       ++RS.bordersHeldAbut;
					}
					const V3 nn = nA * (1.0 - t) + nB * t;
					const Vertex *proto = T.vsrc[size_t(E.a)];
					const double pu = double(proto->U), pv = double(proto->V);
					const uint32_t g = mkVert(proto, pOut, nn, pu, pv);
					vClass[size_t(g)] = uint8_t(E.use == 1 ? 1 : 2);
					it  = borderVert.emplace(key, int32_t(g)).first;
					dit = borderD.emplace(key, dRing).first;
					// how far the shared sample sits off the exact authored line
					const V3 d = P.p - T.vpos[size_t(E.a)];
					const V3 e = T.vpos[size_t(E.b)] - T.vpos[size_t(E.a)];
					const double el = len(e);
					if (el > 0) {
						const V3 perp = d - e * (dot(d, e) / (el*el));
						S.borderMaxDev = std::max(S.borderMaxDev, len(perp));
					}
				}
				P.gid = it->second;
				P.d   = (dit != borderD.end()) ? dit->second : 0.0;
				boundary.push_back(P);
			}
		}

		FaceLattice(T, F, G, boundary, ex, ey, gref, flat, S, pt, tri);

		// ── PHASE 3 (§2d) + PHASE 4 (§2e): interior points get their vertices
		// now, and their HEIGHT.  `gid >= 0` is a point that already has a
		// vertex — a corner or a shared/abutment border sample.  Through P3
		// those were PINNED at 0; as of P4 they carry the RING height from the
		// offset-plane solve above (0 when they are held or when --no-v4_rings),
		// which is already in `pt[i].d`, so the relief census sees the surface
		// the mesh actually has.  Either way the position is decided once, per
		// stitched vertex or per (edge, sample), and both faces index it — so
		// nothing two faces share can take two values and the watertightness P2
		// proved is carried forward untouched. ─────────────────────────────
		const Vertex *pv[3] = { F.src->A, F.src->B, F.src->C };
		const V3 dispDir = chartN[size_t(F.chart)];
		for (size_t i = 0; i < pt.size(); ++i) {
			if (pt[i].gid >= 0) continue;                       // ring/border: placed
			double b0, b1, b2;
			{
				const double u0 = F.uv[0][0], v0 = F.uv[0][1];
				const double du1 = F.uv[1][0]-u0, dv1 = F.uv[1][1]-v0;
				const double du2 = F.uv[2][0]-u0, dv2 = F.uv[2][1]-v0;
				const double det = du1*dv2 - du2*dv1;
				if (std::fabs(det) > 1e-18) {
					const double dU = pt[i].u - u0, dV = pt[i].v - v0;
					b1 = ( dU*dv2 - dV*du2) / det;
					b2 = ( du1*dV - dv1*dU) / det;
					b0 = 1.0 - b1 - b2;
				} else { b0 = b1 = b2 = 1.0/3.0; }
			}
			if (amp != 0.0 && G.fieldOk) {
				const int cls = ReliefClassAt(G, pt[i].u, pt[i].v);
				pt[i].d = amp * (NodeH(G, pt[i].u, pt[i].v, cls) - G.mipMean);
				pt[i].p = pt[i].p + dispDir * pt[i].d;
				++nodeCls[size_t(cls)];
				dMinSeen = std::min(dMinSeen, pt[i].d);
				dMaxSeen = std::max(dMaxSeen, pt[i].d);
			}
			const V3 nn = V3(pv[0]->N)*b0 + V3(pv[1]->N)*b1 + V3(pv[2]->N)*b2;
			pt[i].gid = int32_t(mkVert(pv[0], pt[i].p, nn, pt[i].u, pt[i].v));
		}

		if (reliefCensus) {
			const auto tR = clock::now();
			AccumRelief(F, G, amp, pt, tri, FaceCellW(F, G), RC);
			S.msRelief += std::chrono::duration<double, std::milli>(clock::now() - tR).count();
		}

		chartTris[size_t(F.chart)] += int64_t(tri.size());
		chartFaces[size_t(F.chart)] += 1;
		for (const auto &tr : tri) {
			Face f = *F.src;
			f.frame = nullptr;
			const LatPt &a = pt[size_t(tr[0])], &b = pt[size_t(tr[1])], &c = pt[size_t(tr[2])];
			f.U1 = float(a.u); f.V1 = float(a.v);
			f.U2 = float(b.u); f.V2 = float(b.v);
			f.U3 = float(c.u); f.V3 = float(c.v);
			f.EU1 = f.U1; f.EV1 = f.V1; f.EU2 = f.U2; f.EV2 = f.V2; f.EU3 = f.U3; f.EV3 = f.V3;
			f.ShadowMatID = F.planeOrd;
			outF.push_back(f);
			outI.push_back({ uint32_t(a.gid), uint32_t(b.gid), uint32_t(c.gid) });
			++stoneFaces;
		}
	}
	const auto tLat = clock::now();
	S.msLattice = std::chrono::duration<double, std::milli>(tLat - tGrid).count();

	// ── commit ─────────────────────────────────────────────────────────────
	Vertex *nv = new Vertex[outV.size()];
	std::memcpy(nv, outV.data(), outV.size()*sizeof(Vertex));
	Face *nf = new Face[outF.size()];
	for (size_t i = 0; i < outF.size(); ++i) {
		nf[i] = outF[i];
		nf[i].A = &nv[outI[i][0]]; nf[i].B = &nv[outI[i][1]]; nf[i].C = &nv[outI[i][2]];
	}
	// Re-derive N / NormProd on the STONE faces only — non-stone keeps its
	// authored plane, exactly as the old bake's commit does (the B1 lesson: a
	// stale NormProd mis-culls).  Orientation is taken from the proto face's
	// authored N so a retessellation can never flip a winding.
	for (size_t i = 0; i < outF.size(); ++i) {
		if (!nf[i].Txtr || !nf[i].Txtr->Name) continue;
		bool isStone = false;
		for (int m = 0; m < nMats; ++m)
			if (!std::strcmp(nf[i].Txtr->Name, matNames[m])) { isStone = true; break; }
		if (!isStone) continue;
		const Vector &A = nf[i].A->Pos, &B = nf[i].B->Pos, &C = nf[i].C->Pos;
		const float e1x=B.x-A.x,e1y=B.y-A.y,e1z=B.z-A.z, e2x=C.x-A.x,e2y=C.y-A.y,e2z=C.z-A.z;
		float gx=e1y*e2z-e1z*e2y, gy=e1z*e2x-e1x*e2z, gz=e1x*e2y-e1y*e2x;
		const float gl=std::sqrt(gx*gx+gy*gy+gz*gz);
		if (gl>1e-6f) {
			gx/=gl; gy/=gl; gz/=gl;
			if (gx*outF[i].N.x + gy*outF[i].N.y + gz*outF[i].N.z < 0.0f) { gx=-gx; gy=-gy; gz=-gz; }
			nf[i].N.x=gx; nf[i].N.y=gy; nf[i].N.z=gz;
		}
		nf[i].NormProd = -(nf[i].N.x*A.x + nf[i].N.y*A.y + nf[i].N.z*A.z);
	}
	M->Verts = nv; M->VIndex = int32_t(outV.size());
	M->Faces = nf; M->FIndex = int32_t(outF.size());
	Compute_FaceVertexIndices(M);
	if (M->Flags & Tri_Stationary)
		M->SL = (Color *)getAlignedBlock(sizeof(Color) * M->VIndex, 16);
	const auto tCommit = clock::now();
	S.msCommit = std::chrono::duration<double, std::milli>(tCommit - tLat).count();

	std::fprintf(stderr,
	    "[V4-LATTICE] arm name=%s cpb=%.3f groove_refine=%d chart_budget=%.1f mip=%d\n",
	    flat ? "flat" : "lattice", double(FeatureFlags::v4_cpb()), gref,
	    double(FeatureFlags::v4_chart_budget_deg()), mipReq);
	std::fprintf(stderr,
	    "[V4-LATTICE] topo faces=%zu verts=%zu corners=%zu exact_classes=%zu edges=%zu "
	    "use1=%lld use2=%lld use3plus=%lld charts=%d meshes_with_stone=%d\n",
	    T.faces.size(), T.vpos.size(), T.corners, T.exactClasses, T.edges.size(),
	    (long long)T.use1, (long long)T.use2, (long long)T.use3plus, T.nCharts,
	    T.meshesWithStone);
	// ── the GRID itself, in the map's own texel coordinates ────────────────
	// The breaklines are only "aligned to the recesses" if the runs they come
	// from are.  P3b's look defect (verdict 41112ea861bd) is about exactly
	// that, so the grid the lattice was built from is dumped rather than
	// described: every horizontal run, every band's y-range, and every band's
	// vertical runs, so a reader can check the running-bond offset between two
	// bands instead of assuming it.  Census-only; nothing reads it at runtime.
	if (census) for (int m = 0; m < nMats; ++m) {
		const MatGrid &G = grids[size_t(m)];
		if (!G.ok) continue;
		std::string hs;
		for (const StoneGRun &r : G.grid.h) {
			char b[64]; std::snprintf(b, sizeof(b), "%.3f-%.3f ", double(r.lo), double(r.hi));
			hs += b;
		}
		std::fprintf(stderr, "[V4-GRID] mat=%s mip=%d map=%dx%d pitch=%.3fx%.3f hruns=%s\n",
		             matNames[m], G.useMip, G.mipW, G.mipH,
		             double(G.pitchX), double(G.pitchY), hs.c_str());
		for (size_t b = 0; b < G.grid.vPerBand.size(); ++b) {
			std::string vs;
			for (const StoneGRun &r : G.grid.vPerBand[b]) {
				char t[80];
				std::snprintf(t, sizeof(t), "%.3f-%.3f(c%.3f) ",
				              double(r.lo), double(r.hi), 0.5*(double(r.lo)+double(r.hi)));
				vs += t;
			}
			const double y0 = b < G.grid.bandY.size() ? double(G.grid.bandY[b].first)  : -1.0;
			const double y1 = b < G.grid.bandY.size() ? double(G.grid.bandY[b].second) : -1.0;
			std::fprintf(stderr, "[V4-GRID] mat=%s band=%zu y=%.3f..%.3f vruns=%s\n",
			             matNames[m], b, y0, y1, vs.c_str());
		}
		for (size_t r = 0; r < G.rowTpl.size(); ++r)
			std::fprintf(stderr, "[V4-GRID] mat=%s row=%zu y=%.3f..%.3f type=%d bandA=%d bandB=%d\n",
			             matNames[m], r, double(G.rowTpl[r].y0), double(G.rowTpl[r].y1),
			             G.rowTpl[r].type, G.rowTpl[r].bandA, G.rowTpl[r].bandB);
	}
	for (int m = 0; m < nMats; ++m) {
		const MatGrid &G = grids[size_t(m)];
		std::fprintf(stderr,
		    "[V4-LATTICE] MAT name=%s ok=%d mip=%d map=%dx%d pitch=%.1fx%.1f have_pitch=%d "
		    "target_tex=%.2fx%.2f grid_valid=%d hgrooves=%zu bands=%zu rows=%zu\n",
		    matNames[m], int(G.ok), G.useMip, G.mipW, G.mipH, double(G.pitchX), double(G.pitchY),
		    int(G.havePitch), G.targetTexX, G.targetTexY, int(G.grid.valid),
		    G.grid.h.size(), G.grid.bandY.size(), G.rowTpl.size());
	}
	std::fprintf(stderr,
	    "[V4-LATTICE] borders edges=%zu seg_min=%d seg_max=%d interior_samples=%lld "
	    "capped=%lld max_dev_from_line=%.3e\n",
	    T.edges.size(), S.borderMin, S.borderMax, (long long)S.borderSamples,
	    (long long)S.borderCapped, S.borderMaxDev);
	std::fprintf(stderr,
	    "[V4-LATTICE] nodes generated=%lld kept=%lld outside=%lld margin=%lld capped=%lld "
	    "plateau_min_level0_texels=%.3f level_jump_violations=%lld\n",
	    (long long)S.nodesGen, (long long)S.nodesKept, (long long)S.nodesOutside,
	    (long long)S.nodesMargin, (long long)S.nodesCapped,
	    S.plateauMinLevel0 > 1e29 ? -1.0 : S.plateauMinLevel0,
	    (long long)S.levelJumpViolations);
	std::fprintf(stderr,
	    "[V4-LATTICE] triangulation degenerate_uv=%lld delaunay_fallback=%lld tris=%lld "
	    "row_cap_faces=%lld ny_cap=%lld nx_cap=%lld cell_world_min=%.5f cell_world_max=%.5f "
	    "ms_topo=%.2f ms_grid=%.2f ms_lattice=%.2f ms_commit=%.2f ms_relief_census=%.2f\n",
	    (long long)S.degenerateUV, (long long)S.delaunayFail, (long long)S.triEmitted,
	    (long long)S.rowCapFaces, (long long)S.nyCap, (long long)S.nxCap,
	    S.minNodeSpacing > 1e29 ? -1.0 : S.minNodeSpacing, S.maxNodeSpacing,
	    S.msTopo, S.msGrid, S.msLattice - S.msRelief, S.msCommit, S.msRelief);

	std::fprintf(stderr,
	    "[V4-DISPLACE] arm amp=%.6f pyramid=%d pyr_radius_tex=%.3f pyr_rad_used=%d "
	    "chart_maxdev_deg=%.6f\n",
	    amp, int(FeatureFlags::v4_pyramid()), double(FeatureFlags::v4_pyr_radius_tex()),
	    grids[0].pyrRad, chartMaxDevDeg);
	std::fprintf(stderr,
	    "[V4-DISPLACE] nodes moved=%lld groove=%lld bevel=%lld plateau=%lld pinned=%lld "
	    "d_min=%+.6f d_max=%+.6f\n",
	    (long long)(nodeCls[0]+nodeCls[1]+nodeCls[2]), (long long)nodeCls[0],
	    (long long)nodeCls[1], (long long)nodeCls[2],
	    (long long)(nPinnedCorners + int64_t(borderVert.size())),
	    dMinSeen > 1e299 ? 0.0 : dMinSeen, dMaxSeen < -1e299 ? 0.0 : dMaxSeen);
	for (int m = 0; m < nMats; ++m)
		std::fprintf(stderr,
		    "[V4-DISPLACE] FIELD name=%s field_ok=%d mip_mean=%.6f h=[%.4f..%.4f] "
		    "d=[%+.6f..%+.6f]\n",
		    matNames[m], int(grids[size_t(m)].fieldOk), grids[size_t(m)].mipMean,
		    grids[size_t(m)].h0, grids[size_t(m)].h1,
		    amp * (grids[size_t(m)].h0 - grids[size_t(m)].mipMean),
		    amp * (grids[size_t(m)].h1 - grids[size_t(m)].mipMean));
	// ═══ [V4-RINGS] — design §2e, measured on the mesh that shipped ════════
	// Printed in EVERY arm (not only under --v4_census): the ring solve is what
	// P4 is, and its residual is the invariant §2e names — every ring vertex on
	// both offset planes to 1e-5 u.  `held` is the population P4 does NOT move
	// and why, which is the honest form of "the wall boundaries now recess".
	std::fprintf(stderr,
	    "[V4-RINGS] arm rings=%d grooves=%d cross_both=%d abut=%d mitre_limit=%.2f min_sep=%.3f "
	    "soup_tris=%lld ms=%.3f\n",
	    int(rings), int(ringGrooves), int(crossBoth), int(ringAbut), mitreLimit, minSepFrac,
	    (long long)RS.soupTris, RS.msRings);
	std::fprintf(stderr,
	    "[V4-RINGS] corners solved=%lld held_abut=%lld held_soup=%lld no_field=%lld "
	    "of=%zu\n",
	    (long long)RS.cornersSolved, (long long)RS.cornersHeldAbut,
	    (long long)RS.cornersHeldSoup, (long long)RS.cornersNoField, T.vpos.size());
	std::fprintf(stderr,
	    "[V4-RINGS] borders solved=%lld held_abut=%lld held_soup=%lld "
	    "cross_samples=%lld arc_samples=%lld arc_dropped=%lld edges_with_cross=%lld\n",
	    (long long)RS.bordersSolved, (long long)RS.bordersHeldAbut,
	    (long long)RS.bordersHeldSoup, (long long)RS.crossSamples,
	    (long long)RS.arcSamples, (long long)RS.arcDropped, (long long)RS.edgesWithCross);
	std::fprintf(stderr,
	    "[V4-RINGS] solve planes1=%lld planes2=%lld planes3=%lld planes4plus=%lld "
	    "mitre_clamped=%lld singular=%lld ill_cond=%lld max_residual=%.3e "
	    "max_residual_over=%.3e max_move=%.6f d=[%+.6f..%+.6f]\n",
	    (long long)RS.planes1, (long long)RS.planes2, (long long)RS.planes3,
	    (long long)RS.planes4, (long long)RS.mitreClamped, (long long)RS.singular,
	    (long long)RS.illCond, RS.maxResidual, RS.maxResidualOver, RS.maxMove,
	    RS.dMin > 1e299 ? 0.0 : RS.dMin, RS.dMax < -1e299 ? 0.0 : RS.dMax);
	for (size_t q = 0; q < holdMats.size(); ++q)
		std::fprintf(stderr, "[V4-RINGS] heldby mat=%s verts=%lld\n",
		             holdMats[q].first.c_str(), (long long)holdMats[q].second);

	if (!census) {
		std::fprintf(stderr,
		    "[V4-OUT] mesh verts=%d faces=%d stone_faces=%lld (add --v4_census for the "
		    "watertightness / T-vertex / sliver census)\n",
		    int(M->VIndex), int(M->FIndex), (long long)stoneFaces);
		std::fflush(stderr);
		return;
	}

	// ═══ [V4-RELIEF] — design §2d, e−r along the plane normal per class ═════
	if (reliefCensus) {
		static const char *kClsName[3] = { "groove", "bevel", "plateau" };
		auto qOf = [](std::vector<float> &v, double q) -> double {
			if (v.empty()) return 0.0;
			std::sort(v.begin(), v.end());
			return double(v[size_t(q * double(v.size() - 1))]);
		};
		// scene totals, merged out of the per-plane bins
		std::array<ReliefBin,3> tot;
		for (auto &kv : RC.byPlane)
			for (int c = 0; c < 3; ++c) {
				tot[size_t(c)].emr.insert(tot[size_t(c)].emr.end(),
				    kv.second[size_t(c)].emr.begin(), kv.second[size_t(c)].emr.end());
				tot[size_t(c)].r.insert(tot[size_t(c)].r.end(),
				    kv.second[size_t(c)].r.begin(), kv.second[size_t(c)].r.end());
				tot[size_t(c)].emrCore.insert(tot[size_t(c)].emrCore.end(),
				    kv.second[size_t(c)].emrCore.begin(), kv.second[size_t(c)].emrCore.end());
			}
		std::fprintf(stderr,
		    "[V4-RELIEF] arm amp=%.6f order=%d samples=%lld core_samples=%lld planes=%zu "
		    "tol=0.005 min_n=200\n",
		    amp, kReliefOrder, (long long)RC.samples, (long long)RC.coreSamples,
		    RC.byPlane.size());
		for (int c = 0; c < 3; ++c) {
			ReliefBin &b = tot[size_t(c)];
			std::fprintf(stderr,
			    "[V4-RELIEF] class name=%s n=%zu emr_p50=%+.5f emr_p10=%+.5f emr_p90=%+.5f "
			    "r_p50=%+.5f core_n=%zu core_emr_p50=%+.5f core_emr_p10=%+.5f "
			    "core_emr_p90=%+.5f\n",
			    kClsName[c], b.emr.size(), qOf(b.emr, 0.50), qOf(b.emr, 0.10),
			    qOf(b.emr, 0.90), qOf(b.r, 0.50), b.emrCore.size(),
			    qOf(b.emrCore, 0.50), qOf(b.emrCore, 0.10), qOf(b.emrCore, 0.90));
		}
		{
			static const char *kG[3] = { "groove", "bevel", "plateau" };
			for (int rc = 0; rc < 3; ++rc) for (int gc = 0; gc < 3; ++gc) {
				std::vector<float> &v = RC.cross[size_t(rc)][size_t(gc)];
				if (v.empty()) continue;
				std::fprintf(stderr, "[V4-RELIEF] cross rclass=%s gclass=%s n=%zu "
				             "emr_p50=%+.5f emr_p10=%+.5f emr_p90=%+.5f\n",
				             kClsName[rc], kG[gc], v.size(), qOf(v,0.50), qOf(v,0.10), qOf(v,0.90));
			}
			std::sort(RC.worst.begin(), RC.worst.end(),
			          [](const ReliefCensus::Worst &a, const ReliefCensus::Worst &b)
			          { return std::fabs(a.emr) > std::fabs(b.emr); });
			for (size_t i = 0; i < RC.worst.size() && i < 10; ++i)
				std::fprintf(stderr, "[V4-RELIEF] WORST i=%zu emr=%+.5f e=%+.5f r=%+.5f "
				             "u=%.5f v=%.5f face=%d rclass=%s gclass=%s "
				             "T0=%.5f,%.5f,%+.5f,h%.4f,c%d T1=%.5f,%.5f,%+.5f,h%.4f,c%d "
				             "T2=%.5f,%.5f,%+.5f,h%.4f,c%d\n", i,
				             RC.worst[i].emr, RC.worst[i].e, RC.worst[i].r,
				             RC.worst[i].u, RC.worst[i].v, RC.worst[i].face,
				             kClsName[RC.worst[i].rc], kG[RC.worst[i].gc],
				             RC.worst[i].tu[0], RC.worst[i].tv[0], RC.worst[i].td[0],
				             RC.worst[i].th[0], int(RC.worst[i].tc[0]),
				             RC.worst[i].tu[1], RC.worst[i].tv[1], RC.worst[i].td[1],
				             RC.worst[i].th[1], int(RC.worst[i].tc[1]),
				             RC.worst[i].tu[2], RC.worst[i].tv[2], RC.worst[i].td[2],
				             RC.worst[i].th[2], int(RC.worst[i].tc[2]));
		}

		// per PLANE: the design's invariant is "within ±0.005 u on EVERY plane",
		// so the row that matters is the count of planes that miss it, and the
		// worst one is named so the next round can go and look at it.
		int overAll[3] = {0,0,0}, overCore[3] = {0,0,0}, nPlanes[3] = {0,0,0};
		double worstAll[3] = {0,0,0}, worstCore[3] = {0,0,0};
		uint32_t worstKey[3] = {0,0,0};
		for (auto &kv : RC.byPlane) {
			std::fprintf(stderr, "[V4-RELIEF] PLANE mat=%s ord=%u",
			             matNames[size_t(kv.first >> 16)], unsigned(kv.first & 0xFFFF));
			for (int c = 0; c < 3; ++c) {
				ReliefBin &b = kv.second[size_t(c)];
				const double pa = qOf(b.emr, 0.50), pc = qOf(b.emrCore, 0.50);
				std::fprintf(stderr, " %s_n=%zu %s_emr_p50=%+.5f %s_core_n=%zu "
				             "%s_core_emr_p50=%+.5f",
				             kClsName[c], b.emr.size(), kClsName[c], pa,
				             kClsName[c], b.emrCore.size(), kClsName[c], pc);
				if (b.emr.size() >= 200) {
					++nPlanes[c];
					if (std::fabs(pa) > 0.005) ++overAll[c];
					if (std::fabs(pa) > std::fabs(worstAll[c]))
						{ worstAll[c] = pa; worstKey[c] = kv.first; }
				}
				if (b.emrCore.size() >= 200) {
					if (std::fabs(pc) > 0.005) ++overCore[c];
					if (std::fabs(pc) > std::fabs(worstCore[c])) worstCore[c] = pc;
				}
			}
			std::fprintf(stderr, "\n");
		}
		for (int c = 0; c < 3; ++c)
			std::fprintf(stderr,
			    "[V4-RELIEF] planegate name=%s planes=%d over_tol=%d worst_emr_p50=%+.5f "
			    "worst_mat=%s worst_ord=%u core_over_tol=%d core_worst_emr_p50=%+.5f\n",
			    kClsName[c], nPlanes[c], overAll[c], worstAll[c],
			    nPlanes[c] ? matNames[size_t(worstKey[c] >> 16)] : "-",
			    unsigned(worstKey[c] & 0xFFFF), overCore[c], worstCore[c]);
	}

	// ═══ [V4-OUT] — the design's §2h invariants, measured on what shipped ═══
	const auto tC0 = clock::now();
	std::vector<int32_t> stoneTri;              // 3 vertex ids per stone triangle
	std::vector<int32_t> stoneMat;              // its material index
	std::vector<float>   stoneUV;               // its centroid u,v
	stoneTri.reserve(size_t(stoneFaces) * 3);
	for (size_t i = 0; i < outF.size(); ++i) {
		if (!nf[i].Txtr || !nf[i].Txtr->Name) continue;
		bool isStone = false;
		for (int m = 0; m < nMats; ++m)
			if (!std::strcmp(nf[i].Txtr->Name, matNames[m])) { isStone = true; break; }
		if (!isStone) continue;
		stoneTri.push_back(int32_t(outI[i][0]));
		stoneTri.push_back(int32_t(outI[i][1]));
		stoneTri.push_back(int32_t(outI[i][2]));
		int mi = 0;
		for (int m = 0; m < nMats; ++m)
			if (!std::strcmp(nf[i].Txtr->Name, matNames[m])) { mi = m; break; }
		stoneMat.push_back(mi);
		stoneUV.push_back((nf[i].U1 + nf[i].U2 + nf[i].U3) / 3.0f);
		stoneUV.push_back((nf[i].V1 + nf[i].V2 + nf[i].V3) / 3.0f);
	}
	const size_t nST = stoneTri.size() / 3;

	// ── use-count census over the stone output ─────────────────────────────
	std::map<std::pair<int32_t,int32_t>, int32_t> outEdge;
	for (size_t t = 0; t < nST; ++t)
		for (int k = 0; k < 3; ++k) {
			const int32_t a = stoneTri[t*3 + size_t(k)], b = stoneTri[t*3 + size_t((k+1)%3)];
			outEdge[{std::min(a,b), std::max(a,b)}]++;
		}
	int64_t oUse1 = 0, oUse2 = 0, oUse3 = 0;
	for (const auto &kv : outEdge) {
		if      (kv.second == 1) ++oUse1;
		else if (kv.second == 2) ++oUse2;
		else                     ++oUse3;
	}
	// the boundary the design allows: the 130 authored use-1 abutments, each cut
	// into its own nSeg sub-edges
	int64_t expectedBoundary = 0;
	for (const P2Edge &E : T.edges) if (E.use == 1) expectedBoundary += E.nSeg;

	// ── T-vertex census: a vertex in the INTERIOR of an edge it does not end ──
	int64_t tVerts = 0; int64_t tByClass[5] = {0,0,0,0,0};
	{
		std::vector<int32_t> vids;
		vids.reserve(stoneTri.size());
		for (int32_t v : stoneTri) vids.push_back(v);
		std::sort(vids.begin(), vids.end());
		vids.erase(std::unique(vids.begin(), vids.end()), vids.end());
		const double cell = 1.0;
		auto ckey = [&](double x, double y, double z) -> int64_t {
			const int64_t i = int64_t(std::floor(x/cell)), j = int64_t(std::floor(y/cell)),
			              k = int64_t(std::floor(z/cell));
			return (i * 73856093LL) ^ (j * 19349663LL) ^ (k * 83492791LL);
		};
		std::unordered_map<int64_t, std::vector<std::pair<int32_t,int32_t>>> grid;
		for (const auto &kv : outEdge) {
			const Vector &A = nv[kv.first.first].Pos, &B = nv[kv.first.second].Pos;
			const int64_t i0 = int64_t(std::floor(std::min(A.x,B.x)/cell)), i1 = int64_t(std::floor(std::max(A.x,B.x)/cell));
			const int64_t j0 = int64_t(std::floor(std::min(A.y,B.y)/cell)), j1 = int64_t(std::floor(std::max(A.y,B.y)/cell));
			const int64_t k0 = int64_t(std::floor(std::min(A.z,B.z)/cell)), k1 = int64_t(std::floor(std::max(A.z,B.z)/cell));
			if ((i1-i0+1)*(j1-j0+1)*(k1-k0+1) > 4096) continue;      // pathological, skipped
			for (int64_t i = i0; i <= i1; ++i) for (int64_t j = j0; j <= j1; ++j) for (int64_t k = k0; k <= k1; ++k)
				grid[(i*73856093LL) ^ (j*19349663LL) ^ (k*83492791LL)].push_back(kv.first);
		}
		const double tol = 1e-4;
		for (int32_t v : vids) {
			const Vector &P = nv[v].Pos;
			auto it = grid.find(ckey(P.x, P.y, P.z));
			if (it == grid.end()) continue;
			bool hit = false;
			for (const auto &e : it->second) {
				if (e.first == v || e.second == v) continue;
				const Vector &A = nv[e.first].Pos, &B = nv[e.second].Pos;
				const double ex2 = B.x-A.x, ey2 = B.y-A.y, ez2 = B.z-A.z;
				const double L2 = ex2*ex2 + ey2*ey2 + ez2*ez2;
				if (L2 < 1e-18) continue;
				const double s = ((P.x-A.x)*ex2 + (P.y-A.y)*ey2 + (P.z-A.z)*ez2) / L2;
				if (s <= 1e-9 || s >= 1.0 - 1e-9) continue;
				const double dx = P.x - (A.x + ex2*s), dy = P.y - (A.y + ey2*s), dz = P.z - (A.z + ez2*s);
				if (dx*dx + dy*dy + dz*dz < tol*tol) { hit = true; break; }
			}
			if (hit) { ++tVerts; tByClass[size_t(vClass[size_t(v)] % 5)]++; }
		}
	}

	// ── sliver census ──────────────────────────────────────────────────────
	// abutment-adjacency: a triangle carrying BOTH endpoints of an authored
	// use-1 edge sits on an unsplit abutment (the fan that pinning them costs)
	std::set<std::pair<int32_t,int32_t>> abutPair;
	for (const P2Edge &E : T.edges) {
		if (E.use != 1) continue;
		const int32_t ga = T.vGlobal[size_t(E.a)], gb = T.vGlobal[size_t(E.b)];
		if (ga >= 0 && gb >= 0) abutPair.insert({ std::min(ga,gb), std::max(ga,gb) });
	}
	std::vector<double> minAng, minAngOther;
	minAng.reserve(nST);
	int64_t under1 = 0, under2 = 0, degenerate = 0;
	int64_t u2band = 0, u2abut = 0, u2other = 0, u1band = 0, u1abut = 0, u1other = 0;
	struct Worst { double ang; double x, y, z; int cls; int32_t v[3]; };
	std::vector<Worst> worst;
	for (size_t t = 0; t < nST; ++t) {
		const Vector &A = nv[stoneTri[t*3+0]].Pos, &B = nv[stoneTri[t*3+1]].Pos, &C = nv[stoneTri[t*3+2]].Pos;
		const V3 a(double(A.x),double(A.y),double(A.z)), b(double(B.x),double(B.y),double(B.z)),
		         c(double(C.x),double(C.y),double(C.z));
		const double la = len(c-b), lb = len(a-c), lc = len(b-a);
		if (la < 1e-9 || lb < 1e-9 || lc < 1e-9) { ++degenerate; continue; }
		double m = 180.0;
		m = std::min(m, angDeg(nrm(b-a), nrm(c-a)));
		m = std::min(m, angDeg(nrm(a-b), nrm(c-b)));
		m = std::min(m, angDeg(nrm(a-c), nrm(b-c)));
		minAng.push_back(m);
		int cls = 2;                                          // 0 band, 1 abut, 2 other
		if (InGrooveBand(grids[size_t(stoneMat[t])], double(stoneUV[t*2]), double(stoneUV[t*2+1])))
			cls = 0;
		else {
			for (int k = 0; k < 3 && cls == 2; ++k) {
				const int32_t p = stoneTri[t*3 + size_t(k)], q = stoneTri[t*3 + size_t((k+1)%3)];
				if (abutPair.count({ std::min(p,q), std::max(p,q) })) cls = 1;
			}
		}
		if (cls == 2) minAngOther.push_back(m);
		if (m < 2.0) { if (cls==0) ++u2band; else if (cls==1) ++u2abut; else ++u2other; ++under2; }
		if (m < 1.0) { if (cls==0) ++u1band; else if (cls==1) ++u1abut; else ++u1other; ++under1;
			worst.push_back({ m, (a.x+b.x+c.x)/3, (a.y+b.y+c.y)/3, (a.z+b.z+c.z)/3, cls,
			                  { stoneTri[t*3+0], stoneTri[t*3+1], stoneTri[t*3+2] } }); }
	}
	std::sort(minAng.begin(), minAng.end());
	std::sort(minAngOther.begin(), minAngOther.end());
	std::sort(worst.begin(), worst.end(), [](const Worst &x, const Worst &y){ return x.ang < y.ang; });
	auto pct = [&](double q) -> double {
		if (minAng.empty()) return -1.0;
		return minAng[size_t(q * double(minAng.size() - 1))];
	};

	// ── per-chart face density (design §3's corner-column debt: the old bake
	// fans 73 faces/u² at the x=17.898 column against 11 on the wall) ───────
	double densMin = 1e300, densMax = 0.0; int32_t densMaxChart = -1;
	{
		std::vector<double> chArea(size_t(T.nCharts), 0.0);
		for (const P2Face &F : T.faces) chArea[size_t(F.chart)] += F.area;
		for (size_t c = 0; c < chArea.size(); ++c) {
			if (chArea[c] <= 1e-9 || chartTris[c] == 0) continue;
			const double d = double(chartTris[c]) / chArea[c];
			densMin = std::min(densMin, d);
			if (d > densMax) { densMax = d; densMaxChart = int32_t(c); }
		}
	}

	const auto tC1 = clock::now();
	S.msCensus = std::chrono::duration<double, std::milli>(tC1 - tC0).count();
	double stoneArea = 0.0;
	for (const P2Face &F : T.faces) stoneArea += F.area;
	std::fprintf(stderr,
	    "[V4-OUT] mesh verts=%d faces=%d stone_faces=%zu stone_area=%.4f faces_per_u2=%.3f\n",
	    int(M->VIndex), int(M->FIndex), nST, stoneArea,
	    stoneArea > 0 ? double(nST)/stoneArea : 0.0);
	std::fprintf(stderr,
	    "[V4-OUT] usecount edges=%zu use1=%lld use2=%lld use3plus=%lld expected_use1=%lld "
	    "authored_abutments=%lld\n",
	    outEdge.size(), (long long)oUse1, (long long)oUse2, (long long)oUse3,
	    (long long)expectedBoundary, (long long)T.use1);
	std::fprintf(stderr,
	    "[V4-OUT] tv count=%lld tol=1.0e-4 corner=%lld abut_sample=%lld "
	    "border_sample=%lld interior=%lld authored=%lld\n",
	    (long long)tVerts, (long long)tByClass[0], (long long)tByClass[1],
	    (long long)tByClass[2], (long long)tByClass[3], (long long)tByClass[4]);
	auto pctO = [&](double q) -> double {
		if (minAngOther.empty()) return -1.0;
		return minAngOther[size_t(q * double(minAngOther.size() - 1))];
	};
	std::fprintf(stderr,
	    "[V4-OUT] slivers n=%zu minang_min=%.4f p10=%.4f p50=%.4f under1deg=%lld "
	    "under2deg=%lld degenerate=%lld\n",
	    minAng.size(), minAng.empty()?-1.0:minAng.front(), pct(0.10), pct(0.50),
	    (long long)under1, (long long)under2, (long long)degenerate);
	std::fprintf(stderr,
	    "[V4-OUT] sliverclass u2band=%lld u2abut=%lld u2other=%lld u1band=%lld "
	    "u1abut=%lld u1other=%lld other_n=%zu other_min=%.4f other_p10=%.4f other_p50=%.4f\n",
	    (long long)u2band, (long long)u2abut, (long long)u2other,
	    (long long)u1band, (long long)u1abut, (long long)u1other,
	    minAngOther.size(), minAngOther.empty()?-1.0:minAngOther.front(),
	    pctO(0.10), pctO(0.50));
	for (size_t i = 0; i < worst.size() && i < 6; ++i)
		std::fprintf(stderr, "[V4-OUT] WORST i=%zu minang=%.5f class=%s ctr=%.4f,%.4f,%.4f "
		    "A=%.6f,%.6f,%.6f/%d B=%.6f,%.6f,%.6f/%d C=%.6f,%.6f,%.6f/%d\n",
		    i, worst[i].ang, worst[i].cls==0?"band":(worst[i].cls==1?"abut":"other"),
		    worst[i].x, worst[i].y, worst[i].z,
		    double(nv[worst[i].v[0]].Pos.x), double(nv[worst[i].v[0]].Pos.y), double(nv[worst[i].v[0]].Pos.z), int(vClass[size_t(worst[i].v[0])]),
		    double(nv[worst[i].v[1]].Pos.x), double(nv[worst[i].v[1]].Pos.y), double(nv[worst[i].v[1]].Pos.z), int(vClass[size_t(worst[i].v[1])]),
		    double(nv[worst[i].v[2]].Pos.x), double(nv[worst[i].v[2]].Pos.y), double(nv[worst[i].v[2]].Pos.z), int(vClass[size_t(worst[i].v[2])]));
	std::fprintf(stderr,
	    "[V4-OUT] density charts=%d faces_per_u2_min=%.3f faces_per_u2_max=%.3f ratio=%.3f "
	    "worst_chart=%d census_ms=%.2f\n",
	    T.nCharts, densMin > 1e299 ? -1.0 : densMin, densMax,
	    (densMin > 1e299 || densMin <= 0) ? -1.0 : densMax/densMin, densMaxChart, S.msCensus);
	std::fflush(stderr);
}


// ═══════════════════════════════════════════════════════════════════════════
//  §WorldUV (--v4_world_uv) — the bake-time per-plane WORLD-SPACE re-UV
//
//  His ruling e54d2bd9b654 ("I think we should try the tri-planar idea?"), in
//  the only form a software rasterizer can afford: runtime triplanar blending
//  is 3× texture fetches in the filler's hot loop and is priced off the table,
//  while a bake-time re-UV costs nothing per frame.  Each stone face's
//  authored UV is replaced by the planar projection of its plane's DOMINANT
//  WORLD AXIS, and everything downstream then moves together BY CONSTRUCTION:
//
//    * the ALBEDO the fillers sample is Face::U1..V3 (FRUSTRUM.CPP:1268 stamps
//      it onto the clipped verts every frame — the mesh's Vertex::U/V is dead
//      for rasterization), which RunP2Bake writes from the lattice point's
//      (u, v);
//    * the HEIGHT that displaces the lattice is NodeH(G, pt.u, pt.v) — the
//      SAME (u, v), read out of the same map.  There is one UV and both read
//      it, so the grooves cannot carve anywhere but where the mortar is drawn;
//    * the BREAKLINES the lattice is cut on are the height map's own texel
//      grid, i.e. they live in that same UV space (design §P2: "the lattice is
//      defined per FACE, not per chart … the grid lines live in the height
//      map's own texel space").
//
//  which is exactly why the arm bears on 41112ea861bd, his "the polygons are
//  not aligned to the recesses": today every authored face carries its own
//  affine UV map, so one map texel lands at a DIFFERENT world point on each
//  face and the lattice cannot continue across a face boundary.  Under one
//  projection per world axis the lattice is a single GLOBAL grid in world
//  space, and the 1227.7 u of SAME-PLANE junction — 64.5 % of all stone
//  junction length (e3e93ecd9546) — is dissolved.  The other 35.5 % is true
//  dihedral corners; no projection fixes those and this one does not try.
//
//  THE RULE, stated so it can be refuted:
//
//    axis   = argmax(|n.x|, |n.y|, |n.z|), ties to the LOWER index (x > y > z)
//    pair   = axis 0 (±X wall)  → (u, v) ← (world Z, world Y)
//             axis 1 (±Y floor) → (u, v) ← (world X, world Z)
//             axis 2 (±Z wall)  → (u, v) ← (world X, world Y)
//    u = S·sgn[j₁]·w[j₁] + O[j₁]        v = S·sgn[j₂]·w[j₂] + O[j₂]
//
//  World Y (up) drives V on BOTH wall axes on purpose: it phase-locks the
//  horizontal mortar courses THROUGH the dihedral corners the projection
//  cannot otherwise fix, so the courses at least run continuously in height
//  around a corner even where the vertical joints must break.
//
//  A near-45° plane is the rule's one genuinely arbitrary case: it takes the
//  lower-index axis and the texture stretches by 1/cos(tilt) along the tilted
//  direction.  That is not hidden — [V4-WORLDUV] prints max_tilt_deg and how
//  many planes sit past 40°, so "this scene has none" is a measurement.
//
//  S, sgn[] and O[] are ONE SET PER MATERIAL, FITTED from the authored UVs so
//  the drawn block size and phase stay where the author put them:
//    S      = median over the material's planes of the authored |du/dworld|
//    sgn[j] = the area-majority sign the authored UVs use on world axis j
//    O[j]   = the area-weighted CIRCULAR MEAN (period = one UV tile) of the
//             authored phases on world axis j
//  Nothing is hardcoded to this scene: another map at another tiling re-fits
//  to its own numbers, and the census prints every input and the cost
//  (scale_delta_pct, uv_shift) the fit could not absorb.
//
//  WHERE IT RUNS, and why there: DEMO/GREETS.CPP, at the top of the displace
//  block, BEFORE CaptureStoneProxySnapshot.  The UV-consumer sweep found
//  exactly one consumer that caches UVs across the bake — that snapshot
//  (GREETS.CPP:1621/1625 copy whole Vertex/Face structs), consumed by
//  BuildStoneProxyMesh to render the stone in the offscreen mirror-RTT and
//  env-probe passes.  Rewriting after it would leave the proxy albedo in the
//  dead coordinate system and the mirrors would show the OLD wall.  Every
//  other UV-derived table in greets init is rebuilt after this point:
//  Compute_Vertex_Tangents (via MakeFacesIndependentByAngle, and
//  unconditionally for rooms/floor via DisplaceStoneSmoothNormals),
//  PomShellDomains, the ::mirUV handedness split, PomShell_BuildPrism.  The
//  static shadow lightmap never reads this channel at all (barycentric /
//  world-planar atlas, StaticShadowLightmap.h:15) and Face::EU1..EV3 is
//  rebuilt per frame from the reflection vector (Transform.cpp:2914).
// ═══════════════════════════════════════════════════════════════════════════
namespace {

struct WUVFace {
	Face   *src = nullptr;
	V3      p[3];
	V3      n;
	double  area = 0;
	int     matIdx = 0;
};

struct WUVPlane {
	V3      n;
	double  d = 0;
	int     axis = 0;        // dominant world axis: 0=X 1=Y 2=Z
	int     matIdx = 0;
	double  area = 0;
	int64_t nFaces = 0;
	double  su = 0, ou = 0, ru = 0;   // authored u ≈ su·w[j₁] + ou, RMS resid ru
	double  sv = 0, ov = 0, rv = 0;
	double  ruSwap = 0, rvSwap = 0;   // the same fit against the SWAPPED pair
	double  ctr[3] = {0,0,0};
	double  tiltDeg = 0;
	double  phU = 0, phV = 0;      // the authored phase under the global S/sgn
	double  dPhU = 0, dPhV = 0;    // and how far the global phase moves it, mod 1
	bool    ok = false;
};

// a UV offset is only defined modulo one tile: fold a difference into
// [-0.5, +0.5) tiles, which is the ONLY part of a shift that is visible.
static inline double WUVWrap1(double x)
{
	x -= std::floor(x);
	return (x >= 0.5) ? (x - 1.0) : x;
}

// (u, v) ← (w[j₁], w[j₂]) for each dominant axis.
static inline void WUVAxisPair(int axis, int &j1, int &j2)
{
	if      (axis == 0) { j1 = 2; j2 = 1; }   // ±X wall : u←Z, v←Y
	else if (axis == 1) { j1 = 0; j2 = 2; }   // ±Y floor: u←X, v←Z
	else                { j1 = 0; j2 = 1; }   // ±Z wall : u←X, v←Y
}

static inline double WUVWorld(const V3 &p, int j)
{
	return j == 0 ? p.x : (j == 1 ? p.y : p.z);
}

// least squares y = s·x + o; returns the RMS residual
static double WUVFit1(const std::vector<double> &x, const std::vector<double> &y,
                      double &s, double &o)
{
	const size_t n = x.size();
	if (n < 2) { s = 0; o = n ? y[0] : 0; return 0; }
	double sx = 0, sy = 0;
	for (size_t i = 0; i < n; ++i) { sx += x[i]; sy += y[i]; }
	const double mx = sx / double(n), my = sy / double(n);
	double sxx = 0, sxy = 0;
	for (size_t i = 0; i < n; ++i) {
		const double dx = x[i] - mx;
		sxx += dx * dx; sxy += dx * (y[i] - my);
	}
	s = (sxx > 1e-12) ? (sxy / sxx) : 0.0;
	o = my - s * mx;
	double r = 0;
	for (size_t i = 0; i < n; ++i) { const double e = y[i] - (s * x[i] + o); r += e * e; }
	return std::sqrt(r / double(n));
}

static double WUVMedian(std::vector<double> v)
{
	if (v.empty()) return 0.0;
	std::sort(v.begin(), v.end());
	const size_t m = v.size() / 2;
	return (v.size() & 1) ? v[m] : 0.5 * (v[m-1] + v[m]);
}

// ── the arm's PREMISE, measured directly on the data ──────────────────────
// A "same-plane seam your projection dissolves by construction" is only a
// seam if the two faces sharing that edge assign DIFFERENT UVs to the shared
// endpoints.  This walks every use-2 stone edge and reports exactly that, in
// world-units of junction length, split coplanar vs dihedral — run once
// BEFORE the rewrite and once AFTER, so the arm's own census says whether it
// had anything to dissolve.  Cross-checks e3e93ecd9546's planepair totals
// (1227.7 u coplanar of 1904.2 u) on the same face set.
static void WUVSeamCensus(const std::vector<WUVFace> &faces,
                          const std::vector<int32_t> &planeOf, const char *label)
{
	const size_t nF = faces.size();
	std::unordered_map<PKey, int32_t, PKeyH> vid;
	std::vector<int32_t> corner(nF * 3, 0);
	for (size_t f = 0; f < nF; ++f)
		for (int k = 0; k < 3; ++k) {
			const V3 &q = faces[f].p[k];
			const PKey key { bitsOf(float(q.x)), bitsOf(float(q.y)), bitsOf(float(q.z)) };
			auto it = vid.find(key);
			if (it == vid.end()) it = vid.emplace(key, int32_t(vid.size())).first;
			corner[f*3 + size_t(k)] = it->second;
		}
	std::map<std::pair<int32_t,int32_t>, std::vector<std::pair<int32_t,int>>> byEdge;
	for (size_t f = 0; f < nF; ++f)
		for (int k = 0; k < 3; ++k) {
			const int32_t a = corner[f*3+size_t(k)], b = corner[f*3+size_t((k+1)%3)];
			byEdge[{std::min(a,b), std::max(a,b)}].push_back({int32_t(f), k});
		}
	double lenCo = 0, lenDi = 0, brokenCo = 0, brokenDi = 0;
	double maxCo = 0, maxDi = 0;
	int64_t nCo = 0, nDi = 0, nBrokenCo = 0, nBrokenDi = 0;
	const double kTol = 1.0e-4;          // 1/40 of a 256-texel map texel
	for (const auto &kv : byEdge) {
		if (kv.second.size() != 2) continue;
		const int32_t f0 = kv.second[0].first, f1 = kv.second[1].first;
		const int     k0 = kv.second[0].second;
		const WUVFace &A = faces[size_t(f0)], &B = faces[size_t(f1)];
		const double L = len(A.p[size_t((k0+1)%3)] - A.p[size_t(k0)]);
		const bool co = (planeOf[size_t(f0)] == planeOf[size_t(f1)]);
		// the largest UV disagreement at either shared endpoint
		const float au[3] = { A.src->U1, A.src->U2, A.src->U3 };
		const float av[3] = { A.src->V1, A.src->V2, A.src->V3 };
		const float bu[3] = { B.src->U1, B.src->U2, B.src->U3 };
		const float bv[3] = { B.src->V1, B.src->V2, B.src->V3 };
		double dmax = 0;
		for (int ka = 0; ka < 3; ++ka) {
			const int32_t va = corner[size_t(f0)*3 + size_t(ka)];
			if (va != kv.first.first && va != kv.first.second) continue;
			for (int kb = 0; kb < 3; ++kb) {
				if (corner[size_t(f1)*3 + size_t(kb)] != va) continue;
				dmax = std::max(dmax, std::fabs(double(au[ka]) - double(bu[kb])));
				dmax = std::max(dmax, std::fabs(double(av[ka]) - double(bv[kb])));
			}
		}
		if (co) { ++nCo; lenCo += L; maxCo = std::max(maxCo, dmax);
		          if (dmax > kTol) { ++nBrokenCo; brokenCo += L; } }
		else    { ++nDi; lenDi += L; maxDi = std::max(maxDi, dmax);
		          if (dmax > kTol) { ++nBrokenDi; brokenDi += L; } }
	}
	std::fprintf(stderr,
	    "[V4-WORLDUV] seams %s coplanar_edges=%lld coplanar_len=%.4f "
	    "uv_broken_edges=%lld uv_broken_len=%.4f max_duv=%.6f | "
	    "dihedral_edges=%lld dihedral_len=%.4f uv_broken_edges=%lld "
	    "uv_broken_len=%.4f max_duv=%.6f (tol=%.1e)\n",
	    label, (long long)nCo, lenCo, (long long)nBrokenCo, brokenCo, maxCo,
	    (long long)nDi, lenDi, (long long)nBrokenDi, brokenDi, maxDi, kTol);
}

static void WUVCore(std::vector<WUVFace> &faces, const char *const *matNames,
                    int nMats, bool census, int64_t &rewritten)
{
	rewritten = 0;
	const size_t nF = faces.size();
	if (!nF) return;

	// ── planes: the reference crease census's OWN identity (0.5° / 5 mm), the
	// one e3e93ecd9546's 52-plane count was measured with, so the census here
	// and the junction census there speak about the same objects ───────────
	std::vector<WUVPlane> planes;
	std::vector<int32_t>  planeOf(nF, -1);
	for (size_t f = 0; f < nF; ++f) {
		const WUVFace &F = faces[f];
		const double d = dot(F.n, F.p[0]);
		int32_t pid = -1;
		for (size_t i = 0; i < planes.size(); ++i)
			if (planes[i].matIdx == F.matIdx &&
			    dot(planes[i].n, F.n) > 0.99996 && std::fabs(planes[i].d - d) < 0.005) {
				pid = int32_t(i); break;
			}
		if (pid < 0) {
			WUVPlane P; P.n = F.n; P.d = d; P.matIdx = F.matIdx;
			const double ax = std::fabs(F.n.x), ay = std::fabs(F.n.y), az = std::fabs(F.n.z);
			double amax = ax; P.axis = 0;
			if (ay > amax) { amax = ay; P.axis = 1; }
			if (az > amax) { amax = az; P.axis = 2; }
			P.tiltDeg = std::acos(std::min(1.0, amax)) * 180.0 / 3.14159265358979323846;
			planes.push_back(P);
			pid = int32_t(planes.size()) - 1;
		}
		planeOf[f] = pid;
		planes[size_t(pid)].area   += F.area;
		planes[size_t(pid)].nFaces += 1;
	}

	// ── the per-plane authored fit, and the same fit with u/v swapped, so the
	// CONVENTION above is checked against the author rather than assumed ────
	std::vector<std::vector<double>> w1(planes.size()), w2(planes.size()),
	                                 uu(planes.size()), vv(planes.size());
	std::vector<double> cacc(planes.size() * 3, 0.0);
	for (size_t f = 0; f < nF; ++f) {
		const WUVFace &F = faces[f];
		const size_t p = size_t(planeOf[f]);
		int j1, j2; WUVAxisPair(planes[p].axis, j1, j2);
		const float u3[3] = { F.src->U1, F.src->U2, F.src->U3 };
		const float v3[3] = { F.src->V1, F.src->V2, F.src->V3 };
		for (int k = 0; k < 3; ++k) {
			w1[p].push_back(WUVWorld(F.p[k], j1));
			w2[p].push_back(WUVWorld(F.p[k], j2));
			uu[p].push_back(double(u3[k]));
			vv[p].push_back(double(v3[k]));
			cacc[p*3+0] += F.p[k].x; cacc[p*3+1] += F.p[k].y; cacc[p*3+2] += F.p[k].z;
		}
	}
	for (size_t p = 0; p < planes.size(); ++p) {
		WUVPlane &P = planes[p];
		P.ru = WUVFit1(w1[p], uu[p], P.su, P.ou);
		P.rv = WUVFit1(w2[p], vv[p], P.sv, P.ov);
		double s, o;
		P.ruSwap = WUVFit1(w2[p], uu[p], s, o);
		P.rvSwap = WUVFit1(w1[p], vv[p], s, o);
		const double inv = 1.0 / (3.0 * double(P.nFaces));
		P.ctr[0] = cacc[p*3+0] * inv;
		P.ctr[1] = cacc[p*3+1] * inv;
		P.ctr[2] = cacc[p*3+2] * inv;
		// a plane whose authored UV is a genuine in-plane ROTATION cannot be
		// fitted by an axis-aligned map at all; the residual is how the census
		// says so, and a zero scale is how this loop refuses to average it in
		P.ok = (std::fabs(P.su) > 1e-9 && std::fabs(P.sv) > 1e-9);
	}

	// ── per-material globals ───────────────────────────────────────────────
	struct MatFit {
		double  S = 0;
		double  sgn[3] = {1,1,1};
		double  O[3]   = {0,0,0};
		int64_t votes  = 0;
		double  scaleMin = 0, scaleMax = 0;
	};
	std::vector<MatFit> mf;
	mf.resize(size_t(nMats));
	const double TWO_PI = 6.283185307179586476925286766559;
	for (int m = 0; m < nMats; ++m) {
		std::vector<double> sc;
		double sgnAcc[3] = {0,0,0};
		for (const WUVPlane &P : planes) {
			if (P.matIdx != m || !P.ok) continue;
			sc.push_back(std::fabs(P.su));
			sc.push_back(std::fabs(P.sv));
			int j1, j2; WUVAxisPair(P.axis, j1, j2);
			sgnAcc[j1] += (P.su >= 0 ? 1.0 : -1.0) * P.area;
			sgnAcc[j2] += (P.sv >= 0 ? 1.0 : -1.0) * P.area;
		}
		if (sc.empty()) continue;
		MatFit &M = mf[size_t(m)];
		M.S = WUVMedian(sc);
		M.scaleMin = *std::min_element(sc.begin(), sc.end());
		M.scaleMax = *std::max_element(sc.begin(), sc.end());
		for (int j = 0; j < 3; ++j) M.sgn[j] = (sgnAcc[j] >= 0 ? 1.0 : -1.0);
		// the phase: an area-weighted CIRCULAR mean, because a UV offset is
		// only defined modulo one tile and a linear mean of 0.02 and 0.98 is
		// half a tile away from both
		double cs[3] = {0,0,0}, sn[3] = {0,0,0};
		for (WUVPlane &P : planes) {
			if (P.matIdx != m || !P.ok) continue;
			int j1, j2; WUVAxisPair(P.axis, j1, j2);
			// the offset this plane WOULD need, under the global scale and
			// sign, for its drawn phase at its own centroid to be unchanged
			const double phU = (P.su * P.ctr[j1] + P.ou) - M.S * M.sgn[j1] * P.ctr[j1];
			const double phV = (P.sv * P.ctr[j2] + P.ov) - M.S * M.sgn[j2] * P.ctr[j2];
			P.phU = phU; P.phV = phV;
			cs[j1] += P.area * std::cos(TWO_PI * phU);
			sn[j1] += P.area * std::sin(TWO_PI * phU);
			cs[j2] += P.area * std::cos(TWO_PI * phV);
			sn[j2] += P.area * std::sin(TWO_PI * phV);
			++M.votes;
		}
		for (int j = 0; j < 3; ++j)
			M.O[j] = (cs[j] != 0.0 || sn[j] != 0.0)
			       ? std::atan2(sn[j], cs[j]) / TWO_PI : 0.0;
	}

	// ── rewrite, and measure what the rewrite COST the authored look ───────
	if (census) WUVSeamCensus(faces, planeOf, "BEFORE");
	std::vector<double> dScalePct, dShiftU, dShiftV, dWrapU, dWrapV;
	for (size_t f = 0; f < nF; ++f) {
		WUVFace &F = faces[f];
		const WUVPlane &P = planes[size_t(planeOf[f])];
		const MatFit   &M = mf[size_t(F.matIdx)];
		if (M.S <= 0) continue;
		int j1, j2; WUVAxisPair(P.axis, j1, j2);
		float *pu[3] = { &F.src->U1, &F.src->U2, &F.src->U3 };
		float *pv[3] = { &F.src->V1, &F.src->V2, &F.src->V3 };
		for (int k = 0; k < 3; ++k) {
			const double nu = M.S * M.sgn[j1] * WUVWorld(F.p[k], j1) + M.O[j1];
			const double nv = M.S * M.sgn[j2] * WUVWorld(F.p[k], j2) + M.O[j2];
			dShiftU.push_back(std::fabs(nu - double(*pu[k])));
			dShiftV.push_back(std::fabs(nv - double(*pv[k])));
			dWrapU.push_back(std::fabs(WUVWrap1(nu - double(*pu[k]))));
			dWrapV.push_back(std::fabs(WUVWrap1(nv - double(*pv[k]))));
			*pu[k] = float(nu); *pv[k] = float(nv);
		}
		// keep Vertex::U/V in the SAME coordinate system.  It is dead for
		// rasterization (FRUSTRUM.CPP:1268 re-stamps from U1..V3 every frame),
		// but Compute_Vertex_Tangents and DisplaceStoneSmoothNormals both fall
		// back to it when a face's UV triangle is degenerate, and a fallback
		// into the dead authored space would build a tangent frame for a
		// texture that is no longer there.  A corner shared by two planes takes
		// one of the two — exactly as an authored UV seam does today.
		F.src->uvToVertices();
		++rewritten;
	}
	for (WUVPlane &P : planes) {
		if (!P.ok || mf[size_t(P.matIdx)].S <= 0) continue;
		const MatFit &M = mf[size_t(P.matIdx)];
		dScalePct.push_back(100.0 * (M.S / std::fabs(P.su) - 1.0));
		dScalePct.push_back(100.0 * (M.S / std::fabs(P.sv) - 1.0));
		int j1, j2; WUVAxisPair(P.axis, j1, j2);
		P.dPhU = WUVWrap1(M.O[j1] - P.phU);
		P.dPhV = WUVWrap1(M.O[j2] - P.phV);
	}

	if (!census) return;
	WUVSeamCensus(faces, planeOf, "AFTER ");

	for (int m = 0; m < nMats; ++m) {
		const MatFit &M = mf[size_t(m)];
		int64_t np = 0;
		for (const WUVPlane &P : planes) if (P.matIdx == m) ++np;
		std::fprintf(stderr,
		    "[V4-WORLDUV] mat='%s' planes=%lld fitted=%lld scale_uv_per_u=%.6f "
		    "authored_scale_min=%.6f authored_scale_max=%.6f "
		    "sgn=%+.0f,%+.0f,%+.0f phase=%.6f,%.6f,%.6f\n",
		    matNames[m], (long long)np, (long long)M.votes, M.S,
		    M.scaleMin, M.scaleMax, M.sgn[0], M.sgn[1], M.sgn[2],
		    M.O[0], M.O[1], M.O[2]);
	}
	int axisN[3] = {0,0,0}, near45 = 0, convWins = 0, swapWins = 0;
	double maxTilt = 0, maxResid = 0;
	for (const WUVPlane &P : planes) {
		axisN[P.axis]++;
		if (P.tiltDeg > 40.0) ++near45;
		maxTilt  = std::max(maxTilt,  P.tiltDeg);
		maxResid = std::max(maxResid, std::max(P.ru, P.rv));
		if (P.ru + P.rv <= P.ruSwap + P.rvSwap) ++convWins; else ++swapWins;
	}
	std::sort(dScalePct.begin(), dScalePct.end());
	std::sort(dShiftU.begin(),   dShiftU.end());
	std::sort(dShiftV.begin(),   dShiftV.end());
	auto pct = [](const std::vector<double> &v, double q) -> double {
		if (v.empty()) return 0.0;
		return v[size_t(q * double(v.size() - 1) + 0.5)];
	};
	std::fprintf(stderr,
	    "[V4-WORLDUV] planes=%zu faces_rewritten=%lld axis_x=%d axis_y=%d axis_z=%d "
	    "near45_planes=%d max_tilt_deg=%.4f max_fit_resid_uv=%.6f "
	    "convention_wins=%d swap_wins=%d\n",
	    planes.size(), (long long)rewritten, axisN[0], axisN[1], axisN[2],
	    near45, maxTilt, maxResid, convWins, swapWins);
	std::sort(dWrapU.begin(), dWrapU.end());
	std::sort(dWrapV.begin(), dWrapV.end());
	std::fprintf(stderr,
	    "[V4-WORLDUV] cost scale_delta_pct_p50=%.4f p90=%.4f max=%.4f "
	    "uv_shift_u_p50=%.6f max=%.6f uv_shift_v_p50=%.6f max=%.6f\n",
	    pct(dScalePct, 0.5), pct(dScalePct, 0.9),
	    dScalePct.empty() ? 0.0 : dScalePct.back(),
	    pct(dShiftU, 0.5), dShiftU.empty() ? 0.0 : dShiftU.back(),
	    pct(dShiftV, 0.5), dShiftV.empty() ? 0.0 : dShiftV.back());
	// The VISIBLE part of a shift.  A whole-tile shift redraws the identical
	// texture, so the number that says whether the drawn pattern moved is the
	// shift MODULO one tile — reported in tiles; the 'rooms' mortar pitch is
	// 64 texels of a 256-texel mip-2 map, i.e. one block = 0.25 tile.
	std::fprintf(stderr,
	    "[V4-WORLDUV] visible_shift_tiles u_p50=%.6f p90=%.6f max=%.6f "
	    "v_p50=%.6f p90=%.6f max=%.6f\n",
	    pct(dWrapU, 0.5), pct(dWrapU, 0.9), dWrapU.empty() ? 0.0 : dWrapU.back(),
	    pct(dWrapV, 0.5), pct(dWrapV, 0.9), dWrapV.empty() ? 0.0 : dWrapV.back());
	{   // how much of the stone AREA the near-45° tie-break rule governs
		double a45 = 0, aAll = 0;
		for (const WUVPlane &P : planes) { aAll += P.area; if (P.tiltDeg > 40.0) a45 += P.area; }
		std::fprintf(stderr,
		    "[V4-WORLDUV] near45 planes=%d area=%.4f of %.4f (%.4f%%)\n",
		    near45, a45, aAll, aAll > 0 ? 100.0 * a45 / aAll : 0.0);
	}
	for (size_t p = 0; p < planes.size(); ++p) {
		const WUVPlane &P = planes[p];
		std::fprintf(stderr,
		    "[V4-WORLDUV] PLANE id=%zu mat='%s' faces=%lld area=%.4f "
		    "n=%.6f,%.6f,%.6f d=%.4f axis=%d tilt=%.4f "
		    "su=%.6f sv=%.6f resid=%.6f,%.6f resid_swap=%.6f,%.6f "
		    "phase=%.6f,%.6f dphase=%.6f,%.6f ctr=%.3f,%.3f,%.3f\n",
		    p, matNames[P.matIdx], (long long)P.nFaces, P.area,
		    P.n.x, P.n.y, P.n.z, P.d, P.axis, P.tiltDeg,
		    P.su, P.sv, P.ru, P.rv, P.ruSwap, P.rvSwap,
		    P.phU, P.phV, P.dPhU, P.dPhV,
		    P.ctr[0], P.ctr[1], P.ctr[2]);
	}
	std::fflush(stderr);
}

}  // namespace

// ───────────────────────────────────────────────────────────────────────────
long long WorldUV_Apply(Scene *Sc, const char *const *matNames, int nMats)
{
	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	if (!Sc || !matNames || nMats <= 0) return 0;
	const bool census = FeatureFlags::v4_census();

	// The SAME face set the bake targets: material name match plus the old
	// bake's own guard, so a face the bake would skip keeps its authored UV
	// and the two sets cannot drift apart.
	std::vector<WUVFace> faces;
	for (TriMesh *M = Sc->TriMeshHead; M; M = M->Next) {
		if (M->FIndex == 0 || !M->Faces || !M->Verts) continue;
		for (dword i = 0; i < M->FIndex; ++i) {
			Face &F = M->Faces[i];
			if (!F.A || !F.B || !F.C || !F.Txtr || !F.Txtr->Name) continue;
			int mi = -1;
			for (int m = 0; m < nMats; ++m)
				if (!std::strcmp(F.Txtr->Name, matNames[m])) { mi = m; break; }
			if (mi < 0 || !OldBakeWouldBake(F.Txtr)) continue;
			WUVFace W;
			W.src = &F; W.matIdx = mi;
			W.p[0] = V3(F.A->Pos); W.p[1] = V3(F.B->Pos); W.p[2] = V3(F.C->Pos);
			const V3 gn = cross(W.p[1] - W.p[0], W.p[2] - W.p[0]);
			const double gl = len(gn);
			if (gl < 1e-12) continue;
			W.area = 0.5 * gl;
			W.n = gn * (1.0 / gl);
			const V3 eN = V3(F.N);
			if (len(eN) > 1e-9 && dot(W.n, nrm(eN)) < 0.0) W.n = W.n * -1.0;
			faces.push_back(W);
		}
	}
	int64_t rewritten = 0;
	WUVCore(faces, matNames, nMats, census, rewritten);
	const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
	std::fprintf(stderr, "[V4-WORLDUV] arm=ON faces_rewritten=%lld of %zu ms=%.3f\n",
	             (long long)rewritten, faces.size(), ms);
	std::fflush(stderr);
	return rewritten;
}

}  // namespace v4
}  // namespace fds
