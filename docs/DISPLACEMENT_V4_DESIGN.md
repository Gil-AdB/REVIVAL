# Displacement bake v4 — design

Status: DESIGN, no code. Written 2026-08-30 on `rev-bakedesign` (off `fog-wt` fcca1ed7)
by the coordinator's design fork, after Gil-Ad's decision to rewrite the bake from
scratch (ledger decision `f2696d3c8aa7`, verbatim quote inside). Every choice below
cites the ledger record that measured it; `groundwork query <subject>` reproduces
the citation. Three choices are marked **PENDING HIS RULING** and carry the
assumption taken meanwhile plus its falsifier.

Companion documents: `docs/DISPLACEMENT_LITERATURE.md` (the survey, §A–G +
addendum), `docs/DISPLACEMENT_RESEARCH_II.md` §REF / §REF-2 (the reference
renderer, two rounds), `docs/BULGE_CORPUS.md` (his verdicts, states 1–12).

## 0. What v4 is, and when it is done

v4 is a new `DisplaceStoneSubdiv` that **generates the same solid the reference
renderer marches** — the union of per-face offset slabs of the authored stone
planes — as a tessellated, watertight mesh, instead of patching the 1998 mesh
with a lattice and a chain of normal-side corrections. It runs once at scene init
behind `--greets_displace_v4` (default OFF); the old bake stays the shipped
default until v4 replaces it by his fly-through. The reference renderer
(`--greets_displace_ref`) is the yardstick, not the goal: v4 is judged by its
distance from that definition, per pixel, and by his eye.

Acceptance (all measured, all at the corpus poses cam A / cam B / H6017 / H6194
and the 22-pose review list, `docs/greets_review_poses.txt`):

| criterion | target | instrument | today (old bake) |
|---|---|---|---|
| geometric error vs the reference | dz p50 within 2× the reference's own noise floor (the bare-arm p50, 0.016–0.035 u per pose), p90 < 0.08 u, no pixel > 0.16 u (the amplitude) | `tools/refrender_diff.py` (`7ac873df0332`, `f47faa153876`) | p50 0.026–0.071, p90 0.06–1.22; bake is further from the definition than the FLAT wall at 7 of 8 poses |
| holes | **0** hole px on the 54-pose tear battery, every pose | `tools/tear_battery.sh` + `tools/tear_detect.py` | 2 169 (rev-dispfix tip), 5 454 (fog-wt) |
| shading normal vs the reference | normal-angle p50 < 5°, p90 < 15° on stone pixels | `refrender_diff.py` normal plane | p50 4.7–21°, p90 40–114° |
| his eye | fly-through at the corpus poses reads at least as well as state 11 ("C looks good") | his words, recorded by the coordinator | `6e423128aa33` |
| init | bake wall-clock ≤ 500 ms (old bake: 475–530 ms) | `[STONE-*]` banner timing | 523–534 ms |
| faces | ≤ the old bake's at equal relief (~85–90k scene faces); no sliver family: min-angle p10 > 2°, no face under 1°; corner-column density ≤ 2× the wall's | census of `displace_faces.txt` | 73 vs 11 faces/u² at the x=17.898 column, 154 of 400 faces under 1° |
| per-frame | greets tick t=5743/5965 min-of-11 within the ±1.5–2 % placement floor (`perf.floor` law, §00w) | `--bench=scene` protocol of PERF_LAWS.md | — |
| old default | byte-identical with the flag OFF, every pin | render_gate / warm_gate / 14 pins | — |

## 1. The model v4 generates

The definition is the reference renderer's (§REF "The model it renders"), with the
two corrections its own pictures forced (§REF-2). Each element, and what measured it:

1. **Height field.** `d(u,v) = amp·(h(u,v) − mipMean)`, `h` bilinear at texel
   centres, toroidal wrap, at the bake mip (mip 2; mipMean 0.5471 rooms /
   0.3405 floor; amp 0.300; |d|max 0.164 rooms / 0.050 floor). Same convention as
   the reference so that dz measures the *mesh*, not a sampling difference.
2. **Direction = the face's authored plane normal.** Nothing else, anywhere.
   Law `420fcd4626b8`; the survey's OpenSubdiv quote (§B) and the month's root
   cause: the smoothed 1998 normal reached the geometry through three doors
   (mitre bisector dd798c31, groove-shade target 41ff72ed/16aac2ee, ride sign
   a0c46c40) and each door was a separate bug. v4 has no such field to pollute.
3. **Slab per face.** Signed distance from the face plane in `[−back, d(u,v)]`,
   over the face polygon **extended past each shared edge** by the per-edge
   margin (item 5) and **trimmed at the mitre only at convex edges** (item 6).
