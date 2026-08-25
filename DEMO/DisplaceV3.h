#ifndef REVIVAL_DISPLACEV3_H
#define REVIVAL_DISPLACEV3_H

// DISPLACE v3 — clean-room junction-correct stone displacement
// (docs/DISPLACE_V3_DESIGN.md). Milestones M1 (panel/junction graph +
// instrument dump) and M2 (interiors-only displacement along PANEL PLANE
// normals, junction rings pinned). Deliberately shares NO code with the
// legacy DisplaceStoneSubdiv machinery: panels, junction rings, and height
// sampling are self-contained here.
//
// Call AFTER SubdivideMaterialFaces (density) and BEFORE
// MakeFacesIndependentByAngle, in place of the legacy bake.

struct Scene;

void DisplaceV3_Run(Scene *Sc);

#endif // REVIVAL_DISPLACEV3_H
