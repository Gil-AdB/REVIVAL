#include "ChaseEvents.h"

#include <algorithm>
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

} // namespace chase
