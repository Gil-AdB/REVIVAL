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

// Shared row-band tile dispatch (no write overlap: each job owns a row band of
// the TARGET buffer). body(y1,y2) processes rows [y1,y2). Callers run on the
// tick thread, not a pool worker, so enqueue+wait can't deadlock.
template <class Body>
static void hdrDispatchRows(int rows, Body&& body) {
    constexpr int kBands = 24;
    const int band = (rows + kBands - 1) / kBands;
    if (band <= 0) return;
    int jobs = 0;
    for (int y = 0; y < rows; y += band) {
        const int y2 = std::min(y + band, rows);
        ThreadPool::instance().enqueue([=]() { body(y, y2); renderns::tileDone.release(); });
        ++jobs;
    }
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
    float* const          hbuf  = g_hdrBuf.data();
    hdrDispatchRows(rt.yres, [=](int y1, int y2) {
        for (int y = y1; y < y2; ++y) {
            const uint32_t* row = vpage + size_t(y) * size_t(stride);
            float*          h   = hbuf  + size_t(y) * size_t(xres) * 4;
            const uint32_t* mrow = boostOK ? mat32 + size_t(y) * size_t(xres) : nullptr;
            for (int x = 0; x < xres; ++x) {
                if (h[x*4+3] != 0.0f) continue;   // covered: kernel already wrote radiance
                const uint32_t p = row[x];
                float b = float(p & 0xFFu), g = float((p >> 8) & 0xFFu), r = float((p >> 16) & 0xFFu);
                if (linear) { b = b*b*kInv; g = g*g*kInv; r = r*r*kInv; }  // gamma-2.0 → linear scale
                if (mrow && mrow[x] == 0xFFFFFFFEu) { b *= gain; g *= gain; r *= gain; }
                h[x*4+0] = b; h[x*4+1] = g; h[x*4+2] = r;
            }
        }
    });
    g_hdrActive = true;
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
    const float* const hb = g_hdrBuf.data();
    float* const A = s_acc.data();
    float* const T = s_tmp.data();

    // 1. Bright-pass + downsample: each quarter-res cell = mean over its DS×DS
    //    source block of the radiance ABOVE the knee (per-channel, soft so the
    //    colour is preserved and the transition isn't hard). Reads the linear
    //    g_hdrBuf — the true unclamped source.
    hdrDispatchRows(bh, [=](int by1, int by2) {
        const float inv = 1.0f / float(DS * DS);
        for (int by = by1; by < by2; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                float sb = 0, sg = 0, sr = 0;
                for (int dy = 0; dy < DS; ++dy) {
                    const int sy = by * DS + dy; if (sy >= H) break;
                    const float* h = hb + size_t(sy) * size_t(W) * 4;
                    for (int dx = 0; dx < DS; ++dx) {
                        const int sx = bx * DS + dx; if (sx >= W) break;
                        const float B = h[sx*4+0], G = h[sx*4+1], R = h[sx*4+2];
                        const float lum = R > G ? (R > B ? R : B) : (G > B ? G : B);
                        if (lum > thresh) { const float w = (lum - thresh) / lum; sb += B*w; sg += G*w; sr += R*w; }
                    }
                }
                float* a = A + (size_t(by) * bw + bx) * 3;
                a[0] = sb * inv; a[1] = sg * inv; a[2] = sr * inv;
            }
    });

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
    float* const hw = g_hdrBuf.data();
    hdrDispatchRows(H, [=](int y1, int y2) {
        for (int y = y1; y < y2; ++y) {
            const float fy = (float(y) + 0.5f) / float(DS) - 0.5f;
            int y0 = int(std::floor(fy)); float wy = fy - float(y0);
            if (y0 < 0) { y0 = 0; wy = 0; } if (y0 >= bh - 1) { y0 = bh > 1 ? bh - 2 : 0; wy = bh > 1 ? 1.0f : 0.0f; }
            const int y0b = std::min(y0 + 1, bh - 1);
            float* row = hw + size_t(y) * size_t(W) * 4;
            for (int x = 0; x < W; ++x) {
                const float fx = (float(x) + 0.5f) / float(DS) - 0.5f;
                int x0 = int(std::floor(fx)); float wx = fx - float(x0);
                if (x0 < 0) { x0 = 0; wx = 0; } if (x0 >= bw - 1) { x0 = bw > 1 ? bw - 2 : 0; wx = bw > 1 ? 1.0f : 0.0f; }
                const int x0b = std::min(x0 + 1, bw - 1);
                const float w00=(1-wx)*(1-wy), w10=wx*(1-wy), w01=(1-wx)*wy, w11=wx*wy;
                const float* a00=A+(size_t(y0 )*bw+x0 )*3; const float* a10=A+(size_t(y0 )*bw+x0b)*3;
                const float* a01=A+(size_t(y0b)*bw+x0 )*3; const float* a11=A+(size_t(y0b)*bw+x0b)*3;
                for (int c = 0; c < 3; ++c)
                    row[x*4+c] += (a00[c]*w00 + a10[c]*w10 + a01[c]*w01 + a11[c]*w11) * intensity;
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
    const float* const hb = g_hdrBuf.data();
    float* const BR = s_br.data();

    // 1. Bright-pass + downsample (same soft knee as bloom).
    hdrDispatchRows(bh, [=](int by1, int by2) {
        const float inv = 1.0f / float(DS * DS);
        for (int by = by1; by < by2; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                float sb = 0, sg = 0, sr = 0;
                for (int dy = 0; dy < DS; ++dy) {
                    const int sy = by * DS + dy; if (sy >= H) break;
                    const float* h = hb + size_t(sy) * size_t(W) * 4;
                    for (int dx = 0; dx < DS; ++dx) {
                        const int sx = bx * DS + dx; if (sx >= W) break;
                        const float B = h[sx*4+0], G = h[sx*4+1], R = h[sx*4+2];
                        const float lum = R > G ? (R > B ? R : B) : (G > B ? G : B);
                        if (lum > thresh) { const float w = (lum - thresh) / lum; sb += B*w; sg += G*w; sr += R*w; }
                    }
                }
                float* a = BR + (size_t(by) * bw + bx) * 3;
                a[0] = sb * inv; a[1] = sg * inv; a[2] = sr * inv;
            }
    });

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
    float* const hw = g_hdrBuf.data();
    const float* const SH = s_h.data();
    const float* const SV = (vert > 0.0f) ? s_v.data() : nullptr;
    hdrDispatchRows(H, [=](int y1, int y2) {
        for (int y = y1; y < y2; ++y) {
            const float fy = (float(y) + 0.5f) / float(DS) - 0.5f;
            int y0 = int(std::floor(fy)); float wy = fy - float(y0);
            if (y0 < 0) { y0 = 0; wy = 0; } if (y0 >= bh - 1) { y0 = bh > 1 ? bh - 2 : 0; wy = bh > 1 ? 1.0f : 0.0f; }
            const int y0b = std::min(y0 + 1, bh - 1);
            float* row = hw + size_t(y) * size_t(W) * 4;
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
                row[x*4+0] += b * tB * intensity;
                row[x*4+1] += g * tG * intensity;
                row[x*4+2] += r * tR * intensity;
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

    static std::vector<float> s_src;
    s_src.assign(g_hdrBuf.begin(), g_hdrBuf.begin() + px * 4);
    const float* const SRC = s_src.data();
    float* const DST = g_hdrBuf.data();

    auto cocAt = [=](size_t i) -> float {
        float c = std::fabs(viewZ(i) - focus) / range; if (c > 1.0f) c = 1.0f;
        return c * maxR;
    };

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
    const float* const hb = g_hdrBuf.data();
    float* const BR = s_br.data();
    hdrDispatchRows(bh, [=](int by1, int by2) {
        const float inv = 1.0f / float(DS * DS);
        for (int by = by1; by < by2; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                float sb = 0, sg = 0, sr = 0;
                for (int dy = 0; dy < DS; ++dy) {
                    const int sy = by * DS + dy; if (sy >= H) break;
                    const float* h = hb + size_t(sy) * size_t(W) * 4;
                    for (int dx = 0; dx < DS; ++dx) {
                        const int sx = bx * DS + dx; if (sx >= W) break;
                        const float B = h[sx*4+0], G = h[sx*4+1], R = h[sx*4+2];
                        const float lum = R > G ? (R > B ? R : B) : (G > B ? G : B);
                        if (lum > thresh) { const float w = (lum - thresh) / lum; sb += B*w; sg += G*w; sr += R*w; }
                    }
                }
                float* a = BR + (size_t(by) * bw + bx) * 3;
                a[0] = sb * inv; a[1] = sg * inv; a[2] = sr * inv;
            }
    });

    float* const hw = g_hdrBuf.data();
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
            float* row = hw + size_t(y) * size_t(W) * 4;
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
                row[x*4+0] += sb * intensity;
                row[x*4+1] += sg * intensity;
                row[x*4+2] += sr * intensity;
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
