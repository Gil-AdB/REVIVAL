#ifndef FDS_RENDER_TARGET_H_INCLUDED
#define FDS_RENDER_TARGET_H_INCLUDED

#include <cstdint>

// G-buffer types live in FILLERS/Mekalele.h. Forward-declare to keep
// this header dependency-free for the per-pixel rasterizers that only
// need framebuffer pointers + dimensions.
namespace meka { struct GBuffer; }

namespace fds {

// Where pixels land + how-deep coords encode. Effectively constant for
// the lifetime of one render call (a render-target swap happens before
// the call, not inside it — e.g. the CITY cube-map bake swaps MainSurf
// to TmpSurf before Render and back after). Per-pixel rasterizers take
// `const RenderTarget&` and never write to it; the orchestrator builds
// one at frame top, passes it down, lets it go out of scope at the end.
//
// Replaces these scattered file-scope globals:
//   VPage, ZPage16, XRes, YRes, VESA_BPSL  (engine framebuffer)
//   g_gbuffer, g_gbufferTransparent, g_gbufferTransparentBack,
//   g_xparZ, g_xparZBack                   (deferred G-buffer layers)
struct RenderTarget {
    // Color framebuffer (32-bit BGRA). Tile rasterizers walk this with
    // bytesPerScanline strides; the deferred G-buffer write also lands
    // ROP results into the same vpage at composite time.
    uint32_t *vpage              = nullptr;
    int       bytesPerScanline   = 0;

    // 16-bit z-buffer parallel to vpage. Encoded as
    // `enc = 0xFF80 - round(viewZ * zScale)`; higher enc = closer.
    uint16_t *zpage16            = nullptr;

    // Surface dimensions in pixels. The engine resizes by reallocating
    // vpage / zpage16 and bumping these; a render call sees them frozen.
    int xres = 0;
    int yres = 0;

    // Deferred G-buffer layers. Null when running the forward path.
    // `gbuffer` holds opaque (mat32 + normal + tangent); the two
    // transparent layers hold up to two depth-peeled transparent
    // surfaces composited after the opaque lighting kernel.
    meka::GBuffer *gbuffer                  = nullptr;
    meka::GBuffer *gbufferTransparent       = nullptr;
    meka::GBuffer *gbufferTransparentBack   = nullptr;
    // 16-bit transparent-layer depth buffers parallel to the G-buffers
    // above. Null when forward.
    uint16_t      *xparZ                    = nullptr;
    uint16_t      *xparZBack                = nullptr;
    // Raw opaque mat32 plane (== gbuffer->txtr) + its pixel count. Exposed as a
    // bare pointer so the HDR lift can read the per-pixel forward sentinel
    // (Mat_HdrEmissive → 0xFFFFFFFE) without pulling in the full GBuffer
    // definition. mat32Count lets the lift confirm the plane matches THIS pass's
    // dims — in the mirror RTT the globals are the RTT surface but g_gbuffer is
    // still the MAIN one, so count != rt.xres*yres there and the boost is skipped.
    const uint32_t *mat32                   = nullptr;
    uint32_t        mat32Count              = 0;
};

} // namespace fds

#endif
