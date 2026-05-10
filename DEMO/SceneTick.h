#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <SDL.h>

#include "Base/FDS_VARS.H"
#include "VESA/Vesa.h"

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
    while (true) {
        poll_pending_resize(&driver);
        if (!driver.tick()) break;
    }
    // Fade the last rendered frame to black before cleanup hands control
    // back to the caller (which usually transitions to the next scene).
    // Scene's last tick left the final framebuffer in VPage; we just
    // keep alpha-blending it in place.
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
