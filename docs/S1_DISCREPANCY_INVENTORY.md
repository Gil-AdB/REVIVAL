# S1 — Displacement discrepancy inventory (evidence only)

Status: EVIDENCE DOCUMENT, 2026-08-04, branch `fog-wt`. **Nothing here is a fix,
a tuning, or a default change.** Every flag this campaign added is diagnostic and
default OFF. No recommendation is made about which architecture to adopt — that
call is the user's, and this file exists so it can be made from measurements
instead of from a story.

Companion plan: `docs/S1_PIXEL_DISPLACEMENT_PLAN.md`. Where a number here
contradicts a number there, this file's method is stated in full and the earlier
number is flagged as NOT REPRODUCED rather than quietly replaced.

---

## 0. Tessellation is an arm under test, not the yardstick

Every previous round of this campaign measured the per-pixel path against
`--greets_displace` and called the difference "error". That is invalid.
The tessellation bake is an approximation with its own errors, and this document
measures them:

- it samples the height map at `--greets_displace_mip` (default 2 = 256² per UV
  tile), not at mip 0;
- it carries the relief only at its subdivision LATTICE, and the adaptive bake
  leaves large quads coarse (measured: at t=6097 a single tessellation triangle
  covers a 600×400 screen region of wall and carries **zero** relief depth there
  — §C4);
- it pins patch-border vertices to zero displacement, so its surface and the
  shell's are not the same surface at a border (§C7);
- `--greets_displace` swaps only the HEIGHT map to the residual; the NORMAL map
  stays FULL, so the low band is shaded twice (noted in the plan, not re-measured
  here);
- the bake relaxes 3 818 inverted 'rooms' faces and falls back to fan cells on
  431 of them at default settings (from the `[STONE]` census).

So the inventory is measured against a **converged reference** instead, and
tessellation appears in the tables as one arm among several.

## 1. The converged reference

`--pom_ref_march` (new, DIAGNOSTIC, default OFF) replaces both shipping marches
with **`--pom_ref_steps` uniform steps down the whole relief slab plus a secant
solve on the bracketing pair** — no cone map, no step budget, no
`--parallax_pom_lod` fade, no `--parallax_pom_quarter` lane sharing. Reference
arm:

```
--no-greets_displace --pom_shell --pom_shell_cap=64 --pom_ref_march --pom_ref_steps=512
```

`--pom_shell_cap=64` because the cap is a grazing quality hack, not part of the
surface definition; 64 is the kernel's own uncapped limit.

**Convergence is measured, not assumed.** 512 steps vs 1024 steps, same pose,
same everything:

| | ownership changed | depth off >0.05 world | median dz |
|---|---|---|---|
| t=6097 | 270 px (0.01 %) | 493 px | 0.0000 |
| t=5780 | 311 px (0.01 %) | 4 925 px | 0.0000 |

The reference has converged in geometry and depth. It has **not** fully
converged in colour: 49 301 px at t=6097 and 70 736 px at t=5780 still differ by
>12/255 between 512 and 1024 steps, all of it on the dithered grazing mortar
bands. Treat reference colour on those bands as ±1 step of the march, not as
exact.

### What the reference is NOT (limitations, stated up front)

1. **It rasterises the LID**, so it inherits `PomShell_Build`'s moved vertices
   for every offscreen pass. §C6's offscreen numbers are therefore measured
   between two *arms*, never against the reference.
2. **Its lateral boundary is an open cut, not a pinned taper** (§C7). Where a
   grazing ray leaves the patch it passes through and reveals what is behind;
   the tessellation bake instead pins the border to zero and keeps the surface
   solid. These are different surfaces. Neither is more true a priori, and the
   difference is 143 835 px at t=6097 — the single largest term in the whole
   inventory. It is reported as a *modelling divergence*, not as anybody's bug.
3. Point-sampled, 1× resolution, so silhouette pixels are aliased exactly like
   the arms they are compared to (that is deliberate — a supersampled reference
   would add an AA difference that swamps the classes being measured).
   `--snapshot_ss` exists for a supersampled capture and was used for spot
   checks only.
4. A supersampled ultra-fine tessellation cross-check was run
   (`--greets_displace --greets_displace_cpb=6 --greets_displace_mip=0`,
   146 661 'rooms' faces vs 68 513 at defaults). It does **not** converge to the
   reference — see §C4 — and that non-convergence is itself a finding.

## 2. Instruments added for this document (all default OFF)

