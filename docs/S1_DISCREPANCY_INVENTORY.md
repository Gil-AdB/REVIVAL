# S1 — Displacement discrepancy inventory (evidence only)

Status: EVIDENCE DOCUMENT, 2026-08-04, branch `fog-wt`. **No default changes.**
Every flag this campaign added is default OFF. Sections 0–7 are evidence only,
with no fix in them. **§8 is the one exception and says so**: it adds a candidate
fix (`--pom_shell_world_amp`, default OFF) after its audit, and reports the
before/after as a measured TRADE rather than a win. No recommendation is made
about which architecture to adopt — that call is the user's, and this file exists
so it can be made from measurements instead of from a story.

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
| `--pom_shell_census` | §8: per-face amplitude census from `PomShell_Build` **and** from the tessellation bake. Init-time print only. Also, in a `-DFDS_DEV=ON` build, a raster-time histogram of the height-map mip each shell face actually marched |
| `--pom_shell_world_amp`, `--pom_shell_world_amp_set` | §8's candidate: amplitude authored in WORLD units. **Default OFF; this is the one entry in this table that is a proposal, not an instrument.** |
| `--pom_march_earlyout` | §9: break the march once every lane has bracketed. **BYTE-EXACT** — a bracketed lane's step is already forced to 0 and its bracket state frozen. Instrument and candidate at once: it changes no pixel and saves measured ms |
| `--pom_cone_exact=1\|2` | §9.6: exact PER-TEXEL cone bake (EGSR'24) replacing the max-pooled 128² one, disk-cached. 1 = conservative, 2 = relaxed |
| `--pom_cone_min_step` | §9.6: floor on the cone step in lateral TEXELS. Without it an exact cone quantising to byte 0 freezes the march on the flat lid |
| `--pom_march_steps_auto` | §9.7: per-FACE step budget derived from the measured texels-per-step rule instead of one global `--parallax_pom` |

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

> **SUPERSEDED IN PART BY §9 (P1, 2026-08-04).** The class is real and the
> numbers below reproduce, but two of this section's conclusions do not
> generalise past t=6097: "the cap is not the mechanism" is false at the vista
> (§9.1), and the cone map is not under-converged but 10–17× TOO WIDE (§9.2).
> With `--pom_shell_cap=16 --parallax_pom=32 --pom_cone_exact=1
> --pom_cone_min_step=1` (all default OFF) the depth term drops by 250–1 000×
> at every pose — §9.8. **C1 is no longer rank 1 on depth.**

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

---

## 8. P0 — AMPLITUDE SEMANTICS. Why "different parts of the same render moved by different amounts"

Added 2026-08-04, same branch, same build discipline. The user's report this
round: *"it feels like there is a mismatch between different parts of the same
render regarding how much the face moved."* This section is the audit of that,
followed by one default-OFF candidate fix.

### 8.0 The mechanism, from the code

The two paths author the amplitude in **different units**:

| path | where | units | consequence |
|---|---|---|---|
| tessellation bake | `DisplaceStoneSubdiv`: `dsp = amp*(h − mipMean)` along the vertex normal, `amp = --greets_displace_amp` | **WORLD** | one authored number, the same world depth on every face of every material |
| per-pixel shell | `PomShell_Build(… uvAmp = --parallax_strength × Material::ParallaxScale)`; lid offset `= uvAmp × w × 0.5`; the march's `rayScale = uvAmp × 1/(V·N)`; `pomDepthWorldAmp = uvAmp × w` | **UV** | the world depth is `uvAmp × w`, i.e. it follows each face's own world-per-UV `w` |

`Material::ParallaxScale` was tuned as a **UV-offset magnitude** for flat
offset-parallax (the floor's 0.25 carries the comment *"offset-parallax swims on
the grazing, densely-tiled floor → dial it down"*). S1b reused that number as a
**slab depth**. Nobody had ever printed what depth it implies.

### 8.1 Instrument: `--pom_shell_census` (DIAGNOSTIC, default OFF)

Per target face, at build time, from the AUTHORED positions: `|dP/du|`,
`|dP/dv|` (Lengyel, per axis), their geometric mean `w`, the anisotropy
`|dP/du|/|dP/dv|`, the implied world amplitude, the lid offset actually applied
to its vertices, and — grouped by AUTHORED plane (normal dot ≥ 0.9995, |Δd| ≤
2e-3, the same tolerances the patch union-find uses) — how many distinct LID
plane constants that one authored plane became. The same census runs in
`DisplaceStoneSubdiv`. Image-neutral: with the flag on, greets t=6097 is
byte-identical on all four arms (§8.7).

### 8.2 CENSUS — the per-pixel shell (`--pom_shell`, defaults)

```
'rooms'  uvAmp=0.0300   196 faces   51 authored planes
  |dP/du| min/med/max = 6.0000 / 6.0000 / 7.9069
  |dP/dv| min/med/max = 6.0000 / 6.0000 / 8.4853
  w       min/med/max = 6.0000 / 6.0000 / 7.5446   (x1.26)
  ANISOTROPY          = 0.791 / 1.000 / 1.318
  WORLD AMPLITUDE     = 0.1800 / 0.1800 / 0.2263   (x1.26 across the material)
'floor'  uvAmp=0.0750    30 faces    1 authored plane
  |dP/du| min/med/max = 14.6909 / 14.7243 / 14.9001
  |dP/dv| min/med/max = 14.9779 / 15.1875 / 15.2339
  w       min/med/max = 14.8338 / 14.9598 / 15.0661 (x1.02)
  ANISOTROPY          = 0.968 / 0.973 / 0.981
  WORLD AMPLITUDE     = 1.1125 / 1.1220 / 1.1300   (x1.02 across the material)
```

Per authored plane (`[POM-SHELL-CENSUS-PLANE]`, 52 rows; the shape of every row):

| plane | faces | w | world amp | ratio | lid offset | lid planes |
|---|---|---|---|---|---|---|
| `rooms` P17 N=(0,0,1) d=+37.030 | 30 | 6.000..6.000 | 0.1800..0.1800 | **×1.00** | 0.0900 | 1 |
| `rooms` P30 N=(−.447,0,.894) d=+27.601 | 4 | 6.344..6.344 | 0.1903..0.1903 | **×1.00** | 0.0952 | 1 |
| `rooms` P25 N=(−.333,−.667,−.667) | 1 | 7.545..7.545 | 0.2263..0.2263 | ×1.00 | 0.1132 | 1 |
| `rooms` P44 N=(+.759,0,−.651) d=+5.089 | 3 | 6.888..6.888 | 0.2066..0.2066 | ×1.00 | 0.1033 | **2, spread 0.0503** |
| `floor` P00 N=(0,1,0) d=0 | 30 | 14.834..15.066 | 1.1125..1.1300 | ×1.02 | 0.5563..0.5650 | **2, spread 0.0087** |

### 8.3 FINDINGS — and the hypothesis this audit does NOT support

1. **WITHIN one authored plane the world amplitude is CONSTANT.** Every one of
   `rooms`' 51 planes reports ×1.00; `floor`'s single plane reports ×1.02. The
   hypothesis handed to this task — *"faces on the SAME authored surface
   displace by DIFFERENT world distances whenever their UV charts differ in
   scale"* — is **NOT what greets has**. The wall/floor UVs are a world-axis
   PLANAR PROJECTION at a fixed 6 (rooms) / ~15 (floor) world per tile, so the
   density is one number per plane to four decimals. The trapezoid-chart wall
   that defeats the bake's `edgeAlignedQuad` is a *tessellation* problem, not an
   amplitude one.
2. **BETWEEN authored planes of one material: ×1.26** (0.1800 world on the
   axis-aligned walls, up to 0.2263 on the most tilted). Mechanism: the planar
   projection stretches on faces not perpendicular to its axis, `w = 6/cos θ`.
   Real, modest, and a genuine "neighbouring faces moved by different amounts"
   at the corners where an axis-aligned wall meets a tilted one — 0.0900 vs
   0.1132 world of lid offset, a **0.023 world** step.
3. **BETWEEN MATERIALS: ×6.2, and this is the dominant term.** `rooms` 0.180
   world, `floor` 1.113 world — a stone wall and the stone floor it stands on,
   in the same shot, given relief depths that differ by a factor of six. The
   tessellation bake gives both exactly 0.300. Where they meet, the floor's lid
   stands **0.5650** world proud of the authored floor while the wall's lid
   stands **0.0900** proud of the wall: a **0.475 world** step at a junction
   that is one continuous stone surface in the model.
4. **LID PLANES.** 13 of 51 `rooms` planes became more than one lid plane, worst
   spread 0.0503 world (P44/P45); `floor`'s single plane became 2 constants
   spanning 0.0087 world. **The earlier claim of "SIX lid planes 0.0087 world
   apart" on the floor is NOT REPRODUCED** — at the 1e-3 clustering tolerance
   the code itself uses I measure TWO clusters with a 0.0087 total spread.
   **And the mechanism attributed to it is wrong**: on 13 of those 13 `rooms`
   planes the applied lid offset has min == max (0.0900..0.0900), so the split
   is not per-face UV density at all. It is that `PomShell_Build` moves each
   vertex along its **smooth vertex normal**, so at a hard corner the component
   perpendicular to a given face is `off·(N_v·N_f) < off` and that face's plane
   TILTS. Different defect, different fix; §8.6 does not address it.
5. **ANISOTROPY.** `rooms` 0.791..1.318 (median 1.000), `floor` 0.968..0.981.
   A minority of `rooms` faces map the height map's square texels onto world
   rectangles up to 32 % out of square, and since one amplitude serves both
   axes the relief's slope differs by that much between U and V there. Both
   paths sample the height field in UV, so this is shared by the shell and the
   bake and is an authoring property, not a path defect. No plane exceeds ×1.32.
6. **HEIGHT MIP.** Dev-build instrument, t=6097: the shell march sampled
   **mip 0 on 100 %** of shell faces (3 materials, 4 438 faces). The bake
   samples **mip 2** (256²). Two different bands of one height field — already
   §0's point, now measured on the shell side. Pose-dependent: only t=6097 was
   counted.

### 8.4 CENSUS — the tessellation bake. It does NOT share the defect, and has a worse one

`[STONE-CENSUS]`, defaults (`--greets_displace`, amp 0.300, mip 2):

```
'rooms' WORLD amp=0.300 mip=2: 51 planes, per-face w 5.985/6.000/7.545 (x1.26)
'floor' WORLD amp=0.300 mip=2:  1 plane,  per-face w 14.834/14.922/15.065 (x1.02)
  -> the amplitude is the SAME 0.300 world on every face; w only sets the
     relief WAVELENGTH.
```

So the bake's **amplitude** is world-consistent by construction. Its
inconsistency is the relief its LATTICE actually CARRIES — the p05..p95 spread
of the displacement actually applied to its vertices, per authored plane:

| plane | verts | carried relief span | % of amp |
|---|---|---|---|
| `floor` P01 | 9 780 | 0.0318 | **11 %** |
| `rooms` P01/P02/P03/P04 (the four big walls) | 3.4–4.3 k each | 0.1312 | **44 %** |
| `rooms` P10 | 224 | 0.2059 | 69 % |
| `rooms` P33/P36 | 107 | 0.0263–0.0265 | 9 % |
| `rooms` P45/P46 | 66 | 0.0223–0.0274 | 7–9 % |
| `rooms` P43/P44/P48/P49 | 0–1 | 0.0000 | 0 % |

**In the shipping look the floor's stone moves 0.032 world while the wall beside
it moves 0.131 — 4.1×, and the range across planes is 0 % to 69 % of the
authored amplitude.** That is the same complaint the user made, in the arm he
actually looks at, and it is the *opposite sign* to the shell's (there the floor
moves 6.2× MORE than the wall). Cause is C4 (relief only at the lattice), not
amplitude semantics — §8.6 does not fix it either.

### 8.5 The consequence in the render, and in PIXELS

Relief depth actually written to Z, per authored plane, converged reference
(`refUV`, i.e. today's amplitude semantics). Method as §C4 (least-squares 1/z
plane fit, residual span in world units):

| pose | `floor` span | wall spans | floor / wall |
|---|---|---|---|
| t=6097 | 0.2083 | 0.1164, 0.0687 | 1.8–3.0× |
| t=5780 | 0.5382 | 0.1106, 0.1886 | 2.9–4.9× |
| t=4200 (vista) | 0.4635 | 0.0550, 0.1108, 0.1952 | 2.4–8.4× |

Same measurement on `tess`: floor 0.0159–0.0279, walls 0.0029–0.0718 — the two
surfaces sit in the same band. And on `shell` (cone-8) at t=4200 the floor's
span is **1.7144** and at t=5780 **3.4675**: the march's error scales with the
slab it is marching, so the deepest slab in the scene is also where the shipping
march fails worst. That is the link between this section and C1.

**Pixels.** A world displacement along the surface normal shows up as a depth
step; the screen distance you would have to travel *along the surface* to cover
that step is `|Δz| / |∇z|`, both read straight off the depth dump (no focal
length, no camera pose needed). Measured per stone surface:

| what | pose | median | p95 |
|---|---|---|---|
| the floor's surface moves when its amplitude is put on the wall's world scale (`refW03` vs `refUV`) | t=4200 | **16.9 px** | 111 px |
| same, the walls | t=4200 | 7.1 px | 12 px |
| same, floor | t=6097 | 24.7 px | 88 px |
| the cone march's own floor error, current semantics (`shUV` vs `refUV`) | t=4200 | **45.0 px** | 265 px |
| the same march's floor error at one world amplitude (`shW03` vs `refW03`) | t=4200 | **9.3 px** | 33 px |

Crops (identical framing per pose, one transform for every arm):
`docs/img/s1_discrepancy/P0_4200_floor_amp.png` — the wall/floor junction at the
vista, `refUV | refW03 | shUV | shW03`. In the two `UV` panels the floor's
1.11-deep slab undercuts the wall and opens a black gash along the skirting; in
the two `W03` panels the junction is solid. `P0_6097_corner_amp.png` is the
user's corner framing (there the wall's slab GROWS 0.18 → 0.30 and the grazing
march artefacts on the tan wall grow with it). `P0_4200_full_amp.png` is the
whole vista frame, `refUV | refW03`.

