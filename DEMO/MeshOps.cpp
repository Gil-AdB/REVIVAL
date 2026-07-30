#include "MeshOps.h"

#include "MaterialEditor.h"       // rev::Editor_BaseSurfName (collapse ::mirUV clones)
#include <FLD/LWREAD.H>           // Surf_Smoothing flag (authored per-surface smoothing)
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Scene.h>
#include <Threads.h>              // dispatchIndexed — threaded cone-map bake

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <semaphore>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// PREPROC.CPP — recompute per-vertex tangents from the current Faces +
// per-vertex N. Not declared in FDS_DECS.H, so forward-declare locally.
void Compute_Vertex_Tangents(TriMesh *T);
void Compute_FaceVertexIndices(TriMesh *T);

namespace {

// Per-mesh deduplicated-vertex storage. std::vector keeps the array
// alive for the lifetime of the scene without raw new/delete; std::map
// keys by TriMesh* so storage is naturally scoped to the scene that
// owns the TriMesh.
std::map<TriMesh*, std::vector<Vertex>> g_independentVerts;

// Per-surface smoothing-angle overrides (base material name -> degrees).
// Empty unless a sidecar sets one => default render is byte-identical. See
// MeshOps.h for the semantics; consumed by MakeFacesIndependent below.
std::map<std::string, float> g_surfaceSmoothAngle;

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

void MeshOps_SetSurfaceSmoothAngle(const char *surface, float angleDeg) {
	if (!surface || !*surface) return;
	if (angleDeg < 0.0f)   angleDeg = 0.0f;
	if (angleDeg > 180.0f) angleDeg = 180.0f;
	// Key by BASE name so the ::mirUV handedness clones of a surface share the
	// override (they draw the same geometry — see rev::Editor_BaseSurfName).
	g_surfaceSmoothAngle[rev::Editor_BaseSurfName(surface)] = angleDeg;
}

bool MeshOps_GetSurfaceSmoothAngle(const char *surface, float &angleDegOut) {
	if (!surface || g_surfaceSmoothAngle.empty()) return false;
	auto it = g_surfaceSmoothAngle.find(rev::Editor_BaseSurfName(surface));
	if (it == g_surfaceSmoothAngle.end()) return false;
	angleDegOut = it->second;
	return true;
}

bool MeshOps_AnySurfaceSmoothAngle() { return !g_surfaceSmoothAngle.empty(); }

int MeshOps_SeedAuthoredSmoothAngles(Scene *sc, float defaultDeg, float epsDeg) {
	if (!sc) return 0;
	int seeded = 0;
	// Convert authored SMAN (radians) -> degrees with the SAME expression
	// MakeFacesIndependent's authored path uses, so the seeded value and any
	// future flag-on read agree bit-for-bit.
	for (Material *M = MatLib; M; M = M->Next) {
		if (M->RelScene != sc || !M->Name) continue;
		if (!(M->TFlags & Surf_Smoothing)) continue;    // not authored smooth
		const float deg = M->MaxSmoothingAngle * (180.0f / float(PI));
		if (std::fabs(deg - defaultDeg) <= epsDeg) continue;   // at the default
		MeshOps_SetSurfaceSmoothAngle(M->Name, deg);
		++seeded;
	}
	return seeded;
}

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

	// The two mummies ('momy-1'/'momy-2') are identical organic lathes that
	// LightWave authored as smooth (Surf_Smoothing, MaxSmoothingAngle ~1.6 rad
	// ≈ 92°). The global architectural crease threshold (30°, tuned for hard
	// edges like the City corners) splits their radial facets and the bodies go
	// faceted. For those surfaces, honor the authored smoothing instead, and
	// smooth only among each surface's own faces so it can't bleed into the
	// floor it sits on. Both mummies belong (per-surface case; the 2026-07-30
	// split-bake made the second a distinct surface — without it that mummy
	// re-facets). Every other surface keeps the global threshold => other
	// scenes unchanged.
	auto matIsMomy = [](const Face *f) {
		if (!f || !f->Txtr || !f->Txtr->Name) return false;
		const char *n = f->Txtr->Name;
		return !std::strcmp(n, "momy-1") || !std::strcmp(n, "momy-2");
	};

	// Per-surface smoothing angle. Two opt-in sources, both superseding the
	// legacy momy/global-crease path for the affected surface and both
	// restricting averaging to the surface's OWN incident faces (a material
	// boundary is a hard edge — matches LightWave; keeps momy off the floor):
	//   1. An explicit sidecar override (surface|smoothAngle|value). Wins.
	//   2. The AUTHORED LWO/FLD angle (Material::MaxSmoothingAngle, radians)
	//      when --surf_smoothing_authored is on and the surface is flagged
	//      Surf_Smoothing (else that surface is left faceted).
	// The effective angle then gates the fan: adj is averaged iff within it of
	// THIS face's normal (180 => true shared normal / fully smooth, 0 =>
	// faceted). Both sources are off by default => the block below is inert
	// and the render is byte-identical.
	const bool anyOverride = MeshOps_AnySurfaceSmoothAngle();
	const bool useAuthored = fds::FeatureFlags::surf_smoothing_authored();
	auto surfName = [](const Face *f) -> const char * {
		return (f && f->Txtr && f->Txtr->Name) ? f->Txtr->Name : nullptr;
	};

