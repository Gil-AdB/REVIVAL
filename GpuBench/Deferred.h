// Deferred path for the GPU benchmark: G-buffer -> cube shadow bake -> PBR
// lighting -> ACES tonemap, with PER-PASS GPU timestamps.
//
// Kept separate from GpuBenchMain.mm's Phase 2 albedo arm so that number stays
// reproducible. See docs/GPU_BENCHMARK_PLAN.md §3.

#pragma once

#import <Metal/Metal.h>

#include "SceneIngest.h"

#include <string>
#include <vector>

namespace gpubench {

struct DeferredOptions {
    int  warmup = 60;
    int  iters = 300;
    bool shadows = true;
    int  staticShadowRes = 512;    // GREETS.CPP greets_omni_shadow_res
    int  movingShadowRes = 128;    // GREETS.CPP greets_moving_omni_shadow_res
    float exposure = 1.0f;
    int  viz = -1;                 // -1 = normal render; else fs_viz mode
    // Re-bake EVERY cube every frame instead of only the moving ones. Off by
    // default because greets caches static omni cubes (Omni_StaticShadow); on,
    // this measures the full cold bake.
    bool rebakeAll = false;
    // How many passes to run: 1 = G-buffer only, 2 = +lighting, 3 = +tonemap.
    // The per-encoder timestamps OVERLAP on Apple GPUs (a pass's vertex stage can
    // start before the previous pass's fragment stage retires), so they are upper
    // bounds and do NOT sum to the frame. Differencing clean whole-frame
    // GPUStartTime/GPUEndTime intervals across stage counts is the trustworthy
    // decomposition -- same discipline as the albedo arm's --no-draw floor.
    int  stages = 3;
    // MEASUREMENT-ONLY. See the shader comment on lightRangeScale.
    float lightRangeScale = 1.0f;
    int   vizLight = -1;          // -1 = all lights; else isolate this index
    std::string outPath;
};

// Median/p5/p95 for one pass, in ms.
struct PassTiming {
    std::string name;
    double median = 0, p5 = 0, p95 = 0;
};

struct DeferredResult {
    std::vector<PassTiming> passes;
    PassTiming              frame;
    int                     shadowCubes = 0;
    int                     shadowFaces = 0;
    long                    shadowTexels = 0;
    int                     litLights = 0;
    int                     movingCubes = 0;
    double                  staticBakeMs = 0.0;   // one-time cached bake
};

// Returns false on hard failure. Renders offscreen; never opens a window.
bool RunDeferred(const Scene &scene, const DeferredOptions &opt,
                 const std::string &shaderPath, DeferredResult &out);

}  // namespace gpubench
