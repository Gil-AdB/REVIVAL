#include "RENDER/Hdr.h"

#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/FrameState.h"      // MainRenderTargetFromGlobals
#include "Base/RenderTarget.h"
#include "Base/Scene.h"           // CurScene->FZP for DoF range normalization
#include "Base/Camera.h"          // View->ITarget for DoF camera-target auto-focus
#include "Base/FeatureFlags.h"
#include "Threads.h"             // ThreadPool — tile the tonemap across the pool

#include <algorithm>
#include <cmath>
#include <semaphore>
#include <climits>

// RenderContext migration: this TU reads the CURRENT render target via
// MainRenderTargetFromGlobals() — deliberately TARGET-POLYMORPHIC: the
// mirror RTT swaps the surface globals and runs these passes on its own
// target, and FOUNTAIN's tick drives the whole post stack outside
// renderFrame (deferred-tonemap flow), so there is no ctx to thread.
// CurScene / g_zscale (DoF's focus normalization) stay global for the
// same reason. The poison covers the names that must NEVER appear bare
// here — target state goes through rt or the (w, h) params.
#pragma GCC poison XRes YRes VPage ZPage16 FOVX FOVY CntrEX CntrEY VESA_BPSL

// Shared tile-completion semaphore (defined with the deferred passes; same
// 6x4 tile-job model used by the fog composite / deferred kernel).
namespace renderns { extern std::counting_semaphore<INT_MAX> tileDone; }

