// Deferred path for the GPU benchmark: G-buffer -> cube shadow bake -> PBR
// lighting -> ACES tonemap, with PER-PASS GPU timestamps.
//
// Kept separate from GpuBenchMain.mm's Phase 2 albedo arm so that number stays
// reproducible. See docs/GPU_BENCHMARK_PLAN.md §3.

#pragma once

#import <Metal/Metal.h>

#include "SceneIngest.h"

#include <cstdint>

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
    // DIAGNOSTIC. Read the baked cube for this light back to the CPU and print
    // the raw stored floats (per face: NaN/Inf count, cleared count, min/max, and
    // the decoded world distance), then write a 3x2 face atlas PPM. This exists
    // because "the tap returns shadowed everywhere" has many possible causes and
    // exactly one of them is "there is nothing valid in the cube" -- which is
    // cheap to settle by LOOKING instead of adjusting conventions.
    // Omni flare sprites. ON: they are what the DEMO reference's bright pools
    // actually are (additive 256^2 procedural sprites, FILLERS.CPP), not omni
    // surface lighting. flareGain is a DIAL, default 1.0 = the CPU's own scale.
    bool  flares = true;
    float flareGain = 1.0f;
    // Parity dial with the CPU's --no-nmap (FeatureFlags no_nmap): skip the
    // normal-map perturbation in the G-buffer so both arms can be compared on
    // geometric normals alone. Diagnostic — the shipped look keeps it true.
    bool  nmap = true;
    // WORKLOAD PARITY (docs/GPU_BENCHMARK_PLAN.md §6.2 "other known gaps").
    // cull: BACKFACE-cull the main G-buffer pass, as Transform_Objects does
    //   (Transform.cpp:2434). The shadow bake deliberately does NOT cull —
    //   shadow_backface_cull defaults 0 because single-sided walls must still
    //   occlude — so matching that is parity, not an omission.
    // shadowCull: per-cube-FACE frustum cull of shadow batches, the analogue
    //   of the CPU's per-pass mesh cull. Both default ON so the comparison
    //   table is not measuring work the CPU never does; --no-cull /
    //   --no-shadow_cull price them.
    bool  cull = true;
    bool  shadowCull = true;
    // Headless CPU-side per-frame profile (animation vs upload), N iters.
    // 0 = off. Exists so the comparison table can carry the CPU-side split
    // WITHOUT a --window run (visible runs are the user's to launch).
    int   cpuProf = 0;
    // Bloom. greets sets bloom ON with bloom_intensity 2.0 (GREETS.CPP:1168-9)
    // and the global bloom_threshold default is 200 in the CPU's linear 0-255
    // radiance scale — divided by 255 on the way into this arm's 0..1 buffer.
    bool  bloom = true;
    float bloomThreshold = 200.0f / 255.0f;
    float bloomIntensity = 2.0f;
    // INTERACTIVE WINDOW. Opens a real SDL2 window with a CAMetalLayer, animates
    // the scene through FDS's own Animate_Objects, and overlays live per-pass GPU
    // ms. Never enabled unless asked for -- offscreen stays the default so a
    // measurement run never pops a window.
    bool  interactive = false;
    int   winW = 1280, winH = 720;
    // Demo-timer rate in centiseconds per second. g_FrameTime is a 100 Hz clock
    // (RENDER.CPP), so 100 is real time.
    float timeScale = 100.0f;
    bool  freeFly = true;              // else follow the authored camera spline
    // Auto-close after N frames. 0 = run until ESC. Exists so the window path is
    // testable without a human at the keyboard, and so a short visible run can be
    // bounded rather than killed.
    int   winFrames = 0;
    const LoadOptions *loadOpt = nullptr;   // needed to Reanimate per frame
    int   dumpCube = -1;
    std::string dumpCubePath;
    // GROUND-TRUTH PROBE. For one WORLD point, print for every cube light:
    //   (a) the true nearest shadow-casting triangle between light and point,
    //       found by ray-casting the SAME triangles the bake rasterised, with
    //       the mesh + material NAME — no convention arithmetic involved;
    //   (b) a host-side replica of the shader's tap (face pick, uv, stored
    //       depth, decoded distance, ref, bias, pass/fail).
    // If (a) and (b) disagree, the bug is the tap's conventions. If they agree,
    // the cube is telling the truth and the occlusion is geometry. Written
    // because two rounds of this investigation were spent inferring geometry
    // from an 8x8 grid of decoded depths.
    bool  probe = false;
    float probePoint[3] = {0, 0, 0};
    // Same idea keyed on a SCREEN PIXEL: build that pixel's camera ray from the
    // very constants the vertex shader uses, ray-cast ALL geometry, and then
    // replay the lighting pass's own per-light gate on the hit — distance vs
    // range, NoL, attenuation, and the cube tap — printing which test failed.
    // "This pixel is not lit by light N" had no answerable form before this.
    bool  probePx = false;
    int   probePxXY[2] = {0, 0};
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
bool RunDeferred(Scene &scene, const DeferredOptions &opt,
                 const std::string &shaderPath, DeferredResult &out);

}  // namespace gpubench
