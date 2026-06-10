#ifndef FDS_OFFSCREEN_VIEW_H_INCLUDED
#define FDS_OFFSCREEN_VIEW_H_INCLUDED

#include <mutex>

struct Scene;
struct Camera;
struct VESA_Surface;

// Serializes MainSurf swaps against a concurrent EngineResize (the
// resize path memcpy's the SDL surface over MainSurf; doing that while
// an offscreen pass has MainSurf pointed elsewhere stomps the pass's
// state). Defined in OffscreenView.cpp; DEMO/Resize.cpp declares it
// extern.
extern std::mutex g_engineSurfaceMutex;

namespace fds {

// RAII owner of an offscreen-render world swap. The engine's render
// state is global soup (MainSurf, ::View, FOVX/FOVY, CntrE*, Sc->NZP/
// FZP, AspectRatio, C_NZP/zscale/clipper viewport); any pass that
// renders to a side surface must swap and faithfully restore all of
// it, and several of those globals are STAMPED — written into derived
// state by SetCurrentScene / VESA_Surface2Global rather than read live
// (writing Sc->NZP alone never reaches the clipper; that bug cost a
// day on the mirror RTT). This scope makes the swap safe:
//
//   - construction locks g_engineSurfaceMutex, saves the main view
//     state, and points MainSurf at the target surface;
//   - setNearZ()/setFarZ() are the only sanctioned ways to move the
//     clip planes inside the scope — each re-stamps the scene so
//     C_NZP/C_FZP/zscale/clipper stay in sync;
//   - publishSurface() re-publishes the target's (possibly re-shaped)
//     dimensions into XRes/VPage/CntrE*/YOffs;
//   - destruction restores everything, including the scene re-stamp
//     and FOVX/FOVY (which no engine path recomputes mid-frame).
//
// Used by the mirror RTT pass (DEMO/GreetsMirror.cpp); the CITY
// cube-map bake predates it and still hand-rolls the same dance —
// candidate for adoption.
class OffscreenViewScope {
public:
    OffscreenViewScope(Scene *sc, VESA_Surface *target);
    ~OffscreenViewScope();
    OffscreenViewScope(const OffscreenViewScope &) = delete;
    OffscreenViewScope &operator=(const OffscreenViewScope &) = delete;

    // Re-publish the target surface's dimensions into the engine
    // globals (XRes/YRes/VPage/ZPage16/CntrE*/YOffs). Call after
    // changing the target's X/Y/BPSL (e.g. per-slot aspect shapes).
    void publishSurface();

    // Install an offscreen camera (::View). Restored at scope exit.
    void setView(Camera *cam);

    // Move the near/far clip plane AND re-stamp the scene so the
    // clipper / depth encoding actually see the change.
    void setNearZ(float nzp);
    void setFarZ(float fzp);

private:
    std::lock_guard<std::mutex> lk_;
    Scene        *sc_;
    VESA_Surface *prevMain_;
    Camera       *prevView_;
    float         prevFOVX_, prevFOVY_;
    float         prevNZP_, prevFZP_;
    float         prevAspect_;
};

}  // namespace fds

#endif  // FDS_OFFSCREEN_VIEW_H_INCLUDED
