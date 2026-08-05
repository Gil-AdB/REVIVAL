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
struct Camera;
struct TriMesh;
struct Texture;
struct Vertex;

namespace fds {

struct MirrorPlane {
    Vector  N;            // world-space unit normal (face-side outward)
    float   d;            // plane offset: N·P + d = 0 for P on the plane
    Vector  centroid{};   // world-space centroid of the wall faces
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
    // Source mesh (or an ancestor) actually animates — decided once at
    // build by the same spline-extent heuristic Transform.cpp's
    // isDynamicForBake uses. UpdateMirror re-mirrors only dynamic
    // ranges after the first full pass; static geometry recomputes to
    // identical values, so skipping it is pure savings (~95% of the
    // per-frame re-mirror in greets, where only the robot moves).
    bool     dynamic;
};

// One clone sub-range with its OWN tight bounding sphere, in clone
// space (the clone mesh's transform is identity, so clone space ==
// world space). A mirror clone is built as ONE TriMesh holding the
// whole mirrored room, so its single bsphere is room-sized and the
// mesh-level frustum cull can never reject it — the same disease the
// greets Piramid chunk split cured for the source wall. These are the
// spheres a per-source-mesh SPLIT of the clone would give each piece;
// --mirror_cull_census measures, without changing any geometry, how
// many of the clone's verts such a split would let the existing cull
// reject. Populated at BuildMirror, refreshed for dynamic ranges by
// UpdateMirror, keyed on the clone TriMesh (stable across the
// mirrors-vector reallocations that would dangle a MeshRange pointer).
struct MirrorCloneSubSphere {
    uint32_t vStart = 0, vCount = 0;
    Vector   ctr{};
    float    radSq = 0.0f;
    bool     dynamic = false;
};

// The sub-spheres for a mirror clone mesh, or nullptr for any mesh
// that isn't one. Cheap hash lookup; only --mirror_cull_census calls it.
const std::vector<MirrorCloneSubSphere> *MirrorCloneSubSpheres(const TriMesh *T);

// One mirror WALL face (the mirror's screen window) plus the live mesh
// that owns it, so a caller can transform its verts to world/view. The
// clone can only paint pixels whose gb.mirrorId equals its own tag —
// i.e. INSIDE this window — so clone geometry projecting outside it
// contributes nothing at all, even though it sits inside the camera
// frustum. --mirror_cull_census measures that (much tighter) ceiling
// alongside the plain-frustum one. Registered per frame for ACTIVE
// mirrors by UpdateAllMirrors, keyed on the clone TriMesh.
struct MirrorCloneWall { const Face *face; const TriMesh *mesh; };
const std::vector<MirrorCloneWall> *MirrorCloneWalls(const TriMesh *T);

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
    // Per clone face (parallel to cloneMesh->Faces): the source face +
    // its owning mesh. UpdateMirror re-derives each clone face's world
    // normal (srcMesh->RotMat × srcFace->N, reflected) and NormProd
    // every frame — for ANIMATED sources (the robot) the init-time
    // normal goes stale as the mesh rotates, which mis-culls clone
    // faces (the "flipped face culling" look in the robot reflection).
    struct CloneFaceSrc { const Face *face; TriMesh *mesh; bool dynamic; };
    std::vector<CloneFaceSrc> cloneFaceSrc;
    // First UpdateMirror call does a FULL re-mirror (build-time vert
    // capture may predate the first Animate_Objects); afterwards only
    // dynamic ranges/faces update per frame.
    bool primed = false;
    // Wall face pointers — the actual mirror SURFACE faces in the live
    // scene meshes (NOT in cloneMesh). Used by StampMirrorMasks to
    // rasterize each wall triangle's screen footprint into the gb.mirrorId
    // plane every frame, so the clone-rasterizer's per-pixel check can
    // gate writes to "inside this mirror's wall footprint only".
    std::vector<Face*> wallFaces;
    // Owning TriMesh per wallFaces entry (parallel vector). The mask
    // pre-pass transforms wall verts itself (world → view → pre-divide)
    // instead of reading TPos_AOS, which is STALE whenever the owning
    // mesh was frustum-culled that frame — with greets's chunked room
    // mesh that happened constantly, stamping last-frame footprints
    // over arbitrary geometry (the "mirror visible through walls" leak).
    std::vector<TriMesh*> wallFaceMeshes;
    // Unique 1..255 mirror id. Assigned at BuildMirror time, written to
    // gb.mirrorId by the per-frame mask pre-pass and matched against
    // Face::mirrorMaskTag in Mekalele's inner loop.
    uint8_t     id = 0;
    // Compound (depth-1 recursive) mirror: id of the PARENT mirror
    // whose reflected world this compound lives in. 0 = base mirror.
    // For compound A→B (= "looking at B through A"), parentMirrorId =
    // A.id. The compound's `plane` carries the composed reflection's
    // primary plane (= B.plane, the inner reflection), and
    // `parentPlane` carries A.plane so UpdateMirror can re-apply the
    // composed reflection_A∘reflection_B transform for dynamic verts.
    // StampMirrorMasks uses parentPlane for the viewer-side gate so a
    // compound mirror is suppressed when its parent's wall is not in
    // front of the camera.
    uint8_t     parentMirrorId = 0;
    MirrorPlane parentPlane = {};
    // World AABB of the wall faces (the mirror "window"), computed
    // lazily on first use by the bounce-spot pool. Thin slab in the
    // plane-normal axis.
    Vector      windowMin{}, windowMax{};
    bool        windowValid = false;
    int         wallFacesRetargeted = 0;
    int         clonedFaces  = 0;
    int         clonedVerts  = 0;
    std::string wallMaterialName;
    // Per-frame visibility, decided in UpdateAllMirrors: when the
    // camera cannot see the panel (back side, fully behind the near
    // plane, or projected footprint off screen), the mirror is
    // deactivated — UpdateMirror's vert/face re-mirror is skipped, the
    // clone mesh is hidden from Transform/raster, clone flares are
    // zero-sized, and StampMirrorMasks doesn't stamp it. The test runs
    // on the previous frame's camera pose (UpdateAllMirrors precedes
    // Transform_Objects); a viewport margin absorbs the 1-frame lag.
    bool        active = true;
    // Set by the shatter effect: a broken mirror is permanently closed —
    // UpdateAllMirrors forces it inactive, hides its clone mesh + flares,
    // and skips the per-frame re-mirror so the falling shards are all
    // that's left where the panel was.
    bool        broken = false;
};

