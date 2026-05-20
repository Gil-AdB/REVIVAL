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

    // Draws the cached aggregates onto VPage starting at (0, scrollY). Returns
    // the new scrollY after the block. Caller is responsible for gating on
    // g_profilerActive.
    int drawOverlay(int scrollY,
                    double polysPerFrame,
                    double mPxPerFrame,
                    double mPxPerSec) const;

    // Prints scene-end stats to stdout. Cheap; safe to call once in cleanup().
    void dump() const;

    // For overlay FPS line (caller may also want raw frame count).
    std::uint32_t frames() const { return numFrames_; }

    // Mean per-frame total (ms), refreshed at endFrame(). Use this for the
    // on-screen FPS overlay — the previous Timer-based formula quantized
    // to 10 ms ticks and had an off-by-one over its 20-sample window, so
    // it disagreed with TOTL/mean_fps in the scene-end dump.
    float meanFrameMs() const { return overlayTotalMs_; }

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

    clock::time_point frameStart_;
    clock::time_point sceneStart_;

    // Cached for overlay/dump. Computed at endFrame() so reads are cheap.
    float overlayMeanMs_[PROF_NUM] = {};
    float overlayPercent_[PROF_NUM] = {};
    float overlayTotalMs_ = 0.0f;

    static constexpr const char* kNames[PROF_NUM] = {
        "ZCLR", "SKY", "ANIM", "XFRM", "LGHT", "SORT", "RNDR", "FLIP"
    };
};

// Drain + reset the deferred-skybox accumulator (DeferredLighting.cpp).
// Returns elapsed ns since the previous drain. Scene drivers call this
// once per frame to feed PROF_SKY when --deferred-skybox is on; the
// existing PROF_SKY hooks around RenderSkyCube show 0 ms in that mode
// because RenderSkyCube becomes a no-op.
std::int64_t DeferredSkybox_TakeFrameNs();
