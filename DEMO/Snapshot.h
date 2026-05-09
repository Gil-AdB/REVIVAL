#pragma once

#include <cstdint>
#include <string>
#include <vector>

// CLI-driven scene snapshot harness. Renders one or more deterministic
// frames of a scene without the SDL window or audio path so we can diff
// native vs wasm output at the pixel level.
//
// Invocation (parsed by ParseSnapshotArgs):
//   DEMO --snapshot=city@t=1000,5000,10000 [--out=PATH]
//
// For each requested Timer value we drive one tick() of the City scene
// driver and dump VPage as PPM (color) and the Z-buffer as PGM.

struct SnapshotConfig {
    std::string scene;
    std::vector<int32_t> timestamps;
    std::string outDir = ".";
};

bool ParseSnapshotArgs(int argc, const char* argv[], SnapshotConfig& cfg);

int RunCitySnapshot(const SnapshotConfig& cfg, int xres, int yres);

// Drives Glat through a deterministic Timer sweep and records per-tick
// state to <outDir>/glat_trace.csv. Useful for cross-platform / cross-
// commit comparison of animation behaviour.
int RunGlatTrace(const SnapshotConfig& cfg, int xres, int yres);

// Renders FillerTest's two-triangle quad through TheOtherBarry per seed,
// dumping VPage/Z to <outDir>/filler_t<seed>_color.ppm + _z.pgm. Used to
// reproduce rasterizer-edge / mask divergence between native and wasm in
// isolation from the city pipeline.
int RunFillerTestSnapshot(const SnapshotConfig& cfg, int xres, int yres);

// Reflection-direction diagnostic. Initialises City (with FDS_DEBUG_PANORAMA
// forced on so every per-building panorama is the synthetic quadrant-coloured
// test image), then positions the camera at six fixed points around one
// chosen building and renders one snapshot per camera pose. Each output PNG
// is named by the camera's offset direction relative to the building's
// centre. By reading off the colours that appear in the building's window
// reflections, we can verify which (eu, ev) the lookup formula picks for a
// given reflected ray, isolated from the (typically large) parallax errors
// that come from sampling a static cube-map at unrelated camera positions.
//
//   DEMO --snapshot=refltest [--out=PATH]
int RunReflectionTest(const SnapshotConfig& cfg, int xres, int yres);

// Synthetic rasterizer benchmark. Repeatedly renders the FillerTest fixture
// and reports ms/iter + Mpx/s. Same fixture across native and wasm so the
// numbers compare apples-to-apples.
//
// Invocation:
//   DEMO --bench=raster                 -> default 200 iters, seed 0
//   DEMO --bench=raster@iters=1000      -> 1000 iters
//   DEMO --bench=raster@iters=500,seed=2-> custom iters + seed
//
// Under wasm: run via `node --cpu-prof DEMO_snapshot.js --bench=raster`
// and V8 dumps a .cpuprofile that opens in Chrome DevTools Performance.
struct BenchConfig {
    std::string kind;
    int iters = 200;
    int seed = 0;
    // Used by --bench=scene: which scene driver + Timer value to drive.
    std::string scene;
    int32_t ts = 0;
    // Optional override of resolution (otherwise rev.cfg's value is used).
    // Used by --bench=flip to vary canvas size without editing rev.cfg.
    int xres = 0;
    int yres = 0;
};
bool ParseBenchArgs(int argc, const char* argv[], BenchConfig& cfg);
int RunRasterBench(const BenchConfig& cfg, int xres, int yres);

// Drives a real scene's tick() repeatedly at a fixed Timer value and reports
// total/mean ms. Used to attribute the wasm rasterizer cost to scene-shaped
// triangle workloads (small overdrawn triangles, varied UVs/lighting) versus
// the synthetic full-screen quad of --bench=raster.
//
// Invocation:
//   DEMO --bench=scene@scene=city,t=1961,iters=200
//   DEMO --bench=scene@scene=greets,t=600,iters=200
int RunSceneBench(const BenchConfig& cfg, int xres, int yres);

// FLIP pipeline microbenchmark — isolates the per-frame SDL stages we
// run inside V_Flip (unlock, bar fills, RenderCopy, RenderPresent, lock).
// Forces SDL_RENDERER_SOFTWARE so native numbers track the wasm path's
// cost model. Useful for iterating on the present path without rerunning
// the full demo.
//
// Invocation:
//   DEMO --bench=flip                       -> 3024x1696, 200 iters
//   DEMO --bench=flip@iters=1000            -> 1000 iters
//   DEMO --bench=flip@iters=500,xres=1920,yres=1080
int RunFlipBench(const BenchConfig& cfg, int xres, int yres);

// Greets dynamic-text-texture pipeline harness. Self-contained: doesn't
// load SCENES/GREETS.FLD or any 3D state, just fills a 256x256 CodeImage
// with a centered synthetic glyph, runs the wobbler (Code_GP) + smear
// (Smear_GP) + AlphaBlend chain N times with fixed scalex/scaley, and
// prints centroids of each intermediate buffer. Diff the output between
// native and wasm — centered input + symmetric pipeline should give
// centroids near (128.0, 128.0). Any deviation pinpoints which step
// breaks symmetry.
//
// Invocation:
//   DEMO --bench=greets-pipe              -> 100 iters, scalex=scaley=0
//   DEMO --bench=greets-pipe@iters=200
int RunGreetsPipeBench(const BenchConfig& cfg, int xres, int yres);
