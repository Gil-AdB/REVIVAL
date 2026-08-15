#include "ReproHarness.h"

#include "Rev.h"
#include "Scenes.h"
#include "SceneTick.h"

#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FDS_DEFS.H>
#include <Base/FeatureFlags.h>
#include <Base/Camera.h>
#include <FILLERS/Mekalele.h>
#include <Threads.h>
#include <VESA/Vesa.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <SDL.h>
#endif

extern dword g_profilerActive;

// REV.CPP's free-running scene clock (SDL_AddTimer -> Timer++). Declared here
// rather than in a header because REV.CPP owns main() and exports nothing;
// --repro_play is the only other caller.
int TimerInit(int32_t Freq);

namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void parse_timestamps(std::string_view s, std::vector<int32_t>& out) {
    std::size_t pos = 0;
    while (pos <= s.size()) {
        std::size_t end = s.find(',', pos);
        if (end == std::string_view::npos) end = s.size();
        if (end > pos) {
            std::string token(s.substr(pos, end - pos));
            char* endp = nullptr;
            long v = std::strtol(token.c_str(), &endp, 10);
            if (endp && *endp == '\0') out.push_back(int32_t(v));
            else std::fprintf(stderr, "[REPRO] ignoring non-integer t '%s'\n",
                              token.c_str());
        }
        pos = end + 1;
    }
}

void write_ppm(const char* path, const byte* bgra, int xres, int yres, int bpsl) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr, "[REPRO] fopen('%s') failed: %s\n", path, std::strerror(errno));
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", xres, yres);
    std::vector<unsigned char> row(size_t(xres) * 3);
    for (int y = 0; y < yres; ++y) {
        const dword* src = reinterpret_cast<const dword*>(bgra + size_t(y) * bpsl);
        for (int x = 0; x < xres; ++x) {
            dword px = src[x];                 // VPage is ARGB8888
            row[x * 3 + 0] = (px >> 16) & 0xFF;
            row[x * 3 + 1] = (px >>  8) & 0xFF;
            row[x * 3 + 2] = (px      ) & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[REPRO] wrote %s\n", path);
}

void ensureOutDir(const std::string& outDir) {
    if (outDir.empty() || outDir == ".") return;
    std::string acc;
    for (size_t i = 0; i <= outDir.size(); ++i) {
        if (i == outDir.size() || outDir[i] == '/') {
            if (!acc.empty() && acc != ".") mkdir(acc.c_str(), 0755);
        }
        if (i < outDir.size()) acc.push_back(outDir[i]);
    }
}

void noop_flip(VESA_Surface*) {}

// SceneDriver::tickSceneTimer's paused scrub step, in Timer ticks. Mirrored
// here only to align the start time; the scrub itself is done by the driver.
constexpr int32_t kPausedScrubStep = 10;

// Headless FDS/VESA bootstrap. This is deliberately the harness's OWN copy of
// the snapshot path's initSnapshotEnvironment rather than a shared call: the
// entire point of this harness is that it must not silently inherit whatever
// the snapshot path decides to pin next. Keep it dumb — FDS + a software
// surface + the threadpool, and nothing that changes engine behaviour.
bool initReproEnvironment(int xres, int yres) {
    if (!FDS_Init((unsigned short)xres, (unsigned short)yres, 32)) {
        std::fprintf(stderr, "[REPRO] FDS_Init failed\n");
        return false;
    }
    static VESA_Surface surf = {};
    surf.X = xres;
    surf.Y = yres;
    surf.BPP = 32;
    surf.CPP = 4;
    surf.BPSL = surf.CPP * surf.X;
    surf.PageSize = surf.BPSL * surf.Y;
    const std::size_t zSize = sizeof(word) * std::size_t(xres) * yres;
    surf.Data = (byte*)std::malloc(surf.PageSize);
    surf.Z16  = (byte*)std::malloc(zSize);
    if (!surf.Data || !surf.Z16) {
        std::fprintf(stderr, "[REPRO] malloc framebuffer / Z16 failed\n");
        return false;
    }
    std::memset(surf.Data, 0, surf.PageSize);
    std::memset(surf.Z16,  0, zSize);
    surf.Flip = &noop_flip;

    VESA_VPageExternal(&surf);
    VESA_Surface2Global(MainSurf);
    EngineGBuffer_Resize(xres, yres);

    Generate_RGBFlares();
    InitPolyStats(200);
    ThreadPool::instance().init([]() {
        InitPolyStats(200);
        FPU_LPrecision();
    });
    FPU_LPrecision();

    g_profilerActive = 0;
    return true;
}

// ── Scripted camera ─────────────────────────────────────────────────────────
// Same FDS_GREETS_CAM contract the greets snapshot honours, so a pose captured
// live with F9 pastes straight in. Re-pinned EVERY frame: the user flies to a
// pose and then scrubs from it, so the whole scripted session must sit there.
// Animate_Objects skips the camera spline while View == &FC, so the pin holds
// through the tick.
struct ScriptedCam {
    bool   active = false;
    Vector pos{0, 0, 0};
    Vector fwd{0, 0, 1};
    float  fov = 0.0f;

    void parseEnv() {
        if (const char* s = std::getenv("FDS_GREETS_CAM")) {
            float px, py, pz, fx, fy, fz;
            if (std::sscanf(s, "%f,%f,%f,%f,%f,%f", &px, &py, &pz, &fx, &fy, &fz) == 6) {
                pos = Vector(px, py, pz);
                fwd = Vector(fx, fy, fz);
                active = true;
            }
        }
        if (const char* s = std::getenv("FDS_GREETS_FOV")) fov = float(std::atof(s));
    }

    void pin() const {
        if (!active) return;
        FC.ISource = pos;
        Vector look(pos.x + fwd.x, pos.y + fwd.y, pos.z + fwd.z);
        Kick_Camera(&FC.ISource, &look, 0.0f, FC.Mat);
        if (fov > 0.0f) FC.IFOV = fov;
        CalcPersp(&FC);
        View = &FC;
    }
};

}  // namespace

