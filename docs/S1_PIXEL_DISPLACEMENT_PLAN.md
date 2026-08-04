# S1 — Per-Pixel Shell Displacement Campaign Plan

Status: ACTIVE campaign plan, written 2026-08-04 as a handoff for a long-running
implementation agent. Read this whole file before touching code. The research
basis is `docs/DISPLACEMENT_RESEARCH.md` (commit 07b72c7) — its §S1 sketch is
the design source; this file turns it into ordered, testable stages.

## Mission

Replace (or outcompete) the geometric tessellation bake for the greets stone
relief with per-pixel shell-traced displacement built on the EXISTING POM
march, at a fraction of the cost:

- Tessellation (current shipping look, default ON): measured **+54.5 ms** at
  the worst full-screen-wall pose, plus ~9 ms camera-independent shadow
  re-raster and ~3 ms XFRM it drags along. Looks good.
- Target: tessellation-comparable look — relief in Z-effects, block-edge
  silhouettes with see-through-to-background, light-responsive self-shadow —
  at **≤ 15 ms** at the same pose.

Tessellation stays the shipping default until the user approves the switch.
Success is judged by the USER'S EYES against reference crops, not by metrics
alone. Every stage lands flag-gated DEFAULT OFF; the user flips defaults.

## Engine facts you must internalize first

Read `CLAUDE.md`, `docs/GRAPHICS_PIPELINE.md`, `docs/ENGINE.md`. Key facts:

- POM already exists and is cheap: naive-8 march +1.7–2 ms threaded, converged
  relaxed-cone march (`--parallax_pom_cone`, per-material `Material::ConeMap`)
  +9.4 ms at the worst pose. Arithmetic-bound, not gather-bound. Raster
  integration: `FDS/FILLERS/Mekalele.h` (gate near :1746).
- We OWN the Z buffer: `ZPage16`, encode `0xFF80 − g_zscale*z`. A march can
  write true depth (that is stage S1a, in flight).
- Masked stores in the 8-wide kernels make per-pixel discard free (S1b).
- Deferred G-buffer packs `mip:4|matID:8|swizzledUV:20`; quarter-res shading
  (`--deferred-quarter`) reconstructs view-space from Z — marched Z makes that
  MORE correct, not less.
- Shadows are id-buffer cube maps: the PolyId test is IDENTITY-ONLY, no depth
  compare (`DeferredSurfaceKernel.cpp:712`). Walls run one id per authored
  plane (`greets_displace_shadow_planes`, parent-plane inheritance) — so
  intra-wall relief self-shadow is IMPOSSIBLE in this scheme for BOTH the
  tessellation and per-pixel paths. Do not try to get relief shadows out of
  the id cubes; that is what stage S1c (horizon maps) is for.
- Under `--greets_displace` the wall materials' POM input is swapped to a
  RESIDUAL height map (`MakeResidualHeight`, `DEMO/GREETS.CPP` ~1630,
  lowMip=2). In pure per-pixel mode (`--no-greets_displace`) materials keep
  the FULL height + cone maps. The per-pixel campaign runs full-height.
- Displacement is mean-centered: `amp*(h − meanLow)`, amp default 0.3,
  meanLow ≈ 0.549 for wall_stone3. The shell lid must sit at
  `amp*(1 − meanLow)` above the authored plane (protrusion max), the floor at
  `amp*(0 − meanLow)` below (recess max).

## Working discipline (non-negotiable; documented past failures)

- NEVER run the demo on screen. `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`
  always. Renders SEQUENTIAL, backgrounded (600 s watchdog kills long
  foreground calls). Demo runs from `Runtime/` only.
- Runtime tunables via `FDS/Base/FeatureFlags.def`, never raw getenv.
- Worktree trap: a worktree binary chdirs to the WORKTREE's Runtime; verify
  asset md5s vs the main tree before trusting renders.
