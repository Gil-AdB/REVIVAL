// --greets_uv_seam_phase — see SeamPhase.h for the why. This file is the how.
//
// The model. Every authored stone patch (an edge-connected set of faces of
// one material that already share ONE UV mapping) carries an affine UV. Along
// a seam shared by patches P and Q, u_P and u_Q are both affine in the seam
// parameter; when the two have the same slope the difference u_P - u_Q is a
// constant along the seam, i.e. a pure PHASE, and a per-patch constant shift
// can zero it (mod 1 tile - a whole-tile shift redraws the identical texture).
// When the slopes differ (the sloped roof joints), no shift can, and the seam
// is reported as VARYING and left alone.
//
// Adjacency is by COLLINEAR OVERLAP, not by shared vertex indices: the authored
// stone is full of T-junctions (the door reveal's jamb edge runs door-height
// against the far wall's full-height edge; the spandrel strips abut the wall
// quad mid-edge), and a shared-edge rule saw none of them - measured on the
// first build (2026-09-02): the jamb was never a constraint, the reveal and
// the wall drifted apart, and two faces of one wall got different shifts. Two
// edges of different faces that lie on one line and overlap over a positive
// length are one seam over that overlap; each face's UV is evaluated at the
// overlap's ends by interpolation along its own edge.
//
// The solve, part 1 (offsets). Unknown o_P per patch. Each phase seam asks
//     (u_P + o_P) - (u_Q + o_Q) = 0  (mod 1)
// A BFS spanning tree from the largest patch of each component satisfies its
// tree seams exactly; the non-tree seams then carry each cycle's residual (a
// loop of walls whose perimeter is not a whole number of tiles cannot close).
// Gauss-Seidel sweeps with WRAPPED residuals then spread each loop's residual
// over its seams (weighted by overlap length) instead of leaving it on one.
//
// The solve, part 2 (scale, optional). What part 1 leaves is exactly the loop
// residual, and no offset can remove it. A per-patch u SCALE can: on a
// vertical corner u is constant along the seam, so scaling patch P's u by
// (1 + e_P) moves its phase at each of its corners by e_P * u_corner without
// introducing any along-seam variation, and a loop of perimeter L tiles closes
// when sum(e_P * extent_P) = -residual, i.e. e ~ residual / L ~ 1% (measured
// 2026-09-02: the door-pillar loop, 0.22 tile over ~15 tiles). Least squares:
//     minimise  sum over seam ends  len * (e_P u_P - e_Q u_Q + do_P - do_Q + res)^2
//             + kappa * sum over patches  area_P * e_P^2
// so that closing a loop is worth a ~1% scale but not a large one on a small
// patch. The census [STONE-UVSEAM] (--greets_displace_junction_census) and
// tools/uv_seam_census.py are the tests.
//
// What moves. Face::U1..U3 (the per-corner UV the rasteriser, the bake and the
// tangent build read), then Vertex::U/V re-stamped from them. v never moves;
// the UV determinant keeps its sign under a positive u scale, so handedness
// and tangent direction are what they were.

