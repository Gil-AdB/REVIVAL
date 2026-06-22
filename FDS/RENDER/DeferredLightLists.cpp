// Deferred per-tile / per-strip light culling — split out of
// DeferredLighting.cpp (verbatim function moves; see DeferredCommon.h
// for the shared types and the split layout).
//
// Builds the per-frame compacted TileLights SoA consumed by the surface
// and transparent kernels: per-tile depth bounds from ZPage16, the
// mirror-footprint presence grids, screen-space range-sphere binning,
// the spot cone cull, and the strip-flavored (Y-only) variant for the
// unified TBR transparent path.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <limits>

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FeatureFlags.h"
#include "RENDER/DeferredCommon.h"

// Build per-tile compacted SoA. For each omni: project its view-space
// bounding sphere into screen space, find overlapping tile rects, then
// **append the omni's values** (not its index) into each overlapping
// tile's contiguous SoA arrays. Memory cost is O(omnis × overlapped
// tiles × bytes-per-omni) which for City is ~30 × 4 × 32 ≈ 4 KiB; for
// Greets ~10 × 4 × 32 ≈ 1.3 KiB. Negligible.
//
// The vec inner loop benefits because `tl.posX[slot..slot+8)` is now
// 32-byte aligned and contiguous — one `load_a` instead of four
// `ld1.s {v}[lane]` gathers per Vec4f. Scalar benefits too via
// straight-line prefetchable access.
// Per-tile depth bounds. Scans ZPage16 once per tile (already populated
// by the G-buffer pass), finds the closest and farthest zEnc with
// geometry, and converts back to view-space z. Tiles with no geometry
// get zMin=+inf / zMax=-inf so all lights are culled for them.
//
// Encoding reminder: zEnc = 0xFF80 - g_zscale*z, so larger zEnc means
// closer pixel. zEnc == 0 means the pixel was never touched.
void computeTileDepthBounds(TileLights *tileLights, int numTilesX, int numTilesY,
                                   int tileSizeX, int tileSizeY, int xres, int yres,
                                   float invZScale, const uint16_t *zpage16)
{
	const uint16_t *zp = zpage16;
	for (int j = 0; j < numTilesY; ++j) {
		const int y_lo = j * tileSizeY;
		const int y_hi = std::min(y_lo + tileSizeY, yres);
		for (int i = 0; i < numTilesX; ++i) {
			const int x_lo = i * tileSizeX;
			const int x_hi = std::min(x_lo + tileSizeX, xres);
			const int idx = j * numTilesX + i;

			__m128i vMaxZ = _mm_setzero_si128();           // chasing max
			__m128i vMinZ = _mm_set1_epi16(int16_t(0xFFFF)); // chasing min (zeros mapped up)
			uint16_t maxZ = 0;
			uint16_t minZ = 0xFFFF;

			for (int py = y_lo; py < y_hi; ++py) {
				const uint16_t *row = zp + size_t(py) * xres + x_lo;
				const int width = x_hi - x_lo;
				int px = 0;
				for (; px + 8 <= width; px += 8) {
					__m128i v = _mm_loadu_si128((const __m128i*)(row + px));
					// For min-tracking, replace 0 with 0xFFFF so untouched
					// pixels don't pull the minimum down.
					__m128i isZero = _mm_cmpeq_epi16(v, _mm_setzero_si128());
					__m128i vForMin = _mm_or_si128(v, isZero);
					vMaxZ = _mm_max_epu16(vMaxZ, v);
					vMinZ = _mm_min_epu16(vMinZ, vForMin);
				}
				for (; px < width; ++px) {
					uint16_t z = row[px];
					if (z > maxZ) maxZ = z;
					if (z != 0 && z < minZ) minZ = z;
				}
			}
			// Horizontal reduce the SIMD halves.
			alignas(16) uint16_t mx[8], mn[8];
			_mm_store_si128((__m128i*)mx, vMaxZ);
			_mm_store_si128((__m128i*)mn, vMinZ);
			for (int k = 0; k < 8; ++k) {
				if (mx[k] > maxZ) maxZ = mx[k];
				if (mn[k] < minZ) minZ = mn[k];
			}

			if (maxZ == 0) {
				tileLights[idx].zMin = std::numeric_limits<float>::infinity();
				tileLights[idx].zMax = -std::numeric_limits<float>::infinity();
			} else {
				// Larger zEnc → closer, so maxZ-zEnc maps to zMin-view.
				tileLights[idx].zMin = float(0xFF80 - maxZ) * invZScale;
				tileLights[idx].zMax = float(0xFF80 - minZ) * invZScale;
			}
		}
	}
}

