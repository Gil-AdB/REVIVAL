#include "Base/MemCensus.h"
#include "Base/FeatureFlags.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task.h>
#endif

namespace fds {

namespace {

struct Row {
    std::string subsystem;
    std::string name;
    std::string formula;
    size_t      bytes = 0;
    bool        touched = false;
};

// Function-local statics: construction order is first-use, so a reporter
// registered from any TU's static-init phase is safe.
std::vector<MemCensus::Reporter> &reporters() {
    static std::vector<MemCensus::Reporter> v;
    return v;
}
std::vector<Row> &rows() {
    static std::vector<Row> v;
    return v;
}
std::mutex &mtx() {
    static std::mutex m;
    return m;
}

// Resident set / footprint, for the cross-check the census can never do for
// itself: the sum of what we KNOW about vs what the process actually holds.
// The residual is the interesting number — it is everything this file does
// not yet walk.
size_t procFootprintBytes() {
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return size_t(info.phys_footprint);
#endif
    return 0;
}

const char *humanUnit(double &v) {
    if (v >= 1024.0 * 1024.0 * 1024.0) { v /= 1024.0 * 1024.0 * 1024.0; return "GiB"; }
    if (v >= 1024.0 * 1024.0)          { v /= 1024.0 * 1024.0;          return "MiB"; }
    if (v >= 1024.0)                   { v /= 1024.0;                   return "KiB"; }
    return "B  ";
}

} // namespace

// FDS is a STATIC library, so an object file none of whose symbols are
// referenced is dropped by the linker — taking its self-registering reporters
// with it. Reporters that live beside data already linked for other reasons
// (Mekalele, ShadowMap, Hdr, Shadows, LightmapBake) are safe; a file that
// exists ONLY to hold reporters is not. MemCensusScene.cpp is that file, so it
// gets an explicit anchor called from report(). Any future reporters-only TU
// needs the same two lines — see FDS/Base/MemCensus.h.
void MemCensusScene_Anchor();

bool MemCensus::enabled() { return FeatureFlags::mem_census(); }

MemCensus::Reg::Reg(Reporter fn) { reporters().push_back(fn); }

void MemCensus::add(const char *subsystem, const char *name, size_t bytes,
                    bool touched, const char *formulaFmt, ...) {
    if (!enabled()) return;
    char buf[512];
    va_list ap;
    va_start(ap, formulaFmt);
    std::vsnprintf(buf, sizeof(buf), formulaFmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(mtx());
    rows().push_back(Row{subsystem ? subsystem : "?", name ? name : "?", buf,
                         bytes, touched});
}

void MemCensus::report(const char *label) {
    if (!enabled()) return;
    MemCensusScene_Anchor();          // keeps the reporters-only TU linked
    {
        std::lock_guard<std::mutex> lk(mtx());
        rows().clear();
    }
    // Reporters call add(), which takes the mutex — so run them unlocked.
    for (Reporter r : reporters()) r();

    std::lock_guard<std::mutex> lk(mtx());
    std::vector<Row> &rv = rows();
    std::stable_sort(rv.begin(), rv.end(),
                     [](const Row &a, const Row &b) { return a.bytes > b.bytes; });

    size_t total = 0, totalTouched = 0;
    for (const Row &r : rv) {
        total += r.bytes;
        if (r.touched) totalTouched += r.bytes;
    }

    std::fprintf(stderr,
        "[MEM] ───────────────────────────────────────────────────────────────"
        "──────────────────────────\n");
    std::fprintf(stderr, "[MEM] memory census @ %s\n", label ? label : "?");
    std::fprintf(stderr,
        "[MEM] %14s %10s  T  %-34s %s\n",
        "bytes", "size", "subsystem / buffer", "formula (the variables it scales with)");
    for (const Row &r : rv) {
        if (r.bytes == 0) continue;      // an unallocated buffer is still worth
                                         // knowing about, but not worth a line
        double v = double(r.bytes);
        const char *u = humanUnit(v);
        char qual[80];
        std::snprintf(qual, sizeof(qual), "%s/%s", r.subsystem.c_str(), r.name.c_str());
        std::fprintf(stderr, "[MEM] %14zu %6.2f %s  %c  %-34s %s\n",
                     r.bytes, v, u, r.touched ? 'T' : '.', qual, r.formula.c_str());
    }
    // Zero-byte entries collapsed into one line — "this exists and is empty"
    // is a real answer (e.g. an optional G-buffer plane the flags left off).
    std::string zeros;
    size_t nzero = 0;
    for (const Row &r : rv) {
        if (r.bytes != 0) continue;
        ++nzero;
        if (zeros.size() < 400) {
            if (!zeros.empty()) zeros += ", ";
            zeros += r.subsystem + "/" + r.name;
        }
    }
    if (nzero)
        std::fprintf(stderr, "[MEM] %zu unallocated (0 B): %s%s\n", nzero,
                     zeros.c_str(), nzero > 12 ? " ..." : "");

    double tv = double(total);          const char *tu = humanUnit(tv);
    double wv = double(totalTouched);   const char *wu = humanUnit(wv);
    std::fprintf(stderr,
        "[MEM] CENSUSED TOTAL %14zu (%.2f %s) over %zu buffers; %.2f %s of it TOUCHED\n",
        total, tv, tu, rv.size() - nzero, wv, wu);

    const size_t fp = procFootprintBytes();
    if (fp) {
        double fv = double(fp);                  const char *fu = humanUnit(fv);
        const size_t resid = fp > total ? fp - total : 0;
        double rvv = double(resid);              const char *ru = humanUnit(rvv);
        std::fprintf(stderr,
            "[MEM] process phys_footprint %.2f %s  →  UNCENSUSED RESIDUAL %.2f %s "
            "(code, textures not walked, malloc slack, thread stacks)\n",
            fv, fu, rvv, ru);
    }
    std::fprintf(stderr,
        "[MEM]   T = every byte is written at least once (RSS, not just address "
        "space). Add a subsystem: see FDS/Base/MemCensus.h.\n");
    std::fprintf(stderr,
        "[MEM] ───────────────────────────────────────────────────────────────"
        "──────────────────────────\n");
    std::fflush(stderr);
}

void MemCensus::tick() {
    if (!enabled()) return;
    static int  s_tick = 0;
    static bool s_done = false;
    if (s_done) return;
    ++s_tick;
    int want = FeatureFlags::mem_census_frame();
    if (want < 1) want = 1;
    if (s_tick < want) return;
    s_done = true;
    char label[64];
    std::snprintf(label, sizeof(label), "end of scene tick %d", s_tick);
    report(label);
}

} // namespace fds
