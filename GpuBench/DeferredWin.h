// DeferredWin.h — Standalone Direct3D 11 deferred renderer for GpuBench (Windows).
// Ground-truth instrument & GPU benchmark on Windows.

#pragma once

#include "SceneIngest.h"
#include <cstdint>
#include <string>
#include <vector>

namespace gpubench {

struct DeferredOptions {
    int   warmup = 60;
    int   iters = 300;
    bool  shadows = true;
    int   staticShadowRes = 512;
    int   movingShadowRes = 128;
    float exposure = 1.0f;
    int   viz = -1;
    bool  rebakeAll = false;
    int   stages = 3;
    float lightRangeScale = 1.0f;
    int   vizLight = -1;
    bool  flares = true;
    float flareGain = 1.0f;
    bool  nmap = true;
    bool  cull = true;
    bool  shadowCull = true;
    int   cpuProf = 0;
    bool  bloom = true;
    bool  bloomExplicit = false;
    float bloomThreshold = 200.0f / 255.0f;
    float bloomIntensity = 2.0f;
    bool  hdrLinear = true;
    bool  hdrLinearExplicit = false;
    bool  mirror2 = true;
    float mirror2Scale = 0.5f;
    float mirror2MinPx = 16.0f;
    bool  mirror2Stats = false;
    int   tessUniform = 0;
    bool  interactive = false;
    bool  passDeferred = false;
    std::string outPath;
};

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
    double                  staticBakeMs = 0.0;
    long long               tessVerts = 0;
    long long               tessTris = 0;
    long long               tessPatchesLive = 0;
    long long               tessBoundarySegs = 0;
    int                     tessMaxFactor = 0;
    double                  tessFactorMs = 0.0;
};

bool RunDeferredWin(Scene &scene, const DeferredOptions &opt,
                    const std::string &shaderPath, DeferredResult &out);

bool RunAlbedoWin(Scene &scene, const DeferredOptions &opt,
                  const std::string &outPath);

}  // namespace gpubench
