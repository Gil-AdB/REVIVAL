#include "Mekalele.h"

#include "Base/FeatureFlags.h"

namespace meka {
// See TheOtherBarry.h fwd-decl. Defined here where GBuffer is complete.
uint32_t* gbuffer_mat32_plane(GBuffer* gb) {
	return gb ? gb->txtr.data() : nullptr;
}
// Continuous per-face mip fraction for trilinear albedo filtering, set by
// FrustumClipper::MiplevelClipper before each filler call (see Mekalele.h).
thread_local float g_tlsMipFrac = 0.0f;
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
std::vector<uint16_t> s_engineXparPeelFloor;
} // namespace

meka::GBuffer *g_gbuffer                = nullptr;
meka::GBuffer *g_gbufferTransparent     = nullptr;
meka::GBuffer *g_gbufferTransparentBack = nullptr;
namespace meka { float *g_pomDbgUV = nullptr; int g_pomDbgStride = 0; int g_pomDbgH = 0; }
uint16_t      *g_xparZ                  = nullptr;
uint16_t      *g_xparZBack              = nullptr;
int            g_xparZCount             = 0;
// Depth-peel floor: per-pixel "nearest already-peeled" Z, used to peel
// transparent surfaces deeper than the single front/back fragment. A
// transparent raster pass accepts a fragment only when its z_candidate is
// STRICTLY FARTHER than this floor (z_candidate < floor), so successive
// passes walk away from the camera. 0xFFFF = no floor (everything passes) —
// the K=1 (single front/back) configuration leaves it at 0xFFFF and is
// byte-identical to the legacy 2-deep peel.
uint16_t      *g_xparPeelFloor          = nullptr;
thread_local bool g_xparPeelReverse     = false;

void EngineGBuffer_Resize(int X, int Y) {
    size_t numPixels = size_t(X) * size_t(Y);
    s_engineGBuffer.normal.assign(numPixels, 0);
    s_engineGBuffer.tangent.assign(numPixels, 0);
    s_engineGBuffer.txtr.assign(numPixels, 0);
    // Static-shadow lightmap planes — only allocated when the feature is
    // on (saves ~6 bytes per pixel × W*H + the per-pixel write cost in
    // the Mekalele hot loop). Mekalele's wantLm check picks up
    // span.lightmapMF == nullptr as "off".
    if (fds::FeatureFlags::shadow_lightmap()) {
        s_engineGBuffer.lightmapMF.assign(numPixels, 0);
        s_engineGBuffer.lightmapST.assign(numPixels, 0);
    } else {
        s_engineGBuffer.lightmapMF.clear();
        s_engineGBuffer.lightmapST.clear();
    }
    // Per-pixel ShadowMatID — same lifecycle as the lightmap planes
    // (used by the deferred PolyId cube-shadow path to read receiver
    // identity without bouncing through the 8-bit matID in `txtr`).
    // Allocated unconditionally on the opaque g-buffer so non-lightmap
    // scenes still benefit when their materials set Material::ShadowMatID
    // (greets hull merge) or per-face F->ShadowMatID (greets wall split).
    s_engineGBuffer.shadowMatID.assign(numPixels, 0);
    // DIAGNOSTIC per-pixel face identity (--face_id_dump). Allocated only when
    // the flag is on; empty otherwise, which is what makes the rasterizer skip
    // the write (GBufferSpan hands the inner loop a nullptr).
    if (fds::FeatureFlags::face_id_dump()) {
        s_engineGBuffer.faceId.assign(numPixels, 0);
    } else {
        s_engineGBuffer.faceId.clear();
    }
    // Filtered-albedo plane — allocated only when FDS_TEXTURE_FILTER > 0.
    // Holds the per-pixel bilinear/trilinear diffuse color the Mekalele
    // pass samples at raster time (the sub-texel fraction is gone by the
    // time the kernel sees the swizzled address in `txtr`). Empty → the
    // deferred kernel falls back to point-sampling, byte-identical.
    // --poly_viz writes its ownership colour into the same plane, so it needs
    // the plane allocated even with texture filtering off (which is the default
    // — without this the viz is a silent no-op, and this campaign has already
    // lost time to silently-defaulted renders).
    if (fds::FeatureFlags::texture_filter() > 0 || fds::FeatureFlags::poly_viz()) {
        s_engineGBuffer.albedo.assign(numPixels, 0);
    } else {
        s_engineGBuffer.albedo.clear();
    }
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
    // Peel floor: 0xFFFF everywhere = "no fragment peeled yet, accept all".
    // The dispatch resets/advances it per (mesh, side) batch when peeling
    // more than one pass; left untouched it keeps the legacy single-pass
    // behaviour (every z_candidate < 0xFFFF, since z_candidate <= 0xFF80).
    s_engineXparPeelFloor.assign(numPixels, 0xFFFFu);
    g_xparPeelFloor = s_engineXparPeelFloor.data();
    g_xparZCount = int(numPixels);
}
