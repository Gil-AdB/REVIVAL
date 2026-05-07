#include "Resize.h"

// Forward decl from SDL2.cpp — the SDL-side teardown/setup of texture and
// data buffer plus the call back into VESA_VPageExternal that updates the
// engine globals (XRes, CntrX, PageSize, YOffs, ...).
extern void SDL2_HandleResize(int newX, int newY);

std::atomic<uint64_t> g_pendingResize{0};

void EngineResize(int newX, int newY) {
    SDL2_HandleResize(newX, newY);
}
