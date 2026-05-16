#include "VertexScratch.h"

#include "Face.h"
#include "TriMesh.h"
#include "Vertex.h"

namespace fds {

PerTriMeshClone& VertexScratch::cloneOf(TriMesh* T) {
    auto& c = clones[T];
    if (c.initialized) return c;
    c.verts.assign(T->Verts, T->Verts + T->VIndex);
    c.faces.assign(T->Faces, T->Faces + T->FIndex);
    // Remap each cloned Face's A/B/C to point into our cloned Verts.
    // Original A/B/C are pointers into T->Verts; the offset from
    // T->Verts is the same in the clone (verts is a contiguous copy
    // sized VIndex), so address arithmetic gives the right slot.
    for (auto& f : c.faces) {
        if (f.A) f.A = &c.verts[f.A - T->Verts];
        if (f.B) f.B = &c.verts[f.B - T->Verts];
        if (f.C) f.C = &c.verts[f.C - T->Verts];
    }
    c.initialized = true;
    return c;
}

} // namespace fds
