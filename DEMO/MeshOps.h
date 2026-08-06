#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct TriMesh;
struct Scene;
struct Object;
struct Vector;
struct Texture;
struct PomHorizonMap;

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

// Estimate the dominant BLOCK PITCH (mortar-to-mortar period, TEXELS at `mip`,
// per axis) of an 8-bit height map by gradient autocorrelation — the exact
// map-relative metric DisplaceStoneSubdiv uses to drive its per-quad depth cap.
// Returns false when the field carries no clear periodic grid. Exposed for the
// --scene-displacetest rig so it can report what the PRODUCTION estimator
// measures (docs/ENVDYN_DISPLACEMENT_PLAN.md); no runtime consumer changes.
bool EstimateBlockPitch(const Texture *hm, int mip, float &pitchXtex, float &pitchYtex);

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

// --pom_cone_exact (S1 P1): EXACT PER-TEXEL cone bake at the mip's own
// resolution instead of MakeConeMap's max-pooled 128² grid, after Bán et al.,
// "Robust Cone Step Mapping", EGSR 2024, adapted to our POINT-SAMPLED height
// field (distance is measured to the nearest point of a texel's CELL, not to
// its centre). mode 1 = conservative (the cone contains no geometry), mode 2 =
// relaxed (the cone may penetrate; bounded by where the ray leaves the field
// again, so the first bracket is still the first crossing). Quantised by
// TRUNCATION over [0, kPomConeExactMax] — a cone rounded UP is exactly what
// makes a march skip geometry. Caller owns; nullptr on a bad source or mode.
Texture *MakeConeMapExact(Texture *height, int mode);
// Same, through a disk cache under Runtime/cache/ keyed on the height field's
// bytes + mode + encode ceiling + scan radius (the horizon-map cache pattern).
Texture *LoadOrBakeConeMapExact(Texture *height, int mode, const char *tag);

// Pack a 32-bit BGRA tangent-space normal map to 16-bit RG (X,Y; Z reconstructed
// in-shader), same layout (half the memory). BPP=16 marks the kernel decode.
// Caller owns.
Texture *MakeNormal16(Texture *src);

// S1c (--pom_horizon): bake the relief's own horizon elevation in
// kPomHorizonAzimuths tangent-space azimuths from an 8-bit height map — u8
// sin(horizon) per azimuth, 8 bytes/texel, SAME block-tile + mip layout as the
// source so the rasterizer's swizzled index addresses it (see PomHorizonMap in
// Material.h). heightScaleUV is the relief's UV amplitude (parallax_strength ×
// ParallaxScale — the same number the shell geometry is built with); the world
// scale cancels out of the horizon angle, so nothing else is needed.
// radiusTexels is the mip-0 scan radius (~1.5 block pitches). Threaded;
// seconds for a 1024². Caller owns.
PomHorizonMap *MakeHorizonMap(const Texture *height, float heightScaleUV,
                              int radiusTexels);
// Same, with a Runtime/cache/ disk cache keyed on the height bytes + every bake
// parameter (a stale hit is impossible without a format version bump). `tag` is
// only used for the log line.
PomHorizonMap *LoadOrBakeHorizonMap(const Texture *height, float heightScaleUV,
                                    int radiusTexels, const char *tag);

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
// Populated from the authored native LWO 'SMAN' angle by
// MeshOps_SeedAuthoredSmoothAngles (greets, sidecar-elim §1.5) and by the live
// editor smoothing edit — both BEFORE MakeFacesIndependentByAngle at scene init,
// so the angle is in place when normals are built. The registry is EMPTY unless
// an authored SMAN differs from the scene default, so the default render is
// byte-identical. Angle is clamped to [0,180].
//
void  MeshOps_SetSurfaceSmoothAngle(const char *surface, float angleDeg);
bool  MeshOps_GetSurfaceSmoothAngle(const char *surface, float &angleDegOut);
bool  MeshOps_AnySurfaceSmoothAngle();

