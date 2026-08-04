#include "MeshOps.h"

#include "MaterialEditor.h"       // rev::Editor_BaseSurfName (collapse ::mirUV clones)
#include <FLD/LWREAD.H>           // Surf_Smoothing flag (authored per-surface smoothing)
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Scene.h>
#include <RENDER/WorldAabb.h>     // DisplaceViz_Record (--displace_viz overlay)
#include <Threads.h>              // dispatchIndexed — threaded cone-map bake

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <semaphore>
#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// ── S1 P1: EXACT PER-TEXEL CONE BAKE (--pom_cone_exact) ────────────────────
// MakeConeMap above bakes the cone from a MAX-POOLED 128² coarse grid. Two
// measured consequences (docs/S1_DISCREPANCY_INVENTORY.md §9):
//   1. the max over an 8×8 block of a stone height map is near 255 almost
//      everywhere, so almost no coarse cell has a TALLER cell to be bounded by
//      and the baked cone comes out enormous — mean byte 38.6 = cone ratio 0.61
//      before the ×4 relax, against a competing dlen ≤ ~0.7. The march then
//      takes a near-full gap every step and the cone map barely steers it
//      (--parallax_pom_relax 1 / 4 / 16 move the t=6097 error by <5 %);
//   2. lateral distance is quantised to 1/128 UV, so a texel one texel away
//      from a cliff is told the cliff is EIGHT texels away.
//
// This bakes the cone per TEXEL at the mip's own resolution instead, following
// Bán/Valasek/Bálint/Vad, "Robust Cone Step Mapping", EGSR 2024
// (github.com/Bundas102/robust-cone-map) — with one deliberate deviation:
//
//   THE PAPER'S CONSTRUCTION IS FOR A BILINEARLY INTERPOLATED HEIGHT FIELD.
//   OUR MARCH POINT-SAMPLES (roundi → nearest texel), so the surface it
//   actually intersects is piecewise CONSTANT over texel cells. The exact
//   conservative cone for that surface measures distance to the NEAREST POINT
//   OF THE TEXEL'S CELL, not to its centre — a ray reaches a neighbour's cell
//   half a texel before it reaches the neighbour's sample point. Using centre
//   distance (what the paper does, correctly, for bilinear) would over-state
//   the safe distance by half a texel, which at r=1 is a 100 % error on the
//   tightest constraint there is. So: dist = |max(|Δ|−½, 0)| / size, and the
//   paper's falling-edge (limiting-vertex) prune — which is a statement about
//   bilinear patches — is dropped in favour of the band bound below, which is
//   exact for this surface and prunes just as hard in practice.
//
// Two modes, both exact for the point-sampled surface:
//   1 CONSERVATIVE — the cone contains no geometry at all. c = min over q of
//     dist(p, cell(q)) / (h(q) − h(p)) for every taller q.
//   2 RELAXED (Policarpo, GPU Gems 3 ch.18, as corrected by the EGSR'24 paper)
//     — the cone may PENETRATE the field, and is bounded instead by where the
//     ray LEAVES it again, which is what guarantees the bracket the march finds
//     is the FIRST crossing. Wider cones, so fewer steps, same landing.
//
// Band bound (the early-out that makes this O(texels × small)): every texel on
// ring r is at least (r−½)/size away and can be at most (1 − h(p)) taller, so
// nothing at radius ≥ r can beat (r−½)/(size·(1−h(p))). Stop when that exceeds
// the best cone so far. For a texel at the top of the field (h≈1) the bound is
// immediately infinite and the scan stops at r=1.
//
// Quantisation is by TRUNCATION over [0, kPomConeExactMax] — rounding could
// round a cone UP and a too-wide cone is exactly what makes a march skip
// geometry. Toroidal wrap, as the coarse bake does, keeps a tiling map seamless.
// Threaded over rows. Caller owns the result.
namespace {

// Hard ceiling on the ring scan. The band bound normally stops it in a handful
// of rings; this only bites on unusually smooth regions, and beyond it the same
// band bound is applied as a CONSERVATIVE cap on the answer, so truncating the
// scan can only ever make the cone narrower (safe), never wider.
constexpr int kConeExactMaxRadius = 48;

// Relaxed cone: walk the ray that comes straight down onto the apex and passes
// through the candidate's surface point, past the candidate, until it emerges
// from the field again. The cone is then bounded by the EXIT point rather than
// the entry, which is what makes a relaxed cone wider than a conservative one
// while still guaranteeing a single crossing for any ray inside it.
inline float RelaxedConeExitRatio(const unsigned char *H, int mw, int mh,
                                  float px, float py, float baseH,
                                  float qx, float qy, float qh) {
	// src = directly above the apex at the top of the field; dst = the
	// candidate's surface point. Scale the direction so one unit of parameter
	// drops exactly one unit of height, then continue from dst down to h = 0.
	const float dz = qh - 1.0f;                  // < 0 (qh <= 1)
	if (dz >= -1e-6f) return 1.0f;               // candidate at the very top
	const float invW = 1.0f / float(mw), invH = 1.0f / float(mh);
	// Direction per unit height DROP, in UV.
	const float du = ((qx - px) * invW) / (-dz);
	const float dv = ((qy - py) * invH) / (-dz);
	// Remaining travel from the candidate down to h = 0.
	const float lat = std::sqrt(du * du + dv * dv) * qh;
	int steps = int(lat * float(mw)) + 1;        // ~one texel of lateral travel
	if (steps < 2) steps = 2;
	if (steps > 256) steps = 256;
	const float stepH = qh / float(steps);
	float ru = qx * invW, rv = qy * invH, rh = qh;
	for (int i = 0; i < steps; ++i) {
		ru -= du * stepH; rv -= dv * stepH; rh -= stepH;
		int sx = int(std::floor(ru * float(mw))) % mw; if (sx < 0) sx += mw;
		int sy = int(std::floor(rv * float(mh))) % mh; if (sy < 0) sy += mh;
		const float hs = float(H[size_t(sy) * mw + sx]) * (1.0f / 255.0f);
		if (hs < rh) break;                      // the ray has left the field
	}
	if (rh <= baseH) return 1.0f;                // exited below the apex
	const float ddu = (ru - px * invW), ddv = (rv - py * invH);
	return std::sqrt(ddu * ddu + ddv * ddv) / (rh - baseH);
}

}  // namespace

Texture *MakeConeMapExact(Texture *height, int mode) {
	if (!height || height->BPP != 8 || !height->Mipmap[0] || height->numMipmaps == 0)
		return nullptr;
	if (mode != 1 && mode != 2) return nullptr;
	const int blockX = height->blockSizeX, blockY = height->blockSizeY;
	const int BX = 1 << blockX, BY = 1 << blockY;
	size_t total = 0;
	{ int cx = height->SizeX >> blockX, cy = height->SizeY >> blockY;
	  for (dword i = 0; i < height->numMipmaps; ++i) {
	      total += size_t(cx) * size_t(cy) * size_t(BX) * size_t(BY);
	      cx = (cx + 1) >> 1; cy = (cy + 1) >> 1; } }
	Texture *cone = new Texture;
	*cone = *height;
	cone->Pal = nullptr; cone->FileName = nullptr; cone->ID = 0; cone->Flags = height->Flags;
	for (int i = 0; i < 16; ++i) cone->Mipmap[i] = nullptr;
	byte *dst = (byte *)getAlignedBlock(total);
	cone->Data = dst;
	std::memset(dst, 255, total);                 // default = flat (max cone)
	const byte *h0 = height->Mipmap[0];
	for (dword i = 0; i < height->numMipmaps; ++i)
		cone->Mipmap[i] = dst + size_t(height->Mipmap[i] - h0);

	const float kQuant = 255.0f / kPomConeExactMax;
	for (dword mip = 0; mip < height->numMipmaps; ++mip) {
		const int mw = std::max(1, height->SizeX >> mip);
		const int mh = std::max(1, height->SizeY >> mip);
		if (mw < BX || mh < BY) continue;         // tiny mip → leave flat (255)
		const byte *hmip = height->Mipmap[mip];
		// Un-swizzle once into a linear plane: the scan touches each texel from
		// many apexes, and SwizzledOffset per access would dominate the bake.
		std::vector<unsigned char> H(size_t(mw) * mh);
		for (int y = 0; y < mh; ++y)
			for (int x = 0; x < mw; ++x)
				H[size_t(y) * mw + x] = hmip[SwizzledOffset(x, y, blockX, blockY, mh)];
		std::vector<byte> C(size_t(mw) * mh, 255);
		const float invW = 1.0f / float(mw), invH = 1.0f / float(mh);
		std::counting_semaphore<INT_MAX> done{0};
		auto rowFn = [&](int y) {
			for (int x = 0; x < mw; ++x) {
				const float baseH = float(H[size_t(y) * mw + x]) * (1.0f / 255.0f);
				float minTan = kPomConeExactMax;
				const float headroom = 1.0f - baseH;
				if (headroom > 1e-6f) {
					const float bandK = invW / headroom;     // (r-½) multiplier
					int r = 1;
					for (; r <= kConeExactMaxRadius; ++r) {
						if ((float(r) - 0.5f) * bandK >= minTan) break;
						// Square ring of radius r, toroidal.
						for (int dy = -r; dy <= r; ++dy) {
							const bool edgeRow = (dy == -r || dy == r);
							for (int dx = -r; dx <= r; dx += (edgeRow ? 1 : 2 * r)) {
								int qx = (x + dx) % mw; if (qx < 0) qx += mw;
								int qy = (y + dy) % mh; if (qy < 0) qy += mh;
								const float qh = float(H[size_t(qy) * mw + qx]) * (1.0f / 255.0f);
								const float dh = qh - baseH;
								if (dh <= 0.0f) continue;
								// Distance to the nearest point of the CELL (the
								// march point-samples, so the cell is what the ray
								// actually enters), in UV.
								const float ax = std::max(0.0f, std::fabs(float(dx)) - 0.5f) * invW;
								const float ay = std::max(0.0f, std::fabs(float(dy)) - 0.5f) * invH;
								const float dist = std::sqrt(ax * ax + ay * ay);
								if (dist >= minTan * dh) continue;   // already inside the cone
								const float c = (mode == 2)
								    ? RelaxedConeExitRatio(H.data(), mw, mh,
								          float(x), float(y), baseH,
								          float(x + dx), float(y + dy), qh)
								    : dist / dh;
								if (c < minTan) minTan = c;
							}
						}
					}
					// Truncating the scan can only be allowed to NARROW the cone:
					// clamp by the band bound at the radius we stopped at.
					const float bound = (float(r) - 0.5f) * bandK;
					if (bound < minTan) minTan = bound;
				}
				// TRUNCATE, never round: a cone rounded UP lets the march skip.
				int q = int(minTan * kQuant);
				C[size_t(y) * mw + x] = byte(q < 0 ? 0 : (q > 255 ? 255 : q));
			}
		};
		dispatchIndexed(mh, &done, rowFn);
		for (int k = 0; k < mh; ++k) done.acquire();
		if (std::getenv("FDS_CONE_HIST") && mip < 4) {
			int hist[8] = {0}; double sum = 0;
			for (byte b : C) { hist[std::min(7, b / 32)]++; sum += b; }
			std::fprintf(stderr, "[CONE_HIST-EXACT%d] mip=%u texels=%zu meanByte=%.1f "
				"buckets[0-31,..,224-255]:", mode, mip, C.size(), sum / C.size());
			for (int b = 0; b < 8; ++b) std::fprintf(stderr, " %d", hist[b]);
			std::fprintf(stderr, "\n");
		}
		byte *cmip = cone->Mipmap[mip];
		for (int y = 0; y < mh; ++y)
			for (int x = 0; x < mw; ++x)
				cmip[SwizzledOffset(x, y, blockX, blockY, mh)] = C[size_t(y) * mw + x];
	}
	return cone;
}

// Disk cache for the exact bake, same self-validating single-file shape as the
// horizon cache: the key is the height field's own mip-0 bytes plus every bake
// parameter, so changing the map, the mode or the encode ceiling moves the key
// and a stale cache is impossible without a version bump.
static uint64_t ConeExactCacheKey(const Texture *height, int mode) {
	uint64_t h = 1469598103934665603ull;                       // FNV-1a
	auto mix = [&](const void *p, size_t n) {
		const unsigned char *b = (const unsigned char *)p;
		for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
	};
	mix(height->Mipmap[0], size_t(height->SizeX) * size_t(height->SizeY));
	const int32_t dims[5] = { height->SizeX, height->SizeY, int32_t(height->numMipmaps),
	                          mode, kConeExactMaxRadius };
	mix(dims, sizeof(dims));
	const float enc = kPomConeExactMax;
	mix(&enc, sizeof(enc));
	return h;
}

Texture *LoadOrBakeConeMapExact(Texture *height, int mode, const char *tag) {
	if (!height || height->BPP != 8 || !height->Mipmap[0]) return nullptr;
	constexpr uint32_t kMagic = 0x31454E43u;      // "CNE1"
	const uint64_t key = ConeExactCacheKey(height, mode);
	char path[512];
	std::snprintf(path, sizeof(path), "cache/pom_cone_exact_%016llx.bin",
	              (unsigned long long)key);
	// Total texels across the chain (the cone map mirrors the height layout).
	size_t total = 0;
	{ const int BX = 1 << height->blockSizeX, BY = 1 << height->blockSizeY;
	  int cx = height->SizeX >> height->blockSizeX, cy = height->SizeY >> height->blockSizeY;
	  for (dword i = 0; i < height->numMipmaps; ++i) {
	      total += size_t(cx) * size_t(cy) * size_t(BX) * size_t(BY);
	      cx = (cx + 1) >> 1; cy = (cy + 1) >> 1; } }
	if (FILE *f = std::fopen(path, "rb")) {
		uint32_t magic = 0; uint64_t texels = 0;
		if (std::fread(&magic, 4, 1, f) == 1 && magic == kMagic
		    && std::fread(&texels, 8, 1, f) == 1 && size_t(texels) == total) {
			Texture *cone = new Texture;
			*cone = *height;
			cone->Pal = nullptr; cone->FileName = nullptr; cone->ID = 0;
			for (int i = 0; i < 16; ++i) cone->Mipmap[i] = nullptr;
			byte *dst = (byte *)getAlignedBlock(total);
			cone->Data = dst;
			if (std::fread(dst, 1, total, f) == total) {
				const byte *h0 = height->Mipmap[0];
				for (dword i = 0; i < height->numMipmaps; ++i)
					cone->Mipmap[i] = dst + size_t(height->Mipmap[i] - h0);
				std::fclose(f);
				std::fprintf(stderr, "[POM-CONE-EXACT] '%s': cache hit %s\n", tag, path);
				return cone;
			}
			delete cone;
		}
		std::fclose(f);
	}
	auto t0 = std::chrono::high_resolution_clock::now();
	Texture *cone = MakeConeMapExact(height, mode);
	const double ms = std::chrono::duration<double, std::milli>(
		std::chrono::high_resolution_clock::now() - t0).count();
	if (!cone) {
		std::fprintf(stderr, "[POM-CONE-EXACT] '%s': bake FAILED (mode %d)\n", tag, mode);
		return nullptr;
	}
	std::fprintf(stderr, "[POM-CONE-EXACT] '%s': mode %d (%s), %d x %d, %zu texels, "
		"encode ceiling %.3f (%.0f ms) -> %s\n", tag, mode,
		mode == 2 ? "relaxed" : "conservative", height->SizeX, height->SizeY,
		total, (double)kPomConeExactMax, ms, path);
	std::filesystem::create_directories("cache");
	if (FILE *f = std::fopen(path, "wb")) {
		const uint64_t texels = total;
		std::fwrite(&kMagic, 4, 1, f);
		std::fwrite(&texels, 8, 1, f);
		std::fwrite(cone->Mipmap[0], 1, total, f);
		std::fclose(f);
	}
	return cone;
}

// ── S1c HORIZON MAP BAKE (--pom_horizon) ───────────────────────────────────
// Per texel of an 8-bit height map, the elevation of the relief's own horizon
// in kPomHorizonAzimuths evenly spaced tangent-space azimuths, as u8
// sin(horizon). Output layout: 8 bytes per texel in the SAME block-tile + mip
// order as the source, so the swizzled index the rasterizer already computed
// addresses it (see PomHorizonMap).
//
// THE SCALE IS UV-ONLY. tan(horizon) = Δh_world / Δlateral_world, and both
// scale with the face's world-per-UV: Δh_world = A_uv·worldPerUV·Δh,
// Δlateral_world = (Δtexels/N)·worldPerUV. The worldPerUV cancels, leaving
//   tan(horizon) = (A_uv · N) · Δh / Δtexels
// with A_uv the relief's UV amplitude (parallax_strength × ParallaxScale — the
// SAME number the shell is built with) and N the texels per UV tile at that
// mip. So one scalar, `heightScaleTexels`, plus the height field determines the
// bake at every mip, and a mesh rescale can never desync it.
//
// Sampling: dense for the first 8 texels then geometrically spaced to the scan
// radius. A distant occluder only matters if it is TALL, and max(Δh/r) is
// dominated by the near samples, so log spacing costs ~nothing in quality and
// turns an O(R) per-azimuth march into O(8 + log R). Toroidal wrap keeps a
// tiling map seamless (the cone bake wraps for the same reason).
// Threaded over rows. Caller owns the result.
PomHorizonMap *MakeHorizonMap(const Texture *height, float heightScaleUV,
                              int radiusTexels) {
	if (!height || height->BPP != 8 || !height->Mipmap[0] || height->numMipmaps == 0)
		return nullptr;
	if (!(heightScaleUV > 0.0f) || radiusTexels < 2) return nullptr;
	const int blockX = height->blockSizeX, blockY = height->blockSizeY;
	const int BX = 1 << blockX, BY = 1 << blockY;
	size_t total = 0;
	{ int cx = height->SizeX >> blockX, cy = height->SizeY >> blockY;
	  for (dword i = 0; i < height->numMipmaps; ++i) {
	      total += size_t(cx) * size_t(cy) * size_t(BX) * size_t(BY);
	      cx = (cx + 1) >> 1; cy = (cy + 1) >> 1; } }
	PomHorizonMap *hz = new PomHorizonMap;
	hz->numMipmaps = height->numMipmaps;
	hz->texels = total;
	hz->heightScaleTexels = heightScaleUV * float(height->SizeX);
	hz->radiusTexels = radiusTexels;
	hz->sizeX = height->SizeX; hz->sizeY = height->SizeY;
	hz->blockSizeX = height->blockSizeX; hz->blockSizeY = height->blockSizeY;
	hz->data = (unsigned char *)getAlignedBlock(total * kPomHorizonAzimuths);
	std::memset(hz->data, 0, total * kPomHorizonAzimuths);   // 0 = flat horizon
	const byte *h0 = height->Mipmap[0];
	for (dword i = 0; i < height->numMipmaps; ++i)
		hz->mipOfs[i] = size_t(height->Mipmap[i] - h0);
	// Azimuth k points along (cos, sin) of k·(360/kPomHorizonAzimuths)°.
	float dirX[kPomHorizonAzimuths], dirY[kPomHorizonAzimuths];
	for (int k = 0; k < kPomHorizonAzimuths; ++k) {
		const float a = float(k) * (6.283185307f / float(kPomHorizonAzimuths));
		dirX[k] = std::cos(a); dirY[k] = std::sin(a);
	}
	for (dword mip = 0; mip < height->numMipmaps; ++mip) {
		const int mw = std::max(1, height->SizeX >> mip);
		const int mh = std::max(1, height->SizeY >> mip);
		if (mw < BX || mh < BY) continue;         // tiny mip → leave flat (0)
		// Height units per texel HALVES with each mip (same world relief over
		// half the texels), and so does the useful scan radius.
		const float hst = heightScaleUV * float(mw);
		const int   R   = std::max(4, radiusTexels >> mip);
		// Sample radii: 1..8 dense, then ×1.3 out to R.
		std::vector<float> radii;
		for (int r = 1; r <= 8 && r <= R; ++r) radii.push_back(float(r));
		for (float r = 8.0f * 1.3f; r <= float(R); r *= 1.3f) radii.push_back(r);
		const byte *hmip = height->Mipmap[mip];
		unsigned char *out = hz->data + hz->mipOfs[mip] * kPomHorizonAzimuths;
		std::counting_semaphore<INT_MAX> done{0};
		auto rowFn = [&](int y) {
			for (int x = 0; x < mw; ++x) {
				const float hp = float(hmip[SwizzledOffset(x, y, blockX, blockY, mh)])
				               * (1.0f / 255.0f);
				unsigned char *dst = out + SwizzledOffset(x, y, blockX, blockY, mh)
				                         * kPomHorizonAzimuths;
				for (int k = 0; k < kPomHorizonAzimuths; ++k) {
					float maxSlope = 0.0f;             // max Δh / Δtexels
					for (float r : radii) {
						int qx = int(std::lround(float(x) + dirX[k] * r));
						int qy = int(std::lround(float(y) + dirY[k] * r));
						qx %= mw; if (qx < 0) qx += mw;      // toroidal
						qy %= mh; if (qy < 0) qy += mh;
						const float dh = float(hmip[SwizzledOffset(qx, qy, blockX, blockY, mh)])
						               * (1.0f / 255.0f) - hp;
						if (dh <= 0.0f) continue;
						const float s = dh / r;
						if (s > maxSlope) maxSlope = s;
					}
					// tan → sin, then quantize. tan/sqrt(1+tan²) saturates
					// gracefully, so even a texel-scale cliff stays in range.
					const float t = maxSlope * hst;
					const float sinH = t * (1.0f / std::sqrt(1.0f + t * t));
					const int q = int(sinH * 255.0f + 0.5f);
					dst[k] = (unsigned char)(q < 0 ? 0 : (q > 255 ? 255 : q));
				}
			}
		};
		dispatchIndexed(mh, &done, rowFn);
		for (int k = 0; k < mh; ++k) done.acquire();
	}
	return hz;
}

// Disk cache for the horizon bake. A full 1024² × 8-azimuth bake is seconds,
// which is fine once and intolerable every launch, so the result is keyed on
// the height field's own bytes plus every bake parameter — change the map, the
// amplitude or the radius and the key moves, so a stale cache is impossible
// without a version bump. Same self-validating single-file shape as the city
// panorama cache (Runtime/cache/).
static uint64_t HorizonCacheKey(const Texture *height, float heightScaleUV,
                                int radiusTexels) {
	uint64_t h = 1469598103934665603ull;                       // FNV-1a
	auto mix = [&](const void *p, size_t n) {
		const unsigned char *b = (const unsigned char *)p;
		for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
	};
	// The mip-0 plane is the content; dims/params disambiguate the rest.
	const size_t n0 = size_t(height->SizeX) * size_t(height->SizeY);
	mix(height->Mipmap[0], n0);
	const int32_t dims[5] = { height->SizeX, height->SizeY, int32_t(height->numMipmaps),
	                          radiusTexels, kPomHorizonAzimuths };
	mix(dims, sizeof(dims));
	mix(&heightScaleUV, sizeof(heightScaleUV));
	return h;
}

