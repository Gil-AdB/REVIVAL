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
#include <vector>

#include <Base/Vector.h>

struct Scene;
struct Material;
struct Texture;
struct TriMesh;
struct VESA_Surface;

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
    void renderReflectionCameras(Scene* sc);
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
        float               textUV[4][2] = {{0}};  // fixed text-UV per corner
    };

    TriMesh* makeShardMesh(Scene* sc, const Vector wc[4], const Vector& N,
                           Material* mat, std::vector<Vector>& localOut);
    float frand01();
    float frandS() { return frand01() * 2.0f - 1.0f; }

    bool                built_  = false;
    bool                active_ = false;
    std::vector<Shard>  shards_;
    Vector              origin_{}, uAxis_{}, vAxis_{}, normal_{};
    float               floorY_ = 0.0f;
    Texture*            reflTex_ = nullptr;
    // Per-shard live reflection cameras. All shards share ONE atlas texture
    // + material (a material per shard would blow the 8-bit deferred matID
    // budget); each shard owns one texRes² cell of the atlas.
    bool                reflCamsOn_ = false;
    int                 texRes_ = 64;
    VESA_Surface*       reflSurf_ = nullptr;   // shared 64² render target
    Texture*            atlasTex_ = nullptr;   // shared cell atlas
    Material*           atlasMat_ = nullptr;   // displays the atlas unlit
    int                 atlasCols_ = 0, atlasRows_ = 0;
    int                 reflCursor_ = 0;       // round-robin start (perf budget)
    bool                reflPrimed_ = false;   // first pass fills every cell
    Texture*            textTex_ = nullptr;    // optional composited text
    bool                hasText_ = false;
    float               reflGain_ = 1.0f;      // reflectance (forward bake is unshadowed)
    // Deterministic LCG so headless snapshots reproduce frame-for-frame.
    uint32_t            rng_ = 0x1234567u;
};

}  // namespace fds
