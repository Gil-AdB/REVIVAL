# S1d — Closed shell + cross-patch march continuation

Status: ACTIVE PLAN, written 2026-08-05. This is the design the campaign's
evidence converged on. Read `docs/S1_DISCREPANCY_INVENTORY.md` (evidence,
classes, converged reference, P0/P1/P2-A results) and
`docs/DISPLACEMENT_RESEARCH.md` §3.3 before touching code.

## Why this exists — the tension no knob can resolve

Two user-reported defects, measured, are the same defect:

| symptom | arm | measured |
|---|---|---|
| black gashes between wall panels, bar in the mirror | lid shell | 98,371 void px @t=5743 (tessellation: 3) |
| grazing smear, mortar dragged into ribbons | recess-only | 20,244 px @t=5958 — *the same pixels*, clamped instead of killed |

Proof they are one thing: at t=5958 recess-only with `--pom_recess_edge=0`
(clamp) has **0 void**; the identical arm with `=2` (discard) has **20,244**.
The march has no valid answer once a ray leaves its patch's UV domain; killing
gives holes, clamping gives smears. Neither is correct.

The cap is the same tension seen from the other side: `--pom_shell_cap` bounds
lateral travel. Cap 2 keeps rays inside their patch (few holes, grazing depth
wrong by up to 0.945 world). Cap 16 resolves grazing (holes double: 16,539 →
34,944 px in the coordinator's measurement, 68,122 → 97,329 in the P2-A
agent's — both directions agree, magnitudes disputed, see §10.1 of the
inventory). **You cannot have correct grazing and no seam artifacts with an
open patch.**

Third defect, user-reported, that no amount of march work can fix: recess-only
has no see-through in the stone valleys — nothing rises above the authored
plane, so a stone can never occlude the mortar beside it.

## The literature's answer (research §3.3)

Hirche '04 / shell maps '05 raster a **closed shell volume** — top *and* side
faces — and march inside it. A ray exits only where the surface genuinely ends,
i.e. at a true silhouette, and discarding there is correct: you are meant to see
what is behind. Our implementation rasters only the top lid of a box and
substitutes a UV-box test for the missing sides, so it also "exits" at INTERNAL
seams where the surface continues — and discards there.

`DISPLACEMENT_RESEARCH.md` §6 non-recommended the prism machinery because "our
flat quads reduce it to S1b". That is true for ONE quad and false for a wall
built of many small ones: greets' wall patches are 0.4–2.1 UV tiles wide while a
grazing ray travels ~0.24 UV, so rays cross patch boundaries constantly and must
be handed from one patch to the next.

## The design

Two independent pieces. Either helps alone; together they remove the class.

**A. Cross-patch march continuation (no geometry change).** When a ray leaves
patch A's domain without a hit, transform its current UV + remaining slab height
into patch B's chart and CONTINUE the march there. Requires per-patch-edge
neighbour links with a UV transform. Bounded hop count (start at 4) with a
defined terminal action. Works in BOTH lid and recess modes, and in both it
converts a hole/smear into correct relief.

**B. Side faces at true boundaries (geometry).** Where a patch edge has no
neighbour — the wall genuinely ends — extrude side quads so the shell is closed.
The silhouette becomes the shell's, protrusion is capped legally, and discard at
those faces is correct rather than a bug. This is what restores stones standing
proud with see-through valleys.

### Seam classification — the gating question

Not every patch boundary is the same:

- **Coplanar continuation** — the surface carries on in the neighbouring patch,
  same plane. A ray must continue; a hole here is always wrong. Expected to be
  the large majority (patches are split by mesh topology, not by geometry).
- **Angled junction** — a corner, another wall. The ray leaves this surface. Two
  sub-cases: it should enter the neighbouring wall's shell (continuation with a
  chart change), or it should exit to the scene (true silhouette).
- **True boundary** — the wall ends (doorway jamb, free edge). Exit is correct;
  this is where side faces belong.

