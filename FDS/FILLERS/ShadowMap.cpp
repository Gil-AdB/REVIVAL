#include "ShadowMap.h"

#include "Base/Scene.h"
#include "Base/Omni.h"
#include "Base/Vertex.h"
#include "Base/FDS_DEFS.H"
#include "Base/FDS_DECS.H"
#include "Base/FDS_VARS.H"
#include "Base/FeatureFlags.h"
#include "F4Vec.h"
#include "TheOtherBarry.h"

#include <simd/vectorclass.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

thread_local ShadowMap *g_currentShadowMap = nullptr;
std::vector<ShadowMap> g_shadowMaps;
std::vector<CubeShadowRef> g_cubeShadowRefs;

// Shadow-map debug viewer state. See ShadowMap.h for protocol.
int g_shadowViewIdx = -1;
std::atomic<bool> g_shadowFullscreenView{false};

void ShadowMap_ViewCycle()
{
    // -1 -> 0 -> 1 -> ... -> N-1 -> -1.
    const int n = int(g_shadowMaps.size());
    if (n == 0) {
        std::fprintf(stderr, "[SHADOW-VIEW] no shadow maps registered\n");
        g_shadowViewIdx = -1;
        return;
    }
    // In full-screen mode (greets M), -1 (= "off") would clear the
    // viz; instead wrap 0..N-1. Outside full-screen mode keep the
    // -1 sentinel so V can hide the thumbnail.
    const bool fullscreen = g_shadowFullscreenView.load(std::memory_order_relaxed);
    if (fullscreen) {
        g_shadowViewIdx = (g_shadowViewIdx + 1) % n;
    } else {
        g_shadowViewIdx = (g_shadowViewIdx >= n - 1) ? -1 : g_shadowViewIdx + 1;
    }
    if (g_shadowViewIdx < 0) {
        std::fprintf(stderr, "[SHADOW-VIEW] off (cycled past last of %d)\n", n);
    } else {
        const ShadowMap &sm = g_shadowMaps[g_shadowViewIdx];
        const char *omniName = (sm.omni && sm.omni->Type == Light_Omni) ? "omni" :
                               (sm.omni && sm.omni->Type == Light_SpotLight) ? "spot" : "?";
        // Stats so we can see whether the actual data differs across maps
        // (vs an overlay rendering bug). Sample some unique polyIds + the
        // depth extent and non-empty pixel count.
        const size_t total = sm.polyId.size();
        size_t nonZeroP = 0;
        bool seen[256] = {};
        int uniqueP = 0;
        for (uint8_t p : sm.polyId) {
            if (p) ++nonZeroP;
            if (!seen[p]) { seen[p] = true; ++uniqueP; }
        }
        uint16_t dmin = 0xFFFF, dmax = 0;
        size_t nonZeroD = 0;
        for (uint16_t d : sm.depth) {
            if (d) ++nonZeroD;
            if (d < dmin) dmin = d;
            if (d > dmax) dmax = d;
        }
        // Two cheap FNV-1a-ish content hashes — if these match across
        // maps with the same nonzero-count, the buffers are literally
        // byte-for-byte identical (storage bug), not just statistically
        // similar (real geometry).
        uint64_t pHash = 0xcbf29ce484222325ull;
        for (uint8_t v : sm.polyId) {
            pHash ^= v; pHash *= 0x100000001b3ull;
        }
        uint64_t dHash = 0xcbf29ce484222325ull;
        for (uint16_t v : sm.depth) {
            dHash ^= v; dHash *= 0x100000001b3ull;
        }
        std::fprintf(stderr,
            "[SHADOW-VIEW] %d / %d  %s  %dx%d  cubeFace=%d  "
            "polyId: %zu/%zu nz, %d uniq, h=%016llx  depth: %zu/%zu nz, [%u..%u], h=%016llx\n",
            g_shadowViewIdx, n, omniName, sm.xres, sm.yres, int(sm.cubeFace),
            nonZeroP, total, uniqueP, (unsigned long long)pHash,
            nonZeroD, sm.depth.size(), unsigned(dmin), unsigned(dmax),
            (unsigned long long)dHash);
        if (sm.omni) {
            std::fprintf(stderr,
                "              pos=(%g,%g,%g) IRange=%g\n",
                sm.omni->IPos.x, sm.omni->IPos.y, sm.omni->IPos.z, sm.omni->IRange);
        }
    }
}

