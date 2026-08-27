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
			rel[i] = amp * (f.sample(R.puv[i].u, R.puv[i].v) - f.mean);
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
			std::fclose(out);
			std::fprintf(stderr, "[DISPV3] M1 graph -> dispv3_graph.txt\n");
		}
	}
}