// Per-region (tile or strip) presence bitmask over the planar-mirror
// pre-stamp plane (gb.mirrorMask): bit i set ⇔ some pixel in the region
// carries mirror id i. Clone lights (Omni::mirrorId != 0) can only ever
// light pixels inside their mirror's stamped footprint — the per-pixel
// pmid filter rejects them everywhere else — so the light-list builders
// skip regions whose presence bit is clear. Without this, every screen-
// cluster mirror's 15 cloned omnis (range ≫ scene size) land in every
// tile: greets with 9 mirrors ≈ 150 lights/tile, blowing past
// DEFERRED_MAX_LIGHTS=128 (silently dropping REAL lights) and paying
// the full per-pixel loop for lights that can't contribute.
//
// NOTE if compound mirrors are ever re-enabled: compounds don't stamp
// their own id (they ride the parent's footprint), so their omnis must
// either be tagged with the parent id for this cull or exempted here.
void computeMirrorPresenceGrid(const uint8_t *mask, int w, int h,
                                      int regionW, int regionH,
                                      int regionsX, int regionsY,
                                      uint32_t *out)
{
	for (int ry = 0; ry < regionsY; ++ry) {
		const int y0 = ry * regionH;
		const int y1 = std::min(h, y0 + regionH);
		for (int rx = 0; rx < regionsX; ++rx) {
			const int x0 = rx * regionW;
			const int x1 = std::min(w, x0 + regionW);
			uint32_t bits = 0;
			for (int y = y0; y < y1; ++y) {
				const uint8_t *row = mask + size_t(y) * size_t(w) + x0;
				const int n = x1 - x0;
				int x = 0;
				// 8-byte chunks with a zero fast-path — the plane is
				// mostly zeros outside mirror footprints.
				for (; x + 8 <= n; x += 8) {
					uint64_t chunk;
					std::memcpy(&chunk, row + x, 8);
					if (chunk == 0) continue;
					for (int k = 0; k < 8; ++k) {
						const uint8_t id = row[x + k];
						if (id) bits |= (id < 32) ? (1u << id) : 0xffffffffu;
					}
				}
				for (; x < n; ++x) {
					const uint8_t id = row[x];
					if (id) bits |= (id < 32) ? (1u << id) : 0xffffffffu;
				}
			}
			out[size_t(ry) * size_t(regionsX) + rx] = bits;
		}
	}
}