PomHorizonMap *LoadOrBakeHorizonMap(const Texture *height, float heightScaleUV,
                                    int radiusTexels, const char *tag) {
	if (!height || height->BPP != 8 || !height->Mipmap[0]) return nullptr;
	constexpr uint32_t kMagic = 0x315A4850u;      // "PHZ1"
	const uint64_t key = HorizonCacheKey(height, heightScaleUV, radiusTexels);
	char path[512];
	std::snprintf(path, sizeof(path), "cache/pom_horizon_%016llx.bin",
	              (unsigned long long)key);
	if (FILE *f = std::fopen(path, "rb")) {
		uint32_t magic = 0; uint64_t texels = 0; uint32_t nmip = 0;
		if (std::fread(&magic, 4, 1, f) == 1 && magic == kMagic
		    && std::fread(&texels, 8, 1, f) == 1
		    && std::fread(&nmip, 4, 1, f) == 1 && nmip <= 16) {
			PomHorizonMap *hz = new PomHorizonMap;
			hz->numMipmaps = nmip;
			hz->texels = size_t(texels);
			hz->heightScaleTexels = heightScaleUV * float(height->SizeX);
			hz->radiusTexels = radiusTexels;
			hz->sizeX = height->SizeX; hz->sizeY = height->SizeY;
			hz->blockSizeX = height->blockSizeX; hz->blockSizeY = height->blockSizeY;
			uint64_t ofs[16] = {};
			const size_t bytes = size_t(texels) * kPomHorizonAzimuths;
			if (std::fread(ofs, 8, nmip, f) == nmip) {
				hz->data = (unsigned char *)getAlignedBlock(bytes);
				if (std::fread(hz->data, 1, bytes, f) == bytes) {
					for (unsigned i = 0; i < nmip; ++i) hz->mipOfs[i] = size_t(ofs[i]);
					std::fclose(f);
					std::fprintf(stderr, "[POM-HORIZON] '%s': cache hit %s\n", tag, path);
					return hz;
				}
			}
			delete hz;
		}
		std::fclose(f);
	}
	auto t0 = std::chrono::high_resolution_clock::now();
	PomHorizonMap *hz = MakeHorizonMap(height, heightScaleUV, radiusTexels);
	const double ms = std::chrono::duration<double, std::milli>(
		std::chrono::high_resolution_clock::now() - t0).count();
	if (!hz) {
		std::fprintf(stderr, "[POM-HORIZON] '%s': bake FAILED\n", tag);
		return nullptr;
	}
	std::fprintf(stderr, "[POM-HORIZON] '%s': baked %zu texels x %d azimuths, "
		"scale %.1f height-units/texel, radius %d (%.0f ms) -> %s\n",
		tag, hz->texels, kPomHorizonAzimuths, (double)hz->heightScaleTexels,
		radiusTexels, ms, path);
	std::filesystem::create_directories("cache");
	if (FILE *f = std::fopen(path, "wb")) {
		const uint64_t texels = hz->texels;
		const uint32_t nmip = hz->numMipmaps;
		uint64_t ofs[16] = {};
		for (unsigned i = 0; i < nmip && i < 16; ++i) ofs[i] = hz->mipOfs[i];
		std::fwrite(&kMagic, 4, 1, f);
		std::fwrite(&texels, 8, 1, f);
		std::fwrite(&nmip, 4, 1, f);
		std::fwrite(ofs, 8, nmip, f);
		std::fwrite(hz->data, 1, size_t(texels) * kPomHorizonAzimuths, f);
		std::fclose(f);
	}
	return hz;
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

// Estimate the dominant BLOCK PITCH (mortar-to-mortar period) of a height map,
// in TEXELS at `mip`, per axis. Mortar lines are ridges of the height gradient;
// summing |∂h| down each column gives a 1-D signal in x whose period is the
// vertical-mortar pitch (and the row sum gives the horizontal pitch). The
// dominant period is the highest local maximum of that signal's normalized
// autocorrelation in [tile/16, tile/2] (i.e. a block is between 1/16 and 1/2 of
// a UV tile). This is map-RELATIVE: the pitch is a property of the height field,
// independent of how a wall is UV-mapped, so a subdivision cell whose texel
// footprint is ≤ ~half this pitch resolves one block regardless of how many
// tiles a quad spans (the fix for "cells span 2-3 blocks on the finely-tiled
// wall"). Returns false with no clear periodicity (caller falls back to a fixed
// texel target). Cheap + one-time (per material at bake).
bool EstimateBlockPitch(const Texture *hm, int mip,
                        float &pitchXtex, float &pitchYtex) {
	pitchXtex = pitchYtex = 0.0f;
	if (!hm || !hm->Mipmap[mip]) return false;
	const int mw = std::max(1, int(hm->SizeX) >> mip);
	const int mh = std::max(1, int(hm->SizeY) >> mip);
	if (mw < 16 || mh < 16) return false;
	const byte *data = hm->Mipmap[mip];
	const int bx = hm->blockSizeX, by = hm->blockSizeY;
	auto TX = [&](int x, int y) -> float { return float(data[SwizzledOffset(x, y, bx, by, mh)]); };
	std::vector<float> colsig(mw, 0.0f), rowsig(mh, 0.0f);
	for (int y = 0; y < mh; ++y)
		for (int x = 0; x + 1 < mw; ++x) colsig[x] += std::fabs(TX(x + 1, y) - TX(x, y));
	for (int y = 0; y + 1 < mh; ++y)
		for (int x = 0; x < mw; ++x)     rowsig[y] += std::fabs(TX(x, y + 1) - TX(x, y));
	auto dominant = [](const std::vector<float> &s, int n) -> float {
		double m = 0; for (int i = 0; i < n; ++i) m += s[i]; m /= n;
		std::vector<double> d(n); double a0 = 0;
		for (int i = 0; i < n; ++i) { d[i] = double(s[i]) - m; a0 += d[i] * d[i]; }
		if (a0 < 1e-9) return 0.0f;                       // flat signal: no mortar grid
		const int lo = std::max(4, n / 16), hi = n / 2;
		std::vector<double> ac(hi + 2, 0.0);
		for (int lag = lo - 1; lag <= hi + 1 && lag < n; ++lag) {
			double s2 = 0; for (int i = 0; i + lag < n; ++i) s2 += d[i] * d[i + lag];
			ac[lag] = s2 / a0;
		}
		double bestVal = -1e30; int bestLag = 0;
		for (int lag = lo; lag <= hi; ++lag)
			if (ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1] && ac[lag] > bestVal) {
				bestVal = ac[lag]; bestLag = lag;
			}
		return bestVal > 0.15 ? float(bestLag) : 0.0f;   // require a clear peak
	};
	pitchXtex = dominant(colsig, mw);
	pitchYtex = dominant(rowsig, mh);
	return pitchXtex > 0.0f && pitchYtex > 0.0f;
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
		const Material *targetMat = nullptr;
		int nTarget = 0;
		for (int32_t i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!isTarget(&F)) continue;
			++nTarget;
			if (!targetMat) targetMat = F.Txtr;
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
			// --displace_viz: record the FINAL (post-displacement) position +
			// |offset| so the overlay can wireframe this geometry. No-op unless
			// the flag is on (the recorder self-gates); keyed by position bits,
			// which survive the greets chunk copy + per-face split downstream.
			fds::DisplaceViz_Record(targetMat, V[i].Pos, std::fabs(d));
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

// ── Symmetric adaptive stone subdivision + displacement ─────────────────────
// docs/ENVDYN_DISPLACEMENT_PLAN.md workstream B, slices S1/S2. Replaces the
// old two-step "SubdivideMaterialFaces(linear) + DisplaceMaterialVertices" for
// the greets stone. Fixes the PROVEN diagonal-ridge artifact (commit 4633aeb):
// every stone quad was two triangles sharing one diagonal, so displacing the
// interior put a roof-ridge along every quad diagonal — a uniform diagonal
// grain. Here each quad is retriangulated SYMMETRICALLY (a 2^L grid whose relief
// cells become 4-triangle centre fans, so a block's height peak lands on a
// vertex — a dome — not on a shared edge); lone triangles (unpaired halves) get
// symmetric triangular subdivision (no consistent diagonal). Subdivision DEPTH
// is chosen per quad from the height map's local busyness (S2): flat mortar
// cells stay coarse, block-edge cells go deep. Level-boundary cracks are closed
// by pinning the finer side's extra edge verts onto the coarser neighbour's
// straight segment (a generalisation of the authored-border pinning).
//
// Runs at the momy/stone hook: after Preprocess (per-vertex normals exist) +
// GreetsRetileFloor (per-face UVs final), before MakeFacesIndependentByAngle
// (re-derives vertex normals + tangents on the displaced mesh) and the Piramid
// chunk split. Face N + NormProd re-derived here (B1 lesson).
//
// uniformLevel > 0 forces that level on every patch (the uniform-L2 baseline
// the S2 measurement compares against); <= 0 selects adaptive depth 0..maxLevel
// from busyness scaled by `adapt` (higher = deeper). amp/mip as
// DisplaceMaterialVertices. Records to --displace_viz.
namespace {
inline uint32_t meshF2bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float meshLerpf(float a, float b, float t) { return a + (b - a) * t; }

// Parent-plane registry (see MeshOps.h MeshOps_StoneParentPlane): per target
// material, the deduped authored planes of the displaced faces + the quantized-
// key index used for dedup. Rebuilt per DisplaceStoneSubdiv call.
std::map<std::string, std::vector<StoneParentPlane>> g_stoneParentPlanes;
std::map<std::string, std::map<std::array<int,4>, uint16_t>> g_stoneParentPlaneKeys;

// Edge-aligned tessellation (the flat-top fix): the mortar-groove grid of a
// stone height map, detected in MAP space at the bake mip. A groove RUN is a
// below-threshold interval of a mean-height profile (mortar is recessed, so
// the bimodal plateau/mortar field splits at the profile's min/max midpoint).
// Horizontal runs are GLOBAL (one per-row profile); vertical runs are PER BAND
// — the block row between two horizontal grooves — so running bond, where the
// vertical mortar phase alternates per block row, lands naturally as per-band
// positions. Runs are straight lines through the band: a few texels of organic
// wander is absorbed by the transition-band shoulder pads, not modeled.
struct StoneGRun { float lo, hi; };                  // texels at the bake mip, lo<hi
struct StoneGrooveGrid {
	bool valid = false;
	std::vector<StoneGRun> h;                        // horizontal grooves (y runs)
	std::vector<std::pair<float,float>> bandY;       // band k y-range; second may exceed extent (wrap)
	std::vector<std::vector<StoneGRun>> vPerBand;    // per band: vertical grooves (x runs)
};
// One v-period of the edge-aligned ROW layout (built from the h-grooves +
// shoulder pads): type 0 = plateau (flat block row), 1 = step (transition band
// carrying the wall), 2 = floor (flat mortar floor). bandA/bandB are the
// adjacent block-row bands whose vertical grooves the row's columns follow.
struct StoneRowT { float y0, y1; int type; int bandA, bandB; };
}  // namespace

const StoneParentPlane *MeshOps_StoneParentPlane(const char *matName, uint16_t ordinal) {
	if (!matName || ordinal == 0) return nullptr;
	auto it = g_stoneParentPlanes.find(matName);
	if (it == g_stoneParentPlanes.end()) return nullptr;
	if (size_t(ordinal) > it->second.size()) return nullptr;
	return &it->second[ordinal - 1];
}

void DisplaceStoneSubdiv(Scene *Sc, const char *matName, int uniformLevel,
                         float amp, int mip, float adapt, float cellsPerBlock,
                         const std::vector<std::string> *displacedSet) {
	if (!Sc || !matName) return;
	if (cellsPerBlock < 0.25f) cellsPerBlock = 0.25f;
	g_stoneParentPlanes[matName].clear();       // fresh registry per bake call
	g_stoneParentPlaneKeys[matName].clear();
	auto isTarget = [&](const Face *F) {
		return F && F->Txtr && F->Txtr->Name && !std::strcmp(F->Txtr->Name, matName);
	};
	// A face is "displaced" (a sibling of the target, or the target itself) if its
	// material is in displacedSet; when no set is supplied only matName counts.
	// Non-displaced faces are the neighbours the seam-pin classifies against.
	auto isDisplacedMat = [&](const char *nm) -> bool {
		if (!nm) return false;
		if (!std::strcmp(nm, matName)) return true;
		if (!displacedSet) return false;
		for (const std::string &s : *displacedSet) if (s == nm) return true;
		return false;
	};
	// Hard ceiling on subdivision depth. Was 3 (8×8 cells/quad) — too coarse
	// for walls whose quads span several UV tiles: at the finely-tiled wall
	// (block pitch 0.25 UV, but quads covering 2-3 tiles) L3 leaves each cell
	// spanning 1-1.5 whole blocks, so the geometry carries no per-block relief
	// (ON≈OFF). The real depth is now driven PER QUAD by the block pitch (see
	// blockLevelCap below); this is only the safety cap so a pathological quad
	// can't run away. L5 = 32×32 cells resolves ~half-block on a 4-tile quad.
	const int kMaxLevel = 5;
	// Adaptive depth = the smallest level whose REFINEMENT ERROR is small
	// enough, but never deeper than the block-pitch cap. At level L the patch
	// is a 2^L×2^L cell grid; each cell's error is the max deviation of the
	// true height, probed over the cell's FULL texel footprint (strided) at the
	// bake mip, from the bilinear of its 4 corner heights — the relief the
	// GEOMETRY at that level cannot carry (POM's B4 residual carries it, so the
	// epsilon is literally the geometry/POM split point). Probing the whole
	// footprint (not a fixed 9-point stencil) is what makes the metric HONEST:
	// a coarse cell spanning whole blocks used to alias — its stencil landed on
	// similar-height block interiors and missed the mortar, so the cell read
	// "matched" while carrying no relief. A level passes when ≤12% of its cells
	// exceed the epsilon (a lone busy corner shouldn't force a whole quad
	// deeper). `adapt` scales the epsilon (>1 = stricter = deeper).
	const float kRefineEps  = 0.07f * (adapt > 1e-3f ? 1.0f / adapt : 1.0f);
	const float kRefineFrac = 0.12f;
	// A cell with corner+centre height spread below this carries no visible
	// relief (amp 0.3 → <9mm world): 2 triangles, shortest diagonal.
	const float kCellFlatEps = 0.03f;
	// A cell fans (centre vertex, 4 triangles) only when the centre height
	// genuinely DEVIATES from the corner plane by this much — the actual
	// roof-ridge error of a 2-triangle split. Monotone slope cells stay 2
	// triangles with a field-following diagonal (orientation varies with
	// content — no uniform grain).
	const float kCellDomeEps = 0.04f;

	// ── scene-wide NON-DISPLACED position index (cross-material seam pinning) ──
	// Built ONCE per call over EVERY mesh's faces whose material is not displaced
	// (isDisplacedMat). Two structures, keyed by a fine world-space grid so
	// authored-coincident verts (bit-identical, or a sub-lattice apart) collide:
	//   ndVert  — the set of non-displaced vertex positions.
	//   ndEdge  — the set of non-displaced face EDGES (ordered grid-key pairs).
	// A target vertex coincident with an ndVert, or a target edge coincident with
	// an ndEdge, is an authored border → pinned to zero (see the per-mesh use
	// below). Only ever ADDS pins, so with the seam-pin flag off it's inert.
	const bool seamPin = fds::FeatureFlags::greets_displace_neighbor_pin();
	constexpr double kSeamGrid = 1.0e4;   // 1e-4 world-unit cells (matches SUBDIV-DIAG)
	auto seamKey = [](const Vector &p) -> uint64_t {
		// pack rounded (x,y,z) into 64 bits via a mix — 21 bits/axis is ample for
		// the ~±10^3 unit scene at 1e-4 resolution collision-free-enough for a set.
		const int64_t xi = int64_t(std::llround(double(p.x) * kSeamGrid));
		const int64_t yi = int64_t(std::llround(double(p.y) * kSeamGrid));
		const int64_t zi = int64_t(std::llround(double(p.z) * kSeamGrid));
		uint64_t h = 1469598103934665603ull;
		for (int64_t v : { xi, yi, zi }) { h ^= uint64_t(v); h *= 1099511628211ull; }
		return h;
	};
	auto edgeKey = [&](const Vector &a, const Vector &b) -> uint64_t {
		uint64_t ka = seamKey(a), kb = seamKey(b);
		if (ka > kb) std::swap(ka, kb);
		return (ka * 1099511628211ull) ^ (kb + 0x9e3779b97f4a7c15ull);
	};
	std::unordered_set<uint64_t> ndVert, ndEdge;
	std::unordered_map<uint64_t, const char *> ndVertMat;   // audit: which neighbour
	if (seamPin) {
		for (TriMesh *M = Sc->TriMeshHead; M; M = M->Next) {
			if (M->FIndex == 0 || !M->Faces || !M->Verts) continue;
			for (int32_t i = 0; i < M->FIndex; ++i) {
				const Face &F = M->Faces[i];
				if (!F.A || !F.B || !F.C) continue;
				const char *nm = (F.Txtr && F.Txtr->Name) ? F.Txtr->Name : nullptr;
				if (isDisplacedMat(nm)) continue;
				const Vector &pa = F.A->Pos, &pb = F.B->Pos, &pc = F.C->Pos;
				const uint64_t ka = seamKey(pa), kb = seamKey(pb), kc = seamKey(pc);
				ndVert.insert(ka); ndVert.insert(kb); ndVert.insert(kc);
				if (nm) { ndVertMat.emplace(ka, nm); ndVertMat.emplace(kb, nm); ndVertMat.emplace(kc, nm); }
				ndEdge.insert(edgeKey(pa, pb));
				ndEdge.insert(edgeKey(pb, pc));
				ndEdge.insert(edgeKey(pc, pa));
			}
		}
	}

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
			std::fprintf(stderr, "[STONE] '%s': no 8-bit HeightMap (need "
			             "--greets_stone_tex + --parallax) — skipped\n", matName);
			continue;
		}
		// Clamp mip to a present, block-tileable level (same guard as elsewhere).
		int useMip = mip;
		if (useMip >= int(hm->numMipmaps)) useMip = int(hm->numMipmaps) - 1;
		if (useMip < 0) useMip = 0;
		while (useMip > 0 &&
		       ((std::max(1, int(hm->SizeX) >> useMip) < (1 << hm->blockSizeX)) ||
		        (std::max(1, int(hm->SizeY) >> useMip) < (1 << hm->blockSizeY))))
			--useMip;
		// Degenerate-map guard + mip mean (relief is zero-mean about it) + the
		// map's peak-to-valley height range (drives the viz's absolute error
		// scale: ±full range = "geometry carries none of the block relief").
		float mipMean = 0.5f;
		float reliefRange01 = 0.0f;   // (hi-lo)/255 at the bake mip
		{
			const int mw = std::max(1, int(hm->SizeX) >> useMip);
			const int mh = std::max(1, int(hm->SizeY) >> useMip);
			const byte *d = hm->Mipmap[useMip];
			byte lo = 255, hi = 0; uint64_t sum = 0;
			const size_t n = size_t(mw) * size_t(mh);
			for (size_t i = 0; i < n; ++i) { const byte b = d[i];
				if (b < lo) lo = b; if (b > hi) hi = b; sum += b; }
			if (hi - lo < 2) {
				std::fprintf(stderr, "[STONE] '%s': height mip%d (near-)constant "
				             "(%d..%d) — no relief, skipped\n", matName, useMip, lo, hi);
				continue;
			}
			mipMean = float(double(sum) / double(n)) * (1.0f / 255.0f);
			reliefRange01 = float(hi - lo) * (1.0f / 255.0f);
		}

		// Block pitch (mortar period) in useMip texels → the TARGET cell texel
		// footprint (≤ half a block, so a block gets ~2 cells across and its
		// interior pops out with the mortar recessed at the cell boundary). This
		// is what drives the per-quad depth cap below: map-relative, so however
		// many UV tiles a quad covers, its cells land at block scale. Falls back
		// to a fixed target when the map has no clear grid.
		const int useMipW = std::max(1, int(hm->SizeX) >> useMip);
		const int useMipH = std::max(1, int(hm->SizeY) >> useMip);
		float pitchXtex = 0.0f, pitchYtex = 0.0f;
		const bool havePitch = EstimateBlockPitch(hm, useMip, pitchXtex, pitchYtex);
		// Target cell texel footprint = block pitch / cellsPerBlock, clamped to a
		// sane texel range. Without a pitch, aim for ~cellsPerBlock cells per
		// 1/6-tile (a reasonable stone scale) as a fixed fallback.
		const float invCpb = 1.0f / cellsPerBlock;
		const float kFallbackTargetTex = std::max(2.0f, float(useMipW) * (1.0f/6.0f) * invCpb);
		const float targetTexX = havePitch ? std::max(2.0f, invCpb * pitchXtex) : kFallbackTargetTex;
		const float targetTexY = havePitch ? std::max(2.0f, invCpb * pitchYtex) : kFallbackTargetTex;
		std::fprintf(stderr,
			"[STONE] '%s' block pitch %s: %.0fx%.0f texels @ mip%d (%.2fx%.2f blocks/tile) "
			"-> target cell ≤ %.0fx%.0f texels\n", matName,
			havePitch ? "estimated" : "FALLBACK", (double)pitchXtex, (double)pitchYtex,
			useMip, havePitch ? double(useMipW) / std::max(1.0f, pitchXtex) : 0.0,
			havePitch ? double(useMipH) / std::max(1.0f, pitchYtex) : 0.0,
			(double)targetTexX, (double)targetTexY);

		// ── mortar-groove grid detection (edge-aligned tessellation, the
		// flat-top fix; --greets_displace_edge). Cell borders will SNAP onto
		// these groove lines: block plateaus become single FLAT cells, the step
		// down/up rides a narrow transition band at each groove — instead of
		// the centre-fan dome the uniform 2^L grid made of every block
		// (--scene-displacetest: FLAT-TOP 0% at cpb=1). Detection is skipped
		// (grid.valid=false → the adaptive fan path below is used unchanged)
		// when the pitch estimator failed (structure-free maps: the smooth
		// control must stay coarse), the profile isn't bimodal, or the run
		// layout fails sanity — and the uniform baseline bypasses it. ──
		const bool edgeMode = fds::FeatureFlags::greets_displace_edge() && uniformLevel <= 0;
		StoneGrooveGrid grid;
		const float kPadTex = 1.25f;       // shoulder pad, texels at the bake mip
		const float kMinFloorTex = 1.5f;   // min flat floor width to earn its own row
		std::vector<StoneRowT> rowTpl;     // one v-period, ascending, contiguous
		std::vector<std::vector<float>> colTpl;   // per band: column LINE x-positions, one period
		// Per-LINE representative heights (--greets_displace_line_height, the
		// GRAZING-ZIGZAG fix). Two populations, one principle — verts tracing a
		// groove must displace CONSISTENTLY along it:
		//   * EDGE-aligned patches: every line vert takes the line's rep (median
		//     of the height field along the line) instead of its own mid-slope
		//     bilinear sample. Keeps the edge path's clean carve, kills its
		//     along-line variance.
		//   * FAN/LONE fallback patches (charts the edge path rejects — e.g. the
		//     t=2845 repro wall, whose authored UV chart is a 4:1 TRAPEZOID):
		//     the block-pitch lattice cannot represent a few-texel groove; verts
		//     inside a groove's influence zone each sampled mid-slope and the
		//     resulting smeared carve shears into a per-cell chevron sawtooth at
		//     grazing views (measured: zeroing just the strip's displacement
		//     straightens the groove; making the strip's heights merely
		//     CONSISTENT does not). Those verts pin to the groove's PLATEAU
		//     reference (median sampled at an inset well outside the groove's
		//     mip-blur), so the plateau continues across the groove and the
		//     mortar reads via texture/normal map/POM — which at fallback
		//     granularity it already did.
		// pos uses the same float expressions as rowTpl/colTpl so emitted verts
		// land on it; plateau is the groove-side inset reference.
		struct StoneLineRep { float pos; float rep; float plateau; };
		std::vector<StoneLineRep> hLineRep;                 // horizontal groove lines (y)
		std::vector<std::vector<StoneLineRep>> vLineRep;    // per band: vertical lines (x)
		const bool lineHeightOn = fds::FeatureFlags::greets_displace_line_height();
		if (edgeMode && havePitch) {
			const byte *gdat = hm->Mipmap[useMip];
			const int gbx = hm->blockSizeX, gby = hm->blockSizeY;
			auto GTX = [&](int x, int y) -> float {
				return float(gdat[SwizzledOffset(x, y, gbx, gby, useMipH)]) * (1.0f/255.0f); };
			// below-threshold runs of a mean-height profile, wrap-aware
			auto profileRuns = [&](const std::vector<float> &prof, float pitchTex,
			                       std::vector<StoneGRun> &runs) -> bool {
				const int n = int(prof.size());
				runs.clear();
				if (pitchTex < 4.0f || n < 8) return false;
				float lo = 1e30f, hi = -1e30f;
				for (float v : prof) { lo = std::min(lo, v); hi = std::max(hi, v); }
				if (hi - lo < 0.06f) return false;              // no groove contrast
				const float thr = lo + 0.5f * (hi - lo);
				std::vector<char> low(size_t(n), 0);
				int nLow = 0;
				for (int i = 0; i < n; ++i) { low[i] = prof[i] < thr; nLow += low[i]; }
				if (nLow == 0 || nLow > n / 2) return false;    // mortar must be the minority
				int s0 = 0;
				for (int i = 0; i < n; ++i) if (!low[i]) { s0 = i; break; }
				int i = 0;
				while (i < n) {
					if (!low[(s0 + i) % n]) { ++i; continue; }
					int j = i;
					while (j < n && low[(s0 + j) % n]) ++j;
					float rlo = float(s0 + i), rhi = float(s0 + j);
					if (rhi - rlo >= 1.0f) {
						if (rhi - rlo > 0.6f * pitchTex) return false;   // too wide: not mortar
						while (rlo >= float(n)) { rlo -= float(n); rhi -= float(n); }
						runs.push_back({rlo, rhi});
					}
					i = j;
				}
				if (runs.empty()) return false;
				if (float(runs.size()) > float(n) / pitchTex * 2.0f + 2.0f) return false;
				std::sort(runs.begin(), runs.end(),
				          [](const StoneGRun &a, const StoneGRun &b){ return a.lo < b.lo; });
				return true;
			};
			std::vector<float> rowProf(size_t(useMipH), 0.0f);
			for (int y = 0; y < useMipH; ++y) {
				float s = 0.0f;
				for (int x = 0; x < useMipW; ++x) s += GTX(x, y);
				rowProf[y] = s / float(useMipW);
			}
			const bool hOK = profileRuns(rowProf, pitchYtex, grid.h);
			if (!hOK) grid.h.clear();
			// bands between consecutive horizontal grooves (wrapping); with no
			// horizontal grooves (vertical planks) one band spans the extent.
			if (grid.h.empty()) grid.bandY.push_back({0.0f, float(useMipH)});
			else for (size_t g = 0; g < grid.h.size(); ++g) {
				const float y0 = grid.h[g].hi;
				float y1 = grid.h[(g + 1) % grid.h.size()].lo;
				if (y1 <= y0) y1 += float(useMipH);
				grid.bandY.push_back({y0, y1});
			}
			grid.vPerBand.resize(grid.bandY.size());
			bool anyV = false;
			for (size_t b = 0; b < grid.bandY.size(); ++b) {
				std::vector<float> colProf(size_t(useMipW), 0.0f);
				const int yy0 = int(std::ceil(grid.bandY[b].first));
				const int rows = std::max(1, int(std::floor(grid.bandY[b].second)) - yy0);
				for (int x = 0; x < useMipW; ++x) {
					float s = 0.0f;
					for (int y = yy0; y < yy0 + rows; ++y) s += GTX(x, ((y % useMipH) + useMipH) % useMipH);
					colProf[x] = s / float(rows);
				}
				if (profileRuns(colProf, pitchXtex, grid.vPerBand[b])) anyV = true;
				else grid.vPerBand[b].clear();
			}
			grid.valid = hOK || anyV;
			// row template (one v-period): step/floor/step per wide groove (a
			// flat mortar floor between the pads), step/step around the centre
			// for narrow grooves, plateau rows shoulder-to-shoulder between.
			if (grid.valid && !grid.h.empty()) {
				const int nh = int(grid.h.size());
				for (int g = 0; g < nh; ++g) {
					const int bAbove = (g + nh - 1) % nh;
					const int bBelow = g;
					const float lo = grid.h[g].lo, hi = grid.h[g].hi;
					const float A = lo - kPadTex, B = lo + kPadTex;
					const float C = hi - kPadTex, D = hi + kPadTex;
					if (C - B >= kMinFloorTex) {
						rowTpl.push_back({A, B, 1, bAbove, bAbove});
						rowTpl.push_back({B, C, 2, bAbove, bBelow});
						rowTpl.push_back({C, D, 1, bBelow, bBelow});
					} else {
						const float M = 0.5f * (lo + hi);
						rowTpl.push_back({A, M, 1, bAbove, bAbove});
						rowTpl.push_back({M, D, 1, bBelow, bBelow});
					}
					float nA = grid.h[(g + 1) % nh].lo - kPadTex;
					if (g + 1 == nh) nA += float(useMipH);
					rowTpl.push_back({D, nA, 0, bBelow, bBelow});
				}
			} else if (grid.valid) {
				rowTpl.push_back({0.0f, float(useMipH), 0, 0, 0});
			}
			// column line template per band (same construction along x)
			colTpl.resize(grid.vPerBand.size());
			for (size_t b = 0; b < grid.vPerBand.size() && grid.valid; ++b) {
				for (const StoneGRun &r : grid.vPerBand[b]) {
					const float A = r.lo - kPadTex, B = r.lo + kPadTex;
					const float C = r.hi - kPadTex, D = r.hi + kPadTex;
					colTpl[b].push_back(A);
					if (C - B >= kMinFloorTex) { colTpl[b].push_back(B); colTpl[b].push_back(C); }
					else                         colTpl[b].push_back(0.5f * (r.lo + r.hi));
					colTpl[b].push_back(D);
				}
				std::sort(colTpl[b].begin(), colTpl[b].end());
				for (size_t i = 1; i < colTpl[b].size(); ++i)
					if (colTpl[b][i] - colTpl[b][i-1] < 0.25f) { grid.valid = false; break; }
			}
			// sanity: rows strictly ascending + contiguous (overlapping shoulder
			// pads from grooves closer than 2*pad would fold the layout) —
			// pathological content falls back to the adaptive fan path.
			for (size_t i = 0; i < rowTpl.size() && grid.valid; ++i) {
				if (rowTpl[i].y1 - rowTpl[i].y0 < 0.05f) { grid.valid = false; break; }
				if (i + 1 < rowTpl.size() &&
				    std::fabs(rowTpl[i+1].y0 - rowTpl[i].y1) > 0.01f) { grid.valid = false; break; }
			}
			std::fprintf(stderr,
				"[STONE] '%s' groove grid %s: %zu h-grooves, %zu bands, v-grooves/band",
				matName, grid.valid ? "DETECTED" : "rejected", grid.h.size(), grid.bandY.size());
			for (const auto &vb : grid.vPerBand) std::fprintf(stderr, " %zu", vb.size());
			std::fprintf(stderr, " (edge-aligned tessellation %s)\n",
				grid.valid ? "ON" : "off -> fan path");

			// ── per-LINE representative heights (--greets_displace_line_height,
			// default on). Reps are MEDIANs of the height field bilinear-sampled
			// ALONG a map-space line, at exactly the coordinates a vert on the
			// line samples (u = X/mw on a vertical line, v = Y/mh on a horizontal
			// one — SampleHeight8Bilinear's texel-center convention). Line set
			// mirrors the rowTpl/colTpl construction exactly: per groove
			// A=lo-pad / D=hi+pad (shoulders), plus B=lo+pad / C=hi-pad for wide
			// grooves or M=(lo+hi)/2 for narrow ones (floors). ──
			if (grid.valid && lineHeightOn) {
				auto medianOf = [](std::vector<float> &s) -> float {
					const size_t m = s.size() / 2;
					std::nth_element(s.begin(), s.begin() + m, s.end());
					return s[m];
				};
				auto medH = [&](float yTex) -> float {
					std::vector<float> s(static_cast<size_t>(useMipW));
					for (int x = 0; x < useMipW; ++x)
						s[size_t(x)] = SampleHeight8Bilinear(hm, useMip,
							(float(x) + 0.5f) / float(useMipW), yTex / float(useMipH));
					return medianOf(s);
				};
				auto medV = [&](float xTex, float y0, float y1) -> float {
					const int n = std::max(8, int(y1 - y0));
					std::vector<float> s(static_cast<size_t>(n));
					for (int k = 0; k < n; ++k) {
						const float y = y0 + (y1 - y0) * (float(k) + 0.5f) / float(n);
						s[size_t(k)] = SampleHeight8Bilinear(hm, useMip,
							xTex / float(useMipW), y / float(useMipH));
					}
					return medianOf(s);
				};
				// Line reps = MEDIAN along the line itself (the edge path keeps
				// its authored depth profile, variance removed). Plateau refs =
				// median along a line INSET kPlatIns texels beyond the shoulder
				// pads — outside the groove's mip-blur, so it reads the true
				// plateau level (the pad lines themselves sample down-slope on
				// a narrow blurred groove — measured 0.37 vs plateau 0.60 on
				// the repro wall, which is why the fallback pin must NOT use
				// the shoulder-line rep).
				const float kPlatIns = kPadTex + 1.25f;
				for (const StoneGRun &r : grid.h) {
					const float A = r.lo - kPadTex, B = r.lo + kPadTex;
					const float C = r.hi - kPadTex, D = r.hi + kPadTex;
					const float ctr = 0.5f * (r.lo + r.hi);
					const float pLo = medH(r.lo - kPlatIns), pHi = medH(r.hi + kPlatIns);
					const float pMid = 0.5f * (pLo + pHi);
					hLineRep.push_back({A, medH(A), pLo});
					if (C - B >= kMinFloorTex) {
						hLineRep.push_back({B, medH(B), pMid});
						hLineRep.push_back({C, medH(C), pMid});
					} else {
						hLineRep.push_back({ctr, medH(ctr), pMid});
					}
					hLineRep.push_back({D, medH(D), pHi});
				}
				vLineRep.resize(grid.vPerBand.size());
				for (size_t b = 0; b < grid.vPerBand.size(); ++b) {
					const float y0 = grid.bandY[b].first, y1 = grid.bandY[b].second;
					for (const StoneGRun &r : grid.vPerBand[b]) {
						const float A = r.lo - kPadTex, B = r.lo + kPadTex;
						const float C = r.hi - kPadTex, D = r.hi + kPadTex;
						const float ctr = 0.5f * (r.lo + r.hi);
						const float pLo = medV(r.lo - kPlatIns, y0, y1);
						const float pHi = medV(r.hi + kPlatIns, y0, y1);
						const float pMid = 0.5f * (pLo + pHi);
						vLineRep[b].push_back({A, medV(A, y0, y1), pLo});
						if (C - B >= kMinFloorTex) {
							vLineRep[b].push_back({B, medV(B, y0, y1), pMid});
							vLineRep[b].push_back({C, medV(C, y0, y1), pMid});
						} else {
							vLineRep[b].push_back({ctr, medV(ctr, y0, y1), pMid});
						}
						vLineRep[b].push_back({D, medV(D, y0, y1), pHi});
					}
				}
				size_t nvl = 0;
				for (const auto &vb : vLineRep) nvl += vb.size();
				std::fprintf(stderr,
					"[STONE-LINE] '%s' per-line rep heights (median-along-line "
					"+ plateau insets): %zu h-lines, %zu v-lines over %zu bands\n",
					matName, hLineRep.size(), nvl, vLineRep.size());
			}
		}

		Vertex *const oldV = T->Verts;
		const uint32_t nOrig = uint32_t(T->VIndex);
		auto vidx = [&](const Vertex *v) { return uint32_t(v - oldV); };

		// Per-face corner UV for a given original vertex index (per-face UVs are
		// authoritative — per-vertex U/V is clobbered at shared corners).
		auto cornerUV = [&](const Face &F, uint32_t vi, float &u, float &v) {
			if (vi == vidx(F.A)) { u = F.U1; v = F.V1; }
			else if (vi == vidx(F.B)) { u = F.U2; v = F.V2; }
			else { u = F.U3; v = F.V3; }
		};

		// ── original-mesh topology: edge-use over target faces + non-target
		// incidence (the AUTHORED patch boundary — unaffected by subdivision) ──
		std::map<std::pair<uint32_t,uint32_t>, int> origEdgeUse;   // target faces only
		std::vector<char> origNonTargetVert(nOrig, 0);
		for (int32_t i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C) continue;
			const uint32_t a = vidx(F.A), b = vidx(F.B), c = vidx(F.C);
			if (a >= nOrig || b >= nOrig || c >= nOrig) continue;
			if (!isTarget(&F)) { origNonTargetVert[a] = origNonTargetVert[b] = origNonTargetVert[c] = 1; continue; }
			auto add = [&](uint32_t x, uint32_t y){ origEdgeUse[{std::min(x,y),std::max(x,y)}]++; };
			add(a,b); add(b,c); add(c,a);
		}
		// Cross-material SEAM classification (position-coincidence; the sliver-gap
		// fix). A target ORIGINAL vertex coincident with non-displaced geometry is
		// a border; a target EDGE coincident with a non-displaced face edge is a
		// border edge — so the subdivision verts created ALONG it (edgeVert keys on
		// the original corner pair) pin too, closing the mid-span the user saw open.
		// Purely additive to the index/single-target-face test below.
		std::vector<char> coincidentOrig(nOrig, 0);
		if (seamPin && !ndVert.empty())
			for (uint32_t i = 0; i < nOrig; ++i)
				if (ndVert.count(seamKey(oldV[i].Pos))) coincidentOrig[i] = 1;
		auto isSeamBorderEdge = [&](uint32_t x, uint32_t y) -> bool {
			if (!seamPin || ndEdge.empty() || x >= nOrig || y >= nOrig) return false;
			return ndEdge.count(edgeKey(oldV[x].Pos, oldV[y].Pos)) != 0;
		};
		auto isBorderEdge = [&](uint32_t x, uint32_t y) {
			auto it = origEdgeUse.find({std::min(x,y),std::max(x,y)});
			if (it != origEdgeUse.end() && it->second == 1) return true;   // used by exactly one target face
			return isSeamBorderEdge(x, y);                                 // coincident with a non-displaced edge
		};

		// AUDIT (init-time census): the population the position-coincidence
		// border classification adds over the OLD index/single-target-face rule.
		if (seamPin && !ndVert.empty()) {
			// old classification: origNonTargetVert OR endpoint of an OLD border
			// edge (single-target-face); the seam terms are what this fix adds.
			std::vector<char> oldPinned(nOrig, 0);
			for (uint32_t i = 0; i < nOrig; ++i) if (origNonTargetVert[i]) oldPinned[i] = 1;
			for (auto &kv : origEdgeUse)
				if (kv.second == 1) { oldPinned[kv.first.first] = oldPinned[kv.first.second] = 1; }
			std::vector<char> isTargetVert(nOrig, 0);
			for (int32_t i = 0; i < T->FIndex; ++i) {
				const Face &F = T->Faces[i];
				if (!F.A || !F.B || !F.C || !isTarget(&F)) continue;
				const uint32_t a=vidx(F.A), b=vidx(F.B), c=vidx(F.C);
				if (a<nOrig) isTargetVert[a]=1; if (b<nOrig) isTargetVert[b]=1; if (c<nOrig) isTargetVert[c]=1;
			}
			int newlyPinned = 0, coincTotal = 0;
			std::unordered_set<std::string> neighMats;
			for (uint32_t i = 0; i < nOrig; ++i) {
				if (!isTargetVert[i] || !coincidentOrig[i]) continue;
				++coincTotal;
				if (!oldPinned[i]) {
					++newlyPinned;
					auto it = ndVertMat.find(seamKey(oldV[i].Pos));
					if (it != ndVertMat.end() && it->second) neighMats.insert(it->second);
				}
			}
			int seamEdges = 0;
			for (auto &kv : origEdgeUse)
				if (kv.second == 2 && isSeamBorderEdge(kv.first.first, kv.first.second)) ++seamEdges;
			if (coincTotal || seamEdges) {
				std::string nb;
				for (const std::string &m : neighMats) { if (!nb.empty()) nb += ","; nb += m; }
				std::fprintf(stderr,
					"[STONE-SEAM] '%s': %d target verts coincident w/ non-displaced geom; "
					"%d NEWLY pinned (were unpinned = the gap population), %d interior "
					"edges lie on a non-displaced edge (now border-pinned). neighbours: %s\n",
					matName, coincTotal, newlyPinned, seamEdges,
					nb.empty() ? "(none via vert map)" : nb.c_str());
			}
		}

		// ── longest-edge quad pairing (greedy by descending shared-edge length:
		// the diagonal of a quad-from-triangulation is its longest edge) ──
		std::map<std::pair<uint32_t,uint32_t>, std::vector<int32_t>> edgeTargets;
		std::vector<int32_t> targetFaces;
		for (int32_t i = 0; i < T->FIndex; ++i) {
			const Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C || !isTarget(&F)) continue;
			targetFaces.push_back(i);
			const uint32_t a = vidx(F.A), b = vidx(F.B), c = vidx(F.C);
			auto add = [&](uint32_t x, uint32_t y){ edgeTargets[{std::min(x,y),std::max(x,y)}].push_back(i); };
			add(a,b); add(b,c); add(c,a);
		}
		auto edgeLenSq = [&](uint32_t x, uint32_t y) {
			const Vector &p = oldV[x].Pos, &q = oldV[y].Pos;
			return (p.x-q.x)*(p.x-q.x)+(p.y-q.y)*(p.y-q.y)+(p.z-q.z)*(p.z-q.z);
		};
		// candidate diagonals: interior edges (2 target faces), scored by length.
		struct Cand { float len; uint32_t a, b; int32_t f, g; };
		std::vector<Cand> cands;
		for (auto &kv : edgeTargets) {
			if (kv.second.size() != 2) continue;   // border or non-manifold
			cands.push_back({ edgeLenSq(kv.first.first, kv.first.second),
			                  kv.first.first, kv.first.second, kv.second[0], kv.second[1] });
		}
		std::sort(cands.begin(), cands.end(),
		          [](const Cand &x, const Cand &y){ return x.len > y.len; });
		std::map<int32_t,int32_t> quadPartner;   // face -> its quad partner
		std::map<int32_t,std::pair<uint32_t,uint32_t>> quadDiag;
		{
			std::map<int32_t,char> taken;
			// Per-face corner UV helper usable before cornerUV's declaration order
			// (duplicated tiny lambda — keeps the pairing self-contained).
			auto cuv = [&](const Face &F, uint32_t vi, float &u, float &v) {
				if (vi == vidx(F.A)) { u = F.U1; v = F.V1; }
				else if (vi == vidx(F.B)) { u = F.U2; v = F.V2; }
				else { u = F.U3; v = F.V3; }
			};
			for (const Cand &c : cands) {
				if (taken.count(c.f) || taken.count(c.g)) continue;
				// only pair across each face's OWN longest edge (true diagonal)
				const Face &Ff = T->Faces[c.f]; const Face &Fg = T->Faces[c.g];
				auto longest = [&](const Face &F){
					uint32_t a=vidx(F.A),b=vidx(F.B),cc=vidx(F.C);
					float dab=edgeLenSq(a,b),dbc=edgeLenSq(b,cc),dca=edgeLenSq(cc,a);
					return std::max(dab,std::max(dbc,dca));
				};
				const float diag = c.len;
				if (diag < longest(Ff)*0.999f || diag < longest(Fg)*0.999f) continue;
				// UV charts must AGREE along the diagonal (per-face UVs; a seam
				// across the diagonal would smear the quad's bilinear UV grid) —
				// mismatched pairs fall back to two lone triangles.
				float fu0,fv0,fu1,fv1,gu0,gv0,gu1,gv1;
				cuv(Ff,c.a,fu0,fv0); cuv(Ff,c.b,fu1,fv1);
				cuv(Fg,c.a,gu0,gv0); cuv(Fg,c.b,gu1,gv1);
				const float kUvEps = 1e-4f;
				if (std::fabs(fu0-gu0)>kUvEps || std::fabs(fv0-gv0)>kUvEps ||
				    std::fabs(fu1-gu1)>kUvEps || std::fabs(fv1-gv1)>kUvEps) continue;
				// Coplanarity: the roof-ridge fix retriangulates the pair as ONE
				// bilinear cell — only valid when the two triangles share a plane
				// (authored stone quads do; a genuinely creased pair must keep its
				// authored edge). 1° tolerance.
				if (Ff.N.x*Fg.N.x + Ff.N.y*Fg.N.y + Ff.N.z*Fg.N.z < 0.99985f) continue;
				taken[c.f]=1; taken[c.g]=1;
				quadPartner[c.f]=c.g; quadPartner[c.g]=c.f;
				quadDiag[c.f]={c.a,c.b}; quadDiag[c.g]={c.a,c.b};
			}
		}

		// ── build the subdivided mesh ──
		std::vector<Vertex> verts(oldV, oldV + nOrig);   // originals kept in place
		std::vector<char>   pinnedZero(nOrig, 0);        // authored-border verts
		for (uint32_t i = 0; i < nOrig; ++i)
			if (origNonTargetVert[i] || coincidentOrig[i]) pinnedZero[i] = 1;
		// canonical shared edge vertex (keyed by min-corner, max-corner, param bits)
		std::map<std::array<uint32_t,3>, uint32_t> edgeVertMap;
		auto edgeVert = [&](uint32_t ia, uint32_t ib, float t) -> uint32_t {
			uint32_t lo = ia, hi = ib; float p = t;
			if (ia > ib) { lo = ib; hi = ia; p = 1.0f - t; }
			std::array<uint32_t,3> key{ lo, hi, meshF2bits(p) };
			auto it = edgeVertMap.find(key); if (it != edgeVertMap.end()) return it->second;
			Vertex m = oldV[lo];
			const Vector &A = oldV[lo].Pos, &B = oldV[hi].Pos;
			m.Pos.x = meshLerpf(A.x,B.x,p); m.Pos.y = meshLerpf(A.y,B.y,p); m.Pos.z = meshLerpf(A.z,B.z,p);
			const Vector &NA = oldV[lo].N, &NB = oldV[hi].N;
			float nx = meshLerpf(NA.x,NB.x,p), ny = meshLerpf(NA.y,NB.y,p), nz = meshLerpf(NA.z,NB.z,p);
			const float nl = std::sqrt(nx*nx+ny*ny+nz*nz);
			if (nl > 1e-6f) { m.N.x = nx/nl; m.N.y = ny/nl; m.N.z = nz/nl; }
			const uint32_t id = uint32_t(verts.size()); verts.push_back(m);
			edgeVertMap[key] = id;
			if (isBorderEdge(lo, hi)) { if (id >= pinnedZero.size()) pinnedZero.resize(id+1,0); pinnedZero[id] = 1; }
			return id;
		};
		auto newInterior = [&](const Vector &pos, const Vector &nrm) -> uint32_t {
			Vertex m = oldV[0]; m.Pos = pos;
			float nx=nrm.x,ny=nrm.y,nz=nrm.z; const float nl=std::sqrt(nx*nx+ny*ny+nz*nz);
			if (nl>1e-6f){ m.N.x=nx/nl; m.N.y=ny/nl; m.N.z=nz/nl; }
			const uint32_t id=uint32_t(verts.size()); verts.push_back(m);
			if (id >= pinnedZero.size()) pinnedZero.resize(id+1,0);
			return id;
		};

		std::vector<Face> faces;
		std::vector<std::array<uint32_t,3>> fIdx;
		std::vector<char> faceFromEdge;      // emitted by the edge-aligned path?
		bool curPatchEdge = false;           // set around edgeAlignedQuad emission
		faces.reserve(size_t(T->FIndex) * 4);
		fIdx.reserve(faces.capacity());
		faceFromEdge.reserve(faces.capacity());
		auto emit = [&](const Face &proto, uint32_t i0, uint32_t i1, uint32_t i2,
		                float u0,float v0,float u1,float v1,float u2,float v2){
			Face f = proto; f.frame = nullptr;
			f.U1=u0;f.V1=v0;f.U2=u1;f.V2=v1;f.U3=u2;f.V3=v2;
			f.EU1=u0;f.EV1=v0;f.EU2=u1;f.EV2=v1;f.EU3=u2;f.EV3=v2;
			faces.push_back(f); fIdx.push_back({i0,i1,i2});
			faceFromEdge.push_back(curPatchEdge ? 1 : 0);
		};

		// Smallest level whose refinement error passes (see kRefineEps above),
		// bounded by the per-quad BLOCK-PITCH cap. Evaluated over the patch's UV
		// bounding box (stone quads map affinely, so the bbox ≈ the true
		// footprint). metricVals records each patch's bad-cell fraction at the
		// CHOSEN level for the FDS_SUBDIV_DIAG distribution print; lcapHist below
		// records the depth ceiling the block pitch imposed.
		std::vector<float> metricVals;
		int lcapHist[kMaxLevel + 1] = {0};
		auto refineLevel = [&](float u0,float u1,float v0,float v1)->int{
			// Block-pitch cap: enough levels that the quad's texel footprint is
			// cut into ≤ half-block cells (targetTexX/Y). This is the map-relative
			// depth that makes per-block relief representable however many tiles
			// the quad spans — the fix for "cells span 2-3 blocks". Never exceeds
			// kMaxLevel (the runaway safety cap).
			const float footTexU = std::fabs(u1-u0) * float(useMipW);
			const float footTexV = std::fabs(v1-v0) * float(useMipH);
			const float need = std::max(footTexU/targetTexX, footTexV/targetTexY);
			int Lcap = 0;
			while ((1<<Lcap) < need && Lcap < kMaxLevel) ++Lcap;
			lcapHist[Lcap]++;
			if (uniformLevel > 0) { metricVals.push_back(-1.0f); return std::min(uniformLevel, kMaxLevel); }
			// HONEST cost/benefit: at each candidate level, a cell is "bad" when
			// its bilinear-of-4-corners deviates from the true height by more than
			// kRefineEps ANYWHERE in the cell's texel footprint. We scan that
			// footprint on a strided grid (≥ every ~2 texels, capped at 12×12/cell)
			// instead of a fixed 9-point stencil — the stencil aliased on coarse
			// cells (probes fell on similar-height block interiors, mortar missed),
			// which is exactly why coarse cells used to read "matched" while
			// carrying no relief. Climb 0..Lcap; first level that passes wins,
			// else settle at Lcap (block resolution — the residual POM carries the
			// sub-cell sharpness by construction).
			float fracAt[kMaxLevel + 1];
			for (int L = 0; L <= kMaxLevel; ++L) fracAt[L] = 1.0f;
			int converged = -1;
			for (int L=0; L<=Lcap; ++L) {
				const int n = 1<<L;
				const float cellTexU = footTexU/float(n), cellTexV = footTexV/float(n);
				// probe samples per axis: ~one per 2 texels, in [2,12].
				int su = int(cellTexU*0.5f + 0.999f); if (su<2) su=2; if (su>12) su=12;
				int sv = int(cellTexV*0.5f + 0.999f); if (sv<2) sv=2; if (sv>12) sv=12;
				int bad = 0;
				for (int j=0;j<n;++j) for (int i=0;i<n;++i) {
					const float cs0=float(i)/n, cs1=float(i+1)/n;
					const float ct0=float(j)/n, ct1=float(j+1)/n;
					auto uvAt=[&](float s,float t,float&u,float&v){
						u=u0+(u1-u0)*s; v=v0+(v1-v0)*t; };
					float u,v;
					uvAt(cs0,ct0,u,v); const float c00=SampleHeight8Bilinear(hm,useMip,u,v);
					uvAt(cs1,ct0,u,v); const float c10=SampleHeight8Bilinear(hm,useMip,u,v);
					uvAt(cs0,ct1,u,v); const float c01=SampleHeight8Bilinear(hm,useMip,u,v);
					uvAt(cs1,ct1,u,v); const float c11=SampleHeight8Bilinear(hm,useMip,u,v);
					float err = 0.0f;
					for (int pj=0; pj<sv; ++pj) for (int pi=0; pi<su; ++pi) {
						const float ps=(su>1)?float(pi)/float(su-1):0.5f;
						const float pt=(sv>1)?float(pj)/float(sv-1):0.5f;
						const float s=cs0+(cs1-cs0)*ps, t=ct0+(ct1-ct0)*pt;
						uvAt(s,t,u,v);
						const float h=SampleHeight8Bilinear(hm,useMip,u,v);
						const float bil=(c00*(1-ps)+c10*ps)*(1-pt)
						               +(c01*(1-ps)+c11*ps)*pt;
						err=std::max(err,std::fabs(h-bil));
					}
					if (err > kRefineEps) ++bad;
				}
				fracAt[L] = float(bad)/float(n*n);
				if (fracAt[L] <= kRefineFrac) { converged = L; break; }
			}
			const int chosen = (converged >= 0) ? converged : Lcap;
			metricVals.push_back(fracAt[chosen]);
			return chosen;
		};
		auto busyLevel = refineLevel;   // call-site name

		// Ordered side registry for cross-patch crack pinning, generalized from
		// the old per-LEVEL form to an arbitrary PARAM LIST per side (the edge-
		// aligned path places groove-line verts at arbitrary params, not i/2^L).
		// Params are stored CANONICALLY: measured from the side's min-corner,
		// interior only (0/1 excluded), sorted ascending, and produced by the
		// IDENTICAL float expression edgeVert canonicalizes with (`1.0f - t` when
		// flipped) so a pin lookup hits the exact created vertex key. Two records
		// with differing lists are healed after displacement: the non-anchor
		// side's extra verts are snapped onto the anchor's displaced polyline
		// (anchor = fewer params = the coarser side; deterministic tie-break).
		struct SideRec { std::vector<float> params; };
		std::map<std::pair<uint32_t,uint32_t>, std::vector<SideRec>> sideReg;
		auto regSideP = [&](uint32_t ca, uint32_t cb, const std::vector<float> &fromCa){
			if (isBorderEdge(ca,cb)) return;   // authored border: pinned to zero, no transition
			SideRec r;
			r.params.reserve(fromCa.size());
			for (float t : fromCa)
				if (t > 1e-6f && t < 1.0f - 1e-6f)
					r.params.push_back(ca > cb ? 1.0f - t : t);
			std::sort(r.params.begin(), r.params.end());
			sideReg[{std::min(ca,cb),std::max(ca,cb)}].push_back(std::move(r));
		};
		auto regSide = [&](uint32_t ca, uint32_t cb, int level){
			std::vector<float> p;
			const int n = 1 << level;
			p.reserve(size_t(n));
			for (int i = 1; i < n; ++i) p.push_back(float(i)/float(n));
			regSideP(ca, cb, p);
		};

		int builtQuads = 0, builtLones = 0, fanCells = 0, flatCells = 0;
		int lvlHist[kMaxLevel + 1] = {0};

		// Per-quad (consume both faces) then per remaining lone triangle, emit a
		// symmetric subdivision into `faces`. Non-target faces copied verbatim by
		// the loop after.
		std::vector<char> consumed(T->FIndex, 0);
		auto uvBil = [](const float cu[4], const float cv[4], float s, float t,
		                float &u, float &v){
			const float w0=(1-s)*(1-t), w1=s*(1-t), w2=s*t, w3=(1-s)*t;
			u = cu[0]*w0+cu[1]*w1+cu[2]*w2+cu[3]*w3;
			v = cv[0]*w0+cv[1]*w1+cv[2]*w2+cv[3]*w3;
		};
		auto posBil = [&](const uint32_t cn[4], float s, float t)->Vector{
			const float w0=(1-s)*(1-t), w1=s*(1-t), w2=s*t, w3=(1-s)*t;
			const Vector &A=oldV[cn[0]].Pos,&B=oldV[cn[1]].Pos,&C=oldV[cn[2]].Pos,&D=oldV[cn[3]].Pos;
			return Vector{ A.x*w0+B.x*w1+C.x*w2+D.x*w3,
			               A.y*w0+B.y*w1+C.y*w2+D.y*w3,
			               A.z*w0+B.z*w1+C.z*w2+D.z*w3 };
		};
		auto nrmBil = [&](const uint32_t cn[4], float s, float t)->Vector{
			const float w0=(1-s)*(1-t), w1=s*(1-t), w2=s*t, w3=(1-s)*t;
			const Vector &A=oldV[cn[0]].N,&B=oldV[cn[1]].N,&C=oldV[cn[2]].N,&D=oldV[cn[3]].N;
			return Vector{ A.x*w0+B.x*w1+C.x*w2+D.x*w3,
			               A.y*w0+B.y*w1+C.y*w2+D.y*w3,
			               A.z*w0+B.z*w1+C.z*w2+D.z*w3 };
		};

		// ── edge-aligned quad emitter (the flat-top fix). Returns false when
		// this quad can't take the groove-aligned path — non-parallelogram or
		// rotated/skewed UV chart, degenerate mapping, runaway line count — and
		// the caller falls back to the adaptive fan grid. Layout: ROWS between
		// the horizontal groove lines (instanced across the quad's v footprint);
		// each row's COLUMN breaks come from its band's vertical grooves — step
		// rows carry their adjacent band's breaks so vertical walls stay sharp
		// through block corners; floor rows take both bands' union (their cells
		// all sit at groove depth, so the extra verts stay flat). Each internal
		// line's vertex set is the UNION of the two adjacent rows' break sets
		// and rows are triangulated by a two-pointer march between their two
		// lines: crack-free by construction, no interior T-junctions. Line
		// params are quantized to a 1/2048 lattice so two quads sharing a side
		// derive bit-identical edgeVert keys from the same map-space groove line.
		int edgeQuads = 0, edgePlateauC = 0, edgeStepC = 0, edgeFloorC = 0;
		auto edgeAlignedQuad = [&](const uint32_t cn[4], const float cu[4],
		                           const float cv[4], const Face &Ff) -> bool {
			// parallelogram + axis-alignment (up to u/v swap and sign flips)
			const float us=cu[1]-cu[0], vs=cv[1]-cv[0];
			const float ut=cu[3]-cu[0], vt=cv[3]-cv[0];
			const float scale = std::fabs(us)+std::fabs(vs)+std::fabs(ut)+std::fabs(vt);
			if (scale < 1e-6f) return false;
			if (std::fabs(cu[0]+us+ut-cu[2]) + std::fabs(cv[0]+vs+vt-cv[2]) > 0.002f*scale)
				return false;                            // not a parallelogram in UV
			const float tol = 0.005f;
			bool swapAxes;
			if (std::fabs(vs) <= tol*std::fabs(us) && std::fabs(ut) <= tol*std::fabs(vt))
				swapAxes = false;                        // s carries u, t carries v
			else if (std::fabs(us) <= tol*std::fabs(vs) && std::fabs(vt) <= tol*std::fabs(ut))
				swapAxes = true;                         // s carries v, t carries u
			else return false;                           // rotated/skewed chart
			const float duP = swapAxes ? ut : us;        // ∂u/∂(param carrying u)
			const float dvP = swapAxes ? vs : vt;        // ∂v/∂(param carrying v)
			if (std::fabs(duP) < 1e-7f || std::fabs(dvP) < 1e-7f) return false;
			auto qz  = [](float p){ return std::floor(p*2048.0f + 0.5f)*(1.0f/2048.0f); };
			auto puOf= [&](float u){ return (u - cu[0]) / duP; };
			auto pvOf= [&](float v){ return (v - cv[0]) / dvP; };
			auto uOf = [&](float pu){ return cu[0] + duP*pu; };
			auto vOf = [&](float pv){ return cv[0] + dvP*pv; };

			// rows: instance the per-period row template across the v footprint
			const float vRLo = std::min(vOf(0.0f), vOf(1.0f));
			const float vRHi = std::max(vOf(0.0f), vOf(1.0f));
			struct Row { float pv0, pv1; int type, bandA, bandB; };
			std::vector<Row> rows;
			{
				struct Seg { float v0, v1; int type, bandA, bandB; };
				std::vector<Seg> segs;
				const float mhF = float(useMipH);
				const int k0 = int(std::floor(vRLo)) - 1, k1 = int(std::ceil(vRHi)) + 1;
				for (int k = k0; k <= k1; ++k)
					for (const StoneRowT &r : rowTpl) {
						const float a = float(k) + r.y0/mhF, b = float(k) + r.y1/mhF;
						if (b <= vRLo + 1e-6f || a >= vRHi - 1e-6f) continue;
						segs.push_back({std::max(a, vRLo), std::min(b, vRHi),
						                r.type, r.bandA, r.bandB});
					}
				if (segs.empty() || segs.size() > 220) return false;
				std::sort(segs.begin(), segs.end(),
				          [](const Seg &x, const Seg &y){ return x.v0 < y.v0; });
				for (const Seg &sg : segs) {
					float a = pvOf(sg.v0), b = pvOf(sg.v1);
					if (a > b) std::swap(a, b);
					a = std::max(0.0f, qz(a)); b = std::min(1.0f, qz(b));
					if (b - a < 2e-4f) continue;         // sub-lattice sliver row
					rows.push_back({a, b, sg.type, sg.bandA, sg.bandB});
				}
				if (dvP < 0.0f) std::reverse(rows.begin(), rows.end());
				if (rows.empty()) return false;
				rows.front().pv0 = 0.0f; rows.back().pv1 = 1.0f;
			}

			// per-row pu break lists from the band column templates
			const float uRLo = std::min(uOf(0.0f), uOf(1.0f));
			const float uRHi = std::max(uOf(0.0f), uOf(1.0f));
			auto rowBreaks = [&](const Row &r) -> std::vector<float> {
				std::vector<float> b;
				const float mwF = float(useMipW);
				auto addBand = [&](int band){
					if (band < 0 || band >= int(colTpl.size())) return;
					const int k0 = int(std::floor(uRLo)) - 1, k1 = int(std::ceil(uRHi)) + 1;
					for (int k = k0; k <= k1; ++k)
						for (float x : colTpl[band]) {
							const float u = float(k) + x/mwF;
							if (u <= uRLo + 1e-6f || u >= uRHi - 1e-6f) continue;
							const float p = qz(puOf(u));
							if (p > 1e-6f && p < 1.0f - 1e-6f) b.push_back(p);
						}
				};
				addBand(r.bandA);
				if (r.bandB != r.bandA) addBand(r.bandB);
				b.push_back(0.0f); b.push_back(1.0f);
				std::sort(b.begin(), b.end());
				b.erase(std::unique(b.begin(), b.end(),
				        [](float x, float y){ return std::fabs(x-y) < 1e-5f; }), b.end());
				return b;
			};
			const size_t nR = rows.size();
			std::vector<std::vector<float>> rowPu(nR);
			size_t maxB = 0;
			for (size_t i = 0; i < nR; ++i) {
				rowPu[i] = rowBreaks(rows[i]);
				maxB = std::max(maxB, rowPu[i].size());
			}
			if (maxB > 300) return false;                // runaway safety → fallback

			// lines: line i at rows[i].pv0 (plus the final top line at 1);
			// vertex set = union of the adjacent rows' break sets.
			std::vector<float> linePv(nR + 1);
			std::vector<std::vector<float>> linePu(nR + 1);
			for (size_t i = 0; i < nR; ++i) linePv[i] = rows[i].pv0;
			linePv[nR] = rows[nR-1].pv1;
			for (size_t i = 0; i <= nR; ++i) {
				std::vector<float> m;
				if (i > 0)  m.insert(m.end(), rowPu[i-1].begin(), rowPu[i-1].end());
				if (i < nR) m.insert(m.end(), rowPu[i].begin(),  rowPu[i].end());
				std::sort(m.begin(), m.end());
				m.erase(std::unique(m.begin(), m.end(),
				        [](float x, float y){ return std::fabs(x-y) < 1e-5f; }), m.end());
				linePu[i] = std::move(m);
			}

			// vertices per line (shared by the rows on both sides)
			auto stOf = [&](float pu, float pv, float &s, float &t){
				if (swapAxes) { s = pv; t = pu; } else { s = pu; t = pv; } };
			auto vidAt = [&](float pu, float pv) -> uint32_t {
				float s, t; stOf(pu, pv, s, t);
				const bool sIs0=(s<=1e-6f), sIs1=(s>=1.0f-1e-6f);
				const bool tIs0=(t<=1e-6f), tIs1=(t>=1.0f-1e-6f);
				if (sIs0&&tIs0) return cn[0];
				if (sIs1&&tIs0) return cn[1];
				if (sIs1&&tIs1) return cn[2];
				if (sIs0&&tIs1) return cn[3];
				if (tIs0) return edgeVert(cn[0],cn[1], s);
				if (tIs1) return edgeVert(cn[3],cn[2], s);
				if (sIs0) return edgeVert(cn[0],cn[3], t);
				if (sIs1) return edgeVert(cn[1],cn[2], t);
				return newInterior(posBil(cn,s,t), nrmBil(cn,s,t));
			};
			std::vector<std::vector<uint32_t>> lineVid(nR + 1);
			for (size_t i = 0; i <= nR; ++i) {
				lineVid[i].reserve(linePu[i].size());
				for (float pu : linePu[i]) lineVid[i].push_back(vidAt(pu, linePv[i]));
			}

			// emit: two-pointer march between each row's bottom and top line
			auto triST = [&](uint32_t va, float sa, float ta,
			                 uint32_t vb, float sb, float tb,
			                 uint32_t vc, float sc, float tc){
				if ((sb-sa)*(tc-ta) - (tb-ta)*(sc-sa) < 0.0f) {   // keep quad winding
					std::swap(vb, vc); std::swap(sb, sc); std::swap(tb, tc);
				}
				float ua,vaU,ub,vbU,uc,vcU;
				uvBil(cu,cv,sa,ta,ua,vaU); uvBil(cu,cv,sb,tb,ub,vbU); uvBil(cu,cv,sc,tc,uc,vcU);
				emit(Ff, va, vb, vc, ua,vaU, ub,vbU, uc,vcU);
			};
			for (size_t r = 0; r < nR; ++r) {
				const std::vector<float> &A = linePu[r], &B = linePu[r+1];
				const std::vector<uint32_t> &VA = lineVid[r], &VB = lineVid[r+1];
				const float pva = linePv[r], pvb = linePv[r+1];
				size_t ia = 0, ib = 0;
				int cellsHere = 0;
				while (ia + 1 < A.size() || ib + 1 < B.size()) {
					const bool cA = ia + 1 < A.size(), cB = ib + 1 < B.size();
					const float nA = cA ? A[ia+1] : 2.0f, nB = cB ? B[ib+1] : 2.0f;
					float s0,t0,s1,t1,s2,t2,s3,t3;
					if (cA && cB && std::fabs(nA - nB) < 1e-5f) {
						// matched rectangle: the diagonal follows the height field
						// so a lone off-level corner is isolated on one flat + one
						// sloped triangle (block corners) instead of two sloped.
						const float h00 = SampleHeight8Bilinear(hm, useMip, uOf(A[ia]), vOf(pva));
						const float h10 = SampleHeight8Bilinear(hm, useMip, uOf(nA),    vOf(pva));
						const float h11 = SampleHeight8Bilinear(hm, useMip, uOf(nB),    vOf(pvb));
						const float h01 = SampleHeight8Bilinear(hm, useMip, uOf(B[ib]), vOf(pvb));
						const float hc  = SampleHeight8Bilinear(hm, useMip,
							uOf(0.5f*(A[ia]+nA)), vOf(0.5f*(pva+pvb)));
						const bool useAC = std::fabs(0.5f*(h00+h11) - hc)
						                <= std::fabs(0.5f*(h10+h01) - hc);
						stOf(A[ia],pva,s0,t0); stOf(nA,pva,s1,t1);
						stOf(nB,pvb,s2,t2);    stOf(B[ib],pvb,s3,t3);
						if (useAC) {
							triST(VA[ia],s0,t0, VA[ia+1],s1,t1, VB[ib+1],s2,t2);
							triST(VA[ia],s0,t0, VB[ib+1],s2,t2, VB[ib],s3,t3);
						} else {
							triST(VA[ia],s0,t0, VA[ia+1],s1,t1, VB[ib],s3,t3);
							triST(VA[ia+1],s1,t1, VB[ib+1],s2,t2, VB[ib],s3,t3);
						}
						++ia; ++ib;
					} else if (nA < nB) {
						stOf(A[ia],pva,s0,t0); stOf(nA,pva,s1,t1); stOf(B[ib],pvb,s2,t2);
						triST(VA[ia],s0,t0, VA[ia+1],s1,t1, VB[ib],s2,t2);
						++ia;
					} else {
						stOf(A[ia],pva,s0,t0); stOf(nB,pvb,s1,t1); stOf(B[ib],pvb,s2,t2);
						triST(VA[ia],s0,t0, VB[ib+1],s1,t1, VB[ib],s2,t2);
						++ib;
					}
					++cellsHere;
				}
				if      (rows[r].type == 0) edgePlateauC += cellsHere;
				else if (rows[r].type == 1) edgeStepC    += cellsHere;
				else                        edgeFloorC   += cellsHere;
			}

			// side registration for the cross-patch pin (params along each side)
			{
				std::vector<float> pvL(linePv.begin(), linePv.end());
				if (!swapAxes) {
					regSideP(cn[0], cn[1], linePu[0]);
					regSideP(cn[3], cn[2], linePu[nR]);
					regSideP(cn[0], cn[3], pvL);
					regSideP(cn[1], cn[2], pvL);
				} else {
					regSideP(cn[0], cn[1], pvL);
					regSideP(cn[3], cn[2], pvL);
					regSideP(cn[0], cn[3], linePu[0]);
					regSideP(cn[1], cn[2], linePu[nR]);
				}
			}
			return true;
		};

		for (auto &kv : quadPartner) {
			const int32_t f = kv.first, g = kv.second;
			if (f > g) continue;              // once per quad
			if (consumed[f] || consumed[g]) continue;
			consumed[f] = consumed[g] = 1;
			const Face &Ff = T->Faces[f], &Fg = T->Faces[g];
			const uint32_t d0 = quadDiag[f].first, d1 = quadDiag[f].second;
			auto apexOf = [&](const Face &F)->uint32_t{
				uint32_t a=vidx(F.A),b=vidx(F.B),c=vidx(F.C);
				if (a!=d0&&a!=d1) return a; if (b!=d0&&b!=d1) return b; return c; };
			const uint32_t e1 = apexOf(Ff), e2 = apexOf(Fg);
			// cyclic corners C0=d0, C1=e1, C2=d1, C3=e2
			uint32_t cn[4] = { d0, e1, d1, e2 };
			float cu[4], cv[4];
			cornerUV(Ff, d0, cu[0], cv[0]); cornerUV(Ff, e1, cu[1], cv[1]);
			cornerUV(Ff, d1, cu[2], cv[2]); cornerUV(Fg, e2, cu[3], cv[3]);
			// Make the cyclic order wind like Ff's VERTEX ORDER so emitted cells
			// inherit the authored winding (the commit pass sign-aligns N to the
			// proto, but the raster path also cares about vertex winding; compare
			// against Ff's own A→B→C cross, not its stored N, in case they ever
			// disagree).
			{
				auto windOf = [&](const Vector &A, const Vector &B, const Vector &C,
				                  float &wx, float &wy, float &wz) {
					const float e1x=B.x-A.x,e1y=B.y-A.y,e1z=B.z-A.z;
					const float e2x=C.x-A.x,e2y=C.y-A.y,e2z=C.z-A.z;
					wx=e1y*e2z-e1z*e2y; wy=e1z*e2x-e1x*e2z; wz=e1x*e2y-e1y*e2x;
				};
				float fwx,fwy,fwz, qwx,qwy,qwz;
				windOf(Ff.A->Pos, Ff.B->Pos, Ff.C->Pos, fwx,fwy,fwz);
				windOf(oldV[cn[0]].Pos, oldV[cn[1]].Pos, oldV[cn[2]].Pos, qwx,qwy,qwz);
				if (fwx*qwx + fwy*qwy + fwz*qwz < 0.0f) {
					std::swap(cn[1], cn[3]);
					std::swap(cu[1], cu[3]); std::swap(cv[1], cv[3]);
				}
			}
			// Edge-aligned tessellation first (the flat-top fix): cells snapped
			// onto the detected groove lines, flat plateaus, narrow step bands.
			// Falls through to the adaptive fan grid when the quad's UV chart
			// can't take it (rotated/skewed) or no grid was detected.
			curPatchEdge = true;
			if (grid.valid && edgeAlignedQuad(cn, cu, cv, Ff)) {
				curPatchEdge = false;
				++builtQuads; ++edgeQuads;
				continue;
			}
			curPatchEdge = false;
			// UV footprint → level
			float u0=std::min(std::min(cu[0],cu[1]),std::min(cu[2],cu[3]));
			float u1=std::max(std::max(cu[0],cu[1]),std::max(cu[2],cu[3]));
			float v0=std::min(std::min(cv[0],cv[1]),std::min(cv[2],cv[3]));
			float v1=std::max(std::max(cv[0],cv[1]),std::max(cv[2],cv[3]));
			const int L = busyLevel(u0,u1,v0,v1);
			lvlHist[L]++; ++builtQuads;
			const int n = 1 << L;
			// register 4 sides
			regSide(cn[0],cn[1],L); regSide(cn[1],cn[2],L);
			regSide(cn[2],cn[3],L); regSide(cn[3],cn[0],L);
			// grid vertex ids
			std::vector<uint32_t> gid(size_t(n+1)*(n+1));
			auto GID=[&](int i,int j)->uint32_t&{ return gid[size_t(j)*(n+1)+i]; };
			for (int j=0;j<=n;++j) for (int i=0;i<=n;++i){
				const bool bi = (i==0||i==n), bj = (j==0||j==n);
				if (bi&&bj){ // corner
					GID(i,j)= (i==0&&j==0)?cn[0] : (i==n&&j==0)?cn[1] : (i==n&&j==n)?cn[2] : cn[3];
				} else if (j==0){ GID(i,j)=edgeVert(cn[0],cn[1], float(i)/n); }
				else if (j==n){ GID(i,j)=edgeVert(cn[3],cn[2], float(i)/n); }
				else if (i==0){ GID(i,j)=edgeVert(cn[0],cn[3], float(j)/n); }
				else if (i==n){ GID(i,j)=edgeVert(cn[1],cn[2], float(j)/n); }
				else { GID(i,j)=newInterior(posBil(cn,float(i)/n,float(j)/n), nrmBil(cn,float(i)/n,float(j)/n)); }
			}
			// cells
			for (int j=0;j<n;++j) for (int i=0;i<n;++i){
				const float s0=float(i)/n, s1=float(i+1)/n, tt0=float(j)/n, tt1=float(j+1)/n;
				const uint32_t a=GID(i,j), b=GID(i+1,j), c=GID(i+1,j+1), d=GID(i,j+1);
				float uu[4],vv[4];
				uvBil(cu,cv,s0,tt0,uu[0],vv[0]); uvBil(cu,cv,s1,tt0,uu[1],vv[1]);
				uvBil(cu,cv,s1,tt1,uu[2],vv[2]); uvBil(cu,cv,s0,tt1,uu[3],vv[3]);
				float sc=(s0+s1)*0.5f, tc=(tt0+tt1)*0.5f, ucen,vcen; uvBil(cu,cv,sc,tc,ucen,vcen);
				// Cell triangulation by what the height field DOES inside the cell.
				// The ridge error of a 2-triangle split is the centre's deviation
				// from the corner plane: dome = |hc − mean(corners)|. Only cells
				// that genuinely dome get the 4-triangle centre fan (peak lands on
				// a VERTEX); monotone-slope cells keep 2 triangles whose diagonal
				// FOLLOWS the field (the diagonal whose midpoint height is nearer
				// hc — orientation varies with content, so no uniform grain);
				// no-relief cells split on the shortest diagonal.
				const float h0=SampleHeight8Bilinear(hm,useMip,uu[0],vv[0]);
				const float h1=SampleHeight8Bilinear(hm,useMip,uu[1],vv[1]);
				const float h2=SampleHeight8Bilinear(hm,useMip,uu[2],vv[2]);
				const float h3=SampleHeight8Bilinear(hm,useMip,uu[3],vv[3]);
				const float hc=SampleHeight8Bilinear(hm,useMip,ucen,vcen);
				const float hlo=std::min(std::min(h0,h1),std::min(std::min(h2,h3),hc));
				const float hhi=std::max(std::max(h0,h1),std::max(std::max(h2,h3),hc));
				const float domeErr = std::fabs(hc - 0.25f*(h0+h1+h2+h3));
				if (hhi-hlo >= kCellFlatEps && domeErr >= kCellDomeEps) {
					// domes/dips: 4-triangle centre fan (apex on the centre vertex)
					++fanCells;
					const uint32_t cc = newInterior(posBil(cn,sc,tc), nrmBil(cn,sc,tc));
					emit(Ff,a,b,cc, uu[0],vv[0],uu[1],vv[1],ucen,vcen);
					emit(Ff,b,c,cc, uu[1],vv[1],uu[2],vv[2],ucen,vcen);
					emit(Ff,c,d,cc, uu[2],vv[2],uu[3],vv[3],ucen,vcen);
					emit(Ff,d,a,cc, uu[3],vv[3],uu[0],vv[0],ucen,vcen);
				} else {
					++flatCells;
					bool useAC;
					if (hhi-hlo < kCellFlatEps) {
						// no relief: shortest diagonal from the CURRENT (subdivided)
						// positions (a/b/c/d may be new verts — never index oldV).
						auto d2v=[&](uint32_t x,uint32_t y){ const Vector&p=verts[x].Pos,&q=verts[y].Pos;
							return (p.x-q.x)*(p.x-q.x)+(p.y-q.y)*(p.y-q.y)+(p.z-q.z)*(p.z-q.z); };
						useAC = d2v(a,c) <= d2v(b,d);
					} else {
						// slope: the diagonal that better carries the field through
						// the centre (its height midpoint nearer the true centre h).
						useAC = std::fabs(0.5f*(h0+h2)-hc) <= std::fabs(0.5f*(h1+h3)-hc);
					}
					if (useAC) {
						emit(Ff,a,b,c, uu[0],vv[0],uu[1],vv[1],uu[2],vv[2]);
						emit(Ff,a,c,d, uu[0],vv[0],uu[2],vv[2],uu[3],vv[3]);
					} else {
						emit(Ff,a,b,d, uu[0],vv[0],uu[1],vv[1],uu[3],vv[3]);
						emit(Ff,b,c,d, uu[1],vv[1],uu[2],vv[2],uu[3],vv[3]);
					}
				}
			}
		}

		// Lone triangles — symmetric triangular subdivision (no diagonal bias).
		for (int32_t fi : targetFaces) {
			if (consumed[fi]) continue;
			consumed[fi] = 1; ++builtLones;
			const Face &F = T->Faces[fi];
			const uint32_t c0=vidx(F.A), c1=vidx(F.B), c2=vidx(F.C);
			float cu[3],cv[3]; cornerUV(F,c0,cu[0],cv[0]); cornerUV(F,c1,cu[1],cv[1]); cornerUV(F,c2,cu[2],cv[2]);
			const float u0=std::min(cu[0],std::min(cu[1],cu[2])), u1=std::max(cu[0],std::max(cu[1],cu[2]));
			const float v0=std::min(cv[0],std::min(cv[1],cv[2])), v1=std::max(cv[0],std::max(cv[1],cv[2]));
			const int L = busyLevel(u0,u1,v0,v1); lvlHist[L]++;
			const int n = 1<<L;
			regSide(c0,c1,L); regSide(c1,c2,L); regSide(c2,c0,L);
			// barycentric grid ids: (i,j), i+j<=n. weight0=(n-i-j)/n on c0,
			// weight1=i/n on c1, weight2=j/n on c2.
			auto pos3=[&](int i,int j)->Vector{ const float w0=float(n-i-j)/n,w1=float(i)/n,w2=float(j)/n;
				const Vector&A=oldV[c0].Pos,&B=oldV[c1].Pos,&C=oldV[c2].Pos;
				return Vector{A.x*w0+B.x*w1+C.x*w2,A.y*w0+B.y*w1+C.y*w2,A.z*w0+B.z*w1+C.z*w2}; };
			auto nrm3=[&](int i,int j)->Vector{ const float w0=float(n-i-j)/n,w1=float(i)/n,w2=float(j)/n;
				const Vector&A=oldV[c0].N,&B=oldV[c1].N,&C=oldV[c2].N;
				return Vector{A.x*w0+B.x*w1+C.x*w2,A.y*w0+B.y*w1+C.y*w2,A.z*w0+B.z*w1+C.z*w2}; };
			auto uv3=[&](int i,int j,float&u,float&v){ const float w0=float(n-i-j)/n,w1=float(i)/n,w2=float(j)/n;
				u=cu[0]*w0+cu[1]*w1+cu[2]*w2; v=cv[0]*w0+cv[1]*w1+cv[2]*w2; };
			std::vector<uint32_t> gid(size_t(n+1)*(n+1), UINT32_MAX);
			auto GID=[&](int i,int j)->uint32_t&{ return gid[size_t(j)*(n+1)+i]; };
			for (int j=0;j<=n;++j) for (int i=0;i+j<=n;++i){
				const bool onC0C1=(j==0), onC1C2=(i+j==n), onC2C0=(i==0);
				const bool isC0=(i==0&&j==0), isC1=(i==n&&j==0), isC2=(i==0&&j==n);
				if (isC0) GID(i,j)=c0; else if (isC1) GID(i,j)=c1; else if (isC2) GID(i,j)=c2;
				else if (onC0C1) GID(i,j)=edgeVert(c0,c1,float(i)/n);
				else if (onC1C2) GID(i,j)=edgeVert(c1,c2,float(j)/n);
				else if (onC2C0) GID(i,j)=edgeVert(c0,c2,float(j)/n);
				else GID(i,j)=newInterior(pos3(i,j), nrm3(i,j));
			}
			auto emit3=[&](uint32_t A,uint32_t B,uint32_t C,int ia,int ja,int ib,int jb,int ic,int jc){
				float ua,va,ub,vb,uc,vc; uv3(ia,ja,ua,va); uv3(ib,jb,ub,vb); uv3(ic,jc,uc,vc);
				emit(F,A,B,C, ua,va,ub,vb,uc,vc); };
			for (int j=0;j<n;++j) for (int i=0;i+j<n;++i){
				emit3(GID(i,j),GID(i+1,j),GID(i,j+1), i,j, i+1,j, i,j+1);        // up
				if (i+j < n-1)
					emit3(GID(i+1,j),GID(i+1,j+1),GID(i,j+1), i+1,j, i+1,j+1, i,j+1); // down
			}
		}

		// Copy non-target + any faces not consumed (defensive) verbatim.
		for (int32_t i=0;i<T->FIndex;++i){
			const Face &F=T->Faces[i];
			if (isTarget(&F) && i < int32_t(consumed.size()) && consumed[i]) continue;
			faces.push_back(F);
			fIdx.push_back({ F.A?vidx(F.A):0u, F.B?vidx(F.B):0u, F.C?vidx(F.C):0u });
			faceFromEdge.push_back(0);
		}

		// canonical vertex on the shared side at `param` (used by both the S4a
		// union-conform below and the level-boundary heal after displacement).
		auto sideVid=[&](uint32_t clo,uint32_t chi,float param)->uint32_t{
			if (param<=0.0f) return clo; if (param>=1.0f) return chi;
			return edgeVert(clo,chi,param);   // canonical → already-created shared vert
		};

		// ── S4a: fan↔edge SEAM-HOLE fix (union triangulation at the shared side).
		// The cross-patch heal below only REPOSITIONS the finer side's verts onto
		// the coarser polyline; where the two sides' param lists have NO subset
		// relation (a fan/lone patch's i/2^L side meeting an edge-aligned patch's
		// groove side) the repositioned chord bypasses the other side's groove
		// KINK → a hairline hole. Fix: for such sides, UNION both param lists and
		// make BOTH patches carry a vertex at every union param — by fan-splitting
		// the boundary triangle of each of a patch's own segments that a union
		// param falls inside. Both boundary polylines then pass through the
		// identical shared-vertex set → watertight, and the heal below no-ops on
		// these sides (their records are set equal to the union). SUBSET sides
		// (level boundaries) are LEFT to the heal, so adaptive levels stay
		// byte-identical. Runs BEFORE displacement so the inserted verts displace.
		// Behind greets_displace_seam_union (default on; --no- = pre-fix behavior).
		const bool seamUnion = fds::FeatureFlags::greets_displace_seam_union();
		int nSeamHoleSides = 0, nSeamSplits = 0;
		{
			auto isSubset=[](const std::vector<float>&sub,const std::vector<float>&sup){
				size_t j=0;
				for (float s : sub){
					while (j<sup.size() && sup[j] < s-1e-5f) ++j;
					if (j>=sup.size() || std::fabs(sup[j]-s) > 1e-5f) return false;
				}
				return true;
			};
			// Fan-split the ONE boundary triangle carrying side-edge (v@lo,v@hi)
			// into a strip through the inserted params (ascending, all in (lo,hi)),
			// preserving winding + affine-along-edge UVs. `edgeFace` maps the
			// current target faces' undirected edges → face index (rebuilt per
			// side, so a corner face split on an adjacent side is re-found here).
			for (auto &kv : sideReg){
				auto &recs = kv.second; if (recs.size()!=2) continue;
				const std::vector<float> Pa = recs[0].params, Pb = recs[1].params;
				if (Pa == Pb) continue;
				if (isSubset(Pa,Pb) || isSubset(Pb,Pa)) continue;   // level boundary → heal
				++nSeamHoleSides;
				if (!seamUnion) continue;
				const uint32_t clo=kv.first.first, chi=kv.first.second;
				// union of the two interior param lists
				std::vector<float> U; U.reserve(Pa.size()+Pb.size());
				U.insert(U.end(),Pa.begin(),Pa.end()); U.insert(U.end(),Pb.begin(),Pb.end());
				std::sort(U.begin(),U.end());
				U.erase(std::unique(U.begin(),U.end(),
					[](float x,float y){return std::fabs(x-y)<1e-5f;}),U.end());
				// undirected edge → target-face index over the CURRENT faces
				std::map<std::pair<uint32_t,uint32_t>,int> edgeFace;
				for (size_t fi=0; fi<faces.size(); ++fi){
					if (!isTarget(&faces[fi])) continue;
					const uint32_t v[3]={fIdx[fi][0],fIdx[fi][1],fIdx[fi][2]};
					for (int e=0;e<3;++e){ uint32_t a=v[e],b=v[(e+1)%3];
						edgeFace[{std::min(a,b),std::max(a,b)}]=int(fi); }
				}
				auto fanSplit=[&](float lo,float hi,const std::vector<float>&insideU){
					const uint32_t vLo=sideVid(clo,chi,lo), vHi=sideVid(clo,chi,hi);
					auto it=edgeFace.find({std::min(vLo,vHi),std::max(vLo,vHi)});
					if (it==edgeFace.end()) return;             // defensive: no clean edge
					const int fi=it->second;
					curPatchEdge = faceFromEdge[size_t(fi)] != 0;   // clones inherit origin
					const Face proto=faces[fi];
					const uint32_t tri[3]={fIdx[fi][0],fIdx[fi][1],fIdx[fi][2]};
					const float triU[3]={proto.U1,proto.U2,proto.U3};
					const float triV[3]={proto.V1,proto.V2,proto.V3};
					int ei=-1;                                   // tri[ei]→tri[ei+1] is the edge
					for (int i=0;i<3;++i){ uint32_t p=tri[i],q=tri[(i+1)%3];
						if ((p==vLo&&q==vHi)||(p==vHi&&q==vLo)){ ei=i; break; } }
					if (ei<0) return;
					const int e1i=(ei+1)%3, xi=(ei+2)%3;
					const uint32_t e0=tri[ei], e1=tri[e1i], X=tri[xi];
					const float pe0=(e0==vLo)?lo:hi, pe1=(e1==vLo)?lo:hi;
					// boundary sequence e0 → (inserted, ordered pe0→pe1) → e1
					struct SV{ uint32_t id; float u,v; };
					std::vector<SV> seq;
					seq.push_back({e0,triU[ei],triV[ei]});
					std::vector<float> ins=insideU;             // ascending
					if (pe1<pe0) std::reverse(ins.begin(),ins.end());
					for (float p : ins){
						const float frac=(p-pe0)/(pe1-pe0);
						seq.push_back({ edgeVert(clo,chi,p),
							meshLerpf(triU[ei],triU[e1i],frac),
							meshLerpf(triV[ei],triV[e1i],frac) });
					}
					seq.push_back({e1,triU[e1i],triV[e1i]});
					const float Xu=triU[xi], Xv=triV[xi];
					for (size_t i=0;i+1<seq.size();++i){
						if (i==0){
							Face &f=faces[fi]; f.frame=nullptr;
							fIdx[fi]={seq[0].id,seq[1].id,X};
							f.U1=seq[0].u;f.V1=seq[0].v; f.U2=seq[1].u;f.V2=seq[1].v; f.U3=Xu;f.V3=Xv;
							f.EU1=f.U1;f.EV1=f.V1; f.EU2=f.U2;f.EV2=f.V2; f.EU3=f.U3;f.EV3=f.V3;
						} else {
							emit(proto, seq[i].id,seq[i+1].id,X,
								seq[i].u,seq[i].v, seq[i+1].u,seq[i+1].v, Xu,Xv);
						}
						++nSeamSplits;
					}
					curPatchEdge = false;
				};
				// conform BOTH patches: for each, split each own segment that a
				// union param falls strictly inside.
				for (const std::vector<float> *Pp : {&Pa,&Pb}){
					std::vector<float> nodes; nodes.reserve(Pp->size()+2);
					nodes.push_back(0.0f);
					nodes.insert(nodes.end(),Pp->begin(),Pp->end());
					nodes.push_back(1.0f);
					for (size_t s=0;s+1<nodes.size();++s){
						const float lo=nodes[s], hi=nodes[s+1];
						std::vector<float> insideU;
						for (float u : U) if (u>lo+1e-5f && u<hi-1e-5f) insideU.push_back(u);
						if (!insideU.empty()) fanSplit(lo,hi,insideU);
					}
				}
				recs[0].params = U; recs[1].params = U;   // heal now no-ops this side
			}
		}

		// ── displacement (per-vertex height averaged over incident target faces,
		// pushed along the vertex normal; authored-border verts pinned to zero) ──
		const uint32_t nV = uint32_t(verts.size());
		if (pinnedZero.size() < nV) pinnedZero.resize(nV, 0);
		std::vector<float> hSum(nV, 0.0f); std::vector<int> hCnt(nV, 0);
		auto isTargetNew=[&](const Face &F){ return F.Txtr && F.Txtr->Name && !std::strcmp(F.Txtr->Name,matName); };
		// ── per-LINE height override (--greets_displace_line_height): a vertex
		// whose MAP-space position lies ON a detected groove line — or NEAR one,
		// inside the groove's mid-slope influence zone — displaces consistently
		// with the line instead of by its own bilinear sample. Classification is
		// by UV (wrap-aware) — semantically exact, since the relief is a
		// function of UV — so it also catches the verts the S4a seam-union
		// inserts on groove sides. What a vert takes depends on its PATCH
		// population (see the line-rep table comment above):
		//   EDGE-path verts (edgeOwned): ON-line hits (≤ kLineEps — line
		//     placement is quantized to the 1/2048 param lattice, ≤0.25 texel on
		//     the deepest quads) take the line rep, MINIMUM at crossings (the
		//     mortar recess dominates, matching the map); in-capture near-line
		//     hits take the nearest line's rep (nearest, not min, so a wide
		//     groove's B floor line — 2·pad from its A shoulder line — can't
		//     drag the shoulder ring down).
		//   FALLBACK (fan/lone) verts: any line within kLineCapture pins the
		//     vert to the NEAREST line's PLATEAU ref — the fallback lattice
		//     cannot represent the groove, so it must not carve it (the t=2845
		//     zigzag was exactly this population's smeared carve shearing at
		//     grazing; consistent heights alone measured insufficient).
		// Plateau-interior verts (beyond capture) are untouched everywhere.
		// Pinned border verts are never overridden (they don't displace at all).
		const bool lineHeight = lineHeightOn && grid.valid &&
		                        !(hLineRep.empty() && vLineRep.empty());
		const float kLineEps = 0.45f;                 // texels at the bake mip
		const float kLineCapture = 2.0f * kPadTex;    // groove influence zone
		std::vector<float> lineRepV, linePlatV, linePlatD;
		std::vector<char> edgeOwned;                  // vert used by an edge-path face
		if (lineHeight) {
			lineRepV.assign(nV, 1e30f);
			linePlatV.assign(nV, 1e30f);
			linePlatD.assign(nV, 1e30f);
			edgeOwned.assign(nV, 0);
			for (size_t i = 0; i < faces.size(); ++i) {
				if (!faceFromEdge[i]) continue;
				for (int k = 0; k < 3; ++k)
					if (fIdx[i][k] < nV) edgeOwned[fIdx[i][k]] = 1;
			}
		}
		auto wrapDist = [](float a, float b, float extent) -> float {
			float d = std::fmod(std::fabs(a - b), extent);
			return std::min(d, extent - d);
		};
		for (size_t i=0;i<faces.size();++i){
			const Face &F=faces[i]; if (!isTargetNew(F)) continue;
			const uint32_t vi[3]={fIdx[i][0],fIdx[i][1],fIdx[i][2]};
			const float cu2[3]={F.U1,F.U2,F.U3}, cv2[3]={F.V1,F.V2,F.V3};
			for (int k=0;k<3;++k){ if (vi[k]>=nV||pinnedZero[vi[k]]) continue;
				hSum[vi[k]]+=SampleHeight8Bilinear(hm,useMip,cu2[k],cv2[k]); hCnt[vi[k]]+=1;
				if (!lineHeight) continue;
				const float mwF = float(useMipW), mhF = float(useMipH);
				float xm = std::fmod(cu2[k]*mwF, mwF); if (xm < 0.0f) xm += mwF;
				float ym = std::fmod(cv2[k]*mhF, mhF); if (ym < 0.0f) ym += mhF;
				float bestOn = 1e30f;                 // min rep over ON-line hits
				float nearRep = 1e30f, nearDist = 1e30f;   // nearest in-capture line
				float nearPlat = 1e30f, nearPlatD = 1e30f; // nearest line's plateau ref
				auto consider = [&](float d, float rep, float plat){
					if (d <= kLineEps) bestOn = std::min(bestOn, rep);
					else if (d <= kLineCapture && d < nearDist) { nearDist = d; nearRep = rep; }
					if (d <= kLineCapture && d < nearPlatD) { nearPlatD = d; nearPlat = plat; }
				};
				for (const auto &L : hLineRep)
					consider(wrapDist(ym, L.pos, mhF), L.rep, L.plateau);
				// vertical lines: candidate bands from the row template — every
				// row whose y-range (widened by the capture radius) contains
				// this vertex's map y contributes its bandA/bandB, mirroring
				// rowBreaks.
				if (!vLineRep.empty() && !rowTpl.empty()) {
					const float base = rowTpl.front().y0;
					float yr = std::fmod(ym - base, mhF); if (yr < 0.0f) yr += mhF;
					yr += base;
					int cand[6]; int nCand = 0;
					auto addBandC = [&](int b2){
						if (b2 < 0 || b2 >= int(vLineRep.size())) return;
						for (int c2 = 0; c2 < nCand; ++c2) if (cand[c2] == b2) return;
						if (nCand < 6) cand[nCand++] = b2;
					};
					for (const StoneRowT &R : rowTpl)
						if (yr >= R.y0 - kLineCapture && yr < R.y1 + kLineCapture) {
							addBandC(R.bandA); addBandC(R.bandB);
						}
					for (int c2 = 0; c2 < nCand; ++c2)
						for (const auto &L : vLineRep[size_t(cand[c2])])
							consider(wrapDist(xm, L.pos, mwF), L.rep, L.plateau);
				}
				const float chosen = bestOn < 1e29f ? bestOn
				                   : (nearDist < 1e29f ? nearRep : 1e30f);
				if (chosen < 1e29f) lineRepV[vi[k]] = std::min(lineRepV[vi[k]], chosen);
				if (nearPlatD < linePlatD[vi[k]]) {
					linePlatD[vi[k]] = nearPlatD;
					linePlatV[vi[k]] = nearPlat;
				}
			}
		}
		std::vector<Vector> basePos(nV);
		for (uint32_t i=0;i<nV;++i) basePos[i]=verts[i].Pos;
		int nMoved=0, nLineSnap=0, nPlatPin=0; float dMin=1e30f,dMax=-1e30f;
		for (uint32_t i=0;i<nV;++i){
			if (pinnedZero[i]||hCnt[i]==0) continue;
			const Vector &N=verts[i].N; const float nl=std::sqrt(N.x*N.x+N.y*N.y+N.z*N.z);
			if (nl<1e-6f) continue;
			// Two-population override (see the line-rep table comment):
			//  * edge-path verts on/near a line -> the line's rep;
			//  * fallback-path verts inside a groove's influence zone -> the
			//    groove's plateau ref (no carve — the zigzag's causal set).
			float h = hCnt[i] ? hSum[i]/float(hCnt[i]) : mipMean;
			if (lineHeight) {
				if (edgeOwned[i]) {
					if (lineRepV[i] < 1e29f) { h = lineRepV[i]; ++nLineSnap; }
				} else if (linePlatD[i] < 1e29f) {
					h = linePlatV[i]; ++nPlatPin;
				}
			}
			float dsp=amp*(h-mipMean);
			verts[i].Pos.x+=N.x/nl*dsp; verts[i].Pos.y+=N.y/nl*dsp; verts[i].Pos.z+=N.z/nl*dsp;
			if (dsp<dMin)dMin=dsp; if (dsp>dMax)dMax=dsp; ++nMoved;
		}
		if (lineHeight)
			std::fprintf(stderr, "[STONE-LINE] '%s': %d of %d displaced verts "
				"snapped to their groove line's rep (edge path); %d fallback-path "
				"verts plateau-pinned inside groove zones\n",
				matName, nLineSnap, nMoved, nPlatPin);

		// ── FOLD RELAXATION (--greets_displace_fold_relax, default on) — the
		// t=6097 SLIVER-GAP fix. A displaced target face whose geometric normal
		// no longer agrees with its authored plane has FOLDED: typically a narrow
		// return face (e.g. a jamb/pilaster side one shoulder-band tall) whose
		// verts recede by very different amounts along differently-averaged
		// vertex normals, twisting the strip past 90°. The commit below keeps the
		// AUTHORED orientation sign for N/NormProd, so a folded face's plane
		// equation opposes its actual winding and the backface cull rejects it
		// from the FRONT — a see-through slit through topologically sealed
		// geometry (measured at the repro pose: 4 folded slivers, dot(geomN,
		// authN) −0.14..−0.70, culled → background through the wall). Same defect
		// class B1 fixed in SubdivideMaterialFaces with fold-relaxation; adapted
		// here: iteratively HALVE the displacement of every vert of a folded
		// face until every target face agrees with its authored plane (offsets →
		// 0 restores the authored geometry, so convergence is monotone); a final
		// pass zeroes any straggler. Runs BEFORE the cross-patch heal so healed
		// polylines land on the relaxed anchors. Relief loss is confined to the
		// folded slivers (everything else keeps full displacement).
		//
		// POPULATION (measured on the shipping greets bake, g = (B−A)×(C−A)):
		// healthy displaced faces sit at g·authN ≈ −1 (FLD winding convention),
		// the carve's tilted step faces spread toward 0, and the INVERTED faces
		// are the far positive tail (~3.9k of 63k rooms faces at the repro) —
		// exactly the faces whose commit picks N against their winding, so the
		// plane cull rejects them while they still front the camera (proven:
		// force-two-sided closes the sliver with no other change; z==0 52 → 1).
		// DIAG (FDS_STONE_FOLD_HIST): g·authN histogram over the displaced target
		// faces — shows that population structure on new content. NOTE the
		// histogram is in AUTHORED-N terms (convention-dependent, diagnostic
		// only); the relax criterion below is convention-free.
		if (std::getenv("FDS_STONE_FOLD_HIST")) {
			static const float edges[] = {-1.f,-0.5f,-0.2f,-0.1f,-0.05f,-0.01f,-0.001f,0.f,0.001f,0.01f,0.05f,0.1f,0.2f,0.5f,1.01f};
			int cnt[14] = {0}; int nT=0;
			for (size_t i=0;i<faces.size();++i){
				const Face &F=faces[i]; if(!isTargetNew(F)) continue;
				const uint32_t a=fIdx[i][0],b=fIdx[i][1],c=fIdx[i][2];
				if (a>=nV||b>=nV||c>=nV) continue;
				const Vector &A=verts[a].Pos,&B=verts[b].Pos,&C=verts[c].Pos;
				const float e1x=B.x-A.x,e1y=B.y-A.y,e1z=B.z-A.z,e2x=C.x-A.x,e2y=C.y-A.y,e2z=C.z-A.z;
				float gx=e1y*e2z-e1z*e2y,gy=e1z*e2x-e1x*e2z,gz=e1x*e2y-e1y*e2x;
				const float gl=std::sqrt(gx*gx+gy*gy+gz*gz); if (gl<1e-9f) continue;
				const float d=(gx*F.N.x+gy*F.N.y+gz*F.N.z)/gl; ++nT;
				for (int k=0;k<14;++k) if (d<edges[k+1]) { ++cnt[k]; break; }
			}
			std::fprintf(stderr,"[FOLD-HIST] '%s' n=%d:",matName,nT);
			for (int k=0;k<14;++k) std::fprintf(stderr," [%g,%g)=%d",edges[k],edges[k+1],cnt[k]);
			std::fprintf(stderr,"\n");
		}
		int nFoldFaces = 0, nFoldPasses = 0;
		if (fds::FeatureFlags::greets_displace_fold_relax()) {
			// CONVENTION-FREE criterion: a face is inverted when its DISPLACED
			// winding normal opposes its own BASE (pre-displacement) winding
			// normal — g_disp·g_base < 0, i.e. the face rotated past 90° from
			// its undisplaced self. Comparing against the face's own base (not
			// the authored parent N) makes the test independent of the mesh's
			// winding-vs-N convention: FLD-loaded greets walls and the
			// SceneBuilder test rigs have OPPOSITE conventions (measured — an
			// authored-N criterion flattened the rig's entire bake), and an
			// undisplaced winding-odd face has g_disp = g_base → never marked.
			// Precompute each target face's base winding normal once.
			std::vector<std::array<float,3>> gBase(faces.size(), {0,0,0});
			for (size_t i = 0; i < faces.size(); ++i) {
				if (!isTargetNew(faces[i])) continue;
				const uint32_t a=fIdx[i][0], b=fIdx[i][1], c=fIdx[i][2];
				if (a>=nV||b>=nV||c>=nV) continue;
				const Vector &A=basePos[a],&B=basePos[b],&C=basePos[c];
				const float e1x=B.x-A.x,e1y=B.y-A.y,e1z=B.z-A.z;
				const float e2x=C.x-A.x,e2y=C.y-A.y,e2z=C.z-A.z;
				gBase[i] = { e1y*e2z-e1z*e2y, e1z*e2x-e1x*e2z, e1x*e2y-e1y*e2x };
			}
			for (int pass = 0; pass < 8; ++pass) {
				std::vector<char> mark(nV, 0);
				int nFold = 0;
				for (size_t i = 0; i < faces.size(); ++i) {
					if (!isTargetNew(faces[i])) continue;
					const uint32_t a=fIdx[i][0], b=fIdx[i][1], c=fIdx[i][2];
					if (a>=nV||b>=nV||c>=nV) continue;
					const float bx=gBase[i][0], by=gBase[i][1], bz=gBase[i][2];
					if (bx*bx+by*by+bz*bz < 1e-18f) continue;   // base-degenerate
					const Vector &A=verts[a].Pos,&B=verts[b].Pos,&C=verts[c].Pos;
					const float e1x=B.x-A.x,e1y=B.y-A.y,e1z=B.z-A.z;
					const float e2x=C.x-A.x,e2y=C.y-A.y,e2z=C.z-A.z;
					const float gx=e1y*e2z-e1z*e2y, gy=e1z*e2x-e1x*e2z, gz=e1x*e2y-e1y*e2x;
					if (gx*bx+gy*by+gz*bz >= 0.0f) continue;    // still on its base side
					bool canRelax=false;
					for (uint32_t vi2 : {a,b,c})
						if (!pinnedZero[vi2] && (verts[vi2].Pos.x!=basePos[vi2].x ||
						     verts[vi2].Pos.y!=basePos[vi2].y || verts[vi2].Pos.z!=basePos[vi2].z))
							{ mark[vi2]=1; canRelax=true; }
					if (canRelax) ++nFold;
					if (pass==0 && canRelax) ++nFoldFaces;
				}
				if (!nFold) break;
				nFoldPasses = pass + 1;
				const float s = (pass == 7) ? 0.0f : 0.5f;
				for (uint32_t i2 = 0; i2 < nV; ++i2) {
					if (!mark[i2]) continue;
					verts[i2].Pos.x = basePos[i2].x + s*(verts[i2].Pos.x - basePos[i2].x);
					verts[i2].Pos.y = basePos[i2].y + s*(verts[i2].Pos.y - basePos[i2].y);
					verts[i2].Pos.z = basePos[i2].z + s*(verts[i2].Pos.z - basePos[i2].z);
				}
			}
			if (nFoldFaces)
				std::fprintf(stderr, "[STONE-FOLD] '%s': %d INVERTED faces relaxed "
					"(%d halving passes; inverted = displaced winding crossed its "
					"own base plane, so the committed N opposes the winding and "
					"the plane cull rejects the face while it still fronts the "
					"camera — the see-through sliver)\n",
					matName, nFoldFaces, nFoldPasses);
		}

		// ── cross-patch crack pinning: on each interior side shared by two
		// patches whose vertex PARAM LISTS differ (level boundary, or edge-
		// aligned vs fallback tessellation), snap the non-anchor side's extra
		// verts onto the anchor side's displaced POLYLINE. Anchor = the record
		// with fewer params (the coarser side — the straight-segment rule the
		// old level pinning used, generalized), lexicographic tie-break so the
		// choice never depends on registration order. ── S4a-conformed sides
		// (fan↔edge, non-subset) had their two records set equal to the union
		// above, so they hit the Pa==Pb skip and no-op here.
		int nTJ=0;
		for (auto &kv : sideReg){
			const auto &recs = kv.second; if (recs.size()!=2) continue;
			const std::vector<float> &Pa = recs[0].params, &Pb = recs[1].params;
			if (Pa == Pb) continue;
			const bool aAnchor = Pa.size() != Pb.size()
				? Pa.size() < Pb.size()
				: std::lexicographical_compare(Pa.begin(),Pa.end(),Pb.begin(),Pb.end());
			const std::vector<float> &anchor = aAnchor ? Pa : Pb;
			const std::vector<float> &other  = aAnchor ? Pb : Pa;
			const uint32_t clo=kv.first.first, chi=kv.first.second;
			// anchor polyline nodes (params 0..1 inclusive)
			std::vector<float> nodes;
			nodes.reserve(anchor.size()+2);
			nodes.push_back(0.0f);
			for (float q : anchor) nodes.push_back(q);
			nodes.push_back(1.0f);
			for (float p : other){
				bool onAnchor=false;
				for (float q : nodes) if (std::fabs(p-q) < 1e-5f) { onAnchor=true; break; }
				if (onAnchor) continue;
				int kk=0;
				while (kk+1 < int(nodes.size())-1 && nodes[kk+1] < p) ++kk;
				const float c0=nodes[kk], c1=nodes[kk+1];
				if (c1-c0 < 1e-6f) continue;
				const float local=(p-c0)/(c1-c0);
				const uint32_t fv=sideVid(clo,chi,p), v0=sideVid(clo,chi,c0), v1=sideVid(clo,chi,c1);
				if (fv>=nV||v0>=nV||v1>=nV) continue;
				verts[fv].Pos.x=meshLerpf(verts[v0].Pos.x,verts[v1].Pos.x,local);
				verts[fv].Pos.y=meshLerpf(verts[v0].Pos.y,verts[v1].Pos.y,local);
				verts[fv].Pos.z=meshLerpf(verts[v0].Pos.z,verts[v1].Pos.z,local);
				++nTJ;
			}
		}

		// ── viz record (magnitude = |final − base|, T-junction pins included) ──
		const Material *targetMat=nullptr;
		for (size_t i=0;i<faces.size();++i) if (isTargetNew(faces[i])){ targetMat=faces[i].Txtr; break; }
		{
			std::vector<char> tv(nV,0);
			for (size_t i=0;i<faces.size();++i){ if(!isTargetNew(faces[i])) continue;
				for (int k=0;k<3;++k) if (fIdx[i][k]<nV) tv[fIdx[i][k]]=1; }
			for (uint32_t i=0;i<nV;++i) if (tv[i]){
				const float dx=verts[i].Pos.x-basePos[i].x,dy=verts[i].Pos.y-basePos[i].y,dz=verts[i].Pos.z-basePos[i].z;
				fds::DisplaceViz_Record(targetMat, verts[i].Pos, std::sqrt(dx*dx+dy*dy+dz*dz)); }
		}

		// ── viz record (--displace_viz=2 HEIGHT-ERROR field): per emitted target
		// triangle, the SIGNED error (truth − carried) that has the largest
		// magnitude ANYWHERE in the triangle's texel footprint at the BAKE mip.
		// truth = amp*(h − mipMean); carried = the barycentric interp of the
		// triangle's vertices' applied along-normal offset (what the geometry
		// actually provides). The old 4-point (centroid + edge-midpoint) probe
		// ALIASED exactly like the old refinement stencil — on a coarse cell the
		// four points fell on similar-height block interiors, missing the mortar,
		// so the cell read "matched" (green) while carrying no relief. Scanning
		// the whole footprint (strided ~every 2 texels) makes the tint honest.
		// The value stored is PRE-NORMALIZED to a signed fraction where ±1 = the
		// map's full peak-to-valley relief missing (an ABSOLUTE, self-calibrating
		// scale — the old overlay normalized by the single worst edge cell, which
		// washed everything green). Zero cost unless the mode is on. ──
		if (fds::FeatureFlags::displace_viz() == 2) {
			std::vector<float> carriedV(nV, 0.0f);
			for (uint32_t i=0;i<nV;++i){
				const Vector &N=verts[i].N; const float nl=std::sqrt(N.x*N.x+N.y*N.y+N.z*N.z);
				if (nl<1e-6f) continue;
				const float dx=verts[i].Pos.x-basePos[i].x,dy=verts[i].Pos.y-basePos[i].y,dz=verts[i].Pos.z-basePos[i].z;
				carriedV[i]=(dx*N.x+dy*N.y+dz*N.z)/nl;   // signed along-normal offset
			}
			const float refRelief = amp * std::max(reliefRange01, 1e-4f);   // world units, ±1 scale
			for (size_t i=0;i<faces.size();++i){
				const Face &F=faces[i]; if(!isTargetNew(F)) continue;
				const uint32_t a=fIdx[i][0],b=fIdx[i][1],c=fIdx[i][2];
				if (a>=nV||b>=nV||c>=nV) continue;
				const float cu3[3]={F.U1,F.U2,F.U3}, cv3[3]={F.V1,F.V2,F.V3};
				const float cd3[3]={carriedV[a],carriedV[b],carriedV[c]};
				const float umin=std::min(cu3[0],std::min(cu3[1],cu3[2]));
				const float umax=std::max(cu3[0],std::max(cu3[1],cu3[2]));
				const float vmin=std::min(cv3[0],std::min(cv3[1],cv3[2]));
				const float vmax=std::max(cv3[0],std::max(cv3[1],cv3[2]));
				int nu=int(std::fabs(umax-umin)*float(useMipW)*0.5f)+2; if(nu>16)nu=16;
				int nv=int(std::fabs(vmax-vmin)*float(useMipH)*0.5f)+2; if(nv>16)nv=16;
				// affine barycentric from UV (planar tri, affine UV) → carried + inside test
				const float den=(cv3[1]-cv3[2])*(cu3[0]-cu3[2])+(cu3[2]-cu3[1])*(cv3[0]-cv3[2]);
				const float invDen = std::fabs(den)>1e-12f ? 1.0f/den : 0.0f;
				float bestSigned=0.0f, bestAbs=-1.0f;
				auto probe=[&](float u,float v){
					const float w0=((cv3[1]-cv3[2])*(u-cu3[2])+(cu3[2]-cu3[1])*(v-cv3[2]))*invDen;
					const float w1=((cv3[2]-cv3[0])*(u-cu3[2])+(cu3[0]-cu3[2])*(v-cv3[2]))*invDen;
					const float w2=1.0f-w0-w1;
					if (w0<-0.02f||w1<-0.02f||w2<-0.02f) return;   // outside triangle
					const float truth=amp*(SampleHeight8Bilinear(hm,useMip,u,v)-mipMean);
					const float carried=cd3[0]*w0+cd3[1]*w1+cd3[2]*w2;
					const float e=truth-carried;
					if (std::fabs(e)>bestAbs){ bestAbs=std::fabs(e); bestSigned=e; }
				};
				for (int pj=0;pj<nv;++pj) for (int pi=0;pi<nu;++pi){
					const float su=(nu>1)?float(pi)/float(nu-1):0.5f;
					const float sv=(nv>1)?float(pj)/float(nv-1):0.5f;
					probe(umin+(umax-umin)*su, vmin+(vmax-vmin)*sv);
				}
				// always include the 3 corners + centroid (thin tris the grid may skip)
				for (int k=0;k<3;++k) probe(cu3[k],cv3[k]);
				probe((cu3[0]+cu3[1]+cu3[2])/3.0f,(cv3[0]+cv3[1]+cv3[2])/3.0f);
				const Vector ctr={ (verts[a].Pos.x+verts[b].Pos.x+verts[c].Pos.x)/3.0f,
				                   (verts[a].Pos.y+verts[b].Pos.y+verts[c].Pos.y)/3.0f,
				                   (verts[a].Pos.z+verts[b].Pos.z+verts[c].Pos.z)/3.0f };
				fds::DisplaceViz_RecordError(targetMat, ctr, bestSigned / refRelief);
			}
		}

		// ── PARENT-PLANE stamping (--greets_displace_shadow_planes; see
		// MeshOps.h) — must run BEFORE the commit below overwrites N/NormProd
		// with the displaced facet plane: at this point every emitted target
		// face still carries its PARENT's authored N/NormProd (emit copies the
		// proto face). Record each distinct parent plane (deduped on the greets
		// shadow clustering's own quantization grid: normal 1/16 ≈ 3.5°,
		// distance 1/2 unit) and stamp the face's ShadowMatID with the plane's
		// 1-based ordinal — a transient tag the clustering resolves and
		// replaces. Registry is per material, shared across meshes.
		{
			auto &reg    = g_stoneParentPlanes[matName];
			auto &keyIdx = g_stoneParentPlaneKeys[matName];
			auto q = [](float v, float s){ return int(std::floor(v*s + 0.5f)); };
			for (size_t i = 0; i < faces.size(); ++i) {
				Face &F = faces[i];
				if (!isTargetNew(F)) continue;
				const float len = std::sqrt(F.N.x*F.N.x + F.N.y*F.N.y + F.N.z*F.N.z);
				if (!(len > 1e-4f)) { F.ShadowMatID = 0; continue; }
				const float inv = 1.0f/len;
				const float nx = F.N.x*inv, ny = F.N.y*inv, nz = F.N.z*inv;
				const float d  = -F.NormProd*inv;    // NormProd = -(N·A) → n̂·A
				const std::array<int,4> key{ q(nx,16.0f), q(ny,16.0f), q(nz,16.0f), q(d,2.0f) };
				auto it = keyIdx.find(key);
				if (it == keyIdx.end()) {
					reg.push_back({nx, ny, nz, d});
					it = keyIdx.emplace(key, uint16_t(reg.size())).first;   // 1-based
				}
				F.ShadowMatID = it->second;
			}
		}

		// ── --pom_shell_census, TESSELLATION HALF (DIAGNOSTIC, default OFF) ──
		// Same table as PomShell_Build's, for the arm that ships. The bake's
		// amplitude is authored in WORLD units (dsp = amp*(h-mipMean)), so per
		// authored plane the interesting number is not the amplitude — it is
		// constant by construction — but the relief the lattice actually CARRIES
		// (the spread of the applied offsets) and the per-face world-per-UV that
		// sets the relief's world WAVELENGTH. Run here: the parent planes are
		// stamped, `basePos` still holds the pre-displacement positions, and the
		// faces still carry their parent's authored N.
		if (fds::FeatureFlags::pom_shell_census() && !faces.empty()) {
			struct BRow { int n = 0; float wLo = 1e30f, wHi = -1e30f; std::vector<float> d; };
			std::map<uint16_t, BRow> byPlane;
			std::vector<char> seen(verts.size(), 0);
			std::vector<float> allW;
			for (size_t i = 0; i < faces.size(); ++i) {
				Face &F = faces[i];
				if (!isTargetNew(F) || !F.ShadowMatID) continue;
				BRow &r = byPlane[F.ShadowMatID];
				++r.n;
				const uint32_t vi[3] = { fIdx[i][0], fIdx[i][1], fIdx[i][2] };
				if (vi[0] >= verts.size() || vi[1] >= verts.size() || vi[2] >= verts.size())
					continue;
				const Vector &pa = basePos[vi[0]], &pb = basePos[vi[1]], &pc = basePos[vi[2]];
				const float du1 = F.U2 - F.U1, dv1 = F.V2 - F.V1;
				const float du2 = F.U3 - F.U1, dv2 = F.V3 - F.V1;
				const float det = du1*dv2 - du2*dv1;
				if (std::fabs(det) > 1e-12f) {
					const float inv = 1.0f/det;
					const float e1x=pb.x-pa.x,e1y=pb.y-pa.y,e1z=pb.z-pa.z;
					const float e2x=pc.x-pa.x,e2y=pc.y-pa.y,e2z=pc.z-pa.z;
					const float tx=(e1x*dv2-e2x*dv1)*inv, ty=(e1y*dv2-e2y*dv1)*inv, tz=(e1z*dv2-e2z*dv1)*inv;
					const float bx=(e2x*du1-e1x*du2)*inv, by=(e2y*du1-e1y*du2)*inv, bz=(e2z*du1-e1z*du2)*inv;
					const float w = std::sqrt(std::sqrt((tx*tx+ty*ty+tz*tz)*(bx*bx+by*by+bz*bz)));
					if (w > 0.0f) {
						r.wLo = std::min(r.wLo, w); r.wHi = std::max(r.wHi, w);
						allW.push_back(w);
					}
				}
				for (int k = 0; k < 3; ++k) {
					if (seen[vi[k]]) continue;
					seen[vi[k]] = 1;
					const Vector &b0 = basePos[vi[k]], &p1 = verts[vi[k]].Pos;
					const float dx=p1.x-b0.x, dy=p1.y-b0.y, dz=p1.z-b0.z;
					float s = std::sqrt(dx*dx+dy*dy+dz*dz);
					if (dx*F.N.x + dy*F.N.y + dz*F.N.z < 0.0f) s = -s;
					r.d.push_back(s);
				}
			}
			auto pctv = [](std::vector<float> &v, double p) {
				if (v.empty()) return 0.0f;
				std::sort(v.begin(), v.end());
				return v[size_t(p*double(v.size()-1)+0.5)];
			};
			std::fprintf(stderr,
				"[STONE-CENSUS] '%s' WORLD amp=%.3f mip=%d: planes=%zu, mesh faces=%zu, "
				"per-face w min/med/max = %.3f / %.3f / %.3f (x%.2f). The amplitude is "
				"the SAME %.3f world on every face; w only sets the relief WAVELENGTH.\n",
				matName, (double)amp, useMip, byPlane.size(), faces.size(),
				(double)pctv(allW,0.0), (double)pctv(allW,0.5), (double)pctv(allW,1.0),
				(double)(pctv(allW,0.0) > 0.0f ? pctv(allW,1.0)/pctv(allW,0.0) : 0.0),
				(double)amp);
			for (auto &kv : byPlane) {
				BRow &r = kv.second;
				const StoneParentPlane *pp = MeshOps_StoneParentPlane(matName, kv.first);
				const float p05 = pctv(r.d, 0.05), p50 = pctv(r.d, 0.50), p95 = pctv(r.d, 0.95);
				std::fprintf(stderr,
					"[STONE-CENSUS-PLANE] '%s' P%02u N=(%+.3f,%+.3f,%+.3f) d=%+9.3f "
					"faces=%5d verts=%5zu  w=%.3f..%.3f  carriedRelief p05/p50/p95 = "
					"%+.4f/%+.4f/%+.4f span=%.4f (%.0f%% of amp)\n",
					matName, kv.first,
					pp ? (double)pp->nx : 0.0, pp ? (double)pp->ny : 0.0,
					pp ? (double)pp->nz : 0.0, pp ? (double)pp->d : 0.0,
					r.n, r.d.size(), (double)r.wLo, (double)r.wHi,
					(double)p05, (double)p50, (double)p95, (double)(p95 - p05),
					(double)(amp > 0.0f ? 100.0f*(p95-p05)/amp : 0.0f));
			}
		}
		// Commit new arrays.
		Vertex *nv = new Vertex[verts.size()];
		std::memcpy(nv, verts.data(), verts.size()*sizeof(Vertex));
		Face *nf = new Face[faces.size()];
		for (size_t i=0;i<faces.size();++i){
			nf[i]=faces[i]; nf[i].A=&nv[fIdx[i][0]]; nf[i].B=&nv[fIdx[i][1]]; nf[i].C=&nv[fIdx[i][2]];
			if (!isTargetNew(nf[i])) continue;   // non-target keeps its authored N/NormProd
			const Vector &A=nf[i].A->Pos,&B=nf[i].B->Pos,&C=nf[i].C->Pos;
			const float e1x=B.x-A.x,e1y=B.y-A.y,e1z=B.z-A.z, e2x=C.x-A.x,e2y=C.y-A.y,e2z=C.z-A.z;
			float gx=e1y*e2z-e1z*e2y, gy=e1z*e2x-e1x*e2z, gz=e1x*e2y-e1y*e2x;
			const float gl=std::sqrt(gx*gx+gy*gy+gz*gz);
			if (gl>1e-6f){ gx/=gl;gy/=gl;gz/=gl;
				if (gx*faces[i].N.x+gy*faces[i].N.y+gz*faces[i].N.z<0.0f){gx=-gx;gy=-gy;gz=-gz;}
				nf[i].N.x=gx;nf[i].N.y=gy;nf[i].N.z=gz; }
			nf[i].NormProd = -(nf[i].N.x*A.x+nf[i].N.y*A.y+nf[i].N.z*A.z);
		}
		T->Verts=nv; T->VIndex=int32_t(verts.size());
		T->Faces=nf; T->FIndex=int32_t(faces.size());
		Compute_FaceVertexIndices(T);
		if (T->Flags & Tri_Stationary)
			T->SL=(Color*)getAlignedBlock(sizeof(Color)*T->VIndex, 16);
		if (nMoved==0){ dMin=0.0f; dMax=0.0f; }
		if (std::getenv("FDS_SUBDIV_DIAG") && !metricVals.empty()) {
			std::vector<float> mv = metricVals;
			std::sort(mv.begin(), mv.end());
			auto pct=[&](double p){ return mv[size_t(p*(mv.size()-1))]; };
			std::fprintf(stderr,
				"[STONE-DIAG] '%s' chosen-level bad-cell frac n=%zu "
				"p10/25/50/75/90/max = %.3f/%.3f/%.3f/%.3f/%.3f/%.3f "
				"(eps=%.3f allowFrac=%.2f)\n",
				matName, mv.size(), pct(0.10),pct(0.25),pct(0.50),pct(0.75),pct(0.90),
				mv.back(), (double)kRefineEps, (double)kRefineFrac);
		}
		std::fprintf(stderr,
			"[STONE] '%s' %s L%s amp=%.3f mip=%d: quads=%d (edge-aligned %d: "
			"plat/step/floor cells %d/%d/%d) lones=%d cells(fan/flat)=%d/%d "
			"Lhist=%d/%d/%d/%d/%d/%d Lcap=%d/%d/%d/%d/%d/%d "
			"-> verts %d faces %d, %d displaced [%+.3f..%+.3f], %d T-junction pins, "
			"%d fan↔edge seam-hole sides (%s: %d splits)\n",
			matName, uniformLevel>0?"uniform":"adaptive",
			uniformLevel>0?std::to_string(uniformLevel).c_str():"0..5",
			(double)amp, useMip, builtQuads, edgeQuads,
			edgePlateauC, edgeStepC, edgeFloorC, builtLones, fanCells, flatCells,
			lvlHist[0],lvlHist[1],lvlHist[2],lvlHist[3],lvlHist[4],lvlHist[5],
			lcapHist[0],lcapHist[1],lcapHist[2],lcapHist[3],lcapHist[4],lcapHist[5],
			int(T->VIndex), int(T->FIndex), nMoved, (double)dMin, (double)dMax, nTJ,
			nSeamHoleSides, seamUnion?"union-welded":"heal-only", nSeamSplits);
	}
}