- Perf: interleaved pairs, load-guarded; determinism claims need 24+-run md5
  gates. Never call ms-scale costs "tiny" — argue mechanism or measure.
- Visual defect triage order: `--displace_viz` / geometry viz FIRST, then ONE
  stage-off discriminating render, only then variant batteries.
- Checkpoint a resumable progress file in your scratchpad every major step
  (session limits interrupt long agents routinely). Commit incrementally on
  fog-wt with clear messages. Do NOT push unless the user asks.
- Gates before every merge/commit chain: `./tools/render_gate.sh` 3/3 (dummy
  drivers), city `37e62845` + fountain `51fff7cd` pins byte-exact with new
  flags OFF, wasm link (`make serve` build) when touching shared kernels.

## Stage S1a — depth-writing POM  [IN FLIGHT]

A worktree agent is implementing `--pom_depth_write` (default OFF): the march
writes its intersection depth to ZPage16 with lane-masked stores. Acceptance:
flag-off byte-identical; z-dumps show relief; SSAO gains groove contact
darkening; ordering hazard at wall/prop intersections audited; perf vs the
+2.5–3 ms estimate; 16-bit zEnc span quantified. If that agent's branch is
unmerged when you start: review, merge, re-gate. If it never completed:
re-implement per the above (its worktree branch may hold partial work).

## Stage S1b — shell lid + miss-discard silhouettes  [IMPLEMENTED, TUNING OPEN]