### 8.6 CANDIDATE FIX — `--pom_shell_world_amp` (default OFF)

Amplitude authored in **WORLD** units; each patch's UV amplitude derived from
its own world-per-UV, so one authored surface displaces by one world distance
and `pomDepthWorldAmp` is constant across the material.

- `PomShell_Build` picks the material's world amplitude as `uvAmp ×` its
  **area-weighted median** `w` — *derived, not invented*, so the material's
  typical slab depth is unchanged and the A/B isolates the DISTRIBUTION. The
  vertex offset becomes a constant `worldAmp/2` (one authored plane → one lid
  plane, as far as the offset is concerned).
- `--pom_shell_world_amp_set=<world>` forces one amplitude on **every** shelled
  material. `=0.3` puts the shell on the tessellation bake's world depth and is
  what makes the wall and the floor agree.
- Implementation note that matters: the per-patch UV amplitude is published as a
  **build-time table** on the material (`Material::PomShellPatchUvAmp`, indexed
  by `Face::PomShellGroup` — a patch is coplanar by construction, so one number
  is exact for it) and read once per FACE in the dispatcher. The per-triangle
  and per-pixel code is not touched at all. Two earlier variants that moved the
  amplitude into the per-triangle path — a rasterizer member, then a mutated
  `r.ctx` — were both **measured** to shift 5 px by 1/255 at t=6097 through
  inliner/FMA-contraction drift; that is why the table exists.

What it changes (census, `--pom_shell_world_amp_set=0.3`):

| | before (UV) | after (world = 0.3) |
|---|---|---|
| `rooms` world amplitude | 0.1800..0.2263 (**×1.26**) | 0.3000..0.3000 (**×1.00**) |
| `floor` world amplitude | 1.1125..1.1300 | 0.3000..0.3000 |
| wall : floor ratio | **6.2 : 1** | **1 : 1** |
| `rooms` lid offset | 0.0900..0.1132 | 0.0900 flat (at `worldAmp/2` = 0.15 for the 0.3 arm) |
| `floor` authored planes that became >1 lid plane | 1 of 1 | **0 of 1** |
| `rooms` authored planes that became >1 lid plane | 13 of 51 | 13 of 51 — **unchanged**, this is finding 4's smooth-normal mechanism, not amplitude |
| `rooms` per-patch UV amplitude | one 0.03000 for all 67 | 0.03976..0.05000 |
| `floor` per-patch UV amplitude | one 0.07500 for all 6 | 0.01998..0.02019 |

### 8.7 How the comparison was kept honest

Changing the amplitude changes **what the reference renders**, so "error vs the
reference" is meaningless unless the reference is stated. Two separate questions,
never mixed:

1. **What did the amplitude do to the SURFACE?** `refUV` vs `refW` / `refW03` —
   the converged march (`--pom_ref_march --pom_ref_steps=512`,
   `--pom_shell_cap=64`) on BOTH sides, so the march is not a variable. This
   pair *is* the honest statement that the reference moved.
2. **Did the shipping march get better or worse?** Each arm is scored against
   the reference built with **its own** semantics — `shUV` vs `refUV`, `shW` vs
   `refW`, `shW03` vs `refW03`. Never `shUV` vs `refW`.

A third honesty property is structural: `[POM-SHELL-WORLDAMP]` reports that
**114 of 196 `rooms` faces have `w == wRef` exactly**, so under the per-material
derivation their surface is unchanged *by construction* and the whole difference
is confined to the faces the fix targets. Measured, t=4200, `refW` vs `refUV`:
`rooms` and `rooms::mirUV` move 0.00 px, `floor::mirUV` moves 0.14 px median.

**Results.**

(1) SURFACE, converged march both sides:

| pair | pose | colour >12 | colour >32 | \|dz\|>0.05 | \|dz\|>0.20 |
|---|---|---|---|---|---|
| `refW` vs `refUV` (per-material) | t=6097 | 69 354 | 9 946 | 1 420 | 53 |
| `refW` vs `refUV` | t=4200 | 97 949 | 26 969 | 8 878 | 2 155 |
| `refW03` vs `refUV` (one world amp) | t=6097 | 707 174 | 171 886 | 151 177 | 23 186 |
| `refW03` vs `refUV` | t=4200 | 963 468 | 311 638 | 643 952 | 531 773 |

(2) MARCH, each arm vs the reference of its own semantics:

| arm pair | pose | colour >12 | colour >32 | \|dz\|>0.05 | \|dz\|>0.20 | floor dz med | floor shift med |
|---|---|---|---|---|---|---|---|
| `shUV` vs `refUV` | t=4200 | 621 298 | 275 762 | 757 117 | 637 643 | **−1.019** | **45.0 px** |
| `shW` vs `refW` | t=4200 | 621 299 | 275 337 | 757 306 | 637 819 | −1.024 | 45.2 px |
| `shW03` vs `refW03` | t=4200 | **589 092** | **243 627** | 771 777 | **441 522** | **−0.245** | **9.3 px** |
| `shUV` vs `refUV` | t=6097 | 217 217 | 49 510 | 201 217 | 23 431 | 0.000 | 0.29 px |
| `shW` vs `refW` | t=6097 | 217 016 | 49 339 | 201 199 | 23 274 | 0.000 | 0.30 px |
| `shW03` vs `refW03` | t=6097 | 246 355 | 48 681 | 216 971 | **105 936** | 0.000 | 0.00 px |

