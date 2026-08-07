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
        fds_stats::g_clipperEntered += c->clipperEntered;   c->clipperEntered = 0;
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
}  // namespace fds

#endif
