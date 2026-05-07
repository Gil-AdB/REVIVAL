#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

// Cross-thread resize coordination. The SDL event loop (main thread) packs
// the new (X, Y) into g_pendingResize; the demo thread polls at frame top
// (via SceneTick.h's poll_pending_resize), applies, and clears. A value of
// 0 means "nothing pending."
extern std::atomic<uint64_t> g_pendingResize;

// Serializes engine-surface mutations against scene-init paths that
// temporarily swap the global MainSurf pointer to a private surface (e.g.
// Initialize_City's cube-map render uses a small TmpSurf for the off-screen
// reflection passes). EngineResize calls VESA_VPageExternal which does
// memcpy(MainSurf, &SDL_MainSurf, sizeof) — that would stomp the swap if
// the demo thread fires a resize while the init thread holds MainSurf
// pointed at its private surface. Both sides must lock around the
// critical section.
extern std::mutex g_engineSurfaceMutex;

inline uint64_t pack_size(int x, int y) {
    return (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
}
inline void unpack_size(uint64_t v, int& x, int& y) {
    x = int(v >> 32);
    y = int(v & 0xFFFFFFFFu);
}

// Engine-level resize: frees + reallocates framebuffer/Z-buffer, recreates
// the SDL_Texture at the new size, rebuilds YOffs, and updates the
// resolution globals (XRes, YRes, CntrX/Y, CntrEX/Y, PageSize, ...).
// Called on the demo thread at a frame boundary.
void EngineResize(int newX, int newY);
