#include "RENDER/Hdr.h"

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FrameState.h"      // MainRenderTargetFromGlobals
#include "Base/RenderTarget.h"
#include "Base/FeatureFlags.h"
#include "Threads.h"             // ThreadPool — tile the tonemap across the pool

#include <algorithm>
#include <cmath>
#include <semaphore>
#include <climits>

// Shared tile-completion semaphore (defined with the deferred passes; same
// 6x4 tile-job model used by the fog composite / deferred kernel).
namespace renderns { extern std::counting_semaphore<INT_MAX> tileDone; }

namespace fds {

std::vector<float> g_hdrBuf;
bool g_hdrActive = false;
bool g_hdrDeferTonemap = false;
int g_hdrBufW = 0, g_hdrBufH = 0;

void Hdr_BeginFramePass(int w, int h) {
    g_hdrActive = false;
    g_hdrBufW = w;
    g_hdrBufH = h;
    const size_t n = size_t(w) * size_t(h) * 4;
    if (g_hdrBuf.size() != n) g_hdrBuf.assign(n, 0.0f);
    else std::fill(g_hdrBuf.begin(), g_hdrBuf.end(), 0.0f);
}

// Main view: size g_hdrBuf to the global framebuffer. The mirror RTT calls
// Hdr_BeginFramePass with its own (smaller) dims so its reflection blooms
// through the same tonemap — see RenderSecondOrderMirrors.
void Hdr_BeginFrame() { Hdr_BeginFramePass(XRes, YRes); }

void Hdr_ActivateNoFog() {
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const size_t px = size_t(rt.xres) * size_t(rt.yres);
    if (px == 0 || g_hdrBuf.size() < px * 4 || !rt.vpage) return;
    const bool  linear = FeatureFlags::hdr_linear();
    const int   stride = rt.bytesPerScanline / 4;
    const float kInv   = 1.0f / 255.0f;
    for (int y = 0; y < rt.yres; ++y) {
        const uint32_t* row = rt.vpage + size_t(y) * size_t(stride);
        float*          h   = g_hdrBuf.data() + size_t(y) * size_t(rt.xres) * 4;
        for (int x = 0; x < rt.xres; ++x) {
            if (h[x*4+3] != 0.0f) continue;   // covered: kernel already wrote radiance
            const uint32_t p = row[x];
            float b = float(p & 0xFFu), g = float((p >> 8) & 0xFFu), r = float((p >> 16) & 0xFFu);
            if (linear) { b = b*b*kInv; g = g*g*kInv; r = r*r*kInv; }  // gamma-2.0 → linear scale
            h[x*4+0] = b; h[x*4+1] = g; h[x*4+2] = r;
        }
    }
    g_hdrActive = true;
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

    const float kN = 1.0f / 255.0f;
    const float kE = exposure * kN;
    const float* const hbuf = g_hdrBuf.data();
    uint32_t* const     vp  = rt.vpage;
    const int W = rt.xres, H = rt.yres;

    // Per-tile tonemap, captured BY VALUE so the threaded jobs are self-contained
    // (no dangling refs into this stack frame). ACES filmic (Narkowicz fit): a toe
    // that preserves the low-mid base + a shoulder that rolls highlights off AND
    // desaturates toward white. hdr_white pulls chroma toward/away the tonemapped
    // luminance (Rec.709 weights, applied on the ACES-LINEAR values). Linear path
    // (full coherence): the buffer is already linear radiance, so NO decode —
    // expose, ACES, then sRGB-encode (gamma-2.0 sqrt). Gamma path (--hdr without
    // --hdr_linear) is byte-identical to before.
    auto tileBody = [=](int x1, int y1, int x2, int y2) {
        auto aces = [](float x) -> float {
            x = (x * (2.51f*x + 0.03f)) / (x * (2.43f*x + 0.59f) + 0.14f);
            return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        };
        auto q = [](float c){ int v = int(c*255.0f+0.5f); return uint32_t(v<0?0:(v>255?255:v)); };
        for (int y = y1; y < y2; ++y) {
            uint32_t*    row = vp   + size_t(y) * size_t(stride);
            const float* h   = hbuf + size_t(y) * size_t(W) * 4;
            for (int x = x1; x < x2; ++x) {
                float b = aces(h[x*4+0]*kE), g = aces(h[x*4+1]*kE), r = aces(h[x*4+2]*kE);
                if (sat != 1.0f) {
                    const float L = 0.0722f*b + 0.7152f*g + 0.2126f*r;
                    b = L + (b - L)*sat; g = L + (g - L)*sat; r = L + (r - L)*sat;
                }
                if (linear) {
                    b = b > 0.0f ? std::sqrt(b) : 0.0f;
                    g = g > 0.0f ? std::sqrt(g) : 0.0f;
                    r = r > 0.0f ? std::sqrt(r) : 0.0f;
                }
                row[x] = q(b) | (q(g) << 8) | (q(r) << 16) | 0xFF000000u;
            }
        }
    };

    // 6x4 tile-job dispatch (Render()'s model). The tonemap is embarrassingly
    // parallel; callers run on the tick thread (not a pool worker), so
    // enqueue+wait can't deadlock.
    constexpr int numTilesX = 6, numTilesY = 4;
    const int tsx = (W + numTilesX - 1) / numTilesX;
    const int tsy = (H + numTilesY - 1) / numTilesY;
    for (int j = 0; j < numTilesY; ++j) {
        const int y1 = tsy * j, y2 = std::min(y1 + tsy, H);
        for (int i = 0; i < numTilesX; ++i) {
            const int x1 = tsx * i, x2 = std::min(x1 + tsx, W);
            ThreadPool::instance().enqueue([=]() { tileBody(x1, y1, x2, y2); renderns::tileDone.release(); });
        }
    }
    for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
}

} // namespace fds
