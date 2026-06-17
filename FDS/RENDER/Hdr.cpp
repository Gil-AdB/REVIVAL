#include "RENDER/Hdr.h"

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FrameState.h"      // MainRenderTargetFromGlobals
#include "Base/RenderTarget.h"
#include "Base/FeatureFlags.h"

#include <algorithm>

namespace fds {

std::vector<float> g_hdrBuf;
bool g_hdrActive = false;

void Hdr_BeginFrame() {
    g_hdrActive = false;
    const size_t n = size_t(XRes) * size_t(YRes) * 4;
    if (g_hdrBuf.size() != n) g_hdrBuf.assign(n, 0.0f);
    else std::fill(g_hdrBuf.begin(), g_hdrBuf.end(), 0.0f);
}

void Render_TonemapToVPage() {
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const size_t px = size_t(rt.xres) * size_t(rt.yres);
    // No-op until something has populated the buffer this frame: unsized, or the
    // froxel composite never ran (fog off / non-froxel path) — otherwise we'd
    // tonemap a cleared buffer to black (greets with fog off did exactly this).
    if (px == 0 || g_hdrBuf.size() < px * 4 || !rt.vpage || !g_hdrActive) return;

    const float exposure = FeatureFlags::hdr_exposure();
    const float knee     = std::clamp(FeatureFlags::hdr_white(), 0.05f, 0.95f); // identity below
    const float invHead  = 1.0f / (1.0f - knee);
    const int   stride   = rt.bytesPerScanline / 4;        // VPage row stride in uint32

    // Soft-knee highlight roll-off per channel, in the engine's 0..255 radiance
    // scale: identity below `knee`, smoothly asymptoting to white above. This is
    // glowMax's shape (which we disabled in HDR mode) applied ONCE on the final
    // composited radiance instead of per fog slice. NOTE: gamma-space radiance
    // for now (linear is Phase 3), so it's a roll-off, not a linear→sRGB map.
    // Single-threaded; tile it (Render()'s job model) once the look is validated.
    auto tm = [&](float c) -> uint32_t {
        c *= exposure * (1.0f / 255.0f);
        if (c > knee) { const float e = c - knee; c = knee + e / (1.0f + e * invHead); }
        int v = int(c * 255.0f + 0.5f);
        return uint32_t(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    for (int y = 0; y < rt.yres; ++y) {
        uint32_t*    row = rt.vpage + size_t(y) * stride;
        const float* h   = g_hdrBuf.data() + size_t(y) * size_t(rt.xres) * 4;
        for (int x = 0; x < rt.xres; ++x) {
            const uint32_t b = tm(h[x*4+0]);
            const uint32_t g = tm(h[x*4+1]);
            const uint32_t r = tm(h[x*4+2]);
            row[x] = b | (g << 8) | (r << 16) | 0xFF000000u;
        }
    }
}

} // namespace fds
