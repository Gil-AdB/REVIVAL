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

## Stages, each independently gated

**S1d-1 — topology + classification (DIAGNOSTIC ONLY).** Build per-patch-edge
neighbour links and classify every patch boundary edge into the three classes
above. Report the census per material and the total edge length weighted by
screen coverage at the review poses. Add a viz that colours patch boundaries by
class. NO behaviour change. Deliverable: the census + the verdict on which of
(A)/(B) dominates.

**S1d-2 — march continuation across coplanar seams.** Hand-off at domain exit
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
