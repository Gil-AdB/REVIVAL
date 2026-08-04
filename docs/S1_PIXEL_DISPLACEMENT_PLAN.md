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
> **✅ FLOOR VOID CLOSED + CORNER BAND ADJUDICATED (2026-08-04, session 3).**
> See "S1b follow-ups" below. Void at t=5780: **6 175 → 404 px**; the t=6097
> corner band is measured to be the discard CORRECTING the lid's
> over-coverage, not eating wall.
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

### S1b follow-ups (session 3, 2026-08-04)

**New instrument — the discard CLASSIFIER.** `FDS_SNAPSHOT_GBUFDUMP=1` (greets
snapshot, env-gated like `FDS_SNAPSHOT_ZDUMP`) writes the raw G-buffer material
plane (`greets_t%06d_mat.u32`, `mip:4|matID:8|swizUV:20`) plus a
`[GBUFDUMP] id=N name=…` table on stderr. With the z16 dump that answers, for
every pixel a discard killed, WHICH surface won it instead. The discard set is
`--pom_shell` vs `--pom_shell --no-pom_shell_domain` at the SAME pose — the only
pair that isolates the discard from the lid geometry and the depth write.
**Trap it exposed:** `rooms::mirUV` / `floor::mirUV` are NOT mirror-world
clones, they are `GreetsFixBitangentHandedness`'s UV-handedness clones
(`*c = *M`), so they inherit `PomShellUvAmp` + `PomShellDomains` and their faces
keep `PomShellGroup` — they are shell faces too and any analysis that filters on
the base names alone misses most of the stone.

**(1) Floor void — CLOSED by a MULTI-BOX domain, not by merging boxes.**
`--pom_shell_merge_uv` (default **0.05 UV**, 0 = old behaviour). Diagnosis
confirmed by the classifier: at t=5780 the void was 92 % `floor::mirUV` in one
horizontal band at the far doorway — exactly a doorway threshold, where the
floor's 6 edge-adjacency patches are cut apart although the stone continues.
First attempt merged such patches into one UNION box; measured: void
6 175 → 384 px **but it destroyed the t=6097 corner silhouette** (the union box
also swallows the genuine openings between coplanar patches, so the discard
stopped firing there entirely — wall mask went from `shell` to exactly
`shellnd`). The landed design instead keeps each patch's own tight box and gives
it a SIBLING LIST (`Material::PomShellSibBoxes/SibOfs`, CSR, capped at
`kPomShellMaxSibs = 12`): patches on the same plane whose UV rects abut within
`merge_uv`, transitively. The domain test is "inside my box OR any sibling's" =
the **union of the boxes, never their bounding box**, so an authoring cut stops
discarding while a real opening still does. Kernel cost: one extra compare group
per sibling, evaluated only for lanes that failed their own box
(`horizontal_and` early-out). **Bake trap:** the plane test must use the
AUTHORED plane — `PomShell_Build` re-planes every face onto the LID, and the
lid's plane constant varies with per-face UV density (greets' floor: one
authored plane becomes six lid planes 0.0087 apart, silently failing a 1e-3
coplanarity test). `refPlane[]` snapshots it before the move.

| t=5780 (cone, cap 2) | void px | wall-mask xor vs tess | over | under |
|---|---|---|---|---|
| flat POM | 0 | 5 239 | 5 238 | 1 |
| shell, single box | 6 175 | 31 657 | 19 335 | 12 322 |
| shell, UNION box (rejected) | 384 | 35 956 | 19 337 | 16 619 |
| **shell, MULTI-box (landed)** | **404** | **31 942** | 19 335 | 12 607 |

At t=6097 the multi-box result is pixel-identical to the single-box one (wall
xor 14 041 both) — the corner silhouette is fully preserved. Grouping is driven
by patches whose boxes already OVERLAP, so `merge_uv` 0.02 and 0.20 give the
SAME grouping (rooms 67→55 plane groups, floor 6→1) and the same pixels: the
default is not knife-edge. `--pom_shell_patch_dump` prints the patch table.

**(2) t=6097 corner band — VERDICT: the discard is CORRECTING the lid, not
eating wall.** Rendered tessellation / flat POM / shell / shell-no-discard at
IDENTICAL framing (one `FDS_GREETS_CAM`, one script) and classified every
discarded pixel:

- discard-affected 178 802 px; **VOID 0 (0.0 %)**, drawn-FARTHER 178 802
  (100 %), drawn-NEARER 0. Winners: `rooms` 155 183 px, `floor::mirUV` 23 619.
  Reveal depth median 1 971 zEnc codes ≈ 5.0 world units — a real surface at a
  plausible depth, never background.
