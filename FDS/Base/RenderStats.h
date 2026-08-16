#pragma once
#include "BaseDefs.h"  // dword

// Compile-time gate. Default ON; set to 0 via
// -DFDS_RENDER_STATS_ENABLED=0 on the compile line (or in CMake) for
// prod-shipped builds where the "polys/frame" / "MPx/sec" overlay
// readouts aren't needed. When 0, the if-constexpr at every increment
// site eliminates the TLS access entirely — true zero cost, no branch.
#ifndef FDS_RENDER_STATS_ENABLED
  #define FDS_RENDER_STATS_ENABLED 1
#endif

// Render-loop perf telemetry counters (polys rendered, filler pixels
// covered). Both are written N×per-triangle from worker threads and
// read once per scene end for "polys/frame" / "MPx/sec" display.
//
// Was: shared `std::atomic<dword>` + `std::atomic<double>` directly in
// the clipper / filler hot loops. Atomic<double> on arm64 needs a CAS
// loop per increment AND every increment ping-pongs the cache line
// across cores. Sample profile (city@t=1500 deferred bench, 2026-05-31)
// showed 720+ leaf samples at the increment site inside the per-
// triangle clipper body.
//
// Now: per-thread accumulator (no atomic), aggregated on demand.
// Worker threads bump their TLS; readers call RenderStats_Flush()
// before reading the globals.
//
// Cross-thread visibility: TLS instances self-register in a global
// vector (mutex-protected, write-rare). Flush iterates + sums + zeros.
// Threads that exit before flush get their counts captured by the
// destructor.

// Aggregated bucket totals — flushed-and-reset by RenderStats_Flush.
// Defined in RenderStats.cpp; readers (scene-end overlay) reference them
// after calling Flush(). Use the FDS_STATS_INC(field) macro to bump
// the matching per-thread field at insertion sites.
namespace fds_stats {
extern dword g_clipperEntered;
extern dword g_clipNeedZ;
extern dword g_clipNeed2D;
extern dword g_clipNoClip;
extern dword g_clipEmitted;
extern dword g_mipEntered;
extern dword g_mipFastUniform;
extern dword g_mipSplit;
// --mip_stats histogram: per-selected-miplevel face count and covered
// screen area, plus the three shape counters that explain WHY a face
// landed where it did. See RenderStats_MipReport().
extern dword  g_mipLevelFaces[16];
extern double g_mipLevelPix[16];
extern dword  g_mipNomip;      // Txtr_Nomip faces (forced to level 0)
extern dword  g_mipNegArea;    // faces whose signed pixArea came out < 0
extern dword  g_mipBigFace;    // faces with |pixArea| >= the 2%-screen threshold
// Cumulative (never-reset) mirrors of the three branch counters — the bucket
// totals above are zeroed at the top of each Flush, which discards them before
// the atexit report runs.
extern dword  g_mipEnteredCum;
extern dword  g_mipFastUniformCum;
extern dword  g_mipSplitCum;
}

