#include "MeshOps.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>

#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

// Per-mesh deduplicated-vertex storage. std::vector keeps the array
// alive for the lifetime of the scene without raw new/delete; std::map
// keys by TriMesh* so storage is naturally scoped to the scene that
// owns the TriMesh.
std::map<TriMesh*, std::vector<Vertex>> g_independentVerts;

// Returns true iff some pair of faces sharing a vertex has normals
// more than `thresholdDegrees` apart. Cheap O(V * f²) where f is the
// fanout (typically 4–8 for meshes); plenty fast at scene init.
bool MeshHasCreases(TriMesh *T, float thresholdDegrees) {
	if (!T || T->FIndex == 0 || !T->Faces) return false;

	const float cosThr = std::cos(thresholdDegrees * float(PI) / 180.0f);

	std::unordered_map<Vertex *, std::vector<Face *>> incident;
	incident.reserve(size_t(T->FIndex) * 3);
	for (int32_t i = 0; i < T->FIndex; ++i) {
		Face *F = &T->Faces[i];
		if (F->A) incident[F->A].push_back(F);
		if (F->B) incident[F->B].push_back(F);
		if (F->C) incident[F->C].push_back(F);
	}

	for (auto &kv : incident) {
		auto &faces = kv.second;
		for (size_t i = 0; i < faces.size(); ++i) {
			for (size_t j = i + 1; j < faces.size(); ++j) {
				const float d = Dot_Product(&faces[i]->N, &faces[j]->N);
				if (d < cosThr) return true;
			}
		}
	}
	return false;
}

} // namespace

void MakeFacesIndependent(TriMesh *T, float smoothingThresholdDegrees) {
	if (!T || T->FIndex == 0 || !T->Faces) return;

	// Build the "incident faces per vertex" map BEFORE we duplicate so
	// the keys are still the original (shared) Vertex pointers. Used
	// below to compute per-smoothing-group averaged normals — for each
	// face's vertex copy, average the face normals of incident faces
	// that are within `smoothingThresholdDegrees` of THIS face's normal.
	// Vertices on flat regions keep a smooth averaged normal; vertices
	// at crease boundaries get the face normal alone (no other face is
	// within threshold). This is the deferred path's prerequisite for
	// per-pixel Blinn-Phong instead of per-polygon highlights — without
	// it the rasterizer interpolates a constant normal across every
	// triangle and the half-vector dot product is uniform.
	std::unordered_map<Vertex *, std::vector<Face *>> incident;
	incident.reserve(size_t(T->FIndex) * 3);
	for (int32_t i = 0; i < T->FIndex; ++i) {
		Face *F = &T->Faces[i];
		if (F->A) incident[F->A].push_back(F);
		if (F->B) incident[F->B].push_back(F);
		if (F->C) incident[F->C].push_back(F);
	}
	const float cosSmoothing = std::cos(smoothingThresholdDegrees * float(PI) / 180.0f);

	auto computeSmoothedNormal = [&](Vertex *origVtx, const Face *face) {
		Vector accum{};
		Vector_Form(&accum, 0, 0, 0);
		auto it = incident.find(origVtx);
		if (it == incident.end()) return face->N;
		for (Face *adj : it->second) {
			if (Dot_Product(&face->N, &adj->N) >= cosSmoothing) {
				// Area weight (matches Compute_Vertex_Normals).
				Vector w;
				Vector_Scale(&adj->N,
				             Tri_Surface(&adj->A->Pos, &adj->B->Pos, &adj->C->Pos),
				             &w);
				Vector_SelfAdd(&accum, &w);
			}
		}
		if (Vector_Length(&accum) < EPSILON) return face->N;
		Vector_Norm(&accum);
		return accum;
	};

	const int32_t newCount = T->FIndex * 3;
	auto &storage = g_independentVerts[T];
	storage.assign(size_t(newCount), Vertex{});
	Vertex *newVerts = storage.data();

	for (int32_t i = 0; i < T->FIndex; ++i) {
		Face *F = &T->Faces[i];
		if (!F->A || !F->B || !F->C) continue;
		Vertex *na = &newVerts[3 * i + 0];
		Vertex *nb = &newVerts[3 * i + 1];
		Vertex *nc = &newVerts[3 * i + 2];
		Vertex *origA = F->A, *origB = F->B, *origC = F->C;
		*na = *origA;
		*nb = *origB;
		*nc = *origC;
		na->N = computeSmoothedNormal(origA, F);
		nb->N = computeSmoothedNormal(origB, F);
		nc->N = computeSmoothedNormal(origC, F);
		F->A = na;
		F->B = nb;
		F->C = nc;
	}

	T->Verts = newVerts;
	T->VIndex = newCount;

	// T->SL (static-lighting cache) was sized for the original VIndex.
	// StaticLighting iterates [0, T->VIndex) writing T->SL[vi] — without
	// growing SL too, those writes overflow into neighbouring TriMesh
	// structs. Reallocate to match. Old SL is leaked along with old Verts.
	if (T->Flags & Tri_Stationary) {
		T->SL = (Color *)getAlignedBlock(sizeof(Color) * newCount, 16);
	}
}

