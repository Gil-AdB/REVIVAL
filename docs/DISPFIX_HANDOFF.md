# HANDOFF — bridging-face split round (written 2026-08-28 by the detection/clamp fork at context limit)

## State (all pushed on rev-dispfix; tip fab5b1c0)
- Worktree /Users/gil-ad/work/rev-dispfix; Runtime/DEMO installed, default arm
  byte-identical to pre-round (md5 color eeb15725... at cam A t=5965).
- The GROUND-TRUTH detector is validated (docs/BULGE_CORPUS.md): --refplane_dump
  (+ --bulge_dump + FDS_SNAPSHOT_ZDUMP=1 FDS_SNAPSHOT_GBUFDUMP=1) →
  tools/refdiff_detect.py. It reads 0.00 on bare walls, fires on every era
  Gil-Ad labeled broken, and is the ONLY accepted gate.
- HIS TWO MARKED REGIONS (cam A "22.5084476,3.87992334,-61.8882256,
  -0.829246342,-0.20816116,0.518670499", t=5965): rect (1080,315)-(1235,415),
  ellipse c(1160,650) r(130,115). Current numbers to beat toward ~0:
  27.1% / 14.8% of pixels protruding >0.08u past the outer-mitre envelope,
  p90 |dz| 0.66/0.53 u. Bare floor at the same marks: 0.0000 / p90 0.0011.

## The mandate (coordinator, verbatim intent)
Face-level split at the junction line: NO face may span a junction — split
every bridging face so each fragment belongs to one wall sheet and takes that
sheet's displacement, frames, and UV/height sampling; junction-line verts
follow the proven mitre construction. New FeatureFlags.def flag, default ON in
branch, byte-exact --no- restore proven. Gate per round as above; quiet planes
stay 0.00; shading no regression; locality sweep; before/after textured + geo
heatmaps full-path in docs/img/refdiff/. If the split moves the marks
substantially but not to floor: measure and NAME the remainder before any
second lever. ROUND 2 AFTER the juts: the groove over-carve (−0.0515 med at
groove verts vs reference; plateaus are ON at −0.003) in the notch/ramp chain
— second BECAUSE the recession masks the juts (measured: simulated DC fix
worsens marks 28.7→35.8%).

## Evidence chain for the bridging-face premise (label: supported, not proven)
1. Census: 70 envelope violators at the pier junction are 100% bake-CREATED,
   wall parents (pNy=0), ZERO applied displacement — positions constructed.
2. The vert clamp (3 iterations, final: strict footprint containment,
   junction-scoped, tol 0.05) pulls 576 real off-envelope verts (worst 1.16u),
   changes ~78k px — and the marks DO NOT MOVE. Verts are each ≤tol after the
   pull, yet pixels still protrude 0.66u at p90 → the protruding surface is
   FACE INTERIORS whose corners are individually legal = bridging faces.
   A fresh fork should verify this directly if cheap (per-pixel face resolve at
   the marks; NOTE the _face.u32 plane was reported broken once — floats in
   the storage — so budget that or trust the elimination).

## Pitfalls burned into this branch (do not re-burn)
- The engine winding cross is the ANTI-visible normal; fronts come from the
  authored-majority rule (see dd798c31 and the env-clamp registry code).
- A per-mesh plane registry fragments (8 of 24 dominant planes) — build plane
  registries across ALL meshes from AUTHORED faces at bake start
  (DisplaceStoneSubdiv, the envDoms block at ~MeshOps.cpp:2082 does exactly
  this — REUSE it; in-plane 2D solve, the 4x4 affine on planar points is
  rank-3 and fails).
- Constructed lip verts sit up to 1.2u in FRONT of their base plane — any
  "near the wall" band test must allow that or it skips the offenders.
- tol vs the course-level castellation: profile levels legitimately exceed
  pointwise amp*(h−mean) by intra-course variance (~0.02-0.04); use ≥0.05.
- The reference convention: mip per MATINFO (currently 2), bilinear
  texel-center + toroidal wrap (SampleHeight8Bilinear), d = amp*(h−mipMean),
  mipMean 0.5471 rooms / 0.3405 floor. bare arm reference = amp 0.
- Gate mechanics: raw dumps land in docs/img/refdiff/raw/<arm>_<cam>/ (bare_*
  is the reference geometry — authored faces; NEVER take faces from a
  displaced arm). Driver snippet at the bottom of tools/refdiff_detect.py;
  run_arm(rawdir, baredir, tag, outdir, metrics). zsh: use ${=VAR} to split
  flag strings; never echo ===.
- Byte-exact restore proof per flag: --no-<flag> color md5 == eeb15725... at
  cam A r3 arm. Keep Runtime/DEMO installed after every push.

## Where the code lives
- Bake: DEMO/MeshOps.cpp DisplaceStoneSubdiv (~2057); envDoms registry ~2082;
  refuted clamp block just before "// Commit new arrays." (~7400) — kept as
  instrument (greets_displace_env_clamp, default OFF), its per-vert
  plane/UV/height evaluation code is directly reusable for the split's
  ownership tests. Mitre machinery: search MitreGroup / [STONE-MITRE].
- Instruments: --refplane_dump (Snapshot.cpp ~880), --bulge_dump,
  --displace_dump (WorldAabb.cpp ~538, has created/nParents/pNy attribution),
  --normchain_dump. FDS_ENVCLAMP_DEBUG=1 prints plane-cluster areas.
- Corpus + ledger: docs/BULGE_CORPUS.md (his verbatim labels — the ONLY
  ground truth), docs/SESSION_STATE.md top entries 2026-08-28f/g.


## UPDATE 2026-08-28h (the fork after the handoff — verification round)
The junction-split premise was tested directly and REFUTED: see SESSION_STATE
2026-08-28h. The real producer of his marked juts: course-spanning sliver fan
faces on the corner strip with NO verts inside the bed-joint V-bands — corners
legal, interiors bridge the groove. Three vert-level snap variants refuted by
the gate (numbers in the flag text of greets_displace_joint_snap, default OFF).
NEXT ROUND BUILDS: the joint-band edge split — for strip/fan faces whose edges
cross a bed-joint V-band (bands from the mip row-profile, machinery already in
the joint_snap block at MeshOps.cpp ~6357), split the edge at the band border
crossings and assign the inserted verts the band's row-median height (the
carve). Reuse: the band construction, the per-vert vSum/hCnt V estimates, the
banded pre-split's edge-splitting bookkeeping (~4234). Watch: fan apex
degeneracy (kBandWidth slivers), UV interpolation must use per-FACE corner
UVs, and the gate is his marks toward 0.0000 with quiet planes staying 0.00.
Instruments added this round: displace_faces.txt (world corners per face, same
--displace_dump pass), tools/bridging_verify.py (pixel→face→plane classifier),
weld column in displace_dump.txt.

