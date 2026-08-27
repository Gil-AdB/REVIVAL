#ifndef FDS_RENDER_WORLD_AABB_H_INCLUDED
#define FDS_RENDER_WORLD_AABB_H_INCLUDED

// Foundation F (docs/ENVDYN_DISPLACEMENT_PLAN.md) — world-space AABBs +
// a reusable 6-plane frustum-vs-AABB / -sphere reject.
//
// The engine previously had ONLY a symmetric-frustum bounding-SPHERE cull
// (Transform.cpp) plus the off-axis sphere-vs-viewport-plane variant. This
// module adds the missing pieces the dynamic-env-reflection overlay
// (Workstream A) needs and that the displacement work can reuse:
//
//   • Per-TriMesh world-space AABB (TriMesh::WorldAabb*), maintained by
//     WorldAabb_UpdateScene. Static meshes: computed once (post first
//     transform). Dynamic meshes (the same isDynamicForBake partition the
//     shadow/env bakes use): recomputed each frame by transforming the
//     cached model-space AABB corners through the pose (conservative under
//     rotation — see WorldAabb.cpp for the design note).
//   • A world-space Frustum (6 inward half-space planes) built EITHER from
//     the main camera OR from a probe's padded cube-face pyramid, and the
//     conservative AABB / sphere reject against it.
//   • A gated --draw_aabbs debug overlay.
//
// Non-goals (explicit): no BVH, no occlusion, no precomputed visibility.

struct Scene;
struct TriMesh;
struct Material;
struct Object;
struct Vector;

