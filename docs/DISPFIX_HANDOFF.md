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