void buildTileLightLists(TileLights *tileLights, int numTilesX, int numTilesY,
                                 int tileSizeX, int tileSizeY, int xres, int yres,
                                 const ViewLightsSoA &lights, int numLights,
                                 const uint32_t *tileMirrorPresence,
                                 const fds::CameraContext &cam)
{
	const float FOVX = cam.fovX, FOVY = cam.fovY;
	const float CntrEX = cam.cntrEX, CntrEY = cam.cntrEY;
	const int numTiles = numTilesX * numTilesY;
	for (int t = 0; t < numTiles; ++t) {
		tileLights[t].count = 0;
	}

	// Per-tile depth-bounded light culling. Computed by
	// computeTileDepthBounds (caller fills tileLights[].zMin / zMax);
	// we reject lights whose view-space z extent doesn't overlap the
	// tile's pixel depth range. Default on.
	const bool zCullEnabled = fds::FeatureFlags::deferred_zcull();

	// Per-tile chunk bounding spheres for the spot cone cull.
	const bool coneCull = fds::FeatureFlags::spot_cone_cull();
	static TileChunkSphere chunk[DEFERRED_NUM_TILES];
	if (coneCull) {
		for (int j = 0; j < numTilesY; ++j) {
			for (int i = 0; i < numTilesX; ++i) {
				const int idx = j * numTilesX + i;
				chunk[idx] = tileChunkSphere(
					float(i * tileSizeX), float(std::min((i+1) * tileSizeX, xres)),
					float(j * tileSizeY), float(std::min((j+1) * tileSizeY, yres)),
					tileLights[idx].zMin, tileLights[idx].zMax);
			}
		}
	}

	for (int li = 0; li < numLights; ++li) {
		const float vx = lights.posX[li];
		const float vy = lights.posY[li];
		const float vz = lights.posZ[li];
		const float r2 = lights.range2[li];
		const float r  = std::sqrt(r2);
		const float vz_minus_r = vz - r;
		const float vz_plus_r  = vz + r;

		// Sphere entirely behind camera: skip.
		if (vz + r < 0.0f) continue;

		int sx_min, sx_max, sy_min, sy_max;
		if (vz - r < 1.0f) {
			// Sphere straddles or is in front of near plane — be
			// conservative and tag every tile. (This is rare for
			// City; keeps the math simple.)
			sx_min = 0;        sx_max = xres - 1;
			sy_min = 0;        sy_max = yres - 1;
		} else {
			// Pinhole projection of bounding sphere — small-angle
			// approximation. Center: (CntrEX + vx*FOVX/vz, CntrEY -
			// vy*FOVY/vz). Radius on-screen: r * FOVX/vz (use FOVY for
			// vertical). Slightly over-estimates near the edges of
			// the FOV but that just lights tiles that miss the per-
			// pixel cull, no correctness impact.
			const float invZ = 1.0f / vz;
			const float cx   = CntrEX + vx * FOVX * invZ;
			const float cy   = CntrEY - vy * FOVY * invZ;
			const float rx   = r * FOVX * invZ;
			const float ry   = r * FOVY * invZ;
			sx_min = std::max(0,        int(std::floor(cx - rx)));
			sx_max = std::min(xres - 1, int(std::ceil (cx + rx)));
			sy_min = std::max(0,        int(std::floor(cy - ry)));
			sy_max = std::min(yres - 1, int(std::ceil (cy + ry)));
			if (sx_min > sx_max || sy_min > sy_max) continue;
		}

		const int tile_i_lo = sx_min / tileSizeX;
		const int tile_i_hi = std::min(numTilesX - 1, sx_max / tileSizeX);
		const int tile_j_lo = sy_min / tileSizeY;
		const int tile_j_hi = std::min(numTilesY - 1, sy_max / tileSizeY);

		const float Lpx = lights.posX[li];
		const float Lpy = lights.posY[li];
		const float Lpz = lights.posZ[li];
		const float Lcb = lights.colB[li];
		const float Lcg = lights.colG[li];
		const float Lcr = lights.colR[li];
		const float Lr2 = lights.range2[li];
		const float Lrr = lights.rRange[li];
		const float Ldx = lights.dirX[li];
		const float Ldy = lights.dirY[li];
		const float Ldz = lights.dirZ[li];
		const float Lci = lights.cosInner[li];
		const float Lco = lights.cosOuter[li];
		const uint32_t Lis = lights.isSpot[li];
		const int32_t  Lsi = lights.shadowMapIdx[li];
		const int32_t  Lss = lights.srcShadowMapIdx[li];
		const int32_t  Lscube = lights.srcCubeShadowIdx[li];
		const uint32_t Lbc = lights.bounceClamp[li];
		const float    Lmnx = lights.mirNX[li], Lmny = lights.mirNY[li];
		const float    Lmnz = lights.mirNZ[li], Lmd  = lights.mirD[li];
		const float    Lwx = lights.posWorldX[li];
		const float    Lwy = lights.posWorldY[li];
		const float    Lwz = lights.posWorldZ[li];
		const int32_t  Lci2 = lights.cubeShadowIdx[li];
		const uint32_t Lmid = lights.mirrorId[li];
		const float Lso = lights.sinOuter[li];
		const float Lwnx = lights.winMinX[li], Lwny = lights.winMinY[li], Lwnz = lights.winMinZ[li];
		const float Lwxx = lights.winMaxX[li], Lwxy = lights.winMaxY[li], Lwxz = lights.winMaxZ[li];

		int dbgPlaced = 0;
		for (int j = tile_j_lo; j <= tile_j_hi; ++j) {
			for (int i = tile_i_lo; i <= tile_i_hi; ++i) {
				const int idx = j * numTilesX + i;
				TileLights &tl = tileLights[idx];
				// Depth cull: skip if light's z-extent doesn't overlap
				// the tile's pixel depth range. Empty tiles have
				// zMin=+inf and zMax=-inf so this rejects everything.
				if (zCullEnabled &&
				    (vz_plus_r < tl.zMin || vz_minus_r > tl.zMax)) {
					continue;
				}
				// Spot cone cull: the tile's pixel chunk vs the cone.
				// The chunk's z-range is first clipped to the cone's
				// own z-extent — an unclipped deep tile makes a fat
				// sphere the expanded-cone test can't reject.
				if (coneCull && Lis && chunk[idx].valid) {
					const float pad   = r * Lso;
					const float czLo  = std::min(vz, vz + Ldz * r) - pad;
					const float czHi  = std::max(vz, vz + Ldz * r) + pad;
					const float zLoC  = std::max(tl.zMin, czLo);
					const float zHiC  = std::min(tl.zMax, czHi);
					if (zHiC < zLoC) continue;  // no z overlap at all
					const TileChunkSphere cs = tileChunkSphere(
					    float(i * tileSizeX), float(std::min((i+1) * tileSizeX, xres)),
					    float(j * tileSizeY), float(std::min((j+1) * tileSizeY, yres)),
					    zLoC, zHiC);
					if (cs.valid &&
					    sphereOutsideCone(cs.cx, cs.cy, cs.cz, cs.R,
					                      Lpx, Lpy, Lpz, Ldx, Ldy, Ldz,
					                      r, Lco, Lso)) {
						continue;
					}
				}
				// Clone-light mirror-footprint cull (see
				// computeMirrorPresenceGrid). Ids ≥ 32 don't fit the
				// bitmask and are conservatively kept.
				if (Lmid != 0 && Lmid < 32 && tileMirrorPresence &&
				    !(tileMirrorPresence[idx] & (1u << Lmid))) {
					continue;
				}
				if (tl.count < DEFERRED_MAX_LIGHTS) {
					const int s = tl.count++;
					tl.posX[s]   = Lpx;
					tl.posY[s]   = Lpy;
					tl.posZ[s]   = Lpz;
					tl.colB[s]   = Lcb;
					tl.colG[s]   = Lcg;
					tl.colR[s]   = Lcr;
					tl.range2[s] = Lr2;
					tl.rRange[s] = Lrr;
					tl.dirX[s]     = Ldx;
					tl.dirY[s]     = Ldy;
					tl.dirZ[s]     = Ldz;
					tl.cosInner[s] = Lci;
					tl.cosOuter[s] = Lco;
					tl.isSpot[s]   = Lis;
					tl.shadowMapIdx[s] = Lsi;
					tl.srcShadowMapIdx[s] = Lss;
					tl.srcCubeShadowIdx[s] = Lscube;
					tl.bounceClamp[s] = Lbc;
					tl.mirNX[s] = Lmnx; tl.mirNY[s] = Lmny;
					tl.mirNZ[s] = Lmnz; tl.mirD[s]  = Lmd;
					tl.posWorldX[s] = Lwx;
					tl.posWorldY[s] = Lwy;
					tl.posWorldZ[s] = Lwz;
					tl.cubeShadowIdx[s] = Lci2;
					tl.mirrorId[s]      = Lmid;
					tl.winMinX[s] = Lwnx; tl.winMinY[s] = Lwny; tl.winMinZ[s] = Lwnz;
					tl.winMaxX[s] = Lwxx; tl.winMaxY[s] = Lwxy; tl.winMaxZ[s] = Lwxz;
				}
			}
		}
	}

	// Zero the padding slots (count..paddedCount) so the vec loop's
	// over-read produces range2=0 entries that fail the per-pixel
	// `len2 <= range2` mask and contribute nothing.
	for (int t = 0; t < numTiles; ++t) {
		TileLights &tl = tileLights[t];
		const int padded = (tl.count + 7) & ~7;
		const int pad_to = std::min(padded, DEFERRED_MAX_LIGHTS);
		for (int p = tl.count; p < pad_to; ++p) {
			tl.posX[p]   = 0.0f;
			tl.posY[p]   = 0.0f;
			tl.posZ[p]   = 0.0f;
			tl.colB[p]   = 0.0f;
			tl.colG[p]   = 0.0f;
			tl.colR[p]   = 0.0f;
			tl.range2[p] = 0.0f;
			tl.rRange[p] = 0.0f;
			tl.dirX[p]     = 0.0f;
			tl.dirY[p]     = 0.0f;
			tl.dirZ[p]     = 0.0f;
			tl.cosInner[p] = -2.0f;
			tl.cosOuter[p] = -2.0f;
			tl.isSpot[p]   = 0u;
			tl.shadowMapIdx[p] = -1;
			tl.srcShadowMapIdx[p] = -1;
			tl.srcCubeShadowIdx[p] = -1;
			tl.bounceClamp[p] = 0u;
			tl.mirNX[p] = 0.0f; tl.mirNY[p] = 0.0f;
			tl.mirNZ[p] = 0.0f; tl.mirD[p]  = 0.0f;
			tl.posWorldX[p] = 0.0f;
			tl.posWorldY[p] = 0.0f;
			tl.posWorldZ[p] = 0.0f;
			tl.cubeShadowIdx[p] = -1;
			// 0xffffffff in the padding slot so the per-pixel `==`
			// test against pixelMirrorId (always < 256) is always
			// false; the padded slots contribute nothing whatever
			// the pixel's mirror id.
			tl.mirrorId[p]      = 0xffffffffu;
			// Inverted window AABB → portal-test gate (winMin<=winMax) is
			// false, so padded slots never enter the portal test.
			tl.winMinX[p] =  1e30f; tl.winMinY[p] =  1e30f; tl.winMinZ[p] =  1e30f;
			tl.winMaxX[p] = -1e30f; tl.winMaxY[p] = -1e30f; tl.winMaxZ[p] = -1e30f;
		}
		tl.paddedCount = pad_to;
	}
}

