#pragma once

#include "SceneTick.h"
#include <memory>

// Factory functions for each playable scene. Each scene's SceneDriver
// subclass lives in an anonymous namespace inside its .CPP; these
// factories are the only way to construct one from outside.

std::unique_ptr<SceneDriver> createGlatoScene();
std::unique_ptr<SceneDriver> createCityScene();
std::unique_ptr<SceneDriver> createChaseScene();
std::unique_ptr<SceneDriver> createFountainScene();
std::unique_ptr<SceneDriver> createCrashScene();
std::unique_ptr<SceneDriver> createGreetsScene();
