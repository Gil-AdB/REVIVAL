#pragma once
//
// ChaseEvents — music-sync foundation for the chase scene (Stage S0 of
// docs/CHASE_UPGRADE_PLAN.md). PLACEMENT-AGNOSTIC scaffolding: chase does not
// yet have a slot in the track (it never shipped in the 1998 demo), so nothing
// here assumes a particular song position. It is the infra + determinism
// contract that Stage B1 (blasters) and B2 (hit particles) build on, and the
// §8.B fix for snapshot determinism of stateful systems.
//
// The design (see §8.A/§8.B):
//   1. A BEAT-MAP maps a musical position (order:row) -> scene-tick
//      (centiseconds, the demo's Timer unit). Built offline, deterministically,
//      by tools/build_beatmap.py scanning the song's tempo/speed schedule.
//   2. An EVENT TABLE is authored in MUSICAL units and RESOLVED to scene-tick
//      windows via the beat-map AT LOAD (plus a scene-configurable
//      music-start offset).
//   3. Runtime is a PURE FUNCTION of t: "what is active at scene-tick t" is
//      RECONSTRUCTED from the resolved table every call — never accumulated
//      across ticks. That is what makes it both beat-locked AND
//      snapshot-deterministic (the RunChaseSnapshot harness jumps Timer to t
//      and ticks once; a stateful pool would not survive that — a pure-t
//      reconstruction does).
//
#include <cstdint>
#include <vector>

namespace chase {

// ── Beat-map: musical position (order:row) -> scene-tick (centiseconds) ──────
struct Beatmap {
    struct Entry { int order; int row; int32_t tick; };
    std::vector<Entry> entries;   // sorted ascending by (order, row)
    bool ok = false;

    // Resolve a musical position to its scene-tick. Exact match if present,
    // otherwise the nearest entry at-or-before (order:row) (linear-in-time
    // interpolation is not needed at row granularity). Returns -1 if the map
    // is empty / the position precedes the first entry.
    int32_t tickFor(int order, int row) const;
};

// Load a beat-map produced by tools/build_beatmap.py. Missing/unreadable file
// -> ok=false, entries empty (caller falls back). Never throws.
bool Beatmap_Load(const char* path, Beatmap& out);

// ── Event: resolved, active over the half-open scene-tick window [tStart,tEnd)
// `kind` and `p[]` are opaque to S0 — B1/B2 define the vocabulary (fire, hit,
// camfx, ...). S0 only proves the timing/reconstruction contract.
struct Event {
    int32_t tStart = 0;
    int32_t tEnd   = 0;   // half-open; tEnd <= tStart => never active
    int     kind   = 0;
    float   p[4]   = {0, 0, 0, 0};
};

// Load a MUSICAL-units event table and resolve each row to a scene-tick window
// via the beat-map. Line format ('#' begins a comment, blank lines ignored):
//
//     <order>:<row>  <kind>  <durTicks>  [p0 p1 p2 p3]
//
//   order:row  musical position (looked up in `bm`)
//   kind       integer event kind (opaque here)
//   durTicks   window length in scene-ticks; tEnd = tStart + durTicks
//   p0..p3     optional float params (default 0)
//
// `musicStartTick` is the scene-tick that the song's start position maps to
// (0 today — chase's music placement is undecided; when it is chosen this is
// the only knob that moves the whole table). Missing/empty file -> out empty,
// returns true (a no-op table is valid). Returns false only on a real parse
// error or an order:row absent from the beat-map.
bool Events_Load(const char* path, const Beatmap& bm, int32_t musicStartTick,
                 std::vector<Event>& out);

// THE CONTRACT. Pure function of t: append to `active` every event whose
// [tStart,tEnd) contains t. No state, no accumulation — identical for a given
// (table, t) regardless of how t was reached. This is what a snapshot at t
// reproduces byte-for-byte and what B1/B2 evaluate per frame.
void Events_ActiveAt(const std::vector<Event>& evs, int32_t t,
                     std::vector<const Event*>& active);

// Throwaway self-proof table (no file I/O) for the chase_event_test flag:
// a single event active ONLY at t == anchor (window [anchor, anchor+1)), so a
// snapshot at `anchor` shows a marker and anchor-1 / anchor+1 do not. Proves
// the pure-t reconstruction appears/clears deterministically at a boundary.
std::vector<Event> Events_TestTable(int32_t anchor);

} // namespace chase
