#include "RenderStats.h"

#if FDS_RENDER_STATS_ENABLED

#include <algorithm>
#include <atomic>
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
        fds_stats::g_mipEntered     += c->mipEntered;       c->mipEntered     = 0;
        fds_stats::g_mipFastUniform += c->mipFastUniform;   c->mipFastUniform = 0;
        fds_stats::g_mipSplit       += c->mipSplit;         c->mipSplit       = 0;
    }
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
}  // namespace fds

#endif