> **✅ LANDED 2026-08-04 (fog-wt c556148) — `--pom_shell`, default OFF.**
> Flags-off byte-null: render_gate 3/3, city `37e62845` (2 runs), fountain
> `51fff7cd`. What the implementation had to CHANGE from the sketch below,
> each for a measured reason:
>
> 1. **"Miss ⇒ discard" does not exist.** A downward ray inside a height
>    field ALWAYS crosses it (h ≥ 0 everywhere, the ray runs from above the
>    max to h=0), so a plain miss never happens in the interior and produces
>    no silhouettes. The real mechanism is **LATERAL EXIT from the patch**,
>    and the test for it is exact and nearly free: the march is a straight
>    line in UV, the patch's UV box is convex, and the march returns the
>    FIRST crossing, so "hit UV inside the box" ⟺ "crossed before exiting".
>    One test on the final UV (4 compares/lane), no per-step work.
> 2. **The domain is the CONTIGUOUS COPLANAR PATCH, not the authored quad**
>    (`Face::PomShellGroup` → `Material::PomShellDomains`, union-find over
>    edge-adjacent coplanar faces). Measured: greets wall quads are only
>    **0.4–2.1 UV tiles wide** while a grazing shell ray travels up to
>    amp·cap ≈ 0.24 UV, so per-quad domains fired the discard mid-wall —
>    82 k px of void at t=5780. Patch domains → 47 k; + cone march at
>    `--pom_shell_cap=2` → **6.2 k**.
> 3. **No clone, no base-face suppression.** `PomShell_Build` moves the
>    material's own verts out to the lid, so there is nothing to double-draw
>    and offscreen passes just see a wall amp/2 thicker.
> 4. **The ray is the TRUE view ray (÷V·N, capped)**, not the offset-limited
>    one. Offset limiting bounds lateral travel at `strength` whatever the
>    angle, and grazing lateral travel is exactly what silhouettes are made
>    of. `--pom_shell_cap` (default 8) bounds the 1/(V·N) blowup and is THE
>    tuning dial: it trades silhouette reach against march undersampling
>    (see the open item below).
> 5. **Per-face constant rays were NOT taken** (plan §curved-surfaces): the
>    entry height is a real per-pixel interpolant (`Vertex::ShellH`, gradient
>    lane 11, both clippers), so a tapered/pinned lid marches from its TRUE
>    geometric height and S1d prisms inherit the machinery.
> 6. **A cone march that ran out of steps is UNRESOLVED, not a miss.**
>    Discarding those punched holes wherever bracketing is slow (grazing,
>    where rays travel farthest) — cone was WORSE than naive (124 k vs 82 k
>    void px) until this was separated.
>
> **Measured, greets 1080p, t=5780 / t=2845 / t=6097:**
> void (z==0) opened vs flat POM — naive-8 cap 8: 46 986 / 64 / 2;
> cone cap 8: 32 876 / 0 / 0; **cone cap 2: 6 175 / — / —**.
> "Revealed FARTHER geometry" (the discard letting a back surface win the
> pixel = the see-through effect itself): 88 675 / 3 / 178 827 px (cone).
> Note the reference measurement that reframes the acceptance test:
> **tessellation itself opens 0 / 0 / 5 px of BACKGROUND at these three
> poses** (they are enclosed) — so "see the geometry behind" means revealing
> other SURFACES, not sky, and that is what the discard does.
>
> **OPEN (next session):** the remaining void at t=5780 is the FLOOR, whose
> slab is ~6× deeper than the walls' (ParallaxScale 0.25 → A ≈ 1.1 world vs
> 0.18) and which is seen at extreme grazing, so its rays travel up to 0.6 UV
> = ~9 world units and no fixed-step march resolves that. Candidates, in
> order: (a) merge coplanar patches whose UV rects abut even when not
> edge-adjacent (the floor is split into 6 patches by doorway thresholds —
> this is what the remaining bands sit on); (b) a per-material cap, or a cap
> derived from a texels-per-step budget; (c) shell the walls only. Also open:
> the p=6097 close-up shows the discard changing coverage over a LARGE band
> at a wall corner — audit whether that reads as "eaten wall" on screen.
>
> **Perf [M]**, greets t=5780, `--bench=scene`, iters=40, interleaved pairs,
> machine load 2.6–6.1 (noisy, ±4 ms run-to-run — treat as "inside the
> noise", not as a precise number):
>
> | configuration | mean ms/iter |
> |---|---|
> | flat POM naive-8 (per-pixel baseline) | 51.6 / 54.9 / 58.0 |
> | + shell (naive-8, cap 8) | 59.9 / 53.0 / 55.8 |
> | flat POM cone-8 | 61.3 / 65.5 |
> | + shell (cone-8, cap 2) | 63.0 / 63.5 |
> | **tessellation (`--greets_displace`)** | **123.9** |
>
> The shell's own cost is inside the noise floor (it adds no gathers — a few
> FMAs, one reciprocal and 4 compares per lane); cone-vs-naive is ~+8 ms.
> Against the +70 ms the tessellation bake costs at the same pose, the
> per-pixel path renders this frame in about HALF the time.
>
> Original sketch below, kept for the record.

Goal: true block-edge silhouettes. When a ray misses all stone, the pixel is
DISCARDED and the geometry behind shows through — the user named this the one
major visual win of tessellation; it is the primary acceptance criterion.

Design (research §S1b, adapt as code dictates):
1. At scene init (per-pixel mode only), clone the wall faces offset outward
   by `amp*(1 − meanLow)` along the face normal — the LID. Lid faces carry
   the wall material + a new face flag (e.g. `Face_PomShell`); the base faces
   are suppressed in the main pass (they'd double-draw; they may still serve
   offscreen passes — decide and document). Reuse the S1 proxy pattern
   (`GREETS.CPP` stone proxy) for the cloning mechanics.
2. Raster the lid; per pixel, enter the ray at lid height h=1 equivalent and
   march DOWN through `[amp*(1−meanLow) … −amp*meanLow]`. Hit ⇒ marched UV +
   marched depth (S1a machinery). Miss after exiting the slab ⇒ discard the
   lane (masked store).
3. Border pinning: at patch borders (wall edges, wall↔floor creases) taper
   the lid to the base plane over the border cells — same convention as the
   tessellation bake's border pinning — so no cracks open against neighbors.
   Alternative if taper artifacts: side skirts. Measure both if unclear.