// Seed the smooth-angle registry from AUTHORED native SMAN (sidecar-elim §1.5).
// For every material in `sc` that carries Surf_Smoothing AND whose
// MaxSmoothingAngle (radians) converts to a degree value differing from the
// scene's default `defaultDeg` by more than `epsDeg`, register that authored
// angle — reproducing what the retired `surface|smoothAngle|value` sidecar lines
// did, but from the LWO/FLD source. Surfaces AT the default are left to the
// legacy global-crease / momy special-case path (no per-surface override), so
// only the authored surfaces change. Call BEFORE MakeFacesIndependentByAngle.
// INERT when every material sits at the default (registry stays empty). Returns
// the number of surfaces seeded.
int   MeshOps_SeedAuthoredSmoothAngles(Scene *sc, float defaultDeg, float epsDeg);

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

// Symmetric ADAPTIVE stone subdivision + displacement (S1/S2 of the diagonal-
// grain fix, docs/ENVDYN_DISPLACEMENT_PLAN.md workstream B). Replaces the
// SubdivideMaterialFaces(linear)+DisplaceMaterialVertices pair for the greets
// stone when --greets_displace is on: pairs each stone quad's two coplanar
// triangles across their shared (longest-edge) diagonal and retriangulates the
// quad as a symmetric 2^L×2^L grid whose relief cells are 4-triangle CENTRE
// fans — the height peak lands on a vertex (dome), not on a shared diagonal
// (the proven roof-ridge / uniform diagonal grain of commit 4633aeb). Unpaired
// lone triangles get symmetric barycentric subdivision (no diagonal bias).
// L is chosen PER QUAD from the height map's local max-gradient under the
// quad's UV footprint at `mip` (flat mortar stays coarse, busy block edges go
// deep, 0..3), scaled by `adapt` (>1 = deeper); uniformLevel > 0 forces that
// level everywhere (the uniform baseline for cost comparison). Authored patch
// borders are pinned at zero displacement (as DisplaceMaterialVertices);
// LEVEL-BOUNDARY edges are crack-free by pinning the finer side's inserted
// edge verts onto the coarser side's straight displaced segment. Face N +
// NormProd re-derived; records to --displace_viz. Same hook contract as
// SubdivideMaterialFaces (after Preprocess + GreetsRetileFloor, before
// MakeFacesIndependentByAngle + the chunk split).
//
// CROSS-MATERIAL SEAM PINNING (--greets_displace_neighbor_pin, default on):
// authored borders are also detected by POSITION-COINCIDENCE with NON-DISPLACED
// geometry, not just by shared vertex index / single-target-face edges. A wall
// meeting a lintel/ceiling of a different material is usually authored as
// separate geometry (its own duplicate verts at the same coordinates, possibly
// another mesh) — those target boundary verts are never index-classified as
// borders, so without this they displace and the seam opens (the sliver-gap
// bug). `displacedSet`, when non-null, lists EVERY displaced material name
// (must include matName): coincidence with a face in that set is IGNORED so a
// displaced↔displaced junction (e.g. greets rooms↔floor) keeps its relief.
// When null, only matName is treated as displaced (single-material rigs).
void DisplaceStoneSubdiv(Scene *Sc, const char *matName, int uniformLevel,
                         float amp, int mip, float adapt, float cellsPerBlock,
                         const std::vector<std::string> *displacedSet = nullptr);

