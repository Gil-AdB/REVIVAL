// --greets_mesh_dump — see MeshDump.h.

#include "MeshDump.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace rev {

long long DumpStoneObj(Scene *Sc, const char *const *mats, int nMats, const char *path, const char *tag)
{
	if (!Sc || !mats || nMats <= 0 || !path) return 0;
	FILE *f = std::fopen(path, "w");
	if (!f) {
		std::fprintf(stderr, "[MESH-DUMP] cannot open %s for writing\n", path);
		return 0;
	}
	std::fprintf(f, "# REVIVAL greets stone mesh, %s\n# world units; vt = (u, 1-v); winding agrees with the engine face normal\n",
	             tag ? tag : "");

	long long nFaces = 0, nVerts = 0;
	// One OBJ vertex per engine Vertex, per TriMesh, but only those a target
	// face uses; OBJ indices are 1-based and global across meshes.
	for (TriMesh *M = Sc->TriMeshHead; M; M = M->Next) {
		if (M->FIndex == 0 || !M->Faces || !M->Verts) continue;
		std::map<const Vertex *, long long> idx;   // Vertex -> OBJ index
		std::vector<const Face *> faces;
		std::vector<int> faceMat;
		for (DWord i = 0; i < M->FIndex; ++i) {
			const Face &F = M->Faces[i];
			if (!F.A || !F.B || !F.C || !F.Txtr || !F.Txtr->Name) continue;
			int mi = -1;
			for (int m = 0; m < nMats; ++m)
				if (!std::strcmp(F.Txtr->Name, mats[m])) { mi = m; break; }
			if (mi < 0) continue;
			faces.push_back(&F); faceMat.push_back(mi);
			for (const Vertex *v : { F.A, F.B, F.C })
				if (!idx.count(v)) { idx[v] = ++nVerts; std::fprintf(f, "v %.6f %.6f %.6f\n", v->Pos.x, v->Pos.y, v->Pos.z); }
		}
		if (faces.empty()) continue;
		std::fprintf(f, "o mesh_%lld\n", nFaces);
		int curMat = -1;
		// vt: three per face, written just before the face; vt indices are
		// global in the file, 3 per face written so far
		for (size_t k = 0; k < faces.size(); ++k) {
			const Face &F = *faces[k];
			if (faceMat[k] != curMat) { curMat = faceMat[k]; std::fprintf(f, "usemtl %s\ng %s\n", mats[curMat], mats[curMat]); }
			// winding: make the geometric normal agree with the engine's Face::N
			const Vertex *c[3] = { F.A, F.B, F.C };
			float uv[3][2] = { { F.U1, F.V1 }, { F.U2, F.V2 }, { F.U3, F.V3 } };
			const float ex = F.B->Pos.x - F.A->Pos.x, ey = F.B->Pos.y - F.A->Pos.y, ez = F.B->Pos.z - F.A->Pos.z;
			const float fx = F.C->Pos.x - F.A->Pos.x, fy = F.C->Pos.y - F.A->Pos.y, fz = F.C->Pos.z - F.A->Pos.z;
			const float nx = ey*fz - ez*fy, ny = ez*fx - ex*fz, nz = ex*fy - ey*fx;
			const float d = nx*F.N.x + ny*F.N.y + nz*F.N.z;
			if (d < 0.0f) {   // swap B and C
				const Vertex *t = c[1]; c[1] = c[2]; c[2] = t;
				float tu = uv[1][0], tv = uv[1][1]; uv[1][0] = uv[2][0]; uv[1][1] = uv[2][1]; uv[2][0] = tu; uv[2][1] = tv;
			}
			for (int j = 0; j < 3; ++j) std::fprintf(f, "vt %.6f %.6f\n", uv[j][0], 1.0f - uv[j][1]);
			++nFaces;
			std::fprintf(f, "f %lld/%lld %lld/%lld %lld/%lld\n",
			             idx[c[0]], 3 * nFaces - 2, idx[c[1]], 3 * nFaces - 1, idx[c[2]], 3 * nFaces);
		}
	}
	std::fclose(f);
	std::fprintf(stderr, "[MESH-DUMP] %s: %lld faces, %lld vertices -> %s\n", tag ? tag : "", nFaces, nVerts, path);
	return nFaces;
}

}  // namespace rev