4. **Membership = UNION.** A point is solid if *any* slab contains it. The
   literal "nearest plane governs" partition **deletes material** at grazing
   corners (trap `e43c035dbedf`: face 881 edge-on erases ~90 px of the pier
   front at cam A). **PENDING HIS RULING** (open `5a45f66b1478`, ambiguities
   `5463f141eb8f`); assumption: union. Falsifier: a corpus pose where the
   partition arm reads better to his eye AND deletes nothing (the reference has
   both arms: `--greets_displace_ref_partition`).
5. **Extension margin, per edge, derived — never a constant.** A crease with
   normal angle φ needs `max(cot, tan)(φ/2)·|d|max` (cot is the bisector's need,
   tan the mitre's; the survey's cot correction, §G addendum). A **smooth seam**
   (φ below the crease threshold) is not a corner: sizing it by cot replaced the
   curved wall (trap `60e3e63bed65`). Margin capped at `sqrt(L²−1)·|d|max` for
   mitre limit L = 4 (SVG's default; fires on 1 edge in the scene — every real
   corner here caps at 2× the height, law `f713599ea11d`).
6. **Mitre trim at CONVEX edges only.** Applying the trim at every shared edge
   turned the union into an intersection and produced 97.5 % of the reference's
   own holes (`032cf9150e6a`: 4 007 of 4 009 misses at cam A vetoed, 0 from a
   convex edge; nine-pose misses 31 307 → 773, `582c083abef6`). Concave
   junctions (wall meets floor) need no rule under union.
7. **Free edges → skirts, after classification.** A free edge is used by exactly
   one face with no face of *any* material sharing it or position-coincident
   across it. The scene has **4**. Everything else is shared, and the survey's
   rule stands: at an ambiguous edge the safe default is "shared — weld", never
   "open — skirt" (a skirt on a misclassified edge hides the real crack, and the
   floor-base tears were exactly a material-blind misclassification, bc79e39d).
8. **The corner rule is PER JUNCTION, decided by the crease census** — this is
   the design's answer to his "the height doesn't always agree from both sides"
   (`a0e7dfd23ca4`). The census (`83aa7c0b6c4c`; §REF-2) over 453 junctions,
   7 375 u of crease: **73.3 %** of the length is the *same* relief 1.4 texels out
   of phase (r_best ≥ 0.8) — a dominant-owner height on the crease reconciles
   it; **21.2 %** is two *unrelated* UV charts (r_best < 0.4, no lag helps) — no
   corner rule reaches it; 5.5 % in between. So:
   - phase-shifted junction → **dominant height on the crease** (Dudash's
     dominant data, micro-mesh's duplicated edge values; owner = canonical
     order-independent key; blend back over 2 texels). No step. Exact in band:
     the reference's shared-edge arm proves the band is a closed-form t-interval
     (`75a1ca5fcd65`: 505 ms, hit mask bit-identical).
   - unrelated-chart junction → the options, none free: (i) **steps** — his
     ruling #1 of 2026-08-29, a castellated bisector step; both sides keep their
     own height; honest to the data, reads as artifact where the courses do not
     line up; (ii) **dominant** — picks a winner, hides the other wall's course
     for 2 texels; (iii) **re-author the UVs** at those junctions — the only fix
     that makes the two sides agree in fact; the census names them (junction
     centroids in `docs/img/refrender/`, e.g. the four tiled room modules at
     (4.9, 2.5, −37.0) and its copies).
   **PENDING HIS RULING**; assumption meanwhile: dominant on phase-shifted,
   **steps** on unrelated (his standing ruling), both as one flag
   `--greets_displace_v4_corner=census|steps|dominant`. Falsifier: the
   junction-pair renders (`docs/img/refrender/junction_<pose>.png`) read worse
   to his eye under the census rule than under either uniform rule.
9. **Shading normal** `n ∝ N − ∇ₛh` in the plane's own frame — exact on a plane
   (survey §C); split normals at creases (dihedral > 30°); a smooth base normal
   only inside a merged chart (the curved wall). Never an average across a
   crease (the pillow shading, 16aac2ee; the crease-gate refutation
   `864d9658ae3a` was a gate on the *wrong* field — v4 has no averaged field).
10. **Back and lid.** A slab's back is the amplitude (§REF ambiguity 4); a face
    whose lid is authored (the pier top) is a face like any other — its slab
    extends past its edges by item 5 and meets the wall slabs by item 6 (this is
    the lighter wedge he saw at the pier top in `camA_triptych.png`; it is the
    definition, and the census rule decides whether that crease is dominant or
    stepped).

What v4 deliberately does **not** carry from the old bake: the profile
densification at pitch 0.08 u along every border line (the source of the 73
faces/u² corner column and its 154 sub-1° slivers), the line-height rep
machinery (E/P levels, crossing-minimum rule), the free/pin/recess vertex
classes, the reflex weld, the joint snap/split family (all refuted:
`408a38faafa7`, `09cecbe6d510`, `baf27927b0e5`, `0c2cd2a858e6`, `c165bff2f03a`,
`20be1fac463e`), `cpb` as a wall lever (`558b45ea139b`), and every flag that
chose a sign by dot with a smoothed normal.