bool ParseReproArgs(int argc, const char* argv[], ReproConfig& cfg) {
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (starts_with(a, "--repro=")) {
            std::string_view rest = a.substr(strlen("--repro="));
            std::size_t at = rest.find('@');
            if (at == std::string_view::npos) {
                cfg.scene = std::string(rest);
            } else {
                cfg.scene = std::string(rest.substr(0, at));
                std::string_view tail = rest.substr(at + 1);
                while (!tail.empty()) {
                    auto comma = tail.find(',');
                    std::string_view kv =
                        (comma == std::string_view::npos) ? tail : tail.substr(0, comma);
                    if (starts_with(kv, "t=")) {
                        parse_timestamps(kv.substr(2), cfg.dumpAt);
                    } else if (!kv.empty() &&
                               kv.find_first_not_of("0123456789") == std::string_view::npos) {
                        parse_timestamps(kv, cfg.dumpAt);   // continuation of t=A,B,C
                    }
                    tail = (comma == std::string_view::npos)
                             ? std::string_view{} : tail.substr(comma + 1);
                }
            }
            found = true;
        } else if (starts_with(a, "--out=")) {
            cfg.outDir = std::string(a.substr(strlen("--out=")));
        }
    }
    return found;
}

int RunRepro(const ReproConfig& cfg, int xres, int yres) {
    using FF = fds::FeatureFlags;

    if (cfg.scene != "greets") {
        std::fprintf(stderr,
            "[REPRO] scene '%s' not wired yet (only 'greets'). The harness is "
            "scene-generic apart from the Initialize_/create* pair and the "
            "camera env var — add the scene here when you need it.\n",
            cfg.scene.c_str());
        return 2;
    }

    std::vector<int32_t> dumpAt = cfg.dumpAt;
    if (dumpAt.empty()) dumpAt.push_back(2993);
    std::sort(dumpAt.begin(), dumpAt.end());
    dumpAt.erase(std::unique(dumpAt.begin(), dumpAt.end()), dumpAt.end());

    const int32_t from    = FF::repro_from();
    const int     settle  = std::max(0, FF::repro_settle());
    const int     seq     = std::max(0, FF::repro_seq());
    const int     maxFrm  = std::max(1, FF::repro_max_frames());
    const bool    play    = FF::repro_play();
    const bool    lateCam = FF::repro_late_cam();

    // Match the user's WINDOW, not rev.cfg. rev.cfg is shared state and editing
    // it to chase a repro has already cost one diagnosis; this keeps it alone.
    if (FF::repro_xres() > 0) xres = FF::repro_xres();
    if (FF::repro_yres() > 0) yres = FF::repro_yres();

    ensureOutDir(cfg.outDir);
    if (!initReproEnvironment(xres, yres)) return 3;

    // NOTE what we deliberately DO NOT do here, vs REV.CPP's --snapshot path:
    //   * we leave g_fineSceneClock at its interactive default (true), so the
    //     sub-tick clock runs its real EMA estimator;
    //   * we leave fds::g_occlSnapshotInert false, so --chunk_occlusion (if the
    //     user passes it) actually culls off the previous frame.
    // Those two pins are the known-by-construction blind spots of --snapshot.

    if (play) {
#ifndef __EMSCRIPTEN__
        // Free-running clock, exactly as an interactive session has it: the SDL
        // timer thread bumps Timer at 100 Hz and the driver plays rather than
        // scrubs. Nondeterministic by nature (that is the point of this mode).
        if (SDL_InitSubSystem(SDL_INIT_TIMER) != 0)
            std::fprintf(stderr, "[REPRO] SDL_INIT_TIMER failed: %s\n", SDL_GetError());
#endif
    }

    ScriptedCam cam;
    cam.parseEnv();

    Initialize_Greets();
    // REV.CPP's init thread does NOT stop at greets: it inits Greets, City,
    // Chase, Fountain and Crash into the same process-wide engine globals
    // before the director runs a single scene. A greets-only repro therefore
    // reproduces the user's per-frame history but not his INIT history, which
    // is a real blind spot (see --repro_prescenes). Same order as REV.CPP.
    if (FF::repro_prescenes()) {
        std::fprintf(stderr, "[REPRO] --repro_prescenes: initialising City/Chase/Fountain/Crash "
                             "after Greets, as REV.CPP does\n");
        Initialize_City();
        Initialize_Chase();
        Initialize_Fountain();
        Initialize_Crash();
        std::fprintf(stderr, "[REPRO] --repro_prescenes: done\n");
    }
    // ── --repro_run_fountain: the other scenes' RENDER history ──────────────
    // Initialising a scene is not running it. The fountain is the only scene
    // that drives the transparent depth peel in REVERSE mode (4 passes), and
    // every buffer that mode touches — the deep-layer xpar G-buffer slices,
    // g_xparPeelFloor, the per-strip dirty-column records — is engine-global
    // and survives the scene. Render real fountain frames here so greets
    // inherits exactly what it inherits in a bare ./DEMO run.
    const int runFount = std::max(0, FF::repro_run_fountain());
    if (runFount > 0) {
        if (!FF::repro_prescenes()) {
            // The fountain's tick calls RenderSkyCube(SkySc, ...) and SkySc is
            // built by Initialize_City — same pairing RunFountainSnapshot needs.
            Initialize_City();
            Initialize_Fountain();
        }
        const int32_t savedTimer = Timer.load();
        auto fdrv = createFountainScene();
        fdrv->init();
        std::fprintf(stderr, "[REPRO] --repro_run_fountain: rendering %d real fountain "
                             "frames, peel passes = %d\n",
                     runFount, xparPeelPassesEffective());
        for (int i = 0; i < runFount; ++i) {
            std::memset((void*)Keyboard, 0, sizeof(Keyboard));
            if (!fdrv->tick()) break;
        }
        fdrv->cleanup();
        Timer = savedTimer;
        std::fprintf(stderr, "[REPRO] --repro_run_fountain: done\n");
    }
    // The lightmap bake runs on a background thread and the first rendered
    // frame samples it; Run_Greets joins before its first tick and so must we.
    Greets_JoinBakeThread();

    // Seed the clock BEFORE init(): GreetsScene::init() does TTrd = Timer, and
    // a mismatch there hands the first tick a bogus dTime.
    Timer = from;

    auto driver = createGreetsScene();
    driver->init();

    // ── Land on the requested t EXACTLY ─────────────────────────────────────
    // A paused F2 scrub steps a FIXED +10 ticks (tickSceneTimer), so the
    // reachable scene times are a lattice start+10k. A user's F9 pose is an
    // arbitrary t (he pauses during play, he doesn't scrub onto a multiple of
    // 10), and drivers perturb the start anyway — GreetsScene::init() does a
    // bare Timer++ before seeding TTrd. Dumping at t+8 instead of t is not the
    // same frame: the scene has animated. So align the lattice to the FIRST
    // target by choosing the start time, then walk the real scrub onto it.
    //
    // This one Timer write is the harness's ONLY clock pin, it happens before
    // any frame is rendered, and frame 0 below re-syncs the driver's own TTrd
    // to it through the normal unpaused path. It is not the snapshot's
    // "pin and render immediately" — every frame that follows is a real one.
    const int32_t t0 = Timer.load();
    int32_t alignedStart = t0;
    if (!play) {
        const int32_t target0 = dumpAt.front();
        int32_t steps = (target0 - t0 + kPausedScrubStep - 1) / kPausedScrubStep;
        if (steps < 1) steps = 1;
        alignedStart = target0 - kPausedScrubStep * steps;
        while (alignedStart < 0) { --steps; alignedStart += kPausedScrubStep; }
        if (alignedStart != t0) {
            Timer = alignedStart;
            std::fprintf(stderr,
                "[REPRO] start aligned %d -> %d so the +%d paused scrub lattice "
                "lands exactly on t=%d (%d scrub frames)\n",
                int(t0), int(alignedStart), int(kPausedScrubStep),
                int(target0), int(steps));
        }
        for (std::size_t i = 1; i < dumpAt.size(); ++i) {
            if ((dumpAt[i] - alignedStart) % kPausedScrubStep != 0)
                std::fprintf(stderr,
                    "[REPRO] WARNING t=%d is not on the scrub lattice (start %d "
                    "step %d) — it will be dumped at the first frame PAST it, "
                    "and the file is named by the ACTUAL scene time.\n",
                    int(dumpAt[i]), int(alignedStart), int(kPausedScrubStep));
        }
    }

    if (play) TimerInit(100);   // 100 Hz == centisecond Timer ticks

    std::fprintf(stderr,
        "[REPRO] scene=greets %dx%d from=%d settle=%d seq=%d mode=%s cam=%s\n",
        xres, yres, from, settle, seq, play ? "play" : "paused-scrub",
        cam.active ? "FDS_GREETS_CAM" : "scene");

    std::memset((void*)Keyboard, 0, sizeof(Keyboard));

    std::size_t next     = 0;
    int         held     = 0;      // frames parked at the current dump target
    int         frames   = 0;
    int         produced = 0;
    bool        alive    = true;

    while (next < dumpAt.size() && frames < maxFrm && alive) {
        const int32_t want = dumpAt[next];

        // Transport keys for THIS frame, written into the same global the SDL
        // event pump writes in a real session. tickSceneTimer reads them.
        std::memset((void*)Keyboard, 0, sizeof(Keyboard));
        if (!play) {
            // Frame 0 runs with NO keys and UNPAUSED: tickSceneTimer takes
            // sceneT from the (aligned) Timer and writes it back into the
            // driver's own TTrd, so the driver adopts our aligned start through
            // its normal path instead of us reaching into its state.
            // Frame 1 onward holds P — pauseMode is latched on a persistent
            // member, so one press is enough and it stays paused — plus F2,
            // which is a paused single-step scrub of exactly +10 ticks: what a
            // user gets holding F2 while paused.
            if (frames > 0) {
                Keyboard[ScP] = 1;
                if (Timer < want) Keyboard[ScF2] = 1;
            }
        }

        // --repro_late_cam: don't pin until the target is reached, so the
        // scene's own camera drives the scrub and CameraHead ends up parked at
        // the TARGET pose (what a user's TAB leaves behind) rather than frozen
        // at the init pose (what pinning from frame 0 leaves behind).
        if (!lateCam || Timer >= want) cam.pin();
        alive = driver->tick();
        ++frames;

        if (Timer >= want) {
            // Parked at the pose with no keys down — the state an interactive
            // user is actually looking at when he reports a defect.
            if (++held > settle) {
                // Named by the ACTUAL scene time, never the requested one — a
                // file called t002993 that is really t=3001 is exactly the kind
                // of quiet lie this harness exists to stop.
                const int32_t got = int32_t(Timer.load());
                fds::MirrorRttTrace_Report();   // --mirror_rtt_trace, one burst
                char path[1024];
                std::snprintf(path, sizeof(path), "%s/repro_greets_t%06d.ppm",
                              cfg.outDir.c_str(), int(got));
                write_ppm(path, MainSurf->Data, xres, yres, MainSurf->BPSL);
                std::fprintf(stderr,
                    "[REPRO] requested t=%d -> LANDED t=%d %s (%d frames, "
                    "g_FrameTime=%d, g_FrameTimeF=%.3f) -> %s\n",
                    int(want), int(got), got == want ? "EXACT" : "*** OFF-TARGET ***",
                    frames, int(g_FrameTime), double(g_FrameTimeF), path);
                ++produced;

                // Consecutive-frame series at the same pose: catches anything
                // that only shows up on SOME frames (temporal reprojection
                // phase, a stale async bake, a race).
                for (int k = 0; k < seq && alive; ++k) {
                    std::memset((void*)Keyboard, 0, sizeof(Keyboard));
                    cam.pin();
                    alive = driver->tick();
                    ++frames;
                    std::snprintf(path, sizeof(path), "%s/repro_greets_t%06d_s%02d.ppm",
                                  cfg.outDir.c_str(), int(got), k);
                    write_ppm(path, MainSurf->Data, xres, yres, MainSurf->BPSL);
                }

                held = 0;
                ++next;
            }
        }
    }

    if (next < dumpAt.size()) {
        std::fprintf(stderr,
            "[REPRO] STOPPED after %d frames at Timer=%d with %zu target(s) "
            "unreached (%s). Raise --repro_max_frames, or check the scene's "
            "partTime.\n",
            frames, int(Timer.load()), dumpAt.size() - next,
            alive ? "frame budget" : "driver ended the scene");
    }

    driver->cleanup();
    std::fprintf(stderr, "[REPRO] done: %d dump(s), %d frames\n", produced, frames);

    // Leave via _exit. A long scripted session ends with live engine threads and
    // scene state that the normal teardown does not expect (a --repro run was
    // observed aborting in libc++abi AFTER a good dump, which turns a successful
    // run into SIGABRT and destroys the exit status a script reads). The dumps
    // are already on disk and flushed; there is nothing left worth unwinding, and
    // a diagnostic harness must not report failure for a frame it rendered fine.
    std::fflush(nullptr);
    _exit(produced > 0 ? 0 : 4);
}
