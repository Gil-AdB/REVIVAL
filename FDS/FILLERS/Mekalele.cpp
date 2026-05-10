#include "Mekalele.h"

// Engine-side G-buffers. Two of them:
//   g_gbuffer            — opaque surfaces. Mekalele writes here in the
//                          deferred raster pass; Render_DeferredLighting
//                          reads it.
//   g_gbufferTransparent — closest front-facing transparent surface per
//                          pixel. MekaleleTransparent writes here;
//                          Render_DeferredTransparentLighting reads it
//                          and alpha-blends onto VPage.
// Both share the same dimensions as the engine framebuffer; resized
// together by EngineGBuffer_Resize. The transparent buffer also has
// its own zbuffer (g_xparZ) so per-pixel front-of-transparent Z-test
// is independent of opaque Z (which particles still use).
namespace {
meka::GBuffer       s_engineGBuffer;
meka::GBuffer       s_engineGBufferTransparent;
std::vector<uint16_t> s_engineXparZ;
} // namespace

meka::GBuffer *g_gbuffer            = nullptr;
meka::GBuffer *g_gbufferTransparent = nullptr;
uint16_t      *g_xparZ              = nullptr;
int            g_xparZCount         = 0;

void EngineGBuffer_Resize(int X, int Y) {
    size_t numPixels = size_t(X) * size_t(Y);
    s_engineGBuffer.normal.assign(numPixels, 0);
    s_engineGBuffer.txtr.assign(numPixels, 0);
    g_gbuffer = &s_engineGBuffer;
    s_engineGBufferTransparent.normal.assign(numPixels, 0);
    s_engineGBufferTransparent.txtr.assign(numPixels, 0xFFFFFFFFu);
    g_gbufferTransparent = &s_engineGBufferTransparent;
    s_engineXparZ.assign(numPixels, 0);
    g_xparZ = s_engineXparZ.data();
    g_xparZCount = int(numPixels);
}
