#include "VizCycle.h"
#include "WorldAabb.h"        // WireViz_DrawOverlay + the arming probes
#include "VizLegend.h"        // VizLegend_Build: the per-mode colour key

#include <Base/FDS_VARS.H>    // VPage, XRes/YRes, g_fontScale, Active_Font
#include <Base/FDS_DECS.H>    // OutTextXY
#include <Base/FeatureFlags.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

// Defined in FDS/FILLERS/Mekalele.cpp. Declared here rather than including
// Mekalele.h: that header is a 3.6k-line SIMD template unit, and this TU only
// needs to ask "was the plane allocated?".
bool EngineGBuffer_HasAlbedoPlane();
bool EngineGBuffer_HasNormalPlane();

namespace fds {

// Consumer-side availability probes, defined beside the viz functions whose
// early-outs they mirror (FDS/RENDER/LightmapBake.cpp, FDS/RENDER/EnvBake.cpp).
bool LightmapViz_Available();
bool NormalViz_Available();
bool EnvReflectionViz_Available();
bool EnvMapViz_Available();
bool EnvMapGpuViz_Available();

const char* g_vizLabel = nullptr;

namespace {

using FF = FeatureFlags;

// ── availability probes ───────────────────────────────────────────────────
// "Would selecting this mode show anything in THIS run?" Evaluated once, at
// the first key press — i.e. after scene init, so bake-time data is settled.
//
// Probing the FLAG alone proved insufficient and was caught by measurement:
// with --shadow_lightmap on but nothing baked, --shadow_lightmap_viz=1 renders
// a byte-identical frame. Where a consumer has its own early-outs, the probe is
// that consumer's, defined next to it (LightmapBake.cpp / EnvBake.cpp) so the
// two cannot drift.
bool availAlways()   { return true; }
bool availAlbedo()   { return FF::deferred() && EngineGBuffer_HasAlbedoPlane(); }
bool availGbNormal() { return FF::deferred() && NormalViz_Available(); }
bool availDisp1()    { return DisplaceViz_HasData(); }
bool availDisp2()    { return DisplaceViz_HasErrorData(); }
bool availSeam()     { return PomSeamViz_HasData(); }
bool availHorizon()  { return FF::deferred() && FF::pom_horizon(); }
bool availShadowLm() { return FF::deferred() && LightmapViz_Available(); }
bool availEnvRefl()  { return EnvReflectionViz_Available(); }
bool availEnvMap()   { return EnvMapViz_Available(); }
bool availEnvMapGpu(){ return EnvMapGpuViz_Available(); }
bool availAa()       { return FF::deferred() && FF::aa(); }
bool availSsao()     { return FF::deferred() && FF::ssao(); }

struct VizEntry {
    const char* label;      // on-screen + stderr name
    const char* flag;       // FeatureFlags name written via setParamFromText
    int         value;      // value that selects this mode
    bool      (*avail)();   // availability probe (see above)
    bool        needTexFilter;  // rides the filtered-albedo plane: also force
                                // texture_filter>=1, or the kernel never reads it
    const char* missing;    // printed when the entry is dropped
};

// Cycle ORDER. Index 0 of the built list is always "off"; this table is what
// follows it. Ordered by how often a look review reaches for them.
//
// Deliberately NOT in the cycle:
//   --viz_tangent/_normal/_geonormal/_matid/_pmid  — the deferred kernel caches
//     these into `static const bool` on first use, so a runtime flip cannot
//     take effect. CLI only.
//   --draw_aabbs — its overlay is only invoked from the greets tick (and needs
//     that tick's WorldAabb_UpdateScene call), so it would be a silent no-op
//     everywhere else. CLI + greets only.
//   --pom_path_viz=2 — record-only mode for offline flip analysis, nothing to see.
const VizEntry kEntries[] = {
    { "WIRE over image",         "wire_viz",            1, availAlways,   false, nullptr },
    { "WIRE dimmed",             "wire_viz",            2, availAlways,   false, nullptr },
    { "WIRE by material/chunk",  "wire_viz",            3, availAlways,   false, nullptr },
    { "WIRE facing (back=red)",  "wire_viz",            4, availAlways,   false, nullptr },
    { "DISPLACE magnitude",      "displace_viz",        1, availDisp1,    false,
      "no displacement bake recorded - needs --greets_displace plus --viz_arm "
      "(or --displace_viz=1) at STARTUP; the bake runs at scene init" },
    { "DISPLACE height error",   "displace_viz",        2, availDisp2,    false,
      "no height-error data - the bake computes it only when --displace_viz=2 "
      "was already set at STARTUP (MeshOps gates the whole computation)" },
    { "POLY ownership",          "poly_viz",            1, availAlbedo,   false,
      "needs the filtered-albedo G-buffer plane: --viz_arm or --texture_filter "
      "at STARTUP (it is allocated at framebuffer resize)" },
    { "POM height field",        "pom_viz",             1, availAlbedo,   true,
      "needs the filtered-albedo plane: --viz_arm or --texture_filter at STARTUP" },
    { "POM per-face mip",        "pom_mip_viz",         1, availAlbedo,   true,
      "needs the filtered-albedo plane: --viz_arm or --texture_filter at STARTUP" },
    { "POM march paths",         "pom_path_viz",        1, availAlbedo,   true,
      "needs the filtered-albedo plane: --viz_arm or --texture_filter at STARTUP" },
    { "POM horizon term",        "pom_horizon_viz",     1, availHorizon,  false,
      "needs --pom_horizon (the horizon bake) at STARTUP" },
    { "POM seam classes",        "pom_seam_viz",        1, availSeam,     false,
      "no classified seams recorded - needs --pom_shell at STARTUP" },
    { "NORMAL geometric",        "nmap_viz",            1, availGbNormal, false,
      "needs --deferred (reads the G-buffer normal plane)" },
    { "NORMAL shaded (TBN)",     "nmap_viz",            2, availGbNormal, false,
      "needs --deferred (reads the G-buffer normal plane)" },
    { "NORMAL raw texel",        "nmap_viz",            3, availGbNormal, false,
      "needs --deferred (reads the G-buffer normal plane)" },
    { "SHADOW lightmap id",      "shadow_lightmap_viz", 1, availShadowLm, false,
      "needs --shadow-lightmap (the static lightmap bake) at STARTUP" },
    { "SHADOW lightmap faceIdx", "shadow_lightmap_viz", 2, availShadowLm, false,
      "needs --shadow-lightmap at STARTUP" },
    { "SHADOW bary world delta", "shadow_lightmap_viz", 9, availShadowLm, false,
      "needs --shadow-lightmap at STARTUP" },
    { "AA edge map",             "aa_viz",              1, availAa,       false,
      "needs --aa" },
    { "SSAO term",               "ssao_debug",          1, availSsao,     false,
      "needs --ssao (and --no-hdr, or the tonemap overwrites it)" },
    { "ENV pano #1",             "env_refl_viz",        1, availEnvRefl,  false,
      "needs --env_refl (the panorama bake) at STARTUP" },
    // ── the ENV-MAP INSPECTOR ────────────────────────────────────────────
    // Unlike "ENV pano #1" (probe #1 only, thumbnail, unlabelled) these page
    // through EVERY baked probe with F / Shift+F and name what is on screen.
    // Both entries write the same flag, so stepping X between them keeps the
    // selected probe — env_map_probe is deliberately NOT in this table and so
    // is never cleared by clearAll().
    { "ENV probe faces (3x2)",   "env_map_viz",         1, availEnvMap,   false,
      "no env probe with pixel data in this run - needs --env_refl (on by "
      "default) plus a surface that qualifies (Reflection > 0 or a metalness "
      "map); probes bake during the scene's first frames" },
    { "ENV probe mip chain",     "env_map_viz",         2, availEnvMap,   false,
      "no env probe with pixel data in this run - see 'ENV probe faces'" },
    { "ENV probe CPU|GPU",       "env_map_viz",         3, availEnvMapGpu,false,
      "no GpuBench atlas on disk - produce one with "
      "`../build-gpu/GpuBench/GpuBench --fld=SCENES/GREETS.FLD --pass=deferred "
      "--dump_env_cube`, which writes /tmp/gpuenv_<material>.ppm" },
};
constexpr int kNumEntries = int(sizeof(kEntries) / sizeof(kEntries[0]));

std::vector<int> g_active;      // indices into kEntries, availability-filtered
int  g_pos      = 0;            // 0 = off, else 1 + index into g_active
bool g_built    = false;
// True while WE hold texture_filter on for a needTexFilter entry. Without this
// the companion leaked: cycling past --pom_viz left texture filtering on for the
// rest of the session, silently changing every later render (measured — the
// frame mean did not return to its pre-cycle value on reaching "off").
bool g_forcedTexFilter = false;

// Clear every viz this table can set, back to its compile-time default (0 for
// all of them) AND clear the explicitly-set mark, so leaving the cycle hands
// the flag back to defaults/param-scripts exactly like the console's unset.
// Also drops a borrowed texture_filter. We only ever borrow it from 0 (see
// apply), so restoring 0 + clearing the mark is an exact restore; a user who
// launched with --texture_filter=N is never touched.
void clearAll() {
    for (const VizEntry& e : kEntries) FF::unsetParam(e.flag);
    if (g_forcedTexFilter) {
        g_forcedTexFilter = false;
        FF::setParamFromText("texture_filter", "0");
        FF::clearSetMark("texture_filter");
    }
}

void buildList() {
    g_built = true;
    g_active.clear();
    std::vector<int> dropped;
    for (int i = 0; i < kNumEntries; ++i) {
        if (kEntries[i].avail()) g_active.push_back(i);
        else                     dropped.push_back(i);
    }
    std::fprintf(stderr, "\n[VIZ] runtime viz cycle: X = next, Shift+X = previous. "
                 "%zu modes available in this run:\n", g_active.size());
    std::fprintf(stderr, "[VIZ]   0/%zu  (off)\n", g_active.size());
    for (size_t k = 0; k < g_active.size(); ++k) {
        const VizEntry& e = kEntries[g_active[k]];
        std::fprintf(stderr, "[VIZ] %3zu/%zu  %-24s (--%s=%d)\n",
                     k + 1, g_active.size(), e.label, e.flag, e.value);
    }
    for (int i : dropped) {
        const VizEntry& e = kEntries[i];
        std::fprintf(stderr, "[VIZ]   skipped  %-24s (--%s=%d): %s\n",
                     e.label, e.flag, e.value, e.missing ? e.missing : "unavailable");
    }
    // Seed the position from whatever a CLI flag already selected, so the first
    // press advances from the mode on screen instead of jumping to the top.
    for (size_t k = 0; k < g_active.size(); ++k) {
        const VizEntry& e = kEntries[g_active[k]];
        const FeatureFlags::ParamRef r = FeatureFlags::findParam(e.flag);
        int cur = -1;
        if (r.type == FeatureFlags::ParamType::Int)  cur = FeatureFlags::g_intVals[r.index];
        if (r.type == FeatureFlags::ParamType::Bool) cur = FeatureFlags::g_boolVals[r.index] ? 1 : 0;
        if (cur == e.value) { g_pos = int(k) + 1; break; }
    }
    if (g_pos) std::fprintf(stderr, "[VIZ] starting at %d (already set on the command line)\n", g_pos);
}

void apply() {
    clearAll();
    if (g_pos <= 0 || g_pos > int(g_active.size())) {
        g_pos = 0;
        g_vizLabel = nullptr;
        std::fprintf(stderr, "[VIZ] 0/%zu  (off)\n", g_active.size());
        return;
    }
    const VizEntry& e = kEntries[g_active[g_pos - 1]];
    char v[16];
    std::snprintf(v, sizeof v, "%d", e.value);
    FF::setParamFromText(e.flag, v);
    const char* extra = "";
    if (e.needTexFilter && FF::texture_filter() <= 0) {
        // The deferred kernel only READS the filtered-albedo plane when
        // texture_filter>0 (or poly_viz) — see DeferredSurfaceKernel's
        // texFilterOn. The plane already exists (that was this entry's
        // availability test), so flipping the flag live is safe; it does
        // change texture sampling to filtered, which is worth saying out loud.
        FF::setParamFromText("texture_filter", "1");
        g_forcedTexFilter = true;   // clearAll() gives it back on the next step
        extra = "  [+ --texture_filter=1 while this mode is up: it rides the filtered-albedo plane]";
    }
    if (std::strcmp(e.flag, "ssao_debug") == 0 && FF::hdr())
        extra = "  [WARNING: --hdr tonemaps over it; relaunch with --no-hdr to see it]";
    g_vizLabel = e.label;
    std::fprintf(stderr, "[VIZ] %d/%zu  %s  (--%s=%d)%s\n",
                 g_pos, g_active.size(), e.label, e.flag, e.value, extra);
}

}  // namespace

void VizCycle_Step(int dir) {
    if (!g_built) buildList();
    if (g_active.empty()) {
        std::fprintf(stderr, "[VIZ] no viz is switchable in this run\n");
        return;
    }
    const int n = int(g_active.size()) + 1;              // + the "off" slot
    g_pos = ((g_pos + (dir >= 0 ? 1 : -1)) % n + n) % n; // wraps both ways
    apply();
}

namespace {

// ── legend painting ───────────────────────────────────────────────────────
// The rows come from VizLegend_Build (which derives them from the renderer's
// own palettes/constants); everything here is just where to put them.
// Nothing in this block runs unless a viz is active, so it is byte-null with
// the flags off — and it lives in the post/overlay path, not in a filler.

int fontScale() { return g_fontScale > 0 ? g_fontScale : 1; }
int glyphH()    { return (Active_Font && Active_Font->Y > 0 ? Active_Font->Y : 8) * fontScale(); }

// Width OutTextXY will actually consume — same per-glyph advance it uses
// ((Len+2)*scale), so the backing panel matches the text instead of guessing.
int textWidth(const char* s) {
    const int fs = fontScale();
    const Font* F = Active_Font;
    if (!F || !F->Len) return int(std::strlen(s)) * 8 * fs;
    int w = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        w += (int(F->Len[*p & 0x7F]) + 2) * fs;
    return w;
}

// Multiply a rect down so white text and saturated chips read over a bright
// frame. Integer scale, one pass, clipped — the legend is a few hundred rows
// of pixels at most.
void dimRect(int x0, int y0, int x1, int y1, uint32_t keep256) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > XRes) x1 = XRes;
    if (y1 > YRes) y1 = YRes;
    dword* out = reinterpret_cast<dword*>(VPage);
    for (int y = y0; y < y1; ++y) {
        dword* row = out + size_t(y) * size_t(XRes);
        for (int x = x0; x < x1; ++x) {
            const dword d = row[x];
            const uint32_t r = (((d >> 16) & 0xFFu) * keep256) >> 8;
            const uint32_t g = (((d >>  8) & 0xFFu) * keep256) >> 8;
            const uint32_t b = (( d        & 0xFFu) * keep256) >> 8;
            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void fillRect(int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > XRes) x1 = XRes;
    if (y1 > YRes) y1 = YRes;
    dword* out = reinterpret_cast<dword*>(VPage);
    const dword c = 0xFF000000u | (rgb & 0x00FFFFFFu);
    for (int y = y0; y < y1; ++y) {
        dword* row = out + size_t(y) * size_t(XRes);
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

// Paint the legend as rows stacked UPWARD from `bottomY` (the mode-name line
// when there is one, otherwise the bottom margin). Bottom-left, because the
// resolution readout owns the top-left corner (SDL2.cpp) and the profiler runs
// down that same column; the block is clamped to the lower half so the two can
// never meet, and the tail is dropped rather than allowed to climb.
void drawLegend(const VizLegendRow* rows, int n, int bottomY) {
    const int fs = fontScale();
    const int gh = glyphH();
    // Line pitch: the bitmap font's own height plus air. The 14*fs floor is
    // there because the AFT glyph box (Font::Y) is 8 px at scale 1 while the
    // inked glyphs nearly fill it — pitch = box + 3 rendered as overlapping
    // lines on the first render of this legend.
    const int pitch = std::max(gh + 5 * fs, 14 * fs);
    const int x0 = 6 * fs;
    const int chip = gh;                   // square-ish chip, one text line tall
    const int chipGap = 3 * fs;

    // Clamp to the lower half: above that sits the profiler's column and the
    // resolution readout, and a legend is never worth colliding with those.
    int maxFit = (bottomY - YRes / 2) / pitch;
    if (maxFit < 1) maxFit = 1;
    const int hidden = (n > maxFit) ? (n - maxFit + 1) : 0;   // last slot spent on the note
    const int nDraw  = hidden ? maxFit : n;
    const int bottom = bottomY - 3 * fs;                      // air above the mode name
    const int top    = bottom - nDraw * pitch;

    // Backing panel sized to the widest row (chips + text), so it hugs the
    // legend instead of banding the whole screen width.
    int wMax = 0;
    for (int i = 0; i < nDraw; ++i) {
        const int sw = rows[i].nsw * (chip + chipGap);
        const int w  = sw + textWidth(rows[i].text);
        if (w > wMax) wMax = w;
    }
    dimRect(x0 - 4 * fs, top - 3 * fs, x0 + wMax + 5 * fs, bottom + 2 * fs, 70);

    for (int i = 0; i < nDraw; ++i) {
        const int y = top + i * pitch;
        int x = x0;
        const bool noteRow = hidden && (i == nDraw - 1);
        if (!noteRow) {
            for (int s = 0; s < rows[i].nsw; ++s) {
                fillRect(x, y + fs, x + chip - fs, y + chip, rows[i].sw[s]);
                x += chip + chipGap;
            }
            OutTextXY(VPage, x, y, rows[i].text, 255);
        } else {
            char note[64];
            std::snprintf(note, sizeof note,
                          "(+%d more - full legend on stderr)", hidden);
            OutTextXY(VPage, x, y, note, 255);
        }
    }
}

// Dump the whole legend to stderr the first time a given selection is up. Free,
// never truncated, and copy-pasteable into notes next to a crop.
void dumpLegendOnce(const VizLegendRow* rows, int n, int sig) {
    static int last = -1;
    if (sig == last) return;
    last = sig;
    for (int i = 0; i < n; ++i) {
        if (rows[i].nsw == 0) {
            std::fprintf(stderr, "[VIZ-LEGEND]           %s\n", rows[i].text);
            continue;
        }
        char chips[8 * 9 + 1];
        int p = 0;
        for (int s = 0; s < rows[i].nsw && p < int(sizeof chips) - 9; ++s)
            p += std::snprintf(chips + p, sizeof chips - p, "%06X ",
                               unsigned(rows[i].sw[s] & 0x00FFFFFFu));
        std::fprintf(stderr, "[VIZ-LEGEND] %-9s %s\n", chips, rows[i].text);
    }
}

}  // namespace

void VizCycle_Overlay(Scene* sc) {
    const int wire = FF::wire_viz();
    const char* label = g_vizLabel;

    if (wire > 0) WireViz_DrawOverlay(sc);

    // Self-identifying screenshot: name the cycled mode bottom-left. Only for
    // cycle selections (g_vizLabel) — a CLI-set viz renders exactly as before.
    // --wire_viz is new, so labelling it from the CLI breaks no baseline.
    if (!label && wire > 0) {
        static const char* kWireNames[5] = { "", "WIRE over image", "WIRE dimmed",
                                             "WIRE by material/chunk",
                                             "WIRE facing (back=red)" };
        label = kWireNames[wire > 4 ? 4 : wire];
    }
    if (!VPage || XRes <= 0 || YRes <= 0) return;
    int labelY = YRes - 20 * g_fontScale;
    if (labelY < 0) labelY = 0;
    if (label) {
        char line[96];
        std::snprintf(line, sizeof line, "VIZ: %s  [X/Shift+X]", label);
        OutTextXY(VPage, 6 * g_fontScale, labelY, line, 255);
    }

    // ── the legend for whatever viz is active (CLI-set or cycled) ─────────
    // Unlike the mode NAME above this is not gated on g_vizLabel: a viz set
    // from the command line is exactly the case that needs the colour key,
    // and no gate recipe in docs/SESSION_STATE.md passes a viz flag, so no
    // pin is at risk. --no-viz_legend turns it off.
    if (FF::viz_legend()) {
        VizLegendRow rows[kVizLegendMaxRows];
        int sig = 0;
        const int n = VizLegend_Build(rows, kVizLegendMaxRows, &sig);
        if (n > 0) {
            // With a mode name on screen the legend stacks above it; without
            // one (a viz set purely from the CLI) it takes that line too.
            const int bottomY = label ? labelY : (YRes - 4 * g_fontScale);
            drawLegend(rows, n, bottomY);
            dumpLegendOnce(rows, n, sig);
        }
    }
}

}  // namespace fds
