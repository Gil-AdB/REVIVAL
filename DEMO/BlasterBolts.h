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

// ── Pure-t reconstruction API (chase / §8.B snapshot determinism) ──────────
// The greets test harness accumulates bolt state (Fire → Update → expire). That
// can't survive the chase snapshot, which JUMPS Timer to t and ticks ONCE. The
// chase combat path instead REBUILDS the whole bolt set from a deterministic
// fire table every frame: BlasterBolts_ResetActive() drops all bolts, then one
// BlasterBolts_Place() per bolt alive at t places it at its lerped world
// position. No accumulation → identical for a given t regardless of how t was
// reached. EmitLights/Draw then consume the rebuilt set unchanged.
void BlasterBolts_ResetActive();
void BlasterBolts_Place(const Vector &pos, const Vector &dir,
                        float r, float g, float b);

// ── Muzzle / impact flashes (fountain bolt_flash envelope, pooled) ─────────
// A pool of transient, non-stationary Omni_FogTransient lights (like the
// fountain strike-flash) that light nearby geometry + the froxel fog at muzzles
// and impacts. Driven pure-t: each frame ResetFlashes(Sc) idles the pool, then
// AddFlash() places each active flash (intensity is the caller's exp-decay
// envelope value in [0,1]). ISize = --blaster_flash_peak × intensity, auto-
// scaled by hdr_glow_scale() under --hdr (fountain parity). Call between
// Animate_Objects and the lighting/reflection passes, same slot as EmitLights.
void BlasterBolts_ResetFlashes(Scene *Sc);
void BlasterBolts_AddFlash(const Vector &pos, float intensity,
                           float r, float g, float b);

// Mirror the live bolts across the water plane (y = waterY) and draw them as
// dimmed additive billboards, so the bolt streaks reflect on the water (§8.E,
// cheap: bolts only, NOT dense particles). Draw AFTER the reflection Render and
// BEFORE the main pass, so the transparent water blends over them exactly like
// the reflected world. Occlusion vs mirrored terrain is the filler's Z-test.
void BlasterBolts_DrawReflected(float waterY);

}  // namespace fds

#endif  // DEMO_BLASTER_BOLTS_H
