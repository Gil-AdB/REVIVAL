#ifndef DEMO_BLASTER_BOLTS_H
#define DEMO_BLASTER_BOLTS_H

// Blaster / laser bolts — short bright additive projectiles that travel in a
// straight line and bloom (Star-Wars-style energy bolts). Scene-agnostic so
// it can be driven from any scene tick; first wired into greets for testing,
// destined for the chase scene.
//
// Rendering: one dynamic TriMesh holding a pool of velocity-stretched,
// camera-facing quads. Each quad is an ADDITIVE forward face that accumulates
// into the HDR buffer, so the bolt cores blow past 255 and the --bloom pass
// turns them into glowing shots. Z-tests against the scene (a bolt is hidden
// behind a wall) but does not write Z.
//
// Call order per frame: BlasterBolts_Update AFTER the camera/view is set for
// the frame (it billboards to the live View) and BEFORE Transform_Objects
// reads mesh vertices.

#include "Base/Vector.h"

struct Scene;

namespace fds {

// Build the bolt mesh + additive material and register them in the scene.
// Idempotent-safe to call once at scene init.
void BlasterBolts_Init(Scene *sc);

// Spawn a bolt at `pos` travelling along `dir` (need not be normalized) at
// `speed` world-units per tick. Colour is 0..1 linear-ish (gets scaled hot
// for the additive core). No-op if the pool is full.
void BlasterBolts_Fire(const Vector &pos, const Vector &dir, float speed,
                       float r, float g, float b);

// Advance live bolts by `dtTicks`, expire spent ones, and rebuild the mesh
// geometry billboarded to `camPos`. Cheap when no bolts are live.
void BlasterBolts_Update(Scene *sc, float dtTicks, const Vector &camPos);

}  // namespace fds

#endif  // DEMO_BLASTER_BOLTS_H
