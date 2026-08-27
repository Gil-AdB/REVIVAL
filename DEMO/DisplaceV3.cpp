// DISPLACE v3 — clean-room junction-correct stone displacement.
// Design: docs/DISPLACE_V3_DESIGN.md. M1 (panel + junction graph, instrument
// dump), M2 (interiors along PANEL PLANE normals), M3 (castellated junction
// rings: the exact k-plane offset solve per ring vert, miter-limited), M4
// (wall<->floor rings, same mechanism), plus the asymmetric border taper —
// only RECEDING motion fades near authored borders, or carve-in under an
// authored line (ceiling slabs) opens view slits; proud block ends stay
// proud to the edge.
//
// Deliberately self-contained: no code or concepts from the legacy
// DisplaceStoneSubdiv path. Height fields are loaded fresh from disk
// (stb_image, red channel) into plain linear arrays — no dependency on the
// engine's swizzled texture layout.

#include "Rev.h"
#include "DisplaceV3.h"
#include <Base/FeatureFlags.h>
#include "IMGCODE/stb_image.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <functional>
#include <array>

extern void Compute_FaceVertexIndices(TriMesh *T);

namespace {

// ── height field: plain linear float array, bilinear wrap sampling ─────────
struct HeightField
{
	int W = 0, H = 0;
	std::vector<float> h;      // row-major, [0,1]
	float mean = 0.0f;
	bool ok = false;

