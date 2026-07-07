#include "MeshOps.h"

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

	// The mummy ('momy') is an organic lathe that LightWave authored as
	// smooth (Surf_Smoothing, MaxSmoothingAngle ~1.6 rad ≈ 92°). The global
	// architectural crease threshold (30°, tuned for hard edges like the City
	// corners) splits its radial facets and the whole body goes faceted. For
	// that one surface, honor its authored smoothing angle instead, and smooth
	// only among its own faces so it can't bleed into the floor it sits on.
	// Every other surface keeps the global threshold => other scenes unchanged.
	auto matIsMomy = [](const Face *f) {
		return f && f->Txtr && f->Txtr->Name && !std::strcmp(f->Txtr->Name, "momy");
	};

	auto computeSmoothedNormal = [&](Vertex *origVtx, const Face *face) {
		const bool momy = matIsMomy(face);
		Vector accum{};
		Vector_Form(&accum, 0, 0, 0);
		auto it = incident.find(origVtx);
		if (it == incident.end()) return face->N;
		for (Face *adj : it->second) {
			if (momy) {
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

// Phong-tessellate (curved PN-style) every face whose material is `matName`,
// `levels` times. Each target triangle splits 1→4 at its edge midpoints; the
// midpoint is displaced toward the surface implied by the two endpoints' smooth
// vertex normals (project the linear midpoint onto each endpoint's tangent plane,
// average) so the silhouette rounds out instead of staying a flat polygon. Edge
// midpoints are shared between adjacent target faces (keyed by sorted vertex
// index) so the mesh stays crack-free. Non-target faces are copied verbatim.
//
// Must run AFTER per-vertex normals exist (Preprocess) and BEFORE
// MakeFacesIndependentByAngle (which then re-smooths the finer mesh). Rebuilds
// T->Verts/T->Faces and restamps A_idx exactly like MakeFacesIndependent, so the
// downstream count change is handled the same proven way. Old arrays are leaked
// (init-time, matches MakeFacesIndependent).
void SubdivideMaterialFaces(Scene *Sc, const char *matName, int levels) {
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

			auto edgeMid = [&](uint32_t ia, uint32_t ib) -> uint32_t {
				const auto key = std::make_pair(std::min(ia, ib), std::max(ia, ib));
				auto it = midOf.find(key);
				if (it != midOf.end()) return it->second;
				const Vertex &A = oldV[ia], &B = oldV[ib];
				Vertex m = A;                       // inherit Flags etc.
				const float lx = (A.Pos.x + B.Pos.x) * 0.5f;
				const float ly = (A.Pos.y + B.Pos.y) * 0.5f;
				const float lz = (A.Pos.z + B.Pos.z) * 0.5f;
				// Project the linear midpoint onto each endpoint's tangent plane.
				const float da = (lx - A.Pos.x) * A.N.x + (ly - A.Pos.y) * A.N.y + (lz - A.Pos.z) * A.N.z;
				const float db = (lx - B.Pos.x) * B.N.x + (ly - B.Pos.y) * B.N.y + (lz - B.Pos.z) * B.N.z;
				m.Pos.x = lx - 0.5f * (da * A.N.x + db * B.N.x);
				m.Pos.y = ly - 0.5f * (da * A.N.y + db * B.N.y);
				m.Pos.z = lz - 0.5f * (da * A.N.z + db * B.N.z);
				float nx = A.N.x + B.N.x, ny = A.N.y + B.N.y, nz = A.N.z + B.N.z;
				const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (nl > 1e-6f) { m.N.x = nx / nl; m.N.y = ny / nl; m.N.z = nz / nl; }
				m.U = (A.U + B.U) * 0.5f;   m.V = (A.V + B.V) * 0.5f;
				m.EU = (A.EU + B.EU) * 0.5f; m.EV = (A.EV + B.EV) * 0.5f;
				const uint32_t id = uint32_t(verts.size());
				verts.push_back(m);
				midOf[key] = id;
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
