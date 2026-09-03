// SplitMeshTJunctions — pre-displacement topological repair for stone meshes.
// See SplitTJunctions.h.

#include "SplitTJunctions.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>
#include <Base/TriMesh.h>
#include <Base/Face.h>
#include "MeshOps.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>

extern void Compute_FaceVertexIndices(TriMesh *T);

namespace rev {
namespace {

static inline Vector v_sub(const Vector &a, const Vector &b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline Vector v_add(const Vector &a, const Vector &b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline Vector v_scale(const Vector &a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static inline float v_dot(const Vector &a, const Vector &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float v_lensq(const Vector &a) { return v_dot(a, a); }
static inline float v_len(const Vector &a) { return std::sqrt(v_lensq(a)); }
static inline float v_dist(const Vector &a, const Vector &b) { return v_len(v_sub(a, b)); }

struct FaceRecord {
	Face proto;
	std::array<uint32_t, 3> idx;
	float u[3], v[3];
};

}  // namespace

int SplitMeshTJunctions(Scene *Sc, const char *const *mats, int nMats, float eps)
{
	if (!Sc || !mats || nMats <= 0) return 0;

	auto isTargetMat = [&](const char *name) -> bool {
		if (!name) return false;
		for (int m = 0; m < nMats; ++m)
			if (!std::strcmp(name, mats[m])) return true;
		return false;
	};

	int totalSplitsAllMeshes = 0;

	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
		if (T->FIndex == 0 || !T->Faces || !T->Verts) continue;

		int nTargetFaces = 0;
		for (DWord i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (F.A && F.B && F.C && F.Txtr && isTargetMat(F.Txtr->Name))
				++nTargetFaces;
		}
		if (nTargetFaces == 0) continue;

		Vertex *const oldV = T->Verts;
		auto vidx = [&](const Vertex *v) -> uint32_t { return uint32_t(v - oldV); };

		std::vector<Vertex> verts(oldV, oldV + T->VIndex);

		// Step 1: Find unique geometric vertex positions among target faces
		std::vector<Vector> uniquePos;
		std::vector<uint32_t> vertToUnique(verts.size(), UINT32_MAX);
		std::vector<uint32_t> uniqueToVert;

		for (DWord i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C || !F.Txtr || !isTargetMat(F.Txtr->Name)) continue;
			const Vertex *corners[3] = { F.A, F.B, F.C };
			for (int k = 0; k < 3; ++k) {
				const uint32_t vi = vidx(corners[k]);
				if (vertToUnique[vi] != UINT32_MAX) continue;

				const Vector &p = verts[vi].Pos;
				int found = -1;
				for (size_t u = 0; u < uniquePos.size(); ++u) {
					if (v_dist(p, uniquePos[u]) < eps) {
						found = int(u);
						break;
					}
				}
				if (found < 0) {
					found = int(uniquePos.size());
					uniquePos.push_back(p);
					uniqueToVert.push_back(vi);
				}
				vertToUnique[vi] = uint32_t(found);
			}
		}

		// Separate faces into non-target and target records
		std::vector<FaceRecord> nonTargetFaces;
		std::vector<FaceRecord> targetFaces;

		for (DWord i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C) continue;
			FaceRecord rec;
			rec.proto = F;
			rec.idx = { vidx(F.A), vidx(F.B), vidx(F.C) };
			rec.u[0] = F.U1; rec.v[0] = F.V1;
			rec.u[1] = F.U2; rec.v[1] = F.V2;
			rec.u[2] = F.U3; rec.v[2] = F.V3;

			if (F.Txtr && isTargetMat(F.Txtr->Name)) {
				targetFaces.push_back(rec);
			} else {
				nonTargetFaces.push_back(rec);
			}
		}

		// Step 2: Iteratively split edges that contain T-junction vertices
		int meshSplits = 0;
		bool changed = true;
		int round = 0;

		while (changed && round < 8) {
			changed = false;
			++round;
			std::vector<FaceRecord> nextFaces;
			nextFaces.reserve(targetFaces.size() + 16);

			for (size_t fi = 0; fi < targetFaces.size(); ++fi) {
				const FaceRecord &rec = targetFaces[fi];
				bool splitFound = false;

				for (int k = 0; k < 3; ++k) {
					const int k1 = (k + 1) % 3;
					const int k2 = (k + 2) % 3; // opposite corner

					const uint32_t i0 = rec.idx[k];
					const uint32_t i1 = rec.idx[k1];
					const Vector &p0 = verts[i0].Pos;
					const Vector &p1 = verts[i1].Pos;

					const Vector edgeVec = v_sub(p1, p0);
					const float L = v_len(edgeVec);
					if (L < eps) continue;

					int bestU = -1;
					float bestT = -1.0f;
					float bestDist = 1e30f;

					for (size_t u = 0; u < uniquePos.size(); ++u) {
						const Vector &up = uniquePos[u];
						if (v_dist(up, p0) < eps || v_dist(up, p1) < eps) continue;

						const float t = v_dot(v_sub(up, p0), edgeVec) / (L * L);
						if (t <= (eps / L) || t >= (1.0f - eps / L)) continue;

						const Vector proj = v_add(p0, v_scale(edgeVec, t));
						const float d = v_dist(up, proj);
						if (d < eps && d < bestDist) {
							bestDist = d;
							bestT = t;
							bestU = int(u);
						}
					}

					if (bestU >= 0) {
						// Split face at bestU!
						const uint32_t midVertIdx = uniqueToVert[bestU];
						const float midU = (1.0f - bestT) * rec.u[k] + bestT * rec.u[k1];
						const float midV = (1.0f - bestT) * rec.v[k] + bestT * rec.v[k1];

						// Tri 1: (k, mid, k2)
						FaceRecord t1;
						t1.proto = rec.proto;
						t1.idx = { rec.idx[k], midVertIdx, rec.idx[k2] };
						t1.u[0] = rec.u[k];   t1.v[0] = rec.v[k];
						t1.u[1] = midU;       t1.v[1] = midV;
						t1.u[2] = rec.u[k2];  t1.v[2] = rec.v[k2];

						// Tri 2: (mid, k1, k2)
						FaceRecord t2;
						t2.proto = rec.proto;
						t2.idx = { midVertIdx, rec.idx[k1], rec.idx[k2] };
						t2.u[0] = midU;       t2.v[0] = midV;
						t2.u[1] = rec.u[k1];  t2.v[1] = rec.v[k1];
						t2.u[2] = rec.u[k2];  t2.v[2] = rec.v[k2];

						nextFaces.push_back(t1);
						nextFaces.push_back(t2);

						++meshSplits;
						changed = true;
						splitFound = true;
						break;
					}
				}

				if (!splitFound) {
					nextFaces.push_back(rec);
				}
			}

			targetFaces = std::move(nextFaces);
		}

		if (meshSplits == 0) continue;

		totalSplitsAllMeshes += meshSplits;

		// Step 3: Commit new faces and vertices back to TriMesh T
		const size_t totalFaces = nonTargetFaces.size() + targetFaces.size();
		Vertex *nv = new Vertex[verts.size()];
		std::memcpy(nv, verts.data(), verts.size() * sizeof(Vertex));

		Face *nf = new Face[totalFaces];
		size_t dst = 0;

		auto commitRec = [&](const FaceRecord &r) {
			Face &f = nf[dst++];
			f = r.proto;
			f.A = &nv[r.idx[0]];
			f.B = &nv[r.idx[1]];
			f.C = &nv[r.idx[2]];
			f.U1 = r.u[0]; f.V1 = r.v[0];
			f.U2 = r.u[1]; f.V2 = r.v[1];
			f.U3 = r.u[2]; f.V3 = r.v[2];
			f.EU1 = r.u[0]; f.EV1 = r.v[0];
			f.EU2 = r.u[1]; f.EV2 = r.v[1];
			f.EU3 = r.u[2]; f.EV3 = r.v[2];
			f.NormProd = -(f.N.x * f.A->Pos.x + f.N.y * f.A->Pos.y + f.N.z * f.A->Pos.z);
		};

		for (const auto &r : nonTargetFaces) commitRec(r);
		for (const auto &r : targetFaces) commitRec(r);

		delete[] T->Verts;
		delete[] T->Faces;
		T->Verts = nv;
		T->VIndex = int32_t(verts.size());
		T->Faces = nf;
		T->FIndex = int32_t(totalFaces);

		Compute_FaceVertexIndices(T);
		if (T->Flags & Tri_Stationary) {
			T->SL = (Color *)getAlignedBlock(sizeof(Color) * T->VIndex, 16);
		}

		std::fprintf(stderr, "[STONE-TSPLIT] '%s': split %d T-junctions in %d rounds (%d target faces -> %zu)\n",
		             mats[0], meshSplits, round, nTargetFaces, targetFaces.size());
	}

	return totalSplitsAllMeshes;
}

}  // namespace rev
