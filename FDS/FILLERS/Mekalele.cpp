#include "Mekalele.h"

// Single engine-side G-buffer + the public pointer the rasterizer reads.
// Lifecycle parallels the engine framebuffer (created with V_Create on
// boot + every SDL2_HandleResize). Snapshot harness calls
// EngineGBuffer_Resize directly since it doesn't go through V_Create.
namespace {
meka::GBuffer s_engineGBuffer;
} // namespace

meka::GBuffer *g_gbuffer = nullptr;

void EngineGBuffer_Resize(int X, int Y) {
    size_t numPixels = size_t(X) * size_t(Y);
    s_engineGBuffer.normal.assign(numPixels, 0);
    s_engineGBuffer.txtr.assign(numPixels, 0);
    g_gbuffer = &s_engineGBuffer;
}