**Read it as a trade, not a win.** One world amplitude at 0.3 cuts the floor's
march error by 4.1× (45.0 → 9.3 px median at the vista, 265 → 33 px p95) and
takes 196 k pixels off the vista's `|dz|>0.20` count, because the cone march's
absolute error scales with the slab it marches and the floor's slab shrinks
3.7×. It makes the WALLS worse by the same mechanism in reverse — their slab
grows 1.67×, and at t=6097 `|dz|>0.20` goes 23 431 → 105 936. The per-material
derivation (`--pom_shell_world_amp` alone) is nearly inert by design: it only
removes the ×1.26 between tilted and axis-aligned wall planes.

### 8.8 Gates for this section's commit

`--pom_shell_world_amp`, `--pom_shell_world_amp_set` and `--pom_shell_census`
all default OFF, and `--pom_shell` itself is default OFF.

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (mirrortest / conetest / halotest) |
| city `t=1961` | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact |
| greets `shell` cone-8 t=6097 | `193427ccb28163705ea6baa5500afd0c` — byte-exact vs the pre-change binary (this is §2's own pin) |
| greets `shell` naive-8 t=6097 | `13dcf8e54416816a29ba90f0fe468756` — byte-exact |
| greets `tess` t=6097 | `3f86c73cc7ed8f0ad8f57b12984537d0` — byte-exact |
| greets `flat` t=6097 | `9d095fbcac0c00888578d56172786997` — byte-exact |

The four greets hashes were taken by `git stash`-ing this section's source
changes, rebuilding, rendering, unstashing, rebuilding and rendering again —
not by trusting that the code "looks inert". That A/B is what caught the 5-px
1-LSB drift of the two rejected implementations.

### 8.9 Verdict on the user's observation

There **is** a real mismatch in how far different parts of the render move, and
it is measured; but it is not per-face UV charts within one surface (finding 1).
It is:

- **shell / per-pixel path:** the floor's relief is **6.2× deeper in world units
  than the wall's** (1.113 vs 0.180), because one number tuned as a UV offset
  was reused as a slab depth on surfaces whose UV tiles are 15 and 6 world units
  wide. At the junction the two lids are 0.475 world apart. Between wall planes
  the same mechanism gives a further ×1.26 (0.023 world at a corner).
- **tessellation / the shipping look:** the amplitude is world-consistent, but
  the relief the lattice CARRIES ranges from **0 % to 69 % of it** across
  authored planes — floor 11 %, main walls 44 %. That is a 4.1× floor↔wall
  mismatch in the arm the user actually watches, and it is C4, not this section.

Both are "different parts of the same render moved by different amounts", they
point in **opposite directions**, and no single flag fixes both.
`--pom_shell_world_amp_set=0.3` fixes the first and puts the per-pixel path on
the bake's world scale; C4 remains open.

---

## 9. P1 — C1 ATTACKED. What the grazing streaks and the depth tail actually are

Added 2026-08-04, same branch, same build discipline, same reference definition.
This section supersedes two claims made earlier in this file — both are marked
CORRECTED below — and it ends with a candidate recipe whose every flag is
default OFF.

### 9.0 Method, and the one comparison rule that decides everything

Same converged reference as §1 (`--pom_ref_march --pom_ref_steps=512
--pom_shell_cap=64`), and §8.7's rule applied without exception: **an arm is
only ever scored against the reference built with its own amplitude
semantics.** Every amplitude arm in this section has its own 512-step
reference; `refUV` scores the UV arms, `refW018` the 0.18-world arms.

One thing to hold on to while reading: **the reference marches the TRUE view
ray** (`--pom_shell_cap=64` is the kernel's uncapped limit, and §1 states why a
reference must not have a grazing hack in it). An arm rendered at
`--pom_shell_cap=2` is therefore being scored against the true ray, which is
exactly the question "is this surface where the relief actually is". That is
not a stacked comparison — it is the comparison — but it means the cap enters
these numbers, and §4's C1 table (which compared `shell` against `refcap2`,
cap 2 on both sides) deliberately excluded it. Both are honest; they answer
different questions, and the difference between them turns out to be most of
the class.

125 headless renders, `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, sequential,
every log grepped for `unknown flag` / `requires a value` (zero hits), flags
passed as bash ARRAYS. Scripts: `p1_sweep.sh`, `p1_ctrl.sh`, `p1_cap.sh`,
`p1_exact.sh`, `p1_rule.sh`, `p1_final.sh`, `p1_auto.sh`, `p1_bench.sh`,
`p1_an.py` in the session scratchpad.

### 9.1 CORRECTION 1 — the grazing cap IS the lever at the vista

§4 says "**The cap is not the mechanism** … the `--pom_shell_cap` dial does
essentially nothing at these poses" on the strength of a 4-px measurement at
t=6097. That measurement reproduces exactly. **It does not generalise**, and the
vista pose — where the 0.945-world floor error lives — is where it fails.

Controls at t=4200, every arm UV amplitude, all scored against `refUV`
(floor shift = the median distance, in screen pixels, you would travel along
the surface to cover the depth error; it is amplitude-neutral, which the
world-unit numbers are not):

| arm | what it isolates | colour >12/255 | \|dz\|>0.05 | floor shift |
|---|---|---|---|---|
| cone-8, cap 2 (today's `shell`) | — | 621 446 | 757 117 | **45.00 px** |
| cone-64, cap 2 | 8× the step budget | 602 271 | 728 110 | 42.33 px |
| naive-256, cap 2 (**no cone map at all**) | the cone map | 603 141 | 726 983 | 42.50 px |
| cone-8, relax 1 (¼ the cone width) | cone width | 666 831 | 833 232 | 55.73 px |
| cone-8, relax 16 (4× the cone width) | cone width | 608 677 | 734 240 | 42.25 px |
| cone-8, refine 0 / refine 12 | bisection count | 634 245 / 620 046 | 754 401 / 757 613 | 42.44 / 45.13 px |
| **cone-8, cap 64** | **the cap** | **226 432** | **145 280** | **0.21 px** |

**A 256-step uniform march with no cone map is no better than 8 cone steps, and
removing the cap fixes the floor with 8 steps.** The 0.945-world floor error at
the vista is the march running a ray that is *5–8× less oblique than the real
one*: `--pom_shell_cap=2` bounds `1/(V·N)` at 2 (60° off the normal) while the
vista floor is seen at ~85°. The march then lands on the right height of the
wrong ray.

### 9.2 CORRECTION 2 — the shipping cone map is 10–17× too WIDE, and is nearly inert

Measured cone-ratio means at mip 0 (`FDS_CONE_HIST`, decoded through each bake's
own encode ceiling):

| material | legacy `MakeConeMap` | exact conservative | exact relaxed | legacy / exact |
|---|---|---|---|---|
| `floor` | 0.6055 | 0.0557 | 0.0716 | **10.9×** |
| `rooms` | 1.5231 | 0.0922 | 0.1067 | **16.5×** |

Mechanism, from the code: `MakeConeMap` max-pools the height field to a 128²
grid before computing cones. The max over an 8×8 block of a stone height map is
near 255 almost everywhere, so hardly any coarse cell has a TALLER cell left to
be bounded by, and the cone comes out enormous; and lateral distance is
quantised to 1/128 UV, so a texel one texel from a cliff is told the cliff is
eight texels away. The march's competing quantity is `dlen = uvAmp × tan θ`,
at most ~0.7 at the cap, so a cone of 1.5 means `c·gap/(c+dlen)` is a near-full
gap every step: **the cone map barely steers the march**. That is why
`--parallax_pom_relax` at 1, 4 and 16 moves the t=6097 error by under 5 % —
the dial is multiplying something that was already saturated.

(The commit message for `774a9cb` quotes this as "6.6× wider". That paired the
two histogram blocks to the wrong materials; the correct figures are the table
above. The direction and the mechanism are unchanged.)

### 9.3 The AMPLITUDE × STEP-BUDGET error surface (P1 task 1)

Every cell scored against the 512-step reference of its own amplitude. Cap held
at the shipping 2 so this table is comparable to §8's.

**t=4200, the vista.** `floorShift` is the amplitude-neutral column:

| world amp | N=8 | N=16 | N=32 | N=64 | floor shift / unit world amp |
|---|---|---|---|---|---|
| UV (floor 1.113, wall 0.180–0.226) | 45.00 px | 42.37 | 42.33 | 42.33 | 38.0 |
| 0.15 everywhere | 4.62 px | 4.61 | 4.61 | 4.61 | 30.8 |
| **0.18 everywhere** | **5.52 px** | 5.50 | 5.50 | 5.50 | 30.6 |
| 0.20 everywhere | 6.14 px | 6.12 | 6.12 | 6.12 | 30.7 |
| 0.30 everywhere | 9.27 px | 9.19 | 9.19 | 9.19 | 30.9 |

**The march's floor error is exactly proportional to the slab it marches** —
30.6–30.9 px per unit world amplitude across a 2× amplitude range, four
significant figures of linearity. The UV row sits above the line (38.0) because
its slab is 6× deeper still and the budget starts to bite as well.

**The `--pom_shell_world_amp_set=0.18` hypothesis is confirmed as a near-pure
win at the cap the shipping arm uses.** At t=4200, N=8: floor 45.00 → 5.52 px
(**8.2×**), walls 0.75 → 0.77 px (unchanged — 0.18 *is* the axis-aligned walls'
existing depth, and the only wall faces that move are the tilted planes coming
down from 0.226). At t=6097 the wall column is identical to four decimals.

**t=6097, the corner.** Here the amplitude does little and the STEP BUDGET is
the whole story (cap 2, since the cap does nothing at this pose):

| world amp | metric | N=8 | N=16 | N=32 | N=64 |
|---|---|---|---|---|---|
| UV | colour >12 | 217 217 | 96 896 | 95 623 | 92 362 |
| UV | \|dz\|>0.05 | 201 217 | 15 307 | 13 509 | 10 009 |
| 0.18 | colour >12 | 202 452 | 86 281 | 84 973 | 81 713 |
| 0.18 | \|dz\|>0.05 | 194 794 | 14 294 | 12 506 | 9 006 |

**Going from the shipping 8 steps to 16 cuts the frame's colour error in half
and its depth error by 13×.** Nobody had swept this.

A trap worth recording: the `|dz|>0.20` column is not comparable across
amplitudes. A march that fails completely lands on the lid, so its worst
possible error is one slab; at 0.15 world a failure *cannot* cross a 0.20
threshold and the count collapses for arithmetic reasons, not quality ones.
Use `|dz|>0.05` and the pixel-shift column.

### 9.4 The CAP × STEPS × AMPLITUDE surface — the three levers together

t=4200, UV amplitude, scored against `refUV`:

| cap | N=8 | N=32 | N=64 | floor shift @N=32 |
|---|---|---|---|---|
| 2 | 757 117 | 728 184 | 728 110 | 42.33 px |
| 4 | 309 448 | 231 740 | 231 662 | 0.47 px |
| 8 | 152 814 | 60 715 | 60 631 | 0.15 px |
| 16 | 145 280 | **52 829** | 52 745 | 0.14 px |
| 64 | 145 280 | 52 829 | 52 745 | 0.14 px |

(cells are `|dz|>0.05`.) At 0.18 world amplitude the same table bottoms out at
**2 909** px. At t=6097 the cap column is flat to 4 px — §4's finding, intact.

So the three levers are **orthogonal and pose-selective**: the cap owns the
vista/floor, the step budget owns the walls, the amplitude scales both.

### 9.5 The STEP RULE, in texels per step (P1 task 1's deliverable)

What a march has to resolve is not "a slab" and not "an angle" — it is
**texels of lateral travel down the slab**:

```
T  =  uvAmp × texelsPerUVtile × tan(incidence)
   =  worldAmp × texelsPerUVtile × tan(incidence) / worldPerUVtile
