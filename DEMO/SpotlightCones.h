#pragma once

// Scene-agnostic helper for authoring runtime spotlights (not present
// in the FLD). The actual cone rendering is the screen-space
// ray-march pass in FDS/RENDER/DeferredLighting.cpp (Render_VolumetricCones).
//
// Earlier this file also defined a triangle-rasterized cone overlay;
// that has been replaced by the volumetric ray-march which integrates
// correctly across the cone-ray segment, looks better, and runs in
// the tile-parallel infrastructure.

#include <Base/Vector.h>
#include <cstdint>

struct Scene;
struct Omni;

namespace fds {

// Allocate + link a fully-initialised Light_SpotLight Omni into `sc`'s
// OmniHead chain. Used by scenes that author runtime spotlights not
// present in the FLD (greets robot spots, city streetlights/headlights).
// Includes single-key splines so Animate_Objects doesn't crash.
//
//   hotInnerDeg / fallOuterDeg: cone half-angles in degrees (hot=full
//   intensity inside, falloff to 0 at outer). Stored as cosines.
//   shadowMapRes: 0 = use default, else per-light override.
Omni* MakeSpotLight(Scene* sc,
                     float R, float G, float B,
                     float intensity, float range,
                     const Vector& pos, const Vector& dir,
                     float hotInnerDeg, float fallOuterDeg,
                     uint16_t shadowMapRes = 0,
                     bool castsShadow = true);

} // namespace fds
