#pragma once
// Mirror shatter effect: fracture a rectangular mirror panel into shards
// that break free and fall to the ground under gravity, tumbling as they
// go. Each shard is a separate rigid body with its own plane, and (when a
// reflection panorama is supplied) its own mirror surface — so the broken
// pieces reflect the room and the reflection tracks each shard's normal as
// it tumbles.
//
// Lifecycle:
//   build()          — at scene construction (before Finalize): creates the
//                      hidden shard meshes + records the panel basis. Shards
//                      stay invisible and inert until triggered.
//   setReflection()  — optional: attach a baked room panorama (see
//                      fds::BakeEquirectPanorama). Without it the shards are
//                      flat lit silver.
//   trigger()        — break it: shards go visible, get impact-driven
//                      velocities + tumble (and turn reflective if a
//                      panorama was set). The caller closes the source
//                      mirror via its own hide hook.
//   update()         — per frame, AFTER Animate_Objects and BEFORE
//                      Transform_Objects (Animate resets IPos/RotMat from the
//                      rest-pose splines; this overrides with the integrated
//                      rigid-body pose).

#include <cstdint>
#include <memory>
#include <vector>

#include <Base/Vector.h>

struct Scene;
struct Material;
struct Texture;
struct TriMesh;
struct VESA_Surface;
namespace meka { struct GBuffer; }

namespace fds {

class MirrorShatter {
public:
    // corners: 4 world-space panel corners, CCW, matching the AddQuad order
    // (corners[1]-corners[0] = U axis, corners[3]-corners[0] = V axis).
    // N: outward panel normal (toward the room). shardMat: opaque lit
    // material for the pieces. gx/gy: fracture grid resolution. floorY: the
    // world Y the shards settle onto (mirrortest's floor is 0; greets is
    // higher).
    void build(Scene* sc, const Vector corners[4], const Vector& N,
               Material* shardMat, int gx = 14, int gy = 10, float floorY = 0.0f);

    // Attach an equirectangular reflection panorama (baked from the mirror's
    // vantage). Once set, trigger() flips every shard face to
    // Face_Reflective. Pass nullptr to keep the flat silver look.
    void setReflection(Texture* panoTex) { reflTex_ = panoTex; }

    // Per-shard live reflection cameras: each shard becomes its own planar
    // mirror, re-rendering the scene from its reflected viewpoint into a
    // small (texRes²) texture every frame — parallax-correct, tracks the
    // tumble. Call once at/after trigger(); then call renderReflectionCameras
    // every frame (before the main Transform_Objects, like the mirror RTT).
    // textTex/textWorldAffine (optional): the screen's text texture + a
    // world→text-UV affine (tu = a·world+b over the screen plane: 4 coeffs
    // for U then 4 for V) so each shard carries the fixed text fragment it
    // had on the intact screen, composited over its live reflection.
    void enableReflectionCameras(Scene* sc, int texRes = 64,
                                 Texture* textTex = nullptr,
                                 const float textWorldAffine[8] = nullptr);
    // Re-render every shard's reflection into its atlas cell. Forward path
    // (default) fans the shards across the thread pool — each worker owns its
    // surface / camera / face-list / vertex-scratch and renders WHOLE +
    // single-threaded on a pool thread (inter-render parallelism; Slice 6 of
    // docs/RENDER_CONTEXT_PLAN.md). FDS_SHARD_REFL_SERIAL=1 (or the opt-in
    // deferred bake) forces the original serial path.
    void renderReflectionCameras(Scene* sc);

    // MirrorShatter owns a thread-pool's worth of reflection-render scratch
    // (ReflWorker is nontrivial: surface + vertex clones); ctor + dtor are
    // out-of-line so the unique_ptr<ReflPool> member sees the complete type.
    MirrorShatter();
    ~MirrorShatter();
    // Reflectance gain applied to the baked reflection (the forward bake has
    // no shadows, so greets' over-ranged omnis flood it far brighter than the
    // shadowed deferred main view — same reason the screen RTT uses gain<1).
    void setReflectionGain(float g) { reflGain_ = g; }
    bool reflectionCamerasEnabled() const { return reflCamsOn_; }

    // Break the panel. impactU/impactV in [0,1] across the panel (the crack
    // origin — shards fly radially away from it). No-op if not built or
    // already triggered.
    void trigger(float impactU, float impactV);

    // Advance the shard physics by one frame step. No-op until triggered.
    void update(float dt = 1.0f);

