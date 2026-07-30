#pragma once

#include <cstdint>

struct TriMesh;
struct Scene;
struct Object;
struct Vector;
struct Texture;

// Build a render-ready Texture from a LINEAR, row-major 32-bpp pixel buffer
// (0xAARRGGBB DWords, `width*height` of them).
//
// This encapsulates a two-step gotcha that has cost a debug session every
// time custom geometry got a hand-built texture (disco ball, blaster bolts,
// fountain lightning):
//
//   1. Convert_Image2Texture ONLY resamples to 256x256 and converts BPP. It
//      leaves the pixel data LINEAR (row-major), and it does NOT build mips
//      or block-tile.
//   2. The rasterizer (TheOtherBarry / Mekalele) ALWAYS samples textures
//      block-tile SWIZZLED via packed_tile_u/v. So linear data is read at the
//      wrong offsets — the texture renders scrambled / 4x-repeated (the bug
//      manifests as evenly tiled cells, not noise). The data MUST be run
//      through Generate_Mipmaps(.., DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, ..)
//      with the Txtr_Tiled flag — the "shachletz" (interleave) step.
//
// With correct tiling, UV mapping is the STANDARD U -> texture-column,
// V -> texture-row. (Any code that reads UVs as "swapped" — e.g. the fountain
// bolt's UZ->texture-Y comment — was silently compensating for the un-tiled
// bug and bakes its texture transposed; don't copy that as a convention.)
//
// `pixels` is copied internally — the caller keeps ownership. buildMips=true
// builds the full mip chain (needed for surfaces that minify in the distance);
// false still block-tiles but keeps a single level (fine for screen-space
// sprites the 2D clipper always draws at mip 0, e.g. blaster bolts). Returns a
// newly-allocated Texture* (caller owns).
Texture *Scene_MakeTiledTexture(int width, int height, const uint32_t *pixels,
                                bool buildMips);

// Pack a 32-bit tiled+mipmapped grayscale texture to an 8-bit single-channel
// copy with the IDENTICAL layout (same swizzled index works; ¼ the memory).
// Used for parallax height maps — the variable-texel-size pilot. Caller owns.
Texture *MakeHeight8(Texture *src);

// Tier-2 cone-step POM (--parallax_pom): bake a conservative cone-step map from
// an 8-bit height texture (MakeHeight8 output). The result is an 8-bit texture
// with the IDENTICAL tiled+mip layout, so the rasterizer's swizzled height
// address also indexes the cone byte. Each texel stores min(dist_uv/heightDiff)
// to any taller texel, quantized over [0,kPomConeMax] (Material.h). Baked
// per-mip from a max-pooled coarse grid (conservative → the march never skips
// geometry) with toroidal wrap (seamless for tiling walls). One-time offline
// cost (~0.1-1s/material, threaded); only called when --parallax_pom is on.
// Caller owns the returned Texture. Returns nullptr on a bad/empty source.
Texture *MakeConeMap(Texture *height);

// Pack a 32-bit BGRA tangent-space normal map to 16-bit RG (X,Y; Z reconstructed
// in-shader), same layout (half the memory). BPP=16 marks the kernel decode.
// Caller owns.
Texture *MakeNormal16(Texture *src);

// Invert the GREEN channel of a normal map IN PLACE, across the full mip chain
// (OGL ↔ DX tangent-space convention). Handles both kernel formats: 32-bit
// BGRA (G at bits 8-15) and MakeNormal16's 16-bit R|G<<8 (G = high byte).
// Involutive — flip twice = original.
void FlipNormalMapG(Texture *t);

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
//   • TEXTURE GOTCHA: any hand-built Texture must be block-tiled, not just
//     run through Convert_Image2Texture (which leaves data LINEAR while the
//     rasterizer samples block-tile swizzled — the texture renders as repeated
//     cells). Build it via Scene_MakeTiledTexture (below) and you're safe.
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

