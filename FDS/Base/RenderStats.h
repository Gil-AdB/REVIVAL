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

namespace fds {

struct PerThreadRenderStats {
    dword  polysRendered    = 0;
    double fillerPixelcount = 0.0;
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

// Convenience macros for hot-loop call sites. Expand to nothing when
// stats are compile-disabled, so the TLS write disappears entirely.
#if FDS_RENDER_STATS_ENABLED
  #define FDS_STATS_INC_POLYS()        (::fds::stats_tls().polysRendered++)
  #define FDS_STATS_ADD_PIXELS(area)   (::fds::stats_tls().fillerPixelcount += (area))
#else
  #define FDS_STATS_INC_POLYS()        ((void)0)
  #define FDS_STATS_ADD_PIXELS(area)   ((void)(area))
#endif

} // namespace fds