	auto computeSmoothedNormal = [&](Vertex *origVtx, const Face *face) {
		const char *fname = surfName(face);
		float effAngle = 0.0f;
		bool perSurf = false;
		if (anyOverride && fname && MeshOps_GetSurfaceSmoothAngle(fname, effAngle)) {
			perSurf = true;                     // explicit sidecar override
		} else if (useAuthored && face->Txtr) {
			effAngle = (face->Txtr->TFlags & Surf_Smoothing)
				? face->Txtr->MaxSmoothingAngle * (180.0f / float(PI))   // rad -> deg
				: 0.0f;                          // not authored smooth => faceted
			perSurf = true;
		}
		const float cosPerSurf = perSurf
			? std::cos(effAngle * float(PI) / 180.0f) : 0.0f;
		const std::string fbase = perSurf
			? rev::Editor_BaseSurfName(fname ? fname : "") : std::string();
		const bool momy = !perSurf && matIsMomy(face);
		Vector accum{};
		Vector_Form(&accum, 0, 0, 0);
		auto it = incident.find(origVtx);
		if (it == incident.end()) return face->N;
		for (Face *adj : it->second) {
			if (perSurf) {
				const char *aname = surfName(adj);
				if (!aname || rev::Editor_BaseSurfName(aname) != fbase) continue;
				if (Dot_Product(&face->N, &adj->N) < cosPerSurf) continue;
			} else if (momy) {
				// True shared per-vertex normal: average EVERY incident momy
				// face, with NO per-face-relative angle gate. The gate (used
				// below for the architectural crease pass) is exactly what
				// breaks continuity for a smooth surface: faces A and B sharing
				// a vertex would each keep only the neighbours within threshold
				// of THEIR own normal, so the two copies of that vertex get
				// DIFFERENT averaged normals and Phong interpolation jumps at
				// the shared edge — the visible seam. Averaging all incident
				// momy faces gives every copy the IDENTICAL normal => continuous
				// shading across every edge (this is Compute_Vertex_Normals,
				// reproduced per face-clone). Restricted to momy faces so it
				// can't blend into the floor the mummy stands on.
				if (!matIsMomy(adj)) continue;
			} else if (Dot_Product(&face->N, &adj->N) < cosSmoothing) {
				continue;   // architectural crease: only neighbours within threshold
			}
			// Area weight (matches Compute_Vertex_Normals).
			Vector w;
			Vector_Scale(&adj->N,
			             Tri_Surface(&adj->A->Pos, &adj->B->Pos, &adj->C->Pos),
			             &w);
			Vector_SelfAdd(&accum, &w);
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

	// SoA Phase 6.2 fix: A_idx/B_idx/C_idx were stamped at scene init
	// (PREPROC.CPP:Scene_Computations) against the OLD T->Verts; the
	// loop above repointed F->A/B/C into per-face-cloned newVerts but
	// left A_idx pointing at the original shared-vertex indices. Without
	// this restamp, F->A - T->Verts no longer equals F->A_idx — every
	// SoA consumer (T->frame->TPos_x[A_idx], etc.) reads the wrong slot.
	// Re-stamp now that the new Verts is committed.
	Compute_FaceVertexIndices(T);

	// Recompute per-vertex tangents against the new (per-face-cloned)
	// normals. Without this, each clone keeps the original shared
	// vertex's Tangent — which was computed against the old smooth
	// normal — and the deferred kernel's TBN frame is wrong at crease
	// boundaries (the same orig vertex's clones now have different N
	// but the same Tangent → post-Gram-Schmidt tangents diverge sharply
	// across the seam, producing stable patchy noise on bump-mapped
	// surfaces). Cheap: one pass over T->Faces + T->Verts.
	Compute_Vertex_Tangents(T);

	// T->SL (static-lighting cache) was sized for the original VIndex.
	// StaticLighting iterates [0, T->VIndex) writing T->SL[vi] — without
	// growing SL too, those writes overflow into neighbouring TriMesh
	// structs. Reallocate to match. Old SL is leaked along with old Verts.
	if (T->Flags & Tri_Stationary) {
		T->SL = (Color *)getAlignedBlock(sizeof(Color) * newCount, 16);
	}
}

// LIVE per-surface re-smooth: recompute the per-vertex normals of `surface` on
// the CURRENT rendered meshes for a new angle, without touching geometry — so a
// browser slider drag re-shades next frame with no reload. The rasterizer reads
// Vertex::N indirectly: Transform_Objects recomputes the view-space Vertex::TN
// from N every frame and the deferred G-buffer normal is the interpolated TN,
// so overwriting N is all that's needed.
//
// Adjacency = a GLOBAL spatial hash of the surface's face corners across every
// current mesh, keyed by EXACT position bits. This is deliberately NOT the
// per-mesh original-vertex grouping used at init, because greets copies the
// smoothed meshes into per-cell CHUNKS after init (GREETS.CPP), and it is those
// chunk copies — not the source mesh init smoothed — that actually render. A
// vertex's incident faces therefore span multiple chunk meshes, so the
// adjacency has to be rebuilt scene-wide on the live geometry. Exact position
// bits (not a tolerance grid) are safe because the chunk copy is bitwise, so
// vertices that were one shared point still compare equal.
//
// This averages MakeFacesIndependent's per-surface rule (same base-surface
// only, angle gate Dot(F.N,adj.N) >= cos(angle), area-weighted, EPSILON
// fallback to the face normal) so the LIVE look tracks a reload closely. Two
// residual departures from a bit-identical reload are inherent to rebuilding
// adjacency from geometry instead of init's Vertex* map, and are documented in
// MeshOps.h: (1) at coincident-but-distinct authored vertices (e.g. the momy
// lathe's UV seams) init keeps the vertices apart while a position hash merges
// them, slightly over-smoothing there; (2) the accumulation order differs, so
// area-weighted sums round to within ~1 LSB. Both are sub-visual.
void MeshOps_ResmoothSurface(const char *surface, float angleDeg) {
	if (!surface || !*surface || !CurScene) return;
	if (angleDeg < 0.0f)   angleDeg = 0.0f;
	if (angleDeg > 180.0f) angleDeg = 180.0f;
	const std::string base = rev::Editor_BaseSurfName(surface);
	const float cosThr = std::cos(angleDeg * float(PI) / 180.0f);
	auto surfBaseEq = [&](const Face *f) {
		return f && f->Txtr && f->Txtr->Name
		    && rev::Editor_BaseSurfName(f->Txtr->Name) == base;
	};

	// Collect every corner of every target-surface face across all meshes.
	struct Corner { TriMesh *T; Vertex *V; const Face *F; };
	std::vector<Corner> corners;
	std::vector<TriMesh *> touched;   // meshes needing a tangent rebuild
	for (TriMesh *T = CurScene->TriMeshHead; T; T = T->Next) {
		if (T->FIndex == 0 || !T->Faces || !T->Verts) continue;
		bool any = false;
		for (int32_t i = 0; i < T->FIndex; ++i) {
			Face *F = &T->Faces[i];
			if (!surfBaseEq(F) || !F->A || !F->B || !F->C) continue;
			corners.push_back({ T, F->A, F });
			corners.push_back({ T, F->B, F });
			corners.push_back({ T, F->C, F });
			any = true;
		}
		if (any) touched.push_back(T);
	}
	if (corners.empty()) return;

	// Spatial bucket by exact position bits: verts that were one shared point
	// have bit-identical Pos, so they land together (= incident to one vertex).
	struct PosKey {
		uint32_t x, y, z;
		bool operator==(const PosKey &o) const { return x == o.x && y == o.y && z == o.z; }
	};
	struct PosHash {
		size_t operator()(const PosKey &k) const {
			return (size_t(k.x) * 73856093u) ^ (size_t(k.y) * 19349663u) ^ (size_t(k.z) * 83492791u);
		}
	};
	auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
	auto keyOf = [&](const Vector &p) { return PosKey{ bits(p.x), bits(p.y), bits(p.z) }; };
	std::unordered_map<PosKey, std::vector<int>, PosHash> bucket;
	bucket.reserve(corners.size() * 2);
	for (int i = 0; i < int(corners.size()); ++i)
		bucket[keyOf(corners[i].V->Pos)].push_back(i);

	// Recompute each corner's normal from its position-bucket's incident faces.
	// Face::N (not Vertex::N) is what gets averaged, so no read-after-write
	// hazard; still, write into a scratch and commit at the end for clarity.
	std::vector<Vector> out(corners.size());
	for (int i = 0; i < int(corners.size()); ++i) {
		const Corner &c = corners[i];
		const Vector &fN = c.F->N;
		Vector accum; Vector_Form(&accum, 0, 0, 0);
		for (int j : bucket[keyOf(c.V->Pos)]) {
			const Face *adj = corners[j].F;
			if (Dot_Product(&fN, &adj->N) < cosThr) continue;
			Vector w;
			Vector_Scale(&adj->N,
			             Tri_Surface(&adj->A->Pos, &adj->B->Pos, &adj->C->Pos), &w);
			Vector_SelfAdd(&accum, &w);
		}
		if (Vector_Length(&accum) < EPSILON) { out[i] = fN; continue; }
		Vector_Norm(&accum);
		out[i] = accum;
	}
	// Commit. On a per-face-split mesh (all greets geometry, and any mesh
	// MakeFacesIndependent touched) each Vertex is referenced by exactly one
	// corner, so this writes out[i] verbatim. On a SHARED-vertex mesh (a
	// crease-free surface never split at init) several corners target the same
	// Vertex; average them instead of letting the last write win, so the vertex
	// degrades gracefully to a single smoothed normal (it can't be faceted
	// without a re-split — the documented live/reload gap) rather than garbage.
	std::unordered_map<Vertex *, std::pair<Vector, int>> acc;
	for (int i = 0; i < int(corners.size()); ++i) {
		auto &e = acc[corners[i].V];
		if (e.second == 0) { e.first = out[i]; e.second = 1; }
		else { Vector_SelfAdd(&e.first, &out[i]); ++e.second; }
	}
	for (auto &kv : acc) {
		Vector n = kv.second.first;
		if (kv.second.second > 1 && Vector_Length(&n) >= EPSILON) Vector_Norm(&n);
		kv.first->N = n;
	}

	// New normals => new tangent basis (per-face-split verts, so each mesh's
	// tangents recompute purely from its own faces + the updated normals).
	for (TriMesh *T : touched) Compute_Vertex_Tangents(T);
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

// Pack a 32-bit (BGRA, tiled+mipmapped) grayscale texture down to an 8-bit
// SINGLE-CHANNEL copy with the IDENTICAL block-tile + mip layout — so the same
// swizzled texel index the rasterizer computes for the 32-bit albedo also
// indexes this (just 1 byte/texel instead of 4 → ¼ the memory + cache). The
// mip chain is one contiguous block (Generate_Mipmaps; Mipmap[i] point into
// Data), so a flat low-byte copy preserves the layout exactly, and each mip's
// byte offset == its texel offset in the source. This is the variable-texel-
// size pilot (parallax height is the low-blast-radius first consumer: a new
// texture, point-sampled, one reader). Returns a new Texture (BPP=8); caller owns.
Texture *MakeHeight8(Texture *src) {
	if (!src || src->BPP != 32 || !src->Mipmap[0] || src->numMipmaps == 0) return nullptr;
	const int blockX = src->blockSizeX, blockY = src->blockSizeY;
	const int BX = 1 << blockX, BY = 1 << blockY;
	// Total texels across all mip levels (matches Generate_Mipmaps' layout).
	size_t total = 0;
	int cx = src->SizeX >> blockX, cy = src->SizeY >> blockY;
	for (dword i = 0; i < src->numMipmaps; ++i) {
		total += size_t(cx) * size_t(cy) * size_t(BX) * size_t(BY);
		cx = (cx + 1) >> 1; cy = (cy + 1) >> 1;
	}
	Texture *h = new Texture;
	*h = *src;                       // copy dims/LSize/blockSize/numMipmaps/Optclass
	h->Pal = nullptr; h->FileName = nullptr; h->ID = 0; h->Flags = src->Flags;
	h->BPP = 8;
	for (int i = 0; i < 16; ++i) h->Mipmap[i] = nullptr;
	byte *dst = (byte *)getAlignedBlock(total);
	h->Data = dst;
	const uint32_t *s0 = reinterpret_cast<const uint32_t *>(src->Mipmap[0]);
	for (size_t t = 0; t < total; ++t) dst[t] = byte(s0[t] & 0xFFu);   // low byte = gray
	// Per-mip byte offset in the u8 buffer == the source's per-mip texel offset.
	for (dword i = 0; i < src->numMipmaps; ++i) {
		const size_t texelOff = size_t(reinterpret_cast<const uint32_t *>(src->Mipmap[i]) - s0);
		h->Mipmap[i] = dst + texelOff;
	}
	return h;
}

// Tier-2 cone-step POM: bake a conservative cone-step map from an 8-bit height
// texture. Same tiled+mip layout as the source (so ONE swizzled address indexes
// both). Per mip: max-pool the height to a coarse grid (≤ kConeCoarseMax per
// axis), compute the conservative cone ratio per coarse cell = min over the
// field of dist_uv/heightDiff to any TALLER cell (toroidal wrap → seamless for
// tiling), quantize over [0,kPomConeMax], then nearest-upsample back to the mip
// resolution. Max-pooling makes the coarse cone conservative w.r.t. the fine
// height the march hit-tests against (coarse ≥ fine → gaps under-estimated →
// steps under-shoot → the ray never skips geometry). Tiny mips (< one block)
// are left at 255 (flat) — only distant faces use them and LOD fades POM there.
Texture *MakeConeMap(Texture *height) {
	if (!height || height->BPP != 8 || !height->Mipmap[0] || height->numMipmaps == 0)
		return nullptr;
	const int blockX = height->blockSizeX, blockY = height->blockSizeY;
	const int BX = 1 << blockX, BY = 1 << blockY;
	// Total texels across the mip chain (same walk as MakeHeight8).
	size_t total = 0;
	{ int cx = height->SizeX >> blockX, cy = height->SizeY >> blockY;
	  for (dword i = 0; i < height->numMipmaps; ++i) {
	      total += size_t(cx) * size_t(cy) * size_t(BX) * size_t(BY);
	      cx = (cx + 1) >> 1; cy = (cy + 1) >> 1; } }
	Texture *cone = new Texture;
	*cone = *height;                 // dims/LSize/blockSize/numMipmaps/OptClass
	cone->Pal = nullptr; cone->FileName = nullptr; cone->ID = 0; cone->Flags = height->Flags;
	for (int i = 0; i < 16; ++i) cone->Mipmap[i] = nullptr;
	byte *dst = (byte *)getAlignedBlock(total);
	cone->Data = dst;
	std::memset(dst, 255, total);    // default = flat (max cone = biggest steps)
	const byte *h0 = height->Mipmap[0];
	for (dword i = 0; i < height->numMipmaps; ++i)
		cone->Mipmap[i] = dst + size_t(height->Mipmap[i] - h0);

	constexpr int kConeCoarseMax = 128;   // cap coarse grid per axis (bake speed)
	const float kQuant = 255.0f / kPomConeMax;
	for (dword mip = 0; mip < height->numMipmaps; ++mip) {
		const int mw = std::max(1, height->SizeX >> mip);
		const int mh = std::max(1, height->SizeY >> mip);
		if (mw < BX || mh < BY) continue;         // tiny mip → leave flat (255)
		const int cw = std::min(mw, kConeCoarseMax);
		const int ch = std::min(mh, kConeCoarseMax);
		const int sx = mw / cw, sy = mh / ch;     // integer pool factor (mw%cw==0 for pow2)
		const byte *hmip = height->Mipmap[mip];
		// Max-pool the height mip into a coarse [0,1] field.
		std::vector<float> Hc(size_t(cw) * ch, 0.0f);
		for (int cy = 0; cy < ch; ++cy)
			for (int cx = 0; cx < cw; ++cx) {
				int m = 0;
				for (int yy = cy * sy; yy < (cy + 1) * sy; ++yy)
					for (int xx = cx * sx; xx < (cx + 1) * sx; ++xx) {
						int v = hmip[SwizzledOffset(xx, yy, blockX, blockY, mh)];
						if (v > m) m = v;
					}
				Hc[size_t(cy) * cw + cx] = float(m) * (1.0f / 255.0f);
			}
		// Conservative cone per coarse cell, threaded over rows.
		std::vector<byte> Cc(size_t(cw) * ch, 255);
		const float invCw = 1.0f / float(cw), invCh = 1.0f / float(ch);
		const float minRatioSqClamp = kPomConeMax * kPomConeMax;
		std::counting_semaphore<INT_MAX> done{0};
		auto rowFn = [&](int cy) {
			for (int cx = 0; cx < cw; ++cx) {
				const float hp = Hc[size_t(cy) * cw + cx];
				float minRatioSq = minRatioSqClamp;
				for (int qy = 0; qy < ch; ++qy) {
					int ady = std::abs(qy - cy); if (ady > ch - ady) ady = ch - ady;  // toroidal
					const float dv = float(ady) * invCh;
					const float dv2 = dv * dv;
					const float *Hrow = &Hc[size_t(qy) * cw];
					for (int qx = 0; qx < cw; ++qx) {
						const float dh = Hrow[qx] - hp;
						if (dh <= 0.0f) continue;
						int adx = std::abs(qx - cx); if (adx > cw - adx) adx = cw - adx;
						const float du = float(adx) * invCw;
						const float ratioSq = (du * du + dv2) / (dh * dh);
						if (ratioSq < minRatioSq) minRatioSq = ratioSq;
					}
				}
				float c = std::sqrt(minRatioSq);            // ≤ kPomConeMax
				int q = int(c * kQuant + 0.5f);
				Cc[size_t(cy) * cw + cx] = byte(q > 255 ? 255 : q);
			}
		};
		dispatchIndexed(ch, &done, rowFn);
		for (int k = 0; k < ch; ++k) done.acquire();
		// DEBUG (FDS_CONE_HIST): coarse cone-ratio byte histogram per mip.
		if (std::getenv("FDS_CONE_HIST") && mip < 4) {
			int hist[8] = {0}; double sum = 0;
			for (byte b : Cc) { hist[std::min(7, b / 32)]++; sum += b; }
			std::fprintf(stderr, "[CONE_HIST] mip=%u cells=%zu meanByte=%.1f  buckets[0-31,..,224-255]:",
				mip, Cc.size(), sum / Cc.size());
			for (int b = 0; b < 8; ++b) std::fprintf(stderr, " %d", hist[b]);
			std::fprintf(stderr, "\n");
		}
		// Nearest-upsample the coarse cone into the full mip (swizzled write).
		byte *cmip = cone->Mipmap[mip];
		for (int y = 0; y < mh; ++y) {
			const int cyi = std::min(ch - 1, y * ch / mh);
			for (int x = 0; x < mw; ++x) {
				const int cxi = std::min(cw - 1, x * cw / mw);
				cmip[SwizzledOffset(x, y, blockX, blockY, mh)] = Cc[size_t(cyi) * cw + cxi];
			}
		}
	}
	return cone;
}

// Pack a 32-bit (BGRA) tangent-space normal map down to 16-bit RG (X,Y only;
// Z is reconstructed in the shader as sqrt(1-x²-y²)), same tiled+mip layout as
// the 32-bit source — so the same swizzled texel index the kernel computes
// indexes it (just 2 bytes/texel instead of 4 → half the memory + cache). R
// (>>16) and G (>>8) of each source texel are packed as (R | G<<8); B (=Z) and
// A are dropped. BPP=16 marks the format for the kernel's decode branch.
// Generalizes the variable-texel-size pilot (height was 8-bit) to normals.
Texture *MakeNormal16(Texture *src) {
	if (!src || src->BPP != 32 || !src->Mipmap[0] || src->numMipmaps == 0) return nullptr;
	const int blockX = src->blockSizeX, blockY = src->blockSizeY;
	const int BX = 1 << blockX, BY = 1 << blockY;
	size_t total = 0;
	int cx = src->SizeX >> blockX, cy = src->SizeY >> blockY;
	for (dword i = 0; i < src->numMipmaps; ++i) {
		total += size_t(cx) * size_t(cy) * size_t(BX) * size_t(BY);
		cx = (cx + 1) >> 1; cy = (cy + 1) >> 1;
	}
	Texture *n = new Texture;
	*n = *src;
	n->Pal = nullptr; n->FileName = nullptr; n->ID = 0; n->Flags = src->Flags;
	n->BPP = 16;
	for (int i = 0; i < 16; ++i) n->Mipmap[i] = nullptr;
	uint16_t *dst = (uint16_t *)getAlignedBlock(total * sizeof(uint16_t));
	n->Data = reinterpret_cast<byte *>(dst);
	const uint32_t *s0 = reinterpret_cast<const uint32_t *>(src->Mipmap[0]);
	for (size_t t = 0; t < total; ++t) {
		const uint32_t px = s0[t];
		dst[t] = uint16_t(((px >> 16) & 0xFFu) | (((px >> 8) & 0xFFu) << 8));   // R | G<<8
	}
	for (dword i = 0; i < src->numMipmaps; ++i) {
		const size_t texelOff = size_t(reinterpret_cast<const uint32_t *>(src->Mipmap[i]) - s0);
		n->Mipmap[i] = reinterpret_cast<byte *>(dst + texelOff);
	}
	return n;
}

// Total texel count across the mip chain — the same walk MakeNormal16 does
// (mips live contiguously after the base in one allocation).
static size_t mipChainTexels(const Texture *t) {
	size_t total = 0;
	int cx = t->SizeX >> t->blockSizeX, cy = t->SizeY >> t->blockSizeY;
	const size_t block = size_t(1 << t->blockSizeX) * size_t(1 << t->blockSizeY);
	for (dword i = 0; i < t->numMipmaps; ++i) {
		total += size_t(cx) * size_t(cy) * block;
		cx = (cx + 1) >> 1; cy = (cy + 1) >> 1;
	}
	return total;
}

void FlipNormalMapG(Texture *t) {
	if (!t || !t->Mipmap[0] || t->numMipmaps == 0) return;
	const size_t total = mipChainTexels(t);
	if (t->BPP == 16) {
		uint16_t *px = reinterpret_cast<uint16_t *>(t->Mipmap[0]);
		for (size_t i = 0; i < total; ++i)
			px[i] = uint16_t((px[i] & 0x00FF) | ((0xFF00 - (px[i] & 0xFF00)) & 0xFF00));
	} else if (t->BPP == 32) {
		uint32_t *px = reinterpret_cast<uint32_t *>(t->Mipmap[0]);
		for (size_t i = 0; i < total; ++i)
			px[i] = (px[i] & 0xFFFF00FFu) | ((255u - ((px[i] >> 8) & 0xFFu)) << 8);
	}
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

	// Precompute luminance into a linear (row-major) buffer. Lets us
	// optionally box-blur before Sobel without re-walking the swizzled
	// layout, and removes the per-pixel `% W` cost from Sobel's 8
	// neighbour lookups.
	std::vector<float> lum(size_t(W) * size_t(H));
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			const uint32_t px = src[SwizzledOffset(x, y, blockX, blockY, H)];
			const float b = float(px & 0xFF);
			const float g = float((px >> 8) & 0xFF);
			const float r = float((px >> 16) & 0xFF);
			lum[size_t(y) * size_t(W) + size_t(x)] = 0.299f * r + 0.587f * g + 0.114f * b;
		}
	}

	// FDS_NMAP_BLUR: N passes of 3x3 box blur on luminance before
	// Sobel. Suppresses high-frequency texture content (per-stone
	// grain in brick textures) so the Sobel response concentrates on
	// the big features (mortar lines, tile seams). Two ping-pong
	// buffers; wrap-around at edges to match Sobel's wrap.
	const int blurPasses = std::max(0, fds::FeatureFlags::nmap_blur());
	if (blurPasses > 0) {
		std::vector<float> tmp(lum.size());
		auto idx = [&](int x, int y) {
			x = ((x % W) + W) % W;
			y = ((y % H) + H) % H;
			return size_t(y) * size_t(W) + size_t(x);
		};
		for (int pass = 0; pass < blurPasses; ++pass) {
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					float s = 0.0f;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
							s += lum[idx(x + dx, y + dy)];
					tmp[size_t(y) * size_t(W) + size_t(x)] = s * (1.0f / 9.0f);
				}
			}
			lum.swap(tmp);
		}
	}

	auto fetchLum = [&](int x, int y) -> float {
		x = ((x % W) + W) % W;
		y = ((y % H) + H) % H;
		return lum[size_t(y) * size_t(W) + size_t(x)];
	};

	// FDS_NMAP_STRENGTH overrides the caller's strength arg so we can
	// sweep tuning values from the CLI without rebuilding callers.
	const float effStrength = fds::FeatureFlags::nmap_strength();
	const float invStrength = effStrength * (1.0f / 255.0f);
	(void)strength;
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

			// Tangent-space normal map. Perturbation lives in the
			// surface tangent plane; +Z (the surface normal) carries
			// the unperturbed component. Lighting kernel multiplies
			// by per-pixel TBN to bring it into view space — works
			// on any face orientation (Tier B), unlike the previous
			// world-space encoding which only worked for +Y surfaces.
			float nx = -gx * invStrength;
			float ny = -gy * invStrength;
			float nz =  1.0f;
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