void ShadowMap_Overlay(byte *vpage, int xres, int yres, int pitchBytes)
{
    // When the user is in the full-screen shadow viz (greets M),
    // the entire framebuffer is already showing the current map —
    // skip the thumbnail so it doesn't stack on top.
    if (g_shadowFullscreenView.load(std::memory_order_relaxed)) return;
    if (g_shadowViewIdx < 0) return;
    if (g_shadowViewIdx >= int(g_shadowMaps.size())) return;
    if (!vpage || xres <= 0 || yres <= 0 || pitchBytes <= 0) return;

    const ShadowMap &sm = g_shadowMaps[g_shadowViewIdx];
    if (sm.xres <= 0 || sm.yres <= 0 || sm.polyId.empty()) return;

    int thumbSize = std::min(xres, yres) / 4;
    if (thumbSize < 64)   thumbSize = 64;
    if (thumbSize > sm.xres) thumbSize = sm.xres;
    if (thumbSize > 512)  thumbSize = 512;
    const int dstW = thumbSize;
    const int dstH = thumbSize;
    if (dstW + 4 >= xres || dstH + 4 >= yres) return;

    // Row stride in dwords. SDL locked textures may pad scanlines past
    // xres*4 bytes — use the surface's BPSL (passed as pitchBytes).
    const ptrdiff_t pitchD = ptrdiff_t(pitchBytes) / 4;
    dword *out = reinterpret_cast<dword*>(vpage);
    const int ox = 4;
    const int oy = 4;

    // PolyId visualization: each unique polyId hashes to a unique
    // color. 0 = empty (no surface) → black so you can see the cube
    // face coverage instead of just a uniform splat.
    auto hashPolyColor = [](uint8_t pid) -> dword {
        if (pid == 0) return 0xFF000000u;
        uint32_t h = uint32_t(pid) * 0x9E3779B9u;
        h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
        const uint8_t r = uint8_t((h >> 16) & 0xFF);
        const uint8_t g = uint8_t((h >>  8) & 0xFF);
        const uint8_t b = uint8_t( h        & 0xFF);
        return 0xFF000000u | (dword(r) << 16) | (dword(g) << 8) | dword(b);
    };
    for (int dy = 0; dy < dstH; ++dy) {
        const int sy = (dy * sm.yres) / dstH;
        const uint8_t *row = &sm.polyId[size_t(sy) * size_t(sm.xres)];
        dword *dstRow = out + ptrdiff_t(oy + dy) * pitchD + ptrdiff_t(ox);
        for (int dx = 0; dx < dstW; ++dx) {
            const int sx = (dx * sm.xres) / dstW;
            dstRow[dx] = hashPolyColor(row[sx]);
        }
    }
    // White 1-px border.
    for (int dx = -1; dx <= dstW; ++dx) {
        if (oy - 1 >= 0)
            out[ptrdiff_t(oy - 1) * pitchD + ptrdiff_t(ox + dx)] = 0xFFFFFFFFu;
        if (oy + dstH < yres)
            out[ptrdiff_t(oy + dstH) * pitchD + ptrdiff_t(ox + dx)] = 0xFFFFFFFFu;
    }
    for (int dy = -1; dy <= dstH; ++dy) {
        if (ox - 1 >= 0)
            out[ptrdiff_t(oy + dy) * pitchD + ptrdiff_t(ox - 1)] = 0xFFFFFFFFu;
        if (ox + dstW < xres)
            out[ptrdiff_t(oy + dy) * pitchD + ptrdiff_t(ox + dstW)] = 0xFFFFFFFFu;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "SM %d/%zu  %dx%d  face=%d  (polyId)",
                  g_shadowViewIdx, g_shadowMaps.size(),
                  sm.xres, sm.yres, int(sm.cubeFace));
    OutTextXY(vpage, ox + 4, oy + dstH + 4, buf, 255, xres, yres);
}

void ShadowMaps_Rebuild(Scene *Sc, int res)
{
	g_shadowMaps.clear();
	if (!Sc) return;
	// Shadow camera's far plane is a multiple of the light's IRange. The
	// light only LIGHTS within IRange, but geometry between IRange and
	// FZP must still be in the depth buffer because it can occlude lit
	// receivers near the cone edge. Multiplier is tunable via env.
	const float sFzpMult = fds::FeatureFlags::shadow_fzp_mult();
	for (Omni *O = Sc->OmniHead; O; O = O->Next) {
		if (!(O->Flags & Omni_CastsShadow)) continue;
		// Light_Omni shadow casters use cube shadow maps via
		// CubeShadowMaps_Rebuild — not a single 2D entry here.
		if (O->Type != Light_SpotLight) continue;
		ShadowMap sm;
		// Per-light resolution: Omni.shadowMapRes overrides the global
		// default. Lets short-range orbit lights use 256² (16× less
		// raster cost than 1024²) while keeping the main spot at 1024².
		const int lightRes = (O->shadowMapRes > 0) ? int(O->shadowMapRes) : res;
		sm.xres = lightRes;
		sm.yres = lightRes;
		const size_t n = size_t(lightRes) * size_t(lightRes);
		sm.depth.assign(n, 0);
		sm.polyId.assign(n, 0);
		sm.depth_dynamic.assign(n, 0);
		sm.polyId_dynamic.assign(n, 0);
		sm.omni = O;
		// Camera basis + FOV / z-scale are computed each frame in
		// Render_DeferredShadowMaps from the omni's pose. zScale here
		// is initialized from the omni's range as a sane default but
		// recomputed per frame.
		sm.fzp    = O->IRange * sFzpMult;
		sm.rFZP   = 1.0f / sm.fzp;
		sm.zScale = float(0xFF00) / (sm.fzp * 1.1f);
		g_shadowMaps.push_back(std::move(sm));
	}
	std::fprintf(stderr, "[SHADOW] ShadowMaps_Rebuild: %zu shadow maps "
		"(R/G/B intensities from O->L):\n", g_shadowMaps.size());
	for (const auto& s : g_shadowMaps) {
		std::fprintf(stderr, "  res=%dx%d  R=%.0f G=%.0f B=%.0f  IRange=%.1f\n",
			s.xres, s.yres,
			s.omni ? s.omni->L.R : 0.0f,
			s.omni ? s.omni->L.G : 0.0f,
			s.omni ? s.omni->L.B : 0.0f,
			s.omni ? s.omni->IRange : 0.0f);
	}
	std::fflush(stderr);
}

