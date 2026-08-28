#ifndef FDS_RENDER_PIPELINE_H_INCLUDED
#define FDS_RENDER_PIPELINE_H_INCLUDED

// RenderPath + the legacy free-function entry points (Render, RunScene,
// SetCurrentScene, SetDeferredWaterMatID, deferredEnabled) are declared
// alongside the rest of the engine API in FDS/Base/FDS_DECS.H. This
// header introduces the class form; the free functions forward into it.

struct Scene;
enum class RenderPath;

// Subset selector for the deferred-transparent tile dispatcher. Used by
// renderFrame to peel front-facing and back-facing halves of the same
// mesh into separate G-buffer layers when both contribute pixels.
enum class XparFaceSel { Both, BackOnly, FrontOnly };

namespace fds {

// Owns the per-frame render orchestration. Today this is a thin class
// around the existing pipeline globals — instance() returns a process-
// wide singleton so the legacy free-function entry points (Render,
// RunScene, SetCurrentScene) can forward into it without touching every
// call site. As we extract stage methods out of RENDER.CPP each one
// becomes a member here and the singleton picks up its per-stage state
// implicitly.
class RenderPipeline {
public:
    static RenderPipeline &instance();

    // Stage entry: full per-frame render. Consumes the per-frame globals
    // populated by Transform_Objects / Lighting and writes VPage + the
    // deferred G-buffer / shadow maps as configured.
    // skipVolumetric=true bypasses cones/halos/unified passes (used by
    // City's pass-1 reflection bake where the result gets distorted
    // through dispMap and volumetric work would be largely wasted).
    void renderFrame(RenderPath path, bool skipVolumetric = false);

    // One-shot helpers for the snapshot/test harnesses — drive a scene
    // through `seconds` of Timer ticks and render the final frame, or
    // wire engine globals (CurScene, FOV, zScale, …) to a fresh scene.
    void runScene(Scene *sc, float seconds);
    void setCurrentScene(Scene *sc);

    // City pass-2 water compositing: matches G-buffer mat32 == this id
    // to blend the lit water texel over pass-1's mirrored-world VPage.
    void setDeferredWaterMatID(int id);
    int  deferredWaterMatID() const { return waterMatID_; }

private:
    RenderPipeline()  = default;
    ~RenderPipeline() = default;
    RenderPipeline(const RenderPipeline &)            = delete;
    RenderPipeline &operator=(const RenderPipeline &) = delete;

    int waterMatID_ = -1;
};

// ── "this renderFrame IS the mirrored water-reflection underlay" ───────────
//
// Independent of ReflMirror (which is gated on --refl_correct and is about the
// mirror MATH). This says only WHICH ROLE the pass plays, so the reflection
// pass can be given cheaper work than the main view without the wholesale
// `skipVolumetric=true` city uses — see the --refl_skip_* flags in
// FeatureFlags.def and PERF_STATE §00m.
//
// Read only by renderFrame, on the tick thread, outside every per-pixel loop.
// Every --refl_skip_* flag defaults OFF, so with none of them passed this
// global is READ and never acted on: the shipping frame is byte-identical.
extern bool g_reflUnderlayPass;

struct ReflUnderlayScope {
    bool prev;
    explicit ReflUnderlayScope(bool on) : prev(g_reflUnderlayPass) { g_reflUnderlayPass = on; }
    ~ReflUnderlayScope() { g_reflUnderlayPass = prev; }
    ReflUnderlayScope(const ReflUnderlayScope &)            = delete;
    ReflUnderlayScope &operator=(const ReflUnderlayScope &) = delete;
};

} // namespace fds

#endif