- The reference framing settles it: tess and flat POM have **identical** wall
  masks here (xor 0). `shellnd` over-covers by **35 436 px** (that is the LID
  inflating the silhouette at extreme grazing); `shell` over-covers by
  **12 162 px** and under-covers 1 879. So the discard removes ~23 k px of
  lid over-coverage and lands 2.6× closer to the reference than the
  un-discarded lid. It is a correction, not damage.
- Why the earlier eyeball was undecidable: the two crops being compared
  (`x_shellnd_s.png` vs `x_shellcone_s.png`) differed in TWO variables at once
  (naive vs cone march AND domain on/off), not one.

### S1b follow-ups (session 4, 2026-08-04) — the LID-OVERHANG defect

User report, reviewing the three-way crops: *"some pixels ran away to the left
from the edge, and are floating mid air… it seems like an actual discontinuity,
and not just a few pixels gap"* (t=6097), and at t=5780 the background wrongly
hidden behind a wall edge.

**New instrument — `--poly_viz` (default OFF), and it settles this class of
question in ONE render.** A rasterizer can only write inside the triangle it
fills, so a fragment of foreign texture appearing mid-surface is EITHER (1)
another surface's triangle genuinely covering that screen area — geometry
inflated through a neighbour — or (2) the surface's OWN triangle shading wrong
(bad UV/height march). Those need completely different fixes and the campaign
had twice tried to tell them apart from ordinary crops. `--poly_viz` replaces
every textured face's albedo with: **hue = material id** (fixed 12-entry
palette), **bright = a `--pom_shell` LID face / dark = not**, plus a
**per-triangle hash jitter** so every triangle boundary is visible. Lighting
still runs, so the shape reads. It allocates + reads the `gb.albedo` plane on
its own (it does NOT need `--texture_filter`, whose default 0 would otherwise
have made the viz a silent no-op).

**VERDICT at t=6097: CLASS (1), geometry — measured, not inferred.** The floating
tan fragments render MAGENTA (`rooms::mirUV`) sitting on a CYAN surface
(`rooms`), i.e. a different material id from the surface they sit on, so they
are the tan wall's LID triangles drawn over the blue-grey block. Confirmed
independently in the matID G-buffer dump: the fix below changes **14 379 px** at
this pose, every one of them `rooms::mirUV` before, and the tessellation
reference agrees with the new owner on **14 262 of them (99.2 %)**. Crops:
`scratchpad/D1/PV_pvshell.png` (before) vs `PV_pvbc.png` (after),
`PV_pvtess.png` (reference).

**Mechanism.** `PomShell_Build` moves the patch's verts OUT along their normals
by half the relief amplitude, so the lid is a RIGID TRANSLATION of the patch:
at a patch border it covers screen area the authored plane never covered, and
rays entering there march INWARD, land inside the patch's UV box, and shade as
stone — the lateral-exit discard cannot fire. The tessellation reference has no
such overhang because its bake PINS patch-border verts to zero displacement
(`pinnedZero`, MeshOps.cpp) — measured: flat-POM and tessellation cover the same
wall pixels to within **3 px** at t=6097.

