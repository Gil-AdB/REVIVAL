#include "FaceTileBin.h"

#include "Base/FaceListContext.h"
#include "Base/MemCensus.h"
#include "Threads.h"

#include <algorithm>
#include <climits>
#include <semaphore>

namespace fds {

// Below this face count the two build walks + two pool dispatches cost more
// than the 30 tile walks they replace (each dispatch is ~12 us of enqueue on
// the tick thread — see Threads.h dispatchIndexed). Greets' offscreen mirror
// RTT passes run 18-2 000 faces and land here; the main passes (10k-40k) do
// not.
static constexpr int32_t kMinFacesToBin = 3000;

FaceTileBins& FaceTileBins_Instance() {
	static FaceTileBins s_bins;
	return s_bins;
}

bool FaceTileBins_Build(FaceTileBins& b, const FListEntry* fl, int32_t n,
                        int numTilesX, int numTilesY,
                        int tileSizeX, int tileSizeY,
                        int xres, int yres)
{
	b.valid = false;
	const int numTiles = numTilesX * numTilesY;
	if (n < kMinFacesToBin || numTiles <= 1 || tileSizeX <= 0 || tileSizeY <= 0)
		return false;

	// One chunk per pool worker, but never so few faces per chunk that the
	// per-chunk prefix bookkeeping dominates.
	auto& tp = ThreadPool::instance();
	int numChunks = (int)std::min<size_t>(tp.size(), 64);
	if (numChunks < 1) numChunks = 1;
	while (numChunks > 1 && n / numChunks < 512) --numChunks;
	const int32_t chunk = (n + numChunks - 1) / numChunks;

	b.numTiles = numTiles;
	if ((int32_t)b.range.size() < n) b.range.resize((size_t)n);
	b.counts.assign((size_t)numChunks * numTiles, 0);
	b.start.resize((size_t)numTiles + 1);

	const int lastI = numTilesX - 1, lastJ = numTilesY - 1;

	// ---- pass 1: per-chunk per-tile counts + cache each face's tile range --
	// The range is packed 4x8 bits (i0,i1,j0,j1) so pass 2 re-reads 4 B/face
	// instead of the 24 B FListEntry.
	auto countChunk = [&](int c) {
		const int32_t lo = (int32_t)c * chunk;
		const int32_t hi = std::min<int32_t>(lo + chunk, n);
		int32_t* cnt = b.counts.data() + (size_t)c * numTiles;
		for (int32_t k = lo; k < hi; ++k) {
			const FListEntry& e = fl[k];
			// Off-screen in any axis => rejected by EVERY tile, exactly as the
			// per-tile 4-compare does (tile 0 starts at 0, the last ends at
			// xres/yres). 0xFF marks "no tiles" for pass 2.
			if (e.bbMaxX < 0 || e.bbMinX >= xres ||
			    e.bbMaxY < 0 || e.bbMinY >= yres) { b.range[(size_t)k] = 0xFFu; continue; }
			int i0 = e.bbMinX <= 0 ? 0 : std::min<int>(e.bbMinX / tileSizeX, lastI);
			int i1 = e.bbMaxX <= 0 ? 0 : std::min<int>(e.bbMaxX / tileSizeX, lastI);
			int j0 = e.bbMinY <= 0 ? 0 : std::min<int>(e.bbMinY / tileSizeY, lastJ);
			int j1 = e.bbMaxY <= 0 ? 0 : std::min<int>(e.bbMaxY / tileSizeY, lastJ);
			b.range[(size_t)k] = (uint32_t)i0 | ((uint32_t)i1 << 8) |
			                     ((uint32_t)j0 << 16) | ((uint32_t)j1 << 24);
			for (int j = j0; j <= j1; ++j) {
				int32_t* row = cnt + j * numTilesX;
				for (int i = i0; i <= i1; ++i) ++row[i];
			}
		}
	};

	std::counting_semaphore<INT_MAX> done{0};
	dispatchIndexed(numChunks, &done, countChunk);
	for (int k = 0; k < numChunks; ++k) done.acquire();

	// ---- prefix sum: tile-major, chunk-minor ------------------------------
	// This ordering is what preserves FList order inside a tile: chunk c's
	// faces occupy a contiguous run that precedes chunk c+1's, and within a
	// chunk pass 2 appends in increasing k.
	int32_t total = 0;
	for (int t = 0; t < numTiles; ++t) {
		b.start[(size_t)t] = total;
		for (int c = 0; c < numChunks; ++c) {
			int32_t& slot = b.counts[(size_t)c * numTiles + t];
			const int32_t cnt = slot;
			slot = total;                 // becomes this (chunk,tile)'s write cursor
			total += cnt;
		}
	}
	b.start[(size_t)numTiles] = total;
	if ((int32_t)b.arena.size() < total) b.arena.resize((size_t)total);

	// ---- pass 2: scatter Face* into each (chunk, tile) run -----------------
	auto fillChunk = [&](int c) {
		const int32_t lo = (int32_t)c * chunk;
		const int32_t hi = std::min<int32_t>(lo + chunk, n);
		int32_t* cur = b.counts.data() + (size_t)c * numTiles;
		Face** arena = b.arena.data();
		for (int32_t k = lo; k < hi; ++k) {
			const uint32_t r = b.range[(size_t)k];
			if (r == 0xFFu) continue;
			Face* f = fl[k].face;
			const int i0 = int(r & 0xFF), i1 = int((r >> 8) & 0xFF);
			const int j0 = int((r >> 16) & 0xFF), j1 = int((r >> 24) & 0xFF);
			for (int j = j0; j <= j1; ++j) {
				int32_t* row = cur + j * numTilesX;
				for (int i = i0; i <= i1; ++i) arena[row[i]++] = f;
			}
		}
	};

	dispatchIndexed(numChunks, &done, fillChunk);
	for (int k = 0; k < numChunks; ++k) done.acquire();

	b.valid = true;
	return true;
}

// ── --mem_census: the binning arena ────────────────────────────────────────
// Both vectors grow monotonically to the largest pass this process has run
// and are then reused, so this is a per-process high-water mark, not a
// per-frame allocation. Nothing here scales with resolution — only with the
// face count and with how many tiles each face straddles.
static void MemCensus_FaceTileBin() {
	const FaceTileBins& b = FaceTileBins_Instance();
	const size_t arena = b.arena.capacity() * sizeof(Face*);
	const size_t scratch = b.range.capacity() * sizeof(uint32_t)
	                     + b.counts.capacity() * sizeof(int32_t)
	                     + b.start.capacity()  * sizeof(int32_t);
	MemCensus::add("raster", "face->tile bin arena", arena, false,
		"one Face* per (face, tile) pair the bbox test admits; ~1.3-1.5x the face count");
	MemCensus::add("raster", "face->tile bin scratch", scratch, false,
		"packed tile range per face + per-(chunk, tile) prefix cursors");
}
FDS_MEMCENSUS_REPORTER(MemCensus_FaceTileBin);

} // namespace fds
