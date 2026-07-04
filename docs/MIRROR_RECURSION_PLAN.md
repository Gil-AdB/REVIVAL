# Recursive mirror reflection — campaign plan

Goal: reflections that resolve to arbitrary depth N, so a mirror seen in a
mirror seen in a mirror … renders correctly. Motivating + validation scene:
`--scene-cloaktest` (the 4-mirror periscope cloak), whose see-through needs
**4 reflection bounces** and today renders as a patchwork with black holes.

## Where we are (order ≤ 2)

Two mechanisms, in `FDS/RENDER/GreetsMirror.cpp`:

1. **Order-1 = geometry clones.** `BuildMirror` clones every other mesh with
   world positions reflected across the mirror plane (winding swapped), plus
   reflected omnis. The main deferred pass draws the clones, masked to the
   mirror's screen footprint via `gb.mirrorId == mirrorTag` (Mekalele).
2. **Order-2 = RTT slots.** `PrepareSecondOrderMirrorRtt` builds a slot per
   ordered pair (A,B): the faces are *A's clones of B's panel*, retargeted to
   a material whose texture `RenderSecondOrderMirrors` re-renders each frame
   from the **doubly-reflected** virtual camera.

**The hard cap** (GreetsMirror.cpp ~2588): the RTT render *hides every clone
mesh* ("the RTT view must show the REAL scene only"), so inside a mirror's
texture there are no further reflections. Order-2 is the ceiling, and even
that only for the specific A→B panel pairs the slot generator enumerates.
Depth-3 would need A's clone of (B's clone of C's panel) — a clone-of-clone
that doesn't exist (clones are one level); compound cloning was rejected
(see [[mirror-order2-rtt-plan]]).

## Chosen approach: recursive RTT (live, no precomputed slots)

This IS the mirror-RTT mechanism — just driven by a recursion instead of a
fixed order-2 pair list. Each real mirror panel is a portal that displays a
render-to-texture of its reflected view; inside that texture, other panels
show their one-level-deeper textures.

```
renderView(camera, depth):
    render scene from `camera`                       # into a texture (or fb region)
    if depth == 0: return                            # deepest level: no more bounces
    for each mirror panel M visible in this view:
        vcam = reflect(camera) across M.plane
        renderView(vcam, depth-1)  ->  into M's RTT texture     # DEEPEST-FIRST
        draw M's REAL panel showing that texture (masked to its footprint;
        half-silvered composite = text + reflection/2 for glass)
```

The main frame is `renderView(mainCam, N)`. Reflected-view culling reuses
`g_offAxisFrustumCull` (asymmetric frusta) and the `SetCurrentScene`
re-stamp discipline the RTT path already established.

**It forces render order — deepest-first — and that's the point.** A
mirror's texture must be finished before the view that displays it, so the
depth passes are serial (parallel only *within* a pass). A reflection cycle
(A→B→A→…) has no fixed point, so we cut at depth N; the deepest level shows
the base scene with no further reflection (today's "black hole" = depth 0).

**Why not just extend the existing RTT slots:** the current RTT precomputes
a slot per ordered PAIR (A,B) and displays it on CLONE-of-B geometry inside
A. Depth 3 that way needs clone-of-clone geometry (doesn't exist), and
per-path slots explode (4 mirrors × depth 4 → up to 256). Live recursive RTT
computes the reflected cameras on the fly and displays on the REAL panels —
nothing per-path to precompute; O(visible-mirrors^depth), footprint-pruned.
Same machinery (OffscreenViewScope, off-axis projection, texture-per-panel
that first-order RTT already uses), recursion-driven.

## Slices

- **Slice 0 (this commit): foundation.** `--mirror-recurse-depth=N` flag
  (`FDS_MIRROR_RECURSE_DEPTH`, default 0 = current behavior). Plan doc.
  cloaktest G-key pose dump already in for break-pose capture.
- **Slice 1a (done, 3502dbb): `MirrorReflectedCamera` primitive** — reflected
  eye + reflected basis (det=-1 → render with inverted back-face cull).
- **Design decision:** use the **reflected-basis + cull-flip** portal (render
  the real scene from `MirrorReflectedCamera`, invert cull, clip to the
  mirror's screen footprint via the existing per-pixel `gb.mirrorId` mask),
  NOT the existing RTT's forward-basis + panel-window projection. The
  reflected-basis form generalizes to any mirror seen at any angle and
  composes recursively without per-panel window bookkeeping. (Watch: the
  rasterizer cull sign — Mekalele/TheOtherBarry — must be flippable per pass.)
- **Slice 1b: single-mirror recursion render.** For each mirror visible in a
  view, render the `MirrorReflectedCamera` view to an offscreen target,
  composite into the mirror's `gb.mirrorId` pixels, recurse `depth`. Ignore
  multiple-mirrors-per-view ordering first. Validate: two parallel facing
  mirrors show the tunnel to depth N. **Needs interactive validation (G-key
  poses) — the cloaktest dump only covers fixed angles.**
- **Slice 2: multiple mirrors per view.** Loop over all mirrors visible in
  each reflected view; per-mirror scissor. Validate on cloaktest: the black
  hole fills at depth 2, more at 3, and the bg pillar appears at depth 4.
- **Slice 3: perf + budget.** Depth/footprint budget (stop recursing when a
  mirror's footprint < K px), reuse the RTT texture pool, measure. Wire the
  half-silvered composite through the recursion.
- **Slice 4: fold order-1/2 into the recursive path** (or keep clones for
  depth-1 as the measured-faster base and recurse only ≥2 — decide by perf).

## Validation harness

- `--scene-cloaktest` + `FDS_CLOAK_DUMP=1`: pose dump. Metrics: viewer-pose
  green(bg) px should rise with depth; hero-red stays 0; central-band black
  px should fall. `FDS_CLOAK_NOMIRROR=1` = geometry ground truth.
- `tools/render_gate.sh` mirrortest golden must stay byte-identical at
  depth=0 (the flag off is the gate).
- G key (cloaktest interactive) prints `Pose{}` lines for break cases.

## Hard-won facts to respect (from the order-2 work)

- Writing `Sc->NZP` alone never moves the near plane — re-stamp via
  `SetCurrentScene` (OffscreenViewScope.setNearZ does this).
- Mesh frustum cull is symmetric-only; off-axis reflected frusta need
  `g_offAxisFrustumCull`.
- Face U1..V3 are authoritative; re-stamp with `uvFromVertices()` after
  poking verts.
- `g_engineSurfaceMutex` is non-reentrant; nested offscreen renders must not
  relock it (gate on `g_offscreenViewDepth`). Recursion needs a depth-aware
  scope, not N nested locks.
