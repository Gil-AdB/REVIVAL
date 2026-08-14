#pragma once

#include <cstddef>

// ── --mem_census: a walk of every LARGE allocation the engine holds ─────────
//
// WHY THIS EXISTS. Two memory-size defects were found by hand in August 2026
// and both were invisible until someone went looking:
//
//   943d644 — the static shadow lightmap was `numFaces × lmRes² × numOmnis`
//             BYTES, fully touched at init. Size scaled with FACE COUNT and a
//             flat 128²/face, not with SURFACE AREA, so tessellating a wall
//             multiplied the store ~300× for nothing: 19.4 GB displaced /
//             5.6 GB shipping on a 64 GB machine.
//   af1f8f8 — one logical shadow texel (depth + polyId) lived in four parallel
//             u16 planes 512 KB apart: 8 cache lines per tap, ~1 GB of line
//             traffic per frame. Packing to one u32 plane bought -1.0 ms.
//
// Neither was findable from a profile — the first is a startup + RSS cost the
// frame timer never sees, the second reads as "the lighting wave is slow". The
// only way to catch the next one is to be able to ASK, on any run, of any
// scene: what is resident, how big is it, and WHICH VARIABLE is it a product
// of. That last column is the whole point. A number alone says "0.5 GB"; the
// formula says "0.5 GB *because it scales with face count*", which is what
// makes a wrong-variable defect self-announcing.
//
// USAGE
//   ./DEMO --snapshot=greets@t=1588 --out=/tmp/x --deferred --mem_census
//   ./DEMO --bench=... --mem_census --mem_census_frame=30
//
// The report fires ONCE, at the end of scene tick `--mem_census_frame`
// (default 1 = after the first full frame, by which point every init
// allocation AND every lazy first-frame bake — env panoramas, SH probe, mip
// first-touch — has happened). Ask for a later frame to catch anything that
// grows over time.
//
// COST WHEN OFF: one relaxed bool load per tick. Reporters are never called,
// `add` is never called, nothing is allocated. BYTE-NULL by construction —
// this module only READS sizes and writes to stderr.
//
// ── ADDING A SUBSYSTEM: one function + one line ────────────────────────────
//
//   static void MemCensus_MyThing() {
//       fds::MemCensus::add("mything", "the buffer", bytes, /*touched=*/true,
//                           "W=%d x H=%d x u32(4)", W, H);
//   }
//   FDS_MEMCENSUS_REPORTER(MemCensus_MyThing);
//
// Put it in the SAME translation unit as the data so file-static buffers are
// reachable without widening anyone's header. `add` is a no-op when the flag
// is off, so the reporter body needs no guard of its own.

namespace fds {

class MemCensus {
public:
    // FeatureFlags::mem_census(). Cheap (one array load); reporters do not
    // need to check it — `add` already does.
    static bool enabled();

    // Record one allocation. `bytes` is what is RESIDENT (capacity × sizeof,
    // not size × sizeof — a vector that shrank still owns its block).
    // `touched` = "every byte is written at least once", which is the
    // difference between address space and RSS; the lightmap defect was
    // 19.4 GB of TOUCHED, and that distinction is why it mattered.
    // `formulaFmt` is printf-style and MUST name the variables, e.g.
    // "faces=%u x lmRes=%d^2 x omnis=%d" — not "big".
    static void add(const char *subsystem, const char *name, size_t bytes,
                    bool touched, const char *formulaFmt, ...)
        __attribute__((format(printf, 5, 6)));

    // Self-registration. See FDS_MEMCENSUS_REPORTER below.
    using Reporter = void (*)();
    struct Reg { explicit Reg(Reporter fn); };

    // Run every registered reporter and print the table. Normally driven by
    // tick(); exposed so a caller can force a census at a chosen moment.
    static void report(const char *label);

    // One call per scene tick, from FrameProfiler::beginFrame. Fires the
    // one-shot report when the tick counter reaches --mem_census_frame.
    static void tick();
};

} // namespace fds

#define FDS_MEMCENSUS_REPORTER(fn) \
    namespace { const ::fds::MemCensus::Reg fn##_memcensus_reg_(&fn); }
