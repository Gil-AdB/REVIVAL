#include "Mekalele.h"

namespace meka {
// See TheOtherBarry.h fwd-decl. Defined here where GBuffer is complete.
uint32_t* gbuffer_mat32_plane(GBuffer* gb) {
	return gb ? gb->txtr.data() : nullptr;
}
}

// Engine-side G-buffers. Three of them:
//   g_gbuffer                — opaque surfaces. Mekalele writes here in
//                              the deferred raster pass;
//                              Render_DeferredLighting reads it.
//   g_gbufferTransparent     — front-facing transparent surface per
//                              pixel (closest along the view ray).
//                              MekaleleTransparent writes here.
//   g_gbufferTransparentBack — back-facing transparent surface per
//                              pixel (paired with the front one for
//                              2-deep depth-peel of convex transparent
//                              objects — entry+exit of a glass cube).
//                              MekaleleTransparentBack writes here.
// All three share the same dimensions as the engine framebuffer;
// resized together by EngineGBuffer_Resize. The two transparent
// buffers each have their own zbuffer (g_xparZ, g_xparZBack) so
// per-pixel transparent Z-test is independent of opaque Z (which
// particles still use) and the back layer is independent of the front.
namespace {
meka::GBuffer       s_engineGBuffer;
meka::GBuffer       s_engineGBufferTransparent;
meka::GBuffer       s_engineGBufferTransparentBack;
std::vector<uint16_t> s_engineXparZ;
std::vector<uint16_t> s_engineXparZBack;
} // namespace

meka::GBuffer *g_gbuffer                = nullptr;
meka::GBuffer *g_gbufferTransparent     = nullptr;
meka::GBuffer *g_gbufferTransparentBack = nullptr;
uint16_t      *g_xparZ                  = nullptr;
uint16_t      *g_xparZBack              = nullptr;
int            g_xparZCount             = 0;

void EngineGBuffer_Resize(int X, int Y) {
    size_t numPixels = size_t(X) * size_t(Y);
    s_engineGBuffer.normal.assign(numPixels, 0);
    s_engineGBuffer.tangent.assign(numPixels, 0);
    s_engineGBuffer.txtr.assign(numPixels, 0);
    g_gbuffer = &s_engineGBuffer;
    // Transparent layers don't currently use tangent — leaving those
    // empty so GBufferSpan's nullptr-tangent path keeps the rasterizer
    // from writing.
    s_engineGBufferTransparent.normal.assign(numPixels, 0);
    s_engineGBufferTransparent.txtr.assign(numPixels, 0xFFFFFFFFu);
    g_gbufferTransparent = &s_engineGBufferTransparent;
    s_engineGBufferTransparentBack.normal.assign(numPixels, 0);
    s_engineGBufferTransparentBack.txtr.assign(numPixels, 0xFFFFFFFFu);
    g_gbufferTransparentBack = &s_engineGBufferTransparentBack;
    s_engineXparZ.assign(numPixels, 0);
    g_xparZ = s_engineXparZ.data();
    s_engineXparZBack.assign(numPixels, 0);
    g_xparZBack = s_engineXparZBack.data();
    g_xparZCount = int(numPixels);
}
