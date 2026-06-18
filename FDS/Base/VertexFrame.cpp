#include "VertexFrame.h"
#include "Vertex.h"

// SoA dual-write. Dumps ONLY the fields a live consumer actually reads —
// currently just TPos_z (Shadows.cpp's per-light back-face cull on the shadow
// clone is the sole reader of any frame-> field; everything else still reads
// AoS). The full 18-field transpose cost ~5% of the greets frame to populate
// 17 dead fields. Phase 4 migration rule: when you migrate a consumer to read
// frame->FIELD, add FIELD here (and to the --soa-verify block in Transform.cpp)
// in the same change — never read a field this doesn't populate (stale data).
void VertexFrame_DumpFromAoS(VertexFrame *F_, const Vertex *verts, uint32_t nv) {
    if (!F_ || F_->capacity < int(nv)) return;
    for (uint32_t i = 0; i < nv; ++i)
        F_->TPos_z[i] = verts[i].TPos_AOS.z;
}
