#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <SDL.h>

#include "Base/FDS_VARS.H"
#include "VESA/Vesa.h"

#include "Resize.h"
#include "SDL2.h"
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

// ── Per-scene cinematic settings (--cinematic) ───────────────────────────
// Each scene owns a CinematicProfile: an explicit value object describing its
// look. ApplyCinematicProfile() pushes EVERY field through setDefault(), so a
// profile is fully self-describing — every cinematic scene sets the same flags
// (just to different values, including "off"), so the previous scene's values
// are always overwritten and nothing bleeds between scenes. No global save /
// restore, no ordering assumptions. setDefault still yields to explicit CLI/env
// flags, so user overrides win. deferred_quarter is greets-only and not here.
//
// Member defaults are the engine defaults / "off", so a scene's profile only
// names what differs (e.g. fountain = just a brighter exposure).
struct CinematicProfile {
    float exposure         = 1.0f;     // hdr_exposure
    float bloomThreshold   = 245.0f;   // restrained: only the brightest highlights bloom
    float bloomIntensity   = 0.4f;
    float chromaticAmount  = 2.5f;
    float vignetteStrength = 0.4f;
    // Froxel volumetric fog (off by default).
    bool  fog              = false;
    bool  fogWorley        = false;
    float fogDensity       = 3.0f;
    float fogBottom        = -1.0e9f;
    float fogTop           = 1.0e9f;
    float fogBlobOverlap   = 0.0f;
    float fogWorleyThresh  = 0.5f;
    float fogCell          = 400.0f;
    float fogInscatter     = 0.0f;
    // Storm.
    bool  rain             = false;
    float boltFlashPeak    = 500.0f;
    float boltFlashRange   = 500.0f;
    // Anamorphic streaks (off by default — they smear diffuse-bright scenes).
    bool  anamorphic       = false;
    float anamIntensity    = 0.5f;
    float anamVert         = 0.25f;
    float anamDecay        = 0.85f;
    int   anamPasses       = 6;
    // Lighting / transparency depth / HDR glow compensation.
    bool  shadows          = false;
    float coneStrength     = 0.05f;
    float hdrGlowScale     = 0.25f;
    int   xparPeel         = 1;
};

inline void ApplyCinematicProfile(const CinematicProfile &p)
{
    using FF = fds::FeatureFlags;
    if (!FF::cinematic()) return;
    // Base: HDR + restrained bloom + post-tonemap colour/lens FX (every scene).
    FF::setDefault(FF::BoolId::hdr,                   true);
    FF::setDefault(FF::BoolId::hdr_linear,            true);
    FF::setDefault(FF::FloatId::hdr_exposure,         p.exposure);
    FF::setDefault(FF::BoolId::bloom,                 true);
    FF::setDefault(FF::FloatId::bloom_threshold,      p.bloomThreshold);
    FF::setDefault(FF::FloatId::bloom_intensity,      p.bloomIntensity);
    FF::setDefault(FF::BoolId::chromatic,             true);
    FF::setDefault(FF::FloatId::chromatic_amount,     p.chromaticAmount);
    FF::setDefault(FF::BoolId::vignette,              true);
    FF::setDefault(FF::FloatId::vignette_strength,    p.vignetteStrength);
    FF::setDefault(FF::BoolId::grade,                 true);
    FF::setDefault(FF::BoolId::grain,                 true);
    // Froxel volumetric fog.
    FF::setDefault(FF::BoolId::fast_fog,              p.fog);
    FF::setDefault(FF::BoolId::fast_fog_froxel,       true);
    FF::setDefault(FF::BoolId::fast_fog_worley,       p.fogWorley);
    FF::setDefault(FF::BoolId::fast_fog_xpar,         true);
    FF::setDefault(FF::FloatId::fast_fog_density,       p.fogDensity);
    FF::setDefault(FF::FloatId::fast_fog_bottom,        p.fogBottom);
    FF::setDefault(FF::FloatId::fast_fog_top,           p.fogTop);
    FF::setDefault(FF::FloatId::fast_fog_blob_overlap,  p.fogBlobOverlap);
    FF::setDefault(FF::FloatId::fast_fog_worley_thresh, p.fogWorleyThresh);
    FF::setDefault(FF::FloatId::fast_fog_cell,          p.fogCell);
    FF::setDefault(FF::FloatId::fast_fog_inscatter,     p.fogInscatter);
    // Storm.
    FF::setDefault(FF::BoolId::rain,                  p.rain);
    FF::setDefault(FF::FloatId::bolt_flash_peak,      p.boltFlashPeak);
    FF::setDefault(FF::FloatId::bolt_flash_range,     p.boltFlashRange);
    // Anamorphic.
    FF::setDefault(FF::BoolId::anamorphic,            p.anamorphic);
    FF::setDefault(FF::FloatId::anamorphic_intensity, p.anamIntensity);
    FF::setDefault(FF::FloatId::anamorphic_vert,      p.anamVert);
    FF::setDefault(FF::FloatId::anamorphic_decay,     p.anamDecay);
    FF::setDefault(FF::IntId::anamorphic_passes,      p.anamPasses);
    // Lighting / transparency depth / glow compensation.
    FF::setDefault(FF::BoolId::shadows,              p.shadows);
    FF::setDefault(FF::FloatId::cone_strength,       p.coneStrength);
    FF::setDefault(FF::FloatId::hdr_glow_scale,      p.hdrGlowScale);
    FF::setDefault(FF::IntId::xpar_peel_passes,      p.xparPeel);
}

