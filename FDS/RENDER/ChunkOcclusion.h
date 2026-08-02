#ifndef FDS_RENDER_CHUNK_OCCLUSION_H_INCLUDED
#define FDS_RENDER_CHUNK_OCCLUSION_H_INCLUDED

// Chunk occlusion cull (Phase B of the chunk-occlusion experiment,
// docs/VISIBILITY_PLAN.md §7). A PREVIOUS-FRAME hi-Z reproject, main-view-only.
// Occluders = the REAL scene depth (displaced walls, mummies, robot, ...) for
// FREE — no occluder-raster pass, works on any scene with a Z-buffer.
//
//   1. ChunkOcclusion_EndFrame(sc) — called right AFTER Render, while ZPage16
//      holds this frame's final opaque depth (the tick CLEARS ZPage16 before
//      Transform, so the capture cannot happen at frame start) — min-pools it
//      into a small hi-Z and stores the camera that rendered it. Also runs the
//      --chunk_occl_verify over-cull audit and the --vis_stats report.
//   2. ChunkOcclusion_BeginFrame(sc) — next frame, pre-Transform — arms the
//      cull from that capture and refreshes world AABBs.
//   3. ChunkOcclusion_CullsAabb(mn,mx): project the AABB with the CAPTURED
//      (prev) camera; if the whole box is on-buffer, in front of the near
//      plane, and its nearest corner sits behind the farthest occluder over
//      its screen rect by more than bias + 2*|camera delta|, it is hidden →
//      skip its transform + FList entry.
//
// Conservative in coverage/depth (min-pool: any sky/untouched texel in a block
// = far plane = never cull; near-plane straddle / off-buffer rect → keep;
// rotation-revealed chunks project off the prev buffer → keep). NOT
// byte-identical and NOT pin-safe: the occluder is 1 frame stale, so a chunk
// revealed between frames can be wrongly culled for a single frame (temporal
// pop) — the accepted trade for a free occluder set. The honest gate is the
// --chunk_occl_verify camera-sweep audit (violations must stay 0), not a byte
// pin. MAIN VIEW ONLY — inert on any offscreen/shadow/RTT/probe pass (same
// discipline as Face_MainOnly): those cameras keep their whole worlds.
// Snapshot pin dumps set g_occlSnapshotInert so recipe pins never move.

struct Scene;

namespace fds {

// Per-frame vis-stats accounting (docs/VISIBILITY_PLAN.md instrumentation).
// Written by Transform.cpp (main-view meshes) + the cull; reported in
// EndFrame when --vis_stats. Cheap when off (guarded by g_visStatsActive).
struct ChunkVisStats {
    long long meshesSeen    = 0;   // meshes entering the main-view xform loop
    long long vertsSeen     = 0;
    long long meshesXformed = 0;   // meshes that reached the per-vertex xform
    long long vertsXformed  = 0;
    long long chunksTested  = 0;   // AABBs the occlusion cull tested
    long long chunksCulled  = 0;   // AABBs it rejected (fully occluded)
    long long vertsCulled   = 0;
    long long facesCulled   = 0;
    double    prepassMs     = 0.0; // ZPage16 min-pool + world-AABB refresh cost
};

// True while a main-view occluder buffer is live for the current tick thread.
// Set in BeginFrame; Transform.cpp caches it once and gates its stats + cull.
extern bool g_chunkOcclActive;
extern bool g_visStatsActive;
extern ChunkVisStats g_chunkVisStats;

// Set true by the snapshot (pin-dump) harness so the cull is INERT there:
// multi-t snapshot recipes (chase) run several ticks per process, so ticks
// after the first would otherwise see a prev-frame depth and cull, moving
// recipe-fragile pins. Bench (timing / verify sweep) leaves this false.
extern bool g_occlSnapshotInert;

// FDS_OCCL_VERIFY running total across the sweep: culled chunks that the FINAL
// current-frame depth says would have contributed a pixel (a real over-cull).
extern long long g_occlViolationsTotal;

// Arm the cull from the previous frame's capture. No-op (leaves
// g_chunkOcclActive false) unless --chunk_occlusion is on, a same-scene
// capture exists, we're on the main view, and no snapshot-inert. Call after
// Animate_Objects (poses current), before the main Transform_Objects.
void ChunkOcclusion_BeginFrame(Scene* sc);

// Reject test: true iff the world AABB was fully hidden in the prev-frame
// capture (skip transform + FList). verts/faces feed the stats when culled.
// Returns false immediately when !g_chunkOcclActive. Callers MUST additionally
// be on the main view (Transform gates on !_offscreenPass && !offAxis).
bool ChunkOcclusion_CullsAabb(const float mn[3], const float mx[3],
                              int verts, int faces);

// Capture this frame's depth for the next frame (call right AFTER Render,
// main view only), run the verify audit, dump --vis_stats, reset.
void ChunkOcclusion_EndFrame(Scene* sc);

}  // namespace fds

#endif  // FDS_RENDER_CHUNK_OCCLUSION_H_INCLUDED