4. Tangent-space ray setup per face from the existing TBN; walls are
   axis-aligned quads, so per-face constant ray direction in texture space is
   likely sufficient (verify against a corner pose).
5. Flag: `--pom_shell` (default OFF), requires `--pom_depth_write`.

Watch-outs:
- Front-to-back + Z-early-reject: discarded lanes must leave Z untouched so
  later (farther) geometry wins the pixel. Verify with a pose where a mortar
  gap at a wall edge should reveal the room behind (reference: same pose
  rendered with tessellation ON — crops must match in what-shows-through).
- The lid's screen footprint exceeds the base face near silhouettes — tile
  bboxes (`--tile_bbox_cull` S2 stamps) must use LID geometry, which they do
  automatically since the lid IS the rastered face. Do not hand-adjust.
- Grazing near-tangent rays cross many texels: cap march length; the cone
  map bounds steps. Check the t=2845 grazing pose for shimmer between two
  adjacent t captures.
- Props intersecting walls (momy, letters): the shell may overlap them;
  audit those poses (t=5780 has statues near walls).

Acceptance: side-by-side vs tessellation at the pose battery (below) —
silhouettes, see-through edges, no cracks at borders, no shimmer; perf at
worst pose ≤ +12 ms converged (research estimate +8–12); flags-off byte-null.

## Stage S1c — horizon-map self-shadow  [SHADOW QUALITY]

Goal: light-responsive relief self-shadow, which NEITHER path has today (see
engine facts). Works for both tessellation and per-pixel modes — a pure
shading term. This answers the user's "attack the shadow quality angle".

1. Bake (offline tool or init-time + disk cache under `Runtime/TEXTURES/`):
   per texel of wall_stone3 height (1024²), horizon angle in 8 azimuths —
   max over the height field of `atan((h(q)−h(p))/dist)` along each azimuth,
   scan radius ~1.5 block pitches. Encode sin(horizon) 8-bit; 8 channels =
   two RGBA textures or one 8-byte-per-texel blob; build mips. Cache to disk
   keyed on height-map hash + params (init-time full bake would be minutes).
2. Runtime, per light per wall pixel: tangent-space light dir → azimuth →
   lerp the two adjacent horizon channels → shadow = smoothstep over
   (sin(elev) − sin(horizon)) with a softness knob. Multiply that light's
   diffuse + specular. Gate per-material (walls/floor only), flag
   `--pom_horizon` (default OFF) + `--pom_horizon_soft=<k>`.
3. Cost control: apply to the K strongest lights per tile if the full 7-omni
   cost is heavy; measure both. Estimate before measuring; report both.
4. Interaction: `--ao_direct` becomes largely redundant for walls (AO stays
   as the ambient term via `--ao_map_strength`). Compare crops with horizon
   ON + ao_direct OFF vs today's ao_direct ON; recommend a default to the
   user, do not flip it yourself.
5. Under tessellation mode the horizon bake must use the SAME full height
   field (not residual) — the geometry carries the low band but the shadow
   term models the full relief; double-darkening risk is low (id scheme casts
   nothing intra-wall) but verify on the acne pose.

Acceptance: disco-spot sweep pose — groove shadows must MOVE with the light;
grazing acne pose stays clean; before/after crops at the battery; perf number.

## Curved surfaces — DESIGN CONSTRAINT on S1b, future stage S1d

The user wants curved-surface support eventually (alcoves, curved props).
Therefore S1b MUST be built the generalizable way from the start:

- Per-pixel texture-space entry/exit rays: the rasterizer interpolates the
  texture-space entry point (u,v,h) and exit point across the rastered shell
  face; the march runs entry→exit. Do NOT take the per-face constant-ray
  shortcut even though flat walls would allow it — flat is the degenerate
  case (entry ray constant across the face) of the same code path.