    bool built()  const { return built_; }
    bool active() const { return active_; }
    void debugDump() const;

private:
    struct Shard {
        TriMesh*            mesh = nullptr;
        std::vector<Vector> local;     // shard verts relative to centroid
        Vector              pos{};     // world centroid
        Vector              eul{};     // accumulated euler angles (rad)
        Vector              vel{};     // linear velocity (world units / frame)
        Vector              angVel{};  // euler angular velocity (rad / frame)
        bool                settled = false;
        float               settleRot[3][3] = {{0}}; // final rest orientation (published each frame once settled)
        float               cFloor = -1e30f;        // cached floor height under this shard
        float               cfX = 1e30f, cfZ = 1e30f; // (x,z) the cached floor was sampled at
        float               textUV[4][2] = {{0}};  // fixed text-UV per corner
    };

    TriMesh* makeShardMesh(Scene* sc, const Vector wc[4], const Vector& N,
                           Material* mat, std::vector<Vector>& localOut);
    float frand01();
    float frandS() { return frand01() * 2.0f - 1.0f; }

    // Per-pool-thread reflection-render scratch (defined in the .cpp): an
    // owned offscreen surface + camera + face-list + vertex-clone scratch, so
    // N shards render concurrently with zero shared mutable state. Forward-
    // declared here; held by value in reflWorkers_ (out-of-line dtor).
    struct ReflWorker;
    struct ReflPool;   // holds std::vector<ReflWorker> (pimpl — keeps the heavy
                       // engine-context types out of this header)
    // The original serial pass — preserved verbatim as the deferred-bake path
    // and the FDS_SHARD_REFL_SERIAL fallback (exact prior behaviour).
    void renderReflectionCamerasSerial(Scene* sc);
    // Render one shard's live reflection into its atlas cell using only the
    // worker's own state (no engine globals beyond the thread_local cull cone).
    void renderShardIntoCell(Scene* sc, int si, ReflWorker& w,
                             const Vector& camEye, int aw, int ah);

    bool                built_  = false;
    bool                active_ = false;
    std::vector<Shard>  shards_;
    Vector              origin_{}, uAxis_{}, vAxis_{}, normal_{};
    float               floorY_ = 0.0f;
    // Static floor/stage surfaces (world tris, flattened 3-per) sampled at
    // build time: up-facing opaque faces at or below the panel base. Each
    // shard ray-casts straight down into these to find the surface beneath
    // it, so debris settles on a stage, a step, or the floor — whatever is
    // actually under it — instead of one global plane.
    std::vector<Vector> floorTris_;
    float castFloorAt(float x, float z) const;
    Texture*            reflTex_ = nullptr;
    // Per-shard live reflection cameras. All shards share ONE atlas texture
    // + material (a material per shard would blow the 8-bit deferred matID
    // budget); each shard owns one texRes² cell of the atlas.
    bool                reflCamsOn_ = false;
    int                 texRes_ = 64;
    // Forward parallel path: one ReflWorker per pool thread, kept warm across
    // frames (vertex-clone scratch + face buffers stay allocated). Sized lazily
    // in renderReflectionCameras to ThreadPool::size().
    std::unique_ptr<ReflPool> reflPool_;
    VESA_Surface*       reflSurf_ = nullptr;   // shared 64² target (serial/deferred path)
    Texture*            atlasTex_ = nullptr;   // shared cell atlas
    Material*           atlasMat_ = nullptr;   // displays the atlas unlit
    int                 atlasCols_ = 0, atlasRows_ = 0;
    int                 reflCursor_ = 0;       // round-robin start (perf budget)
    bool                reflPrimed_ = false;   // first pass fills every cell
    Texture*            textTex_ = nullptr;    // optional composited text
    bool                hasText_ = false;
    float               reflGain_ = 1.0f;      // reflectance (forward bake is unshadowed)
    // Slice 2 (RenderContext): optional per-context G-buffer for a DEFERRED
    // shard bake (shadowed, matches the main view) — opt-in via
    // FDS_SHARD_DEFERRED. Sized to texRes²; swapped into the g_gbuffer
    // globals around each shard's deferred render. Null when forward-baking.
    bool                deferredBake_ = false;
    meka::GBuffer*      reflGB_     = nullptr;
    meka::GBuffer*      reflGBxF_   = nullptr;  // transparent front
    meka::GBuffer*      reflGBxB_   = nullptr;  // transparent back
    std::vector<uint16_t> reflXparZ_, reflXparZBack_;
    // Deterministic LCG so headless snapshots reproduce frame-for-frame.
    uint32_t            rng_ = 0x1234567u;
};

}  // namespace fds
