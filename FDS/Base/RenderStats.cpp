#include "RenderStats.h"

#if FDS_RENDER_STATS_ENABLED

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

// Forward decls of the global accumulators (defined elsewhere).
extern std::atomic<dword>  g_renderedPolys;
extern std::atomic<double> FillerPixelcount;

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