// Cube shadow map setup for omnis. Each omni gets 6 ShadowMap entries
// appended to g_shadowMaps (treated as 90°-FOV "spotlights" for the
// existing render pass) plus a CubeShadowRef that groups them.
void CubeShadowMaps_Rebuild(Scene *Sc, int res)
{
	g_cubeShadowRefs.clear();
	if (!Sc) return;
	const float sFzpMult = fds::FeatureFlags::shadow_fzp_mult();
	for (Omni *O = Sc->OmniHead; O; O = O->Next) {
		if (!(O->Flags & Omni_CastsShadow)) continue;
		if (O->Type != Light_Omni) continue;  // spots handled by ShadowMaps_Rebuild

		CubeShadowRef ref;
		ref.omni = O;
		ref.lightISource = O->IPos;
		const int faceRes = (O->shadowMapRes > 0) ? int(O->shadowMapRes) : res;

		for (int f = 0; f < 6; ++f) {
			ShadowMap sm;
			sm.xres = faceRes;
			sm.yres = faceRes;
			const size_t n = size_t(faceRes) * size_t(faceRes);
			sm.depth.assign(n, 0);
			sm.polyId.assign(n, 0);
			sm.depth_dynamic.assign(n, 0);
			sm.polyId_dynamic.assign(n, 0);
			sm.omni = O;  // shared across all 6 faces
			sm.cubeFace = int8_t(f);  // tells render pass which axis to face
			sm.fzp    = O->IRange * sFzpMult;
			sm.rFZP   = 1.0f / sm.fzp;
			sm.zScale = float(0xFF00) / (sm.fzp * 1.1f);
			ref.faceIdx[f] = int32_t(g_shadowMaps.size());
			g_shadowMaps.push_back(std::move(sm));
		}
		g_cubeShadowRefs.push_back(std::move(ref));
	}
	if (!g_cubeShadowRefs.empty()) {
		std::fprintf(stderr, "[SHADOW] CubeShadowMaps_Rebuild: %zu cube maps "
			"(6 faces each):\n", g_cubeShadowRefs.size());
		for (const auto& cr : g_cubeShadowRefs) {
			std::fprintf(stderr, "  res=%dx%d  R=%.0f G=%.0f B=%.0f  IRange=%.1f\n",
				g_shadowMaps[cr.faceIdx[0]].xres,
				g_shadowMaps[cr.faceIdx[0]].yres,
				cr.omni->L.R, cr.omni->L.G, cr.omni->L.B,
				cr.omni->IRange);
		}
		std::fflush(stderr);
	}
}

// Forward decl — defined in RENDER/Shadows.cpp.
void Render_DeferredShadowMaps(Scene *Sc, bool staticOnly);

void ShadowMaps_BakeStatic(Scene *Sc)
{
	// One-shot: render shadow maps for Omni_StaticShadow lights. After
	// this returns, Render_DeferredShadowMaps's per-frame skip filter
	// avoids re-rendering them. Intended to be called from scene init,
	// hiding inside the existing init bake window (city's Glato cube
	// bake, etc.) so the demo start time is unaffected.
	Render_DeferredShadowMaps(Sc, /*staticOnly=*/true);
	int n = 0;
	for (Omni *O = Sc ? Sc->OmniHead : nullptr; O; O = O->Next) {
		if ((O->Flags & Omni_CastsShadow) && (O->Flags & Omni_StaticShadow)) ++n;
	}
	std::fprintf(stderr, "[SHADOW] ShadowMaps_BakeStatic: %d static light(s) baked\n", n);
}

// ShadowBarry: tile-based AVX2 depth+polyId rasterizer modeled on
// TheOtherBarry. Walks 8×8 tiles via super-tile (4 tiles per side =
// 32px) hierarchical coverage culling. Per-tile fast path skips edge
// mask construction when the tile is fully inside the triangle.
//
// Output is just (depth: uint16, polyId: uint8) — none of TheOtherBarry's
// UV / texture / color / specular machinery. Constructor is no-Txtr.
//
// Encoded Z: `enc = 0xFF80 - round(z * zScale)`. Higher enc = closer
// to light. PolyId is matID+1 of the writing face; only written under
// the same Z-pass mask so the closest-occluder polyId wins.
struct ShadowBarry {
	ShadowMap *sm;
	uint16_t *zArr;   // sm->depth or sm->depth_dynamic
	uint8_t  *idArr;  // sm->polyId or sm->polyId_dynamic
	float drzdx, drzdy;
	uint8_t idByte;

	ShadowBarry(ShadowMap *smIn, uint16_t idIn, bool useDynamic)
		: sm(smIn),
		  zArr (useDynamic ? smIn->depth_dynamic.data()  : smIn->depth.data()),
		  idArr(useDynamic ? smIn->polyId_dynamic.data() : smIn->polyId.data()),
		  drzdx(0), drzdy(0), idByte(uint8_t(idIn)) {}