- This means one extra interpolated channel (shell height h) in the Mekalele
  interpolant setup alongside U/V, and per-vertex TBN through the fill.

S1d proper (later, Hirche '04 prism tracing — see DISPLACEMENT_RESEARCH.md):
shell geometry builder extrudes triangles along SMOOTH VERTEX normals into
prisms (top + boundary/silhouette side faces; extrusion clamped by local
curvature radius against prism self-intersection); raster the hull; the
warped-linear march is unchanged. Horizon maps (S1c) transfer as-is (local
tangent frame). Content rule: curved UV charts must be near-affine per
triangle — the t=2845 trapezoid-chart lesson (fan-fallback artifacts)
applies doubly on curved charts. Cheap middle tier for gently curved props:
per-pixel-TBN POM without a shell (relief, no silhouettes) — may suffice for
the alcoves; offer it in the comparison. Est. cost of S1d over flat S1b:
+10–30% on the same covered pixels (setup + slightly larger hull area; the
march dominates and is unchanged) — estimate, to be measured.

## Stage S3 — march quality (optional, after S1b lands)

EGSR 2024 exact relaxed-cone bake (Bán et al., impl:
github.com/Bundas102/robust-cone-map) replacing our approximate cone bake +
secant refinement in the march. Disk-cache the cone map (same key scheme as
S1c). Research estimate −2–4 ms off the converged march. Only worth doing if
S1b ships with the cone march.

## Mode matrix + flags summary

| mode | flags |
|---|---|
| tessellation (today's default) | `--greets_displace` (+ residual POM) |
| per-pixel S1 | `--no-greets_displace --pom_depth_write --pom_shell [--pom_horizon] --parallax_pom_cone` |
| hybrid eval | tessellation + `--pom_horizon` (S1c is path-agnostic) |

All new flags default OFF. The user decides default flips and the eventual
tessellation retirement (it likely stays as an editor/reference mode).

## Validation battery (use for every stage)

Poses (greets, `FDS_GREETS_CAM=...` + t): the campaign standards —
t=2845 grazing close-up `"-7.38721609,2.72471762,-50.8239441,0.817980111,-0.113630958,0.563911617"`,
t=5780 (statues/walls), t=6097 bleed/gap pose
`"18.4499683,5.16043377,-57.6482239,-0.824408829,-0.544822097,-0.153357133"`,
plus the DisplaceTest rig poses (frontal/diag45/grazing/silhouette/topdown,
`DEMO/DisplaceTest.cpp`, `--scene-displacetest`) — extend the rig with a
shell-mode case so per-pixel vs tessellation is comparable on the SAME quad.
Captures: color + `FDS_SNAPSHOT_ZDUMP` depth; convert PPM→PNG (sips) and
LOOK at them. Perf: interleaved pairs at t=2845 + t=6097. User flag list for
"real look" checks lives in git history / ask the user (includes --hdr --pbr
--ssao-gtao --deferred-quarter etc.; note bare `--parallax_pom_lod` is a
silent no-op — needs `=value`).

## Sequencing for the week

1. Absorb S1a (merge or finish). 2. S1b lid+discard — the prize; iterate
until the see-through acceptance passes. 3. S1c horizon bake+term. 4. Present
the three-way comparison (tessellation / S1 / S1+horizon) to the user with
crops + ms table; they pick defaults. 5. S3 if cone march shipped. Keep
`docs/SESSION_STATE.md` updated (resume protocol) and this file's stage
statuses current as stages land.

## Parked (do not chase)

Greets mirror bug (cones through wall + doubled text — Known Issues in
SESSION_STATE.md), cube-shadow tap cost (~32 ms elephant, separate campaign),
groove-line zigzag fix (separate in-flight agent on the tessellation path),
SoA transform Phase 2 + chunk normal cones + chunk-sort (poly-side campaign,
separate plan — see git log for the directions discussion).
