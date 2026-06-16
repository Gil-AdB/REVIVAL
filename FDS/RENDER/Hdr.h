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

// Size g_hdrBuf to the current XRes*YRes and clear it. Call once per frame,
// before any HDR write (Phase 1 wires this ahead of the deferred passes).
void Hdr_BeginFrame();

// Tonemap g_hdrBuf -> VPage (8-bit BGRA). Call after all HDR writes, before the
// UI/text overlays (those must NOT be tonemapped). No-op if g_hdrBuf is unsized.
void Render_TonemapToVPage();

} // namespace fds
