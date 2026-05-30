#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Per-mesh, per-frame-writable transformed-vertex state. The B-not-C
// split from docs/SOA_VERTEX_REFACTOR.md — input fields (Pos/N/Tangent/
// U/V/OrigBary) stay in the AoS Vertex struct; output fields (this
// struct) become per-mesh SoA arrays for SIMD-friendly mass-write from
// Transform_Objects.
//
// Phase 1 (this commit): allocated alongside the AoS Vertex output
// fields, written in parallel. Phase 2: Transform converts to wide-SIMD
// writing only SoA. Phase 4-5: consumers migrate from `Vtx->TPos_AOS.x`
// (AoS) to `T->frame->TPos_x[idx]` (SoA), then the AoS fields drop.
//
// Allocation: lazy via ensureSized() in Transform_Objects on first
// touch. One big slab keeps the field arrays cache-adjacent across a
// vertex's outputs (e.g. TPos_x/_y/_z share lines when iterating).
struct VertexFrame {
    // Arrays of length `capacity`, all sharing one slab. Indexed by
    // vertex's position in TriMesh::Verts[0..VIndex-1].
    float    *TPos_x       = nullptr;
    float    *TPos_y       = nullptr;
    float    *TPos_z       = nullptr;
    float    *TN_x         = nullptr;
    float    *TN_y         = nullptr;
    float    *TN_z         = nullptr;
    float    *TTangent_x   = nullptr;
    float    *TTangent_y   = nullptr;
    float    *TTangent_z   = nullptr;
    float    *PX           = nullptr;
    float    *PY           = nullptr;
    float    *RZ           = nullptr;
    float    *UZ           = nullptr;
    float    *VZ           = nullptr;
    float    *EUZ          = nullptr;
    float    *EVZ          = nullptr;
    uint32_t *Flags        = nullptr;
    uint32_t *BGRA         = nullptr;
    int       capacity     = 0;

    // 32-byte alignment so future Vec8f loads/stores in Phase 2 can use
    // aligned variants. Slab is over-allocated by the alignment.
    static constexpr size_t kAlign = 32;
    void *_slab = nullptr;  // raw alloc, freed in ~VertexFrame

    static constexpr int kFloatFieldCount = 16;
    static constexpr int kU32FieldCount   = 2;

    void ensureSized(int newCap) {
        if (newCap <= capacity) return;
        // Round capacity up to a multiple of 8 so Vec8f loads at the
        // tail don't read past end. Round size in bytes per field to
        // alignment.
        const int newCapPadded = (newCap + 7) & ~7;
        const size_t bytesPerFloatField = size_t(newCapPadded) * sizeof(float);
        const size_t bytesPerU32Field   = size_t(newCapPadded) * sizeof(uint32_t);
        const size_t total = bytesPerFloatField * kFloatFieldCount
                           + bytesPerU32Field   * kU32FieldCount
                           + kAlign;
        free(_slab);
        _slab = std::malloc(total);
        if (!_slab) { capacity = 0; return; }
        capacity = newCapPadded;
        std::memset(_slab, 0, total);
        // Carve aligned sub-arrays out of the slab.
        char *p = (char *)_slab;
        uintptr_t pi = (uintptr_t)p;
        pi = (pi + (kAlign - 1)) & ~(uintptr_t)(kAlign - 1);
        p = (char *)pi;
        TPos_x     = (float *)p; p += bytesPerFloatField;
        TPos_y     = (float *)p; p += bytesPerFloatField;
        TPos_z     = (float *)p; p += bytesPerFloatField;
        TN_x       = (float *)p; p += bytesPerFloatField;
        TN_y       = (float *)p; p += bytesPerFloatField;
        TN_z       = (float *)p; p += bytesPerFloatField;
        TTangent_x = (float *)p; p += bytesPerFloatField;
        TTangent_y = (float *)p; p += bytesPerFloatField;
        TTangent_z = (float *)p; p += bytesPerFloatField;
        PX         = (float *)p; p += bytesPerFloatField;
        PY         = (float *)p; p += bytesPerFloatField;
        RZ         = (float *)p; p += bytesPerFloatField;
        UZ         = (float *)p; p += bytesPerFloatField;
        VZ         = (float *)p; p += bytesPerFloatField;
        EUZ        = (float *)p; p += bytesPerFloatField;
        EVZ        = (float *)p; p += bytesPerFloatField;
        Flags      = (uint32_t *)p; p += bytesPerU32Field;
        BGRA       = (uint32_t *)p; p += bytesPerU32Field;
    }

    ~VertexFrame() {
        free(_slab);
        _slab = nullptr;
    }

    // Non-copyable / non-movable for safety — TriMesh holds a pointer
    // to a heap-allocated instance.
    VertexFrame() = default;
    VertexFrame(const VertexFrame &) = delete;
    VertexFrame &operator=(const VertexFrame &) = delete;
    VertexFrame(VertexFrame &&) = delete;
    VertexFrame &operator=(VertexFrame &&) = delete;
};

struct Vertex;  // for the helper below

// Dual-write helper. Mirrors Transform_Objects' end-of-mesh dump loop
// at FDS/RENDER/Transform.cpp:1280+ so any alternative transform path
// (CITY/CHASE Reflected_Transform, future hand-written transforms) can
// keep T->frame in lockstep with T->Verts after writing AoS. Without
// this, the clipper's Phase 6.1 TPos-from-frame override (FRUSTRUM.CPP)
// reads stale main-camera values during reflection rendering.
//
// `F_` must already be ensureSized(nv). Inline so Transform.cpp keeps
// the inlined per-mesh loop it had, and Reflected_Transform pays the
// same per-mesh cost without an extra call.
void VertexFrame_DumpFromAoS(VertexFrame *F_, const struct Vertex *verts, uint32_t nv);
