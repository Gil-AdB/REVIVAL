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
extern bool g_offAxisFrustumCull;

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
