#pragma once

// Greets back-wall mirror — forward-path planar reflection on faces whose
// material is "teleporter" (see CLAUDE.md / SESSION notes). Implemented in
// stages:
//   Phase 1 (this file): find teleporter faces, compute the mirror plane
//                        normal + offset, log it.
//   Phase 2 (TODO):     per-frame reflected camera, render scene to a
//                        temp surface from the reflected POV.
//   Phase 3 (TODO):     screen-projected UVs on teleporter faces so each
//                        wall pixel samples the matching reflection pixel.

#include <Base/Vector.h>

struct Scene;

namespace fds {

struct MirrorPlane {
    Vector  N;            // world-space unit normal (face-side outward)
    float   d;            // plane offset: N·P + d = 0 for P on the plane
    int     faceCount;    // number of teleporter faces averaged in
    bool    valid;        // false if no teleporter faces found
};

// Walk every face of every TriMesh in the scene; for those whose material
// name is "teleporter", accumulate world-space normal + plane offset and
// return the averaged plane. Faces whose normal disagrees with the
// majority by > ~30° are ignored so a one-off stray face doesn't pull
// the mirror plane askew.
MirrorPlane FindTeleporterMirrorPlane(Scene *sc);

}  // namespace fds
