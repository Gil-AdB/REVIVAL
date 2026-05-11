#pragma once

struct TriMesh;
struct Scene;

// Per-face vertex duplication: replace shared-vertex topology with one
// independent Vertex copy per face. Each new copy's N is the area-
// weighted average of incident face normals within
// `smoothingThresholdDegrees` of THIS face's normal — vertices on
// flat regions of a creased mesh get a smoothed normal, vertices at
// crease boundaries get the face normal alone. Without that
// per-smoothing-group averaging the deferred Blinn-Phong term reads
// a constant normal across every triangle and highlights look
// per-polygon instead of per-pixel.
//
// Storage is owned internally; old T->Verts is left orphaned. Also
// reallocates T->SL (static-lighting cache) to the new VIndex when
// the mesh is Tri_Stationary, otherwise StaticLighting overflows it.
//
// Run once at scene init, before Lighting() so the forward path's
// Gouraud shading also sees the new normals.
void MakeFacesIndependent(TriMesh *T, float smoothingThresholdDegrees = 30.0f);

// Walk every TriMesh in the scene; for each, check whether any pair
// of incident face normals at a shared vertex exceeds the threshold.
// If so, apply MakeFacesIndependent to that mesh; otherwise leave it
// alone. Threshold is in degrees — 30 is a reasonable default
// matching common DCC tools' "auto-smooth angle". This is the
// systematic version that auto-detects which meshes need crisp
// creases (City buildings, Greets walls, Crash laptop facade) without
// breaking smooth meshes (Fountain crystal, Greets curved letters).
void MakeFacesIndependentByAngle(Scene *Sc, float thresholdDegrees);