// ── The scenes' profiles ─────────────────────────────────────────────────
namespace cine {

// City + chase share the cityscape: tuned storm-city look — froxel volumetric
// fog (worley + blob overlap + inscatter glow), rain + bright lightning flash,
// shadows, strong cones, deep transparency peel, punchy anamorphic + CA/vignette.
// Low exposure (the fog inscatter supplies the brightness). Lens ghosts + DoF
// deliberately excluded.
inline constexpr CinematicProfile kCity{
    .exposure = 0.3f,
    .chromaticAmount = 3.0f,
    .vignetteStrength = 0.8f,
    .fog = true, .fogWorley = true,
    .fogDensity = 3.0f, .fogBottom = -400.0f, .fogTop = 420.0f,
    .fogBlobOverlap = 3.0f, .fogWorleyThresh = 2.0f, .fogCell = 500.0f, .fogInscatter = 1.5f,
    .rain = true, .boltFlashPeak = 10000.0f, .boltFlashRange = 600.0f,
    .anamorphic = true, .anamIntensity = 3.0f, .anamVert = 0.0f, .anamDecay = 0.3f, .anamPasses = 3,
    .shadows = true, .coneStrength = 2.0f, .hdrGlowScale = 0.12f, .xparPeel = 4,
};

// Chase = the same cityscape atmosphere, a touch brighter (it's darker than city).
inline constexpr CinematicProfile kChase = [] { CinematicProfile p = kCity; p.exposure = 0.45f; return p; }();

// Fountain: dark night scene — just lift exposure; bloom-only (its discrete
// lights bloom; fog/anamorphic would smear the orbs).
inline constexpr CinematicProfile kFountain{ .exposure = 1.5f };

// Crash: the register/crash dump — dark by design, lift slightly.
inline constexpr CinematicProfile kCrash{ .exposure = 1.5f };

// Greets keeps its own richer flag block (GreetsApplyRunDefaults); this is just
// the exposure it pins so it doesn't inherit the prior scene's.
inline constexpr float kGreetsExposure = 1.0f;

}  // namespace cine

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

// One frame of inter-scene fade: alpha-blend VPage in place toward black
// using the engine's existing AlphaBlend primitive (same SIMD path Glat
// uses for its smear/composite passes), then Flip. The cumulative
// per-frame factor (totalFrames-frame-1)/(totalFrames-frame) gives a
// linear fade from V_0 to 0 over `totalFrames` calls (frame=0..N-1).
inline void engineFadeStep(int frame, int totalFrames) {
    if (!MainSurf || !VPage || totalFrames <= 0) return;
    int denom = totalFrames - frame;
    if (denom <= 0) denom = 1;
    int modValue = (totalFrames - frame - 1) * 255 / denom;
    if (modValue < 0) modValue = 0;
    DWord perSrc = (DWord)((modValue & 0xFF) * 0x01010101u);
    DWord perDst = 0;
    // Source==Target==VPage. Within a single 16-byte SIMD chunk, the
    // reads happen before the writes, and chunks don't overlap, so the
    // in-place modify is safe.
    AlphaBlend(VPage, VPage, perSrc, perDst, PageSize);
    Flip(MainSurf);
}

// Native: drive the fade as a blocking loop at ~60 fps.
inline void runFadeOut(int frames) {
    for (int i = 0; i < frames; ++i) {
        engineFadeStep(i, frames);
        SDL_Delay(16);  // ~60 fps cadence; renderer's vsync absorbs jitter
    }
}

inline void runSceneBlocking(SceneDriver& driver) {
    driver.init();
    // Arm a fade-in: V_Flip will scale VPage in place by an increasing
    // factor for the first N flips, so the scene comes up from black.
    // Symmetric with the runFadeOut at scene end.
    EngineStartFadeIn(15);
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
    // Fade the last rendered frame to black before cleanup hands control
    // back to the caller (which usually transitions to the next scene).
    runFadeOut(15);
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
