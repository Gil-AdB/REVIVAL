#include "ChaseEvents.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace chase {

int32_t Beatmap::tickFor(int order, int row) const {
    if (entries.empty()) return -1;
    // entries are sorted by (order,row); find the last entry at-or-before.
    const int64_t key = (int64_t(order) << 20) | uint32_t(row);
    int32_t best = -1;
    for (const Entry& e : entries) {
        const int64_t k = (int64_t(e.order) << 20) | uint32_t(e.row);
        if (k <= key) best = e.tick;
        else break;
    }
    return best;
}

bool Beatmap_Load(const char* path, Beatmap& out) {
    out.entries.clear();
    out.ok = false;
    if (!path || !*path) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        // Strip comments and skip blanks.
        char* hash = std::strchr(line, '#');
        if (hash) *hash = '\0';
        int order = 0, row = 0;
        long long tick = 0;
        // "order row tick [bpm speed ...]" — extra columns ignored.
        if (std::sscanf(line, "%d %d %lld", &order, &row, &tick) == 3) {
            Beatmap::Entry e;
            e.order = order;
            e.row = row;
            e.tick = int32_t(tick);
            out.entries.push_back(e);
        }
    }
    std::fclose(f);

    std::sort(out.entries.begin(), out.entries.end(),
              [](const Beatmap::Entry& a, const Beatmap::Entry& b) {
                  if (a.order != b.order) return a.order < b.order;
                  return a.row < b.row;
              });
    out.ok = !out.entries.empty();
    return out.ok;
}

bool Events_Load(const char* path, const Beatmap& bm, int32_t musicStartTick,
                 std::vector<Event>& out) {
    out.clear();
    if (!path || !*path) return true;   // no table => valid no-op
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return true;                // missing => valid no-op

    bool okAll = true;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        char* hash = std::strchr(line, '#');
        if (hash) *hash = '\0';

        int order = 0, row = 0, kind = 0;
        long long durTicks = 0;
        float p[4] = {0, 0, 0, 0};
        // "<order>:<row> <kind> <durTicks> [p0 p1 p2 p3]"
        int n = std::sscanf(line, "%d:%d %d %lld %f %f %f %f",
                            &order, &row, &kind, &durTicks,
                            &p[0], &p[1], &p[2], &p[3]);
        if (n < 4) continue;   // blank / partial line

        const int32_t base = bm.tickFor(order, row);
        if (base < 0) { okAll = false; continue; }   // unknown musical pos

        Event e;
        e.tStart = base + musicStartTick;
        e.tEnd   = e.tStart + int32_t(durTicks);
        e.kind   = kind;
        for (int i = 0; i < 4; ++i) e.p[i] = p[i];
        out.push_back(e);
    }
    std::fclose(f);
    return okAll;
}

void Events_ActiveAt(const std::vector<Event>& evs, int32_t t,
                     std::vector<const Event*>& active) {
    active.clear();
    for (const Event& e : evs) {
        if (t >= e.tStart && t < e.tEnd) active.push_back(&e);
    }
}

std::vector<Event> Events_TestTable(int32_t anchor) {
    // One event, active for exactly one scene-tick at `anchor`. kind/p carry a
    // fixed signature so the stamped marker is deterministic across runs.
    Event e;
    e.tStart = anchor;
    e.tEnd   = anchor + 1;   // active only at t == anchor
    e.kind   = 1;
    e.p[0] = 1.0f;           // marker channel (red), see the snapshot stamp
    return {e};
}

// ── Stage B1: blaster fire table ────────────────────────────────────────────
const std::vector<BlasterBurst>& BlasterFireTable() {
    // ship2 fires hot orange/red (contrasts the blue water). Bursts are placed
    // so bolts are in flight / impacting through Ship1's authored evasive
    // clusters (dense rotation keys at ~1408, ~1495, ~1623 — that IS why it
    // wiggles there). Miss offsets are in Ship1-bounding-radius fractions
    // (resolved to world in the driver); a negative Y drops near-misses toward
    // the water. Palette + cadence are user picks later; these are sane defaults.
    static const std::vector<BlasterBurst> kTable = {
        // fireFrame count spacing lead flight   miss(x,y,z)          colour(r,g,b)     kind
        // A near-continuous pursuit barrage across the scene — bursts every
        // ~90-150 frames, denser + longer through the authored evasive clusters
        // (t≈1408/1495/1623). Mix of hull near-hits (small offset), hull sparks,
        // WATER near-misses (kind=1: aim the sea beside/ahead of Ship1 → vertical
        // splash columns marching toward it), and — through the mm7 gorge
        // (t≈1074-1300) — MOUNTAIN hits (kind=2: bolts that overshoot the ship
        // spark off the nearest canyon wall; the driver resolves the wall point).
        {  340.0f,  4,  9.0f,  46.0f, 36.0f,   0.55f, -1.30f,  0.35f,  1.00f, 0.34f, 0.07f      }, // opening range-finding (hull miss)
        {  420.0f,  5,  7.0f,  50.0f, 40.0f,   0.90f,  0.00f,  0.70f,  0.75f, 0.85f, 1.00f, 1   }, // WATER near-miss column
        {  600.0f,  5,  7.0f,  42.0f, 32.0f,   0.35f,  0.20f,  0.40f,  1.00f, 0.42f, 0.11f      }, // near-hit
        {  720.0f,  6,  7.0f,  44.0f, 33.0f,  -0.60f, -1.40f,  0.25f,  1.00f, 0.30f, 0.06f      },
        {  830.0f,  6,  6.0f,  52.0f, 42.0f,  -1.00f,  0.00f,  0.50f,  0.70f, 0.85f, 1.00f, 1   }, // WATER near-miss march
        {  980.0f,  6,  7.0f,  45.0f, 34.0f,   0.60f, -1.20f,  0.30f,  1.00f, 0.34f, 0.07f      },
        { 1075.0f,  6,  6.0f,  40.0f, 30.0f,   0.30f,  0.20f,  0.25f,  1.00f, 0.40f, 0.10f, 2   }, // MOUNTAIN — enter the gorge, walk fire up the wall
        { 1160.0f,  7,  5.0f,  38.0f, 28.0f,  -0.40f,  0.30f, -0.20f,  1.00f, 0.36f, 0.08f, 2   }, // MOUNTAIN — the other wall
        { 1230.0f,  6,  6.0f,  48.0f, 40.0f,   0.85f,  0.00f, -0.80f,  0.72f, 0.86f, 1.00f, 1   }, // WATER near-miss column
        { 1285.0f,  6,  5.0f,  40.0f, 30.0f,   0.55f,  0.25f,  0.30f,  1.00f, 0.38f, 0.09f, 2   }, // MOUNTAIN — parting gorge wall
        { 1340.0f,  7,  5.0f,  42.0f, 30.0f,  -0.45f, -1.30f,  0.20f,  1.00f, 0.30f, 0.06f      }, // cluster 1 (~1408)
        { 1440.0f,  8,  5.0f,  40.0f, 29.0f,   0.20f,  0.10f, -0.30f,  1.00f, 0.44f, 0.12f      }, // cluster 1→2, near-hit
        { 1540.0f,  8,  5.0f,  40.0f, 30.0f,  -0.35f, -0.20f,  0.35f,  1.00f, 0.40f, 0.10f      }, // cluster 2 (~1495-1560)
        { 1620.0f,  8,  4.0f,  44.0f, 30.0f,  -0.55f, -1.00f,  0.45f,  1.00f, 0.28f, 0.05f      }, // cluster 3 (~1623)
        { 1690.0f,  5,  5.0f,  38.0f, 26.0f,   0.25f,  0.15f,  0.20f,  1.00f, 0.46f, 0.14f      }, // parting shots
    };
    return kTable;
}

