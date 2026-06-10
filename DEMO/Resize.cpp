#include "Resize.h"

// Forward decl from SDL2.cpp — the SDL-side teardown/setup of texture and
// data buffer plus the call back into VESA_VPageExternal that updates the
// engine globals (XRes, CntrX, PageSize, YOffs, ...).
extern void SDL2_HandleResize(int newX, int newY);

std::atomic<uint64_t> g_pendingResize{0};
// Defined in FDS/RENDER/OffscreenView.cpp (the RAII offscreen-view
// scope owns the MainSurf-swap discipline this mutex protects).
extern std::mutex g_engineSurfaceMutex;

void EngineResize(int newX, int newY) {
    // Try-lock + defer: if a scene-init thread is mid-MainSurf-swap (e.g.
    // Initialize_City's cube-map render), don't block the demo thread for
    // the duration of that init — it can take ~10s on wasm. Push the
    // pending resize back into the atomic and let the next frame's poll
    // try again. Newest size wins regardless.
    std::unique_lock<std::mutex> lock(g_engineSurfaceMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        g_pendingResize.store(pack_size(newX, newY));
        return;
    }
    SDL2_HandleResize(newX, newY);
}
