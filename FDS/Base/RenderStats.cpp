#include "RenderStats.h"

#if FDS_RENDER_STATS_ENABLED

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

// Forward decls of the global accumulators (defined elsewhere).
extern std::atomic<dword>  g_renderedPolys;
extern std::atomic<double> FillerPixelcount;

// Clipper bucket totals — summed across all worker threads on flush.
// Exposed for scene-end overlay (header declares them in fds_stats).
// Race-free under the same flush-mutex used for the TLS sum.
namespace fds_stats {
dword g_clipperEntered = 0;
dword g_clipNeedZ      = 0;
dword g_clipNeed2D     = 0;
dword g_clipNoClip     = 0;
dword g_clipEmitted    = 0;
dword g_mipEntered     = 0;
dword g_mipFastUniform = 0;
dword g_mipSplit       = 0;
dword  g_mipLevelFaces[16] = {0};
double g_mipLevelPix[16]   = {0.0};
dword  g_mipNomip      = 0;
dword  g_mipNegArea    = 0;
dword  g_mipBigFace    = 0;
// Cumulative mirrors of the three branch counters. The bucket totals above are
// RESET at the top of every Flush (per-scene semantics), and greets' scene-end
// flush lands before the atexit report — so the report keeps its own running
// sums, which are never reset.
dword  g_mipEnteredCum     = 0;
dword  g_mipFastUniformCum = 0;
dword  g_mipSplitCum       = 0;
// Cumulative, per-ClipSrc mirrors for the --clip_stats atexit report, for the
// same reason the mip ones exist: Flush zeroes the per-scene buckets above and
// a scene-end flush lands before atexit runs.
dword  g_clipEnteredSrcCum[unsigned(fds::ClipSrc::Count)] = {0};
dword  g_clipNoClipSrcCum [unsigned(fds::ClipSrc::Count)] = {0};
dword  g_clipEmittedSrcCum[unsigned(fds::ClipSrc::Count)] = {0};
dword  g_clipNeedZCum      = 0;
dword  g_clipNeed2DCum     = 0;
}