void BlasterBoltsAliveAt(const std::vector<BlasterBurst>& table, float t,
                         std::vector<BoltState>& outBolts) {
    outBolts.clear();
    for (const BlasterBurst& b : table) {
        for (int i = 0; i < b.count; ++i) {
            const float fFire = b.fireFrame + float(i) * b.spacingFrames;
            const float age   = t - fFire;
            if (age < 0.0f || age >= b.flightFrames) continue;
            BoltState s;
            s.fireFrame = fFire;
            s.aimFrame  = fFire + b.leadFrames;
            s.u         = b.flightFrames > 0.0f ? age / b.flightFrames : 0.0f;
            s.wing      = i & 1;
            s.missX = b.missX; s.missY = b.missY; s.missZ = b.missZ;
            s.r = b.r; s.g = b.g; s.b = b.b;
            s.water = b.water;
            outBolts.push_back(s);
        }
    }
}

void BlasterFlashesAt(const std::vector<BlasterBurst>& table, float t,
                      float decay, float window,
                      std::vector<FlashState>& outFlashes) {
    outFlashes.clear();
    for (const BlasterBurst& b : table) {
        for (int i = 0; i < b.count; ++i) {
            const float fFire       = b.fireFrame + float(i) * b.spacingFrames;
            const float aimFrame    = fFire + b.leadFrames;
            const float impactFrame = fFire + b.flightFrames;
            // Muzzle flash at the shot leaving ship2.
            const float ageM = t - fFire;
            if (ageM >= 0.0f && ageM < window) {
                FlashState f;
                f.kind = 0; f.fireFrame = fFire; f.aimFrame = aimFrame; f.wing = i & 1;
                f.missX = b.missX; f.missY = b.missY; f.missZ = b.missZ;
                f.intensity = std::exp(-decay * ageM);
                f.r = b.r; f.g = b.g; f.b = b.b;
                f.water = b.water;
                outFlashes.push_back(f);
            }
            // Impact flash at the (fixed, fire-time) aim point.
            const float ageI = t - impactFrame;
            if (ageI >= 0.0f && ageI < window) {
                FlashState f;
                f.kind = 1; f.fireFrame = fFire; f.aimFrame = aimFrame; f.wing = i & 1;
                f.missX = b.missX; f.missY = b.missY; f.missZ = b.missZ;
                f.intensity = std::exp(-decay * ageI);
                f.r = b.r; f.g = b.g; f.b = b.b;
                f.water = b.water;
                outFlashes.push_back(f);
            }
        }
    }
}

void BlasterImpactsAt(const std::vector<BlasterBurst>& table, float t,
                      float particleLife, std::vector<ImpactState>& outImpacts) {
    outImpacts.clear();
    uint32_t bolt = 0;   // running index → unique per-impact seed
    for (const BlasterBurst& b : table) {
        for (int i = 0; i < b.count; ++i, ++bolt) {
            const float fFire       = b.fireFrame + float(i) * b.spacingFrames;
            const float impactFrame = fFire + b.flightFrames;
            const float age = t - impactFrame;
            if (age < 0.0f || age >= particleLife) continue;
            ImpactState s;
            s.aimFrame = fFire + b.leadFrames;
            s.missX = b.missX; s.missY = b.missY; s.missZ = b.missZ;
            s.age = age;
            s.life = particleLife;
            s.seed = bolt * 2654435761u + 0x9e3779b9u;
            s.r = b.r; s.g = b.g; s.b = b.b;
            s.water = b.water;
            outImpacts.push_back(s);
        }
    }
}

} // namespace chase