#include "SeamPhase.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace rev {
namespace {

struct V3 { double x, y, z; };
static V3 v3(const Vector &v) { return { v.x, v.y, v.z }; }
static V3 sub(const V3 &a, const V3 &b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
static V3 add(const V3 &a, const V3 &b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
static V3 mul(const V3 &a, double s) { return { a.x*s, a.y*s, a.z*s }; }
static V3 cross(const V3 &a, const V3 &b) { return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x }; }
static double dot(const V3 &a, const V3 &b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static double len(const V3 &a) { return std::sqrt(dot(a, a)); }

// signed wrap to (-0.5, 0.5]
static double wrapS(double d) { return d - std::round(d); }

struct PFace {
	Face    *F;
	int      mat;
	V3       p[3];
	double   u[3], v[3];
	V3       n;        // unit geometric normal (authored-facing)
	double   area;
	int      patch = -1;
};

struct Seam {
	int      f0, f1;   // faces
	int      p0, p1;   // patches
	V3       A, B;     // the overlap's ends (world)
	double   len;
	double   dih;      // degrees
	double   uP[2], uQ[2];   // raw u of f0 (P) and f1 (Q) at A and at B
	double   dA, dB;   // wrapS(u_f0 - u_f1) at A and at B
	double   dvA, dvB; // the same for v (a continuous seam has all four ~0)
	bool     phase;    // |dA - dB| small: a constant step along the seam
	double   d;        // the phase (mean of dA, dB) when phase
};

int findRoot(std::vector<int> &parent, int i) {
	while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
	return i;
}

// dense solve, Gaussian elimination with partial pivoting; n is at most a few hundred
static bool solveDense(std::vector<double> &N, std::vector<double> &g, int n) {
	for (int c = 0; c < n; ++c) {
		int piv = c;
		for (int r = c + 1; r < n; ++r) if (std::fabs(N[size_t(r)*n+c]) > std::fabs(N[size_t(piv)*n+c])) piv = r;
		if (std::fabs(N[size_t(piv)*n+c]) < 1e-14) return false;
		if (piv != c) {
			for (int k = 0; k < n; ++k) std::swap(N[size_t(c)*n+k], N[size_t(piv)*n+k]);
			std::swap(g[c], g[piv]);
		}
		for (int r = c + 1; r < n; ++r) {
			const double f = N[size_t(r)*n+c] / N[size_t(c)*n+c];
			if (f == 0.0) continue;
			for (int k = c; k < n; ++k) N[size_t(r)*n+k] -= f * N[size_t(c)*n+k];
			g[r] -= f * g[c];
		}
	}
	for (int c = n - 1; c >= 0; --c) {
		double s = g[c];
		for (int k = c + 1; k < n; ++k) s -= N[size_t(c)*n+k] * g[k];
		g[c] = s / N[size_t(c)*n+c];
	}
	return true;
}

}  // namespace

long long SeamPhaseUV_Apply(Scene *Sc, const char *const *mats, int nMats, int sweeps, int scaleTerm)
{
	if (!Sc || !mats || nMats <= 0) return 0;

	// ── 1. collect the target faces ────────────────────────────────────────
	std::vector<PFace> faces;
	for (TriMesh *M = Sc->TriMeshHead; M; M = M->Next) {
		if (M->FIndex == 0 || !M->Faces || !M->Verts) continue;
		for (DWord i = 0; i < M->FIndex; ++i) {
			Face &F = M->Faces[i];
			if (!F.A || !F.B || !F.C || !F.Txtr || !F.Txtr->Name) continue;
			int mi = -1;
			for (int m = 0; m < nMats; ++m)
				if (!std::strcmp(F.Txtr->Name, mats[m])) { mi = m; break; }
			if (mi < 0) continue;
			PFace P;
			P.F = &F; P.mat = mi;
			P.p[0] = v3(F.A->Pos); P.p[1] = v3(F.B->Pos); P.p[2] = v3(F.C->Pos);
			P.u[0] = F.U1; P.u[1] = F.U2; P.u[2] = F.U3;
			P.v[0] = F.V1; P.v[1] = F.V2; P.v[2] = F.V3;
			const V3 gn = cross(sub(P.p[1], P.p[0]), sub(P.p[2], P.p[0]));
			const double gl = len(gn);
			if (gl < 1e-12) continue;
			P.area = 0.5 * gl;
			P.n = mul(gn, 1.0 / gl);
			const V3 eN = v3(F.N);
			if (len(eN) > 1e-9 && dot(P.n, eN) < 0.0) P.n = mul(P.n, -1.0);
			faces.push_back(P);
		}
	}
	if (faces.empty()) {
		std::fprintf(stderr, "[STONE-UVPHASE] no target faces - nothing to do\n");
		return 0;
	}

	// ── 2. seams: collinear overlapping edge pairs of different faces ──────
	struct Edge { int f; int k; V3 P0, P1; double u0, v0, u1, v1; double L; V3 dir; };
	std::vector<Edge> edges;
	for (int i = 0; i < int(faces.size()); ++i) {
		const PFace &P = faces[i];
		for (int k = 0; k < 3; ++k) {
			const int k1 = (k + 1) % 3;
			Edge E; E.f = i; E.k = k; E.P0 = P.p[k]; E.P1 = P.p[k1];
			E.u0 = P.u[k]; E.v0 = P.v[k]; E.u1 = P.u[k1]; E.v1 = P.v[k1];
			E.L = len(sub(E.P1, E.P0));
			if (E.L < 1e-6) continue;
			E.dir = mul(sub(E.P1, E.P0), 1.0 / E.L);
			edges.push_back(E);
		}
	}
	const double kLineTol = 2e-3;     // world u: a point this close to the other edge's line is on it
	const double kMinOverlap = 5e-3;  // world u
	const double kUVTol = 0.005;      // tile: continuous if both u and v agree within this at both ends
	struct Raw { int f0, f1; V3 A, B; double len; double ua0, ub0, ua1, ub1, va0, vb0, va1, vb1; };
	std::vector<Raw> raws;
	for (size_t a = 0; a < edges.size(); ++a) {
		const Edge &ea = edges[a];
		for (size_t b = a + 1; b < edges.size(); ++b) {
			const Edge &eb = edges[b];
			if (ea.f == eb.f) continue;
			if (faces[ea.f].mat != faces[eb.f].mat) continue;   // rooms|floor: different textures
			// collinear: eb's endpoints both on ea's line
			const V3 r0 = sub(eb.P0, ea.P0), r1 = sub(eb.P1, ea.P0);
			if (len(cross(r0, ea.dir)) > kLineTol || len(cross(r1, ea.dir)) > kLineTol) continue;
			double t0 = dot(r0, ea.dir), t1 = dot(r1, ea.dir);
			if (t0 > t1) std::swap(t0, t1);
			const double lo = std::max(0.0, t0), hi = std::min(ea.L, t1);
			if (hi - lo < kMinOverlap) continue;
			Raw R; R.f0 = ea.f; R.f1 = eb.f; R.len = hi - lo;
			R.A = add(ea.P0, mul(ea.dir, lo)); R.B = add(ea.P0, mul(ea.dir, hi));
			auto uvAlong = [](const Edge &e, const V3 &X, double &u, double &v) {
				const double t = std::max(0.0, std::min(1.0, dot(sub(X, e.P0), e.dir) / e.L));
				u = e.u0 + (e.u1 - e.u0) * t; v = e.v0 + (e.v1 - e.v0) * t;
			};
			uvAlong(ea, R.A, R.ua0, R.va0); uvAlong(ea, R.B, R.ua1, R.va1);
			uvAlong(eb, R.A, R.ub0, R.vb0); uvAlong(eb, R.B, R.ub1, R.vb1);
			raws.push_back(R);
		}
	}

	// ── 3. patches: union-find over seams whose authored UV is CONTINUOUS ────
	// The unit that must move as one is the set of faces that already share a
	// UV mapping: join two faces when u and v agree at both ends of their
	// overlap. A crease whose UV is already continuous is then one patch and
	// can never be torn by the solve; a T-junction inside one wall joins too.
	std::vector<int> parent(faces.size());
	for (int i = 0; i < int(parent.size()); ++i) parent[i] = i;
	auto continuous = [&](const Raw &R) {
		return std::fabs(wrapS(R.ua0 - R.ub0)) <= kUVTol && std::fabs(wrapS(R.va0 - R.vb0)) <= kUVTol &&
		       std::fabs(wrapS(R.ua1 - R.ub1)) <= kUVTol && std::fabs(wrapS(R.va1 - R.vb1)) <= kUVTol;
	};
	int nContinuous = 0;
	for (const Raw &R : raws) {
		if (!continuous(R)) continue;
		++nContinuous;
		const int ra = findRoot(parent, R.f0), rb = findRoot(parent, R.f1);
		if (ra != rb) parent[ra] = rb;
	}
	std::map<int, int> rootToPatch;
	for (int i = 0; i < int(faces.size()); ++i) {
		const int r = findRoot(parent, i);
		auto it = rootToPatch.find(r);
		if (it == rootToPatch.end()) it = rootToPatch.emplace(r, int(rootToPatch.size())).first;
		faces[i].patch = it->second;
	}
	const int nPatch = int(rootToPatch.size());
	std::vector<double> patchArea(nPatch, 0.0);
	for (const PFace &P : faces) patchArea[P.patch] += P.area;

	// ── 4. the inter-patch seams: the u step at both ends of the overlap ───
	std::vector<Seam> seams;
	int nIntra = 0;
	for (const Raw &R : raws) {
		if (faces[R.f0].patch == faces[R.f1].patch) { ++nIntra; continue; }   // inside one mapping: nothing to solve
		Seam S;
		S.f0 = R.f0; S.f1 = R.f1; S.p0 = faces[R.f0].patch; S.p1 = faces[R.f1].patch;
		S.A = R.A; S.B = R.B; S.len = R.len;
		const double c = std::max(-1.0, std::min(1.0, dot(faces[R.f0].n, faces[R.f1].n)));
		S.dih = std::acos(c) * 180.0 / M_PI;
		S.uP[0] = R.ua0; S.uP[1] = R.ua1; S.uQ[0] = R.ub0; S.uQ[1] = R.ub1;
		S.dA = wrapS(R.ua0 - R.ub0); S.dB = wrapS(R.ua1 - R.ub1);
		S.dvA = wrapS(R.va0 - R.vb0); S.dvB = wrapS(R.va1 - R.vb1);
		S.phase = std::fabs(wrapS(S.dA - S.dB)) < kUVTol;
		S.d = S.phase ? (S.dA + 0.5 * wrapS(S.dB - S.dA)) : 0.0;
		seams.push_back(S);
	}

	// ── 5. solve o[patch]: BFS tree per component, then wrapped Gauss-Seidel ─
	std::vector<double> o(nPatch, 0.0), sc(nPatch, 1.0);
	std::vector<std::vector<int>> adj(nPatch);
	int nPhase = 0, nVary = 0;
	for (int e = 0; e < int(seams.size()); ++e) {
		if (!seams[e].phase) { ++nVary; continue; }
		++nPhase;
		adj[seams[e].p0].push_back(e); adj[seams[e].p1].push_back(e);
	}
	// residual of a phase seam at end k under the current (scale, offset)
	auto residAt = [&](const Seam &S, int k) {
		return wrapS(sc[S.p0] * S.uP[k] + o[S.p0] - sc[S.p1] * S.uQ[k] - o[S.p1]);
	};
	auto resid = [&](const Seam &S) {
		const double a = residAt(S, 0), b = residAt(S, 1);
		return std::fabs(a) >= std::fabs(b) ? a : b;
	};
	std::vector<char> seen(nPatch, 0), isRoot(nPatch, 0);
	std::vector<int> order;
	int nComp = 0;
	std::vector<int> byArea(nPatch);
	for (int p = 0; p < nPatch; ++p) byArea[p] = p;
	std::sort(byArea.begin(), byArea.end(), [&](int x, int y) { return patchArea[x] > patchArea[y]; });
	for (int root : byArea) {
		if (seen[root]) continue;
		++nComp; isRoot[root] = 1; seen[root] = 1;
		std::vector<int> q{ root };
		for (size_t h = 0; h < q.size(); ++h) {
			const int p = q[h];
			for (int e : adj[p]) {
				const Seam &S = seams[e];
				const int nb = (S.p0 == p) ? S.p1 : S.p0;
				if (seen[nb]) continue;
				seen[nb] = 1;
				o[nb] = (S.p0 == p) ? (o[p] + S.d) : (o[p] - S.d);
				q.push_back(nb); order.push_back(nb);
			}
		}
	}
	auto maxResid = [&]() {
		double m = 0.0;
		for (const Seam &S : seams) if (S.phase) m = std::max(m, std::fabs(resid(S)));
		return m;
	};
	const double residTree = maxResid();
	for (int s = 0; s < std::max(0, sweeps); ++s) {
		for (int p : order) {
			double num = 0.0, den = 0.0;
			for (int e : adj[p]) {
				const Seam &S = seams[e];
				const double r = 0.5 * (residAt(S, 0) + residAt(S, 1));
				num += S.len * ((S.p0 == p) ? -r : r);
				den += S.len;
			}
			if (den > 0.0) o[p] += num / den;
		}
	}
	const double residSweeps = maxResid();

	// ── 5b. the scale term: close the loops the offsets cannot ─────────────
	// Unknowns x = [e_0..e_n-1, do_0..do_n-1]; rows per phase-seam end
	//   (e_P u_P - e_Q u_Q + do_P - do_Q) = -res     weight len/2
	// plus the regularisers  sqrt(kappa area_P) e_P = 0  and  eps do_P = 0
	// (the latter fixes the per-component gauge). Normal equations, dense.
	// A patch is never scaled by more than kMaxScale: the first build let the
	// 0.6-tile roof rungs stretch 7-16% to close their own tiny loops, a
	// block-width change that reads. A patch whose least-squares e exceeds the
	// cap is clamped to +-cap and the system re-solved without it, so the rest
	// of its loop's residual lands on the seams instead (rounds until stable).
	double scaleMax = 0.0, scaleP50 = 0.0;
	bool scaleSolved = false;
	int nClamped = 0;
	if (scaleTerm && nPhase > 0) {
		const double kKappa = 0.01;      // a 2% scale on a 100 u^2 patch costs what a 0.01-tile residual on a 5 u seam does
		const double kMaxScale = 0.02;   // |e| cap
		const int n = nPatch, m = 2 * nPatch;
		const std::vector<double> oBase = o;
		std::vector<int> clamp(n, 0);    // 0 free, +-1 held at +-kMaxScale
		for (int round = 0; round < 8; ++round) {
			for (int p = 0; p < n; ++p) { sc[p] = 1.0; o[p] = oBase[p]; }
			std::vector<double> N(size_t(m) * m, 0.0), g(m, 0.0);
			auto addRow = [&](const int *idx, const double *c, int k, double t, double w) {
				for (int i = 0; i < k; ++i) {
					g[idx[i]] += w * c[i] * t;
					for (int j = 0; j < k; ++j) N[size_t(idx[i]) * m + idx[j]] += w * c[i] * c[j];
				}
			};
			for (const Seam &S : seams) {
				if (!S.phase) continue;
				for (int k = 0; k < 2; ++k) {
					const int idx[4] = { S.p0, S.p1, n + S.p0, n + S.p1 };
					const double c[4] = { S.uP[k], -S.uQ[k], 1.0, -1.0 };
					addRow(idx, c, 4, -residAt(S, k), 0.5 * S.len);
				}
			}
			for (int p = 0; p < n; ++p) {
				const int ie = p, io = n + p; const double one = 1.0;
				addRow(&ie, &one, 1, 0.0, kKappa * patchArea[p]);
				if (clamp[p]) addRow(&ie, &one, 1, clamp[p] * kMaxScale, 1e6);
				addRow(&io, &one, 1, 0.0, 1e-9);
			}
			if (!solveDense(N, g, m)) { scaleSolved = false; break; }
			scaleSolved = true;
			int newly = 0;
			for (int p = 0; p < n; ++p)
				if (!clamp[p] && std::fabs(g[p]) > kMaxScale + 1e-9) { clamp[p] = g[p] > 0 ? 1 : -1; ++newly; }
			for (int p = 0; p < n; ++p) { sc[p] = 1.0 + g[p]; o[p] = oBase[p] + g[n + p]; }
			if (newly == 0) break;
		}
		if (!scaleSolved) for (int p = 0; p < n; ++p) { sc[p] = 1.0; o[p] = oBase[p]; }
		std::vector<double> es;
		for (int p = 0; p < n; ++p) { es.push_back(std::fabs(sc[p] - 1.0)); nClamped += clamp[p] != 0; }
		std::sort(es.begin(), es.end());
		scaleMax = es.empty() ? 0.0 : es.back();
		scaleP50 = es.empty() ? 0.0 : es[es.size() / 2];
		std::fprintf(stderr, "[STONE-UVPHASE] scale e%% by patch (|e| > 0.5%%, * = clamped at %.0f%%):", 100.0 * kMaxScale);
		for (int p = 0; p < n; ++p)
			if (std::fabs(sc[p] - 1.0) > 0.005)
				std::fprintf(stderr, " p%d(a%.0f)%+.2f%s", p, patchArea[p], 100.0 * (sc[p] - 1.0), clamp[p] ? "*" : "");
		std::fprintf(stderr, "\n");
	}
	const double residFinal = maxResid();

	// ── 6. report ──────────────────────────────────────────────────────────
	std::fprintf(stderr,
		"[STONE-UVPHASE] faces %zu | seams (collinear overlaps) %zu: %d continuous (joined into %d patches, %d components), "
		"%d intra-patch, %d PHASE (constant step, solved), %d VARYING (step changes along the seam: scale/direction, left alone) | "
		"max residual after tree %.4f tile, after %d sweeps %.4f tile, after scale term %.4f tile (%s: |e| p50 %.2f%% max %.2f%%, %d patches clamped)\n",
		faces.size(), raws.size(), nContinuous, nPatch, nComp, nIntra, nPhase, nVary,
		residTree, std::max(0, sweeps), residSweeps, residFinal,
		!scaleTerm ? "scale term OFF" : (scaleSolved ? "scale term solved" : "scale term SINGULAR, skipped"),
		100.0 * scaleP50, 100.0 * scaleMax, nClamped);
	for (const Seam &S : seams) {
		if (!S.phase) {
			std::fprintf(stderr,
				"[STONE-UVPHASE] VARYING '%s' dih %6.2f A(%.3f,%.3f,%.3f) B(%.3f,%.3f,%.3f) du %+.3f/%+.3f dv %+.3f/%+.3f (left alone)\n",
				mats[faces[S.f0].mat], S.dih, S.A.x, S.A.y, S.A.z, S.B.x, S.B.y, S.B.z, S.dA, S.dB, S.dvA, S.dvB);
		} else if (std::fabs(resid(S)) > kUVTol) {
			std::fprintf(stderr,
				"[STONE-UVPHASE] RESIDUAL '%s' dih %6.2f A(%.3f,%.3f,%.3f) B(%.3f,%.3f,%.3f) was %+.3f now %+.3f/%+.3f tile "
				"(patch %d area %.2f | patch %d area %.2f)\n",
				mats[faces[S.f0].mat], S.dih, S.A.x, S.A.y, S.A.z, S.B.x, S.B.y, S.B.z, S.d, residAt(S, 0), residAt(S, 1),
				S.p0, patchArea[S.p0], S.p1, patchArea[S.p1]);
		}
	}
	{
		std::vector<double> mv;
		for (int p = 0; p < nPatch; ++p) if (!isRoot[p]) mv.push_back(std::fabs(wrapS(o[p])));
		std::sort(mv.begin(), mv.end());
		const double p50 = mv.empty() ? 0.0 : mv[mv.size() / 2];
		const double mx  = mv.empty() ? 0.0 : mv.back();
		std::fprintf(stderr, "[STONE-UVPHASE] shifted patches %zu: |shift mod 1| p50 %.3f max %.3f tile (roots: the largest patch of each component, offset-unmoved)\n",
		             mv.size(), p50, mx);
	}

	// ── 7. apply: Face::U1..U3, then Vertex::U/V re-stamped from them ─────────
	// The same write-back the v4 world-UV rewrite does (V4Bake.cpp WUVCore):
	// per-vertex U/V is dead for rasterisation (FRUSTRUM.CPP re-stamps from
	// U1..V3 every frame) but Compute_Vertex_Tangents and the smooth-normal
	// weld fall back to it for a degenerate UV triangle, so it must sit in the
	// same coordinate system. A corner shared by two patches takes one of the
	// two, exactly as an authored UV seam does today.
	long long rewritten = 0;
	for (const PFace &P : faces) {
		const double s = sc[P.patch], t = o[P.patch];
		if (s == 1.0 && t == 0.0) continue;
		P.F->U1 = float(s * double(P.F->U1) + t);
		P.F->U2 = float(s * double(P.F->U2) + t);
		P.F->U3 = float(s * double(P.F->U3) + t);
		P.F->uvToVertices();
		++rewritten;
	}
	std::fprintf(stderr, "[STONE-UVPHASE] applied: %lld faces' U1..U3 rewritten as s*u+o (v untouched), Vertex::U/V re-stamped from them\n",
	             rewritten);
	std::fflush(stderr);
	return rewritten;
}

}  // namespace rev
