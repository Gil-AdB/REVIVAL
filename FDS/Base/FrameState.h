#ifndef FDS_FRAME_STATE_H_INCLUDED
#define FDS_FRAME_STATE_H_INCLUDED

// Transitional header. Phase 2 of the re-entrant refactor split the
// per-pass state into two separate structs (`CameraContext` and
// `FaceListContext`); their canonical singletons are `g_mainCamera`
// and `g_mainFaces`, declared below. `FrameState` is kept only so
// existing `Transform_Objects(..., FrameState*)` callers still compile
// while phase-2 migrates them across; new code should take the two
// context refs directly.

#include <cstdint>

#include "CameraContext.h"
#include "FaceListContext.h"
#include "Vector.h"
#include "RenderTarget.h"

namespace fds {

// New canonical singletons for the main render pass. The legacy global
// names (CAll, FList, FOVX, View, …) declared in FDS_VARS.H are
// reference aliases into the fields of these two structs.
extern CameraContext   g_mainCamera;
extern FaceListContext g_mainFaces;

// Off-axis projection support for offscreen passes (mirror RTT): when
// set, Transform_Objects replaces its folded mesh-level lateral cull
// — the fabs(S.x) form models a SYMMETRIC frustum and discards
// everything when the projection center lies outside the viewport —
// with four individual sphere-vs-viewport-plane tests that are valid
// for any projection center. Survivors get Tri_Inside cleared and
// take the fully-clipped per-vertex path (whose PX/PY screen-bound
// flags are off-axis-correct). Depth classification is unchanged.
// Set + restore around the offscreen Transform call; the main pass
// keeps the cheaper folded test.
// thread_local so Slice 6's parallel shard workers each carry their own
// off-axis flag; byte-neutral for serial callers (set+read same thread).
extern thread_local bool g_offAxisFrustumCull;

// Per-vertex cone cull for the mirror-shard reflection pass. When enabled,
// meshes with a worldVerts cache do a cheap world-space cone test per vertex
// and skip the view transform for vertices outside the shard's (narrow,
// off-axis) reflection frustum — the bulk of the room is rejected before the
// matmul. Apex MUST equal the reflection camera's ISource (Er) so the world
// delta doubles as the view delta (same trick as the cube-shadow path). Set
// + cleared around each shard's Transform_Objects; never set by the main
// pass or shadows. (Defined in FrameState.cpp.)
// thread_local: each parallel shard worker (Slice 6) owns its reflection cone.
extern thread_local bool   g_reflVertCull;
extern thread_local Vector g_reflConeApex;   // = Er (camera ISource)
extern thread_local Vector g_reflConeDir;    // = shard normal (cone axis, unit)
extern thread_local float  g_reflConeTan2;   // tan²(half-angle): perp² > tan2·axisDist² → out

// Per-FACE cone cull for the same pass — --shard_cone_cull=2, compiled only
// into a lab build (-DFDS_SHARD_BAKE_LAB=ON; the top-level CMakeLists.txt says
// why, and docs/OPTIMIZATION_BACKLOG.md says why it is PARKED).
// It replaces the per-VERTEX form above, which was unsound by construction: it
// decided FACE visibility from VERTEX positions and stamped the rejects with
// fake screen positions, so a quad whose INTERIOR covered the whole shard view
// vanished and a straddler rasterized through the fakes (ddb1d15). This one
// tests the FACE's own world bounding sphere against the cone and rejects the
// face WHOLE — no vertex is ever touched, so a surviving face renders exactly
// as with no cull at all. That is the correctness invariant, and it is why the
// arm measures 0 of 1 048 576 atlas pixels different from no cull.
// The apex is shared with the per-vertex globals above (g_reflConeApex = Er);
// the AXIS and the half-angle are this test's own, and that is the whole
// reason it culls anything. The legacy cone points along the shard NORMAL, and
// the reflected eye does not look at its own shard — the window sits metres off
// to the side — so that cone has to open to 17-19° on greets just to reach it
// (measured), not the ~1° the window subtends. MirrorShatter's shardFaceCone
// builds these two by INVERTING the shard's actual off-axis projection at the
// four screen corners, which assumes nothing about the camera basis (it is not
// orthonormal: a shard quad is not a rectangle). Superset by convexity — a
// circular cone of half-angle < 90° is convex and the frustum is the convex
// hull of its four corner rays. (Defined in FrameState.cpp.)
extern thread_local bool     g_reflFaceCull;
extern thread_local Vector   g_reflFaceConeDir;   // apex → viewport centre, unit
extern thread_local float    g_reflFaceConeTan2;  // tan²(half-angle) about THAT axis
// Cull census, summed per shard by MirrorShatter and printed with the
// [SHARD-CULL] line under FDS_SHARD_REFL_PROF. thread_local so the per-face
// increments cost no atomic.
extern thread_local uint64_t g_reflFaceTested;
extern thread_local uint64_t g_reflFaceCulled;
extern thread_local uint64_t g_reflFaceDrawn;   // FList entries the bake pushed

// Per-phase core-ms accumulators for the shard bake's [SHARD-PHASE] line
// (FDS_SHARD_REFL_PROF). This is the attribution that settled what the bake
// costs — 4 % geometry front-end, 78 % Render_DeferredLighting — and therefore
// why the cull above is parked. Written only while that env var is set.
extern thread_local double   g_phSetup;
extern thread_local double   g_phXform;
extern thread_local double   g_phRaster;
extern thread_local double   g_phFill;
extern thread_local double   g_phLight;
extern thread_local double   g_phCone;

// Snapshot of the current main-pass render target globals (VPage,
// ZPage16, XRes, YRes, VESA_BPSL, g_gbuffer*, g_xparZ*) into a
// RenderTarget. Phase 4 dispatch sites use this to bridge the
// signature change while filler bodies still read globals; once the
// bodies migrate to read from the arg, the dispatcher can stop
// rebuilding from globals and pass an explicit per-pass target.
RenderTarget MainRenderTargetFromGlobals();

// Legacy aggregate. No longer the source of truth — kept as a thin
// compatibility shim so any code that still names `FrameState` keeps
// resolving while phase 2 migrates. New code: don't extend this.
struct FrameState {
    // (Empty: fields moved to CameraContext + FaceListContext.)
};
extern FrameState g_mainFrame;

} // namespace fds

#endif
