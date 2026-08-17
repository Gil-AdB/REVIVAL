// GpuBenchMainWin.cpp — Standalone Direct3D 11 GPU benchmark for Windows.

#include "DeferredWin.h"
#include "SceneIngest.h"
#include "ParticleReplay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL.h>

namespace {

const char *kUsage =
    "GpuBench — standalone Direct3D 11 GPU benchmark for Windows\n"
    "\n"
    "Run from Runtime/ (asset paths are CWD-relative).\n"
    "\n"
    "  --fld=PATH        scene file            (default SCENES/GREETS.FLD)\n"
    "  --t=N             demo-timer pose       (default 5743)\n"
    "  --cam=\"px,py,pz,fx,fy,fz\"   review pose string\n"
    "  --xres=N --yres=N resolution            (default 1920x1080)\n"
    "  --warmup=N        untimed frames        (default 60)\n"
    "  --iters=N         timed frames          (default 300)\n"
    "  --out=PATH        write a PPM of the last frame (default gpubench.ppm; '' = none)\n"
    "  --pass=albedo|deferred\n"
    "  --window          open an interactive SDL2 window\n"
    "  --help            show this message\n";

} // namespace

int main(int argc, char *argv[]) {
    gpubench::LoadOptions opt;
    gpubench::DeferredOptions defOpt;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::printf("%s", kUsage);
            return 0;
        } else if (arg.rfind("--fld=", 0) == 0) {
            opt.fldPath = argv[i] + 6;
        } else if (arg.rfind("--t=", 0) == 0) {
            opt.demoT = std::atoi(argv[i] + 4);
            opt.demoTExplicit = true;
        } else if (arg.rfind("--cam=", 0) == 0) {
            opt.camPose = argv[i] + 6;
        } else if (arg.rfind("--xres=", 0) == 0) {
            opt.xres = std::atoi(argv[i] + 7);
        } else if (arg.rfind("--yres=", 0) == 0) {
            opt.yres = std::atoi(argv[i] + 7);
        } else if (arg.rfind("--warmup=", 0) == 0) {
            defOpt.warmup = std::atoi(argv[i] + 9);
        } else if (arg.rfind("--iters=", 0) == 0) {
            defOpt.iters = std::atoi(argv[i] + 8);
        } else if (arg.rfind("--out=", 0) == 0) {
            defOpt.outPath = argv[i] + 6;
        } else if (arg == "--window") {
            defOpt.interactive = true;
            defOpt.passDeferred = true;
        } else if (arg == "--pass=deferred") {
            defOpt.passDeferred = true;
        } else if (arg == "--pass=albedo") {
            defOpt.passDeferred = false;
        }
    }

    if (defOpt.interactive) {
        defOpt.passDeferred = true;
    }

    std::fprintf(stderr, "[GPUBENCH] Loading scene '%s' at pose t=%d (%dx%d)…\n",
                 opt.fldPath, opt.demoT, opt.xres, opt.yres);

    gpubench::Scene scene;
    if (!gpubench::Load(scene, opt)) {
        std::fprintf(stderr, "[GPUBENCH] FATAL: could not load scene '%s'\n", opt.fldPath);
        return 1;
    }

    std::fprintf(stderr, "[GPUBENCH] Ingest complete: %zu verts, %u tris, %zu batches, %u textures.\n",
                 scene.verts.size(), scene.faceCount, scene.batches.size(), scene.texturesLoaded);

    if (defOpt.passDeferred) {
        gpubench::DeferredResult res;
        gpubench::RunDeferredWin(scene, defOpt, "shaders", res);
    } else {
        if (defOpt.outPath.empty()) defOpt.outPath = "gpubench.ppm";
        gpubench::RunAlbedoWin(scene, defOpt, defOpt.outPath);
    }

    return 0;
}
