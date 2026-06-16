#include "RENDER/Hdr.h"

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FrameState.h"      // MainRenderTargetFromGlobals
#include "Base/RenderTarget.h"
#include "Base/FeatureFlags.h"

#include <algorithm>

namespace fds {

std::vector<float> g_hdrBuf;

void Hdr_BeginFrame() {
    const size_t n = size_t(XRes) * size_t(YRes) * 4;
    if (g_hdrBuf.size() != n) g_hdrBuf.assign(n, 0.0f);
    else std::fill(g_hdrBuf.begin(), g_hdrBuf.end(), 0.0f);
}

void Render_TonemapToVPage() {
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const size_t px = size_t(rt.xres) * size_t(rt.yres);
    // No-op until something has populated the buffer (Phase 0 / hdr() off path).
    if (px == 0 || g_hdrBuf.size() < px * 4 || !rt.vpage) return;

    const float exposure = FeatureFlags::hdr_exposure();
    const float W        = FeatureFlags::hdr_white();      // white point (≈1 = display white)
    const float invW2    = (W > 0.0f) ? 1.0f / (W * W) : 0.0f;
    const int   stride   = rt.bytesPerScanline / 4;        // VPage row stride in uint32

    // Extended Reinhard per channel, in the engine's 0..255 radiance scale.
    // NOTE: the buffer is gamma-space radiance for now (linear is Phase 3), so
    // this is a highlight roll-off, not a linear→sRGB tonemap. Single-threaded;
    // tile it (Render()'s job model) once the look is validated.
    auto tm = [&](float c) -> uint32_t {
        c *= exposure * (1.0f / 255.0f);
        c  = c * (1.0f + c * invW2) / (1.0f + c);
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
