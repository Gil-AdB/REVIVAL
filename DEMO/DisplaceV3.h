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

// After DisplaceV3_Run: the mip level whose texel size matches the bake's
// median cell size for material mat (0='rooms', 1='floor') — the low band the
// GEOMETRY now carries, i.e. the argument for MakeResidualHeight so POM
// marches only the residual. -1 if the bake didn't run for that material.
int DisplaceV3_ResidualMip(int mat);

#endif // REVIVAL_DISPLACEV3_H
