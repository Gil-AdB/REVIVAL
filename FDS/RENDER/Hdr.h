#pragma once
#include <vector>
#include <cstdint>

// HDR prototype — see docs/fountain_hdr_plan.md. Phase 0: an f32 radiance
// accumulation buffer parallel to VPage, plus a tonemap pass that maps it down
// to the 8-bit framebuffer. Gated on FeatureFlags::hdr(); a no-op until the
// deferred passes populate g_hdrBuf (Phase 1). Prototype uses f32 storage to
// de-risk the pipeline; production switches to f16 (cheap convert both targets).
namespace fds {

// Per-pixel radiance, channel order B,G,R,(pad) to match VPage's BGRA. 4 floats
// per pixel, XRes*YRes, contiguous (no scanline pad). Deferred passes write here
// when hdr() is on; Render_TonemapToVPage maps it to VPage.
// HDR texel component type. f16 storage on arm64 (__fp16 is storage-only:
// loads/stores are 1-cycle fcvt, all arithmetic promotes to float) — HALVES
// the bandwidth of every pass that streams g_hdrBuf (~10-14 full-buffer
// streams/frame: kernel write, fog/xpar composites, SSAO, DoF, bright-pass,
// bloom/anamorphic/ghost composites, tonemap). Radiance range fits f16
// (max 65504, values here 0..~10^4); each pass fully rewrites its output
// (no iterative in-place accumulation), so rounding doesn't compound beyond
// ~1 ulp per pass. Define FDS_HDR_F32=1 (or non-arm64) for float storage.
#if defined(__aarch64__) && !defined(FDS_HDR_F32)
using hdrf = __fp16;
#else
using hdrf = float;
#endif
extern std::vector<hdrf> g_hdrBuf;

// Set true by the froxel composite (the only writer of g_hdrBuf) when it runs in
// HDR mode; reset by Hdr_BeginFrame. The tonemap no-ops when false, so frames
// that never populated the buffer (fog off, non-froxel paths) render normally
// instead of tonemapping a cleared buffer to black.
extern bool g_hdrActive;

// Dimensions g_hdrBuf was last sized for by Hdr_BeginFrame (= the MAIN view).
// g_hdrBuf parallels the main framebuffer; a pass that renders into a DIFFERENT
// target — notably the order-2 mirror RTT, which calls Render_DeferredLighting
// directly without going through renderFrame/Hdr_BeginFrame — must NOT write
// g_hdrBuf: it's either unsized (null .data() → crash) or sized for the wrong
// resolution. Every g_hdrBuf write gates on Hdr_WritableFor with the CURRENT
// pass's dims (ctx.xres/ctx.yres); only the main pass matches.
extern int g_hdrBufW, g_hdrBufH;
void Hdr_DebugScan(const char* tag);

// f16-safe radiance ceiling for every g_hdrBuf store. __fp16 overflows to +inf
// above 65504, and one inf turns the whole post chain to NaN/black tiles (the
// bright-pass computes (lum-thresh)/lum, DoF divides weight sums). 60000 is
// within 5% of the largest radiance measured in real content (63008, a spec
// hotspot) and far beyond where the bright-pass weight and the tonemap already
// saturate — clamping is visually free. Applies identically under f32 storage
// (keeps the two configurations rendering the same image).
constexpr float kHdrMax = 60000.0f;
static inline float HdrClamp(float v) { return v > kHdrMax ? kHdrMax : v; }
inline bool Hdr_WritableFor(int xr, int yr) {
    return !g_hdrBuf.empty() && xr == g_hdrBufW && yr == g_hdrBufH;
}

// Phase 2.3: when true, renderFrame SKIPS its own end-of-pipeline tonemap — the
// scene tonemaps later itself (the fountain tick tonemaps AFTER the bolt so the
// bolt accumulates into g_hdrBuf and blooms). The fountain sets this before
// Render() and clears it after its own tonemap. NOT reset by Hdr_BeginFrame
// (which runs inside Render(), after the scene set it). Other scenes leave it
// false → renderFrame tonemaps at its own end.
extern bool g_hdrDeferTonemap;

// Size g_hdrBuf to the current XRes*YRes and clear it. Call once per frame,
// before any HDR write (Phase 1 wires this ahead of the deferred passes).
void Hdr_BeginFrame();

// Same, but for an arbitrary target resolution — the mirror RTT pass sizes
// g_hdrBuf to its offscreen surface so its reflection accumulates + tonemaps
// HDR-correctly (Hdr_WritableFor then matches that pass's ctx.xres/yres).
void Hdr_BeginFramePass(int w, int h);

// Full-screen HDR bloom. Bright-passes the linear radiance in g_hdrBuf, blurs a
// quarter-res pyramid, and adds the glow back INTO g_hdrBuf — so call it after
// all HDR accumulation (scene/fog/cones/overlays) and BEFORE Render_TonemapToVPage
// (the bloom then rolls off through ACES with everything else). No-op unless
// --bloom && g_hdrActive. Threaded (tile-job dispatch).
void Render_BloomPass();

// Anamorphic lens streaks (Star Trek 2009): horizontal + faint vertical light
// streaks off hot HDR sources, added pre-tonemap. No-op unless --anamorphic &&
// g_hdrActive. Threaded. Call alongside Render_BloomPass, before the tonemap.
void Render_AnamorphicPass();

// Depth-of-field: circle-of-confusion bokeh blur off the G-buffer depth, applied
// to the linear g_hdrBuf. Call BEFORE Render_BloomPass so defocused highlights
// bloom into discs. No-op unless --dof && g_hdrActive. Threaded.
void Render_DoFPass();

// Screen-space lens-flare ghosts: a chain of chroma-fringed discs + a halo ring
// mirrored through screen-centre off each hot HDR source, sampled from the
// bright-pass and added pre-tonemap. No-op unless --lens_ghosts && g_hdrActive.
// Threaded. Call alongside Render_BloomPass/Render_AnamorphicPass, before the tonemap.
void Render_LensGhostPass();

// Post-tonemap lens "glass": radial chromatic aberration + vignette on the final
// 8-bit VPage. No-op unless --chromatic / --vignette. Threaded. Call AFTER
// Render_TonemapToVPage, and only on the main view (not the mirror RTT).
void Render_LensPostPass();

// Post-tonemap "film stock": colour grade (temperature/contrast/saturation +
// teal-orange split-tone) + animated film grain, on the final 8-bit VPage.
// No-op unless --grade / --grain. Threaded. Call AFTER Render_LensPostPass,
// main view only.
void Render_GradeGrainPass();

// Tonemap g_hdrBuf -> VPage (8-bit BGRA). Call after all HDR writes, before the
// UI/text overlays (those must NOT be tonemapped). No-op if g_hdrBuf is unsized.
void Render_TonemapToVPage();

// Fog-off HDR activation. Scenes without the froxel fog composite (no --fast_fog,
// e.g. greets) otherwise never set g_hdrActive, so the tonemap AND the transparent
// peel's HDR/reflection path no-op and the view stays LDR. This does what the
// composite does minus the fog: covered opaque pixels already hold the kernel's
// linear radiance (B1/B2); uncovered pixels (sky/forward, coverage h[3]==0) are
// lifted from VPage so the tonemap doesn't black them out — then it sets
// g_hdrActive. Call AFTER the deferred kernel and BEFORE the transparent peel,
// only when hdr() && !g_hdrActive (composite already activated → skip).
void Hdr_ActivateNoFog();

} // namespace fds