Texture *Scene_MakeTiledTexture(int width, int height, const uint32_t *pixels,
                                bool buildMips) {
	// Step 1: resample to 256^2 + BPP-convert. Leaves data LINEAR.
	Image img; img.x = width; img.y = height; img.FileName = nullptr;
	img.Data = new DWord[size_t(width) * size_t(height)];
	std::memcpy(img.Data, pixels, size_t(width) * size_t(height) * sizeof(DWord));
	Texture *t = new Texture;
	std::memset(t, 0, sizeof(Texture));
	t->BPP = 32;
	Convert_Image2Texture(&img, t);
	delete[] img.Data;
	// Step 2: block-tile ("shachletz") so the rasterizer's swizzled UV
	// addressing (packed_tile_u/v) hits the right texels. Without this the
	// texture samples scrambled / repeated. See MeshOps.h for the full story.
	t->Flags |= Txtr_Tiled;
	Generate_Mipmaps(t, DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, buildMips ? 1 : 0);
	return t;
}

// True iff any face of the mesh belongs to a surface with a smoothing-angle
// override. Used to force the face-clone pass on an otherwise-smooth mesh so
// a low override angle can FACET it (a smooth mesh has no creases at the
// global threshold and would otherwise be skipped). Fast no-op when the
// override registry is empty.
static bool MeshHasOverrideSurface(TriMesh *T) {
	if (!MeshOps_AnySurfaceSmoothAngle() || !T || !T->Faces) return false;
	float a;
	for (int32_t i = 0; i < T->FIndex; ++i) {
		const Face *F = &T->Faces[i];
		if (F->Txtr && F->Txtr->Name && MeshOps_GetSurfaceSmoothAngle(F->Txtr->Name, a))
			return true;
	}
	return false;
}