| flag | what it does |
|---|---|
| `--pom_ref_march`, `--pom_ref_steps` | the converged brute-force march above |
| `--face_id_dump` | per-pixel FACE identity plane + `greets_t<t>_face.u32` dump + a `[FACEID]` key→(mesh, face index, material, world plane, verts, UVs) table. matID is one byte shared by every greets wall, so it can never answer "which polygon won this pixel, and should it have been occluded" |
| `--pom_shell_lid_probe` | DIAGNOSTIC PROBE: forces `PomShell_Build`'s lid offset to ZERO while leaving `Vertex::ShellH`, `Material::PomShellUvAmp`, the patch domains, the sibling boxes and every kernel path bit-for-bit identical. Differencing it against the real shell attributes a discrepancy to the MOVED VERTICES (it vanishes) or to the MARCH (it survives). **It is not a proposal** — with the geometry unmoved the relief hangs entirely below the authored plane instead of straddling it |
| `--snapshot_ss` | render a snapshot at N× resolution (snapshot path only) |

The face-id plane is image-neutral: `--pom_shell --pom_shell_cap=2` at t=6097
gives md5 `193427ccb28163705ea6baa5500afd0c` with and without `--face_id_dump`.

The lid probe is exact: with the shell built at zero offset, the per-frame shadow
bake is **byte-identical** to the no-shell arm (0 of 13 533 184 shadow-map texels
differ across 76 maps).

### Cross-process face identity

The raw face key is a pointer, so it is process-local. Two further traps had to
be worked around and both are worth recording:

- `(mesh, faceIndex)` is **not** comparable between a shell arm and a non-shell
  arm: the Piramid mesh is split on a spatial CHUNK grid and moving the stone
  vertices 0.1 world outward re-bins some faces (measured at pmir: chunk index
  off by 2, face index off by 1). `--greets_displace` subdivides outright, so its
  face indices do not exist in the other arms at all.
- The comparable identity is the **authored PLANE** a face belongs to (material +
  nearest authored plane, planes taken from the `flat` arm whose faces *are* the
  authored polygons). A first cut of this classifier rejected faces whose normal
  tilted more than 60° off the plane and thereby mislabelled **266 012 px** of
  ordinary displaced wall as an "ownership change" — the bake's groove walls are
  nearly perpendicular to the plane, which is what a carved groove *is*. The
  numbers below use distance-to-plane only (≤0.5 world, 3× the bake amplitude).

## 3. Arms and poses

