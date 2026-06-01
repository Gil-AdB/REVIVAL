#pragma once

// Interactive mirror-test scene. Same geometry as the snapshot
// (--snapshot=mirrortest): floor + back wall + mirror panel + test
// cube, plus a single omni so the lighting kernel has something to
// chew on. Wraps the SceneBuilder-constructed scene in a SceneDriver
// so the engine's standard free-cam / Dynamic_Camera / Animate /
// Transform / Lighting / Render loop drives it. Plug into REV.CPP
// via `--scene=mirrortest`.

#include "Scenes.h"   // SceneDriver

#include <memory>

std::unique_ptr<SceneDriver> createMirrorTestScene();

// Convenience entry — builds + runs the driver blocking on the main
// thread. The demo's CodeEntry calls Run_<Scene>() for each scene in
// the canonical demo sequence; the mirror test isn't part of that
// sequence but the same Run_* shape keeps REV.CPP dispatch uniform.
void Run_MirrorTest();
