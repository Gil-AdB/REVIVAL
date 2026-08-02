# Visibility culling — research + campaign plan

Status: **research complete, 2026-08-02.** Prompted by the user direction: *"it's
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