// Per-surface smoothing-angle override registry (engine-only; no LWO/FLD
// field). When a surface — addressed by BASE material name, "::mirUV"
// handedness clones collapsed — has an angle set here, MakeFacesIndependent
// rebuilds that surface's per-vertex normals by averaging ONLY its own
// incident faces whose normal is within `angleDeg` of each face:
//   180  = fully smooth  (every incident same-surface face averaged → a true
//          shared normal, continuous Phong shading — like the 'momy' path)
//   0    = fully faceted (each face keeps its own normal → hard edges)
// This OVERRIDES both the global crease threshold and the built-in 'momy'
// auto-smooth for that one surface, and (unlike the global gate) restricts
// averaging to the surface's own faces so it can't bleed into neighbours.
//
// Populated from the scene sidecar line `surface|smoothAngle|value` by
// MaterialImport_ApplySidecar, which runs BEFORE MakeFacesIndependentByAngle
// at scene init — so the angle is in place when normals are built. The
// registry is EMPTY unless a sidecar sets it, so the default render is
// byte-identical. Angle is clamped to [0,180].
//
void  MeshOps_SetSurfaceSmoothAngle(const char *surface, float angleDeg);
bool  MeshOps_GetSurfaceSmoothAngle(const char *surface, float &angleDegOut);
bool  MeshOps_AnySurfaceSmoothAngle();

// LIVE re-smooth: recompute `surface`'s per-vertex normals on the CURRENT
// rendered meshes at `angleDeg`, so an editor slider drag re-shades the next
// frame with NO scene reload. Does NOT change topology. Applies
// MakeFacesIndependent's per-surface averaging rule (same-surface faces only,
// area-weighted, gated by Dot(faceN,adjN) >= cos(angle), EPSILON fallback to
// the face normal), rebuilding adjacency from a scene-wide spatial hash of the
// surface's face corners keyed by exact position bits.
//
// The spatial rebuild (rather than init's per-mesh Vertex* adjacency) is
// required because greets copies/merges the init-smoothed source meshes into
// other meshes (per-cell CHUNKS and per-round face subsets) and those copies
// are what render — a vertex's incident faces span several of them, so
// adjacency must be gathered scene-wide on the live geometry.
//
// The live look tracks a reload closely but is NOT guaranteed bit-identical;
// the departures are all inherent to editing in place instead of re-running the
// init split + re-baking:
//   • Per-face-split meshes (each Vertex used by one face — the norm for
//     anything MakeFacesIndependent touched, and greets' Piramid chunks) get
//     the full per-face treatment and match a reload to within ~1 LSB (the
//     area-weighted sums differ only in summation order), except a few pixels
//     at coincident-but-distinct authored vertices (e.g. the momy lathe's UV
//     seams) that the position hash merges but init keeps apart.
//   • SHARED-vertex meshes (a vertex used by several faces — greets' big merged
//     per-round meshes, some detail models) have only one normal per vertex, so
//     this can SMOOTH them (raise the angle) but cannot FACET them: each shared
//     vertex is written the average of its corners' results, degrading
//     gracefully to a smooth normal rather than an order-dependent facet. A
//     reload, which re-splits, DOES facet them — so faceting such a surface
//     live shows less change than the reload will (documented gap; the affected
//     pixels stay put rather than corrupting).
//
// Register the angle with MeshOps_SetSurfaceSmoothAngle first (so it
// round-trips + persists on Save). Off any hot path — safe to call once per
// slider event (debounce in the UI).
void  MeshOps_ResmoothSurface(const char *surface, float angleDeg);

// Phong-tessellate (curved PN-style) every face whose material name == matName,
// `levels` times (each level = 1→4 split per target triangle, edge midpoints
// displaced toward the smooth surface so the silhouette rounds). Crack-free
// (shared edge midpoints; crease-guarded projection + fold relaxation +
// per-face NormProd recompute — see the .cpp), non-target faces untouched.
// Run after Preprocess (needs vertex normals) and before
// MakeFacesIndependentByAngle. `phong=false` keeps midpoints LINEAR (pure
// density split, base surface bit-unchanged) — for the stone displacement
// bake, where PN rounding would bow flat walls near corners.
void SubdivideMaterialFaces(Scene *Sc, const char *matName, int levels,
                            bool phong = true);

// Height-map displacement bake (docs/ENVDYN_DISPLACEMENT_PLAN.md B3): push
// interior verts of matName's faces along their smooth vertex normal by
// amp*(h-mipMean), h bilinear-sampled from the material's 8-bit HeightMap at `mip`
// at the per-FACE UVs (averaged over incident target faces). Patch-border
// verts are pinned (no T-junction/cross-material cracks); face N + NormProd
// re-derived. Run after SubdivideMaterialFaces (density) and before
// MakeFacesIndependentByAngle (re-derives vertex normals) + the chunk split.
void DisplaceMaterialVertices(Scene *Sc, const char *matName, float amp, int mip);

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
