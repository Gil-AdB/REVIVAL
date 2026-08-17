# STONE BORDER PIPELINE REORDER — design (2026-08-18)

The standing fix for the t=5968 pier arris (and every split-vertex corner).
Everything below is derived from measurements taken 2026-08-17/18; the numbers
live in `docs/SESSION_STATE.md` (2026-08-17b and 2026-08-18 blocks) and in the
flag texts of `--greets_displace_profile_agree` / `--greets_displace_band_ladder`.
Implement behind **one** new flag `--greets_displace_border_v2` (BOOL, default
OFF, byte-null by construction — the v1 path must remain the shipping arm until
the full gate battery passes and Gil-Ad's eye approves).

## The invariant being built

One continuous, single-valued height along every corner line, sampled by both
sheets from ONE profile (the agree-MAX collapse, already landed as
`--greets_displace_profile_agree=1`), carried by strips whose cells span one
border pitch, with no stage after the weld allowed to move a welded vert
per-sheet.

## Why the current order cannot get there (each fact measured)

Current order: classify → cell-tessellate → band pre-split (authored scale)
→ border densification recursion → [displace → weld → band-blend] → fold-relax
→ cross-patch pinning.

1. The **recursion** only splits segments whose BOTH endpoints are freed, so
   spans bounded by authored break verts / merged corner ends never densify —
   the weld span stops at the first/last course (rig: weld covers 1.28–7.51 of
   0–8; graze punch-through floor 667 px is exactly this).
2. The **band pre-split** is authored-scale-only. Run before densification it
   leaves the strip's inner side ~30× coarser than the border (the mega-sliver
   fans, [STONE-CORNERF]); run after ANY densification it makes one micro-band
   PER FACE (measured: 1 530 bands, green ×30).
3. **Fold-relax** halves the verts of inverted faces toward base per-sheet.
   It repairs twisted slivers (removing it: rig green ×70, front 154 → 11 290)
   AND it re-splits the weld exactly where the mitre sealed it (scene: the
   cull-stripes; mode 1 3 346 px, +ladder 5 086 px). Both effects come from
   the same lever — under the current strip topology the conflict is
   unresolvable: a sliver bridging a welded step has no non-weld lever.
4. A **post-hoc ladder** (rebuild strips as border-pitch quads) can't fix it:
   its inner side has nothing correct to attach to (the interior boundary is a
   cell polyline, not a chord) and its near-vertical cells feed exactly the
   relax conflict of (3). Adjudicated against at t=5968: 3 346 → 5 086 px.

## The v2 order

The key structural move: **the band strip and the border densify in LOCKSTEP as
quads**, so a strip cell never spans more than one border pitch and every
border step has a matching inner node — then the weld's steps are carried by
near-vertical WALL cells (rotated ≈90° from base, which the relax criterion
`g_disp·g_base < 0` correctly does NOT mark) instead of past-90° twisted
slivers (which it correctly does).

Stages, replacing today's band pre-split + recursion under the v2 flag:

1. **Band pre-split at authored scale, unchanged** — it correctly plants the
   inner polyline at authored features. Keep recording `bandInner` and, NEW,
   record the band PAIRING: for each border vert `a` of authored edge E, its
   inner partner `a2` (map `(vert, E) → inner`, filled in `sideVert`).
2. **Free the interior break verts** (already implemented behind the ladder
   flag — move it to v2): an authored border vert between freed same-line
   edges is freed after an abut-veto re-check, so the weld can cover it.
   [STONE-MITRE-CAND] verifies: no `not-freed` rejects mid-line.
3. **Lockstep densification**: rewrite the recursion so that when it splits a
   border segment (a,b) at m, and (a,b) belongs to a band pair (partners
   a2,b2 known from stage 1), it simultaneously inserts m2 on (a2,b2) and
   restructures the two band triangles into four — the ladder's quad, built
   IN PLACE during densification instead of bolted on after. m2's edge
   (a2,b2) is interior-shared: split the interior face across it too (one
   extra face split per band split — the interior face is authored-scale, so
   this is a plain edge split, not a cascade; T-junction pins cover any side
   the split cannot reach). Allow the one-freed-endpoint case (the end
   courses) — with the lockstep quads there are no orphan fans for it to
   manufacture, which is what killed it standalone (green ×20).
4. **Displace → weld (agree-MAX) → band-blend**, unchanged.
5. **Fold-relax, unchanged criterion** — with the lockstep strips the welded
   steps present as ≈90° walls (unmarked) and genuinely twisted cells have
   band/apex levers, so the weld/relax conflict dissolves rather than being
   suppressed.

## Gates (in order; stop at the first failure)

1. Corner rig, all modes, ladder flag OFF (v2 replaces it):
   `FDS_DISPLACETEST_CORNER=1 ./DEMO --scene-displacetest` with
   `FDS_GREETS_DISPLACE_BORDER_V2=1`. PASS at mode 1 = border-gap max ≤ 0.002 u,
   flips 0 within the corner cylinder, green graze+front = 0. The unwelded-end
   floor (667) must go to 0 — that is the whole point of stage 3's end case.
2. v2 OFF byte-null: default t=5968 == `bf75aa2739c0b6d015ebba2705992953`,
   pin t=5743 == `440aa6bbb350ae95fbacf339dd2ad957`.
3. Scene: t=5968 full umbrella + `profile_agree=1` + v2: pure-black px ≤ 105
   (the pre-campaign floor; target ~0), and the crop must show the channel
   GONE, not smeared — compare `docs/img/fogwt/arris5968_crop_before.png`.
4. The 25-pose battery + acceptance ×4 + greets t=1588 pin + render_gate
   (SDL dummy drivers), quantified per the campaign's rules. Look changes go
   to Gil-Ad's eye before any default flip.

## Traps already sprung once (do not re-spring)

- SceneBuilder rigs wind counter-clockwise; FLD content winds clockwise. The
  abut veto's convexity test reads the difference as concave-vs-convex. Any
  new rig geometry must wind clockwise (see buildCorner).
- `posEdgeCount > 1` (coincident authored edges) pins BOTH sides by design —
  the split-vertex corner only frees because the two sheets' authored corner
  segmentations differ. Do not "fix" that check.
- The relax criterion is convention-free (`g_disp·g_base`); an authored-N
  criterion flattens SceneBuilder rigs entirely.
- `[STONE-FREEV]` prints PRE-weld state; only `[STONE-FINALV]` is the final
  word. Census windows retarget with `FDS_STONE_CENSUS_BOX`.
- greets acceptance pins move if `ssao_downscale`/profiler/camera-env vary —
  use the recipes verbatim from the gates table in SESSION_STATE.
