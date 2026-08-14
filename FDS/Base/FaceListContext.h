#ifndef FDS_FACE_LIST_CONTEXT_H_INCLUDED
#define FDS_FACE_LIST_CONTEXT_H_INCLUDED

#include <cstdint>
#include <vector>

struct Face;

namespace fds {

// One slot in the per-pass face list. Stores the sort key inline next
// to the Face* so the radix sort doesn't have to dereference Face* to
// fetch SortZ.DW — that pointer-chase was a cache miss per comparison
// on the legacy `Face**` layout. With sortKey co-located, each sort
// iteration reads contiguous slots.
//
// SIZE, kept honest because two audits mis-priced this list from the stale
// figure that used to be here ("16-byte slots, 2 per 32-byte cacheline"):
// sizeof(FListEntry) is 24 B — sortKey(4) + 4 B of PADDING + face(8) +
// bbox(8) — since the bbox fields were added. Reordering does not recover the
// padding (8+4+8 = 20 still rounds to 24 at alignof(Face*) = 8); only shrinking
// `face` to a 32-bit index would. That matters because the list is allocated
// TWICE per context (fStorage + sStorage) and once per SHADOW MAP: see the
// --mem_census row `shadow.scratch/per-light FList`, which is 83 MiB on greets
// and 403 MiB with --greets_displace, filled to 2.3 % of capacity.
//
// sortKey is a copy of Face::SortZ.DW (the 32-bit IEEE float bit pattern
// — the radix sort treats it as an unsigned integer key, which works
// because all values written to SortZ.F at fill time are non-negative).
//
// bbMinX/Y..bbMaxX/Y — the face's screen-space bounding box (quantized to
// int16, floor/ceil with a 1px margin), stamped at FList-build time from
// the projected PX/PY (docs/ENVDYN_DISPLACEMENT_PLAN.md B5 / S2 tile
// pre-reject). The tile-walk rasterizer dispatchers (RenderInner*) reject
// a face whose bbox misses the tile rect BEFORE dereferencing the Face —
// so a rejected face costs only this sequential read, not the three
// scattered Vertex loads the visibility test would do. Kept inline in the
// FListEntry so the reject reads the same 16-byte-ish slot the walk
// already streams (co-located with sortKey/face). Default = "covers all"
// (INT16_MIN..INT16_MAX) so the reject never fires unless the box was
// filled: near-plane-straddling faces (PX/PY invalid) and every non-main
// pass that doesn't fill it stay conservatively un-rejected — byte-safe.
// This is a PURE reject (the clipper already clips to the tile rect, so
// output is byte-identical); --tile_bbox_cull gates it for A/B.
struct FListEntry {
    uint32_t sortKey;
    Face*    face;
    int16_t  bbMinX = -32768, bbMinY = -32768;
    int16_t  bbMaxX =  32767, bbMaxY =  32767;
};

// Per-pass face list + counters. The main pass and each shadow pass
// build one of these — Transform_Objects fills it from the scene
// geometry projected through the active CameraContext, then the sort +
// rasterizer dispatchers walk it. Per-light shadow parallelism needs
// one FaceListContext per light, with its own backing storage, so the
// per-light workers don't race on a shared FList.
//
// Replaces these scattered file-scope globals:
//   FList, SList                                 (sort buffers)
//   CAll, CPolys, COmnies, CPcls, Polys          (per-pass counts)
//
// Storage is owned by fStorage / sStorage; fList / sList are raw views
// kept in sync with storage.data() so legacy code reading the bare
// `FList`/`SList` reference aliases still works. Call resize(n) to
// (re)allocate both buffers; existing call sites that used to call
// FList_Allocate / new[] / make_unique now collapse to one method.
struct FaceListContext {
    std::vector<FListEntry> fStorage;
    std::vector<FListEntry> sStorage;
    FListEntry *fList = nullptr;
    FListEntry *sList = nullptr;     // scratch for radix sort
    int32_t  cAll    = 0;          // total slots used in fList
    int32_t  cPolys  = 0;          // face count (excludes omnis + particles)
    int32_t  cOmnies = 0;          // omni flare entries
    int32_t  cPcls   = 0;          // particle face entries
    int32_t  polys   = 0;          // total polys in the scene (informational)

    void resize(size_t n) {
        fStorage.resize(n);
        sStorage.resize(n);
        fList = fStorage.data();
        sList = sStorage.data();
    }
};

} // namespace fds

#endif