// Re-SMOOTH the displaced stone surface's vertex normals. After displacement,
// MakeFacesIndependentByAngle sees the new relief cells' face planes differing
// by more than its 30° architectural-crease threshold, so it SPLITS every such
// edge and each cell keeps its own flat face normal — the mesh reads as facets
// and the triangulation shows through as hairline seams (and, because the
// tangent basis splits with the normal, as normal-map noise on top; cf. the
// crease-tangent note in MakeFacesIndependent). The fix is a true smooth
// normal across the displaced patch: a scene-wide position-bucket WELD of the
// material's face corners (verts that were one point have bit-identical Pos —
// the greets chunk copy is bitwise), averaging the DISPLACED Face::N (area-
// weighted) over incident faces within `smoothAngleDeg` of each corner's face.
// Continuous normals ⇒ continuous Gouraud shading AND a continuous Gram-Schmidt
// tangent basis, so BOTH the geometric faceting and the normal-map seam vanish;
// the geometry still carries the true silhouette. The angle gate keeps authored
// HARD creases hard: wall-to-wall 90° corners survive for any angle < 90° (the
// cross-corner faces are ~90° apart, so they don't average), and material
// boundaries (wall↔floor↔ceiling) are hard for free (bucketed per base surface,
// so a 'rooms' corner never averages a 'floor'/ceiling face). This is the
// editor's MeshOps_ResmoothSurface machinery, scoped to the init scene + one
// material. Call AFTER MakeFacesIndependentByAngle at the greets hook.
void DisplaceStoneSmoothNormals(Scene *Sc, const char *matName, float smoothAngleDeg) {
	if (!Sc || !matName) return;
	if (smoothAngleDeg < 0.0f)   smoothAngleDeg = 0.0f;
	if (smoothAngleDeg > 180.0f) smoothAngleDeg = 180.0f;
	const std::string base = rev::Editor_BaseSurfName(matName);
	const float cosThr = std::cos(smoothAngleDeg * float(PI) / 180.0f);
	auto surfBaseEq = [&](const Face *f) {
		return f && f->Txtr && f->Txtr->Name
		    && rev::Editor_BaseSurfName(f->Txtr->Name) == base;   // catches ::mirUV clones
	};
	// Corners of every target-surface face across every mesh in the scene
	// (greets copies the smoothed mesh into per-cell chunks, so adjacency is
	// scene-wide — same reasoning as MeshOps_ResmoothSurface).
	struct Corner { Vertex *V; const Face *F; };
	std::vector<Corner> corners;
	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
		if (T->FIndex == 0 || !T->Faces || !T->Verts) continue;
		for (int32_t i = 0; i < T->FIndex; ++i) {
			Face *F = &T->Faces[i];
			if (!surfBaseEq(F) || !F->A || !F->B || !F->C) continue;
			corners.push_back({F->A, F}); corners.push_back({F->B, F}); corners.push_back({F->C, F});
		}
	}
	if (corners.empty()) return;
	// Position-bucket by exact bits (bitwise chunk copy => coincident verts key together).
	struct PK { uint32_t x, y, z; bool operator==(const PK &o) const { return x==o.x&&y==o.y&&z==o.z; } };
	struct PH { size_t operator()(const PK &k) const {
		return (size_t(k.x)*73856093u) ^ (size_t(k.y)*19349663u) ^ (size_t(k.z)*83492791u); } };
	auto keyOf = [&](const Vector &p) { return PK{ meshF2bits(p.x), meshF2bits(p.y), meshF2bits(p.z) }; };
	std::unordered_map<PK, std::vector<int>, PH> bucket;
	bucket.reserve(corners.size() * 2);
	for (int i = 0; i < int(corners.size()); ++i) bucket[keyOf(corners[i].V->Pos)].push_back(i);

	// Per-face tangent from the per-FACE UV gradient (matches Compute_Vertex_Tangents,
	// incl. its degenerate-UV fallback to the per-vertex UVs). Returns false if the
	// face has no usable UV gradient.
	auto faceTangent = [](const Face *F, Vector &t) -> bool {
		const Vector &p0 = F->A->Pos, &p1 = F->B->Pos, &p2 = F->C->Pos;
		const float e1x=p1.x-p0.x,e1y=p1.y-p0.y,e1z=p1.z-p0.z;
		const float e2x=p2.x-p0.x,e2y=p2.y-p0.y,e2z=p2.z-p0.z;
		float du1=F->U2-F->U1, dv1=F->V2-F->V1, du2=F->U3-F->U1, dv2=F->V3-F->V1;
		if (std::fabs(du1*dv2 - du2*dv1) < 1e-8f) {
			const float vdu1=F->B->U-F->A->U, vdv1=F->B->V-F->A->V, vdu2=F->C->U-F->A->U, vdv2=F->C->V-F->A->V;
			if (std::fabs(vdu1*vdv2 - vdu2*vdv1) >= 1e-8f) { du1=vdu1;dv1=vdv1;du2=vdu2;dv2=vdv2; }
		}
		const float denom = du1*dv2 - du2*dv1;
		if (std::fabs(denom) < 1e-8f) return false;
		const float r = 1.0f/denom;
		t.x=(e1x*dv2-e2x*dv1)*r; t.y=(e1y*dv2-e2y*dv1)*r; t.z=(e1z*dv2-e2z*dv1)*r;
		return true;
	};

	// Weld BOTH the normal AND the tangent over each corner's gated position bucket.
	// A split mesh (greets, post-MakeFacesIndependent) gives every corner its OWN
	// per-face tangent, so coincident corners get DIFFERENT tangents (per-cell
	// bilinear UV + displaced positions differ per triangle) → a TBN flip at every
	// edge → a normal-map/POM seam that survives even continuous normals (visible
	// at any amp). Averaging the tangent over the same gated neighbourhood as the
	// normal makes the whole TBN frame continuous, so the last hairlines go too.
	std::vector<Vector> outN(corners.size()), outT(corners.size());
	for (int i = 0; i < int(corners.size()); ++i) {
		const Vector &fN = corners[i].F->N;
		Vector nAcc, tAcc; Vector_Form(&nAcc, 0, 0, 0); Vector_Form(&tAcc, 0, 0, 0);
		for (int j : bucket[keyOf(corners[i].V->Pos)]) {
			const Face *adj = corners[j].F;
			if (Dot_Product(&fN, &adj->N) < cosThr) continue;   // crease: keep hard
			const float area = Tri_Surface(&adj->A->Pos, &adj->B->Pos, &adj->C->Pos);
			Vector w; Vector_Scale(&adj->N, area, &w); Vector_SelfAdd(&nAcc, &w);
			Vector ta; if (faceTangent(adj, ta)) { Vector_Scale(&ta, area, &w); Vector_SelfAdd(&tAcc, &w); }
		}
		Vector n;
		if (Vector_Length(&nAcc) < EPSILON) n = fN; else { Vector_Norm(&nAcc); n = nAcc; }
		outN[i] = n;
		// Gram-Schmidt the tangent against the smoothed normal (T ⟂ N), like PREPROC.
		const float TdotN = Dot_Product(&tAcc, &n);
		tAcc.x -= TdotN*n.x; tAcc.y -= TdotN*n.y; tAcc.z -= TdotN*n.z;
		if (Vector_Length(&tAcc) > EPSILON) { Vector_Norm(&tAcc); outT[i] = tAcc; }
		else {   // no usable UV gradient: any vector ⟂ N (matches PREPROC's fallback)
			if (std::fabs(n.y) < 0.9f) { outT[i].x=-n.z; outT[i].y=0.0f; outT[i].z=n.x; }
			else                       { outT[i].x=0.0f; outT[i].y=n.z; outT[i].z=-n.y; }
			Vector_Norm(&outT[i]);
		}
	}
	// Commit (split mesh: exactly one corner per Vertex).
	for (int i = 0; i < int(corners.size()); ++i) { corners[i].V->N = outN[i]; corners[i].V->Tangent = outT[i]; }
	std::fprintf(stderr, "[STONE] '%s' re-smoothed %zu displaced corners @ %.0f deg "
	             "(weld-aware normals + tangents; hard creases + material borders kept)\n",
	             matName, corners.size(), (double)smoothAngleDeg);
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
// ── S1d-1 PATCH-BOUNDARY TOPOLOGY + SEAM CLASSIFICATION ─────────────────────
// (--pom_seam_census, DIAGNOSTIC, default OFF, docs/S1D_CLOSED_SHELL_PLAN.md)
//
// The march has no valid answer once a ray leaves its patch's UV domain: killing
// it gives holes, clamping gives grazing smears, and both are the SAME pixels.
// The literature's answer is a CLOSED shell where a ray exits only at a true
// silhouette. Which of the two repairs — cross-patch march continuation (A) or
// side faces at true boundaries (B) — is worth building depends on what the
// patch boundaries actually ARE, and nobody had counted them. This is that
// count. It changes no pixel: it only reads the mesh and prints.
//
// Everything here keys on POSITION (quantized to 1e-4 world), never on face
// index: face indices are not comparable across arms (the Piramid chunk split
// re-bins them) and MakeFacesIndependentByAngle has already duplicated the
// vertex OBJECTS at every crease, so pointer identity is not adjacency either.
// It must therefore run with the lid offset ZERO (--pom_recess_only or
// --pom_shell_lid_probe): a non-zero offset moves the two copies of a corner
// vertex along DIFFERENT smooth normals and position coincidence — the whole
// basis of the topology — is gone. PomShell_Build warns when that is violated.
namespace {

struct SeamFace {
	TriMesh        *T   = nullptr;
	int32_t         fi  = 0;
	const Material *M   = nullptr;
	Vector          P[3];              // AUTHORED positions (captured pre-move)
	float           U[3] = {0,0,0}, V[3] = {0,0,0};
	Vector          N;                 // authored plane normal
	float           d   = 0.0f;        // authored plane constant (N·P + d = 0)
	Vector          dPdu, dPdv;        // chart basis (0 length ⇒ degenerate chart)
	bool            chartOk = false;
	Vector          bbLo, bbHi;        // AABB, so the outward probe can reject fast
};

struct SeamEdgeKey {
	long long a, b;
	bool operator<(const SeamEdgeKey &o) const { return a != o.a ? a < o.a : b < o.b; }
};

enum SeamClass {
	SC_COPLANAR = 0,    // the surface carries on in the same plane — a hole here is always wrong
	SC_ANGLED_IN,       // concave fold: the neighbouring surface is what the ray runs into
	SC_ANGLED_OUT,      // convex fold: the surface turns away — exit is a TRUE silhouette
	SC_TRUE,            // nothing continues — the free edge where side faces belong
	SC_N
};
static const char *SeamClassName(int c) {
	switch (c) {
	case SC_COPLANAR:   return "COPLANAR";
	case SC_ANGLED_IN:  return "ANGLED_IN";
	case SC_ANGLED_OUT: return "ANGLED_OUT";
	default:            return "TRUE_BOUNDARY";
	}
}

// Solve a face's UV chart: P(u,v) = P0 + dPdu·(u−U1) + dPdv·(v−V1). Same Lengyel
// inversion the amplitude census and the shell's own density solve use.
static bool SeamChart(SeamFace &f) {
	const float du1 = f.U[1] - f.U[0], dv1 = f.V[1] - f.V[0];
	const float du2 = f.U[2] - f.U[0], dv2 = f.V[2] - f.V[0];
	const float det = du1 * dv2 - du2 * dv1;
	if (std::fabs(det) < 1e-12f) return false;
	const float inv = 1.0f / det;
	const Vector e1 = { f.P[1].x-f.P[0].x, f.P[1].y-f.P[0].y, f.P[1].z-f.P[0].z };
	const Vector e2 = { f.P[2].x-f.P[0].x, f.P[2].y-f.P[0].y, f.P[2].z-f.P[0].z };
	f.dPdu = { (e1.x*dv2 - e2.x*dv1)*inv, (e1.y*dv2 - e2.y*dv1)*inv, (e1.z*dv2 - e2.z*dv1)*inv };
	f.dPdv = { (e2.x*du1 - e1.x*du2)*inv, (e2.y*du1 - e1.y*du2)*inv, (e2.z*du1 - e1.z*du2)*inv };
	return true;
}

// Inverse chart: world point (assumed on/near the face's plane) → (u,v).
static void SeamUVofPoint(const SeamFace &f, const Vector &x, float &u, float &v) {
	const Vector r = { x.x - f.P[0].x, x.y - f.P[0].y, x.z - f.P[0].z };
	const float a = f.dPdu.x*f.dPdu.x + f.dPdu.y*f.dPdu.y + f.dPdu.z*f.dPdu.z;
	const float b = f.dPdu.x*f.dPdv.x + f.dPdu.y*f.dPdv.y + f.dPdu.z*f.dPdv.z;
	const float c = f.dPdv.x*f.dPdv.x + f.dPdv.y*f.dPdv.y + f.dPdv.z*f.dPdv.z;
	const float p = f.dPdu.x*r.x + f.dPdu.y*r.y + f.dPdu.z*r.z;
	const float q = f.dPdv.x*r.x + f.dPdv.y*r.y + f.dPdv.z*r.z;
	const float det = a*c - b*b;
	if (std::fabs(det) < 1e-20f) { u = f.U[0]; v = f.V[0]; return; }
	u = f.U[0] + ( c*p - b*q) / det;
	v = f.V[0] + (-b*p + a*q) / det;
}

static Vector SeamCentroid(const SeamFace &f) {
	return { (f.P[0].x+f.P[1].x+f.P[2].x)/3.0f,
	         (f.P[0].y+f.P[1].y+f.P[2].y)/3.0f,
	         (f.P[0].z+f.P[1].z+f.P[2].z)/3.0f };
}

// Point-in-triangle within tol of the face's plane (the outward probe's test).
static bool SeamPointOnFace(const SeamFace &f, const Vector &Q, float planeTol) {
	const float pd = f.N.x*Q.x + f.N.y*Q.y + f.N.z*Q.z + f.d;
	if (std::fabs(pd) > planeTol) return false;
	// Project out the plane distance, then barycentric.
	const Vector X = { Q.x - f.N.x*pd, Q.y - f.N.y*pd, Q.z - f.N.z*pd };
	const Vector v0 = { f.P[1].x-f.P[0].x, f.P[1].y-f.P[0].y, f.P[1].z-f.P[0].z };
	const Vector v1 = { f.P[2].x-f.P[0].x, f.P[2].y-f.P[0].y, f.P[2].z-f.P[0].z };
	const Vector v2 = { X.x-f.P[0].x, X.y-f.P[0].y, X.z-f.P[0].z };
	const float d00 = v0.x*v0.x+v0.y*v0.y+v0.z*v0.z;
	const float d01 = v0.x*v1.x+v0.y*v1.y+v0.z*v1.z;
	const float d11 = v1.x*v1.x+v1.y*v1.y+v1.z*v1.z;
	const float d20 = v2.x*v0.x+v2.y*v0.y+v2.z*v0.z;
	const float d21 = v2.x*v1.x+v2.y*v1.y+v2.z*v1.z;
	const float den = d00*d11 - d01*d01;
	if (std::fabs(den) < 1e-20f) return false;
	const float bv = (d11*d20 - d01*d21) / den;
	const float bw = (d00*d21 - d01*d20) / den;
	const float bu = 1.0f - bv - bw;
	const float e = -1e-3f;
	return bu >= e && bv >= e && bw >= e;
}

}  // namespace