namespace fds {

std::vector<hdrf> g_hdrBuf;
bool g_hdrActive = false;
bool g_hdrDeferTonemap = false;
int g_hdrBufW = 0, g_hdrBufH = 0;
thread_local HdrTarget* t_hdrOverride = nullptr;

// Single-threaded target variants for pool-worker offscreen bakes (see
// Hdr.h). Same math as Hdr_ActivateNoFog / Render_TonemapToVPage minus
// the pool dispatch and the emissive boost (no-op for RTT targets).
void Hdr_ActivateNoFogTarget(HdrTarget& ht, const uint32_t* vpage, int W, int H) {
    if (!ht.buf || !vpage) return;
    const bool  linear = FeatureFlags::hdr_linear();
    const float kInv   = 1.0f / 255.0f;
    for (int y = 0; y < H; ++y) {
        const uint32_t* row = vpage + size_t(y) * size_t(W);
        hdrf*           h   = ht.buf + size_t(y) * size_t(W) * 4;
        for (int x = 0; x < W; ++x) {
            if (h[x*4+3] != 0.0f) continue;   // covered: kernel already wrote radiance
            const uint32_t p = row[x];
            float b = float(p & 0xFFu), g = float((p >> 8) & 0xFFu), r = float((p >> 16) & 0xFFu);
            if (linear) { b = b*b*kInv; g = g*g*kInv; r = r*r*kInv; }
            h[x*4+0] = HdrClamp(b); h[x*4+1] = HdrClamp(g); h[x*4+2] = HdrClamp(r);
        }
    }
    ht.active = true;
}

void Render_TonemapToTarget(const HdrTarget& ht, uint32_t* vpage, int W, int H) {
    if (!ht.buf || !vpage || !ht.active) return;
    const float exposure = FeatureFlags::hdr_exposure();
    const float sat      = FeatureFlags::hdr_white();
    const bool  linear   = FeatureFlags::hdr_linear();
    const float kN = 1.0f / 255.0f;
    const float kE = exposure * kN;
    auto aces = [](float x) -> float {
        x = (x * (2.51f*x + 0.03f)) / (x * (2.43f*x + 0.59f) + 0.14f);
        return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    };
    auto q = [](float c){ int v = int(c*255.0f+0.5f); return uint32_t(v<0?0:(v>255?255:v)); };
    for (int y = 0; y < H; ++y) {
        uint32_t*   row = vpage  + size_t(y) * size_t(W);
        const hdrf* h   = ht.buf + size_t(y) * size_t(W) * 4;
        for (int x = 0; x < W; ++x) {
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
}

void Hdr_BeginFramePass(int w, int h) {
    g_hdrActive = false;
    g_hdrBufW = w;
    g_hdrBufH = h;
    const size_t n = size_t(w) * size_t(h) * 4;
    if (g_hdrBuf.size() != n) g_hdrBuf.assign(n, 0.0f);
    else std::fill(g_hdrBuf.begin(), g_hdrBuf.end(), 0.0f);
}

// Shared row-band tile dispatch (no write overlap: each job owns a row band of
// the TARGET buffer). body(y1,y2) processes rows [y1,y2). Callers run on the
// tick thread, not a pool worker, so enqueue+wait can't deadlock.
template <class Body>
static void hdrDispatchRows(int rows, Body&& body) {
    constexpr int kBands = 24;
    const int band = (rows + kBands - 1) / kBands;
    if (band <= 0) return;
    const int jobs = (rows + band - 1) / band;
    // Work-stealing chunk dispatch via dispatchIndexed (which encodes the
    // straggler/lifetime rules once — see its header comment). `body` is
    // captured by reference: it is only invoked before that band's release,
    // strictly before the drain below returns. Row bands are disjoint and
    // order-free → byte-identical.
    dispatchIndexed(jobs, &renderns::tileDone, [&body, band, rows](int b) {
        const int y = b * band;
        body(y, std::min(y + band, rows));
    });
    for (int k = 0; k < jobs; ++k) renderns::tileDone.acquire();
}

void Hdr_ActivateNoFog() {
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const size_t px = size_t(rt.xres) * size_t(rt.yres);
    if (px == 0 || g_hdrBuf.size() < px * 4 || !rt.vpage) return;
    const bool  linear = FeatureFlags::hdr_linear();
    const int   stride = rt.bytesPerScanline / 4;
    const float kInv   = 1.0f / 255.0f;
    // Mat_HdrEmissive boost: forward env surfaces (the disco ball) stamp the
    // 0xFFFFFFFE sentinel; lift them ×gain so they exceed 255 and over-bloom.
    // Only when the mat32 plane matches THIS pass's dims (in the mirror RTT the
    // globals are the RTT surface but g_gbuffer is the MAIN one → skip).
    const bool  boostOK = rt.mat32 && rt.mat32Count == uint32_t(px);
    const float gain    = boostOK ? FeatureFlags::hdr_refl_gain() : 1.0f;
    // Threaded over disjoint row bands — was a serial full-screen scan that
    // parked all workers between the lighting and cone passes.
    const int            xres  = rt.xres;
    const uint32_t* const vpage = rt.vpage;
    const uint32_t* const mat32 = rt.mat32;
    hdrf* const          hbuf  = g_hdrBuf.data();
    hdrDispatchRows(rt.yres, [=](int y1, int y2) {
        for (int y = y1; y < y2; ++y) {
            const uint32_t* row = vpage + size_t(y) * size_t(stride);
            hdrf*           h   = hbuf  + size_t(y) * size_t(xres) * 4;
            const uint32_t* mrow = boostOK ? mat32 + size_t(y) * size_t(xres) : nullptr;
            for (int x = 0; x < xres; ++x) {
                if (h[x*4+3] != 0.0f) continue;   // covered: kernel already wrote radiance
                const uint32_t p = row[x];
                float b = float(p & 0xFFu), g = float((p >> 8) & 0xFFu), r = float((p >> 16) & 0xFFu);
                if (linear) { b = b*b*kInv; g = g*g*kInv; r = r*r*kInv; }  // gamma-2.0 → linear scale
                if (mrow && mrow[x] == 0xFFFFFFFEu) { b *= gain; g *= gain; r *= gain; }
                h[x*4+0] = HdrClamp(b); h[x*4+1] = HdrClamp(g); h[x*4+2] = HdrClamp(r);
            }
        }
    });
    g_hdrActive = true;
}

// ── Shared HDR bright-pass ────────────────────────────────────────────────
// Bloom, anamorphic, and lens-ghost each built an IDENTICAL bright-pass (DS=4
// downsample of g_hdrBuf, soft knee at bloom_threshold) — streaming the full
// HDR buffer 3× (memory-bound). Compute once per frame; each consumer copies it
// into its own working buffer. Built only when a consumer is active.
static std::vector<float> g_brightPass;   // g_bpBW*g_bpBH*3
static int g_bpBW = 0, g_bpBH = 0;

// The DS=4 soft-knee downsample loop (parallel over rows; scalar inner — it's
// memory-bound, so avoiding the 2 redundant streams matters more than SIMD).
static void hdrBrightPassInto(float* dst, int bw, int bh, int W, int H,
                              const hdrf* hb, float thresh) {
    hdrDispatchRows(bh, [=](int by1, int by2) {
        const float inv = 1.0f / 16.0f;   // DS*DS
        for (int by = by1; by < by2; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                float sb = 0, sg = 0, sr = 0;
                for (int dy = 0; dy < 4; ++dy) {
                    const int sy = by*4 + dy; if (sy >= H) break;
                    const hdrf* h = hb + size_t(sy) * size_t(W) * 4;
                    for (int dx = 0; dx < 4; ++dx) {
                        const int sx = bx*4 + dx; if (sx >= W) break;
                        const float B = h[sx*4+0], G = h[sx*4+1], R = h[sx*4+2];
                        const float lum = R > G ? (R > B ? R : B) : (G > B ? G : B);
                        if (lum > thresh) { const float w = (lum - thresh) / lum; sb += B*w; sg += G*w; sr += R*w; }
                    }
                }
                float* a = dst + (size_t(by) * bw + bx) * 3;
                a[0] = sb * inv; a[1] = sg * inv; a[2] = sr * inv;
            }
    });
}

// Copy the shared bright-pass into `dst`, or compute it if the shared one wasn't
// built for these dims (fallback — keeps each pass correct standalone).
static void getBrightPass(float* dst, int bw, int bh, int W, int H,
                          const hdrf* hb, float thresh) {
    const size_t n = size_t(bw) * size_t(bh) * 3;
    if (g_bpBW == bw && g_bpBH == bh && g_brightPass.size() >= n)
        std::memcpy(dst, g_brightPass.data(), n * sizeof(float));
    else
        hdrBrightPassInto(dst, bw, bh, W, H, hb, thresh);
}

// Build the shared bright-pass once per frame — call BEFORE the consumers.
// No-op unless at least one of bloom / anamorphic / lens-ghost is active.
void Render_HdrBrightPass() {
    g_bpBW = g_bpBH = 0;
    // Sharing is OPT-IN (FDS_SHARED_BP=1), default OFF: the sequential passes
    // deliberately compound — bloom's bright-pass sees anamorphic's streaks,
    // and the lens-ghost HALO samples the post-bloom buffer, so bright sources
    // arrive smoothed (a clean chromatic ring). The shared pre-bloom bright-
    // pass fed the halo raw sparse spec hotspots instead → red/green speckles
    // (user-visible regression, greets t=191). Sharing saved only ~0.4 ms;
    // not worth the look change. Kept for A/B and for configs without the
    // halo/ghost pass.
    static const bool on = std::getenv("FDS_SHARED_BP") != nullptr;
    if (!on) return;
    if (!g_hdrActive) return;
    if (!(FeatureFlags::bloom() || FeatureFlags::anamorphic() || FeatureFlags::lens_ghosts())) return;
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const int W = rt.xres, H = rt.yres;
    const size_t px = size_t(W) * size_t(H);
    if (px == 0 || g_hdrBuf.size() < px * 4) return;
    const int bw = (W + 3) / 4, bh = (H + 3) / 4;
    g_brightPass.assign(size_t(bw) * size_t(bh) * 3, 0.0f);
    hdrBrightPassInto(g_brightPass.data(), bw, bh, W, H, g_hdrBuf.data(),
                      FeatureFlags::bloom_threshold());
    g_bpBW = bw; g_bpBH = bh;
}

// FDS_HDR_SCAN=1: env-gated inf/NaN + max-finite scan of g_hdrBuf, printed
// with a tag per call site. Diagnostic for f16-overflow hunts: the first tag
// reporting inf/NaN names the phase that produced it (before post spreads it).
void Hdr_DebugScan(const char* tag) {
    static const bool on = std::getenv("FDS_HDR_SCAN") != nullptr;
    if (!on || g_hdrBuf.empty()) return;
    const size_t n = g_hdrBuf.size();
    size_t nInf = 0, nNan = 0; float mx = 0; size_t mxI = 0, firstBad = SIZE_MAX;
    for (size_t i = 0; i < n; ++i) {
        if ((i & 3) == 3) continue;                // skip the coverage lane
        const float v = float(g_hdrBuf[i]);
        if (std::isnan(v)) { ++nNan; if (firstBad == SIZE_MAX) firstBad = i; }
        else if (std::isinf(v)) { ++nInf; if (firstBad == SIZE_MAX) firstBad = i; }
        else if (v > mx) { mx = v; mxI = i; }
    }
    const int W = g_hdrBufW > 0 ? g_hdrBufW : 1;
    std::fprintf(stderr, "[HDR-SCAN] %-14s inf=%zu nan=%zu maxFinite=%.0f @(%d,%d)%s\n",
                 tag, nInf, nNan, mx, int((mxI/4)%W), int((mxI/4)/W),
                 firstBad != SIZE_MAX ? " <-- BAD" : "");
}

void Render_BloomPass() {
    if (!FeatureFlags::bloom() || !g_hdrActive) return;
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const int W = rt.xres, H = rt.yres;
    const size_t px = size_t(W) * size_t(H);
    if (px == 0 || g_hdrBuf.size() < px * 4) return;
    const float thresh    = FeatureFlags::bloom_threshold();
    const float intensity = FeatureFlags::bloom_intensity();
    if (intensity <= 0.0f) return;

    constexpr int DS = 4;                              // quarter-res bloom buffer
    const int bw = (W + DS - 1) / DS, bh = (H + DS - 1) / DS;
    static std::vector<float> s_acc, s_tmp;            // reused across frames
    s_acc.assign(size_t(bw) * size_t(bh) * 3, 0.0f);
    s_tmp.assign(size_t(bw) * size_t(bh) * 3, 0.0f);
    const hdrf* const hb = g_hdrBuf.data();
    float* const A = s_acc.data();
    float* const T = s_tmp.data();

    // Bright-pass + downsample (soft knee above the threshold) — SHARED with
    // anamorphic + lens-ghost via Render_HdrBrightPass (computed once/frame;
    // getBrightPass falls back to inline if the shared one wasn't built).
    getBrightPass(A, bw, bh, W, H, hb, thresh);

    // 2. Separable 5-tap gaussian ([1 4 6 4 1]/16), 2 passes for a wider glow.
    //    Horizontal A->T then vertical T->A (each pass writes disjoint rows).
    const float gwc = 6.0f/16.0f, gw1 = 4.0f/16.0f, gw2 = 1.0f/16.0f;
    for (int pass = 0; pass < 2; ++pass) {
        hdrDispatchRows(bh, [=](int y1, int y2) {            // horizontal A -> T
            for (int y = y1; y < y2; ++y)
                for (int x = 0; x < bw; ++x) {
                    const int xm2=std::max(x-2,0), xm1=std::max(x-1,0), xp1=std::min(x+1,bw-1), xp2=std::min(x+2,bw-1);
                    const float* r = A + size_t(y) * bw * 3;
                    float* d = T + (size_t(y)*bw + x)*3;
                    for (int c=0;c<3;++c)
                        d[c] = r[xm2*3+c]*gw2 + r[xm1*3+c]*gw1 + r[x*3+c]*gwc + r[xp1*3+c]*gw1 + r[xp2*3+c]*gw2;
                }
        });
        hdrDispatchRows(bh, [=](int y1, int y2) {            // vertical T -> A
            for (int y = y1; y < y2; ++y) {
                const int ym2=std::max(y-2,0), ym1=std::max(y-1,0), yp1=std::min(y+1,bh-1), yp2=std::min(y+2,bh-1);
                for (int x = 0; x < bw; ++x) {
                    const float* a=T+(size_t(ym2)*bw+x)*3; const float* b=T+(size_t(ym1)*bw+x)*3;
                    const float* c0=T+(size_t(y)*bw+x)*3;   const float* d0=T+(size_t(yp1)*bw+x)*3;
                    const float* e=T+(size_t(yp2)*bw+x)*3;  float* o=A+(size_t(y)*bw+x)*3;
                    for (int c=0;c<3;++c) o[c]=a[c]*gw2+b[c]*gw1+c0[c]*gwc+d0[c]*gw1+e[c]*gw2;
                }
            }
        });
    }

    // 3. Bilinear upsample + add intensity*bloom back into g_hdrBuf (pre-tonemap,
    //    so the glow rolls off through ACES). Each job writes disjoint rows.
    hdrf* const hw = g_hdrBuf.data();
    hdrDispatchRows(H, [=](int y1, int y2) {
        for (int y = y1; y < y2; ++y) {
            const float fy = (float(y) + 0.5f) / float(DS) - 0.5f;
            int y0 = int(std::floor(fy)); float wy = fy - float(y0);
            if (y0 < 0) { y0 = 0; wy = 0; } if (y0 >= bh - 1) { y0 = bh > 1 ? bh - 2 : 0; wy = bh > 1 ? 1.0f : 0.0f; }
            const int y0b = std::min(y0 + 1, bh - 1);
            hdrf* row = hw + size_t(y) * size_t(W) * 4;
            for (int x = 0; x < W; ++x) {
                const float fx = (float(x) + 0.5f) / float(DS) - 0.5f;
                int x0 = int(std::floor(fx)); float wx = fx - float(x0);
                if (x0 < 0) { x0 = 0; wx = 0; } if (x0 >= bw - 1) { x0 = bw > 1 ? bw - 2 : 0; wx = bw > 1 ? 1.0f : 0.0f; }
                const int x0b = std::min(x0 + 1, bw - 1);
                const float w00=(1-wx)*(1-wy), w10=wx*(1-wy), w01=(1-wx)*wy, w11=wx*wy;
                const float* a00=A+(size_t(y0 )*bw+x0 )*3; const float* a10=A+(size_t(y0 )*bw+x0b)*3;
                const float* a01=A+(size_t(y0b)*bw+x0 )*3; const float* a11=A+(size_t(y0b)*bw+x0b)*3;
                for (int c = 0; c < 3; ++c)
                    row[x*4+c] = HdrClamp(float(row[x*4+c]) + (a00[c]*w00 + a10[c]*w10 + a01[c]*w01 + a11[c]*w11) * intensity);
            }
        }
    });
}

// Anamorphic lens streaks (Star Trek 2009 look). Bright-pass the linear
// g_hdrBuf, build a long HORIZONTAL streak (+ a shorter faint VERTICAL cross)
// off every hot source via exponential doubling-blur at quarter-res, tint cool
// blue-white, and add back BEFORE the tonemap so it rolls off through ACES.
// Independent of --bloom (own bright-pass at bloom_threshold).
void Render_AnamorphicPass() {
    if (!FeatureFlags::anamorphic() || !g_hdrActive) return;
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const int W = rt.xres, H = rt.yres;
    const size_t px = size_t(W) * size_t(H);
    if (px == 0 || g_hdrBuf.size() < px * 4) return;
    const float intensity = FeatureFlags::anamorphic_intensity();
    if (intensity <= 0.0f) return;
    const float thresh  = FeatureFlags::bloom_threshold();
    const int   passesH = std::max(0, FeatureFlags::anamorphic_passes());
    const float decay   = FeatureFlags::anamorphic_decay();
    const float vert    = FeatureFlags::anamorphic_vert();

    constexpr int DS = 4;                              // quarter-res like bloom
    const int bw = (W + DS - 1) / DS, bh = (H + DS - 1) / DS;
    const size_t n = size_t(bw) * size_t(bh) * 3;
    static std::vector<float> s_br, s_h, s_v, s_tmp;
    s_br.assign(n, 0.0f);
    s_h.assign(n, 0.0f);
    s_v.assign(vert > 0.0f ? n : 0, 0.0f);
    s_tmp.assign(n, 0.0f);
    const hdrf* const hb = g_hdrBuf.data();
    float* const BR = s_br.data();

    // 1. Bright-pass + downsample (same soft knee as bloom) — SHARED.
    getBrightPass(BR, bw, bh, W, H, hb, thresh);

    // 2. Directional exponential streak via offset-doubling (length ~2^passes
    //    quarter-res px). Each pass: out[x] = in[x] + decay*(in[x-s] + in[x+s]),
    //    s = 1,2,4,... Ping-pong cur/tmp. The vertical uses fewer passes so it
    //    stays a short faint cross rather than matching the horizontal length.
    auto buildStreak = [&](float* out, bool horiz, int passes) {
        std::copy(BR, BR + n, out);
        float* cur = out;
        float* tmp = s_tmp.data();
        int step = 1;
        for (int i = 0; i < passes; ++i) {
            const int   s   = step;
            const float w   = decay;
            const float* src = cur;
            float* dst = tmp;
            const int lbw = bw, lbh = bh;
            hdrDispatchRows(bh, [src, dst, s, w, horiz, lbw, lbh](int y1, int y2) {
                for (int y = y1; y < y2; ++y)
                    for (int x = 0; x < lbw; ++x) {
                        const float* c = src + (size_t(y) * lbw + x) * 3;
                        const float* a; const float* b;
                        if (horiz) {
                            const int xm = x - s < 0 ? 0 : x - s, xp = x + s >= lbw ? lbw - 1 : x + s;
                            a = src + (size_t(y) * lbw + xm) * 3;
                            b = src + (size_t(y) * lbw + xp) * 3;
                        } else {
                            const int ym = y - s < 0 ? 0 : y - s, yp = y + s >= lbh ? lbh - 1 : y + s;
                            a = src + (size_t(ym) * lbw + x) * 3;
                            b = src + (size_t(yp) * lbw + x) * 3;
                        }
                        float* d = dst + (size_t(y) * lbw + x) * 3;
                        for (int k = 0; k < 3; ++k) d[k] = c[k] + w * (a[k] + b[k]);
                    }
            });
            std::swap(cur, tmp);
            step <<= 1;
        }
        if (cur != out) std::copy(cur, cur + n, out);
    };
    buildStreak(s_h.data(), /*horiz=*/true, passesH);
    if (vert > 0.0f) buildStreak(s_v.data(), /*horiz=*/false, std::max(0, passesH - 2));

    // 3. Bilinear upsample + tinted add into g_hdrBuf (pre-tonemap). Cool
    //    blue-white streak tint (B>G>R) for the JJ-Abrams cast.
    const float tB = 1.0f, tG = 0.9f, tR = 0.7f;
    hdrf* const hw = g_hdrBuf.data();
    const float* const SH = s_h.data();
    const float* const SV = (vert > 0.0f) ? s_v.data() : nullptr;
    hdrDispatchRows(H, [=](int y1, int y2) {
        for (int y = y1; y < y2; ++y) {
            const float fy = (float(y) + 0.5f) / float(DS) - 0.5f;
            int y0 = int(std::floor(fy)); float wy = fy - float(y0);
            if (y0 < 0) { y0 = 0; wy = 0; } if (y0 >= bh - 1) { y0 = bh > 1 ? bh - 2 : 0; wy = bh > 1 ? 1.0f : 0.0f; }
            const int y0b = std::min(y0 + 1, bh - 1);
            hdrf* row = hw + size_t(y) * size_t(W) * 4;
            for (int x = 0; x < W; ++x) {
                const float fx = (float(x) + 0.5f) / float(DS) - 0.5f;
                int x0 = int(std::floor(fx)); float wx = fx - float(x0);
                if (x0 < 0) { x0 = 0; wx = 0; } if (x0 >= bw - 1) { x0 = bw > 1 ? bw - 2 : 0; wx = bw > 1 ? 1.0f : 0.0f; }
                const int x0b = std::min(x0 + 1, bw - 1);
                const float w00=(1-wx)*(1-wy), w10=wx*(1-wy), w01=(1-wx)*wy, w11=wx*wy;
                auto samp = [&](const float* S, int c) {
                    return S[(size_t(y0 )*bw+x0 )*3+c]*w00 + S[(size_t(y0 )*bw+x0b)*3+c]*w10
                         + S[(size_t(y0b)*bw+x0 )*3+c]*w01 + S[(size_t(y0b)*bw+x0b)*3+c]*w11;
                };
                float b = samp(SH,0), g = samp(SH,1), r = samp(SH,2);
                if (SV) { b += samp(SV,0)*vert; g += samp(SV,1)*vert; r += samp(SV,2)*vert; }
                row[x*4+0] = HdrClamp(float(row[x*4+0]) + b * tB * intensity);
                row[x*4+1] = HdrClamp(float(row[x*4+1]) + g * tG * intensity);
                row[x*4+2] = HdrClamp(float(row[x*4+2]) + r * tR * intensity);
            }
        }
    });
}

// Depth-of-field: circle-of-confusion bokeh blur off the G-buffer depth.
// Applied to the linear g_hdrBuf BEFORE bloom so defocused highlights bloom
// into discs. Gather blur over a golden-angle unit disc scaled by each pixel's
// CoC; "scatter-as-gather" weighting (a tap contributes only if its OWN CoC is
// large enough to reach the centre) keeps sharp foreground from bleeding into
// blurred background. In-focus pixels (CoC < ~1px) are left untouched. No-op
// unless --dof && g_hdrActive.
void Render_DoFPass() {
    if (!FeatureFlags::dof() || !g_hdrActive) return;
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const int W = rt.xres, H = rt.yres;
    const size_t px = size_t(W) * size_t(H);
    if (px == 0 || g_hdrBuf.size() < px * 4 || !rt.zpage16) return;
    // dof_max is specified at a 540p reference height; scale to the actual
    // resolution so the perceptual blur is consistent (a fixed pixel radius
    // looks half as strong at 1080p as at 540p — the "not seeing it" trap).
    const float maxR = FeatureFlags::dof_max() * (float(rt.yres) / 540.0f);
    if (maxR < 0.75f) return;
    // dof_range is a fraction of the scene far-plane → absolute view-z units,
    // so a single default works whether the scene is a small room or a vista.
    const float fzp = (CurScene && CurScene->FZP > 0.0f) ? CurScene->FZP : 1000.0f;
    const float range = std::max(1e-3f, FeatureFlags::dof_range() * fzp);
    const float invZScale = (g_zscale != 0.0f) ? 1.0f / g_zscale : 1.0f;
    const uint16_t* const Z = rt.zpage16;
    auto viewZ = [=](size_t i) -> float {
        const int enc = int(Z[i]);
        if (enc == 0) return 3.0e9f;          // no geometry → treat as far (sky)
        return float(0xFF80 - enc) * invZScale;
    };

    // Focus distance: explicit (dof_focus), or auto. Auto prefers the camera
    // track's look-at TARGET (View->ITarget — the point the camera spline is
    // aimed at, i.e. the intended subject), projected onto the view forward
    // axis (Mat row 2) to get camera-space z in the same units as viewZ. Falls
    // back to the average depth of a small screen-centre window (skipping sky)
    // when there's no usable target (free-look scenes, target behind the eye).
    float focus = FeatureFlags::dof_focus();
    if (focus <= 0.0f) {
        focus = 0.0f;
        if (View) {
            const float dxt = View->ITarget.x - View->ISource.x;
            const float dyt = View->ITarget.y - View->ISource.y;
            const float dzt = View->ITarget.z - View->ISource.z;
            const float fz = dxt * View->Mat[2][0] + dyt * View->Mat[2][1] + dzt * View->Mat[2][2];
            if (fz > 1.0f && fz < fzp) focus = fz;       // target in front, within the scene
        }
        if (focus <= 0.0f) {                              // no usable target → centre-window depth
            double acc = 0.0; int n = 0;
            const int cx = W / 2, cy = H / 2, R = std::max(2, std::min(W, H) / 40);
            for (int dy = -R; dy <= R; ++dy) {
                const int yy = cy + dy; if (yy < 0 || yy >= H) continue;
                for (int dx = -R; dx <= R; ++dx) {
                    const int xx = cx + dx; if (xx < 0 || xx >= W) continue;
                    const float z = viewZ(size_t(yy) * W + xx);
                    if (z < 2.0e9f) { acc += z; ++n; }
                }
            }
            if (n == 0) return;
            focus = float(acc / double(n));
        }
    }

    // Golden-angle unit-disc tap offsets — deterministic (no RNG, banned in scripts).
    constexpr int TAPS = 24;
    static float s_ox[TAPS], s_oy[TAPS], s_or[TAPS];
    static bool s_init = false;
    if (!s_init) {
        const float GA = 2.39996323f;
        for (int t = 0; t < TAPS; ++t) {
            const float r = std::sqrt((float(t) + 0.5f) / float(TAPS));
            const float a = float(t) * GA;
            s_ox[t] = r * std::cos(a); s_oy[t] = r * std::sin(a); s_or[t] = r;
        }
        s_init = true;
    }

    auto cocAt = [=](size_t i) -> float {
        float c = std::fabs(viewZ(i) - focus) / range; if (c > 1.0f) c = 1.0f;
        return c * maxR;
    };

    // ── Reduced-res path (--dof_downscale > 1, same idea as --ssao_downscale):
    // gather at W/d x H/d (~1/d² the taps, and skips the full-res f16→f32
    // source copy the full-res path pays), then a 4-tap depth-aware bilinear
    // upsample. Defocused content is blurry by definition, so the res cut is
    // invisible except near the focus boundary — where CoC (and thus the blur)
    // is small — so the composite blends toward the sharp original as CoC → the
    // in-focus threshold and the transition band stays clean.
    const int down = std::max(1, std::min(4, FeatureFlags::dof_downscale()));
    if (down > 1) {
        const int lowW = (W + down - 1) / down, lowH = (H + down - 1) / down;
        const size_t lpx = size_t(lowW) * size_t(lowH);
        static std::vector<float> s_lowS;   // pre-blur colour (B,G,R)
        static std::vector<float> s_lowC;   // blurred colour  (B,G,R)
        static std::vector<float> s_lowZ;   // view-z of the sampled texel
        static std::vector<float> s_lowCoc; // CoC in FULL-res pixel units
        s_lowS.resize(lpx * 3); s_lowC.resize(lpx * 3);
        s_lowZ.resize(lpx);     s_lowCoc.resize(lpx);
        float* const lowS = s_lowS.data(); float* const lowC = s_lowC.data();
        float* const lowZ = s_lowZ.data(); float* const lowCoc = s_lowCoc.data();
        const hdrf* const HB = g_hdrBuf.data();
        const int half = down / 2;

        // Pass A: point-sample each cell's centre pixel (colour + z + CoC).
        // No box prefilter — the gather blur is itself a low-pass.
        hdrDispatchRows(lowH, [=](int y1, int y2) {
            for (int ly = y1; ly < y2; ++ly) {
                for (int lx = 0; lx < lowW; ++lx) {
                    const int fx = std::min(W - 1, lx * down + half);
                    const int fy = std::min(H - 1, ly * down + half);
                    const size_t fi = size_t(fy) * size_t(W) + size_t(fx);
                    const size_t li = size_t(ly) * size_t(lowW) + size_t(lx);
                    lowS[li*3+0] = float(HB[fi*4+0]);
                    lowS[li*3+1] = float(HB[fi*4+1]);
                    lowS[li*3+2] = float(HB[fi*4+2]);
                    const float z = viewZ(fi);
                    lowZ[li] = z;
                    float c = std::fabs(z - focus) / range; if (c > 1.0f) c = 1.0f;
                    lowCoc[li] = c * maxR;
                }
            }
        });

        // Pass B: golden-angle gather at low res. CoC and tap distances stay
        // in full-res pixel units (so scatter-as-gather weighting is
        // unchanged); only the tap OFFSETS are divided into low-res texels.
        const float invDown = 1.0f / float(down);
        hdrDispatchRows(lowH, [=](int y1, int y2) {
            for (int ly = y1; ly < y2; ++ly) {
                for (int lx = 0; lx < lowW; ++lx) {
                    const size_t li = size_t(ly) * size_t(lowW) + size_t(lx);
                    const float coc = lowCoc[li];
                    if (coc < 0.75f) {
                        lowC[li*3+0] = lowS[li*3+0];
                        lowC[li*3+1] = lowS[li*3+1];
                        lowC[li*3+2] = lowS[li*3+2];
                        continue;
                    }
                    float sb = lowS[li*3+0], sg = lowS[li*3+1], sr = lowS[li*3+2], wsum = 1.0f;
                    const float cocLow = coc * invDown;
                    for (int t = 0; t < TAPS; ++t) {
                        const int sx = int(float(lx) + s_ox[t] * cocLow + 0.5f);
                        const int sy = int(float(ly) + s_oy[t] * cocLow + 0.5f);
                        if (sx < 0 || sx >= lowW || sy < 0 || sy >= lowH) continue;
                        const size_t si = size_t(sy) * size_t(lowW) + size_t(sx);
                        const float d = s_or[t] * coc;        // full-res px
                        float w = lowCoc[si] - d + 1.0f;
                        if (w <= 0.0f) continue; if (w > 1.0f) w = 1.0f;
                        sb += lowS[si*3+0] * w; sg += lowS[si*3+1] * w; sr += lowS[si*3+2] * w;
                        wsum += w;
                    }
                    const float inv = 1.0f / wsum;
                    lowC[li*3+0] = sb * inv; lowC[li*3+1] = sg * inv; lowC[li*3+2] = sr * inv;
                }
            }
        });

        // Pass C: full-res composite. 4-tap depth-aware bilinear from the
        // blurred low-res field, then lerp(sharp, blur, blend) where blend
        // ramps 0→1 over CoC 0.75..2.0 px.
        hdrf* const DSTh = g_hdrBuf.data();
        const float depthSig = range * 0.5f;
        const float invDepthK = 1.0f / (4.0f * depthSig * depthSig);
        hdrDispatchRows(H, [=](int y1, int y2) {
            for (int y = y1; y < y2; ++y) {
                for (int x = 0; x < W; ++x) {
                    const size_t i = size_t(y) * size_t(W) + size_t(x);
                    const float coc = cocAt(i);
                    if (coc < 0.75f) continue;                // in focus → untouched
                    const float zf = viewZ(i);
                    const float gx = (float(x) - float(half)) * invDown;
                    const float gy = (float(y) - float(half)) * invDown;
                    int x0 = (int)std::floor(gx), y0 = (int)std::floor(gy);
                    const float fxs = gx - float(x0), fys = gy - float(y0);
                    int x1c = x0 + 1, y1c = y0 + 1;
                    x0  = std::max(0, std::min(lowW - 1, x0));  x1c = std::max(0, std::min(lowW - 1, x1c));
                    y0  = std::max(0, std::min(lowH - 1, y0));  y1c = std::max(0, std::min(lowH - 1, y1c));
                    const size_t o00 = size_t(y0 )*lowW + x0, o10 = size_t(y0 )*lowW + x1c;
                    const size_t o01 = size_t(y1c)*lowW + x0, o11 = size_t(y1c)*lowW + x1c;
                    const float bw00 = (1-fxs)*(1-fys), bw10 = fxs*(1-fys);
                    const float bw01 = (1-fxs)*fys,     bw11 = fxs*fys;
                    float ab = 0, ag = 0, ar = 0, wsum = 0;
                    #define DOF_TAP(O, BW) { const float dz = zf - lowZ[O]; \
                        float wd = 1.0f - dz*dz*invDepthK; \
                        if (wd > 0.0f) { const float w = (BW)*wd; \
                            ab += lowC[(O)*3+0]*w; ag += lowC[(O)*3+1]*w; ar += lowC[(O)*3+2]*w; wsum += w; } }
                    DOF_TAP(o00, bw00) DOF_TAP(o10, bw10)
                    DOF_TAP(o01, bw01) DOF_TAP(o11, bw11)
                    #undef DOF_TAP
                    if (wsum <= 1e-6f) continue;              // no depth-compatible texel → keep sharp
                    const float inv = 1.0f / wsum;
                    float blend = (coc - 0.75f) * 0.8f;       // 0 at 0.75px, 1 at 2px
                    if (blend > 1.0f) blend = 1.0f;
                    const float keep = 1.0f - blend;
                    hdrf* h = DSTh + i * 4;
                    h[0] = float(h[0]) * keep + ab * inv * blend;
                    h[1] = float(h[1]) * keep + ag * inv * blend;
                    h[2] = float(h[2]) * keep + ar * inv * blend;
                }
            }
        });
        return;
    }

    static std::vector<float> s_src;
    s_src.assign(g_hdrBuf.begin(), g_hdrBuf.begin() + px * 4);
    const float* const SRC = s_src.data();
    hdrf* const DST = g_hdrBuf.data();

    hdrDispatchRows(H, [=](int y1, int y2) {
        for (int y = y1; y < y2; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                const float coc = cocAt(i);
                if (coc < 0.75f) continue;               // in focus → keep DST (== SRC)
                float sb = SRC[i*4+0], sg = SRC[i*4+1], sr = SRC[i*4+2], wsum = 1.0f;
                for (int t = 0; t < TAPS; ++t) {
                    const int sx = int(float(x) + s_ox[t] * coc + 0.5f);
                    const int sy = int(float(y) + s_oy[t] * coc + 0.5f);
                    if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
                    const size_t si = size_t(sy) * size_t(W) + size_t(sx);
                    const float d = s_or[t] * coc;        // tap distance from centre (px)
                    float w = cocAt(si) - d + 1.0f;       // tap reaches centre iff its CoC >= d
                    if (w <= 0.0f) continue; if (w > 1.0f) w = 1.0f;
                    sb += SRC[si*4+0] * w; sg += SRC[si*4+1] * w; sr += SRC[si*4+2] * w;
                    wsum += w;
                }
                const float inv = 1.0f / wsum;
                DST[i*4+0] = sb * inv; DST[i*4+1] = sg * inv; DST[i*4+2] = sr * inv;
            }
        }
    });
}

// Screen-space lens-flare ghosts. From the bright-pass, march a chain of
// discs (+ a halo ring) along the line through screen-centre, mirrored from
// each bright source, with a subtle per-channel radial chroma split. Added to
// g_hdrBuf BEFORE the tonemap (like bloom/anamorphic). Reuses the same
// quarter-res bright-pass; independent toggle.
void Render_LensGhostPass() {
    if (!FeatureFlags::lens_ghosts() || !g_hdrActive) return;
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const int W = rt.xres, H = rt.yres;
    const size_t px = size_t(W) * size_t(H);
    if (px == 0 || g_hdrBuf.size() < px * 4) return;
    const float intensity = FeatureFlags::lens_ghost_intensity();
    if (intensity <= 0.0f) return;
    const float thresh = FeatureFlags::bloom_threshold();
    const int   count  = std::max(0, FeatureFlags::lens_ghost_count());
    const float disp   = FeatureFlags::lens_ghost_dispersal();
    const float halo   = FeatureFlags::lens_ghost_halo();

    constexpr int DS = 4;
    const int bw = (W + DS - 1) / DS, bh = (H + DS - 1) / DS;
    static std::vector<float> s_br;
    s_br.assign(size_t(bw) * size_t(bh) * 3, 0.0f);
    const hdrf* const hb = g_hdrBuf.data();
    float* const BR = s_br.data();
    // Bright-pass — SHARED (computed once/frame by Render_HdrBrightPass). With
    // count=0 the ghost chain below is a no-op; the shared pass means this pass
    // no longer pays its own full-HDR downsample just for the halo.
    getBrightPass(BR, bw, bh, W, H, hb, thresh);

    hdrf* const hw = g_hdrBuf.data();
    hdrDispatchRows(H, [=](int y1, int y2) {
        auto sampleBR = [&](float u, float v, int c) -> float {
            if (u < 0 || u > 1 || v < 0 || v > 1) return 0.0f;
            float fx = u * bw - 0.5f, fy = v * bh - 0.5f;
            int x0 = int(std::floor(fx)); float wx = fx - x0;
            int y0 = int(std::floor(fy)); float wy = fy - y0;
            if (x0 < 0) { x0 = 0; wx = 0; } if (x0 >= bw - 1) { x0 = bw > 1 ? bw - 2 : 0; wx = bw > 1 ? 1 : 0; }
            if (y0 < 0) { y0 = 0; wy = 0; } if (y0 >= bh - 1) { y0 = bh > 1 ? bh - 2 : 0; wy = bh > 1 ? 1 : 0; }
            const int x1 = std::min(x0 + 1, bw - 1), y1b = std::min(y0 + 1, bh - 1);
            const float* a = BR + (size_t(y0 )*bw + x0)*3; const float* b = BR + (size_t(y0 )*bw + x1)*3;
            const float* d = BR + (size_t(y1b)*bw + x0)*3; const float* e = BR + (size_t(y1b)*bw + x1)*3;
            return (a[c]*(1-wx) + b[c]*wx)*(1-wy) + (d[c]*(1-wx) + e[c]*wx)*wy;
        };
        const float ca = 0.012f;          // per-channel chroma split for the fringe
        for (int y = y1; y < y2; ++y) {
            const float v = (float(y) + 0.5f) / float(H);
            hdrf* row = hw + size_t(y) * size_t(W) * 4;
            for (int x = 0; x < W; ++x) {
                const float u = (float(x) + 0.5f) / float(W);
                // Mirror the sample origin through centre: ghosts appear on the
                // far side of centre from each source.
                const float tu = 1.0f - u, tv = 1.0f - v;
                const float gx = 0.5f - tu, gy = 0.5f - tv;
                float sb = 0, sg = 0, sr = 0;
                for (int i = 1; i <= count; ++i) {
                    const float t = disp * float(i);
                    // distance of this ghost from centre → bright-near-centre falloff
                    const float ox = tu + gx * t, oy = tv + gy * t;
                    float dc = std::sqrt((0.5f - ox)*(0.5f - ox) + (0.5f - oy)*(0.5f - oy)) * 2.0f;
                    float wgt = 1.0f - dc; if (wgt < 0) wgt = 0; wgt = wgt * wgt * wgt;
                    if (wgt <= 0) continue;
                    sr += sampleBR(tu + gx * t * (1 + ca), tv + gy * t * (1 + ca), 2) * wgt;
                    sg += sampleBR(ox, oy, 1) * wgt;
                    sb += sampleBR(tu + gx * t * (1 - ca), tv + gy * t * (1 - ca), 0) * wgt;
                }
                if (halo > 0.0f) {
                    const float len = std::sqrt(gx*gx + gy*gy) + 1e-5f;
                    const float nx = gx / len, ny = gy / len;
                    const float hr = 0.42f;   // halo radius (uv)
                    const float ox = tu + nx * hr, oy = tv + ny * hr;
                    float dc = std::sqrt((0.5f - ox)*(0.5f - ox) + (0.5f - oy)*(0.5f - oy));
                    float wgt = 1.0f - std::fabs(dc - hr * 0.5f) * 4.0f; if (wgt < 0) wgt = 0;
                    wgt *= halo;
                    if (wgt > 0) {
                        sr += sampleBR(tu + nx * hr * (1 + ca*2), tv + ny * hr * (1 + ca*2), 2) * wgt;
                        sg += sampleBR(ox, oy, 1) * wgt;
                        sb += sampleBR(tu + nx * hr * (1 - ca*2), tv + ny * hr * (1 - ca*2), 0) * wgt;
                    }
                }
                row[x*4+0] = HdrClamp(float(row[x*4+0]) + sb * intensity);
                row[x*4+1] = HdrClamp(float(row[x*4+1]) + sg * intensity);
                row[x*4+2] = HdrClamp(float(row[x*4+2]) + sr * intensity);
            }
        }
    });
}

// Post-tonemap lens "glass": radial chromatic aberration (R outward / B inward,
// growing toward the corner) + vignette. Operates on the final 8-bit VPage
// (BGRA: byte0=B, byte1=G, byte2=R). Call AFTER Render_TonemapToVPage, only on
// the main view (not the mirror RTT).
void Render_LensPostPass() {
    const bool doCA = FeatureFlags::chromatic();
    const bool doVG = FeatureFlags::vignette();
    if (!doCA && !doVG) return;
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const int W = rt.xres, H = rt.yres;
    if (W <= 0 || H <= 0 || !rt.vpage) return;
    dword* const vp = reinterpret_cast<dword*>(rt.vpage);
    const float caAmt = doCA ? FeatureFlags::chromatic_amount() : 0.0f;
    const float vgStr = doVG ? FeatureFlags::vignette_strength() : 0.0f;
    const float cx = W * 0.5f, cy = H * 0.5f;
    const float invMaxR = 1.0f / std::sqrt(cx*cx + cy*cy);
    // CA reads offset neighbours → needs an unmodified source copy.
    static std::vector<dword> s_src;
    const dword* src = nullptr;
    if (doCA) { s_src.assign(size_t(W) * size_t(H), 0u); std::copy(vp, vp + size_t(W)*size_t(H), s_src.data()); src = s_src.data(); }
    hdrDispatchRows(H, [=](int y1, int y2) {
        for (int y = y1; y < y2; ++y) {
            dword* row = vp + size_t(y) * size_t(W);
            for (int x = 0; x < W; ++x) {
                const float dx = float(x) - cx, dy = float(y) - cy;
                const float dist = std::sqrt(dx*dx + dy*dy);
                const float r = dist * invMaxR;       // 0 centre .. 1 corner
                int B, G, R;
                if (doCA) {
                    const float ux = dx / (dist + 1e-3f), uy = dy / (dist + 1e-3f);
                    const float off = caAmt * r;
                    auto samp = [&](float fx, float fy, int sh) -> int {
                        int sx = int(fx + 0.5f), sy = int(fy + 0.5f);
                        if (sx < 0) sx = 0; if (sx >= W) sx = W - 1;
                        if (sy < 0) sy = 0; if (sy >= H) sy = H - 1;
                        return int((src[size_t(sy) * W + sx] >> sh) & 0xFFu);
                    };
                    B = samp(x - ux * off, y - uy * off, 0);
                    G = samp(float(x),     float(y),     8);
                    R = samp(x + ux * off, y + uy * off, 16);
                } else {
                    const dword p = row[x];
                    B = int(p & 0xFFu); G = int((p >> 8) & 0xFFu); R = int((p >> 16) & 0xFFu);
                }
                if (doVG) {
                    float vf = 1.0f - vgStr * (r * r); if (vf < 0) vf = 0;
                    B = int(B * vf); G = int(G * vf); R = int(R * vf);
                }
                row[x] = dword(B) | (dword(G) << 8) | (dword(R) << 16) | 0xFF000000u;
            }
        }
    });
}

// Post-tonemap colour grade + film grain — the "film stock" tier, on the final
// 8-bit VPage (after lens-post). Grade: temperature, contrast (around mid), HSL
// saturation, and a teal-orange split-tone (shadows → teal, highlights → orange,
// the blockbuster look). Grain: animated per-pixel luminance noise (hashed on
// x,y,frame). No-op unless --grade / --grain. Threaded.
void Render_GradeGrainPass() {
    const bool doGrade = FeatureFlags::grade();
    const bool doGrain = FeatureFlags::grain();
    if (!doGrade && !doGrain) return;
    const RenderTarget rt = MainRenderTargetFromGlobals();
    const int W = rt.xres, H = rt.yres;
    if (W <= 0 || H <= 0 || !rt.vpage) return;
    dword* const vp = reinterpret_cast<dword*>(rt.vpage);
    const float teal = doGrade ? FeatureFlags::grade_teal_orange() : 0.0f;
    const float con  = doGrade ? FeatureFlags::grade_contrast()    : 1.0f;
    const float sat  = doGrade ? FeatureFlags::grade_saturation()  : 1.0f;
    const float temp = doGrade ? FeatureFlags::grade_temp()        : 0.0f;
    const float grainAmt = doGrain ? FeatureFlags::grain_strength() : 0.0f;
    static unsigned s_frame = 0;
    const unsigned frame = doGrain ? s_frame++ : 0u;     // advances → grain animates live
    hdrDispatchRows(H, [=](int y1, int y2) {
        auto clamp255 = [](float v) -> int { int i = int(v + 0.5f); return i < 0 ? 0 : (i > 255 ? 255 : i); };
        for (int y = y1; y < y2; ++y) {
            dword* row = vp + size_t(y) * size_t(W);
            for (int x = 0; x < W; ++x) {
                const dword p = row[x];
                float R = float((p >> 16) & 0xFFu), G = float((p >> 8) & 0xFFu), B = float(p & 0xFFu);
                if (doGrade) {
                    float r = R * (1.0f/255.0f), g = G * (1.0f/255.0f), b = B * (1.0f/255.0f);
                    r += temp * 0.1f; b -= temp * 0.1f;                          // temperature
                    r = (r - 0.5f) * con + 0.5f; g = (g - 0.5f) * con + 0.5f; b = (b - 0.5f) * con + 0.5f; // contrast
                    const float luma = 0.299f*r + 0.587f*g + 0.114f*b;
                    r = luma + (r - luma) * sat; g = luma + (g - luma) * sat; b = luma + (b - luma) * sat;  // saturation
                    const float t = (luma - 0.5f) * 2.0f;                        // -1 shadow .. +1 highlight
                    const float wH = teal * (t > 0.0f ? t : 0.0f);               // highlight → orange
                    const float wS = teal * (t < 0.0f ? -t : 0.0f);              // shadow → teal
                    r += wH * 0.18f - wS * 0.12f;
                    g += wH * 0.07f + wS * 0.04f;
                    b += -wH * 0.16f + wS * 0.14f;
                    R = r * 255.0f; G = g * 255.0f; B = b * 255.0f;
                }
                if (doGrain) {
                    unsigned h = (unsigned(x) * 73856093u) ^ (unsigned(y) * 19349663u) ^ (frame * 83492791u);
                    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
                    const float n = (float(h & 0xFFFFu) * (1.0f/65535.0f) - 0.5f) * 2.0f;
                    const float ofs = n * grainAmt;
                    R += ofs; G += ofs; B += ofs;
                }
                row[x] = dword(clamp255(B)) | (dword(clamp255(G)) << 8) | (dword(clamp255(R)) << 16) | 0xFF000000u;
            }
        }
    });
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
    const hdrf* const hbuf = g_hdrBuf.data();
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
            const hdrf* h   = hbuf + size_t(y) * size_t(W) * 4;
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
    dispatchIndexed(numTilesX * numTilesY, &renderns::tileDone,
        [&tileBody, tsx, tsy, W, H](int t) {
            const int j = t / numTilesX, i = t - j * numTilesX;
            const int y1 = tsy * j, y2 = std::min(y1 + tsy, H);
            const int x1 = tsx * i, x2 = std::min(x1 + tsx, W);
            tileBody(x1, y1, x2, y2);
        });
    for (int n = numTilesX * numTilesY, k = 0; k < n; ++k) renderns::tileDone.acquire();
}

} // namespace fds