**Stage 1 exists to measure this split.** If most seams are coplanar
continuation, (A) alone fixes most of the reported defects and (B) is a look
upgrade. If most are true boundaries, (B) is the priority. Do not assume.

> **ANSWERED 2026-08-05 by S1d-1 — and the expectation above is WRONG for
> greets. Read §S1d-1 at the end of this file before acting on anything in this
> section.** Measured over all 13 review poses: of the 800 513 pixels the march
> cannot answer, **0.004 % (31 px) sit at a coplanar seam**; 72.9 % sit at a
> CONVEX angled ridge, 15.1 % at a concave fold, 11.9 % at a true boundary.
> Coplanar continuation is already shipping as `--pom_shell_merge_uv`'s sibling
> boxes (worth 70 585 px, and the coplanar UV transform is the IDENTITY).
> **(B) dominates, not (A).**

## Stages, each independently gated

**S1d-1 — topology + classification (DIAGNOSTIC ONLY).** Build per-patch-edge
neighbour links and classify every patch boundary edge into the three classes
above. Report the census per material and the total edge length weighted by
screen coverage at the review poses. Add a viz that colours patch boundaries by
class. NO behaviour change. Deliverable: the census + the verdict on which of
(A)/(B) dominates.

**S1d-2 — RE-SCOPED by S1d-1's census (see §S1d-1.10).** The stage as written
below targets COPLANAR seams, which the census measures at 31 px of 800 513 and
which `--pom_shell_merge_uv` already handles. The re-scoped order is:
**(a) close the shell at convex ridges and true boundaries (84.9 % of the
population), (b) a per-boundary-class edge policy — discard at TRUE boundaries,
where the geometry behind already fills the pixel correctly and the clamp is the
bug, (c) angled continuation last.** The original text is kept for the record:

**S1d-2 (as originally written) — march continuation across coplanar seams.** Hand-off at domain exit
with the UV transform; bounded hops; terminal action defined and flagged.
Gates: void → single digits AND clamp/smear area → near zero at the review
poses, in BOTH lid and recess modes; grazing depth stays at P1's accuracy with
the cap RAISED (that is the point).

**S1d-3 — side faces + protrusion restored.** Extrude side quads at true
boundaries; re-enable the outward lid. Gates: void vs tessellation; no
interpenetration with neighbouring meshes (the C2 metric); shadow-cube and
mirror deltas measured, with the recess-only arm's **0 of 13.5M texels** as the
standard to beat or justify; see-through in the valleys demonstrated in a crop.

