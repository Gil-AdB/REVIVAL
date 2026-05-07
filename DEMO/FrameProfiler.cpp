#include "FrameProfiler.h"

#include <Base/FDS_DECS.H>
#include <Base/FDS_VARS.H>

#include <algorithm>
#include <cstdio>

namespace {

double nsToMs(std::int64_t v) { return static_cast<double>(v) / 1e6; }

double percentile(std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1) + 0.5);
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return nsToMs(sorted[idx]);
}

} // namespace

double FrameProfiler::cumulativeMs(int section) const {
    return nsToMs(sectionTotal_[section]);
}

FrameProfiler::FrameProfiler(const char* sceneName) : sceneName_(sceneName) {
    sceneStart_ = clock::now();
    for (int i = 0; i < PROF_NUM; ++i) samples_[i].reserve(2048);
    frameTotals_.reserve(2048);
}

void FrameProfiler::enter(int section) {
    sectionStart_[section] = clock::now();
    activeSection_ = section;
}

void FrameProfiler::leave(int section) {
    auto now = clock::now();
    std::int64_t delta = std::chrono::duration_cast<ns>(now - sectionStart_[section]).count();
    sectionTotal_[section] += delta;
    currentFrame_[section] += delta;
    activeSection_ = -1;
}

void FrameProfiler::switchTo(int section) {
    auto now = clock::now();
    if (activeSection_ >= 0) {
        std::int64_t delta = std::chrono::duration_cast<ns>(now - sectionStart_[activeSection_]).count();
        sectionTotal_[activeSection_] += delta;
        currentFrame_[activeSection_] += delta;
    }
    sectionStart_[section] = now;
    activeSection_ = section;
}

void FrameProfiler::beginFrame() {
    for (int i = 0; i < PROF_NUM; ++i) currentFrame_[i] = 0;
    frameStart_ = clock::now();
}

void FrameProfiler::endFrame() {
    auto now = clock::now();
    ++numFrames_;

    std::int64_t frameTotal = 0;
    for (int i = 0; i < PROF_NUM; ++i) {
        samples_[i].push_back(currentFrame_[i]);
        frameTotal += currentFrame_[i];
    }
    frameTotals_.push_back(std::chrono::duration_cast<ns>(now - frameStart_).count());

    // Refresh overlay aggregates: cumulative mean per section.
    std::int64_t scenarioTotal = 0;
    for (int i = 0; i < PROF_NUM; ++i) scenarioTotal += sectionTotal_[i];
    overlayTotalMs_ = static_cast<float>(nsToMs(scenarioTotal) / numFrames_);
    for (int i = 0; i < PROF_NUM; ++i) {
        overlayMeanMs_[i] = static_cast<float>(nsToMs(sectionTotal_[i]) / numFrames_);
        overlayPercent_[i] = scenarioTotal
            ? static_cast<float>(100.0 * sectionTotal_[i] / scenarioTotal)
            : 0.0f;
    }
}

int FrameProfiler::drawOverlay(int scrollY,
                                double polysPerFrame,
                                double mPxPerFrame,
                                double mPxPerSec) const {
    char buf[128];
    snprintf(buf, sizeof(buf), "%.3f Mpx/frame", mPxPerFrame);
    scrollY = OutTextXY(VPage, 0, scrollY + 15 * g_fontScale, buf, 255);
    snprintf(buf, sizeof(buf), "%.3f Mpx/sec", mPxPerSec);
    scrollY = OutTextXY(VPage, 0, scrollY + 15 * g_fontScale, buf, 255);
    snprintf(buf, sizeof(buf), "%d polys/frame", static_cast<int>(polysPerFrame));
    scrollY = OutTextXY(VPage, 0, scrollY + 15 * g_fontScale, buf, 255);
    for (int i = 0; i < PROF_NUM; ++i) {
        snprintf(buf, sizeof(buf), "%s %3.1fms (%3.1f%%)",
                 kNames[i], overlayMeanMs_[i], overlayPercent_[i]);
        scrollY = OutTextXY(VPage, 0, scrollY + 15 * g_fontScale, buf, 255);
    }
    snprintf(buf, sizeof(buf), "TOTL %3.1fms", overlayTotalMs_);
    scrollY = OutTextXY(VPage, 0, scrollY + 15 * g_fontScale, buf, 255);
    return scrollY;
}

void FrameProfiler::dump() const {
    if (numFrames_ == 0) return;

    auto wallMs = nsToMs(std::chrono::duration_cast<ns>(clock::now() - sceneStart_).count());

    std::int64_t sectionSum = 0;
    for (int i = 0; i < PROF_NUM; ++i) sectionSum += sectionTotal_[i];

    // FPS stats from frame totals.
    std::vector<std::int64_t> fps = frameTotals_;
    std::sort(fps.begin(), fps.end());
    double fpsMean = numFrames_ * 1000.0 / wallMs;
    double frameMin = nsToMs(fps.front());
    double frameMax = nsToMs(fps.back());
    double frameP50 = percentile(fps, 0.50);
    double frameP95 = percentile(fps, 0.95);

    // stderr is unbuffered by default — survives a crashing modplayer or a
    // forced window-close better than stdout.
    fprintf(stderr, "\n==== profiler: %s ====\n", sceneName_);
    fprintf(stderr, "frames=%u  wall=%.1fs  mean_fps=%.1f  frame_ms min/p50/p95/max = %.2f/%.2f/%.2f/%.2f\n",
           numFrames_, wallMs / 1000.0, fpsMean,
           frameMin, frameP50, frameP95, frameMax);
    fprintf(stderr, "%-6s  %8s  %6s  %8s  %8s  %8s  %8s\n",
           "sect", "mean_ms", "%", "min_ms", "p50_ms", "p95_ms", "max_ms");

    // Sort section indices by total time desc so the dominant sections come first.
    int order[PROF_NUM];
    for (int i = 0; i < PROF_NUM; ++i) order[i] = i;
    std::sort(order, order + PROF_NUM, [&](int a, int b) {
        return sectionTotal_[a] > sectionTotal_[b];
    });

    for (int k = 0; k < PROF_NUM; ++k) {
        int i = order[k];
        std::vector<std::int64_t> s = samples_[i];
        std::sort(s.begin(), s.end());
        double mean = nsToMs(sectionTotal_[i]) / numFrames_;
        double pct  = sectionSum ? (100.0 * sectionTotal_[i] / sectionSum) : 0.0;
        double mn   = nsToMs(s.front());
        double mx   = nsToMs(s.back());
        double p50  = percentile(s, 0.50);
        double p95  = percentile(s, 0.95);
        fprintf(stderr, "%-6s  %8.3f  %5.1f%%  %8.3f  %8.3f  %8.3f  %8.3f\n",
               kNames[i], mean, pct, mn, p50, p95, mx);
    }
    fprintf(stderr, "%-6s  %8.3f\n", "TOTL", nsToMs(sectionSum) / numFrames_);
    fflush(stderr);
}
