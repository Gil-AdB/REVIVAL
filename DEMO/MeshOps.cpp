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

void DisplaceStoneSubdiv(Scene *Sc, const char *matName, int uniformLevel,
                         float amp, int mip, float adapt, float cellsPerBlock) {
	if (!Sc || !matName) return;
	if (cellsPerBlock < 0.25f) cellsPerBlock = 0.25f;
	auto isTarget = [&](const Face *F) {
		return F && F->Txtr && F->Txtr->Name && !std::strcmp(F->Txtr->Name, matName);
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
		auto isBorderEdge = [&](uint32_t x, uint32_t y) {
			auto it = origEdgeUse.find({std::min(x,y),std::max(x,y)});
			return it != origEdgeUse.end() && it->second == 1;   // used by exactly one target face
		};

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
		for (uint32_t i = 0; i < nOrig; ++i) if (origNonTargetVert[i]) pinnedZero[i] = 1;
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
		faces.reserve(size_t(T->FIndex) * 4);
		fIdx.reserve(faces.capacity());
		auto emit = [&](const Face &proto, uint32_t i0, uint32_t i1, uint32_t i2,
		                float u0,float v0,float u1,float v1,float u2,float v2){
			Face f = proto; f.frame = nullptr;
			f.U1=u0;f.V1=v0;f.U2=u1;f.V2=v1;f.U3=u2;f.V3=v2;
			f.EU1=u0;f.EV1=v0;f.EU2=u1;f.EV2=v1;f.EU3=u2;f.EV3=v2;
			faces.push_back(f); fIdx.push_back({i0,i1,i2});
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
			if (grid.valid && edgeAlignedQuad(cn, cu, cv, Ff)) {
				++builtQuads; ++edgeQuads;
				continue;
			}
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
		}

		// ── displacement (per-vertex height averaged over incident target faces,
		// pushed along the vertex normal; authored-border verts pinned to zero) ──
		const uint32_t nV = uint32_t(verts.size());
		if (pinnedZero.size() < nV) pinnedZero.resize(nV, 0);
		std::vector<float> hSum(nV, 0.0f); std::vector<int> hCnt(nV, 0);
		auto isTargetNew=[&](const Face &F){ return F.Txtr && F.Txtr->Name && !std::strcmp(F.Txtr->Name,matName); };
		for (size_t i=0;i<faces.size();++i){
			const Face &F=faces[i]; if (!isTargetNew(F)) continue;
			const uint32_t vi[3]={fIdx[i][0],fIdx[i][1],fIdx[i][2]};
			const float cu2[3]={F.U1,F.U2,F.U3}, cv2[3]={F.V1,F.V2,F.V3};
			for (int k=0;k<3;++k){ if (vi[k]>=nV||pinnedZero[vi[k]]) continue;
				hSum[vi[k]]+=SampleHeight8Bilinear(hm,useMip,cu2[k],cv2[k]); hCnt[vi[k]]+=1; }
		}
		std::vector<Vector> basePos(nV);
		for (uint32_t i=0;i<nV;++i) basePos[i]=verts[i].Pos;
		int nMoved=0; float dMin=1e30f,dMax=-1e30f;
		for (uint32_t i=0;i<nV;++i){
			if (pinnedZero[i]||hCnt[i]==0) continue;
			const Vector &N=verts[i].N; const float nl=std::sqrt(N.x*N.x+N.y*N.y+N.z*N.z);
			if (nl<1e-6f) continue;
			const float h=hSum[i]/float(hCnt[i]); const float dsp=amp*(h-mipMean);
			verts[i].Pos.x+=N.x/nl*dsp; verts[i].Pos.y+=N.y/nl*dsp; verts[i].Pos.z+=N.z/nl*dsp;
			if (dsp<dMin)dMin=dsp; if (dsp>dMax)dMax=dsp; ++nMoved;
		}

		// ── cross-patch crack pinning: on each interior side shared by two
		// patches whose vertex PARAM LISTS differ (level boundary, or edge-
		// aligned vs fallback tessellation), snap the non-anchor side's extra
		// verts onto the anchor side's displaced POLYLINE. Anchor = the record
		// with fewer params (the coarser side — the straight-segment rule the
		// old level pinning used, generalized), lexicographic tie-break so the
		// choice never depends on registration order. ──
		int nTJ=0;
		auto sideVid=[&](uint32_t clo,uint32_t chi,float param)->uint32_t{
			if (param<=0.0f) return clo; if (param>=1.0f) return chi;
			return edgeVert(clo,chi,param);   // canonical → already-created shared vert
		};
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
			"-> verts %d faces %d, %d displaced [%+.3f..%+.3f], %d T-junction pins\n",
			matName, uniformLevel>0?"uniform":"adaptive",
			uniformLevel>0?std::to_string(uniformLevel).c_str():"0..5",
			(double)amp, useMip, builtQuads, edgeQuads,
			edgePlateauC, edgeStepC, edgeFloorC, builtLones, fanCells, flatCells,
			lvlHist[0],lvlHist[1],lvlHist[2],lvlHist[3],lvlHist[4],lvlHist[5],
			lcapHist[0],lcapHist[1],lcapHist[2],lcapHist[3],lcapHist[4],lcapHist[5],
			int(T->VIndex), int(T->FIndex), nMoved, (double)dMin, (double)dMax, nTJ);
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
