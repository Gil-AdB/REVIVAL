#include "VizCycle.h"
#include "WorldAabb.h"        // WireViz_DrawOverlay + the arming probes

#include <Base/FDS_VARS.H>    // VPage, XRes/YRes, g_fontScale
#include <Base/FDS_DECS.H>    // OutTextXY
#include <Base/FeatureFlags.h>

#include <cstdio>
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

void VizCycle_Overlay(Scene* sc) {
    const int wire = FF::wire_viz();
    const char* label = g_vizLabel;
    if (wire <= 0 && !label) return;                     // nothing active

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
    if (label && VPage && XRes > 0 && YRes > 0) {
        char line[96];
        std::snprintf(line, sizeof line, "VIZ: %s  [X/Shift+X]", label);
        int y = YRes - 20 * g_fontScale;
        if (y < 0) y = 0;
        OutTextXY(VPage, 6 * g_fontScale, y, line, 255);
    }
}

}  // namespace fds
