#ifndef FDS_FACE_LIST_CONTEXT_H_INCLUDED
#define FDS_FACE_LIST_CONTEXT_H_INCLUDED

#include <cstdint>

struct Face;

namespace fds {

// One slot in the per-pass face list. Stores the sort key inline next
// to the Face* so the radix sort doesn't have to dereference Face* to
// fetch SortZ.DW — that pointer-chase was a cache miss per comparison
// on the legacy `Face**` layout. With sortKey co-located, each sort
// iteration reads contiguous 16-byte slots (2 per 32-byte cacheline).
//
// sortKey is a copy of Face::SortZ.DW (the 32-bit IEEE float bit pattern
// — the radix sort treats it as an unsigned integer key, which works
// because all values written to SortZ.F at fill time are non-negative).
struct FListEntry {
    uint32_t sortKey;
    Face*    face;
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
// fList / sList are non-owning views; the orchestrator allocates the
// backing storage (currently `std::unique_ptr<FListEntry[]>`).
struct FaceListContext {
    FListEntry *fList = nullptr;
    FListEntry *sList = nullptr;     // scratch for radix sort
    int32_t  cAll    = 0;          // total slots used in fList
    int32_t  cPolys  = 0;          // face count (excludes omnis + particles)
    int32_t  cOmnies = 0;          // omni flare entries
    int32_t  cPcls   = 0;          // particle face entries
    int32_t  polys   = 0;          // total polys in the scene (informational)
};

} // namespace fds

#endif