namespace fds {

// The static/dynamic partition used everywhere (shadow bakes, env bakes, the
// world-AABB build): true iff this object's — or an ancestor's — Pos spline
// spans > 0.1 world units or its Rotate spline spans > 0.01. Exposed so the
// dynamic-env overlay can gather the movers' bspheres with the same rule.
bool WorldAabb_MeshIsDynamic(Object* obj);

// A world-space axis-aligned bounding box.
struct WorldAabb {
    float mn[3] = { 0, 0, 0 };
    float mx[3] = { 0, 0, 0 };
    bool  valid = false;
};

// 6-plane frustum in WORLD space. Each plane's inside half-space is
// n·p >= d (n = inward normal, NOT necessarily unit length). A point/box/
// sphere is safely rejected iff it lies fully in some plane's OUTSIDE
// half-space. `count` planes are populated (6 for a full frustum).
struct Frustum {
    float n[6][3] = {};
    float d[6]    = {};
    int   count   = 0;
};

// Refresh every TriMesh's world AABB. No rendering side effects — writes
// only the TriMesh AABB fields. Cheap (static meshes touched once; dynamic
// meshes = 8 corner transforms/frame). Call after Animate_Objects (pose is
// current) and before any consumer. Callers gate on whether a consumer is
// live (--draw_aabbs / env_dynamic) so the flag-off path stays zero-cost.
void WorldAabb_UpdateScene(Scene* sc);

// Union of the world AABBs of every mesh that references material `M`
// (Face::Txtr == M). valid=false when no live mesh uses M or its boxes
// aren't computed yet. Reads the last WorldAabb_UpdateScene result.
WorldAabb WorldAabb_ForMaterial(Scene* sc, const Material* M);

// Build the 6 world-space frustum planes for the MAIN camera from the live
// projection globals (View->Mat / ISource, FOVX/FOVY, CntrEX/CntrEY,
// XRes/YRes, Scene NZP/FZP). Handles the symmetric case; the off-axis
// center is honored via the per-edge tangents. Returns count==0 if the
// camera/scene aren't ready.
Frustum Frustum_FromMainCamera(Scene* sc);

// Build the padded cube-face pyramid frustum for probe face `face`
// (EnvCube convention), apex at world point `bake`, extending to `range`
// along the face-forward axis. The four side planes use the padded
// half-tangent (kEnvCubePad); near = the apex plane, far = `range`.
Frustum Frustum_FromProbeFace(const float bake[3], int face, float range);

// Conservative reject: true iff the AABB is fully OUTSIDE the frustum
// (safe to cull). False = keep (may still be partly outside). An empty /
// invalid box or a count==0 frustum is never culled (returns false).
bool Frustum_CullsAabb(const Frustum& f, const WorldAabb& b);
bool Frustum_CullsAabb(const Frustum& f, const float mn[3], const float mx[3]);

// Conservative reject: true iff the sphere is fully outside the frustum.
bool Frustum_CullsSphere(const Frustum& f, const float c[3], float r);

// --draw_aabbs overlay: project each mesh's world AABB through the main
// camera and stroke its 12 edges into VPage (post-tonemap). Green = static,
// yellow = dynamic, dim red = culled by the main-camera frustum. The caller
// gates on FeatureFlags::draw_aabbs(); no-op when VPage/camera aren't ready
// or inside an offscreen pass.
void WorldAabb_DrawOverlay(Scene* sc);

// ── --displace_viz: stone-displacement bake overlay ────────────────────────
// A debug view of the geometry the height-map displacement bake
// (DisplaceMaterialVertices, docs/ENVDYN_DISPLACEMENT_PLAN.md B3) actually
// added/moved. The bake records each displaced vertex here; the overlay draws
// the target materials' post-bake triangles as a wireframe, tinted by
// per-vertex displacement magnitude. Both no-op unless --displace_viz is on.

// Called by the bake for every displaced vertex: `M` = the target material,
// `localPos` = the vertex's FINAL (post-displacement) model-space position
// (keyed by exact bits, so the per-cell chunk COPIES the greets init makes
// after the bake still resolve — the copy is bitwise), `dispAbs` = |offset|
// along the normal (world units). Only records when FeatureFlags::displace_viz
// is set (the bake gates the call), so the flag-off path allocates nothing.
void DisplaceViz_Record(const Material* M, const Vector& localPos, float dispAbs);

// --displace_viz=3/4 (DIRECTION/HEIGHT combination): called by the bake for
// every displaced vertex with its FINAL model-space position (exact bits, same
// keying as DisplaceViz_Record) and the full displacement VECTOR final − base
// (model space). Mode 3 tints each displaced triangle by the WORST corner's
// angle between its displacement vector and the triangle's BASE-plane normal
// (reconstructed as base = final − vec, so the pre-bake wall plane, not the
// displaced one); mode 4 draws the vectors themselves as depth-tested needles.
// This is the "which normal did this vertex actually ride, and how much height
// did it carry" view: interiors must read 0° (green), a legitimate mitre reads
// ~45° (yellow), a slide along the wall reads 90° (red), and a vertex carrying
// ~no displacement at all reads solid BLUE (flush — no height), which is a
// different failure than a wrong direction. Gated like Record (--viz_arm arms).
void DisplaceViz_RecordVec(const Material* M, const Vector& finalLocal, const Vector& dispLocal);

// --displace_viz=2 (HEIGHT-ERROR field): called by the bake once per emitted
// displaced triangle with its FINAL centroid (model space, exact bits — same
// keying rationale as DisplaceViz_Record) and the SIGNED height error there =
// truth − carried (world units): truth = the height map's relief at a FINE mip
// (mip 1) probed inside the triangle, carried = the barycentric interp of the
// three vertices' applied displacement. Positive = geometry UNDER-carries the
// map (missing relief → warm), negative = OVER-carries (→ cold). Only records
// when FeatureFlags::displace_viz()==2 (the bake gates the call), so mode 0/1
// allocate nothing for it.
void DisplaceViz_RecordError(const Material* M, const Vector& centroidLocal, float signedErr);

// Draw the recorded displaced materials' triangles as a wireframe over VPage
// (post-tonemap), projected through the main camera, edge colour = per-vertex
// displacement magnitude (cool blue = 0 / pinned borders → warm red = the
// bake's max). Front-facing + near-plane culled, and DEPTH-TESTED against the
// frame's opaque Z (ZPage16; the line z is pulled 1% nearer so a wireframe
// lying on the surface it annotates wins instead of z-fighting) — hidden
// walls stay hidden, so the fan/diagonal structure at the looked-at wall is
// readable. Falls back to draw-through when ZPage16 isn't live. No-op (with
// a one-shot stderr hint) when nothing was recorded — i.e. --displace_viz on
// but --greets_displace off.
void DisplaceViz_DrawOverlay(Scene* sc);

// ── S1d-1 SEAM VIZ (--pom_seam_viz, docs/S1D_CLOSED_SHELL_PLAN.md) ──────────
// PomShell_Build's seam census records every classified patch-boundary edge
// here (model-space endpoints of the Piramid mesh, authored positions), and the
// overlay draws them over the final frame, depth-tested against the frame's
// opaque Z exactly like DisplaceViz's wireframe. Colour = class:
//   GREEN   coplanar continuation (the surface carries on; a hole here is wrong)
//   ORANGE  angled-in  (concave fold — the ray should enter the neighbour)
//   MAGENTA angled-out (convex fold — exit is a TRUE silhouette)
//   RED     true boundary (nothing continues — where side faces belong)
// Both no-op unless --pom_seam_viz > 0, so the flag-off path is byte-null.
void PomSeamViz_Record(const Vector& aLocal, const Vector& bLocal, int cls);
void PomSeamViz_DrawOverlay(Scene* sc);

// ── Arming probes for the runtime viz cycle (VizCycle.cpp) ─────────────────
// "Does this run actually have the data the viz needs?" The cycle drops any
// mode whose data is absent instead of offering a mode that draws nothing —
// the recorders below only run when their flag (or --viz_arm) was set at BAKE
// time, which is long before a live key press.
bool DisplaceViz_HasData();        // --displace_viz=1: per-vertex magnitudes
bool DisplaceViz_HasErrorData();   // --displace_viz=2: per-triangle height error
bool DisplaceViz_HasVecData();     // --displace_viz=3/4: per-vertex vectors
bool PomSeamViz_HasData();         // --pom_seam_viz: classified boundary edges

// ── --wire_viz: whole-scene triangle wireframe ─────────────────────────────
// Post-tonemap overlay over the MAIN view: every visible mesh triangle's three
// edges, DEPTH-TESTED against the frame's opaque Z through the same drawLineZ
// the displacement/seam overlays use (line z pulled 1% nearer so an edge on its
// own surface wins the compare; a far wall behind a near one stays hidden).
// Scene-wide, so it shows the tessellation state honestly: --greets_displace ON
// puts the bake's added faces on screen as real edges, OFF shows the authored
// quads. Modes (see the flag help): 1 = white over the image, 2 = white over the
// dimmed image, 3 = per-material hue + per-mesh brightness, 4 = back-facing red /
// front-facing green. No-op when wire_viz == 0; the caller gates on the flag so
// the off path costs one load and a branch.
void WireViz_DrawOverlay(Scene* sc);

}  // namespace fds

#endif  // FDS_RENDER_WORLD_AABB_H_INCLUDED
