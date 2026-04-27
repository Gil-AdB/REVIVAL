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
