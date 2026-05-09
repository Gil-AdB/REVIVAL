#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <SDL.h>

#include "Resize.h"
#include "SDL2.h"

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
};

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

// Re-Flips MainSurf with a decreasing fade for `frames` frames at ~60 fps.
// Used between scenes on native to mirror the wasm FADE_OUT state.
// Caller sets fade back to 1.0 implicitly (we end at 0.0; restore on the
// next scene's first Flip).
inline void runFadeOut(int frames) {
    if (!MainSurf) return;
    for (int i = 0; i < frames; ++i) {
        float fade = 1.0f - (float)(i + 1) / (float)frames;
        if (fade < 0.0f) fade = 0.0f;
        SDL2_SetFade(fade);
        Flip(MainSurf);
        SDL_Delay(16);  // ~60 fps cadence; renderer's vsync absorbs jitter
    }
    SDL2_SetFade(1.0f);  // restore — next scene starts at full brightness
}

inline void runSceneBlocking(SceneDriver& driver) {
    driver.init();
    while (true) {
        poll_pending_resize(&driver);
        if (!driver.tick()) break;
    }
    // Fade the last rendered frame to black before cleanup hands control
    // back to the caller (which usually transitions to the next scene).
    // Scenes write their last output into the engine surface during their
    // final tick; we just keep re-presenting it with decreasing colour-mod.
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

        if (!current_) {
            current_ = factories_[index_]();
            current_->init();
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
