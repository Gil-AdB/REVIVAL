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

// Build one consolidated "mirror" TriMesh that contains every face of
// every existing mesh (except faces with the "teleporter" material —
// the wall itself), with world-space vertex positions reflected across
// `plane`, face winding swapped (B↔C) to keep them outward-facing for
// the real camera, and normals reflected. Wraps the new mesh in an
// Object named "mirror_clone" and inserts at scene->ObjectHead /
// TriMeshHead. Returns the count of cloned faces (0 → no-op).
//
// Static-only: dynamic per-frame mesh updates (e.g., a moving robot)
// snapshot at init time. Iterate later if needed.
//
// Also stamps F->Filler = no-op on the original teleporter faces so
// the wall becomes a "hole" — the mirror geometry behind it shows
// through the Z-buffer, surrounded by the unchanged Piramid walls.
int BuildMirrorMeshAndHideWall(Scene *sc, const MirrorPlane &plane);

// Per-frame update of the mirror clone: re-mirrors every source mesh's
// current world vertex positions across `plane` and overwrites the
// matching clone verts. Also re-mirrors every cloned omni's IPos / IDir
// so dynamic lights track their source per frame. Call AFTER
// Animate_Objects (which advances source splines) and BEFORE
// Transform_Objects (which reads positions). No-op if Build hasn't
// been run.
void UpdateMirrorPerFrame(Scene *sc, const MirrorPlane &plane);

// Draws a bright outline around every teleporter wall edge in screen
// space — gives the mirror a "defined transition" so it doesn't look
// like a featureless hole. Reads vertex .PX/.PY computed by the
// preceding Transform_Objects. Call AFTER Render so the lines land
// on top of the framebuffer.
void DrawMirrorFrame(Scene *sc);

// Dumps mirror mesh state + first few vert/face pointers to stderr,
// tagged so we can locate it in a call sequence (e.g. "post-init",
// "post-tick0"). Used to triage why the clone renders wrong.
void DumpMirrorState(Scene *sc, const char *tag);

}  // namespace fds