namespace fds {

namespace {
std::mutex                            s_regMutex;
std::vector<PerThreadRenderStats*>    s_registry;

// One-per-thread holder. Ctor registers, dtor flushes-then-unregisters
// so a thread that exits before any RenderStats_Flush() call still
// gets its counts captured.
struct TlsHolder {
    PerThreadRenderStats c;
    TlsHolder() {
        std::lock_guard<std::mutex> g(s_regMutex);
        s_registry.push_back(&c);
    }
    ~TlsHolder() {
        std::lock_guard<std::mutex> g(s_regMutex);
        if (c.polysRendered) {
            g_renderedPolys.fetch_add(c.polysRendered, std::memory_order_relaxed);
        }
        if (c.fillerPixelcount != 0.0) {
            // Atomic<double> add via CAS loop. Rare (per thread exit),
            // not per-triangle.
            double cur = FillerPixelcount.load(std::memory_order_relaxed);
            while (!FillerPixelcount.compare_exchange_weak(
                       cur, cur + c.fillerPixelcount,
                       std::memory_order_relaxed)) {}
        }
        // The --mip_stats histogram used to be DROPPED here, and that made the
        // flag print nothing at all under --snapshot: the tile workers exit
        // (and unregister) during shutdown, so by the time the atexit report
        // ran the registry was empty and RenderStats_MipReport bailed on
        // totFaces == 0. Everything a thread still holds has to be merged on
        // its way out, exactly as Flush would have merged it — Flush zeroes
        // what it takes, so this cannot double-count.
        for (int m = 0; m < 16; ++m) {
            fds_stats::g_mipLevelFaces[m] += c.mipLevelFaces[m]; c.mipLevelFaces[m] = 0;
            fds_stats::g_mipLevelPix[m]   += c.mipLevelPix[m];   c.mipLevelPix[m]   = 0.0;
        }
        fds_stats::g_mipEnteredCum     += c.mipEntered;
        fds_stats::g_mipFastUniformCum += c.mipFastUniform;
        fds_stats::g_mipSplitCum       += c.mipSplit;
        for (unsigned k = 0; k < kClipSrcCount; ++k) {
            fds_stats::g_clipEnteredSrcCum[k] += c.clipperEntered[k];
            fds_stats::g_clipNoClipSrcCum[k]  += c.clipNoClip[k];
            fds_stats::g_clipEmittedSrcCum[k] += c.clipEmitted[k];
        }
        fds_stats::g_clipNeedZCum  += c.clipNeedZ;
        fds_stats::g_clipNeed2DCum += c.clipNeed2D;
        fds_stats::g_mipNomip   += c.mipNomip;    c.mipNomip   = 0;
        fds_stats::g_mipNegArea += c.mipNegArea;  c.mipNegArea = 0;
        fds_stats::g_mipBigFace += c.mipBigFace;  c.mipBigFace = 0;
        s_registry.erase(std::remove(s_registry.begin(), s_registry.end(), &c),
                         s_registry.end());
    }
};
thread_local TlsHolder t_holder;
}  // namespace

PerThreadRenderStats& stats_tls() {
    return t_holder.c;
}

void RenderStats_Flush() {
    std::lock_guard<std::mutex> g(s_regMutex);
    // Reset bucket totals; readers sum the per-thread contributions
    // accumulated since the last flush.
    fds_stats::g_clipperEntered = fds_stats::g_clipNeedZ = fds_stats::g_clipNeed2D = 0;
    fds_stats::g_clipNoClip = fds_stats::g_clipEmitted = 0;
    fds_stats::g_mipEntered = fds_stats::g_mipFastUniform = fds_stats::g_mipSplit = 0;
    for (auto* c : s_registry) {
        if (c->polysRendered) {
            g_renderedPolys.fetch_add(c->polysRendered, std::memory_order_relaxed);
            c->polysRendered = 0;
        }
        if (c->fillerPixelcount != 0.0) {
            double cur = FillerPixelcount.load(std::memory_order_relaxed);
            while (!FillerPixelcount.compare_exchange_weak(
                       cur, cur + c->fillerPixelcount,
                       std::memory_order_relaxed)) {}
            c->fillerPixelcount = 0.0;
        }
        for (unsigned k = 0; k < kClipSrcCount; ++k) {
            fds_stats::g_clipEnteredSrcCum[k] += c->clipperEntered[k];
            fds_stats::g_clipNoClipSrcCum[k]  += c->clipNoClip[k];
            fds_stats::g_clipEmittedSrcCum[k] += c->clipEmitted[k];
            fds_stats::g_clipperEntered += c->clipperEntered[k]; c->clipperEntered[k] = 0;
            fds_stats::g_clipNoClip     += c->clipNoClip[k];     c->clipNoClip[k]     = 0;
            fds_stats::g_clipEmitted    += c->clipEmitted[k];    c->clipEmitted[k]    = 0;
        }
        fds_stats::g_clipNeedZCum   += c->clipNeedZ;
        fds_stats::g_clipNeed2DCum  += c->clipNeed2D;
        fds_stats::g_clipNeedZ      += c->clipNeedZ;        c->clipNeedZ      = 0;
        fds_stats::g_clipNeed2D     += c->clipNeed2D;       c->clipNeed2D     = 0;
        fds_stats::g_mipEnteredCum     += c->mipEntered;
        fds_stats::g_mipFastUniformCum += c->mipFastUniform;
        fds_stats::g_mipSplitCum       += c->mipSplit;
        fds_stats::g_mipEntered     += c->mipEntered;       c->mipEntered     = 0;
        fds_stats::g_mipFastUniform += c->mipFastUniform;   c->mipFastUniform = 0;
        fds_stats::g_mipSplit       += c->mipSplit;         c->mipSplit       = 0;
        // The mip histogram is CUMULATIVE for the process — unlike the bucket
        // totals above it is not zeroed at the top of Flush, so a scene-end
        // flush mid-run doesn't discard it before RenderStats_MipReport runs.
        for (int m = 0; m < 16; ++m) {
            fds_stats::g_mipLevelFaces[m] += c->mipLevelFaces[m]; c->mipLevelFaces[m] = 0;
            fds_stats::g_mipLevelPix[m]   += c->mipLevelPix[m];   c->mipLevelPix[m]   = 0.0;
        }
        fds_stats::g_mipNomip   += c->mipNomip;    c->mipNomip   = 0;
        fds_stats::g_mipNegArea += c->mipNegArea;  c->mipNegArea = 0;
        fds_stats::g_mipBigFace += c->mipBigFace;  c->mipBigFace = 0;
    }
}

void RenderStats_MipReport() {
    RenderStats_Flush();
    double totPix = 0.0; dword totFaces = 0;
    for (int m = 0; m < 16; ++m) {
        totPix   += fds_stats::g_mipLevelPix[m];
        totFaces += fds_stats::g_mipLevelFaces[m];
    }
    if (totFaces == 0) return;
    fprintf(stderr, "[MIP] histogram (cumulative, all frames rendered this process)\n");
    fprintf(stderr, "[MIP]  level |      draws   %%draws |        area px    %%area\n");
    for (int m = 0; m < 16; ++m) {
        if (!fds_stats::g_mipLevelFaces[m]) continue;
        fprintf(stderr, "[MIP]  %5d | %10u   %5.1f%% | %13.0f   %5.1f%%\n", m,
                fds_stats::g_mipLevelFaces[m],
                100.0 * double(fds_stats::g_mipLevelFaces[m]) / double(totFaces),
                fds_stats::g_mipLevelPix[m],
                totPix > 0.0 ? 100.0 * fds_stats::g_mipLevelPix[m] / totPix : 0.0);
    }
    fprintf(stderr, "[MIP]  total | %10u          | %13.0f\n", totFaces, totPix);
    fprintf(stderr, "[MIP]  branches: entered=%u fastUniform=%u split=%u | nomip=%u "
                    "negSignedArea=%u bigFace(|area|>=thresh)=%u\n",
            fds_stats::g_mipEnteredCum, fds_stats::g_mipFastUniformCum, fds_stats::g_mipSplitCum,
            fds_stats::g_mipNomip, fds_stats::g_mipNegArea, fds_stats::g_mipBigFace);
    fflush(stderr);
}

void RenderStats_MipReportAtExit() {
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(&RenderStats_MipReport); });
}

