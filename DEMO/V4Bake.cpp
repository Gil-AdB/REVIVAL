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

}  // namespace v4
}  // namespace fds
