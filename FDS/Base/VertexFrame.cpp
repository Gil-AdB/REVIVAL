#include "VertexFrame.h"
#include "Vertex.h"

void VertexFrame_DumpFromAoS(VertexFrame *F_, const Vertex *verts, uint32_t nv) {
    if (!F_ || F_->capacity < int(nv)) return;
    for (uint32_t i = 0; i < nv; ++i) {
        const Vertex *v = &verts[i];
        F_->TPos_x[i]     = v->TPos_AOS.x;
        F_->TPos_y[i]     = v->TPos_AOS.y;
        F_->TPos_z[i]     = v->TPos_AOS.z;
        F_->TN_x[i]       = v->TN.x;
        F_->TN_y[i]       = v->TN.y;
        F_->TN_z[i]       = v->TN.z;
        F_->TTangent_x[i] = v->TTangent.x;
        F_->TTangent_y[i] = v->TTangent.y;
        F_->TTangent_z[i] = v->TTangent.z;
        F_->PX[i]         = v->PX;
        F_->PY[i]         = v->PY;
        F_->RZ[i]         = v->RZ;
        F_->UZ[i]         = v->UZ;
        F_->VZ[i]         = v->VZ;
        F_->EUZ[i]        = v->EUZ;
        F_->EVZ[i]        = v->EVZ;
        F_->Flags[i]      = v->Flags;
        F_->BGRA[i]       = v->BGRA;
    }
}