// PARENT-PLANE registry for the displaced stone's shadow clustering
// (--greets_displace_shadow_planes). DisplaceStoneSubdiv records, per target
// material, the set of AUTHORED (pre-displacement) face planes — deduped on the
// same quantization grid the greets shadow clustering uses — and stamps each
// emitted target face's ShadowMatID with its parent plane's 1-based ordinal
// (a TRANSIENT tag; the greets clustering consumes it and overwrites the field
// with the real cluster id). This lets the clustering key displaced facets by
// the PARENT plane instead of collapsing the whole material to one id: facets
// of one flat wall still share one id (no per-facet acne), but DIFFERENT walls
// get different ids, so an occluding wall shadows a receiver wall again (the
// single-id collapse made the PolyId identity test skip every rooms-vs-rooms
// occlusion = the light bleed). Returns null when the material has no registry
// (bake didn't run) or ordinal is 0/out of range.
struct StoneParentPlane { float nx, ny, nz, d; };   // unit normal + n·p distance
const StoneParentPlane *MeshOps_StoneParentPlane(const char *matName, uint16_t ordinal);

// Companion post-pass, called AFTER MakeFacesIndependentByAngle: re-SMOOTH the
// displaced stone surface's vertex normals. That pass facets the displaced
// relief (adjacent cell faces exceed its 30° crease threshold → split → flat
// per-face normals → the triangulation shows as hairline/facet seams, plus a
// split tangent basis → normal-map noise). This does a scene-wide position-
// bucket weld of the material's corners and averages the DISPLACED face normals
// (area-weighted) within `smoothAngleDeg`, so shading AND the Gram-Schmidt
// tangent basis are continuous — faceting and the normal-map seam both vanish.
// The angle gate (< 90°) keeps authored HARD creases hard (wall-to-wall 90°
// corners); material boundaries are hard for free (bucketed per base surface).
// Same machinery as the editor's MeshOps_ResmoothSurface, scoped to one scene +
// material. No-op when the material has no faces in the scene.
void DisplaceStoneSmoothNormals(Scene *Sc, const char *matName, float smoothAngleDeg);

// S1b POM SHELL builder (docs/S1_PIXEL_DISPLACEMENT_PLAN.md §S1b): turn
// matName's flat faces into the LID of a relief slab — push every vertex they
// use out along its normal by (uvAmp × that face's world-per-UV-tile)/2 and
// stamp Vertex::ShellH with the slab height it reached (1 = lid), then record
// uvAmp on the material (Material::PomShellUvAmp) so the rasterizer's
// --pom_shell march enters through this surface, marches DOWN through the slab
// and discards rays that leave the patch (= the silhouette). uvAmp is in UV
// units — pass the same effective parallax strength the march runs at
// (parallax_strength × Material::ParallaxScale). pinCrossMaterial leaves verts
// shared with non-target faces alone (no junction can be pulled apart, at the
// cost of relief there — and on un-subdivided quads it can pin a whole face).
// Run after MakeFacesIndependentByAngle (final vertex normals, per-face verts)
// and before any chunk split. Returns the amp stamped (0 = nothing built).
float PomShell_Build(Scene *Sc, const char *matName, float uvAmp,
                     bool pinCrossMaterial = false);

// S1d-2e CROSS-MATERIAL LID WELD (--pom_shell_weld=3, default OFF).
// PomShell_Build runs ONCE PER MATERIAL, so --pom_shell_weld=1's position-bucket
// weld only ever averages normals within the material being built: greets'
// 'rooms' and 'floor' (and their ::mirUV clones) still move their shared
// wall/floor corners along DIFFERENT directions and tear against each other.
// That residue is measured — 13 986 of the lid arm's 14 163 remaining void
// pixels survive --no-parallax, i.e. they are geometry, at cross-material
// junctions (docs/S1D_CLOSED_SHELL_PLAN.md §S1d-2d.5/§S1d-2d.10).
//
// Call PomShell_WeldPrepare ONCE with every material that is about to be
// shelled, BEFORE the first PomShell_Build moves anything: it snapshots the
// pristine mesh and buckets vertex normals by POSITION across all of them, so
// every copy of a shared corner extrudes along one direction. It also memoises
// the offset VECTOR actually applied at each position, so materials with
// different amplitudes still land their shared corners on exactly one point.
// PomShell_WeldReset releases it. Both are no-ops unless --pom_shell_weld >= 3;
// with the flag off the tables stay empty and PomShell_Build is byte-identical.
void PomShell_WeldPrepare(Scene *Sc, const char *const *matNames, int numMats);
void PomShell_WeldReset();