void MakeFacesIndependentByAngle(Scene *Sc, float thresholdDegrees) {
	if (!Sc) return;
	// Authored-angle mode re-derives every surface's smoothing from its LWO
	// angle, so no mesh may be skipped. Off by default => the term drops out.
	const bool useAuthored = fds::FeatureFlags::surf_smoothing_authored();
	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
		if (T->FIndex == 0 || !T->Faces) continue;
		// Process a mesh if it has hard creases at the global threshold, OR one
		// of its surfaces carries an explicit smoothing-angle override (which
		// may need to facet an otherwise-smooth mesh), OR authored-angle mode
		// is on. With no override and the flag off, only the first term
		// survives => byte-identical to before.
		if (!MeshHasCreases(T, thresholdDegrees) && !MeshHasOverrideSurface(T)
		    && !useAuthored) continue;
		// Same threshold gates both: a face pair within `thresholdDegrees`
		// is "smooth" (averaged normal); beyond is a "crease" (face
		// normal alone). Per-surface overrides are applied inside
		// MakeFacesIndependent per face regardless of this threshold.
		MakeFacesIndependent(T, thresholdDegrees);
	}
}

// Phong-tessellate (curved PN-style) every face whose material is `matName`,
// `levels` times. Each target triangle splits 1→4 at its edge midpoints; the
// midpoint is displaced toward the surface implied by the two endpoints' smooth
// vertex normals (project the linear midpoint onto each endpoint's tangent plane,
// average) so the silhouette rounds out instead of staying a flat polygon. Edge
// midpoints are shared between adjacent target faces (keyed by sorted vertex
// index) so the mesh stays crack-free; the projection is crease-guarded (faded
// out where endpoint normals disagree / are degenerate — see edgeMid) so folds
// and lathe poles don't invert sub-triangles. Non-target faces are copied
// verbatim.
//
// Must run AFTER per-vertex normals exist (Preprocess) and BEFORE
// MakeFacesIndependentByAngle (which then re-smooths the finer mesh). Rebuilds
// T->Verts/T->Faces and restamps A_idx exactly like MakeFacesIndependent, so the
// downstream count change is handled the same proven way. Old arrays are leaked
// (init-time, matches MakeFacesIndependent).
void SubdivideMaterialFaces(Scene *Sc, const char *matName, int levels, bool phong) {
	if (!Sc || !matName || levels <= 0) return;
	auto isTarget = [&](const Face *F) {
		return F && F->Txtr && F->Txtr->Name && !std::strcmp(F->Txtr->Name, matName);
	};
	for (int lvl = 0; lvl < levels; ++lvl) {
		for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
			if (T->FIndex == 0 || !T->Faces || !T->Verts) continue;
			int nTarget = 0;
			for (int32_t i = 0; i < T->FIndex; ++i)
				if (isTarget(&T->Faces[i])) ++nTarget;
			if (nTarget == 0) continue;

			Vertex *const oldV = T->Verts;
			std::vector<Vertex> verts(oldV, oldV + T->VIndex);   // copy originals
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> midOf;
			auto vidx = [&](const Vertex *v) { return uint32_t(v - oldV); };

			// FDS_SUBDIV_DIAG: topology + normal-quality report for the target
			// surface. Settled a long-standing misdiagnosis (2026-07-30): the momy
			// "seam cracks" were NOT unshared coincident verts — the lathe is
			// index-closed (boundary-edges raw=0: every edge, keyed by raw vertex
			// index, is shared by exactly 2 faces, seam included), and welding by
			// quantized position actually CREATED non-manifold edges. The real
			// artifact was the unguarded Phong projection (see edgeMid below):
			// |N|=0 at a pole + edges whose endpoint normals disagree >45..90°
			// (4 neg-dot + 165 low-dot edges on momy) displaced midpoints until
			// sub-triangles inverted and got backface-culled → holes. Kept as a
			// measurement tool for future targets ('rooms'/'floor' displacement):
			// boundary-edges raw>0 with canon==0 would mean position-coincident
			// seam duplicates DO exist there and need seam-aware handling.
			if (std::getenv("FDS_SUBDIV_DIAG")) {
				std::vector<uint32_t> canon(size_t(T->VIndex));
				for (int32_t i = 0; i < T->VIndex; ++i) canon[i] = uint32_t(i);
				constexpr float kWeldGrid = 1.0f / 1e-4f;
				auto qkey = [&](const Vector &p) {
					return std::array<int64_t, 3>{
						int64_t(std::llround(p.x * kWeldGrid)),
						int64_t(std::llround(p.y * kWeldGrid)),
						int64_t(std::llround(p.z * kWeldGrid)) };
				};
				std::map<std::array<int64_t, 3>, uint32_t> firstOf;
				for (int32_t i = 0; i < T->FIndex; ++i) {
					const Face &F = T->Faces[i];
					if (!F.A || !F.B || !F.C || !isTarget(&F)) continue;
					const Vertex *tri[3] = { F.A, F.B, F.C };
					for (const Vertex *vp : tri) {
						const uint32_t vi = vidx(vp);
						if (vi >= uint32_t(T->VIndex)) continue;   // defensive
						const auto k = qkey(oldV[vi].Pos);
						auto it = firstOf.find(k);
						if (it == firstOf.end()) firstOf.emplace(k, vi);
						else canon[vi] = it->second;
					}
				}
				auto countBoundary = [&](bool useCanon) {
					std::map<std::pair<uint32_t,uint32_t>, int> ec;
					for (int32_t i = 0; i < T->FIndex; ++i) {
						const Face &F = T->Faces[i];
						if (!F.A || !F.B || !F.C || !isTarget(&F)) continue;
						uint32_t a = vidx(F.A), b = vidx(F.B), c = vidx(F.C);
						if (useCanon) { a = canon[a]; b = canon[b]; c = canon[c]; }
						auto add = [&](uint32_t x, uint32_t y){
							ec[{std::min(x,y),std::max(x,y)}]++; };
						add(a,b); add(b,c); add(c,a);
					}
					int bnd = 0, nonManifold = 0;
					for (auto &kv : ec) { if (kv.second == 1) ++bnd; else if (kv.second > 2) ++nonManifold; }
					return std::make_pair(bnd, nonManifold);
				};
				int nCoin = 0;
				for (int32_t i = 0; i < T->VIndex; ++i) if (canon[i] != uint32_t(i)) ++nCoin;
				auto raw = countBoundary(false);
				auto can = countBoundary(true);
				int dotNeg = 0, dotLow = 0, dotOk = 0, nZero = 0;
				std::map<std::pair<uint32_t,uint32_t>, char> seen;
				for (int32_t i = 0; i < T->FIndex; ++i) {
					const Face &F = T->Faces[i];
					if (!F.A || !F.B || !F.C || !isTarget(&F)) continue;
					const uint32_t v[3] = { vidx(F.A), vidx(F.B), vidx(F.C) };
					for (int e = 0; e < 3; ++e) {
						uint32_t x = v[e], y = v[(e+1)%3];
						auto k = std::make_pair(std::min(x,y), std::max(x,y));
						if (seen.count(k)) continue;
						seen[k] = 1;
						const Vector &NA = oldV[x].N, &NB = oldV[y].N;
						const float la = std::sqrt(NA.x*NA.x+NA.y*NA.y+NA.z*NA.z);
						const float lb = std::sqrt(NB.x*NB.x+NB.y*NB.y+NB.z*NB.z);
						if (la < 1e-6f || lb < 1e-6f) { ++nZero; continue; }
						const float d = (NA.x*NB.x+NA.y*NB.y+NA.z*NB.z)/(la*lb);
						if (d < 0.0f) ++dotNeg; else if (d < 0.7f) ++dotLow; else ++dotOk;
					}
				}
				std::fprintf(stderr,
					"[SUBDIV-DIAG] '%s' lvl%d verts=%d coincident=%d boundary-edges raw=%d "
					"pos-welded=%d (nonmanifold raw=%d welded=%d) edge-normal-dot: "
					"zeroN=%d neg=%d low(<0.7)=%d ok=%d\n",
					matName, lvl, T->VIndex, nCoin, raw.first, can.first,
					raw.second, can.second, nZero, dotNeg, dotLow, dotOk);
			}

			// Linear position of each created midpoint, for the fold-relaxation
			// pass below (id-indexed alongside `verts`).
			std::map<uint32_t, Vector> midLinear;

			auto edgeMid = [&](uint32_t ia, uint32_t ib) -> uint32_t {
				const auto key = std::make_pair(std::min(ia, ib), std::max(ia, ib));
				auto it = midOf.find(key);
				if (it != midOf.end()) return it->second;
				const Vertex &A = oldV[ia], &B = oldV[ib];
				Vertex m = A;                       // inherit Flags etc.
				const float lx = (A.Pos.x + B.Pos.x) * 0.5f;
				const float ly = (A.Pos.y + B.Pos.y) * 0.5f;
				const float lz = (A.Pos.z + B.Pos.z) * 0.5f;
				// Phong projection, CREASE-GUARDED (fixed 2026-07-30 — this was the
				// "momy crack" root cause, previously misattributed to unshared
				// coincident seam verts; the mesh is index-closed, see the DIAG note
				// above). The raw formula assumes unit endpoint normals that roughly
				// agree; at the lathe's cap rim / profile folds the endpoint normals
				// disagree by 45–>90° (and one pole vert has |N|=0), so projecting
				// onto both tangent planes displaced the midpoint enough to invert
				// sub-triangles — which the rasterizer backface-culls → holes with
				// the background showing through. Guard: normalize the endpoint
				// normals (skip if degenerate), and fade the projection out by
				// their agreement w = clamp(dot(nA,nB),0,1)² — full PN rounding on
				// smooth edges (dot→1), pure linear midpoint across creases/folds
				// (dot≤0) where "the surface implied by the normals" is undefined
				// anyway. Verified: momy close-cam cap renders closed at SUBDIV=2,
				// silhouette rounding preserved on the smooth barrel.
				float dispx = 0.0f, dispy = 0.0f, dispz = 0.0f;
				const float laN = std::sqrt(A.N.x*A.N.x + A.N.y*A.N.y + A.N.z*A.N.z);
				const float lbN = std::sqrt(B.N.x*B.N.x + B.N.y*B.N.y + B.N.z*B.N.z);
				if (phong && laN > 1e-6f && lbN > 1e-6f) {
					const float ax = A.N.x/laN, ay = A.N.y/laN, az = A.N.z/laN;
					const float bx = B.N.x/lbN, by = B.N.y/lbN, bz = B.N.z/lbN;
					float w = ax*bx + ay*by + az*bz;
					w = (w < 0.0f) ? 0.0f : w;
					w *= w;
					// Project the linear midpoint onto each endpoint's tangent plane.
					const float da = (lx - A.Pos.x)*ax + (ly - A.Pos.y)*ay + (lz - A.Pos.z)*az;
					const float db = (lx - B.Pos.x)*bx + (ly - B.Pos.y)*by + (lz - B.Pos.z)*bz;
					dispx = -0.5f * w * (da*ax + db*bx);
					dispy = -0.5f * w * (da*ay + db*by);
					dispz = -0.5f * w * (da*az + db*bz);
				}
				m.Pos.x = lx + dispx;
				m.Pos.y = ly + dispy;
				m.Pos.z = lz + dispz;
				float nx = A.N.x + B.N.x, ny = A.N.y + B.N.y, nz = A.N.z + B.N.z;
				const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (nl > 1e-6f) { m.N.x = nx / nl; m.N.y = ny / nl; m.N.z = nz / nl; }
				m.U = (A.U + B.U) * 0.5f;   m.V = (A.V + B.V) * 0.5f;
				m.EU = (A.EU + B.EU) * 0.5f; m.EV = (A.EV + B.EV) * 0.5f;
				const uint32_t id = uint32_t(verts.size());
				verts.push_back(m);
				midOf[key] = id;
				midLinear[id] = Vector{lx, ly, lz};
				return id;
			};

			std::vector<Face> faces;
			std::vector<std::array<uint32_t, 3>> fIdx;
			faces.reserve(size_t(T->FIndex) + size_t(nTarget) * 3);
			fIdx.reserve(faces.capacity());
			auto emit = [&](const Face &proto, uint32_t i0, uint32_t i1, uint32_t i2,
			                float u0, float v0, float u1, float v1, float u2, float v2) {
				Face f = proto;            // inherit Txtr/Flags/ShadowMatID/mirror tags
				f.frame = nullptr;
				f.U1 = u0; f.V1 = v0; f.U2 = u1; f.V2 = v1; f.U3 = u2; f.V3 = v2;
				f.EU1 = u0; f.EV1 = v0; f.EU2 = u1; f.EV2 = v1; f.EU3 = u2; f.EV3 = v2;
				faces.push_back(f);
				fIdx.push_back({i0, i1, i2});
			};

			// Target-face vertex tuples [ia,ib,ic,mab,mbc,mca] for the
			// fold-relaxation pass below.
			std::vector<std::array<uint32_t, 6>> targetTuples;
			targetTuples.reserve(size_t(nTarget));
			for (int32_t i = 0; i < T->FIndex; ++i) {
				Face &F = T->Faces[i];
				if (!F.A || !F.B || !F.C || !isTarget(&F)) {
					faces.push_back(F);
					fIdx.push_back({F.A ? vidx(F.A) : 0u, F.B ? vidx(F.B) : 0u, F.C ? vidx(F.C) : 0u});
					continue;
				}
				const uint32_t ia = vidx(F.A), ib = vidx(F.B), ic = vidx(F.C);
				const uint32_t mab = edgeMid(ia, ib), mbc = edgeMid(ib, ic), mca = edgeMid(ic, ia);
				const float uab = (F.U1 + F.U2) * 0.5f, vab = (F.V1 + F.V2) * 0.5f;
				const float ubc = (F.U2 + F.U3) * 0.5f, vbc = (F.V2 + F.V3) * 0.5f;
				const float uca = (F.U3 + F.U1) * 0.5f, vca = (F.V3 + F.V1) * 0.5f;
				emit(F, ia,  mab, mca, F.U1, F.V1, uab, vab, uca, vca);
				emit(F, mab, ib,  mbc, uab, vab, F.U2, F.V2, ubc, vbc);
				emit(F, mca, mbc, ic,  uca, vca, ubc, vbc, F.U3, F.V3);
				emit(F, mab, mbc, mca, uab, vab, ubc, vbc, uca, vca);
				targetTuples.push_back({ia, ib, ic, mab, mbc, mca});
			}

			// FOLD RELAXATION (Phong mode only — linear midpoints cannot fold).
			// Even crease-guarded, a Phong-displaced midpoint can
			// fold a sub-face over (sliver parents: the sagitta of a long edge
			// exceeds the triangle's width) — the rasterizer's screen-winding cull
			// then renders it from the wrong side → a hole (this + the unguarded
			// projection were the REAL "momy cracks"; the mesh topology was never
			// cracked). Detect: a sub-face whose winding normal dots negative
			// against its parent's (same cross convention both sides, so the
			// engine's stored-N handedness doesn't matter), or collapses to zero
			// area. Fix: revert ALL midpoints of the offending face to the plain
			// linear split — linear subdivision of a valid triangle cannot fold.
			// Midpoints are shared, so reverting one face can newly fold a
			// neighbour: iterate to fixpoint (monotone — displacement only ever
			// gets removed — so it terminates; ≤2 passes in practice).
			if (phong) {
				auto windN = [&](uint32_t i0, uint32_t i1, uint32_t i2, Vector &out) {
					const Vector &A = verts[i0].Pos, &B = verts[i1].Pos, &C = verts[i2].Pos;
					const float e1x = B.x-A.x, e1y = B.y-A.y, e1z = B.z-A.z;
					const float e2x = C.x-A.x, e2y = C.y-A.y, e2z = C.z-A.z;
					out.x = e1y*e2z - e1z*e2y; out.y = e1z*e2x - e1x*e2z; out.z = e1x*e2y - e1y*e2x;
					return std::sqrt(out.x*out.x + out.y*out.y + out.z*out.z);
				};
				int relaxedMids = 0, foldedFaces = 0, iters = 0;
				for (bool changed = true; changed && iters < 16; ++iters) {
					changed = false;
					for (const auto &t : targetTuples) {
						Vector pn; const float pl = windN(t[0], t[1], t[2], pn);
						if (pl < 1e-12f) continue;   // degenerate parent: leave as-is
						const uint32_t sub[4][3] = {
							{t[0],t[3],t[5]}, {t[3],t[1],t[4]},
							{t[5],t[4],t[2]}, {t[3],t[4],t[5]} };
						bool folded = false;
						for (auto &s : sub) {
							Vector sn; const float sl = windN(s[0], s[1], s[2], sn);
							if (sl < 1e-12f * pl ||
							    pn.x*sn.x + pn.y*sn.y + pn.z*sn.z < 0.0f) { folded = true; break; }
						}
						if (!folded) continue;
						++foldedFaces;
						for (int k = 3; k < 6; ++k) {
							auto lin = midLinear.find(t[k]);
							if (lin == midLinear.end()) continue;
							Vector &P = verts[t[k]].Pos;
							if (P.x != lin->second.x || P.y != lin->second.y || P.z != lin->second.z) {
								P = lin->second;
								++relaxedMids;
								changed = true;
							}
						}
					}
				}
				if (std::getenv("FDS_SUBDIV_DIAG"))
					std::fprintf(stderr,
						"[SUBDIV-DIAG] '%s' lvl%d fold-relax: %d face-visits folded, "
						"%d midpoints reverted to linear, %d passes\n",
						matName, lvl, foldedFaces, relaxedMids, iters);
			}

			Vertex *nv = new Vertex[verts.size()];
			std::memcpy(nv, verts.data(), verts.size() * sizeof(Vertex));
			Face *nf = new Face[faces.size()];
			for (size_t i = 0; i < faces.size(); ++i) {
				nf[i] = faces[i];
				nf[i].A = &nv[fIdx[i][0]];
				nf[i].B = &nv[fIdx[i][1]];
				nf[i].C = &nv[fIdx[i][2]];
				// Recompute the face normal from the new positions, aligned to the
				// proto's orientation (preserve winding/cull sign).
				const Vector &A = nf[i].A->Pos, &B = nf[i].B->Pos, &C = nf[i].C->Pos;
				const float e1x = B.x - A.x, e1y = B.y - A.y, e1z = B.z - A.z;
				const float e2x = C.x - A.x, e2y = C.y - A.y, e2z = C.z - A.z;
				float gx = e1y * e2z - e1z * e2y, gy = e1z * e2x - e1x * e2z, gz = e1x * e2y - e1y * e2x;
				const float gl = std::sqrt(gx * gx + gy * gy + gz * gz);
				if (gl > 1e-6f) {
					gx /= gl; gy /= gl; gz /= gl;
					if (gx * faces[i].N.x + gy * faces[i].N.y + gz * faces[i].N.z < 0.0f) { gx = -gx; gy = -gy; gz = -gz; }
					nf[i].N.x = gx; nf[i].N.y = gy; nf[i].N.z = gz;
				}
				// Re-derive the plane constant for the (possibly new) plane. The
				// backface cull (Transform.cpp: AP·N < NormProd) evaluates (N,
				// NormProd) as a plane equation; sub-faces displaced off the
				// parent plane but carrying the PARENT's NormProd cull wrongly
				// near grazing → pinprick/notch holes on the rounded surface
				// (the last visible piece of the "momy cracks", after the crease
				// guard + fold relaxation). Convention per PREPROC.CPP:113 /
				// GreetsMirror: NormProd = -(N · A.Pos).
				nf[i].NormProd = -(nf[i].N.x * A.x + nf[i].N.y * A.y + nf[i].N.z * A.z);
			}
			T->Verts  = nv; T->VIndex = int32_t(verts.size());
			T->Faces  = nf; T->FIndex = int32_t(faces.size());
			Compute_FaceVertexIndices(T);
			if (T->Flags & Tri_Stationary)
				T->SL = (Color *)getAlignedBlock(sizeof(Color) * T->VIndex, 16);
			std::fprintf(stderr, "[SUBDIV] '%s' lvl%d -> verts %d faces %d (+%d target split)\n",
			             matName, lvl, T->VIndex, T->FIndex, nTarget);
		}
	}
}

