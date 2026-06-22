#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Resize.h"
#include "Base/FaceListContext.h"
#include "Base/FeatureFlags.h"

// Quit/skip flags live in Rev.h, but we forward-declare here to avoid
// pulling the rest of Rev.h's transitive includes (FDS_VARS, Modplayer,
// etc.) into every scene-tick consumer.
extern std::atomic<bool> g_shouldQuit;
extern std::atomic<bool> g_skipScene;

struct Scene;
struct Face;

// Scene driver interface for the tick-based scene loop.
//
// Each scene is expressed as three phases:
//   - init()    one-time setup (scene state, face lists, camera, factors)
//   - tick()    one frame of work; returns true to keep going, false to stop
//   - cleanup() one-time teardown (dealloc, Timer adjust, ESC-release wait)
//
// runSceneBlocking runs a driver to completion on the calling thread.
// SceneSequence walks a vector of scene factories one-at-a-time and is
// itself tick-driven, so the same sequence can be run blocking on native
// or driven frame-by-frame by emscripten_set_main_loop on WASM.

struct SceneDriver {
    virtual ~SceneDriver() = default;
    virtual void init() = 0;
    virtual bool tick() = 0;
    virtual void cleanup() = 0;
    // Called at frame boundary when an SDL window resize is pending. Override
    // in scenes with resolution-dependent buffers (City's dispMap, Fountain's
    // TBR span buffer, ...). Engine globals (XRes, YRes, PageSize, VPage,
    // YOffs, ...) have already been updated by the time this fires.
    virtual void on_resize(int /*newX*/, int /*newY*/) {}

protected:
    // Count polys + omnis in `sc`, resize fds::g_mainFaces sort buffers to
    // match, and wire up the engine-side camera globals: View = sc->CameraHead,
    // C_FZP, C_rFZP. includeOmnisInCount=true matches CITY/CRASH semantics
    // (each omni counts as 1 face for budgeting). The other scenes pass false.
    void setupFaceLists(Scene *sc, bool includeOmnisInCount);

    // Rising-edge Tab key handler — toggles `View` between `sc->CameraHead`
    // and the engine free-cam (`FC`), logging the switch. Per-driver state in
    // tabPrev_ so two scenes don't collide.
    void tickTabToggle(Scene *sc, const char *sceneName);

    // parallel_memset of VPage + ZPage16 to zero. Same pattern as every
    // scene's per-frame clear.
    void clearFrame();

    // Cleanup helper: spins until the user releases Backspace, otherwise the
    // next scene's tick would immediately see Keyboard[ScBackSpace] and skip.
    // Honours g_shouldQuit so ESC/Ctrl-C still exits.
    void waitBackspaceRelease();

    // Shared scene-time / pause / fast-forward-rewind, used by every scene
    // so the transport controls behave identically:
    //   P / U     pause / unpause (toggles the caller's `pauseMode`)
    //   F2 / F1   fast-forward / rewind — 8× the frame delta in play, a
    //             small fixed step while paused (single-frame stepping)
    // `Timer` is a free-running atomic (the SDL timer thread bumps it); this
    // snapshots it once, freezes scene time at `TTrd` while paused, and only
    // writes the atomic back when pausing or scrubbing — so a background tick
    // can't creep the time forward mid-frame. Updates the globals dTime and
    // g_FrameTime and the caller's `TTrd`; returns the scene time. Callers
    // should seed `TTrd = Timer` in init() and feed g_FrameTime into CurFrame.
    int32_t tickSceneTimer(int32_t &TTrd, bool &pauseMode);

    bool tabPrev_ = false;
};

// Initialize_X helpers — used by each scene's free Initialize function.
//
// Allocate a 16-byte-aligned Scene, zero it, and load the FLD into it. The
// pointer is returned to the caller; ownership stays with whoever called us
// (typically a file-scope static, freed by the matching Destroy_Scene later).
Scene *loadSceneAligned(const char *fldPath);

// Cinematic preset (--cinematic): turn HDR + the filmic post-FX stack on for
// THIS scene at its own per-scene exposure. Called at the top of each scene's
// factory. Notes:
//   - setDefault yields to anything the user set explicitly on the CLI, so
//     individual --flags still override the preset.
//   - Each scene passes its OWN exposure; since setDefault overwrites the value
//     (it only skips when the flag was CLI-set), values don't leak between
//     scenes in the full sequence — but a scene that wants HDR must call this
//     (or pin hdr_exposure itself) so it doesn't inherit the prior scene's.
//   - Deliberately does NOT touch deferred_quarter: that's a greets-only
//     optimization whose 2x2 reconstruction checkerboards on other scenes.
// Per-scene cinematic exposures, measured so the tonemap rolls off instead of
// blowing out: bright daylit water → low, dark scenes → high. Used by both the
// live scene factories and the headless snapshots so they stay in lockstep.
namespace cine {
    constexpr float kCityExposure     = 0.4f;   // bright water mirror
    constexpr float kChaseExposure    = 1.0f;   // neutral (no headless snapshot; tune live)
    constexpr float kFountainExposure = 1.5f;   // dark night scene, lift slightly
    constexpr float kCrashExposure    = 1.5f;   // dark by design
    constexpr float kGreetsExposure   = 1.0f;   // greets' own tuning
}

