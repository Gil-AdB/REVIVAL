#include "RENDER/Hdr.h"

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FrameState.h"      // MainRenderTargetFromGlobals
#include "Base/RenderTarget.h"
#include "Base/FeatureFlags.h"

#include <algorithm>
#include <cmath>

namespace fds {

std::vector<float> g_hdrBuf;
bool g_hdrActive = false;
bool g_hdrDeferTonemap = false;

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
    const float sat      = FeatureFlags::hdr_white();       // repurposed: chroma scale (1=neutral)
    const bool  linear   = FeatureFlags::hdr_linear();      // Phase 3 Stage A: tonemap in linear
    const int   stride   = rt.bytesPerScanline / 4;         // VPage row stride in uint32

    // ACES filmic (Narkowicz fit) per channel: a toe that preserves the low-mid
    // base + a shoulder that rolls highlights off AND desaturates them toward
    // white — which tames the over-saturation from running the glow uncapped
    // (glowMax off). Optional chroma scale (hdr_white) lets us pull saturation
    // further toward/away from the tonemapped luminance. Single-threaded; tile
    // (Render()'s job model) once the look is validated.
    auto aces = [](float x) -> float {
        x = (x * (2.51f*x + 0.03f)) / (x * (2.43f*x + 0.59f) + 0.14f);
        return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    };
    const float kN = 1.0f / 255.0f;
    const float kE = exposure * kN;
    // Chroma scale (linear luma weights) + 8-bit pack. In the linear path the
    // sat block runs on ACES-LINEAR values (where these Rec.709 weights actually
    // belong) and the result is sRGB-encoded (gamma-2.0 sqrt) before packing.
    auto finish = [&](float b, float g, float r, uint32_t& outPix) {
        if (sat != 1.0f) {
            const float L = 0.0722f*b + 0.7152f*g + 0.2126f*r;
            b = L + (b - L)*sat; g = L + (g - L)*sat; r = L + (r - L)*sat;
        }
        if (linear) {
            b = b > 0.0f ? std::sqrt(b) : 0.0f;
            g = g > 0.0f ? std::sqrt(g) : 0.0f;
            r = r > 0.0f ? std::sqrt(r) : 0.0f;
        }
        auto q = [](float c){ int v = int(c*255.0f+0.5f); return uint32_t(v<0?0:(v>255?255:v)); };
        outPix = q(b) | (q(g) << 8) | (q(r) << 16) | 0xFF000000u;
    };
    // Linear path (Stage A): the accumulated buffer is gamma-encoded (lit scene
    // copied from the 8-bit VPage + additive overlays). Decode gamma->linear with
    // the cheap gamma-2.0 approx (square), scale exposure in linear, ACES, then
    // finish() re-encodes (sqrt). Gamma path is byte-identical to before.
    auto tmRGB = [&](float B, float G, float R, uint32_t& outPix) {
        float b, g, r;
        if (linear) {
            const float bn = B*kN, gn = G*kN, rn = R*kN;
            b = aces(bn*bn*exposure); g = aces(gn*gn*exposure); r = aces(rn*rn*exposure);
        } else {
            b = aces(B*kE); g = aces(G*kE); r = aces(R*kE);
        }
        finish(b, g, r, outPix);
    };
    for (int y = 0; y < rt.yres; ++y) {
        uint32_t*    row = rt.vpage + size_t(y) * stride;
        const float* h   = g_hdrBuf.data() + size_t(y) * size_t(rt.xres) * 4;
        for (int x = 0; x < rt.xres; ++x)
            tmRGB(h[x*4+0], h[x*4+1], h[x*4+2], row[x]);
    }
}

} // namespace fds
