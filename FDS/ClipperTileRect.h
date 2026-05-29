#pragma once

#include <climits>

namespace fds {

// Set by FrustumClipper::Render at the start of each call; read by
// the rasterizers (Mekalele, ShadowBarry, TheOtherBarry) to clamp
// their per-triangle tile iteration to the OWNING clipper tile.
// Without this, a triangle whose clipped vertex lands exactly on the
// shared tile boundary is rasterized by BOTH adjacent workers into
// the same 8x8 SIMD tile of the output buffer -> blendv RMW race.
//
// Values are inclusive tile-coord upper/lower bounds (tile.x in
// [tile_mx_lo, tile_mx_hi], same for y). Set to INT_MIN/INT_MAX when
// not in a clipper Render() so non-clipper callers (none today, but
// defensive) don't accidentally clamp.
struct ClipperTileRect {
    int tile_mx_lo;  // inclusive lower
    int tile_mx_hi;  // inclusive upper
    int tile_my_lo;
    int tile_my_hi;
};

extern thread_local ClipperTileRect g_clipperTileRect;

} // namespace fds