const char* ClipSrcName(ClipSrc s) {
    switch (s) {
    case ClipSrc::ForwardTiled:   return "RenderInner (fwd, tile job)";
    case ClipSrc::ForwardInline:  return "RenderForwardRegionInline";
    case ClipSrc::DeferredTiled:  return "RenderInnerMekalele (gbuffer)";
    case ClipSrc::DeferredXpar:   return "RenderInnerDeferredTransparent";
    case ClipSrc::DeferredInline: return "MekaleleFillRegionInline (RTT)";
    case ClipSrc::DeferredStrip:  return "xpar strip raster (surf kernel)";
    case ClipSrc::ShadowMap:      return "Shadows.cpp depth raster";
    default:                      return "(untagged)";
    }
}

void RenderStats_ClipReport() {
    RenderStats_Flush();
    dword ent = 0, none = 0, emit = 0;
    for (unsigned k = 0; k < kClipSrcCount; ++k) {
        ent  += fds_stats::g_clipEnteredSrcCum[k];
        none += fds_stats::g_clipNoClipSrcCum[k];
        emit += fds_stats::g_clipEmittedSrcCum[k];
    }
    if (!ent) return;
    fprintf(stderr,
        "[CLIP] per-(face, tile) clipper census — cumulative over every\n"
        "[CLIP] FrustumClipper::Render this process. Each entry copies 3x140 B of\n"
        "[CLIP] Vertex into the worker clipper's scratch, stamps this FACE's UV on\n"
        "[CLIP] the copies and this TILE's visibility flags, then clips.\n");
    fprintf(stderr, "[CLIP] %-32s %11s %11s %6s %11s %6s %11s %6s\n",
            "dispatcher", "entered", "no-clip", "%", "emitted", "%", "rejected", "%");
    for (unsigned k = 0; k < kClipSrcCount; ++k) {
        const dword e = fds_stats::g_clipEnteredSrcCum[k];
        if (!e) continue;
        const dword n = fds_stats::g_clipNoClipSrcCum[k];
        const dword m = fds_stats::g_clipEmittedSrcCum[k];
        const dword r = (e > n + m) ? (e - n - m) : 0;
        const double inv = 100.0 / double(e);
        fprintf(stderr, "[CLIP] %-32s %11u %11u %5.1f%% %11u %5.1f%% %11u %5.1f%%\n",
                ClipSrcName(ClipSrc(k)), e, n, n * inv, m, m * inv, r, r * inv);
    }
    {
        const dword r = (ent > none + emit) ? (ent - none - emit) : 0;
        const double inv = 100.0 / double(ent);
        fprintf(stderr, "[CLIP] %-32s %11u %11u %5.1f%% %11u %5.1f%% %11u %5.1f%%\n",
                "TOTAL", ent, none, none * inv, emit, emit * inv, r, r * inv);
    }
    fprintf(stderr, "[CLIP]   no-clip  = wholly inside this tile rect (no Z and no 2D bit)\n");
    fprintf(stderr, "[CLIP]   emitted  = the clip manufactured at least one vertex\n");
    fprintf(stderr, "[CLIP]   rejected = clipped away to nothing; the copy + stamp bought no pixels\n");
    fprintf(stderr, "[CLIP]   needZ %u (%.1f%%)  need2D %u (%.1f%%)  |  mip entered %u"
                    " fastUniform %u split %u\n",
            fds_stats::g_clipNeedZCum,  100.0 * double(fds_stats::g_clipNeedZCum)  / double(ent),
            fds_stats::g_clipNeed2DCum, 100.0 * double(fds_stats::g_clipNeed2DCum) / double(ent),
            fds_stats::g_mipEnteredCum, fds_stats::g_mipFastUniformCum, fds_stats::g_mipSplitCum);
    fflush(stderr);
}

void RenderStats_ClipReportAtExit() {
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(&RenderStats_ClipReport); });
}

}  // namespace fds

#else  // FDS_RENDER_STATS_ENABLED == 0

// Stub so callers can still link RenderStats_Flush(). No-op.
namespace fds {
PerThreadRenderStats& stats_tls() {
    static PerThreadRenderStats dummy;
    return dummy;
}
void RenderStats_Flush() {}
void RenderStats_MipReport() {}
void RenderStats_MipReportAtExit() {}
void RenderStats_ClipReport() {}
void RenderStats_ClipReportAtExit() {}
const char* ClipSrcName(ClipSrc) { return ""; }
}  // namespace fds

#endif