	template <barry::TCoverage Coverage = barry::TCoverage::PARTIAL>
	void apply_exact(const barry::Tile& tile) {
		const int xres = sm->xres;
		uint16_t * const zRowBase  = zArr
			+ size_t(tile.y) * barry::TILE_SIZE * size_t(xres)
			+ size_t(tile.x) * barry::TILE_SIZE;
		uint8_t  * const idRowBase = idArr
			+ size_t(tile.y) * barry::TILE_SIZE * size_t(xres)
			+ size_t(tile.x) * barry::TILE_SIZE;

		barry::TScreenCoord a0 = tile.a0, b0 = tile.b0, c0 = tile.c0;

		// Edge function lanes only needed for PARTIAL.
		Vec8i p_a, p_b, p_c;
		if constexpr (Coverage == barry::TCoverage::PARTIAL) {
			p_a = v8_from_arith_seq(a0, tile.dadx);
			p_b = v8_from_arith_seq(b0, tile.dbdx);
			p_c = v8_from_arith_seq(c0, tile.dcdx);
		}

		Vec8f p_rz = v8_from_arith_seq(tile.rz0, drzdx);
		const float zScale = sm->zScale;
		const Vec8f vZScale(zScale);

		uint16_t *zRow = zRowBase;
		uint8_t  *idRow = idRowBase;
		for (int row = 0; row < barry::TILE_SIZE; ++row,
				zRow += xres, idRow += xres) {
			Vec8ib p_mask;
			bool row_has_pixels;
			if constexpr (Coverage == barry::TCoverage::FULL) {
				p_mask = Vec8i(0) == Vec8i(0);
				row_has_pixels = true;
			} else {
				p_mask = (p_a | p_b | p_c) >= 0;
				row_has_pixels = barry::any_lane_set(p_mask);
			}
			if (row_has_pixels) {
				const Vec8f p_z = approx_recipr(p_rz);
				Vec8i enc = Vec8i(0xFF80) - roundi(p_z * vZScale);
				enc = max(enc, Vec8i(0));
				enc = min(enc, Vec8i(0xFFFF));

				Vec8us z_existing_c;
				z_existing_c.load(zRow);
				const Vec8i z_existing = extend(z_existing_c);
				p_mask &= Vec8ib(enc > z_existing);

				if (barry::any_lane_set(p_mask)) {
					*(__m128i*)zRow = _mm_blendv_epi8(
						*(__m128i*)zRow,
						compress(Vec8ui(enc)),
						compress(Vec8ui(Vec8i(p_mask))));

					if (idByte) {
						alignas(32) int mask_l[8];
						Vec8i(p_mask).store_a(mask_l);
						for (int lane = 0; lane < 8; ++lane) {
							if (mask_l[lane]) idRow[lane] = idByte;
						}
					}
				}
			}
			if constexpr (Coverage == barry::TCoverage::PARTIAL) {
				p_a += tile.dady;
				p_b += tile.dbdy;
				p_c += tile.dcdy;
			}
			p_rz += Vec8f(drzdy);
		}
	}