// Bilinear sample of an 8-bit block-tiled height map mip at a normalized UV
// (u=1.0 spans the tile; toroidal wrap, matching the rasterizer's masked
// addressing). Returns h in [0,1].
static float SampleHeight8Bilinear(const Texture *hm, int mip, float u, float v) {
	const int mw = std::max(1, int(hm->SizeX) >> mip);
	const int mh = std::max(1, int(hm->SizeY) >> mip);
	const byte *data = hm->Mipmap[mip];
	// Texel-center convention: u*mw - 0.5 puts u=0.5/mw at texel 0's center.
	const float x = u * float(mw) - 0.5f;
	const float y = v * float(mh) - 0.5f;
	const float fxf = std::floor(x), fyf = std::floor(y);
	const float fx = x - fxf, fy = y - fyf;
	// Positive modulo (UVs may be negative or span many tiles).
	auto wrap = [](int a, int m) { int r = a % m; return r < 0 ? r + m : r; };
	const int x0 = wrap(int(fxf), mw), x1 = wrap(int(fxf) + 1, mw);
	const int y0 = wrap(int(fyf), mh), y1 = wrap(int(fyf) + 1, mh);
	const int bx = hm->blockSizeX, by = hm->blockSizeY;
	const float h00 = data[SwizzledOffset(x0, y0, bx, by, mh)];
	const float h10 = data[SwizzledOffset(x1, y0, bx, by, mh)];
	const float h01 = data[SwizzledOffset(x0, y1, bx, by, mh)];
	const float h11 = data[SwizzledOffset(x1, y1, bx, by, mh)];
	const float top = h00 + (h10 - h00) * fx;
	const float bot = h01 + (h11 - h01) * fx;
	return (top + (bot - top) * fy) * (1.0f / 255.0f);
}

