#ifndef FDS_RENDER_CONTEXT_H_INCLUDED
#define FDS_RENDER_CONTEXT_H_INCLUDED

#include "RenderTarget.h"
#include "CameraContext.h"
#include "FaceListContext.h"

// See docs/RENDER_CONTEXT_PLAN.md.
//
// One self-contained bundle of everything a render pass reads/writes that is
// currently scattered across file-scope globals (MainSurf/VPage/ZPage16/
// XRes/YRes/VESA_BPSL, the G-buffer set, View/FOVX/CntrE*, FList/CAll,
// CurScene). The migration's end state: passes take a RenderContext explicitly
// instead of swapping globals, so offscreen renders (shard reflections, mirror
// RTT, cube bake) can own their own state and — eventually — run concurrently.
//
// Slice 1 (this file): the type + a builder that bundles today's canonical
// globals into one. Nothing is forced through it yet, so the engine is
// byte-identical; offscreen callers (Slice 2+) construct their own by value.

struct Scene;

namespace fds {

struct RenderContext {
    RenderTarget    target;          // surface + G-buffer (where pixels land)
    CameraContext   camera;          // view + projection
    FaceListContext faces;           // FList / SList / CAll
    Scene          *scene = nullptr; // replaces CurScene

    // Offscreen contexts own their framebuffer + G-buffer here so they don't
    // alias the engine's. Null/empty for the primary (engine-framebuffer)
    // context, which points `target` at the live VPage/g_gbuffer. Wired in
    // Slice 2 (per-context G-buffer); declared now so the shape is fixed.
    // (Owned-buffer fields intentionally omitted until Slice 2 defines their
    // exact types — RenderContext stays a plain aggregate until then.)
};

// Bundle the current canonical render globals (g_mainCamera, g_mainFaces,
// MainRenderTargetFromGlobals(), CurScene) into a RenderContext. This is the
// "primary" context — a view onto the engine framebuffer. Behaviour-neutral:
// it reads the same memory the globals do.
RenderContext primaryRenderContext();

} // namespace fds

#endif // FDS_RENDER_CONTEXT_H_INCLUDED
