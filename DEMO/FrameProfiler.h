#pragma once

#include "Rev.h"

#include <chrono>
#include <cstdint>
#include <vector>

// Per-scene frame profiler. Replaces the open-coded `Profiler[PROF_X] -= Timer`
// pattern that CITY/GREETS/FOUNTAIN used to maintain inline.
//
// Why std::chrono and not the global Timer? Timer ticks at 100Hz (10ms), which
// is coarser than a single 60fps frame. steady_clock gives ~µs resolution, so
// per-frame distribution stats (min/avg/max/p50/p95) are meaningful instead of
// quantized to {0,10,20}ms.

struct VESA_Surface;

class FrameProfiler {
public:
    explicit FrameProfiler(const char* sceneName);

    // Per-section accumulators. Call enter() before the section, leave() after.
    // switchTo() collapses leave(prev) + enter(next) into a single clock read.
    void enter(int section);
    void leave(int section);
    void switchTo(int section);

    // External-source contribution. For work that doesn't run inside an
    // enter()/leave() bracket on the caller's thread (e.g. the deferred
    // skybox dispatched from Render(), which threads its tile work).
    // Adds to both scene-cumulative and current-frame totals so the on-
    // screen overlay and end-of-scene stats stay consistent.
    void addExternalNs(int section, std::int64_t ns);

    // Frame boundaries. beginFrame() before any enter(); endFrame() after the
    // last leave(). endFrame() rolls per-frame deltas into the sample arrays
    // and refreshes the cached overlay aggregates.
    void beginFrame();
    void endFrame();

    // Feed the SCENE-CUMULATIVE render counters once per frame (right
    // before drawOverlay). The profiler differentiates against the
    // previous frame's values and smooths with the same EMA as the
    // timing rows — so polys/frame and Mpx/frame track the CURRENT view
    // instead of a whole-scene mean that dilutes any change across
    // thousands of historical frames. First call (or after a counter
    // reset) seeds the EMA instead of producing a bogus delta.
    void noteCounters(double renderedPolys, double fillerPixels);

    // Draws the cached aggregates onto VPage starting at (0, scrollY). Returns
    // the new scrollY after the block. Caller is responsible for gating on
    // g_profilerActive.
    int drawOverlay(int scrollY) const;

    // Prints scene-end stats to stdout. Cheap; safe to call once in cleanup().
    void dump() const;

    // For overlay FPS line (caller may also want raw frame count).
    std::uint32_t frames() const { return numFrames_; }

    // Trailing-window mean (last ~60 frames). Use this for the on-screen
    // FPS overlay so frame-rate dips show up immediately instead of being
    // smoothed into the scene-cumulative average.
    float meanFrameMs() const { return overlayWindowMs_; }
    // Scene-cumulative mean — the historic value drawOverlay used to
    // print as TOTL.
    float cumulativeFrameMs() const { return overlayTotalMs_; }

    // Scene-cumulative ms in a section. Useful for derived stats like MPx/sec.
    double cumulativeMs(int section) const;

private:
    using clock = std::chrono::steady_clock;
    using ns = std::chrono::nanoseconds;

    const char* sceneName_;
    std::uint32_t numFrames_ = 0;

    // sectionStart_[i] is set by enter(i); leave(i) reads it and adds the
    // delta to sectionTotal_[i] and currentFrame_[i].
    clock::time_point sectionStart_[PROF_NUM] = {};
    std::int64_t sectionTotal_[PROF_NUM] = {}; // ns, scene-cumulative
    std::int64_t currentFrame_[PROF_NUM] = {}; // ns, current frame in flight

    int activeSection_ = -1; // for switchTo()

    // Per-frame samples in nanoseconds. Used for min/max/p50/p95 in dump().
    std::vector<std::int64_t> samples_[PROF_NUM];
    std::vector<std::int64_t> frameTotals_; // wall ns per frame (for FPS stats)
    std::vector<std::int32_t> frameT_;      // g_FrameTime per frame — dump()
                                            // prints the slowest frames' t so a
                                            // heavy stretch can be re-visited
                                            // (G-key dump / --bench ts=<t>)

    clock::time_point frameStart_;
    clock::time_point sceneStart_;

    // Cached for overlay/dump. Computed at endFrame() so reads are cheap.
    float overlayMeanMs_[PROF_NUM] = {};
    float overlayPercent_[PROF_NUM] = {};
    float overlayTotalMs_ = 0.0f;       // scene-cumulative
    float overlayWindowMs_ = 0.0f;      // trailing 60-frame window
    static constexpr int kFpsWindow = 60;
    std::int64_t fpsWindow_[kFpsWindow] = {};
    int fpsWindowIdx_ = 0;
    int fpsWindowCount_ = 0;
    std::int64_t fpsWindowSum_ = 0;

    // noteCounters() state: previous cumulative values + EMA'd per-frame
    // rates for the overlay.
    double prevPolys_ = -1.0;   // -1 = unseeded
    double prevPixels_ = 0.0;
    float  emaPolysPerFrame_ = 0.0f;
    float  emaMPxPerFrame_   = 0.0f;
    float  emaMPxPerSec_     = 0.0f;

    static constexpr const char* kNames[PROF_NUM] = {
        "ZCLR", "SKY", "ANIM", "XFRM", "LGHT", "SORT", "BAKE", "RNDR", "FLIP"
    };
};

// Drain + reset the deferred-skybox accumulator (DeferredVolumetric.cpp).
// Returns elapsed ns since the previous drain. Scene drivers call this
// once per frame to feed PROF_SKY when --deferred-skybox is on; the
// existing PROF_SKY hooks around RenderSkyCube show 0 ms in that mode
// because RenderSkyCube becomes a no-op.
std::int64_t DeferredSkybox_TakeFrameNs();
// Fold the xpar-peel front depth into the main Z so a later additive overlay
// (fountain bolt) is occluded by translucent surfaces too. No-op if no peel.
void Deferred_FoldXparDepthIntoMainZ();