```

with `tan(incidence) ≤ sqrt(cap² − 1)` because `--pom_shell_cap` bounds
`1/(V·N)` directly. For greets at mip 0 (1024 texels/tile), cap 16:

| surface | uvAmp | worst-case T | N at 16 texels/step |
|---|---|---|---|
| `rooms` (walls) | 0.030 | 491 texels | 31 |
| `floor`, UV amplitude | 0.075 | 1 226 texels | 77 |
| `floor`, 0.18 world | 0.012 | 196 texels | 13 |

`--pom_cone_min_step` is a direct dial on texels-per-step, so sweeping it
against N measures the rule in exactly those units. t=6097, exact cone bake,
cap 16, UV amplitude, cells are `|dz|>0.05` against `refUV`:

| texels/step \ N | 8 | 16 | 32 | 64 |
|---|---|---|---|---|
| 0.25 | 55 098 | 5 779 | 721 | 721 |
| 0.5 | 47 592 | 3 680 | 739 | 739 |
| 1 | 36 742 | 945 | 761 | 761 |
| 2 | 20 838 | 846 | 846 | 846 |
| 4 | 14 816 | 949 | 949 | 949 |
| 8 | 14 594 | 1 398 | 1 398 | 1 398 |

and t=4200:

| texels/step \ N | 8 | 16 | 32 | 64 |
|---|---|---|---|---|
| 0.25 | 420 835 | 59 073 | 11 727 | 10 508 |
| 1 | 361 985 | 42 558 | 11 014 | 10 611 |
| 4 | 277 675 | 24 448 | 13 892 | 13 889 |
| 8 | 205 729 | 23 853 | 21 170 | 21 170 |

**Read it as: the budget N is the binding constraint until N × (texels per
step) covers T, and past that point the minimum step is irrelevant** (the
N=32 and N=64 columns are flat in texels/step, and identical to each other).
Below the knee, forcing a bigger step buys coverage at the cost of resolution —
the N=8 column improves 3.8× as the forced step grows, and still never reaches
the N=32 level.

The knee sits at **T / N ≈ 16 texels per step** for the walls (T=491, knee at
N≈31) and **≈ 38** for the floor (T=1 226, knee at N≈32, only 4 % left on the
table at N=64 — grazing floor rays hit dense relief long before they travel
their worst-case T). 16 texels/step is the conservative end of that measured
range and is the number `--pom_march_steps_auto` is quoted at below.

This is the rule the task asked for, and it replaces a hand-set global: it
scales with the slab (uvAmp), with the map resolution (so a face marching a
higher height mip needs proportionally fewer steps), and with the incidence
bound the cap already imposes.

### 9.6 EXACT PER-TEXEL CONE BAKE (P1 task 2)

`--pom_cone_exact=1|2` (default 0 = OFF), after Bán/Valasek/Bálint/Vad,
"Robust Cone Step Mapping", EGSR 2024 (github.com/Bundas102/robust-cone-map),
with **one deliberate deviation, stated because it changes the construction**:
the paper's cone is exact for a *bilinearly interpolated* height field, and
ours point-samples (`roundi` → nearest texel), so the surface the ray actually
intersects is piecewise CONSTANT over texel cells. Distance is therefore
measured to the nearest point of a texel's **cell**, not to its centre — at
r=1 that is a factor-of-two difference on the tightest constraint there is —
and the paper's falling-edge (limiting-vertex) prune, which is a statement
about bilinear patches, is replaced by the band bound (`every texel on ring r
is ≥ (r−½)/size away and at most (1 − h) taller`), which is exact for this
surface and prunes just as hard. Quantisation is by **truncation**, never
rounding: a cone rounded up is precisely what makes a march skip geometry.
Mode 1 = conservative, mode 2 = relaxed (Policarpo's construction: the cone may
penetrate the field and is bounded by where the ray *leaves* it again).

Disk-cached under `Runtime/cache/` on the S1c horizon-map pattern, keyed on the
height field's mip-0 bytes + mode + encode ceiling + scan radius.

**Bake cost, 1024², threaded, cold:** conservative **1 921 ms** (`floor`) /
**1 927 ms** (`rooms`); relaxed **8 407 ms** / **5 027 ms**. Both then load from
a 1 398 096-byte file per material. The relaxed bake is 2.6–4.4× the
conservative one and, as the table below shows, does not earn it.

t=6097, cap 16, UV amplitude, all against `refUV`:

| arm | colour >12 | colour >32 | \|dz\|>0.05 | \|dz\|>0.20 |
|---|---|---|---|---|
| legacy bake, N=8 | 217 214 | 49 506 | 201 213 | 23 427 |
| legacy bake, N=32 | 95 620 | 20 764 | 13 505 | 12 300 |
| legacy bake, N=64 | 92 359 | 19 405 | 10 005 | 8 851 |
| exact cons., N=8, **no min step** | 439 104 | 126 662 | 564 961 | 93 087 |
| exact cons., N=32, no min step | 92 268 | 24 280 | 28 190 | 18 258 |
| exact relaxed, N=8, no min step | 280 945 | 78 154 | 304 772 | 70 862 |
| exact relaxed, N=32, no min step | 90 984 | 20 633 | 15 673 | 12 316 |
| exact cons., N=8, **min step 1 texel** | 96 742 | 30 451 | 36 742 | 25 876 |
| **exact cons., N=32, min step 1 texel** | **70 407** | **12 332** | **761** | **320** |

Three things this says, in order of importance:

1. **The exact bake is useless without `--pom_cone_min_step`, and excellent
   with it.** A true per-texel cone next to a cliff is legitimately smaller than
   the byte quantum, truncates to 0, and `c·gap/(c+dlen)` is then exactly ZERO:
   **the march freezes in place**, never brackets, keeps the un-shifted entry
   UV, and for the shell that renders the flat lid — up to a full slab in front
   of the true surface. That is the 439 104 row, and it is the same failure the
   legacy bake never had only because its cones were 16× too wide to ever
   quantise to zero. Flooring the step at one lateral texel removes it.
2. **With the floor in place, the exact bake beats the legacy bake by 17.7× on
   depth at equal step count** (761 vs 13 505 at N=32) and by 13.1× against the
   legacy bake at *eight times* the steps (761 vs 10 005 at N=64). At N=8 it
   already matches the legacy bake's N=32 colour (96 742 vs 95 620).
3. **The relaxed bake (mode 2) is not worth its 2.6–4.4× bake cost here.** It
   is 1.16–1.29× wider than the conservative one, and at N=32 it lands at
   15 673 — worse than conservative+min-step's 761. `--parallax_pom_relax=4` on
   top of an exact conservative cone reaches the same place for free.
   `--parallax_pom_relax=1` is much worse at every budget (313 077 colour at
   N=32): the exact cone alone genuinely does need widening.

Secant refinement was **not** added: the existing `--parallax_pom_refine` is a
6-iteration bisection on the same bracket, and at the settings above the
bisection count is already off the critical path (refine 0 vs 6 vs 12 at the
vista: 754 401 / 757 117 / 757 613 `|dz|>0.05` — a 0.4 % spread). The converged
reference already carries a secant solve; adding one to the shipping march
would be optimising a term measured not to matter.

### 9.7 PER-FACE STEP BUDGET FROM THE RULE (P1 task 3)

`--pom_march_steps_auto=<texels per step>` (default 0 = off) computes §9.5's
rule per FACE in the dispatcher and uses the result instead of
`--parallax_pom`'s global number. The per-triangle and per-pixel paths are not
touched, which is what keeps flag-off byte-exact (§8.6 records the two earlier
variants that moved work into those paths and drifted 5 px by 1/255).

At `=16` it derives N=31 for `rooms`, N=77 for the UV-amplitude `floor`, and
N=13 for the floor once `--pom_shell_world_amp_set=0.18` shrinks its slab —
the budget following the surface, and shrinking when the amplitude fix removes
the reason for it.

Quality against the hand-set N=32 (both with cap 16, exact bake, min step 1,
`|dz|>0.05` / colour >12 against the matching reference):

| pose | fixed N=32 | auto @16 tex/step | fixed N=32 + 0.18 | auto + 0.18 |
|---|---|---|---|---|
| t=6097 | 761 / 70 407 | **761 / 70 407** | 406 / 63 562 | 406 / 63 562 |
| t=5780 | 11 378 / 105 496 | **10 393 / 102 260** | 4 140 / 67 559 | 5 045 / 67 708 |
| t=2845 | 95 / 46 532 | **95 / 46 532** | 95 / 47 119 | 95 / 47 119 |
| t=4200 | 11 014 / 113 302 | **10 837 / 114 601** | 740 / 56 188 | 1 096 / 55 086 |

Equal to the hand-set budget within noise at three poses and slightly better at
two, while spending 31 steps on a wall instead of 32 and 77 on the floor
instead of 32. The one place it is measurably *worse* is `auto + 0.18` at the
vista (1 096 vs 740): with the floor's slab shrunk to 0.18 the rule asks for
only 13 steps, and that is the rule spending less than a hand-set 32 would.
Whether that trade is right is a look question, not a metric one.

### 9.8 BEFORE / AFTER, all four poses

`BASE` = today's shipping shell arm (`--pom_shell --parallax_pom_cone
--pom_shell_cap=2`, 8 steps, legacy cone bake).
`BEST` = `--pom_shell_cap=16 --parallax_pom=32 --pom_cone_exact=1
--pom_cone_min_step=1 --pom_march_earlyout`.
`+AMP` adds `--pom_shell_world_amp --pom_shell_world_amp_set=0.18` and is
scored against `refW018`. **Every flag involved is default OFF.**

```
pose  arm                            colour>12  colour>32  |dz|>.05  |dz|>.20   floor medDz  floor shift  wall shift
t=6097 BASE                             217217      49510    201217     23431       +0.0000      0.29 px     0.00 px
       AMP only (0.18)                  202452      42041    194794     17798       +0.0000      0.00 px     0.00 px
       BEST                              70407      12332       761       320       +0.0000      0.00 px     0.00 px
       BEST+AMP                          63562       8960       406       306       +0.0000      0.00 px     0.00 px
