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

// ── Stage B1: blaster fire table (pure function of CurFrame) ────────────────
// The combat spine. A BURST is ship2 firing `count` bolts at Ship1, one every
// `spacingFrames`, starting at `fireFrame` (frames == CurFrame; chase plays
// 1 authored frame ≈ 1 tick). Each bolt aims at Ship1's spline position
// `leadFrames` ahead of ITS OWN fire time, plus an authored miss offset
// expressed in TARGET-BOUNDING-RADIUS fractions (resolved to world units in the
// chase driver — this header stays engine-free). A bolt reaches its aim in
// `flightFrames`. Everything here is data; the driver evaluates the ship splines.
//
// Determinism: state is never accumulated. "Which bolts/flashes exist at t and
// where" is reconstructed from this table each frame (BlasterBoltsAliveAt /
// BlasterFlashesAt), so a snapshot that jumps Timer to t reproduces it exactly.
struct BlasterBurst {
    float fireFrame;      // CurFrame at which the burst's first bolt fires
    int   count;          // bolts in the burst
    float spacingFrames;  // frames between consecutive bolts
    float leadFrames;     // aim at target spline (bolt fireFrame + this)
    float flightFrames;   // frames the bolt takes to reach its aim point
    float missX, missY, missZ;  // aim offset, in target bounding-radius fractions
    float r, g, b;        // bolt colour (ship2 fires hot orange by default)
};

// The default chase fire table: bursts aligned to Ship1's authored evasive
// clusters (t≈1408, 1495, 1623) plus an opening sighting burst. Deterministic,
// hardcoded (the FdsMuzzle keyword + authored fire events are a later batch).
const std::vector<BlasterBurst>& BlasterFireTable();

// One bolt reconstructed at frame t. The driver resolves the world endpoints:
//   muzzle = ship2 world transform @ fireFrame, applied to wing `wing`
//   aim    = ship1 centre @ aimFrame  +  miss·(ship1 world radius)
//   pos    = lerp(muzzle, aim, u),  dir = normalize(aim - muzzle)
struct BoltState {
    float fireFrame;   // ship2 transform sample frame (this bolt's muzzle)
    float aimFrame;    // ship1 transform sample frame (fireFrame + leadFrames)
    float u;           // 0..1 fraction along the flight at t
    int   wing;        // 0/1 muzzle selector (alternates per bolt)
    float missX, missY, missZ;
    float r, g, b;
};

// One flash reconstructed at frame t (fountain bolt_flash envelope, pure-t).
struct FlashState {
    int   kind;        // 0 = muzzle (ship2 @ fireFrame, wing), 1 = impact (aim)
    float fireFrame;   // muzzle sample frame
    float aimFrame;    // impact sample frame (= fireFrame + leadFrames)
    int   wing;
    float missX, missY, missZ;
    float intensity;   // 0..1 exp-decay envelope value at t
    float r, g, b;
};

// Fill `outBolts` with every bolt alive at frame t (0 ≤ t-fireFrame <
// flightFrames); clears it first. Pure function of t.
void BlasterBoltsAliveAt(const std::vector<BlasterBurst>& table, float t,
                         std::vector<BoltState>& outBolts);

// Fill `outFlashes` with every muzzle/impact flash active at frame t (clears it
// first). `decay` = exp rate per frame, `window` = frames after the event past
// which the flash is dropped. intensity = exp(-decay · age) for 0 ≤ age < window.
void BlasterFlashesAt(const std::vector<BlasterBurst>& table, float t,
                      float decay, float window,
                      std::vector<FlashState>& outFlashes);

} // namespace chase