	void rasterize_triangle(const Vertex& v1, const Vertex& v2, const Vertex& v3) {
		// AABB in tile coords, clamped to shadow-map dimensions.
		using barry::TILE_SIZE;
		using barry::SUBPIXEL_BITS;
		using barry::SUBPIXEL_MULT;
		using barry::TScreenCoord;
		using barry::orient2d;

		const int xres = sm->xres;
		const int yres = sm->yres;
		auto clampX = [xres](int v) { return std::min(std::max(v, 0), xres - 1); };
		auto clampY = [yres](int v) { return std::min(std::max(v, 0), yres - 1); };

		const int tile_mx = clampX(int(std::min({v1.PX, v2.PX, v3.PX}))) / TILE_SIZE;
		const int tile_Mx = clampX(int(std::max({v1.PX, v2.PX, v3.PX}))) / TILE_SIZE;
		const int tile_my = clampY(int(std::min({v1.PY, v2.PY, v3.PY}))) / TILE_SIZE;
		const int tile_My = clampY(int(std::max({v1.PY, v2.PY, v3.PY}))) / TILE_SIZE;
		if (tile_mx > tile_Mx || tile_my > tile_My) return;

		// Subpixel-precise vertex coords.
		const TScreenCoord v1x = TScreenCoord(std::lroundf(v1.PX * SUBPIXEL_MULT));
		const TScreenCoord v1y = TScreenCoord(std::lroundf(v1.PY * SUBPIXEL_MULT));
		const TScreenCoord v2x = TScreenCoord(std::lroundf(v2.PX * SUBPIXEL_MULT));
		const TScreenCoord v2y = TScreenCoord(std::lroundf(v2.PY * SUBPIXEL_MULT));
		const TScreenCoord v3x = TScreenCoord(std::lroundf(v3.PX * SUBPIXEL_MULT));
		const TScreenCoord v3y = TScreenCoord(std::lroundf(v3.PY * SUBPIXEL_MULT));

		const TScreenCoord x0 = tile_mx * TILE_SIZE << SUBPIXEL_BITS;
		const TScreenCoord y0 = tile_my * TILE_SIZE << SUBPIXEL_BITS;
		TScreenCoord _a0 = orient2d(v2x, v2y, v1x, v1y, x0, y0);
		TScreenCoord _b0 = orient2d(v3x, v3y, v2x, v2y, x0, y0);
		TScreenCoord _c0 = orient2d(v1x, v1y, v3x, v3y, x0, y0);

		const TScreenCoord dadx = (v2y - v1y);
		const TScreenCoord dady = (v1x - v2x);
		const TScreenCoord dbdx = (v3y - v2y);
		const TScreenCoord dbdy = (v2x - v3x);
		const TScreenCoord dcdx = (v1y - v3y);
		const TScreenCoord dcdy = (v3x - v1x);

		// Hierarchical traversal gating — small triangles take the direct
		// per-tile loop; big triangles use the 4×4-tile super grid.
		constexpr int SUPER = 4;
		constexpr int SUPER_PIXELS = SUPER * TILE_SIZE;
		const int super_mx = tile_mx / SUPER;
		const int super_Mx = tile_Mx / SUPER;
		const int super_my = tile_my / SUPER;
		const int super_My = tile_My / SUPER;
		const bool spans_multi_super = (super_mx != super_Mx) || (super_my != super_My);

		auto build_tile = [&](int x, int y, TScreenCoord a, TScreenCoord b, TScreenCoord c) {
			barry::Tile tile = {};
			tile.x = x;
			tile.y = y;
			tile.a0 = a;
			tile.dadx = dadx; tile.dady = dady;
			tile.b0 = b;
			tile.dbdx = dbdx; tile.dbdy = dbdy;
			tile.c0 = c;
			tile.dcdx = dcdx; tile.dcdy = dcdy;
			tile.rz0 = v1.RZ + (x * TILE_SIZE - v1.PX) * drzdx
			                  + (y * TILE_SIZE - v1.PY) * drzdy;
			return tile;
		};

		if (!spans_multi_super) {
			for (int y = tile_my; y <= tile_My; ++y,
					_a0 += TILE_SIZE * dady, _b0 += TILE_SIZE * dbdy, _c0 += TILE_SIZE * dcdy) {
				TScreenCoord a0 = _a0, b0 = _b0, c0 = _c0;
				for (int x = tile_mx; x <= tile_Mx; ++x,
						a0 += TILE_SIZE * dadx, b0 += TILE_SIZE * dbdx, c0 += TILE_SIZE * dcdx) {
					const TScreenCoord max_a = a0 + ((dadx > 0) ? dadx * TILE_SIZE : 0) + ((dady > 0) ? dady * TILE_SIZE : 0);
					const TScreenCoord max_b = b0 + ((dbdx > 0) ? dbdx * TILE_SIZE : 0) + ((dbdy > 0) ? dbdy * TILE_SIZE : 0);
					const TScreenCoord max_c = c0 + ((dcdx > 0) ? dcdx * TILE_SIZE : 0) + ((dcdy > 0) ? dcdy * TILE_SIZE : 0);
					if ((max_a | max_b | max_c) < 0) continue;

					const TScreenCoord min_a = a0 + ((dadx < 0) ? dadx * TILE_SIZE : 0) + ((dady < 0) ? dady * TILE_SIZE : 0);
					const TScreenCoord min_b = b0 + ((dbdx < 0) ? dbdx * TILE_SIZE : 0) + ((dbdy < 0) ? dbdy * TILE_SIZE : 0);
					const TScreenCoord min_c = c0 + ((dcdx < 0) ? dcdx * TILE_SIZE : 0) + ((dcdy < 0) ? dcdy * TILE_SIZE : 0);
					const bool full_cover = (min_a >= 0) && (min_b >= 0) && (min_c >= 0);

					auto tile = build_tile(x, y, a0, b0, c0);
					if (full_cover) {
						apply_exact<barry::TCoverage::FULL>(tile);
					} else {
						apply_exact<barry::TCoverage::PARTIAL>(tile);
					}
				}
			}
			return;
		}

		// Super-tile path for large triangles.
		const TScreenCoord ssx0 = super_mx * SUPER_PIXELS << SUBPIXEL_BITS;
		const TScreenCoord ssy0 = super_my * SUPER_PIXELS << SUBPIXEL_BITS;
		TScreenCoord _sa0 = orient2d(v2x, v2y, v1x, v1y, ssx0, ssy0);
		TScreenCoord _sb0 = orient2d(v3x, v3y, v2x, v2y, ssx0, ssy0);
		TScreenCoord _sc0 = orient2d(v1x, v1y, v3x, v3y, ssx0, ssy0);

		for (int sy = super_my; sy <= super_My; ++sy,
				_sa0 += SUPER_PIXELS * dady, _sb0 += SUPER_PIXELS * dbdy, _sc0 += SUPER_PIXELS * dcdy) {
			TScreenCoord sa0 = _sa0, sb0 = _sb0, sc0 = _sc0;
			for (int sx = super_mx; sx <= super_Mx; ++sx,
					sa0 += SUPER_PIXELS * dadx, sb0 += SUPER_PIXELS * dbdx, sc0 += SUPER_PIXELS * dcdx) {
				const TScreenCoord smax_a = sa0 + ((dadx > 0) ? dadx * SUPER_PIXELS : 0) + ((dady > 0) ? dady * SUPER_PIXELS : 0);
				const TScreenCoord smax_b = sb0 + ((dbdx > 0) ? dbdx * SUPER_PIXELS : 0) + ((dbdy > 0) ? dbdy * SUPER_PIXELS : 0);
				const TScreenCoord smax_c = sc0 + ((dcdx > 0) ? dcdx * SUPER_PIXELS : 0) + ((dcdy > 0) ? dcdy * SUPER_PIXELS : 0);
				if ((smax_a | smax_b | smax_c) < 0) continue;

				const TScreenCoord smin_a = sa0 + ((dadx < 0) ? dadx * SUPER_PIXELS : 0) + ((dady < 0) ? dady * SUPER_PIXELS : 0);
				const TScreenCoord smin_b = sb0 + ((dbdx < 0) ? dbdx * SUPER_PIXELS : 0) + ((dbdy < 0) ? dbdy * SUPER_PIXELS : 0);
				const TScreenCoord smin_c = sc0 + ((dcdx < 0) ? dcdx * SUPER_PIXELS : 0) + ((dcdy < 0) ? dcdy * SUPER_PIXELS : 0);
				const bool super_full = (smin_a >= 0) && (smin_b >= 0) && (smin_c >= 0);

				const int ty_start = std::max(sy * SUPER, tile_my);
				const int ty_end   = std::min(sy * SUPER + SUPER - 1, tile_My);
				const int tx_start = std::max(sx * SUPER, tile_mx);
				const int tx_end   = std::min(sx * SUPER + SUPER - 1, tile_Mx);

				if (super_full) {
					// Every tile in this super-tile is FULL — skip per-tile
					// edge tests, dispatch apply_exact<FULL> directly.
					for (int y = ty_start; y <= ty_end; ++y) {
						for (int x = tx_start; x <= tx_end; ++x) {
							auto tile = build_tile(x, y, 0, 0, 0);
							apply_exact<barry::TCoverage::FULL>(tile);
						}
					}
					continue;
				}

				TScreenCoord _ta0 = sa0 + (tx_start - sx * SUPER) * TILE_SIZE * dadx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dady;
				TScreenCoord _tb0 = sb0 + (tx_start - sx * SUPER) * TILE_SIZE * dbdx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dbdy;
				TScreenCoord _tc0 = sc0 + (tx_start - sx * SUPER) * TILE_SIZE * dcdx
				                       + (ty_start - sy * SUPER) * TILE_SIZE * dcdy;

				for (int y = ty_start; y <= ty_end; ++y,
						_ta0 += TILE_SIZE * dady, _tb0 += TILE_SIZE * dbdy, _tc0 += TILE_SIZE * dcdy) {
					TScreenCoord a0 = _ta0, b0 = _tb0, c0 = _tc0;
					for (int x = tx_start; x <= tx_end; ++x,
							a0 += TILE_SIZE * dadx, b0 += TILE_SIZE * dbdx, c0 += TILE_SIZE * dcdx) {
						const TScreenCoord max_a = a0 + ((dadx > 0) ? dadx * TILE_SIZE : 0) + ((dady > 0) ? dady * TILE_SIZE : 0);
						const TScreenCoord max_b = b0 + ((dbdx > 0) ? dbdx * TILE_SIZE : 0) + ((dbdy > 0) ? dbdy * TILE_SIZE : 0);
						const TScreenCoord max_c = c0 + ((dcdx > 0) ? dcdx * TILE_SIZE : 0) + ((dcdy > 0) ? dcdy * TILE_SIZE : 0);
						if ((max_a | max_b | max_c) < 0) continue;

						const TScreenCoord min_a = a0 + ((dadx < 0) ? dadx * TILE_SIZE : 0) + ((dady < 0) ? dady * TILE_SIZE : 0);
						const TScreenCoord min_b = b0 + ((dbdx < 0) ? dbdx * TILE_SIZE : 0) + ((dbdy < 0) ? dbdy * TILE_SIZE : 0);
						const TScreenCoord min_c = c0 + ((dcdx < 0) ? dcdx * TILE_SIZE : 0) + ((dcdy < 0) ? dcdy * TILE_SIZE : 0);
						const bool full_cover = (min_a >= 0) && (min_b >= 0) && (min_c >= 0);

						auto tile = build_tile(x, y, a0, b0, c0);
						if (full_cover) {
							apply_exact<barry::TCoverage::FULL>(tile);
						} else {
							apply_exact<barry::TCoverage::PARTIAL>(tile);
						}
					}
				}
			}
		}
	}
};

