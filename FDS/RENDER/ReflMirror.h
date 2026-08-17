#ifndef FDS_RENDER_REFL_MIRROR_H_INCLUDED
#define FDS_RENDER_REFL_MIRROR_H_INCLUDED

// ReflMirror.h — the planar-water reflection pass's LIGHT state (--refl_correct).
//
// chase and city draw their water reflection by MIRRORING THE GEOMETRY, not the
// camera: per frame the tick runs `Reflected_Transform(Sc)` -> `Radix_Sort` ->
// `Render()` and then `Transform_Objects(...)` -> `Radix_Sort` -> `Render()`.
// `Render()` IS `renderFrame` (RENDER.CPP:1868), so the reflected pass is a
// COMPLETE second deferred frame with its own G-buffer, its own
// `Render_DeferredLighting` dispatch and therefore its own `ViewLightsSoA`.
// That is what makes this fix cheap: there is no need to disambiguate reflected
// from main pixels inside one dispatch (the greets `gb.mirrorId` machinery) —
// the whole pass is reflected, so the whole light list can be.
//
// WHAT WAS WRONG. `Reflected_Transform` mirrors each mesh's origin about the
// water plane and folds a mirror into its rotation, but the deferred light list
// is built from `O->IPos` in MAIN world space
// (DeferredSurfaceKernel.cpp's per-omni loop) and was never mirrored with it.
// A real planar mirror reflects the surface normal AND the light, so `N·L` is
// preserved; mirroring only the geometry lights a mirrored world with
// unmirrored lights. Combined with `Reflected_Transform` never writing `TN` at
// all (docs/OPTIMIZATION_BACKLOG.md 2026-08-16w: 19 092 of 19 092 reflected
// triangles arrive with a zero normal at chase t=100, and the *stale* form
// survives every frame after), the reflected pass has never had a correct
// normal or a correct light.
//
// WHAT THIS DOES. The scene arms the state around its reflected `Render()`;
// `Render_DeferredLighting` calls `ReflMirror_MirrorLights` immediately after
// its per-omni SoA build, which REWRITES each entry's view-space position, its
// world-space position and its spot axis from the light REFLECTED about the
// same plane the geometry was mirrored about. Nothing is appended, so the light
// count, the tile binning and every per-pixel loop are untouched — the cost is
// one pass over ~40 lights per reflected frame.
//
// The mirroring is deliberately kept OUT of DeferredSurfaceKernel.cpp's body:
// that TU is compiled -ffp-contract=fast and FDS/RENDER/ReflFaceCull.cpp
// records (with three moved scene pins as evidence) that straight-line code
// added to such a function re-schedules the floating point in the loops AROUND
// it. One `noinline` call across a TU boundary keeps every other scene's frame
// byte-identical.

#include <Base/Vector.h>

struct Scene;
struct Camera;
struct ViewLightsSoA;

namespace fds {

// The plane is the one `Reflected_Transform` ACTUALLY mirrors geometry about,
// which for both scenes is `P' = P - 2(P·N)N` — the plane through the WORLD
// ORIGIN with normal `RflSurfNorm`, i.e. d = 0. (city also keeps an
// `RflSurfOfs` for its fog reflection, but its geometry mirror ignores it; the
// lights must follow the GEOMETRY or the two desynchronise.)
struct ReflMirrorState {
	bool   active = false;
	Vector N      = Vector(0.0f, 1.0f, 0.0f);
	float  d      = 0.0f;
};
extern ReflMirrorState g_reflMirror;

// Arm / disarm around the reflected Render(). No-ops when --no-refl_correct,
// so the 1998 behaviour is one flag away.
void ReflMirror_Begin(const Vector &N, float d);
void ReflMirror_End();

inline bool ReflMirror_Active() { return g_reflMirror.active; }

// Rewrite the already-built light list into the mirrored world. Walks
// `Sc->OmniHead` with the SAME filter and order the SoA build used, so entry i
// belongs to the i-th active omni.
void ReflMirror_MirrorLights(ViewLightsSoA &lights, int numLights,
                             Scene *Sc, Camera *View);

// Reflect a point / a direction about the armed plane.
//
// NOTE, so nobody trusts this more than it deserves: this is NOT the only
// spelling of the mirror in the tree, it is the THIRD. The mesh-origin mirror is
// written out inline in each scene (`CHASE.CPP` / `CITY.CPP`, `L1 =
// -2*Dot(IPos, RflSurfNorm)` …), and each scene's reflected FLARE loop hardcodes
// a third (`ReflectedPos.y = -ReflectedPos.y`). All three agree only because
// city's `RflSurfNorm` — derived at load from the water mesh's own cross product
// — comes out axis-aligned, and because both scenes' geometry mirror passes
// through the world origin. If either ever stops being true, all three have to
// move together.
inline Vector ReflMirror_Point(const Vector &P, const Vector &N, float d)
{
	const float t = -2.0f * (P.x * N.x + P.y * N.y + P.z * N.z + d);
	return Vector(P.x + t * N.x, P.y + t * N.y, P.z + t * N.z);
}
inline Vector ReflMirror_Dir(const Vector &D, const Vector &N)
{
	const float t = -2.0f * (D.x * N.x + D.y * N.y + D.z * N.z);
	return Vector(D.x + t * N.x, D.y + t * N.y, D.z + t * N.z);
}

}  // namespace fds

#endif  // FDS_RENDER_REFL_MIRROR_H_INCLUDED