// Translate linear (x, y) pixel coordinate into the swizzled byte
// offset used by Generate_Mipmaps' block-tiled layout. Outer loop is
// block columns (x), inner is block rows (y); within each block,
// rows then columns. Mirrors IMGCODE.CPP:1547-1562.
static inline size_t SwizzledOffset(int x, int y, int blockSizeX, int blockSizeY,
                                     int SizeY) {
	const int BX = 1 << blockSizeX;
	const int BY = 1 << blockSizeY;
	const int blockRowsPerCol = SizeY >> blockSizeY;
	const int bx = x >> blockSizeX;
	const int by = y >> blockSizeY;
	const int k  = x & (BX - 1);
	const int j  = y & (BY - 1);
	return (size_t(bx) * blockRowsPerCol + by) * size_t(BX * BY)
	       + size_t(j) * BX + k;
}

Texture *BakeNormalMapFromDiffuse(Texture *diffuse, float strength) {
	if (!diffuse || diffuse->BPP != 32 || !diffuse->Mipmap[0]) return nullptr;
	const int W = diffuse->SizeX;
	const int H = diffuse->SizeY;
	if (W <= 0 || H <= 0) return nullptr;

	Texture *nm = new Texture;
	*nm = *diffuse;  // copy basic fields (BPP, SizeX/Y, LSizeX/Y, blockSizeX/Y, OptClass)
	nm->Pal       = nullptr;
	nm->FileName  = nullptr;
	nm->ID        = 0;
	nm->Flags     = 0;
	for (int i = 0; i < 16; ++i) nm->Mipmap[i] = nullptr;

	// Allocate level-0 storage matching the diffuse's layout. We only
	// populate mip 0 — the rasterizer's miplevel value will index into
	// nm->Mipmap[miplevel], which is null past 0; the kernel already
	// null-checks `nmData` before sampling, so far-mip pixels just
	// fall back to the geometric normal.
	const int blockX = diffuse->blockSizeX;
	const int blockY = diffuse->blockSizeY;
	const int BX     = 1 << blockX;
	const int BY     = 1 << blockY;
	const int X      = W >> blockX;  // block columns
	const int Y      = H >> blockY;  // block rows
	const size_t numPixels = size_t(X) * size_t(Y) * size_t(BX) * size_t(BY);
	nm->Data       = (byte*)getAlignedBlock(numPixels * sizeof(uint32_t));
	nm->Mipmap[0]  = nm->Data;
	nm->numMipmaps = 1;

	const uint32_t *src = reinterpret_cast<const uint32_t*>(diffuse->Mipmap[0]);
	uint32_t       *dst = reinterpret_cast<uint32_t*>(nm->Data);

	auto fetchLum = [&](int x, int y) -> float {
		x = ((x % W) + W) % W;  // wrap
		y = ((y % H) + H) % H;
		const uint32_t px = src[SwizzledOffset(x, y, blockX, blockY, H)];
		const float b = float(px & 0xFF);
		const float g = float((px >> 8) & 0xFF);
		const float r = float((px >> 16) & 0xFF);
		return 0.299f * r + 0.587f * g + 0.114f * b;
	};

	const float invStrength = strength * (1.0f / 255.0f);
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			// Sobel-X / Sobel-Y of luminance over the 3x3 neighborhood.
			const float lTL = fetchLum(x-1, y-1);
			const float lT  = fetchLum(x  , y-1);
			const float lTR = fetchLum(x+1, y-1);
			const float lL  = fetchLum(x-1, y  );
			const float lR  = fetchLum(x+1, y  );
			const float lBL = fetchLum(x-1, y+1);
			const float lB  = fetchLum(x  , y+1);
			const float lBR = fetchLum(x+1, y+1);
			const float gx = (lTR + 2.0f*lR + lBR) - (lTL + 2.0f*lL + lBL);
			const float gy = (lBL + 2.0f*lB + lBR) - (lTL + 2.0f*lT + lTR);

			// World-space normal — assumes the surface base normal
			// points along +Y. (gx, gy) is the heightmap gradient in
			// texture (u, v) space; mapped to world (x, z) for a
			// horizontal surface.
			float nx = -gx * invStrength;
			float ny =  1.0f;
			float nz = -gy * invStrength;
			const float invLen = 1.0f / std::sqrt(nx*nx + ny*ny + nz*nz);
			nx *= invLen; ny *= invLen; nz *= invLen;

			const uint8_t r = uint8_t((nx + 1.0f) * 127.5f);
			const uint8_t g = uint8_t((ny + 1.0f) * 127.5f);
			const uint8_t b = uint8_t((nz + 1.0f) * 127.5f);
			const uint32_t packed = uint32_t(b)
			                      | (uint32_t(g) << 8)
			                      | (uint32_t(r) << 16)
			                      | 0xFF000000u;
			dst[SwizzledOffset(x, y, blockX, blockY, H)] = packed;
		}
	}

	// Mip levels 1..N — downsample by averaging each 2x2 block of
	// normals from the previous level, then renormalize. Without
	// this, far pixels at mip > 0 see Mipmap[miplevel]==null and the
	// lighting kernel falls back to the geometric normal, producing
	// a visible discontinuity at the mip-switch distance.
	nm->numMipmaps = diffuse->numMipmaps > 0 ? diffuse->numMipmaps : 1;
	int prevW = W, prevH = H;
	int prevX = X, prevY = Y;
	const uint32_t *prevData = dst;
	size_t mipDataOffset = numPixels;  // we'll allocate one big block after counting

	// First count total pixels across all mip levels (matches Generate_Mipmaps).
	{
		size_t total = numPixels;
		int curX = X, curY = Y;
		for (uint32_t i = 1; i < nm->numMipmaps; ++i) {
			curX = (curX + 1) >> 1;
			curY = (curY + 1) >> 1;
			total += size_t(curX) * size_t(curY) * size_t(BX) * size_t(BY);
		}
		// Resize the data buffer to hold all levels. Re-alloc + copy
		// the mip-0 we already populated.
		byte *newBuf = (byte*)getAlignedBlock(total * sizeof(uint32_t));
		std::memcpy(newBuf, nm->Data, numPixels * sizeof(uint32_t));
		nm->Data = newBuf;
		nm->Mipmap[0] = newBuf;
		prevData = reinterpret_cast<const uint32_t*>(nm->Mipmap[0]);
	}

	for (uint32_t mi = 1; mi < nm->numMipmaps; ++mi) {
		const int curX = (prevX + 1) >> 1;
		const int curY = (prevY + 1) >> 1;
		const int curW = curX << blockX;
		const int curH = curY << blockY;
		uint32_t *curData = reinterpret_cast<uint32_t*>(nm->Data) + mipDataOffset;
		nm->Mipmap[mi] = reinterpret_cast<byte*>(curData);

		for (int y = 0; y < curH; ++y) {
			for (int x = 0; x < curW; ++x) {
				// Sample 2x2 in previous level.
				float ax = 0, ay = 0, az = 0;
				for (int dy = 0; dy < 2; ++dy) {
					for (int dx = 0; dx < 2; ++dx) {
						const int sx = std::min(2*x + dx, prevW - 1);
						const int sy = std::min(2*y + dy, prevH - 1);
						const uint32_t px = prevData[SwizzledOffset(sx, sy, blockX, blockY, prevH)];
						const float r = (float((px >> 16) & 0xFF) * (1.0f/127.5f)) - 1.0f;
						const float g = (float((px >>  8) & 0xFF) * (1.0f/127.5f)) - 1.0f;
						const float b = (float( px        & 0xFF) * (1.0f/127.5f)) - 1.0f;
						ax += r; ay += g; az += b;
					}
				}
				const float invLen = 1.0f / std::sqrt(ax*ax + ay*ay + az*az);
				ax *= invLen; ay *= invLen; az *= invLen;
				const uint8_t r8 = uint8_t((ax + 1.0f) * 127.5f);
				const uint8_t g8 = uint8_t((ay + 1.0f) * 127.5f);
				const uint8_t b8 = uint8_t((az + 1.0f) * 127.5f);
				curData[SwizzledOffset(x, y, blockX, blockY, curH)] =
					uint32_t(b8) | (uint32_t(g8) << 8) | (uint32_t(r8) << 16) | 0xFF000000u;
			}
		}

		prevW = curW; prevH = curH;
		prevX = curX; prevY = curY;
		prevData = curData;
		mipDataOffset += size_t(curX) * size_t(curY) * size_t(BX) * size_t(BY);
	}

	return nm;
}

void MakeFacesIndependentByAngle(Scene *Sc, float thresholdDegrees) {
	if (!Sc) return;
	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
		if (T->FIndex == 0 || !T->Faces) continue;
		if (!MeshHasCreases(T, thresholdDegrees)) continue;
		// Same threshold gates both: a face pair within `thresholdDegrees`
		// is "smooth" (averaged normal); beyond is a "crease" (face
		// normal alone).
		MakeFacesIndependent(T, thresholdDegrees);
	}
}