## UPDATE 2026-08-28i (the joint-band edge-split round — refuted, attribution sharpened)
The edge-split lever family is REFUTED by the gate in three iterations (full
numbers in greets_displace_joint_split's flag text; all three shipped OFF,
--no- proven byte-exact eeb15725). What the round measured instead:
- [STONE-JSQ] census (junction_census+box gated, MeshOps ~5290): the marked
  faces (f1475 family, y 3.34-4.68) span PLATEAU-to-PLATEAU with their corners
  ON adjacent joint rows — the joints sit at the edge ENDPOINTS, so there is
  no in-face band to traverse, and any corner-in-band exemption skips exactly
  the diseased faces.
- Per-pixel face resolve on the SPLIT arm (bridging_verify vs jsplit raw):
  marked pixels sit on SAME-plane faces with corner excess p90 +0.102u — the
  LINE-REP course level ('E', +0.102 measured on a BED-JOINT row where the
  crossing rule says the mortar minimum should dominate) vs the pointwise
  reference's +0.015 plateau. ~0.09u normal error, ray-stretched at grazing =
  the 0.66u p90 marked juts. This population survived snap x3, clamp x3, and
  split x3 untouched — every lever was tessellation-side while the REP LEVELS
  are wrong.
- NEXT LEVER (not built, needs the coordinator's go): the line-height rep
  assignment. First instrument: dump the E/P rep provenance at the strip's
  joint rows — which LINE contributed the +0.102 rep to a bed-joint vert (a
  vertical head-joint line's plateau ref bleeding onto the bed joint?), and
  why the crossing MINIMUM rule did not fire. Then bound or reclassify; the
  pitfall note's legit excess is 0.02-0.04, tol 0.05.
- Round 2 (groove over-carve, scene-wide -0.0515) remains queued and untouched;
  it cannot be the marks' owner (sign is opposite).

## UPDATE 2026-08-28j (the provenance round — the juts were a RIDE SIGN)
The "+0.102 'E' rep over-raise" reading was WRONG: every marked joint-row vert
carries the correct mortar-floor rep (hEff 0.200/0.208, the crossing minimum);
|dsp| 0.104 was displaced along the ANTI-front on verts within ~1.1u of the
corner because the plane-normal ride (MeshOps ride site, "PLANE-NORMAL RIDE")
resolved planeN's sign by dot with the polluted smoothed normal. Part 5
(`greets_displace_plane_front_majority`, default ON) orients planeN by the
patch majority front; 38 verts flipped scene-wide, all on the pier-front
strip. Marks 28.7→13.5% / 14.9→6.7% protruding; --no- byte-exact eeb15725.
Instruments: [STONE-RIDEPROV] (census+box, at the ride site: smN, planeN,
sRide, sUsed, cls, hEff, dsp, motion along the plane normal) and the
[STONE-PLANEFRONT] flip census (count + bbox, always printed);
tools/mark_faces.py joins his marked pixels to bake FACES (index, corners,
above-plane height) — run it on raw/<arm>_A with a fresh displace_faces.txt
(NOTE: --displace_dump only fires on an OVERLAY frame — snapshot runs do not
write it; the 12:46 Runtime/displace_faces.txt is the last valid one).
REMAINDER at the marks (measured, not chased): median dz now -0.14/-0.17 —
the strip's course-spanning faces sit at the SHOULDER level (hEff 0.419,
dsp -0.038) between correctly carved joints, ray-stretched; plus thin red
rims at course edges (frac 13.5/6.7%). That is the groove/shoulder LEVEL
family (round-2 in the queue), not a sign. Census box for the strip:
FDS_STONE_CENSUS_BOX="16.3,17.95,-58.2,-56.9,1.6,4.75".