// `anamorphic` adds horizontal lens streaks. Unlike bloom (which a high
// threshold keeps to the brightest highlights), anamorphic streaks ANY bright
// region into long bars — gorgeous on DISCRETE hot sources (greets disco,
// fountain bolts) but it smears scenes whose brightness is broad and diffuse:
// city's sunlit water is one giant bright source, so the streaks wash the
// frame. Diffuse-bright scenes leave it off and keep the rest.
inline void ApplyCinematicSceneDefaults(float exposure, bool anamorphic = false)
{
    using FF = fds::FeatureFlags;
    if (!FF::cinematic()) return;
    FF::setDefault(FF::BoolId::hdr,              true);
    FF::setDefault(FF::BoolId::hdr_linear,       true);
    FF::setDefault(FF::FloatId::hdr_exposure,    exposure);
    // Restrained bloom — a high threshold keeps it to the brightest highlights,
    // so even bright water doesn't wash (measured safe). Greets overrides hotter.
    FF::setDefault(FF::BoolId::bloom,            true);
    FF::setDefault(FF::FloatId::bloom_threshold, 245.0f);
    FF::setDefault(FF::FloatId::bloom_intensity, 0.4f);
    // Post-tonemap colour/lens FX — safe everywhere (no bright-pass):
    FF::setDefault(FF::BoolId::chromatic,        true);
    FF::setDefault(FF::BoolId::vignette,         true);
    FF::setDefault(FF::BoolId::grade,            true);
    FF::setDefault(FF::BoolId::grain,            true);
    if (anamorphic) FF::setDefault(FF::BoolId::anamorphic, true);
}

// Walk MatLib and stamp Mat_RGBInterp + Txtr_Tiled on every textured material.
// Restricted to materials belonging to `sc` when restrictToScene=true (the
// CITY/FOUNTAIN/GREETS pattern); set false to apply to every material in the
// library (CRASH historically does this).
void tagSceneMaterials(Scene *sc, bool restrictToScene);

// Drain g_pendingResize: if a resize is pending, apply it engine-side and
// notify the active driver. Called at frame top so the upcoming tick()
// renders at the new dimensions.
inline void poll_pending_resize(SceneDriver* driver) {
    uint64_t v = g_pendingResize.exchange(0);
    if (!v) return;
    int x, y;
    unpack_size(v, x, y);
    EngineResize(x, y);
    if (driver) driver->on_resize(x, y);
}

inline void runSceneBlocking(SceneDriver& driver) {
    driver.init();
    while (true) {
        // Quit (ESC, SDL_QUIT, Ctrl-C) wins over everything else: we don't
        // even bother running another tick; just tear down and return so
        // the orchestrator can short-circuit to the program exit.
        if (g_shouldQuit.load(std::memory_order_relaxed)) break;
        // "Skip to next scene" (Backspace): consume the flag so the next
        // scene starts clean, then break out so this scene's cleanup runs.
        if (g_skipScene.exchange(false, std::memory_order_relaxed)) break;
        poll_pending_resize(&driver);
        if (!driver.tick()) break;
    }
    driver.cleanup();
}

// Walks a list of scene factories. Each tick() advances the current scene
// by one frame; when a scene's tick returns false, its cleanup runs and
// the next scene is instantiated+initialized on the subsequent tick.
// Returns false once all scenes have finished — a driver loop can use this
// as its exit condition.
struct SceneSequence {
    using Factory = std::function<std::unique_ptr<SceneDriver>()>;

    SceneSequence(std::vector<Factory> factories) : factories_(std::move(factories)) {}

    bool tick() {
        if (index_ >= factories_.size()) return false;

        // Same global-flag semantics as runSceneBlocking.
        if (g_shouldQuit.load(std::memory_order_relaxed)) {
            if (current_) {
                current_->cleanup();
                current_.reset();
            }
            index_ = factories_.size();
            return false;
        }

        if (!current_) {
            current_ = factories_[index_]();
            current_->init();
        }

        if (g_skipScene.exchange(false, std::memory_order_relaxed)) {
            current_->cleanup();
            current_.reset();
            ++index_;
            return index_ < factories_.size();
        }

        poll_pending_resize(current_.get());

        if (!current_->tick()) {
            current_->cleanup();
            current_.reset();
            ++index_;
        }
        return index_ < factories_.size();
    }

    void runBlocking() {
        while (tick()) continue;
    }

private:
    std::vector<Factory> factories_;
    std::unique_ptr<SceneDriver> current_;
    std::size_t index_ = 0;
};