**S1d-4 — the honest comparison.** Tessellation vs closed-shell vs recess-only:
look crops at every review pose, void/smear/offscreen tables, and perf at
MATCHED QUALITY (error vs the converged reference of each arm's own semantics),
not at matched flags. Only then does anyone discuss defaults.

## Mandatory gates on every arm, every stage

These exist because the campaign shipped a recommendation with 143,835 px of
known error in it, and because the converged reference is blind to seam holes
(it shares the open-boundary model — inventory class C7).

1. **void (z==0) vs tessellation**, at every pose in
   `docs/greets_review_poses.txt`. Tessellation scores 0–24. Anything in the
   thousands is a failed arm, whatever else improved.
2. **Clamp/fallback area** — the pixels where the march had no answer, reported
   as a percentage of frame. Holes and smears are the same population; report it
   even when the mode makes it invisible.
3. **Offscreen deltas** — shadow-cube texels + mirror content vs the no-shell
   arm. Recess-only achieves 0/13,533,184.
4. **Error vs a converged reference of the SAME semantics.** Never across
   semantics (P0 §8 documents the rule).
5. **Look at the output images before reporting.** A metric that improves while
   the picture breaks is how this campaign reached this document.

## Discipline (documented failures)

Dummy SDL drivers always, never on screen. Renders sequential + backgrounded
(600 s watchdog). ZSH does not word-split unquoted `$FLAGS` — use arrays, and
grep run logs for `unknown flag` / `requires a value`. Never pipe a build
through `head`/`tail` (SIGPIPEs ninja → stale binary). FeatureFlags.def for all
tunables; new flags default OFF; the user flips defaults. Measured vs inferred
stated explicitly; "I don't know" beats a plausible story. Never call ms-scale
costs "tiny". Gates before each commit: render_gate 3/3, city `37e62845`,
fountain `51fff7cd`. Commit on fog-wt; do not push.

## Current standing arm (what to compare against)

```
--deferred --no-greets_displace --pom_shell --pom_recess_only \
  --parallax_pom_cone --parallax_pom=32 --pom_cone_exact=1 --pom_cone_min_step=1 \
  --pom_march_earlyout --pom_shell_cap=16 \
  --pom_shell_world_amp --pom_shell_world_amp_set=0.18 --pom_normal
```

Zero holes, correct shadows/mirrors, no protrusion, smears at grazing.
Tessellation (`--greets_displace`) remains the shipping default.

---

# S1d-1 — PATCH-BOUNDARY TOPOLOGY AND SEAM CENSUS (diagnostic, no behaviour change)

Added 2026-08-05, branch `fog-wt`. Two new flags, **both default OFF**:
`--pom_seam_census` and `--pom_seam_viz`. No pixel of any existing arm moves —
gates in §S1d-1.9. This section answers the gating question above, and the
answer **overturns this plan's own expectation**, so read §S1d-1.6 before
§S1d-1.7.

## S1d-1.0 Method, and the two traps it had to route around

Topology is built inside `PomShell_Build`, on the AUTHORED mesh, from
**position coincidence** (quantized to 1e-4 world) — never from face indices
(the Piramid chunk split re-bins them) and never from vertex pointers
(`MakeFacesIndependentByAngle` has already duplicated every crease vertex, so
two Vertex objects sit at one position with different normals). The census is
captured *before* `PomShell_Build`'s vertex move for the same reason the
amplitude census is, and it refuses to be trusted otherwise: with a non-zero lid
offset the two copies of a corner vertex move along **different** smooth normals
and coincidence is destroyed. The flag prints a WARNING if `offMax > 0`; every
number below was taken under `--pom_recess_only` (offset exactly 0).

A boundary edge of a patch is an edge no *other face of the same patch* shares.
For each one, every face in the scene sharing that edge position is examined:

| relation to this patch's plane | class | why |
|---|---|---|
| normal dot ≥ 0.999 and \|Δd\| ≤ 1e-3 | **COPLANAR CONTINUATION** | same surface, same plane — a hole here is always wrong |
| folds so the neighbour lies IN FRONT of this plane | **ANGLED-IN** (concave) | the material is a UNION across the seam: a ray leaving laterally is *still inside solid* and must enter the neighbour's shell |
| folds so the neighbour lies BEHIND this plane | **ANGLED-OUT** (convex) | the material is an INTERSECTION: the ray *leaves the solid*. Exit is the geometrically correct answer — this is a true silhouette |
| nothing continues | **TRUE BOUNDARY** | free edge; exit is correct and side faces belong here |

The concave/convex decision is exact, not heuristic: a plane hinged on the
shared edge lies entirely on one side of this patch's plane, so the sign of
`N_patch · centroid_neighbour + d_patch` **is** the fold direction.

Edges with no shared-edge partner get a geometric **outward probe** — the edge
pushed 0.02 / 0.10 / 0.35 world into the plane beyond the patch, tested against
every triangle in the scene — which is how T-junctions and non-edge-adjacent
coplanar abutments (the doorway thresholds) are found. Probe-classified edges
are **subdivided at 0.25 world** and each piece classified separately; at
1 world the t=6097 population was still being mislabelled, because its exits sit
within 0.8 world of a class transition. That correction is recorded because it
changed the verdict at one pose (see §S1d-1.8).

## S1d-1.1 The scene's topology, measured

```
'rooms'  196 faces -> 67 patches over 51 authored planes; 55 plane groups
         patch UV box: short side 0.082 / 0.422 / 3.086 (min/med/max)
                       long  side 0.103 / 1.131 / 5.246
'floor'   30 faces ->  6 patches over  1 authored plane;   1 plane group
         patch UV box: short 0.338 / 0.689 / 1.750, long 0.699 / 1.654 / 2.132
```

Grazing lateral travel at the standing arm (`uvAmp` 0.03 × `--pom_shell_cap=16`)
is **0.48 UV** on the walls. The median wall patch is 0.42 UV across its short
axis. **The median patch is narrower than one grazing ray's travel** — the
premise in "The design" above is confirmed, and it is worse than the 0.4–2.1
range quoted there (the narrowest patch is 0.082 UV).

## S1d-1.2 EDGE CENSUS — by count and by world length

Counts are of classified SEGMENTS (probe-classified edges are subdivided), so
**length is the meaningful column**.

| material | segments | total length | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY |
|---|---|---|---|---|---|---|
| `rooms` | 2 057 | 1 847.73 world | 139.48 (**7.5 %**) | 1 393.30 (**75.4 %**) | 305.07 (**16.5 %**) | 9.88 (**0.5 %**) |
| `floor` | 449 | 441.51 world | 53.81 (**12.2 %**) | 387.70 (**87.8 %**) | 0.00 (0 %) | 0.00 (0 %) |
| **both** | 2 506 | 2 289.24 world | 193.29 (**8.4 %**) | 1 781.00 (**77.8 %**) | 305.07 (**13.3 %**) | 9.88 (**0.4 %**) |

`::mirUV` clones do not appear here: `GreetsFixBitangentHandedness` runs AFTER
`PomShell_Build`, so a clone is a memberwise copy that **shares** the patch
domains, the sibling boxes and `Face::PomShellGroup`. One topology serves both,
and the screen tables below report the clones separately because they own
different pixels.

Two structural facts that fell out and matter later:

- **greets has essentially no free edges.** 9.88 world of 2 289 (0.4 %) is a
  true boundary. The architecture is closed: every wall edge meets the floor,
  the ceiling, another wall or a column.
- **The walls are NOT zero-thickness double-sided sheets.** The patch pairs that
  look coincident in the dump (e.g. `g=22` N=(1,0,0) d=12.344 and `g=23`
  N=(−1,0,0) d=12.344) are two *different* walls at x=∓12.344 — a symmetric
  room, not a sheet. Measured: **0** edges whose only partner was a reverse
  face, in either material.
- **ANGLED-IN neighbours are mostly shelled surfaces but not entirely.**
  `rooms`: 969.24 world of the 1 393.30 has a `rooms`/`floor` neighbour;
  **424.06 world (30 %) has an UNSHELLED neighbour** — the ceiling `siling`
  (43 edges) and the `teleporter`. Continuation into those requires shelling
  them first.

## S1d-1.3 SCREEN WEIGHTING — the number the next stage needs

**Instrument.** Two arms of the standing recipe per review pose, differing in
one flag, with `FDS_DUMP_TXTR=1`:

- `clamp` = the standing arm (`--pom_recess_edge=0`). Its dumped march UV *is
  the geometric UV* exactly on the pixels the march could not answer.
- `nodom` = the same arm plus `--no-pom_shell_domain`. Its dump is the
  **unclamped landed UV**, i.e. where the ray actually crossed the height field
  after leaving the patch.

The pixels where the two dumps differ are exactly the population §10.6 of the
inventory sized by the same differencing — and because `--pom_recess_edge=0`
also catches never-bracketed lanes in BOTH arms, the difference isolates
**domain exits alone**. The pair gives the ray's entry UV and its landed UV, so
the exit point (where the segment leaves the domain = own UV box ∪ sibling
boxes) is solved by bisection and attributed to the nearest classified boundary
segment. Attribution quality, measured: the exit-to-nearest-classified-edge
distance is **0.0000 UV at p50 and p90, 0.0076 at p99** — the exits land on the
classified edges, they are not being snapped to something far away.

**Control (this is what makes the mask trustworthy).** A third arm,
`--pom_recess_edge=2` (discard), renders the same geometry and march. Its void
must be a SUBSET of the clamped mask. Measured over all 13 review poses:
**231 068 void px, of which 231 064 are inside the clamped mask (4 px outside,
0.002 %)**. And the per-pose voids reproduce the inventory's published figures
to the digit: **20 244 at t=5958** (§10.1's grazing-smear number) and
**85 065 at t=5743** (§10.6's `recW18` "if it discarded instead" column).

### The census, all 13 review poses

`clamped` = pixels the march could not answer. `void` = the subset that goes
black when the answer is a discard, i.e. the reported gash.

| pose | shell px | clamped | % frame | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY |
|---|---|---|---|---|---|---|---|
| p5743 | 1 719 288 | 94 952 | 4.58 | 2 | 12 346 | **82 604** | 0 |
| p5773 | 1 716 354 | 104 943 | 5.06 | 0 | 12 373 | **92 570** | 0 |
| p5813 | 1 723 713 | 115 466 | 5.57 | 0 | 21 768 | **93 698** | 0 |
| p5843 | 1 716 718 | 39 798 | 1.92 | 0 | 17 469 | 22 329 | 0 |
| p5963 | 1 743 657 | 36 386 | 1.76 | 0 | **20 308** | 16 078 | 0 |
| p6133 (mirror) | 1 993 796 | 8 940 | 0.43 | 20 | **8 920** | 0 | 0 |
| p6293 (mirror) | 2 034 521 | 7 961 | 0.38 | 9 | **7 952** | 0 | 0 |
| p5958a | 1 912 600 | 89 803 | 4.33 | 0 | 4 138 | **85 665** | 0 |
| p5958b | 2 003 920 | 73 557 | 3.55 | 0 | 3 305 | **70 252** | 0 |
| p5958c | 1 744 740 | 51 871 | 2.50 | 0 | 6 911 | **44 960** | 0 |
| p5958d | 1 674 997 | 63 364 | 3.06 | 0 | 5 296 | **58 068** | 0 |
| p6097 | 2 073 600 | 113 244 | 5.46 | 0 | 0 | 17 698 | **95 546** |
| p2845 | 2 073 600 | 228 | 0.01 | 0 | 228 | 0 | 0 |

| population | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY | total |
|---|---|---|---|---|---|
| **all clamped** | **31 (0.004 %)** | 121 014 (15.1 %) | **583 922 (72.9 %)** | 95 546 (11.9 %) | 800 513 |
| **void subset** | **31 (0.013 %)** | 76 765 (33.2 %) | **154 268 (66.8 %)** | 0 (0.0 %) | 231 064 |

### The answer to the question the plan asked

> *of the pixels the march currently cannot answer, what fraction sits at a
> coplanar seam that continuation would fix?*

**0.004 %.** 31 pixels of 800 513, across every pose the user reviews from.
Of the pixels that actually go BLACK, **0.013 %**.

By owning surface:

| material | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY | total |
|---|---|---|---|---|---|
| `rooms` | 0 | 14 167 | 485 287 | 0 | 499 454 |
| `rooms::mirUV` | 2 | 82 502 | 98 635 | 95 546 | 276 685 |
| `floor::mirUV` | 29 | 24 345 | 0 | 0 | 24 374 |
| `floor` | 0 | 0 | 0 | 0 | 0 |

By what is on the FAR side of the seam the ray left through:

| neighbour | COPLANAR | ANGLED-IN | ANGLED-OUT | TRUE BOUNDARY | total |
|---|---|---|---|---|---|
| `rooms` | 2 | 55 386 | 565 744 | 0 | 621 132 |
| nothing | 0 | 0 | 0 | 95 546 | 95 546 |
| `siling` (ceiling, **not shelled**) | 0 | 25 350 | 18 178 | 0 | 43 528 |
| `floor` | 29 | 39 041 | 0 | 0 | 39 070 |
| `teleporter` | 0 | 1 237 | 0 | 0 | 1 237 |

## S1d-1.4 WHY coplanar is zero: (A) is already shipping, under another name

`--pom_shell_merge_uv` (default **0.05**, landed in `dfb4272`) gives each patch
a SIBLING BOX LIST and the domain test is the UNION of the boxes. On a plane the
UV chart is a world-axis planar projection, so **the UV relationship between two
coplanar patches is the IDENTITY** — measured, worst disagreement across any
coplanar seam **1e-6 UV** on `rooms` and **6.8e-4 UV** on `floor`, over 549 + 217
classified coplanar segments. A coplanar hand-off therefore needs *no transform
at all*, and extending the domain to the union is exactly equivalent to
performing it.

Measured directly: pixels whose landed UV left their patch's OWN box but landed
inside a sibling's — i.e. coplanar continuations the kernel already makes:

| pose | clamped | already carried by siblings | share of the would-be population |
|---|---|---|---|
| p6133 (mirror) | 8 940 | 18 661 | **67.6 %** |
| p6293 (mirror) | 7 961 | 10 338 | **56.5 %** |
| p6097 | 113 244 | 30 465 | 21.2 % |
| p5773 | 104 943 | 4 277 | 3.9 % |
| p5743 | 94 952 | 3 112 | 3.2 % |
| **all 13 poses** | 800 513 | **70 585** | **8.1 %** |

So coplanar continuation is worth 70 585 px scene-wide and up to two thirds of
the population at the mirror poses — **and it is already being done.** What is
left over is, by construction, not coplanar.

## S1d-1.5 VIZ — look at it, do not trust the table

`--pom_seam_viz=1` (default 0) draws every classified boundary segment over the
final frame, depth-tested against the frame's own Z, on the `DisplaceViz`
overlay path: GREEN coplanar, ORANGE angled-in, MAGENTA angled-out, RED true
boundary. Modes 2/3/4 isolate one class. Main view only.

| crop (`docs/img/s1d_seams/`) | what |
|---|---|
| `p5743_A_seamviz_full.png` | the engine viz at the user's primary hole pose (full frame, ½ scale) |
| `p5743_C_classmap_full.png` | every clamped pixel, coloured by the class it exits through |
| `p5743_E_voidclassmap_crop.png` | **the money shot** — only the pixels that go black, at the right-hand wall corner (crop 1480,60–1920,1040) |
| `p5743_F_recdisc_crop.png` | the SAME crop of the discard arm: the black gash itself |
| `p6097_B_seamviz_crop.png` | the corner pose, engine viz (crop 150,60–800,1040) |
| `p6097_C_classmap_full.png` | the corner pose — a wide RED band where the wall genuinely ends |
| `p5958b_C_classmap_full.png` | the grazing-smear pose — MAGENTA at every convex panel corner |
| `p5958b_F_recdisc_crop.png` | the same pose's discard arm (crop 120,60–800,1040) |

`p5743_E` and `p5743_F` are the same crop of the same frame: **the magenta band
and the black gash are the same shape, pixel for pixel.** The user's reported
"full-height black gash on the right wall" is one boundary segment —
`rooms` g=9, world (9.8748, 0..4.9374, −49.3740), a 27° CONVEX ridge against
patch g=10 — and it alone owns **482 171 of the 800 513** clamped pixels across
the review poses.

## S1d-1.6 What the classes MEAN here — the mechanism, stated before the verdict

- **ANGLED-OUT (convex ridge), 72.9 % of clamped / 66.8 % of void.** The
  material is the intersection of the two half-spaces, so a ray that crosses the
  ridge while inside the slab has genuinely *left the solid*. Discard is the
  correct shell-theoretic answer. It voids anyway, because greets models its
  walls as surfaces with no interior: past the ridge there is no geometry to be
  revealed. A CLOSED shell fixes this by construction — the side face at the
  ridge terminates the ray on the shell boundary instead of letting it escape
  into nothing.
- **ANGLED-IN (concave fold), 15.1 % / 33.2 %.** The material is the union, so
  the ray is still inside solid and the neighbouring surface is what it runs
  into. This one genuinely needs a hand-off into the neighbour's chart.
- **TRUE BOUNDARY, 11.9 % of clamped, 0.0 % of void.** Every one of these 95 546
  px is at p6097, where the wall really ends and **something behind legitimately
  wins the pixel** — measured: zero of them void under the discard arm. So at a
  true boundary the *discard is already right*, and it is the recess arm's CLAMP
  that is wrong there (it paints flat wall over a hole you are meant to see
  through). This is the one class where the two failure modes are NOT symmetric.
- **COPLANAR, 0.004 %.** Already handled (§S1d-1.4).

## S1d-1.7 VERDICT

**Cross-patch continuation across COPLANAR seams — the plan's option (A) as
scoped above — would fix 31 pixels of 800 513. It does not dominate. It is
already implemented.**

The plan's expectation ("Expected to be the large majority — patches are split
by mesh topology, not by geometry") is **wrong for greets**, and the reason is
in the code: the union-find already merges every coplanar edge-adjacent face
into ONE patch, and `--pom_shell_merge_uv` already unions the coplanar patches
that do not share an edge. By the time a ray leaves a patch domain, it has left
the *plane*.

What dominates instead, in order:

| rank | class | clamped | void | what fixes it |
|---|---|---|---|---|
| 1 | ANGLED-OUT (convex ridge) | 72.9 % | 66.8 % | **(B) side faces** — close the shell at the ridge |
| 2 | ANGLED-IN (concave fold) | 15.1 % | 33.2 % | **(A′) angled continuation** — hand-off with a chart change |
| 3 | TRUE BOUNDARY | 11.9 % | 0.0 % | **(B) side faces**; discard already correct, the CLAMP is the bug |
| 4 | COPLANAR | 0.004 % | 0.013 % | already shipping |

**(B) dominates: 84.9 % of the pixels the march cannot answer, and 66.8 % of the
pixels that actually go black, sit at a boundary where the correct answer is a
closed shell, not a chart hand-off.** The remaining 15.1 % / 33.2 % needs a
hand-off, but the coplanar kind that is cheap is not the kind that is needed.

## S1d-1.8 FEASIBILITY for S1d-2, measured

**Is the coplanar UV relationship a simple affine transform?** It is the
IDENTITY. Worst disagreement 1e-6 UV (`rooms`) / 6.8e-4 UV (`floor`). Zero
transforms to build. (And zero pixels to win — §S1d-1.4.)

**The angled ones are not simple.** Over 1 700 classified angled segments:

- **27 distinct fold angles**: 18, 21, 25, 26, 27, 28, 29, 32, 33, 37, 38, 41,
  42, 44, 45, 49, 50, 52, 58, 63, 64, 65, 71, 78, 88, 90, 91 degrees.
- **71 distinct (fold, chart-scale-to-0.1, mirror) buckets.** The largest are
  90°: (scale 2.0 mirrored) ×219, (0.4) ×171, (2.5) ×168, (1.0) ×126,
  (1.0 mirrored) ×120, (2.0) ×115.
- Chart scale ratio across a seam spans **0.79 … 2.51** — the wall↔ceiling and
  wall↔floor seams carry a factor of 2–2.5 because the materials' UV densities
  are 6 and ~15 world per tile.
- **710 of 1 700 angled segments (41.8 %) are MIRRORED** — the neighbour's chart
  has the opposite handedness, so a hand-off must flip an axis. This is the same
  handedness split `GreetsFixBitangentHandedness` already knows about; it is not
  a surprise, but it means the transform is not a rotation+scale, it is a full
  2×3 affine including reflection.
- No patch mixes handedness internally (each is coplanar with one chart), so the
  4:1 trapezoid chart that broke the tessellation bake's `edgeAlignedQuad` has
  **no analogue here** — every patch chart is a single affine map. Measured:
  `rooms` per-plane world-per-UV is constant to four decimals (§8.3 finding 1
  of the inventory), and this census found zero degenerate charts.

**Hop budget — how far past the boundary does the ray actually need to go?**
Measured on every attributed pixel (the distance from the exit point to where
the march landed, i.e. how far into the neighbour the hit is):

| quantity | p50 | p90 | p99 | max |
|---|---|---|---|---|
| remaining UV travel after the exit | 0.0217 | 0.1393 | 0.2263 | 8.55 |
| … as a fraction of the patch's own UV width | 0.022 | 0.159 | 0.259 | 9.24 |
| same-plane box crossings on the whole ray | 1 | 1 | 2 | 6 |

At mip 0 (1 024 texels/tile) the median is **22 texels past the boundary** and
p99 is **232 texels**. **One hop resolves the p99 case**; the plan's proposed
budget of 4 is generous, and 2 would do. The ray leaves its patch only just
before it would have hit — it is not travelling across the room.

**What S1d-2 cannot do without more work:** 43 528 px (5.4 % of the clamped
population) exit toward `siling`, the ceiling, which has **no height map and no
shell**. A hand-off has nothing to hand off to there until the ceiling is
shelled.

## S1d-1.9 GATES — every new flag default OFF, every pin byte-exact

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (mirrortest `4ac809e5`, conetest `b41894f9`, halotest `166fa25a`) |
| city `t=1961` | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact |
| greets `shell` cone-8 `t=6097` | `193427ccb28163705ea6baa5500afd0c` — byte-exact |
| greets `tess` `t=6097` | `3f86c73cc7ed8f0ad8f57b12984537d0` — byte-exact |
| greets `flat` `t=6097` | `9d095fbcac0c00888578d56172786997` — byte-exact |
| wasm | `cmake --build build-wasm` links clean (80/80) |
| bad flags | 0 across all 61 run logs (55 snapshot renders + 5 gate runs + the census run) (`unknown flag` / `requires a value`) |

The census is init-time only and the viz is a post-tonemap overlay on the main
view, so neither can reach the rasterizer, the offscreen passes or the G-buffer.
Cost of `--pom_seam_census` when ON: the whole greets init run takes 3.7 s wall.

## S1d-1.10 RECOMMENDATION for S1d-2

Build **side faces first (the plan's B), not march continuation (A)**.

1. **S1d-2a — close the shell at ANGLED-OUT ridges and TRUE boundaries.**
   Together 84.9 % of the clamped population and 66.8 % of the void. At a convex
   ridge the side face is the neighbour's own plane, so it is derivable from the
   topology this stage already builds — no new authoring. Gate it on void at all
   13 review poses and on the `recdisc` arm's void going to single digits.
2. **S1d-2b — fix the clamp at TRUE boundaries independently and cheaply.**
   Those 95 546 px void ZERO under a discard: the geometry behind already fills
   them correctly. A per-boundary-class edge policy (discard at TRUE, clamp
   elsewhere) is a *table lookup on data this stage already produces* and needs
   no new geometry. It is the cheapest measured win available and should be
   costed before anything else.
3. **S1d-2c — angled continuation, last.** 15.1 % / 33.2 %, and it is the
   expensive one: 71 distinct affine transforms, 42 % of them mirrored, and 5.4 %
   of its target pixels point at an unshelled ceiling. One hop suffices
   (p99 = 232 texels), so the kernel cost is bounded; the *bake* is where the
   work is.
4. **Do NOT build coplanar continuation.** It is 31 pixels, and
   `--pom_shell_merge_uv` already does it.

One caveat stated plainly: this census is of GREETS. The verdict "coplanar is
nothing, convex ridges are everything" is a property of this scene's
architecture (closed rooms, symmetric wall pairs, no free edges), not a general
truth about the shell model. The instrument is scene-agnostic — point it at
another material and it will answer again.
