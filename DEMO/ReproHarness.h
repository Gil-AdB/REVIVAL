#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ── Headless INTERACTIVE repro harness ──────────────────────────────────────
//
// WHY THIS EXISTS (read before reaching for --snapshot again)
//
// Every diagnostic in this tree renders through --snapshot, which pins Timer,
// pins the camera and runs ONE tick from a cold scene. Twice now a user-reported
// visual defect has failed to reproduce under --snapshot at the user's exact
// pose, exact flags and exact resolution while being plainly visible in his
// interactive run. A single cold tick cannot express anything that ACCUMULATES
// across frames, and it cannot express the interactive clock:
//
//   * REV.CPP forces g_fineSceneClock = false for --snapshot/--bench, so the
//     sub-tick float scene clock (g_FrameTimeF) is pinned to the integer Timer.
//     An interactive run drives it through its EMA rate estimator, so at any
//     given integer t the fine clock is generally NOT equal to t.
//   * REV.CPP forces g_occlSnapshotInert = true for --snapshot, disabling the
//     prev-frame chunk occlusion cull outright.
//   * Anything with per-frame history — Face::LastMip + --mip_hysteresis, the
//     mirror RTT, temporal froxel reprojection (--fast_fog_froxel_temporal),
//     async dynamic shadow bakes joined one frame late, one-shot probe bakes
//     whose result depends on WHERE the camera was when they first ran — is
//     cold on tick 1 and converged after a few hundred.
//
// So this harness runs the REAL per-frame path: the real scene driver, the real
// tick(), the real transport controls. It differs from an interactive session
// only in that no window is opened and the keys are scripted. It advances the
// scene clock through the SAME code path F1/F2 scrubbing uses
// (SceneDriver::tickSceneTimer) by writing the transport scancodes into the
// global Keyboard[] array — it does NOT poke Timer behind the driver's back,
// which is exactly the mistake that made the old TimerProc scrub a silent no-op
// (see the block comment on TimerProc in REV.CPP).
//
// INVOCATION
//
//   ./DEMO --repro=greets@t=2993 --out=DIR [scene flags...]
//
// Reaches scene time 2993 the way a user does — by holding F2 from
// --repro_from with the scene PAUSED — holds the pose for --repro_settle
// frames, then dumps. Multiple dump points are allowed and are visited in
// ascending order during one continuous session:
//
//   ./DEMO --repro=greets@t=2900,2993,3100 --out=DIR
//
// Camera: honours FDS_GREETS_CAM (the same six-float
// "px,py,pz,fx,fy,fz" pose the interactive tick prints on F9), re-pinned every
// frame so the whole scripted session sits at the user's pose exactly as his
// does while he scrubs.
//
// Output: <outDir>/repro_<scene>_t<NNNNNN>.ppm, plus _sNN.ppm for the
// --repro_seq consecutive-frame series.
//
// THE DIAGNOSTIC DIAL: --repro_from is how you MEASURE how much history a
// defect needs. If a defect reproduces with --repro_from=0 but not with
// --repro_from=2900, the cause accumulates over more than ~9 scrub frames, and
// the bisection on --repro_from bounds it.
//
// See docs/INTERACTIVE_REPRO.md.

struct ReproConfig {
    std::string scene;
    std::vector<int32_t> dumpAt;   // scene times to dump at, ascending
    std::string outDir = ".";
};

// Parses --repro=<scene>@t=T1,T2,... and --out=DIR. Returns true if --repro=
// was present. Deliberately mirrors ParseSnapshotArgs' shape.
bool ParseReproArgs(int argc, const char* argv[], ReproConfig& cfg);

// Runs the scripted interactive session and writes the dumps. Returns a
// process exit code (0 = ok).
int RunRepro(const ReproConfig& cfg, int xres, int yres);
