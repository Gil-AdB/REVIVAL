#ifndef FDS_FACE_BBOX_H_INCLUDED
#define FDS_FACE_BBOX_H_INCLUDED

#include "Face.h"
#include "FaceListContext.h"

#include <math.h>
#include <algorithm>

namespace fds {

// S2 / B5 tile pre-reject bbox stamp — the ONE definition, shared by every
// FList builder. Extracted from Transform.cpp's per-face push (which had it
// inline) because the scene-local mirror transforms (CITY.CPP / CHASE.CPP
// `Reflected_Transform`, which build FList by hand for the water-reflection
// underlay pass) pushed `{sortKey, face}` aggregates and left the bbox at its
// cover-all default. That made `--tile_bbox_cull` completely INERT on those
// passes: every face was handed to every tile's clipper. Measured on city
// t=1961 (1512x848, 6x5 tiles): the main pass stamps 98.8 % of its entries and
// averages 1.45 tiles touched per face -> 29 671 (face, tile) pairs, while the
// mirror pass was 100 % cover-all -> 621 180 pairs, a 21x multiplier on the
// SAME geometry, feeding FrustumClipper::Render (docs/PERF_STATE.md 00b row 3).
//
// CONTRACT, unchanged from the original site:
//  - The box is a conservative superset of the un-clipped triangle (floor/ceil
//    with a 1 px margin, int16-saturated). The clipper only SHRINKS coverage,
//    so a box that misses a tile means zero output there -> rejecting is
//    byte-identical to clipping.
//  - Valid only when all three verts are in FRONT of the near plane:
//    behind-near PX/PY are stale (the projection skips the divide there), so
//    those faces get the cover-all box and are never rejected.
//  - Read PX/PY straight off the face's own Vertex pointers (A/B/C — exactly
//    what the clipper + rasterizer read), so it is robust for meshes whose SoA
//    `F->*_idx` aren't populated.
//
// `nearZ` is the pass's own near plane (cam.nearZ for the main transform,
// Scene::NZP for the hand-written mirror transforms — the same value).
//
// Transform.cpp DELIBERATELY keeps its own verbatim copy of this body inline
// rather than calling here: its per-mesh face loop is the one documented in
// that file as pin-fragile under `-ffp-contract=fast` + LTO ("merely carrying
// this CALL inside the per-mesh body moved all three scene pins"), and the
// perf case for this header is entirely on the mirror-transform side. Keep the
// two bodies in sync by hand; any divergence shows up as a pin move.
inline void FaceBBox_Stamp(FListEntry* e, const Face* F, float nearZ) {
	const Vertex* va = F->A; const Vertex* vb = F->B; const Vertex* vc = F->C;
	const float za = va->TPos_AOS.z, zb = vb->TPos_AOS.z, zc = vc->TPos_AOS.z;
	if (za > nearZ && zb > nearZ && zc > nearZ) {
		const float pxa = va->PX, pxb = vb->PX, pxc = vc->PX;
		const float pya = va->PY, pyb = vb->PY, pyc = vc->PY;
		const float minx = std::min(std::min(pxa, pxb), pxc);
		const float maxx = std::max(std::max(pxa, pxb), pxc);
		const float miny = std::min(std::min(pya, pyb), pyc);
		const float maxy = std::max(std::max(pya, pyb), pyc);
		auto satI16 = [](float v) -> int16_t {
			if (v < -32768.0f) return -32768;
			if (v >  32767.0f) return  32767;
			return int16_t(v);
		};
		e->bbMinX = satI16(floorf(minx) - 1.0f);
		e->bbMinY = satI16(floorf(miny) - 1.0f);
		e->bbMaxX = satI16(ceilf (maxx) + 1.0f);
		e->bbMaxY = satI16(ceilf (maxy) + 1.0f);
	} else {
		e->bbMinX = e->bbMinY = -32768;
		e->bbMaxX = e->bbMaxY =  32767;
	}
}

} // namespace fds

#endif
