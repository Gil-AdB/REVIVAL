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

}  // namespace fds

#endif  // FDS_RENDER_WORLD_AABB_H_INCLUDED