**Fix — `--pom_shell_base_clip` (ON inside `--pom_shell`, like
`--pom_shell_domain`; inert without it, so flags-off stays byte-null).** Ask
where THIS pixel's view ray crosses the AUTHORED plane (h = 0.5). Same affine UV
chart, so `uv_base = uv_lid + (V·T, V·B)·(0.5 − hEnter)·A/(V·N)`; outside the
domain (own box OR a sibling's — the same union the lateral-exit test uses) the
flat wall does not cover this pixel, so the lane is lid overhang and is killed.
2 FMAs + one compare group per covered pixel, no per-step work.

**The ray must be the march's CAPPED one, and that is a measured correction, not
a preference.** The uncapped 1/(V·N) is the exact footprint of an INFINITE
affine patch, but at 84° incidence (measured on a t=5780 corridor wall: V·N =
0.07–0.16) the crossing lands ~1.1 world units along the surface, where that
model no longer describes the authored geometry, and the clip ate **81 px past
the flat wall's own edge**. `--pom_shell_base_clip_raw` (default OFF) keeps the
uncapped form for A/B.

**Measured** (per-surface material masks over `rooms`, `rooms::mirUV`, `floor`,
`floor::mirUV` — merging them hides the biggest over-coverage there is, wall lid
over the NEIGHBOURING wall). "wrong-surface" = px where the arm paints a shelled
surface that is not what tessellation shows there:

| arm | t=6097 | t=5780 |
|---|---|---|
| flat POM (= the authored footprint) | 3 | 7 537 |
| lid only (no discard, no clip) | 132 871 | 83 729 |
| shell BEFORE (discard only) | 38 814 | 54 625 |
| **shell AFTER (+ base clip, capped)** | **24 990** | **45 699** |
| shell + base clip, UNCAPPED (rejected) | 24 990 | 72 569 |

With the lateral-exit discard OFF — i.e. the pure geometry error — the clip
takes t=6097 from **132 871 → 8 801 px**. The remaining ~16 k at that pose is
the lateral-exit discard's own see-through, which is the stage's purpose and was
adjudicated in session 3. Crops (identical framing):
`scratchpad/D1/L6097_{tess,before,after}.png`,
`scratchpad/D1/R5780_{tess,before,after}.png`.

**What is NOT fixed (honest residual).** The base clip removes overhang OUTSIDE
the authored footprint. It cannot remove interpenetration INSIDE it: at a corner
the lid of wall A is A/2 proud of A's own plane and still pokes into wall B's
space, and Z resolves that per pixel. Measured residual at t=6097 with the
discard off: **8 801 px** vs flat POM's 3. Removing that needs the plan's §S1b
step 3 proper — a GEOMETRIC border taper (subdivide the shell faces, then pin
the patch-border ring by POSITION, since `--pom_shell_pin`'s pointer-based
`nonTarget` test never fires on a mesh split by `MakeFacesIndependentByAngle`,
and un-subdivided quads have nothing but border verts). Not attempted here.

## Stage S1e — height-field shading normal  [IMPLEMENTED 2026-08-04]

> **✅ `--pom_normal`, default OFF** (+ `--pom_normal_strength`, 1.0).

User report on the same three-way set: at t=5780 the TESSELLATED wall's grooves
sit in visibly deeper shadow than the shell's.

**Discriminator, cheapest first, and it settled it in one pair of renders:
`--no-nmap` on both arms.** With the normal map removed, tessellation STILL
shows fully shaped, bevelled blocks with dark mortar joints
(`scratchpad/D1/G_tessNN.png`) while the per-pixel path shows an essentially
FLAT wall (`G_shellNN.png`). So the depth difference is not the normal map and
not AO — it is that **the POM march only moves UVs; nothing in it ever tilts a
shading normal**, whereas the tessellation bake puts the height map's low band
into the geometry, so its vertex normals tilt with the blocks. The material's
normal map cannot close the gap: it carries the fine grain, not the block-scale
relief, which lives in the HEIGHT map.

Hypotheses that were checked and are NOT the cause:
- **(a) `--pom_depth_write` off in the shell arm** — refuted in code:
  `pomDepthWrite = (flag || pomShellFace) && marchArmed`, so `--pom_shell` arms
  it implicitly. Moot anyway: **neither arm ran SSAO/GTAO** (`--ssao` defaults
  OFF and no arm passes it), so no Z consumer could have produced the shadow.
- **(d) unequal AO/shadow flags between arms** — `--ao_map` ON (default) and
  `--ao_direct` OFF (default) in BOTH arms; `--pom_horizon` OFF in both of the
  arms being compared. The arms differ only in `--greets_displace` vs
  `--pom_shell --parallax_pom_cone`.
- **(c) height domain** is real but secondary and it cuts the OTHER way: under
  `--greets_displace` only the HEIGHT map is swapped to the residual, the NORMAL
  map stays FULL — so the tessellation path DOUBLE-COUNTS the low band
  (geometry + full normal map) and reads slightly deeper than physically
  correct. `--pom_normal_strength` exists to match it by eye if wanted.

**Implementation** (Mekalele.h, at the marched hit, after the offset clamp):
central differences of the height map over ±1 texel of its own mip; the
per-face world-per-UV cancels exactly, so `dH/dU_world = Δh · A_uv ·
heightUScale` with `A_uv` the SAME amplitude the march travelled with (the shell
geometry's under `--pom_shell`, `parallax_strength × ParallaxScale` otherwise) —
the normal therefore cannot disagree with the parallax or the depth write.
`N' = normalize(N − sU·T − sV·B)` in the view-space TBN the march already built,
written to the G-buffer normal plane; the kernel applies the material's normal
map ON TOP, in the rotated frame — the same layering the tessellation path
already has. `--pom_horizon` consequently takes its azimuth frame from the
bumped normal, exactly as it already does on the tessellation path.

**Measured, greets t=5780:** 987 740 px change, 199 505 by >12/255, 48 720 by
>32/255. Perf, `--bench=scene` iters=40, 3 interleaved pairs, load 1.7–3.2:
off 58.89 / 58.04 / 58.73, on 59.09 / 60.67 / 59.12 → per-pair deltas
**+0.21 / +2.62 / +0.39 ms, median +0.4 ms** for 4 extra height gathers per
covered pixel. Honest caveat: one pair is 6× the other two; treat the cost as
"a few tenths to ~2.5 ms at this pose", not as +0.4.

Crops (identical framing, t=5780 wall):
`scratchpad/D1/G_{tess,tessNN,shell,shellNN,shellPN,shellPNnn}.png`.
NOTE: whole-image luminance statistics do NOT separate these arms (lumSD 19.4–22.0
across all six) because the albedo texture dominates them — the judgement here is
the eye's, and the `--no-nmap` pair is what makes it unambiguous.

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

## Stage S1c — horizon-map self-shadow  [IMPLEMENTED 2026-08-04]

> **✅ LANDED — `--pom_horizon`, default OFF.** Flags-off byte-null (see the
> gate line below). What it does and what it cost, measured:
>
> - **Bake** `MakeHorizonMap` / `LoadOrBakeHorizonMap` (DEMO/MeshOps.cpp): per
>   texel of the 8-bit height map, the elevation of the relief's own horizon in
>   **8 azimuths**, u8 sin(horizon), **8 bytes/texel in the SAME block-tile+mip
>   layout as the source** so the albedo's swizzled index addresses it (the 8
>   azimuths of a texel are adjacent → one cache line per lookup). Disk-cached
>   under `Runtime/cache/pom_horizon_<key>.bin`, keyed on the mip-0 height
>   bytes + dims + radius + azimuth count + amplitude.
> - **The scale is UV-only, and that is a correctness point, not a shortcut.**
>   `tan(horizon) = Δh_world / Δlateral_world`, and both sides carry the face's
>   world-per-UV, which cancels:
>   `tan(horizon) = (A_uv · N) · Δh / Δtexels`, with `A_uv` the relief's UV
>   amplitude (`parallax_strength × ParallaxScale` — the SAME number
>   `PomShell_Build` builds the lid with) and `N` the texels per UV tile at that
>   mip. So one scalar per mip, and a mesh rescale can never desync the shadow
>   from the relief.
> - Bake cost **99 ms (rooms) / 128 ms (floor)** for 1024² × 9 mips, threaded —
>   NOT the minutes the plan feared, because the per-azimuth march is dense for
>   8 texels then geometrically spaced to the radius (a distant occluder only
>   matters if it is tall, and `max(Δh/r)` is dominated by the near samples).
>   The disk cache is still there so a launch never pays even that.
> - **Runtime** (`DeferredSurfaceKernel.cpp`, scalar light loop — stone always
>   takes it, it is normal-mapped): the unit pixel→light direction goes into the
>   surface's tangent frame; `L·N_geo` is sin(elevation), `L·T` / `L·B` give the
>   azimuth, which picks two adjacent baked channels + a blend weight, and the
>   light is faded out below the interpolated horizon
>   (`smoothstep`, `--pom_horizon_soft`, `--pom_horizon_strength`). The frame is
>   built from the **geometric** normal (the bake's azimuths live on the
>   authored UV axes; re-basing on the bumped normal would rotate the lookup by
>   the bump) with `Mat->TbnHandedness` on B, which is what keeps the mirrored-UV
>   clones' shadows from running backwards. **No atan2**: for 8 even azimuths
>   the octant is two sign tests and one magnitude compare, and the in-octant
>   weight is the ratio of the smaller to the larger component (≤ ~4 % of an
>   octant off — invisible under a deliberately soft edge, one divide instead of
>   a transcendental).
> - **It serves BOTH paths.** It is a shading term, and the bake runs at
>   height-LOAD time, i.e. on the FULL height field, before `--greets_displace`
>   swaps the POM input for the residual. `--greets_displace --pom_horizon` is
>   a valid configuration and is in the comparison table.
>
> **Measured — the acceptance test.** `--pom_horizon_viz` renders the term
> alone (neutral albedo, no specular, N·L-weighted over the lights that reach
> the pixel). At the p6097 wall, t=6097 vs t=6217 (same camera, lights moved):
> the shadow goes from a thin line on the mortar joint to a **wide band
> spreading up from the joint** — 757 214 px differ by >12/255 between the two.
> **The groove shadow moves with the light**, which is exactly what neither the
> tessellation bake nor the shell march can do (PolyId identity-only shadows).
> Crops: `scratchpad/viz2_6097_s.png` vs `viz2_6217_s.png`.
>
> Bake sanity (mip 0 sin(horizon) bytes): rooms mean 23.9, 12.3 % of texels
> > 0.25, 6.5 % > 0.5 — the grooves, and only the grooves, as designed (a block
> top has nothing above it, so its horizon is 0). floor mean 75.6, 47.8 % > 0.25
> — its relief is 2.5× deeper in UV terms, so the floor is where the term reads
> strongest. On final colour the term moves 186 653 px at t=5780 (61 574 by
> >12/255) and the frame mean luminance by −0.4/255: a groove-local effect, not
> a global darkening.
>
> **Perf [M]**: see the three-way table at the end of this file.
>
> Original sketch below, kept for the record.



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

## Three-way comparison — the numbers to judge by  [M, 2026-08-04]

Bench recipe, stated so it can be attacked: `./DEMO --bench=scene@scene=greets,
t=5780,iters=40 --profiler=0 <flags>`, headless (`SDL_VIDEODRIVER=dummy
SDL_AUDIODRIVER=dummy`) from `Runtime/`, 1080p, macOS arm64 Release. A and B
INTERLEAVED in the same script, 4 pairs, 1-minute load average recorded per
pair (2.27 → 4.88 — the machine was not idle; that is the honest condition, and
the pair structure is what makes it survivable). Run-to-run spread within an arm
was 1.7 ms (shell) and 2.0 ms (shell+horizon); the references are single runs at
load 4.60, so treat them as ±2 ms, not as three-digit precision.

| configuration | mean ms/iter @ t=5780 |
|---|---|
| flat POM (cone-8), no shell, no horizon | 56.9 |
| **S1: shell (cone-8, cap 2) + depth write** | **58.1** (57.40 / 58.12 / 57.70 / 59.14) |
| **S1 + horizon** | **61.0** (60.14 / 61.08 / 62.10 / 60.48) |
| **tessellation (`--greets_displace`)** | **104.7** |
| tessellation + horizon | 106.8 |

Per-pair horizon deltas on the shell path: **+2.74 / +2.96 / +4.41 / +1.35 ms**
→ median **+2.9 ms** for all 7 omnis, no per-light budget needed (the plan's
"K strongest lights" fallback was not required). On the tessellation path the
same term costs +2.1 ms. The shell itself is +1.2 ms over flat POM — inside the
run-to-run spread, as in the earlier session.

**Headline: the per-pixel path renders this frame in 55 % of the tessellation
path's time (58.1 vs 104.7 ms), and 58 % with the relief self-shadow the
tessellation path cannot produce at all (61.0 vs 104.7).** Caveats stated
plainly: one pose, one resolution, a loaded machine, single-run references.

Crops for the eye (all `scratchpad/`, 1080p downscaled to 900 px wide):

| what | file |
|---|---|
| **THREE-WAY, one build**: tess / S1 shell / S1+horizon, t=5780 | `T_tess_5780_s.png` `T_shell_5780_s.png` `T_shellhz_5780_s.png` |
| **THREE-WAY, one build**: tess / S1 shell / S1+horizon, t=6097 | `T_tess_6097_s.png` `T_shell_6097_s.png` `T_shellhz_6097_s.png` |
| t=5780 tessellation / flat / shell(pre-fix) / shell(landed) | `t_tess_s.png` `t_flat_s.png` `t_shellm0_s.png` `m_shell5780_s.png` |
| t=5780 discard classification (green = see-through, red = void) | `base_cls_p5780_s.png` |
| t=6097 tess / flat / shell / shell-no-discard | `v_tess_6097_s.png` `v_flat_6097_s.png` `v_shell_6097_s.png` `v_shellnd_6097_s.png` |
| t=6097 discard classification | `base_cls_p6097_s.png` |
| horizon term alone, light moved (t=6097 → t=6217) | `H_shellviz_p6097_s.png` `H_shellviz_p6217_s.png` |
| horizon on/off at the grazing acne pose t=2845 | `H_shell_p2845_s.png` `H_shellhz_p2845_s.png` |

Defaults are ALL still OFF (`--pom_depth_write`, `--pom_shell`,
`--pom_horizon`); `--greets_displace` remains the shipping look. The user picks.

## Parked (do not chase)

Greets mirror bug (cones through wall + doubled text — Known Issues in
SESSION_STATE.md), cube-shadow tap cost (~32 ms elephant, separate campaign),
groove-line zigzag fix (separate in-flight agent on the tessellation path),
SoA transform Phase 2 + chunk normal cones + chunk-sort (poly-side campaign,
separate plan — see git log for the directions discussion).
