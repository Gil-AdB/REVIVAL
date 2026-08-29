#include "Mekalele.h"

#include "Base/FeatureFlags.h"
#include "Base/MemCensus.h"

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
// Dimensions the planes above were last sized for — recorded so --mem_census
// can print the formula's variables and not just its product.
int s_engineGBufW = 0, s_engineGBufH = 0;
} // namespace

meka::GBuffer *g_gbuffer                = nullptr;
meka::GBuffer *g_gbufferTransparent     = nullptr;
meka::GBuffer *g_gbufferTransparentBack = nullptr;
namespace meka { float *g_pomDbgUV = nullptr; int g_pomDbgStride = 0; int g_pomDbgH = 0; }
// --pom_path_viz: per-pixel MARCH PATH CODE plane (see FeatureFlags.def for the
// bit layout). Armed by the snapshot driver for one tick, exactly like
// g_pomDbgUV, and dumped beside the PPM. nullptr = nothing recorded.
namespace meka { uint32_t *g_pomPathBuf = nullptr; }
namespace meka { float *g_pomDbgUVGeo = nullptr; }
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

// Storage probes for the runtime viz cycle (FDS/RENDER/VizCycle.cpp). The
// planes below are sized only at framebuffer resize, so a viz that writes into
// one can only be enabled live if the plane already exists — the cycle asks
// here instead of offering a mode that draws nothing. Declared extern in
// VizCycle.cpp to keep this template-heavy header out of that TU.
bool EngineGBuffer_HasAlbedoPlane() { return !s_engineGBuffer.albedo.empty(); }
bool EngineGBuffer_HasNormalPlane() { return !s_engineGBuffer.normal.empty(); }

