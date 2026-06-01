#pragma once

// Planar mirrors for greets / any scene. Each mirror is built from
// a "wall material name" — the set of faces with that material on
// their Material defines the mirror surface; their world-space
// average normal + offset becomes the reflection plane.
//
// Phase 1 (this header): one-mirror-per-call API on top of a single
// `Mirror` value object. Multiple mirrors = multiple Build calls into
// a vector. Per-mirror state (cloned geometry, cloned omnis, wall mat
// clone, per-source-mesh vert ranges) is owned by the Mirror struct
// instead of file-scope globals so distinct mirrors don't trample
// each other's data.
//
// Phase 2 (TODO): octree-pruned face selection so each mirror's
// clone only contains source faces actually visible through its
// surface. Today every face is cloned, which scales linearly with
// scene face count × mirror count.
//
// Phase 3 (TODO): mirror-in-mirror recursion cap. Today two facing
// mirrors would produce infinite reflections in the math — the
// engine won't crash but the clones would diverge over frames as
// each mirror's geometry leaks into the other's clone.

#include <Base/Vector.h>

#include <cstdint>
#include <string>
#include <vector>

struct Face;
struct Material;
struct Object;
struct Omni;
struct Scene;
struct TriMesh;

namespace fds {

struct MirrorPlane {
    Vector  N;            // world-space unit normal (face-side outward)
    float   d;            // plane offset: N·P + d = 0 for P on the plane
    int     faceCount;    // number of wall faces averaged in
    bool    valid;        // false if no wall faces found
};

// Per-source-mesh vert range inside a Mirror's clone. UpdateMirror
// re-mirrors the source's current world-space verts into this range
// each frame so dynamic / parented meshes track correctly.
struct ClonedMeshRange {
    TriMesh *sourceMesh;
    uint32_t vStart;
    uint32_t vCount;
};

// Source/clone omni pair owned by a Mirror. Per-frame IPos / IDir
// updates re-reflect the source's current pose; IRange gets clamped
// to plane distance for soft compartmentalization.
struct ClonedOmniRef {
    Omni *sourceOmni;
    Omni *mirrorOmni;
    float origSourceRange;
    float origMirrorRange;
};

// One planar mirror. Built once via BuildMirror; updated each frame
// via UpdateMirror. Holding by value is safe — Build only allocates
// engine-side objects (TriMesh, Verts, Omnis, Material) which the
// Mirror struct points at but doesn't own deletion of.
struct Mirror {
    MirrorPlane plane;
    TriMesh    *cloneMesh = nullptr;
    Material   *wallMatClone = nullptr;
    std::vector<ClonedMeshRange> meshRanges;
    std::vector<ClonedOmniRef>   omniClones;
    // Wall face pointers — the actual mirror SURFACE faces in the live
    // scene meshes (NOT in cloneMesh). Used by StampMirrorMasks to
    // rasterize each wall triangle's screen footprint into the gb.mirrorId
    // plane every frame, so the clone-rasterizer's per-pixel check can
    // gate writes to "inside this mirror's wall footprint only".
    std::vector<Face*> wallFaces;
    // Unique 1..255 mirror id. Assigned at BuildMirror time, written to
    // gb.mirrorId by the per-frame mask pre-pass and matched against
    // Face::mirrorMaskTag in Mekalele's inner loop.
    uint8_t     id = 0;
    int         wallFacesRetargeted = 0;
    int         clonedFaces  = 0;
    int         clonedVerts  = 0;
    std::string wallMaterialName;
};

// Pass 1: find the mirror plane by averaging the world-space normal /
// offset of every face whose Material name matches `wallMaterialName`.
// Outlier faces (>30° from majority normal) dropped. Returns valid=false
// if no matching faces found.
MirrorPlane FindMirrorPlaneByMatName(Scene *sc, const char *wallMaterialName);

// Same plane finder, but selects wall faces by Texture::FileName
// substring match. Useful when many distinct Materials share one
// texture (greets's text-display screens all use TEXTURES/P_TEXT.JPG
// regardless of material name).
MirrorPlane FindMirrorPlaneByTextureName(Scene *sc, const char *textureFileName);

// Pass 2: build the mirror — clone every other mesh's geometry with
// world positions reflected across `plane.plane`, swap winding, clone
// every omni with reflected position, retarget the wall faces to a
// transparent material clone. Returns a Mirror handle with state for
// the per-frame update. Mirror is invalid (cloneMesh==nullptr) if
// the plane isn't valid.
Mirror BuildMirror(Scene *sc, const char *wallMaterialName);

// Parallel entry — picks wall faces by Texture::FileName substring
// match. Use this when you want to mirror a *texture* (e.g. all
// surfaces sharing the dynamic greets text texture) without needing
// to enumerate all the material-name variants.
Mirror BuildMirrorByTextureName(Scene *sc, const char *textureFileName);

// Per-frame: re-mirror dynamic source meshes' world verts + cloned
// omnis' positions across this mirror's plane. Clamps omni IRange to
// plane distance for soft compartmentalization. Call AFTER
// Animate_Objects, BEFORE Transform_Objects.
void UpdateMirror(Scene *sc, Mirror &m);

// Convenience: update every mirror in a list. Empty list = no-op.
void UpdateAllMirrors(Scene *sc, std::vector<Mirror> &mirrors);

// Debug viz — overlays gb.mirrorId onto the framebuffer so we can see
// exactly which pixels each mirror's wall pre-pass stamped (and where
// the clone-rasterizer's per-pixel check is consequently keeping vs
// rejecting writes). Gated by --greets-mirror-debug-mask.
void DebugOverlayMirrorMask(Scene *sc);

// Per-frame mask pre-pass: walks every mirror's wall faces, projects
// each triangle to screen space using already-transformed PX/PY, and
// scanline-fills gb.mirrorId with the mirror's id at each covered
// pixel. Cleared to 0 at entry so previous-frame coverage doesn't
// leak. Call AFTER Transform_Objects (which populates PX/PY on the
// wall verts) and BEFORE Render() (so the stamp is in place when
// clone faces rasterize).
void StampMirrorMasks(Scene *sc, const std::vector<Mirror> &mirrors);

}  // namespace fds
