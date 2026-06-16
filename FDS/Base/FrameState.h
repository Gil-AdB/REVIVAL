#ifndef FDS_FRAME_STATE_H_INCLUDED
#define FDS_FRAME_STATE_H_INCLUDED

// Transitional header. Phase 2 of the re-entrant refactor split the
// per-pass state into two separate structs (`CameraContext` and
// `FaceListContext`); their canonical singletons are `g_mainCamera`
// and `g_mainFaces`, declared below. `FrameState` is kept only so
// existing `Transform_Objects(..., FrameState*)` callers still compile
// while phase-2 migrates them across; new code should take the two
// context refs directly.

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
