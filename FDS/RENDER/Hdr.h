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
extern std::vector<float> g_hdrBuf;

// Set true by the froxel composite (the only writer of g_hdrBuf) when it runs in
// HDR mode; reset by Hdr_BeginFrame. The tonemap no-ops when false, so frames
// that never populated the buffer (fog off, non-froxel paths) render normally
// instead of tonemapping a cleared buffer to black.
extern bool g_hdrActive;

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

// Tonemap g_hdrBuf -> VPage (8-bit BGRA). Call after all HDR writes, before the
// UI/text overlays (those must NOT be tonemapped). No-op if g_hdrBuf is unsized.
void Render_TonemapToVPage();

} // namespace fds