// Height-map displacement bake (docs/ENVDYN_DISPLACEMENT_PLAN.md, B3): push
// every INTERIOR vertex of `matName`'s faces along its smooth vertex normal by
// amp*(h-mipMean) (mean of the sampled mip - zero-mean relief, no DC bulge
// against the pinned borders), h bilinear-sampled at `mip`
// at the PER-FACE UV (ENGINE.md rule — per-vertex U/V is clobbered at shared
// corners), averaged over the vertex's incident target faces (different faces
// may map the vertex differently across projection seams; the average keeps
// one deterministic offset for the one shared position).
//
// Crack safety: verts on the target patch BORDER are pinned (zero
// displacement). A border vert is any endpoint of an edge used by exactly ONE
// target face (wall/ceiling + wall-floor junction lines, doorway jambs, open
// borders — this also catches subdivision midpoints created ON a border edge,
// whose T-junction against the unsplit non-target neighbour would otherwise
// open), plus any vert incident to a non-target face (so non-target geometry
// never moves). Relief fades to the authored edge there.
//
// Runs at the stone-subdiv hook: AFTER SubdivideMaterialFaces (density) and
// BEFORE MakeFacesIndependentByAngle (which re-derives per-vertex normals from
// the face normals this bake recomputes) and the Piramid chunk split (chunk
// bsphere bounds then wrap the displaced verts). Face N + NormProd are
// re-derived here for every target face (displaced planes; stale NormProd
// mis-culls — the B1 lesson). Tangents recompute downstream as today
// (Compute_Vertex_Tangents inside MakeFacesIndependent).
void DisplaceMaterialVertices(Scene *Sc, const char *matName, float amp, int mip) {
	if (!Sc || !matName || amp == 0.0f) return;
	auto isTarget = [&](const Face *F) {
		return F && F->Txtr && F->Txtr->Name && !std::strcmp(F->Txtr->Name, matName);
	};
	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
		if (T->FIndex == 0 || !T->Faces || !T->Verts) continue;
		const Texture *hm = nullptr;
		int nTarget = 0;
		for (int32_t i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!isTarget(&F)) continue;
			++nTarget;
			if (!hm && F.Txtr->HeightMap) hm = F.Txtr->HeightMap;
		}
		if (nTarget == 0) continue;
		if (!hm || hm->BPP != 8 || !hm->Mipmap[0]) {
			std::fprintf(stderr, "[DISPLACE] '%s': no 8-bit HeightMap on the material "
			             "(need --greets_stone_tex + --parallax) — skipped\n", matName);
			continue;
		}
		// Clamp mip to levels that are (a) present and (b) still block-tileable
		// (below one block the swizzled layout degenerates — same guard as
		// MakeConeMap's tiny-mip skip).
		int useMip = mip;
		if (useMip >= int(hm->numMipmaps)) useMip = int(hm->numMipmaps) - 1;
		if (useMip < 0) useMip = 0;
		while (useMip > 0 &&
		       ((std::max(1, int(hm->SizeX) >> useMip) < (1 << hm->blockSizeX)) ||
		        (std::max(1, int(hm->SizeY) >> useMip) < (1 << hm->blockSizeY))))
			--useMip;

		// Degenerate-map guard + mip MEAN: a (near-)constant height map carries
		// no relief — displacing by it would just push the whole surface out
		// uniformly against the pinned borders (a bulge, not detail). Skip with
		// a log. Real case: the shipping greets_wall_h.png is ALL-WHITE (a
		// placeholder from the stone3 texture swap; the real map sits in the
		// user's _bak_greets_wall_* dirs) — walls displace only once a real map
		// is installed (or overlaid via --material-import=rooms:DIR).
		// The same scan computes the mip's MEAN: displacement is centered on it
		// — d = amp*(h - mean) — so a map whose average isn't 0.5 (the floor's
		// is ~0.34) yields zero-mean RELIEF instead of a DC sink/bulge fighting
		// the pinned borders. (B4's residual uses the same convention.)
		float mipMean = 0.5f;
		{
			const int mw = std::max(1, int(hm->SizeX) >> useMip);
			const int mh = std::max(1, int(hm->SizeY) >> useMip);
			const byte *d = hm->Mipmap[useMip];
			byte lo = 255, hi = 0;
			uint64_t sum = 0;
			const size_t n = size_t(mw) * size_t(mh);
			for (size_t i = 0; i < n; ++i) {
				const byte b = d[i];
				if (b < lo) lo = b;
				if (b > hi) hi = b;
				sum += b;
			}
			if (hi - lo < 2) {
				std::fprintf(stderr, "[DISPLACE] '%s': height map mip%d is (near-)constant "
				             "(%d..%d) — no relief to bake, skipped\n", matName, useMip, lo, hi);
				continue;
			}
			mipMean = float(double(sum) / double(n)) * (1.0f / 255.0f);
		}

		Vertex *const V = T->Verts;
		auto vidx = [&](const Vertex *v) { return uint32_t(v - V); };

		// Border detection: edge-use counts over target faces + non-target
		// incidence.
		std::vector<char> pinned(size_t(T->VIndex), 0);
		{
			std::map<std::pair<uint32_t, uint32_t>, int> edgeUse;
			for (int32_t i = 0; i < T->FIndex; ++i) {
				const Face &F = T->Faces[i];
				if (!F.A || !F.B || !F.C) continue;
				const uint32_t a = vidx(F.A), b = vidx(F.B), c = vidx(F.C);
				if (a >= uint32_t(T->VIndex) || b >= uint32_t(T->VIndex) ||
				    c >= uint32_t(T->VIndex)) continue;   // defensive
				if (!isTarget(&F)) {
					pinned[a] = pinned[b] = pinned[c] = 1;   // non-target incidence
					continue;
				}
				auto add = [&](uint32_t x, uint32_t y) {
					edgeUse[{std::min(x, y), std::max(x, y)}]++; };
				add(a, b); add(b, c); add(c, a);
			}
			for (const auto &kv : edgeUse)
				if (kv.second == 1) {   // patch-border edge
					pinned[kv.first.first] = 1;
					pinned[kv.first.second] = 1;
				}
		}

		// Per-vertex height accumulation at per-FACE corner UVs.
		std::vector<float> hSum(size_t(T->VIndex), 0.0f);
		std::vector<int>   hCnt(size_t(T->VIndex), 0);
		for (int32_t i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C || !isTarget(&F)) continue;
			const uint32_t vi[3] = { vidx(F.A), vidx(F.B), vidx(F.C) };
			const float cu[3] = { F.U1, F.U2, F.U3 };
			const float cv[3] = { F.V1, F.V2, F.V3 };
			for (int k = 0; k < 3; ++k) {
				if (vi[k] >= uint32_t(T->VIndex) || pinned[vi[k]]) continue;
				hSum[vi[k]] += SampleHeight8Bilinear(hm, useMip, cu[k], cv[k]);
				hCnt[vi[k]] += 1;
			}
		}

		// Displace along the (unit) smooth vertex normal.
		int nMoved = 0;
		float dMin = 1e30f, dMax = -1e30f;
		for (int32_t i = 0; i < T->VIndex; ++i) {
			if (pinned[i] || hCnt[i] == 0) continue;
			const Vector &N = V[i].N;
			const float nl = std::sqrt(N.x*N.x + N.y*N.y + N.z*N.z);
			if (nl < 1e-6f) continue;                    // degenerate: pin
			const float h = hSum[i] / float(hCnt[i]);
			const float d = amp * (h - mipMean);
			V[i].Pos.x += N.x / nl * d;
			V[i].Pos.y += N.y / nl * d;
			V[i].Pos.z += N.z / nl * d;
			if (d < dMin) dMin = d;
			if (d > dMax) dMax = d;
			++nMoved;
		}

		// Re-derive target-face plane data from the displaced positions:
		// geometric N (sign-aligned to the old normal — engine handedness) +
		// NormProd (stale plane constants mis-cull near grazing; B1 lesson).
		for (int32_t i = 0; i < T->FIndex; ++i) {
			Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C || !isTarget(&F)) continue;
			const Vector &A = F.A->Pos, &B = F.B->Pos, &C = F.C->Pos;
			const float e1x = B.x - A.x, e1y = B.y - A.y, e1z = B.z - A.z;
			const float e2x = C.x - A.x, e2y = C.y - A.y, e2z = C.z - A.z;
			float gx = e1y*e2z - e1z*e2y, gy = e1z*e2x - e1x*e2z, gz = e1x*e2y - e1y*e2x;
			const float gl = std::sqrt(gx*gx + gy*gy + gz*gz);
			if (gl > 1e-6f) {
				gx /= gl; gy /= gl; gz /= gl;
				if (gx*F.N.x + gy*F.N.y + gz*F.N.z < 0.0f) { gx = -gx; gy = -gy; gz = -gz; }
				F.N.x = gx; F.N.y = gy; F.N.z = gz;
			}
			F.NormProd = -(F.N.x*A.x + F.N.y*A.y + F.N.z*A.z);
		}
		if (nMoved == 0) { dMin = 0.0f; dMax = 0.0f; }
		std::fprintf(stderr,
			"[DISPLACE] '%s' amp=%.3f mip=%d(%dx%d): %d/%d verts displaced "
			"[%+.3f..%+.3f], %d faces re-planed\n",
			matName, (double)amp, useMip,
			std::max(1, int(hm->SizeX) >> useMip), std::max(1, int(hm->SizeY) >> useMip),
			nMoved, T->VIndex, (double)dMin, (double)dMax, nTarget);
	}
}