// One second-order RTT slot: mirror A's reflection shows mirror B's
// panel (one CONNECTED panel component — a coplanar cluster can span
// several separate boxes); the faces listed here are A's clones of
// that panel, retargeted at init to `mat` whose texture is re-rendered
// per frame from the doubly-reflected camera.
struct MirrorRttSlot {
    // Reflection order. 2 = mirror-in-mirror (camera doubly reflected,
    // faces are A's clones of B's panel). 1 = the panel ITSELF is the
    // mirror: camera singly reflected across bN/bD, faces are the REAL
    // panel faces. No winding/axis flip in either order — the RTT is a
    // normal render of the real scene from the virtual position with a
    // proper basis; the mirror inversion lives in the ray geometry
    // (texel(W) = scene along camPos→W) and the UV stamp shares the
    // projection, so the mapping is consistent by construction.
    uint8_t order = 2;
    uint8_t aId = 0;          // outer mirror (whose reflection shows the panel)
    uint8_t bId = 0;          // inner mirror (the panel being re-rendered)
    Vector  bN{};             // B's plane normal / offset
    float   bD = 0.0f;
    Vector  axisU{}, axisV{}; // orthonormal basis in B's plane
    float   u0 = 0, u1 = 0, v0 = 0, v1 = 0;  // panel window in (u,v)
    // Texture dimensions, aspect-matched to the window at a constant
    // 64K-texel budget (a 4:1 panel gets 512x128, not a stretched
    // square). Render surface + projection adapt per slot.
    int     texW = 256, texH = 256;
    // Build-time MAX dims = the size mat->Txtr->Data is allocated at.
    // texW/texH may be shrunk per frame to the panel's on-screen
    // footprint (--mirror-rtt-adaptive) so a distant/oblique panel bakes
    // at the resolution it actually occupies, never above texWMax.
    int     texWMax = 256, texHMax = 256;
    Material *mat = nullptr;  // RTT material (mat->Txtr->Data updated per frame)
    std::vector<Face*> faces; // A-clone faces displaying the RTT
    // Clone verts of those faces with their (static) panel-plane
    // coordinates. UVs are re-stamped each time the slot re-renders:
    // the engine's mesh frustum cull only supports symmetric frusta,
    // so the RTT view is centered on the camera's plane-foot and the
    // panel window lands in a camera-dependent sub-rect of the texture.
    struct SlotVert { Vertex *v; float pu, pv; };
    std::vector<SlotVert> verts;
    // First-order half-silvered composite: the panel's ORIGINAL
    // (dynamic text) texture + the affine map from panel-plane (pu,pv)
    // to its authored UVs. After each re-render the text is composited
    // over the reflection on the CPU (text + reflection/2 — the same
    // formula the deferred transparent kernel uses for glass), so the
    // opaque mirror panel reads as a half-silvered display. Null =
    // no composite (second-order slots, untextured sources).
    Texture *textTex = nullptr;
    float    tA[6] = {0, 0, 0, 0, 0, 0};
    // Frames since this slot's texture was last re-rendered. Drives
    // the job scheduler's staleness weighting: with many slots and a
    // 2-jobs/frame cap, pure footprint-area ranking permanently
    // starves small panels (a never-rendered column quad stays black
    // forever while bigger screens are on screen). Initialized huge so
    // a slot's FIRST fill wins over routine refreshes.
    int staleFrames = 1 << 20;
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

// Clustered entry — groups the texture's faces into coplanar clusters
// (normals within ~18° AND plane offsets within 0.5 world units) and
// builds one independent Mirror per cluster, appended to `out`.
// BuildMirrorByTextureName fits a single plane to ALL matching faces,
// which works only when every surface sharing the texture is coplanar;
// greets's text screens face four directions at different depths, so
// the single-plane fit kept 12/64 faces and silently dropped the rest
// (screens that never became mirrors). Returns mirrors appended.
// rttSlots (optional): clusters whose area falls below the clone-mirror
// threshold (--greets-mirror-min-area) but above the sliver cutoff —
// greets's column screens — become FIRST-order RTT mirrors instead of
// being skipped: the real panel faces are retargeted to a per-slot
// texture re-rendered each frame from the singly-reflected camera.
// Requires --mirror-rtt; pass nullptr to skip (columns stay ordinary
// screens).
// allowedMatNames (optional): if non-null, ONLY coplanar clusters whose
// source surface name is in this list become mirrors — an explicit, per-
// surface mirror designation that replaces the area heuristic for SELECTION
// (area/aspect is still used to pick the front face of a marked screen's box
// vs its side caps). The mark is authored on the LWO surface (e.g. the big
// greets display is surface "screen 3"); the small amudim screens ("screen 4")
// and column panels ("screen2") are simply left off the list. Pass nullptr to
// keep the legacy area-gated behaviour (any P_TEXT cluster >= min-area).
// byMatName: match faces by Material NAME instead of texture filename
// (textureFileName is then read as the material name). Lets a hand-built
// scene route its named mirror panels through the RTT-slot builder — the
// path recursive mirrors need (--mirror-recurse-depth demotes clones to
// first-order RTT panels here). Default false = legacy texture-name match.
int BuildMirrorsByTextureName(Scene *sc, const char *textureFileName,
                              std::vector<Mirror> &out,
                              std::vector<MirrorRttSlot> *rttSlots = nullptr,
                              const std::vector<std::string> *allowedMatNames = nullptr,
                              bool byMatName = false);

// Depth-1 recursive: for each ordered pair (A, B) of already-built
// base mirrors, append a compound mirror representing "looking at B
// through A". The compound's wall surface is A's existing clone of
// B's wall (faces in A.cloneMesh whose Txtr matches B.wallMatClone —
// they get retagged with the new compound id); its clone geometry is
// the scene reflected across reflect_A ∘ reflect_B, and its omnis are
// tagged so the deferred light filter routes them only to the
// compound's pixels. Returns count of compound mirrors appended.
int BuildCompoundMirrors(Scene *sc, std::vector<Mirror> &mirrors);

// Per-frame: re-mirror dynamic source meshes' world verts + cloned
// omnis' positions across this mirror's plane. Clamps omni IRange to
// plane distance for soft compartmentalization. Call AFTER
// Animate_Objects, BEFORE Transform_Objects.
void UpdateMirror(Scene *sc, Mirror &m);

// Convenience: update every mirror in a list. Empty list = no-op.
void UpdateAllMirrors(Scene *sc, std::vector<Mirror> &mirrors);

// Tag every original (non-clone) face with a bitmask of which mirrors
// it sits behind (Face::behindMirrorMask). Mekalele's opaque commit
// then rejects those faces' pixels inside the matching mirror's screen
// footprint, so real-world geometry behind a (transparent) mirror can't
// leak through and beat the reflected clones on Z. Call ONCE after all
// mirrors are built (BuildMirror / BuildCompoundMirrors) and before the
// first render. Static geometry only — re-call if a mirror plane or
// mesh transform changes.
void TagFacesBehindMirrors(Scene *sc, const std::vector<Mirror> &mirrors);

// Init-time: enumerate (A, B, connected-panel) combos, create one RTT
// material + texture per slot, retarget A's clone-of-B's-panel faces
// to it and stamp their vertex UVs with normalized panel-window
// coordinates (static — the mapping is in B's plane basis). Rebuilds
// the per-scene mat table once at the end. Call AFTER all mirrors are
// built. No-op (returns 0) unless --mirror-rtt.
int PrepareSecondOrderMirrorRtt(Scene *sc, std::vector<Mirror> &mirrors,
                                std::vector<MirrorRttSlot> &out);

// Per-frame: pick the most visible slots (projected footprint area,
// cap kMirrorRttPerFrame), render the REAL scene (clone meshes hidden,
// clone flares muted) from C_B = reflect_B(reflect_A(cam)) through B's
// panel window — off-axis projection, near plane AT the mirror plane —
// into the slot texture via a low-res offscreen surface swap (CITY
// cube-bake pattern, forward path). Call AFTER Animate_Objects and
// BEFORE the main Transform_Objects: the pass overwrites per-vertex
// transforms (main Transform redoes them) and restores every camera /
// surface global it touches. Uses the previous frame's camera pose
// (one frame of lag in the second bounce).
// Jobs rendered by the last RenderSecondOrderMirrors call. In
// deferred mode the tick's forward Lighting() exists solely to feed
// the RTT pass's vertex colors — 0 here lets the tick skip it.
extern int g_rttJobsLastFrame;

void RenderSecondOrderMirrors(Scene *sc, std::vector<Mirror> &mirrors,
                              std::vector<MirrorRttSlot> &slots);

// Recursion primitive (mirror-recursion campaign, docs/MIRROR_RECURSION_PLAN.md):
// the virtual camera whose view of the REAL scene equals `src`'s view of the
// reflection in the mirror plane (N·x + d = 0). Position is mirrored; the
// basis is `src`'s basis with each row reflected across N — a det = -1
// (left-handed) basis, so a view rendered with it is winding-flipped and MUST
// be rasterized with inverted back-face culling (the reflection reverses
// handedness). Forward = row 2, as everywhere in the engine.
Camera MirrorReflectedCamera(const Camera &src, const Vector &N, float d);

// Per-frame probe for second-order ("mirror in mirror") pairs, gated
// by --mirror-rtt-probe. For each ordered pair (A, B) of ACTIVE base
// mirrors where A's clone of B's panel projects on screen, logs:
//   - the projected pixel bbox (= the RTT resolution actually needed),
//   - the doubly-reflected virtual camera C_B = reflect_B(reflect_A(cam))
//     (rendering the real scene from C_B through B's panel window is
//     exactly what should appear on the panel inside A's reflection),
//   - B's panel window rect in its plane basis (the off-axis frustum).
// Call AFTER the camera is current (post-Transform). Diagnostic only —
// groundwork for the order-2 render-to-texture path.
void ProbeSecondOrderMirrors(Scene *sc, const std::vector<Mirror> &mirrors);

// Debug viz — overlays gb.mirrorId onto the framebuffer so we can see
// exactly which pixels each mirror's wall pre-pass stamped (and where
// the clone-rasterizer's per-pixel check is consequently keeping vs
// rejecting writes). Gated by --greets-mirror-debug-mask.
void DebugOverlayMirrorMask(Scene *sc);

// Per-frame mask pre-pass: walks every mirror's wall faces, transforms
// each triangle world → view → screen itself (NOT via Vertex::TPos_AOS,
// which is stale for frustum-culled meshes), and scanline-fills
// gb.mirrorMask with the mirror's id at each covered pixel. Cleared to
// 0 at entry so previous-frame coverage doesn't leak. Call AFTER the
// camera update for this frame and BEFORE Render() (so the stamp is in
// place when clone faces rasterize).
void StampMirrorMasks(Scene *sc, const std::vector<Mirror> &mirrors);

}  // namespace fds