Arms (one build, `fog-wt` @ this commit, all rendered headless from `Runtime/`
with `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, `--deferred --profiler=0
--face_id_dump`, `FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1`):

| arm | flags |
|---|---|
| `ref` | `--no-greets_displace --pom_shell --pom_shell_cap=64 --pom_ref_march --pom_ref_steps=512` |
| `refcap2` | as `ref` but `--pom_shell_cap=2` (isolates the cap) |
| `refnodisc` | as `ref` but `--no-pom_shell_domain --no-pom_shell_base_clip` |
| `flatref` | `--no-greets_displace --pom_ref_march` (converged march, NO shell) |
| `tess` | `--greets_displace` (today's shipping look) |
| `tessfine` | `--greets_displace --greets_displace_cpb=6 --greets_displace_mip=0` |
| `flat` | `--no-greets_displace --parallax_pom_cone` |
| `shell` | `--no-greets_displace --parallax_pom_cone --pom_shell --pom_shell_cap=2` |
| `shellnaive` | as `shell` without `--parallax_pom_cone` (naive-8) |
| `shell64` | as `shell` with `--pom_shell_cap=64` |
| `lid` | `shell` + `--no-pom_shell_domain --no-pom_shell_base_clip` (masks off) |
| `lidraw` | `lid` + `--pom_shell_cap=64 --pom_shell_merge_uv=0` (all four masks off) |
| `bconly` | `shell` + `--no-pom_shell_domain` (base clip only) |
| `bconlyref` | `bconly` with the converged march |
| `probe` | `shell` + `--pom_shell_lid_probe` |
| `probelid` | `lid` + `--pom_shell_lid_probe` (masks off AND geometry unmoved) |

Poses (identical framing per pose across every arm — one script, one camera):

| tag | t | camera | what it is |
|---|---|---|---|
| `p6097` | 6097 | `FDS_GREETS_CAM="18.4499683,5.16043377,-57.6482239,-0.824408829,-0.544822097,-0.153357133"` | the user's corner repro; 100 % stone |
| `p5780` | 5780 | scripted | bench wall + statues |
| `p2845` | 2845 | `FDS_GREETS_CAM="-7.38721609,2.72471762,-50.8239441,0.817980111,-0.113630958,0.563911617"` | grazing close-up; 100 % stone, one wall plane |
| `p4200` | 4200 | scripted | **wide/vista** — several walls, a doorway, deep floor |
| `pmir` | 5780 | `--greets-mirror-cam` | **the teleporter mirror is on screen** |

Crops use one transform for every arm: 1920×1080 → 900×506 LANCZOS → crop
(0,0,340,506) → NEAREST ×2. That is the user's t=6097 framing.

---

## 4. RANKED CLASS INVENTORY

Ranking is by how much of the frame the class breaks at the poses where it
appears, weighted toward classes the eye picks up as structure (streaks, bands,
a surface in the wrong place) over classes that read as a small brightness shift.

---

### C1 — CONE-MARCH UNDER-CONVERGENCE. Grazing streaks and up-to-a-full-slab depth error. *(shading path)*

**Rank 1. This is the largest colour class, the largest depth class, and it is
what is visible in the user's own crop.**

**What it looks like:** long horizontal smears across a grazing wall, and a hard
dark band under a wall edge. Crop path (identical framing):
`docs/img/s1_discrepancy/6097_E_shell.png` (cone-8) against
`6097_B_reference_cap2.png` (converged, **identical geometry, identical masks,
identical cap** — the only difference is the march).
`6097_F_shell_naive8.png` shows the naive-8 march for contrast.

**Where:** wherever the view grazes the surface. Frame-wide at t=6097 and
t=5780; concentrated on the FLOOR at t=4200 and pmir.

**Measured**, arm `shell` (cone-8) against `refcap2` (uniform-512), which differ
in nothing but the march:

| pose | colour >12/255 | colour >32/255 | depth off >0.05 world | depth off >0.20 world | ownership changed |
|---|---|---|---|---|---|
| t=6097 | 213 266 | 46 480 | 197 180 | 19 748 | 4 033 (3 749 of them drawn NEARER) |
| t=5780 | 181 304 | 55 394 | 117 211 | 69 597 | 5 254 (5 166 NEARER) |

Without the shell at all (`flat` cone-8 vs `flatref` uniform-512, no geometry
difference whatsoever): 183 287 px >12/255 at t=6097, 137 384 at t=5780 —
so this class is **not** a shell artefact, it is the shipping POM march.

**The depth tail is the FLOOR.** Same-owner pixels where `shell` is more than
0.20 world off the reference:

| pose | count | dominant surface | median signed dz |
|---|---|---|---|
| t=5780 | 515 556 | `floor::mirUV` 283 073 | **−0.379** (nearer) |
| t=4200 | 611 560 | `floor::mirUV` 575 517 | **−0.945** (nearer) |
| pmir | 480 461 | `floor::mirUV` 391 240 | **−0.844** (nearer) |

The floor's slab is ≈1.1 world deep (`uvAmp` 0.075 × worldPerUV ≈15), so at
t=4200 the cone march is landing the floor surface most of a slab **in front of**
where the relief actually is.

**The cap is not the mechanism.** `refcap2` (cap 2) vs `ref` (cap 64):
**4 px** of ownership change, 0 colour, 0 depth at t=6097. `shell64` vs `shell`:
4 px. The `--pom_shell_cap` dial does essentially nothing at these poses.

**Naive-8 has the opposite error profile** (measured against `refcap2`, t=6097):
colour 701 133 px >12/255 (3.3× worse than cone) but depth only 51 954 px
>0.05 world (3.8× better) and 3 107 px >0.20 (6.4× better). Neither shipping
march is close to the converged one.

**Cause: the MARCH, not the moved geometry** — established by holding geometry,
masks and cap fixed and changing only the march.

---

### C2 — THE MOVED VERTICES INTERPENETRATE THE NEIGHBOURING WALL, inside the authored footprint. *(moved geometry)*

**What it looks like:** at a corner, the near wall's stone is painted over the
far wall's pixels, several world units in front of where the true surface is.
Crop path: `6097_M_map_shell_vs_ref_crop.png` — the RED patch at the corner
(x≈440-590 of the ×2 crop).

**Measured, with the march eliminated as a variable.** Arm `bconlyref` = shell
geometry + base clip ON + lateral-exit discard OFF + **converged march**. Its
colour and depth error against the reference on same-owner pixels is **zero**
(1 px >12/255, 0 px |dz|>0.05), so every pixel it loses is pure geometry:

| arm (t=6097) | ownership changed | of those drawn NEARER | median depth in front of truth |
|---|---|---|---|
| `bconlyref` (converged march) | **17 326** | 17 326 (100 %) | −5.338 world |
| `bconly` (cone march) | 19 658 | 19 658 (100 %) | −5.260 world |

**The prior campaign's "8 801 px of interpenetration at t=6097" is NOT
REPRODUCED.** My independent count for the same configuration is
**17 326 px** (converged march) / **19 658 px** (cone march) — roughly 2×. The
methods differ: the earlier figure compared per-MATERIAL coverage masks against
the tessellation arm, and tessellation has ~99 k px of its own ownership error at
this pose (§C4), so it cannot serve as the zero. I am not claiming the earlier
number was computed wrongly for what it measured; I am reporting that it does not
survive re-measurement against a converged reference with per-face ownership.

The lateral-exit discard removes this population (the reference has it by
construction; `shell` is left with 3 753 nearer px, and those are C1's depth
error losing a Z fight, since they persist against the same-geometry
`refcap2` arm).

**Sub-class C2b — the wall wins against a PROP.** At t=6097 the tan wall
(`rooms::mirUV`, plane x=+17.90) takes 3 px from the robot legs
(`R_leg1.lwo::hull`, `R_leg2.lwo::hull`) at **6.99–8.59 world units nearer** than
the true surface. Three pixels at this pose, but the mechanism is a wall
overwriting geometry that is genuinely in front of it, so it is listed.

---

### C3 — RAW LID OVER-COVERAGE (what the four masks exist to remove). *(moved geometry)*

**What it looks like:** stone hanging past the end of a wall into space.
Crop path: `6097_G_lid_masksOFF.png` vs `6097_A_reference.png`;
class map `6097_M_map_lid_vs_ref.png` (solid RED = surface nearer than truth).

**Measured**, t=6097:

| arm | ownership changed | NEARER | median in front of truth | colour/depth error otherwise |
|---|---|---|---|---|
| `refnodisc` (converged march, all masks off) | **143 835** | 143 835 (100 %) | −5.308 world | 9 px >12/255, **0 px** |dz|>0.05 |
| `lid` (cone march, masks off) | 145 761 | 145 761 (100 %) | −5.298 world | (C1 on top) |
| `shell` (masks on) | 4 037 | 3 753 | −0.693 world | (C1 on top) |

So the domain test + base clip together remove ≈144 k px of over-coverage at this
pose and change nothing else — `refnodisc` differs from `ref` in coverage only.

**Attribution to the moved vertices, measured directly** by the probe. `lid` and
`probelid` differ in exactly one thing (the lid offset); every mask, march and
kernel path is identical:

| pose | ownership changed | of those NEARER | median dz |
|---|---|---|---|
| t=6097 | 182 891 (8.8 %) | 180 138 | −5.093 |
| t=5780 | 428 488 (20.7 %) | 398 956 | −1.547 |
| t=4200 | **669 033 (32.3 %)** | 602 862 | −1.686 |
| pmir | 535 773 (25.8 %) | 483 882 | −1.782 |
| t=2845 | 26 798 (1.3 %) | 2 753 | −0.579 |

At the vista pose the moved geometry alone changes which surface owns **a third
of the frame** before any mask runs.

---

### C4 — TESSELLATION CARRIES RELIEF ONLY AT ITS LATTICE; large wall regions carry NONE. *(the shipping look's own error, geometry)*

**What it looks like:** a wall that should show block relief renders perfectly
flat. Crop path: `6097_C_tessellation.png` (upper-right tan wall) vs
`6097_A_reference.png`; class map `6097_M_map_tess_vs_ref.png` — the whole
upper-right quadrant is YELLOW (depth off by >0.05 world).

**Measured — relief depth actually written to Z.** Method: take a rectangle
lying wholly on one authored wall plane, least-squares fit 1/z linearly in screen
space over that plane's pixels (exact for a planar surface under perspective),
and measure the residual in world units. The spread of the residual IS the relief
the arm put into the Z buffer.

t=6097, box (1150,120)-(1750,520) on the tan wall, 240 000 px:

| arm | p05 | p50 | p95 | span (p5..p95) |
|---|---|---|---|---|
| `ref` | −0.0074 | 0.0000 | +0.0131 | **0.0206** |
| `ref1024` | −0.0074 | 0.0000 | +0.0132 | 0.0206 |
| `shell` | −0.0065 | −0.0002 | +0.0157 | 0.0222 |
| `tess` | −0.0011 | 0.0000 | +0.0011 | **0.0023** |
| `tessfine` | −0.0011 | 0.0000 | +0.0011 | **0.0023** |
| `flat` | −0.0011 | 0.0000 | +0.0011 | 0.0023 |

0.0023 world is the zEnc quantum (1 code = 0.00253 world). **The tessellation
carries no measurable relief at all in that region, and raising the bake to
`cpb=6, mip=0` does not change it.** The face-id dump says why: that whole
600×400 region is covered by ONE tessellation triangle (`Piramid.lwo:c57` face
140, 239 815 of 240 000 px; `tessfine` likewise one face,
`Piramid.lwo:c56` face 234). The reference and the shell resolve relief there at
pixel granularity.

**Frame-wide at t=6097**, same-owner depth error against the reference:

| arm | depth off >0.05 world | colour >12/255 | colour >32/255 |
|---|---|---|---|
| `tess` | **818 655 px (39 %)** | 896 313 | 166 786 |
| `tessfine` | 552 301 px (27 %) | 874 141 | 206 110 |
| `shell` | 197 180 px (9.5 %) | 213 265 | 46 480 |

The ultra-fine bake reduces the class by a third but does not remove it, so the
cross-check the coordinator asked for **fails to agree** with the brute-force
marcher. Report that as a finding: the two displacement models do not converge to
the same surface, and the gap is not a subdivision-density problem alone.

At a pose where the bake does subdivide (t=2845, one wall plane, 240 000 px box)
the relief spans are `ref` 0.0525 / `tess` 0.0475 / `shell` 0.0459 / `flat`
0.0023 — i.e. tessellation carries 90 % of the true relief depth there. The class
is strongly pose- and quad-dependent, not uniform.

---

### C5 — FLAT POM WRITES NO RELIEF DEPTH AT ALL. *(shading path, configuration)*

Measured relief span for `flat` is 0.0023 world — under one zEnc code — in every
box measured, against 0.0206–0.0525 for the reference. `--pom_depth_write`
defaults OFF and the flat arm does not pass it, so the Z buffer sees the flat
polygon plane while the colour shows parallax. Consequences that follow
mechanically and were not separately measured here: SSAO/GTAO, fog, DoF and the
quarter-res reconstruction all see a flat wall.

Listed for completeness — it is a property of that arm's flags, not a defect
anyone has to fix.

---

### C6 — OFFSCREEN CONSEQUENCES OF THE MOVED GEOMETRY. *(moved geometry)*

**Nobody had checked this. The walls really are `amp/2` thicker for every
offscreen consumer, and it shows.**

**Shadow bake — measured.** `--dump_shadowmap` (76 cube faces, 13 533 184 depth
texels) from a `-DFDS_DEV=ON` build in `build-dev/` (which does **not** overwrite
`Runtime/DEMO`), t=5780:

| pair | texels differing | by >8/255 |
|---|---|---|
| `flat` vs `probelid` (both unmoved geometry) | **0 (0.00 %)** | 0 |
| `lid` vs `probelid` (only the lid offset differs) | **4 044 301 (29.88 %)** | 181 301 |
| `tess` vs `flat` | 720 250 (5.32 %) | 81 809 |

The zero row is what makes the rest trustworthy: the probe genuinely leaves
geometry untouched, and the shell's raster-side machinery does not reach the
shadow bake. The moved lid changes **30 % of the per-frame shadow depth cube**.
Note the engine's PolyId shadow test is identity-only, so what these depth
deltas do to the final image is *not* read off this table — it is measured
separately below.

**Final image, on NON-STONE pixels only** (so the wall's own shading cannot
contribute anything; `lid` vs `probelid`, geometry the only variable):

| pose | non-stone px | changed >4/255 | >12/255 | >32/255 | max | worst-hit surfaces |
|---|---|---|---|---|---|---|
| t=5780 | 323 021 | 17 303 | **10 370** | 5 626 | 162 | `siling` 5 912, `L_leg2.lwo::hull` 1 875, `Hull.lwo::cockpit_upper::mirUV` 1 104 |
| t=4200 | 143 381 | 5 347 | 2 047 | 768 | 149 | `siling` 1 881 |
| pmir | 78 821 | 7 303 | **4 284** | 2 335 | 129 | `sss` (shattered mirror) 2 062, `siling` 1 133, `amudim::mirUV` 350 |
| t=6097, t=2845 | 0 | 0 | 0 | 0 | 0 | (frames are 100 % stone) |

**Mirror pose.** At `pmir`, with stone included, `lid` vs `probelid` changes
1 255 479 px by >12/255 and 537 236 by >32/255, and the affected surfaces include
`sss` (the shattered-mirror shards) and `amudim::mirUV` — mirror-side content, not
just direct wall pixels.

**Mechanism: PARTIALLY UNKNOWN, deliberately.** What is established: the moved
walls change the shadow maps by 30 % of texels, and they change surfaces that are
not the wall by up to 162/255. What is *not* established is which offscreen
consumer (per-frame shadow bake / mirror RTT / env probes / lightmap bake) is
responsible for a given ceiling or robot-leg pixel — the arms differ in all of
them at once and I did not build a per-consumer isolation. Do not read the 30 %
shadow figure as the cause of the 10 370 px; they are two separate measurements.

---

### C7 — THE REFERENCE'S OWN BOUNDARY MODEL. A modelling divergence, not anyone's bug.

The shell (and therefore the reference) treats a patch's relief as **ending at
the patch border with an open side**: a grazing ray that leaves the UV domain
passes through and reveals what is behind. The tessellation bake instead **pins
border vertices to zero displacement**, keeping one continuous solid surface.

**Measured.** `refnodisc` vs `ref` = 143 835 px at t=6097 — the discard's entire
coverage effect, with zero colour or depth difference otherwise. And at t=6097
`tess`, `tessfine`, `flat` and `probelid` all differ from the reference on the
**same 103 05x px** — that is the reference's own silhouette signature showing up
identically in every arm that lacks it, not four independent errors.

This is the largest single number in the inventory and it is **definitional**. It
is the reason "differs from the reference" must be read per class and not summed.

---

### C8 — Per-pose summary table

`ref` = converged reference. `ownChg` = pixels whose owning authored WALL/FLOOR
PLANE differs. `ownNearer` = of those, how many the arm draws more than 0.05
world IN FRONT of the true surface. `dz` on same-owner pixels only, + = arm
farther than truth. Wall slab ≈0.18–0.23 world deep, floor ≈1.1, 1 zEnc code =
0.00253 world.

```
=== p6097 (t=6097), 100 % stone ===
  arm       |   ownChg ownChg% ownNearer medNearZ |  dCol>12  dCol>32 | |dz|>.05 |dz|>.20   medDz     p05     p95
  tess      |   103055   4.97%     99401   -0.210 |   896313   166786 |   818655        0  0.0430  0.0025  0.0632
  tessfine  |   103052   4.97%     99417   -0.205 |   874141   206110 |   552301        0  0.0379 -0.0379  0.0657
  flat      |   103054   4.97%     99371   -0.202 |   423222    96127 |   148543        0  0.0177 -0.0607  0.0278
  shell     |     4037   0.19%      3753   -0.693 |   213265    46480 |   197180    19748  0.0000 -0.1340  0.0025
  lid       |   145761   7.03%    145761   -5.298 |   199259    41287 |   190511    18882  0.0000 -0.1340  0.0025
  lidraw    |   145761   7.03%    145761   -5.298 |   199260    41287 |   190511    18882  0.0000 -0.1340  0.0025
  probe     |   190743   9.20%      3523   -0.179 |  1045534   281034 |  1664895     1003  0.1011  0.0000  0.1491
  probelid  |   103054   4.97%     21069   -5.128 |  1105538   304225 |  1751746     1002  0.0986  0.0000  0.1491

=== p5780 (t=5780), 81.9 % stone ===
  tess      |   485171  23.40%    458654   -0.498 |   727073   230100 |   679616    90181  0.0000 -0.2098  0.0885
  flat      |   487275  23.50%    462468   -0.498 |   496439   166987 |   532977   152366  0.0000 -0.2578  0.0329
  shell     |   116282   5.61%     93832  -10.967 |   695033   241832 |   932378   515556 -0.0329 -1.5216  0.0000
  lid       |   157642   7.60%    129936   -9.592 |   680209   235052 |   911950   498521 -0.0329 -1.4205  0.0000
  lidraw    |   152881   7.37%    123725   -9.847 |   199182    58006 |   130955    97133  0.0000 -0.1668  0.0051
  probe     |   484383  23.36%    103771   -1.491 |   713921   201222 |  1015922    85788  0.0607 -0.1264  0.1845
  probelid  |   487281  23.50%    132149   -3.842 |   720676   205620 |  1021306    87660  0.0632 -0.1264  0.1870

=== p2845 (t=2845), 100 % stone, ONE wall plane ===
  tess      |      241   0.01%        26   -4.354 |  1212840   430866 |   299961        0  0.0076 -0.0506  0.0581
  flat      |      241   0.01%        31   -0.068 |  1137169   361348 |    83870        0  0.0202 -0.0430  0.0354
  shell     |       78   0.00%        78   -0.962 |   164995    45097 |    84646    13650  0.0000 -0.0227  0.0025
  lid       |    26811   1.29%      2428   -0.391 |   158132    44532 |    84031    13356  0.0000 -0.0177  0.0025
  lidraw    |    27298   1.32%      3104   -0.336 |   127197    41753 |    82733    12640  0.0000  0.0000  0.0025
  probe     |      241   0.01%        13  -29.092 |  1271029   448425 |  1998389    40912  0.1062  0.0556  0.1618

=== p4200 (t=4200) WIDE/VISTA, 91.8 % stone ===
  tess      |   617441  29.78%    582749   -0.495 |   557504    85333 |   143359    13128  0.0152 -0.0329  0.0708
  flat      |   613608  29.59%    582744   -0.511 |   520814    71448 |   121979     1804  0.0177 -0.0354  0.0506
  shell     |    43981   2.12%     27160   -1.115 |   578937   242881 |   726791   611560  0.0000 -2.2040  0.0000
  lid       |    70992   3.42%     27939   -0.938 |   578508   243489 |   725212   612608  0.0000 -2.2192  0.0000
  lidraw    |    67955   3.28%      8459   -1.203 |   223365    79421 |   143129    99912  0.0000 -0.0152  0.0101
  probe     |   604866  29.17%     83098   -0.758 |   583980   108497 |  1281721    53723  0.0910  0.0000  0.1946

=== pmir (t=5780) MIRROR ON SCREEN, 91.7 % stone ===
  tess      |   541431  26.11%    400057   -0.536 |   673090   158149 |   271402     9979  0.0177 -0.0480  0.0784
  flat      |   516705  24.92%    399563   -0.554 |   587516   134640 |   318325     9702  0.0227 -0.0657  0.0657
  shell     |    77222   3.72%     16211   -1.476 |   779455   275041 |   905668   480461 -0.0430 -1.7137  0.0000
  lid       |   121356   5.85%     29431   -2.426 |   778705   276995 |   897349   474846 -0.0430 -1.7162  0.0000
  lidraw    |   114798   5.54%     18209   -4.744 |   219173    78771 |   133467   106603  0.0000 -0.1668  0.0051
  probe     |   517572  24.96%    120213   -0.867 |   765546   218645 |  1408809   159394  0.1365  0.0000  0.2250
```

Reading notes:

- The ~103 k / ~487 k / ~614 k / ~517 k `ownChg` figures shared by `tess`,
  `flat` and `probelid` at a pose are C7 (the reference's boundary model), not
  those arms' error. Compare arms *within* the shell family (`shell`, `lid`,
  `lidraw`, `bconly`) or *within* the non-shell family, and use `refcap2`,
  `refnodisc`, `flatref`, `bconlyref` and `probelid` for the isolated variables.
- `probe` and `probelid` columns are diagnostic only: with the lid offset zero
  the relief hangs entirely below the authored plane, so their `medDz ≈ +0.10`
  and their large `|dz|>0.05` counts are the probe's construction, not a
  candidate look. Their VALUE is the pairwise differences in §C2/§C3/§C6.

---

## 5. THE BLEEDING VERDICT (t=6097)

The user reported "clear discrepancies even not at the edge" and "something that
seems like bleeding from behind the wall".

**What I found — measured.**

1. **Pixels where a surface that is BEHIND won the pixel are essentially
   nonexistent.** Arm `shell` at t=6097: of 4 037 ownership changes, **284 px**
   (0.014 % of the frame) are cases where the arm's owner sits *farther* than the
   reference's. That is not what a viewer would notice.

2. **The visible dark band under the tan wall's edge is 5 546 px** (shell darker
   than the reference by >40 luminance). Resolved per owning FACE:

   | px | reference owner | shell owner | median dz |
   |---|---|---|---|
   | 2 668 | `floor::mirUV` `Piramid.lwo:c19` #0 | **same face** | 0.0000 |
   | 1 670 | `rooms::mirUV` `Piramid.lwo:c50` #0 | **same face** | −0.116 (nearer) |
   | 728 | `rooms` `Piramid.lwo:c19` #2 (plane 0.45,0,0.90 d+48.40) | `rooms::mirUV` `Piramid.lwo:c50` #0 (plane 1,0,0 d−17.90) | **−5.72 (nearer)** |
   | 229 | `rooms` `Piramid.lwo:c19` #2 | `floor::mirUV` `Piramid.lwo:c19` #0 | −0.361 |

   So **4 338 of 5 546 px are the SAME polygon shading wrong** — the march landed
   on the wrong height and sampled a texel that belongs somewhere else, which
   reads exactly like foreign content bleeding in. The remaining 728 px are the
   NEAR wall overwriting the FAR wall — bleeding *forward*, not from behind.

3. **The band is the march.** It is present against `refcap2` — identical
   geometry, identical masks, identical cap, converged march instead of cone-8 —
   on the same 5 546 px.

4. **The horizontal streaks** in the upper-right of the user's crop are the same
   class (C1) and are visible in `flat` too, i.e. they exist without any shell
   geometry at all.

**Verdict:** at t=6097 I find **no population of pixels owned by a surface that
should have been occluded**. What reads as "bleeding from behind the wall" is,
by measurement, (a) the cone march landing on the wrong height and shading a
distant texel on the correct polygon, and (b) 728 px of genuine near-over-far
overwrite at the corner. If the user is pointing at something else in the frame,
**I have not identified it** — the classifier above is exhaustive over ownership
and over >40-luminance darkening, so anything I missed is subtler than either
threshold, and I would need him to point at a pixel.

---

## 6. Cause attribution summary

| class | moved geometry | shading path (march) | tessellation bake |
|---|---|---|---|
| C1 grazing streaks + depth tail | — | **YES** (isolated: same geometry, masks, cap) | — |
| C2 corner interpenetration in-footprint | **YES** (isolated: converged march, 0 colour/depth error) | — | — |
| C3 raw lid over-coverage | **YES** (isolated: `lid` vs `probelid`) | — | — |
| C4 relief only at the lattice | — | — | **YES** |
| C5 no depth relief in flat POM | — | flag configuration | — |
| C6 offscreen (shadows / mirror / probes) | **YES** (isolated: `lid` vs `probelid`; `flat` vs `probelid` = 0) | — | partially (`tess` vs `flat` = 5.32 % of shadow texels) |
| C7 boundary model | definitional | — | definitional |

---

## 7. Reproduction

Scripts and derived data live in the session scratchpad
(`run_d2.sh`, `d2an.py`, `d2plane.py`, `d2tab3.py`, `d2relief.py`, `d2map.py`,
`d2crop.py`, `d2own.py`). The render recipe is in §3; every run's log was grepped
for `unknown flag` / `requires a value` before the numbers were taken (zero hits
across all 60 renders), and the flag arrays are bash arrays, never an unquoted
`$FLAGS` under zsh.

Gate status for this commit (all new flags default OFF, so nothing shipping
moves): see the commit message.

Evidence images: `docs/img/s1_discrepancy/`.

| file | what |
|---|---|
| `6097_A_reference.png` | converged reference, the user's t=6097 framing |
| `6097_B_reference_cap2.png` | converged reference at the shipping cap — C1's control |
| `6097_C_tessellation.png` | `--greets_displace` |
| `6097_D_flatPOM.png` | flat POM, cone-8 |
| `6097_E_shell.png` | shell as shipped-behind-flags (cone-8, cap 2) |
| `6097_F_shell_naive8.png` | shell, naive-8 march |
| `6097_G_lid_masksOFF.png` | raw lid, all masks off |
| `6097_H_probe_zeroLid.png` | the zero-lid diagnostic probe (NOT a candidate look) |
| `6097_M_map_*_vs_ref.png` (+`_crop`) | class maps: RED = drawn nearer than truth, BLUE = drawn farther, GREEN = same owner colour >32/255, YELLOW = same owner depth >0.05 world |
| `6097_M_map_MARCHONLY_shell_vs_converged.png` | C1 with geometry, masks and cap held fixed |
| `5780_*`, `4200_*`, `mir_*`, `2845_*` | full frames, `ref` / `tess` / `shell` at the other four poses |
