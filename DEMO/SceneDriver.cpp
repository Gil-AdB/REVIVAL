#include "SceneTick.h"
#include <Base/ParamScript.h>

#include "Rev.h"

#include <Base/FDS_DECS.H>
#include <Base/FDS_DEFS.H>
#include <Base/FDS_VARS.H>
#include <Base/Scene.h>
#include <Base/Face.h>
#include <Base/TriMesh.h>
#include <Base/Omni.h>
#include <Base/Camera.h>
#include <Base/Material.h>
#include <Threads.h>

#include <cmath>
#include <cstdio>
#include <cstring>

void SceneDriver::setupFaceLists(Scene *sc, bool includeOmnisInCount)
{
    int32_t polys = 0;
    for (TriMesh *t = sc->TriMeshHead; t; t = t->Next)
        polys += t->FIndex;
    if (includeOmnisInCount) {
        for (Omni *o = sc->OmniHead; o; o = o->Next) {
            (void)o;
            ++polys;
        }
    }
    // Particles insert into the same FList: 1 face each, or 2 for trail
    // quads (addParticleTrail). Scenes that allocate Sc->Pcl before
    // driver init (city rain) need the headroom; fountain sizes its own
    // lists and doesn't come through here.
    polys += sc->NumOfParticles * 2;
    fds::g_mainFaces.resize(polys);
    View  = sc->CameraHead;
    C_FZP  = sc->FZP;
    C_rFZP = 1.0f / C_FZP;
}

void SceneDriver::tickTabToggle(Scene *sc, const char *sceneName)
{
    // Per-scene scripted parameters ride this hook: every scene already
    // calls tickTabToggle(sc, "<name>") once per frame after its Timer
    // update, which is exactly the (scene name, per-frame) pair the
    // script system needs. SetScene is a no-op while the name is stable.
    fds::ParamScript_SetScene(sceneName);
    fds::ParamScript_Tick(float(Timer));

    const bool tabNow = Keyboard[ScTab] != 0;
    if (tabNow && !tabPrev_) {
        const bool switchingToFC = (View != &FC);
        if (switchingToFC) {
            // Capture the scripted cam's CURRENT pose into FC so you
            // always land where you were watching. Init-time capture
            // gets stale once Animate_Objects walks the camera spline.
            // Without this, FC could be at a default sentinel or a
            // stale init pose → frozen frame, feels stuck.
            FC.ISource = View->ISource;
            Matrix_Copy(FC.Mat, View->Mat);
            FC.IFOV    = View->IFOV;
            CalcPersp(&FC);
        }
        View = switchingToFC ? &FC : sc->CameraHead;
        std::fprintf(stderr, "[TAB %s] toggled -> View=%s\n",
                     sceneName, (View == &FC) ? "FC" : "scene");
    }
    tabPrev_ = tabNow;
}

