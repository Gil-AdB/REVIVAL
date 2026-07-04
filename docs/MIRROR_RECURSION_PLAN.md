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
  eye + reflected basis. Kept for reference/analytic use; the render path
  below does NOT use its det=-1 basis (see the decision).

- **Design decision — CHOSEN: forward-basis recursive RTT on the REAL panels**
  (supersedes the earlier reflected-basis+cull-flip idea). A reflected-basis
  camera has a det=-1 basis → flipped screen winding → fights the rasterizer's
  back-face cull AND fill-area sign. The existing RTT path already avoids this
  with a forward-facing basis + panel-window off-axis projection (proven,
  no winding issues). So recurse THAT, not a reflected-basis portal:
  - Every real mirror panel gets a first-order-style RTT material/texture
    (like the existing order-1 RTT that retargets REAL panel faces — no clone
    geometry, so no clone-of-clone and no combinatorial slots).
  - Render **N deepest-first passes**. Pass k renders each panel's reflected
    view (forward basis, panel window, near plane at the mirror) into its
    texture, sampling the OTHER panels' pass-(k-1) textures. Pass 1's nested
    panels show black = depth-0 (no further reflection); each pass adds a
    bounce. Camera reflection lives in the ray geometry + shared UV stamp
    (texel(W) = scene along camPos→W), exactly as order-1/2 RTT already does.
  - Cost: N × (panels) offscreen renders; footprint-prune + adaptive res as
    the existing RTT already does. Serial across passes (deepest-first).

- **Slice 1b: implement the multi-pass recursive RTT.** (i) generalize
  first-order RTT to cover every BuildMirror panel; (ii) wrap the slot render
  in a deepest-first N-pass loop gated by `--mirror-recurse-depth`; (iii) at
  pass boundaries keep panel textures from the previous pass visible.
  Validate on cloaktest dump poses: probe1 hero occluded, back1-4 void panels
  fill, viewer see-through deepens with depth (bg-green rises, black falls).

### Slice 1b — IMPLEMENTED, partial (this session)

Landed in `GreetsMirror.cpp` + `CloakTest.cpp`, all gated on
`--mirror-recurse-depth>0` (depth 0 = byte-identical legacy; greets build
verified unchanged: "3 mirrors + 0 RTT" at depth 0):

1. **Verdict demotion** (`BuildMirrorsByTextureName`): when `recurse>0`, a
   would-be CLONE mirror (`verdict 'built'`) is demoted to a first-order RTT
   panel (`'rtt'`). Clones are hidden in every RTT bake (depth-0 dead end);
   RTT panels are real faces that stay visible, so they can appear inside
   each other's bakes.
2. **`byMatName` option** on `BuildMirrorsByTextureName` — routes a hand-built
   scene's *named* mirror panels through the RTT-slot builder (cloaktest uses
   this instead of the clone-only `BuildMirror`).
3. **Off-screen slot baking** (`RenderSecondOrderMirrors`): in recurse mode a
   slot with no MAIN-screen footprint is still baked (full-frame) — a mirror
   is usually visible only *inside another mirror* (the far wall of a tunnel
   sits behind the camera). Without this the far mirror stays black forever.
4. **Opaque recurse panels**: the RTT bake fills only the OPAQUE G-buffer, so
   the greets `Mat_Transparent` glass panels are invisible inside each other's
   bakes. Recurse panels are made OPAQUE so a mirror renders into every bake.
5. **N-pass bake loop**: `numPasses = recurseDepth`; single-buffer in place, so
   a later panel in a pass already sees earlier panels' new textures.

**Validation scene**: `--scene-cloaktest FDS_CLOAK_PARALLEL=1` — two mirrors
facing each other (infinity tunnel), the *unambiguous* recursion test. (The
V-cloak can't validate: its two V's present opaque BACKS to each other's
reflected cameras — nothing to bounce — which is the "unresolved cloak optics"
noted in memory, a scene-geometry problem, not the engine.)

**Result**: recursion WORKS but the first cut washed out to white and
converged after one bounce (depth 2 == depth 4).

### Slice 1b follow-up — washout FIXED (this session)

The washout was **misdiagnosed** as "panels re-lit by scene omnis each
bounce". The deferred kernel actually displays a Lum=1/Diffuse=0 textured
material as `texel * 250/256` — the 250 saturation cap absorbs Luminosity's
255 base *plus any omni spill*, so the kernel is a near-passthrough (mild
2.3% decay), NOT a brightener. The real washout was the bake's
**half-silvered text composite**: `texel = baseTex + reflection*0.55`
(mirror_rtt_gain) adds the panel's base texture at full strength every
bounce — `v' = base + 0.55·v` has its fixed point past white. Two fixes,
both gated on recurse:

1. **Pure-mirror panels**: recurse slots don't capture `textTex`, so the
   composite never runs (no base add, effective gain 1.0). Greets'
   text-over-reflection look under recursion = the slice-3 composite item.
2. **Passthrough compensation**: the stored texture is pre-multiplied by
   256/250 (rounded), exactly cancelling the kernel's display attenuation —
   bit-stable bounces (±1 LSB), guaranteed by the clamp regardless of
   scene lights.

Plus a harness fix: cloaktest **resets slot textures to build content
(black) before every pose** — textures persist across bakes, so pose 2+
used to inherit pose 1's converged tunnel and its cross-depth diffs
measured nothing.

**Validated** (FDS_CLOAK_PARALLEL tunnel, 2 poses × depths 1-4): a real
receding infinity tunnel with stable brightness and correct colors. Depth
is monotone: each +1 fills the next nested black core (off-axis pose 1→2 =
~357k px, 2→3 = ~2k px, 3→4 = 0 — repeats shrink sub-pixel, genuine
structural convergence). Depth-0 gates: render_gate 3/3 PASS, greets build
unchanged, all 9 V-cloak poses byte-identical.

**Remaining limitation (→ slice 2/3):**
- **Geometric approximation**: each panel is baked from its OWN order-1
  reflected camera and shares ONE texture. That texture is only correct for
  looking *directly* at the panel, not for the panel *seen through another*
  mirror — so nested bounces are geometrically crude (parallel facing
  mirrors happen to be the friendly case: the order-1 virtual camera is
  nearly right on-axis, which is why the tunnel reads well). Exact nesting
  needs per-context reflected cameras (the recursive `renderView(vcam,
  depth-1)` tree in the pseudocode above).
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
