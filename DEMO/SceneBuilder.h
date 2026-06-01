#pragma once

// SceneBuilder — programmatic scene construction for tests, snapshots,
// and (future) scripted scenes. Encapsulates the engine boilerplate
// of allocating Texture / Material / TriMesh / Face / Object / Omni /
// Camera, linking them into the right global lists, picking a safe
// Filler for each material, and rebuilding the per-scene matTable so
// the deferred path's matID lookup works.
//
// Why a class instead of free functions: the engine spreads state
// across globals (MatLib, OmniHead, ObjectHead, TriMeshHead, MainSurf,
// View, FOVX/FOVY, FList…). Holding one builder per scene gives a
// single place to attach incremental state — current "default
// material," accumulated bsphere bounds, an explicit owning Scene*,
// stats for diagnostics. Free functions would push that state into
// either globals or argument soup; neither survives the move to a
// scripted interface later.

#include <Base/Color.h>
#include <Base/Face.h>      // RasterFunc typedef
#include <Base/Vector.h>

#include <cstdint>

struct Material;
struct Omni;
struct Scene;
struct Texture;
struct TriMesh;

namespace fds::scene_builder {

class SceneBuilder {
public:
    // Construct a fresh empty Scene; caller owns the result via scene().
    SceneBuilder();
    // Attach to an existing scene (e.g. one loaded from FLD that we
    // want to extend programmatically).
    explicit SceneBuilder(Scene *attach);
    ~SceneBuilder() = default;

    // ── Scene configuration ────────────────────────────────────────
    void SetNearFar(float nzp, float fzp);
    void SetAmbient(int r, int g, int b);
    void SetFlags(unsigned flags);

    // ── Textures ───────────────────────────────────────────────────
    // 1×1 to NxN BGRA solid-color texture. Sachletz-tiles internally
    // so the deferred transparent rasterizer's LSizeX/Y read is valid.
    Texture *AddSolidColorTexture(int w, int h, std::uint32_t bgra);
    // 16x16 alpha texture with a single recognizable character ('M')
    // for half-silvered-glass test scenes.
    Texture *AddAlphaTextTexture(const char *fileName);

    // ── Materials ──────────────────────────────────────────────────
    // Links into MatLib + sets RelScene to this builder's scene.
    // PickFillerForMaterial(mat) is called when a face takes this
    // material so the Filler is always set to a known-good handler.
    Material *AddMaterial(const char *name, Texture *tex,
                          Color baseCol, unsigned flags);

    // ── Geometry ───────────────────────────────────────────────────
    // CCW quad → 2 triangles, full Vertex setup (Pos, N, TN, LR/LG/LB,
    // U/V), Face Filler from PickFillerForMaterial. v[4] must be CCW
    // from the outside.
    TriMesh *AddQuad(const char *name, const Vector v[4], Material *mat);

    // 6-face cube of side 2*halfEdge centered at ctr, one Material
    // for all faces.
    TriMesh *AddCube(const char *name, Vector ctr, float halfEdge,
                     Material *mat);

    // ── Lights ─────────────────────────────────────────────────────
    // Plain omni with single-key Pos / Size / Range splines so it
    // survives Animate_Objects / Lighting without crashes (the naked
    // omni hang that bit mirrortest).
    Omni *AddOmni(Vector pos, Color color, float intensity, float range);

    // ── Camera ─────────────────────────────────────────────────────
    void SetCamera(Vector eye, Vector lookAt, float fov);

    // ── Finalize ───────────────────────────────────────────────────
    // Scene_RebuildMatTable + any other deferred-pipeline wiring.
    // Call once after all Add* before rendering.
    void Finalize();

    // ── Accessors ──────────────────────────────────────────────────
    Scene *scene() const { return sc_; }

    // Public filler picker — pick a safe-default RasterFunc based on
    // material flags + texture presence. Used internally by AddQuad
    // etc.; exposed so callers can stamp Filler on hand-built faces.
    // Replaces direct Assign_Fillers for scripted scenes (the engine's
    // table-based picker assumes FLD-loaded materials and can route
    // synthetic mats to handlers that hang on simple inputs).
    static RasterFunc PickFillerForMaterial(const Material *mat);

private:
    Scene *sc_ = nullptr;
    bool   ownsScene_ = false;
};

}  // namespace fds::scene_builder
