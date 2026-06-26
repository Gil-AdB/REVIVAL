#ifndef DEMO_BLASTER_BOLTS_H
#define DEMO_BLASTER_BOLTS_H

// Blaster / laser bolts — bright tapered energy shots that travel in a
// straight line and glow (Star-Wars-style). Scene-agnostic; first wired into
// greets for testing, destined for the chase scene.
//
// Rendering: a SCREEN-SPACE additive pass (the fountain-bolt model). Each bolt
// projects its two world endpoints to screen and draws a camera-facing quad
// through the 2-D clipper with a baked radial gradient texture — white-hot core
// fading to the coloured halo AND tapering to points at both ends. Drawn
// post-render (after lighting/tonemap), additive over the finished frame.
//
// Per-frame: BlasterBolts_Update(dt) early in the tick to advance/expire, then
// BlasterBolts_Draw() after the scene Render() (it billboards to the live View).

struct Vector;
struct Scene;

namespace fds {

void BlasterBolts_Init();
void BlasterBolts_Fire(const Vector &pos, const Vector &dir, float speed,
                       float r, float g, float b);
void BlasterBolts_Update(float dtTicks);
// Drive transient per-bolt Omnis so live bolts light the scene (deferred + fog).
// Call AFTER Animate_Objects, BEFORE the deferred lighting pass, passing the
// scene the renderer actually reads (e.g. GreetSc — NOT the global CurScene).
// Gated on --blaster_light. See BlasterBolts.cpp.
void BlasterBolts_EmitLights(Scene *Sc);
void BlasterBolts_Draw();

}  // namespace fds

#endif  // DEMO_BLASTER_BOLTS_H
