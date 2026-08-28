# BULGE VERDICT CORPUS — Gil-Ad's labeled states (assembled 2026-08-28)

The ONLY ground-truth labels for the wall-look defect are his words. Every
detector must separate these; a detector that passes a broken-labeled state
is invalid (five did: black-px counts, coordinator eye, geometry
cross-sections, GBI/SWEEP, SGM). Labels below quote him verbatim; absence of
complaint is NEVER an OK label.

Arm recipes reproduce on the rev-dispfix binary (t=5965; cam A
"22.5084476,3.87992334,-61.8882256,-0.829246342,-0.20816116,0.518670499",
cam B "20.7104416,3.05759621,-59.807045,-0.863270998,-0.251949817,0.437360346";
base flags --deferred --hdr --hdr-linear --texture-filter=2 --ssao
--ssao-gtao). Corresponding raw dumps: docs/img/refdiff/raw/<arm>_<cam>/.

| # | state / arm | label | his words |
|---|---|---|---|
| 1 | bare wall (no --greets-displace) | **OK (geometry)** | "the bare wall is flat" — a claim about the undisplaced wall's shape. NOT a blanket shading-OK: he separately said the first displacement-map version "already had an issue where parts of the wall jumped outside (it didn't even use tessalation)", so pre-existing shading pollution on the bare wall is consistent with the corpus, not contradicted by it. |
| 2 | month-old bulge era (`prefo`: --greets-displace --no-greets_displace_front_orient --no-greets_displace_groove_shade_plane --no-greets_displace_groove_front_majority --no-greets_displace_corner_front_ride) | **BROKEN** | "still bulges, still not even close to be ok"; the month of "the fucking wall issue" |
| 3 | front_orient era (`fixcam`: as #2 but front_orient ON) — the fix_camA_tex.png / fix_camB_tex.png build | **BROKEN** | "i took a look on the pics you've watched - they have the same issue as always - a bulge" |
| 4 | groove_shade_plane era (`ship`: as #3 + groove_shade_plane ON) | **BROKEN, improved** | "it still sucks (although a bit bit better)" |
| 5 | r3 / current defaults (all four fixes ON) | **BROKEN** | "you are showing me obviously wrong results, and you are unable to detect that the results are bad" |
| 6 | v3 clean-room bake (rev-dispv3 branch, m5/m3b images) | **BROKEN** (not reproducible in this binary — label from images) | "wow, this is worse than useless"; "even worse - the 10% didn't even work" |
| 7 | reflex weld build (superseded; weld now default OFF) | **BROKEN (added tears)** | "the last change added tears. I don't care about those tears, it's a bug written when trying to fix a real bug" |
| 8 | pom_shell first-version era (bare + --pom_shell) | **BROKEN (historical)** | "the first version of the displacment map already has an issue where parts of the wall jumped outside (it didn't even use tessalation)" — no honest plane reference exists for shell geometry; corpus label only. |

| 9 | his IN-PLACE MARKS on the r3 render (docs/img/bulgedetect/A_r3_tex_MARKED.png; clean re-render restored as A_r3_tex.png) | **BROKEN (geometry, localized)** | rect ~(1080,315)-(1235,415): the grout course on the seam column JUTS OUT past the arris (mortar lip/shelf, dog-leg in the course line); ellipse c~(1160,650) r~(130,115): a stone wedge STEPS OUT past the arris with a dark crack beneath. Protruding-silhouette class — the reason normal-field metrics under-scored. |

Ambiguous / unlabeled: everything else — including all curated crops ever
shown without his explicit verdict.

## Validation matrix — refdiff (ground-truth plane+heightfield reference)

Metric: LF-band (sigma 16px) angle between the engine's pre-nmap G-buffer
normal field and the reference normal of the ideal displaced surface, per
dominant plane; fire = worst-plane median well above the bare floor.
Full numbers: docs/img/refdiff/metrics_refdiff.txt.

| corpus # | arm | worst-plane LF med/p90 (cam A) | verdict | matches label? |
|---|---|---|---|---|
| 1 | bare | planes 43/48: **0.00/0.01** (exact floor); planes 45/41: 7.6/15.1, 9.3/9.9 | quiet on 2 planes, fires on 2 | YES with a FINDING: pre-existing shading pollution on planes 45/41 (bare!), consistent with his #8 datum |
| 2 | prefo | 9.37/19.71 | FIRES | YES |
| 3 | fixcam | 9.30/19.55 | FIRES | YES |
| 4 | ship | 3.23/14.63 | FIRES (reduced) | YES — his "bit better" ordered correctly |
| 5 | r3 | 3.23/14.59 | FIRES | **YES — the state every prior metric passed** |

Ordering check: 2 ≈ 3 > 4 ≈ 5 >> bare-clean-planes(0.0) — matches his
verdict order (front_orient changed corners he wasn't judging; groove_shade
_plane was his visible "bit better"; r1/r3 were seam-local).

## What the r3 heatmap localizes (docs/img/refdiff/refdiff_r3_A.png)

The surviving error wraps EVERY grout band / block border in red (>=15 deg
LF) while block-top interiors are green — on both pier walls, everywhere,
plus the corner-fan diagonals and the wall base. The engine's vertex-normal
field is wrong exactly at the bevels; every block gets a soft wrong-lit
frame — the per-block pillow read. (Mechanism hypothesis, NOT proven here:
the 80-deg weld averaging bevel normals with top normals, i.e. the named
"bevel-aware gate" open design + the groove band blends.) ## The GEOMETRY channel and his marks (the binding validation)

dz = z_reference(displaced envelope) − z_engine, per pixel, unmasked by any
depth-agreement filter. Bare arm at his two marked regions: dz median
0.0000, p90 |dz| 0.0011 u — the reference is proven exact at his exact
marks. The r3 build he marked:

| region | frac protruding past the outer mitre envelope (dz>+0.08 u) | frac |dz|>0.08 | p90 |dz| |
|---|---|---|---|
| MARK-rect | **28.7 %** | 64.6 % | **0.665 u** (4x the bake amplitude) |
| MARK-ellipse | **14.9 %** | 52.6 % | 0.53 u |

FIRES with huge margin inside both marks — and near-identically in every
displaced era (prefo/fixcam/ship/r3 all ~15–30 % protruding): the seam
-column geometry defect he keeps circling was never touched by any of the
month's normal-side fixes, which is why every round read as "the same
shit". Heatmap: docs/img/refdiff/refdiff_geo_r3_A.png (his regions outlined
yellow; the seam column is a red river of protrusion top to bottom).

Scene-wide geometry reading from the same channel (measured; mechanism NOT
established): block faces render ~0.05 u RECESSED behind the ideal
displaced surface (blue), while grout/course lines rim RED (engine in front
of the reference there — grooves not carved to reference depth and/or
course lips overshooting), and the seam column protrudes outright. Bare
arm: 0.0000 exact everywhere. Whatever the mechanism, the displaced
surface's relief is systematically offset from the height field's truth —
a defect class every engine-self-referencing instrument was blind to.

## Fix-round ledger (2026-08-28, post-validation)

- **Dependency (measured first, as ordered):** simulating a DC fix (engine
  +0.05 out) makes the marked protrusions WORSE (rect 28.7→35.8%, ellipse
  14.9→21.2%): the recession partially masks the juts. Mechanisms are
  independent-and-opposing → juts first.
- **Vert-level audit of the profile offset:** plane-43 interior verts sit
  ON the reference at plateaus (own-excess med −0.003) but grooves are
  over-carved (med −0.0515); the rasterized faces sag between correct
  plateau centres and too-deep widened grooves → the pixel-level −0.05
  plateau recession. Mechanism (2) is the groove-carve arithmetic (the
  notch/ramp chain), not the plateau displacement. No lever built yet.
- **Envelope vert clamp — REFUTED as the juts fix** (flag
  `greets_displace_env_clamp`, default OFF, kept as instrument): catches a
  real off-envelope population (576 verts, worst 1.16 u; the census's 70
  zero-displacement bake-CREATED lip verts included) and changes ~78k px,
  but his marked regions' protrusion fractions are unmoved (28.7→27.1%,
  14.9→14.8%). The juts therefore are not vert positions: supported
  hypothesis = BRIDGING FACES spanning the junction whose corner verts are
  each legal on their own wall. Indicated fix: face-level split at the
  junction line (the border-pipeline reorder named twice before), so no
  face crosses the envelope corner.
