#ifndef FDS_FRAME_STATE_H_INCLUDED
#define FDS_FRAME_STATE_H_INCLUDED

#include <cstdint>

struct Camera;
struct Face;

namespace fds {

// Per-render-pass scratch state. Each call to Transform_Objects (one for
// the main pass, one per shadow-casting light) writes into one of these.
// Today there's a single g_mainFrame used by the legacy globals; later
// the shadow pass will hand each light its own instance to enable
// cross-light parallelism.
//
// All fields are owned by the caller — the FrameState struct just bundles
// them so they can travel together as a parameter instead of being
// scattered file-scope externs.
struct FrameState {
    // Face list + per-stage counts. Populated by Transform_Objects;
    // consumed by Radix_SortingASM, Lighting, the rasterizer dispatchers.
    Face   **FList   = nullptr;
    Face   **SList   = nullptr;
    int32_t  CAll    = 0;     // total slots used in FList (faces + omnis + particles)
    int32_t  CPolys  = 0;     // face count (excludes omnis + particles)
    int32_t  COmnies = 0;     // omni flare entries
    int32_t  CPcls   = 0;     // particle face entries
    int32_t  Polys   = 0;     // total polys in the scene (informational)

    // Active camera + projection constants for this pass. The main pass
    // uses the scene's camera; each shadow pass swaps in a per-light
    // camera. Perspective constants are derived from the camera via
    // CalcPersp; we cache them here so worker threads don't race on the
    // CalcPersp setter.
    Camera  *View      = nullptr;
    float    FOVX      = 0.0f;
    float    FOVY      = 0.0f;
    int32_t  CntrX     = 0;     // screen center x (int — pixel coord)
    int32_t  CntrY     = 0;     // screen center y
    float    CntrEX    = 0.0f;  // sub-pixel-precise extended center x
    float    CntrEY    = 0.0f;

    // Depth encoding for this pass. zscale is derived from the active
    // FZP per CalcPersp / SetCurrentScene; it sits here so shadow passes
    // can use a different encoding than the main pass.
    float    g_zscale    = 0.0f;
    float    g_zscale256 = 0.0f;

    // Frustum near/far plane reciprocals used by the clipper + per-vertex
    // 1/z lerp. C_FZP/C_NZP are the actual planes; C_rFZP/C_rNZP are the
    // precomputed reciprocals.
    float    C_FZP  = 0.0f;
    float    C_rFZP = 0.0f;
    float    C_NZP  = 0.0f;
    float    C_rNZP = 0.0f;
};

// Process-wide instance. The legacy externs in FDS_VARS.H bind to fields
// inside this struct via reference aliases (FrameState.cpp), so any old
// code reading/writing `CAll` actually reads/writes g_mainFrame.CAll.
extern FrameState g_mainFrame;

} // namespace fds

#endif
