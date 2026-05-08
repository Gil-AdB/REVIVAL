#pragma once

struct TriMesh;
struct Scene;

// Per-face vertex duplication: replace shared-vertex topology with one
// independent Vertex copy per face, then write the face normal into
// each new copy's N. Faces meeting at sharp creases (90° corners on
// rectangular blocks, panel intersections) thereby get a constant
// shading normal across each triangle, instead of the area-weighted
// average that Compute_Vertex_Normals produces. Curved surfaces want
// the smoothing — see MakeFacesIndependentByAngle to apply this only
// to meshes that actually have creases.
//
// Storage is owned internally; old T->Verts is left orphaned. Also
// reallocates T->SL (static-lighting cache) to the new VIndex when
// the mesh is Tri_Stationary, otherwise StaticLighting overflows it.
//
// Run once at scene init, before Lighting() so the forward path's
// Gouraud shading also sees the faceted normals.
void MakeFacesIndependent(TriMesh *T);

// Walk every TriMesh in the scene; for each, check whether any pair
// of incident face normals at a shared vertex exceeds the threshold.
// If so, apply MakeFacesIndependent to that mesh; otherwise leave it
// alone. Threshold is in degrees — 30 is a reasonable default
// matching common DCC tools' "auto-smooth angle". This is the
// systematic version that auto-detects which meshes need crisp
// creases (City buildings, Greets walls, Crash laptop facade) without
// breaking smooth meshes (Fountain crystal, Greets curved letters).
void MakeFacesIndependentByAngle(Scene *Sc, float thresholdDegrees);