	bool load(const char *path)
	{
		int n = 0;
		unsigned char *px = stbi_load(path, &W, &H, &n, 1);   // grey = red-ish
		if (!px) { std::fprintf(stderr, "[DISPV3] FAILED to load '%s'\n", path); return false; }
		h.resize((size_t)W * H);
		double acc = 0.0;
		for (size_t i = 0; i < h.size(); ++i) { h[i] = px[i] / 255.0f; acc += h[i]; }
		stbi_image_free(px);
		mean = (float)(acc / h.size());
		ok = true;
		std::fprintf(stderr, "[DISPV3] height '%s' %dx%d mean=%.4f\n", path, W, H, mean);
		return true;
	}
	// uv in TILE units (1.0 = one texture repeat), wrap addressing.
	float sample(float u, float v) const
	{
		float x = (u - std::floor(u)) * W;
		float y = (v - std::floor(v)) * H;
		const int x0 = (int)x % W, y0 = (int)y % H;
		const int x1 = (x0 + 1) % W, y1 = (y0 + 1) % H;
		const float fx = x - std::floor(x), fy = y - std::floor(y);
		const float a = h[(size_t)y0 * W + x0], b = h[(size_t)y0 * W + x1];
		const float c = h[(size_t)y1 * W + x0], d = h[(size_t)y1 * W + x1];
		return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
	}
	// Robust two-level course structure of the map: plateau = median of
	// samples above the mean, groove = median below. The GRAIN (surface
	// roughness) rides on top of these levels; a silhouette that traces the
	// raw signal wobbles by the grain, one that traces the levels does not.
	float lvlHi = 0, lvlLo = 0;
	void computeLevels()
	{
		std::vector<float> hi, lo;
		hi.reserve(h.size() / 2); lo.reserve(h.size() / 2);
		for (float v : h) (v > mean ? hi : lo).push_back(v);
		if (hi.empty() || lo.empty()) { lvlHi = lvlLo = mean; return; }
		std::nth_element(hi.begin(), hi.begin() + hi.size() / 2, hi.end());
		std::nth_element(lo.begin(), lo.begin() + lo.size() / 2, lo.end());
		lvlHi = hi[hi.size() / 2];
		lvlLo = lo[lo.size() / 2];
	}
};

struct PanelRec {
	Vector n;              // VISIBLE-side unit normal ((C-A)x(B-A): engine
	                       // winding cross is anti-visible — measured, see design §7)
	float  d = 0;          // n · point-on-plane
	int    mat = 0;        // 0 rooms, 1 floor
	int    faces = 0;
	float  uMin = 1e9f, uMax = -1e9f, vMin = 1e9f, vMax = -1e9f;
	Vector lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
};

struct PosRec {
	Vector p;
	std::vector<Vertex*> inst;          // every Vertex instance at this position
	// first UV seen per DISTINCT panel (junction verts carry one per panel so
	// each side's height sample uses its own parameterization — M3 §2.5).
	struct PUV { int panel; float u, v; };
	PUV  puv[4];
	int  nPanels = 0;                   // distinct panels at this position
	bool overflow = false;              // >4 panels (never seen; counted)
	bool boundary = false;              // on an edge with only ONE target face
	void addPanel(int pl, float u, float v) {
		for (int i = 0; i < nPanels; ++i) if (puv[i].panel == pl) return;
		if (nPanels < 4) puv[nPanels++] = { pl, u, v };
		else overflow = true;
	}
};

int g_residMip[2] = { -1, -1 };

inline unsigned long long qkey(const Vector &p)
{
	const long long qx = llroundf(p.x * 1024.0f) + (1ll << 20);
	const long long qy = llroundf(p.y * 1024.0f) + (1ll << 20);
	const long long qz = llroundf(p.z * 1024.0f) + (1ll << 20);
	return ((unsigned long long)qx << 42) | ((unsigned long long)qy << 21)
	     | (unsigned long long)qz;
}

// M3b (the wobble fix, 2026-08-27): per-COURSE ring levels, keyed by
// quantized position per material. Filled by insertCourseRingVerts; consumed
// by pass 3 (ring verts displace by their course's level, not the raw
// sample). The raw signal's grain belongs to the FACES; the SILHOUETTE is
// the course structure — measured: the raw ring at ~3 verts/course against
// ~0.05-0.1u grooves hits them at random phase (docs/img/dispv3/
// wobble_xsec.png), which was the arris wobble.
std::unordered_map<unsigned long long, float> g_ringLvl[2];

// Insert ring verts AT the course-structure transitions of every junction
// edge (two verts per transition, straddling it), so the castellation's
// notch walls exist in the geometry instead of being smeared across
// whatever inter-vert span the uniform subdivision happened to leave.
// Self-contained mini graph build (panel cluster + edge map), then a
// per-mesh fan split in the SubdivideMaterialFaces array-rebuild style.
void insertCourseRingVerts(Scene *Sc, const HeightField hf[2])
{
	struct EKey { unsigned long long a, b; };
	struct Adj { TriMesh *T; int face; int ca, cb; int panel; };
	struct JEdge {
		Vector pA, pB;
		std::vector<Adj> adj;
		std::vector<int> panelSet;
		std::vector<float> tIns;          // insertion params, sorted
	};
	struct PanelKey { int mat; Vector n; float d; };
	std::vector<PanelKey> panels;
	std::map<std::pair<unsigned long long, unsigned long long>, JEdge> edges;

	for (TriMesh *M = Sc->TriMeshHead; M; M = M->Next)
		for (DWord i = 0; i < M->FIndex; ++i) {
			Face *F = &M->Faces[i];
			if (!F->Txtr || !F->Txtr->Name) continue;
			const bool isRooms = !std::strcmp(F->Txtr->Name, "rooms");
			const bool isFloor = !std::strcmp(F->Txtr->Name, "floor");
			if (!isRooms && !isFloor) continue;
			const Vector a = F->A->Pos, b = F->B->Pos, c = F->C->Pos;
			Vector n = (c - a) ^ (b - a);
			const float nl = n.length();
			if (nl < 1e-8f) continue;
			n /= nl;
			const float d = n * ((a + b + c) / 3.0f);
			const int mat = isRooms ? 0 : 1;
			int pl = -1;
			for (int p = 0; p < (int)panels.size(); ++p)
				if (panels[p].mat == mat && panels[p].n * n > 0.9995f
				    && std::fabs(panels[p].d - d) < 0.05f) { pl = p; break; }
			if (pl < 0) { pl = (int)panels.size(); panels.push_back({ mat, n, d }); }
			const Vector *ps[3] = { &a, &b, &c };
			for (int k = 0; k < 3; ++k) {
				const int j = (k + 1) % 3;
				unsigned long long ka = qkey(*ps[k]), kb = qkey(*ps[j]);
				int ca = k, cb = j;
				if (ka > kb) { std::swap(ka, kb); std::swap(ca, cb); }
				JEdge &E = edges[{ ka, kb }];
				if (E.adj.empty()) { E.pA = (ka == qkey(*ps[k])) ? *ps[k] : *ps[j];
				                     E.pB = (ka == qkey(*ps[k])) ? *ps[j] : *ps[k]; }
				E.adj.push_back({ M, (int)i, ca, cb, pl });
				bool have = false;
				for (int q : E.panelSet) if (q == pl) { have = true; break; }
				if (!have) E.panelSet.push_back(pl);
			}
		}

	// classify course transitions per junction CHAIN — a connected collinear
	// run of junction edges with the same panel set. Per-EDGE classification
	// was refuted by render (m3b_pair_camA first attempt): a course spanning
	// two subdiv edges got two independent segment medians → a level jump
	// mid-course → a bulge per half-course. The chain is the physical
	// junction line; courses and levels only make sense on it whole.
	auto faceUV = [](const Face *F, int corner, float &u, float &v) {
		switch (corner) {
		case 0: u = F->U1; v = F->V1; break;
		case 1: u = F->U2; v = F->V2; break;
		default: u = F->U3; v = F->V3; break;
		}
	};
	std::vector<JEdge *> jes;
	for (auto &kv : edges)
		if (kv.second.panelSet.size() >= 2 && (kv.second.pB - kv.second.pA).length() > 1e-4f)
			jes.push_back(&kv.second);
	const int nJE = (int)jes.size();
	int nTrans = 0, nLvl = 0, nChains = 0;
	std::unordered_map<unsigned long long, std::vector<int>> byEnd;
	for (int i = 0; i < (int)jes.size(); ++i) {
		byEnd[qkey(jes[i]->pA)].push_back(i);
		byEnd[qkey(jes[i]->pB)].push_back(i);
	}
	auto sortedPanels = [](const JEdge *E) {
		std::vector<int> s = E->panelSet;
		std::sort(s.begin(), s.end());
		return s;
	};
	std::vector<char> used(jes.size(), 0);
	for (int seed = 0; seed < (int)jes.size(); ++seed) {
		if (used[seed]) continue;
		Vector dir = jes[seed]->pB - jes[seed]->pA;
		dir /= dir.length();
		const std::vector<int> pset = sortedPanels(jes[seed]);
		// chain as ordered edge list with per-edge "fwd" (pA→pB along chain)
		struct CE { int je; bool fwd; };
		std::vector<CE> chain = { { seed, true } };
		used[seed] = 1;
		for (int side = 0; side < 2; ++side) {
			for (;;) {
				const CE &endCE = side ? chain.front() : chain.back();
				const JEdge *EE = jes[endCE.je];
				const Vector tail = side ? (endCE.fwd ? EE->pA : EE->pB)
				                         : (endCE.fwd ? EE->pB : EE->pA);
				int found = -1; bool ffwd = true;
				for (int cand : byEnd[qkey(tail)]) {
					if (used[cand]) continue;
					const JEdge *C = jes[cand];
					Vector cd = C->pB - C->pA;
					cd /= cd.length();
					if (std::fabs(cd * dir) < 0.999f) continue;
					if (sortedPanels(C) != pset) continue;
					found = cand;
					// forward = continues AWAY from tail in chain direction
					ffwd = (qkey(C->pA) == qkey(tail));
					break;
				}
				if (found < 0) break;
				used[found] = 1;
				if (side) chain.insert(chain.begin(), { found, !ffwd ? true : false });
				else chain.push_back({ found, ffwd });
				// NOTE for side==1 (prepend): chain direction runs front→back;
				// the new edge ends at `tail`, so fwd means its pB==tail.
				if (side) chain.front().fwd = (qkey(jes[found]->pB) == qkey(tail));
			}
		}
		nChains++;
		// SCOPE (refuted-by-render trail, 2026-08-27): castellate ONLY
		// same-material SHARP-REFLEX chains (visible-side θ > 240 — the class
		// Gil-Ad named broken). The ε-pair notch walls are long thin strips
		// from the ring to the first interior row; on the gentle base
		// bed-joints this pose views them at grazing angles and they invert
		// into black slivers (t=5743: 46→1142 px; rel-unification made it
		// 2082). The gentle class works today — leave it exactly alone.
		bool doInsert = false;
		{
			const JEdge *E0 = jes[chain[0].je];
			const Adj *rA = &E0->adj[0];
			const Adj *rB = nullptr;
			for (const Adj &A : E0->adj)
				if (A.panel != rA->panel) { rB = &A; break; }
			if (!rB) continue;
			if (panels[rA->panel].mat != panels[rB->panel].mat) continue;
			const Vector nA = panels[rA->panel].n;
			const Vector nB = panels[rB->panel].n;
			const Face *FB = &rB->T->Faces[rB->face];
			const Vertex *corners[3] = { FB->A, FB->B, FB->C };
			const Vertex *third = corners[3 - rB->ca - rB->cb];
			Vector uB = third->Pos - E0->pA;
			uB -= dir * (uB * dir);
			const float ul = uB.length();
			if (ul < 1e-6f) continue;
			uB /= ul;
			const float delta = std::acos(std::max(-1.0f, std::min(1.0f, nA * nB)))
			                  * 57.29578f;
			const float theta = (uB * nA < 0.0f) ? 180.0f + delta : 180.0f - delta;
			doInsert = theta > 240.0f;
			// gentle same-material chains get LEVELS ONLY: constant rel per
			// course straightens the silhouette (collinear ring verts, grain
			// gone) with no inserted twist strips; at their small silhouette
			// projection the un-inserted transition smear is subpixel.
		}
		// cumulative arclength + per-edge material rep faces
		const int NE = (int)chain.size();
		std::vector<float> cum(NE + 1, 0.0f);
		for (int k = 0; k < NE; ++k)
			cum[k + 1] = cum[k] + (jes[chain[k].je]->pB - jes[chain[k].je]->pA).length();
		const float totalLen = cum[NE];
		struct SideUV { bool ok; float uA, vA, uB, vB; };
		std::vector<SideUV> suv[2];
		for (int m = 0; m < 2; ++m) suv[m].assign(NE, { false, 0, 0, 0, 0 });
		for (int k = 0; k < NE; ++k) {
			const JEdge *E = jes[chain[k].je];
			for (int m = 0; m < 2; ++m) {
				const Adj *rep = nullptr;
				for (const Adj &A : E->adj)
					if (panels[A.panel].mat == m) { rep = &A; break; }
				if (!rep || !hf[m].ok) continue;
				const Face *SF = &rep->T->Faces[rep->face];
				float u0, v0, u1, v1;
				faceUV(SF, rep->ca, u0, v0);
				faceUV(SF, rep->cb, u1, v1);
				// store oriented along the CHAIN direction
				if (chain[k].fwd) suv[m][k] = { true, u0, v0, u1, v1 };
				else              suv[m][k] = { true, u1, v1, u0, v0 };
			}
		}
		auto sampleAt = [&](int m, float s) -> float {
			int k = 0;
			while (k + 1 < NE && s > cum[k + 1]) ++k;
			const float t = (cum[k + 1] - cum[k] > 1e-6f)
			    ? (s - cum[k]) / (cum[k + 1] - cum[k]) : 0.0f;
			const SideUV &S = suv[m][k];
			if (!S.ok) return -1.0f;
			return hf[m].sample(S.uA + (S.uB - S.uA) * t, S.vA + (S.vB - S.vA) * t);
		};
		const int tMat = suv[0][0].ok ? 0 : 1;
		if (!suv[tMat][0].ok) continue;
		const HeightField &tf = hf[tMat];
		const float thr = 0.5f * (tf.lvlHi + tf.lvlLo);
		auto cls = [&](float s) { return sampleAt(tMat, s) > thr; };
		const int N = std::min(4096, std::max(96, (int)(totalLen / 0.005f)));
		std::vector<float> trans;
		bool c0 = cls(0.0f);
		for (int st = 1; st <= N; ++st) {
			const float s = totalLen * st / N;
			const bool c1 = cls(s);
			if (c1 != c0) {
				float lo = totalLen * (st - 1) / N, hi = s;
				for (int it = 0; it < 6; ++it) {
					const float mid = 0.5f * (lo + hi);
					if (cls(mid) == c0) lo = mid; else hi = mid;
				}
				trans.push_back(0.5f * (lo + hi));
				c0 = c1;
			}
		}
		nTrans += (int)trans.size();
		// insertion points: straddle each transition by 0.01u, mapped into
		// the owning edge's LOCAL t (in the edge's own pA→pB orientation)
		const float epsW = 0.010f;
		auto planInsert = [&](float s) {
			if (s < 1e-4f || s > totalLen - 1e-4f) return;
			int k = 0;
			while (k + 1 < NE && s > cum[k + 1]) ++k;
			const float eLen = cum[k + 1] - cum[k];
			if (eLen < 1e-6f) return;
			float t = (s - cum[k]) / eLen;
			if (!chain[k].fwd) t = 1.0f - t;
			if (t < 0.02f || t > 0.98f) return;
			jes[chain[k].je]->tIns.push_back(t);
		};
		if (doInsert)
			for (float s : trans) { planInsert(s - epsW); planInsert(s + epsW); }
		for (const CE &ce : chain) {
			std::sort(jes[ce.je]->tIns.begin(), jes[ce.je]->tIns.end());
			jes[ce.je]->tIns.erase(std::unique(jes[ce.je]->tIns.begin(),
			                                   jes[ce.je]->tIns.end()),
			                       jes[ce.je]->tIns.end());
		}
		// per-course levels over the WHOLE chain segment, per material side;
		// assigned to every ring vert (chain vertices + planned inserts)
		std::vector<float> segB = { 0.0f };
		for (float s : trans) segB.push_back(s);
		segB.push_back(totalLen);
		for (int m = 0; m < 2; ++m) {
			if (!suv[m][0].ok) continue;
			for (size_t sg = 0; sg + 1 < segB.size(); ++sg) {
				const float a = segB[sg], b = segB[sg + 1];
				if (b - a < 1e-5f) continue;
				std::vector<float> smp;
				const int NS = std::max(8, (int)((b - a) / 0.01f));
				for (int st = 0; st <= NS; ++st) {
					const float v = sampleAt(m, a + (b - a) * st / NS);
					if (v >= 0.0f) smp.push_back(v);
				}
				if (smp.empty()) continue;
				std::nth_element(smp.begin(), smp.begin() + smp.size() / 2, smp.end());
				const float lvl = smp[smp.size() / 2];
				auto putAtS = [&](float s) {
					int k = 0;
					while (k + 1 < NE && s > cum[k + 1]) ++k;
					const float eLen = cum[k + 1] - cum[k];
					float t = eLen > 1e-6f ? (s - cum[k]) / eLen : 0.0f;
					if (!chain[k].fwd) t = 1.0f - t;
					const JEdge *E = jes[chain[k].je];
					// same canonical lerp form as the mesh-edit vert creation,
					// so the quantized keys match to the ulp
					g_ringLvl[m][qkey(E->pA + (E->pB - E->pA) * t)] = lvl;
					nLvl++;
				};
				// chain vertices inside [a,b]
				for (int k = 0; k <= NE; ++k)
					if (cum[k] > a - 1e-5f && cum[k] < b + 1e-5f) putAtS(std::min(cum[k], totalLen));
				// inserted verts inside [a,b]
				for (float s : trans) {
					if (s - epsW > a - 1e-5f && s - epsW < b + 1e-5f) putAtS(s - epsW);
					if (s + epsW > a - 1e-5f && s + epsW < b + 1e-5f) putAtS(s + epsW);
				}
			}
		}
	}

	// mesh edit: fan-split every adjacent face at its junction edges' tIns
	int nVertIns = 0, nFaceSplit = 0;
	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
		// collect this mesh's planned splits: face -> per-edge list
		struct Split { int face; int ca, cb; const JEdge *E; };
		std::vector<Split> splits;
		for (const auto &kv : edges) {
			const JEdge &E = kv.second;
			if (E.panelSet.size() < 2 || E.tIns.empty()) continue;
			for (const Adj &A : E.adj)
				if (A.T == T) splits.push_back({ A.face, A.ca, A.cb, &E });
		}
		if (splits.empty()) continue;

		Vertex *const oldV = T->Verts;
		std::vector<Vertex> verts(oldV, oldV + T->VIndex);
		std::vector<Face> faces;
		std::vector<std::array<uint32_t, 3>> fIdx;
		std::vector<std::array<float, 6>> fUV;      // U1,V1,U2,V2,U3,V3
		faces.reserve(T->FIndex + splits.size() * 4);
		fIdx.reserve(faces.capacity());
		fUV.reserve(faces.capacity());
		auto vidx = [&](const Vertex *v) { return uint32_t(v - oldV); };
		// one shared new vertex per (edge, t) in this mesh
		std::map<std::pair<const JEdge *, int>, uint32_t> newVert;

		std::vector<std::vector<Split> > perFace(T->FIndex);
		for (const Split &S : splits) perFace[S.face].push_back(S);

		for (DWord i = 0; i < T->FIndex; ++i) {
			Face &F = T->Faces[i];
			std::array<uint32_t, 3> idx = { F.A ? vidx(F.A) : 0u,
			                                F.B ? vidx(F.B) : 0u,
			                                F.C ? vidx(F.C) : 0u };
			std::array<float, 6> uv = { F.U1, F.V1, F.U2, F.V2, F.U3, F.V3 };
			if (perFace[i].empty()) {
				faces.push_back(F); fIdx.push_back(idx); fUV.push_back(uv);
				continue;
			}
			// worklist of sub-faces; each split edge handled once, matched by
			// the ORIGINAL corner indices still present in the sub-face
			struct Work { std::array<uint32_t, 3> idx; std::array<float, 6> uv; };
			std::vector<Work> wl = { { idx, uv } };
			for (const Split &S : perFace[i]) {
				const uint32_t ia = idx[S.ca], ib = idx[S.cb];
				std::vector<Work> next;
				for (Work &w : wl) {
					int ka = -1, kb = -1;
					for (int k = 0; k < 3; ++k) {
						if (w.idx[k] == ia) ka = k;
						if (w.idx[k] == ib) kb = k;
					}
					if (ka < 0 || kb < 0) { next.push_back(w); continue; }
					// fan split along S.E->tIns between corners ka,kb
					nFaceSplit++;
					const int kc = 3 - ka - kb;
					// t of the edge runs pA->pB; corner ka may be either end
					const bool fwd = qkey(verts[w.idx[ka]].Pos) == qkey(S.E->pA);
					uint32_t prev = w.idx[ka];
					float pu = w.uv[ka * 2], pv = w.uv[ka * 2 + 1];
					const float au = w.uv[ka * 2], av = w.uv[ka * 2 + 1];
					const float bu = w.uv[kb * 2], bv = w.uv[kb * 2 + 1];
					std::vector<float> ts = S.E->tIns;
					if (!fwd) { for (float &t : ts) t = 1.0f - t; std::sort(ts.begin(), ts.end()); }
					for (size_t q = 0; q < ts.size(); ++q) {
						const float t = ts[q];
						// shared new vertex for this (edge, canonical t index)
						int canonIdx = 0;
						{
							const float tc = fwd ? t : 1.0f - t;
							for (size_t r = 0; r < S.E->tIns.size(); ++r)
								if (std::fabs(S.E->tIns[r] - tc) < 1e-6f) { canonIdx = (int)r; break; }
						}
						uint32_t mid;
						auto itNV = newVert.find({ S.E, canonIdx });
						if (itNV != newVert.end()) mid = itNV->second;
						else {
							Vertex nvx = verts[w.idx[ka]];
							// canonical form from the stored edge-local param —
							// both panels' instances must land on the SAME
							// quantized key or the junction unwelds here
							const float tc2 = S.E->tIns[canonIdx];
							const Vector P = S.E->pA + (S.E->pB - S.E->pA) * tc2;
							nvx.Pos = P;
							const Vertex &VA = verts[w.idx[ka]], &VB = verts[w.idx[kb]];
							nvx.U = VA.U + (VB.U - VA.U) * t;  nvx.V = VA.V + (VB.V - VA.V) * t;
							nvx.EU = VA.EU + (VB.EU - VA.EU) * t; nvx.EV = VA.EV + (VB.EV - VA.EV) * t;
							Vector nn = VA.N + (VB.N - VA.N) * t;
							const float nl2 = nn.length();
							if (nl2 > 1e-6f) nvx.N = nn / nl2;
							mid = (uint32_t)verts.size();
							verts.push_back(nvx);
							newVert[{ S.E, canonIdx }] = mid;
							nVertIns++;
						}
						const float mu = au + (bu - au) * t, mv = av + (bv - av) * t;
						Work sub;
						sub.idx = { prev, mid, w.idx[kc] };
						sub.uv = { pu, pv, mu, mv, w.uv[kc * 2], w.uv[kc * 2 + 1] };
						// keep original winding: ka->kb order must match
						if ((kb - ka + 3) % 3 != 1) {
							std::swap(sub.idx[0], sub.idx[1]);
							std::swap(sub.uv[0], sub.uv[2]); std::swap(sub.uv[1], sub.uv[3]);
						}
						next.push_back(sub);
						prev = mid; pu = mu; pv = mv;
					}
					Work last;
					last.idx = { prev, w.idx[kb], w.idx[kc] };
					last.uv = { pu, pv, bu, bv, w.uv[kc * 2], w.uv[kc * 2 + 1] };
					if ((kb - ka + 3) % 3 != 1) {
						std::swap(last.idx[0], last.idx[1]);
						std::swap(last.uv[0], last.uv[2]); std::swap(last.uv[1], last.uv[3]);
					}
					next.push_back(last);
				}
				wl.swap(next);
			}
			for (Work &w : wl) {
				Face f = F;
				f.frame = nullptr;
				f.U1 = w.uv[0]; f.V1 = w.uv[1]; f.U2 = w.uv[2]; f.V2 = w.uv[3];
				f.U3 = w.uv[4]; f.V3 = w.uv[5];
				f.EU1 = w.uv[0]; f.EV1 = w.uv[1]; f.EU2 = w.uv[2]; f.EV2 = w.uv[3];
				f.EU3 = w.uv[4]; f.EV3 = w.uv[5];
				faces.push_back(f); fIdx.push_back(w.idx); fUV.push_back(w.uv);
			}
		}

		Vertex *nv = new Vertex[verts.size()];
		std::memcpy(nv, verts.data(), verts.size() * sizeof(Vertex));
		Face *nf = new Face[faces.size()];
		for (size_t i = 0; i < faces.size(); ++i) {
			nf[i] = faces[i];
			nf[i].A = &nv[fIdx[i][0]];
			nf[i].B = &nv[fIdx[i][1]];
			nf[i].C = &nv[fIdx[i][2]];
			// planar split: face N unchanged; refresh NormProd from the new A
			nf[i].NormProd = -(nf[i].N * nf[i].A->Pos);
		}
		T->Verts = nv; T->VIndex = (int32_t)verts.size();
		T->Faces = nf; T->FIndex = (int32_t)faces.size();
		Compute_FaceVertexIndices(T);
		if (T->Flags & Tri_Stationary)
			T->SL = (Color *)getAlignedBlock(sizeof(Color) * T->VIndex, 16);
	}
	std::fprintf(stderr, "[DISPV3] course rings: %d junction edges in %d chains, "
	             "%d transitions, %d verts inserted, %d face splits, %d levels\n",
	             nJE, nChains, nTrans, nVertIns, nFaceSplit, nLvl);
}

} // namespace