// B4 residual height map (docs/ENVDYN_DISPLACEMENT_PLAN.md): the POM input for
// a DISPLACED material. Geometry now carries the low band of the relief
// (amp*(h_lowMip - mean)), so if POM keeps marching the ORIGINAL map the block
// relief is counted twice — once as real silhouette, once as parallax shift.
// The residual removes the geometry's band:
//     res[m](x,y) = clamp(h[m](x,y) − bilinear(h[lowMip], uv) + mean_low)
// evaluated PER MIP at each texel's UV center, so every mip is consistent with
// the same subtraction (mips ≥ lowMip flatten toward mean_low — correct: at
// those scales the geometry carries everything). Same 8-bit tiled+mip layout as
// the source (the rasterizer's swizzled index works unchanged). Mips smaller
// than one tile block are byte-copied verbatim (layout degenerates below the
// block; MakeConeMap skips them for the same reason). Degenerate (constant)
// source at lowMip → nullptr (nothing was displaced — see the bake's guard —
// so the original map must stay installed).
// Caller owns the returned Texture; re-run MakeConeMap on it when cone POM is
// active (the cone geometry changed with the heights).
Texture *MakeResidualHeight(Texture *height, int lowMip) {
	if (!height || height->BPP != 8 || !height->Mipmap[0] || height->numMipmaps == 0)
		return nullptr;
	const int blockX = height->blockSizeX, blockY = height->blockSizeY;
	const int BX = 1 << blockX, BY = 1 << blockY;
	// Clamp lowMip exactly like the displacement bake (same band).
	int useMip = lowMip;
	if (useMip >= int(height->numMipmaps)) useMip = int(height->numMipmaps) - 1;
	if (useMip < 0) useMip = 0;
	while (useMip > 0 &&
	       ((std::max(1, int(height->SizeX) >> useMip) < BX) ||
	        (std::max(1, int(height->SizeY) >> useMip) < BY)))
		--useMip;
	// Degenerate guard + mean of the low mip (identical to the bake's scan).
	float meanLow = 0.5f;
	{
		const int mw = std::max(1, int(height->SizeX) >> useMip);
		const int mh = std::max(1, int(height->SizeY) >> useMip);
		const byte *d = height->Mipmap[useMip];
		byte lo = 255, hi = 0;
		uint64_t sum = 0;
		const size_t n = size_t(mw) * size_t(mh);
		for (size_t i = 0; i < n; ++i) {
			const byte b = d[i];
			if (b < lo) lo = b;
			if (b > hi) hi = b;
			sum += b;
		}
		if (hi - lo < 2) return nullptr;   // bake skipped this map too
		meanLow = float(double(sum) / double(n)) * (1.0f / 255.0f);
	}
	// Allocate the full chain (same walk as MakeHeight8/MakeConeMap).
	size_t total = 0;
	{ int cx = height->SizeX >> blockX, cy = height->SizeY >> blockY;
	  for (dword i = 0; i < height->numMipmaps; ++i) {
	      total += size_t(cx) * size_t(cy) * size_t(BX) * size_t(BY);
	      cx = (cx + 1) >> 1; cy = (cy + 1) >> 1; } }
	Texture *res = new Texture;
	*res = *height;
	res->Pal = nullptr; res->FileName = nullptr; res->ID = 0;
	for (int i = 0; i < 16; ++i) res->Mipmap[i] = nullptr;
	byte *dst = (byte *)getAlignedBlock(total);
	res->Data = dst;
	const byte *h0 = height->Mipmap[0];
	for (dword i = 0; i < height->numMipmaps; ++i)
		res->Mipmap[i] = dst + size_t(height->Mipmap[i] - h0);

	for (dword m = 0; m < height->numMipmaps; ++m) {
		const int mw = std::max(1, int(height->SizeX) >> m);
		const int mh = std::max(1, int(height->SizeY) >> m);
		const byte *src = height->Mipmap[m];
		byte *out = res->Mipmap[m];
		const size_t mipBytes = (m + 1 < height->numMipmaps)
			? size_t(height->Mipmap[m + 1] - height->Mipmap[m])
			: total - size_t(height->Mipmap[m] - h0);
		if (mw < BX || mh < BY) {           // sub-block mip: copy verbatim
			std::memcpy(out, src, mipBytes);
			continue;
		}
		for (int y = 0; y < mh; ++y) {
			const float v = (float(y) + 0.5f) / float(mh);
			for (int x = 0; x < mw; ++x) {
				const float u = (float(x) + 0.5f) / float(mw);
				const size_t off = SwizzledOffset(x, y, blockX, blockY, mh);
				const float hFull = float(src[off]) * (1.0f / 255.0f);
				const float hLow  = SampleHeight8Bilinear(height, useMip, u, v);
				float r = hFull - hLow + meanLow;
				if (r < 0.0f) r = 0.0f;
				if (r > 1.0f) r = 1.0f;
				out[off] = byte(r * 255.0f + 0.5f);
			}
		}
	}
	std::fprintf(stderr,
		"[DISPLACE] residual height map: %dx%d mips=%u, lowMip=%d meanLow=%.3f\n",
		height->SizeX, height->SizeY, height->numMipmaps, useMip, (double)meanLow);
	return res;
}

