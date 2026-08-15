#ifndef FDS_FACE_TILE_BIN_H_INCLUDED
#define FDS_FACE_TILE_BIN_H_INCLUDED

#include <cstdint>
#include <vector>

struct Face;

namespace fds {

struct FListEntry;

// Face -> raster-tile binning for the opaque tile pass.
//
// WHAT IT REPLACES. Every raster tile used to re-walk the WHOLE face list and
// 4-compare-reject the faces whose screen bbox misses its rect
// (--tile_bbox_cull). At city t=1961 that is 30 tiles x 2 renderFrame passes x
// ~20 500 entries = 1.23 M FListEntry reads per frame to find the ~30 000
// (face, tile) pairs that actually exist. Binning walks the list TWICE (count,
// then fill) instead of thirty times, and hands each tile a dense, sequential
// `Face*` run.
//
// ORDER IS PRESERVED EXACTLY, which is the correctness bar: the rasterizers are
// Z-buffered but not order-independent (the forward fillers blend, the mip
// hysteresis and the mirror-mask centroid gate are stateful), so a tile MUST
// see its faces in the same sequence the walk produced. The build splits FList
// into contiguous chunks, and the prefix sum lays each tile's slots out
// chunk-major — so tile t's run is the subsequence of FList order that passes
// t's bbox test, byte-for-byte the same sequence the walk yields.
//
// The bin is a PURE restatement of the --tile_bbox_cull test, so it is only
// built when that flag is on (flag off = the walk rejects nothing, and every
// tile legitimately sees every face).
//
// MEMORY: two vectors reused across frames, grown monotonically — no per-frame
// allocation once the scene's face count has been seen. `arena` holds
// sum-over-faces(tiles touched) Face* (1.3-1.5x the face count in practice);
// `range` holds one packed uint32 per face.
struct FaceTileBins {
	std::vector<Face*>   arena;   // tile-major, chunk-major inside a tile
	std::vector<int32_t> start;   // numTiles + 1 offsets into arena
	std::vector<uint32_t> range;  // per-face packed (i0,i1,j0,j1), build scratch
	std::vector<int32_t> counts;  // numChunks * numTiles, build scratch
	int  numTiles = 0;
	bool valid    = false;

	Face* const* tileFaces(int t) const { return arena.data() + start[t]; }
	int32_t      tileCount(int t) const { return start[t + 1] - start[t]; }
};

// The one process-wide instance renderFrame builds and its tile jobs read.
// renderFrame is single-threaded by construction (it already drives the shared
// `renderns::tileDone` semaphore and the FList globals), and the build sits
// between the sort and the tile dispatch with no nested renderFrame in that
// window — so one instance is enough and nothing needs a lock.
FaceTileBins& FaceTileBins_Instance();

// Build `b` from the first `n` entries of `fl`. Returns true if `b` is usable;
// false (and b.valid=false) when the tile geometry is degenerate or the list is
// too small to be worth binning — callers fall back to the legacy walk.
//
// The tile grid must match the caller's exactly: tile (i, j) owns
// x in [tileSizeX*i, tileSizeX*(i+1)) and y in [tileSizeY*j, tileSizeY*(j+1)),
// except the last row/column, which absorb the remainder up to xres/yres.
bool FaceTileBins_Build(FaceTileBins& b, const FListEntry* fl, int32_t n,
                        int numTilesX, int numTilesY,
                        int tileSizeX, int tileSizeY,
                        int xres, int yres);

} // namespace fds

#endif