void EngineGBuffer_Resize(int X, int Y) {
    size_t numPixels = size_t(X) * size_t(Y);
    s_engineGBufW = X; s_engineGBufH = Y;
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
    // --viz_arm allocates it too: --pom_viz / --pom_mip_viz / --pom_path_viz /
    // --poly_viz all write here, and the plane is only ever sized at resize —
    // so without arming, switching into those modes at runtime (the X-key viz
    // cycle, FDS/RENDER/VizCycle.cpp) would be a silent no-op. An allocated but
    // unread plane changes no pixel: the deferred kernel's texFilterOn also
    // requires texture_filter>0 || poly_viz.
    if (fds::FeatureFlags::texture_filter() > 0 || fds::FeatureFlags::poly_viz()
        || fds::FeatureFlags::viz_arm()) {
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
    g_xparPeelFloorDirty.store(false, std::memory_order_relaxed);
    g_xparZCount = int(numPixels);
}

// ── The peel floor's invariant, and who is allowed to break it ─────────────
// The single-pass (legacy) transparent raster has NO floor logic of its own:
// its gate is `zmask &= (z_candidate < peelFloor)` (Mekalele.h), which is a
// no-op ONLY because every entry is 0xFFFF and z_candidate <= 0xFF80. That
// all-0xFFFF state is established exactly once, by EngineGBuffer_Resize
// above, and the comment there has always said so.
//
// The MULTI-pass reverse peel writes the plane: pass 0 zeroes it and later
// passes copy the side's Z into it (RENDER.CPP's peel loop; the strip path's
// fillFloor/copyFloor in DeferredSurfaceKernel.cpp). Nothing ever restored it.
// Since the plane is engine-global and outlives any scene, a scene that peels
// deep hands the next scene a floor full of zeros — and `z_candidate < 0` is
// FALSE, so the next scene's single-pass transparents are silently Z-REJECTED
// wherever the previous scene's transparent geometry happened to land.
//
// That is the greets mirror band: the fountain sets Scene::XparPeelPasses = 4
// (FOUNTAIN.CPP), greets runs the legacy single pass, and greets' mirror-mask
// wall loses its own fragment over the fountain's leftover rectangle while the
// reflection clone still composites — an additive copy of the reflected room,
// hard-edged to a rectangle that belongs to a scene that ended minutes ago.
//
// Both halves are fixed: the dirty flag below lets the dispatcher restore the
// invariant once per frame, and XparPeel_ResetAll is the belt for scene entry.
std::atomic<bool> g_xparPeelFloorDirty{false};

void XparPeel_ResetAll()
{
    const size_t n = size_t(g_xparZCount);
    if (g_xparPeelFloor && n) std::fill_n(g_xparPeelFloor, n, uint16_t(0xFFFFu));
    g_xparPeelFloorDirty.store(false, std::memory_order_relaxed);
    // The two deep-layer slices and their Z. The legacy path full-screen
    // clears these per batch and both TBR schedulers re-declare every strip
    // dirty per frame, so they cannot leak today — but they are the same
    // class of cross-scene global and cost nothing to re-establish here.
    if (g_gbufferTransparent && g_gbufferTransparent->txtr.size() >= n)
        std::fill_n(g_gbufferTransparent->txtr.begin(), n, 0xFFFFFFFFu);
    if (g_gbufferTransparentBack && g_gbufferTransparentBack->txtr.size() >= n)
        std::fill_n(g_gbufferTransparentBack->txtr.begin(), n, 0xFFFFFFFFu);
    if (g_xparZ)     std::fill_n(g_xparZ,     n, uint16_t(0));
    if (g_xparZBack) std::fill_n(g_xparZBack, n, uint16_t(0));
    // Per-strip dirty-column bookkeeping (the XparSliceDirty records).
    XparStripSlices_MarkAllDirty();
    // thread_local, so this only disarms the CALLING thread — which is the
    // one that runs the inline offscreen bakes (the mirror RTT). Every
    // threadpool raster sets it immediately before its own raster.
    g_xparPeelReverse = false;
}

// ── --mem_census: the three G-buffers and the transparent Z/peel planes ────
// Every one of these is W*H elements and every one is `assign`-ed, i.e. fully
// TOUCHED at resize. The interesting column is the per-pixel byte total: the
// planes are the reason a 1920x1080 deferred frame carries tens of MB before
// a single triangle is drawn, and several of them are allocated
// UNCONDITIONALLY for scenes that never read them.
static void MemCensus_GBuffers() {
    const size_t n = s_engineGBuffer.normal.capacity();
    if (!n) return;
    const int X = s_engineGBufW, Y = s_engineGBufH;
    auto plane = [&](const char *sub, const char *nm, size_t cap, size_t elem,
                     const char *what) {
        fds::MemCensus::add(sub, nm, cap * elem, cap != 0,
                            "W*H=%d*%d px x %zu B/px (%s)", X, Y, elem, what);
    };
    plane("gbuf.opaque", "normal",      s_engineGBuffer.normal.capacity(),      4, "oct16.16 shading normal");
    plane("gbuf.opaque", "tangent",     s_engineGBuffer.tangent.capacity(),     2, "oct tangent, read ONLY by normal-mapped materials");
    plane("gbuf.opaque", "txtr",        s_engineGBuffer.txtr.capacity(),        4, "mip|matID|swizzled UV");
    plane("gbuf.opaque", "albedo",      s_engineGBuffer.albedo.capacity(),      4, "filtered albedo; only if texture_filter>0 || poly_viz || viz_arm");
    plane("gbuf.opaque", "lightmapMF",  s_engineGBuffer.lightmapMF.capacity(),  4, "static-LM mesh|face; only if shadow_lightmap");
    plane("gbuf.opaque", "lightmapST",  s_engineGBuffer.lightmapST.capacity(),  2, "static-LM barycentric; only if shadow_lightmap");
    plane("gbuf.opaque", "shadowMatID", s_engineGBuffer.shadowMatID.capacity(), 2, "receiver id, allocated UNCONDITIONALLY");
    plane("gbuf.opaque", "faceId",      s_engineGBuffer.faceId.capacity(),      4, "diagnostic; only if face_id_dump");
    plane("gbuf.opaque", "mirrorId",    s_engineGBuffer.mirrorId.capacity(),    1, "per-pixel mirror ownership; empty on mirrorless scenes");
    plane("gbuf.opaque", "mirrorMask",  s_engineGBuffer.mirrorMask.capacity(),  1, "immutable mirror gate");
    plane("gbuf.opaque", "mirrorMaskZ", s_engineGBuffer.mirrorMaskZ.capacity(), 2, "mirror wall depth");
    plane("gbuf.xpar",   "front.normal", s_engineGBufferTransparent.normal.capacity(), 4, "UNCONDITIONAL, even with no transparent geometry");
    plane("gbuf.xpar",   "front.txtr",   s_engineGBufferTransparent.txtr.capacity(),   4, "UNCONDITIONAL");
    plane("gbuf.xpar",   "back.normal",  s_engineGBufferTransparentBack.normal.capacity(), 4, "UNCONDITIONAL, 2-deep peel back layer");
    plane("gbuf.xpar",   "back.txtr",    s_engineGBufferTransparentBack.txtr.capacity(),   4, "UNCONDITIONAL");
    plane("gbuf.xpar",   "z.front",      s_engineXparZ.capacity(),          2, "UNCONDITIONAL");
    plane("gbuf.xpar",   "z.back",       s_engineXparZBack.capacity(),      2, "UNCONDITIONAL");
    plane("gbuf.xpar",   "peelFloor",    s_engineXparPeelFloor.capacity(),  2, "depth-peel floor; K>1 only, allocated UNCONDITIONALLY");
}
FDS_MEMCENSUS_REPORTER(MemCensus_GBuffers);