// ── S1b POM SHELL builder (docs/S1_PIXEL_DISPLACEMENT_PLAN.md) ──────────────
// Turn matName's flat faces into the LID of a relief slab: push every vertex
// they use OUT along its normal by half the relief amplitude and stamp
// Vertex::ShellH with the slab height it landed at (1 = lid). The rasterizer's
// shell march (--pom_shell) then enters through this surface and marches DOWN
// through the slab, discarding rays that leave the patch — which is what makes
// silhouettes. See Material::PomShellUvAmp for why the amplitude is carried in
// UV units and not world units.
//
// The offset is per FACE UV DENSITY: amp (UV units) × that face's
// world-per-UV-tile (the Lengyel |dP/du|,|dP/dv| geometric mean — the same
// solve the rasterizer's depth write does per triangle, computed here in OBJECT
// space, which matches as long as the mesh scale is uniform). A vertex shared by
// faces of differing density or normal direction (a hard corner) gets the mean,
// and ShellH records the height it ACTUALLY reached relative to its incident
// faces' planes (0.5 + mean(N_v·N_face)/2) — so a corner or a pinned border
// enters the march at its true geometric height instead of a fictitious h = 1.
// That is the whole point of carrying ShellH as an interpolant.
//
// pinCrossMaterial: leave verts that non-target faces also use where they are
// (ShellH 0.5, no offset) so a cross-material junction can't be pulled apart.
// Costs relief near those borders and, on un-subdivided quads, can pin an
// entire face — hence a flag, not a default.
//
// Run AFTER the vertex normals are final (MakeFacesIndependentByAngle) and
// before any chunk split (both copy Vertex/Face structs wholesale, so ShellH
// and the moved positions propagate). Returns the amp stamped on the material
// (0 = nothing built).
float PomShell_Build(Scene *Sc, const char *matName, float uvAmp,
                     bool pinCrossMaterial) {
	if (!Sc || !matName || uvAmp <= 0.0f) return 0.0f;
	auto isTarget = [&](const Face *F) {
		return F && F->Txtr && F->Txtr->Name && !std::strcmp(F->Txtr->Name, matName);
	};
	Material *mat = nullptr;
	long long nMoved = 0, nPinned = 0, nCorner = 0, nFaces = 0;
	// ── PATCH GROUPING (the lateral-exit domain) ────────────────────────────
	// The march discards a ray that leaves the patch's UV box before crossing
	// the height field — that discard IS the silhouette. The patch must
	// therefore be the whole CONTIGUOUS COPLANAR wall, not the authored quad:
	// measured on greets, wall quads are 0.4-2.1 UV tiles wide while a grazing
	// shell ray travels up to amp*cap ~= 0.24 UV, so per-quad domains fired the
	// discard mid-wall and punched holes in every wall and the whole floor.
	// Union-find over target faces joined by a POSITION-COINCIDENT edge with
	// (near-)equal planes; the group's UV box is the union of its faces'. A
	// coplanar neighbour across a UV chart SEAM merges too — the failure mode
	// there is a missing discard (a smear, exactly like plain POM), never a
	// hole, which is the right way round.
	struct EdgeKey {
		long long a, b;
		bool operator<(const EdgeKey &o) const { return a != o.a ? a < o.a : b < o.b; }
	};
	auto qpos = [](const Vector &p) {
		auto q = [](float v) { return (long long)std::llround(v * 10000.0); };
		// Pack 3 quantized coords into one key (21 bits each, +-1e5 range).
		return ((q(p.x) & 0x1FFFFF) << 42) ^ ((q(p.y) & 0x1FFFFF) << 21) ^ (q(p.z) & 0x1FFFFF);
	};
	std::vector<int> uf;                       // union-find parent, per (mesh,face)
	std::function<int(int)> find = [&](int x) {
		while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; }
		return x;
	};
	struct FaceRef { TriMesh *T; int fi; };
	std::vector<FaceRef> refs;
	// The AUTHORED plane of each target face, snapshotted here because the
	// vertex move below re-planes every face onto the LID — whose plane constant
	// varies with the per-face UV density (measured on greets' floor: one
	// authored plane becomes six lid planes 0.0087 world apart, which silently
	// fails any 1e-3 coplanarity test done afterwards).
	std::vector<std::array<float,4>> refPlane;
	// --pom_shell_census (DIAGNOSTIC, default OFF): per-face UV-density census.
	// Filled here because this loop still sees the AUTHORED positions — the
	// vertex move below is what the census exists to characterise.
	// --pom_shell_world_amp (P0, default OFF) needs the same per-face table, so
	// the fill below runs for either flag.
	const bool worldAmpMode = fds::FeatureFlags::pom_shell_world_amp();
	// --pom_recess_only (P2-A): build the shell WITHOUT moving anything. The
	// patch grouping, the sibling boxes, the amplitude and every kernel path are
	// unchanged; only the lid offset and the entry height differ, so the
	// offscreen consumers (shadow bake, mirror RTT, env probes) see exactly the
	// authored wall — C6 goes to zero by construction rather than by tuning.
	const bool recessOnly = fds::FeatureFlags::pom_recess_only();
	const bool censusPrint = fds::FeatureFlags::pom_shell_census();
	const bool census = censusPrint || worldAmpMode;      // fill the table
	// --pom_seam_census / --pom_seam_viz (S1d-1): snapshot EVERY face in the
	// scene, authored positions and all, so the boundary classification below
	// runs on the mesh as authored. Captured here for the same reason the
	// amplitude census is: the vertex move further down is what would destroy it.
	const bool seamCensus = fds::FeatureFlags::pom_seam_census()
	                     || fds::FeatureFlags::pom_seam_viz() > 0;
	std::vector<SeamFace> seamAll;
	std::map<SeamEdgeKey, std::vector<int>> seamEdges;
	struct CensusFace { float wu, wv, w, area; };
	std::vector<CensusFace> cenF;                       // parallel to refs
	std::map<std::pair<const TriMesh*, int32_t>, int> refIdx;
	{
		std::map<EdgeKey, std::vector<int>> edges;
		for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
			if (T->FIndex == 0 || !T->Faces) continue;
			for (int32_t i = 0; i < T->FIndex; ++i) {
				Face &F = T->Faces[i];
				if (!F.A || !F.B || !F.C || !isTarget(&F)) continue;
				const int id = int(refs.size());
				refs.push_back({ T, i });
				refPlane.push_back({ F.N.x, F.N.y, F.N.z, F.NormProd });
				uf.push_back(id);
				if (census) {
					refIdx[{ T, i }] = id;
					// Authored-position Lengyel solve, per AXIS (the shell's own
					// solve keeps only the geometric mean).
					const Vector &pa = F.A->Pos, &pb = F.B->Pos, &pc = F.C->Pos;
					const float du1 = F.U2 - F.U1, dv1 = F.V2 - F.V1;
					const float du2 = F.U3 - F.U1, dv2 = F.V3 - F.V1;
					const float det = du1 * dv2 - du2 * dv1;
					CensusFace cf{ 0.0f, 0.0f, 0.0f, 0.0f };
					{   // object-space triangle area — the weight for the median w
						const float ax = pb.x-pa.x, ay = pb.y-pa.y, az = pb.z-pa.z;
						const float bx0 = pc.x-pa.x, by0 = pc.y-pa.y, bz0 = pc.z-pa.z;
						const float cx0 = ay*bz0 - az*by0;
						const float cy0 = az*bx0 - ax*bz0;
						const float cz0 = ax*by0 - ay*bx0;
						cf.area = 0.5f * std::sqrt(cx0*cx0 + cy0*cy0 + cz0*cz0);
					}
					if (std::fabs(det) > 1e-12f) {
						const float inv = 1.0f / det;
						const float e1x = pb.x-pa.x, e1y = pb.y-pa.y, e1z = pb.z-pa.z;
						const float e2x = pc.x-pa.x, e2y = pc.y-pa.y, e2z = pc.z-pa.z;
						const float tx = (e1x*dv2 - e2x*dv1) * inv;
						const float ty = (e1y*dv2 - e2y*dv1) * inv;
						const float tz = (e1z*dv2 - e2z*dv1) * inv;
						const float bx = (e2x*du1 - e1x*du2) * inv;
						const float by = (e2y*du1 - e1y*du2) * inv;
						const float bz = (e2z*du1 - e1z*du2) * inv;
						cf.wu = std::sqrt(tx*tx + ty*ty + tz*tz);
						cf.wv = std::sqrt(bx*bx + by*by + bz*bz);
						cf.w  = std::sqrt(cf.wu * cf.wv);
					}
					cenF.push_back(cf);
				}
				const Vector *pv[3] = { &F.A->Pos, &F.B->Pos, &F.C->Pos };
				for (int e = 0; e < 3; ++e) {
					long long ka = qpos(*pv[e]), kb = qpos(*pv[(e + 1) % 3]);
					if (ka > kb) std::swap(ka, kb);
					edges[{ ka, kb }].push_back(id);
				}
			}
		}
		// S1d-1: the same walk, over EVERY face of every mesh (not just this
		// material's), because a patch boundary's neighbour is usually a
		// DIFFERENT surface — the ceiling, the floor, a column — and the
		// classification has to see it. Positions are still authored here.
		if (seamCensus) {
			for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
				if (T->FIndex == 0 || !T->Faces) continue;
				for (int32_t i = 0; i < T->FIndex; ++i) {
					Face &F = T->Faces[i];
					if (!F.A || !F.B || !F.C) continue;
					SeamFace sf;
					sf.T = T; sf.fi = i; sf.M = F.Txtr;
					sf.P[0] = F.A->Pos; sf.P[1] = F.B->Pos; sf.P[2] = F.C->Pos;
					sf.U[0] = F.U1; sf.V[0] = F.V1;
					sf.U[1] = F.U2; sf.V[1] = F.V2;
					sf.U[2] = F.U3; sf.V[2] = F.V3;
					sf.N = F.N; sf.d = F.NormProd;
					sf.chartOk = SeamChart(sf);
					sf.bbLo = { std::min({sf.P[0].x,sf.P[1].x,sf.P[2].x}),
					            std::min({sf.P[0].y,sf.P[1].y,sf.P[2].y}),
					            std::min({sf.P[0].z,sf.P[1].z,sf.P[2].z}) };
					sf.bbHi = { std::max({sf.P[0].x,sf.P[1].x,sf.P[2].x}),
					            std::max({sf.P[0].y,sf.P[1].y,sf.P[2].y}),
					            std::max({sf.P[0].z,sf.P[1].z,sf.P[2].z}) };
					const int id = int(seamAll.size());
					seamAll.push_back(sf);
					for (int e = 0; e < 3; ++e) {
						long long ka = qpos(sf.P[e]), kb = qpos(sf.P[(e + 1) % 3]);
						if (ka > kb) std::swap(ka, kb);
						seamEdges[{ ka, kb }].push_back(id);
					}
				}
			}
		}
		for (const auto &kv : edges) {
			for (size_t j = 1; j < kv.second.size(); ++j) {
				const Face &F0 = refs[kv.second[0]].T->Faces[refs[kv.second[0]].fi];
				const Face &Fj = refs[kv.second[j]].T->Faces[refs[kv.second[j]].fi];
				const float nd = F0.N.x*Fj.N.x + F0.N.y*Fj.N.y + F0.N.z*Fj.N.z;
				if (nd < 0.999f) continue;                       // not coplanar-parallel
				if (std::fabs(F0.NormProd - Fj.NormProd) > 1e-3f) continue;   // parallel but offset
				const int ra = find(kv.second[0]), rb = find(kv.second[j]);
				if (ra != rb) uf[ra] = rb;
			}
		}
	}
	float offMin = 1e30f, offMax = -1e30f, wMin = 1e30f, wMax = -1e30f;
	float hMinStamp = 1e30f;
	// --pom_shell_census: what the vertex move BELOW actually did, per face.
	std::vector<float> cenOffMin, cenOffMax, cenOffMean;
	std::vector<std::array<float,4>> cenLidPlane;
	if (census) {
		cenOffMin.assign(refs.size(), 0.0f);
		cenOffMax.assign(refs.size(), 0.0f);
		cenOffMean.assign(refs.size(), 0.0f);
		cenLidPlane.assign(refs.size(), { 0,0,0,0 });
	}
	// ── --pom_shell_world_amp (P0, default OFF) ─────────────────────────────
	// One WORLD amplitude for the whole material instead of one UV amplitude
	// converted per face. Derived (not invented) as uvAmp × the AREA-WEIGHTED
	// MEDIAN world-per-UV, so the material's typical slab depth is unchanged and
	// the A/B isolates the per-face DISTRIBUTION; --pom_shell_world_amp_set
	// overrides it with an explicit world number for every material at once.
	float worldAmp = 0.0f, wRef = 0.0f;
	if (worldAmpMode) {
		std::vector<std::pair<float,float>> ws;      // (w, area)
		double totA = 0.0;
		for (const auto &c : cenF)
			if (c.w > 0.0f && c.area > 0.0f) { ws.push_back({ c.w, c.area }); totA += c.area; }
		std::sort(ws.begin(), ws.end());
		double acc = 0.0;
		for (const auto &p : ws) { acc += p.second; if (acc >= 0.5 * totA) { wRef = p.first; break; } }
		if (wRef <= 0.0f && !ws.empty()) wRef = ws[ws.size()/2].first;
		const float forced = fds::FeatureFlags::pom_shell_world_amp_set();
		worldAmp = (forced > 0.0f) ? forced : (uvAmp * wRef);
		int exact = 0;
		for (const auto &c : cenF) if (c.w > 0.0f && std::fabs(c.w - wRef) < 1e-4f) ++exact;
		std::fprintf(stderr, "[POM-SHELL-WORLDAMP] '%s': worldAmp=%.4f world "
			"(%s; uvAmp=%.4f x area-weighted median w=%.4f); %d of %zu faces have "
			"w == wRef so their surface is UNCHANGED by this flag\n",
			matName, (double)worldAmp,
			forced > 0.0f ? "--pom_shell_world_amp_set" : "derived per material",
			(double)uvAmp, (double)wRef, exact, cenF.size());
	}
	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next) {
		if (T->FIndex == 0 || !T->Faces || !T->Verts) continue;
		Vertex *const V = T->Verts;
		auto vidx = [&](const Vertex *v) { return uint32_t(v - V); };
		const uint32_t nV = uint32_t(T->VIndex);
		std::vector<float>  wSum(nV, 0.0f);      // world-per-UV over incident target faces
		std::vector<float>  ndSum(nV, 0.0f);     // N_v·N_face over the same
		std::vector<int>    cnt(nV, 0);
		std::vector<char>   nonTarget(nV, 0);
		std::vector<float>  offVert;                 // census: offset actually applied
		if (census) offVert.assign(nV, 0.0f);
		int nTargetHere = 0;
		for (int32_t i = 0; i < T->FIndex; ++i) {
			Face &F = T->Faces[i];
			if (!F.A || !F.B || !F.C) continue;
			const uint32_t vi[3] = { vidx(F.A), vidx(F.B), vidx(F.C) };
			if (vi[0] >= nV || vi[1] >= nV || vi[2] >= nV) continue;   // defensive
			if (!isTarget(&F)) {
				nonTarget[vi[0]] = nonTarget[vi[1]] = nonTarget[vi[2]] = 1;
				continue;
			}
			++nTargetHere;
			if (!mat) mat = F.Txtr;
			// world(object)-per-UV-tile for this face: invert the UV Jacobian to
			// get dP/du, dP/dv, take the geometric mean of their lengths (the
			// march applies ONE amplitude to both axes).
			const Vector &pa = F.A->Pos, &pb = F.B->Pos, &pc = F.C->Pos;
			const float du1 = F.U2 - F.U1, dv1 = F.V2 - F.V1;
			const float du2 = F.U3 - F.U1, dv2 = F.V3 - F.V1;
			const float det = du1 * dv2 - du2 * dv1;
			float w = 0.0f;
			if (std::fabs(det) > 1e-12f) {
				const float inv = 1.0f / det;
				const float e1x = pb.x - pa.x, e1y = pb.y - pa.y, e1z = pb.z - pa.z;
				const float e2x = pc.x - pa.x, e2y = pc.y - pa.y, e2z = pc.z - pa.z;
				const float tx = (e1x * dv2 - e2x * dv1) * inv;
				const float ty = (e1y * dv2 - e2y * dv1) * inv;
				const float tz = (e1z * dv2 - e2z * dv1) * inv;
				const float bx = (e2x * du1 - e1x * du2) * inv;
				const float by = (e2y * du1 - e1y * du2) * inv;
				const float bz = (e2z * du1 - e1z * du2) * inv;
				const float t2 = tx*tx + ty*ty + tz*tz;
				const float b2 = bx*bx + by*by + bz*bz;
				w = std::sqrt(std::sqrt(t2 * b2));
			}
			if (w <= 0.0f) continue;             // degenerate chart: no lid here
			if (w < wMin) wMin = w;
			if (w > wMax) wMax = w;
			++nFaces;
			for (int k = 0; k < 3; ++k) {
				const Vector &vn = V[vi[k]].N;
				const float nl = std::sqrt(vn.x*vn.x + vn.y*vn.y + vn.z*vn.z);
				const float nd = (nl > 1e-6f)
				    ? ((vn.x*F.N.x + vn.y*F.N.y + vn.z*F.N.z) / nl) : 0.0f;
				wSum[vi[k]]  += w;
				ndSum[vi[k]] += nd;
				cnt[vi[k]]   += 1;
			}
		}
		if (nTargetHere == 0) continue;
		for (uint32_t i = 0; i < nV; ++i) {
			if (cnt[i] == 0) continue;
			if (pinCrossMaterial && nonTarget[i]) { ++nPinned; continue; }
			const Vector &vn = V[i].N;
			const float nl = std::sqrt(vn.x*vn.x + vn.y*vn.y + vn.z*vn.z);
			if (nl < 1e-6f) { ++nPinned; continue; }
			const float wv  = wSum[i]  / float(cnt[i]);
			const float ndv = ndSum[i] / float(cnt[i]);
			// --pom_shell_lid_probe (DIAGNOSTIC, default OFF): force the lid
			// offset to ZERO. Everything else about the shell — ShellH, the
			// material amplitude, the patch domains, the sibling boxes, every
			// kernel path — is left bit-for-bit identical, so differencing this
			// against the real shell attributes a discrepancy to the MOVED
			// VERTICES (it vanishes) or to the MARCH (it survives). Not a
			// rendering proposal: with the geometry unmoved the relief hangs
			// entirely below the authored plane instead of straddling it.
			// --pom_shell_world_amp: half the WORLD amplitude, the same on every
			// vertex, so one authored plane produces exactly one lid plane.
			// --pom_recess_only (P2-A): no offset at all. The authored wall IS
			// the surface; the relief carves inward from it.
			const float off = (fds::FeatureFlags::pom_shell_lid_probe()
			                   || recessOnly)
			                ? 0.0f
			                : (worldAmpMode ? (worldAmp * 0.5f) : (uvAmp * wv * 0.5f));
			V[i].Pos.x += vn.x / nl * off;
			V[i].Pos.y += vn.y / nl * off;
			V[i].Pos.z += vn.z / nl * off;
			if (census) offVert[i] = off;
			// Height actually reached, measured against the incident faces'
			// planes (ndv = 1 on a flat patch → exactly the lid at h = 1).
			// --pom_recess_only: the vertex was not moved, so it lies EXACTLY on
			// every incident face's plane, and under the recess convention that
			// plane is the top of the field — h = 1, with no corner correction
			// (the ndv term models how much of an OFFSET a smooth normal
			// delivered perpendicular to a given face; with no offset it would
			// only invent a fictitious entry height below the real surface).
			V[i].ShellH = recessOnly ? 1.0f : (0.5f + 0.5f * ndv);
			if (V[i].ShellH < hMinStamp) hMinStamp = V[i].ShellH;
			if (ndv < 0.99f) ++nCorner;
			if (off < offMin) offMin = off;
			if (off > offMax) offMax = off;
			++nMoved;
		}
		// Re-plane the moved faces (NormProd is a plane CONSTANT — stale values
		// mis-cull at grazing, the B1 lesson). N itself is unchanged for a rigid
		// offset but re-derive it anyway for the corner-averaged cases.
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
			if (census) {
				auto it = refIdx.find({ T, i });
				if (it != refIdx.end()) {
					const uint32_t va = vidx(F.A), vb = vidx(F.B), vc = vidx(F.C);
					const float o[3] = { (va < nV ? offVert[va] : 0.0f),
					                     (vb < nV ? offVert[vb] : 0.0f),
					                     (vc < nV ? offVert[vc] : 0.0f) };
					cenOffMin [it->second] = std::min({ o[0], o[1], o[2] });
					cenOffMax [it->second] = std::max({ o[0], o[1], o[2] });
					cenOffMean[it->second] = (o[0] + o[1] + o[2]) / 3.0f;
					cenLidPlane[it->second] = { F.N.x, F.N.y, F.N.z, F.NormProd };
				}
			}
		}
		// The lid sticks out past the authored bsphere by at most the offset.
		if (offMax > 0.0f) {
			T->BSphereRadius += offMax;
			T->BSphereRad     = T->BSphereRadius * T->BSphereRadius;
		}
	}
	if (!mat || nMoved == 0) {
		std::fprintf(stderr, "[POM-SHELL] '%s': nothing built (%lld target faces, "
		             "%lld verts pinned) — no shell\n", matName, nFaces, nPinned);
		return 0.0f;
	}
	// Collapse the union-find into 1-based group ids, union each group's UV box
	// (from the AUTHORED per-face UVs, which the rasterizer also reads), and
	// publish the table on the material.
	double domUMin = 1e30, domUMax = -1e30, domVMin = 1e30, domVMax = -1e30;
	unsigned nGroups = 0;
	{
		std::map<int, int> rootToGroup;
		std::vector<float> table;
		for (size_t i = 0; i < refs.size(); ++i) {
			const int r = find(int(i));
			auto it = rootToGroup.find(r);
			int g;
			if (it == rootToGroup.end()) {
				g = int(rootToGroup.size()) + 1;
				rootToGroup[r] = g;
				table.insert(table.end(), { 1e30f, -1e30f, 1e30f, -1e30f });
			} else {
				g = it->second;
			}
			Face &F = refs[i].T->Faces[refs[i].fi];
			F.PomShellGroup = uint16_t(g);
			float *d = table.data() + 4 * (g - 1);
			d[0] = std::min({ d[0], F.U1, F.U2, F.U3 });
			d[1] = std::max({ d[1], F.U1, F.U2, F.U3 });
			d[2] = std::min({ d[2], F.V1, F.V2, F.V3 });
			d[3] = std::max({ d[3], F.V1, F.V2, F.V3 });
		}
		nGroups = unsigned(rootToGroup.size());
		if (nGroups > 65535) {   // uint16 group id
			std::fprintf(stderr, "[POM-SHELL] '%s': %u patches exceeds the 16-bit "
			             "group id — domains disabled for this material\n", matName, nGroups);
			for (auto &rf : refs) rf.T->Faces[rf.fi].PomShellGroup = 0;
			nGroups = 0;
		} else if (nGroups) {
			mat->PomShellDomains = new float[table.size()];
			std::memcpy(mat->PomShellDomains, table.data(), table.size() * sizeof(float));
			mat->PomShellDomainCount = nGroups;
			for (unsigned g = 0; g < nGroups; ++g) {
				domUMin = std::min(domUMin, (double)table[4*g+0]);
				domUMax = std::max(domUMax, (double)table[4*g+1]);
				domVMin = std::min(domVMin, (double)table[4*g+2]);
				domVMax = std::max(domVMax, (double)table[4*g+3]);
			}
			// ── PLANE GROUPS: the multi-box domain (--pom_shell_merge_uv) ────
			// Edge adjacency alone leaves ONE physical surface split wherever the
			// authoring interrupts it without sharing an edge. Measured on
			// greets: the floor is one plane cut into 6 patches by the doorway
			// thresholds, and the residual grazing void sat exactly on those
			// cuts — a ray crossing a threshold in UV left its patch and was
			// discarded, although the stone physically continues.
			//
			// The fix is NOT to merge the boxes into their union: that union
			// also swallows the genuine OPENINGS between coplanar patches (a
			// doorway in a wall), and measured at t=6097 it killed the corner
			// silhouette outright (the discard went from correcting 23 k px of
			// lid over-coverage to correcting none). Instead each patch keeps
			// its own tight box and gains a SIBLING LIST: the other patches on
			// its plane whose UV rects abut/overlap within pom_shell_merge_uv.
			// The domain test becomes "inside my box OR inside any sibling's" —
			// the union of the boxes, never their bounding box, so a gap that no
			// patch covers still discards. Runtime cost is one extra compare
			// group per sibling and only for lanes that failed their own box
			// (horizontal_and early-out); bake cost is O(patches^2), trivial at
			// these counts.
			const float mergeUv = fds::FeatureFlags::pom_shell_merge_uv();
			if (mergeUv > 0.0f && nGroups > 1) {
				// Plane of each group (first face's; the union-find guarantees
				// the members agree to the same tolerance used below).
				std::vector<std::array<float,4>> gPlane(nGroups, { 0,0,0,0 });
				std::vector<char> gHave(nGroups, 0);
				for (size_t i = 0; i < refs.size(); ++i) {
					const Face &F = refs[i].T->Faces[refs[i].fi];
					const unsigned g = F.PomShellGroup - 1u;
					if (g < nGroups && !gHave[g]) {
						gPlane[g] = refPlane[i];      // AUTHORED plane, pre-lid
						gHave[g] = 1;
					}
				}
				const float pad = mergeUv * 0.5f;
				// Transitive closure over "same plane AND boxes within pad":
				// a chain of patches across several thresholds is one surface.
				std::vector<int> pg(nGroups);
				for (unsigned g = 0; g < nGroups; ++g) pg[g] = int(g);
				std::function<int(int)> pfind = [&](int x) {
					while (pg[x] != x) { pg[x] = pg[pg[x]]; x = pg[x]; }
					return x;
				};
				auto touches = [&](unsigned a, unsigned b) {
					const auto &pa = gPlane[a], &pb = gPlane[b];
					if (pa[0]*pb[0] + pa[1]*pb[1] + pa[2]*pb[2] < 0.999f) return false;
					if (std::fabs(pa[3] - pb[3]) > 1e-3f) return false;
					const float *A = table.data() + 4*a, *B = table.data() + 4*b;
					if (A[0] - pad > B[1] + pad || B[0] - pad > A[1] + pad) return false;
					if (A[2] - pad > B[3] + pad || B[2] - pad > A[3] + pad) return false;
					return true;
				};
				for (unsigned a = 0; a < nGroups; ++a)
					for (unsigned b = a + 1; b < nGroups; ++b)
						if (touches(a, b)) {
							const int ra = pfind(int(a)), rb = pfind(int(b));
							if (ra != rb) pg[ra] = rb;
						}
				// CSR sibling boxes per group (own box excluded — it is tested
				// first in the kernel, which is what makes the early-out pay).
				std::vector<unsigned> members;
				std::vector<uint32_t> ofs(nGroups + 1, 0);
				std::vector<float> sib;
				long long nSib = 0; unsigned maxSib = 0;
				for (unsigned g = 0; g < nGroups; ++g) {
					ofs[g] = uint32_t(sib.size() / 4);
					const int r = pfind(int(g));
					unsigned k = 0;
					for (unsigned h = 0; h < nGroups; ++h) {
						if (h == g || pfind(int(h)) != r) continue;
						if (k >= Material::kPomShellMaxSibs) break;   // bounded hot loop
						const float *B = table.data() + 4*h;
						sib.insert(sib.end(), { B[0], B[1], B[2], B[3] });
						++k;
					}
					nSib += k; maxSib = std::max(maxSib, k);
				}
				ofs[nGroups] = uint32_t(sib.size() / 4);
				if (!sib.empty()) {
					mat->PomShellSibBoxes = new float[sib.size()];
					std::memcpy(mat->PomShellSibBoxes, sib.data(), sib.size() * sizeof(float));
					mat->PomShellSibOfs = new uint32_t[nGroups + 1];
					std::memcpy(mat->PomShellSibOfs, ofs.data(), (nGroups + 1) * sizeof(uint32_t));
				}
				unsigned nPlaneGroups = 0;
				for (unsigned g = 0; g < nGroups; ++g) if (pfind(int(g)) == int(g)) ++nPlaneGroups;
				std::fprintf(stderr, "[POM-SHELL] '%s': %u patches -> %u plane groups "
					"(merge_uv=%.3f), %lld sibling boxes, max %u/patch%s\n",
					matName, nGroups, nPlaneGroups, (double)mergeUv, nSib, maxSib,
					maxSib >= Material::kPomShellMaxSibs ? " (CLAMPED)" : "");
			}
			// --pom_shell_patch_dump: the per-patch table (this is how you see
			// what --pom_shell_merge_uv did, and which UV seams remain).
			if (fds::FeatureFlags::pom_shell_patch_dump()) {
				std::vector<int> gFaces(nGroups, 0);
				std::vector<std::array<float,4>> gPlane(nGroups, { 0,0,0,0 });
				for (size_t i = 0; i < refs.size(); ++i) {
					const Face &F = refs[i].T->Faces[refs[i].fi];
					if (!F.PomShellGroup) continue;
					const unsigned g = F.PomShellGroup - 1u;
					if (!gFaces[g]) gPlane[g] = { F.N.x, F.N.y, F.N.z, F.NormProd };
					++gFaces[g];
				}
				for (unsigned g = 0; g < nGroups; ++g)
					std::fprintf(stderr,
						"[POM-SHELL-PATCH] '%s' g=%u faces=%d N=(%.3f,%.3f,%.3f) d=%.3f "
						"u[%.4f..%.4f] v[%.4f..%.4f] (%.3f x %.3f UV)\n",
						matName, g + 1, gFaces[g], (double)gPlane[g][0],
						(double)gPlane[g][1], (double)gPlane[g][2], (double)gPlane[g][3],
						(double)table[4*g+0], (double)table[4*g+1],
						(double)table[4*g+2], (double)table[4*g+3],
						(double)(table[4*g+1] - table[4*g+0]),
						(double)(table[4*g+3] - table[4*g+2]));
			}
		}
	}
	// ── S1d-1 SEAM CENSUS (--pom_seam_census / --pom_seam_viz, default OFF) ──
	// Classify every boundary edge of every patch. See the SeamFace block above
	// for the method and for why this must run with the lid offset at zero.
	if (seamCensus && nGroups && !seamAll.empty()) {
		const bool moved = (offMax > 1e-6f);
		if (moved)
			std::fprintf(stderr, "[POM-SEAM] '%s' WARNING: the lid offset is %.4f "
				"world, NOT zero. The angle-split corner vertices have been moved "
				"along DIFFERENT smooth normals, so position coincidence — the whole "
				"basis of this topology — is broken. Re-run with --pom_recess_only "
				"or --pom_shell_lid_probe.\n", matName, (double)offMax);
		const float *dom = mat->PomShellDomains;
		// Per-face group id, read back from the Faces (assigned just above).
		auto grpOf = [&](const SeamFace &sf) -> unsigned {
			if (sf.M != mat) return 0u;
			return (unsigned)sf.T->Faces[sf.fi].PomShellGroup;
		};
		// Per-class tallies, and per-class tallies of who the neighbour was.
		long long nEdge[SC_N] = {0,0,0,0};
		double    lEdge[SC_N] = {0,0,0,0};
		long long nInShelled = 0, nInUnshelled = 0;   // ANGLED_IN neighbour material
		double    lInShelled = 0.0, lInUnshelled = 0.0;
		long long nBackfaceOnly = 0, nProbeFound = 0, nNoNeighbour = 0;
		long long nCoplanarSameEdge = 0, nCoplanarProbe = 0;
		// Per-patch class length, for the [POM-SEAM-PATCH] rows.
		std::vector<std::array<double,SC_N>> pLen(nGroups, {0.0,0.0,0.0,0.0});
		std::vector<std::array<int,SC_N>>    pCnt(nGroups, {0,0,0,0});
		// Distinct UV transforms across CONTINUATION seams (the S1d-2 cost).
		std::map<std::string, long long> xformHist;
		double maxCoplanarUvErr = 0.0;
		int nMirroredChart = 0, nChartOk = 0;
		for (int ai = 0; ai < int(seamAll.size()); ++ai) {
			const SeamFace &fa = seamAll[ai];
			const unsigned g = grpOf(fa);
			if (!g || g > nGroups) continue;             // not a patch face of this material
			for (int e = 0; e < 3; ++e) {
				const int i0 = e, i1 = (e + 1) % 3, i2 = (e + 2) % 3;
				long long ka = qpos(fa.P[i0]), kb = qpos(fa.P[i1]);
				if (ka > kb) std::swap(ka, kb);
				auto it = seamEdges.find({ ka, kb });
				if (it == seamEdges.end()) continue;     // impossible, defensive
				const std::vector<int> &lst = it->second;
				bool interior = false;
				for (int bi : lst)
					if (bi != ai && grpOf(seamAll[bi]) == g) { interior = true; break; }
				if (interior) continue;                  // inside the patch, not a boundary
				// ── this IS a patch-boundary edge ──
				const Vector &Pa = fa.P[i0], &Pb = fa.P[i1], &Pc = fa.P[i2];
				const Vector mid = { (Pa.x+Pb.x)*0.5f, (Pa.y+Pb.y)*0.5f, (Pa.z+Pb.z)*0.5f };
				Vector ed = { Pb.x-Pa.x, Pb.y-Pa.y, Pb.z-Pa.z };
				const float elen = std::sqrt(ed.x*ed.x + ed.y*ed.y + ed.z*ed.z);
				if (elen < 1e-6f) continue;
				ed.x /= elen; ed.y /= elen; ed.z /= elen;
				Vector inw = { Pc.x-mid.x, Pc.y-mid.y, Pc.z-mid.z };
				const float pr = inw.x*ed.x + inw.y*ed.y + inw.z*ed.z;
				inw.x -= ed.x*pr; inw.y -= ed.y*pr; inw.z -= ed.z*pr;
				const float il = std::sqrt(inw.x*inw.x + inw.y*inw.y + inw.z*inw.z);
				if (il < 1e-6f) continue;
				const Vector out = { -inw.x/il, -inw.y/il, -inw.z/il };
				int   clsShared = -1, nbShared = -1;
				bool  sawBackface = false;
				float dotShared = 0.0f;
				int   cls = -1, nbIdx = -1;
				float bestDot = 0.0f;
				auto consider = [&](int bi, bool viaProbe) {
					const SeamFace &fb = seamAll[bi];
					const float nd = fa.N.x*fb.N.x + fa.N.y*fb.N.y + fa.N.z*fb.N.z;
					int c;
					if (nd >= 0.999f && std::fabs(fa.d - fb.d) <= 1e-3f) {
						c = SC_COPLANAR;
					} else if (nd <= -0.999f && std::fabs(fa.d + fb.d) <= 1e-3f) {
						sawBackface = true;              // the sheet's own reverse face
						return;
					} else {
						// A plane hinged on the shared edge lies entirely on ONE
						// side of this patch's plane, so the neighbour centroid's
						// side IS the fold direction: in FRONT of us (concave, the
						// material continues under the ray) or BEHIND (convex, the
						// material ends and the ray leaves the solid).
						const Vector cb = SeamCentroid(fb);
						const float side = fa.N.x*cb.x + fa.N.y*cb.y + fa.N.z*cb.z + fa.d;
						c = (side > 1e-4f) ? SC_ANGLED_IN : SC_ANGLED_OUT;
					}
					if (cls < 0 || c < cls) { cls = c; nbIdx = bi; bestDot = nd;
						if (c == SC_COPLANAR) { if (viaProbe) ++nCoplanarProbe; else ++nCoplanarSameEdge; } }
				};
				for (int bi : lst) if (bi != ai) consider(bi, false);
				clsShared = cls; nbShared = nbIdx; dotShared = bestDot;
				if (clsShared < 0) { if (sawBackface) ++nBackfaceOnly; else ++nNoNeighbour; }
				// A shared-edge partner is EXACT and applies to the whole edge. A
				// probe is a point sample, and greets has 18-world edges whose
				// neighbour changes along their length (the x=17.898 wall carries a
				// coplanar panel over its top half and an opening under it). So
				// probe-classified edges are SUBDIVIDED and each piece classified on
				// its own — without this the whole edge takes the midpoint's class
				// and the screen weighting inherits the error.
				// 0.25 world per piece: the measured screen population at the
				// t=6097 corner sits within 0.8 world of a class transition, so a
				// 1-world subdivision was still coarse enough to mislabel most of it.
				const int K = (clsShared >= 0)
				            ? 1 : std::min(128, std::max(1, int(std::ceil(elen / 0.25f))));
				for (int kseg = 0; kseg < K; ++kseg) {
				const float t0 = float(kseg) / float(K), t1 = float(kseg + 1) / float(K);
				const Vector Psa = { Pa.x + (Pb.x-Pa.x)*t0, Pa.y + (Pb.y-Pa.y)*t0, Pa.z + (Pb.z-Pa.z)*t0 };
				const Vector Psb = { Pa.x + (Pb.x-Pa.x)*t1, Pa.y + (Pb.y-Pa.y)*t1, Pa.z + (Pb.z-Pa.z)*t1 };
				const float tm = 0.5f * (t0 + t1);
				const Vector smid = { Pa.x + (Pb.x-Pa.x)*tm, Pa.y + (Pb.y-Pa.y)*tm, Pa.z + (Pb.z-Pa.z)*tm };
				const float slen = elen / float(K);
				cls = clsShared; nbIdx = nbShared; bestDot = dotShared;
				// No continuing neighbour on the shared edge. Probe outward, in the
				// patch's own plane, for a surface that abuts without sharing an
				// edge — the doorway thresholds and every T-junction live here.
				if (cls < 0) {
					static const float kProbe[3] = { 0.02f, 0.10f, 0.35f };
					for (int s = 0; s < 3 && cls < 0; ++s) {
						const Vector Q = { smid.x + out.x*kProbe[s],
						                   smid.y + out.y*kProbe[s],
						                   smid.z + out.z*kProbe[s] };
						for (int bi = 0; bi < int(seamAll.size()); ++bi) {
							if (bi == ai) continue;
							const SeamFace &fb = seamAll[bi];
							if (Q.x < fb.bbLo.x - 0.06f || Q.x > fb.bbHi.x + 0.06f ||
							    Q.y < fb.bbLo.y - 0.06f || Q.y > fb.bbHi.y + 0.06f ||
							    Q.z < fb.bbLo.z - 0.06f || Q.z > fb.bbHi.z + 0.06f) continue;
							if (grpOf(fb) == g) continue;         // our own patch
							const float nd = fa.N.x*fb.N.x + fa.N.y*fb.N.y + fa.N.z*fb.N.z;
							if (nd <= -0.999f && std::fabs(fa.d + fb.d) <= 1e-3f) continue;  // reverse sheet
							if (!SeamPointOnFace(fb, Q, 0.06f)) continue;
							consider(bi, true);
							if (cls >= 0) { ++nProbeFound; break; }
						}
					}
				}
				if (cls < 0) cls = SC_TRUE;
				nEdge[cls] += 1; lEdge[cls] += slen;
				pCnt[g-1][cls] += 1; pLen[g-1][cls] += slen;
				const Material *nbM = (nbIdx >= 0) ? seamAll[nbIdx].M : nullptr;
				const bool nbShelled = nbM && (nbM->PomShellUvAmp > 0.0f
				                       || (nbM->Name && (!std::strcmp(nbM->Name, "rooms")
				                                      || !std::strcmp(nbM->Name, "floor"))));
				if (cls == SC_ANGLED_IN) {
					if (nbShelled) { ++nInShelled; lInShelled += slen; }
					else           { ++nInUnshelled; lInUnshelled += slen; }
				}
				// UV of the sub-segment in OUR chart (the chart is affine, so the
				// edge's UVs interpolate exactly) — what a hand-off would start from.
				const float uA = fa.U[i0] + (fa.U[i1]-fa.U[i0])*t0;
				const float vA = fa.V[i0] + (fa.V[i1]-fa.V[i0])*t0;
				const float uB = fa.U[i0] + (fa.U[i1]-fa.U[i0])*t1;
				const float vB = fa.V[i0] + (fa.V[i1]-fa.V[i0])*t1;
				// ── the transform a continuation would have to apply ──
				char xf[96] = "n/a";
				if (nbIdx >= 0 && seamAll[nbIdx].chartOk && fa.chartOk) {
					const SeamFace &fb = seamAll[nbIdx];
					float nuA, nvA, nuB, nvB;
					SeamUVofPoint(fb, Psa, nuA, nvA);
					SeamUVofPoint(fb, Psb, nuB, nvB);
					if (cls == SC_COPLANAR) {
						const double err = std::max(std::max(std::fabs(nuA-uA), std::fabs(nvA-vA)),
						                            std::max(std::fabs(nuB-uB), std::fabs(nvB-vB)));
						maxCoplanarUvErr = std::max(maxCoplanarUvErr, err);
						std::snprintf(xf, sizeof(xf), err < 1e-3 ? "IDENTITY" : "COPLANAR_OFFSET");
					} else {
						// Scale ratio of the two charts + handedness. A chart's
						// handedness is the sign of (dPdu × dPdv)·N.
						const Vector ca = { fa.dPdu.y*fa.dPdv.z - fa.dPdu.z*fa.dPdv.y,
						                    fa.dPdu.z*fa.dPdv.x - fa.dPdu.x*fa.dPdv.z,
						                    fa.dPdu.x*fa.dPdv.y - fa.dPdu.y*fa.dPdv.x };
						const Vector cbv= { fb.dPdu.y*fb.dPdv.z - fb.dPdu.z*fb.dPdv.y,
						                    fb.dPdu.z*fb.dPdv.x - fb.dPdu.x*fb.dPdv.z,
						                    fb.dPdu.x*fb.dPdv.y - fb.dPdu.y*fb.dPdv.x };
						const float ha = ca.x*fa.N.x + ca.y*fa.N.y + ca.z*fa.N.z;
						const float hb = cbv.x*fb.N.x + cbv.y*fb.N.y + cbv.z*fb.N.z;
						const float wa = std::sqrt(std::sqrt(
							(fa.dPdu.x*fa.dPdu.x+fa.dPdu.y*fa.dPdu.y+fa.dPdu.z*fa.dPdu.z) *
							(fa.dPdv.x*fa.dPdv.x+fa.dPdv.y*fa.dPdv.y+fa.dPdv.z*fa.dPdv.z)));
						const float wb = std::sqrt(std::sqrt(
							(fb.dPdu.x*fb.dPdu.x+fb.dPdu.y*fb.dPdu.y+fb.dPdu.z*fb.dPdu.z) *
							(fb.dPdv.x*fb.dPdv.x+fb.dPdv.y*fb.dPdv.y+fb.dPdv.z*fb.dPdv.z)));
						const bool mirrored = (ha * hb) < 0.0f;
						if (mirrored) ++nMirroredChart;
						++nChartOk;
						std::snprintf(xf, sizeof(xf), "FOLD%.0fdeg scale%.3f%s",
							(double)(std::acos(std::max(-1.0f, std::min(1.0f, bestDot)))
							         * 57.29578f),
							(double)(wb > 1e-9f ? wa / wb : 0.0f),
							mirrored ? " MIRRORED" : "");
					}
				}
				{
					char kbuf[160];
					std::snprintf(kbuf, sizeof(kbuf), "%s|%s|%s", SeamClassName(cls),
						nbM && nbM->Name ? nbM->Name : "(none)", xf);
					xformHist[kbuf] += 1;
				}
				if (fds::FeatureFlags::pom_seam_census())
					std::fprintf(stderr,
						"[POM-SEAM] '%s' g=%u cls=%s len=%.4f A=(%.4f,%.4f,%.4f) "
						"B=(%.4f,%.4f,%.4f) uvA=(%.5f,%.5f) uvB=(%.5f,%.5f) "
						"nb=%s nbdot=%.4f xf=%s%s\n",
						matName, g, SeamClassName(cls), (double)slen,
						(double)Psa.x, (double)Psa.y, (double)Psa.z,
						(double)Psb.x, (double)Psb.y, (double)Psb.z,
						(double)uA, (double)vA, (double)uB, (double)vB,
						nbM && nbM->Name ? nbM->Name : "(none)", (double)bestDot, xf,
						sawBackface ? " +backface" : "");
				if (fds::FeatureFlags::pom_seam_viz() > 0)
					fds::PomSeamViz_Record(Psa, Psb, cls);
				}   // sub-segment loop
			}
		}
		double lTot = 0.0; long long nTot = 0;
		for (int c = 0; c < SC_N; ++c) { lTot += lEdge[c]; nTot += nEdge[c]; }
		std::fprintf(stderr,
			"[POM-SEAM-SUM] '%s' %u patches, %lld boundary SEGMENTS (edges subdivided where the class was decided by a probe), %.2f world total\n",
			matName, nGroups, nTot, lTot);
		for (int c = 0; c < SC_N; ++c)
			std::fprintf(stderr,
				"[POM-SEAM-SUM] '%s'   %-14s edges=%5lld (%5.1f%%)  length=%9.3f (%5.1f%%)\n",
				matName, SeamClassName(c), nEdge[c],
				nTot ? 100.0*double(nEdge[c])/double(nTot) : 0.0,
				lEdge[c], lTot > 0 ? 100.0*lEdge[c]/lTot : 0.0);
		std::fprintf(stderr,
			"[POM-SEAM-SUM] '%s'   ANGLED_IN neighbour SHELLED: %lld edges / %.3f world; "
			"UNSHELLED: %lld / %.3f\n", matName, nInShelled, lInShelled,
			nInUnshelled, lInUnshelled);
		std::fprintf(stderr,
			"[POM-SEAM-SUM] '%s'   edges whose only shared-edge partner was the "
			"REVERSE sheet: %lld; with no partner at all: %lld; resolved by the "
			"outward probe: %lld\n", matName, nBackfaceOnly, nNoNeighbour, nProbeFound);
		std::fprintf(stderr,
			"[POM-SEAM-SUM] '%s'   COPLANAR found via a shared edge: %lld, via the "
			"probe (T-junction / non-adjacent abutment): %lld; worst UV disagreement "
			"across a coplanar seam: %.6f UV; charts mirrored across an angled seam: "
			"%d of %d\n", matName, nCoplanarSameEdge, nCoplanarProbe,
			maxCoplanarUvErr, nMirroredChart, nChartOk);
		for (const auto &kv : xformHist)
			std::fprintf(stderr, "[POM-SEAM-XFORM] '%s' %-64s x%lld\n",
				matName, kv.first.c_str(), kv.second);
		if (fds::FeatureFlags::pom_seam_census()) {
			for (unsigned g = 0; g < nGroups; ++g)
				std::fprintf(stderr,
					"[POM-SEAM-PATCH] '%s' g=%u cop=%d/%.3f in=%d/%.3f out=%d/%.3f "
					"true=%d/%.3f\n", matName, g + 1,
					pCnt[g][0], pLen[g][0], pCnt[g][1], pLen[g][1],
					pCnt[g][2], pLen[g][2], pCnt[g][3], pLen[g][3]);
			// Machine-readable domain table for the offline screen-weighting:
			// each patch's own UV box followed by its sibling boxes (the domain
			// the kernel actually tests is the UNION of them).
			for (unsigned g = 0; g < nGroups; ++g) {
				std::fprintf(stderr, "[POM-SEAM-DOMAIN] '%s' g=%u box=%.6f,%.6f,%.6f,%.6f",
					matName, g + 1, (double)dom[4*g+0], (double)dom[4*g+1],
					(double)dom[4*g+2], (double)dom[4*g+3]);
				if (mat->PomShellSibBoxes && mat->PomShellSibOfs) {
					const uint32_t o0 = mat->PomShellSibOfs[g], o1 = mat->PomShellSibOfs[g+1];
					for (uint32_t s = o0; s < o1; ++s)
						std::fprintf(stderr, " sib=%.6f,%.6f,%.6f,%.6f",
							(double)mat->PomShellSibBoxes[4*s+0],
							(double)mat->PomShellSibBoxes[4*s+1],
							(double)mat->PomShellSibBoxes[4*s+2],
							(double)mat->PomShellSibBoxes[4*s+3]);
				}
				std::fprintf(stderr, "\n");
			}
			// Every patch face's UV triangle — lets the offline pass ask "is this
			// landed UV inside the patch's actual footprint, or only inside its
			// bounding box?", which the kernel's box test cannot distinguish.
			for (const auto &sf : seamAll) {
				const unsigned g = grpOf(sf);
				if (!g || g > nGroups) continue;
				std::fprintf(stderr, "[POM-SEAM-TRI] '%s' g=%u uv=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
					matName, g, (double)sf.U[0], (double)sf.V[0], (double)sf.U[1],
					(double)sf.V[1], (double)sf.U[2], (double)sf.V[2]);
			}
		}
	}
	// ── --pom_shell_census (DIAGNOSTIC, default OFF) ────────────────────────
	// The amplitude is authored in UV and converted per FACE by that face's own
	// world-per-UV, so one authored surface can displace by several different
	// world distances. This prints exactly how many and how far apart.
	if (censusPrint && !cenF.empty()) {
		auto pct = [](std::vector<float> v, double p) {
			if (v.empty()) return 0.0f;
			std::sort(v.begin(), v.end());
			return v[size_t(p * double(v.size() - 1) + 0.5)];
		};
		// Authored-plane clustering (same tolerances the union-find uses).
		std::vector<std::array<float,4>> planes;
		std::vector<int> planeOf(cenF.size(), -1);
		for (size_t i = 0; i < cenF.size(); ++i) {
			const auto &p = refPlane[i];
			int hit = -1;
			for (size_t k = 0; k < planes.size(); ++k) {
				const auto &q = planes[k];
				if (p[0]*q[0] + p[1]*q[1] + p[2]*q[2] < 0.9995f) continue;
				if (std::fabs(p[3] - q[3]) > 2e-3f) continue;
				hit = int(k); break;
			}
			if (hit < 0) { hit = int(planes.size()); planes.push_back(p); }
			planeOf[i] = hit;
		}
		std::vector<float> allWu, allWv, allW, allA, allAmp;
		for (const auto &c : cenF) {
			if (!(c.w > 0.0f)) continue;
			allWu.push_back(c.wu); allWv.push_back(c.wv); allW.push_back(c.w);
			allA.push_back(c.wv > 1e-9f ? c.wu / c.wv : 1.0f);
			// EFFECTIVE world amplitude: constant under --pom_shell_world_amp,
			// uvAmp x this face's own density otherwise.
			allAmp.push_back(worldAmpMode ? worldAmp : uvAmp * c.w);
		}
		const float ampLo = allAmp.empty() ? 0.0f : *std::min_element(allAmp.begin(), allAmp.end());
		const float ampHi = allAmp.empty() ? 0.0f : *std::max_element(allAmp.begin(), allAmp.end());
		std::fprintf(stderr,
			"[POM-SHELL-CENSUS] '%s' uvAmp=%.4f  faces=%zu  authored planes=%zu\n"
			"[POM-SHELL-CENSUS]   |dP/du| (world per U tile)  min/med/max = %.4f / %.4f / %.4f\n"
			"[POM-SHELL-CENSUS]   |dP/dv| (world per V tile)  min/med/max = %.4f / %.4f / %.4f\n"
			"[POM-SHELL-CENSUS]   w = sqrt(|dP/du||dP/dv|)    min/med/max = %.4f / %.4f / %.4f  (x%.2f)\n"
			"[POM-SHELL-CENSUS]   UV ANISOTROPY |dP/du|/|dP/dv| min/med/max = %.3f / %.3f / %.3f  "
			"(1 = square texels; !=1 = relief stretched along one axis)\n"
			"[POM-SHELL-CENSUS]   IMPLIED WORLD AMPLITUDE uvAmp*w min/med/max = %.4f / %.4f / %.4f  "
			"(x%.2f across the material)\n",
			matName, (double)uvAmp, cenF.size(), planes.size(),
			(double)pct(allWu,0.0), (double)pct(allWu,0.5), (double)pct(allWu,1.0),
			(double)pct(allWv,0.0), (double)pct(allWv,0.5), (double)pct(allWv,1.0),
			(double)pct(allW,0.0),  (double)pct(allW,0.5),  (double)pct(allW,1.0),
			(double)(pct(allW,0.0) > 0.0f ? pct(allW,1.0)/pct(allW,0.0) : 0.0f),
			(double)pct(allA,0.0),  (double)pct(allA,0.5),  (double)pct(allA,1.0),
			(double)ampLo, (double)pct(allAmp,0.5), (double)ampHi,
			(double)(ampLo > 0.0f ? ampHi/ampLo : 0.0f));
		// Histogram of the implied world amplitude (12 linear bins).
		if (ampHi > ampLo) {
			int bins[12] = {0};
			for (float a : allAmp) {
				int b = int(12.0f * (a - ampLo) / (ampHi - ampLo));
				if (b < 0) b = 0; if (b > 11) b = 11;
				++bins[b];
			}
			for (int b = 0; b < 12; ++b) {
				const float lo = ampLo + (ampHi-ampLo)*float(b)/12.0f;
				const float hi = ampLo + (ampHi-ampLo)*float(b+1)/12.0f;
				std::fprintf(stderr, "[POM-SHELL-CENSUS-HIST] '%s' [%.4f..%.4f) %5d %s\n",
					matName, (double)lo, (double)hi, bins[b],
					std::string(size_t(std::min(60, bins[b] ? 1 + 59*bins[b]/std::max(1,
						*std::max_element(bins, bins+12)) : 0)), '#').c_str());
			}
		}
		// Per authored plane: the spread, and how many LID planes it became.
		struct PlaneRow { int p, n, lidDistinct; float wLo, wHi, aLo, aHi, oLo, oHi,
		                  lidSpread, ratio, anLo, anHi; };
		std::vector<PlaneRow> rows;
		for (size_t k = 0; k < planes.size(); ++k) {
			PlaneRow r{ int(k), 0, 0, 1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f,
			            0.0f, 1.0f, 1e30f, -1e30f };
			std::vector<float> lidD;
			for (size_t i = 0; i < cenF.size(); ++i) {
				if (planeOf[i] != int(k) || !(cenF[i].w > 0.0f)) continue;
				++r.n;
				r.wLo = std::min(r.wLo, cenF[i].w);   r.wHi = std::max(r.wHi, cenF[i].w);
				const float a = worldAmpMode ? worldAmp : uvAmp * cenF[i].w;
				r.aLo = std::min(r.aLo, a);           r.aHi = std::max(r.aHi, a);
				r.oLo = std::min(r.oLo, cenOffMin[i]); r.oHi = std::max(r.oHi, cenOffMax[i]);
				const float an = (cenF[i].wv > 1e-9f) ? cenF[i].wu / cenF[i].wv : 1.0f;
				r.anLo = std::min(r.anLo, an); r.anHi = std::max(r.anHi, an);
				if (cenLidPlane[i][0] != 0.0f || cenLidPlane[i][1] != 0.0f
				    || cenLidPlane[i][2] != 0.0f)
					lidD.push_back(cenLidPlane[i][3]);
			}
			if (!r.n) continue;
			std::sort(lidD.begin(), lidD.end());
			for (size_t i = 0; i < lidD.size(); ++i)
				if (i == 0 || lidD[i] - lidD[i-1] > 1e-3f) ++r.lidDistinct;
			r.lidSpread = lidD.empty() ? 0.0f : (lidD.back() - lidD.front());
			r.ratio = (r.aLo > 0.0f) ? r.aHi / r.aLo : 0.0f;
			rows.push_back(r);
		}
		std::sort(rows.begin(), rows.end(),
		          [](const PlaneRow &a, const PlaneRow &b) { return a.ratio > b.ratio; });
		int nSplit = 0;
		for (const auto &r : rows) if (r.lidDistinct > 1) ++nSplit;
		std::fprintf(stderr, "[POM-SHELL-CENSUS] '%s' %d of %zu authored planes became "
			"MORE THAN ONE lid plane\n", matName, nSplit, rows.size());
		for (const auto &r : rows)
			std::fprintf(stderr,
				"[POM-SHELL-CENSUS-PLANE] '%s' P%02d N=(%+.3f,%+.3f,%+.3f) d=%+9.3f "
				"faces=%3d  w=%.3f..%.3f  worldAmp=%.4f..%.4f (x%.2f)  "
				"aniso=%.3f..%.3f  lidOff=%.4f..%.4f  lidPlanes=%d spread=%.4f world\n",
				matName, r.p, (double)planes[r.p][0], (double)planes[r.p][1],
				(double)planes[r.p][2], (double)planes[r.p][3], r.n,
				(double)r.wLo, (double)r.wHi, (double)r.aLo, (double)r.aHi,
				(double)r.ratio, (double)r.anLo, (double)r.anHi,
				(double)r.oLo, (double)r.oHi,
				r.lidDistinct, (double)r.lidSpread);
	}
	mat->PomShellUvAmp = uvAmp;
	mat->PomShellWorldAmp = worldAmp;      // 0 = UV semantics (the default)
	// --pom_shell_world_amp: publish the PER-PATCH UV amplitude the rasterizer
	// looks up (worldAmp / that patch's world-per-UV). A patch is coplanar by
	// construction so one number is exact for it; this is what lets the whole
	// per-triangle and per-pixel code path stay untouched.
	if (worldAmpMode && nGroups) {
		std::vector<float> amps(nGroups, uvAmp);
		std::vector<char>  have(nGroups, 0);
		for (size_t i = 0; i < refs.size(); ++i) {
			const Face &F = refs[i].T->Faces[refs[i].fi];
			const unsigned g = F.PomShellGroup;
			if (!g || g > nGroups || have[g-1]) continue;
			if (i < cenF.size() && cenF[i].w > 0.0f) {
				amps[g-1] = worldAmp / cenF[i].w;
				have[g-1] = 1;
			}
		}
		mat->PomShellPatchUvAmp = new float[nGroups];
		std::memcpy(mat->PomShellPatchUvAmp, amps.data(), nGroups * sizeof(float));
		float aLo = 1e30f, aHi = -1e30f;
		for (unsigned g = 0; g < nGroups; ++g) { aLo = std::min(aLo, amps[g]); aHi = std::max(aHi, amps[g]); }
		std::fprintf(stderr, "[POM-SHELL-WORLDAMP] '%s': per-patch UV amplitude "
			"%.5f..%.5f over %u patches (was one %.5f for all)\n",
			matName, (double)aLo, (double)aHi, nGroups, (double)uvAmp);
	}
	std::fprintf(stderr,
		"[POM-SHELL] '%s' amp=%.3f UV: %lld verts moved [%.4f..%.4f world] over "
		"%lld faces, worldPerUV %.3f..%.3f, %lld corner verts (ShellH min %.3f), "
		"%lld pinned%s; %u coplanar patches, UV span u[%.2f..%.2f] v[%.2f..%.2f], "
		"max lateral travel %.3f UV\n",
		matName, (double)uvAmp, nMoved, (double)offMin, (double)offMax, nFaces,
		(double)wMin, (double)wMax, nCorner,
		(double)(hMinStamp < 1e29f ? hMinStamp : 1.0f), nPinned,
		pinCrossMaterial ? " (cross-material pinning ON)" : "",
		nGroups, domUMin, domUMax, domVMin, domVMax,
		(double)(uvAmp * fds::FeatureFlags::pom_shell_cap()));
	return uvAmp;
}

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