t=5780 BASE                             809291     344168   1026343    609121       -0.6091     29.82 px     0.49 px
       AMP only                         754858     311360    904568    411187       -0.0935      4.20 px     0.40 px
       BEST                             105496      26802     11378      5671       +0.0000      0.00 px     0.00 px
       BEST+AMP                          67559      16158      4140      2626       +0.0000      0.00 px     0.00 px
t=2845 BASE                             165052      45136     84724     13728        (no floor in frame)     0.00 px
       AMP only                         168250      46433     84676     13664                               0.00 px
       BEST                              46532       9595        95        13                               0.00 px
       BEST+AMP                          47119       9712        95        13                               0.00 px
t=4200 BASE                             621446     275663    757117    637643       -1.0186     45.00 px     0.75 px
 VISTA AMP only                         548793     225862    714862    239746       -0.1441      5.52 px     0.77 px
       BEST                             113302      31361     11014      3163       +0.0000      0.00 px     0.00 px
       BEST+AMP                          56188      17033       740       195       +0.0000      0.00 px     0.00 px
```

Against the acceptance target:

| target | result |
|---|---|
| C1 **depth** error down ≥5× at the vista | `\|dz\|>0.05` 757 117 → **740** (**1 023×**); the floor's median signed error −1.0186 → **0.0000** world; median floor shift 45.00 px → **0.00 px**. **Hit, by three orders of magnitude.** |
| no regression at the wall poses | t=6097 depth 201 217 → 406 (496×), colour 217 217 → 63 562 (3.4×). t=5780 depth 1 026 343 → 4 140 (248×), colour 809 291 → 67 559 (12.0×). t=2845 depth 84 724 → 95 (892×), colour 165 052 → 46 532 (3.5×). **No pose regresses on any column.** |

Two caveats on those numbers, stated so they are not over-read:

- t=5780's `BASE` row (809 291 colour) is far above §4's C1 figure for the same
  arm (181 304). §4 scored `shell` against `refcap2` — cap 2 on both sides —
  and this section scores it against the true-ray reference. The gap between
  the two IS §9.1's cap term. Neither number is wrong; they answer different
  questions, and this section's is the one that says where the surface is.
- **Colour is improving far less than depth** (3.4–12× vs 250–1 000×). The
  residual 46 k–113 k colour pixels are on the dithered grazing mortar bands,
  which §1 already flagged as the one place the reference itself is only
  converged to ±1 step. Do not read the colour column as "still broken" without
  looking at where it lives.

### 9.9 PERF

Recipe: `./DEMO --bench=scene@scene=greets,t=5780,iters=60 --deferred
--profiler=0`, five arms **interleaved** (A B C D E, repeated ×4) so thermal
drift hits every arm equally, headless dummy drivers, load-guarded (the script
refuses to start with another `DEMO` up; nothing else was running). t=5780 is
the worst pose — walls fill the frame. Deltas are computed **per rep and then
averaged**, so the ± is the spread of the paired difference, not of the arm.

| arm | flags added to `--pom_shell --parallax_pom_cone --no-greets_displace` | mean ms/iter (4 reps) | run-to-run spread |
|---|---|---|---|
| A `BASE` | `--pom_shell_cap=2` (8 steps, legacy bake) | **58.13** | 0.97 ms |
| B | A `+ --pom_march_earlyout` | **55.96** | 1.10 ms |
| C | `--pom_shell_cap=16 --parallax_pom=32 --pom_cone_exact=1 --pom_cone_min_step=1` | **76.42** | 0.95 ms |
| D `BEST` | C `+ --pom_march_earlyout` | **59.52** | 1.20 ms |
| E `AUTO` | D but `--pom_march_steps_auto=16` instead of `--parallax_pom=32` | **58.82** | 0.81 ms |

| paired delta | ms | sd over 4 reps |
|---|---|---|
| **early-out on today's 8-step arm** (B − A) | **−2.17** | 0.09 |
| **early-out on the 32-step exact arm** (C − D) | **−16.90** | 0.25 |
| BEST vs BASE, no early-out (C − A) | +18.29 | 0.45 |
| **BEST vs BASE, with early-out** (D − A) | **+1.39** | 0.48 |
| **AUTO vs BASE, with early-out** (E − A) | **+0.69** | 0.51 |

Read honestly:

- **The whole recipe costs +1.39 ms** at the worst pose — 2.4 % of a 58 ms
  frame — for the error reductions in §9.8. That is not "tiny"; on a 58 ms
  budget 1.4 ms is real and someone has to decide it is worth buying.
- **The early-out is what makes it affordable, and it is byte-exact.** Without
  it the same quality costs +18.29 ms. It also takes **2.17 ± 0.09 ms off the
  arm that ships today**, changing nothing about the image — the cheapest
  measured win in this section, and the one that needs no look review.
- **The per-face budget (E) is 0.69 ± 0.51 ms over BASE**, i.e. **0.70 ± 0.24 ms
  cheaper than the hand-set 32** while matching or beating its quality at three
  of four poses (§9.7). The ± is comparable to the number, so state it as
  "within about ±0.5 ms of free relative to BASE", not as zero.

**Init cost.** The exact bake is 1 921 / 1 927 ms per 1024² material COLD, then
written to `Runtime/cache/` (1 398 096 B each). Warm, the whole
`LoadOrBakeConeMapExact` call is **1 ms per material** — against the legacy
`MakeConeMap`'s **240 ms (`floor`) + 151 ms (`rooms`)** every launch. So on any
run after the first, the exact bake is **389 ms FASTER to initialise** than the
map it replaces.

### 9.10 Gates

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (mirrortest / conetest / halotest) |
| city `t=1961` colour | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact |
| greets `shell` cone-8 t=6097 | `193427ccb28163705ea6baa5500afd0c` — byte-exact |
| greets `shell` naive-8 t=6097 | `13dcf8e54416816a29ba90f0fe468756` — byte-exact |
| greets `tess` t=6097 | `3f86c73cc7ed8f0ad8f57b12984537d0` — byte-exact |
| greets `flat` t=6097 | `9d095fbcac0c00888578d56172786997` — byte-exact |
| wasm | `cmake --build build-wasm` **links clean** (118/118, `DEMO.wasm` + `DEMO_snapshot.wasm`) — `Mekalele.h` is a shared kernel, so this was required |

Taken by `git stash`-ing this section's source changes, rebuilding, rendering,
unstashing, rebuilding and rendering again — not by trusting that the code looks
inert. That A/B is what proves the `ctx.coneUnit` hoist (which replaced a
constant-folded expression in the innermost march loop) did not drift.

A second, independent check: `--pom_march_steps_auto` landed after most of this
section's renders, so three arms captured with the earlier binary
(`c8UV`, `k16c32UV`, `e1r4m1c32UV` at t=6097) were re-rendered with the final
one and are **md5-identical**, which validates the whole earlier data set rather
than assuming it.

`--pom_march_earlyout`'s byte-exactness also showed up incidentally in the
bench: the with- and without-early-out arms print identical
`[GREETS-CENTROID post-bench]` masses to the last digit.

**Note on §8.8's city pin.** It is the md5 of `city_t001961_color.ppm` alone.
A gate script that concatenates the colour PPM *and* the `z.pgm` dump produces
`7438016d804807154892c019b8b69c73`, which is not a regression — it is a
different hash of a different set of bytes. Recorded here because it cost a
stash-and-rebuild to establish.

### 9.11 Verdict — is C1 small enough that the geometry fork decides?

**On depth, yes, decisively.** C1's depth term is what made the per-pixel shell
unusable as a surface: a floor most of a slab in front of where the relief is.
It is now 740–4 140 px frame-wide at the two poses where it was 757 k and
1 026 k, and 95 px at t=2845. For comparison, §C3 measures the *raw lid
over-coverage* at 143 835 px and §C7 the reference's own boundary model at the
same order. **The march's depth error is now two to three orders of magnitude
below the geometry classes it was competing with**, so lid-vs-recess-vs-border-
taper is no longer being decided against a moving surface.

**On colour, partly — and now measured rather than guessed.** 46 k–113 k pixels
still differ by >12/255. §1 warns that the reference itself is only converged to
±1 march step on the dithered grazing mortar bands, so I rendered
`--pom_ref_steps=1024` at both poses and intersected the masks:

| pose | reference 512-vs-1024 disagrees | arm | residual >12/255 | of it, inside the reference's own band | **outside the band** |
|---|---|---|---|---|---|
| t=6097 | 49 570 px | BASE | 217 217 | 23 654 (10.9 %) | 193 563 |
| | | **BEST** | 70 407 | 22 281 (**31.6 %**) | **48 126** |
| t=4200 | 81 066 px | BASE | 621 446 | 63 843 (10.3 %) | 557 603 |
| | | **BEST** | 113 302 | 45 559 (**40.2 %**) | **67 743** |

So **a third to two fifths of what is left is the reference's own uncertainty,
not the arm's error** — and the honest, attributable colour residual is 48 126
px at t=6097 (**4.0×** better than today's 193 563) and 67 743 at the vista
(**8.2×** better than 557 603). Those remain the largest per-pixel-path numbers
in the inventory and I have not resolved what they are beyond "grazing mortar
bands"; the reference cannot be pushed much further without a supersampled
capture, which §1 excluded on purpose.

**What this section does NOT settle.** C2 (17 326 px of corner
interpenetration), C3 (143 835 px of raw lid over-coverage), C6 (30 % of the
shadow cube, 10 370 px of non-stone pixels moved) and C7 (the boundary model)
are all untouched by anything here — they are properties of the moved geometry
and of the shell's boundary definition, and every one of them is now LARGER
than C1's depth term. If the campaign is ranking by size, the ranking in §4 is
now stale: **C1 has dropped from rank 1 to below C3, C6 and C7 on depth.**

---

## 10. P2-A — RECESS-ONLY DISPLACEMENT. The hole class, and a model that cannot have it

Added 2026-08-05, same branch, same build discipline. **`--pom_recess_only`
(+ `--pom_recess_edge`) is new and DEFAULT OFF**, like every other flag in this
file. This section exists because the user ran P1's recommended shell recipe and
got **BLACK HOLES** — full-height gashes between wall panels and a black bar
inside the mirror — and **no metric in §§1–9 could see them**: the converged
reference shares the shell's open-patch-boundary model (C7), so it scores those
holes as ZERO error. Every P1 number was measured against a yardstick blind to
this artefact.

**So VOID is now a mandatory column on every arm.** Void = pixels the arm left
with `z == 0`, i.e. nothing rasterised at all.

### 10.1 THE VOID TABLE (the headline)

**The poses, and one honesty note about them.** The task named seven review
poses and said the list was in the user's report and the progress files; I could
not find such a list anywhere in `docs/`, the scratchpad or the worktrees, so I
used the primary pose it DID give plus the campaign's five standards plus a
second mirror capture: `p5743` (t=5743,
`FDS_GREETS_CAM="9.07557869,3.19592357,-52.9277191,-0.20672597,-0.140846997,0.968207836"`
— the user's own camera, the primary), `p6097`, `p5780`, `p2845`, `p4200`,
`pmir` (t=5780 + `--greets-mirror-cam`) and `pmir5743` (t=5743 +
`--greets-mirror-cam`). If the real seven differ, the void column is one script
away for any pose.

1080p, 2 073 600 px/frame, `FDS_SNAPSHOT_ZDUMP=1`, one script and one framing
per pose across every arm. `lid` = P1's recommended recipe
(`--pom_shell --parallax_pom_cone --pom_shell_cap=16 --parallax_pom=32
--pom_cone_exact=1 --pom_cone_min_step=1 --pom_march_earlyout`); `rec` = the
same recipe **plus `--pom_recess_only`**; `recW18` adds
`--pom_shell_world_amp --pom_shell_world_amp_set=0.18` (§8's candidate).

| arm | p5743 | p6097 | p5780 | p2845 | p4200 | pmir | pmir5743 |
|---|---|---|---|---|---|---|---|
| `tess` (`--greets_displace`) | 3 | 0 | 5 | 0 | 2 | 24 | 24 |
| `flat` (flat POM, no shell) | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `lid` (P1 recipe) | **98 371** | 0 | **21 909** | 0 | **26 765** | **88 603** | **88 603** |
| `refUV` (the converged LID reference) | 99 251 | 0 | 22 697 | 0 | 26 794 | 89 725 | 89 725 |
| **`rec` (recess-only, clamp)** | **0** | **0** | **0** | **0** | **0** | **0** | **0** |
| **`recW18`** | **0** | **0** | **0** | **0** | **0** | **0** | **0** |
| `recdisc` (recess-only + `--pom_recess_edge=2`) | 129 606 | 0 | 73 399 | 228 | 68 112 | 119 818 | 119 818 |

**The gate is PASSED: zero void at every pose, against tessellation's 0–24.**
Not "single digits" — zero, and by construction rather than by tuning: the
rastered surface is the authored polygon, and the clamp has no branch that can
leave a pixel unwritten.

Three reading notes, all measured:

- **`refUV` has the SAME holes as the arm it scores** (99 251 vs 98 371 at
  t=5743). That is C7 in one line, and it is why P1's error columns could not
  see this class. The recess reference (`refrec`, §10.3) has **0 void at every
  pose**, so from here on the reference is not blind to it.
- **`recdisc` is the discriminator, and it is why the flag has a sub-flag.**
  Same geometry, same march, same domain test as `rec`; the only difference is
  that a ray leaving the patch is KILLED instead of clamped. It voids
  68 k–130 k px. So the holes are not "the moved lid" alone — the *mandatory
  discard* is the mechanism, and removing the need for it is what fixes them.
- **pmir and pmir5743 give identical void counts** (88 603 / 119 818) although
  the frames differ (different md5). The gashes sit on static wall seams; only
  the robot moves between t=5743 and t=5780.

**NOT REPRODUCED.** The task handed me a void table reading tessellation 24 /
flat 0 / shell cap 2 16 539 / shell cap 16 34 944 at this pose. My independent
counts with the recipes stated above are tessellation **3**, flat 0, shell cap 2
**68 122**, shell cap 16 **97 329** — same direction, same conclusion, 2–4× the
magnitude. I also rendered the cap-2 arm without the cone march (71 299) and
both arms at `--pom_shell_world_amp_set=0.18` (66 951 / 92 306); none lands near
16 539. I do not know which recipe difference accounts for it, so both numbers
are recorded rather than one quietly replacing the other. The cap 2 → cap 16
ratio is 1.43× in my data, not the 2.11× implied by the earlier pair.

### 10.2 What the flag does, and the one thing it deliberately does NOT do

`--pom_recess_only` is a MODE of `--pom_shell` (inert without it — the builder
is what stamps the amplitude and the patch domains):

1. **`PomShell_Build` moves nothing.** The lid offset is zero, so the authored
   wall stays exactly where the artist put it. Every offscreen consumer sees the
   true wall by construction (verified, §10.4 — not assumed).
2. **The height field's maximum sits AT the authored plane.** `Vertex::ShellH`
   is stamped **1.0 exactly**, with no smooth-normal corner correction: that
   correction models how much of an *offset* a smooth vertex normal delivered
   perpendicular to a given face, and with no offset it would only invent a
   fictitious entry height below the real surface. The march therefore starts on
   the authored surface and only ever goes deeper.
3. **The lateral-exit kill becomes OPTIONAL, and killing becomes the wrong
   default.** `--pom_recess_edge`: 0 (default) = clamp to FLAT (the geometric
   UV + the plane's depth — always in-domain, invents no surface), 1 = clamp the
   marched UV into the patch box, 2 = discard (the lid model's behaviour, kept
   as the diagnostic that produces the `recdisc` row above).
4. **`--pom_shell_base_clip` is forced OFF.** It exists to remove LID overhang
   outside the authored footprint; with the geometry unmoved there is none, and
   left on it would test the domain at a half-slab lateral offset
   (`A·(0.5 − hEnter)` = `−0.5·A` here) and clip real wall at every patch border.

What it does **not** do: it does not carry relief across a patch seam. The ray
that leaves is clamped, not continued. That is the honest gap and §10.8 sizes it.

### 10.3 Colour + depth error, each arm against the reference of ITS OWN semantics

§8.7's rule applies without exception. `rec` is scored against `refrec`
(`--pom_ref_march --pom_ref_steps=512 --pom_shell_cap=64 --pom_recess_only`),
`recW18` against `refrecW18`, `lid` against `refUV`. Never across semantics.

| arm | pose | colour >12 | colour >32 | \|dz\|>0.05 | \|dz\|>0.20 | void arm/ref |
|---|---|---|---|---|---|---|
| `lid` vs `refUV` | p5743 | 93 765 | 22 471 | 14 160 | 6 542 | 98 371 / 99 251 |
| | p6097 | 70 407 | 12 332 | 761 | 320 | 0 / 0 |
| | p5780 | 105 294 | 26 942 | 11 378 | 5 671 | 21 909 / 22 697 |
| | p2845 | 46 532 | 9 595 | 95 | 13 | 0 / 0 |
| | p4200 | 115 194 | 32 071 | 11 014 | 3 163 | 26 765 / 26 794 |
| | pmir | 111 464 | 31 602 | 23 941 | 17 949 | 88 603 / 89 725 |
| **`rec` vs `refrec`** | p5743 | 84 629 | 19 178 | 10 521 | 5 030 | **0 / 0** |
| | p6097 | 62 638 | 12 020 | 738 | 41 | 0 / 0 |
| | p5780 | 86 771 | 20 481 | 7 840 | 3 528 | 0 / 0 |
| | p2845 | 45 888 | 9 209 | 112 | 12 | 0 / 0 |
| | p4200 | 102 334 | 28 716 | 8 966 | 2 432 | 0 / 0 |
| | pmir | 95 346 | 25 656 | 21 126 | 16 326 | 0 / 0 |
| **`recW18` vs `refrecW18`** | p5743 | **54 093** | 11 071 | **5 361** | 3 336 | 0 / 0 |
| | p6097 | 52 514 | 7 273 | 429 | 35 | 0 / 0 |
| | p5780 | 61 193 | 14 470 | 3 390 | 2 186 | 0 / 0 |
| | p2845 | 45 888 | 9 210 | 112 | 12 | 0 / 0 |
| | p4200 | 56 021 | 17 141 | **706** | 203 | 0 / 0 |
| | pmir | 64 656 | 15 479 | 14 097 | 11 612 | 0 / 0 |
| `recdisc` vs `refrec` | p5743 | 218 969 | 150 364 | 20 096 | 14 621 | 129 606 / 0 |
| | p5780 | 250 068 | 171 134 | 100 578 | 96 275 | 73 399 / 0 |
| | p4200 | 171 940 | 98 176 | 10 403 | 3 870 | 68 112 / 0 |

Three things this says:

1. **The march is no worse under recess semantics.** `rec` scores at or slightly
   below `lid` on every column at every pose (e.g. t=5743 colour 84 629 vs
   93 765; depth 10 521 vs 14 160). Recess-only is not buying its zero void with
   march quality.
2. **The pipeline reproduces P1 exactly.** `lid` vs `refUV` at t=6097 is
   70 407 / 12 332 / 761 / 320 — the same four numbers as §9.8's BEST row, taken
   independently. That is the control that makes the rest of this table
   comparable to §9.
3. **`recdisc` is punished by its own reference, as it should be.** Its error is
   2.5–3× `rec`'s precisely because `refrec` clamps and the arm voids. This is
   the property C7 lacked: the recess reference *can* see a hole.

**The surface DID move, and here is the honest statement of it** (converged
march on both sides, so the march is not a variable):

| pair | pose | colour >12 | colour >32 | \|dz\|>0.05 | median dz |
|---|---|---|---|---|---|
| `refrec` vs `refUV` | p5743 | 1 137 103 | 453 217 | 1 597 614 | **+0.1668** |
| | p6097 | 1 270 325 | 404 190 | 1 929 554 | +0.1062 |
| | p5780 | 1 167 114 | 501 507 | 1 695 646 | +0.1466 |
| | p4200 | 1 094 476 | 380 129 | 1 895 260 | +0.0986 |
| | pmir | 1 255 446 | 566 915 | 1 877 230 | +0.1820 |

Recess-only is **a different surface**, not a bug fix applied to the same one:
the whole relief hangs below the authored plane instead of straddling it, so the
visible surface sits about half a slab deeper. Per material, view-space, against
the flat arm:

| pose | `rooms` median | `rooms::mirUV` median | `floor::mirUV` median | floor p95 |
|---|---|---|---|---|
| p5743 | +0.2300 | +0.1517 | **+1.6960** | +6.6905 |
| p5780 | +0.3084 | +0.1415 | **+1.7718** | +6.4731 |
| p4200 | +0.0784 | +0.1592 | **+2.0878** | +4.1579 |
| pmir | +0.1719 | +0.1592 | **+2.0296** | +7.8203 |

Those are VIEW depths, so a normal-direction recess of `A/2` shows up multiplied
by `1/(V·N)` at grazing. The wall's `A/2` is 0.09 world and reads as +0.14..+0.31
view depth — small. **The floor's is 0.56 world**, because §8 measured its slab
at 1.11 world (6.2× the wall's, from a `ParallaxScale` tuned as a UV offset), and
at the grazing angles this scene actually uses that becomes 1.7–2.1 world of view
depth. Recess-only does not create that defect, it **exposes** it: the centred
lid model hid half of it above the plane. `--pom_shell_world_amp_set=0.18` puts
the floor on the wall's scale and is why every `recW18` column above is better.

### 10.4 OFFSCREEN — C6 goes to zero, VERIFIED

**Shadow bake, exact.** `--dump_shadowmap` from the `-DFDS_DEV=ON` build in
`build-dev/` (which does not overwrite `Runtime/DEMO`), t=5780, 76 cube faces,
13 533 184 depth texels:

| pair | texels differing | by >8/255 |
|---|---|---|
| `flat` vs **`rec`** | **0 (0.00 %)** | **0** |
| `flat` vs `lid` | 4 044 301 (29.88 %) | 181 301 |
| `flat` vs `tess` | 720 250 (5.32 %) | 81 809 |

The two control rows reproduce §C6's figures to the digit (4 044 301 / 29.88 %
and 720 250 / 5.32 %), which is what makes the zero row trustworthy rather than
a broken instrument. **The moved lid changes 30 % of the per-frame shadow depth
cube; recess-only changes nothing at all.**

**Final image, on NON-STONE pixels only** (the wall's own shading excluded, so
what is left came through an offscreen consumer or a Z-ownership change):

| pair | pose | non-stone px | >4/255 | >12/255 | >32/255 | max | worst-hit surfaces |
|---|---|---|---|---|---|---|---|
| `flat`→**`rec`** | p5743 | 354 312 | 3 807 | **1 423** | 306 | 97 | `Hull.lwo::cockpit_upper::mirUV` 1 128 |
| | p5780 | 350 386 | 4 323 | **1 494** | 310 | 105 | cockpit 1 001, `amudim::mirUV` 278 |
| | p4200 | 146 068 | 285 | **109** | 13 | 79 | cockpit 38 |
| | pmir | 83 414 | 623 | **66** | 1 | 35 | `amudim::mirUV` 42, `sss` 1 |
| `flat`→`lid` | p5743 | 354 312 | 34 089 | **26 328** | 19 964 | 168 | `siling` 8 491, robot legs 9 124 |
| | p5780 | 350 386 | 35 669 | **28 615** | 22 490 | 208 | `siling` 6 881, legs 12 097 |
| | p4200 | 146 068 | 7 909 | **4 467** | 2 775 | 184 | `siling` 3 132 |
| | pmir | 83 414 | 8 377 | **6 065** | 4 165 | 204 | `sss` 2 447, `siling` 1 842 |

**18× less off-target contamination** (26 328 → 1 423 px >12/255 at t=5743), and
the residual is a different KIND of pixel: the lid arm moves the CEILING and the
robot's legs — surfaces the wall has no business touching, reached through the
shadow cube and the offscreen passes — while recess-only's residual is almost
entirely `Hull.lwo::cockpit_upper::mirUV` and `amudim::mirUV`, i.e. reflective
surfaces showing the wall's *changed shading*. The mirror shard material `sss`
moves by 1 px at pmir under recess-only, against 2 447 px under the lid.

Honest boundary on this: `rec` vs `flat` differs in the MARCH as well as the
geometry, so a nonzero number in that table is not proof of moved geometry. The
shadow-cube zero is the geometric proof; the non-stone table is the consequence.

### 10.5 DEPTH — the S1a hazard is gone by construction, VERIFIED

S1a's design comment flagged that a protruding block writes Z *closer* than the
authored wall, so a later face inside that band wins pixels it should not.
Recess-only cannot do that. Measured frame-wide (every rasterised pixel, both
arms finite), against the flat arm, "nearer" = more than 2 zEnc codes
(0.0051 world) closer:

| arm | p5743 | p6097 | p5780 | p2845 | p4200 | pmir | worst nearer |
|---|---|---|---|---|---|---|---|
| **`rec`** | **0** | **0** | **0** | **0** | **0** | **0** | **+0.0000** |
| **`recW18`** | **0** | **0** | **0** | **0** | **0** | **0** | **+0.0000** |
| `lid` | 864 618 | 1 215 750 | 537 126 | 1 540 103 | 1 053 203 | 1 089 068 | **−31.85 world** |

**Not one pixel in any of the six frames is drawn nearer than the authored
plane.** The lid arm draws 26–74 % of the frame nearer, by up to 31.85 world
units at grazing (that tail mixes the depth write with ownership changes; the
sign and the population are the point).

### 10.6 THE CLAMP — what it costs, and where it fires

The clamp is not free: a clamped pixel shows the FLAT wall, with no relief. It
is measurable exactly, by rendering the same arm with `--no-pom_shell_domain`
(no clamp at all — the ray keeps its out-of-patch UV). The pixels that differ
ARE the clamped ones:

| arm | pose | clamped px | >12/255 | % of frame | (same arm's void if it discarded instead) |
|---|---|---|---|---|---|
| `rec` | p5743 | 147 618 | 116 199 | **7.12 %** | 129 606 |
| | p5780 | 176 837 | 147 232 | 8.53 % | 73 399 |
| | p4200 | 70 296 | 54 645 | 3.39 % | 68 112 |
| | pmir | 132 542 | 112 307 | 6.39 % | 119 818 |
| `recW18` | p5743 | 100 712 | 81 912 | **4.86 %** | 85 065 |
| | p5780 | 119 508 | 105 654 | 5.76 % | 24 188 |
| | p4200 | 17 243 | 12 322 | **0.83 %** | 14 049 |
| | pmir | 68 468 | 52 859 | 3.30 % | 49 130 |

So **3.4–8.5 % of the frame (0.8–5.8 % at the 0.18 world amplitude) renders as
flat wall instead of carved stone**, and it is the same population that the lid
model rendered as holes. `docs/img/s1_p2a/CLAMP_p5743_recW18.png` tints it red
over the arm's own frame: it lies in thin bands along the panel seams and the
wall/ceiling junctions — exactly where the gashes were. A band of flat stone is
a far cheaper artefact than a void, but it is an artefact, and the literature fix
(§10.8) is what removes it rather than trading it.

### 10.7 LOOK, and PERF

**Look.** Crops in `docs/img/s1_p2a/`, identical framing per pose (1920×1080 →
900×506 LANCZOS), one transform for every arm:

| file | what |
|---|---|
| `F_p5743_{tess,best,rec,recW18pn}.png` | the user's primary pose. `best` is the reported defect: a full-height black gash on the right wall. `rec` is solid. |
| `F_pmir_{tess,best,recW18pn}.png` | the mirror pose. `best` has black bars along every panel seam and a black slab across the floor; `recW18pn` is solid and reads closest to `tess` of any per-pixel arm. |
| `F_p4200_{tess,best,recW18}.png` | the vista. Black bar beside the doorway in `best`, absent in `recW18`. |
| `CLAMP_p5743_recW18.png` | §10.6's clamp map (red = clamped). |
| `Z_p2845_{tess,recW18}.png` | grazing close-up, ×2. Mortar groove and block face are equivalent; recess's groove is straighter. |

My own read, having looked at all of them: **the wall reads as carved, and at
the block scale it is competitive with tessellation** — but only with two
companions. On its own (`rec`, UV amplitude) the FLOOR is visibly worse than
`tess`: mottled and melted, because its 1.11-world slab now hangs entirely below
the plane (§10.3). `--pom_shell_world_amp_set=0.18` fixes that (`F_pmir_recW18pn`
vs `F_pmir_rec`), and `--pom_normal` (S1e) is what gives the blocks their
bevelled edges — without it they read flat next to the bake. The honest ranking
at pmir is `tess` ≈ `recW18pn` > `recW18` > `rec` >> `best`.

**What it gives up is visible too, and it is structural, not a tuning miss:**
the wall's outline is exactly the authored polygon. No stone stands proud, none
breaks a silhouette, none occludes its neighbour across a corner. At p4200's
doorway edge (`SIL` crops in the scratchpad) `tess` is *also* a straight line —
its bake pins patch-border verts to zero — so at a patch BORDER the two models
agree; the loss is inside the wall, where tessellation's blocks can rise above
the mean surface and recess's can only sink below it.

**Perf.** `./DEMO --bench=scene@scene=greets,t=5780,iters=60 --deferred
--profiler=0`, five arms INTERLEAVED (A B C D E, ×4 reps) so thermal drift hits
each equally, headless dummy drivers, load-guarded (the script refuses to start
with another `DEMO` up). **Machine load 2.9 → 10.2 rising** — the user was
running an 86Box VM and Firefox throughout; that is the honest condition and the
paired structure is what makes it survivable. Deltas are per-rep then averaged.

| arm | mean ms/iter | run-to-run spread |
|---|---|---|
| A `lid` (P1 recipe) | 57.80 | 3.06 |
| B **`rec`** | **56.98** | 2.89 |
| C **`recW18`** | **56.12** | 3.58 |
| D `flat` (no shell) | 56.99 | 6.15 |
| E `tess` (shipping) | 92.54 | 8.16 |

| paired delta | ms | sd over 4 reps |
|---|---|---|
| `rec` − `lid` | **−0.82** | 0.93 |
| `recW18` − `lid` | −1.68 | 1.10 |
| `recW18` − `rec` | **−0.86** | **0.30** |
| `rec` − `flat` | −0.01 | 1.42 |
| `tess` − `rec` | **+35.56** | 2.26 |

A second interleaved run isolating the mechanism (A `lid`, B `lid
--no-pom_shell_base_clip`, C `rec`): B−A = −0.56 ± 1.72, C−B = **−1.80 ± 1.02**.

Read honestly: **recess-only has no measured cost over the lid arm — it is
0.8–2.4 ms FASTER, with a paired sd of 0.9–2.7 ms.** I will not claim a win of
that size on a machine at load 8; the safe statement is "no cost, plausibly
about a millisecond cheaper", and three mechanisms all point that way: it skips
the base-clip test (2 FMAs + a compare group per covered pixel, measured at
−0.56 ms on its own), it kills no lanes so the full-row vector-store path is
never disabled by a partial mask, and it leaves no holes for later geometry to
fill. `recW18` − `rec` = −0.86 ± 0.30 is the tightest number here and has a
clean mechanism: the floor's slab is 6× shallower, so the march brackets sooner
under `--pom_march_earlyout`. And `rec` sits **within 0.01 ms of flat POM**,
i.e. at this pose the whole shell march is inside the noise, while tessellation
costs **+35.6 ms** on the same frame.

### 10.8 WHAT RECESS-ONLY CANNOT DO (state this before deciding)

Not a caveat list — these are the terms of the trade:

1. **Nothing can stand proud of the authored plane.** No protruding block, no
   relief silhouette, no stone occluding its neighbour across a corner, and the
   wall's outline is exactly the authored polygon's at every angle. Measured as
   the flip side of §10.5's zero: 0 px of the frame is ever drawn nearer.
2. **The visible surface recedes by half a slab.** Rooms get bigger: +0.09 world
   on `rooms`, +0.56 on the `floor` under today's UV amplitude (measured as
   +0.14..+0.31 and +1.70..+2.09 of VIEW depth at grazing). Under
   `--pom_shell_world_amp_set=0.18` both become 0.09. Anything authored to touch
   the wall (trim, props, the mirror frame) now has the stone receding behind it
   — a gap the lid model hid by pushing the wall out instead.
3. **Relief still does not cross a patch seam.** The clamp turns the hole into a
   flat band, 0.8–8.5 % of the frame (§10.6). The march still cannot address a
   neighbouring patch's UV chart, so the information is not there to shade with.
4. **It is not the literature's model.** Hirche '04 / shell maps raster a CLOSED
   shell volume — top *plus* side faces — so a ray only exits where the surface
   genuinely ends and discarding there is CORRECT. That model keeps protrusion
   AND has no internal holes; it costs prism geometry (side faces, extrusion
   clamped by local curvature) and a cross-patch march continuation, which is
   this campaign's S1d. Recess-only buys the hole fix by giving up the half of
   the slab that made the holes necessary. **It is the cheap correct-by-
   construction option, not the correct one.**
5. **It changes the surface, so it invalidates cross-arm comparisons.** Every
   number in §§1–9 was taken against `refUV`. A recess arm must be scored
   against `refrec`, and the two references differ by 1.1–1.3 M px (§10.3).
6. **It does not fix C4 (tessellation's lattice), C1's residual grazing colour
   (§9.11's 48 k–68 k px) or the amplitude semantics of §8.** It is orthogonal
   to all three, and it makes §8's floor amplitude MORE visible, not less.

### 10.9 GATES

| gate | result |
|---|---|
| `tools/render_gate.sh` | **3/3 PASS** (mirrortest / conetest / halotest) |
| city `t=1961` colour | `37e62845c4d30eefa321730c5bb7e0b8` — byte-exact |
| city colour+z concatenated | `7438016d804807154892c019b8b69c73` — matches §9.10's recorded value |
| fountain `t=2500` | `51fff7cd38767d619280afe0498a6f24` — byte-exact |
| greets `shell` cone-8 t=6097 | `193427ccb28163705ea6baa5500afd0c` — byte-exact |
| greets `shell` naive-8 t=6097 | `13dcf8e54416816a29ba90f0fe468756` — byte-exact |
| greets `tess` t=6097 | `3f86c73cc7ed8f0ad8f57b12984537d0` — byte-exact |
| greets `flat` t=6097 | `9d095fbcac0c00888578d56172786997` — byte-exact |
| `lid` vs `refUV` @ t=6097 | 70 407 / 12 332 / 761 / 320 — identical to §9.8's BEST row |
| wasm | `cmake --build build-wasm` **links clean** (80/80, `DEMO.wasm` + `DEMO_snapshot.wasm`) — `Mekalele.h` is a shared kernel, so this was required |

Every render's log was grepped for `unknown flag` / `requires a value` before
its numbers were taken (zero hits across all 88 snapshot renders plus the four
shadow-dump runs and the gate runs), and every flag list is
a bash ARRAY, never an unquoted `$FLAGS`. Scripts: `p2_run.sh`, `p2_base.sh`,
`p2_batt.sh`, `p2_amp.sh`, `p2_clamp.sh`, `p2_sm.sh`, `p2_bench.sh`,
`p2_bench2.sh`, `p2_an.py`, `p2_err.py`, `p2_mat.py`, `p2_nonstone.py`,
`p2_smdiff.py`, `p2_clampviz.py`, `p2_crop.py` in the session scratchpad.