static void rasterize_depth_tri(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                                 ShadowMap& sm,
                                 uint16_t idOverride = 0,
                                 bool useDynamic = false)
{
	const float x0 = v0.PX, y0 = v0.PY;
	const float x1 = v1.PX, y1 = v1.PY;
	const float x2 = v2.PX, y2 = v2.PY;

	const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (std::fabs(area) < 1e-4f) return;
	const float invArea = 1.0f / area;

	float xmin = std::min(std::min(x0, x1), x2);
	float xmax = std::max(std::max(x0, x1), x2);
	float ymin = std::min(std::min(y0, y1), y2);
	float ymax = std::max(std::max(y0, y1), y2);
	if (xmin < 0.0f) xmin = 0.0f;
	if (ymin < 0.0f) ymin = 0.0f;
	if (xmax > float(sm.xres - 1)) xmax = float(sm.xres - 1);
	if (ymax > float(sm.yres - 1)) ymax = float(sm.yres - 1);

	const int ixmin = int(std::floor(xmin));
	const int ixmax = int(std::ceil (xmax));
	const int iymin = int(std::floor(ymin));
	const int iymax = int(std::ceil (ymax));
	if (ixmin > ixmax || iymin > iymax) return;

	const float rz0 = v0.RZ;
	const float rz1 = v1.RZ;
	const float rz2 = v2.RZ;
	const float zScale = sm.zScale;

	// Bary partial derivatives wrt screen X (constant across the tri).
	// Expanding w0 = ((x1-px)(y2-py) - (x2-px)(y1-py)) * invArea gives
	//   ∂w0/∂px = (y1 - y2) * invArea
	//   ∂w1/∂px = (y2 - y0) * invArea
	//   ∂w2/∂px = -∂w0/∂px - ∂w1/∂px  (since w0+w1+w2 ≡ 1)
	const float dw0dx = (y1 - y2) * invArea;
	const float dw1dx = (y2 - y0) * invArea;
	const float drzdx = dw0dx * rz0 + dw1dx * rz1 + (-dw0dx - dw1dx) * rz2;

	// 8-lane offsets [0,1,2,…,7]; used to broadcast (base + i*dx) per row.
	const Vec8f laneOffsets(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
	const Vec8f vDw0dx(dw0dx), vDw1dx(dw1dx), vDrzdx(drzdx);
	const Vec8f vDw0dx8 = vDw0dx * 8.0f;
	const Vec8f vDw1dx8 = vDw1dx * 8.0f;
	const Vec8f vDrzdx8 = vDrzdx * 8.0f;
	const Vec8f vZScale(zScale);
	const uint8_t idByte = uint8_t(idOverride);

	uint16_t * const zBase  = useDynamic ? sm.depth_dynamic.data()  : sm.depth.data();
	uint8_t  * const idBase = useDynamic ? sm.polyId_dynamic.data() : sm.polyId.data();
	for (int y = iymin; y <= iymax; ++y) {
		uint16_t *zRow = zBase  + size_t(y) * size_t(sm.xres);
		uint8_t  *idRow = idBase + size_t(y) * size_t(sm.xres);
		const float py = float(y) + 0.5f;
		const float px0 = float(ixmin) + 0.5f;
		const float w0Row = ((x1 - px0) * (y2 - py) - (x2 - px0) * (y1 - py)) * invArea;
		const float w1Row = ((x2 - px0) * (y0 - py) - (x0 - px0) * (y2 - py)) * invArea;
		const float rzRow = w0Row * rz0 + w1Row * rz1 + (1.0f - w0Row - w1Row) * rz2;

		Vec8f vW0 = Vec8f(w0Row) + laneOffsets * vDw0dx;
		Vec8f vW1 = Vec8f(w1Row) + laneOffsets * vDw1dx;
		Vec8f vRz = Vec8f(rzRow) + laneOffsets * vDrzdx;

		int x = ixmin;
		// SIMD body: process [x, x+7] in chunks of 8. Stop early so the
		// 8-uint16 load doesn't read past row end into the next row's
		// memory (write-back through blendv would corrupt it).
		const int xSimdEnd = ixmax - 7;
		for (; x <= xSimdEnd; x += 8) {
			const Vec8f vW2 = Vec8f(1.0f) - vW0 - vW1;
			Vec8fb bary = (vW0 >= 0.0f) & (vW1 >= 0.0f) & (vW2 >= 0.0f);
			if (horizontal_or(bary)) {
				const Vec8f vZ = 1.0f / vRz;
				Vec8i enc = Vec8i(0xFF80) - roundi(vZ * vZScale);
				// Clamp to [0, 0xFFFF].
				enc = max(enc, Vec8i(0));
				enc = min(enc, Vec8i(0xFFFF));

				// Load 8 existing uint16; extend to int32 for compare.
				Vec8us existing_c;
				existing_c.load(zRow + x);
				const Vec8i existing = extend(existing_c);
				Vec8ib pass = enc > existing;
				pass &= Vec8ib(bary);
				if (horizontal_or(pass)) {
					// Store Z under mask. compress() packs 8 int32 → 8
					// uint16; _mm_blendv_epi8 does masked byte-level
					// select (each pair of mask bytes gates one uint16).
					// Same pattern as TheOtherBarry::apply_exact.
					const __m128i encU16 = compress(Vec8ui(enc));
					const __m128i maskU16 = compress(Vec8ui(Vec8i(pass)));
					*(__m128i*)(zRow + x) = _mm_blendv_epi8(
						*(__m128i*)(zRow + x), encU16, maskU16);

					// PolyId: scalar 8-byte fallback under the same mask.
					if (idByte) {
						alignas(32) int mask_l[8];
						Vec8i(pass).store(mask_l);
						for (int lane = 0; lane < 8; ++lane) {
							if (mask_l[lane]) idRow[x + lane] = idByte;
						}
					}
				}
			}
			vW0 += vDw0dx8;
			vW1 += vDw1dx8;
			vRz += vDrzdx8;
		}
		// Scalar tail for the last (ixmax - x + 1) pixels.
		float w0Tail = w0Row + float(x - ixmin) * dw0dx;
		float w1Tail = w1Row + float(x - ixmin) * dw1dx;
		float rzTail = rzRow + float(x - ixmin) * drzdx;
		for (; x <= ixmax; ++x) {
			const float w2 = 1.0f - w0Tail - w1Tail;
			if (w0Tail >= 0.0f && w1Tail >= 0.0f && w2 >= 0.0f) {
				if (rzTail > 0.0f) {
					int enc = 0xFF80 - int((1.0f / rzTail) * zScale);
					if (enc < 0) enc = 0;
					if (enc > 0xFFFF) enc = 0xFFFF;
					const uint16_t cand = uint16_t(enc);
					if (cand > zRow[x]) {
						zRow[x] = cand;
						if (idByte) idRow[x] = idByte;
					}
				}
			}
			w0Tail += dw0dx;
			w1Tail += dw1dx;
			rzTail += drzdx;
		}
	}
}

void MekaleleShadowDepth(Face *F, Vertex** V, dword numVerts, dword /*miplevel*/,
                          const fds::RenderTarget& /*rt*/,
                          const fds::CameraContext& /*cam*/)
{
	ShadowMap *sm = g_currentShadowMap;
	if (!sm) return;
	if (numVerts < 3) return;

	// Capture clipper outputs that aren't within the input triangle's
	// convex hull, to find clipper edge cases the near-skip in the
	// orchestrator missed. Capped to first 8 events.
	const bool sValidate = fds::FeatureFlags::shadow_validate();
	if (sValidate && F) {
		static std::atomic<int> sLogged{0};
		const float Ax = F->A->PX, Ay = F->A->PY;
		const float Bx = F->B->PX, By = F->B->PY;
		const float Cx = F->C->PX, Cy = F->C->PY;
		const float denom = (Ax - Cx) * (By - Cy) - (Bx - Cx) * (Ay - Cy);
		// Skip degenerate inputs: |denom| < 10 means triangle area < 5
		// pixels² in 2D — bary math gives wild values from float noise.
		if (std::fabs(denom) >= 10.0f) {
			const float invDen = 1.0f / denom;
			const float slack = 0.05f;
			bool bad = false;
			for (dword i = 0; i < numVerts && !bad; ++i) {
				const float Ox = V[i]->PX, Oy = V[i]->PY;
				if (!std::isfinite(Ox) || !std::isfinite(Oy)) { bad = true; break; }
				const float a = ((Ox - Cx) * (By - Cy) - (Bx - Cx) * (Oy - Cy)) * invDen;
				const float b = ((Ax - Cx) * (Oy - Cy) - (Ox - Cx) * (Ay - Cy)) * invDen;
				const float c = 1.0f - a - b;
				if (a < -slack || b < -slack || c < -slack ||
				    a > 1.0f + slack || b > 1.0f + slack || c > 1.0f + slack) {
					bad = true;
				}
			}
			if (bad && sLogged.fetch_add(1) < 8) {
				std::fprintf(stderr,
					"[SHADOW-CLIP] t=%d frame=%.2f n=%u Face=%p  "
					"Az=%.3g Bz=%.3g Cz=%.3g  "
					"in: A(%.1f,%.1f)F%x B(%.1f,%.1f)F%x C(%.1f,%.1f)F%x  out:",
					int(Timer.load()), CurFrame, numVerts, (void*)F,
					F->A->TPos.z, F->B->TPos.z, F->C->TPos.z,
					Ax, Ay, (unsigned)F->A->Flags,
					Bx, By, (unsigned)F->B->Flags,
					Cx, Cy, (unsigned)F->C->Flags);
				for (dword i = 0; i < numVerts; ++i) {
					std::fprintf(stderr, " (%.1f,%.1f)",
						V[i]->PX, V[i]->PY);
				}
				std::fprintf(stderr, "\n");
			}
		}
	}

	// Always write the face's material ID (+1 so the 0-sentinel
	// "unassigned" stays distinct from matID=0) into the shadow
	// buffer's polyId attachment, regardless of render mode. The
	// lighting kernel decides whether to USE it (PolyId mode) or
	// ignore it (Depth mode) via g_shadowMode. Unconditional write
	// lets the M-key viz read polyId even while rendering in Depth.
	uint16_t idOverride = (F && F->Txtr) ? uint16_t(F->Txtr->ID + 1) : 0;

	// Triangulate the clipped n-gon as a fan from V[0] — same shape as
	// Mekalele's tri loop. Per-triangle: compute the RZ screen-space
	// gradient via the affine inverse of the (v2-v1, v3-v1) screen
	// matrix, then hand off to ShadowBarry which does tile-hierarchical
	// AVX2 rasterization.
	extern thread_local bool g_inDynamicShadowBake;
	const bool useDynamic = g_inDynamicShadowBake;
	ShadowBarry r(sm, idOverride, useDynamic);
	for (dword i = 2; i < numVerts; ++i) {
		const Vertex& v1 = *V[0];
		const Vertex& v2 = *V[i - 1];
		const Vertex& v3 = *V[i];
		const float mxx = v2.PX - v1.PX, mxy = v2.PY - v1.PY;
		const float myx = v3.PX - v1.PX, myy = v3.PY - v1.PY;
		const float det = mxx * myy - mxy * myx;
		if (std::fabs(det) <= 0.01f) continue;  // degenerate
		const float invDet = 1.0f / det;
		const float imxx =  myy * invDet, imxy = -mxy * invDet;
		const float imyx = -myx * invDet, imyy =  mxx * invDet;
		r.drzdx = imxx * (v2.RZ - v1.RZ) + imxy * (v3.RZ - v1.RZ);
		r.drzdy = imyx * (v2.RZ - v1.RZ) + imyy * (v3.RZ - v1.RZ);
		r.rasterize_triangle(v1, v2, v3);
	}
}

// Extract the matID byte the lighting kernel reads from gb.txtr packs.
// Mekalele's packed format: miplevel(4) | matID(8) | swizzled_uv(20).
// Defined here so the kernel can compare against shadow buffer matIDs.
// matID maps to the +1-shifted value we wrote into sm.polyId.
