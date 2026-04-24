#pragma once

// Scene driver interface for the tick-based scene loop.
//
// Each scene is expressed as three phases:
//   - init()    one-time setup (scene state, face lists, camera, factors)
//   - tick()    one frame of work; returns true to keep going, false to stop
//   - cleanup() one-time teardown (dealloc, Timer adjust, ESC-release wait)
//
// runSceneBlocking runs a driver to completion on the calling thread. The
// Emscripten entry point will instead hand the driver to
// emscripten_set_main_loop and call cleanup when tick returns false — the
// SceneDriver shape stays the same.

struct SceneDriver {
    virtual ~SceneDriver() = default;
    virtual void init() = 0;
    virtual bool tick() = 0;
    virtual void cleanup() = 0;
};

inline void runSceneBlocking(SceneDriver& driver) {
    driver.init();
    while (driver.tick()) continue;
    driver.cleanup();
}
