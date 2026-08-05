# Visibility culling — research + campaign plan

Status: **research complete, 2026-08-02; §0's verdict CHALLENGED by the user and
RE-TESTED WITH CODE 2026-08-03 — see §7 for the experiment (finer chunking + a
prev-frame hi-Z occlusion cull, both landed default-OFF) and the revised verdict.**
Prompted by the user direction: *"it's
about time to seriously think about visibility culling (bsp? bvh? something better?)
even before we start transforming the polys — this is becoming a very big chunk of
the work, and I think this will greatly help later stages of the pipeline."*

This doc measures the actual occlusion waste in the current pipeline, surveys the
architectures against that waste, and lands a recommendation. It follows the
`ENVDYN_DISPLACEMENT_PLAN.md` house style: verified anchors, measured decision
points, slices with byte-gates and explicit non-goals.

---

## 0. TL;DR — the verdict (read this first)

**Do NOT build a general occlusion-visibility architecture (cells & portals, PVS,
BSP, BVH, or a software occluder-raster) right now.** The measurements below show
the reclaimable envelope is small *because the engine already solved the problem
occlusion culling exists to solve*, three times over:

1. **It is a DEFERRED renderer.** Shading runs once per *final screen pixel*, not
   per face. Measured: the shaded-pixel count (`zWritten`) is **~2.08 M on greets
   at any face count** — flags-off (6.7 k faces) and displaced (73 k faces) both
   shade the same ~2.08 M pixels. The ~44 ms of deferred lighting + cube-shadow
   tap (the frame's biggest block) is **invariant to geometry and occlusion.**
   Occlusion culling cannot touch it. In a *forward* renderer it could — that's
   the classic win — but the engine already banked that by going deferred.
2. **Front-to-back sort + Z-early-reject already makes occluded pixels cheap.**
   The Mekalele G-buffer fill gates its expensive per-pixel work (texel gather,
   normal encode, G-buffer store) *behind* the Z-test — a z-failed lane pays only
   an edge test + a Z read/compare. So the measured 40–63 % "wasted" fill samples
   are the cheap samples, not full fills.
3. **The geometry front-end is already culled** by the existing bsphere-vs-frustum
   mesh cull + the greets Piramid chunk split + the B5 `--tile_bbox_cull`
   per-tile face pre-reject. Measured: those already fire on 35–83 % of meshes and
   reclaim ~17.6 ms at the 73 k-face displaced pose.

**Specifically on the user's ask ("before we start transforming the polys"):** a
*perfect* pre-transform occlusion cull saves **0.05–0.8 ms** (see §2). The reason
is subtle and measured: occlusion here is overwhelmingly **face-level within
visible chunks**, not **chunk-level**. 73–90 % of *rastered faces* are fully
occluded, but only **3–27 % of transformed verts** live in a *fully-occluded
mesh/chunk* — because each spatial chunk that sits behind the front wall still
keeps at least one visible face, so it can't be rejected wholesale. The transform
cost the user is worried about is spent on geometry that is genuinely visible (the
wall you're looking at); occlusion won't cut it. The right levers for that cost are
the in-flight **SoA vertex refactor** (faster transform) and **chunk LOD**
(`docs/OPTIMIZATION_BACKLOG.md` S5) — not visibility.

What ships from this research: **Slice 0 — a permanent, default-off `--vis_stats`
diagnostic** so this decision stays measured as scenes evolve, and a documented
**trip-wire** (§3) for the one scenario that could flip the verdict.

---

## 1. Verified anchors (the pipeline reality this stands on)

- **Deferred path** (`docs/GRAPHICS_PIPELINE.md`): G-buffer fill (`RenderInnerMekalele`
  → `Mekalele.h`) → per-pixel `Render_DeferredLighting`. Shading is per screen pixel.
- **Front-to-back sort** (`Transform.cpp:66` `FRONT_TO_BACK_SORTING`; radix in
  `SORTS.H`). Closer faces first → farther faces z-fail and skip work.
- **Z-early-reject in the fill** (`Mekalele.h` `apply_exact`): `p_mask &= zmask`
  precedes the `if (barry::any_lane_set(p_mask))` block that does the texel gather
  + G-buffer store. **Occluded pixels skip the expensive work already.**
- **Existing geometry culls:** per-mesh bsphere-vs-6-frustum-plane cull
  (`Transform.cpp:982–1082`, `continue` on `Tri_Invisible`); greets Piramid split
  into an N³ chunk grid (`GREETS.CPP:2016+`, default grid 8); B5 per-tile face
  screen-bbox pre-reject (`RenderInner.cpp`, `FListEntry.bbMin/Max`,
  `--tile_bbox_cull` default ON, commit 9b6d70d).
- **No prior geometry-visibility system exists.** `WorldAabb.h` states its
  non-goals verbatim: *"no BVH, no occlusion, no precomputed visibility."* The
  word "portal" in the code (`bouncePortalReject`, GreetsMirror) is a *light /
  reflection* gate, not a visibility portal. Prior **light** contribution-culling
  (`--contrib-cull-thresh`) was built and **reverted** as a measured no-win with
  tile-boundary artifacts (149ba77). S3 mesh-AABB-vs-frustum cull was measured
  **not worth it** (backlog). The mirror campaign hit *"chunk culls constantly
  firing → stale mirror footprints"* (`GreetsMirror.h:108`) — a caution that
  aggressive per-chunk culls interact badly with the offscreen passes.
- **Measurement method:** temporary `--vis_stats` counters (reverted; see §5).
  Per main-camera frame it reports, from the opaque `FList` + a per-face won-pixel
  tally set in the Mekalele commit: faces WON vs FULLY-OCCLUDED (z-fail every
  pixel), G-buffer edge-covered vs z-written samples, per-mesh(chunk) occlusion +
  vert counts, and Transform meshes-seen / -xformed. Validated: `vertsXformed`
  tracks the documented XFRM linearly (flags-off 82,975 verts ↔ 0.70 ms;
  displaced 958,339 verts ↔ 7.8 ms — 11.5× both).

---

## 2. PHASE 1 — the waste, measured (1920×1080, this machine)

### 2a. Per-pose occlusion + overdraw

| pose | frame ms | opaque faces | faces fully occluded | fill overdraw (edge/written) | wasted fill samples | verts transformed | verts in fully-occluded meshes |
|---|--:|--:|--:|--:|--:|--:|--:|
| greets flags-off t=5780 | 48.7 | 6,769 | **73.0 %** | 1.90× | 47.3 % | 82,975 | **7.1 %** |
| greets flags-off t=2145 | ~48 | 4,709 | **90.2 %** | 1.70× | 41.1 % | 73,400 | **12.7 %** |
| greets displaced t=5780 | 88.8 | 73,258 | **89.1 %** | 1.96× | 49.0 % | 958,339 | **10.2 %** |
| greets displaced t=2145 | ~90 | 51,261 | **88.3 %** | 1.61× | 37.7 % | 862,283 | **3.0 %** |
| city t=1961 | 99.0 | 19,606 | **73.4 %** | 2.22× | 55.0 % | 138,550 | **8.0 %** |
| chase t=800 | ~ | 25,827 | **89.2 %** | 2.67× | 62.6 % | 30,548 | **27.5 %** |

`zWritten` (shaded opaque pixels) = greets ~2.08 M (full screen), city 1.13 M
(45 % sky), chase 0.22 M (11 % — mostly sky + transparent water). **This is what
the deferred kernel shades, and it does not move with face count.**

### 2b. The two questions the plan turns on

**Q1 — of faces surviving frustum+backface into FList, what fraction end up fully
occluded (the RNDR occlusion opportunity)?** Very high: **73–90 %.** Indoor pyramid
+ city depth = you face a wall, everything behind z-fails. *But* the fill cost of
those faces is small (z-early-reject skips their expensive work; §1), so this large
fraction converts to a small time saving — bounded by §2c.

**Q2 — of verts transformed in XFRM, what fraction belong to fully-occluded
meshes/chunks (the pre-transform opportunity, the user's ask)?** Low: **3–27 %,
typically ~8–10 %.** Occlusion is face-level *inside* visible chunks; chunks are
spatial cells that almost always keep ≥1 visible face. Absolute pre-transform
ceiling = that fraction × XFRM:

| pose | XFRM (measured) | occluded-mesh verts | **pre-transform cull ceiling** |
|---|--:|--:|--:|
| greets flags-off | 0.70 ms | 7–13 % | **~0.05–0.09 ms** |
| city t=1961 | 2.31 ms | 8.0 % | **~0.18 ms** |
| greets displaced t=5780 | 7.8 ms | 10.2 % | **~0.80 ms** |
| chase t=800 | ~0.3 ms | 27.5 % | **~0.08 ms** |

Even the fat displaced pose caps a *perfect, zero-cost* pre-transform occlusion
cull at **~0.8 ms** of an 88.8 ms frame.

### 2c. Q3 — per-mesh/chunk stats + where the 7.8 ms XFRM goes

- Greets is ~140 meshes flags-off / ~227 displaced (Piramid chunk grid + momy +
  robot + screens + chunks). The frustum cull already rejects **49–186** of them
  per frame (t=2145 rejects 186/227 — camera faces one wall).
- The 7.8 ms displaced XFRM is **958 k verts across the 112 non-culled meshes**,
  ~90 % of which are genuinely visible or in mixed chunks. It is **on-screen work**
  — the displaced wall you're looking at, not hidden geometry. Occlusion cannot
  reclaim it; **LOD / SoA-transform can.** (Side note: transform processes *all*
  verts of a non-culled mesh even where most faces are later backface-culled from
  FList — a real but sub-ms inefficiency the SoA / face-driven-transform work
  addresses, orthogonal to visibility.)

### 2d. Q4 — SORT cost vs FList length

Radix is 4 linear passes over `CAll` 16-byte `FListEntry` slots — O(CAll), so
halving faces halves sort. But sort is **not a cost worth culling for**: at the
73 k-face displaced pose, disabling it (`FDS_NO_SORT=1`) made the frame **slower**
(90.9 vs 88.8 ms) — front-to-back ordering pays for itself via Z-reject. Sort at
that face count is < 2 ms and net-positive.

### 2e. The face-front-end is already reclaimed (the ceiling that matters)

`--tile_bbox_cull` A/B (the existing B5 reject, the closest proxy for "cull faces
cheaply"):

| pose | bbox cull OFF | ON | **face-front-end reclaimed** |
|---|--:|--:|--:|
| greets displaced t=5780 | 106.4 ms | 88.8 ms | **17.6 ms** |
| city t=1961 | 102.4 ms | 99.0 ms | **3.4 ms** |

At normal poses the whole face-front-end is only ~3 ms; at the 400 k-poly displaced
pose it's ~18 ms and **B5 already captures it.** A new occlusion system would
compete for the *remainder* — in-frustum, on-tile, occluded faces that B5 doesn't
already reject — a fraction of these numbers.

---

## 3. The honest counterfactual (why the ceiling is small)

An occlusion cull removes geometry that is inside the frustum but hidden. Its
payoff is: (a) skip shading it, (b) skip filling it, (c) skip transforming it.
In THIS engine:

- **(a) shading — already gone.** Deferred shades each final pixel once.
  `zWritten` is invariant to face count. **0 ms reclaimable.**
- **(b) filling — mostly gone.** Z-early-reject means occluded pixels skip the
  texel/store. What remains is per-face clipper entry + per-pixel edge/z test for
  in-frustum on-tile occluded faces — a slice of the ~3 ms (normal) / ~18 ms
  (displaced) front-end that B5's bbox cull already takes most of. **Low
  single-digit ms reclaimable at the extreme pose; sub-ms normally.**
- **(c) transforming — 0.05–0.8 ms** (§2b), and it's not even chunk-shaped.

So the reclaimable envelope for a *from-scratch, perfect, free* occlusion system is
roughly **< 1 ms (normal scenes) to a few ms (the opt-in 400 k-poly displaced
pose)**, on frames of 48–99 ms — and a real system is neither free (the cull test
costs) nor perfect (it must be conservative or it moves pins). The frame's actual
elephants are pixel-bound and untouched by visibility: cube-shadow tap ~32 ms,
deferred kernel ~12 ms, shadow bake ~12–27 ms (a *different* camera with its own
culls). **Visibility culling is not where the frame is.**

**Trip-wire that flips this verdict:** if a future scene becomes *forward-shaded*,
*heavily depth-complex with expensive per-fragment work that Z-early-reject can't
skip*, or *geometry-front-end-bound past what B5 reclaims* (e.g. a displaced-LOD
regime pushing well past 100 k on-screen occluded faces), re-open §4. The
`--vis_stats` diagnostic (Slice 0) is exactly how to detect that: watch `overdraw`,
`faces fully occluded`, and the `no-tile_bbox_cull` delta.

---

## 4. PHASE 2 — architecture survey (verdicts against §2/§3)

Each is judged on: expected win vs the §2 numbers, cost, determinism risk (pins —
a cull that fires wrongly moves gate hashes), interaction with the offscreen passes
(shadow / mirror-RTT / env-probe have their OWN cameras — a main-view visibility
set must never cull the shadow/RTT world), and authoring burden.

| Option | Expected win here | Cost / risk | Verdict |
|---|---|---|---|
| **Cells & portals** (greets = indoor pyramid; chunks exist; author via LWS/editor) | Targets exactly the occluded 73–90 % of faces. But reclaim is bounded by §3: shade-once + z-reject already neutralise most of it → **sub-ms to a few ms**. | High authoring (portal placement per scene); conservative PVS-per-frame or pins move; must be main-view-only vs the 4 offscreen camera passes. Dynamic fly-through camera needs fine cells. | **NO.** Classic big win is shade-overdraw; deferred already took it. |
| **PVS** (precomputed per-cell visible set, Quake lineage; fits the bake culture) | Same ceiling as portals, computed offline. | Heaviest authoring + bake; greets/chase cameras move continuously → many cells; determinism of the bake (greets env-bake nondeterminism already breaks its md5 gate). | **NO.** Same ceiling, more machinery. |
| **BSP** | Would *split* authored quad geometry → **more** faces, worsening the very face-front-end that's the only real cost. | Splitting fights authored content; classic use (sort order) already solved by radix + Z-buffer. | **NO.** Dominated by cells/portals and counterproductive here. |
| **BVH over chunks+meshes** | Would make the existing frustum cull *hierarchical* (fewer tests). But the frustum cull is already sub-ms and fires well (§2c). | Medium build; entry point for hierarchical occlusion — but occlusion's ceiling is the problem, not the test count. | **NO (low value).** The test isn't the cost. |
| **Software occlusion** (prev-frame hi-Z reproject, or low-res occluder raster of big flat walls/buildings, tested per chunk/mesh AABB *before transform*) | The only option aimed at *true* occlusion, and the user's "before transform" ask. City buildings + greets front wall are good occluders. But the pre-transform ceiling is **0.05–0.8 ms** (§2b) and the fill sliver is what B5 already reclaims. | The occluder raster itself costs (a depth pre-pass); determinism risk; "chunk culls constantly firing" already bit the mirror path. | **NO now.** Reclaim < its own cost at these poses. Revisit only via the §3 trip-wire; even then LOD beats it for displaced greets (the faces are the on-screen wall, not occluded). |
| **Hybrid per-scene** | — | — | **Moot** given the above. |
| **Finer frustum granularity** | Chunks already ARE the granularity (~98–227 cells); finer = per-face = what the clipper + B5 bbox already do. | — | **Already have it.** |

---

## 5. PHASE 3 — the plan

### Recommended architecture: **keep the current culls; add measurement, not machinery.**

The current stack (per-mesh bsphere frustum cull → greets chunk split → radix
front-to-back → B5 tile bbox pre-reject → deferred shade-once with Z-early-reject)
is, for a deferred software renderer, already the right visibility architecture.
The research does not justify a portal/PVS/BVH/occluder campaign. It justifies
**locking in the ability to keep this decision measured**, plus two orthogonal
levers (already tracked elsewhere) that address the cost the user actually feels.

### Slice 0 — permanent `--vis_stats` diagnostic (SHIP)

Promote the temporary research counters to a permanent default-off flag: opaque
faces WON vs FULLY-OCCLUDED, G-buffer overdraw (edge vs z-written), per-mesh(chunk)
occlusion + vert counts, Transform meshes-seen/-xformed. This is Slice 0 of any
future visibility work and the trip-wire detector for §3.
- **Expected win:** 0 ms (diagnostic). Keeps the verdict honest as scenes grow.
- **Gate:** default OFF; no hot-loop cost when off (the fill counters sit behind a
  cached `g_visStatsActive` bool set only during the main opaque pass). Byte-null:
  render_gate 3/3, city `37e62845` + fountain `51fff7cd` exact, wasm links.
- **Non-goal:** it does not cull anything.
- *(This research REVERTED its counters rather than ship them — see §6. Slice 0 is
  the clean re-land if/when wanted; it is deliberately deferred, not done, because
  the research task's remit was measurement, not landing engine features.)*

### Slice 1 — (CONDITIONAL, de-scoped) per-chunk hierarchical frustum+coarse-Z cull

Only if the §3 trip-wire fires. A cheap per-chunk test before transform: existing
bsphere frustum + a coarse previous-frame hi-Z reproject of the chunk AABB. Wins
the 3–27 % occluded-mesh verts + their faces.
- **Expected win FROM §2:** ≤ 0.8 ms today (displaced greets); sub-ms elsewhere.
  **Below the noise floor of most poses → not worth building now.**
- **Gate (if ever built):** default OFF; conservative (never cull a mesh with any
  visible face — false-negatives OK, false-positives move pins); MAIN-VIEW ONLY
  (`g_offscreenViewDepth==0 && !g_inShadowPass`) so shadow/RTT/env keep their
  worlds; byte-null off; 24-run greets determinism gate (env-bake race) + city/
  fountain byte pins + render_gate 3/3 + wasm.
- **Non-goals:** no BSP splitting; no PVS bake; no portal authoring; never applied
  to an offscreen camera.

### Explicit non-goals for the whole campaign

- No cells & portals, no PVS, no BSP, no BVH. (§4.)
- No software occluder-raster pass. (§4.)
- Do **not** target the deferred lighting / cube-tap with visibility — it's
  per-final-pixel and geometry-invariant (§3).
- Do **not** try to cull the shadow-bake world from the main view.

### Where the frame time actually is (redirect, not part of this campaign)

Tracked in `docs/PERF_STATE.md` + `docs/OPTIMIZATION_BACKLOG.md`, and confirmed by
this research to be the real levers:
1. **Cube-shadow tap ~32 ms** — per-pixel, the #1 cost. `shadow_polyid_no_pcf`,
   fewer shadow omnis, better cache layout.
2. **Deferred kernel ~12 ms** + **shadow bake ~12–27 ms.**
3. For the **displaced-greets face explosion** the user is feeling: **chunk LOD**
   (backlog S5) to cut the *visible* face count, and the **SoA vertex refactor**
   to speed the transform of visible geometry — *not* occlusion (the faces are the
   on-screen wall).

---

## 6. What this research committed

**Nothing to the engine.** The `--vis_stats` instrumentation (a `FeatureFlags.def`
flag, two `Face` fields, counters in `Mekalele.h` / `Transform.cpp` /
`RENDER.CPP`, and a `VisStats_Report()`) was built, used to produce §2, and then
**reverted** — the research remit was to measure and plan, and the numbers now live
here. Slice 0 (§5) is the clean re-land recipe if a permanent diagnostic is wanted;
it carries the gate obligations listed there. This document is the deliverable.

---

## 7. ADDENDUM 2026-08-03 — the chunk-granularity challenge, tested with code

Status: **experiment complete.** The user rejected §0's pre-transform reading
with a sharp argument: the displaced-greets tessellation costs ~40 ms of pure
per-poly front-end, ~89 % of those faces are fully occluded in-frustum, and §2's
"only 3–27 % of transformed verts live in fully-occluded chunks" FROZE the chunk
granularity — grid-8 cells sized for the flat mesh hold ~406 faces each once
displacement multiplies the walls ~20×, so the ~10 % figure could be an artifact
of coarse chunks rather than a property of occlusion. Directive: *"check this
with actual code before declaring this won't work."* This section is that code,
its measurements, and the revised verdict.

### 7a. What was built (committed, all default-OFF, byte-null off)

- **Phase A (1739e95): `--greets_chunk_size=S`** — size-based near-cubic chunk
  cells (per-axis grid = ceil(span/S)) replacing the uniform N³ grid, so cell
  size is independent of face count. 0 = legacy grid, byte-identical. Chunks
  also store world/local AABBs (from the same worldVerts the cube cull uses).
  `--vis_stats` prints a per-chunk face histogram + a per-frame visibility
  census (the §5 Slice-0 diagnostic, landed for real this time).
- **Phase B (5bcd6cc): `--chunk_occlusion`** — a PREVIOUS-FRAME hi-Z occlusion
  cull BEFORE transform. Design pivot mid-experiment (user direction): instead
  of rasterising the S1 flat-proxy occluders in a current-frame prepass (built
  first; byte-neutral but pays its own raster), reuse the frame's own opaque
  depth. Engine subtlety: the tick CLEARS ZPage16 before Transform, so the
  final depth is captured at END of frame (post-Render) together with its
  camera; the next frame's Transform tests each mesh/chunk world AABB (after
  the frustum cull, before the per-vertex transform) against the min-pooled
  hi-Z (240×135 at 1080p), projected with the CAPTURED prev camera — exact for
  static geometry; rotation-revealed chunks land off-buffer and are kept;
  depth margin = `--chunk_occl_bias` + 2× the camera translation delta.
  Occluders = the REAL scene depth (displaced walls, mummies, robot) for free.
  MAIN-VIEW-ONLY (shadow / RTT / env / off-axis passes untouched); first frame
  inert; snapshot pin dumps force it inert (`--chunk_occl_snapshot_force`
  [dev] overrides for the pop rig); `--chunk_occl_verify` (FDS_OCCL_VERIFY)
  audits every culled chunk against the final current-frame depth.

### 7b. Phase A: finer granularity alone is pure overhead

Displaced Piramid, 87,256 faces / 261,768 verts (1080p, this machine):

| chunking | chunks | faces/chunk mean / max | XFRM p50 @ t=5780 |
|---|--:|--:|--:|
| grid-8 (ship) | 215 | 406 / 2,095 | ~7.9–8.5 ms |
| size=2 | 1,770 | 49 / 359 | ~8.4–8.7 ms |
| size=1 | 6,669 | 13 / 170 | ~9.2–9.6 ms (**+~13 %**) |

More chunks = more per-mesh transform setup + FList entries, and nothing
rejects them at an in-room pose. Granularity only pays through a rejection
mechanism — which is Phase B's job.

### 7c. Phase B census: the cull works — and the catch stays small at every granularity

`--vis_stats`, displaced greets, prev-frame cull ON (final build; "frustum-
surviving" = verts the existing bsphere cull already kept, ~830–960 k/frame):

| pose | chunking | in-frustum chunks tested | occl-culled | verts culled | % of frustum-surviving |
|---|---|--:|--:|--:|--:|
| t=5780 (into room) | grid-8 | 109 | 17 | 25,944 | 2.8 % |
| t=5780 | size=2 | 764 | 339 | 63,183 | **6.7 %** |
| t=5780 | size=1 | 2,847 | 1,588 | 72,198 | **7.7 %** |
| t=2145 (faces wall) | size=2 | 173 | 9 | 2,403 | **0.3 %** |
| t=6097 (corridor) | size=2 | 428 | 250 | 41,688 | 5.0 % |

This is the decisive number. §2 measured ~10 % occluded-mesh verts at coarse
chunks; the challenge predicted fine chunks would blow that open. Measured:
**8× more chunks than ship moves the occludable fraction to 6.7 %; 31× more
moves it to 7.7 %.** Granularity was NOT the bottleneck. At t=2145 the reason
is structural: the camera faces a wall, so the frustum cull already rejects
1,606 of 1,782 chunks — the hidden geometry is OUT-of-frustum, not in-frustum-
occluded. And at t=5780 the ~93 % of transformed verts that remain are the
displaced wall the camera is LOOKING AT. Occlusion cannot reclaim on-screen
work; only LOD / a faster transform can.

### 7d. Cost/benefit + the prev-frame trade, measured

Timing (40-iter p50, interleaved OFF/ON reps; the box was shared with another
session's renders for part of the run — contaminated pairs [frame p50 > 120 ms
or an obviously inflated section] discarded; XFRM/SORT deltas were consistent
across every clean pair, frame-level deltas are noise-limited):

| pose | chunking | OFF frame / XFRM / SORT / RNDR | ON frame / XFRM / SORT / RNDR | Δframe |
|---|---|---|---|--:|
| t=5780 | grid-8 | 94.5 / 8.06 / 0.61 / 57.3 | 95.2 / 7.76 / 0.56 / 58.0 | +0.7 |
| t=5780 | size=2 (r1) | 100.2 / 8.71 / 0.65 / 60.9 | 90.2 / 8.06 / 0.49 / 53.8 | −10.0 |
| t=5780 | size=2 (r2) | 96.3 / 8.53 / 0.61 / 58.4 | 96.7 / 7.96 / 0.49 / 58.0 | +0.4 |
| t=5780 | size=1 (r1) | 101.9 / 9.33 / 0.63 / 57.4 | 100.9 / 8.87 / 0.48 / 56.4 | −1.1 |
| t=5780 | size=1 (r3) | 94.3 / 8.66 / 0.59 / 52.7 | 91.9 / 8.37 / 0.45 / 50.8 | −2.5 |
| t=2145 | size=2 | 101.5 / 6.74 / 0.42 / 66.1 | 102.6 / 6.51 / 0.41 / 67.4 | **+1.1** |
| t=6097 | size=2 (r1) | 79.1 / 6.80 / 0.14 / 43.4 | 77.8 / 6.22 / 0.07 / 43.0 | −1.3 |
| t=6097 | size=2 (r2) | 71.0 / 6.13 / 0.13 / 38.0 | 72.3 / 5.85 / 0.06 / 39.2 | +1.3 |

Consistent signal: **XFRM −0.2…−0.65 ms, SORT −0.05…−0.16 ms** (fewer FList
entries; t=6097 halves SORT). Against that, the hi-Z min-pool costs **~0.8 ms**
per frame (inside RNDR at EndFrame). Net frame delta: within measurement noise
at the poses where the cull catches, and a ~+1 ms LOSS at wall-facing t=2145
where it catches nothing. There is no pose where the cull buys a resolvable
frame-level win.

Temporal-pop audit (the prev-frame design's honest cost):
- Fixed camera, 60 frames: **0 violations** (the static-world prev-frame test
  is exact).
- Real-frame-delta camera sweep (9 ticks/frame, t=2000..6100): **8.1
  violations/frame** (audit upper bound — the rect test is loose at
  silhouettes). Bias 0.5→4.0 barely moves it (661→595 over a 111-frame
  window): structural disocclusion, not margin. Violations occur even at
  <0.05 units of camera delta (dynamic occluders + audit looseness).
- **Ground truth** (2-timestamp snapshot rig at the two worst audit poses,
  N=4 OFF/ON, systematic-byte metric that excludes the known ~1-in-12 kernel
  flip): 2918→2927 = 1,545 systematic bytes (~515 px, **0.025 %** of the
  frame); 2972→2981 = 2,454 (~818 px, **0.039 %**). The pops are REAL,
  single-frame, small, concentrated at fast-camera segments.

Gates: flags-off is byte-null — city `37e62845` exact, fountain `51fff7cd`
exact, render_gate 3/3, wasm links; snapshot pins cannot move (harness-forced
inert). Scope: the cull is wired into the greets tick; city/chase never call
it (the mechanism is scene-agnostic — prev-frame ZPage16 exists everywhere —
but city's two-deferred-passes-per-frame structure needs its own
which-pass-depth audit before wiring, deliberately not attempted here).

### 7e. Revised verdict

**§0's ceiling is CONFIRMED — now with code instead of extrapolation — with
one honest correction in the user's favor.**

- The user was right that §2's occluded-vert figure was granularity-bound at
  the top end, and right to demand code: ship grid-8 chunks catch only 2.8 %
  where size-2 chunks catch 6.7 % at the same pose. Coarse chunks DID
  understate the catch.
- But the ceiling saturates immediately: 31× more chunks buys 6.7 %→7.7 %,
  squarely inside §2's 3–27 % envelope, because the transformed verts are the
  looked-at wall. "A lot of perf headroom" is REFUTED by measurement: gross
  reclaim (XFRM+SORT ≈ 0.4–0.8 ms) ≈ the hi-Z pool cost (~0.8 ms) at the best
  poses, a net LOSS at wall-facing poses, plus a real (small) temporal pop and
  +13 % XFRM overhead if fine chunking is left on without the cull.
- The ~40 ms displaced front-end the user is feeling remains real — and
  remains pointed at **chunk LOD (backlog S5)** and the **SoA transform**,
  exactly as §5 concluded. B5 already banked the face-front-end; the deferred
  shade-once + Z-early-reject already banked the pixels.

What survives (kept in-tree, default-OFF): `--greets_chunk_size` (the knob
chunk-LOD will want anyway), `--chunk_occlusion` + `--chunk_occl_verify` +
`--vis_stats` (§5's Slice 0/1 machinery, now real code with a free occluder
source and a working audit), and this measured record. If a future scene is
genuinely geometry-front-end-bound with true in-frustum occlusion (deep
portals, street-level city canyons), the §3 trip-wire now has a working
prototype to light up instead of a research doc.

---

## 8. ADDENDUM 2026-08-05 — the MIRROR passes, measured per pass

Status: **measurement complete, nothing culled, no default changed.** Prompted
by a directive to "make frustum culling actually work for the mirror RTT
passes" on the premise that a mirror clone's room-sized bsphere defeats the
off-axis cull at `Transform.cpp:1186`. Two new default-OFF instruments landed
for it: **`--xfrm_pass_prof=N`** (per-PASS front-end census — `--xfrm_prof` is
main-view-only by construction) and **`--mirror_cull_census=N`** +
**`--mirror_cull_census_cell=S`** (the clone-split CEILING, computed by
re-running the *same* sphere test on the sub-spheres a split would produce).

**They are COMPILE-TIME gated (`cmake -DFDS_VIS_CENSUS=ON`), not merely
flag-gated, and that is a measured finding in its own right.** The first cut
guarded every added statement with `if (flag)` and left the code in
`Transform_Objects`. That is NOT byte-null in this build: `-O3 -flto
-ffp-contract=fast` means carrying the never-taken branches changes which
expressions the compiler contracts into FMAs in the *surrounding* vertex/face
work. Isolated worktree, HEAD vs HEAD+patch, cold-cache city @ t=1961, each
arm stable 2–3/3: **b2af24de → 850be968, 216 differing bytes of 6.2 M
(0.0035 %), max |Δ| 44** — i.e. ~72 pixels landing on the other side of a
raster/z boundary. Reverting only `Transform.cpp` restored the pin exactly, so
it is that TU's codegen and not the `FeatureFlags.def` insertion or the
`GreetsMirror` side. Moving the census body behind a `noinline` call boundary
did not fix it (a third hash). `#if FDS_VIS_CENSUS` does: with it off the
preprocessor removes every line, `Transform_Objects` is textually the
un-instrumented function, and byte-nullity is guaranteed by construction —
verified **census-OFF city = b2af24de, exact**, render_gate 3/3, and
census-ON mirrortest byte-identical to its baseline (so the numbers below were
taken without perturbing the render).

*General lesson for this tree: "the flag is off so it is byte-null" is not a
sound argument for anything added inside a hot `-ffp-contract=fast` function.
Either prove it with a controlled A/B or gate it at compile time.*

*Gate hygiene note found on the way: the city pin depends on whether
`Runtime/cache/city_envmap_cube.bin` exists — the same binary hashes
`2dd5e5dd` warm and `850be968` cold. Any city A/B must fix the cache state on
both arms (the numbers above are all cold-cache).

### 8a. Where the geometry front-end actually goes (`--xfrm_pass_prof`)

greets, 1920×1080, `--deferred --greets_displace`, 8 poses (7 review poses +
the t=5780 bench). Counts are exact and load-independent; ms are per frame,
box shared with another agent's renders (load 10–18), so read them as
"which pass dominates", not as clean absolutes. SHADOW/OFFSCREEN ms are
summed across workers (core-ms); MAIN is serial frame-ms.

| pass | calls/frame | meshes xformed/seen | verts xformed/seen | ms |
|---|--:|--:|--:|--:|
| MAIN | 1 | 43–112 / 229 | 852 k–958 k / 1 069 k | 4.2–6.8 |
| **MIRROR-RTT** | **0.00** | — | — | **0** |
| SHADOW | 33–36 | ~1 000 / 7 400–8 240 | 7.55–7.73 M / 17.5–19.4 M | 340–790 core-ms |
| OFFSCREEN (env/SH probes) | 5.4 | ~362 / 1 238 | 2.70 M / 6.73 M | 13–60 core-ms |

**Three structural facts the premise missed:**

1. **There is no mirror RTT pass.** `--mirror_rtt` defaults to 0, so the
   second-order RTT bake never runs in the shipping config — measured 0 calls
   at every one of the 8 poses.
2. **Even when it does run, the RTT HIDES every clone** (`UpdateAllMirrors`'
   RTT scope clears `HTrack_Visible` on each `m.cloneMesh`, GreetsMirror.cpp
   ~2670): a reflection of a reflection is what the RTT itself provides. So a
   clone is never in an off-axis pass, and the off-axis per-plane test at
   `Transform.cpp:1186` only ever sees the REAL scene — whose Piramid is
   already chunked. That cull works.
3. **The clone's cost is MAIN-VIEW.** Exactly ONE clone is active at the wall
   poses; at 534 356 verts it is **50 % of what the main view sees and 56 % of
   what it transforms**. (At t=2845 no mirror is potentially visible, the clone
   is `HTrack_Visible`-cleared and costs nothing — that gate already works.)
4. The passes/frame count is **~40**, not ~16: 1 main + ~34 shadow (7 omnis ×
   6 cube faces + bakes) + ~5 env/SH probe faces + 0 RTT.

### 8b. The clone-split CEILING (`--mirror_cull_census`)

The census runs the mesh cull's own sphere test per sub-sphere, plus a second
arm against the **mirror WINDOW** — the screen rect of the mirror's wall faces.
The window matters because a clone paints only where `gb.mirrorId` equals its
own `mirrorMaskTag`, so clone geometry projecting outside that rect is dead
weight even when it is inside the camera frustum. Measured window sizes:
**0.04–3.9 % of the screen.**

**Arm A — one sub-sphere per SOURCE MESH** (the cheapest possible split: emit
one clone TriMesh per source mesh; 228 spheres):

| pose | window %screen | frustum-cullable | window-cullable |
|---|--:|--:|--:|
| t=5743 | 0.80 | 0.0 % | 30.3 % |
| t=5780 | 0.80 | 8.2 % | 31.5 % |
| t=5963 | 0.17 | 40.6 % | 41.3 % |
| t=6133 | 0.06 | 33.1 % | 42.2 % |
| t=6293 | 0.04 | 29.8 % | 41.8 % |
| t=6097 | — | 36.7 % | (window rect unusable: a wall vert behind near) |
| t=1588 | 3.89 | 2.7 % | 23.3 % |

It **saturates at ~40 %**, and finer SOURCE chunking does not move it
(`--greets_chunk_size` 0→1 at t=5743: 228→6 955 spheres, 30.3 %→41.4 %). The
reason is structural: only the Piramid is chunked at source, so the statues /
ceiling / robot arrive as ~11 room-sized ranges holding **51 %** of the clone.

**Arm B — SPATIAL cells over the WHOLE clone** (`--mirror_cull_census_cell=S`,
the granularity a real spatial split would have), frustum / window:

| cell | sub-spheres | t=5743 | t=5780 | t=6133 |
|---|--:|--:|--:|--:|
| per-source-mesh | 228 | 0.0 / 30.3 | 8.2 / 31.5 | 33.1 / 42.2 |
| 8 | **103** | 1.7 / **62.9** | 17.4 / **62.9** | 68.3 / **89.3** |
| 4 | 425 | 5.2 / 72.4 | 18.8 / 72.4 | 74.5 / 94.7 |
| 2 | 1 735 | 7.3 / 79.4 | 20.3 / 79.2 | 77.4 / 98.2 |
| 1 | 5 174 | 8.4 / 82.0 | 20.7 / 82.3 | 79.0 / 99.3 |

**The ceiling is 63 % at the wall poses and 89 % at a mirror-panel pose, at
cell=8 — which is 103 sub-meshes, FEWER than the 228 a per-source split would
make.** Two corrections to the premise fall out: the split must be SPATIAL
over the clone (per-source-mesh caps at 40 %), and the cull that pays is the
**mirror window**, not the frustum (frustum alone: 2–20 % at wall poses).

Estimated win at cell=8, from the measured per-vert / per-face rates at t=5743
(VERT 4.363 ms / 955 051 verts = 4.57 ns; FACE 1.473 ms / 143 931 tested =
10.2 ns) — ESTIMATE, not an A/B of an implemented cull: 63 % × 534 356 verts =
337 k verts ≈ **1.5 ms**, plus the matching face-loop share ≈ **0.6 ms**, i.e.
**~2 ms off a 5.9 ms main-view `Transform_Objects` (−35 %)**, ~2.5 % of an
~82 ms frame, for a sweep cost of 103 × 60 ns = 6 µs. Sort + raster savings on
top are unquantified (opaque clone faces are currently rasterised over their
full projection and rejected per pixel; the TRANSPARENT path already bounds
clone batches by the window, RENDER.CPP ~940, the opaque path does not).

### 8c. Acceleration structure (BVH / octree) — measured verdict: NO

Per-frame mesh-level tests across all ~40 passes: **~9 150**, of which
**~7 690 are rejections**. Cost of one rejected sweep, measured as the slope of
the `--xfrm_prof` OTHER bucket against rejected-mesh count at t=5743
(`--greets_chunk_size` 0/4/2/1 → 120/244/1 108/4 368 rejects, OTHER
0.024/0.060/0.085/0.284 ms): **~55–61 ns**.

So a *perfect* hierarchy that reduced the O(n) sweep to O(1) would reclaim
**7 690 × 58 ns ≈ 0.45 core-ms/frame**, and ~90 % of that sits inside 12-way
threaded shadow passes → well under 0.1 ms of wall-clock frame time. Against
an ~82 ms frame that is not worth a BVH.

And a hierarchy does **not** reduce transformed verts — with mesh-sized leaves
it gives the same conservative answer the per-mesh spheres already give. The
only lever that reduces survivors is finer leaves, and finer leaves measured a
net LOSS in the main view (t=5743, `--greets_chunk_size` 0→1: verts
955 051→923 221, −3.3 %, while `Transform_Objects` went 5.875→6.954 ms,
+1.08) — §7b's result, re-confirmed with the clone in frame.

**The one place the "hierarchy" argument does pay is the clone, and there it
is not a hierarchy**: 103 flat spatial cells already deliver 63–89 %, so the
sweep stays trivially small and no tree is needed.

### 8d. Cheaper alternative found while measuring (a look call, not a perf call)

**49 % of the clone (261 768 of 534 356 verts) is the DISPLACED Piramid**, and
it is being cloned at full tessellation to be seen through a window covering
**0.04–0.8 % of the screen**. S1 already built a flat stand-in of exactly that
geometry for offscreen consumers (`stone_shadow_proxy`, ~226 faces vs 87 256,
`--greets_shadow_proxy`). Cloning the proxy instead of the displaced chunks
removes 49 % of the clone unconditionally, at every pose, from a small change
to `BuildMirror`'s source-mesh selection — versus a multi-day clone-split
refactor for 63 %. It costs block-level relief in mirror reflections at a
sub-1 % screen footprint: **a look call for the user, not a measurement.**

### 8e. Why the split was NOT built here

The ceiling justifies it, but the implementation is not the Piramid pattern
copied over. A SPATIAL split breaks the invariant `UpdateMirror` relies on —
`ClonedMeshRange{sourceMesh, vStart, vCount}` assumes each source mesh owns a
CONTIGUOUS clone vertex range, and a spatial cell's verts are contiguous in
neither the clone nor the source. It needs a per-clone-vertex source index
(~2 MB/mirror) plus per-chunk `cloneFaceSrc`, and every `m.cloneMesh`
consumer becomes a loop: the RTT hide scope, `MirrorShatter`,
`DisplaceRebuild`, `MaterialEditor`, `EnvBake`, `MainLoop`. Plus the
`Tri_Possessed` / per-chunk `Obj->Pos`+`Rot` allocation hazards GREETS.CPP
documents. That is a real refactor with a pixel-risk surface across the mirror
system, so it is specced here and left for a green-light rather than landed
against a 2 ms estimate.