namespace fds {

// Which dispatcher fed FrustumClipper::Render — the axis --clip_stats splits
// its per-(face, tile) census on. Without it the census is a single number
// that hides WHERE the entries come from, and that number is misleading: at
// greets t=5743 the tiled deferred pass contributes 16 % of the entries and
// the shadow-map rasteriser contributes 84 %, which is not what a reader of
// "the G-buffer fill's clipper" would assume. Set once per dispatcher call
// (i.e. per tile), never per face.
enum class ClipSrc : unsigned char {
    Unknown = 0,     // nothing tagged the clipper
    ForwardTiled,    // RenderInner            — forward raster, tile job
    ForwardInline,   // RenderForwardRegionInline — offscreen forward (shards)
    DeferredTiled,   // RenderInnerMekalele    — deferred opaque, tile job
    DeferredXpar,    // RenderInnerDeferredTransparent
    DeferredInline,  // MekaleleFillRegionInline — offscreen G-buffer (RTT/shard)
    DeferredStrip,   // DeferredSurfaceKernel's transparent strip raster
    ShadowMap,       // Shadows.cpp's per-light depth raster
    Count
};
constexpr unsigned kClipSrcCount = unsigned(ClipSrc::Count);
const char* ClipSrcName(ClipSrc s);

struct PerThreadRenderStats {
    dword  polysRendered    = 0;
    double fillerPixelcount = 0.0;
    // Clipper bucket counters — see FrustumClipper::Render. Used to
    // estimate the perf opportunity of skipping clipper work for polys
    // that don't need it (e.g. fully-inside, single-mip, etc.).
    // Indexed by ClipSrc. One indexed bump per face-visit — the SAME single
    // increment the flat counter used to cost, so tagging is free.
    dword  clipperEntered[kClipSrcCount] = {0};  // every poly that enters Render()
    dword  clipNeedZ        = 0;  // had any Vtx_VisNear or Vtx_VisFar
    dword  clipNeed2D       = 0;  // had any Vtx_VisLeft/Right/Up/Down
    // The PASS-THROUGH census (--clip_stats), also per ClipSrc. clipNoClip
    // counts entries whose OR-of-vertex-flags carries NEITHER a Z nor a 2D
    // clip bit — the face lies wholly inside this tile's rect, so the three
    // 140-byte Vertex copies are handed to the filler verbatim. clipEmitted
    // counts entries where the clip actually manufactured a vertex (newVert
    // advanced). The remainder — entered − noClip − emitted — is the population
    // that pays the copy and the UV stamp and then produces NOTHING, because
    // the tile clip rejected the polygon outright.
    dword  clipNoClip[kClipSrcCount]  = {0};
    dword  clipEmitted[kClipSrcCount] = {0};
    dword  mipEntered       = 0;  // entered MiplevelClipper (textured non-shadow)
    dword  mipFastUniform   = 0;  // exited via small-area / uniform-mip fast path
    dword  mipSplit         = 0;  // multi-mip split into sub-polys
    // --mip_stats: histogram of the level actually handed to the filler,
    // weighted both per-draw and by covered screen area (the area weight is
    // what the eye sees; the draw weight is what the texture cache sees).
    dword  mipLevelFaces[16] = {0};
    double mipLevelPix[16]   = {0.0};
    dword  mipNomip          = 0;
    dword  mipNegArea        = 0;
    dword  mipBigFace        = 0;
};

// Returns the calling thread's TLS counter. Registers on first call.
// Only meaningful when FDS_RENDER_STATS_ENABLED — callers should wrap
// uses in `if constexpr (FDS_RENDER_STATS_ENABLED)`.
PerThreadRenderStats& stats_tls();

// Sums all TLS counters into the global accumulators (g_renderedPolys
// and FillerPixelcount) and zeros the TLS counts. Cheap (one mutex
// take + N adds, where N = number of worker threads ≈ cores). Call
// once per scene end before reading the global stats. No-op when
// FDS_RENDER_STATS_ENABLED is 0.
void RenderStats_Flush();

// --mip_stats: flush, then print the mip-level histogram (draws + covered
// screen area per level) and the branch/shape counters to stderr. Registered
// with atexit() on first MiplevelClipper entry when the flag is set, so it
// reports once per process without any scene-driver plumbing.
void RenderStats_MipReport();
void RenderStats_MipReportAtExit();

// --clip_stats: flush, then print the per-(face, tile) clipper census, split
// by ClipSrc. Registered with atexit() on the first Render() entry when the
// flag is set — same pattern as the mip report, so it needs no scene-driver
// plumbing and works under --snapshot as well as --bench.
void RenderStats_ClipReport();
void RenderStats_ClipReportAtExit();

// Convenience macros for hot-loop call sites. Expand to nothing when
// stats are compile-disabled, so the TLS write disappears entirely.
#if FDS_RENDER_STATS_ENABLED
  #define FDS_STATS_INC_POLYS()        (::fds::stats_tls().polysRendered++)
  #define FDS_STATS_ADD_PIXELS(area)   (::fds::stats_tls().fillerPixelcount += (area))
  #define FDS_STATS_INC(field)         (::fds::stats_tls().field++)
  // Scoped variants: capture the per-thread block ONCE with FDS_STATS_SCOPE()
  // at the top of a hot function, then bump via the _S macros. `stats_tls()`
  // is a non-inlined cross-TU call guarded by a thread-local init check
  // (TlsHolder has a non-trivial ctor/dtor), so the per-face clipper — which
  // hits ~10 increment sites — was paying that guard+call each time. One
  // capture per call collapses it; the counters are identical (byte-for-byte
  // output), only the lookup is hoisted.
  #define FDS_STATS_SCOPE()            ::fds::PerThreadRenderStats& _fds_st = ::fds::stats_tls()
  #define FDS_STATS_INC_POLYS_S()      (_fds_st.polysRendered++)
  #define FDS_STATS_ADD_PIXELS_S(area) (_fds_st.fillerPixelcount += (area))
  #define FDS_STATS_INC_S(field)       (_fds_st.field++)
#else
  #define FDS_STATS_INC_POLYS()        ((void)0)
  #define FDS_STATS_ADD_PIXELS(area)   ((void)(area))
  #define FDS_STATS_INC(field)         ((void)0)
  #define FDS_STATS_SCOPE()            ((void)0)
  #define FDS_STATS_INC_POLYS_S()      ((void)0)
  #define FDS_STATS_ADD_PIXELS_S(area) ((void)(area))
  #define FDS_STATS_INC_S(field)       ((void)0)
#endif

} // namespace fds
