// DISPLACE v3 — clean-room junction-correct stone displacement.
// Design: docs/DISPLACE_V3_DESIGN.md. This file implements M1 (panel +
// junction graph, instrument dump) and M2 (interiors-only displacement along
// PANEL PLANE normals; junction ring verts carried as first-class data but
// PINNED at zero until M3's castellated height-profiled mitre).
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
	int   panel = -1;                   // owning panel if single-panel
	bool  multiPanel = false;           // junction candidate (>=2 panels)
	bool  boundary = false;             // on an edge with only ONE target face
	bool  haveUV = false;
	float u = 0, v = 0;
};

} // namespace

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
				if (R.panel < 0) R.panel = pl;
				else if (R.panel != pl) R.multiPanel = true;
				if (!R.haveUV) { R.u = us[k]; R.v = vts[k]; R.haveUV = true; }
				P.uMin = std::min(P.uMin, us[k]); P.uMax = std::max(P.uMax, us[k]);
				P.vMin = std::min(P.vMin, vts[k]); P.vMax = std::max(P.vMax, vts[k]);
				const Vector &pp = vs[k]->Pos;
				P.lo.x = std::min(P.lo.x, pp.x); P.hi.x = std::max(P.hi.x, pp.x);
				P.lo.y = std::min(P.lo.y, pp.y); P.hi.y = std::max(P.hi.y, pp.y);
				P.lo.z = std::min(P.lo.z, pp.z); P.hi.z = std::max(P.hi.z, pp.z);
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

	// ── pass 2: pin classification (M2: ring + boundary verts pinned) ───────
	int nJunctionEdges = 0, nBoundaryEdges = 0;
	for (const ERec &e : erecs) {
		bool multi = false;
		for (int p : e.panels) if (p != e.panels[0]) { multi = true; break; }
		if (e.nTargetFaces == 1) { pos[e.p0].boundary = pos[e.p1].boundary = true; nBoundaryEdges++; }
		if (multi) { pos[e.p0].multiPanel = pos[e.p1].multiPanel = true; nJunctionEdges++; }
	}

	// ── pass 3: displace interiors along the PANEL PLANE normal ─────────────
	int nInterior = 0, nPinned = 0, nNoUV = 0;
	float maxAbsD = 0.0f;
	for (PosRec &R : pos) {
		if (R.panel < 0) continue;
		if (R.multiPanel || R.boundary) { nPinned++; continue; }   // M3 drops in here
		const HeightField &f = hf[panels[R.panel].mat];
		if (!f.ok || !R.haveUV) { nNoUV++; continue; }
		const float dh = amp * (f.sample(R.u, R.v) - f.mean);
		const Vector dp = panels[R.panel].n * dh;
		for (Vertex *V : R.inst) V->Pos += dp;
		maxAbsD = std::max(maxAbsD, std::fabs(dh));
		nInterior++;
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

	std::fprintf(stderr, "[DISPV3] M2 bake: %d panels, %zu positions, "
	             "%d interior displaced, %d pinned (ring/boundary), %d no-uv, "
	             "junction-edges=%d boundary-edges=%d maxAbsD=%.4f amp=%.3f\n",
	             (int)panels.size(), pos.size(), nInterior, nPinned, nNoUV,
	             nJunctionEdges, nBoundaryEdges, maxAbsD, amp);

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