int DisplaceV3_ResidualMip(int mat)
{
	return (mat == 0 || mat == 1) ? g_residMip[mat] : -1;
}

void DisplaceV3_Run(Scene *Sc)
{
	const float amp = fds::FeatureFlags::greets_displace_amp();
	const bool  viz = fds::FeatureFlags::greets_displace_v3_viz();

	HeightField hf[2];
	hf[0].load("TEXTURES/greets_wall_h.png");
	hf[1].load("TEXTURES/greets_floor_h.png");
	hf[0].computeLevels();
	hf[1].computeLevels();
	std::fprintf(stderr, "[DISPV3] levels rooms=%.4f/%.4f floor=%.4f/%.4f\n",
	             hf[0].lvlHi, hf[0].lvlLo, hf[1].lvlHi, hf[1].lvlLo);

	g_ringLvl[0].clear();
	g_ringLvl[1].clear();
	const bool ringCourses = fds::FeatureFlags::greets_displace_v3_ring_courses();
	if (ringCourses)
		insertCourseRingVerts(Sc, hf);

	std::vector<PanelRec> panels;
	std::unordered_map<unsigned long long, int> posId;
	std::vector<PosRec> pos;
	struct ERec { int p0, p1; int nTargetFaces = 0; std::vector<int> panels; };
	std::unordered_map<unsigned long long, int> edgeId;
	std::vector<ERec> erecs;
	struct FRef { Face *F; int panel; };
	std::vector<FRef> frefs;
	std::vector<float> uvSpanTex[2];        // per-material face UV diameters, texels

	auto pid = [&](const Vector &p) -> int {
		const long long qx = llroundf(p.x * 1024.0f) + (1ll << 20);
		const long long qy = llroundf(p.y * 1024.0f) + (1ll << 20);
		const long long qz = llroundf(p.z * 1024.0f) + (1ll << 20);
		const unsigned long long k = ((unsigned long long)qx << 42)
		                           | ((unsigned long long)qy << 21)
		                           | (unsigned long long)qz;
		auto it = posId.find(k);
		if (it != posId.end()) return it->second;
		posId.emplace(k, (int)pos.size());
		pos.push_back({});
		pos.back().p = p;
		return (int)pos.size() - 1;
	};

	// ── pass 1: panels, positions, edges ────────────────────────────────────
	for (TriMesh *M = Sc->TriMeshHead; M; M = M->Next) {
		for (DWord i = 0; i < M->FIndex; ++i) {
			Face *F = &M->Faces[i];
			if (!F->Txtr || !F->Txtr->Name) continue;
			const bool isRooms = !std::strcmp(F->Txtr->Name, "rooms");
			const bool isFloor = !std::strcmp(F->Txtr->Name, "floor");
			if (!isRooms && !isFloor) continue;
			const Vector a = F->A->Pos, b = F->B->Pos, c = F->C->Pos;
			Vector n = (c - a) ^ (b - a);            // VISIBLE-side normal
			const float nl = n.length();
			if (nl < 1e-8f) continue;
			n /= nl;
			const Vector cent = (a + b + c) / 3.0f;
			const float d = n * cent;
			const int mat = isRooms ? 0 : 1;
			int pl = -1;
			for (int p = 0; p < (int)panels.size(); ++p)
				if (panels[p].mat == mat && panels[p].n * n > 0.9995f
				    && std::fabs(panels[p].d - d) < 0.05f) { pl = p; break; }
			if (pl < 0) { pl = (int)panels.size(); panels.push_back({}); panels[pl].n = n; panels[pl].d = d; panels[pl].mat = mat; }
			PanelRec &P = panels[pl];
			P.faces++;
			frefs.push_back({F, pl});

			Vertex *vs[3] = { F->A, F->B, F->C };
			const float us[3] = { F->U1, F->U2, F->U3 };
			const float vts[3] = { F->V1, F->V2, F->V3 };
			int ids[3];
			for (int k = 0; k < 3; ++k) {
				const int id = pid(vs[k]->Pos);
				ids[k] = id;
				PosRec &R = pos[id];
				R.inst.push_back(vs[k]);
				R.addPanel(pl, us[k], vts[k]);
				P.uMin = std::min(P.uMin, us[k]); P.uMax = std::max(P.uMax, us[k]);
				P.vMin = std::min(P.vMin, vts[k]); P.vMax = std::max(P.vMax, vts[k]);
				const Vector &pp = vs[k]->Pos;
				P.lo.x = std::min(P.lo.x, pp.x); P.hi.x = std::max(P.hi.x, pp.x);
				P.lo.y = std::min(P.lo.y, pp.y); P.hi.y = std::max(P.hi.y, pp.y);
				P.lo.z = std::min(P.lo.z, pp.z); P.hi.z = std::max(P.hi.z, pp.z);
			}
			{
				const HeightField &f = hf[mat];
				float span = 0.0f;
				for (int k = 0; k < 3; ++k) {
					const int j = (k + 1) % 3;
					span = std::max(span, std::fabs(us[k] - us[j]) * (f.ok ? f.W : 1024));
					span = std::max(span, std::fabs(vts[k] - vts[j]) * (f.ok ? f.H : 1024));
				}
				uvSpanTex[mat].push_back(span);
			}
			for (int k = 0; k < 3; ++k) {
				const int lo = std::min(ids[k], ids[(k + 1) % 3]);
				const int hi = std::max(ids[k], ids[(k + 1) % 3]);
				if (lo == hi) continue;
				const unsigned long long ek =
					((unsigned long long)(unsigned)lo << 32) | (unsigned)hi;
				auto it = edgeId.find(ek);
				int ei;
				if (it != edgeId.end()) ei = it->second;
				else { ei = (int)erecs.size(); edgeId.emplace(ek, ei); erecs.push_back({lo, hi}); }
				erecs[ei].nTargetFaces++;
				erecs[ei].panels.push_back(pl);
			}
		}
	}

	// ── pass 1b: weld near-coincident positions (union-find, eps 2e-3) ──────
	// Authoring leaves junction verts split by float noise; exact-quant
	// identity then displaces one side of a junction line and pins the other
	// (the t=5743 doorway-header tears). Cluster positions across the 27
	// neighboring quant cells and merge their records so a junction is ONE
	// ring by identity even when the FLD stored it twice.
	{
		std::vector<int> uf(pos.size());
		for (size_t i = 0; i < uf.size(); ++i) uf[i] = (int)i;
		std::function<int(int)> find = [&](int a) {
			while (uf[a] != a) { uf[a] = uf[uf[a]]; a = uf[a]; }
			return a;
		};
		const float eps = 2e-3f, eps2 = eps * eps;
		for (size_t i = 0; i < pos.size(); ++i) {
			const Vector &p = pos[i].p;
			for (int dx = -1; dx <= 1; ++dx)
			for (int dy = -1; dy <= 1; ++dy)
			for (int dz = -1; dz <= 1; ++dz) {
				const Vector q{ p.x + dx / 1024.0f, p.y + dy / 1024.0f, p.z + dz / 1024.0f };
				const long long qx = llroundf(q.x * 1024.0f) + (1ll << 20);
				const long long qy = llroundf(q.y * 1024.0f) + (1ll << 20);
				const long long qz = llroundf(q.z * 1024.0f) + (1ll << 20);
				const unsigned long long k = ((unsigned long long)qx << 42)
				                           | ((unsigned long long)qy << 21)
				                           | (unsigned long long)qz;
				auto it = posId.find(k);
				if (it == posId.end() || (size_t)it->second == i) continue;
				const Vector d = pos[it->second].p - p;
				if (d * d < eps2) uf[find((int)i)] = find(it->second);
			}
		}
		// merge records into cluster roots, remap edges
		int nMerged = 0;
		for (size_t i = 0; i < pos.size(); ++i) {
			const int r = find((int)i);
			if (r == (int)i) continue;
			nMerged++;
			PosRec &src = pos[i], &dst = pos[r];
			for (Vertex *V : src.inst) dst.inst.push_back(V);
			for (int k = 0; k < src.nPanels; ++k)
				dst.addPanel(src.puv[k].panel, src.puv[k].u, src.puv[k].v);
			dst.boundary = dst.boundary || src.boundary;
			src.inst.clear();
			src.nPanels = 0;                 // root carries the cluster
		}
		std::unordered_map<unsigned long long, int> emerge;
		std::vector<ERec> merged;
		for (const ERec &e : erecs) {
			const int a = find(e.p0), b = find(e.p1);
			if (a == b) continue;
			const unsigned long long k =
				((unsigned long long)(unsigned)std::min(a, b) << 32) | (unsigned)std::max(a, b);
			auto it = emerge.find(k);
			if (it == emerge.end()) {
				emerge.emplace(k, (int)merged.size());
				merged.push_back({ std::min(a, b), std::max(a, b), e.nTargetFaces, e.panels });
			} else {
				ERec &m = merged[it->second];
				m.nTargetFaces += e.nTargetFaces;
				for (int p : e.panels) m.panels.push_back(p);
			}
		}
		erecs.swap(merged);
		std::fprintf(stderr, "[DISPV3] weld: %d near-coincident positions merged, "
		             "%zu edges after merge\n", nMerged, erecs.size());
	}

	// ── pass 2: boundary classification (edges against NON-target faces) ────
	int nJunctionEdges = 0, nBoundaryEdges = 0;
	for (const ERec &e : erecs) {
		bool multi = false;
		for (int p : e.panels) if (p != e.panels[0]) { multi = true; break; }
		if (e.nTargetFaces == 1) { pos[e.p0].boundary = pos[e.p1].boundary = true; nBoundaryEdges++; }
		if (multi) nJunctionEdges++;
	}
	// Boundary condition at AUTHORED edges: relief must FADE to zero at
	// borders against non-target geometry, or carving-in right below an
	// authored line (the ceiling slab over the t=5743 doorway jamb) opens a
	// view slit between the receded stone and the straight authored edge —
	// the wireframe showed exactly that. World-unit taper, the same contract
	// the legacy bake documented ("relief fades to the authored edge");
	// junction castellation everywhere else stays at full amplitude.
	int nRingBoundary = 0;                    // positions inside the taper band
	std::vector<float> borderScale(pos.size(), 1.0f);
	{
		const float taperT = fds::FeatureFlags::greets_displace_v3_border_taper();
		std::vector<Vector> bpts;
		for (const PosRec &R : pos)
			if (R.nPanels > 0 && R.boundary) bpts.push_back(R.p);
		if (taperT > 1e-4f && !bpts.empty())
			for (size_t i = 0; i < pos.size(); ++i) {
				if (pos[i].nPanels == 0 || pos[i].boundary) continue;
				float d2min = taperT * taperT;
				for (const Vector &b : bpts) {
					const Vector d = pos[i].p - b;
					const float d2 = d * d;
					if (d2 < d2min) d2min = d2;
				}
				const float s = std::sqrt(d2min) / taperT;
				if (s < 0.999f) { borderScale[i] = s; nRingBoundary++; }
			}
	}

	// ── pass 3: displace — interiors along the PANEL PLANE normal, junction
	// ring verts by the exact k-plane offset solve (M3, the castellated
	// corner): find d with n_i · d = rel_i for every incident panel, so each
	// face remains a parallel offset of its wall AT ITS OWN height value.
	// For k=2 and rel_A=rel_B this is exactly the mitre bisector scaled by
	// 1/cos(δ/2); the normal-sum form is branchless-correct for convex AND
	// reflex junctions (design §2). Miter limit: |d| clamped to
	// kMiterLimit·max|rel| (bevel-strip fallback deferred — never fires in
	// this scene, δ ≤ 90° everywhere measured).
	const float miterLimit = fds::FeatureFlags::greets_displace_v3_miter();
	// TEST INSTRUMENT (--greets_displace_v3_ring_twolevel): ring verts take
	// the map's COURSE LEVEL (plateau/groove medians) instead of the raw
	// sample — the grain-on-the-silhouette discriminator.
	const bool ringTwoLevel = fds::FeatureFlags::greets_displace_v3_ring_twolevel();
	struct RingDump { Vector p; int n; int panel[4]; float u[4], v[4], rel[4]; Vector d; };
	std::vector<RingDump> ringDump;
	int nInterior = 0, nRing2 = 0, nRing3 = 0, nRingFallback = 0,
	    nPinned = 0, nClamped = 0;
	float maxAbsD = 0.0f;
	for (size_t pi = 0; pi < pos.size(); ++pi) {
		PosRec &R = pos[pi];
		if (R.nPanels == 0) continue;
		if (R.boundary) { nPinned++; continue; }    // authored edge vs non-target mat
		float rel[4];
		float maxRel = 0.0f;
		bool bad = false;
		for (int i = 0; i < R.nPanels; ++i) {
			const HeightField &f = hf[panels[R.puv[i].panel].mat];
			if (!f.ok) { bad = true; break; }
			const float hs = f.sample(R.puv[i].u, R.puv[i].v);
			float hEff = (ringTwoLevel && R.nPanels >= 2)
			    ? (hs > 0.5f * (f.lvlHi + f.lvlLo) ? f.lvlHi : f.lvlLo)
			    : hs;
			// M3b: ring verts on a course-classified junction take their
			// COURSE level (the map's own per-segment median) — the
			// silhouette is the course structure, the grain stays on faces.
			if (ringCourses && R.nPanels >= 2) {
				const int m = panels[R.puv[i].panel].mat;
				auto itL = g_ringLvl[m].find(qkey(R.p));
				if (itL != g_ringLvl[m].end()) hEff = itL->second;
			}
			rel[i] = amp * (hEff - f.mean);
			maxRel = std::max(maxRel, std::fabs(rel[i]));
		}
		if (bad || R.overflow) { nRingFallback++; continue; }
		Vector d;
		bool solved = false;
		if (R.nPanels == 1) {
			d = panels[R.puv[0].panel].n * rel[0];
			solved = true;
			nInterior++;
		} else if (R.nPanels == 2) {
			const Vector nA = panels[R.puv[0].panel].n, nB = panels[R.puv[1].panel].n;
			const float c = nA * nB, det = 1.0f - c * c;
			if (det > 1e-5f) {
				const float al = (rel[0] - c * rel[1]) / det;
				const float be = (rel[1] - c * rel[0]) / det;
				d = nA * al + nB * be;
				solved = true;
				nRing2++;
			}
		} else if (R.nPanels == 3) {
			const Vector n0 = panels[R.puv[0].panel].n, n1 = panels[R.puv[1].panel].n,
			             n2 = panels[R.puv[2].panel].n;
			const Vector c12 = n1 ^ n2, c20 = n2 ^ n0, c01 = n0 ^ n1;
			const float det = n0 * c12;
			if (std::fabs(det) > 1e-5f) {
				d = (c12 * rel[0] + c20 * rel[1] + c01 * rel[2]) / det;
				solved = true;
				nRing3++;
			}
		}
		if (!solved) {                       // degenerate: average normals/rels
			Vector nSum{ 0, 0, 0 };
			float rSum = 0;
			for (int i = 0; i < R.nPanels; ++i) { nSum += panels[R.puv[i].panel].n; rSum += rel[i]; }
			const float nl = nSum.length();
			if (nl < 1e-6f) { nRingFallback++; continue; }
			d = (nSum / nl) * (rSum / R.nPanels);
			nRingFallback++;
		}
		const float dl = d.length();
		const float cap = miterLimit * maxRel;
		if (dl > cap && dl > 1e-9f) { d *= cap / dl; nClamped++; }
		// Fade to the authored edge — ASYMMETRIC: only motion that RECEDES
		// into the wall opens a view slit under an authored line, so only
		// that fades; proud block ends stay proud right up to the border.
		if (borderScale[pi] < 1.0f) {
			Vector nAvg{ 0, 0, 0 };
			for (int i = 0; i < R.nPanels; ++i) nAvg += panels[R.puv[i].panel].n;
			if (d * nAvg < 0.0f) d *= borderScale[pi];
		}
		if (viz && R.nPanels >= 2) {
			RingDump rd; rd.p = R.p; rd.n = R.nPanels; rd.d = d;
			for (int i = 0; i < R.nPanels; ++i) {
				rd.panel[i] = R.puv[i].panel;
				rd.u[i] = R.puv[i].u; rd.v[i] = R.puv[i].v; rd.rel[i] = rel[i];
			}
			ringDump.push_back(rd);
		}
		for (Vertex *V : R.inst) V->Pos += d;
		maxAbsD = std::max(maxAbsD, d.length());
	}

	// ── pass 4: re-derive face planes; inflate bspheres ─────────────────────
	for (const FRef &fr : frefs) {
		Face *F = fr.F;
		const Vector a = F->A->Pos, b = F->B->Pos, c = F->C->Pos;
		Vector n = (b - a) ^ (c - a);
		const float nl = n.length();
		if (nl < 1e-12f) continue;
		n /= nl;
		if (n * F->N < 0) n = n * -1.0f;             // keep the face's own sign convention
		F->N = n;
		F->NormProd = -(n.x * a.x + n.y * a.y + n.z * a.z);
	}
	if (maxAbsD > 0.0f)
		for (TriMesh *M = Sc->TriMeshHead; M; M = M->Next) {
			M->BSphereRadius += maxAbsD;
			M->BSphereRad = M->BSphereRadius * M->BSphereRadius;
		}

	// The low band the geometry now carries: mip whose texel size matches the
	// median bake cell (face UV diameter). POM must march the residual above
	// this or block edges are counted twice (geometry + parallax).
	for (int m = 0; m < 2; ++m) {
		if (uvSpanTex[m].empty()) { g_residMip[m] = -1; continue; }
		std::nth_element(uvSpanTex[m].begin(),
		                 uvSpanTex[m].begin() + uvSpanTex[m].size() / 2,
		                 uvSpanTex[m].end());
		const float med = uvSpanTex[m][uvSpanTex[m].size() / 2];
		int mip = 0;
		while ((1 << (mip + 1)) <= (int)med && mip < 10) ++mip;
		g_residMip[m] = mip;
	}

	std::fprintf(stderr, "[DISPV3] M3/M4 bake: %d panels, %zu positions, "
	             "%d interior, ring2=%d ring3=%d fallback=%d clamped=%d, "
	             "%d pinned(border+%d ring-adj), junction-edges=%d "
	             "boundary-edges=%d maxAbsD=%.4f amp=%.3f miterLimit=%.2f "
	             "residMip=%d/%d\n",
	             (int)panels.size(), pos.size(), nInterior, nRing2, nRing3,
	             nRingFallback, nClamped, nPinned, nRingBoundary, nJunctionEdges,
	             nBoundaryEdges, maxAbsD, amp, miterLimit,
	             g_residMip[0], g_residMip[1]);

	// ── M1 instrument dump ──────────────────────────────────────────────────
	if (viz) {
		FILE *out = std::fopen("dispv3_graph.txt", "w");
		if (out) {
			std::fprintf(out, "# DISPV3 M1 graph dump (post-subdiv, pre-displacement positions quantized 1/1024)\n");
			for (int p = 0; p < (int)panels.size(); ++p) {
				const PanelRec &P = panels[p];
				std::fprintf(out, "panel %d mat=%s n=(%.4f,%.4f,%.4f) d=%.4f faces=%d "
				             "uv=[%.3f..%.3f]x[%.3f..%.3f] bbox=(%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f)\n",
				             p, P.mat ? "floor" : "rooms", P.n.x, P.n.y, P.n.z, P.d,
				             P.faces, P.uMin, P.uMax, P.vMin, P.vMax,
				             P.lo.x, P.lo.y, P.lo.z, P.hi.x, P.hi.y, P.hi.z);
			}
			for (const ERec &e : erecs) {
				bool multi = false;
				for (int p : e.panels) if (p != e.panels[0]) { multi = true; break; }
				if (!multi && e.nTargetFaces != 1) continue;
				const Vector A = pos[e.p0].p, B = pos[e.p1].p;
				int plA = e.panels[0], plB = plA;
				for (int p : e.panels) if (p != plA) { plB = p; break; }
				std::fprintf(out, "%s (%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) panels=%d,%d\n",
				             multi ? "junction" : "boundary",
				             A.x, A.y, A.z, B.x, B.y, B.z, plA, plB);
			}
			for (const RingDump &rd : ringDump) {
				std::fprintf(out, "ringvert (%.4f,%.4f,%.4f) k=%d d=(%.5f,%.5f,%.5f)",
				             rd.p.x, rd.p.y, rd.p.z, rd.n, rd.d.x, rd.d.y, rd.d.z);
				for (int i = 0; i < rd.n; ++i)
					std::fprintf(out, " s%d=%d,%.5f,%.5f,%.5f",
					             i, rd.panel[i], rd.u[i], rd.v[i], rd.rel[i]);
				std::fprintf(out, "\n");
			}
			std::fclose(out);
			std::fprintf(stderr, "[DISPV3] M1 graph -> dispv3_graph.txt\n");
		}
	}
}
