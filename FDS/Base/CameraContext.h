#ifndef FDS_CAMERA_CONTEXT_H_INCLUDED
#define FDS_CAMERA_CONTEXT_H_INCLUDED

#include <cstdint>

struct Camera;

namespace fds {

// Per-camera projection state. The main pass uses one of these built
// from the scene's CameraHead; each shadow pass builds one from its
// light's pose. Consumed by Transform_Objects (per-vertex projection),
// the clipper's near/far plane test, and every filler that reads
// per-pixel `viewZ * zScale` for depth encoding.
//
// Replaces these scattered file-scope globals:
//   View                          (active Camera*)
//   FOVX, FOVY                    (perspective fov pixels)
//   CntrX, CntrY, CntrEX, CntrEY  (screen center for projection)
//   C_FZP, C_rFZP, C_NZP, C_rNZP  (far/near plane + reciprocals)
//   g_zscale, g_zscale256         (depth encoding scalars)
struct CameraContext {
    Camera *view = nullptr;

    // Perspective constants derived from view->IFOV via CalcPersp.
    // FOVX/Y are pixel-space focal lengths; CntrX/Y are the screen
    // center in pixels; CntrEX/Y are the same in float (subpixel-
    // precise) so projection math doesn't round mid-frame.
    float fovX  = 0.0f;
    float fovY  = 0.0f;
    int32_t cntrX  = 0;     // pixel coord (integer)
    int32_t cntrY  = 0;
    float   cntrEX = 0.0f;  // subpixel-precise extended center x
    float   cntrEY = 0.0f;

    // Near/far plane + reciprocals. For the main pass these match
    // Scene::NZP / FZP; for a shadow pass `farZ` is the light's IRange
    // and `nearZ` stays at Scene::NZP. Each clipper uses `farZ` to
    // reject vertices behind the light's range.
    float nearZ  = 0.0f;
    float invNearZ = 0.0f;
    float farZ   = 0.0f;
    float invFarZ = 0.0f;

    // Depth encoding for this pass. zScale is derived from `farZ`
    // (zScale = 0xFF00 / (farZ * 1.1)). Both main and shadow passes
    // encode depth with their own zScale so the 16-bit z-buffer uses
    // its full dynamic range against the relevant far plane.
    float zScale    = 0.0f;
    float zScale256 = 0.0f;
};

} // namespace fds

#endif
