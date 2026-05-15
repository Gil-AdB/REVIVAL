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
    void renderFrame(RenderPath path);

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

} // namespace fds

#endif