// R2 MITRE ROOT-CAUSE — GEOMETRIC SLIT CENSUS (--pom_shell_slit_census, OFF).
// Purely observational and entirely OUTSIDE PomShell_Build, so that function's
// -ffp-contract=fast vertex move is textually untouched and the flag is
// byte-null by construction. Snapshot every vertex position BEFORE the first
// build, then after the last one attribute, per POSITION, the delta the shell
// actually applied and score it against the two surfaces it can tear from:
//   • an UNSHELLED incident face (nothing on the other side moves) — the gap is
//     |d.N_unshelled|, the distance the corner lifted OFF that neighbour;
//   • another SHELLED copy of the same position — the gap is |d_a - d_b|, which
//     the cross-material memo is supposed to drive to exactly zero.
// Reports both as counts, extremes and an EDGE-LENGTH-WEIGHTED AREA, which is
// the geometric predictor the rendered void count should track. No behaviour
// change; the snapshot is freed by PomShell_SlitCensus.
void PomShell_SlitSnapshot(Scene *Sc);
void PomShell_SlitCensus(Scene *Sc, const char *const *matNames, int numMats);

// S1d-3 PRISM / CLOSED-SHELL SIDE GEOMETRY (--pom_prism, default 0 = OFF).
// The lid shell moves the shelled surface OUT by half the relief slab. Wherever
// the surface it moves away from does NOT move with it — an unshelled ceiling,
// a column, a free edge, a T-junction, or (with the weld off) a torn corner —
// a slit of exactly the offset width opens, and that slit is 100 % of the void
// the weld cannot reach (docs/S1D_CLOSED_SHELL_PLAN.md §S1d-2f.1: every extra
// void pixel is --pom_path_viz code 0, i.e. NO FRAGMENT was ever rasterised).
//
// Hirche 2004's answer is a PRISM per base triangle: the extruded surface keeps
// a real SIDE QUAD at every edge, so the shell stays watertight while the lids
// move. This builds exactly those side quads, as real geometry in a new TriMesh,
// and only where they are load-bearing:
//   an edge gets a side quad iff no partner face shares its AUTHORED endpoints
//   with the SAME offset delta at both of them.
// Under the (default) weld that reduces to the boundary of the shelled surface —
// the skirt — because interior edges already agree by construction. With the
// weld off it emits the full Hirche side set, which is the A/B that tests
// whether the prism replaces the weld or only completes it.
//
// The quad spans lid (Pos) to base (Pos - 2*delta), so it passes exactly through
// the AUTHORED edge and therefore seals against static neighbours whatever the
// weld's tangential slide did. ShellH runs 1 -> 0 down it, so the march enters
// at the true slab height of the fragment; N/Tangent are the PATCH's, so the
// march's tangent frame and the UV domain are the neighbouring lid's.
//
// Call PomShell_PrismSnapshot BEFORE the first PomShell_Build (it records the
// pristine positions) and PomShell_BuildPrism after the last one, BEFORE the
// ::mirUV handedness split and the mirror clone build so both pick the new
// faces up. Both are no-ops with --pom_prism 0.
void PomShell_PrismSnapshot(Scene *Sc);
void PomShell_BuildPrism(Scene *Sc, const char *const *matNames, int numMats);

// B4 residual height map: full-res height minus the bilinear-upsampled lowMip
// band (per mip, clamped, same 8-bit tiled+mip layout) — the POM input for a
// displaced material, so geometry + parallax don't double-count the relief.
// nullptr for degenerate (constant-at-lowMip) sources, matching the bake's
// skip. Caller owns; re-run MakeConeMap on it when cone POM is active.
Texture *MakeResidualHeight(Texture *height, int lowMip);

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
