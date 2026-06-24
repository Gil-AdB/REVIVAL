#pragma once

struct TriMesh;
struct Scene;
struct Object;
struct Vector;

// Register a hand-built mesh into a scene as a DYNAMIC, per-frame-updated mesh,
// with all the plumbing the engine needs — each item below cost a debug session
// when missed while adding custom geometry (disco ball, blaster bolts, ...):
//
//   • Compute_FaceVertexIndices: the SoA transform indexes verts via
//     F->A_idx/B_idx/C_idx; unstamped (0) → every face collapses to vertex 0
//     and is culled.
//   • 2 Pos spline keys >0.1 apart: marks the mesh dynamic so Transform
//     re-reads its vertices every frame instead of caching a static silhouette.
//   • A large bounding sphere: so the mesh is never frustum-culled as you move
//     its verts around (override via bsphereRadius for a localized mesh).
//   • Flags: HTrack_Visible | Tri_Possessed (Animate_Objects won't touch it —
//     you stamp verts directly) | Tri_Noshading | Tri_NoShadowCast.
//   • Links into Sc->ObjectHead + Sc->TriMeshHead.
//
// Preconditions the CALLER must satisfy before calling:
//   • mesh->Verts / mesh->Faces allocated; each face's A/B/C, Txtr, Filler,
//     Flags, N, and per-face U1..V3 (call Face::uvFromVertices) wired.
//   • mesh->VIndex / mesh->FIndex set to the FULL pool size (reserved here so
//     setupFaceLists counts them in the poly budget — shrink-per-frame drops
//     faces; degenerate unused slots instead).
//   • RUNTIME GOTCHA (not enforceable here): every face Material needs a
//     non-null Txtr->Txtr (a real texture). The deferred per-tile pass skips
//     untextured faces. And additive faces want WriteZ=true in deferred so the
//     mat32 "skip-lighting" sentinel survives.
//
// Returns the created Object (already linked); call BEFORE the scene's
// setupFaceLists / mirror build so the faces are budgeted and (if greets)
// cloned. Idempotent registration is the caller's concern.
Object *Scene_AddDynamicMesh(Scene *sc, TriMesh *mesh, const char *name,
                             const Vector &bsphereCtr, float bsphereRadius);

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

struct Texture;

// Bake an object-space normal map from a 32-bpp diffuse texture's
// luminance gradient. Output is a freshly-allocated Texture* with the
// SAME dimensions and SAME block-tile layout as the input, so the
// existing rasterizer-computed swizzledUV indexes both consistently.
//
// Sobel-style gradient of luminance → perturbation in the surface
// tangent plane → encoded as a world-space normal assuming the
// surface's base normal points along +Y. Good for ground-like surfaces
// (floors, water). For walls (whose base normal points along ±X or
// ±Z) the resulting bumps point in the wrong direction; production
// use would need a per-orientation bake or tangent-space maps.
//
// strength controls the bump amplitude (0 = flat, large = very
// bumpy). 4.0f is a sensible starting point.
//
// Caller owns the returned Texture and its buffers. Returns nullptr
// if the input isn't 32-bpp or has no Mipmap[0].
Texture *BakeNormalMapFromDiffuse(Texture *diffuse, float strength = 4.0f);