int32_t SceneDriver::tickSceneTimer(int32_t &TTrd, bool &pauseMode)
{
    // Timer is a free-running atomic — TimerProc (the SDL timer thread,
    // REV.CPP) bumps it every tick. Snapshot it ONCE and do all scene-time
    // math on a local: the old per-scene code pinned the atomic (Timer =
    // TTrd) and then RE-READ it into g_FrameTime, so a background tick
    // landing between the pin and the read crept 't' upward a few units
    // each paused frame.
    const int32_t nowTimer = Timer.load();
    dTime = float(nowTimer - TTrd);
    if (dTime > 300.0f) dTime = 300.0f;   // cap a post-stall jump
    if (Keyboard[ScP]) pauseMode = true;
    if (Keyboard[ScU]) pauseMode = false;

    // Scene time this frame: frozen at TTrd while paused (the free-running
    // clock is ignored), else caught up to wall clock.
    int32_t sceneT = pauseMode ? TTrd : nowTimer;
    // Fast forward / rewind: 8× the frame delta in play; in pause dTime is
    // ~0 so fall back to a small per-tick step so F1/F2 still scrubs while
    // paused (fine enough to step single anim frames).
    const int32_t scrubStep = pauseMode ? 10 : int32_t(dTime * 8.0f);
    bool scrubbed = false;
    if (Keyboard[ScF2]) { sceneT += scrubStep; scrubbed = true; }
    if (Keyboard[ScF1]) { sceneT = (scrubStep > sceneT) ? 0 : sceneT - scrubStep; scrubbed = true; }

    // Override the free-running clock only when we must: paused (re-pin
    // every frame so a background tick can't advance it) or scrubbing.
    // Normal play leaves Timer free-running so no wall-clock ticks elapsed
    // during frame processing are dropped.
    if (pauseMode || scrubbed) Timer = sceneT;

    // Smooth scene clock (see g_FrameTimeF). Two measured properties of the
    // int Timer force this shape:
    //   1. it resolves whole 10ms ticks → per-frame animation steps
    //      quantize (30fps: 3,4,3,4);
    //   2. its CADENCE is load-elastic — SDL_AddTimer callbacks reschedule
    //      relative, so under render load Timer runs up to ~20% slower
    //      than wall time (measured: PC-rate fine clock outran Timer ~1
    //      tick/frame and any anchor snap re-created a sawtooth).
    // So: advance a float clock by (wall delta × Timer's own average rate),
    // where the rate is an EMA of dTimer/dWall. Steps are then smooth AND
    // track the demo's actual (load-elastic) tempo; a gentle anchor plus a
    // generous snap guard bound the offset without visible correction.
    {
        const uint64_t pc = SDL_GetPerformanceCounter();
        if (pcFreq_ == 0) pcFreq_ = SDL_GetPerformanceFrequency();
        const bool live = g_fineSceneClock && !pauseMode && !scrubbed;
        if (live && fineT_ >= 0.0f && pcPrev_ != 0) {
            const double dWall =
                double(pc - pcPrev_) * 100.0 / double(pcFreq_);
            const float dTimer = float(sceneT - lastSceneT_);
            if (dWall > 0.0 && dWall <= 60.0 && dTimer >= 0.0f && dTimer <= 60.0f) {
                // Rate = EMA(dTimer)/EMA(dWall) — ratio of MEANS. The old
                // EMA of the instantaneous ratio was Jensen-biased ~10%
                // HIGH under spiky frame times, so fineT_ steadily outran
                // Timer and every hitch discharged the accumulated lead as
                // a visible jump back (city, ~10 content-frames at t≈240).
                dTimerEma_ += 0.05f * (dTimer       - dTimerEma_);
                dWallEma_  += 0.05f * (float(dWall) - dWallEma_);
                float rate = (dWallEma_ > 1e-3f) ? dTimerEma_ / dWallEma_ : 1.0f;
                if (rate < 0.2f) rate = 0.2f;
                if (rate > 3.0f) rate = 3.0f;
                fineT_ += float(dWall) * rate;
                // Anchor: gentle 1%/frame normally; firm 10%/frame once the
                // offset exceeds 8 ticks. Continuous in both regimes — the
                // offset shrinks across frames, never in one step (a hard
                // snap here is exactly the sawtooth this clock exists to
                // remove). A true snap remains only for pathological
                // divergence (>120 ticks: scene switch / debugger stall).
                const float off = float(sceneT) - fineT_;
                fineT_ += (std::fabs(off) > 8.0f ? 0.10f : 0.01f) * off;
                if (std::fabs(off) > 120.0f)
                    fineT_ = float(sceneT);
            } else {
                // Hitch (>600ms wall stall or timer leap): follow the
                // integer timer RELATIVELY — the fineT_-vs-sceneT offset is
                // preserved and bleeds out through the anchor on later
                // frames. Snapping to sceneT here zeroed the offset in one
                // frame — the user-visible "camera jumps back". The outlier
                // deltas also stay OUT of the rate EMAs.
                if (dTimer > 0.0f) fineT_ += dTimer;
            }
        } else {
            fineT_ = float(sceneT);
        }
        pcPrev_ = pc;
        lastSceneT_ = sceneT;
        g_FrameTimeF = fineT_;
    }

    g_FrameTime = TTrd = sceneT;
    return sceneT;
}

void SceneDriver::clearFrame()
{
    parallel_memset(VPage, 0, PageSize);
    parallel_memset(ZPage16, 0, size_t(XRes) * size_t(YRes) * sizeof(word));
}

void SceneDriver::waitBackspaceRelease()
{
    while (Keyboard[ScBackSpace] && !g_shouldQuit.load()) continue;
}

Scene *loadSceneAligned(const char *fldPath)
{
    Scene *sc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
    std::memset(sc, 0, sizeof(Scene));
    LoadFLD(sc, fldPath);
    return sc;
}

void tagSceneMaterials(Scene *sc, bool restrictToScene)
{
    for (Material *m = MatLib; m; m = m->Next) {
        if (restrictToScene && m->RelScene != sc) continue;
        if (!m->Txtr) continue;
        m->Flags |= Mat_RGBInterp;
        m->Txtr->Flags |= Txtr_Tiled;
    }
}
