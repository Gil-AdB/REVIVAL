// ReflMirror.cpp — see ReflMirror.h for what this is and why it is its own
// translation unit.

#include <RENDER/ReflMirror.h>

#include <Base/FDS_DEFS.H>
#include <Base/FDS_VARS.H>
#include <Base/FDS_DECS.H>
#include <Base/FeatureFlags.h>
#include <Base/Scene.h>
#include <Base/Camera.h>
#include <Base/Omni.h>
#include <RENDER/DeferredCommon.h>
#include <RENDER/OffscreenView.h>   // g_offscreenViewDepth: nested-bake guard

namespace fds {

ReflMirrorState g_reflMirror;

void ReflMirror_Begin(const Vector &N, float d)
{
	if (!FeatureFlags::refl_correct()) return;
	g_reflMirror.active = true;
	g_reflMirror.N      = N;
	g_reflMirror.d      = d;
}

void ReflMirror_End()
{
	g_reflMirror.active = false;
}

// noinline + its own TU: this must not be allowed to perturb the code
// generation of Render_DeferredLighting (see the header's last paragraph).
#if defined(_MSC_VER) && !defined(__clang__)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void ReflMirror_MirrorLights(ViewLightsSoA &lights, int numLights,
                             Scene *Sc, Camera *View)
{
	if (!g_reflMirror.active || !Sc || !View || numLights <= 0) return;

	// NESTED-BAKE GUARD — the one way this feature could do PERSISTENT damage.
	// The scene arms the mirror around its reflected `Render()`, which is a
	// depth-0 `renderFrame`; and `renderFrame`'s prologue (RENDER.CPP:520) runs
	// `EnvReflection_FramePrep` — likewise gated on `g_offscreenViewDepth == 0`
	// — INSIDE that armed pass whenever `--env_refl` is live. That prep bakes
	// env-probe cube faces through six nested `OffscreenViewScope` renders, each
	// of which reaches `Render_DeferredLighting` again. Without this line those
	// faces would be lit by MIRRORED lights and then WRITTEN TO THE ON-DISK CUBE
	// CACHE (Runtime/cache/city_envmap_cube_*.bin) — a corruption that outlives
	// the process and that no pin would catch, because the pin runs one tick on
	// an already-warm cache.
	//
	// `env_refl` is NOT the dormant default the flag text used to claim: greets'
	// PBR metallic import calls `setDefault(env_refl, true)` process-globally
	// (MaterialImport.cpp:865), greets inits FIRST, and the setting is one-way —
	// so in a full demo run city and chase render with it ON.
	//
	// The gate is exact rather than conservative: the reflected pass IS depth 0
	// and stays mirrored; every nested bake is depth > 0 and stays main-space,
	// which is what a probe cached across frames must be.
	if (g_offscreenViewDepth != 0) return;

	const Vector N = g_reflMirror.N;
	const float  d = g_reflMirror.d;

	// INVARIANT, and the only thing that can silently break this function: the
	// walk below must stay bit-for-bit the same traversal the SoA build in
	// Render_DeferredLighting performs — same head, same `Omni_Active` filter,
	// same order, same cap — because it identifies entry i with the i-th active
	// omni by POSITION and nothing else. If a filter term is ever added to one,
	// it has to be added to the other or the lights silently swap identities.
	int i = 0;
	for (Omni *O = Sc->OmniHead; O && i < numLights; O = O->Next) {
		if (!(O->Flags & Omni_Active)) continue;

		// Position: reflect in world space, then re-place it in the pass's
		// view space with the same eye and the same view rotation the mirrored
		// GEOMETRY was transformed by. (`Reflected_Transform` mirrors the mesh
		// origin and leaves the camera alone, so view space is unchanged and
		// only the light's world position moves.)
		Vector wp = ReflMirror_Point(O->IPos, N, d);
		Vector u, w;
		Vector_Sub(&wp, &View->ISource, &u);
		MatrixXVector(View->Mat, &u, &w);
		lights.posX[i] = w.x;
		lights.posY[i] = w.y;
		lights.posZ[i] = w.z;
		lights.posWorldX[i] = wp.x;
		lights.posWorldY[i] = wp.y;
		lights.posWorldZ[i] = wp.z;

		// Spot axis: a direction, so it takes the plane's linear part only.
		if (lights.isSpot[i]) {
			Vector wd = ReflMirror_Dir(O->IDir, N);
			Vector dv;
			MatrixXVector(View->Mat, &wd, &dv);
			Vector_Norm(&dv);
			lights.dirX[i] = dv.x;
			lights.dirY[i] = dv.y;
			lights.dirZ[i] = dv.z;
		}

		// SHADOWS. A mirrored light's visibility of a mirrored receiver equals
		// the ORIGINAL light's visibility of the ORIGINAL receiver, so the
		// existing clone machinery is exactly right here: move this light's own
		// map index into `srcShadowMapIdx` / `srcCubeShadowIdx` (which the
		// kernel samples at the receiver REFLECTED across mirN/mirD — and the
		// reflected pass's receiver reflects straight back into the real world)
		// and hand it the water plane. Costs no extra bake.
		//
		// INERT IN BOTH SCENES TODAY, and more strongly than "they bake none":
		// CHASE.CPP never calls ShadowMaps_Rebuild AT ALL, so --shadows cannot
		// switch this on there — the path is unreachable, not merely idle. City's
		// single call sits behind --city_test_spots (default 0). Wired anyway so
		// the pass stays correct if either scene ever grows shadows; verified
		// byte-null by --shadow_plane_hash (greets' 28+12-map stream is identical
		// across this change, cum 51344bf5f3816c23; chase/city emit no stream).
		if (lights.shadowMapIdx[i] >= 0 || lights.cubeShadowIdx[i] >= 0) {
			if (lights.srcShadowMapIdx[i] < 0)
				lights.srcShadowMapIdx[i] = lights.shadowMapIdx[i];
			if (lights.srcCubeShadowIdx[i] < 0)
				lights.srcCubeShadowIdx[i] = lights.cubeShadowIdx[i];
			lights.shadowMapIdx[i]  = -1;
			lights.cubeShadowIdx[i] = -1;
			lights.mirNX[i] = N.x;
			lights.mirNY[i] = N.y;
			lights.mirNZ[i] = N.z;
			lights.mirD [i] = d;
		}

		// LEFT UNMIRRORED, ON PURPOSE — listing them so a later reader can tell
		// "decided" from "overlooked":
		//   colR/G/B, range2/rRange, cosInner/cosOuter, isSpot, isFlash,
		//     forceCone, sinOuter, halo*, coneGain — scalars of the light
		//     itself; a mirror is an isometry, so none of them change.
		//   winMin*/winMax* (the bounce-cone window AABB) and mirN*/mirD on
		//     non-shadow lights — Omni_BounceCone exists only in greets, so in
		//     chase and city the AABB stays inverted and the kernel's portal
		//     test is gated off entirely.
		//   mirrorId — deliberately left at 0. These are not clone lights: the
		//     whole PASS is mirrored, so they must light every pixel of it. A
		//     nonzero id would be rejected at every pixel (chase and city never
		//     allocate gb.mirrorId, so the per-pixel `pmid` is hard 0) and the
		//     reflection would go black.
		++i;
	}
}

}  // namespace fds
