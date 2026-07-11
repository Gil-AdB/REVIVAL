#pragma once

// Shared procedural-water machinery, factored out of CITY.CPP so chase (which
// has the same water.lwo planar mirror) can use it too.
//
//   - a small SEAMLESS tiling wave-slope field (built once from integer-
//     wavevector sines), sampled as two scrolling layers → the shared field
//     that drives BOTH the reflection-displacement ripple and the specular
//     glints, so they ripple coherently;
//   - a procedural caustic-network texture (Worley cell edges), warped by the
//     same field and modulated over the water;
//   - the screen-space post-pass that ray-casts the water plane per pixel and
//     lays the caustics + view-dependent specular glints onto the framebuffer.
//
// The field + texture are global (one set, scene-independent). The glint pass
// takes the calling scene's water PLANE (height + extent), and reads the engine
// view/framebuffer globals (View, VPage, ZPage16, CurScene->FZP, ...), so the
// caller must have SetCurrentScene + the camera globals live before calling.

namespace pwater {

// Build the wave normal-field + caustic texture. Call once per scene init.
void BuildField();

// Wave slope (nx,nz) at world XZ for the shared field — used by the reflection
// dispMap as well as the glint pass. `scale` = water_bump_scale; `t` = animated
// time (Timer * 0.02 * ripple_speed).
void WaveSlope(float wx, float wz, float t, float scale, float& bnx, float& bnz);

// Screen-space caustic-texture + specular-glint post-pass over VPage. `waterY`
// is the water plane height; [minX,maxX]/[minZ,maxZ] are the plane extent (used
// only as a "water set up yet" early-out). No-op when water_bump is off / extent
// unset / no View. Row-parallel across the thread pool.
void RenderGlints(float waterY, float minX, float maxX, float minZ, float maxZ);

// Caustic-cell modulation factors at world XZ — the EXACT formula of
// RenderGlints' texMix block (keep in lockstep; not shared with that hot loop
// so the screen pass keeps its single wave-slope evaluation per pixel), for
// callers that shade water OUTSIDE the screen pass (the city env-bake
// procedural water re-shade). On return the caller applies:
//     B = B*mod + blueAdd;  G = G*mod + blueAdd*0.40;  R = R*mod + blueAdd*0.08
// `t` is the wave clock, `scale` = water_bump_scale, texMix/texScale/texWarp =
// the water_albedo_mix/water_tex_scale/water_tex_warp values resolved by the
// caller, flowU/flowV = the caustic UV translation (0 for a frozen bake).
// Returns false (factors untouched) when texMix<=0 or BuildField hasn't run.
bool CausticModulation(float wx, float wz, float t, float scale,
                       float texMix, float texScale, float texWarp,
                       float flowU, float flowV,
                       float& mod, float& blueAdd);

}  // namespace pwater
