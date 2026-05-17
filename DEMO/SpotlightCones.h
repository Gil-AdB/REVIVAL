#pragma once

// Scene-agnostic volumetric spotlight cone overlay. Extracted from
// GREETS.CPP's draw_cones; for each Light_SpotLight + Omni_Active omni
// in the scene, build a fan cone (apex at light → rim at cos(outer)
// boundary), 3D-clip each fan triangle against the near plane, project
// + rasterize with additive blend, per-pixel Z test against ZPage16
// (so walls correctly occlude the glow). Two-sided.
//
// Gated by FeatureFlags::draw_cones at the call site.
//
// Tuning flags:
//   FDS_CONE_STRENGTH      apex-alpha multiplier (0..1, default 0.35)
//   FDS_CONE_FALLOFF_EXP   alpha curve exponent (1=linear, 2=square,
//                          higher = darker rim; default 2.0)
//   FDS_CONE_DIST_FALLOFF  fade with view distance (0=off, 1=on; default 1)

#include <Base/Vector.h>
#include <cstdint>

struct Scene;
struct Omni;

namespace fds {

struct SpotlightConeOverlay {
    static void render(Scene *sc);
};

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