#include <Base/Object.h>
#include <Base/TriMesh.h>
#include <Base/Vector.h>
#include <cstdlib>

Object *Scene_AddDynamicMesh(Scene *sc, TriMesh *mesh, const char *name,
                             const Vector &bsphereCtr, float bsphereRadius)
{
	if (!sc || !mesh) return nullptr;

	// SoA: stamp F->A_idx/B_idx/C_idx so the transform indexes the right verts.
	Compute_FaceVertexIndices(mesh);

	// Identity transform: caller writes world-space vertex positions directly.
	Matrix_Copy(mesh->RotMat, Mat_ID);
	Matrix_Copy(mesh->UnscaledRotMat, Mat_ID);
	mesh->IPos = { 0, 0, 0 };

	// Bounding sphere — large by default so moving verts never frustum-cull it.
	mesh->BSphereCtr    = bsphereCtr;
	mesh->BSphereRadius = bsphereRadius;
	mesh->BSphereRad    = bsphereRadius * bsphereRadius;

	// Dynamic marking: 2 Pos keys >0.1 apart → re-transformed every frame
	// (Transform won't treat it as a cached-static silhouette). Possessed so
	// Animate_Objects leaves IPos/RotMat alone (we own the verts).
	mesh->Pos.NumKeys = 2; mesh->Pos.CurKey = 0; mesh->Pos.Flags = 0;
	mesh->Pos.Keys = (SplineKey*)std::calloc(2, sizeof(SplineKey));
	mesh->Pos.Keys[0].Frame = 0;
	mesh->Pos.Keys[0].Pos.x = 0.0f; mesh->Pos.Keys[0].Pos.y = 0.0f; mesh->Pos.Keys[0].Pos.z = 0.0f;
	mesh->Pos.Keys[1].Frame = 100;
	mesh->Pos.Keys[1].Pos.x = 0.2f; mesh->Pos.Keys[1].Pos.y = 0.0f; mesh->Pos.Keys[1].Pos.z = 0.0f;

	mesh->Flags |= HTrack_Visible | Tri_Possessed | Tri_Noshading | Tri_NoShadowCast;

	Object *O = getAlignedType<Object>(16);
	std::memset(O, 0, sizeof(Object));
	O->Type = Obj_TriMesh;
	O->Data = mesh;
	O->Pos  = &mesh->IPos;
	O->Rot  = &mesh->RotMat;
	O->Name = strdup(name ? name : "__dynamicMesh");
	O->Next = sc->ObjectHead;
	if (sc->ObjectHead) sc->ObjectHead->Prev = O;
	sc->ObjectHead = O;
	mesh->Next = sc->TriMeshHead;
	if (sc->TriMeshHead) sc->TriMeshHead->Prev = mesh;
	sc->TriMeshHead = mesh;
	return O;
}