// Strip-flavored light list builder (1D, Y-only) for the unified TBR
// path's transparent strip rendering. Mirrors buildTileLightLists but
// over 8-row Y-strips matching the TBR tile shape. Each strip's list
// is consumed by RenderXparClumpInStrip via a DeferredLightingCtx
// variant whose `tileLights` points at this array.
//
// DEFERRED_MAX_STRIPS lives in DeferredCommon.h (shared with the xpar
// strip dispatcher).
TileLights g_stripLights[DEFERRED_MAX_STRIPS];
static int        g_numStripLights = 0;

void buildStripLightLists(int numStrips, int stripHeight, int yres,
                                  const ViewLightsSoA &lights, int numLights,
                                  const uint32_t *stripMirrorPresence,
                                  const fds::CameraContext &cam)
{
	const float FOVY = cam.fovY;
	const float CntrEY = cam.cntrEY;
	if (numStrips > DEFERRED_MAX_STRIPS) numStrips = DEFERRED_MAX_STRIPS;
	for (int s = 0; s < numStrips; ++s) {
		g_stripLights[s].count = 0;
	}

	for (int li = 0; li < numLights; ++li) {
		const float vx = lights.posX[li];
		const float vy = lights.posY[li];
		const float vz = lights.posZ[li];
		const float r  = std::sqrt(lights.range2[li]);

		if (vz + r < 0.0f) continue;

		int sy_min, sy_max;
		if (vz - r < 1.0f) {
			sy_min = 0;        sy_max = yres - 1;
		} else {
			const float invZ = 1.0f / vz;
			const float cy   = CntrEY - vy * FOVY * invZ;
			const float ry   = r * FOVY * invZ;
			sy_min = std::max(0,       int(std::floor(cy - ry)));
			sy_max = std::min(yres - 1, int(std::ceil (cy + ry)));
			if (sy_min > sy_max) continue;
		}

		const int strip_lo = sy_min / stripHeight;
		const int strip_hi = std::min(numStrips - 1, sy_max / stripHeight);

		const float Lpx = lights.posX[li];
		const float Lpy = lights.posY[li];
		const float Lpz = lights.posZ[li];
		const float Lcb = lights.colB[li];
		const float Lcg = lights.colG[li];
		const float Lcr = lights.colR[li];
		const float Lr2 = lights.range2[li];
		const float Lrr = lights.rRange[li];
		const float Ldx = lights.dirX[li];
		const float Ldy = lights.dirY[li];
		const float Ldz = lights.dirZ[li];
		const float Lci = lights.cosInner[li];
		const float Lco = lights.cosOuter[li];
		const uint32_t Lis = lights.isSpot[li];
		const uint32_t Lmid = lights.mirrorId[li];

		for (int s = strip_lo; s <= strip_hi; ++s) {
			TileLights &tl = g_stripLights[s];
			// Clone-light mirror-footprint cull — strip flavor (see
			// computeMirrorPresenceGrid).
			if (Lmid != 0 && Lmid < 32 && stripMirrorPresence &&
			    !(stripMirrorPresence[s] & (1u << Lmid))) {
				continue;
			}
			if (tl.count < DEFERRED_MAX_LIGHTS) {
				const int idx = tl.count++;
				tl.posX[idx]   = Lpx;
				tl.posY[idx]   = Lpy;
				tl.posZ[idx]   = Lpz;
				tl.colB[idx]   = Lcb;
				tl.colG[idx]   = Lcg;
				tl.colR[idx]   = Lcr;
				tl.range2[idx] = Lr2;
				tl.rRange[idx] = Lrr;
				tl.dirX[idx]     = Ldx;
				tl.dirY[idx]     = Ldy;
				tl.dirZ[idx]     = Ldz;
				tl.cosInner[idx] = Lci;
				tl.cosOuter[idx] = Lco;
				tl.isSpot[idx]   = Lis;
				tl.mirrorId[idx] = Lmid;
				// Transparent path doesn't run the bounce portal test;
				// keep the window AABB inverted so the gate is a no-op
				// and nothing reads uninitialized strip memory.
				tl.winMinX[idx] =  1e30f; tl.winMinY[idx] =  1e30f; tl.winMinZ[idx] =  1e30f;
				tl.winMaxX[idx] = -1e30f; tl.winMaxY[idx] = -1e30f; tl.winMaxZ[idx] = -1e30f;
			}
		}
	}

	// Zero the padding slots so the vec loop's overread is benign
	// (range2=0 lanes fail the per-pixel `len2 <= range2` mask).
	for (int s = 0; s < numStrips; ++s) {
		TileLights &tl = g_stripLights[s];
		const int padded = (tl.count + 7) & ~7;
		const int pad_to = std::min(padded, DEFERRED_MAX_LIGHTS);
		for (int p = tl.count; p < pad_to; ++p) {
			tl.posX[p]   = 0.0f;
			tl.posY[p]   = 0.0f;
			tl.posZ[p]   = 0.0f;
			tl.colB[p]   = 0.0f;
			tl.colG[p]   = 0.0f;
			tl.colR[p]   = 0.0f;
			tl.range2[p] = 0.0f;
			tl.rRange[p] = 0.0f;
			tl.dirX[p]     = 0.0f;
			tl.dirY[p]     = 0.0f;
			tl.dirZ[p]     = 0.0f;
			tl.cosInner[p] = -2.0f;
			tl.cosOuter[p] = -2.0f;
			tl.isSpot[p]   = 0u;
			tl.mirrorId[p] = 0xffffffffu;
			tl.winMinX[p] =  1e30f; tl.winMinY[p] =  1e30f; tl.winMinZ[p] =  1e30f;
			tl.winMaxX[p] = -1e30f; tl.winMaxY[p] = -1e30f; tl.winMaxZ[p] = -1e30f;
		}
		tl.paddedCount = pad_to;
		// Strip Z bounds: not used by transparent kernel (depth is
		// already in the xpar G-buffer). Set sentinels.
		tl.zMin = -std::numeric_limits<float>::infinity();
		tl.zMax =  std::numeric_limits<float>::infinity();
	}
	g_numStripLights = numStrips;
}
