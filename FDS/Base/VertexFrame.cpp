#include "VertexFrame.h"
#include "Vertex.h"

// SoA dual-write. Dumps ONLY the fields a live consumer actually reads (the
// full 18-field transpose cost ~5% of the greets frame to populate dead
// fields). Phase 4 migration rule: when you migrate a consumer to read
// frame->FIELD, add FIELD here (and to the --soa-verify block in Transform.cpp)
// in the same change — never read a field this doesn't populate (stale data).
//
// Live readers:
//   TPos_z         — Shadows.cpp per-light back-face cull on the shadow clone.
//   TPos_x/y/z     — IsFrontFacingInViewSpace (legacy xpar peel front/back),
//                    and the unified-TBR clump classifier (FILLERS.CPP
//                    computeFront). These were migrated to frame-> reads but
//                    TPos_x/y were NEVER added here, so the face normal came
//                    out (0,0,0) and every transparent face read as back-facing
//                    (degenerate). Fixed by adding x/y.
//   PY             — InsertTransparentToTBR's screen-Y strip span. Was reading
//                    frame->PY (0) → every transparent face binned to strip 0 →
//                    clipped to rows [0,8) and dropped (the fountain glass orbs
//                    rendered nothing). Fixed by adding PY.
void VertexFrame_DumpFromAoS(VertexFrame *F_, const Vertex *verts, uint32_t nv) {
    if (!F_ || F_->capacity < int(nv)) return;
    for (uint32_t i = 0; i < nv; ++i) {
        const Vertex& v = verts[i];
        F_->TPos_x[i] = v.TPos_AOS.x;
        F_->TPos_y[i] = v.TPos_AOS.y;
        F_->TPos_z[i] = v.TPos_AOS.z;
        F_->PY[i]     = v.PY;
    }
}
