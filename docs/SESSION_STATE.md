# SESSION STATE — glass / editor / authoring campaign (updated 2026-07-11)
## 2026-09-02d — HIS RULING: every v4 rung is bad, the OLD bake ships; the defect is the high-angle junctions, and H6194 is dissected

- **His words** (verdict `d98c40a5f377`, answer `02d53f6d37db` on the density card, verbatim): *"all are
  bad. the old bake is very good almost everywhere, apart from where the high angle walls meet"*. v4 stays
  built and default OFF; the campaign lever moves to the OLD bake's junctions. Then: *"this is starting to
  get real old real fast. Please dispatch an agent to suggest how we should attack this issue from scratch.
  not just the code itself, but what approach in general to use for attacking this general issue. and how
  to actually get a test that actually tests for the correct output. and how to actually get the correct
  output."* — a from-scratch proposal agent (`subagent:attack`) is dispatched with everything below; its
  deliverable is `scratchpad/attack/PROPOSAL.md`, to be put in front of him before any code moves.
- **H6194 anatomy** (`71704eb9e2b5`, `ec75f7f0d4d2`, `ad0b8faf2405`): the black wedge at his pose is a
  HOLE — z16==0 at 5273 px, exactly the black pixels. It is the doorway on the west wall x=17.898 meeting a
  **splayed reveal** (authored normal (−0.445,0,−0.896), dihedral ≈116.4°) = **mitre line 8**; the whole
  "high-angle" class in greets is the ten `[STONE-MITRE]` lines (4 sloped near z≈−22, 4 vertical at
  y 3.2–6.3, 2 door reveals). One-stage-off ladder at the pose (unrasterised px): shipping 5273 · bare 0 ·
  `--no-greets_displace_free_edge` 0 (sealed, dead-straight arris) · `--no-greets_displace_mitre` 1444
  (**the mitre makes it 3.7× worse**) · `--no-…seam_weld` 5273 byte-identical · `--no-…plane_normal` 5182 ·
  `--no-…border_pin` 625 · `--no-…fold_relax` 5084.
- **Why the weld is not watertight**: the two sheets sample different UV columns (u −4.0774 vs +0.5000),
  their 105+105 verts interleave 1–4 mm apart, and the shared profile is their UNION (`profile_agree`
  default 0) — double-valued, so each sheet takes its own dsp (course y 1.9–3.2: +0.031 vs −0.03…−0.055,
  polylines 0.08 u apart). In-frame the mismatch is ≤0.037 u yet the wedge is ~0.09 u wide, so which sheet
  is missing there is **open** (`a1588f2a3bda`, waiting on the attack agent).
- **Instrument trap**: `--displace_viz` back-face-culls with the authored face normal and draws nothing on
  flipped-winding walls (the x=17.898 wall). Renders/logs: `scratchpad/oldbake/h6194_*`.
- Prior: rev-dispfix (46 commits, "still a lot of tears") is not merged and its merge question
  `12a396dd0f85` is moot until the proposal lands.

## 2026-09-02e — the from-scratch proposal landed (`docs/JUNCTION_ATTACK_PROPOSAL.md`), and it moves the defect off the crease

- **The agent's headline, MEASURED with a `--face_id_dump` + z16 world decode** (`64e87ccec15d`, `087a63970e4c`):
  the H6194 wedge is a **T-junction hinge crack on the coplanar far-wall/spandrel seam** of the jamb line, not
  a crease failure. The far-wall sheet has no border vertex between the authored y=4.937 and y=6.202, so it
  bounds itself with one chord from the pinned top to the mitred (17.840,4.810,−62.988); the spandrel owns the
  y=4.937 vertex. Gap 0.062 u at the door head → 0.002 u at the top; lip-to-lip p50 0.031 / max 0.122 u.
  69.2 % of hole-boundary pixels have all neighbouring faces within 30°; the splayed reveal bounds ~88 of 509.
  The coordinator's earlier box census shows the same geometry (faces 5463/5464, `[STONE-PROV]` 4.937 pin →
  4.810 free, nothing between) — filed as the answer to `a1588f2a3bda`. The mitre widens the crack ~10×
  (`8e5c22727eec`). **Trap** `8fd6c6d41202`: min triangle-pair distance is blind to a hinge crack — that is how
  rev-dispfix read "watertight at 99 % of hole pixels" on a hole visible across the room.
- **Diagnosis**: continuity is a property of an EDGE; the bake decides per VERTEX (own UV grid per sheet + ~8
  independent classifiers + fold-relax of 1 783 inverted faces). **Correct output** defined bake-free (§2):
  height graph per authored plane; convex → intersection curve, trim both; concave → other branch; coplanar →
  one continuous curve, no ride, no level; the jamb switches class at y=4.937, which must be a vertex of BOTH.
- **Test**: T1 seam-consistency audit on a dumped mesh (per authored edge, symmetric Hausdorff of the two
  boundary polylines, bar **0.000**, T-junctions 0; needs a `--greets_displace_mesh_dump` + `tools/seam_audit.py`),
  T2 coverage vs the bare wall (`tools/v4_tear_cover.py` retargeted, pass 0 at H6194 + the 10 mitre lines + 8
  split seams + controls), T3 an offline Python ray-cast of §2 last. `--greets_displace_ref` is NOT the oracle.
- **Approach**: A — shared seam ladder (one canonical sample list per authored edge, created once, indexed by
  both sheets, displaced once; 0 runtime ms, bake-only) with D (pin the arris, `--no-greets_displace_free_edge`,
  0 holes today, as a one-cell band) as the bisect control. B true-mitre re-trim is refuted by §0 (undefined at
  n₁=n₂); C per-pixel relief is 349–505 ms. **Delete, not tune**: mitre + satellites, fold_relax (once T1=0),
  border_mean ×0.40, seam_weld (byte-identical here), the `--displace_viz` back-face cull, the tri-tri metric.
- **Nothing built yet** — his call on the proposal first. Page (wipe deck of the four H6194 arms + the UV
  strip + the proposal): https://claude.ai/code/artifact/bbb06c85-e255-469f-8e56-c5ff6cb76434
- **New instrument, his ask** (`53c493538584`): `--uv_viz=1..4` paints each pixel's diffuse texel column/row
  (decoded from the G-buffer txtr word, texel-exact): 1 R=u G=v, 2 8×8 checker, 3 u saw, 4 v saw; in the X
  cycle; `--mat_probe` appends `uv u,v / W×H mipN`. Byte-null off: H6194 `36cd059a…`, cam A `d92cb6f5…`,
  render_gate 4/4. At H6194 the UV is continuous across the door-head seam where the mesh is not.

## 2026-09-02f — HIS READING of the UV viz: "the uv at the seam is not continous" (`8295209b2e97`), and the census that localises it

- **MEASURED** (`34058c4a916f`, `tools/uv_seam_census.py` over the H6194 G-buffer dump, decode verified against
  the on-screen `--mat_probe` readout at (960,540) = 860,227 mip0, silhouettes excluded by |dz| < 0.05 u):
  of 83 181 world-adjacent stone seam pixel pairs, every SAME-material seam is UV-continuous — coplanar
  (36 052 pairs) du p90 3 / max 10 texels, 5–30° (19 388) max 12, **crease ≥30° (27 241) p90 2 / max 10.5** —
  against an in-face pixel gradient of p99 5 texels. Every one of the 500 `rooms` | `rooms::mirUV` BOUNDARY
  pairs steps **405–443 texels in u (0.40–0.43 of the 1024 tile)**, dv ≤ 4, fraction > 32 texels = 1.000. Named:
  the door jamb x=17.9 z=−62.94 (mirUV u0 −4.077 vs rooms u0 +0.497, 434–438 tx, dihedral 48–79°) and the pier
  x≈9.96 z≈−59.0 (rooms −0.819 vs mirUV −3.422, 403–407 tx, dihedral 6–31°, shared vertex on both sides).
- **What that boundary is**: `GreetsFixBitangentHandedness` (GREETS.CPP:1524, `--greets_tbn_fix`) re-points every
  negative-UV-determinant face onto a `<mat>::mirUV` clone and changes NO UV — so the step is the AUTHORED
  projection phase between a mirrored sheet and its unmirrored neighbour, ≈0.4 tile at both places. Consequence:
  only those seams sample different height columns on their two sides (the double-valued profile
  `ad0b8faf2405` is specific to them: mitre lines 8–9 and any line crossing a handedness boundary); every
  same-family crease already samples the same column both sides. The T-junction hinge crack (`087a63970e4c`)
  is a separate, coplanar, same-family defect — UV-continuous, vertex-discontinuous.
- **"the viz only shows once"** (`5c093641b31e`, OPEN, waiting on him): NOT reproduced on the snapshot path —
  one process rendering `--snapshot=greets@t=6194,6195,6230 --uv_viz=2` paints the checker on all three frames
  (98.3 % of pixels each, `fe27618c4b3c`). The live X-cycle path has no headless frame dump for greets. Need from
  him: CLI flag or X cycle, live window or snapshot, and whether "once" means one frame or one tile gradient
  across a 6 u wall (u = z/6, so a 1024-texel tile is 6 world units wide).
- Modes, for the record: 1 = u fraction in R and v fraction in G across the tile (one gradient per 6 u);
  2 = the tile cut into 8×8 cells, alternating light/dark, so a jog or a step reads as a broken checker;
  3 = u only, folded into 16 saw-tooth stripes per tile (each stripe 64 texels, so a 0.4-tile step is ~6 stripes
  of phase); 4 = the same for v (the course direction). Dark grey = nothing rasterised, mid grey = no diffuse
  map or forward-rendered.

## 2026-09-02c — his verdict on the tip is a FAIL; the density ladder is measured and on his deck

- **His words** (verdict `43494afa5f5d`, verbatim): *"under the latest v4, walls are still wobbly, mesh
  does not conform to the heightmap, and there are no view-through-adjacent-walls..."* — a fail on all
  three, and the look ruling P4 was stopped at.
- **Points 1+2 are one measured mechanism** (`256d9f38b114`): at `--v4_cpb=1` a block interior owns no
  vertex, so every plateau is covered by triangles whose corners are mortar-deep — the block face bakes
  as a skewed plane 0.106 u below the field. The 2× crop at cam A shows exactly that: tilted faces with
  diagonal seams. **The ladder, re-measured at the tip with the rings on** (`ae78e6541f01`, per-arm
  `greets.displace.v4.relief.plateau_bias {state=P4tip}`), cam A, interleaved min-of-5:

  | rung | faces | plateau core e−r p50 | min-ang p10 | frame ms | Δ vs shipped |
  |---|---|---|---|---|---|
  | shipped `--v4_cpb=1` | 61 430 | −0.106 u | 2.4° | 48.63 | — |
  | `--v4_block_mid` | 76 680 | −0.017 u | 6.2° | 48.54 | −0.1 |
  | `--v4_cpb=2` | 111 721 | −0.023 u | 7.9° | 50.54 | +1.9 |
  | `--v4_band_union --v4_cpb=2` | 148 255 | −0.003 u | 6.3° | 52.40 | +3.8 (5 known pinholes) |
  | `--v4_cpb=4` | 229 826 | +0.000 u | 15.1° | 55.87 | +7.2 |
  | old bake (v4 off) | — | — | — | 50.50 | +1.9 |

- **Ruling card `a6816582fd62`** (`greets.displace.v4.density_rung`, waiting on him): the deck
  https://claude.ai/code/artifact/af0a33e5-8c7a-4638-8c0d-adfb695a3cbe wipes each rung against the
  shipped one at cam A, the corner pose and the panel pose, full frame and 2× crop, and builds the
  answer command. Whatever he picks becomes the default under `--greets_displace_v4` after the gates.
- **Point 3 is not in the ladder**: no recess / no view-through at wall boundaries is his earlier
  `98b9eba830ec` + `7cec6b7a171d`; P4 unpinned the crease vertices but 104 of 155 corners and 520 of
  1 906 border samples are still held (abutments → `--v4_ring_abut`, unmeasured; wall-to-ceiling →
  P5 skirts). Separate lever, next after his rung.

## 2026-09-02b — P4 junction rings merged (3a665e7a), STOPPED AT ITS GATE; the city single flip did not reproduce

- **P4 is built and merged** (rev-p4 pinned `7978ab5c`, `--greets_displace_v4` default OFF, `--v4_rings`
  default ON inside it; `docs/DISPLACEMENT_V4_DESIGN.md` §P4; ledger `b2dedcb218bd` supersedes the
  proposal). Coordinator re-ran every gate on a fresh build of the pinned SHA: `render_gate` 4/4,
  ovec 12/12, cam A off-arm `d92cb6f5…` = the 24-run pin, on-arm `79b9e309…` 3/3 (agent 24/24),
  P3 control `--no-v4_rings` `7c259253…` = P3's own md5, lint 0 ERROR, tears 14 ≤ 15 with the wall
  ends at 0. The FPCR fix (`ff76525d`) is inside the merge and byte-null there.
- **Two exit criteria FAIL, stated**: faces 85 566 vs the 84 742 budget (+0.97 %, exactly the 824
  groove-crossing border samples the round exists to place); dz p90 < 0.08 u **refused as
  unreachable** (`b3b2139d736c`): the `--flat-deg` mask is the block interior P4 never touches, and
  the ring vertices move p50 0.016 u against a 0.15–0.35 u residual that is P3's interior recession.
  Instrument trap `a149cc880bbe`: 13.6 % of corner6097's crease band is reference-coverage
  disagreement, identical in both arms. A stale committed picture (`docs/img/p4/camA_p4.png` at a
  pre-fix SHA) was caught by provenance and all eight re-rendered at the tip.
- **Whether the wall boundaries now look recessed is his eye's call** — nobody has claimed it.
  Fly: `cd /Users/gil-ad/work/revival-fog/Runtime && ./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --greets_displace_v4`
  (add `--no-v4_rings` for the P3 control).
- **City single flip closed as a measurement** (`6e2be8b5e19b` supersedes `92c790078de2`): 1 of 18
  observations under numpy contention; 48 sequential dev runs launched during the P4 gate battery
  read `bd4ffbf8…` ×48, 48 M5 runs read `0debc80a…` ×48 — 96/96 deterministic. If it recurs, keep
  the PPM and sidecar of the differing run.
- Still held at 104 of 155 corners (48 abutments, 56 against non-baked faces); `--v4_ring_abut`
  lifts the first half, unmeasured.

## 2026-09-02 — THE M5 DIVERGENCE IS FOUND AND FIXED: FPCR.AH (FEAT_AFP), one line in `FPU_LPrecision`

- **Cause** (ledger `10e91a31d5d8`, `platform.m5.divergence_mechanism`): `FDS/Base/FDS_VARS.H`
  `FPU_LPrecision()` wrote FPCR = `FZ|AH` on every thread. `AH` (bit 1) is ARMv8.7 FEAT_AFP
  "alternate handling". The M2 Max lacks FEAT_AFP — the bit is RES0, silently dropped, every pin
  was measured under plain `FZ` (denormal inputs AND outputs flushed). The M5 Max honours it, and
  with AH=1 **FNEG/FABS leave a NaN's sign bit unchanged**, FZ flushes outputs only, FRECPE/FRSQRTE
  precision changes, FMIN/FMAX NaN propagation changes. The bite: clang materialises
  `g_rasterXExtent = {INT32_MAX, -1}` as `mvni.2s v0,#0x80,lsl#24 ; fneg d0,d0 ; str d0` — a NaN
  bit pattern negated — so on the M5 `hi` stayed INT32_MAX, the raster's max-update never fired,
  `cx1=(hi+1)<<3` overflowed and **every transparent-clump composite range was empty**: the greets
  text panels and the fountain spire orbs were rasterised but never composited. Found by the
  same-binary protocol (his call: "just run same binary on both"), the `FDS_XPAR_TRACE` census
  (154 identical clumps, `filled=0` on the M5), address prints refuting the thread-local-duplicate
  hypothesis (same thread, same address, different VALUE), the disassembly of the arming store,
  `scratchpad/fpprobe/ahprobe.c` (M5 AH=1 → `fneg(NaN)` UNCHANGED, AH=0 → FLIPPED; M2 cannot set
  the bit; fresh threads FPCR=0 on both), then an in-demo FPCR readout: M5 workers `0x1000002 AH=1`,
  dev `0x1000000 AH=0`.
- **Fix**: `SetFPCR(base | FZ)` — AH and FIZ cleared, reason in the comment; `docs/BUILDING_WINDOWS.md`
  x86 analogue corrected to FTZ=1 DAZ=1. **Dev is byte-null**: `tools/render_gate.sh` 4/4,
  `tools/ovec/gates.sh` 12/12, greets t=1043 `f96caaf9…` and fountain t=500 `4d62d581…` unchanged.
- **The M5 converges on the same binary** (dev build shipped as `Runtime/DEMO_dev`, `install_name_tool`
  to `/opt/homebrew/opt/sdl2`, ad-hoc `codesign`): greets t=1043 `cbe52ab0…` → `f96caaf9…` = dev,
  **24/24** (`cac95d63b7c9`); fountain t=500 `048f3905…` → `4d62d581…` = dev, sweep t=100..1200
  identical 5/5 (`31c3617e958f`, `172cb8803514`); `render_gate.sh` 4/4 at the dev hashes where it
  used to read 4043aaaa/88dd4632/ffa28566/1a98cdf6 (`0a6f5548d79a`); ovec 11/12 rows equal to the
  dev pins; **cam A t=5965 `d92cb6f5…` = the 24-run pin** (`2ce6effc5f0a`) — the class-1 Piramid
  face hole (`80f240d51a18`) and the three missing cyan omnis (`d6abf45f834e`, his `m5_diag4` run
  is moot) are gone, in the judging arm and both bare candidates.
- **Still apart**: the plain city pin row (`FDS_CITY_ENV_PIXEL=1 city@t=1961 --deferred`) reads
  `0debc80a…` on the M5 vs `bd4ffbf8…` — deterministic, cache and thread count excluded on both
  machines, 3.358 % of pixels, max 18, upper frame only (`ad4012153bce`; open `1dec19addb82`,
  waiting on coordinator, z16 cross-section first). The three city-acc arms ARE identical.
- **Global trap** (`~/.groundwork` `5158416a130a`): never set FPCR.AH; clang's `mvni+fneg` int-pair
  idiom is only an integer store under AH=0.
- **What the M5 needs from him**: `cd /Users/gil-ad/REVIVAL && git pull origin fog-wt && cmake --build build`,
  then his own fly-through of greets and fountain. Diagnostic hooks kept, all env-gated
  (`FDS_XPAR_TRACE=1`: `[XT-FACE]`/`[XT-EXT]`(+FPCR)/`[XT-RAST]` and the xtrace census from the
  snapshot path; `[GREETSSNAP-DIAG]` camera planes).

## 2026-09-01 — his M5 round filed; fountain G key; the corner-rule pictures were never evidence

- **M5 transparent faces**: missing in BOTH arms at greets t=1043 (his answer); the dev box renders the
  same recipe with the text panels present (`docs/img/m5/xpar_t1043_dev.png`, md5 `f96caaf9…`). The
  xpar surfaces there are the half-silvered `screen2` text panels (columns are opaque marble — his
  correction, verified in code; dispute e759f25d4440). Next: his M5 headless snapshot md5 + the
  interactive grab with `FDS_XPAR_TRACE=1` — **press G, not F9** (F9 is Mission Control on macOS;
  greets reads F9 OR G). Cards `platform.m5.greets_transparent_faces.m5_frame`,
  `platform.m5.fountain_missing_polys` carry the M5 command lines.
- **Fountain**: his sighting = polys vanish ~0.5 s into the scene. The unconditional stream-head
  H-labels (since b3d8142b) are REMOVED at his ruling ("remove the h1/2/0/w/e"); in their place
  **G** dumps `/tmp/fountain_dump_N_tT.ppm` + sidecar + xtrace and prints `[FOUNTAIN-CAM]` with
  `FNTSNAP_POS/FWD/FOV` and `t` — the exact headless repro form. Fountain t=2500 ovec pin moves
  `8db68ccb…` → `0222f903…` (label pixels; greets t=1588 unchanged in the same run).
- **Trap**: `--snapshot` rows without `--force_xres/--force_yres` render at `Runtime/rev.cfg`
  resolution, found from the REAL executable path (REV.CPP:526) — a locally edited cfg (1384×768 on
  the dev box) fails every ovec row with no tree change. `tools/ovec/gates.sh` now forces 1920×1080.
- **Corner rule (9e7cde227316)**: the committed `junction_*.png` pairs had one side 2.4× darker
  (composite artifact — he caught it). Fresh renders: identical means; the arms differ in 638 /
  1 643 px, none at the H6194 crease; **ref_step_px = 0 at all 8 battery poses** — no pose shows a
  step. Rulings now go through the wipe-compare deck
  (https://claude.ai/code/artifact/4438b364-1c0c-4518-878b-02a71d8e1a73), never loose images.
- World-UV arm: ruled "both are shit" (ef6f65c1348f) — stays default OFF; the campaign lever is P4
  pinning, gated on corner_rule + membership.

## 2026-08-31b — knife-edge landed (both hypotheses refuted); the queue is live

- **M5** (rev-knifedge merged, 1d975e61, NO engine source): class 1 is ONE near-plane-clipped
  triangle (Piramid.lwo:c0 face 0) with 2^22-ULP margins — no knife edge to guard; the ±1-quantum
  speckle is explained (iNearZ=100 near-clip blowup, ~100 ULP view-z reconstruction). Class 2 is
  THREE MISSING OMNI LIGHTS (114/115/116, cyan mech) on a bit-identical G-buffer. Decisive next
  step is HIS run on the M5: `bash tools/m5_diag4.sh` (queue 951fa2dce8de); prime suspect
  `--light_rect_exact`.
- **The queue**: pending questions/decisions are ledger records now — `groundwork queue` in the
  terminal, test-command copy buttons + per-option ANSWER buttons in `groundwork ui --open`
  (`groundwork answer <id> --value <option>` files his decision, no quote ceremony). Six items
  wait on him: 3 v4 rulings (d746f3373da0 / 9e7cde227316 / e325bd082cbc), dispfix merge
  (12a396dd0f85), GpuBench M5 build (362f18df7f23), the m5_diag4 table (951fa2dce8de).
- v4 P2 agent still running on rev-v4.



> ## 2026-08-31 — SESSION HANDOFF (pre-compaction): three agents in flight, the M5 hunt
> localized to two knife-edge amplifiers, v4 at P2, GpuBench OBJCXX fix landing
>
> **Resume protocol:** `groundwork status` from this tree, then this entry. The ledger
> (267 records) carries every measurement/elimination chain — `groundwork query
> platform.m5.missing_polys --all` is the M5 dossier; `greets.displace.v4` the rewrite.
>
> **RUNNING (background agents; results arrive as task notifications):**
> 1. KNIFE-EDGE (worktree rev-knifedge): the two M5 amplifiers — (a) whole-floor-chunk
>    cull flip (115,416 px, bare arm, bbox x[1040,1919] y[818,1079], the floor before the
>    pier base; find the acceptance test within ~1 ULP via 24x 1-ULP camera jitter, guard
>    band, flag default ON) and (b) co-planar ownership tie-break (22,860 mat flips on the
>    displaced arm = the ceiling water-vs-base z-fight; deterministic tie-break). On report:
>    verify, per-pin deltas are NOT re-baselined without his word; if the fix changes
>    current-machine pixels STOP for him.
> 2. V4 P2 (worktree rev-v4, branch tip 7c621be8 = P1 done, gate 8/8): the undisplaced
>    per-chart lattice under R1/R2/R3. Two-tier gate: byte-identical-to-bare is the target;
>    if only <=1-quantum + zero raster/ownership flips, STOP for coordinator sign-off before
>    P3. P1 corrected the design: bake set = 226 tris / 1 mesh, 0 free edges (130 abutments,
>    74 T-junction class), 142 junctions (453 was with mirror clones), charts exactly planar.
> 3. GPUBENCH build-verify chain (background shell): the OBJCXX fix (LANGUAGE CXX + -ObjC++
>    dies under CMP0119 on fresh CMake caches — the M5 build failure). If the link fails,
>    fix before the commit/push that chain performs.
>
> **WAITING ON GIL-AD:** (a) M5: pull + the GpuBench rebuild command he was given; (b) fly
> chase/city under his all-scenes GTAO ruling (pairs in docs/img/gtaoedge/allscenes/);
> (c) the three v4 rulings — membership (assumed union), corner rule (assumed census:
> dominant on the 73% phase-shifted, steps on the 21% unrelated), the 21% (assumed accept)
> — needed before P4 only; (d) whether rev-dispfix (old-bake taper, "still a lot of tears",
> corpus state 13) merges as the interim shipped path — less broken, not fixed.
>
> **THE M5 THREAD (all in the ledger chain):** same binary + same OS + identical FP env
> (FPCR, denorms, full FRECPE/FRSQRTE tables, mass libm sweeps) + assets/caches eliminated
> (pristine tree bit-identical) + worker count eliminated (z16 identical at 1/5/12) — yet
> deterministic divergence: ±1-quantum z16 speckle (~10k texels) plus the two amplified
> defects above. The ±1-ULP ORIGIN is unexplained and PARKED; the amplifiers are the fix.
> M5 ground truth planes: incoming_m5/m5planes/ (bare/disp z16, mat, color). Reference
> outputs for cross-machine probes: docs/data/{libm,fpenv,neon_est}_*_m2max.txt,
> planes_*_m2max.txt; probes tools/{libm,fpenv,neon_est}_probe.c, m5_diag*.sh.
>
> **DECIDED THIS SESSION (ledger):** GTAO guarded-geometric default ALL scenes, bias 0.2
> (decision f2f179f017df; pins re-baselined 24/24; his "much better"); the bake REWRITE
> (f2696d3c8aa7) with design docs/DISPLACEMENT_V4_DESIGN.md and survey
> docs/DISPLACEMENT_LITERATURE.md; the conclusion-ledger tool GROUNDWORK
> (github.com/Gil-AdB/groundwork, 45 fixtures, hooks installed locally, edit-guard
> block-once ON) with CLAUDE.md section as the protocol.
>
> **TREES:** fog-wt = everything merged (flag cull -13, provenance sidecars, semgrep,
> force_xres/yres, shadow_polyid fix, reference renderer both rounds, GTAO all-scenes,
> ledger 267). Unmerged: rev-dispfix (old-bake fixes, his call), rev-v4 (P1+P2 in flight),
> rev-knifedge (in flight). rev-m5pin = pinned 03ef825e reference env, KEEP. rev-gtaoedge
> merged (prunable). His rev.cfg (1384x768) stays uncommitted-dirty by design.

> ## 2026-08-30 — **THE GTAO DEFAULT FLIP, ON HIS RULING: the guarded geometric
> arm is the default for ALL SCENES.** Four pins re-baselined, ten verified
> unmoved, and the pre-2026-08-30 render is still reachable byte-exactly from
> ONE flag
>
> He flew the guarded geometric arm with `--ssao_gtao_bias=0.2` and ruled,
> verbatim, **"all scenes"** (ledger decision `f2f179f017df`). What flipped:
> `--ssao_gtao_step_dist` 0 → **1** (geometric ladder), `--ssao_gtao_srad_max`
> 256 → **1024** (the cap stops binding at greets' review poses, so the AO scale
> is the authored world radius again instead of a depth-proportional artefact of
> the pixel grid), the AUTO self-occlusion bias 0.05 → **0.2**;
> `--ssao_gtao_round_fix` was already ON. Both near-step guards stay AUTO
> (`-1`) — on exactly when the ladder is front-loaded.
>
> **THE BIAS DEFAULT IS STILL SPELLED `-1` IN `FeatureFlags.def`, AND THAT IS THE
> POINT.** A literal `0.2` would apply the guard to the UNIFORM ladder too, and
> that is **not** byte-null there — measured: greets acceptance t=5743 goes
> `a7fa9214…` → `aa08f5b1f5594250407ddbb3f14cbd20` under `--ssao_gtao_bias=0.2`
> with `--ssao_gtao_step_dist=0`. Keeping AUTO and moving the AUTO *value* is
> what buys the exact revert arm. AUTO now resolves to **0.2** with a
> front-loaded ladder and **0** with the uniform one.
>
> **THE REVERT ARM IS EXACT, PROVED ON THE WHOLE 14-PIN BATTERY.** The shipped
> binary under `--ssao_gtao_step_dist=0 --ssao_gtao_srad_max=256
> --no-ssao_gtao_round_fix` reproduces the countersigned 2026-08-16y greets
> acceptance values (`440aa6bb… / 00d17bc5… / 29c1e7fb… / bc1b0a8a…`) and every
> other pin byte-identically; greets **cam A t=5965** under that arm gives
> `dbe2d4c2716e2d762b366d5a45c151a6`, the value already on file at
> `docs/DISPLACEMENT_RESEARCH_II.md:1519` from months earlier; and chase's five
> pinned poses under **his SSAO arm** are byte-identical to the pre-round
> binary. Drop only the `--no-ssao_gtao_round_fix` term and you get the
> intermediate set the tip actually rendered between the round-fix commit and
> this one (`a7fa9214… / 8593c712… / 4ea379d0… / 03160c82…`), which was
> deliberately never pinned.
>
> **MOVED (4 of 14) — re-baselined:** greets acceptance t=5743
> `08f0741fb8c5d324a2303fb03ed23437`, t=2845 `84e18552fa079409a3da12c4b922bf77`,
> t=6097 `0cc05784dd3e9e448e3ad06df77180bc`, t=6133
> `456eac7dc6d6b2135f0e975e11b3a4fe` — **24/24 identical each, 0 flips**. Plus
> the warm suite's `greets-warm` row.
> **UNMOVED (10 of 14) — VERIFIED 3/3, not assumed:** city t=1961, greets
> t=1588, fountain t=2500, chase default ×5, chase cinematic ×2. Also unmoved:
> the three city acceptance arms, `render_gate.sh` 4/4 (`groundwork recheck
> --subject pin.render_gate`: 12 reverified, 0 disagree), and six of the seven
> warm rows. The mechanism is structural: greets-acceptance and greets-warm are
> the **only** recipes in the entire suite that pass `--ssao --ssao-gtao`, so
> nothing else can enter the changed code.
>
> **COST — min-of-11 interleaved, both arms on ONE binary, `iters=40`,
> 1920×1080, `--deferred_prof=5`, `wall_min`:**
>
> | scene / pose | renderFrame | `ssao` | `ssao-march` |
> |---|--:|--:|--:|
> | greets t=5743 | 46.240 → 46.367 ms **+0.27 %** | +3.69 % | +5.15 % |
> | chase t=800 `@main` | 19.630 → 19.634 ms **+0.02 %** | −0.99 % | −1.77 % |
> | chase t=800 `@refl` | 16.601 → 16.649 ms **+0.29 %** | −1.72 % | −2.67 % |
> | city t=1961 | 56.159 → 56.228 ms **+0.12 %** | +2.18 % | +2.91 % |
>
> **Every renderFrame figure is an order of magnitude inside the ±1.5–2 %
> placement floor**, and the greets one is not even sign-stable: a second
> 11-round battery on the same binary minutes earlier gave **−0.64 %**. That is
> what "inside the floor" means, and it is why the `+1.40 %` on file for the
> 0.05-bias arm (`f555f3979175`) is superseded — its *row* numbers reproduce to
> a tenth of a point, its frame headline does not. **chase's SSAO rows get
> CHEAPER**, which is not a contradiction: at cap 1024 the march is ~4× longer
> in screen space, far more samples land off-frame, and an off-frame sample
> skips the gather, both `gtaoAcos` calls and the bitmask build. In greets'
> tight interior the guards outweigh that saving; in chase's open corridor they
> do not.
>
> **WHAT HIS NEXT FLY WILL SEE — chase and city have only ever been flown under
> the OLD arm.** Old-vs-new on one binary, his SSAO arms, 1920×1080:
>
> | pose | px moved | mean \|Δ\| on moved | max |
> |---|--:|--:|--:|
> | chase t=100 | 218 499 (10.54 %) | 2.56 | 117 |
> | chase t=400 | 209 190 (10.09 %) | 4.03 | 107 |
> | chase t=800 | 338 442 (16.32 %) | 4.48 | 119 |
> | chase t=1200 | 1 023 767 (49.37 %) | 2.59 | 118 |
> | chase t=1600 | 35 600 (1.72 %) | 1.69 | 255 |
> | chase t=1105 | 1 789 422 (**86.30 %**) | 2.69 | **17** |
> | city t=1961 | 926 178 (44.67 %) | 3.94 | 145 |
>
> chase t=1105 is the most-moved pose in the campaign and its max is 17/255 — a
> broad low-amplitude lift, the signature of the AO *scale* changing rather than
> a defect appearing. The city diff shows the change reaching every opaque
> surface and **not** the water, which is the expected shape. Images:
> `docs/img/gtaoedge/allscenes/` (`chase_t800_{before_olddefaults,after_newdefaults,diff}.png`,
> `chase_t1105_*` at 960×540, `city_t1961_*`).
> **NOTE the city PIN is blind to this change** — its recipe carries no
> `--ssao`. Judge city's AO from the acceptance arm plus his SSAO flags, never
> from the pin.
>
> ## 2026-08-29 — **`Render_SSAO` HAS AN INTERIOR AT LAST**: five wave scopes
> replace one, the inferred split is CONFIRMED, and the march's per-lane slice
> setup goes 4-wide BIT-EXACT — `ssao` **−16.4 % at chase**, −9.3 % at greets
>
> The row was 26.3 % of chase's `renderFrame` and had **one profiler scope with
> no `effPar` at all** — it dispatched with `dispatchIndexed(..., nullptr, ...)`
> and joined on a bare `tileDone.acquire()` loop, so its march/apply/blur split
> had only ever been INFERRED from the `--ssao_downscale` slope. Five scopes now
> exist. **The stamp must be taken BEFORE the dispatch** (TailProf.h's own drain
> contract); putting the drain inside the lambda prints `0.00 calls/f`.
>
> **THE INFERENCE WAS RIGHT** — march 5.712 ms measured vs ≈5.8 inferred, apply
> 1.756 vs ≈1.9, blur 0.272 vs ≈0.33, and the scopes cover 99.3 % of the row, so
> there is no hidden fourth block. What the inference could not give is what
> chose the target: **`effPar` 9–11 of 12 (no serial bottleneck) and IPC — the
> apply runs at 5.30, near the core ceiling, the march at 3.26.** The march is
> 64.5 % of the instructions and 73.3 % of the time; it is the one that stalls.
>
> **TWO CANDIDATES REFUTED BEFORE ANY CODE.** The cone round's arm64
> `_mm256_movemask_ps` defect is **not present** — zero movemask sites in
> `DeferredSSAO.cpp`, and the 32-sector bitmask's eight scalar popcounts are
> **already vectorised by clang** into 2× `cnt.16b`. And sky waste is nil:
> ALL-SKY groups **0.00 % in greets**, 5.08 % in chase, scalar-tail cells **0**.
>
> **LANDED (S1):** the per-lane slice setup — the one item §00l called never
> attempted — priced by a new `-DFDS_SSAO_DIAG` ladder at **22.5 % of the march**
> (135 instructions per lane×slice, 1.04 M a frame), now 4-wide in **plain NEON**
> so `fast_rsqrt`'s vrsqrte+1-Newton is exact. Predicted −0.09 to −0.105 Gi/f;
> **measured −0.092.** greets march 5.781 → **4.827 ms**, `ssao` 8.028 →
> **7.280**; chase march 8.394 → **6.547 ms**, `ssao` 11.279 → **9.432**.
> `ssao-apply` and `ssao-blur` unchanged to the digit.
>
> **THE DURABLE HALF — three FMA-contraction rules, and a verify harness.** The
> first build failed ONE pin, so `-DFDS_SSAO_VERIFY` ran the scalar behind the
> vector counting mismatches PER TERM, and each fault was settled by compiling
> the scalar expression standalone and reading its assembly: (1) `a*b - c*d` is
> ONE `fnmsub`; (2) for `A+B+C` all products clang chains from the **SECOND**
> term, `fma(C, fma(A, mul(B)))` — starting at A moved 26 % of lanes; (3) a
> trailing `x*poly` feeding an add/sub is **never materialised alone**
> (`halfPi - a*poly` is one `fmsub`) — rounding it separately cost 32 196 lanes.
> Final: **0 mismatches in 1 036 800 lanes at two poses, every term.**
>
> **NOT TAKEN:** `atan_approx_x8` exists and uses `_mm256_rcp_ps` where the
> scalar divides — faster, and it moves AO values. That is a look call in the
> same family as the 8-wide GTAO rsqrt item already in his stack (2026-08-17a).
>
> **GATES: 13/13 pins + `render_gate` 4/4 at every step.**

> ## 2026-08-28b — **ROUND 2 ON THE SAME KERNEL: the env bilinear fetch goes
> 8-wide and city's `lighting-w1` reaches −27.5 % / `renderFrame` −6.4 %
> cumulative — and round 1's own "2.9 % left on the table" is REFUTED**
>
> **THE FLAG COLLAPSE IS A REFUTATION, NOT A WIN.** Round 1 predicted that
> collapsing the four dials to flagless would return the 2.9 % its ALL-OFF arm
> carried. Measured: **−0.19 to −0.27 % of the row**, i.e. the ±0.14 % Ginstr
> floor. Clang had already hoisted the loop-invariant bools out of both loops —
> the disassembly loses exactly **two `adrp`** (the flag-array address
> materialisations) and one callee-save pair, while static size *rises* 3677 →
> 4051 because constant predicates let it specialise more. The 2.9 % was the OFF
> arm executing the slow paths plus LTO layout. Kept anyway (byte-null, simpler
> body), now as `-DFDS_OVEC_HATCH=ON` rebuild arms. `--deferred_ovec_nomirror`
> is the ONE dial kept live: it is read once per TILE and it is the only way to
> force the `kMirror == true` instantiation from a shipping binary.
>
> **THE LADDER RE-RANKED ITSELF, which is why round 2 went where it did.** Round
> 1 shrank everything around the pack loop, so the pack GREW as a share: 24.8 %
> → **29.9 %** of the row (0.221 of 0.739 Gi/f), second only to the omni loop's
> 45.5 %. A new `-DFDS_OVEC_ENVDIAG=n` ladder split it — the two per-lane
> `EnvCubeFetchBil` calls are **5.1 % of the row**, the live-water tilt 3.4 %,
> the face pick 1.9 %, the rest of the lane loop 19.5 %.
>
> **THE CENSUS CHOSE THE SHAPE BEFORE A LINE WAS WRITTEN.** Of the groups
> carrying a vec-env lane, **90.2 / 95.9 / 96.2 %** have every such lane on ONE
> cube face AFTER the live-water tilt (t=2400 / t=400 / t=1961), at 7.4–7.6 env
> lanes per group, and **100 % of lanes need BOTH mip levels** — always two
> fetches, ~498 k a frame. Same-face implies same-mip 100 % of the time.
>
> **LANDED: C9** (`EnvCubeFetchBil8` — one bilinear for eight lanes sharing a
> face and a level; the face pick and live-water tilt hoisted into a pre-pass so
> the uniformity test can see all eight answers; mixed-face groups fall back to
> the scalar fetch reading the pre-pass's own face/uv, so never worse) and
> **C10** (the 8-wide pack extended to env groups — C7 only ever fired for the
> 58–63 % with NO env lane). Predicted −3.4 % and −1.5 % of the row; measured
> **−2.1 to −3.2 %** and **−1.4 to −2.2 %**.
>
> **BYTE-EXACT ON THE FIRST TRY, no tuning**, on three details worth keeping:
> `u*fr - 0.5f` is a CONTRACTED fmsub under `-ffp-contract=fast`;
> `if (px < 0) px = 0` must be `_mm256_max_ps(zero, px)` and NOT
> `max_ps(px, zero)`, because maxps returns its SECOND operand when unordered and
> the scalar leaves a NaN alone; and the inter-level lerp keeps `lf` PER LANE,
> because `lvlF` can differ inside one integer level even when `lvl0` agrees.
> C10 additionally reproduces the scalar's ORDER OF ROUNDING — truncate each
> term, clamp the INTEGER sum — which is deliberately different from C7's
> float-clamp-before-convert.
>
> **CUMULATIVE, rounds 1+2 against `e017d611`**, both binaries in one worktree,
> interleaved min-of-5, Ginstr floor ±0.14 %: city t=1961 `lighting-w1`
> **0.978 → 0.709 (−27.5 %)** and `renderFrame` **4.184 → 3.915 (−6.4 %)**;
> t=400 **−30.7 % / −7.1 %**; t=2400 −19.2 % / −3.3 %; fountain −14.3 %;
> **greets −0.14 %, at the floor** — the control that keeps proving the work is
> confined to the OuterVec kernel.
>
> **GATES: no pin moved, at any step.** 13/13 pinned poses + `render_gate` 4/4
> after the flag collapse, after C9 and after C10.
>
> **NEXT:** the pack loop's remaining "everything else" (19.5 %, and C10 has just
> taken a bite the next ladder run should re-measure), then the live-water tilt
> (3.4 %) and face pick (1.9 %), both now sitting in C9's pre-pass as per-lane
> scalar loops and both 8-wide-able. The omni loop is still 45.5 % but C2/C4 took
> ~29 % out of it and what is left is genuine per-(pixel × light) arithmetic;
> the only structural idea remaining there is transposing to 8 lights × 8 pixels,
> which is a different kernel, not a lever.

> ## 2026-08-28 — **CITY'S OUTER-VEC LIGHTING KERNEL, THE FIRST ROUND EVER RUN
> ON IT: `lighting-w1` −24.2 % instructions, `renderFrame` −5.7 %, six byte-null
> levers, 13/13 pins and `render_gate` 4/4 — and the single biggest candidate is
> killed by its own census**
>
> `Render_DeferredLighting_Tile_OuterVec` — the kernel city, fountain and crash
> select through `Scene::PreferOuterVec` — was 22.9 % of city's `renderFrame`
> and had **no instrument of any kind**: all four existing ablation ladders live
> inside `TileT` / `TileFill`, which it never enters. This round built
> `-DFDS_OVEC_ABLATE` / `-DFDS_OVEC_OMNI_ABLATE` / `-DFDS_OVEC_CENSUS`
> (compile-time, `if constexpr`, cumulative sink — a *runtime* predicate in this
> body cost 16l +4.3 % with the flag OFF), then measured before choosing.
>
> **THE CENSUS KILLED THE BIGGEST CANDIDATE.** The analysis's C6 (give the
> `wantSpec` redo lane a specular-only loop) predicted **~18 % of the row** at an
> assumed *f* = 0.30 and set its own kill line at *f* < 0.08. Measured *f* — the
> fraction of alive lanes wanting the scalar redo — is **0.91 / 3.25 / 1.06 %**
> at city t=400 / t=1961 / t=2400. Worse for it: the redo only *duplicates* a vec
> diffuse in a group that also has a vec lane, and that is **0.01–0.30 % of
> lanes**, because the kernel's own `anyVecLane` early-out already zeroes
> `omniLoopN` for an all-spec group. **C6 is closed, and with it C6a** — the
> rsqrt unification that would have moved every city pixel. C6a survives ONLY as
> a standalone look/correctness question for Gil-Ad and `SHADING_CONTRACT.md`
> (the vec loop's `_mm256_rsqrt_ps` is a bare estimate, ±0.3 % and piecewise-
> constant; the scalar redo's `fast_rsqrt` is refined to ±6e-5), never as a perf
> item. It was not built.
>
> **WHAT LANDED, all six byte-null, measured min-of-5 interleaved against the
> parent `e017d611` with both binaries built in one worktree** (Ginstr floor
> ±0.14 %): city t=1961 `lighting-w1` **0.978 → 0.741 Gi/f (−24.2 %)**,
> `renderFrame` **4.183 → 3.945 (−5.7 %)**; t=400 −26.7 % / −6.2 %; t=2400
> −15.9 % / −2.8 %; fountain t=2500 −16.2 %; **greets t=5743 exactly 0.00 %**,
> the control that proves nothing outside OuterVec moved. Cycles track
> instructions (IPC 4.01 → 3.96), so this is not the cube-prepass trap.
> Per-lever on ONE binary from the ALL-OFF arm: `--deferred_ovec_light_skip`
> (skip a light that reaches no lane of the group; 28–40 % of pairs) **−9.2 %**,
> `--deferred_ovec_mat_uniform` (one `Material` walk per group; 95 % of groups
> are uniform) **−8.0 %**, `--deferred_ovec_nomirror` (the provably-dead mirror
> compare) **−4.8 %**, `--deferred_ovec_vec_pack` (one 256-bit store for an
> all-plain group; 58–63 %) **−2.7 %**. Every one beat its prediction.
>
> **C1 + C8, the two flagless ones, priced on their OWN binary**: city t=1961
> `lighting-w1` 0.977 → 0.953 Gi/f (−2.5 %) but **0.243 → 0.231 Gcyc/f (−4.9 %)**;
> t=400 −3.3 % Gi / −3.2 % Gcyc. The cycle win exceeding the instruction win is
> **16m's signature** (that round measured Gi −0.96 to 0.00 % against Gcyc −1.17
> to −2.85 %), and the disassembly confirms the mechanism directly: publishing
> the in-loop `static const bool` took the OuterVec symbol from **2
> `__cxa_guard` references, 13 `bl` calls and 10 callee-save `stp` pairs to 0, 6
> and 9**.
>
> **THE NEXT TARGET IS NAMED BY THE LADDER, and the analysis mis-sized it: the
> per-lane PACK loop is 24.8 % of the row**, not ~5 %, because **33–36 % of
> city's alive lanes carry an env store** and pay a scalar face pick, live-water
> weight, tilt + re-projection and one or two `EnvCubeFetchBil` inside it.
> `EnvComposeCityVec8` already vectorises the front end and arms on 33–37 % of
> groups; the fetch is still eight scalar bilinear taps.
>
> **GATES: no pin moved.** 13/13 pinned poses reproduce and `render_gate` is
> 4/4 from this worktree's stock `rev.cfg`. Byte-nullity is also proved
> DIFFERENTIALLY on one binary: every lever flipped off individually and all four
> together give the same hash at city t=1961 (`4cb8d2ca…`), fountain t=2500
> (`8db68ccb…`), crash t=400/1200 (`e33b57d5…` ×2) and greets t=5743 forced
> through `--deferred_outer_vec` (`5a9190d2…`) — the last being the only arm that
> exercises the mirror-compare instantiation and the real normal-map lane loop.
> greets acceptance t=5743 / t=2845 are byte-identical to their pins.

> ## 2026-08-26 — **CITY AND FOUNTAIN HAVE AMBIENT OCCLUSION FOR THE FIRST
> TIME**: the discard is `Scene::PreferOuterVec`, not the tonemap — the
> outer-vec lighting kernel writes NO HDR radiance, so SSAO was multiplying a
> buffer that was merely SIZED. `--ssao_hdr_transport` defaults ON; 13/13 pins
> and `render_gate` 4/4 reproduce
>
> **THE MECHANISM, NAMED.** `Render_DeferredLighting_Tile_OuterVec` — the kernel
> city, fountain and crash select through `Scene::PreferOuterVec = 1` — stores
> **8-bit VPage only** and deliberately leaves the HDR coverage lane `h[3]` at
> **0**: its pack *is* the HDR transport, lifted afterwards by the froxel
> composite (`h[3] > 0 ? h : VPage`) or by `Hdr_ActivateNoFog`. greets and chase
> run the SCALAR wave-1 kernel, which does write `g_hdrBuf` and does stamp
> coverage. `Render_SSAO` picked its target on `hdr() && Hdr_WritableFor(W,H)` —
> **"is the buffer SIZED for this view"**, true in both cases — so on
> city/fountain the AO multiply landed on a sized, cleared, **all-zero** buffer
> and the lift then seeded that buffer from the **un-occluded** VPage. The pass
> ran, cost ~5 ms, and produced nothing. It is the same defect the wave-2
> checkerboard fill kernel already carries a "⚠ WAVE-1 TRANSPORT MUST MATCH"
> warning about, one call site further downstream.
>
> **THE ONE-RENDER PROOF** (`FDS_HDR_SCAN=1`, extended this round to print an FNV
> of `g_hdrBuf`, the coverage population, the ZPage coverage and a VPage FNV per
> pipeline tag). One arm, `--deferred --hdr --hdr-linear --texture-filter=2
> --ssao --ssao-gtao`:
>
> | scene | tag `kernel` | tag `ssao-post` | who overwrites |
> |---|---|---|---|
> | fountain t=2500 | `cov=0 maxFinite=0 zcov=396687` | hash **UNCHANGED** | `activate` (`maxFinite 0 → 255`) |
> | city t=1961 (main) | `cov=0 maxFinite=0 zcov=1116488` | hash **UNCHANGED** | `fog-post` (froxel, `act 0→1`, `0 → 2478`) |
> | greets t=5743 | `cov=2072779 maxFinite=617` | hash **CHANGES** | nobody — AO survives |
> | chase t=1105 | `cov=1957160 maxFinite=24256` | hash **CHANGES** | nobody |
>
> `zcov` proves the frames have opaque coverage; `cov=0` proves none of it
> reached `g_hdrBuf`. **This also explains why the hunt's `--no-fast_fog` control
> "EXONERATED" the fog and found nothing**: dropping the froxel composite merely
> hands the same VPage lift to `Hdr_ActivateNoFog`. Two different overwriters,
> one upstream cause.
>
> **THE FIX IS ONE PREDICATE.** `Deferred_KernelWritesHdrRadiance()`
> (`= !deferredLightingOuterVecEnabled()`, exported in `DeferredCommon.h`) now
> gates SSAO's HDR arm. When the kernel wrote no HDR, SSAO modulates **VPage** —
> the same 8-bit buffer the whole frame is shaded into — and the lift carries the
> occlusion into the radiance. `--no-ssao_hdr_transport` restores the old
> predicate EXACTLY and is the pre-fix A/B arm. `docs/GRAPHICS_PIPELINE.md` §4
> was actively recommending the broken test and now carries the "sized is not
> populated" rule.
>
> **ACCEPTANCE.** city t=1961 `--city_env_pixel`: no-ssao `b3372d0f…` (the exact
> hash the defect report recorded), ssao **`6964d6e8…`**, `--ssao_strength=8`
> **`3256a72f…`**, OFF arm back to `b3372d0f…`. fountain t=2500: no-ssao
> `32ff5896…`, ssao **`4e831ea0…`**, strength=8 **`d0a95fe9…`**, OFF arm back to
> `32ff5896…`. Look: city 44.95 % px moved, mean |Δ| 4.77 on the moved, max 145;
> fountain 15.33 %, mean 5.30, max 107. **Eyeballed at 1×**: fountain reads as
> real contact AO — the pod housings darken under the pods, the pylon shafts pick
> up modelling, the ship gets grime; no halo, no rim, no over-darkening. city is
> a broad, low-amplitude facade dimming (the scene is blown out, so AO has little
> room) and the water plane is essentially untouched — in the `--env_live_water
> --city_env_pixel` arm the water band moves 12.5 % of its pixels at **max Δ
> 12/255**, all of it on the quay and the poles, none on the water itself. At 10×
> amplification the city facade shows the known low-amplitude SSAO diagonal hatch
> (GRAPHICS_PIPELINE §9's documented residual); at 1× I cannot see it.
> **LOOK-DELTA FOR HIS EYE**, since these two scenes have never rendered with AO:
> `docs/img/hdrssao/{city_t1961_ao,fountain_t2500_ao}_{before,after,diff}.png`,
> full frames `{city,fountain}_full_sbs.png`, water band
> `city_t1961_waterband_{before,after,diff8x}.png`, facade zoom
> `city_t1961_facade_{before,after,diff10x}.png`.
>
> **THE `ssao_radius_zfloor=48` PIN IS NOW A VISIBLE FACT IN THESE SCENES, and
> its bijection re-verifies AS AN IMAGE.** city default == `--ssao_radius_zfloor=0
> --ssao_radius=6.0661764` **byte-identical**, `zfloor=0` alone (effective 4.0) a
> different image (22.65 % px, mean 1.54, 412 px > 12/255). fountain default ==
> `zfloor=0 --ssao_radius=4.0441175` byte-identical, `zfloor=0` alone differs
> (1.61 % px, mean 1.18, max 8). The 2026-08-25 effective-radius table was
> derived from an arm where the AO never reached the frame; it now reproduces on
> the frame itself.
>
> **COST, min-of-11 INTERLEAVED arms, `--bench=scene`, no `--ssao_dump`.**
> fountain t=2500 `[ssao]` pass **4.12 min / 4.16 median** (fix) vs **4.07 /
> 4.11** (pre-fix) → **+0.05 ms**; apply stage alone 0.56 vs 0.52 → **+0.04 ms**.
> city t=1961 pass **5.50 / 5.58** vs **5.48 / 5.59** → **+0.02 ms min, −0.01
> median**; apply 1.10 vs 1.06 → **+0.04 ms**. The **+0.04 ms on the apply** is
> the real reproducible term (8-bit RMW with three int converts + pack against
> the f16 RMW) and it is the entire price. It is not nothing — but the other ~4
> to 5.5 ms of that pass was previously being paid for a discarded result.
>
> **GATES: 13/13 pinned poses reproduce, plus the city acceptance arm, plus
> `render_gate.sh` ALL PASS 4/4** (from this worktree's stock `rev.cfg`). city
> `bd4ffbf8…`, city acceptance `4cb8d2ca…`, greets t=1588 `570a7b44…`, greets
> acceptance ×4 `440aa6bb… 00d17bc5… 29c1e7fb… bc1b0a8a…`, fountain `8db68ccb…`,
> chase default ×5 `f16bedd0… fcc9d561… 397b878d… 3539492d… 0622d56e…`, chase
> cinematic `b61b3397… 4d70fdbd…`. Byte-nullity on greets/chase is also proved
> DIFFERENTIALLY on one binary: chase t=1105 his arm `63d1613e…` and greets
> t=5743 `440aa6bb…` are identical with and without `--no-ssao_hdr_transport`.
> **No pin is re-pinned and no value is struck.**

> ## 2026-08-25 — **THE PER-SCENE SSAO RADIUS IS PINNED**: `ssao_radius_zfloor`
> defaults to 48, the chase moire is gone in the arm he runs, and the flip is
> byte-null at **every single pinned gate row** — which is a finding, not a relief
>
> Gil-Ad approved the chase `zfloor=48` arm by eye and ordered the pin: *"the
> moire was fixed - we need to pin the per-scene radius."* `origin/rev-moire`
> fast-forwarded into `fog-wt` (it sat directly on `7cd8503d`, touched three
> source files and no docs, so there was nothing to union-resolve), and
> `ssao_radius_zfloor`'s default went **0 → 48** — the standing
> umbrella-flags-imply-features rule: a fix he approved has to be live in
> `--ssao --ssao-gtao`, not a dark opt-in. `--ssao_radius_zfloor=0` is the OFF
> switch and the exact pre-flip arm; every claim below is differential against
> it on ONE binary.
>
> **WHAT THE PIN ACTUALLY PINS.** The floor is `k × ZPage16 quantum`, and the
> quantum is `FZP*1.1/0xff00` — per-SCENE. At k=48 and `--ssao_radius=4.0` the
> effective radius is therefore:
>
> | scene | FZP | quantum | quanta @ r=4 | **effective radius @ k=48** | |
> |---|---|---|---|---|---|
> | greets | 150 | 0.00252757 | 1582.6 | **4.000000** | floor INERT |
> | crash | 2000 | 0.03370098 | 118.7 | **4.000000** | floor INERT |
> | fountain | 5000 | 0.08425245 | 47.5 | **4.044117** | +1.10 % |
> | city | 7500 | 0.12637867 | 31.6 | **6.066176** | +51.7 % |
> | chase | 50000 | 0.84252453 | **4.75** | **40.441177** | ×10.11 — the defect |
>
> **Every row of that table is MEASURED, not computed-and-hoped**: for each
> scene, the default arm is byte-identical to
> `--ssao_radius_zfloor=0 --ssao_radius=<effective>`. That is a bijection —
> greets/crash confirmed at 4.0 (floor inert by construction, `k*q` never
> reaches 4.0), fountain at 4.0441175, city at 6.0661764, chase at 40.441177.
> The flag now means the same thing in every scene, which is the whole point of
> the pin.
>
> **THE GATE RESULT IS THE SURPRISE: NOTHING MOVES. 0 of 13 pinned hashes.**
> The brief expected the chase rows to move. They do not, and the mechanism is
> mechanical: `ssao` defaults to **0** and no scene `setDefault`s it on, so
> `Render_SSAO` early-returns in every gate recipe except one — none of
> `chase (default)`, `chase (cinematic)`, `city`, `fountain`, `greets t=1588`
> passes `--ssao`. The single row that does, `greets (acceptance ×4)`, is one of
> the two scenes the construction exempts. Differential, verbatim recipes,
> `--ssao_radius_zfloor=0` vs default, all in one clean worktree:
>
> * chase default t100/400/800/1200/1600 `f16bedd0…` `fcc9d561…` `397b878d…`
>   `3539492d…` `0622d56e…` — all five identical across arms **and all five
>   reproduce the 2026-08-17 pins exactly**
> * chase cinematic t800 `b61b3397…` t1600 `4d70fdbd…` — identical, pins hold
> * city `bd4ffbf8…`, fountain `8db68ccb…`, greets t=1588 `570a7b44…` — identical,
>   pins hold
> * greets acceptance ×4 t5743 `440aa6bb…` t2845 `00d17bc5…` t6097 `29c1e7fb…`
>   t6133 `bc1b0a8a…` — identical, pins hold. **The t=5743/t=2845 byte-identity
>   claim the hunt made by construction is now measured, and it holds.**
>
> So **no row is re-pinned and no old value is struck** — the table below is
> unchanged by this flip, and that statement is itself now a gated fact.
>
> **`render_gate.sh`: ALL PASS 4/4 on the flipped binary** (`mirrortest
> 4ac809e5…`, `rttslot 826c09e6…`, `conetest b41894f9…`, `halotest 166fa25a…`),
> run from a worktree carrying the STOCK committed `rev.cfg`. Run from the main
> tree it fails 4/4 — **and that is his `rev.cfg` (1384×768 vs the baselines'
> 1920×1080), not this change.** Proved two ways: the same four hashes come back
> byte-identical with `FDS_SSAO_RADIUS_ZFLOOR=0`, and the MAIN tree's own binary
> pointed at stock assets (`--no-chdir_assets`) reproduces `4ac809e5…` exactly.
> New trap recorded below; his `rev.cfg` was not touched.
>
> **THE LOOK, EVERY PAIR EYEBALLED.** chase, his arm
> (`--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao`), at
> t=400/800/1105/1300: the herringbone weave and the isoline contours are gone
> and the rock faces lose their mottled AO grime — 11.10 / 18.32 / 91.91 /
> 19.33 % of pixels move, mean |Δ| 4.92 / 4.93 / 7.96 / 6.24 on the moved, max
> 128 / 171 / 68 / 185. This is the approved look and it was not retuned beyond
> k=48. Crops: `docs/img/moire/pin48_chase_t00{0400,0800,1105,1300}_crop_{before,after}.png`.
>
> **city and fountain: NO look-delta, and the reason is a defect worth its own
> round.** On the LDR deferred+SSAO arm the flip is visible in the numbers and
> invisible to the eye — city 37.38 % of pixels at mean |Δ| **1.132**/255 (only
> 1337 px above 12/255), fountain 2.17 % at mean **0.442**, max 9. I looked at
> both pairs at the densest-change window; they are indistinguishable.
> `docs/img/moire/pin48_{city_t1961,fountain_t2500}_ldr_crop_{before,after}.png`.
> **But under `--hdr` the question is moot, because SSAO does not reach those
> frames at all**: city and fountain are byte-identical with `--no-ssao` and with
> `--ssao --ssao-gtao --ssao_strength=8`, and even at `--ssao_radius=200`. SSAO
> demonstrably RUNS (`[ssao] 1920x1080 /2, GTAO+bitmask, HDR g_hdrBuf: 5.61 ms`,
> and `--ssao_debug` paints a correct AO frame) — its HDR write is simply lost
> before the dump. **`--no-fast_fog` does NOT recover it, so the froxel composite
> is EXONERATED**; chase under the same `--hdr --hdr-linear` keeps its AO fine.
> Filed to OPTIMIZATION_BACKLOG; NOT fixed here, and it means his HDR city and
> fountain have been running with no ambient occlusion at all.
>
> **COST, RE-MEASURED AND HONESTLY DOWNGRADED.** chase t=1105, 1920×1080,
> `--ssao_downscale=2`, **14 runs per arm interleaved**, `[ssao]` pass wall, no
> `--ssao_dump`: k=0 min 4.15 / median 4.25 / mean 4.264 ms; k=48 min 4.07 /
> median 4.26 / mean 4.238. Delta min **−0.08**, median **+0.01**, mean
> **−0.03** — the sign flips between statistics and the distributions sit on top
> of each other. The honest quote is **no resolvable cost at 14×14**, |Δ| ≤ 0.08
> ms on a ~4.25 ms pass. The hunt's original **+0.17 ms min / +0.05 ms median
> (6 runs, un-interleaved) DOES NOT REPRODUCE** and is retracted as noise — the
> flag description and the code comment now carry the interleaved numbers.
>
> **MEASUREMENT TRAP, and it burned the hunt's cost figure once already:
> `--ssao_dump` forces the scalar apply loop and inflates the `[ssao]` pass to
> 13.7–16.0 ms against a real ~4.2 ms — ~3.5×. Timings from a dumping run are
> not pass timings.** Recorded in the traps list below and in the backlog.

> ## 2026-08-18d — **THE CROSS-BAKE GUARD LANDS AND THE BATTERY FLIPS TO A
> CLEAN SWEEP**: v2+guard beats the default arm at ALL 15 review poses, the
> base slits are gone, and the corner pose falls to 60 px against the
> default's 105
>
> The addendum-4 mechanism, implemented: `foreignCoincident(P)` — any
> foreign-material face within kAbutEps, convexity deliberately ignored —
> applied at v2's four vert-creation/classification sites (end-course
> sideVert pair, lockstep m2, recursion midpoints, break-free). A vert
> created at another bake's junction pins, so the 'rooms' bake can no longer
> diverge from the separately-baked floor at their shared seam.
>
> **THE BATTERY (default → v2+guard, pure-black px):** 2845 18→4, 5534
> 1474→1127, 5743 251→147, 5773 284→127, 5813 228→191, 5814 355→308, 5843
> 390→215, 5854 394→214, 5958 232→112, 5963 307→192, 5967 458→283, 5987
> 218→118, **6097 532→198**, 6133 1064→731, 6293 456→278. Better at 15/15
> (12 by >50 px), worse at ZERO. The guard closes base holes the DEFAULT arm
> always had: the guarded dense base geometry seals seams the coarse default
> merely straddled. t=5968 (the commissioned corner): **60 px** vs default
> 105, channel still zero. Base crop after:
> `docs/img/fogwt/battery_v2guard_base_t6097.png`.
>
> **Gates:** default t=5968 `bf75aa27…` byte-identical, pin t=5743
> `440aa6bb…` — both re-verified on this exact binary. Flags remain
> default-OFF: what stands between `--greets_displace_profile_agree=1
> --greets_displace_border_v2` and the umbrella default is now ONLY Gil-Ad's
> eye (the acceptance ×4 re-pin under the new arm is mechanical once he
> approves the look).
>
> The night's chain, for the record: census box → [STONE-PROV] provenance →
> face-ownership plane → foreignCoincident. Three refuted levers (two
> tapers, one relax exemption) are documented in the addenda with
> byte-identity proofs; every refutation narrowed the population until the
> fix was one lambda.

> ## 2026-08-18c — THE REVIEW-POSE BATTERY IS IN: v2 closes the commissioned
> corner and pays 100–180 px of WALL-BASE HAIRLINES at 10 of 15 poses — the
> end-case residual, localized and named, gating the default flip
>
> All 15 poses of `docs/greets_review_poses.txt`, his umbrella arm vs
> + `--greets_displace_profile_agree=1 --greets_displace_border_v2`,
> pure-black px (default → v2): t=2845 18→18, t=5534 1474→1556, t=5743
> 251→363, t=5773 284→278, t=5813 228→311, t=5814 355→429, t=5843 390→491,
> t=5854 394→509, t=5958 232→244, t=5963 307→441, t=5967 458→538, t=5987
> 218→387, t=6097 532→715, t=6133 1064→1061, t=6293 456→460. (These are
> REVIEW-pose cameras — the acceptance-pin recipes, which use the scene
> camera, are byte-identical under v2-off and were re-verified 4/4 alongside
> `render_gate` 4/4 after the merge.)
>
> **Every worse pose's new black is the same population**: thin hairlines
> along the WALL-BASE / floor junction plus small base-course sliver notches
> (diffed per pixel at t=6097/5987/5963 — bboxes all y>697, bottom of frame;
> crop `docs/img/fogwt/battery_v2_newblack_t6097.png` vs `…_t6097_ref.png`).
> This is the end-case residual the v2 build already halved once by refusing
> abut-pinned far ends: the base border is floor-pinned while its band strip
> now densifies in lockstep, and the strip's bottom cells bridge
> pinned-to-freed. Next lever, scoped: terminate the lockstep strip one cell
> above a pinned end (taper the band to the pinned border instead of
> bridging), then re-run this battery — the corner rig will not see it (its
> floor edge pins the whole bottom course); the battery is the gate.
>
> The trade as it stands is HIS call: the commissioned corner closes to zero
> channel pixels; the cost is base-course hairlines under 0.01 % of frame at
> two thirds of the review poses. Flags remain default OFF.
>
> **ADDENDUM — the hairline MECHANISM IS LOCATED** (census box
> "11,17,-61,-56,-0.2,0.8" at the t=6097 base seam, FINALV diffed per vert
> across arms): v2 adds **116 verts** in the base band the default arm never
> had — **93 of them PROUD (field level, cls '-', unwelded)** and ZERO shared
> verts changed. The stone field's proudness now reaches down to the PINNED
> base border; the bottom cells slope proud→0 over one pitch and the proud
> course's unmodeled underside silhouettes as the black hairline against the
> floor. (The earlier pinned-midpoint taper never fired because these verts
> are not lockstep midpoints at all — they are the densified field verts
> above the pinned border.) The correctly-scoped fix: a FIELD end-taper —
> scale displacement to 0 over ~0.3 u of distance to an abut-pinned border,
> the exact rule the mitre profile already applies at its own line ends
> ("kEndTaper", the t=6039 blue-sliver fix) — applied to v2's densified
> strip verts. Gate stays the 15-pose battery.
>
> **ADDENDUM 2 — the end-taper AS SCOPED is refuted, and the scoping is the
> finding.** Built it (v2-created verts — end-course band inners + lockstep
> midpoints — scale dsp→0 within 0.3 u of a pinned-bordering-freed vert,
> weld excluded): it FIRES on 201 verts at t=6097 and changes **zero
> pixels** — 15/15 battery renders and a direct t=6097 md5 byte-identical
> to the taper-less v2 arm. Therefore the 93 proud base verts are NOT
> v2-CREATED verts. Sharpened hypothesis: they are v2-FREED — verts pinned
> in the default arm (so FINALV never prints them there) that one of v2's
> classification changes lets displace. Next instrument, before ANY further
> lever: print the vert INDEX + creation class in [STONE-FINALV] (or a
> one-off provenance census keyed on the 93 positions) under both arms, and
> diff pinnedZero/recessOnly per position. Reverted; the tree carries no
> dead taper code.
>
> **ADDENDUM 3 — provenance answered, and the METRIC is now the suspect.**
> [STONE-PROV] (prints every boxed vert's classification BEFORE the
> displacement loop, both arms): the proud population is **92/93 BAND-INNER
> nodes of bands only v2's eligibility creates** (pin0 free0 inner1, absent
> in def) plus 23 v2-freed formerly-pinned verts. Taper v3 (population =
> bake-created OR band-inner, unwelded, unpinned) FIRES on 481 verts at
> t=6097 and **visibly changes the render (md5 moves) while the pure-black
> count stays exactly 715** — the black pixels are not made by these verts'
> displacement. Three refuted geometry levers + one pixel-neutral hit means
> the "pure-black px" metric may be counting a SHADING seam (groove-shade /
> lightmap keyed off displaced positions?), not holes. NEXT INSTRUMENT:
> classify the 263 delta px at t=6097 as z==0 hole vs shaded geometry
> (--face_id_dump or a z-plane dump), BEFORE any further lever. Taper v3 is
> KEPT inside the opt-in v2 arm (counter printed as [STONE-V2TAPER]); its
> look effect is unjudged.
>
> **ADDENDUM 4 — CLASSIFIED, AND THE MECHANISM IS COMPLETE.** The face plane
> (`--face_id_dump`, snapshot writes `_face.u32`) says **263/263 new-black px
> have NO FACE — genuine holes**, not shading. And the [STONE-PROV] overlay
> across the 'rooms' and 'floor' bakes at those positions (y=0.000/0.020,
> wall-base line) shows the def arm PINNED in both bakes while under v2 the
> 'rooms' bake displaces there (band-inner nodes + freed verts) and the
> separately-baked floor still pins — **the two bakes diverge at their shared
> junction and the seam opens: the exact through-slit class the 2026-08-15
> FOREIGN-FAMILY guard exists for.** Root cause: v2's three vert-creation
> paths (end-course sideVert, lockstep m2, break-free) classify with
> `abutPointMat` alone — the pier base meets the floor CONVEXLY from the
> visible side, so the veto frees — and never consult the cross-bake
> coincidence rules (ndVert / neighbor-pin / foreign-family contact) that pin
> exactly these junctions everywhere else. THE FIX (next cycle, fully
> specified): route v2's creation-time classification through the same
> coincidence guards the classifier uses; a new vert coinciding with another
> mesh's geometry at a junction pins, convexity notwithstanding. Gate: the
> 15-pose battery — expect the +74..+183 column to collapse.

> ## 2026-08-18b — **THE ARRIS CHANNEL CLOSES**: the lockstep reorder lands as
> `--greets_displace_border_v2`, the t=5968 slit contributes ZERO pixels, and
> what stands between this and the umbrella default is Gil-Ad's eye plus the
> battery
>
> Implemented by a worktree agent from `docs/STONE_BORDER_REORDER.md` (its
> "BUILT" section is the full gate record); **independently re-verified in the
> main tree**: byte-null t=5968 default `bf75aa27…` and pin t=5743 `440aa6bb…`
> reproduce, and the headline arm reproduces exactly.
>
> **THE NUMBER: t=5968, his umbrella + `--greets_displace_profile_agree=1
> --greets_displace_border_v2` → 148 pure-black px** (double-valued default
> 105, agree-alone 3 346, +ladder 5 086), **and the arris channel contributes
> 0 of the 148** — the slit is gone, not smeared; the residual lives in the
> left-of-frame buckets the default arm also has. My own eyes on the verified
> crop: continuous arris, no sawtooth shear, no wide smear; a narrow soft band
> transition remains — HIS look call. Crops (same region, three arms):
> `docs/img/fogwt/arris5968_crop_m1_v2_ref_default.png` /
> `…_ref_agree1.png` / `…_m1_v2.png`.
>
> **What v2 is** (all in `DisplaceStoneSubdiv`, flag default OFF): band
> pairings recorded per border segment; narrow border faces adopt their cell
> quad; break verts freed (band-inner excluded); the densification splits
> border and inner edge in LOCKSTEP (2 band tris → 4, interior tri split,
> chord-pin to the parent midpoint); end-course one-freed-endpoint splits only
> with a live pairing and never against an abut-pinned far end.
>
> **Two design premises overturned by measurement** (recorded in the design
> doc): the rig's 667-px "unwelded end-course floor" was a RIG BUG — backA
> protruded 2.8 u past sheet A's own edge; inset (this commit), the graze
> count is now a true metric and reads **0** on every sane arm. And fold-relax
> does NOT become dispensable under v2 (without it: 3 878 px) — the
> weld/relax conflict shrank to a residual (the rig's 0.2474 gap outlier),
> it did not dissolve.
>
> **Corner-rig truth table after the backdrop fix** (mode 1): v2 off gap
> 0.0942/0.0199, flips 492, green 0/156 → v2 on gap 0.2474/0.0337, flips 939,
> green 0/**63**. Mode 2 under v2 is pathological (5 088) — MIN is dead,
> thrice over. Gate 1's thresholds need re-adjudication against what the
> backdrop fix revealed; gate 4 (25-pose battery, acceptance ×4, t=1588,
> render_gate) is NOT yet run — it gates any default flip, after his eye.

> ## 2026-08-18 — THE CORNER HAS A TEST SCENE NOW, THE LADDER IS ADJUDICATED
> AGAINST ON THE REAL MESH, and the fix that remains standing is a border-
> pipeline STAGE REORDER nobody should attempt as an add-on pass
>
> Commissioned in anger: *"HOW HARD COULD IT BE TO MAKE SURE THAT WE HAVE A
> CONTINUOUS HEIGHT OVER A CORNER? YOU CAN ALSO CREATE A TEST SCENE AND TEST
> THIS EXACT SCENARIO."* Both done; the second half of the first is a refusal
> with numbers. **Shipping arm untouched: default t=5968 `bf75aa27…` == parent
> byte-exact, acceptance pin t=5743 `440aa6bb…` reproduces, all new machinery
> flag- or census-gated.**
>
> **THE TEST SCENE — `FDS_DISPLACETEST_CORNER=1 ./DEMO --scene-displacetest`.**
> Two sheets, ONE TriMesh, normals 59° apart like the pier (A=(+1,0,0),
> B=(+0.514,0,+0.858)), corner segmented DIFFERENTLY per sheet (A breaks at
> y=3.7, B at 2.6/5.9 — identical full-height edges weld into one shared
> interior edge and the rig goes vacuous; the split-vertex population needs
> mismatched authored segmentation, the t=1088 comment's own mechanism), u
> phases in maximal conflict at the border (B's column ON a vertical groove
> u=0.5000, A's just off one at 0.7455), authored winding CLOCKWISE (the FLD
> convention the veto's convexity test measures against — counter-clockwise
> reads as a concave corner and pins everything; the rig's other builders are
> counter-clockwise and never noticed because a flat quad never asks). It
> reproduces the exact greets machinery: same mitre bisector (+0.87,0,+0.49),
> cosHalf 0.870, split columns, fan-sliver bands. VERDICT LINE per arm:
> border-polyline gap max/mean, twisted-face count, green punch-through from
> two poses against inset backdrops. Census upgrades that made it usable:
> `FDS_STONE_CENSUS_BOX="x0,x1,z0,z1,y0,y1"` retargets [STONE-FREEV]/[FINALV]/
> [CORNERF] off the hard-coded greets coordinates; new [STONE-MITRE-CAND]
> prints WHICH filter rejected a candidate (found: the authored BREAK verts sit
> unfreed mid-line — zero-pop candidates the weld skips).
>
> **WHAT THE RIG ESTABLISHED (all measured, at the approved arm, amp 0.3):**
> * mode comparison (no ladder): agree-MAX gap max/mean **0.094/0.020**,
>   double-valued 0.152/0.056, agree-MIN 0.150/0.038 — **MAX is the right
>   profile semantics**, twice confirmed (scene: it closes the joint-row shear).
> * the punch-through floor (graze 667 px, all arms) is the UNWELDED END
>   COURSES: the weld span runs 1.28–7.51 of 0–8 because the densification
>   recursion only splits segments with BOTH endpoints freed, banding only
>   exists where the pre-split ran at authored scale, and the corner's first/
>   last course has neither.
> * fan slivers twist and cull exactly as diagnosed 08-17b (491 flipped faces
>   at the corner cylinder).
>
> **THE LADDER — BUILT, FOUR VARIANTS, ADJUDICATED AGAINST.**
> `--greets_displace_band_ladder` (default OFF, byte-null, KEPT as the A/B
> instrument): frees authored break verts between freed same-line edges, then
> rebuilds each banded strip as border-pitch quads bounded to the inner
> polyline's span. Rig: front punch-through 156 → 112. **Greets t=5968: 3 346
> → 5 086 px of z==0 — REGRESSION, thin cull-stripes down the whole channel**
> (`docs/img/fogwt/arris5968_crop_m1_ladder.png`). Mechanism: near-vertical
> cells trip fold-relax, which halves welded verts PER SHEET and re-splits the
> weld. Variants killed on the rig: chord-pinning the inner nodes (the
> interior side is a CELL polyline, not a chord), one-freed-endpoint end
> densification (mega-fans to interior apexes, green ×20), post-hoc re-band
> (per-face micro-bands, green ×30).
>
> **THE FIX THAT REMAINS STANDING — a stage REORDER of the border pipeline:**
> densify freed borders full-length FIRST (ends included), band ONCE at that
> density against the cell-tessellated interior, then tessellate strips as
> cells of the same lattice, then weld with the single-valued (MAX) profile.
> Everything measured this round says each stage is individually sound and the
> ORDER is what breaks the corner. That is a bake-pipeline redesign with the
> pin battery as its gate — next session's work, with the corner rig as the
> inner loop.

> ## 2026-08-17b — THE t=5968 PIER ARRIS IS DIAGNOSED TO THE VERTEX AND **NOT FIXED**: the split-vertex corner's mitre profile is DOUBLE-VALUED, both single-valued repairs are look calls his eye has not judged, and the thing his screenshot actually shows is the band's FAN-SLIVER tessellation
>
> Commissioned by the 2026-08-17 screenshot (`FDS_GREETS_CAM=
> "18.9410915,3.36436033,-60.2976227,-0.501057684,-0.0739506483,0.862247944"`
> t=5968, his full umbrella arm). Images: `docs/img/fogwt/arris5968_*`.
> **NOTHING CHANGED UNDER HIS COMMAND LINE — landed byte-null, twice proven**
> (below). What landed is an A/B instrument + census extensions + this diagnosis.
>
> **THE DISCRIMINATOR LADDER** (his exact recipe ± one flag, z==0 punch-through
> px at 1920×1080): full arm **105** + the smeared channel + a mid-height shear;
> `--no-greets-displace` **0** (clean arris — the defect is entirely the
> displacement's); `--no-greets_displace_block_level` **4 029** (block_level was
> PARTIALLY PAPERING over this corner, not fixing it);
> `--no-greets_displace_geom_bisector` = full arm (73 px differ, not the driver);
> `--no-greets_displace_free_edge` **0** (carve-on-freed-borders is the enabling
> stage — the 16y minimal pair again).
>
> **ROOT CAUSE, measured to the vertex** (`--greets_displace_junction_census`):
> 1. The corner is a **SPLIT-VERTEX seam** — 'rooms' local (±2.469,y,-4.937),
>    this one at world (17.898,y,-58.014), the [STONE-JUNC] 91.1° population:
>    two index-distinct border columns, one per sheet, interleaved 3–5 milli-u
>    apart in y.
> 2. Both columns DO join mitre line 9 (bis +0.870,0,+0.493 — the one-sided-line
>    fix works) — but the shared profile is the UNION of both columns' samples,
>    and at one s the sides disagree about stone-vs-joint because each carries
>    its OWN u-column of the height field (u=+0.5000 vs u=-3.2545): welded dsp
>    **-0.1104 at s=3.200 against -0.0532 at s=3.204** — a sawtooth the weld
>    itself wrote. Each sheet's border chords its OWN subsequence → the two
>    welded polylines diverge up to ~0.06 u mid-chord → the joint-row shear and
>    the 105 px slit.
> 3. **What his eye actually reads — the wide stretched-texel channel — is the
>    band TESSELLATION**, not the profile: [STONE-CORNERF] (new census) shows
>    the 0.02 band strip triangulated as MEGA-SLIVER FANS — single authored
>    apexes (y=0.307, 1.652, 2.316, 2.413…) each serving up to **1.4 u of
>    0.043-pitch corner line** (~30× density mismatch). Those slivers smear the
>    texels; their twist is what opens holes when corner and apex depths differ.
>
> **TWO SINGLE-VALUED REPAIRS BUILT, NEITHER LANDED AS DEFAULT —
> `--greets_displace_profile_agree` (INT, default 0 = byte-null):**
> * **=1 MAX** ("carve only where both sheets agree", the 2026-08-14 rule
>   enforced): census-verified — paired welds agree to ≤4 milli-u, the
>   joint-row shear closes. **But punch-through grows 105 → 3 346 px**: the
>   proud corner vs its own carving fan apex twists the slivers into backface
>   culls. Mode 1 fails on the TESSELLATION, not the rule.
>   `arris5968_crop_mode1_max.png`, wireframe `arris5968_wire_mode1.png`.
> * **=2 MIN** (a joint on either sheet cuts through): 105 px, look near the
>   double-valued arm. `arris5968_crop_mode2_min.png`.
> * Both modes move ≥11 % of the t=5968 frame (SSAO ripple included) — LOOK
>   CALLS, his per the ledger; deliberately NOT umbrella-defaulted (the
>   umbrella-implies-features rule cuts the other way here: defaulting an
>   unjudged look change under his flags would be worse than the opt-in).
>
> **THE STRUCTURAL FIX, next session's work: the band LADDER.** Densify the
> band's INNER polyline to the border pitch (the border side already densified
> to 0.043; the inner side kept authored density — `MeshOps.cpp` band pre-split
> at ~3872, "fan bound"), re-triangulating the 0.02 strip as quads; the
> existing T-junction-pin and seam-union machinery (214 pins / 565 splits) are
> the tools for the interior side. That kills the smear AND makes either
> agreement mode safe. Then re-judge mode 1 vs 2 with his eye.
>
> **GATES:** default arm t=5968 `bf75aa27…` == parent binary byte-exact;
> mode 1/2 reproduce their experiment renders exactly (`bcbd44d0…`/`b84838d7…`);
> greets acceptance pin t=5743 `440aa6bbb350ae95fbacf339dd2ad957` reproduces on
> the landed binary. Census extensions (all `--greets_displace_junction_census`-
> gated, byte-null off): [STONE-FINALV] y-window widened 2.9→0.0, new
> [STONE-CORNERF] face dump at the t=5968 corner cylinder.

> ## 2026-08-17a — THE GPU ARM HAS GTAO, and the port matches to **100.000 %** at two of four poses — the residual is the CPU's OWN 8-wide reciprocal
>
> Commissioned: *"make the gpu test have gtao"*. Full write-up:
> `docs/SHADING_CONTRACT.md` **§13**. Images: `docs/img/gpugtao/`.
>
> `GpuBench` had **no ambient occlusion at all**; `FDS/RENDER/DeferredSSAO.cpp`
> had no second implementation to be checked against. It has one now —
> `fs_ssao` / `fs_ssao_blur` / `fs_ssao_apply` in `GpuBench/shaders/deferred.metal`,
> a port with the CPU as the authority on every expression: same 32-sector
> visibility bitmask, same Eberly acos, same 5-term minimax atan2 (**not** MSL's
> `atan2`), same 16-entry 4x4 rotation, same per-frame slice-azimuth table (the
> host fills it from `buildSliceTrig`'s own expression), same matched 4x4 denoise,
> same depth-aware upsample, same apply point (multiply the linear radiance right
> after lighting, before flares/cones/xpar/bloom, main view only). Hemisphere
> fallback ported too. `--ssao_temporal` is NOT ported (CPU default 0) and warns.
>
> **CLI mirrors the FDS names and defaults**, with FDS's own `-`->`_`
> normalisation, so ONE command line drives both renderers:
> `--ssao --ssao-gtao --ssao-downscale=N --ssao_radius/strength/bias/power/blur/samples
> --ssao_gtao_slices/steps/thickness`.
>
> **THE COMPARABLE QUANTITY IS THE AO FIELD, NOT THE FRAME** — D1/D3/D6/D7/E6/E7/E8
> all move the same pixels. Both arms now dump the applied AO multiplier **and its
> inputs** in one shared format (`AOF3`: AO + view-Z + geometric normal): CPU
> `--ssao_dump` (new, **default OFF**, path from `FDS_SSAO_DUMP_PATH`, noinline
> reporter, instrument state declared in the flag help), GPU `--ssao_dump=PATH`.
>
> **RESULT** (4 greets poses, 1920x1080, contract line
> `|gpu-cpu| <= 0.005*max + 1e-4`, per pixel over 2 073 600):
>
> | pose | as shipped | same inputs, CPU 8-wide | **same inputs, CPU scalar ref** |
> |---|--:|--:|--:|
> | t=4871 | 51.93 % | 75.79 % | **99.979 %** |
> | t=5743 | 31.10 % | 88.15 % | **99.931 %** |
> | t=2845 | 49.41 % | 98.01 % | **100.000 %** |
> | t=6097 | 19.09 % | 99.83 % | **100.000 %** |
>
> Column 3 is the verdict on the port: same function, mean |d| 7-9e-5, every
> failing pixel off by at most ONE sector bit out of 64. Column 1 -> column 2 is
> the **G-buffer** (new `--ssao_ref=PATH` drives the GPU AO from the CPU's own
> depth+normal planes; t=6097 goes 19.09 -> 99.83 % on that switch alone — the two
> arms' depth planes differ by rel |dZ| mean 0.005 and their normals by 2.55 deg).
>
> **Column 2 -> column 3 is a CPU-INTERNAL divergence and it is the biggest term.**
> The 8-wide GTAO uses `_mm256_rsqrt_ps` with **no Newton-Raphson** for the two
> horizon cosines; simde lowers that to bare `vrsqrteq_f32` on arm64, ~8 bits.
> Differencing the CPU against ITSELF (`FDS_SSAO_NOSIMD=1`, the file's own escape
> hatch) gives 75.85 / 88.29 / 98.00 / 99.83 % — **the same numbers as column 2 to
> a tenth of a percent**. The shipped path is systematically DARKER than its own
> scalar reference (AO mean 0.85431 vs 0.85601 at t=4871). Blur off, the
> difference is exactly integral: 82.60 % bit-identical, 4.94 % -1 bit, 11.79 %
> +1, 0.65 % +/-2, 0.0037 % non-integer. **This is §12.4's `--deferred_vec`
> finding in a second kernel.** REPORTED, NOT FIXED — contended file, perf
> decision on ~40 % of his acceptance frame. Backlog item.
>
> **Ruled out, measured:** MSL fast math. New `--slow_math` (fastMathEnabled = NO)
> moves the arithmetic-only comparison by **three pixels** out of 360 790.
>
> **BYTE-NULL, twice over.** ABSOLUTE: the recorded greets acceptance pin
> reproduces exactly with this diff in — t=5743 `26ad272aaa6cc9050c66e84cdaaf5436`.
> DIFFERENTIAL (taken first): two DEMO binaries from the identical tree with and
> without the FDS diff — greets t=5743 `ff2169dfa37317081b60b3d63d0aba49`,
> t=2845 `7e0dba9dfca9149992e0ef1cf5f25f83` (same recipe minus
> `--greets-displace`), identical on both. GPU: with `--ssao`
> off the §11 pose is still `d3a8301a22495c80dfdd5c3f8509f771` — the recorded E6
> value — even though the normal plane widened `RG16Snorm` -> `RGBA16Snorm` (`.xy`
> shading normal bit-for-bit, `.zw` the GEOMETRIC normal, because the CPU's SSAO
> reads geometric normals and this arm only had perturbed ones).
>
> **COST** (t=5743, median of 120 after 50 warmup): `ssao` pass 1.94 ms at
> downscale 1 / 1.57 ms at 2 / 2.04 ms hemisphere; FRAME TOTAL 4.83 -> 5.79 / 5.29 / 6.32 ms.
>

> ## 2026-08-17 — THE JAMB CUSHION IS FIXED: the minimal pair was free_edge+mitre, the instrument overturned two of three handed-on candidates, and what landed is the PER-BLOCK mitre level (+ a geometric-bisector hardening)
>
> Continues 2026-08-16y under his standing "keep going until you manage to fix
> this." Everything below is measured; scripts are `scratchpad/xsec.py`,
> `scratchpad/bulge_matrix.sh`, `scratchpad/constraint_battery.sh`,
> `scratchpad/battery_report.py`.
>
> **THE MINIMAL REPRODUCING PAIR — free_edge + mitre.** 16-arm pairwise matrix
> at t=5970 (singles + all 10 pairs, metric from the bake's own [STONE-FINALV]
> dump): the F+M pair reproduces the full five-flag arm's jamb metric TO THE
> DECIMAL (proud +18.2 milli-u / undulation 160.6 / asym 1.93); every other
> single and pair collapses to the no-free_edge baseline. B/W/P contribute
> nothing at this junction (consistent with 16y's one-off matrix).
>
> **TWO OF THE THREE HANDED-ON CANDIDATES DIED ON THE INSTRUMENT.**
> (2) "narrow the roll band" — the roll was never band-width;
> (3) "per-side plane ride" — the [STONE-MITRE-POP]/[STONE-MITRE-GEOM] census
> proved the fan bisector already EQUALS the geometric bisector here: the
> "lopsided 0.85/0.53" reading was my own projection onto a wall that does not
> exist — the jamb reveal is an authored OBLIQUE sheet, w1(+0.447,0,+0.894),
> 91 real faces. The weld geometry was always sound.
>
> **WHAT WAS ACTUALLY WRONG — the per-line CONSTANT level.** The 2026-08-15
> level fix welded each corner line to ONE median stone-top (medEnv). Each
> block's own top differs from that median by the stone-band wobble (~±0.05 h
> → ±15 milli-u at amp 0.3, [corner-vs-block mismatch: mean 6.2, max 15.4
> milli-u]), so every block either overhung the corner or showed a raised lip,
> and the faces rolled into it — the cushions/undulation his eye reads.
>
> **LANDED, default ON under the umbrella (opt-outs kept):**
> * `--greets_displace_block_level` — the mitre level is PIECEWISE CONSTANT:
>   one median of the local upper envelope per notch-bounded run (a stone
>   block), steps landing inside the groove cuts where masonry has joints.
>   Straight per block (no grazing S-bow), flush per block (no overhang/lip).
> * `--greets_displace_geom_bisector` — bisector + cosHalf from the incident
>   FACE-plane clusters instead of per-index fan normals (which straddle at
>   shared-index corners). At this jamb it is a hardening: 133 px at max 7.
>
> **VERDICTS, all on renders:**
> * t=5970/5975: the arris reads crisp, the reveal smear narrows, and the
>   residual face softness is the TEXTURE's own baked shading (proved by the
>   no-displace discriminator at the same crop). t=5975's black tear slits
>   COLLAPSE: z==0 background 1122 → 319 px; t=5967 647 → 458.
> * t=6001/6039 (the approved arris): notched masonry read SURVIVES, eyeballed
>   both arms at 2x — arguably straighter per block. t=1088 wall gap: 0 px.
> * 08-12 five: 5799/5869/5929 byte-identical (0 px), 5967/5987 improve/hold.
> * Full 25-pose battery on the fixed binary: no new tears (worst black counts
>   t5958_c 5887 / t5534_back 1929 are BYTE-IDENTICAL to the off arm).
> * `render_gate.sh` 4/4 PASS. greets t=1588 pin `570a7b44` reproduces exactly
>   (its recipe has no `--greets-displace`). Off-arm (`--no-` both flags)
>   reproduces the parent binary BYTE-EXACTLY at t=5970.
>
> **ACCEPTANCE PINS: two of four move, quantified.** t=5743 `440aa6bb…` and
> t=2845 `00d17bc5…` reproduce UNCHANGED 3/3. t=6097 and t=6133 move (the
> corner/mirror poses): 93 219 px (4.50 %, max 130) / 116 832 px (5.63 %, max
> 146), background unchanged (545→545, 1062→1064). New values in the gates
> table, old struck in place.
>
> Images: `docs/img/bulgecushion/t5970_jamb3x_{before,after,nodisplace}.png`,
> `t5975_reveal_{before,after}.png`, `t6001_arris_{before,after}.png`,
> `t6039_arris_{before,after}.png`.
> ## 2026-08-17 — **THE COMMISSION LANDS**: `--refl_correct` default ON. chase's reflected pass has a normal and a mirrored light for the first time; city's pins **could never have seen it**; and the one thing that could have done lasting damage was a probe bake nobody had looked at
>
> Full write-up: `docs/OPTIMIZATION_BACKLOG.md` **2026-08-17**; price in
> `docs/PERF_STATE.md` §00j; images in `docs/img/reflmir/`.
> Commissioned by Gil-Ad 2026-08-16 — *"for chase, commission the correct look;
> there is no '98 look to compare to anyway."* 16w characterised this and
> refused to land it because the direction was a look call. It is landed now,
> **default ON**, with `--no-refl_correct` as an exact escape hatch:
> **12/12 pin recipes byte-identical to the parent with the flag off.**
>
> ### THE THREE THINGS WORTH KNOWING
>
> 1. **chase moves everywhere, 8.5 – 27.0 % of the frame** at t=100/400/800/
>    1000/1300/1600, max |Δ| 73. Reflections stop being milky wedges and start
>    carrying the object's own colour: the reflected lighthouse's red/white
>    bands read as bands, the island reflections take the island's rock tone,
>    the mech's reflection in city gains legible limbs. Seven chase hashes
>    re-pinned in the gates table below.
> 2. **city's pins are blind to it — and that is a measurement, not an excuse.**
>    `RunCitySnapshot` ticks once per timestamp; city's water carries no
>    mirrored content on the first tick (1 tick byte-identical, 2 ticks differ).
>    Continuous play moves **277 214 px / 13.37 %** at t=1961. City's three pin
>    values are therefore UNCHANGED, and must not be read as "no look change".
> 3. **A nested probe bake was baking mirrored lights into the ON-DISK cube
>    cache.** `renderFrame`'s prologue runs `EnvReflection_FramePrep` inside the
>    armed reflected pass; its six cube-face renders reached
>    `Render_DeferredLighting` again. `ReflMirror_MirrorLights` now returns early
>    when `g_offscreenViewDepth != 0`. **It fires:** the unguarded child moved
>    city t=2400 by 22 px; the guarded one returns the parent's hash exactly.
>
> ### WHAT READS WORSE — flagged for the user's eye
>
> chase's lighthouse light shafts read **thinner**, and one sky shaft vanishes.
> The volumetric passes read only `ctx.lights`, so cones and halos inherit the
> mirror for free; chase never passes `skipVolumetric`, so its reflected pass had
> been painting a second, unmirrored shaft across the sky. Removing it is more
> correct — a reflection has no business above the waterline — but it dims a
> signature element. `docs/img/reflmir/chase_t000800_{before,after}.png`.
>
> ### GATES
>
> * `--no-refl_correct` on the child == parent, **12/12 recipes byte-identical**.
> * **greets ×5 and fountain unmoved, proven by pin** (neither has a
>   `Reflected_Transform`): greets t=1588 `570a7b44…`, acceptance ×4
>   `440aa6bb…`/`00d17bc5…`/`135ea9dd…`/`aaeb89b6…`, fountain `8db68ccb…`.
> * city plain `bd4ffbf8…`, acceptance t=1961 `4cb8d2ca…`, t=2400 `f473fe2b…` —
>   all unmoved (see point 2).
> * `--shadow_plane_hash` unchanged `51344bf5f3816c23`.
> * **`tools/render_gate.sh` has ZERO coverage of this** — none of its four
>   scenes has a `Reflected_Transform`. Said plainly instead of run for a green
>   tick that would mean nothing.
> * Pre-existing drift recorded, not papered over: the chase CINEMATIC row's
>   old values (`857d899d…`/`567e6153…`) do not reproduce on the unmodified
>   parent; `c2185330…`/`3fc9686c…` do, 3/3.
>
> ### AND ONE FACT THAT RE-FRAMES EVERY PIN IN THIS CAMPAIGN
>
> `env_refl` is **not** the dormant default-0 the flag text claimed: greets' PBR
> metallic import calls `setDefault(env_refl, true)` process-globally
> (`MaterialImport.cpp:865`), greets inits FIRST, and `setDefault` is one-way —
> so a full `./DEMO` run has it ON in city and chase, while `--snapshot=<scene>`
> (which inits only that scene) does not. Every snapshot pin measures a
> configuration the shipping demo does not have.


> ## 2026-08-16z — 16b's LAST THREE CITY ITEMS, ALL PRICED IN ONE ROUND: the `atanf` and the live-water function pointer are **below bar, closed with numbers**; the punt census that refuted the third **found the item that pays** — 61 056 px a pass going scalar because 1512/12 is not a multiple of 8
>
> Full write-up: `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16z**; `docs/PERF_STATE.md`
> §00b carries the amendment. **One landing, byte-null at 12 pins; two
> refutations, each with its ladder committed so nobody re-derives them.** All
> four instruments are compile-time switches and proven null — the shipping
> `DEMO` md5s `44be69e4…` with them compiled out, identical to its parent.
>
> ### ITEM 1 — the glow integral's `atanf`: 609 214 CALLS A FRAME, AND DELETING ALL OF THEM IS 0.79 % OF THE FRAME
>
> `-DFDS_FOG_ATAN_CENSUS=1` answers both playbook questions with counts:
> **not tableable** (609 214 all-distinct arguments a frame at t=1961) and
> **not hoistable** (`(2αz+β)/√disc` varies per column, per light AND per
> slice). It also kills the reorder move without a build: **96.5 % of the atans
> CONTRIBUTE**, 0.4 % are discarded by a later test, and deferring the atan
> costs an extra one per contiguous run. `-DFDS_GLOW_ATAN=n` then prices it:
> the atan deleted OUTRIGHT is `fog-glow` 0.092 → 0.061 Gi/f (−33.7 %) and
> **`renderFrame` −0.79 % at t=1961, −0.47 % at t=400** — and the frame wall does
> not resolve it at t=400 at all. A polynomial atan, the only attack left,
> collects **0.24 %** and is a numerics judge call on a *difference* of atans.
> **Below bar. Not landed.** Bonus: `Froxel_ColumnTile`'s pass-2 copy of the
> loop makes **zero** calls in city, so all of §00b row 10's `atanf` is the
> coarse glow grid's.
>
> ### ITEM 2 — the composite punt is REFUTED; its census found 61 056 free pixels a pass
>
> `-DFDS_FOG_PUNT_CENSUS=1`: 41 898 of 152 640 groups punt to the scalar
> composite at t=1961 (27.4 %) and **87.7 % of them have all EIGHT lanes
> reflective** — a punted group is the water region, not a boundary, so "punt
> only the LANES" recovers ≤6 % and is refused before building. What the same
> census counted: `tsx = ceil(1512/12) = 126 = 15 groups + 6 leftover`, so
> `Froxel_CompositeTileVec8`'s tail loop hands **6 px × 106 rows × 96 tiles =
> 61 056 px per composite pass** to the per-pixel path, in BOTH passes, 4.76 %
> of the frame, for no reason but arithmetic.
>
> **`--fog_composite_tile_align8`, default ON, BYTE-NULL**: round the
> composite's per-tile X span up to a multiple of 8 (eleven tiles of 128 + one
> of 104 = 13 groups), tail loop runs zero times.
>
> **READ THE RESOLUTION BEFORE QUOTING IT.** `ceil(1920/12) = 160` is already
> 8-aligned, so at his stock `rev.cfg` resolution this flag is a measured
> NO-OP. It pays at the campaign's 1512×848 and at 1280 / 1024 / 800 / 640 /
> 1366 / 2560.
>
> ### ITEM 3 — the live-water tilt's function pointer: THE CALL IS NOT THE COST
>
> `-DFDS_LWTILT_CENSUS=1`: **94 483 / 37 429 / 103 538** `EnvLiveWater_TiltDir`
> calls a frame at t=1961 / 2400 / 400, **all in the main pass**.
> `-DFDS_LWTILT_ABLATE=n` splits the cost: a DIRECT devirtualized call (LTO free
> to inline; it needs a layering violation the shipping tree must not make) is
> `renderFrame` **−0.10 %**, while zeroing the slope entirely — the ceiling for
> ANY lane-walk restructure — is **−0.38 %** (t=400: −0.06 % / −0.57 %). So the
> indirection is 27 % of it and the arithmetic 73 %, and an 8-wide form at
> 16e's measured 3.6× headroom tops out at **0.26 % of the frame** before paying
> for the restructure 16h measured at **+8 % to +23 %**. **Refuted, not built.**
> Correction to §00b: of `--env_live_water`'s +0.041 Gi/f, the slope evaluation
> is **0.015** and the mask read + weight + plane-hit + re-projection is 0.026.
>
> ### GATES
>
> * **11 pin recipes / 15 hashes 3/3 at their recorded values** on the
>   default-ON binary and **2/2 with `--no-fog_composite_tile_align8`**,
>   byte-identical arm to arm and to the parent.
> * `render_gate.sh` **4/4 PASS** (`conetest` IS the fog path);
>   `--shadow_plane_hash` **`51344bf5f3816c23`** 2/2 per binary.
> * **The two refutation commits leave the shipping binary byte-identical to its
>   parent** (`44be69e4…`) — null to the executable, not merely to the pixels.
> * **After the rebase onto `7763281d`** (`ssao_downscale` 1 → 2): re-run
>   pairwise, **all 11 recipes byte-identical between that parent and this tip,
>   0 mismatches.** That commit moves the four greets acceptance pins, which now
>   read t=5743 **`440aa6bb`**, t=2845 **`00d17bc5`**, t=6097 **`135ea9dd`**,
>   t=6133 **`aaeb89b6`**; everything else is unchanged.
>
> ### A BATTERY TRAP, REPRODUCED — the plain-city pin needs a WARM env cache
>
> `bd4ffbf8…` does **not** reproduce on the first run in a fresh worktree: with
> `Runtime/cache/` removed the same binary and recipe give
> **`31035019890c02083af0fb70c3384ed2`**, and the very next run gives
> `bd4ffbf8…`. It is the pin the tracked battery runs FIRST, so a fresh
> worktree reads it as a one-row failure; the `FDS_CITY_ENV_PIXEL=1` prefix,
> which *looks* like the culprit because it is written as `VAR=x shellfunc`, is
> NOT (bash exports that form — verified separately). "Discard run 1" applies
> to the pins, not only to the bench.

> ## 2026-08-16y — **`--ssao_downscale=2` is the default**, countersigned. −9.6 ms of his greets frame, realized and measured, not projected; the flip is proved to be PURE DEFAULT MOTION in both directions
>
> **COUNTERSIGN, verbatim (Gil-Ad, 2026-08-16): "ssao downscale 2 is ok (no
> downscale looks much better, but too slow)."** Full-res is preferred
> aesthetically and rejected on cost. `FDS/Base/FeatureFlags.def`
> `ssao_downscale` default **1 → 2**, and the help text now carries the
> countersign and that rationale so the next round cannot re-open it as an
> unowned look question.
>
> ### THE REALIZED WIN (not the round's projection)
>
> Interleaved parent-vs-child, two binaries in ONE worktree / ONE asset tree,
> `prof1.py`, **12 rounds with round 0 dropped = min-of-11**, 1512×848 (his
> window), his acceptance arm
> (`--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao
> --greets-displace --deferred_prof=1 --hw_prof --profiler=1`), load 16 → 7:
>
> | | frame min t=5743 | frame min t=6097 | `ssao` ms | `ssao` Ginstr/f | `renderFrame` Ginstr/f |
> |---|--:|--:|--:|--:|--:|
> | `ssao_downscale=1` (old default) | 49.25 | 41.39 | 14.55 / 14.23 | 1.650 / 1.652 | 4.686 / 4.191 |
> | **`=2` (NEW DEFAULT)** | **39.62** | **31.86** | **4.96 / 4.93** | **0.603 / 0.604** | **3.639 / 3.143** |
> | delta | **−9.63 ms (−19.6 %)** | **−9.53 ms (−23.0 %)** | −66 % / −65 % | −63.5 % | −22.3 % / −25.0 % |
>
> The 2026-08-16 dial round projected **−9.1 ms**; realized is **−9.6 ms** at
> both poses. `gbuffer` and `DeferredLighting-call` are flat to three decimals
> of Ginstr/f in the same batch — the only thing that moved is `ssao`, which is
> what the change claims.
>
> ### THE FLIP IS PURE DEFAULT MOTION, PROVED IN BOTH DIRECTIONS
>
> * child + `--ssao_downscale=1` → the four **OLD** acceptance hashes, EXACTLY, 3/3.
> * parent + `--ssao_downscale=2` → the four **NEW** acceptance hashes, EXACTLY, 3/3.
>
> A bijection between the two arms means no second edit rode along. `=1` still
> restores full-res exactly, at both resolutions.
>
> ### NEW PINS (gates table below carries them, old values struck in place)
>
> 1920×1080 acceptance ×4: t=5743 **`440aa6bb…`**, t=2845 **`00d17bc5…`**,
> t=6097 **`135ea9dd…`**, t=6133 **`aaeb89b6…`** (3/3 each).
> 1512×848 sensitivity row, re-taken: **`4667aa83…` / `c250427…` /
> `85bbb555…` / `15de194a…`** (3/3; the parent reproduced the recorded 1512×848
> set 3/3 first, which is what licenses the re-take).
>
> ### GATES — nothing else moves, and it is proved DIFFERENTIALLY
>
> * **10 pin recipes, 3/3 on each binary, at their recorded values**: city plain
>   `4a094fba` (no `FDS_CITY_ENV_PIXEL`), city arm `4cb8d2ca` / t=2400 `f473fe2b`
>   / t=400 `d3374de6`, chase 5-pose `3bfd4244` / `42d79fad` / `622b96a2` /
>   `31aa5203` / `ca07a814`, fountain `8db68ccb`, greets t=1588 `570a7b44`.
>   **Every one byte-identical parent-to-child** — ssao is greets-acceptance-arm-only
>   today and this is the proof, not the argument.
> * `render_gate.sh` **4/4 PASS on BOTH binaries** (`4ac809e5` / `826c09e6` /
>   `b41894f9` / `166fa25a`).
> * `--shadow_plane_hash` **`03587397…`** (the recorded value) — 3-line [SPH]
>   stream byte-identical parent-to-child, 2/2 each.
> * Eyeballed at t=5743 against the dial round's own crop region (located by
>   multi-scale template match at (672,544), 192², the source of
>   `dial_t5743_crop_d1_d2_d4.png`): **`docs/img/ssaoperf/default_flip_t5743_crop_d1_d2.png`**
>   (d=1 | d=2 side by side, 3× nearest), full frame
>   **`docs/img/ssaoperf/default_flip_t5743_full_d2.jpg`**. Quantitatively the
>   render IS the accepted variant: 52.5 % of pixels moved, mean |Δ| **0.869**
>   over channels on the moved, **max 74** — the dial round recorded 44–63 %,
>   mean 0.53–0.87, max **74** at this exact pose; and the per-pixel delta field
>   correlates **0.859** with the recorded d2−d1 delta (against 0.804 for
>   d4−d1, the discriminating control).
>
> ### WHAT THIS DOES NOT TOUCH
>
> Nothing outside the SSAO pass. `--ssao` itself is still default OFF, so only
> arms that already ask for SSAO see the dial at all — city, chase, fountain and
> the whole gate suite are byte-identical across the flip. The remaining SSAO
> items from the dial round (the per-lane scalar slice setup, the two
> `_mm256_sqrt_ps`) are unaffected and stay in the backlog; note the denoise now
> shrinks quadratically with the dial, so their absolute headroom is ~4× smaller
> than the round priced them at.
> ## 2026-08-16y — THE BULGE IS REAL, IT IS THE APPROVED ARM ITSELF, AND IT IS NOT A REGRESSION: bit-identical since the day the corner arm shipped; no single flag owns it
>
> His report: "so why the fuck there is still a fucking bulge?" — no pose given.
> Hunted on tip `8cc5e5e7`, 27-pose battery (docs/greets_review_poses.txt + the
> corner cams + the 08-12 five + t=1088), his acceptance arm, 1920×1080, plus a
> flag-matrix at t=5970. Everything below is measured on renders; the battery
> script is `scratchpad/bulge_battery.sh`.
>
> **WHERE IT SHOWS.** t=5970 and t=5975 (the doorway jamb): the block faces
> round outward approaching the arris, the reveal carries a smeared vertical
> band, the arris silhouette undulates — cushions, not dressed stone.
> t=6001/t=6039 read as the approved notched masonry; t=1088's wall gap stays
> sealed; the mirror/graze/corridor poses show nothing new. Full grid:
> `docs/img/bulgehunt/` (arm vs no-displace vs all-off crops, 8× diff heat).
>
> **IT IS NOT A REGRESSION — measured three ways.**
> tip-bare == bc823331+explicit-arm: **22 px differ of 2 073 600**. The bake's
> [STONE] summary counts are identical, and tip with all five corner flags off
> == bc823331-bare **byte-exact (0 px)** — the shared bake (incl. the
> zero-normal/NaN fixes since) moved nothing at this pose. The look he is
> seeing today is the look the arm shipped with on 2026-08-15; the cornerlvl
> acceptance evidence was face-region crops that do not contain the jamb
> cushions.
>
> **NO SINGLE FLAG OWNS IT — the one-off matrix at t=5970** (vs tip-bare):
> `--no-…free_edge` 36.6% differs, `border_mean=0` 0.47%, `--no-…seam_weld`
> **0.00% (inert here: the weld merges 0 verts at this junction)**,
> `--no-…plane_normal` 84.1%, `--no-…mitre` 33.6% — and EVERY one of those
> arms still cushions (crops in the grid). All five off = flat (the old dead
> look). The cushion is an interaction of the arm, not a knob.
>
> **Interior facts that bound the mechanism:** the bake displaces
> [-0.131..+0.033] — recess-dominant; a face can only stand +0.033 proud, so
> the cushions are the roll from managed border levels down into deep grooves
> spread across the face, not gross outward push. The 91.1° split seams at
> this jamb are position-coincident pairs the seam weld does NOT merge
> (nMerged=0), and [STONE-BMEAN]/[STONE-WELD] never fire in the bake log at
> tip-bare — which of the border stages actually holds these border verts, and
> at what level, is exactly what the next round must instrument.
>
> **METHOD TRAP RE-EARNED (cost this hunt an hour):** `git checkout <old>` in
> a measurement worktree without `cmake --build` = every subsequent "tip"
> render runs the OLD binary. A whole false "flag-machinery bug" was measured,
> and died only on a rebuild. Check `git log -1` + rebuild before EVERY arm.
>
> **HANDED ON (the fix round):** (1) pairwise flag matrix at t=5970 to isolate
> the interacting pair; (2) a border-band cross-section instrument (extend
> [STONE-FREEV]: dump vert level vs its line's field profile along the jamb
> border) to show the roll in numbers; (3) fix candidates, in order: border
> level FOLLOWS the field along the line (level-shifted so plateau meets
> stone-top — kills the constant-level roll while keeping 483ee71e's
> flushness), narrow the roll band to the densification width, per-side
> plane-normal ride at ≥90° junctions. Acceptance: his corner cams 5970/5975
> read as dressed stone without losing 6001/6039's approved arris; the 08-12
> gap/crack directive still holds at >90° grooves.
> ## 2026-08-16x — THE COLLINEAR-NEEDLE CULL: **the faces two rounds wanted to cull are already free**, and the cull that pays is the SCREEN determinant at the push. Landed default ON, byte-null by construction — **and the frame does not resolve it**. Plus: the greets acceptance pins were never orphaned
>
> Two items, two commits. Full write-ups: `docs/OPTIMIZATION_BACKLOG.md`
> **2026-08-16x**; the pin recipe is now a row in the gates table below.
>
> ### ITEM 1 — THE PREMISE WAS WRONG BEFORE THE CULL WAS WORTH WRITING
>
> `-DFDS_NEEDLE_CENSUS=1` (new compile switch; shipping `DEMO` md5s identically
> with its macros compiled out) classifies every face the transform WALKS in
> **object space** and again at the push:
>
> * **chase has ZERO collinear faces** — 0 of 42 932 walked, at t=100 and t=800.
>   Its **2 302 degenerate rejects a frame are 100 % pose-dependent** (edge-on
>   quads, sub-pixel slivers). A load-time scan would have found nothing.
> * **city HAS 182–203 a pass** (16v's `bilding type 1 windows`) **and not one
>   is ever PUSHED**: a zero-area face keeps the un-normalized zero `N` that
>   `Compute_Face_Normals` leaves it, so its backface test is `0 < 0` = false.
>   They cost one dot product and nothing downstream — **the "525 needles paying
>   transform + clip + sort every frame" reading was wrong.**
> * The only place they cost anything is **greets' shadow bake**, where backface
>   culling is off by design: **724 of the 5 408 faces culled there** are 3-D
>   degenerate. That is the whole prize a load-time cull could have won.
>
> **So `--needle_cull` (default ON) does the rasterizers' OWN test one stage
> earlier**: at the FList push, drop the face when its projected screen
> determinant is `<= 0.01f` — verbatim the value `Mekalele.h`,
> `TheOtherBarry.h` and `ShadowMap.cpp` all reject a fan triangle at. Live at
> all three FList builders (`Transform_Objects` main/shadow/offscreen + both
> `Reflected_Transform`s). **Byte-null BY CONSTRUCTION and the construction is
> verified in code**: `FrustumClipper::FInterpolator`'s first line lerps PX/PY
> **linearly in screen space**, so every clipped/subdivided polygon lies inside
> the projected triangle and every fan triangle has `|det|` no larger than the
> face's. Guards: all three verts in FRONT of the near plane (behind-near PX/PY
> are stale), never sprites (A == B). Measured at the rasterizer: chase t=100
> degenerate rejects **1 191 → 8** (main) and **1 111 → 7** (reflected) for
> 1 183 + 1 104 faces culled, and the ACCEPTED triangle counts **19 420 → 19 420
> / 19 092 → 19 092** — not one accepted triangle came from a culled face.
>
> **THE PRICE, honestly (1512×848, off/on/floor interleaved, 16 rounds/arm on
> chase):** the row that carries the work moves — chase t=100 gbuffer
> **−1.60 % instructions (exact, every round) and −0.30 ms** (−3.7 % median,
> −4.9 % min) against floors of +0.5 % / −0.3 %; chase t=800 −0.60 %
> instructions. **The FRAME does not resolve it**: `renderFrame`'s wall delta at
> t=100 is −2.54 % in one ladder and **+1.12 % in the other**, its instruction
> delta −0.13 %. city −0.07 %, greets **+0.00 %** frame instructions. **A row
> win in one scene, frame-neutral elsewhere, nothing lost anywhere** — landed ON
> because it is a strict work-remover that costs ~6 flops on values the push has
> already loaded, not because the frame got faster by a quotable number.
>
> ### ITEM 2 — THE FOUR GREETS ACCEPTANCE PINS REPRODUCE, FIRST TRY
>
> 16w's "written down nowhere in `docs/`, eight arms tried, none reproduces" was
> right about `docs/` and wrong about the pins. The recipe was in
> **`scratchpad/xform_pins.sh`**, 16r's own untracked battery:
> `./DEMO --snapshot=greets@t=<T> --out=<dir> --deferred --hdr --hdr-linear
> --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0`, one
> pose per process, stock 1920×1080 `rev.cfg`, **no `FDS_GREETS_CAM`**. 3/3 on
> two binaries. **Nothing retired.** The three things that move it (measured):
> `FDS_GREETS_CAM` set → `19d94f48…` (the likeliest thing the eight candidates
> did — `docs/greets_review_poses.txt` lists a camera for all four t values, and
> the t=1588 pin next door REQUIRES the prefix); an explicit `--profiler` →
> `cb7f4a51…`; 1512×848 → a different self-consistent set, 2/2.
>
> ### GATES
>
> * **12 pin recipes at their recorded values on three binaries** — 3/3 OFF and
>   3/3 ON on one binary (byte-identical arm to arm), 1/1 on the default-ON build.
> * `render_gate.sh` **4/4 PASS** on all three arms.
> * `--shadow_plane_hash` **`03587397…`** (recorded) 2/2 per arm — the gate that
>   matters most, since the shadow bake is where the most faces are culled.
> * **crash**, which no pin covers, byte-identical with and without the cull.

> ## 2026-08-16w — 16v's 19 092 zero-TN triangles are **the whole reflected pass**: `Reflected_Transform` has never written a view normal, in chase OR city. Named, counted exactly, counterfactual rendered — **and NOT landed, because it is a look call and it exposes a second defect under it**
>
> Full write-up: `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16w**. The census and
> both counterfactual arms are compile-time-gated and **byte-null to the binary
> itself** — the shipping `DEMO` md5s identically before and after
> (`45e9aa34…`). The one landed change that does alter codegen (the `[MESH]`
> load warning) clears the ordinary 12-pin bar.
>
> ### THE MECHANISM — `DEMO/CHASE.CPP:260`
>
> chase mirrors its GEOMETRY, not its camera: each tick is
> `Reflected_Transform` → sort → `Render()`, then `Transform_Objects` → sort →
> `Render()` (`CHASE.CPP:1447` / `1470`). `Reflected_Transform` is a **second,
> demo-side transform** — a 1998 copy of the main one — whose three non-Phong
> vertex loops write `TPos_AOS`, `RZ`, `PX`, `PY`, `Flags` **and no `TN`, and no
> `TTangent`**, into the SAME `T->Verts[]` the main pass uses. So 16v's audit was
> right and looking in the wrong function: the reflected pass is not a
> `Transform_Objects` call site, so no counter inside `Transform.cpp` can ever
> see it. `CITY.CPP:439` is the same function with the same omission.
>
> ### THE COUNT IS EXACT
>
> `-DFDS_REFLTN_CENSUS=1` (new, compile switch, never shipped, proven pixel-null)
> counts at the triangle the tiled rasterizer accepts, and the scene calls a
> `noinline` reporter after each `Render()`. chase t=100: **REFLECTED 19 092
> tris, 19 092 all-TN-zero (100 %); MAIN 19 420, 0.** **19 092 + 19 420 =
> 38 512** — 16v's two numbers are one pass each. t=100 is 100 % because
> `RunChaseSnapshot` ticks once per timestamp, so it is the process's FIRST tick
> and nothing has written `TN` yet; later poses keep only the meshes the previous
> main pass skipped (`'moutines surface'`, the terrain scrolling through
> `Tri_Invisible`). In continuous play the zero form is first-frame-only
> (t=98,99,100,101,102 → 19 078, then **0, 0, 0, 0**); the STALE form survives
> every frame.
>
> **Why city never showed it, measured:** **71** `Transform_Objects` calls have
> already run before city's first reflected pass (its init bakes), against **3**
> for chase — and chase's 3 are shadow-pass calls, which skip the TN write by
> design. City has the identical defect in its stale-normal form, permanently.
>
> ### THE COUNTERFACTUAL — NOT LANDED, HIS CALL
>
> `-DFDS_REFLTN_FIX=1` gives the three loops `MatrixXVector(IM, &Vtx->N,
> &Vtx->TN)` (+ tangent), `IM` staged exactly as `Transform.cpp:1901` stages the
> main pass's. **all-TN-zero → 0 at all five pins, triangle totals unmoved.** But
> the pixels: **t=100 94 804 px (4.57 %), t=800 181 591 (8.76 %), t=1000 269 669
> (13.00 %), t=1300 137 473 (max \|Δ\| 125), t=1600 9 206 — and t=1600 had ZERO
> zero-TN triangles.** The 19 092 was the tip; the reflected pass has never had a
> CORRECT normal at any pose, only an occasionally non-zero one.
>
> **Eyeballed, and it cuts both ways.** The reflected SHIPS gain their shading —
> today a black silhouette, corrected a lit hull with panel detail
> (`docs/img/refltn/chase_t000800_threeway.png`, the clearest single image). The
> reflected ISLAND skirts LOSE theirs — today a pale milky wedge under the
> waterline, corrected dark and gone (`chase_t000100_sbs.png`,
> `chase_t001300_sbs.png`). Full frames + magenta extent overlay:
> `docs/img/refltn/chase_t000{100,800}_{shipping,corrected}.png`,
> `chase_t000100_where.png`; mountains-dominant pose `chase_t001000_sbs.png`.
>
> ### AND THE REASON IT IS NOT A ONE-LINE FIX
>
> A third arm (`-DFDS_REFLTN_FIX=2`, the UNMIRRORED rotation = today's stale
> value made deterministic) reads like shipping (t=800: mean \|Δ\| 3.6 vs arm 1's
> 11.8). So the delta is dominated by the mirrored-vs-unmirrored **basis**, not
> by the zero — and the deferred light list is built in MAIN view space
> (`DeferredSurfaceKernel.cpp:7686`) and is **not** mirrored with the geometry.
> Arm 1 therefore lights a mirrored world with unmirrored lights: `N·L` flips
> where a real mirror would preserve it. **A correct reflected pass needs the
> LIGHTS mirrored too** — bigger than this round, and the reason the two-line
> write should not be landed on its own.
>
> ### ALSO LANDED (byte-null) / PRICED
>
> * **`[MESH]` load-time zero-normal warning** at `PREPROC.CPP:220`, the site
>   that creates the value (16u item 4). Unconditional, self-limiting at 10,
>   stderr only. Fires **once in the whole demo**: greets `66 of 3704 verts …
>   'momy-2'`. city / chase / fountain silent.
> * **city's 525 needles: PARKED, priced.** Degenerate rasterizer rejects/frame:
>   city 818 of 55 929 setups (**1.46 %**), **chase 2 302 of 38 512 (6.0 %)**.
>   Not worth a load-time geometry edit that perturbs the FList — and if one is
>   ever built, **chase is the scene**, keyed on the runtime reject rather than a
>   collinearity scan of `CITY.FLD`.
>
> ### GATES
>
> * **12 pin recipes 3/3 at their recorded values** (city `bd4ffbf8` /
>   `4cb8d2ca`, chase `3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` /
>   `ca07a814`, fountain `8db68ccb`, greets t=1588 `570a7b44`) + a four-pose
>   greets displaced-stone arm run differentially, 3/3.
>   **DOC GAP:** the flag list behind the recorded greets acceptance pins
>   (`26ad272a` / `10adec3a` / `418fc1fa` / `6d02f31b`) is written down nowhere
>   in `docs/`; eight candidate arms were tried, none reproduces them.
>   > **2026-08-16x — CLOSED, and the pins were never orphaned.** All four
>   > reproduce **3/3 on two binaries**, first try, under
>   > `./DEMO --snapshot=greets@t=<T> --out=<dir> --deferred --hdr --hdr-linear
>   > --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0`
>   > with **no `FDS_GREETS_CAM`** and stock 1920×1080 `rev.cfg`. The recipe was
>   > in `scratchpad/xform_pins.sh` (16r's own untracked battery), not in
>   > `docs/` — the gap was real, the conclusion drawn from it was not. Setting
>   > `FDS_GREETS_CAM` (which `docs/greets_review_poses.txt` tells you to do for
>   > these four t values) is what breaks it: t=5743 then gives `19d94f48…`.
>   > Full row + the three sensitivities now in the gates table below.
> * `render_gate.sh` **4/4 PASS**. `--shadow_plane_hash` identical parent vs
>   final, 2/2 stable each (`03587397…`).
> * No perf arm: the binary is byte-identical, so there is nothing to price.
>
> ### HANDED ON
>
> **The look call is his.** Arm 1 vs shipping at seven poses, images above. If
> the brighter reflected ships are wanted, the landing shape is the two-line
> write PLUS mirroring the light list for the reflected `Render()` (chase and
> city both) — not the write alone.
>

> ## 2026-08-16v — 16u's THREE LOOSE ENDS, CLOSED: city's zero normals are **collinear authored triangles (1575/1575), not cancellation** — and the normal plane's missing mask is **not latent: it fires 137 207 times a frame in chase**, where what gets stored today is decided by the host ISA's NaN→int rule
>
> Three guards landed, one instrument, **0 pixels move at every pin and every
> acceptance pose** — and the reason is different for each. Full write-up:
> `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16v**.
>
> ### THE NORMAL PLANE (hand-off #1) — GUARDED, AND THE CASE IS REACHABLE
>
> `Mekalele.h:3159` normalizes the interpolated view normal with no zero test.
> What that stores is not a NaN: `approx_rsqrt(0)=+inf`, `0*inf=NaN`, and the
> encode's `_mm256_cvtps_epi32(NaN)` is **0 on arm64/NEON** (measured) but the
> integer indefinite `0x80000000` on x86 (Intel SDM) → oct code `0` vs
> `0x80008000`, i.e. view-space **(0,0,1) here and ≈(0,0,−1) there**. The mask
> (`n2 > 1e-12`, stored word ANDed with it — the tangent plane's convention
> verbatim) makes that a decision instead of an ISA artifact, and code 0 IS
> `oct_encode(0,0,1)`, so arm64 is byte-identical.
>
> **Byte-nullity therefore cannot prove unreachability here** (the masked value
> and the accident coincide), so the control is a probe build
> (`-DFDS_ZERO_NORMAL_PROBE=1`) storing a loud code in exactly the masked lanes:
> city (9 poses + his arm), fountain, crash, greets ×4 **identical** — **chase
> differs at 4 of 5 pins**: t=100 **93 426 px (4.51 %)**, t=800 15 870, t=1200
> 3 868, t=400 252, t=1600 0. The probe's per-material counter: **137 207
> degenerate-normal lane stores at t=100, 96 % of them on `'moutines surface'`**
> — the island skirts at the waterline. **Eyeballed: it does not look like 4.5 %
> of a frame** — the surface is the SUBMERGED island geometry seen through the
> water, dark and low-contrast, and at max \|Δ\| 27/255 the two frames read the
> same at a glance. Which is why it survived. Images:
> `docs/img/zeronorm/chase_t000100_{shipping,probe,probe_diff,where}.png` (+ the
> t=000800 set).
>
> ### CITY'S WINDOWS MESHES (hand-off #3) — CLASSIFIED, THEN FIXED
>
> `--zero_normal_census` (new, default 0) classifies every `|N|==0` vertex as
> ORPHAN / ALL-DEGENERATE / CANCEL. City: **1575 verts over 35 meshes, 1575
> ALL-DEGENERATE, 0 CANCEL, 0 ORPHAN.** Each has exactly one incident face and
> that face is **collinear** — the hand-off's "REAL area 1.22e-4" is the float
> residue of a **102-unit-long, 1.2e-6-thick needle** (longest edge = the sum of
> the other two). They appear only AFTER `MakeFacesIndependentByAngle`: with
> `face->N == 0` its gate `Dot(face->N, adj->N) >= cos30` is `0 >= 0.866` for
> every neighbour including the face itself, so it returns `face->N` — 16u's
> chain, one stage later. Fixed in the same guard family: an empty accumulator
> plus a directionless `face->N` inherits `origVtx->N`, the normal the vertex had
> one stage earlier. **city 1575 → 0, crash 6 → 0, greets 1074 → 1058**, 0 px
> changed over 12 pins + an 18-arm differential.
>
> ### THE POM READERS (hand-off #2) — GUARDED
>
> Both sit inside `if (ctx.heightData && wantTangent)`, and `--parallax` is
> **default 1**, so the march is live in the shipping arms. Counted on the
> incident FACE: city/fountain/crash/chase have **0** zero-normal verts on a
> height-mapped face; **greets has 23**. The march's own two normalizes (normal
> AND tangent) were unguarded and run BEFORE the plane masks — it is the frame's
> first consumer. Both now length-guarded, degenerate lanes get the identity
> (T,B = 0 → no UV shift). Byte-null.
>
> ### PERF (both guards are per-pixel; 11 interleaved rounds, min-of-arm, floor arm)
>
> greets t=5743 **+0.053 ms (+0.07 %)** against a floor of −0.075 ms; city t=1961
> **−0.427 ms (−0.68 %)** against a floor of +0.170 ms. Not resolvable, signs
> disagree — no measurable slowdown, and the city figure is not a win to quote.
>
> ### GATES
>
> * **12 pin recipes 3/3 at their recorded values on four binaries** (census,
>   city fix, normal guard, guard+march guard) and on the assembled tree.
> * `render_gate.sh` **4/4 PASS**.
> * `--shadow_plane_hash` identical base-vs-final, 2/2 stable each.
> * `--zero_normal_census`: city 1575 → 0, crash 6 → 0, greets 1074 → 1058.
>
> ### HANDED ON — CHASE RASTERIZES HALF ITS TRIANGLES WITH NO VIEW NORMAL
>
> At chase t=100, **19 092 of the 38 512 triangles** of the single 1920-wide pass
> arrive with all three corner `TN == (0,0,0)`; none has one or two, so it is
> whole primitives, not interpolation. chase has **0** vertices with `|N| == 0`
> — the authored normals are fine, the zero is in the view-space `TN`. The
> transform's counters say all 23 229 of its TN writes went to the AoS `Vertex`
> and none of the four skip sites fired; the zero-TN vertices the rasterizer sees
> are the clipper's stack `C_Verts` copies, so the zero comes from their SOURCE.
> Those pixels are shaded from a fabricated camera-facing normal today; giving
> them their true normal WILL move the frame, so it is a look call — with the
> guard already in place to make whatever lands deterministic.
>
> ## 2026-08-16u — THE 216 NaN TANGENTS ARE ONE CAUSE, AND IT IS NOT THE DISPLACEMENT BAKE: **every one is a zero-area authored triangle normalized without a guard.** Fixed; **0 pixels move**, and the reason is a rasterizer reject with a line number, not luck
>
> 16t handed on *"216 vertices of greets' displaced `Piramid` chunks carry a NaN
> `Vertex::Tangent` — own round."* Instrumented (`--tangent_nan_census`),
> classified, fixed at the source, quantified. Full write-up:
> `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16u**.
>
> ### THE CAUSE, 216 / 216, ONE
>
> **All 216 have `|N| == 0`.** `Compute_Face_Normals` (`PREPROC.CPP:22`)
> *deliberately* leaves a degenerate (zero-area) face's `N` as the un-normalized
> zero cross; `MakeFacesIndependent`'s per-face clone returns `face->N` verbatim
> (its angle-gated accumulator rejects every neighbour including itself when
> `F->N == 0`); and `Compute_Vertex_Tangents`' perpendicular-axis fallback then
> called `Vector_Norm` on `Cross_Product(N=0, ref)` — and `Vector_Norm` is
> `Vector_Scale(V, 1.0/Vector_Length(V), V)`, so a zero vector is `0 * inf` =
> **NaN in all three lanes**.
>
> **None of the hand-off's hypotheses were right.** Not the weld, not the mitre,
> not the corner treatment, not the freed-border densification — and **not the
> displacement bake at all**: the counts are identical with and without
> `--greets_displace` (66 at load, 216 after `MakeFacesIndependentByAngle` = 72
> degenerate faces × 3 per-face clones, unchanged by
> `DisplaceStoneSmoothNormals`). Not even the displaced stone: the materials are
> `amudim` 192 / `momy-1` 12 / `momy-2` 12; `rooms`/`floor` contribute zero. And
> **not greets-only** — the same fallback fires 1386 times in `Initialize_City`
> and 6 in crash. The clone census found greets' because greets is what it walked.
>
> ### WHY IT NEVER SHOWED, AND WHY THE FIX MOVES NOTHING
>
> Each of the 216 has **exactly one incident face** and the **largest incident
> area over all of them is 1.54e-10**. All three rasterizers reject a fan
> triangle whose screen determinant is ~0 before any setup — `Mekalele.h:4012`,
> `TheOtherBarry.h:1098`, `ShadowMap.cpp:1156`, all `if (fabs(det) <= 0.01f)
> continue;`. Zero fragments, deferred / forward / shadow alike. And even if one
> had reached the lanes, `Mekalele.h:3189`'s `tLen2 > vEps` is **false for NaN**,
> so the G-buffer tangent is written 0 and `DeferredSurfaceKernel.cpp:2719` takes
> the Mikkelsen fallback — no black pixel, ever.
>
> ### THE FIX
>
> Length-guard the second normalize the way the branch above it and
> `Compute_Face_Normals` guard theirs; when `N` carries no direction, pin the
> tangent to **+X** (the axis the fallback's own `ref` choice leaves free). Twin
> guard in `DisplaceStoneSmoothNormals`, whose comment already claims to match
> PREPROC's — latent, 0 hits, kept in lockstep.
>
> ### LOOK DELTA: **0 px, and the value is unobservable, not merely equal**
>
> Differential, one worktree, one asset tree, run 1 discarded: **every arm
> byte-identical** — the four greets acceptance poses, greets t=1588, a camera
> parked on the `amudim` pillars so they fill the frame, two cameras on the
> mummies, a 9-pose city sweep (colour **and** z), chase ×5, fountain, crash.
> The poses are not blind: the census's post-render half projects the
> degenerate-normal verts through the rendered frame and **744 of the 1080
> copies land inside the 1920×1080 viewport at t=5743**, 40 distinct pixels,
> bbox x[200..1833] y[221..479]. The strong control: a third binary whose
> fallback emits **+Z instead of +X** is byte-identical to the +X one everywhere
> — so the tangent there is read by nothing that reaches a pixel.
>
> Images: `docs/img/nantan/greets_t5743_nantan_{before,after,diff,where,crop_strip}.png`,
> `docs/img/nantan/greets_amudim_nantan_{before,after,diff}.png` (`_where` marks
> the 40 on-screen positions; `_diff` is black).
>
> ### TRAP EARNED — A FLAG-GATED INSTRUMENT IS NOT AUTOMATICALLY BYTE-NULL
>
> The first census kept its counters and a `std::vector<float>` as **locals of
> `Compute_Vertex_Tangents`**, behind `if (tangent_nan_census())` and never
> executed. **That alone moved greets t=1588, `570a7b44` → `a045c99b`, 3/3
> stable on each binary**, while the acceptance poses, city, chase and fountain
> did not move. `-ffp-contract=fast` + thin LTO: extra live state across that
> function's two hot loops changes how the tangent solve fuses. Moving every
> counter into a `noinline` reporter that re-derives its numbers afterwards
> restored `570a7b44` exactly. Chase an unexplained pin move to its cause before
> blaming the change under test — here the change under test was innocent.
>
> ### GATES
>
> * **12 pin recipes, base-vs-fix identical AND at their recorded values** —
>   city `bd4ffbf8` / `4cb8d2ca`, chase `3bfd4244` / `42d79fad` / `622b96a2` /
>   `31aa5203` / `ca07a814`, fountain `8db68ccb`, greets t=1588 `570a7b44`, the
>   four greets acceptance poses `26ad272a` / `10adec3a` / `418fc1fa` / `6d02f31b`.
> * `--tangent_nan_census` **216 → 0** at all five greets stage boundaries.
> * `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
> * `--shadow_plane_hash` identical base-vs-fix, 2/2 stable each — `h=7f0f7d68…`,
>   `cum=6aa86b38…`, the 16t values.
>
> ### HANDED ON
>
> 1. **The normal plane has no `tValid` equivalent** — `Mekalele.h:3159-3163`
>    `approx_rsqrt(n2)` on the interpolated view normal, no zero guard, stored
>    unmasked. The same verts carry `TN == (0,0,0)`. Unreachable today for the
>    same reason the tangent was; the tangent had a mask and this does not.
> 2. **Two tangent readers sit upstream of the mask** — the POM march
>    (`Mekalele.h:1705`) and `--pom_normal` (`Mekalele.h:2438`, default 0).
> 3. **City's 21 `bilding type 1 windows` meshes have 9 verts each with
>    `|N| == 0` cornering a face of REAL area (1.22e-4)** — reachable geometry,
>    inert only because that material has no `NormalMap`/`HeightMap` so
>    `Mekalele.h:3816`'s `writeTangent` is false. An authoring accident, not a
>    guarantee. Worth a census of why those normals cancel.
>
> ## 2026-08-16t — THE NEVER-INVALIDATED CLONE: **`Pos` IS STALE ZERO TIMES IN 856 MILLION COMPARES.** The two fields that DO go stale are both recomputed downstream, and the bug worth fixing next door is a SIZE mismatch, not a value one
>
> **16s handed on "`PerTriMeshClone` is never invalidated, five files write
> `Vertex::Pos`, nothing enforces it — worth its own round." Built the census,
> counted, then made the divergence go away to see whether it had ever
> mattered.** Verdict: outcome **(b) + (c)** — no caster's geometry is ever
> stale, two non-geometry fields are, and neither can reach a pixel. Evidence:
> `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16t**,
> `docs/SOA_VERTEX_REFACTOR.md` (the hazard note, now resolved).
>
> ### WHAT THE CENSUS COUNTED — `--clone_stale_census`, census build
>
> Hardest arm: greets, his acceptance flags, **13-pose timeline sweep with
> `FDS_GREETS_SHATTER=1`**, so the shatter's 238-shard / 12-worker reflection
> bake — the SECOND clone-backed pass (`MirrorShatter.cpp:1375`, a per-worker
> `VertexScratch` kept warm across frames) — is live alongside the 42 shadow bakes.
>
> | | compares | diverged |
> |---|--:|--:|
> | clone reuses | 630 622 | 330 401 |
> | **`Pos` / `N` / `Tangent`** | 856 176 679 each | **0 / 0 / 0** |
> | tail (`BGRA` … `ShellH`) | 856 176 679 | 355 630 633 — **100 % `BGRA`** |
> | `Face` inputs | 285 626 101 | 644 742 — **100 % `EU1..EV3`**, all `__discoBall` |
> | clone array size vs live `VIndex`/`FIndex` | 630 622 | **0** |
>
> Zero in `UZ/VZ`, `EUZ/EVZ`, `U/V`, `EU/EV`, `i`, `OrigBary`, `ShellH`,
> `N`/`NormProd`, `U1..V3`, `LwDU/DV`, `Filler`/`Txtr`/`ReflectionTexture`, ids.
> **Both fields that move are rewritten downstream anyway** — `BGRA` by
> `Lighting(Scene*)` on the live mesh every frame (`Lighting.cpp:416`),
> `EU1..EV3` by the transform's own face loop per pass.
>
> **WHY IT HOLDS** (not luck — five files really do write `Pos`): the only
> per-frame writer of a live mesh's `Verts[].Pos` is `UpdateMirror`
> (`GreetsMirror.cpp:2134`), and its `__mirrorClone_*` targets carry
> `Tri_NoShadowCast`, which `Transform.cpp:1567` honours **~245 lines before
> `cloneOf` is reached**. Same for the disco ball's per-tick `LR/LG/LB`. Every
> other `Pos` writer is scene-load-time; rigid animation moves `IPos`/`RotMat`,
> which the transform reads off the `TriMesh`, never off the clone.
>
> ### AND IT DOES NOT REACH PIXELS — `--clone_refresh_inputs`, SHIPPING-shaped binary
>
> Level 1 re-copies the 88-byte vertex input block on every clone reuse, level 2
> also the `Face[]`. One binary, three levels, **eight greets configurations ×
> 44 md5s each** (four acceptance poses, the t=1588 pin recipe, the 13-pose
> sweep, the 13-pose sweep under `FDS_GREETS_SHATTER=1`, the shatter at t=5743):
> **levels 0 / 1 / 2 byte-identical everywhere.**
>
> ### WHAT LANDED
>
> * **`cloneOf` size-drift invalidation (DEFAULT ON, byte-null).** The real
>   defect next door: `Transform_Objects` walks the clone to the **LIVE** bound
>   (`VEnd = tVerts + T->VIndex`) while the storage is whatever FIRST use sized
>   it to — a mesh that grows after being cloned is read AND WRITTEN past its
>   allocation. `MeshOps_ResmoothSurface` grows `VIndex` to `FIndex*3` and
>   `DisplaceRebuild_Apply` re-runs the subdivision bake, both on live shadow-
>   casting meshes, both reachable from the material editor mid-session. Fixed
>   by rebuilding when the sizes disagree: **two integer compares on the
>   `cloneOf` map-MISS path** (the one-pointer-compare fast path untouched),
>   and the condition fired **0 times in 630 622 reuses**. NOT reproduced at
>   runtime — the editor trigger is interactive — so it is a code-level finding.
> * **`--clone_stale_census`** (instrument, census build) and
>   **`--clone_refresh_inputs`** (armed fix, default 0), plus the invariant
>   written down where the clone is built (`Base/VertexScratch.h`).
> * **`BuildCompoundMirrors` gets its missing `Tri_NoShadowCast`**
>   (`GreetsMirror.cpp:1884`) — its clone is re-mirrored every frame by the same
>   `UpdateMirror` and had no such flag, unlike its base-mirror twin. Inert (no
>   caller in the tree) and **the shipping binary is byte-identical after LTO**.
>
> **TRAP FOR THE NEXT CENSUS: compare BYTES.** The first version used `!=` on
> floats and reported a permanent `Tangent` divergence on four displaced-stone
> chunks — it was NaN, which is `!=` itself, so a frozen byte-identical clone
> reads as diverged forever.
>
> ### GATES
>
> * **11 pin recipes 3/3, parent-identical, all at their recorded 16f/16r
>   values** (city `bd4ffbf8` / `4cb8d2ca` / `f473fe2b` / `d3374de6`, chase
>   `3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain
>   `8db68ccb`, greets t=1588 `570a7b44`, greets acceptance t=5743 `26ad272a` /
>   t=2845 `10adec3a` / t=6097 `418fc1fa` / t=6133 `6d02f31b`).
> * `render_gate.sh` **4/4 PASS on BOTH binaries** (`4ac809e5` / `826c09e6` /
>   `b41894f9` / `166fa25a`).
> * `--shadow_plane_hash` **identical base vs child, 2/2 stable on each**
>   (`h=7f0f7d68…`, `cum=6aa86b38…`).
> * **Perf neutral**, min-of-11 order-rotated: `DynOmnis` core **10.250 →
>   10.250** ms, `DynMeshes` wall 0.210 → 0.210, `DynMeshes` core 0.680 → 0.670,
>   `DynOmnis` wall 1.190 → 1.200 (one printed LSB, columns disagree in sign).
> * Renders eyeballed, one per scene:
>   `docs/img/cloneinv/{greets_t005743,city_t001961,chase_t000800,fountain_t002500}_cloneinv.png`.
> * **Inertness control**: city / chase / fountain run **zero** clone-backed
>   passes (`reuses=0`) — this whole subject is greets-only.
>
> ### HANDS ON
>
> **Item 2 (the dense 32-byte out record) is re-priced and still PARKED — not
> because item 1 blocked it (it did not; it made the read-half's byte case
> stronger), but because of two coherency requirements 16s's spec did not name:
> `Vtx_Spike` shares the `Vertex::Flags` word and survives only because the
> transform's mask is `~Vtx_Visible`; and the `Ahead` loop leaves the PREVIOUS
> pass's `PX`/`PY`/`RZ` live for near-plane verts, so the dense array must be
> per-clone, persistent and seeded, never zero-initialised.** Ladder re-run on
> the post-fix tree, min-of-11: arm 32 → 288 → 1568 = 0.870 / 0.860 / **0.610**
> wall, 7.340 / 7.180 / **4.980** core, DynMeshes 0.170 / 0.170 / **0.140** —
> **0.280 ms/frame = 0.56 %**, 16s's number to two decimals. Full write-up in
> `docs/OPTIMIZATION_BACKLOG.md` 2026-08-16t.
>
> **SECOND FINDING, SAME SUBJECT, MEASURED: `--bake_tick_overlap` (and
> `--shadow_gbuffer_overlap`) leak ~400 MiB PER FRAME.** Clone lifetime is a
> property of the THREAD — `ShadowScratchTLS` is a `static thread_local` that is
> heap-allocated and deliberately never freed, which is correct only if the set
> of baking threads is bounded. `ShadowBake_DispatchGreets` constructs a NEW
> `std::thread` every frame under either flag (`Shadows.cpp:1516`).
> `--mem_census` at greets t=5743, his arm + `--bake_tick_overlap`: tick 2 = 3
> threads / 1 777 clones / **1.25 GiB**; tick 12 = 13 / 6 857 / **5.27 GiB**;
> tick 24 = 25 / 12 953 / **10.13 GiB**. One thread per frame, ~403 MiB/frame,
> unbounded; flat at 1 269 clones / 209.64 MiB without the flag. It also
> **invalidates any timing taken under those flags** — a fresh thread means a
> fresh clone set, so every frame pays the 1 269-clone init copy the design
> assumes is one-time. Both flags default 0 and no scene `setDefault` sets them,
> so nothing shipping is affected. Fix is one persistent orchestrator thread (the
> join point already exists) or moving the scratch off `thread_local`; threading
> work, own round, own gates.
>
> **Loose end, unrelated and NOT this round's item: 216 vertices of greets'
> displaced `Piramid` chunks carry a NaN `Vertex::Tangent`** under
> `--greets-displace` (`c149`, `c150`, `c162`, `c166`). `Tangent` → `TTangent` →
> tangent-space normal mapping, so those verts hand the kernel a NaN basis. Every
> pin in this round's battery still reproduces, so it is absorbed somewhere or
> lands where nobody has looked. `--clone_stale_census` prints `NaN-live N` per
> mesh. **Own round.**
>
> ## 2026-08-16s — SoA PHASE 5, PRICED BY REBUILDING THE LOOP INSTEAD OF THE STRUCT: **0.6 % OF A GREETS FRAME, NOT 1.25 %** — AND NEITHER HALF OF THE SPLIT PAYS ALONE
>
> **16r handed Phase 5 on at 1.25 % and said "argue against this number, not
> 0.3 %". The number is an extrapolation of a byte-slope calibrated by
> INFLATING `sizeof(Vertex)` 140 → 192, run in the direction it was never
> measured.** Nothing in the tree can shrink `Vertex` without doing the
> refactor, so instead of arguing, the **per-vertex loop was rebuilt as a ladder**
> — replicas that differ ONLY in where the read and the write land (identical FP
> sequence in the disassembly; the read-source select unswitched out of the
> loop) — and the end state timed. Evidence: `docs/PERF_STATE.md` **00h**,
> `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16s**, `docs/SOA_VERTEX_REFACTOR.md`
> top section. **Status: PARKED / BLOCKED ON SCOPE.** Nothing shipped but the
> instrument, and the shipping binary is byte-identical to the parent's.
>
> ### THE LADDER — greets t=5743, his arm, DynOmnis phase-A `xform`, face loop ablated
>
> | arm | `Pos` read from | outputs written to | wall ms | floor |
> |---|---|---|--:|--:|
> | **32** — what ships | per-light clone `Vertex` (140 B) | the same record | **0.870** | 0.00 % |
> | **288** — replica CONTROL | same | same | **0.840** | 1.19 % |
> | 544 | clone `Vertex` | dense 32 B/vert | 0.99 **(+16 %)** | 1.01 % |
> | 1056 | shared `T->Verts` | clone `Vertex` | 1.13 **(+33 %)** | 1.77 % |
> | 2080 | compact shared 12 B/vert `Pos` | clone `Vertex` | 0.87 **(0 %)** | 0.00 % |
> | **1568 — Phase 5's END STATE** | **shared `T->Verts`** | **dense 32 B/vert** | **0.600 (−28.6 %)** | 1.67 % |
>
> Verdict rows **min-of-11, order rotated, floors quoted** — signal-to-floor 17×;
> half-arms min-of-5. `DynMeshes` on the same runs **0.170 → 0.130 (−23.5 %)**;
> the core-ms column agrees (7.130 / 7.200 / … / **4.870**, −32.4 %).
>
> **Read the control first** (0.840 vs 0.870, opposite sign on the core column) — the replica IS the shipping loop,
> so any branch it carries cancels across arms. **Then read the three middle
> rows: each is one HALF of Phase 5, and every half alone is neutral or worse.**
> A `sizeof(Vertex)` model cannot produce that: arm 544 takes the 28 written
> bytes OUT of the walked record and costs **+16 %**.
>
> ### THE VARIABLE IS THE CLONE, NOT THE STRUCT — `--mem_census` names it in one line
>
> `shadow.scratch/per-light mesh clones (Vertex[])` = **209.64 MiB across 1 269
> (shadow-map × mesh) clones**, and **68 of every 140 of those bytes are
> read-only duplicates** (`Pos`/`N`/`Tangent`/UV/bary) — 42 concurrent bakes
> cannot share one `Vertex`. Today's loop is ONE stream over that. Split only
> the write and you keep the cold 209.6 MiB and add a stream; split only the
> read and you get two 140-byte strides. Do BOTH and the read collapses onto the
> single `T->Verts` (~15 MiB, warm across all 42 bakes) while the write goes
> dense. **That pair is the whole −31 %.**
>
> ### PREDICTION vs MEASUREMENT — over-predicted 2.0–2.2×
>
> | | ms/frame | % of a 49.59 ms frame |
> |---|--:|--:|
> | PREDICTED (16r: 72 B × 0.0086 ms/B) | 0.62 | **1.25 %** |
> | **MEASURED** (min-of-11, vs the replica control) | **0.28** | **0.56 %** |
> | same, vs the shipping arm | 0.31 | 0.63 % |
>
> Reproduces at t=5743 / 2845 / 1588 / 6097 → **0.61 / 0.61 / 0.66 / 0.68 %**.
> **The MAIN VIEW is ±5 %, neutral** (replica control 0.911 → 0.882 ms VERT;
> write-split alone +5.3 %) — no clone there, so nothing to collapse, and no
> regression either.
>
> ### BLOCKED ON SCOPE, COUNTED NOT ESTIMATED
>
> Of the mesh-side `out`-field derefs Phase 5 must migrate, **274 are in DEMO
> scene code** — `CITY.CPP` 128, `CHASE.CPP` 95, `FOUNTAIN.CPP` 51 — i.e. three
> whole alternative transform pipelines that must each learn to write the out
> array **or the image breaks silently**, which is precisely the Phase 6.1/6.2
> bug list plus the one it never found. That is past "Vertex layout + the
> transform/filler readers". **0.6 % of one scene's frame does not buy it.**
>
> ### THE SUCCESSOR ITEM, IF ANYONE RE-OPENS IT — shadow-only, does NOT touch `Vertex`
>
> A dense 32-byte out record on `PerTriMeshClone`; the shadow vertex loops write
> it and read `Pos` from `T->Verts`; the two clone-backed readers
> (`Transform_Objects`' face loop, `FrustumClipper::Render`'s `*A = *F->A`
> entry) source from it. `F->frame` / `F->A_idx` are **already plumbed for
> clone-backed faces**. No filler, no DEMO scene file, no layout change. Ceiling
> 0.6 %; the risk is that a runtime branch in that face loop is not byte-null
> under `-ffp-contract=fast` (`docs/VISIBILITY_PLAN.md` §8).
>
> ### TWO THINGS TO CARRY FORWARD
>
> * **The cheap half is byte-null AND worth zero.** Sourcing `Pos` from
>   `T->Verts` instead of the clone gives IDENTICAL snapshots (greets t=5743
>   `818f0336…`, t=1588 `756790e4…`, control repeated) and moves nothing (arm
>   2080). The read was already free — the line comes in for the write.
> * **`PerTriMeshClone` is NEVER invalidated.** Nothing in the tree clears
>   `VertexScratch::clones` or resets `initialized`, so a clone's `verts` —
>   `Pos` included — is a snapshot from that mesh's FIRST shadow bake, while
>   `DisplaceRebuild.cpp`, `MeshOps.cpp`, `GreetsDisco.cpp`, `MirrorShatter.cpp`
>   and `FOUNTAIN.CPP` all write `Vertex::Pos`. Not stale at the poses measured;
>   nothing enforces it. **Deserves its own round.**
>
> ### GATES
>
> * **Shipping binary byte-identical to the parent** (`md5 f5cc3479…`) — the
>   instrument is `#if FDS_VIS_CENSUS` and textually absent. Stronger than a pin.
> * 11 pin recipes **3/3 parent-identical**; ten at their recorded 16f/16r values
>   (city `4cb8d2ca` / `f473fe2b` / `d3374de6`, chase `3bfd4244` / `42d79fad` /
>   `622b96a2` / `31aa5203` / `ca07a814`, fountain `8db68ccb`, greets t=1588
>   `570a7b44`) plus the four greets acceptance poses (t=5743 `26ad272a`,
>   t=2845 `10adec3a`, t=6097 `418fc1fa`, t=6133 `6d02f31b`).
> * `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` /
>   `166fa25a`); `--shadow_plane_hash` stable 2/2 (`03587397…`, all 43 bakes).
> * Renders eyeballed, one per scene, all four matching their pins:
>   `docs/img/soa5/{greets_t_5743,city_t_1961,chase_t_800,fountain_t_2500}.png`.
> * Harness note (cost one confused reading): `city-t1961-plain`'s recorded
>   `bd4ffbf8` needs **`FDS_CITY_ENV_PIXEL=1` in the environment** — dropping it
>   gives a perfectly self-consistent `4a094fba` at every run. Recipe
>   transcription, not drift. Battery: `scratchpad/soa5_pins.sh <binA> <binB>`.
>
> ## 2026-08-16r — `Transform_Objects` (16q's 3.35 % ROW) IS NOT SHARED MACHINERY: IT IS GREETS' SHADOW BAKE, 42 OF ITS 45 CALLS A FRAME
>
> > **AMENDED 2026-08-16s (block above): the "Phase 5 is 1.25 %" hand-on is
> > refuted — built and timed, the end state is 0.56-0.63 %, and the variable is the
> > 209.6 MiB per-light clone, not `sizeof(Vertex)`. Everything else below
> > stands and is what located the clone.**
>
> **16q handed on "`Transform_Objects` at 3.35 % is now more than double what is
> left of the clipper". Reproduced (3.363 %) — and it is 0.309 % at city and
> 0.190 % at chase.** Decomposed by invocation source, then by ablation, then to
> the instruction. **Six attacks refuted by measurement, one landed.** Evidence:
> `docs/PERF_STATE.md` **00g**, `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16r**.
>
> | pass, greets t=5743 his arm | calls/f | core-ms | verts xformed |
> |---|--:|--:|--:|
> | MAIN | 1 | 1.93 | 201 751 |
> | MIRROR-RTT | 2 | 0.83 | 105 914 |
> | **SHADOW** | **42** | **11.78 (81 %)** | **596 446** |
> | OFFSCREEN probes (intermittent) | 0–2.7 | 0–2.29 | 257 604 |
>
> `DynOmnis` xform 1.22 ms/f splits: **vertex loops 0.83, per-face
> `VisibilityFlagsAll` 0.26, accepted-face work 0.05, Face walk 0.01, residue
> 0.07** — and the four sum to the total.
>
> ### REFUTED, EACH WITH THE MEASUREMENT
>
> * **SIMD width / ALU** — the whole projection block is 0.01–0.03 of 1.22 ms;
>   `--shadow_cube_vert_cull` (removes the 3-FMA matmul for out-of-pyramid verts)
>   moves core time 10.67 → **10.77**. Not ALU-bound.
> * **Scheduling** — new `--shadow-prof` columns `xformCore`/`effPar`: **8.4–8.7**,
>   and this box is **8 P-cores + 4 E-cores**. At the P-core count.
> * **`--shadow_cone_cull`** (default OFF) — ON: 10.67 → 10.55. Null.
> * **`--shadow_cube_face_cull`** — OFF: 10.67 → **13.15 (+23 %)**. The cull is
>   live; nothing dead to reclaim.
> * **More main-view shards** — `--xfrm_par` 26/52/104 → WORK 0.445/0.450/0.443.
>   Flat.
> * **The 3 dead SoA arrays in the shadow pass** (only `TPos_z` has a reader
>   there) — removing all four buys 0.10 ms ⇒ three of four ≈ **0.15 % of frame**.
>   Refused by arithmetic.
>
> ### THE PREDICTION THAT FAILED — and it corrects a closed doc by 4×
>
> The shadow pass touches a 64-byte hot window at a 140-byte stride (straddles two
> lines ~98 % of the time). Predicted: pad `sizeof(Vertex)` to 192 and halve the
> lines. Measured (`-DFDS_VERTEX_PAD_BYTES`): **140 → 1.21 ms, 144 → 1.23,
> 160 → 1.33, 192 (aligned) → 1.58.** Monotone in SIZE; the aligned arm is the
> WORST. Slope **0.0086 ms/byte**. **SoA Phase 5 was closed 2026-08-09 at
> "0.24–0.31 % of frame" on a MAIN-VIEW-ONLY instrument; at this slope 140 → 68 is
> 0.62 ms/f = 1.25 % of a greets frame.** `docs/SOA_VERTEX_REFACTOR.md` is amended
> in place at its top.
>
> ### WHAT LANDED — `--greets_displace_offscreen_skip` (default ON), byte-exact
>
> `--greets_displace` marks the **149 chunks that are 100 % displaced**
> `Tri_NoShadowCast`, which is gated on `g_inShadowPass` — so it spared the shadow
> bake **and nothing else**. The mirror RTT bakes and the env/SH probes are
> offscreen passes too and were transforming those chunks in full, then dropping
> every one of their faces on `Face_MainOnly`: **54 073 verts/f (51 % of the RTT's)
> and 151 500 verts/f (59 % of the probes')**. New `Tri_AllFacesMainOnly` is the
> offscreen-wide form of the same fact; byte-exact by the same "no faces ⇒ no
> output" invariant the `FIndex == 0` skip rests on. The hatch is at SCENE INIT,
> not in the mesh loop (a runtime flag read in that `-ffp-contract=fast` function
> is not byte-null even when never taken).
>
> **ANIM (where greets' mirror RTT lives) −16.3 / −12.4 / −14.6 %** at t=5743 /
> 2845 / 1588 on his arm, **0 % at both poses where the RTT does not run**
> (t=6097, ANIM 0.041 ms; t=1588 bare `--deferred`, nothing sets `Face_MainOnly`).
> TOTL −1.67 / −2.00 / −2.64 %; `renderFrame` Ginstr/f never moves by more than
> one printed LSB, which is the correct control — the change is outside it.
>
> ### GATES
>
> * 11 pin recipes **3/3, parent `8dde99fd` vs child, identical** — all ten at
>   their recorded 16f values plus greets t=1588 `570a7b44`; and the four greets
>   ACCEPTANCE poses (`--greets-displace`, the only arm where this does anything)
>   differential-identical: t=5743 `26ad272a`, t=2845 `10adec3a`, t=6097
>   `418fc1fa`, t=6133 `6d02f31b`.
> * `--shadow_plane_hash` identical (43 bakes, running digest equal at every seq).
> * `render_gate.sh` 4/4 PASS. Measured, not assumed: no gate scene sets
>   `Face_MainOnly`, so the gate is an inertness control here, not a
>   discriminator; the acceptance pins are the discriminator.
> * Renders eyeballed: `docs/img/xform/{greets_t005743,city_t001961,chase_t000800,fountain_t002500}_color_xform.png`.
>
> ### TRAP RE-EARNED
>
> **chase's recorded pins reproduce WITHOUT `--profiler=0`.** Adding it gives a
> different, self-consistent set of five. 16f's "the flag is inert on snapshots"
> holds for city/fountain/greets and NOT for chase — run chase's recipe verbatim.
>
> ### WHAT IS LEFT (nothing clears 0.5 % of frame on its own)
>
> 1. shadow per-vertex loops **0.91 ms (1.8 %)** — memory-bound to the byte; only
>    lever is Phase 5, priced above at 1.25 %.
> 2. main-view `Transform_Objects` **0.48 ms (1.0 %)** — same wall.
> 3. per-face `VisibilityFlagsAll` **0.26 ms (0.53 %)** — a compact per-mesh
>    `uint8_t` flags array moves only the rejected half (the accepted half needs
>    the same line for the bbox stamp) ⇒ ceiling ≈0.32 % of frame. PARKED.
> 4. the **13.2 MB/frame shadow plane clear** (0.51 core-ms) — `sm.dirtyX0..Y1`
>    already records the previous bake's texel rect, so a dirty-rect clear is
>    byte-exact and available. ≈0.1 ms. Below bar.
>
> ## 2026-08-16q — THE SHADOW RASTER HAD NO PRE-REJECT. 231 735 CLIPPER ENTRIES/FRAME → 41 787, AND THE LEVEL IS PER-TILE, NOT PER-MAP
>
> **16p's row: `Shadows.cpp`'s per-(light, tile) walk handed every survivor of
> its four rejects straight to `FrustumClipper::Render` — 189 567 of 231 735
> entries a frame clipped away to nothing. `--shadow_bbox_cull` (DEFAULT ON)
> gives it `RenderInner`'s 4-compare screen-bbox test verbatim.** Entries
> **231 735 → 41 787 (−82.0 %)**, clipped-away-to-nothing **189 567 → 22**,
> `FrustumClipper::Render` self time **2.265 % → 1.429 %** of DEMO self samples,
> `DynMeshes` bake RASTER **1.31 → 0.78 ms/frame (−40 %)**, frame minimum
> **−0.8 to −2.3 %** across five greets poses. Evidence:
> `docs/PERF_STATE.md` **00f**, `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16q**.
>
> ### 16p's THREE CAVEATS, ANSWERED
>
> | caveat | answer |
> |---|---|
> | "the per-light clone FList may not stamp bboxes" | **it does** — `Transform_Objects` stamps the shadow path too (its ablation gate is main-view-only), and the PX/PY are SHADOW-MAP pixels, so `RenderInner`'s test transplants unmodified. Nothing had to be built; the data was there and nobody read it. |
> | "the shadow tile is often the WHOLE map — maybe a map-rect reject" | **a map-rect reject is worth exactly ZERO.** `FDS_SHADOW_TILE_GRID=1` → 0 rejectable faces in 42 bakes: the frustum-level cull is already done upstream. Every reject is INSIDE a map the face overlaps, and the yield is a pure function of `gridFor(res)` — 128² maps single-tile get nothing, 512² maps at 4×4 are the whole win. |
> | "51 % of the clipper is the Z clip; the reject must run before it and be conservative" | it runs first and removes **zero** Z-clip work: `needZ` is **14 514/frame on both arms, to the unit** (behind-near faces keep the cover-all box). The `no-clip` population is likewise identical to the unit. It takes the cheap half and leaves the expensive one — which is why −82 % of entries is −37 % of the symbol. |
>
> ### BYTE-EXACTNESS — TWO INSTRUMENTS, NOT AN ARGUMENT
>
> * **`--shadow_plane_hash`** (landed, default OFF): FNV-1a over every packed
>   shadow plane a bake wrote, depth AND polyId, one `[SPH]` line per bake.
>   **43–59 bakes per pose IDENTICAL** default vs `--no-shadow_bbox_cull` at
>   greets t=5743/2845/6097/6133/1588/3122/4871, at 640×360, without displace,
>   with `--shadow-backface-cull`, with `--shadow-dynamic`, under
>   `--no-tile_bbox_cull`, with `FDS_GREETS_SHATTER=1`, and on `conetest`.
> * **`-DFDS_SHADOW_BBOX_VERIFY=ON`**: computes the reject, does NOT apply it,
>   counts polygons the raster receives from a face it would have discarded —
>   **79.7 M rejectable face-visits, 0 polygons.**
>
> ### GATES
>
> * **11 pin recipes 3/3, parent-identical** (`DEMO_base` = `5071cc37`): city
>   `bd4ffbf8` / `4cb8d2ca` / `f473fe2b` / `d3374de6`, chase `3bfd4244` /
>   `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain `8db68ccb` — all
>   ten at their recorded 16f values — plus greets t=1588 and the four greets
>   acceptance poses, differential.
> * `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` /
>   `166fa25a`), **and `conetest` discriminates**: its shadow clipper entries go
>   **8 448 → 2 496 (−70.5 %)** and its reject rate **48.9 % → 6.7 %** while the
>   surface stays byte-identical.
> * **Only greets and `conetest` bake a shadow map at all** — city / chase /
>   fountain / crash produce ZERO bake invocations even with `--shadows` on. Their
>   ladders are the inertness control and they are inert.
> * One render eyeballed per scene: `docs/img/shadowbbox/greets_t5743_shadowbbox.png`,
>   `city_t1961_shadowbbox.png`, `chase_t800_shadowbbox.png`,
>   `fountain_t2500_shadowbbox.png`.
>
> ### HANDS ON
>
> **A per-light face→tile BIN** (`--face_tile_bin` for the shadow pass) would
> collapse the 237 609 (face, tile) pair-visits a frame to two list walks;
> `FaceTileBin.cpp` and its order-preservation proof already exist. Not built —
> the four-line reject took 80 % of the prize. Also: **`DynOmnis` is now the
> bigger bake** (1.21 ms raster vs `DynMeshes`' 0.78) and this row does not touch
> it, and **`Transform_Objects` is 3.35 % of DEMO self samples at greets t=5743**,
> more than double what is left of the clipper.

> ## 2026-08-16p — THE CLIPPER'S COPY IS 2.6 % OF THE CLIPPER: 16c's handover is refuted and the row is CLOSED BELOW BAR. greets' clipper is 83 % SHADOW
>
> **16c handed on "cutting it further means cutting the copy, not the traversal".
> Priced at all five acceptance poses, that is wrong by 10-40x.** The three
> 140-byte `Vertex` copies in `FrustumClipper::Render` are **2.6 % of the
> symbol's self time — 0.033 % of frame at city t=1961, 0.35 % at their worst
> pose**; copies + `MiplevelClipper`, the residue exactly as 16c named it, is
> **0.25 / 0.25 / 0.48 % of frame** at city t=1961 / chase t=800 / greets t=5743.
> Below the 0.5 % bar everywhere. Landed: the instrument only — **`--clip_stats`**.
> Evidence: `docs/PERF_STATE.md` **00e**, `docs/OPTIMIZATION_BACKLOG.md`
> **2026-08-16p**.
>
> | mechanism | verdict |
> |---|---|
> | (a) copy elision / copy-on-clip | **structurally impossible** — the clipper mutates the copies per-FACE (`A->U = F->U1`, UVs live on the face) and per-TILE (`Calc_Flags`), 4 instructions after the copy, from 12 pool workers at once |
> | (b) shrink the payload | **audited safe at 104 B, refuted by measurement** — a 52-B ceiling probe (63 % of the payload gone) moves `renderFrame` Ginstr/f by less than one printed LSB at all three poses, exactly as the disassembly predicts (43 instructions/visit → 0.027 % of frame) |
> | (c) SIMD the copy | clang already emits 128-bit `ldr q`/`str q` pairs — the widest arm64 has |
>
> ### THE NEW ROW, and it is bigger than the one that closed
>
> `--clip_stats` splits the census by dispatcher, and that split is what found it.
> **greets t=5743: 277 777 clipper entries per frame, 83.4 % of them from
> `Shadows.cpp`'s depth raster, and 81.8 % of THOSE (189 567/frame) are clipped
> away to nothing** — three 140-byte copies, the UV stamp and `Calc_Flags` paid
> for zero pixels. That path has no screen-bbox / tile pre-reject at all;
> `--tile_bbox_cull` and `--face_tile_bin` serve `RenderInner*` only. Same SHAPE
> as 16c's `Reflected_Transform` finding, different function. city t=1961 for
> contrast: 63 418 entries/f, **zero** shadow entries, G-buffer pass rejecting
> 3.3 %.
>
> ### GATES
>
> * **11 pin recipes, 3/3 each, parent-binary-identical** (`DEMO_base` = tip
>   `dc752523` vs the instrumented child, one worktree, one asset tree): city
>   `bd4ffbf8` / `4cb8d2ca` / `f473fe2b` (t=2400) / `d3374de6` (t=400), chase
>   `3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain
>   `8db68ccb` — **all ten at their recorded 16f values** — plus greets t=1588 and
>   the four greets acceptance poses t=5743 / 2845 / 6097 / 6133, differential
>   only (greets' absolute pin keys on uncommitted authoring files).
> * `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` /
>   `166fa25a`) — **and this round says what that is worth, measured**: the four
>   arms issue **21 857 clipper entries** between them (`mirrortest` 3 872,
>   `rttslot` 3 947, `conetest` 13 764, `halotest` 274) reaching 4 of the 7
>   `ClipSrc` buckets. `conetest` is the ONLY arm that rasterises a shadow map
>   (8 448 entries, 48.9 % rejected — the greets finding in miniature).
>   `ForwardInline` is exercised by nothing in this round's battery.
> * One render eyeballed per scene (city t=1961, chase t=800, greets t=5743,
>   fountain t=2500) — all correct.

> ## 2026-08-16k — THE SHATTER RACE IS CLOSED: `static TileChunkSphere chunk[]`, shared by 12 shard workers. 15/49 → 0/48
>
> **`FDS/RENDER/DeferredLightLists.cpp:247` — a function-local `static` scratch
> array in `buildTileLightLists`, written per tile at `:252` and read back across
> the light loop at `:328-333`.** The mirror-shard bake calls that function from
> 12 pool workers at once (`MirrorShatter.cpp:1073` → `:1218` → `:1436` →
> `DeferredSurfaceKernel.cpp:7380`), each with its own camera and its own tile
> depth bounds, all writing `chunk[0..3]` (a 64² cell grids 2×2). Last writer
> decided everyone's sphere cull. **Fix: drop the `static`** — 1.9 KB of stack,
> nothing else changes. Full evidence: `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16k**.
>
> | arm (greets `t=6293,6294`, his acceptance arm, `FDS_GREETS_SHATTER=1`) | runs | flips |
> |---|--:|---|
> | parent `6c3d38d8` | 49 | **15 (30.6 %)**, 13 distinct |
> | parent + `--no-deferred_tile_sphere_cull` (the only reader of the array's data) | 24 | **0** — and byte-identical to disabling both culls |
> | **fixed** | **48** | **0** |
> | fixed, `FDS_SHARD_REFL_SERIAL=1` | 24 | 0 |
>
> The fixed binary is stable at **`852aabe6…` / `f3c3a2018…`** — *the parent's own
> modal hashes*, i.e. 16j's recorded parallel modal. The modal was always the
> self-consistent outcome; the fix makes it the only one. Serial stays at 16j's
> recorded `0ff07c73…` / `467625df…`. Size of the defect: a flipped frame vs the
> modal is **2 145 px (0.103 %), max Δ 1/255**.
>
> ### GATES
>
> * city `bd4ffbf8` / `4cb8d2ca` / `f473fe2b` (t=2400) / `d3374de6` (t=400) and
>   fountain `8db68ccb` — **all five at their recorded values**, parent-identical.
> * greets t=1588 and chase t=100/400/800/1200/1600 — **identical parent-to-fixed**.
> * `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
>   It **cannot discriminate this fix** — no row shatters a mirror; the flip
>   battery is the real coverage.
> * Perf: `[SHARD-REFL]` wall split by call ordinal (cold 46 ms / warm 18 ms),
>   order-rotated — **min Δ sign flips between two independent batteries**
>   (+1.53 %/−0.54 % at n=30, +0.00 %/+1.63 % at n=16), paired means favour the
>   fix. No measurable cost; the change removes a 12-thread shared cache line and
>   adds no work. **Split a shatter wall by call ordinal — pooling cold+warm gave
>   a meaningless "+10 %".**
>
> ### HANDS ON — a second, DETERMINISTIC defect in the same function, measured, NOT landed
>
> `tileChunkSphere` (`DeferredCommon.h:496`) takes no projection parameters: it
> reads the **globals** `FOVX/FOVY/CntrEX/CntrEY`. `buildTileLightLists` shadows
> those names with `cam`'s values at `:211-212`, which reads as if the helper uses
> them — it does not. The SERIAL shard bake assigns the globals per shard
> (`MirrorShatter.cpp:833-838`) so it is correct; the PARALLEL bake leaves the
> MAIN camera's 1920×1080 projection in them while the tile rect is 64². Control:
> on the serial arm `--no-deferred_tile_sphere_cull` is byte-identical (6/6); on
> the parallel arm it **changes 25 567 px (1.23 %), max Δ 1/255**. It is a LOOK
> change in a scene under tuning, so it is his call. It does NOT close the
> serial-vs-parallel gap on its own — those are different functions, as 16j said.
>
> ## 2026-08-16j — THE BLACK CHECKERBOARD WAS REAL (city, crash, fountain), THE SHARD "RACE" ISSUES NO WRITES, AND THE SHATTER IS 24.5 % NONDETERMINISTIC
>
> **Three read-derived items from 16i, all run. One fixed, one refuted, one clean —
> and the battery that refuted the second found a live nondeterminism nobody was
> looking for.** Full evidence, censuses and images: `docs/OPTIMIZATION_BACKLOG.md`
> **2026-08-16j**.
>
> | item | verdict |
> |---|---|
> | OuterVec + `--hdr` + `--deferred_checkerboard`/`--deferred_quarter` | **CONFIRMED by rendering, FIXED** — `DeferredSurfaceKernel.cpp:6309` |
> | `VolCompositeAdd` racing `g_hdrActive`/`g_hdrBuf` from shard workers | **REFUTED** — precondition real, **0 calls issued** at every pose/arm tried |
> | fountain t=2500 ~2 % flip | **0 in 49** on the parent at tip `aa60d0ce`; running total 1 in 140 |
> | **NEW — greets mirror shatter** | **12 flips in 49 (24.5 %)**, deterministic without the shatter, **25/25 stable serial** |
>
> ### THE FIX, AND WHY IT WAS NOT AT THE KERNEL THE WARNING COMMENT GUARDS
>
> `Render_DeferredLighting_Tile_OuterVec` writes no `ctx.hdrBuf` on purpose — its
> 8-bit pack IS the HDR transport, lifted by `Hdr_ActivateNoFog` because it leaves
> `h[3]` at 0. The **wave-2 fill** did not honour that: it averaged all-zero
> neighbour radiance out of `ctx.hdrBuf` and stamped `h[3]=1.0f`, which BLOCKS the
> lift, so the tonemap printed a cleared buffer on exactly half the pixels.
> Measured, city t=1961 1920×1080 `--deferred --hdr --deferred_checkerboard`:
> **91 764 pixels below luma 4, 99.2 % of them on the wave-2 parity**, wave-1 half
> bit-identical to the full-rate arm (175.36). Under `--hdr_linear`: 80 973, 100 %
> on wave-2 parity. crash f120: the wave-2 half is 0.14 luma against 4.47.
> The fix adds one term (`&& !outerVecG`) so wave 2 takes wave 1's transport:
> parity Δ **17.03 → 0.05**, dark wave-2 pixels **91 764 → 733** (against 722
> legitimately dark on the wave-1 half); `--hdr_linear` **80 973 → 0**.
>
> **`PreferOuterVec = 1` is THREE scenes, not the two the handoff named** — city
> `CITY.CPP:2537`, crash `CRASH.CPP:25`, **fountain `FOUNTAIN.CPP:1029`**. None
> `setDefault`s checkerboard, so the shipping demo never hit it; `--cinematic
> --deferred_checkerboard` on any of the three did.
> Images: `docs/img/ovchk/city_t1961_chk_crop8x_before.png` /
> `..._after.png` (64×64 at (1376,128), nearest-8×), plus full frames
> `city_t1961_hdrchk_before/after.png`, `city_t1961_hdr_fullrate.png`,
> `city_t1961_hdrlin_chk_before/after.png`, `crash_f120_chk_before/after.png`.
>
> ### GATES AT THIS COMMIT
>
> * `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
> * city **`bd4ffbf8`** (`--deferred`, `FDS_CITY_ENV_PIXEL=1`) and **`4cb8d2ca`** /
>   **`f473fe2b`** (t=2400) / **`d3374de6`** (t=400) on his acceptance arm — all
>   four at their recorded 16f values.
> * fountain t=2500 **`8db68ccb`** — recorded value, and 49/49 on the parent.
> * city `--hdr`-only `4b0e31bf…` and city `--deferred_checkerboard`-only
>   `58644ea7…`: **identical parent-to-fixed** (the byte-null controls for the fix).
> * chase t=100/400/800/1200/1600 and greets t=1588: **identical parent-to-fixed**.
>   Their absolute values in a clean worktree are NOT the 16f figures — greets'
>   pin keys on uncommitted authoring files (`render_gate.sh` says so) and the 16f
>   chase values were taken one-pose-per-process. Parent-vs-fixed identity on the
>   same tree is the control that carries weight there.
>
> **Say what the gate is worth, again:** `render_gate.sh` passes no
> `--deferred_checkerboard` on any arm, so 4/4 could not have failed for this fix.
> The city/crash parity censuses are the real coverage.
>
> ### THE SHATTER NONDETERMINISM — the open item this round hands on
>
> `--snapshot=greets@t=6293,6294`, his arm, parent binary: **25/25 identical
> without the shatter** (and 48/48 at the single pose), **37/49 modal with
> `FDS_GREETS_SHATTER=1`**. One environment variable apart. **Tick 1 flips at the
> same rate as tick 2**, and tick 1 has `g_hdrActive == false` during the bake, so
> no HDR-global mechanism can be the cause. **`FDS_SHARD_REFL_SERIAL=1` is 25/25**,
> so it lives in the 12-worker fan-out of `renderReflectionCameras`
> (`MirrorShatter.cpp:1073`), not in the per-shard math. Method note for whoever
> takes it: `--repro` is **not** a determinism instrument — it leaves
> `g_fineSceneClock` free-running by design and gave 48 distinct hashes in 48
> launches. Use `--snapshot`.

> ## 2026-08-16f — THE CITY PINS NEVER MOVED: THERE IS NO MOVER COMMIT, THE INSTRUMENT WAS MEASURING ITS OWN HUD
>
> **VERDICT: NO CODE CHANGE MOVED CITY. Both recorded values reproduce byte-exactly
> at tip `eb5e57d9`, 3/3 each.** The hunt for "the commit that moved the city pins"
> (candidate span `b2de6323..e965dc26`) was called off before a single bisect build,
> because the *first* control refuted the premise:
>
> ```
> ./DEMO --snapshot=city@t=1961 --out=D --env_live_water --deferred --city_env_pixel
>   → 925ecd43f45d8f0574acc9c9a5a958a1     ← the recorded pin, at TIP, 3/3
> ./DEMO --snapshot=city@t=1961 --out=D --env_live_water --deferred --city_env_pixel --profiler=0
>   → 4cb8d2ca68b72f8a24627f42077eef25     ← the "drifted" value, at TIP, 3/3
> ```
>
> Same binary, same commit, same asset tree, same env cube. **The variable is
> `--profiler=0`.** The recorded pins `4031ceec…` / `925ecd43…` were taken WITHOUT
> it; every report that they "no longer reproduce" was taken WITH it, following the
> `--profiler=0` harness rule that `0610ddbb`'s round had just (correctly, for
> *greets*) established. Two rounds and two agents chased a nonexistent regression
> through this.
>
> ### THE MECHANISM: THE CITY SNAPSHOT LOOP NEVER SILENCED THE OVERLAY
>
> `Runtime/rev.cfg` ships `ProfilerEnable 1`, which `DEMO/REV.CPP:1424` seeds into
> the `profiler` FeatureFlag. `DEMO/CITY.CPP:3942` paints the FrameProfiler overlay
> into `VPage` whenever that flag is live — and `RunCitySnapshot` writes its PPM
> straight out of `VPage` after `driver->tick()`. `initSnapshotEnvironment` zeroes
> `g_profilerActive`, but that is not enough: `FrameProfiler::beginFrame()`
> re-mirrors the flag into the global every frame, so the overlay returns on tick 1.
> **`RunChaseSnapshot` already had the fix and a comment saying exactly this
> (`DEMO/Snapshot.cpp`); city, fountain and greets never got it** — which is why
> chase was the one narrative scene insensitive to the flag, and why chase's pins
> reproduced all along while the city report looked like drift.
>
> ### QUANTIFIED — city t=1961, HUD-bearing vs HUD-free, 1920×1080
>
> | | |
> |---|---|
> | changed px | **3 718 of 2 073 600 (0.179 %)** |
> | bbox | **x 0–113, y 15–235** (114×221, top-left corner) |
> | max \|Δ\| / mean \|Δ\| on changed | **245 / 215.1** — glyph pixels, not shading |
> | scene pixels changed | **zero** |
>
> Identical to the pixel in *both* arms (`--deferred` and his `--env_live_water
> --deferred --city_env_pixel`) — it is the same text block, not a render
> difference. Picture:
> `/Users/gil-ad/work/revival-fog/docs/img/citypin/city_t1961_profiler_ab_crop.png`
> (left = the recorded `4031ceec`, right = `bd4ffbf8`; full-frame amplified diff at
> `docs/img/citypin/city_t1961_profiler_diff.png`). The overlay reads
> `0.000000 FPS`, `ZCLR 0.0ms (0.0%)` … `TOTL 0.0ms` — **all zeros**, which is the
> whole reason `4031ceec` looked like a stable pin at all.
>
> ### AND THAT ACCIDENT WAS ONE POSE DEEP
>
> The zeros only hold on the **first tick of the process**, where the profiler has
> no accumulated frames. Every later pose of a sweep prints real wall-clock
> timings. Measured before the fix, `--snapshot=city@t=1961,2400`, cfg-seeded
> profiler, three runs:
>
> ```
> run1  t1961 4031ceec…   t2400 3ee3d676…
> run2  t1961 4031ceec…   t2400 d214425266…
> run3  t1961 4031ceec…   t2400 05df6d6f…      ← 3 distinct values in 3 runs
> ```
>
> So the city snapshot was **non-deterministic at every pose but the first**, and
> had been for as long as the overlay has been unsilenced. That is a live
> instrument defect, not a recipe nit.
>
> ### THE FIX
>
> `silenceProfilerOverlayForSnapshot()` in `DEMO/Snapshot.cpp` — chase's existing
> block lifted into a named helper and called from **city, fountain and greets**
> too. It clears the flag only when it was **cfg-seeded** (unmarked), so an explicit
> `--profiler` on a snapshot run is still honored: that is the control that proves
> the change is the HUD and nothing else — **`--profiler` reproduces the old
> `4031ceec…` / `925ecd43…` exactly, 2/2.** Scoped to the four scene ticks that
> paint an overlay; `conetest` / `halotest` / `mirrortest` render without a scene
> tick and are measured insensitive to the flag, so `render_gate.sh`'s baselines are
> untouched.
>
> ### RE-PINNED — city, both arms, HUD-free (the render, not the HUD)
>
> | arm | recipe | OLD (HUD) | **NEW** |
> |---|---|---|---|
> | `--deferred` | `FDS_CITY_ENV_PIXEL=1 ./DEMO --snapshot=city@t=1961 --out=D --deferred` | `4031ceec1a1090372575c4f9c39e2839` | **`bd4ffbf87d1492175a9b6c1111fb3f5f`** |
> | his acceptance arm | `./DEMO --snapshot=city@t=1961 --out=D --env_live_water --deferred --city_env_pixel` | `925ecd43f45d8f0574acc9c9a5a958a1` | **`4cb8d2ca68b72f8a24627f42077eef25`** |
>
> Both now reproduce **with or without `--profiler=0`** — the flag is inert on
> snapshots again, which is the point.
>
> ### GATES AT TIP, WITH THE FIX
>
> * `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
> * chase t=100/400/800/1200/1600 `3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814` — **2/2, unmoved** (chase already had the fix).
> * fountain t=2500 `8db68ccb` — now **both ways**; before the fix a cfg-seeded run gave `658cba5c…` (HUD).
> * greets t=1588 `570a7b44` — both ways, and it reproduces on **committed** assets in a clean worktree.
> * city his arm t=2400 `f473fe2b`, t=400 `d3374de6` — unmoved.
> * city 2-pose sweep `t=1961,2400` now **3/3 identical** (`bd4ffbf8` / `a6d82ed9`).
>
> ### THE LESSON, SINCE IT HAS NOW COST THREE ROUNDS
>
> `--profiler=0` is a **byte-relevant argument on the narrative scenes**, not
> hygiene — a recipe that omits it and a recipe that passes it are two different
> measurements, and a pin is meaningless without knowing which one produced it.
> After this commit that is no longer true, which is the durable fix. Until every
> pin in these docs has been re-taken on a post-fix binary, treat any hash quoted
> without its profiler state as ambiguous.

> ## 2026-08-16 — THE MIRROR RTT'S PER-LAUNCH DITHER IS NOT THE PEEL LEAK: IT IS AN UNINITIALISED SHADOW MATRIX, AND THE WINDOW IS FRAME 1
>
> **VERDICT: A DIFFERENT BUG.** The parked `--repro_prescenes` dither is not
> `44c8aeed`'s peel-floor leak, and the argument is mechanical rather than
> statistical: in this recipe **there is nothing to leak**. `--mirror_rtt_trace`
> now digests the shared `g_xparPeelFloor` at every RTT bake and counts its
> non-0xFFFF entries, and across **48 launches × 349 bakes, with the per-frame
> restore explicitly DISABLED, that count is 0 every single time** — the fountain
> is only *initialised* by `--repro_prescenes`, never rendered, so the deep peel
> that dirties the plane never runs. The two arms of `44c8aeed` are byte-identical
> under this recipe by construction. The 30/30 that "did not establish a fix" was
> not evidence of one, and now it does not need to be.
>
> ### THE NAMED WRITE
>
> ```
> FDS/FILLERS/ShadowMap.h:193    Matrix lightViewMat;      // no initializer
> FDS/FILLERS/ShadowMap.h:222    Matrix viewToLight;       // no initializer  <- the one that reaches pixels
> FDS/FILLERS/ShadowMap.cpp:475  ShadowMap sm;             // ShadowMaps_Rebuild     — DEFAULT-init
> FDS/FILLERS/ShadowMap.cpp:534  ShadowMap sm;             // CubeShadowMaps_Rebuild — DEFAULT-init
> read per pixel at
> FDS/RENDER/DeferredSurfaceKernel.cpp:627 and :677
>       lx = sm.viewToLight[0][0]*x + ... + sm.viewToLightOffset.x
> ```
>
> `Matrix` is `typedef float[3][3]` (`FDS/Base/Matrix.h:5`) — a bare array with
> **no member initializer**, while every scalar beside it in `ShadowMap` carries
> an NSDMI and `Vector` carries its own. Both Rebuild functions construct their
> entry as `ShadowMap sm;`, which is **default**-initialization: the NSDMI members
> get their values, the two bare arrays get **that stack frame**. Neither Rebuild
> writes them — they are computed at the *end* of `Render_DeferredShadowMaps`
> against whatever `View` is current there. So between "the scene built its shadow
> maps" and "the first bake ran", any lighting pass that samples a shadow map
> multiplies its view-space position by **nine floats of stack garbage**.
>
> **The greets tick walks straight into that window.** `RenderSecondOrderMirrors`
> (`DEMO/GREETS.CPP:3977`) runs BEFORE `ShadowBake_DispatchGreets` (`:4081`), so
> the mirror RTT's **frame-1** deferred lighting is, in a bare launch, the first
> shadow-sampling pass in the process — and the only one that ever reads the
> matrices unwritten. From frame 2 the bake has written them and every launch
> agrees, which is exactly why the defect heals after one frame.
>
> ### THE EVIDENCE CHAIN (48 traced launches per claim, pre-fix arm)
>
> `--mirror_rtt_trace` now records a digest after **every stage of the bake in
> execution order**, so a launch diff names the first stage that moved *and*
> every stage before it that did not:
>
> | stage | what it covers | launches differing (of 48) |
> |---|---|--:|
> | `cam` | camera basis + off-axis projection + near/far | 0 |
> | `floor` | `g_xparPeelFloor` + count of non-0xFFFF entries | 0 (**count 0 always**) |
> | `cAll` / `xfrm` | draw list + every scene vertex's PX/PY/RZ/BGRA/flags/UV | 0 |
> | `omni` | animated light state the lighting pass rebuilds from | 0 |
> | `shad` (planes only) | packSD / packDyn / uniSD / uniDyn / dirty boxes | 0 |
> | `gb` | every G-buffer plane + Z + surface after the surface kernel | 0 |
> | **`lit`** | **surface after `Render_DeferredLighting`** | **3** |
> | `lith` / `cone` / `hash` | downstream of `lit` | 3 |
>
> **First divergence: FRAME 1, column `lit`, in 3 of 48 — and frame 1 only** (all
> three launches match the modal trace again from frame 2 on).
>
> Then the two questions that separate the two kinds of non-determinism:
>
> 1. **Race, or input?** New `--mirror_rtt_relight` re-runs the lighting stage a
>    second time from a byte-restored copy of its inputs. **`lit2 == lit` in every
>    launch including both diverging ones** — the stage is a function of what it
>    reads, so a race inside it is ruled out and a launch-varying INPUT is not.
> 2. **Which input?** Adding the shadow-map **transforms** (`viewToLight`,
>    `lightViewMat`, `lightISource`, `viewToLightOffset`) to the `shad` digest —
>    the planes alone had been identical 48/48 — made it take **6 distinct values
>    in 6 launches**, while `mat` (every material scalar + every mip byte of every
>    texture they point at) and `gb` were 1 of 6. That is the input, named.
>
> **WHAT IT LOOKS LIKE.** `[RTTPIX]` dumps frame 1's RTT surface verbatim. The
> diverging image differs on **4 851 of 8 192 px (59.2 %)**, rows **25..63** of 64
> across the full width, **max |Δ| 216, mean signed +32.88** — a large coherent
> region of the reflected room lit *brighter*. Not a dither in the LSBs: a whole
> surface. And the diff picture says which surfaces, which is the confirmation
> the digests cannot give — **the MECH's silhouette and the floor/lower walls
> light up while the ceiling stays black**, i.e. exactly the pixels a shadow tap
> decides, and nothing else:
>
> - `docs/img/fogwt/rttdither_f1_modal.png` (46 of 48 launches)
>   — /Users/gil-ad/work/revival-fog/docs/img/fogwt/rttdither_f1_modal.png
> - `docs/img/fogwt/rttdither_f1_flipped.png` (2 of 48)
>   — /Users/gil-ad/work/revival-fog/docs/img/fogwt/rttdither_f1_flipped.png
> - `docs/img/fogwt/rttdither_f1_diff.png` (|Δ| ×2, red)
>   — /Users/gil-ad/work/revival-fog/docs/img/fogwt/rttdither_f1_diff.png
>
> (128×64 slot textures, nearest-upscaled ×6.)
>
> ### THE FIX, AND WHY ZERO IS THE RIGHT VALUE
>
> `Matrix lightViewMat{}` / `Matrix viewToLight{}` at the declarations, plus
> `ShadowMap sm{}` (value-init) at both construction sites as the belt to that
> brace. Zero is not merely *defined*: with a zero transform the pre-bake tap
> reads the all-zero planes as **"no occluder"**, which is exactly what
> `ShadowMaps_Rebuild`'s own uniformity-pyramid comment already promises a tap
> taken before the first bake will see. Post-fix frame 1 lands on the *brighter*
> of the two pre-fix images — the fully-lit one — which is that promise kept.
>
> ### VERIFIED
>
> Recipe throughout: `FDS_GREETS_CAM=<his pose> ./DEMO --repro=greets@t=3409
> --repro_from=0 --repro_xres=1512 --repro_yres=848 --repro_prescenes
> --profiler=0 --mirror_rtt_trace`, dummy drivers, run 1 discarded.
>
> - **ON TIP, 48 launches × 349 bakes: every RENDER column of the trace
>   (`cam` `floor` `cAll` `xfrm` `gb` `lit` `lith` `cone` `hash`) is ONE distinct
>   value — flip rate 0/48**, against **3/48** on the pre-fix arm. `shad` is
>   48/48 identical at each of frames 1..4, where pre-fix it took 6 distinct
>   values in 6 launches.
> - The dumped **t=3409 frame md5 is `10f9d3255d01f6358a6e1683490db2b2` 48/48**
>   in every arm — the frame-1 error is overwritten by 348 later rebakes, so at
>   this pose it never reaches the dump. It *does* reach the first rendered frame
>   of greets in a live run, which is where a viewer would see it.
> - **`render_gate.sh` 4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` /
>   `166fa25a`).
> - **Pins, 2/2 each on tip `0fed9f95`+fix**: chase `3bfd4244` `42d79fad`
>   `622b96a2` `31aa5203` `ca07a814`, fountain `8db68ccb`, greets `570a7b44` —
>   all seven at their recorded values. **city reads `4031ceec…`, not the
>   recorded `3413028b…` — and that is NOT this change**: the same tip with these
>   edits stashed and rebuilt gives `4031ceec…` too. The city move is pre-existing
>   on `0fed9f95` (the perf work landed between `44c8aeed` and it touches
>   `DEMO/CITY.CPP`, `ProceduralWater.cpp`, `Lighting.cpp`, `DeferredFastFog.cpp`
>   and adds `FaceTileBin`), and the city pin is anyway documented as conditional
>   on `Runtime/cache/city_envmap_cube.bin`. Differential control == arm on all
>   eight rows; whoever owns that perf series should re-pin city.
>   > **2026-08-16f:** this reading was right for the wrong reason. `4031ceec…`
>   > was not a "move" at all — it is the correct value of the recorded
>   > (`--profiler`-less) city recipe, which `3413028b…` predates by one round.
>   > What no one caught is that `4031ceec…` is HUD-BEARING: this round's own
>   > `--profiler=0` trap note applies to *city snapshots as a whole*, not just
>   > greets, and `RunCitySnapshot` had no silencer. Current city pin
>   > **`bd4ffbf8`**; see the 2026-08-16f block at the top.
>
> ### TWO TRAPS THIS ROUND, BOTH OF WHICH HAD ALREADY BITTEN
>
> - **`Runtime/rev.cfg` has `ProfilerEnable 1`.** The profiler overlay draws live
>   millisecond numbers into the framebuffer, so **every** run's frame md5 differs
>   and the instrument measures the HUD. `--profiler=0` restores the documented
>   `10f9d325…` exactly. Shared mutable state, second time it has cost a round.
> - **The trace's own `FULL` digest hashed the record's alignment holes.**
>   `RttTraceRec` is `push_back({...})`-aggregate-initialised, and its padding
>   bytes are indeterminate — so `FULL` reported per-launch differences the render
>   never made. The previous round's "`FULL` differed in 4 of 24" is therefore
>   partly probe noise. The record is now memset + assigned field by field.
>
> ### STILL OPEN (small, and NOT this defect)
>
> Post-fix, the new `mat` digest — every material scalar plus **every mip level**
> of every texture they reference — takes **2 distinct values in 48 launches**
> (43 / 5, the same split at frames 1 and 2) while every render column stays
> identical 48/48. Some texture byte is launch-varying and no
> frame-1 pixel samples it; the shape of that is an allocated-but-never-filled mip
> level (`Materialize` in `DEMO/CITY.CPP:1662` writes level 0 only and sets
> `numMipmaps = 1`, so the suspect is elsewhere). Inert at this pose, worth one
> round when something reads it.


> ## 2026-08-16 — THE GREETS MIRROR BAND, CLOSED: THE FOUNTAIN LEFT ITS DEPTH-PEEL FLOOR BEHIND
>
> **The band is the FOUNTAIN's, drawn on greets' mirror, three scenes later.**
> Reproduced deterministically, mechanism read off the source, fixed, and
> verified 24/24. Gil-Ad's own A/B (`./DEMO` bands always, `--xpar-peel-passes=0`
> never) was the datum that cracked it — and it never touched greets at all.
>
> ### THE INVARIANT, AND WHO BREAKS IT
>
> `EngineGBuffer_Resize` fills `g_xparPeelFloor` with **0xFFFF everywhere**
> (`FDS/FILLERS/Mekalele.cpp:141-147`), and its comment has always said why:
> the SINGLE-pass transparent raster has no floor logic of its own, its gate is
>
> ```
> zmask &= (z_candidate < extend(bound_c));    // FDS/FILLERS/Mekalele.h:1458
> ```
>
> which is a **no-op only while every entry is 0xFFFF** (`z_candidate <= 0xFF80`).
> That state is established **once, at resize**, and the plane is scoped to the
> ENGINE — not to a scene, not to a render target.
>
> The MULTI-pass reverse peel writes it and never puts it back:
> `FDS/RENDER/RENDER.CPP:1172` zeroes it at pass 0, `:1174` copies the side's Z
> into it at later passes; the unified-TBR strip path does the same per column
> (`FDS/RENDER/DeferredSurfaceKernel.cpp:4643` `fillFloor`, `:4669` `copyFloor`).
>
> **Only one scene in the tree peels deep: the fountain**
> (`DEMO/FOUNTAIN.CPP:1083`, `FntSc->XparPeelPasses = 4`). Greets runs one pass.
> So after the fountain, greets' transparents meet a floor full of **zeros**, and
> `z_candidate < 0` is FALSE — every single-pass transparent fragment is silently
> **Z-REJECTED** over the fountain's leftover footprint. Greets' mirror-MASK wall
> loses its own fragment there while the reflection CLONE still composites, so the
> reflected room lands without the wall pass that should attenuate it: **an
> additive copy of the reflection, hard-edged to a rectangle that belongs to a
> scene that ended minutes ago.**
>
> **WHY HIS FLAG "FIXED" IT WITHOUT CHANGING GREETS.** `--xpar-peel-passes=0`
> clamps to 1 in `xparPeelPassesEffective()`, so greets' own passes are 1 either
> way. What it changes is the `isSet()` gate: an explicit flag makes the resolver
> ignore `Scene::XparPeelPasses`, so **the FOUNTAIN drops to one pass and never
> pollutes**. His A/B was a fountain A/B all along.
>
> **WHY EVERY HEADLESS ARM MISSED IT.** All of them started at greets.
> `--repro_prescenes` runs the other scenes' `Initialize_*` — it never RENDERS
> them, and the pollution is a render-time write.
>
> ### THE MEASUREMENT THAT NAILED IT
>
> New harness flag **`--repro_run_fountain=N`** renders N real fountain frames
> before greets, as the director does. At his pose (t=3409, his camera,
> 1512×848, `--repro_from=0`, 349 frames, `--repro_run_fountain=30`):
>
> | | md5 | peel floor at greets' xpar entry |
> |---|---|---|
> | bare (fountain peels 4) | `7a2b6920…` | **238 597 / 1 282 176 non-0xFFFF, bbox rows 152..311** |
> | `--xpar-peel-passes=0` | `af5f7256…` | 0 non-0xFFFF, every frame |
>
> The band, bare minus clean: **bbox rows 152..329 × cols 475..1288**, dense rows
> **152..311**, **zero changed pixels above row 152**, per-channel
> **B +44.25 / G +42.41 / R +35.77**. `da286d8b` recorded his live band as
> **+43 / +41 / +35** in rows **152..319**. The polluted rows and the band rows
> are the same rows.
>
> - before: `docs/img/fogwt/greets_band_t3409_before.png`
> - after: `docs/img/fogwt/greets_band_t3409_after.png`
> - the additive layer: `docs/img/fogwt/greets_band_t3409_diff.png`
> - his acceptance arm, after: `docs/img/fogwt/greets_band_t3409_acceptarm_after.png`
>
> And the "unexplained" geometry is explained: the 168-row height and the −16
> offset from greets' 6×5 frame-tile grid were never a greets grid. They are the
> screen footprint of the fountain's transparent strips.
>
> ### THE FIX, AND A TRAP INSIDE IT
>
> 1. `XparPeel_ResetAll()` (`FDS/FILLERS/Mekalele.cpp`) re-establishes every
>    engine-scoped peel global: floor → 0xFFFF, both deep-layer slices + their Z,
>    `XparStripSlices_MarkAllDirty()`, and this thread's `g_xparPeelReverse`.
>    Called from `DEMO/SceneDriver.cpp` `setupFaceLists` — the one seam every
>    driver's `init()` passes through, i.e. once per scene entry.
> 2. The primary fix: a per-frame restore at the top of `renderFrame`'s
>    transparent phase, gated on a new `g_xparPeelFloorDirty` that every floor
>    writer sets, and on `xparPeelPassesEffective() <= 1` — **restore before the
>    only reader that cares**, so the deep-peel scene pays nothing.
>
> **THE TRAP, because the first cut of this fix silently did nothing:** size the
> restore by `g_xparZCount`, the PLANE's own length, never by this pass's
> `XRes*YRes`. `renderFrame` runs for offscreen targets too — the greets mirror
> RTT bakes at 256×256 and 512×229 reach that line **before** the main frame — and
> they share the one main-sized plane. A target-sized restore memset 65 536 of
> 1 282 176 entries and cleared the dirty flag, so the main frame skipped it and
> the band survived untouched. That is what the `[PEELFLOOR]` census caught.
>
> ### VERIFIED
>
> - **Chained repro 24/24 `af5f7256…`** (all bare-equivalent), and the
>   `--xpar-peel-passes=0` control lands on the SAME hash 6/6 + 3/3: the two arms
>   now agree, which is the fix's real signature. Re-run post-rebase: 8/8 + 4/4,
>   same hash.
> - **Gil-Ad's acceptance arm** (`--deferred --hdr --hdr-linear --texture-filter=2
>   --ssao --ssao-gtao --greets-displace`) **4/4 self-identical `b02fb36a…`** and
>   **equal to its own `--xpar-peel-passes=0` control 2/2** — no band. (Numbers
>   are post-rebase onto `b0905ee1`'s SSAO/GTAO work; the whole battery was re-run
>   on the rebased tree, chained repro included, and every arm held.)
> - **Fountain's peel still engages and is unchanged**: t=1200 `40ce5f1e…` 2/2,
>   t=2500 pin `8db68ccb…` unmoved.
> - **All eight pins unmoved**; **`render_gate.sh` 4/4 PASS**.
>
> ### PARKED, STILL OPEN: the mirror RTT's own per-launch dither
>
> The 2026-08-15f 1-in-15 `--repro_prescenes` flip is a SEPARATE and much milder
> defect, and the RTT round-robin is now **exonerated by measurement**: 24 traced
> launches (`--mirror_rtt_trace`, new) give `SCHED` identical **24/24** — one slot,
> re-baked every frame, constant 128×64, `kRttPerFrame` never even binds at this
> pose — while the texture-content digest `FULL` differed in **4 of 24**. The
> non-determinism is INSIDE the RTT render, not in which slot or at what
> resolution. The frame md5 was stable 24/24, so it rarely reaches the screen.
>
> Post-fix the `--repro_prescenes` arm is **30/30 canonical** — but say what that
> is worth: the pre-fix rate was **1 flip in 27** runs (3.7 %), so 30 clean runs
> would happen by luck about a third of the time. **It is consistent with the
> dither being the same leak and it does NOT establish it.** The item stays open,
> and the next round should either run it to 100+ or, better, chase the RTT
> render directly with `--mirror_rtt_trace` and bisect the first frame whose
> `hash=` column diverges.
> ## 2026-08-16 — THE WATER VERDICT IS ONE BIT PER BAKED TEXEL NOW, AND THE MEASUREMENT SAYS THE REMAINING BAND IS THE ENV TAP, NOT THE MASK
>
> His verdict on the coverage build: *"--env_live_water - mostly ok, but I think
> you use the original water line (not reflected) as a reference to what displace
> and what not. I think a simple bit map on the reflection bake can really help
> here to decide what pixels should be displaced and which won't - since currently
> it's not exact at all and bleeds."*
>
> **His diagnosis of the STORAGE was right; his guess at the MECHANISM was not.**
> The reference was never the un-reflected waterline — the bake ray-casts every
> source pixel to the water plane and z-tests it, so the classification was
> already about reflected content. What was wrong is that the answer was then
> **box-accumulated 4×4 into a 128²-per-face coverage plane and read back
> BILINEARLY**, i.e. a blur on top of a blur between the bake's exact answer and
> the consumer. That is the bleed, and it is visible at storage resolution:
> `docs/img/envwaterbit/waterbit_storage_b5_face5.png`
> (/Users/gil-ad/work/rev-waterbit/docs/img/envwaterbit/waterbit_storage_b5_face5.png)
> — the BEFORE plane washes a soft red ramp over the reflected quay and the
> building bases; the AFTER bit follows those silhouettes texel for texel.
>
> ### WHAT SHIPPED
>
> `CITY_ENV_WATERMASK_RES` 128 → **512 = the face resolution**, stored as **ONE
> BIT PER BAKED TEXEL** (face-major, row-major, LSB first). Every consumer
> POINT-samples it — `EnvLiveWater_MaskAt`'s bilinear tap is deleted and replaced
> by `EnvLiveWater_MaskBit`, which floors to the texel that CONTAINS the
> direction: the same texel the colour fetch is reading. The forward paraboloid
> sheets carry the bit in their alpha byte **exactly by construction and for
> free** — a sheet gather entry IS a flat face-texel index, hence also the bit
> index — so the 128²→512² bilinear upsample the sheets used to pay is gone
> outright (one buffer and one pass over 6×512² per probe deleted).
>
> **MEMORY, MEASURED (the `[CITY]` census line prints it): 13.3 MiB for 71
> probes** at 6 × 512²/8 bytes each — exactly 2× the 6.7 MiB coverage plane it
> replaces, **1/8** of the 106.5 MiB a full-res BYTE plane would have cost, and
> ~3 % of the ~450 MB of colour it gates. Water share 33.7 % (the coverage plane
> read 33.5 % mean — the two agree, which is the point: the classification was
> never the bug, the storage resolution was).
>
> ### `env_live_water_mask_bias` DID NOT DIE, IT MOVED — and maskGain DID die
>
> With the verdict stored per texel there is no fraction left to threshold at
> sample time, so **`maskBias`/`maskGain`, `lwBias`/`lwGain`/`lwAlphaMin` and
> `EnvLiveWater_MaskAt` are all deleted** (grep-verified: zero references in the
> tree). The per-pixel gate is now ONE integer compare on a 0/255 alpha, with no
> int→float convert, no remap multiply and no clamp — strictly fewer instructions
> per gated pixel than the ramp it replaces. The flag survives **re-sited to the
> bake**, where a fraction still genuinely exists: the bake renders 1024² and
> stores 512², so a texel owns exactly 4 source pixels and the ladder is
> 1/4·2/4·3/4·4/4. **Default 0.0 → 0.5 = MAJORITY**, the exact classification.
> 0 dilates the water region by up to a texel, 1 erodes it.
>
> ### THE ACCEPTANCE ARM, MEASURED — and the honest half of the result
>
> Camera pinned, wave clock moved ALONE (`--water_ripple_speed` 1.0 vs 1.6),
> control = the same clock change with the flag OFF, region classes from
> `--env_water_region_viz` at `amp=0`. **His arm literally**:
> `./DEMO --env_live_water --deferred --city-env-pixel`. Regions are scored
> EXCLUSIVELY, because a mipped bilinear env tap reads a NEIGHBOURHOOD of baked
> texels and the water/non-water sets overlap on every pixel that straddles the
> reflected waterline — scoring against the union hides the whole result inside
> the overlap.
>
> | pose | arm | DRY moved (reads ONLY non-water) | boundary band moved | water motion |
> |---|---|--:|--:|--:|
> | y=190 | before | **0** of 126 248 | 39 664 (45.16 %) | 100 % |
> | y=190 | after | **0** | **36 203 (41.22 %)** | 100.3 % |
> | y=423 (his pin pose) | before | **0** of 202 160 | 72 530 (59.66 %) | 100 % |
> | y=423 | after | **0** | **65 560 (53.92 %)** | 100.4 % |
> | y=800 | before | **0** of 307 417 | 89 008 (63.47 %) | 100 % |
> | y=800 | after | **0** | **80 740 (57.58 %)** | 100.1 % |
>
> **THE DEFERRED PATH NEVER HAD A GROSS BLEED: 0 pixels that read only non-water
> content move, in EITHER arm, at every pose.** It already read the mask per
> pixel (`5f1ffa92`); the forward path was the one with the structural leak and
> `b2e6c915` fixed that. So on his arm the exact bit buys a **boundary
> tightening** — 8 354 px stop being displaced, 1 412 start, net −6 942, and
> 99.6 % of the ones that stop are boundary pixels. Water motion is not damped to
> buy it (100.1–100.4 %). Crop: `docs/img/envwaterbit/waterbit_city_t1961_his_arm.png`.
>
> ### THE DISCRIMINATOR — the residual band is the TAP's mip footprint, not the mask
>
> One run separates them. With the env tap sharpened (`--city-env-gloss=4000`,
> mip ≈ 0) at the same pose, the boundary band collapses 121 582 → 42 457 px and
> **the two arms separate hard**:
>
> | arm | DRY moved | band moved | water motion |
> |---|--:|--:|--:|
> | before | **226 px** (0.10 % of 234 980) | 36 090 (85.00 %) | 100 % |
> | after | **0 px** | **25 842 (60.87 %)** | 100.3 % |
>
> So the coverage plane **did** displace content that reads only non-water — 226
> px of it — and the exact bit displaces none. At his default `city_env_gloss`
> 24 the tap averages ~8–16 baked texels, so the band there is **the filter's
> width, not the mask's**, and no mask can shrink it: displacing a filtered
> lookup drags whatever else is inside the filter. Crop:
> `docs/img/envwaterbit/waterbit_city_t1961_sharp_tap.png`. **If he wants the
> band itself narrower, the lever is the tap (gloss / a sharper env mip), not the
> verdict** — that is now measured rather than assumed, and it is the next thing
> to put to him.
>
> ### GATES — FLAG OFF IS BYTE-NULL
>
> Differential, one asset tree, base = `f25bb992`: **all eight pins reproduce
> their recorded values 2/2 on both arms** — chase `3bfd4244`/`42d79fad`/
> `622b96a2`/`31aa5203`/`ca07a814`, city `3413028b`, fountain `8db68ccb`, greets
> `570a7b44`. The FORWARD city row agrees at `bbc0056b` once run 1 is discarded
> (it cold-bakes its own cube — the documented discard, and the base arm's run 1
> `316b9f8e` is exactly that trap firing). `render_gate.sh` **ALL FOUR PASS** at
> `4ac809e5`/`826c09e6`/`b41894f9`/`166fa25a`.
>
> **COST: NOT RESOLVABLE, and I am not going to invent a number.** Two
> interleaved ABBA batteries (6×20 and 8×25 iters) on this box read a within-arm
> spread of 85–315 ms and 117–207 ms against an effect that can only be a few
> tenths; the paired deltas change sign round to round. What IS known is
> mechanical and directional: the per-pixel gate lost a 4-tap bilinear plus a
> subtract/multiply/clamp and gained nothing, the sheet build lost a whole
> 6×512²-per-probe upsample pass, and resident memory went 6.7 → 13.3 MiB. Re-run
> it on a quiet machine before quoting anything.

> ## 2026-08-15f — THE GREETS PEEL A/B WAS MEASURING NOTHING: GREETS HAS NEVER RUN MORE THAN ONE PEEL PASS
>
> ### 1. THE BAND LEAD IS DEAD — the user's `--xpar-peel-passes=1` A/B had no independent variable
>
> `xparPeelPassesEffective()` (`DeferredSurfaceKernel.cpp:4469`) honours the CLI
> flag only when `isSet()`; otherwise it reads `CurScene->XparPeelPasses`.
> **`FOUNTAIN.CPP:1083` is the only writer of that field in the tree.** Greets
> never writes it, so `CurScene->XparPeelPasses == 0 → 1`. His "default (band)"
> and his "`=1` (no band)" launches were the SAME xpar configuration.
>
> **MEASURED, and it is stronger than the wiring argument:** at four greets
> poses — t=3409 (his band pose, his camera, 1512×848), t=2845, t=3122, t=5743 —
> `--xpar-peel-passes=4` is **byte-identical** to the default. The peel-pass axis
> is a complete no-op for greets: no (mesh, side) batch there has two fragments
> stacked on one pixel. Everything the last two rounds built on "the band is
> composited by peel passes ≥ 2" — the stale-slice / `--xpar_strip_extent`
> hypothesis included — rests on a null A/B and should be dropped.
>
> ### 2. WHAT THE BAND IS INSTEAD: per-launch intermittent, and I caught a flip
>
> Combined with his "it also vanishes with explicit `=4`, and solves itself",
> the band is **per-launch intermittent**, not flag-borne. Batteries at his exact
> pose (`--repro=greets@t=3409 --repro_from=0 --repro_xres=1512 --repro_yres=848`,
> 349 real frames, dummy drivers):
>
> | arm | runs | verdict |
> |---|--:|---|
> | plain | 12 | all `10f9d325…` |
> | `MallocPreScribble=1 MallocScribble=1` | 6 | all `10f9d325…` — **no uninitialised-heap read reaches this frame** |
> | `--repro_late_cam` (scene camera drives the scrub → real camera MOTION) | 4 | all `10f9d325…` |
> | `--repro_prescenes` | **15** | **14 × `10f9d325…`, 1 × `c2fa243c…`** |
> | `--repro_late_cam --repro_prescenes` | 3 | all `10f9d325…` |
> | `--snapshot=greets@t=3409` ± scribble | 12 | all `4754c66f…` |
>
> **One flip in 15.** The flipped frame differs on **109 933 px (8.57 %), max
> |Δ| 107, mean signed −0.50**, and its bounding box is rows 129–554 × cols
> 478–1286 — **exactly the mirror panel's reflected content**, dithered over the
> whole panel rather than banded:
> `docs/img/fogwt/mirror_launch_nondet_t3409_diff.png`
> (/Users/gil-ad/work/revival-fog/docs/img/fogwt/mirror_launch_nondet_t3409_diff.png).
>
> So: **the greets mirror is a per-launch non-deterministic surface**, at ~7 %
> flip rate, and the flip lands on exactly the pixels his band lands on. This is
> not yet his +40 band (wrong sign, wrong shape, 100× rarer than he sees it) —
> but it is the first headless reproduction of ANY per-launch mirror variance,
> and it is the thread to pull. Suspects, in order: the RTT slot round-robin
> (`GreetsMirror.cpp:1270`, `kRttPerFrame = 2` of greets' 7–8 slots, so a slot's
> texture is 0–3 frames stale depending on scheduling) and the adaptive per-slot
> resolution (`GreetsMirror.cpp:3133`, `pow2clamp` of the on-screen footprint).
>
> ### 3. A REAL BUG FOUND ON THE WAY: the strip composite lit itself off the 12×8 grid
>
> `da286d8b` made the transparent kernel resolve its light tile from the PIXEL
> using `ctx.lt{NumX,NumY,SizeX,SizeY}` — correct for the `runTilePass` path.
> But `RenderXparClumpInStrip` swaps `tileLights` to `g_stripLights`, a
> **1 × numStrips** grid of 8-row Y strips, and left the 12×8 LIGHTING geometry
> in the ctx. So `lightTileAt()` subscripted the strip array as
> `(py/106)*12 + px/126`: **the same category error `da286d8b` fixed, one
> dispatch level down**, and a regression that path did not have before it.
> Fountain is the only scene on the unified TBR, so it is the only victim.
>
> **MEASURED before the fix**, fountain t=1200 at 1512×848: `--xpar_tile_lights`
> vs `--no-xpar_tile_lights` differ on 845 px, max |Δ| 19 — and the OFF arm is
> the correct one. **After the fix the ON arm is byte-identical to OFF** at both
> 1512×848 and 1920×1080. The fix also repairs a pre-existing legacy defect it
> supersedes: the old ordinal subscript clamped to `DEFERRED_NUM_TILES-1 = 95`,
> so every strip below row 768 shared one light list.
>
> **Eight pins all unmoved** (chase `3bfd4244`/`42d79fad`/`622b96a2`/`31aa5203`/
> `ca07a814`, city `3413028b`, fountain `8db68ccb`, greets `570a7b44`),
> `render_gate.sh` **4/4 PASS**.
>
> ### 4. SEPARATE DELIVERABLE: `--cinematic`'s deep peel for CITY and CHASE is dark
>
> `SceneTick.h:222` gives `cine::kCity` (and `kChase`) `.xparPeel = 4`, applied
> via `FF::setDefault(IntId::xpar_peel_passes, …)`. **`setDefault` writes the
> value but never the set-bit** (`FeatureFlags.h:149`, deliberately — it exists
> so a scene can tune a global without trampling a user override), and
> `xparPeelPassesEffective()` gates on `isSet()`. So the profile's 4 is invisible
> and city/chase run 1 pass under `--cinematic`. **Fountain is NOT affected** —
> it has an explicit `FntSc->XparPeelPasses = 4`.
>
> **DO NOT RUSH TO WIRE IT.** Engaging it costs 4× the transparent raster +
> composite and buys **nothing measurable**: city t=1961, chase t=800 and chase
> t=1600, all under `--cinematic`, are **byte-identical** between the default
> and an explicit `--xpar-peel-passes=4`. The honest fix is either to delete the
> dead `.xparPeel` field from the profile or to give the resolver a third tier
> (`isSet` → `Scene::XparPeelPasses` → profile value) — but the second only
> matters once a scene has stacked transparents that need it, and neither city
> nor chase does today.

> ## 2026-08-15e — THE CAUSTIC SAMPLER INTERPOLATED THREE CHANNELS TO PRODUCE ONE NUMBER (JUDGE CALL: 7 PX AT |Δ|=1)
>
> The parked item from 2026-08-15d, built and landed. `sampleWaterTex` did a
> full bilinear tap on **each of B, G and R** — three sets of four byte-extracts,
> four int→float converts and seven flops — and **every caller collapsed the
> result on the very next line**: `cell = (cb + cg + cr) * (1/765)`. All four of
> them (the two glint passes, `causticCellVaried`'s three octaves, and
> `CausticModulation`'s env-bake re-shade). The per-channel colour is never read
> by anything. `buildWaterDetail` now also writes a `g_waterCell` plane of
> `float(B+Gn+R)` and the sampler is one lerp over it.
>
> **The corner values are exact** — three bytes, ≤ 765, exactly representable —
> so nothing is lost at the texels; only the lerp itself reassociates
> (`lerp(B)+lerp(G)+lerp(R)` → `lerp(B+G+R)`).
>
> ### MEASURED — interleaved min-of-6 against `ebb03fc9`, one asset tree (load 5.5→8.7)
>
> | item | ebb03fc9 | child | Δ |
> |---|--:|--:|--:|
> | chase t=800 `water-glints` | 11.190 | **9.297** | **−1.893 ms (−16.9 %)** |
> | city t=1961 `water-glints` | 4.727 | **4.187** | **−0.540 ms (−11.4 %)** |
>
> `Ginstr` 1.125 → 1.020 chase, 0.431 → 0.395 city; `Gcyc` 0.341 → 0.286 chase,
> 0.154 → 0.139 city. **The internal control is exact**: `water-ripple` does not
> sample the caustic texture and its Ginstr is **0.403 vs 0.403**; `renderFrame`
> is 3.756/3.756 and 6.110/6.110, `gbuffer` 0.602/0.602 and 0.769/0.769.
>
> ### THE WHOLE ROUND, END TO END — parent = `d7a62231` + the instrument only (load 13→18)
>
> | item | parent | final | Δ |
> |---|--:|--:|--:|
> | chase t=800 `water-glints` | 17.559 | **9.175** | **−8.384 ms (−47.7 %)** |
> | city t=1961 `water-glints` | 8.035 | **4.574** | **−3.461 ms (−43.1 %)** |
> | city t=1961 `water-ripple` | 4.561 | **3.557** | **−1.004 ms (−22.0 %)** |
> | city `FRAME_MIN` | 85.590 | **82.470** | −3.120 ms |
>
> Load-robust columns: `Ginstr` chase 1.193 → 1.020 (−14.5 %), city glints
> 0.497 → 0.395 (−20.5 %); `Gcyc` chase 0.344 → 0.289 (−16.0 %), city glints
> 0.172 → 0.139 (−19.2 %). Controls flat to 3 decimals throughout
> (`renderFrame` 3.767/3.762 and 6.112/6.115, `gbuffer` 0.603/0.602 and
> 0.769/0.770, `DeferredLighting-call` 0.587/0.586 and 1.166/1.167).
> **The percentage is load-dependent and honestly so** — the parent was
> parallelism-limited, so it degrades faster under load than the child does;
> the −29.4 % measured for the banding fix at load 19 and the −47.7 % here are
> the same change seen at two loads.
>
> ### THE BYTE VERDICT — a judge call, and a very small one
>
> **7 changed pixels out of 12.4 M across six poses, every one of them
> |Δ| = 1/255.** Per pose: chase t100 1 px, t400 **0**, t800 1 px, t1200 **0**,
> t1600 2 px, city t1961 3 px. fountain and greets are untouched (greets has no
> water; the fountain has no `pwater` call site). Temporal battery, **one pose
> per process** because water is animated: chase t=795/800/805/810 → 1/0/2/1 px,
> city t=1959/1961/1963 → 4/3/0 px. The changed pixels are **isolated
> singletons, not a structure** — city t=1961's three are at (1603,412),
> (894,890) and (1880,946), and the amplified diff shows them as three separate
> dots with nothing between them:
> `docs/img/water/L4_celllerp_city_t1961_diff.png`
> (/Users/gil-ad/work/revival-fog/docs/img/water/L4_celllerp_city_t1961_diff.png).
> This cannot change glint flicker character: the twinkle comes from the wave
> field's motion, and a 1/255 flip on 0.0001 % of pixels is below the
> quantisation of the effect it would have to perturb.
>
> **FOUR PIN VALUES MOVE, AND THESE ARE THE NEW ONES** (child self-identical
> 3/3, `render_gate.sh` 4/4 PASS):
>
> | pin | old | new |
> |---|---|---|
> | chase t=100 | `7678a6bc6ea964b3b859ecb11c0673c3` | **`3bfd424458a74b7892821de04ab69ca9`** |
> | chase t=400 | `42d79fadd825a329b36143efe052edfb` | *unmoved* |
> | chase t=800 | `b29c73f1c54f42a02e0dc2484780cc03` | **`622b96a214404a0abec1d21aae47a478`** |
> | chase t=1200 | `31aa52039f9b228fa6307c12e14811eb` | *unmoved* |
> | chase t=1600 | `1544b0e775900b099ac9e38d42fd750d` | **`ca07a81450afc8f1594d32d5e62c10cb`** |
> | city t=1961 | `3f8948232c192a979ffe7f76c4b387ab` | **`3413028bc70b99f4bc3ee9eec9de7c14`** |
> | fountain t=2500 | `8db68ccb59416e9a44037e9f387b7bd9` | *unmoved* |
> | greets t=1588 | `570a7b443f768393dc6647044a9e67b3` | *unmoved* |
>
> **This commit is deliberately separate from `ebb03fc9`, which is bit-exact** —
> revert this one alone and the old pin table returns with −29 %/−40 % of the
> round still in place. Countersign or revert; the numbers to weigh are 7 px
> against 1.9 ms of chase and 0.5 ms of city.
>
> > **COUNTERSIGNED 2026-08-15 by the user — "6 seems ok"** (item 6 of the
> > round-up: this judge call together with `f1ffc925`'s chase/greets ≤ 2/255).
> > The four moved pin values above are ADOPTED and are the ones every later
> > gate is held to — they reproduce exactly in the 2026-08-16 exact-water-bit
> > round's differential gate. No revert is held open.

> ## 2026-08-15d — THE WATER PASSES WERE NEVER SLOW, THEY WERE BADLY SHARED: −29 % CHASE, −40 % CITY, BIT-EXACT
>
> Round-1 row 7 (`docs/PERF_STATE.md` §00) — "water simulation + glints, never
> profiled before this round" — profiled and attacked. Two instruments land with
> it, because the passes had **no phase row at all**: they run OUTSIDE
> `renderFrame`, so every `--deferred_prof` table ever printed omitted them.
>
> **CENSUS FIRST — `--water_census`, the denominators nobody had.** Every water
> pass scans the WHOLE framebuffer and ray-casts each pixel to the water plane:
>
> | pass | scene | scanned/f | above horizon | past far plane | occluded | LIVE |
> |---|---|--:|--:|--:|--:|--:|
> | `ripple` (dispMap) | city t=1961 | 2 073 600 | 385 920 | — (no far cut) | — (no occl test) | 1 687 680 (81.4 %) |
> | `glints` | city t=1961 | 2 073 600 | 385 920 | 109 440 | 652 958 | 925 282 (44.6 %) |
> | `glintsVaried` | chase t=800 | 2 073 600 | 837 120 | 15 360 | 103 222 | 1 117 898 (53.9 %) |
>
> **GREETS HAS NO WATER AT ALL** — no `pwater::` call site, no water surface in
> `GREETS.FLD`. The handover's "greets' water ceiling presumably runs the same
> machinery" is false; there is nothing to census there.
>
> **THE FOUNTAIN-198M PATTERN IS *NOT* WHAT THIS IS, and that is the finding.**
> 40.4 % of chase's scan and 23.9 % of city's produce nothing, and all of it is
> exactly row-aligned (837 120 = 436 × 1920; 385 920 = 201 × 1920; 109 440 = 57 ×
> 1920) — so a row-level early-out captures 100 % of the reject set with no
> floating-point risk. It was built, and it is **worth 0.7 %**: the reject path is
> ~10 instructions against **~1050 instructions per LIVE pixel**. The scan is not
> the cost. Two sibling micro-levers died the same way and are recorded below.
>
> **WHAT IT ACTUALLY WAS: the row banding.** Each pass handed every worker ONE
> CONTIGUOUS BLOCK of rows — the worst possible split of a screen whose top half
> is a 10-instruction reject and whose bottom half is a full wave-slope + caustic
> + specular evaluation. `pwater::runRowBands` hands out 8-row chunks from a
> shared atomic cursor instead. Rows are independent (each writes only its own
> VPage row), so this is **bit-exact for any scheduling order**, and it now
> reports `thrsum`/`effPar` — the column whose absence hid this for the whole
> campaign. `effPar` 11.1 / 10.9 / 11.1 of 12 on the three passes.
>
> **AND ONE REAL COMPUTE LEVER: `powf` was being called where its answer could
> not matter.** The write is `add = int(g*255 + 0.5)` with
> `g = pow(ndh,shin)*strength*distFade` and `distFade <= 1`, so `add == 0` —
> already a `continue` — for every `ndh` with `pow(ndh,shin)*strength < 0.5/255`.
> Inverting that ONCE per pass gives a plain compare ahead of the libm call that
> provably skips only pixels whose output was zero. **Bit-exact by algebra**, and
> at city t=1961 it is −11.7 % of the glint pass's CYCLES on its own.
>
> ### MEASURED — three arms, one asset tree, interleaved min-of-6 (r0 dropped, load 19→17)
>
> | item | parent | child | Δ |
> |---|--:|--:|--:|
> | chase t=800 `water-glints` | 14.195 | **10.021** | **−4.174 ms (−29.4 %)** |
> | city t=1961 `water-glints` | 7.720 | **4.602** | **−3.118 ms (−40.4 %)** |
> | city t=1961 `water-ripple` | 4.015 | **3.117** | **−0.898 ms (−22.4 %)** |
> | city `FRAME_MIN` | 76.960 | **73.410** | **−3.550 ms (−4.6 %)** |
>
> Parent is `d7a62231` + the instrument ONLY, so both arms carry the same timer.
> Attribution: `renderFrame` Ginstr/f **3.728 vs 3.732** (chase) and **6.057 vs
> 6.056** (city); `gbuffer` and `DeferredLighting-call` flat in wall and
> instructions on both scenes. `Ginstr` chase glints 1.268 → 1.128, city glints
> 0.523 → 0.434. `Gcyc` chase 0.342 → 0.333, city 0.171 → 0.151 — **chase's win
> is almost purely parallelism, city's is parallelism plus the `powf` skip.**
>
> ### GATES
>
> **BIT-EXACT. All eight pins reproduce their RECORDED values — parent 2/2, child
> 3/3** (chase `7678a6bc` / `42d79fad` / `b29c73f1` / `31aa5203` / `1544b0e7`,
> city `3f894823`, fountain `8db68ccb`, greets `570a7b44`). `render_gate.sh`
> **4/4 PASS**. No flag, no judge call, default on. The chase pins exercise
> `RenderGlintsVaried` (CHASE.CPP `setDefault(water_variation, true)`) and the
> city pin exercises both `RenderGlints` and `updateRippleDispMap`.
>
> ### KILLED THIS ROUND, WITH NUMBERS (all three bit-exact, all three flat)
>
> * **Constant texture dimensions** in `sampleWaterTex` — `g_waterTexW/H` are
>   mutable globals, so `u / W` was a runtime FLOAT DIVIDE and the varied path
>   takes three taps, i.e. **six fdivs per shaded pixel** for a number that has
>   been 256 since the texture existed. Folding it removed 5 of the band's 9
>   `fdiv`. Ginstr 1.172 → 1.181, Gcyc 0.346 → 0.340. **The divides are
>   independent per tap and the out-of-order core hides them entirely.**
> * **Occlusion test hoisted above the world-position reconstruction** (fires on
>   652 958 px/f in city, each of which was paying two 3-term dot products
>   first): 6.5 M instructions of 477 M = **1.4 %**, sub-noise.
> * **Per-row horizon / far-plane early-out** (above): **0.7 %**.
>
> ### WHAT IS LEFT, PRICED
>
> The per-live-pixel body is the whole cost, and it is **five libm calls** in the
> chase path — `cosf` ×3 + `sinf` (the `waterWaveSlopeVaried` swell) + `powf` —
> confirmed by disassembly of the outlined band (`$_0::operator()(int,int)`:
> 909 instructions, 3 `fsqrt`, 5 `bl`). Ablation prices them:
>
> * the 4 swell transcendentals → **−20.1 % instructions / −11.7 % wall** of the
>   chase pass. No bit-exact route exists (they are always consumed); the lever
>   is a 4-wide vector `cos`, which is a judge call on ~1 ulp.
> * `powf` beyond the ndhMin skip → the remaining calls are the pixels that
>   actually glint, so what is left is the lobe itself.
>
> A third, untried and structural: **`sampleWaterTex` bilinearly interpolates
> three channels and every caller then collapses them to `(cb+cg+cr)/765`.**
> Storing the sum plane instead is one lerp instead of three (~45 instructions
> per tap, ×3 in the varied path ≈ 15 % of the band) and is exact in the corner
> values but reassociates the lerp — a small, quantifiable judge call.

> ## 2026-08-15c — THE SPOT PYRAMID HAS A READER NOW, AND THE HANDOVER'S 48.9 % WAS NOT ITS NUMBER
>
> **THE TARGET DOES NOT EXIST AT THE SIZE THE HANDOVER CLAIMED.** 91891249 left
> the 8x8 uniformity pyramid built for all 76 shadow-map entries and read by only
> the cube tap, with the 2-D spot tap in `computeMapShadowAtten` named as the next
> customer at "48.9 % of the omni loop at his pose". **That 48.9 % is 0b85e5df's
> ablation-ladder figure for the whole `computeMapShadowAtten` STAGE, which is
> three independent bodies, and the spot tap is the smallest of them by three
> orders of magnitude.** `--omni_census`, greets, deterministic frame to frame:
>
> | | calls/f reaching the stage | own 2-D map (`smIdx>=0`) | clone SOURCE map (`srcSm`) | clone SOURCE cube (`srcCube`) | none of the three |
> |---|--:|--:|--:|--:|--:|
> | his pose t=3122 | 2.615 M | **0.02 %** | 2.41 % | 65.14 % | 32.43 % |
> | t=5743 | 4.749 M | **0.08 %** | 0.00 % | 0.42 % | 99.50 % |
> | t=1588 | 7.660 M | **0.76 %** | 0.19 % | 1.02 % | 98.03 % |
>
> At his pose the stage costs what it costs because of the **`srcCube` mirror-clone
> branch** — 65.14 % of the calls, a world->view round trip plus a full
> `CubeShadow_Sample` — and 0b85e5df's guard had already deleted the 32.43 %
> that carry nothing. The light's OWN spot map is live in about **520 calls a
> frame**.
>
> **THE ANSWER TO "DOES THE PYRAMID REACH THE 1.77 M SURVIVING MIRROR-CLONE
> CALLS": 96.4 % of them, and it already did.** Of the 1.767 M calls that survive
> the guard at his pose, 1.703 M take `srcCube`, whose `CubeShadow_Sample` carries
> 91891249's fast path; they are inside that census's `reached 2.555 M/f` row, of
> which **81.6 % skip** (the row's other 0.848 M is the own-cube
> `resolveCubeAtten` — the two add up to 2.551 M against 2.555 M measured). The
> remaining 0.063 M take `srcSm`, a single NON-PCF **depth** comparison
> (`pixZ + 128 < zS`); the pyramid is an **id** summary and structurally cannot
> speak for a depth test, so that branch is out of its reach — as are the
> volumetric spot readers (`volSpotShadow`, `DeferredFastFog.cpp:344`) for the
> same reason.
>
> **SHIPPED ANYWAY, BECAUSE IT IS FREE AND BYTE-NULL.** The lookup is wired into
> the spot tap on the same construction as the cube's, ABOVE the addressing block.
> The condition is SIMPLER here: this tap's PolyId arm reads `psB` and only `psB`
> (the dynamic plane appears only in the `z00..z11` quartet the DEPTH arm uses),
> so `uniSD` alone is the whole verdict and no `uniDyn == 0` precondition is
> needed. Skip rates and what they buy, measured:
>
> | pose | spot taps reaching the pyramid | uniform-lit | uniform-occ | **skipped** | cube taps, same frame |
> |---|--:|--:|--:|--:|--:|
> | his t=3122 | 0.001 M/f | 0.0 % | 100.0 % | **100.0 %** | 2.555 M/f |
> | t=5743 | 0.004 M/f | 17.4 % | 80.4 % | **97.8 %** | 4.744 M/f |
> | t=1588 | 0.058 M/f | 56.3 % | 36.1 % | **92.4 %** | 7.593 M/f |
>
> ### MEASURED — two arms (parent 91891249 / child), one worktree, one asset tree, interleaved min over rounds 1-8 of 9, load 5.3-10.6
>
> | | parent | child | delta |
> |---|--:|--:|--:|
> | **greets t=1588, 1920x1080** | | | |
> | `lighting-w1` Ginstr/f | 4.853 | **4.850** | **-0.003** |
> | `renderFrame` Ginstr/f | 7.464 | **7.461** | **-0.003** |
> | frame_ms min | 69.53 | 69.45 | -0.08 |
> | **greets t=3122, HIS POSE, 1512x848** | | | |
> | `lighting-w1` Ginstr/f | 1.747 | 1.747 | **0.000** |
> | `renderFrame` Ginstr/f | 4.772 | 4.772 | **0.000** |
> | **greets t=5743, 1920x1080** | | | |
> | `lighting-w1` Ginstr/f | 2.761 | 2.761 | **0.000** |
> | `renderFrame` Ginstr/f | 4.365 | 4.365 | **0.000** |
> | **city t=1961** `renderFrame` Ginstr/f | 6.048 | 6.049 | +0.001 |
> | **fountain t=1200** `renderFrame` Ginstr/f | 1.580 | 1.580 | 0.000 |
> | `shadow-uniformity` Ginstr/f (all three greets poses) | 0.010-0.011 | 0.010-0.011 | **0.000** |
>
> **THE ONE MOVING ROW IS PREDICTED EXACTLY.** 0.0536 M skipped taps a frame at
> t=1588 x the ~50 instructions 91891249 measured per skipped tap = 0.0027 Gi;
> measured -0.003, and `renderFrame` gives back the same -0.003, so the
> attribution is the tap and nothing else. At the other two poses the prediction
> is 0.0002 and 0.00005 Gi — three to four orders below the 0.3 % reproducibility
> floor, and the measurement duly reports 0.000. **The point of the two poses that
> move nothing is the other direction: hoisting the four PCF weights and the mode
> load above the addressing block does not COST anything either**, on a function
> whose other branches run 1.77 M times a frame at his pose.
>
> `shadow-uniformity` is IDENTICAL between the arms, which is the confirmation
> that the build cost was already being paid: 91891249 built the spot pyramids
> every frame with no reader, and this commit adds no build work at all.
>
> ### GATES
>
> Differential, both binaries from ONE tree snapshot in ONE worktree, one asset
> tree. **All eight pins reproduce their RECORDED values 3/3 on both arms** —
> chase t100 `7678a6bc` t400 `42d79fad` t800 `b29c73f1` t1200 `31aa5203` t1600
> `1544b0e7`, greets `570a7b443f768393dc6647044a9e67b3`, fountain
> `8db68ccb59416e9a44037e9f387b7bd9`, city `3f8948232c192a979ffe7f76c4b387ab`
> (the parent's fountain run 1 returned `b91cb2ba` — the documented cold-cache
> first-run-after-a-rebuild artefact, gone from runs 2 and 3 and never seen on the
> child, whose runs read the cache the parent's had already written).
> `render_gate.sh` **ALL FOUR PASS, twice on each arm**.
>
> **THE STALENESS GATE: 9 greets poses x 6 configurations, all 54 hashes
> identical** — default, `--shadow_swizzle`, `--shadow_lightmap
> --shadow_lm_dynamic`, `--no-shadow_dynamic`, `--shadow_polyid_no_pcf`, and
> Depth mode via `FDS_SHADOW_POLYID=0`. Both fast-path arms are exercised across
> it (uniform-occ dominates at his pose and t=5743, uniform-lit at t=1588).
>
> **A TRAP FOUND WHILE BUILDING THAT GATE, worth keeping: a MULTI-t snapshot
> (`--snapshot=greets@t=a,b,c,...`) is NOT run-to-run stable past its FIRST
> pose.** Two consecutive runs of the SAME parent binary agree on pose 1 and
> disagree on all eight others, in every configuration. Any sweep that batches
> poses into one process is measuring noise; `spotpyr_sweep.sh` gives each pose
> its own process and is then self-identical 2/2.
>
> ### WHAT THIS MEANS FOR THE NEXT LEVER
>
> The `computeMapShadowAtten` stage at his pose is now fully accounted: 32.43 %
> of its calls deleted by 0b85e5df, 65.14 % served by the cube pyramid at an
> 81.6 % skip rate, 0.02 % served here, and 2.41 % (`srcSm`) structurally out of
> reach of an id pyramid. What is left inside it is the `srcCube` branch's
> world->view round trip that 0b85e5df already flagged as a judge-call
> re-association to a world->light matrix — not a tap count.

---

> ## 2026-08-15b — 80 % OF THE CUBE TAP'S TEXEL READS ARE A LOOKUP THE MAP ALREADY ANSWERED: THE 8x8 PolyId UNIFORMITY PYRAMID
>
> The backlog's re-specified 8x8 item (43ac3456), built. In `ShadowMode::PolyId`
> a tap's verdict is a pure id comparison, so an 8x8 shadow-map block whose
> texels all carry one id `c` settles ANY 2x2 PCF footprint inside it without a
> single texel read — same verdict, computed once. **BYTE-NULL, no flag,
> default on: all eight pins unmoved 3/3 differentially, `render_gate` 4/4 PASS
> x3, and 9 poses x 4 flag configurations identical.**
>
> **-1.27 ms of `lighting-w1` and -1.01 ms of the frame at greets t=5743;
> -0.93 ms of `lighting-w1` and -0.33 ms of the frame at his t=3122 pose.
> chase / city / fountain build no shadow maps at all and are structurally
> inert (renderFrame Ginstr/f identical to 3 decimals).**
>
> ### THE DESIGN, AND THE TWO THINGS THE RESPEC ASKED FOR THAT TURNED OUT TO BE UNNECESSARY
>
> `ShadowMap::uniSD` / `uniDyn`: one u32 per 8x8 block = the ShadowMatID every
> texel of that block's **9x9 APRON** carries, or `kShadowUniMixed`. 16 KB per
> 512^2 face.
>
> **The apron is what removes the boundary condition instead of handling it.** A
> 2x2 footprint anchored at (iX, iY) reads (iX..iX+1, iY..iY+1), so summarising
> the bare 8x8 block would leave every footprint anchored on the block's last row
> or column straddling two blocks — 23 % of anchor positions, and precisely the
> ones in the interior of a large uniform region where the fast path is worth
> most. Summarising one texel PAST the right and bottom edges makes the block
> index `(iX>>3, iY>>3)` sufficient on its own. No straddle branch exists to get
> wrong. At the map edge the apron clamps to `xres-1`; the tap's own
> `iX + 1 >= xres` reject already guarantees nothing reads past it.
>
> The respec asked for (a) per-8x8 DEPTH BOUNDS and (c) a per-(block x light)
> classification projecting the block's frustum-cell corners and bailing when
> they straddle two cube faces. **Neither is needed.** (a) is a depth-mode
> concept — PolyId compares no depths. (c) was an artefact of classifying in
> SCREEN space; the pyramid is indexed in SHADOW-MAP space by the tap's own
> `iX/iY`, which the tap has already computed and which has already selected its
> cube face, so there is no second projection and no seam.
>
> **The dynamic plane is handled by the pyramid, not around it.** `closestPacked`
> reads both planes, so the full tap's fast path fires only where the DYNAMIC
> apron is uniformly EMPTY (`dId == 0` everywhere => `closestPacked` returns the
> static id verbatim, whatever the z halves hold) and the static apron is
> uniform. The lightmap-composite `dynamicOnly` tap reads only `packDyn`, so
> `uniDyn` alone settles it. Measured under `--shadow_lightmap
> --shadow_lm_dynamic`, that form is **49.3 %** of taps and 78.8 % still skip.
>
> **Byte-exactness is by construction, not by argument.** The weights are
> computed ONCE, by the same four statements, and merely READ by the fast path —
> recomputing them inside it would be a bet on clang contracting
> `(1.0f - fx) * (1.0f - fy)` into the same fnmsub in two places. Uniform-LIT
> returns 1.0f (the tail's `1.0f - 0.0f`) before the weights are even formed;
> uniform-OCCLUDING falls through to the shared weight block and runs the same
> four `occ += w` in the same order. `--shadow_polyid_no_pcf` is mirrored
> explicitly (`return 0.0f`) rather than summed, because that flag sets
> `occ = 1.0f` outright and the weight sum can land a few ULP short and return
> ~6e-8. Depth mode never enters (it passes `surfaceMatId = -1`).
>
> ### THE FAST PATH IS A BRANCH AROUND THE TAP, IN TWO PIECES
>
> Three refutations say the tap sits at its register-allocation limit, so nothing
> was added INSIDE it. The block lookup runs first (it needs only iX/iY) and a
> uniform-lit block returns before the weights; the addressing block — the
> swizzle test, the row offset, the four texel offsets, the two plane base
> pointers — is skipped whole by both arms. Ordering matters: the first cut
> placed the check after the addressing block and bought **-0.138 Gi/f** at
> t=5743; moving it above bought **-0.192**.
>
> ### THE BUILD COST ATE THE WIN ONCE, AND THAT IS WHY THE DIRTY BOX EXISTS
>
> First form: eager rebuild of the whole plane for every map the bake wrote.
> **MEASURED 0.375 ms/frame at his pose against a 0.336 ms tap saving — the
> build ate the win whole** (t=5743: 0.337 vs 0.846). Cause: greets' fourteen
> 512^2 DYNAMIC planes are 14 MB, streamed every frame to rediscover that the
> mech is the only thing in them.
>
> Fix: phase A clears the plane, so every texel outside what the raster actually
> wrote is known-zero and its pyramid entry is known-zero. `MekaleleShadowDepth`
> stamps each clipped n-gon's clamped bbox into a THREAD-LOCAL box (no atomic —
> the raster runs thousands of polygons per tile and four CAS each would price
> the bake instead of the build); the phase-B tile task copies it into its own
> job slot; the tick thread unions the slots per map after the drain, serially,
> so the result cannot depend on which worker finished first. The build then
> zeroes the 16 KB pyramid and rescans only that box, expanded one block on the
> low side because a block one column earlier still sees into it through its
> apron.
>
> | build, ms/frame at greets t=5743 | eager | dirty-box |
> |---|--:|--:|
> | `DynMeshes` (14 x 512^2 dyn planes) | 0.270 | **0.038** |
> | `DynOmnis` (28 maps, 128^2 + spots) | 0.115 | 0.113 |
> | `--deferred_prof` `shadow-uniformity` row (both, per frame) | 0.337 | **0.106** |
>
> The `DynOmnis` figure does not move because it is dispatch-bound, not
> scan-bound: 28 tiny tasks through `dispatchIndexed` + semaphore drain. Init
> (`StaticOnce`, 48 maps) is a one-shot **0.78-0.82 ms**.
>
> ### TAPS SKIPPED — the new `--shadow_tap_census` rows (`-DFDS_SHADOW_TAP_CENSUS=ON`)
>
> | pose / config | taps reaching the pyramid | uniform-lit | uniform-occ | **skipped** |
> |---|--:|--:|--:|--:|
> | greets t=5743 (bench) | 4.744 M/f | 29.5 % | 50.8 % | **80.3 %** |
> | greets t=1588 (pin recipe) | 6.402 M/f | 32.4 % | 28.7 % | **61.1 %** |
> | greets t=1588 + `--shadow_lightmap --shadow_lm_dynamic` | 6.041 M/f | 57.8 % | 21.0 % | **78.8 %** |
>
> The screen-space census that motivated this (76.1 % of taps in an 8x8-uniform
> SCREEN block) turns out to have been a lower bound on the shadow-map-space
> number at t=5743 and an upper bound at t=1588 — related quantities, not the
> same one, and only the shadow-map one is the thing the pyramid can act on.
>
> ### MEASURED — no flag, two arms, one worktree, one asset tree, interleaved min-of-6 (round 0 discarded), load 2.6-5.5
>
> | | parent | child | delta |
> |---|--:|--:|--:|
> | **greets t=5743, 1920x1080** | | | |
> | `lighting-w1` wall | 24.155 | **22.885** | **-1.270 (-5.3 %)** |
> | `lighting-w1` Ginstr/f | 2.953 | **2.761** | **-0.192 (-6.5 %)** |
> | `lighting-w1` Gcyc/f | 0.825 | 0.759 | -0.066 (-8.0 %) |
> | `renderFrame` wall | 39.688 | 38.565 | -1.123 (-2.8 %) |
> | `renderFrame` Ginstr/f | 4.557 | **4.365** | **-0.192 (-4.2 %)** |
> | frame_ms min | 46.34 | **45.33** | **-1.01** |
> | `shadow-uniformity` (new row, outside renderFrame) | - | 0.106 ms / 0.010 Gi | +0.106 / +0.010 |
> | **greets t=3122, HIS POSE, 1512x848** | | | |
> | `lighting-w1` wall | 15.555 | **14.627** | **-0.928 (-6.0 %)** |
> | `lighting-w1` Ginstr/f | 1.872 | **1.747** | **-0.125 (-6.7 %)** |
> | `renderFrame` Ginstr/f | 4.896 | **4.772** | **-0.124 (-2.5 %)** |
> | frame_ms min | 43.31 | **42.98** | **-0.33** |
> | `shadow-uniformity` | - | 0.144 ms / 0.011 Gi | +0.144 / +0.011 |
> | **city t=1961** `renderFrame` Ginstr/f | 0.640 | 0.640 | **0.000** |
> | **fountain t=1200** `renderFrame` Ginstr/f | 0.301 | 0.301 | **0.000** |
>
> **ATTRIBUTION IS EXACT AT BOTH POSES**: the `renderFrame` Ginstr delta equals
> the `lighting-w1` delta to the last printed digit (-0.192 / -0.192 at t=5743,
> -0.124 / -0.125 at his pose), which is what "the only thing that changed inside
> renderFrame is the tap" predicts. The build is OUTSIDE renderFrame (depth 3,
> same bucket as the bake it must be judged against), so it is NOT hidden in
> those rows — net of it the change is **-0.180 Gi/f** at t=5743 and
> **-0.111 Gi/f** at his pose. `shadow-bake` moves +0.165 / +0.059 ms wall for
> +0.002 / +0.003 Gi — the n-gon bbox stamp, at the edge of what wall resolves.
>
> **chase is NOT in the table because `--bench=scene` does not support it**
> ("scene='chase' not supported (try city, fountain, greets)") — the five chase
> pins carry it instead, and a chase snapshot emits no `[SHADOW]` line at all:
> chase, city and fountain build ZERO shadow maps, so the pyramid is never
> allocated and never queried there.
>
> ### POLYID-VS-DEPTH COVERAGE
>
> `g_shadowMode` is ONE process-wide global (`FDS/RENDER/Shadows.cpp:161`), not a
> per-light property, and its compile-time default is PolyId
> (`FDS_SHADOW_POLYID_DEFAULT_ON = 1`). So the census is a scene census, not a
> light census: **greets is the only scene with shadow maps** — 10 spot (2-D)
> maps + 11 cube omnis x 6 faces = 76 entries, of which 8 omnis (48 faces) are
> `Omni_StaticShadow` and baked once, 14 of those faces take a per-frame
> `DynMeshes` dynamic bake, and 28 (18 moving-omni cube faces + the 10 spots)
> take a per-frame `DynOmnis` static-plane bake. Depth mode is reachable only via
> `FDS_SHADOW_POLYID=0` or the F3 toggle; it takes no pyramid path (it passes
> `surfaceMatId = -1`) and is byte-identical, verified. The 10 spot maps get a
> pyramid built that nothing reads yet — the 2-D tap in `computeMapShadowAtten`
> is the obvious next customer (48.9 % of the omni loop at his pose) and is left
> for its own measured commit. **[SUPERSEDED 2026-08-15c — that customer was
> wired and it is worth nothing: the 48.9 % is the whole `computeMapShadowAtten`
> STAGE, and its own 2-D spot map is live in 0.02 % of the stage's calls at his
> pose. See the block at the top of this file.]**
>
> ### GATES
>
> Differential, both binaries built in ONE worktree from ONE tree snapshot, one
> asset tree, run 1 discarded. **All eight pins UNMOVED 3/3 on the child and 3/3
> on the parent**: chase t100 `7678a6bc` t400 `42d79fad` t800 `b29c73f1` t1200
> `31aa5203` t1600 `1544b0e7`, greets `570a7b443f768393dc6647044a9e67b3`,
> fountain `8db68ccb59416e9a44037e9f387b7bd9`, city
> `3f8948232c192a979ffe7f76c4b387ab`. `render_gate.sh` **ALL FOUR PASS, three
> times** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
>
> Beyond the pins, because the pin recipe does NOT exercise the composite tap:
> **9 greets poses (1588 / 2000 / 3122 / 4871 / 5534 / 5743 / 5780 / 5814 / 5970)
> x 4 configurations — default, `--shadow_swizzle`, `--shadow_lightmap
> --shadow_lm_dynamic`, `--no-shadow_dynamic` — every hash identical.** Plus
> single-pose differentials on `--shadow_polyid_no_pcf`
> (`fefdf162ec8c2b85df86129d92b188b1`, 2/2 each) and Depth mode
> `FDS_SHADOW_POLYID=0` (`ec057b9d3102f5b3970dd04c3e194df6`, 2/2 each). The
> multi-pose sweep is the STALENESS gate: a pyramid that failed to track a
> re-baked plane would show as a frame-dependent divergence, not a constant one.
>
> ### WHAT THIS DOES NOT BUY, STATED PLAINLY
>
> The parked ceiling read "76 % of 7.27 ms". It is not 76 % of 7.27 ms, and the
> reason is that the tap's cost is not its loads. Measured: **~50 instructions
> per skipped tap** (0.192 Gi / 3.81 M skipped) — the 8 packed loads, the four
> `closestPacked` id resolutions, the four compares and the addressing block.
> Everything BEFORE the block lookup — the face select, the 3x3 view-to-light
> matmul, the two frustum-ratio rejects, `1/lz`, `smX/smY` — is untouched and is
> the majority of the tap. IPC barely moves (3.579 -> 3.564 in the first form),
> which says the same thing from the other side: the tap was compute-bound, not
> load-bound, so removing loads pays in issue slots and not in stalls. Cutting
> the projection needs a different lever than this one.

> ## 2026-08-15 — THE OMNI LOOP IS A SHADOW LOOP: 74 % of it is the shadow chain, and 99.5 % of its 2-D-shadow calls compute the constant 1.0f
>
> Round 1's #1 item — "deferred omni loop, 19.9 of 47.8 ms at greets t=5743,
> 2.490 Ginstr/f, compute-bound, never itemized below the tap chain". Now
> itemized, with a committed ladder and a new per-pixel census. Full write-up +
> every table in `docs/OPTIMIZATION_BACKLOG.md` (2026-08-15).
>
> **-1.75 ms of `lighting-w1` and -1.78 ms of the frame at t=5743, -0.55 ms at
> his t=3122 pose, -1.05 ms of chase's `lighting-w1`; city and fountain neutral.
> BIT-EXACT — all eight pins unmoved 3/3, `render_gate` 4/4 PASS, no flag.**
>
> ### The instruments
>
> `-DFDS_OMNI_ABLATE=n` (12 staged `continue`s in the per-light body, each
> sinking what it retains) and `--omni_census` / `-DFDS_OMNI_CENSUS=ON` (per
> (pixel x light): where each light dies, and the live-lights-per-PIXEL
> histogram the tap census structurally could not produce). Drivers
> `scratchpad/omni_ablate.sh`, `scratchpad/omni_ladder.py`, `scratchpad/omni_run.py`,
> `scratchpad/omni_pins.sh`. Both compile out by default and the shipping build
> measures `lighting-w1` **3.247 Gi/f against the parent's 3.247** at t=5743 and
> 1.916 vs 1.917 at his pose.
>
> **Two independent sessions, separate builds, loads 11-21 apart, agree to
> <= 0.10 % on every row of the ladder at both poses.** Stage 1 — the loop
> deleted — is 0.761 Gi/f, reproducing round 1's `--prof_no_lights` remainder
> (0.801) independently, so the omni loop proper is 2.486 Gi/f against the map's
> recorded 2.490.
>
> ### The split, both sessions (Ginstr/f, w1 Gi/f cumulative)
>
> | st | what is KEPT | t5743 A | t5743 B | his A | his B |
> |---|---|--:|--:|--:|--:|
> | 1 | loop floor (loop deleted) | 0.761 | 0.761 | 0.472 | 0.472 |
> | 2 | + mirrorId test | 0.804 | 0.804 | 0.511 | 0.511 |
> | 3 | + w, N.L dot, dot<0 | 0.916 | 0.916 | 0.556 | 0.556 |
> | 4 | + len2, range | 0.976 | 0.977 | 0.586 | 0.586 |
> | 5 | + bounce portal | 1.006 | 1.007 | 0.611 | 0.611 |
> | 6 | + rsqrt/dist/k | 1.057 | 1.057 | 0.638 | 0.638 |
> | 7 | + spot cone | 1.063 | 1.063 | 0.640 | 0.640 |
> | 8 | **+ computeMapShadowAtten** | **1.468** | **1.468** | **1.346** | **1.346** |
> | 9 | **+ cube tap** | **2.890** | **2.889** | **1.618** | **1.617** |
> | 10 | + relief horizon | 2.928 | 2.928 | 1.643 | 1.643 |
> | 11 | + diffuse accumulate | 2.983 | 2.983 | 1.687 | 1.687 |
> | 0 | **FULL (+ specular lobe)** | **3.247** | **3.247** | **1.916** | **1.917** |
>
> Shadow chain = **73.5 %** of the loop at t=5743, **67.7 %** at his pose. The
> largest NON-shadow item is the specular lobe at 10.6 % / 15.9 %; attenuation,
> N.L, cone, portal and the accumulate are 12 % / 15 % between them. The thing
> the round-1 map hoped to find in "the rest of the omni loop" is not there.
>
> ### The 24.5 % figure is superseded, and the reason is a measurement artefact
>
> Against the same `lighting-w1` denominator the cube tap alone is **43.8 %** at
> t=5743, not 24.5 %. `--prof_no_cube_tap` short-circuits `resolveCubeAtten` to
> **1.0f = fully lit**, so the 54.6 % of taps that return 0 stop taking their
> `continue` and pay diffuse AND specular in the no-tap arm: 2.59 M extra shaded
> pairs a frame at ~148 instructions = 0.385 Gi of the 1.42 Gi gap. An ablation
> that makes lights BRIGHTER cannot price what it removed. The ladder cuts
> downstream in BOTH arms, so it can.
>
> ### The per-pixel distribution (`--omni_census`, deterministic frame to frame)
>
> | | t=5743 | his t=3122 |
> |---|--:|--:|
> | (px x light) pairs / frame | 8.687 M | 7.164 M |
> | lights entered per shaded px | 8.32 | 11.14 |
> | **reach the accumulate** | **24.79 %** | **28.50 %** |
> | **live lights per shaded px** | **2.06** | **3.17** |
> | dies on mirrorId | 4.79 % | **44.28 %** |
> | dies on N.L<0 / range / cone | 21.5 / 8.4 / 10.7 % | 10.3 / 3.6 / 5.3 % |
> | dies on the **cube tap** | **29.77 %** | 0.02 % |
>
> Histogram of live lights per shaded pixel, t=5743: 0:6.7 1:20.5 2:39.5 3:26.6
> 4:6.6 >=5:0.2 %. His pose: 0:1.5 1:6.0 2:21.0 3:17.3 **4:53.7** >=5:0.5 %.
> **A pixel is lit by two to four lights and the loop walks eight to eleven to
> find them.** The poses fail in different places — his on the mirror clones,
> t=5743 on the cube — so no single pose names the lever.
>
> ### What shipped
>
> `computeMapShadowAtten` is an out-of-line function with a **176-byte frame and
> ten callee-save pairs**, called once per (pixel x light) past the cone test.
> All three of its bodies are guarded on an index being `>= 0`, so with all three
> negative it can only return its `1.0f` initialiser — and the census says
> **99.50 % of its 4.749 M calls a frame carry none of them** (his pose 32.43 %
> of 2.615 M). The guard is the function's own three tests hoisted to the call
> site as one AND (`(smIdx & srcSm & srcCube) >= 0`, since absent indices are -1).
> Bit-exact by construction. No flag: there is no arm to compare, the skipped
> calls returned 1.0f.
>
> ### Two levers refuted with numbers
>
> **Skipping the all-zero dynamic shadow plane** — provably byte-null (`packDyn`
> is `assign`-ed 0 and written only by a bake reached from two
> `if (shadow_dynamic())` sites; the two-plane resolve then collapses
> algebraically) — **costs +12.4 % of `lighting-w1`'s instructions at t=5743 and
> +10.3 % at his pose, i.e. worse than the parent.** One extra bool in the tap's
> innermost lambda. That is the THIRD independent measurement of this mechanism
> (tap-census hooks +2.0 %, `d9248f6d`'s `FDS_DEV` branch). **The cube tap is at
> its register-allocation limit: it only gets cheaper by being CALLED LESS.**
>
> **Passing the pixel's world position into `computeMapShadowAtten`** instead of
> recomputing it in both mirror-clone branches (9 muls + 9 adds per pair for a
> per-pixel quantity the caller already hoists): predicted 0.027 Gi, **measured
> 0.010 Gi (-0.53 % at his pose, 0.000 at t=5743)** — the compiler was already
> CSE-ing most of it. Not worth a signature change against the pins.
>
> ### Gates
>
> greets `570a7b443f768393dc6647044a9e67b3`, fountain
> `8db68ccb59416e9a44037e9f387b7bd9`, city `3f8948232c192a979ffe7f76c4b387ab`,
> chase t100 `7678a6bc6ea964b3b859ecb11c0673c3` t400 `42d79fadd825a329b36143efe052edfb`
> t800 `b29c73f1c54f42a02e0dc2484780cc03` t1200 `31aa52039f9b228fa6307c12e14811eb`
> t1600 `1544b0e775900b099ac9e38d42fd750d` — **3/3 each on the child, 2/2 on the
> parent in the same worktree** (fountain's run-1 cold bake discarded as
> documented). `render_gate.sh` 4/4 PASS (`4ac809e5` / `826c09e6` / `b41894f9` /
> `166fa25a`). No pin value moves; the table is unchanged by this commit.
>
> **RE-VERIFIED AFTER REBASE onto `d8fc4978` (cone round 7) — the campaign was
> measured on `b502c394` and two other agents landed underneath it.** All eight
> pins reproduce on the NEW parent 2/2 and on the child 3/3, `render_gate` 4/4
> PASS at the rebased HEAD, and the instruction deltas are unchanged to three
> decimals: greets t=5743 `lighting-w1` 3.246 -> 2.953 Gi/f, 26.15 -> 24.06 ms,
> `renderFrame` 41.76 -> 39.62 ms, frame min 48.30 -> **46.26 ms (-2.04)**; his
> pose 1.916 -> 1.872 Gi/f, frame 43.89 -> 43.68; chase t=800 `lighting-w1`
> 0.738 -> 0.572 Gi/f and 5.23 -> **3.94 ms**, `renderFrame` 35.47 -> 34.47 ms.
> The two changes do not interact — cone round 7 is in `DeferredVolumetric.cpp`,
> this one in the surface kernel's light loop.

> ## 2026-08-15 — ROUND 7: THE CONE KERNEL'S INNERMOST LOOP WAS REBUILDING PER-LIGHT CONSTANTS 10 800 TIMES A TILE. NOT ONE PIXEL MOVES, AND IT IS THE FIRST CONE WIN THAT HELPS ALL THREE SCENES
>
> `f1ffc925` §14.7 parked the per-spot scalar prologue as *"bit-exact by
> construction and unattacked"* — **8.3 % of chase's cone pass, 7.2 % of its
> cycles, 104 instructions per (batch × spot) including a DIVIDE**. Cashed now.
> Full write-up, inventory table and reproduction: `docs/HW_PROFILING.md` §15.
>
> ### THE DEFECT IS THE LOOP ORDER, NOT THE MATH
>
> The nest is **row → 8-px batch → spot**, spot loop **innermost**. So twelve
> scattered SoA loads, two three-term dot products, a square, four selects and
> `1/(cosI − cosO)` ran for every (batch × spot) pair to produce values that
> depend only on **which light it is**. They now live in a `ConeSpotPre` record
> built once per tile — a coarse 6×4 tile at 1920×1080 is 320×270 px, so
> **10 800 evaluations collapse to one**. Five more per-spot values were lifted
> out of the solve itself (`sphereC`, `cq`, and the exact constants `c2+c2`,
> `−(DP+DP)`, `cq·−4`).
>
> ### NOT ONE PIXEL MOVES — EVERY PIN AT ITS CURRENT VALUE, FIRST TRY
>
> Every field is a **verbatim move** of the line it replaces, so the
> contraction map (§13) travels with the value. Differential battery (both
> binaries in one worktree, one asset tree, run 1 discarded), **2/2 each**:
> chase t100 `7678a6bc…` t400 `42d79fad…` t800 `b29c73f1…` t1200 `31aa5203…`
> t1600 `1544b0e7…`, greets `570a7b44…`, city `3f894823…`, fountain
> `8db68ccb…` — **all eight UNMOVED**. `render_gate.sh` **ALL FOUR PASS**
> byte-identical (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
> **No pin table edit is needed and none was made.**
>
> ### MEASURED — no flag (§14.3 priced the dual-arm tax at +5.9 %), two arms,
> ### parent binary, interleaved min-of-6, TWO independent sessions
>
> | pose | cones wall | cones Ginstr/f | cones Gcyc/f |
> |---|--:|--:|--:|
> | chase t=800 | 14.766 → **13.601** (−7.9 %) | 2.191 → **1.992 (−9.1 %)** | 0.502 → 0.464 (−7.6 %) |
> | chase t=400 | 20.114 → **18.527** (−7.9 %) | 3.013 → **2.691 (−10.7 %)** | 0.688 → 0.625 (−9.2 %) |
> | city t=1961 | 15.529 → 14.998 (−3.4 %) | 2.264 → **2.081 (−8.1 %)** | 0.540 → 0.520 (−3.7 %) |
> | greets t=1588 | 6.545 → 6.270 (−4.2 %) | 0.994 → **0.951 (−4.3 %)** | 0.220 → 0.207 (−5.9 %) |
> | **greets t=3122 (your pose)** | 6.186 → 5.997 (−3.1 %) | 0.893 → 0.889 (−0.4 %) | 0.212 → 0.206 (−2.8 %) |
>
> `Ginstr/f` reproduced **to 0.15 %** across the two sessions on every row.
> Attribution (frame Ginstr delta vs pass delta): chase t=800 −0.199/−0.199,
> t=400 −0.320/−0.322, city −0.181/−0.183, greets −0.042/−0.043.
>
> **It beats its own 8.3 % price** because the five values lifted out of the
> *solve* are not in that bucket. And it is the **first cone change of the
> campaign that helps all three cone scenes** — the prologue ran for every pair
> regardless of which branch it took, so city's all-wide cones paid it exactly
> as chase's narrow ones did. Round 6 could only reach city as codegen.
>
> ### THE OTHER PARKED ITEM: BUILT, MEASURED, NOT KEPT
>
> §14.7's 8-segment `W²`/`D·W` closed form (3 vector ops per segment against
> 11, ×8) is **+0.1 / +0.2 / +0.7 % INSTRUCTIONS** on chase t800 / t400 /
> greets t1588 — a small LOSS — and −1.7 / −1.6 / −2.8 % cycles. The
> arithmetic explains it: that loop runs only on **alive** pairs (8.1 % of
> chase's at t=800), so the whole block is ~0.9 % of the pass *gross*. It is a
> re-association, and it was priced in bytes so nobody has to again: chase
> 75/85 px, greets 2 323 px, all at max |Δ| 2/255. Under §14.7's 2 % bar →
> **not kept**, compiled out in place as `FDS_CONE_SEG_CLOSEDFORM` with its
> numbers. The shipping binary is **byte-identical** with the arm present,
> which is the proof it costs nothing to carry.
>
> **The reusable rule**: an optimisation inside a branch is worth its op count
> times the branch's **fire rate**, not its op count. §14.4 said the same thing
> about culls from the other end.

> ## 2026-08-14c — FOUNTAIN'S 77 % FRAME ITEM IS CLOSED: -11.99 ms, AND THE CAUSE WAS THAT NOTHING IN THE PIPELINE BOUNDED X
>
> Round 1's #3 item — "two-layer transparent lighting, 21.7 of 28.1 ms at
> fountain t=1200, achievable 5-8 ms". Measured cause, two byte-null fixes,
> **-11.99 ms of a 27.46 ms frame (-43.7 %)**. Full write-up + every table in
> `docs/OPTIMIZATION_BACKLOG.md` (2026-08-14b).
>
> ### FIRST, THE PROFILE'S OWN NAME FOR IT WAS MISLEADING
>
> `Render_DeferredTransparentLighting_Tile<0>` / `<1>` are **front-FACING and
> back-FACING**, not two depth layers. The depth peel is a different axis:
> `FOUNTAIN.CPP:1083` sets `XparPeelPasses = 4`, so every clump rasters and
> composites four times per side. "Do one layer instead of two" would have been
> aimed at the wrong structure.
>
> ### THE CENSUS (`--xpar_extent_census`, new)
>
> A clump flushes on every (mesh, side) change **and on every interleaved
> sprite** — and the spray is 33 358 sprites a frame. fountain t=1200:
>
> | | |
> |---|--:|
> | clump flushes / frame | **3 221** |
> | composite invocations (x4 peel passes) | **12 884** |
> | px scanned by the full-strip-width composite | **197.90 M** |
> | px with a live transparent fragment | **0.97 M — 0.491 %** |
>
> The per-strip dispatch bounds Y and **only** Y: every clump composited
> `x = 0 .. XRes` regardless of where its handful of faces actually landed. And
> peel passes 1-3 are **100.0 % / 100.0 % / 100.0 %** empty on the front layer
> (99.9 % / 100 % / 100 % on the back) — 9 663 of 12 884 passes a frame render
> nothing at all.
>
> ### TWO FLAGS, BOTH DEFAULT ON, BOTH BYTE-NULL BY CONSTRUCTION
>
> * **`--xpar_strip_extent`** — the rasterizer records the tile columns it
>   touched; the clump clears and composites only those. Outside them the slice
>   is in its cleared state and the kernel's first test is
>   `mat32 == 0xFFFFFFFF -> continue`, so the skipped columns contributed zero.
>   Scanned px **197.90 M -> 6.66 M**, live count identical.
> * **`--xpar_peel_early_out`** — reverse peel accepts on
>   `(z < z_existing) & (z > peelFloor)` with `z_existing` pre-cleared to
>   `0xFFFF`; if the previous pass committed nothing its extent is still all
>   `0xFFFF`, so this pass — and every later one — accepts nothing. Passes
>   **12 884 -> 6 014**.
>
> ### THREE ARMS (parent binary / OFF / ON), one asset tree, interleaved, min-of-6
>
> | pose | parent | ON | delta | `TBR-render` Ginstr/f |
> |---|--:|--:|--:|---|
> | **fountain t=1200** | **27.46** | **15.47** | **-11.99 (-43.7 %)** | 3.011 -> **1.081** |
> | fountain t=2500 | 22.73 | **13.88** | -8.85 (-38.9 %) | 2.211 -> 0.766 |
> | fountain t=600 | 19.96 | **12.28** | -7.68 (-38.5 %) | 1.821 -> 0.538 |
> | city t=1961 | 76.34 | 76.30 | 0.00 | 0.846 -> 0.841 |
> | greets t=3122 (your pose) | 47.04 | 46.96 | -0.08 | 1.178 -> 1.162 |
> | chase t=1600 | — | — | — | 0.853 -> 0.853 |
>
> `renderFrame` falls 11.992 ms and `TBR-render` falls 12.003 ms at t=1200 —
> **100.1 % of the frame saving is in the phase attacked.** `parent` and `off`
> agree on instructions to 3-4 decimals at every pose, so the OFF arm is the
> parent and the flags carry no dark cost.
>
> ### GATES — ALL BYTE-IDENTICAL, THREE FLAG CONFIGURATIONS
>
> fountain `8db68ccb59416e9a44037e9f387b7bd9` 3/3, greets
> `778fa6acd85a69cf241babefcdaf598e` 2/2, city `3f8948232c192a979ffe7f76c4b387ab`
> 2/2, all five chase pins, `render_gate.sh` **ALL FOUR PASS** — under flags OFF,
> peel-only, and both ON. Both TBR schedulers covered (glass rows go through
> `TBR_Render_GlassLayered`, the plain-deferred fountain row through the plain
> strip walk). **Animated evidence: `--snapshot=fountain@t=100..3000` step 100,
> 30 frames, every per-frame md5 identical between arms** — the additive spray is
> exactly where a composite/peel-order error would flicker, and it does not move.
> There is no look call to make.
>
> ### ONE MORE MEASUREMENT WHILE IN THERE — `gbuffer` PARALLELISM, AND THE OBVIOUS FIX IS REFUTED
>
> Round 1 flagged `gbuffer` at `effPar` 5.0-5.5 of 12 as the survey's only
> scheduling problem. It IS granularity — the G-buffer raster dispatches a
> **fixed 6x5 = 30-tile grid** (`RENDER.CPP:449`), 320x216 px a tile at
> 1920x1080 — but the naive fix is measured and loses. Probe build with the grid
> at **12x10 = 120 tiles**, same binary otherwise, snapshot harness:
>
> | pose | grid | `gbuffer` wall | thrsum | `effPar` | `gbuffer` Ginstr/f |
> |---|---|--:|--:|--:|--:|
> | chase t=800 (x2) | 6x5 | **10.22** | 57.20 | 5.2 | 0.597 |
> | chase t=800 (x2) | 12x10 | 14.41 | 141.89 | **9.1** | **1.424** |
> | chase t=1600 (x2) | 6x5 | **5.64** | 28.78 | 5.0 | 0.312 |
> | chase t=1600 (x2) | 12x10 | 9.00 | 89.02 | **9.3** | **0.881** |
> | fountain t=2500 | 6x5 | 2.64 | 14.13 | 5.0 | 0.160 |
> | fountain t=2500 | 12x10 | **2.41** | 19.08 | **6.7** | 0.195 |
>
> **The idle is real and subdividing removes it — `effPar` 5.2 -> 9.1 — but the
> total work more than DOUBLES** (chase t=800 thrsum 57.2 -> 141.9 ms, +139 %
> instructions), because each clipper tile re-walks the whole scene's face list:
> per-tile traversal is re-paid per tile. Net wall +41 % on chase. So it is not a
> serial section (a serial section would put `thrsum` at or below `wall`) and it
> is not fixable by "more tiles". The shape that could work is splitting only the
> HEAVY tiles, or cutting the per-tile traversal cost so subdivision is cheap;
> both move `ClipperTileRect` ownership and want their own round. **Do not
> re-propose a uniform finer grid — it is priced here and it loses.**

> ## 2026-08-14b — THE PARKED SHADOW EARLY-OUT, MEASURED: HALF OF EVERY TILE'S LIGHT LIST AT YOUR POSE CANNOT LIGHT ONE PIXEL OF THAT TILE
>
> The shadow-diet round parked *"per-tile light/shadow early-outs"* because they
> change shadow bytes. Measured now, and the two halves split cleanly: one is
> byte-null and ships default ON, the other is refuted at the tile with numbers.
> Full write-up + every table in `docs/OPTIMIZATION_BACKLOG.md` (2026-08-14).
>
> ### THE DENOMINATOR WAS WRONG BY 2x, SO FIX IT FIRST
>
> The parked note said shadow sampling is 48.8 % of the lighting stage. By
> ablation (`--prof_no_cube_tap`, interleaved min-of-4, greets t=5743): the whole
> cube tap is **7.269 ms of a 29.710 ms lighting-w1 — 24.5 %**, and 15.8 % of the
> frame. Every ceiling below is a fraction of 7.27 ms.
>
> ### SHIPPED, DEFAULT ON, BYTE-NULL — `--deferred_tile_sphere_cull`
>
> The tile light list culled each light against the tile's screen rect **and**,
> separately, against its z-extent. Two separable projections of a sphere are
> strictly weaker than the sphere: a light off the **diagonal** corner of a
> tile's frustum chunk passes both and reaches no pixel. Now the light's range
> sphere is tested against the tile's chunk sphere — the one `tileChunkSphere()`
> already builds for the spot-cone cull, reading the **same** `range2` the
> per-pixel test compares against.
>
> New instrument `--shadow_tap_census` is what made this decidable, and at
> **your pose** (t=3122, 1512x848) it found the thing worth finding:
>
> | | greets t=5743 @1920x1080 | **your pose** t=3122 @1512x848 |
> |---|--:|--:|
> | lights/tile | 8.72 | **15.14** |
> | tile-light pairs that light ZERO pixels of their tile | 9.6 % | **52.5 %** |
> | loop-prologue px/frame spent on them | 0.87 M | **5.17 M** |
> | after the cull | 0.39 M | **2.50 M** |
> | cube taps/frame, before AND after | 4.725 M | 0.848 M |
>
> **The tap count not moving is the structural proof of byte-nullity** — 397
> (tile x light) pairs deleted a frame at your pose and not one tap changed.
>
> Cost, three arms (parent binary / OFF / ON), one asset tree, interleaved, min
> over rounds. The box was at load 10-13 from other agents, so read `Ginstr/f`
> — it reproduces to 0.3 % and a descheduled worker retires no instructions:
>
> | | parent | OFF | **ON** |
> |---|--:|--:|--:|
> | your pose, lighting-w1 Ginstr/f | 1.998 | 2.001 | **1.935 (-3.3 %)** |
> | your pose, renderFrame Ginstr/f | 5.163 | 5.168 | **5.097 (-1.4 %)** |
> | t=5743, lighting-w1 Ginstr/f | 3.296 | 3.296 | **3.278 (-0.55 %)** |
>
> **Said plainly: the wall column does not separate the arms at t=5743** (per-round
> spread +/-1 ms against a 0.5 % delta). At your pose lighting-w1's wall min moves
> 19.545 -> 18.979. The instruction column is monotone at both poses and the
> mechanism predicts the size it measures.
>
> **GATES, cull ON *and* OFF on the same binary** (differential, so the claim is
> "this moved nothing" rather than "the hash matches"): greets
> `778fa6acd85a69cf241babefcdaf598e` 3/3 ON, 2/2 OFF; fountain
> `8db68ccb59416e9a44037e9f387b7bd9` 2/2 ON (run 1 cold-bake discarded), 3/3 OFF;
> city `3f8948232c192a979ffe7f76c4b387ab` 2/2 both; `render_gate.sh` **ALL FOUR
> PASS** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`). The glass paths are
> covered on purpose — the greets pin runs `--glass-refract=1 --glass-test
> --xpar-peel-passes=4` — because the chunk sphere spans OPAQUE depth bounds and a
> transparent pixel in front of `zMin` sits outside it. That exposure is not new
> (the shipped `deferred_zcull` rejects on the same `zMin`) and it does not fire.
>
> ### REFUTED AT THE TILE, AND THE REASON IS GRANULARITY
>
> A **perfect oracle** — free, error-free — collapsing every tile-uniform
> (tile x light) entry to one answer removes 16.8 % of taps at t=5743: a
> **1.22 ms ceiling**, for a byte-moving change whose errors would land on
> 160x135 px blocks, the most visible seam size in the frame. Not built.
>
> The coherence is real, just not at the tile. `--shadow_tap_census_block=B`,
> same frame, uniform share of all 4.725 M taps: tile **16.8 %**, 32x32 35.8 %,
> 16x16 50.9 %, **8x8 76.1 %**, 4x4 89.5 %. So the parked idea had the right
> instinct and the wrong scale by a factor of ~16. The backlog now carries a
> **byte-null** 8x8 respecification instead of the byte-moving tile one: in
> PolyId mode, a block footprint whose cube-face texels all carry one id `c`
> gives the 2x2 PCF exactly, `occ = (c != 0 && c != receiverId)`, with no tap and
> no error — it needs per-8x8 depth bounds and an id-uniformity pyramid per cube
> face. That is a real build, not a tweak, so it is specified rather than started.
>
> ### DEAD HYPOTHESES, WITH NUMBERS
>
> * *"Taps still run for out-of-range lights."* **False in the shipping kernel** —
>   the scalar loop tests `len2 > r2` before every tap. The census closes it.
> * *...but TRUE in the 8-wide `--deferred_vec` kernel*, where an out-of-range lane
>   gets `safe_len2 = 1` and so `k = dot * (1 - 1/Range) > 0`, passes the
>   `kArr[lane] <= 0` guard, taps, and has the result blended away. Costs this box
>   nothing (`FDS_DEFERRED_VEC_DEFAULT` is 0 on arm64); live waste on x86. **Left
>   alone deliberately — no pin covers that path**, so a "free" fix there would be
>   an unverifiable one.
> * *"The tile lists do no range culling."* They do (screen rect, z-extent, cone,
>   mirror presence). The gap was only that rect AND z is a separable sphere.
>
> ### THE INSTRUMENT IS COMPILE-GATED, AND THAT IS ITSELF A MEASUREMENT
>
> Never-taken census hooks in the light loop's two innermost bodies still cost
> **+2.0 % of lighting-w1's instructions** as first written (+0.9 % after moving
> the block index to the tap site; folding four counter arrays into one did not
> help). Register pressure, the same mechanism `d9248f6d` found in the cube tap's
> `FDS_DEV` abort branch. So they are behind `-DFDS_SHADOW_TAP_CENSUS=ON`,
> default OFF, and the shipping kernel measures **+0.15 %** against its parent —
> inside the 0.3 % floor. The flag stays registered and prints the rebuild line.

> ## 2026-08-14 — `--env_live_water` STILL MOVED THE WHOLE REFLECTION, BECAUSE 5f1ffa92 MEASURED THE PATH HE DOES NOT RUN
>
> His report on a binary rebuilt at `5f1ffa92`: **"--env_live_water still moves the
> whole reflection."** He was right, and the miss is legible in 5f1ffa92's own
> sentence — *"the deferred EnvPanoStore and the forward TriMesh both carry it"*.
> **Carrying the mask is not applying it.** Every number in that commit came from
> `FDS_CITY_ENV_PIXEL=1 --deferred`; he runs `PRESETS/city-noir.flags`, which names
> neither, and `deferred` defaults OFF — so his city renders through the **forward
> paraboloid-sheet path**, the one consumer that was never measured.
>
> ### WHY A CORRECT MASK STILL LEAKED THERE
>
> The forward path perturbed the reflected direction **per VERTEX**; the rasterizer
> interpolates the resulting UV **affinely**. A corner whose reflection is water
> therefore drags EVERY pixel of its triangle — the reflected skyline included —
> weighted by that corner's barycentric. A per-vertex mask cannot localize below
> face granularity, however exactly right the mask is. The deferred path never had
> the problem: each pixel reads the mask for itself.
>
> ### MEASURED UNDER HIS PRESET, THREE EYE HEIGHTS
>
> Camera pinned, wave clock moved ALONE (`--water_ripple_speed` 1.0 vs 1.6), scored
> only over pixels that are provably static with the flag OFF. Region classes from
> the new `--env_water_region_viz`.
>
> | eye y | arm | reflected SKY moving | of region | mean \|Δ\| / max | reflected WATER Σ\|Δ\| |
> |---|---|---|---|---|---|
> | 190 (street) | before | 22 185 | 16.33 % | 2.99 / 17 | 100 % |
> | 190 | **after** | **882** | **0.65 %** | 1.94 / 8 | **105.5 %** |
> | 423 (5f1ffa92's pin pose) | before | 38 148 | 15.19 % | 4.94 / 41 | 100 % |
> | 423 | **after** | **5 594** | **2.23 %** | 5.68 / 40 | **110.6 %** |
> | 800 (high) | before | 53 251 | 13.12 % | 7.17 / 94 | 100 % |
> | 800 | **after** | **8 366** | **2.06 %** | 7.01 / 93 | **102.4 %** |
>
> The DEFERRED path at the pin pose **under the same preset** reads 6 927 (3.1 %) —
> so the forward path is now better localized than the reference it is held to.
> Two thirds of the residual is the INTENDED soft ramp across the reflected
> waterline, not leak: `--env_live_water_mask_bias=0.5` takes 5 594 → 1 672 and
> 8 366 → 2 120 **with the water motion unchanged** (102 %, 98.8 %). What is left
> is bloom/CA spill from the moving water, and it matches the floor an
> all-or-nothing per-face gate reaches (1 702).
>
> ### THE FIX, AND THE ARM THAT WAS REJECTED WITH NUMBERS
>
> `EU/EV` stay UNPERTURBED. Transform hands the filler a per-FACE UV offset
> (`Face::LwDU/LwDV`) = the corners' full-tilt (w=1) UV displacement averaged
> **weighted by each corner's own coverage**; the sheets carry the bake's coverage
> plane in their **ALPHA byte** (bilinear 128²→512² per cube face, then the gather
> table the colour already uses); `TheOtherBarry<OVERWRITE, TEXTURETEXTURE>` scales
> the offset by **each pixel's own** coverage before a second gather. The mask is
> read at the UNPERTURBED lookup structurally — it is the alpha of the texel the
> pixel was already fetching. A flat (unweighted) corner mean freezes the skyline
> just as well but retains only 87.2 % of the water motion instead of 110.6 %.
> **REJECTED: an all-or-nothing per-FACE gate.** It freezes the skyline equally
> (1 702 px) and costs **57 % of the water motion** (Σ\|Δ\| 42.9 % of before) —
> at these poses most panes straddle the reflected waterline and freeze whole.
>
> ### AUDIT — EVERY CONSUMER OF THE TILT
>
> | site | granularity | mask-gated | verdict |
> |---|---|---|---|
> | `DeferredSurfaceKernel.cpp:1158` scalar env compose | per pixel | yes, unperturbed dir | correct, unchanged |
> | `DeferredSurfaceKernel.cpp:5061` OuterVec env-only lane | per pixel | yes, unperturbed dir | correct, unchanged |
> | `Transform.cpp:2583` forward paraboloid sheets | per **vertex** | yes — and it did not matter | **THE LEAK; now per-pixel** |
> | `CITY.CPP cityMirrorGlassForward()` (`--city_env_pixel` pass 1) | per vertex | **never perturbs at all** | gap, not leak (0 px); flag default off |
> | `Transform.cpp` equirect else-branch | per vertex | no mask exists | correct: documented "no mask → no tilt" |
>
> ### COST AND GATES
>
> Flag OFF is byte-null AND instruction-null (`LwDU/LwDV` exactly 0, `lwAlphaMask`
> false, block skipped) — proved DIFFERENTIALLY: the pre-fix binary and this one
> render the flag-off frame to the same md5 at two poses (`f592a411…`,
> `2b833c09…`). Flag ON, city t=1961 under his preset, `iters=25`, 7 interleaved
> rounds with the arm order reversed halfway, min-of-7: **92.775 → 93.539 ms
> (+0.76 ms, +0.8 %)**, within-arm spread ±4.3 ms — the paired per-round deltas
> (median +1.0 ms) say it is real rather than noise, and it is accounted for: every
> reflective pixel now pays an alpha extract, an integer compare and a
> `horizontal_or` branch, and the wet ones a second 8-lane gather.
> GATES, flags default off: city `3f8948232c192a979ffe7f76c4b387ab` 2/2, **forward
> city `8dc44df9e014629d7db2e1567c4c2810` 2/2** (run 1 cold-bakes its own cube —
> discarded, as documented), greets `778fa6acd85a69cf241babefcdaf598e` 2/2,
> fountain `8db68ccb59416e9a44037e9f387b7bd9` 3/3, `render_gate.sh` **ALL PASS**
> (mirrortest `4ac809e5`, rttslot `826c09e6`, conetest `b41894f9`, halotest
> `166fa25a`).
>
> ### NEW INSTRUMENT — `--env_water_region_viz` (default 0, byte-null)
>
> The SCREEN-SPACE half of `FDS_ENVBAKE_DUMP`'s mask PGM. The PGM says where the
> mask thinks the water is in CUBE space; this says which SCREEN pixels read those
> texels. It INVERTS one class of baked env texels (1 = water, 2 = non-water), so
> that class's screen region is the diff against the un-inverted frame. Invert
> rather than paint a flag colour: a night skyline is mostly near-black, so "paint
> the non-water black" classifies almost nothing, while the complement differs for
> every texel but an exact 0x808080 and survives bloom, grade, CA and tonemap.
> It is the only way to ask the FORWARD path what a pixel is reading, and without
> it not one row of the table above is measurable. Evidence:
> `/Users/gil-ad/work/revival-fog/docs/img/envmap/envwaterfwd_region_viz_t1961.png`,
> `…/envwaterfwd_pin_y423_before_after.png`,
> `…/envwaterfwd_high_y800_before_after.png`,
> `…/envwaterfwd_low_y190_before_after.png`.
> The flag stays default OFF; write-up in `docs/BACKLOG_PLANS.md` section 2.


> ## 2026-08-13e — THE JAMB STRIPING IS ONE 6:1 FACE, `--mip_aniso` FIXES IT, AND THE DEFAULT STAYS OFF BECAUSE THE BLUR IS YOURS TO CALL
>
> His report: at `FDS_GREETS_CAM="18.8969765,3.21025538,-58.888485,-0.896694958,-0.0735020638,0.436503887"`
> t=5970, the grazing doorway jamb shows compressed high-frequency **striping** against
> the neighbouring wall's coarse look — "a texture discontinuity near the edge".
> `705b70da` proved it is not displacement (it reproduces bit for bit in the flat arm).
> This session measured it. **The fix already existed as `--mip_aniso`; what did not
> exist was any measurement of it, and two of the things its own flag doc asserted
> turn out to be wrong.**
>
> ### THE DEFECT IS ONE FACE, AND THE NUMBER IS 6.0:1
>
> New instrument **`--mip_aniso_stats`** (default OFF, changes no pixel): a per-face
> anisotropy census keyed on `Face*`, area-weighted across the tiles a face is clipped
> into, printed once at exit. It reports the **singular values** of the UV→screen
> Jacobian in texels per screen pixel — not the ratio of its two columns, which
> understates a wall whose compression runs diagonally on screen.
>
> The striping face at his pose:
>
> | | value |
> |---|---|
> | anisotropy (σmax/σmin) | **6.0 : 1** |
> | σmax / σmin | 1.73 / 0.29 texels per screen px |
> | legacy geometric-mean LOD | −0.50 → **level 0** |
> | max-axis LOD | +0.79 → **level 1** |
> | dLOD | **+1.29** |
> | undersampling at the chosen level | **1.73× → 0.87×** |
>
> With `mip_bias` 0.5 + truncation (= round-to-nearest) the geometric mean lands level 0
> and the face still samples 1.73 texels per pixel along its worst axis. Point-sampled,
> **that is the striping**. `--pom_mip_viz` confirms it per pixel: the whole jamb is
> mip 0 under the legacy metric, and exactly that sliver turns mip 1 under `--mip_aniso`
> — `docs/img/mipaniso/his_t5970_mipviz_legacy_vs_aniso.png`.
>
> **Before/after: `docs/img/mipaniso/his_t5970_sliver_legacy_vs_aniso.png`** (3×) and
> `docs/img/mipaniso/his_t5970_jamb_wide_legacy_vs_aniso.png`.
>
> ### TWO CORRECTIONS TO THE `--mip_aniso` DOC, BOTH MEASURED
>
> 1. **"greets' corridor walls run ~16:1" is wrong** as an area-weighted claim. True
>    area-weighted anisotropy is **1.80:1** at t=5970, **1.91:1** at p1 t=5743,
>    **1.84:1** at t=5799. Only 18.3 % of covered area is ≥2:1 and 9.2 % ≥4:1.
>    The first cut of my own census *did* print 16.6 — from one edge-on sliver with
>    σmin→0 owning the mean. That was a clamping bug in the instrument, and it is
>    almost certainly where the original 16:1 came from too.
> 2. **The blurry neighbour is not a mip problem at all.** The wall the eye reads as
>    "coarse" next to the striping one is at **0.22 texels per pixel — magnified 4.5×**,
>    at level 0 under both metrics. No mip metric can touch it; that is point
>    magnification, and only `--texture_filter>=1` (bilinear) would.
>    **Half the reported discontinuity is therefore out of scope for this fix.**
>
> ### WHAT THE FLAG DOES FRAME-WIDE
>
> Undersampled area (σmax > 2^level) falls **71.8 % → 44.0 %** at t=5970 (82.4 → 53.6 at
> p1, 76.1 → 57.0 at t=5799), and area undersampled by **more than 2×** falls
> **6.3 % → 0.0 %** at every pose measured. Level-0 area 82.6 % → 79.1 %; max level 8
> either way, so **the G-buffer's `mip:4` field does not overflow**.
>
> ### GROUND TRUTH, BECAUSE "SHARPER" AND "MORE CORRECT" ARE DIFFERENT CLAIMS
>
> Scored against a **4× supersampled capture** (`--snapshot_ss=4`, box-downsampled):
>
> | region | legacy RMSE | aniso RMSE |
> |---|---|---|
> | t=5970, the striping sliver | 9.300 | **8.726** (−6.2 %) |
> | t=5970, whole frame | 6.070 | **6.042** |
> | **p1 t=5743, whole frame** | **7.070** | 7.156 (+1.2 %) |
>
> **GpuBench at the same pose agrees with the supersampled reference and not with the
> legacy grain** — four-way strip (legacy | aniso | 4× reference | GpuBench):
> `docs/img/mipaniso/his_t5970_four_way_legacy_aniso_ss4x_gpubench.png`. Two
> independent second opinions, same direction. Corroboration, not authority.
>
> ### THE COST, WHICH IS WHY THE DEFAULT STAYS OFF
>
> All 18 poses of `docs/greets_review_poses.txt`, both arms: **0.5 %–7.0 % of pixels
> change** (p12/p13 at 1 LSB only). Frame-wide gradient energy falls **0–1.1 %**. But
> the worst 128 px window in the battery loses **23 %** of its gradient energy, and it
> is a real loss: distant low-contrast stonework flattens —
> `docs/img/mipaniso/p1_t5743_BLURCOST_legacy_vs_aniso.png` (5×). That is the same pose
> whose whole-frame ground-truth error gets *worse*. Other pairs:
> `p1_t5743_floor_*`, `p2_t5773_wall_*`, `p17_t5967_floor_jamb_*`,
> `t5799_longwall_*`, all in `docs/img/mipaniso/`.
>
> **A partial dial would not help and is not worth building.** `lod = lodGeo +
> k·(lodMax−lodGeo)` only flips the reported face once `k·1.29 ≥ 1.0`, i.e. **k ≥ 0.78**
> — any setting mild enough to protect the distant stonework leaves his defect exactly
> as it was. Closed analytically from the census number, no arm built.
>
> ### TEMPORAL: IT DOES NOT FLICKER, IT FLICKERS LESS
>
> 13-step dolly along the real camera path (t=5967→5987, **identical poses in both
> arms**, so motion is normalised out). Consecutive-frame mean |d| in the jamb band
> **14.048 → 13.749 (−2.1 %)**, px>20 **23.26 % → 22.46 % (−3.5 %)**; bit-identical on
> the walls it does not touch. `--mip_hysteresis` still engages on top of it (0.38 % of
> pixels under the legacy metric, 0.65 % under this one) — measured through `--repro`
> with 16 frames of real history, because `--snapshot` cannot express `Face::LastMip`
> at all. Under `--repro` the flag's effect is 4.97 % vs the cold snapshot's 5.04 %, so
> **none of this is a single-cold-tick artifact**.
>
> ### PERF: IT COSTS, IT DOES NOT PAY FOR ITSELF
>
> Interleaved ×6 at t=5970. The box was at **load 18**, so the wall column is not
> resolvable below ~1 ms and the **hardware counters** are the measurement:
>
> | | legacy | aniso |
> |---|---|---|
> | renderFrame Ginstr/f | 4.287 | 4.292 (**+0.12 %**) |
> | gbuffer Ginstr/f | 0.655 | 0.660 (**+0.76 %**) |
>
> The whole delta lands in the G-buffer phase, which is where `MiplevelClipper` runs.
> **Mechanism, both halves:** the 5 extra MACs per fan triangle, *plus* the poly-SPLIT
> branch firing more often — **166 → 236 invocations** at t=5970, because a higher LOD
> puts more faces across a level boundary. There is **no** measurable texture-cache win
> from the coarser mips; "coarser must be cheaper" did not happen.
>
> ### KNOCK-ONS CHECKED
>
> * **G-buffer `mip:4`** — max level 8 both arms, clamp to `numMipmaps-1` unchanged.
> * **POM is not entangled with it.** The height march takes the per-face albedo mip
>   when `--pom_height_mip` is −1, so the metric *does* feed POM's UV offset — but
>   pinning the height mip to 1 leaves this flag's effect essentially unchanged
>   (5.04 % of pixels vs 4.91 %). The change is albedo, not re-offset relief. For scale:
>   `--pom_height_mip=1` **on its own** moves 5.07 % of pixels with 38 280 above 12/255,
>   nearly 3× this flag's >12 count. That knob is the bigger lever and is untouched here.
> * **`--mip_hysteresis`** — engages under both metrics, unchanged code path.
>
> ### A BUG FIXED ON THE WAY: `--mip_stats` PRINTED NOTHING
>
> `--mip_stats` has been **silently dead under `--snapshot`**. `TlsHolder::~TlsHolder`
> merged `polysRendered` and `fillerPixelcount` on the way out but **dropped the mip
> histogram**, then unregistered — so the tile workers exiting during shutdown emptied
> the registry, and the atexit report bailed on `totFaces == 0`. Fixed by merging the
> histogram and the mip counters in the dtor exactly as `Flush` would (Flush zeroes what
> it takes, so it cannot double-count). Every `[MIP]` number in this block, and the
> ability to re-check the `--mips` doc's own figures, depends on that fix.
>
> ### GATES — ALL HOLD, FLAGS DEFAULT OFF
>
> greets `778fa6acd85a69cf241babefcdaf598e` **3/3**, fountain
> `8db68ccb59416e9a44037e9f387b7bd9` (run 1 discarded, cold-cache as documented),
> `render_gate.sh` **ALL FOUR PASS** (`mirrortest 4ac809e5`, `rttslot 826c09e6`,
> `conetest b41894f9`, `halotest 166fa25a`). Differential byte-null proof: greets
> t=5970 rendered by the **pre-instrument** binary and by the final one are the same
> md5 `a1399305b45d0b869939dbba3a318abd`. city cold-bakes its own cube in a fresh
> worktree (`cache/city_envmap_cube_c0c60ff9.bin`) and reads
> `3f8948232c192a979ffe7f76c4b387ab` stable 2/2 — correct-for-that-cube, not drift,
> per the standing city-cube trap.
>
> ### THE CALL IS YOURS
>
> **`--mip_aniso` stays default 0.** Max-axis *is* the correct metric for a point
> sampler, every objective score in the defect region agrees, and the GPU and a 4×
> reference both back it — but there is no aniso-tap filler to win the detail back, so
> the distant-surface softening is a look decision, not a correctness one, and the
> battery does show it. One token to look at it:
>
> ```sh
> cd Runtime && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
>   FDS_GREETS_CAM="18.8969765,3.21025538,-58.888485,-0.896694958,-0.0735020638,0.436503887" \
>   ./DEMO --deferred --mip_aniso --snapshot=greets@t=5970 --out=/tmp/x
> ```
>
> **If you countersign the flip, the pin moves are already measured** — re-verify them
> in the flipping commit rather than re-deriving:
>
> | gate | today (`--mip_aniso` off) | under `--mip_aniso` |
> |---|---|---|
> | greets t=1588 | `778fa6acd85a69cf241babefcdaf598e` 3/3 | **`7ac564bc7b3a6c2e0c88975fc9949258`** 3/3 |
> | fountain t=2500 | `8db68ccb59416e9a44037e9f387b7bd9` 2/2 | **`5ac2f2c8c9cc15818fecdb4aa866ff9c`** 3/3 |
> | city t=1961 (this worktree's cold cube) | `3f8948232c192a979ffe7f76c4b387ab` 2/2 | **`ee68cb19b0cc20c3cf53b70690b3c052`** 2/2 |
> | `render_gate.sh` | ALL FOUR PASS | **ALL FOUR PASS, same hashes** |
>
> The render_gate rows do **not** move — those scenes carry no minified textured
> geometry, exactly as the `--mips` doc says; run with `FDS_MIP_ANISO=1` to confirm.
> Note the fountain row needed 3 consecutive runs: the first fountain run after a
> different scene has run returns a cold-cache value (`ac38f73d…` here), the same
> discard-run-1 class already documented under the city cube.
>
> **STILL OPEN, and it is the other half of his report:** the neighbouring wall's
> blockiness is 4.5× point magnification. The fix for that is bilinear
> (`--texture_filter>=1`), which is default 0 and is a separate, larger decision.


> ## 2026-08-13d — ROUND 5, THE SPELLING SWEEP: −1.8 % MORE CYCLES, AND THE METRIC ITSELF GETS CORRECTED — OPS ONLY COUNT WHEN THEY ARE ON THE DEPENDENCY CHAIN
>
> Round 4 left a metric — **vector-ALU op count** — and one example of it.
> Round 5 applied it *systematically*: census every op the hot loop emits,
> bucket it by the pipe it issues on, map each back to the intrinsic that
> produced it, and hunt every 2-for-1. Two changes landed, two were killed,
> and **the sweep is now dry** — but the reason it dried is the round's real
> result.
>
> ### THE CENSUS (single-arm control, the per-spot loop that runs 3.2 M×/frame)
>
> 484 vector-ALU ops → **461** after this round (−4.8 %). Composition:
> arith 246→236, cmp 50, logic 48, blend 36, **mov 49→42**, dup 23→21,
> const 17→13, permute 6, plus 12 `fdiv`/`fsqrt` and 131→124 vector memory ops
> that are NOT on the metric. New instruments in `scratchpad/`:
> `cone_census.py` (bucket + opcode histogram), `cone_loops.py` (loop-nest
> census — separates the per-spot loop from the per-tile prologue),
> `cone_srcmap.py` (op → `.loc` attribution; thin-LTO strips the line map and
> `dsymutil` cannot recover it, so it reads a `clang -S -g` listing instead),
> `cone_census_ladder.sh`.
>
> ### THE `mov.16b` QUESTION, ANSWERED
>
> 139 of 4487 statically, 44 in the hot loop. **Not** simde shuffling 128-bit
> halves — the solve emits ZERO cross-half permutes. Three origins:
> (1) **blend-operand copies** — `BSL` destroys the mask, `BIT`/`BIF` destroy a
> data operand, so a select whose mask *and* both data operands stay live must
> copy; (2) **FMA-accumulator copies** — an `__m256` FMA with a broadcast addend
> needs it in BOTH halves' destination registers, so `dup` + `mov.16b`; (3)
> allocator/phi copies. Only (2) responds to respelling, and the fold below
> removes two.
>
> ### THE NAMED SIMDE SUSPECTS: ALL CHECKED IN THE DISASSEMBLY, ALL ALREADY CLEAN
>
> `_mm256_blendv_ps` → one `bsl`/`bit`/`bif` (only **3** `cmlt.4s` mask
> normalisations survive against 36 blends). Unordered `_CMP_NLT_UQ`/`_NLE_UQ`
> → the NaN-correct negation is absorbed into `BIC`/`ORN`; the hot loop has
> **exactly one `mvn.16b`**, so round 3's careful predicates are FREE.
> `and`/`or` chains → already `bic`/`orn`. `set1` broadcasts → **54 by-element
> (indexed) `fmul`/`fmla`/`fmls`** already emitted; the ~21 surviving `dup.4s`
> feed `fadd`/`fsub`/`fcmp`/`fdiv`/`bsl`, which arm64 has **no** by-element
> encoding for. Compare-against-zero → 20 immediate-`#0.0` forms.
> `andnot(-0.0,x)` → `fabs.4s`. Conversions: 4 `ucvtf.4s`. Horizontal
> reductions: exactly one `faddp.2s` in the whole kernel.
>
> ### LANDED 1 — THE NEWTON STEP IS ONE INSTRUCTION (and is worth <0.5 %)
>
> arm64 has `FRECPS` (= 2 − Vn·Vm) and `FRSQRTS` (= (3 − Vn·Vm)/2), fused, one
> rounding. The kernel wrote both longhand and emitted **zero `frsqrts.4s`** at
> three sites. `rcp_step_x8`/`rsqrt_step_x8` route them native: `frsqrts.4s`
> 0→10, `frecps.4s` 10→12, kernel 4487→4454, hot loop **484→469 (−3.1 %)**.
> **Bit-exact over 61.4 M inputs** (zero differences for x > 2.4e-38; below that
> the LONGHAND form is the wrong one, `0.5*x` goes subnormal). Instructions and
> wall reproduce (−1.5 %, −1.2 %) but **cycles do not** (0.542→0.542, then
> 0.548→0.534) — kept because it is free, simpler and more accurate, not because
> it is a win.
>
> ### LANDED 2 — `Y·Py + Pz` IS A SCALAR: −1.8 % CYCLES, REPRODUCED TWICE
>
> `VP = X·Px + Y·Py + Pz`, and **`Y·Py + Pz` is a per-(row × spot) scalar** — the
> whole tail of the dot product is one broadcast. The port spelled it as the
> scalar arm does, at **seven** vector-ALU ops (`dup(YPy)`, a `mov.16b` because
> both halves need the FMA accumulator, two `fmla`, `dup(Pz)`, two `fadd`);
> folded it is **four**. Two sites. Plus `b = 2·(c2·VP − DP·DV)` folding the
> doubling into both broadcasts (exact, −2 ops). Hot loop 469 → **461**.
>
> | cones @ city t=1961 | wall | `Ginstr/f` | `Gcyc/f` |
> |---|--:|--:|--:|
> | parent `4e643a25` | 16.139 ms | 2.304 | 0.545 |
> | **ships** | **15.802 ms** | **2.239** | **0.535** |
>
> **−1.8 % cyc / −2.1 % wall / −2.8 % instr**, the cycle figure reproduced in two
> independent interleaved sessions. Sweep: t=400 −3.5 %/−6.0 %, t=900
> −1.8 %/−2.3 %, t=1400 −0.9 %/−2.2 %, t=2400 −3.0 %/−3.0 %. Greets −2.1 % cyc.
>
> ### BYTES — JUDGE CALL, LANDED DEFAULT-ON, PIN MOVED
>
> > **COUNTERSIGNED 2026-08-15 by the user — "6 seems ok".** This was item 6 of
> > the round-up put to him (the two outstanding pin-move judge calls: this one,
> > chase/greets at ≤ 2/255, and `593f7842`'s water caustic at 7 px/1 LSB). The
> > pin values below are therefore ADOPTED, not pending; the fold stays
> > default-on and no revert is held open.
>
> The VP/DV fold is NOT bit-exact (the scalar arm rounds `Y·Py` before adding
> `Pz`; this rounds the fused sum once). Measured: **t=900, t=1400, t=2400
> byte-identical**; **t=400 3 px of 2 073 600, max |Δ| 2/255**; **t=1961 2 px,
> max |Δ| 1/255** — `(1852,429) (150,93,87)→(151,94,87)` and `(1875,435)
> (104,89,209)→(103,89,209)`. Z-buffer identical everywhere; greets and fountain
> byte-identical. Artifact battery clean: pixels are ISOLATED (no striping/banding
> possible), NaN excluded by construction at max |Δ| = 2 LSB (a NaN blows out a
> whole 8-lane batch), no temporal structure when 3 of 5 poses are identical.
> `render_gate` **ALL FOUR rows PASS** incl. `conetest b41894f9` BIT-IDENTICAL.
> Images: `docs/img/conevec/r5_city_t1961_crop_before_after.png`,
> `r5_city_t1961_diff.png`, `r5_city_t400_diff.png`, `r5_city_t1961_after.png`.
>
> **PIN MOVED: city `3cbe42b166847e40f7071eedb48d613c` →
> `3f8948232c192a979ffe7f76c4b387ab`** (2/2 stable, verified again on the
> rebased tree). greets `778fa6acd85a69cf241babefcdaf598e` and fountain
> `8db68ccb59416e9a44037e9f387b7bd9` **unmoved**, 2/2 each.
>
> ### KILLED, WITH NUMBERS
>
> * **Hoisting `1/uV` out of the spot loop** — `uV` is batch-invariant and the
>   solve looks like it divides per spot. **LLVM's LICM already hoists it**: the
>   parent's control divides at batch level (`0x1001e7edc`), spills and reloads.
>   Built; disassembly unchanged, 10 `fdiv.4s` before and after. Killed on the
>   census, never benched.
> * **Collapsing the `zLo`/`zHi` select cascade** — `zLoP` re-selects on `mDisc`
>   what `zLoRt` selected on `mFwd` against the same fallback, so they fold to
>   one blend on `mDisc & mFwd`; bitwise identical, and it takes `zLo`/`zHi` off
>   the sqrt through TWO serial selects instead of three. Built deliberately as
>   a test that trades op count AGAINST chain length: hot loop **461 → 469
>   (+8)** — two new masks cost two logic ops and, through register pressure,
>   **four more `mov.16b`** — and it measured **+0.6 % cyc / +1.2 % wall**.
>   Reverted.
>
> ### THE FINDING — THE METRIC NEEDED A QUALIFIER
>
> | change | Δ vector-ALU ops | on the chain? | Δ cycles |
> |---|--:|---|--:|
> | r4 `NEONMINMAX` | −2 × 19 sites | **yes** | **−4.5 %** |
> | r4b algebra folds | −18 static | **yes** | **−2.0 %** |
> | r5 VP/DV fold | −8 (−1.7 %) | **yes** | **−1.8 %** |
> | r5 Newton step | −15 (−3.1 %) | **no** | **~0** |
> | r5 select collapse | **+8** | shortens by one link | **+0.6 %** |
>
> **The two biggest cycle wins removed the fewest ops.** Round 4's 81 %-of-issue-
> ceiling number is real but not the whole constraint: the solve's long pole is
> `fma→fma→fma→fsqrt→fsub→fmul→fmin/fmax→bsl→bsl→bsl→fmax→fmax`, ~13 links with
> a ~12-cycle `fsqrt` in the middle, which accounts for most of the ~63
> cycles/pair on its own. Ops that dual-issue into the slack around it are free
> to remove AND free to keep — round 3's finding again, now with a mechanism.
> And the last row stings: **shortening the chain does not automatically pay
> either**, because the register file pushes back.
>
> **CONES: I call this done as a spelling problem.** What is left in the hot
> loop is 236 arith ops of real math, 134 ops of branchless mask/select control
> flow (29 % — that IS the 8-wide algorithm), and 76 ops of copies, broadcasts
> and constants that are off-chain by construction and therefore worth ~nothing.
> The next win has to shorten the solve's dependency chain WITHOUT adding live
> values, or do less work — and culling is already closed at ~3 % (round 4).
> Full worked example, census tables and disassembly: **`docs/HW_PROFILING.md`
> section 13**.


> ## 2026-08-13c — THE REGISTER-PRESSURE QUESTION IS ANSWERED NO, WITH THE ARM THAT PROVES IT: RELIEVING IT COSTS +7.5 % CYCLES
>
> His question was **"any way to rewrite this while relieving register
> pressure?"** The premise is exact: arm64 has no 256-bit unit, simde lowers
> every `__m256` op to two 128-bit NEON ops, so a live `__m256` costs **two** of
> 32 `v` registers, the file is effectively 16 deep, the cone solve holds ~30
> live values, and it spills. All true — and the rewrite makes the pass slower.
>
> **`FDS_CONE_W4` (in-tree, default 0, emits nothing)** spells the solve as two
> 4-wide passes. Identical NEON op count by construction, so only the live set
> moves. Single-arm control builds (`-DFDS_CONE_FORCE=1 -DFDS_CONE_HOTONLY=1`,
> no dual-arm tax), city t=1961, interleaved min-of-6:
>
> | arm | cones wall | `Gcyc/f` | stack `ldr q`/`str q` |
> |---|---|---|---|
> | `__m256`, as shipped | 16.673 ms | 0.576 | 89 / 79 |
> | W4, half loop **unrolled** | 16.618 ms | 0.580 (+0.7 %) | 91 / 82 |
> | W4, half loop **rolled** | 18.083 ms | **0.619 (+7.5 %)** | **72 / 70** |
>
> Rolling is the only spelling that actually halves the live set — left alone the
> compiler unrolls a trip count of 2 and schedules both halves together, which
> restores the 8-wide live set exactly (spills go *up*). The build that gets the
> pressure relief pays **+7.5 % cycles / +8.5 % wall** for it. **The two halves
> of an `__m256` op are independent NEON chains: the spelling is already
> unroll-and-jam by 2, and taking it apart costs more than the spills it saves.**
> Flagged at runtime instead it read −0.1 % instr / +1.2 % cyc / +2.6 % wall, and
> merely compiling the arm in taxed the OFF path +3.2 % instructions — hence
> compile-time, not a FeatureFlag.
>
> ### WHY, IN ONE NUMBER: 81 % OF THIS CORE'S NEON ALU ISSUE CEILING
>
> Round 2's ladder had never been read on the **cycle** column. Read: the solve
> is **0.250 of the pass's 0.585 `Gcyc/f` — 42.7 %**, its largest bucket. Its
> disassembly is 251 instructions of which **202 are vector ALU**; the DIAG
> census gives 3.20 M (batch × spot) pairs; that is **~63 cycles per pair against
> a ~51-cycle vector-ALU-port floor** on the M2 Max's 4 NEON pipes.
>
> That one number explains the whole campaign's recent results: pressure relief
> cannot pay because the ALU port, not the spill slot, is the constraint; round
> 3's "deleting 10 % of instructions moved cycles by zero" was scalar and branch
> work issuing on other pipes into slack; and **more** ILP is capped at −19 % of
> the solve (−8 % of the pass). **The metric is VECTOR-ALU OP COUNT** — not
> instructions, not registers.
>
> ### WHAT THAT METRIC FOUND, AND IT SHIPS: −4.5 % CYCLES, BIT-EXACT
>
> The shipping cone kernel emitted **zero `fmin.4s`/`fmax.4s`** at **19 min/max
> sites**. Two reasons: the solve and the dz/fade loop spell `std::min`/`max` as
> cmp+blend (7e34645 needed bit-exactness; NEON `FMIN` resolves NaN and −0 the
> other way from `FCSEL`), and every `_mm256_max_ps` already in the body ALSO
> lowers to cmp+blend because `SIMDE_FAST_NANS` is undefined here and simde's
> NaN-correct fallback is `m = a<b; (a&m)|(b&~m)`. The intrinsic that looks like
> one op is two. **`FDS_CONE_NEONMINMAX` (default 1) routes all 19 through
> `vmaxq_f32`/`vminq_f32`.**
>
> | | cones wall_min | `Ginstr/f` | `Gcyc/f` | IPC |
> |---|---|---|---|---|
> | parent `67441d86` | 16.922 ms | 2.388 | 0.584 | 4.067 |
> | new tree, `NEONMINMAX=0` | 16.924 ms | 2.387 | 0.581 | 4.074 |
> | **new tree, default (ships)** | **16.285 ms** | **2.347** | **0.558** | **4.176** |
>
> **−4.5 % cycles, −3.8 % wall (−0.64 ms), −1.7 % instructions**; renderFrame
> 57.143 → 56.423 ms and the −0.72 ms frame saving matches the −0.64 ms cones
> saving, which is the attribution check. The `NEONMINMAX=0` row is the control
> proving the compiled-out W4 arm costs the shipping binary nothing. Sweep:
> −3.2 % to −4.7 % cycles at every city pose. **Greets improves too** (the body's
> max/min sites are hot on the segmented branch): −3.2 % cyc / −3.5 % wall,
> 7.595 → 7.330 ms.
>
> **Written as a judge call under the standing byte rule, and it turned out
> BIT-EXACT.** The NaN/±0 tie-break never materialises: city
> `3cbe42b166847e40f7071eedb48d613c`, greets `778fa6acd85a69cf241babefcdaf598e`,
> fountain `8db68ccb59416e9a44037e9f387b7bd9` all **3/3** (fountain 2/2), and
> `render_gate` **ALL FOUR rows PASS** — `mirrortest 4ac809e5`, `rttslot
> 826c09e6`, `conetest b41894f9` (direct coverage of this kernel), `halotest
> 166fa25a`.
>
> ### LEVERS PRICED AND CLOSED
>
> New DIAG counter `sphdead`: of 3 204 900 (batch × spot) pairs, 39.9 % produce
> zero alive lanes but only **7.7 % lose all eight at the range sphere** — the
> rest die on the cone-interval and chord tests, which have no cheap conservative
> screen-space form. That caps every sphere-based cull (finer tiles, per-row
> X-intervals, the reverted per-batch rect cull, `FDS_CONE_SOLVE_EARLYOUT`) at
> ~3 % of the pass, and independently reproduces a16567b's tile result and round
> 1's early-out rejection. Unroll-and-jam beyond the free 2× is capped at ~8 % of
> the pass. Outlining the cold arms was already priced at zero cycles by round
> 3's `HOTONLY`. Full worked example, tables and disassembly: **`docs/HW_PROFILING.md`
> section 12**.
>
> ### ROUND 4b — THE SAME METRIC AGAIN: −2.0 % MORE CYCLES OUT OF THE SOLVE'S ALGEBRA
>
> Three spellings in the solve cost an op they need not, all bit-exact to fold:
> `fma(a,b,NEG(mul(set1(k),v)))` → broadcast `-k` and drop the `fneg.4s`
> (`fl((-k)*v) == -fl(k*v)` exactly — IEEE negation is exact, rounding is
> symmetric); `mul(set1(cq), mul(a, set1(-4)))` → `mul(set1(cq*-4), a)` (both
> inner products are power-of-two scalings, hence exact, so either spelling
> rounds the same real product once); and `or(mFwd, mBwd)` IS the `mDVBig`
> the apex cut computes twenty lines later — same mask for every input, NaN and
> ±0 included — so hoisting it retires a compare and an or. 18 instructions of
> 4505. **cones 0.559/0.557 → 0.548/0.543 `Gcyc/f` (−2.0 %, −2.5 %), 16.10/16.07
> → 15.89/15.89 ms**, two independent min-of-6 sessions with the arm order
> reversed between them. Greets held. Pins 3/3 / 3/3 / 2/2, `render_gate` four
> rows PASS.
>
> **Round 4 cumulative, measured directly (both binaries interleaved in one
> min-of-6, not composed across sessions): cones 0.582 → 0.553 `Gcyc/f`
> (−5.0 %), 16.953 → 15.930 ms (−6.0 %), 2.388 → 2.302 `Ginstr/f` (−3.6 %),
> IPC 4.073 → 4.157; renderFrame 57.392 → 55.976 ms. All of it bit-exact.**


> ## 2026-08-13b — THE RTT GATE HOLE IS CLOSED, AND THE ROW IS PROVED IN BOTH DIRECTIONS: `render_gate` NOW FAILS ON `00d28a8b`
>
> The entry below leaves one thing undone: a regression that changed 98.9 % of an
> RTT slot was invisible to every standing gate, and the fix shipped on
> hand-run byte-identity rather than on anything that would fire again. That hole
> is now a committed row — `render_gate.sh`'s fourth, **`rttslot`
> `826c09e63217e778cfcef70fe0167279`**.
>
> ```
> FDS_MIRRORTEST_MULTI_DUMP=1 FDS_MIRROR_RTT_DUMP=1 \
>   ./DEMO --scene-mirrortest --mirror_rtt --shard_deferred --hdr
> md5 of the 4 /tmp/rtt_*.ppm slot dumps
> ```
>
> ### IT IS BUILT ON `mirrortest`, NOT ON THE BACKLOG'S GREETS POSE — AND THAT IS DELIBERATE
>
> The backlog spec'd greets t=3122 `--hdr --deferred` with
> `FDS_MIRROR_RTT_DUMP=1`. That recipe is the one that *found* the bug and it
> stays the out-of-band check, but it cannot be a `render_gate` row: greets is
> excluded from that script by its own header **because the greets pin keys on
> the user's UNCOMMITTED authoring files** (`GREETS.FLD`, `Hull.lwo` — both dirty
> in the main tree right now). A committed hash of a scene the user edits daily
> is a row that goes red on authoring, i.e. a row that gets ignored.
>
> `mirrortest` needs no such compromise: its two mirrors face each other and
> `MirrorTestDriver.cpp:269` already calls `PrepareSecondOrderMirrorRtt`, so the
> instant `mirror_rtt` is on it prepares **2 order-2 slots** (`m1→m2` 512×512,
> `m2→m1`) and bakes 4 dumps across its 8 poses. Same code path, same kernel,
> committed scene.
>
> ### ALL THREE FLAGS ARE LOAD-BEARING, EACH PROVED BY A CONTROL THAT DOES NOT DISCRIMINATE
>
> | arm on `mirrortest` | slot hash, `6656300b` | slot hash, `00d28a8b` | discriminates? |
> |---|---|---|---|
> | no `--mirror_rtt` (today's row) | `d41d8cd9…` (md5 of nothing — 0 slots) | `d41d8cd9…` | **no** |
> | `--mirror_rtt --shard_deferred`, no `--hdr` | `09c9d4d8…` | `09c9d4d8…` | **no** |
> | `--mirror_rtt --hdr`, no `--shard_deferred` (forward bake) | `a48afe1b…` | `a48afe1b…` | **no** |
> | **`--mirror_rtt --shard_deferred --hdr`** | **`826c09e6…`** | **`2ecd5e81…`** | **YES** |
>
> Drop any one flag and the row is vacuous. `mirror_rtt` gates slot creation,
> `shard_deferred` is what routes the bake through the deferred kernel (`ov` is
> only constructed there — `GreetsMirror.cpp:3236`), `hdr` is what makes
> `ctx.hdrBuf` matter at all.
>
> ### BOTH DIRECTIONS, MEASURED
>
> * **PASS at HEAD (`6656300b`): 3/3** full-gate runs, `ALL PASS` — mirrortest
>   `4ac809e5…`, rttslot `826c09e6…`, conetest `b41894f9…`, halotest `166fa25a…`.
> * **FAIL on the broken binary (`00d28a8b`)**, same script, second worktree +
>   second build dir, 2/2: `FAIL rttslot got 2ecd5e81… want 826c09e6…`, `rc=1`,
>   **while the other three rows PASS unchanged there** — which is the gate hole
>   restated as a passing test suite, and the reason this row had to exist.
> * **Determinism 5/5** on HEAD before the hash was recorded (`826c09e6…` every
>   run, 4 files every run); 3/3 on the broken binary too, so the FAIL is a
>   stable FAIL, not a flake.
> * Regression signature on the slot dumps, matching the greets one: **99.95 % of
>   the covered texels change** (46 588 of 46 610 on `rtt_m1_m2_0`), mean |Δ|
>   **28.5**/channel, luma over the changed texels **157.7 → 129.9 (−27.8)** —
>   against greets t=3122's 98.9 %, 26.7, 126.68 → 105.64 (−21.0). Same mechanism,
>   same magnitude.
>
> ### WHAT THE ROW CERTIFIES, AND WHAT IT STILL DOES NOT
>
> **Certifies:** an order-2 RTT slot still bakes through the deferred kernel with
> a live HDR buffer, at mirrortest's two facing mirrors, at the adaptive
> resolutions its poses pick — and it fails loudly if the RTT stops producing
> slots at all (0 files hashes to `d41d8cd9…`, not to the baseline).
>
> **Does not:** greets' own slot set (7–8 slots vs mirrortest's 2) — that remains
> out-of-band via the pin recipe, because of the uncommitted-authoring problem
> above; the FIRST-order RTT panel path (`greets_mirror_rtt_min_area` keeps it
> empty in both scenes, so nothing here would notice it breaking); the panel
> composite as it reaches the main frame — **measured: under `--hdr` the
> mirrortest FRAME is byte-identical between the forward and the deferred bake
> (`a5bb109c…` both), so the frame is not a valid surface for slot content here
> and the SLOT is what is gated**; and any RTT change invisible at these two
> mirrors, e.g. an adaptive-res policy change that only moves greets' sizes. The
> flip side of that last one: the row IS sensitive to the sizes mirrortest picks,
> so an intentional sizing change needs `--update`, not a shrug.
>
> One incidental hardening: the script now exports `SDL_AUDIODRIVER=dummy`
> alongside `SDL_VIDEODRIVER=dummy` (no device grab from a bisect loop). Measured
> not to move any baseline — mirrortest still reproduces `4ac809e5…`.

> ## 2026-08-13 — THE RTT REGRESSION `00d28a8b` SHIPPED IS REAL AND IS NOW PROVED FIXED BY BYTE-IDENTITY WITH THE PRE-RESTRUCTURE BASELINE
>
> `283b46ca` was written as a self-review catch and committed but never pushed
> (its author stopped mid-task). **It is correct, and this is the independent
> verification it never got.** Three binaries from three worktrees, one
> pose, dummy drivers throughout:
>
> | binary | greets t=3122 `--hdr --deferred` frame | RTT slot `m4→m1` surface |
> |---|---|---|
> | `5adcae12` — *before* the whole restructure | `4abe5214e04a215d8b12b0438c1a0dc6` | `5199d3d1cd84d7d1b54cedb4ccb6afcb` |
> | `00d28a8b` — origin/fog-wt tip, **broken** | `c7ef96f690126cca2c5297812c52c3bd` | `ab17ac64d35d268e1457c17793accba3` |
> | `283b46ca` — **the fix** | `4abe5214e04a215d8b12b0438c1a0dc6` | `5199d3d1cd84d7d1b54cedb4ccb6afcb` |
>
> **The fix is byte-identical to the pre-restructure baseline** — not "looks
> right", not "pins still pass": the same bytes `5adcae12` produced. That is the
> strongest statement available about a restore-the-old-predicate change, and it
> is what a self-review catch is owed before it ships.
>
> **What the regression cost, measured.** The RTT slot surface: **16 207 of
> 16 384 px differ (98.9 %)**, mean |Δ| 26.7, max 87, mean luma **126.68 → 105.64
> (−21.0)**. On the 1920×1080 frame the panel is a 24×60 wedge: 785 px, max Δ 76,
> luma over the changed pixels **118.12 → 105.78 (−12.3)**. Image:
> `docs/img/fogwt/rtthdr_t3122_regression.png`.
>
> **Why the mechanism produces exactly that.** With `ov` non-null and `ov->hdr`
> null the kernel's `hdrWrite` went false, so nothing wrote radiance and every
> pixel kept `h[3]==0`. `Hdr_ActivateNoFog()` then lifted the *whole* 8-bit
> LDR-combined RTT surface into `g_hdrBuf` and `Render_TonemapToVPage()`
> tonemapped it back down — an ACES curve applied to pixels that had never been
> linear radiance. The slot did not lose its bracket; it lost the only pass that
> made the bracket meaningful.
>
> **THE GATES WERE BLIND, AND NOW IT IS MEASURED, NOT ASSUMED.** `283b46ca`
> reasoned that `mirrortest` misses this because it runs without `--hdr`. It is
> worse than that: `mirrortest` is byte-identical on all three binaries **with
> `--hdr` too** (`3a91879af8856a4d6a4f9703921455b4` on each) — `--scene-mirrortest`
> never turns on `mirror_rtt` (default 0; only `GREETS.CPP`'s `setDefault` and the
> editor do), so `render_gate` has *no* coverage of the order-2 RTT path at all,
> HDR or not. The greets pin `778fa6ac…` also holds **3/3 on the broken binary**.
> A gate that cannot fail is not a gate: the RTT's first real coverage is the
> `4abe5214`/`5199d3d1` pair in the table above.
>
> ### THE "TONEMAPS THE SAME PIXELS TWICE" TITLE IS A DIAGNOSIS, NOT A SHIPPED DEFECT
>
> Re-measured independently at the bracket, panel window derived the same way
> (mirror-on/off change mask → bbox **1267×769**, against the recorded 1258×767;
> the intact-mirror frame reads 73.24 against the recorded 73.07, so the window
> is the same window):
>
> | arm | panel-window luma | vs reference |
> |---|--:|--:|
> | pre-break, intact half-silvered mirror | 73.24 | |
> | **reference** — main deferred pass from the shard's own reflected eye | **73.70** | — |
> | **shipping defaults (`--shard_hdr` off) — what HEAD renders** | **86.58** | **+12.88** |
> | `--shard_hdr` on | 128.83 | +55.14 |
> | shipping, `--hdr_exposure=2.0` | 129.81 | +56.11 |
>
> **At HEAD's defaults the shard atlas is tonemapped exactly ONCE**, by the main
> frame that samples it as a texel. The doubling is what happens *when you turn
> `--shard_hdr` on*, and the flag ships 0 for that reason — `on`-at-exposure-1.0
> (128.83) landing on `off`-at-exposure-2.0 (129.81) is the doubling's signature,
> reproduced. **No second bug to fix.** The +12.88 residual is the ORIGINAL
> `ddb1d15` item and is still open; the remedy is an HDR atlas
> (`hdrRefl`-shaped), not a tonemapped 8-bit cell.
>
> ### THE HAZARD THE FIX RE-ADMITS, ANALYSED AND DISMISSED WITH THE ORDERING
>
> The fix restores `Hdr_WritableFor(XRes,YRes)` as the fallback for override
> passes — the very "the global happens to be sized like me" predicate `00d28a8b`
> set out to kill. It is safe, and here is why it is not merely "it was like that
> before": a **64² RTT slot is reachable** (`mirror_rtt_adaptive` is default **1**
> and both sizing sites clamp to a floor of 64), so `g_hdrBuf` really can be sized
> 64² — the shard bake's own dims. If a shard bake then ran while that size stood,
> all 12 workers would take `g_hdrBuf.data()` and race on one buffer.
> **The frame order forecloses it**: the shard bake is `GREETS.CPP:3808`, the RTT
> is `GREETS.CPP:3853` (after it), and `renderFrame`'s `Hdr_BeginFrame()`
> (`RENDER.CPP:659`, main dims) runs after *both* — so a shard bake always
> observes `g_hdrBuf` at 1920×1080 and lands on `nullptr`. `XRes,YRes` here are
> the **pass's** dims (`const int32_t XRes = ov ? ov->xres : ::XRes;`), so the
> predicate is exactly the one the kernels used to evaluate. Recorded because the
> guarantee is an *ordering*, and ordering is what changes without anyone noticing.
>
> ### GATES, ALL RE-RUN ON THE FIXED BINARY
>
> greets `778fa6acd85a69cf241babefcdaf598e` **3/3**, fountain
> `8db68ccb59416e9a44037e9f387b7bd9` 2/2, city `3cbe42b166847e40f7071eedb48d613c`
> 3/3, `render_gate` **3/3 PASS ×3**. Shard reflection atlas at break+1
> `95a760c42203b411370a00a4872440c7` — identical on `5adcae12` / `00d28a8b` /
> `283b46ca` **and** on all four tile/semaphore arms, six values, one hash. The
> shatter FRAME was stable 5/5 here at `280cf102ddd5775ee0ef06bbbb20fdbe` (the
> recorded value), but the **atlas stays the gating surface** — the frame has a
> recorded ~1 600 px run-to-run history elsewhere and a gate you have to
> re-litigate is not one.
>
> ### THE CAMPAIGN'S HEADLINE NUMBER, FINAL
>
> Shard bake at the shatter bracket, **min-of-8 paired interleaved, run 1
> discarded**, second shatter frame:
>
> | arm | bake wall | `Render_DeferredLighting` core-ms |
> |---|--:|--:|
> | `5adcae12` binary (the 14.5 ms baseline this campaign started from) | **13.8** (median 14.25) | 118.4 |
> | fixed binary, legacy levers (`--deferred_offscreen_tile_px=0 --deferred_inline_tile_sem=1`) | 13.7 | 112.9 |
> | **fixed binary, shipping defaults** | **11.5** | **94.3** |
>
> **−16 % of the bake, 8 of 8 pairs favouring the fix on both metrics**, and the
> shipped-defaults arm is byte-identical to the legacy one on the atlas.
> ## 2026-08-13 — THE CONE STAGE ROUND-TRIP: REAL, BIT-EXACT, WORTH 0.1 % — AND THE PASS STOPPED BEING INSTRUCTION-BOUND
>
> The user's question was *"for the scalar→simd→scalar — any way to reorder this
> so we won't need the round-trip?"* Round 2 (`03ef0ff0`) widened the two scalar
> per-lane loops but left the **stack arrays** between the 8-wide stages: the
> solve ends with three `__m256`, spills them to `zLoArr`/`zHiArr`/`aliveLane`,
> and the next stage loads them straight back. So round 3 built the fusion —
> delete the arrays, hand the stages over in registers.
>
> **ANSWER: yes, it reorders, and no, it is not the cost.** The fusion is
> bit-exact (all three pins **3/3** with it on, first try — no contraction map
> needed, the same masked `__m256` is simply kept rather than stored) and the
> disassembly confirms it works: **four of the solve's six `str q` disappear**.
> It is worth **0.003 Ginstr/f, 0.1 %**. The arrays are not a buffer, they are a
> **phi node** between the 8-wide and per-lane solve arms — and the register
> allocator re-spends every register the fusion frees, immediately: net stack
> `str q` in the per-spot loop 116 → 115, `ldr q` 248 → **263**.
>
> **AND IN THE BINARY WE SHIP IT IS A REGRESSION, SO IT IS NOT SHIPPED.** The
> 0.1 % is measured in a single-arm control build where the scalar fallbacks do
> not exist. Ship it into the real two-arm structure and it reads **2.437 G vs
> the parent's 2.389 G — +2.0 %, every run**; behind a runtime flag it is worse
> still (+3.5 % on the OFF arm, +2.4 % on ON). There is no form in which this
> ships. Kept reproducible as **`scratchpad/cone_fuse.patch`** rather than as
> `#if` arms in the kernel, because leaving it in would cost the shipping binary
> the very 2 % that disqualified it.
>
> **THE FINDING THAT MATTERS, AND IT RETIRES A DIRECTION.** New
> `-DFDS_CONE_HOTONLY=1` deletes the arms city never executes (segmented hybrid,
> ray-march fallback, midpoint shadow tap); city is **byte-identical** under it,
> which makes it a clean price for interference from code that never runs.
> Across the city t-sweep that is **−7.4 % to −10.5 % of the pass's instructions
> at every pose — and −2.0 % to +0.5 % cycles, i.e. nothing.** IPC just falls,
> 4.11 → 3.64.
>
> The counter is fine: on the same binaries `--no-vol_cone_lane_vec` still reads
> +23 % instructions for **+43 %** cycles. **Not all instructions cost the same.**
> Round 2's 0.55 G were dependent scalar load-modify-store chains and bought
> 0.26 Gcyc; these 0.25 G are well-scheduled spill and branch code that
> dual-issues into slack and buys nothing. **After a 4.217 → 2.390 Ginstr/f cut
> across rounds 1–2 the cone pass has crossed from instruction-bound to
> dependency-bound. Stop counting instructions on it** — the next win has to come
> from cycles (the dependency structure, the non-pipelined `fdiv`/`fsqrt`) or
> from doing less work (fewer pixels × spots).
>
> * **Ships:** two compile-time instruments only — `-DFDS_CONE_FORCE=1` (folds
>   the `vol_cone_*` arms so the allocator and the disassembly show one path) and
>   `-DFDS_CONE_HOTONLY=1` (the cold-arm price). Both emit **literally nothing**
>   at their default 0, verified rather than assumed: the shipping kernel
>   disassembles to the **identical histogram** as the parent's (4538 insns,
>   334/210 stack `ldr`/`str` q, 210 `fmul.4s`, 114 `fmla.4s`, 39 `dup.4s`) and
>   measures 2.388–2.390 Ginstr/f against the parent's 2.387–2.390.
> * **Pins:** nothing moved. city `3cbe42b166847e40f7071eedb48d613c`, greets
>   `778fa6acd85a69cf241babefcdaf598e`, fountain `8db68ccb59416e9a44037e9f387b7bd9`
>   — 2/2 each on the shipping tree, and 3/3 each on the *fused* arm too (that is
>   how the bit-exactness is certified). `render_gate` **3/3 runs, ALL FOUR
>   rows PASS** — including `ec9a5716`'s new `rttslot`
>   `826c09e63217e778cfcef70fe0167279`, re-run after the rebase onto it.
> * Full write-up, disassembly, sweep tables and the reproduction recipe:
>   **`docs/HW_PROFILING.md` section 11**.


> ## 2026-08-12d — THE SHARD BAKE'S "FIXED WORK PAID 238 TIMES" IS REAL, BUT IT IS A CONTENDED SEMAPHORE AND 96 TILES OVER 4 096 PIXELS — NOT LIGHT SETUP. −23 % OF THE PASS, BYTE-NULL
>
> `5adcae12` closed with the item: *"`Render_DeferredLighting` runs 238 times per
> shatter frame on a 64² target and is 78 % of the pass. Its per-invocation fixed
> work — light binning into tiles, the tile-light lists, the shadow-atlas setup —
> is being paid 238 times for 4 096 pixels each."* **The diagnosis was half
> right and named the wrong suspects.** Measured first, restructured second.
>
> ### STEP 1 — WHERE THE PER-INVOCATION TIME ACTUALLY GOES
>
> New `[SHARD-DL]` sub-attribution inside `Render_DeferredLighting`, offscreen
> path only, compile-time gated (`-DFDS_SHARD_BAKE_LAB=ON`) for exactly the
> reason `5adcae12` records — clocks in a shared hot function move pins.
> At HEAD, at the bracket, of **124.7 core-ms** of `Render_DeferredLighting`:
>
> | component | core-ms | share |
> |---|--:|--:|
> | the orchestrator prologue the item named (light SoA + depth bounds + tile binning + ctx) | 11–13 | **9–10 %** |
> | per-tile `renderns::tileDone` release+acquire, 22 848 of them | ~16 | **13 %** |
> | per-tile kernel prologue, 22 848 invocations | ~11 | **9 %** |
> | the actual per-pixel shading of 975 000 pixels | ~85 | **68 %** |
>
> **The named suspect is 9 %.** The real fixed work is *per TILE*, not per
> invocation: the tile grid is engine-global at 12×8, so a 64×64 shard cell
> walks the same 96 tiles a 1920×1080 frame does — 8×8-pixel tiles, and **32 of
> the 96 lie entirely off the right edge** (12 columns × 8 px = 96 px of a 64 px
> image) and shade nothing at all. 96 × 238 shards = **22 848 tile invocations
> per shatter frame.**
>
> ### THE SEMAPHORE, MEASURED STANDALONE
>
> Each tile kernel ends with `renderns::tileDone.release()` and the inline
> dispatch loop immediately `acquire()`s it back — "net-zero on the shared
> semaphore". It is not free, because it is *shared*: all 12 shard workers hit
> the same `std::counting_semaphore` at once. Standalone microbenchmark on this
> box (M2 Max, `/tmp/shardamort/sem2.cpp`):
>
> | | per release+acquire pair |
> |---|--:|
> | uncontended, 1 thread | **34.5 ns** |
> | 12 threads on ONE semaphore | **3.4–4.0 µs of CORE time** |
>
> A 100× contention penalty, paid 22 848 times a frame. On the inline path the
> permit is pure ceremony — the thread that posts it is the thread that takes it
> back.
>
> ### STEP 2 — THE SHAPE CHOSEN, AND THE LOSER'S NUMBERS
>
> **Shape (a), "light the ATLAS, not the cells", was rejected on measurement,
> not on the structural objection.** The structural objection is real (each cell
> has its OWN reflected camera, so view-space reconstruction differs per cell and
> the kernel would need per-tile camera indirection) — but the decisive point is
> that after step 1 there is nothing left for it to amortize: the only cost that
> is genuinely *per invocation* rather than per tile or per pixel is the
> orchestrator prologue, **11 core-ms of 124.7**, and it is now 2.6.
>
> **What shipped is two independent fixes, both byte-null:**
>
> 1. `--deferred_inline_tile_sem` (default 0 = fixed). An inline dispatch posts
>    no permit and drains none. `DeferredLightingCtx::inlineDispatch` carries the
>    mode; the main frame is untouched (`ov == nullptr`).
> 2. `--deferred_offscreen_tile_px` (default 32). An offscreen target sizes its
>    tile grid to ITSELF: `clamp(xres/32,1,12) × clamp(yres/32,1,8)`, so a 64²
>    cell gets 2×2 and everything ≥ 384 px wide keeps the full 12×8. The main
>    frame keeps 12×8 unconditionally.
>
> Grid sweep, semaphore fix already on, `Render_DeferredLighting` core-ms /
> wall ms, min-of-6 interleaved: **12×8 = 113.7 / 13.7 · 8×8 = 115.2 / 13.9 ·
> 4×4 = 97.9 / 11.6 · 2×2 = 94.0 / 11.6 · 1×1 = 96.6 / 11.5.** The optimum is
> broad and flat from 2×2 down to 1×1 — which says the win is *fewer tile
> prologues*, not better culling: 1×1 hands every pixel the union of all lights
> and still ties. The per-tile light cull buys almost nothing at this narrow
> off-axis FOV.
>
> ### STEP 5 — WHAT IT BUYS
>
> **Min-of-8 paired reps, interleaved, one binary, the two flags the only
> difference** (`--repro=greets@t=3122 --repro_from=3112 --repro_settle=0`,
> `FDS_GREETS_SHATTER=1`, `FDS_GREETS_CAM="28.8,10.8,-62.85,1,0,0"`, the SECOND
> shatter frame — frame 1 is the cold bake):
>
> | | HEAD | fixed | Δ |
> |---|--:|--:|--:|
> | `Render_DeferredLighting` core-ms | 121.7 | **93.6** | **−23 %** |
> | shard-bake wall ms | 13.6 | **11.6** | **−15 %** |
> | fixed-work share of the pass (`[SHARD-DL]`) | ~32 % | **3 %** | |
>
> Every one of the 8 paired reps favours the fix (HEAD 13.6–16.6 ms, fixed
> 11.6–13.6). Ablation, min-of-6: semaphore alone 128.5 → 109.0, grid alone
> → 92.5, both → 95.5 — **the two overlap**, because at 2×2 there are only 4
> tiles per call and the semaphore traffic is already 96× smaller. Both ship:
> the semaphore fix is the one that generalizes to any offscreen inline
> dispatch, the grid fix is the one that removes the tile prologues.
>
> **The break frame the user actually feels**, `--bench=scene@scene=greets,t=3122,iters=1`
> (one break frame, 1920×1080, cold bake), min-of-6 interleaved:
> **46.25 → 43.88 ms**, with the bake inside it 16.4 → 14.0 ms. −2.4 ms, −5.1 %
> of the frame.
>
> ### GATES — BYTE-NULL EVERYWHERE, INCLUDING THE SHATTER FRAME
>
> * **1024² reflection atlas**, break+1: `95a760c42203b411370a00a4872440c7` on
>   ALL FOUR arms (legacy / semaphore-only / grid-only / both). A coarser tile's
>   light list is a SUPERSET of a finer one's *in the same global light order*,
>   and the extra lights fail the per-pixel `len2 <= range2` mask → exact `0.0f`.
> * **The break+1 FRAME itself**: `280cf102ddd5775ee0ef06bbbb20fdbe`, **6 runs on
>   each arm, one value**. Note this contradicts `5adcae12`'s "the shatter frame
>   is nondeterministic at HEAD, 1 600 of 2 073 600 px run-to-run" — at this
>   commit, on this bracket, it is stable 12/12. Whatever produced that spread is
>   not present here; not chased.
> * **Pins**: greets `778fa6acd85a69cf241babefcdaf598e`, fountain
>   `8db68ccb59416e9a44037e9f387b7bd9`, city `3cbe42b166847e40f7071eedb48d613c`
>   — 2/2 each with the fix ON, and 2/2 again with both revert levers set.
> * **`render_gate.sh` 3/3 PASS** (`4ac809e5` / `b41894f9` / `166fa25a`) —
>   `mirrortest` is what covers the mirror RTT, the other `DeferredOverride` user.
>
> Images: `docs/img/fogwt/shardtile_t3122_bytenull.png` (mosaic + atlas),
> `docs/img/fogwt/shardtile_t3122_atlascells.png` (cell zoom).
>
> ### TWO METHOD TRAPS, RECORDED SO THE NEXT AGENT DOES NOT PAY THEM
>
> 1. **zsh does not word-split unquoted parameters.** An A/B loop that builds an
>    arm as `E="--flag_a=1 --flag_b=0"` and passes bare `$E` sends ONE argv token
>    under zsh — and this binary's flag parser drops a token containing a space
>    *silently* (a genuinely unknown flag is reported and fatal; this is not).
>    The revert arm then measures the DEFAULT and the A/B reads "no difference".
>    It cost a full round of measurements here. Use bash + an array
>    (`ARM=(--a --b)` … `"${ARM[@]}"`), which is what `tools/`-style scripts do.
> 2. **`--bench=scene@...,iters=N` is not an A/B harness for the shatter past
>    iteration 1.** The shard poses advance by WALL-CLOCK dt, so the face count
>    per iteration is non-monotonic and the two arms diverge into different
>    poses. `iters=1` (one break frame) and the frozen `--repro` bracket are the
>    comparable measurements.
>
> ### WHAT IS LEFT, HONESTLY
>
> After this, **97 % of the pass is the per-pixel shading of 974 848 pixels**
> (238 cells × 64²) at ~96 ns/px with ~10–18 lights surviving the tile cull per
> tile. That is real work, not overhead: it is roughly half a 1080p frame's worth
> of deferred shading, and the shards cover about that much screen at the break.
> The remaining levers are all *rate or resolution* reductions — checkerboard /
> quarter-rate for the offscreen bake (the wave-2 `TileFill` machinery already
> exists and is per-target selectable), or a smaller `texRes_` — and every one of
> them is a LOOK change on a surface the user gates by eye, so none was taken
> unilaterally. The per-target HDR item below lands in this same call, and the
> addendum is what happened to it.
>
> ### ADDENDUM, SAME DAY — THE PER-TARGET HDR ITEM: BUILT, MEASURED, AND OVERTURNED
>
> The backlog's designed fix (per-worker HDR buffer through `DeferredOverride`,
> activate + tonemap after the kernel) is written as `--shard_hdr`. **It makes
> the residual four times worse**, so it ships default OFF as a measured arm.
> Panel-window luma at the bracket, against the MAIN deferred pass from the
> shard's own reflected eye (`FDS_GREETS_CAM="68.79,10.8,-62.85,-1,0,0"`):
> reference **74.78**; shipping **87.31 (+12.53** — reproducing `ddb1d15`'s
> recorded +12.5 to two decimals, so the harness is right); `--shard_hdr`
> **129.79 (+55.01)**.
>
> **The atlas is an ALBEDO TEXTURE, not a finished image.** The shards are
> ordinary opaque geometry, so the main frame's deferred kernel samples the
> atlas as a texel, lights it, and tonemaps it with everything else. One A/B
> settles it: with the flag OFF, sweeping the **frame's** `--hdr_exposure`
> 1.0 → 2.0 moves the mosaic **87.31 → 130.78**. Tonemapping the cell as well
> applies the transfer function twice — and `--shard_hdr` at exposure 1.0
> landing on legacy-at-exposure-2.0 is exactly that signature. The mirror RTT
> is not a counterexample but the clue: it keeps FLOAT radiance in `hdrRefl`
> and hands *that* to the frame. **The real remedy is an HDR atlas, and it is
> not started.**
>
> **What shipped from it anyway, byte-null:** `DeferredLightingCtx::hdrBuf`
> carries each pass's own HDR target, replacing the kernels'
> `Hdr_WritableFor(ctx.xres, ctx.yres)` — "the global happens to be sized like
> me" as a proxy for "am I the main pass?", which is what made the failure
> silent. Pins 2/2 ×3, `render_gate` 3/3, shatter frame `280cf102…` unchanged.
> Image: `docs/img/fogwt/shardhdr_t3122_doubletonemap.png`.

> ## 2026-08-12c — THE CONE SOLVE IS 8 LANES WIDE NOW, −9.4 ms/FRAME ON CITY, AND NOT ONE PIN MOVED
>
> The standing biggest perf item in the tree, closed. `a16567b` had located it
> and left the lever: `Render_VolumetricCones_Tile` is **37.5 % of all running
> CPU** at city t=1961, and **63.6 % of that pass (2.681 of 4.217 Ginstr/f)** was
> ONE SCALAR LOOP — 25.6 M per-lane quadratic solves a frame, ~105 instructions
> each, sitting inside the per-spot loop and feeding a body that was already
> 8-wide. IPC 4.0–4.2, so it was instruction COUNT, not stalls. Now widened,
> default ON as `--vol_cone_solve_vec`.
>
> **MEASURED, city t=1961, interleaved ABBA min-of-6 on one binary (the flag is
> the only difference between arms), load 15–20:**
>
> | | scalar | 8-wide | |
> |---|--:|--:|--:|
> | cones wall_min | 30.764 ms | **21.406 ms** | **−9.36 ms, −30.4 %** |
> | cones Ginstr/f | 4.091 | **2.868** | −29.9 % |
> | cones Gcyc/f | 1.020 | **0.718** | −29.6 % |
> | renderFrame wall_min | 71.753 ms | **62.309 ms** | **−9.44 ms, −13.2 %** |
> | renderFrame Ginstr/f | 8.288 | 7.067 | −14.7 % |
>
> The −9.44 ms frame saving and the −9.36 ms cones saving agree, which is the
> internal check that the attribution is real. IPC is flat (3.91 → 3.94) — the
> win is removed instructions, not unblocked stalls, exactly as diagnosed.
> Across the city t-sweep the pass drops **24–31 % at every pose** and its share
> of frame instructions goes **32–50 % → 27–41 %** (full table in
> docs/HW_PROFILING.md §9).
>
> **NOT ONE PIN MOVED — the port is BIT-EXACT.** city
> `3cbe42b166847e40f7071eedb48d613c` 2/2 with the flag ON *and* 2/2 with it OFF
> (which is what proves the scalar arm is untouched), greets
> `778fa6acd85a69cf241babefcdaf598e` 2/2, fountain
> `8db68ccb59416e9a44037e9f387b7bd9` 2/2, `render_gate` 3/3 PASS (mirrortest
> `4ac809e5`, conetest `b41894f9`, halotest `166fa25a`). **You waived
> byte-exactness for this task and it turned out not to need waiving** — so
> there is nothing here for your eye to countersign, and the frame is unchanged
> to the last bit: `docs/img/fogwt/conevec_t1961_city.png` is what both arms
> render.
>
> **HOW THE BIT-EXACTNESS WAS GOT: read the compiler's FMA contraction map off
> the DISASSEMBLY, not off the source.** Under the tree-wide
> `-ffp-contract=fast` the compiler picks, for each `a*b + c*d`, which product to
> fuse and which to round, **and its picks do not follow source order** — it
> compiles `Dx*Px + Dy*Py + Dz*Pz` into `fma(Pz,Dz, fma(Px,Dx, fl(Py*Dy)))`.
> Release is thin-LTO so the `.o` files are bitcode: disassemble the LINKED
> binary. The map it had chosen, reproduced per lane, is in §9; in every pair it
> is the SECOND product that ends up rounded. Two traps worth carrying forward:
> **simde does not give you the fused op you asked for** (`_mm256_fmadd_ps` →
> `vfmaq_f32`, but `_mm256_fmsub_ps`/`_mm256_fnmadd_ps` → `sub(mul(a,b),c)`,
> handing the choice back to the compiler — spell every `a*b - c` as
> `fma(a,b,NEG(c))` with an explicit sign-bit xor, since an FMA intrinsic's
> operand is a barrier it will not re-contract across); and **`std::min`/`max`
> are not `_mm256_min_ps`/`_mm256_max_ps`** (NEON `FMIN` resolves NaN and −0 the
> opposite way from the scalar `FCSEL`) — transcribe as cmp + blend, and use the
> unordered compare predicates wherever the scalar reads `if (x < y) continue;`.
>
> **ACHIEVED vs CEILING, honestly.** The solve went **2.65 → 1.37 Ginstr/f, a
> 1.94×**, not the 8× the lane count suggests: **55 % of a perfect 8-wide port,
> 64 % of the realistic floor.** Two reasons, and the second generalises: on
> arm64 there is no 8-wide unit — simde emulates every `__m256` op with TWO
> 128-bit NEON ops, so a `_mm256_`-spelled port cannot beat ¼ of scalar; and the
> wide arm computes both `a`-sign branches and the whole tail for all eight lanes
> where the scalar arm's dead lanes bail early.
>
> **THREE VARIANTS BUILT, MEASURED AND REJECTED** (each benched in the same
> interleaved session as the arm it is compared against; `Ginstr/f` reproduces to
> 0.3 % for a fixed binary+arm, so a 1.5–2 % move is a real effect):
>
> * **Range-sphere early-out** — trying to buy back exactly the dead-lane
>   overhead named above. **+2.0 % instructions.** The premise ("39.9 % of pairs
>   have zero alive lanes") is what is wrong: the branch only fires when ALL
>   EIGHT lanes miss the sphere, and most dead pairs lose their lanes later.
>   Kept compiled out at its site as `FDS_CONE_SOLVE_EARLYOUT`.
> * **Raw `rcp`/`rsqrt` instead of true div/sqrt** — **+1.6 % instructions,
>   −1.7 % cycles, −1.0 % wall: nothing.** The loop is instruction-bound (IPC
>   3.9), and NEON's `vrecpe`/`vrsqrte` are **8-bit** estimates, half what the
>   x86 intrinsic names imply, so usable accuracy needs NR steps costing more
>   than the divide they replace. Raw is the FASTEST that family can be, so one
>   build closes the whole family — the cheap discriminator. **Your read on the
>   numerical rule was right and is now measured:** this consumer is not the
>   `vec_ggx` case at all — the raw-estimate build moved city by **200 px of
>   2 073 600, every one at 1 LSB**
>   (`docs/img/fogwt/conevec_t1961_rejected_approx_diff.png`). Approximation is
>   harmless here; it just does not pay. Kept as `FDS_CONE_SOLVE_APPROX`.
> * **Relaxed FP association** (`_mm256_min_ps`/`_mm256_fmsub_ps` instead of the
>   exactness-preserving spellings) — **−0.5 % instructions**, and it moves the
>   city pin by **3 px at 1 LSB**. Half a percent is not worth a byte gate.
>
> **GREETS IS WHY THE WIDE ARM IS GATED TO NON-SEGMENTED CONES.** Cones DO run in
> greets (1.18 Ginstr/f) — the greets pin is not vacuous coverage — but ungated
> there the wide arm reads **instructions −3.8 % yet cycles +3.6 % and wall
> +1.4 %** (7.63 → 7.73 ms), reproduced in two independent min-of-6 sessions.
> Greets' cones are the narrow disco beams on the 8-segment hybrid body, where
> the solve is a minor share and the dead-lane work is not repaid. With
> `!segPath` in the gate greets is neutral (1.184 → 1.183, wall −1.6 %) and city
> keeps the whole win, since every city headlight is a wide cone. Both arms are
> bit-identical so the gate is byte-null. **The general lesson: a
> per-lane→wide port is not uniformly a win across call sites with different
> bodies — measure the second scene before defaulting it on.** Fountain runs no
> cones at all (0.000 Ginstr/f, 0.003 ms); its pin is a no-regression control
> only, and saying so is the honest version of "three pins held".
>
> **WHAT IS NEXT IN THIS PASS.** Cones is STILL the biggest single item in the
> frame — 2.868 of 7.067 Ginstr/f, against DeferredLighting 1.247, fastfog
> 1.092, gbuffer 0.891, TBR-render 0.846 — but it is down from 49 % of frame
> instructions to 41 %, and what it is made of has flipped: the untouched SIMD
> body + shadow taps + accumulate is now the majority of it (~1.52 of 2.87 G,
> **inferred** by holding a16567b's ablation split, not re-measured). The next
> lever inside this pass is the integration body, not the prologue — and whoever
> takes it should re-run the a16567b ablation against the new arm first rather
> than trust that inference.
> ## 2026-08-12 — THE PER-FACE CONE CULL IS BUILT AND BYTE-IDENTICAL. IT RECOVERS NONE OF THE 10.5 ms, BECAUSE THE SHARD BAKE IS 78 % DEFERRED LIGHTING
>
> `ddb1d15` ended with a recorded design: *"The right accelerator is a per-FACE
> test (face bounding sphere vs cone); nobody has written it."* Sent to write
> it. **It is written, it is exactly as conservative as claimed, and the thing
> it was built to accelerate is not there.**
>
> ### THE CULL IS CORRECT — 0 PIXELS, NOT "CLOSE"
>
> `--shard_cone_cull=2`, `FDS/RENDER/ReflFaceCull.cpp`. Reject a face iff its
> own world bounding SPHERE lies entirely outside the shard's cone. At the
> bracket (`--repro=greets@t=3122 --repro_from=3112 --repro_settle=0`,
> `FDS_GREETS_SHATTER=1`, `FDS_GREETS_CAM="28.8,10.8,-62.85,1,0,0"`):
>
> | | result |
> |---|---|
> | 1024² reflection atlas, cull ON vs OFF | **0 of 1 048 576 px different** |
> | face tests rejected | **67 825 of 80 415 (84.3 %)** |
> | face-list entries | **9 921 → 8 678 (−12.5 %)** |
>
> The invariant that buys it: a face whose sphere reaches the cone survives
> **whole**, and a face that survives is rendered with its vertices
> **untouched**. No stamped fake positions — which is the entire failure mode of
> the per-vertex form (mode 1) that ate two thirds of the reflection.
>
> **The break+1 FRAME cannot be a gate and that is not the cull's fault:** the
> shatter frame is nondeterministic run-to-run at HEAD. Base binary, same
> command twice: **1 600 of 2 073 600 px differ (0.077 %)**. The cull's own
> arm differs by 1 385 px — *below* that floor. The ATLAS is deterministic
> (0 px across runs), which is why the gate above is the atlas.
>
> ### THE PREMISE IS WRONG: THE BAKE IS NOT GEOMETRY-BOUND
>
> New `[SHARD-PHASE]` attribution on `FDS_SHARD_REFL_PROF`. Core-ms summed over
> 12 workers, min-of-6 interleaved, run 1 discarded, load 10-22:
>
> | phase | cull off | per-face cull | legacy per-vertex |
> |---|--:|--:|--:|
> | `Transform_Objects` | 6.5 | 7.5 | 6.4 |
> | G-buffer fill (raster) | 27.4 | 29.7 | 7.6 |
> | **`Render_DeferredLighting`** | **124.4** | **129.1** | **46.2** |
> | volumetric cones | 0.2 | 0.3 | 0.1 |
> | **wall ms** | **14.1** | **14.7** | **6.8** |
>
> The geometry front-end is **4 %** of the pass; the deferred shading of the
> reflection's own pixels is **78 %**. So `ddb1d15`'s 4.0 → 14.5 ms is not lost
> culling, it is **the reflection appearing**, and no conservative cull can take
> it back: the 12.5 % of faces this one drops are faces that rasterize zero
> pixels. Mode 1's 6.8 ms was never the speed of culling either — it is the
> speed of not drawing.
>
> ### IT COSTS ALL THREE PINS MERELY TO CARRY, SO IT SHIPS COMPILE-TIME GATED
>
> With the mask pass written inline in `Transform_Objects`, **all three scene
> pins moved on frames that never shatter a mirror**: greets
> `778fa6ac→7a6370a1`, fountain `8db68ccb→eebf68e6`, city
> `3cbe42b1→80583b85`. Bisected under `-ffp-contract=fast`: the *branch* the
> cull adds to the face loop is byte-null; the *call* is not, at either site
> tried (before the vertex loops, and after them). Same hazard
> `docs/VISIBILITY_PLAN.md §8a` records for `--xfrm_pass_prof`, same remedy:
> `option(FDS_SHARD_BAKE_LAB)`, default OFF, preprocessor-removed. **0.1 ms is
> not worth three pins.** With the gate off: greets `778fa6ac`, fountain
> `8db68ccb`, city `3cbe42b1` (2/2 each, matched against a clean worktree at
> the same commit) and `render_gate.sh` ALL PASS (`4ac809e5` / `b41894f9` /
> `166fa25a`).
>
> **One honest residual.** The `[SHARD-PHASE]` clocks stay compiled in (they are
> the instrument that settled this) and their presence drifts the shard atlas by
> **300 of 1 048 576 px (0.029 %, mean |Δ| 4.2 on changed)** against the parent
> build — FP contraction, no semantic change, and nothing pins the shard bake
> because no pin recipe shatters a mirror. The pins and the gates, which are
> what this project gates on, are byte-identical.
>
> ### TWO ERRORS IN THE CONE `ddb1d15` DESCRIBED, BOTH CAUGHT BY MEASUREMENT
>
> 1. **It is not "~1° wide". It is 17-19°.** The legacy cone is built about the
>    shard NORMAL, and the reflected eye does not look at its own shard — the
>    window sits metres off to the side — so the cone has to open that far just
>    to reach it. A 19° cone culls almost nothing, which is why the first
>    per-face arm rejected only 39 % and still cost what it saved.
> 2. **The shard camera's basis is not orthonormal.** Its rows are
>    `axisU = wc1−wc0`, `axisV = wc3−wc0` and the normal, and a shard quad is
>    not a rectangle, so reconstructing a window point as
>    `Er + D·N + du·axisU + dv·axisV` is wrong by the skew — **measured 6.8° of
>    axis error, four window widths**, and it culled faces sitting in the middle
>    of the viewport. The shipping construction (`shardFaceCone`) instead
>    **inverts the actual projection** at the four screen corners (one 3×3
>    Cramer solve per shard) and assumes nothing.
>
> The tell for both was a margin sweep (`--shard_cone_cull_margin`) that never
> converged to the no-cull image: a cone that is wrong in SHAPE cannot be fixed
> by widening, only one that is merely narrow can.
>
> ### WHERE THE ms ACTUALLY IS
>
> `Render_DeferredLighting` is called **238 times per shatter frame on a 64²
> target** and is 78 % of the pass — its per-invocation fixed work (tile light
> binning, tile-light lists, shadow-atlas setup) is paid 238 times for 4 096
> pixels each. Logged in `docs/OPTIMIZATION_BACKLOG.md` as the item that would
> actually move this pass; it touches the same call as the per-target-HDR item
> already logged there. Also logged: the greets **shatter frame is
> nondeterministic** (1 600 px run-to-run), which nobody has chased.

> ## 2026-08-12 — THE JAMB DOES NOT BOW BECAUSE OF THE NORMAL; THE PINNED BORDER STANDS PROUD OF ITS OWN WALL
>
> His report at t=5998 (`/tmp/greets_dump_0_t5998.ppm`): the doorway jamb "still
> bulges", the face rounding outward approaching the edge, plus faint blue dotted
> vertical artifacts. His instinct was that this is the POM edge disease again —
> smoothed normals off the surface plane. **It is not. Two hypotheses died on
> measurement and the surviving mechanism is a LEVEL, not a DIRECTION.**
>
> ### THE MECHANISM, MEASURED
>
> `--greets_displace_junction_census` on the jamb plane x=17.898 (133 displaced
> verts, binned by distance from the border line z=-58.014):
>
> | dist into wall (u) | n | angle(ride, plane normal) | out-of-plane (u) |
> |---|---|---|---|
> | 0.05–0.10 | 4 | 23.55° | −0.05657 |
> | 0.35–0.60 | 4 | 24.49° | −0.07127 |
> | 1.0–1.6 | 16 | 30.83° | −0.06255 |
> | 2.5–4.0 | 33 | 42.02° | −0.05208 |
> | 4.0–6.0 | 48 | 54.22° | −0.04372 |
>
> **DEAD HYPOTHESIS 1 — "the smoothed normal is contaminated NEAR the corner."**
> The opposite is true by a factor of 2.3: the ride direction is *closest* to the
> wall's own normal at the border (23.6°) and *furthest* from it in the far field
> (54.2°). Whatever tilts the normals, it is not the jamb's return face reaching
> in from the edge.
>
> **THE LIVE MECHANISM.** The border is pinned at displacement exactly 0 while the
> wall it bounds sits ~0.055–0.077 u *behind* its authored plane (the convention is
> zero-mean against the WHOLE height map, `d = amp*(h − 0.5491)`, and this wall's
> window is not that mean). So the authored border line stands PROUD of the surface
> it bounds and the last band ramps outward to meet it. As one number — near-edge
> (0–40 px) minus far-field (350–800 px) out-of-plane offset from the z16 dump —
> **bow −0.01912 u at t=5998 and −0.01858 u at t=5987.** That is the rounding-out.
>
> ### DEAD HYPOTHESIS 2 — RIDING THE PATCH-PLANE NORMAL MAKES IT WORSE, NOT BETTER
>
> `--greets_displace_plane_normal` (new, default OFF) rides each vertex's own
> coplanar-fan normal instead of the smoothed one, guarded on coplanarity within
> 2° and on having no position twin. It is the direct analogue of the POM per-face
> fix, and **it is the worst arm measured**: at t=5998 silhouette std **16.49 px**
> (span 59) against the shipping arm's 3.23 and the authored geometry's 1.73, and
> it opens **797 background px** where the shipping arm opens 0. At t=5967 it is
> std 27.82, span 95. Composed with the mean fix it is still bad (std 8.68, 5 612
> background px). **Kept as a flag only so the refutation is reproducible.**
>
> ### THE FIX: A BORDER LEVEL, NOT A BORDER DIRECTION
>
> `--greets_displace_border_mean` (INT, default 0 = OFF, byte-null) holds every
> FREED border vert at ONE CONSTANT displacement instead of at zero, so the border
> stays a straight line by construction and only its depth is chosen.
>
> * **mode 1 — one constant per MATERIAL.** Straightest silhouette in the campaign
>   (std **1.37**, span 4, better than the authored geometry) but the depth is
>   wrong: bow flips −0.01912 → **+0.02226**.
> * **mode 2 — one constant per authored PLANE** (canonical quantised plane hash,
>   sign-folded on the dominant component; 1 594/1 594 freed border verts of
>   `rooms` bind to their own plane, 0 fall back). Straightness holds and improves
>   (std **1.35**) — but **THE PREDECESSOR'S PREMISE WAS WRONG**: per-material was
>   never the reason mode 1 over-recessed. The per-plane means are **−0.0765 to
>   −0.0781** against the material's −0.0715 — agreement to 8% — so mode 2 goes
>   *deeper*, bow **+0.02521**. Per-plane is the right structure for a different
>   reason (it is a per-wall level, not a per-room one) but it did not move the
>   number that was wrong.
>
> **WHAT WAS ACTUALLY WRONG: THE ARITHMETIC MEAN IS NOT THE VISIBLE LEVEL.** At the
> grazing angles where this defect is visible the rendered surface is the relief's
> UPPER ENVELOPE, not its mean — the peaks occlude the valleys. Measured: vertex
> mean −0.0765 u, rendered far-field level only **0.0248 u**, a factor of **3.1**.
> Pinning the border to the full vertex mean therefore overshoots and the bow just
> changes sign. `--greets_displace_border_mean_scale` prices it, and the bow is
> **linear in the scale to three figures**:
>
> | scale | bow t=5998 | sil std | bow t=5987 | sil std |
> |---|---|---|---|---|
> | 0.00 (= the shipping zero-pin, reproduced exactly) | −0.01912 | 3.23 | −0.01858 | 6.63 |
> | 0.20 | −0.00921 | 2.04 | −0.00741 | 4.65 |
> | 0.30 | −0.00425 | 1.66 | −0.00241 | 3.70 |
> | **0.40** | **+0.00047** | **1.61** | **+0.00228** | **2.82** |
> | 0.50 | +0.00499 | 1.55 | +0.00673 | 2.40 |
> | 0.80 | +0.01754 | 1.45 | +0.01886 | 2.27 |
> | 1.00 | +0.02521 | 1.35 | +0.02615 | 2.16 |
>
> Fits: `bow = 0.0443*scale − 0.0179` (t=5998) and `0.0442*scale − 0.0164`
> (t=5987) → **zero crossings 0.405 and 0.372, mean 0.39.** Default set to **0.40**
> as a MEASURED calibration constant. At 0.40 the bow is 40× smaller than the
> shipping arm's and the silhouette std is **1.61 — straighter than the authored
> geometry itself (1.73)**. Note the two objectives do not peak together:
> straightness keeps improving past 0.40 while flushness degrades, which is why
> this is a knob and why 0.40 is the flushness point.
>
> ### WHAT IT COSTS — STATED LOUDLY, AND IT IS NOT FREE
>
> Background (z==0) pixels over the 18 poses of `docs/greets_review_poses.txt`:
>
> | arm | total bg px |
> |---|---|
> | shipping `--greets_displace` | **157** |
> | `+ --greets_displace_free_edge` | **3 626** |
> | `+ --greets_displace_border_mean=2` (scale 0.40) | **6 338** |
>
> **The holes are overwhelmingly the free-edge arm's** (157 → 3 626, ×23); the
> border level roughly doubles them again. Worst poses t=5743 (3 → 1 380) and
> t=6133 (4 → 901). Mechanism: a freed border no longer meets the *other
> material's* face across a T-junction whose far side is not vertex-coincident, so
> the constant offset opens a slit. **This needs his eye before it goes anywhere
> near a default.** At the two >90° seam-corner poses the picture reverses once
> `--greets_displace_seam_weld` is added: shipping 1 992/1 948 → fix+weld
> **1 434/1 255**, i.e. better than shipping.
>
> ### THE DOTTED BLUE ARTIFACT IS NOT OURS
>
> Isolated blue-excess pixels at t=5998: flat **2 037**, shipping displaced
> **1 939**, free-edge **1 956** — the undisplaced arm has the *most*. The vertical
> dotted column he saw is at x≈979–981 and is the **lamp/torch stem**, a ~1 px-wide
> post under the blue key light, rasterised with alternating coverage so it reads
> as a dotted line (`/tmp/dot_x980.png`). Present identically without any
> displacement. Not a displacement artifact; a thin-geometry rasterisation one.
>
> ### CRISP-PER-STONE NOTCH: NO ARM HAS ONE
>
> Silhouette profile over rows 150–950 at t=5998, transitions and run lengths:
> flat 6 steps / 7 runs / median run 132 rows; shipping 13 / 14 / 79; free-edge
> **29 / 30 / 3**; mode 1 4 / 5 / 168; mode 2 **4 / 5 / 173**. **Every transition
> in every arm is exactly 1 px (count of |Δ|≥2 is zero everywhere).** So nothing
> here notches per stone course: the pinned and mean arms make the border a
> straight line — mode 2 most of all — and the free-edge arm's 29 steps are
> per-vertex *wander* (median run 3 rows), not stone-aligned notching. If he wants
> a crenellated doorway edge, none of these arms is the tool; the mortar structure
> is not recoverable from the 62 px strip this pose leaves, so that measurement is
> owed at a face-on pose.
>
> ### THE GPU ARM HAS THE SAME DEFECT BY CONSTRUCTION — READ FROM SOURCE, NOT RENDERED
>
> `GpuBench/shaders/deferred.metal` `tessShade()` computes
> `op += nrm * (tu.k.y * (h - tu.k.z) * att)` with `nrm = normalize(on)`, the
> **interpolated (smoothed) vertex normal** — term for term the CPU bake's
> convention. `--tess_border_ramp` fades `att` to **zero** approaching a
> one-face edge, which is the GPU's analogue of the CPU zero-pin and therefore
> carries the *same* proud-border defect, spread over the ramp width instead of
> concentrated at the last cell. **NOT MEASURED HERE**: GpuBench writes a colour
> PPM only (no depth dump), and it is not built in this worktree, so a 0.02 u bow
> is not recoverable from its output. The aligned change would be to fade `att`
> toward the plane's mean level × 0.40 rather than toward 0, which needs a
> per-patch mean uploaded alongside `borderMask`. **Owed work, not done.**
>
> ### GATES
>
> All new flags default OFF / byte-null. Shipping displaced arm at t=5967
> **`c0beec384141e4f18525a84e6b07a9bc`, byte-identical** before and after (checked
> twice, either side of the flag-type change). greets pin
> `778fa6acd85a69cf241babefcdaf598e` **4/4**, fountain `8db68ccb…` 2/2 after
> discarding run 1 (run 1 was `b91cb2ba…` — the documented post-rebuild cache
> write), city `3cbe42b166847e40f7071eedb48d613c` 3/3, `render_gate` **3/3 PASS**.
>
> ### STEP 0 — THE GREETS PIN ADJUDICATION IS SETTLED, AND THE LOSER IS A RECIPE BUG
>
> Two agents reported contradictory greets pins. **Winner:
> `778fa6acd85a69cf241babefcdaf598e`, 16 runs across FOUR content/code
> configurations at origin tip `3b00bbc7`, one value every time:**
>
> | arm | tree | greets pin |
> |---|---|---|
> | A | worktree at tip, COMMITTED `GREETS.FLD` (`62c68fc9…`) | `778fa6ac…` 4/4 |
> | B | same binary, Runtime seeded with the USER'S dirty `GREETS.FLD` (`89c4ec35…`) | `778fa6ac…` 4/4 |
> | C | independent worktree + own build, user's `GREETS.FLD` + `Hull.lwo` | `778fa6ac…` 4/4 |
> | D | fresh build with the main tree's parent-commit control revert of `0b466b77` applied, user's content | `778fa6ac…` 4/4 |
>
> **THE PIN DOES NOT DEPEND ON THE USER'S UNCOMMITTED AUTHORING FILES.** His
> `GREETS.FLD` edit is invisible at t=1588 (arm A ≡ arm B, same binary). The note
> in `tools/render_gate.sh` saying greets is gated out-of-band "because its pin
> depends on the user's UNCOMMITTED authoring files" is **measurably wrong** and
> should be corrected when someone touches that file.
>
> **THE LOSER, `2e96e91d9ce0188981cd71c3fdebb954`, IS REPRODUCIBLE ON DEMAND: it
> is the pin recipe run WITHOUT the `FDS_GREETS_CAM=` prefix** — the scene's own
> scripted camera at t=1588 instead of the pinned one. Verified exactly, first try.
> Its "parent-commit control" was internally consistent because *both* of its arms
> dropped the same prefix — **a differential control cannot detect a recipe
> transcription error**, which is the lesson worth keeping. Recipe perturbations
> that do NOT produce it, for the record: `--env_refl` on `e5f38b40…`, no
> `--glass*` `42be82c8…`, `+--greets_displace` `0d05726a…`, bare `--deferred`
> `8ba504ae…`, dropping `--hdr` reproduces the pin exactly.
>
> ### FLAGS ADDED (all default OFF / byte-null)
>
> * `--greets_displace_border_mean` INT 0/1/2 — the fix, mode 2 recommended
> * `--greets_displace_border_mean_scale` FLOAT, default **0.40** (measured)
> * `--greets_displace_plane_normal` — the refuted direction arm, kept for repro
> * `--greets_displace_junction_census` gains `[STONE-BOW]` / `[STONE-BMEANP]`
>
> Recommended arm for his eye:
> `--greets_displace --greets_displace_free_edge --greets_displace_border_mean=2
> --greets_displace_seam_weld`. Before/after strips at all 8 of his poses:
> `docs/img/fogwt/bmeanwt_p{1..8}_*_before_after.png`.
>
> ## 2026-08-12c — FADING THE FOLLOW SNAP: THE OBVIOUS SHAPE LOST, BECAUSE THE SNAP IS THE WHOLE CUBE
>
> User: *"the mech pop - let's fade the snap."* Two shapes were on the table and
> the brief said to pick by measurement. **The cheap, obvious one lost.**
>
> **THE POP.** A followed capture point is budgeted (1 re-bake/frame) and
> thresholded (`--env_probe_follow_eps` 1 u): it sits still while its owner walks
> away, then jumps the whole accumulated drift in one frame. On his line the
> greets canopy re-bakes at **t=7029**; across that frame the live cube's mean
> |dRGB| against the previous overlay goes **1.80 → 18.54**, and the mech's own
> coverage of its own cube steps **36 864 → 98 222** texels.
>
> **(a) GLIDE THE POINT — LOST.** Render the movers from a point that smoothsteps
> to the new one. It does its own job: the own-assembly coverage step falls from
> **+61 358 to +1 272** texels (N=8). Its price is bounded and decays to zero —
> overlay-vs-base misregistration max **0.988 u** at N=8 (mean 0.069, 3 frames in
> 30) to max **1.029 u** at N=32 (mean 0.275, 14 in 30).
>
> **AND IT STILL LEAVES MOST OF THE POP STANDING, for a structural reason worth
> keeping written down: A RE-BAKE RE-CAPTURES THE WHOLE CUBE — the room, not just
> the movers — from a point 1 u away, so all six faces jump at once, and moving
> the overlay's camera cannot reach any of it.** Mean |dRGB summed over channels|
> per face, at the snap frame:
>
> | arm | +X | -X | +Y | -Y | +Z | -Z | whole-cube pop |
> |---|---|---|---|---|---|---|---|
> | legacy snap | 76.2 | 35.2 | 34.5 | 87.6 | 49.8 | 50.4 | **18.54** |
> | (a) glide | 43.5 | 29.9 | 32.8 | 76.5 | 41.7 | 42.6 | **14.83** |
> | (b) dissolve | 0.9 | 1.6 | 1.4 | 1.6 | 1.6 | 1.5 | **0.48** |
>
> Glide halves the +X face (the one the mech fills, 76.2 → 43.5) and leaves every
> other face essentially untouched. That is the whole story in one row.
>
> **(b) CROSS-DISSOLVE THE CUBE — SHIPPED (`--env_dyn_fade_mode=1`, default).**
> Keep the pre-snap cube, blend it into the post-snap one, leave the point alone;
> every frame is then one coherent cube. **38× less change at the snap frame.**
> `docs/img/envmap/envfade_snap_vs_dissolve_strip.png` — the +X face across the
> re-bake: a hard cut (hull → room in one frame) against a ramp.
>
> **FADE LENGTH, `--env_dyn_fade`, DEFAULT 16 BY MEASUREMENT.** Peak
> consecutive-overlay change inside the fade window: N=4 **13.58** (a SPIKE —
> a fade this short is worse than none at that frame), N=8 10.03, N=16 10.09,
> N=32 **5.03**. Over the five overlays after the snap, N=16 gives
> 0.50/1.41/2.90/4.22/5.60 against the **~6.2 the scene moves by itself** in those
> same frames — the ramp stays under the motion already there. NO NEW POP AT THE
> END: the N=16 tail (5.60/7.62/8.71/10.09) converges onto the legacy arm's own
> values for those frames (6.36/7.85/8.66/10.03), i.e. what is left is the
> content's motion, not the fade's. N=32 is smoother still if the ramp ever reads.
> The fade is indexed on `dynFrame`, never wall-clock — a wall-clock fade would
> make the determinism gate red by construction.
>
> **COST — CAVEATED, because the box was not mine.** Other agents held the machine
> at load **13→55** throughout. An interleaved min-of-6 was attempted and
> ABANDONED: round 1 alone spread 931–5519 ms across arms. Least-contaminated
> sample (per-arm min-of-6, total overlay ms across the 61-frame walk): off
> **919.29**, glide16 **917.47** (inside noise — gliding is free), dissolve16
> **928.32** → **+9.03 ms over ~8 fading overlays, ≈1.1 ms each**. Treat that as
> an order of magnitude, not a number. Load-independent and exact: the blend
> touches 6·256² = **393 216 texels**, and the refilter widens from the ~2.7
> touched faces to all six (**58 061 → 129 024** texels). Memory **1.57 MB** per
> fading probe, freed when the fade ends.
>
> **GATES.** Pins, same worktree whose control binary reproduced all four exactly
> earlier today, run 1 discarded: greets `778fa6ac…`, greets+env_refl
> `e5f38b40…`, fountain `8db68ccb…`, city `3cbe42b1…` — **4/4 byte-identical,
> 2/2 each**. Determinism **24/24 on one value**
> (`9a30e9dc549c8167d6fadc71f576fed0`), so the ramp replays exactly.
>
> **TWO TRAPS, both of which silently produced a clean-looking wrong answer.**
> (1) **zsh does not word-split an unquoted `$VAR`**: `EX="--a=1 --b=2"; ./DEMO
> $EX` passes ONE argv token, both flags are lost, and `--strict_flags` did not
> catch it — a whole arm of the first A/B was measuring defaults while printing a
> plausible trace. Use `EX=(--a=1 --b=2); "${EX[@]}"`. (2) The snap is at TICK 29,
> not overlay 14: the canopy is SCHEDULED (~1 overlay per 2 ticks), so an
> overlay-indexed event is at ~2× that tick. The first pop measurement sampled
> t=7011..7022, returned all four arms **byte-identical**, and nearly bought the
> conclusion "the fade does nothing".
>
> **TOOL.** `--env_dyn_dump_seq` writes a frame-indexed copy of the selected
> store's live cube. Judging anything TEMPORAL used to cost one process per frame,
> each replaying the walk from its start — MEASURED at ~60 s/frame here, ~40
> minutes for one 13-frame strip. It is now one run, and that is the only reason
> the shapes could be compared at all.

> ## 2026-08-12b — EVERY OVERLAID PROBE HELD THE MECH TWICE: ONE COPY LIVE, ONE FROZEN AT BAKE TIME
>
> User: *"for the mech dynamic env bake - you forgot to move the camera - only
> some of the mech parts are actually changing the height - I see some mech parts
> changing height, while some don't."* His line:
> `./DEMO --greets-displace --scene-greets --env_probe_center --env-dynamic`.
>
> **THE HANDED-DOWN HYPOTHESIS IS DEAD, and it is dead by census, not by
> argument.** The suspicion was partial mover classification — that the mech is a
> multi-submesh assembly and the submeshes not carrying their own spline stay in
> the followed canopy probe's static capture. They do not. `isDynamicForBake`
> walks the PARENT CHAIN, and greets parents the whole mech under one null:
> `mech null → Hull2.lwo → {Hull.lwo, L_leg1 → L_leg2, R_leg1 → R_leg2}`. All six
> meshes classify as movers, all six were already excluded (ece0dc27), and the new
> `[ENVDYN-CENSUS]` reports **0 mover meshes in `Hull.lwo::cockpit_upper`'s static
> capture** in every arm. The 0.55 % that *does* differ between
> `--env_bake_include_animated` arms is 2 147 texels on the DOWN face only — the
> reflective floor, whose own probe carried the ghost: 1-bounce inter-reflection,
> not misclassification.
>
> **WHAT IT IS.** `--env_bake_include_animated` (default **ON** since 2026-08-09)
> lets the movers into ordinary probe bakes. `--env_dynamic`'s overlay then draws
> those same movers live over the retained static master, every frame. So each
> flagged probe holds the mech **twice**, and the second copy is frozen at bake
> time. `envProbeOwnerIsMover` (ece0dc27) exempted exactly one probe — the one
> that *rides* the owner — and left the other four.
>
> It is not a cosmetic duplicate: `overlayComposite` resolves the two by DEPTH
> (`win = rendered && mZ >= sZ`). The frozen copy contributes its own depth to
> `sZ`, so wherever the ghost is nearer the probe it **wins and the live mech is
> discarded behind it**. That is his sentence: the parts that do not change height
> are the ghost showing through.
>
> **THE CENSUS**, his line, t=7000..7060. `[ENVDYN-CENSUS]` counts what each
> static capture kept and names it. Movers = 6 mech meshes + `__discoBall`:
>
> | probe | overlaid? | mover meshes in the static capture — BEFORE | AFTER |
> |---|---|---|---|
> | `Hull.lwo::cockpit_upper` | yes (followed) | **0** (already fixed, ece0dc27) | **0** |
> | `momy-1` | yes | 7 meshes, 22 032 faces | **0** |
> | `momy-2` | yes | 7 meshes, 22 032 faces | **0** |
> | `stairs` | yes | 7 meshes, 22 032 faces | **0** |
> | `screen emiter` | yes | 7 meshes, 22 032 faces | **0** |
> | `amudim` | **no** | 7 meshes, 22 032 faces | 7 meshes (unchanged — legacy, by design) |
>
> **THE DEPTH TEST, MEASURED**, `stairs`, 60 overlays: the live mech rasterises
> **688 339** texels in BOTH arms — same live population, which is the control —
> of which **54 104 lose the depth test with the duplicate in and 5 899 with it
> out**. 48 205 texels of live mech were hidden behind its own ghost; survivors
> 1 283 120 → 1 384 120.
>
> **THE FROZEN POPULATION, MEASURED** — texels covered by mech at t=7020/7040/7060
> *and* pixel-identical across all three (`stairs` cube, one bake point):
>
> | arm | mech-covered at some t | covered at all 3 | **FROZEN** |
> |---|---|---|---|
> | before | 66 785 | 34 949 | **26 905 = 40.3 % of the mech** |
> | after | 44 182 | 3 218 | **345 = 0.8 %** |
>
> Control: the CANOPY probe, which this change does not touch, is **0.3 %** frozen
> over one bake interval — it never had the defect, which is the second thing that
> kills the handed-down hypothesis.
>
> `docs/img/envmap/envdyn_ghost_stairs_upface_strip.png` — the up-looking face
> across the walk, before/after: a stationary mass of black limbs plus a moving
> mech, then one mech that moves.
> `docs/img/envmap/envdyn_ghost_stairs_atlas_strip.png` — the whole cube.
>
> **THE FIX: `--env_dyn_static_exclude` (default 1).** One rule, about
> POPULATIONS rather than ownership — *where the overlay is live it is the SOLE
> source of movers, so no mover may be baked into a master the overlay composites
> onto.* `envProbeStaticMustExcludeMovers` ORs the new term beside
> `envProbeOwnerIsMover`, which stays (it must hold even with the overlay off).
>
> **MAGNITUDE — READ THIS BEFORE EXPECTING A BIG PICTURE.** The duplicate is large
> in the PROBE and small on SCREEN at every pose measured: **221 px > 12/765 over
> 16 frames** at the scripted pose (max Δ 153), and **0 px** at the momy pose
> (`FDS_GREETS_CAM="-12.1,3.2,-27,0,-0.06,-1"`). That is consistent with the
> 2026-08-12 entry below, which priced the mech's share of the stairs' env term at
> 0.03-0.3 LSB. **Trap for the next agent: a first pass measured "max Δ 617, mean
> 440" at the momy pose and it was the PROFILER HUD digits, which differ run to
> run — always `--profiler=0` for an A/B on frames.** So: the wiring defect is
> real, measured, and fixed; whether it is the thing his eye caught is his call,
> and `--no-env_dyn_static_exclude` restores the old behaviour for a live A/B.
>
> **NOT FIXED, RECORDED.** The followed canopy probe's overlay draws the owner's
> own assembly from a capture point glued to that owner: the mech covers **23.9 %
> of its own cube** (9-72 % per frame), and at each re-bake the point snaps ~1 u
> and the coverage jumps in one frame (36 864 → 98 222 texels). Coherent within a
> bake interval (0.3 % pinned), so it is a POP, not a frozen population — a
> separate look call, not this bug.
>
> **GATES.** Pins run as a DIFFERENTIAL against a control binary built from the
> parent commit in the same worktree (shared `Runtime/`, so the city cube is a
> common input), run 1 discarded: greets `778fa6acd85a69cf241babefcdaf598e`,
> greets+env_refl `e5f38b40179fad4d3705dd84d816e155`, fountain
> `8db68ccb59416e9a44037e9f387b7bd9`, city `3cbe42b166847e40f7071eedb48d613c` —
> **all four byte-identical before vs after, 2/2 each, and all four match the
> recorded table.** Byte-null holds because the term is ANDed with `--env_dynamic`
> (compile-default OFF, greets-only). Determinism, 24 runs each on the walk
> t=7000..7060: `stairs` live cube **24/24 `4b90d0ce3a10f406b1913606f9c2e9bb`**,
> canopy live cube **24/24 `a93e8bfcda64647dbb4109135aaf3874`** — one value each,
> so the new exclusion is deterministic and did not open a race.

> ## 2026-08-12 — THE MECH IS IN THE STAIRS PROBE AND IN THE STAIRS' REFLECTION; IT IS ~2 ORDERS OF MAGNITUDE BELOW ONE LSB
>
> User: *"env probe center still doesn't show the mech on the stairs."* His line:
> `./DEMO --greets-displace --scene-greets --env_probe_center --env-dynamic`.
> **The observation is real. Every stage the suspicion pointed at is not.**
> Measured on his line, greets, `--greets-displace` throughout.
>
> **1. THE CAPTURE POINT IS FIXED, AND THE +Y FACE DOES GET THE MECH.** Same
> frame, same mech pose (43.1 4.5 -62.1), `stairs` static cube vs live cube,
> per-face texel diff:
>
> | arm | stairs capture point | mech texels in +Y | where they land |
> |---|---|---|---|
> | shipping | (45.4 2.3 -54.9) | **0** | 2026, all in -Z |
> | `--env_probe_center` | (42.6 0.4 -62.1) | **12442 (4.75 %)** | +Y 12442, +X 1413 |
>
> `docs/img/envmap/stairs_upface_probe_center_pair.png`. e0abd02 reproduces.
> The MERGE picks the first material in MatLib order, so the shared store sits on
> `stairs` (42.6 0.4 -62.1), not on `stairs::mirUV` (43.4 1.9 -63.5) and not on
> their area-weighted union (43.0 1.2 -62.9) — order-dependent, worth knowing,
> and **not** the defect: the union point is 1.5 u HIGHER, which would drop the
> overhead mech from 54 deg elevation to 40 deg, i.e. OUT of +Y.
>
> **2. THE OVERLAY ROUTES IT, EVERY SCHEDULED FRAME.** Across t=6000..7100 the
> `stairs` store took 338 / 1042 / 1407 / 3138 / 33045 / 13855 mech texels into
> 2-6 touched faces. Not starvation: the legacy scheduler skipped it on 5 of 12
> frames (OWNER-OFFSCREEN) but never on a frame where the stairs were on screen.
>
> **3. THE STAIRS READ THAT STORE, IN THE SAME FRAME.** `EnvDynamic_Overlay` runs
> pre-Transform (GREETS.CPP:3969), so the composite is visible to the kernel that
> frame. Proof rather than code-reading: under `FDS_ENV_GRID=1` — where an
> overlaid face reverts from synthetic grid to real room, a huge colour move —
> toggling `--env-dynamic` changes **86.8 % of 1 935 277 `stairs::mirUV` pixels**
> and 100 % of `stairs`.
>
> **4. AND YET THE MECH MOVES EXACTLY ZERO PIXELS.** Real content, `--env-dynamic`
> on vs off, HUD excluded: **0 of 614 461** stairs pixels at t=6800 (natural
> camera), **0 of 1 937 404** at a pinned steep top-down pose over the stairs at
> t=6900. Same runs, `--no-env_refl` moves 100 % of them, so the env term is
> live and large: mean |dRGB| **40.8** (`stairs`) / **47.7** (`stairs::mirUV`) out
> of 765. The mech's share of that is below one LSB.
>
> **THE ARITHMETIC.** The mech is 0.2-2 % of the cube's texels, box-filtered into
> the mips a rough surface samples, weighted by the surface's env term (~14 LSB
> per channel): 0.03-0.3 LSB. It cannot survive 8-bit output. Compounding it at
> the natural pose, the visible stairs reflect SIDEWAYS, not up — per-face
> classification of the env term at t=6800: `stairs` **87.7 % +X, 8.9 % -Z,
> 0.0 % (1 px) +Y**; `stairs::mirUV` **52.0 % -Z, 27.9 % +X, 1.6 % +Y** — while
> the mech sits in +Y/-X. A floor viewed at a grazing angle reflects the horizon,
> not what is above it. But that is secondary: the steep pose reads +Y heavily and
> still moves 0 pixels, so MAGNITUDE is the binding constraint.
>
> **NOT FIXED, AND DELIBERATELY.** Nothing here is a wiring bug to repair — the
> levers are look calls that are his: make the mech brighter in the probe (it is a
> dark silhouette on a dark wall), raise the stairs' reflectance/`hdr_refl_gain`,
> or lower their roughness so they sample a sharper mip. Recorded so the next
> agent does not re-derive the four stages.

> ## 2026-08-12 — THE FREE-EDGE BULGE IS A SLIDE, NOT A SIGN; AND THE >90 deg SEAM IS NOT FLAT, IT IS CRACKED OPEN
>
> Two directives. **(1)** *"--greets_displace_free_edge - it makes most of the
> sites better, but for the specific pose I sent you, it adds a bulge similar to
> the gpu one - which is less than optimal."* **(2)** *"the >90 deg flattening is
> still an issue - the height map from the texture actually means that there
> should be a gap there ... I think we still should support this scenario."*
>
> ### (1) THE SIGN CLAMP WAS THE OBVIOUS FIX AND THE MEASUREMENT KILLED IT
>
> The hypothesis handed down was that the freed border swings both ways, so a
> stone plateau (h > mean) pushes it OUTWARD — the bulge GpuBench had before
> `--tess_border_ramp`. Implemented as a d <= 0 clamp on freed borders, then
> measured: it moved **134 of 1594** freed `rooms` verts, and at t=5967 it left
> **every row above 687 byte-identical** to the old arm. It never touched the
> jamb.
>
> **WHAT DOES.** A dump of every freed vert in the jamb box (`[STONE-FREEV]`,
> behind `--greets_displace_junction_census`) shows **178 of 183 already
> displaced NEGATIVE** — there was no outward push to clamp. What they carry is
> a DIRECTION: the displacement rides the SMOOTHED vertex normal, which at a
> patch border is averaged with whatever else the authored mesh joins there. The
> verts on the wall plane x=17.898 ride **N ~ (+0.894,+0.419,-0.155)** — 26.5 deg
> out of their own plane, tilted mostly DOWN THE EDGE, and the border runs in y.
> So a pure recess of -0.10 slid the vertex **~0.045 world units ALONG its own
> border line**. That slide is the swollen doorway reveal and the tab at the
> lintel. Mesh-wide: **1579 of 1594** freed `rooms` verts were sliding, the worst
> at **|cos| 0.968** against its own edge — 97% slide, 3% relief.
>
> **THE FIX, folded into `--greets_displace_free_edge` (SEMANTICS CHANGED — the
> flag now means free + no-slide + recess-only).** A freed border vertex may move
> ACROSS its border line, never along it. Recess-only is kept as a second,
> smaller constraint because it cannot cost anything.
>
> **REJECTED ON MEASUREMENT.** Removing the WHOLE tangential component (riding
> the face plane normal) tears the border off neighbours that share its position
> without sharing an edge: **1408 background pixels at t=5967 against 46**. A
> coplanarity gate does not save it — all 1594 verts pass it, because their faces
> ARE coplanar; it is the authored vertex NORMAL that is skewed.
>
> **BACKGROUND (z==0) PIXELS, old free arm -> new**, his five poses: t=5799
> 41 -> 27, t=5869 7 -> 8, t=5929 0 -> 0, t=5967 75 -> 46, t=5987 0 -> 0. Figures:
> `docs/img/fogwt/freewt_t5967_slide_before_after.png`,
> `freewt_t5987_slide_before_after.png`,
> `freewt_t5869_goodgrooves_preserved.png` (his grooves untouched),
> `freewt_t5869_floorborder_before_after.png` (the floor's freed border stops
> warping the tile grid at the wall base, which the old arm did).
>
> **HONEST CAVEAT, NEW AND POSE-DEPENDENT.** free_edge's crack cost is far larger
> away from his five poses. At the corridor poses used for directive (2) its OWN
> contribution is **868 px** (pose A) and **702** (pose B) — against 46 at t=5967.
> The old arm was worse at the same poses (1270 and 1125), so this change reduces
> it by a third, but the flag is not cheap everywhere.
>
> **GPU PARITY: `--tess_border_ramp` does NOT need the recess-only treatment.**
> Re-measured at HEAD, silhouette x per row, rows 500-640: t=5967 CPU oracle
> median **1516** / GPU ramp=0.15 **1520** (4 px) / GPU ramp=0 **1424**; t=5987
> CPU oracle **1172** / GPU ramp=0.15 **1174** (2 px) / GPU ramp=0 **1157**. The
> ramp drives the whole displacement to zero at the border — both signs — so it
> already subsumes a sign clamp, and it is slide-free for the same reason. The
> ramp arm is the analogue of the CPU's PINNED arm and still tracks it. What has
> NO GPU counterpart is the CPU's new free_edge arm (t=5967 CPU free median
> **1475**, 41 px off the oracle by design — that is the jamb opening; t=5987
> **1170**, and std 7.55 against the pinned arm's 8.36, i.e. straighter than the
> pin while carrying relief). Porting it needs ramp=0 on FREE edges only plus the
> de-slide; not built.
>
> ### (2) THE >90 deg SEAM IS A HOLE, AND `--greets_displace_seam_weld` CLOSES IT
>
> The 12 split-vertex seams sit on two vertical 91.10 deg corners at
> **x = +-2.469, z = -4.937**, each cut into three segments (mid y 1.265 / 3.765 /
> 6.233). Poses that put one in profile: `FDS_GREETS_CAM=
> "-1.5,3.2,-8.5,0.743,-0.037,0.667"` (A) and `"1.5,3.2,-8.5,-0.743,-0.037,0.667"`
> (B), t=5967.
>
> **THE SHIPPING ARM DOES NOT FLATTEN THAT CORNER — IT TEARS IT OPEN.** Under
> `--greets_displace` the pinned seam shows a see-through gash running down the
> corner: **1992 background pixels at pose A, 1948 at pose B** (0 at his five
> review poses, which is why it had never surfaced).
> `docs/img/fogwt/seamwt_t5967_poseA_crack_closed.png`,
> `seamwt_t5967_poseB_crack_closed.png` (holes painted red),
> `seamwt_t5967_poseA_corner_zoom.png`.
>
> MECHANISM: the border pin holds the SUBDIVISION verts on each side at zero, but
> the two coincident ORIGINAL corner verts are NOT pinned (`pinnedZero` over
> originals covers only non-target incidence and cross-material coincidence), so
> they displace along their own distinct vertex normals and separate.
>
> **`--greets_displace_seam_weld` closes it: 1992 -> 14, 1948 -> 2.** It merges 4
> target-only verts, converting 2 of the 6 seam segments to index-interior
> (`rooms` 211 -> 213 welded interior edges, 12 -> 8 split edge entries). It is
> byte-identical to shipping at 4 of his 5 review poses (5869, 5929, 5967, 5987);
> only t=5799 moves.
>
> **T-JUNCTION SAFETY IS NOT THE BLOCKER — it is already solved.** The newly
> interior seam edges go straight through the existing S4a seam-union / heal
> machinery: fan<->edge seam-hole sides **25 -> 27**, union-welded splits
> **539 -> 565**. No matched-tessellation work is needed first.
>
> **BUT THE NOTCH DOES NOT OPEN AT FULL DEPTH.** Corner-apex depth down the seam
> (pose A, rows 300-890, world units): welded minus pinned is **+0.0000 to
> +0.0253**, with no per-mortar-row oscillation, against **0.13 u** grooves on the
> flat wall. Two measured reasons:
> * **Only 2 of the 6 segments weld.** The census under `--greets_displace_seam_weld`
>   lists exactly the BOTTOM (mid y 1.265, floor end) and TOP (mid y 6.233,
>   far-side `siling`) segments as still split at both corners; only the MIDDLE
>   ones (mid y 3.765) merged. The weld excludes any vertex incident to a
>   non-target face, on purpose, to protect the cross-material neighbour pin.
> * **Along the welded segments only 14 verts were freed** (displaced verts
>   30472 -> 30486), and they ride the 45.55 deg bisector, so they project
>   cos(45.55) = **0.70** of their depth onto either wall.
>
> **VERDICT: SUPPORTED, PARTIAL, AND WORTH TURNING ON ANYWAY** — it fixes a real
> hole regardless of the notch. Full notch support needs the weld to take verts
> shared with `floor`/`siling` too. Merging position-coincident verts MOVES
> nothing (identical positions); it re-indexes and averages normals, and the
> averaged normal would then be seen by the floor/ceiling faces as well. The
> identified next step is to remap only the TARGET faces onto the canonical
> vertex and leave non-target faces on their own copy — NOT built, NOT measured.
>
> **COMPOSITION.** seam_weld and free_edge are independent populations and
> compose: at the seam poses weld+free gives 882/704/255/3 background px, which
> is free_edge's own cost (868/702/255/3) with the seam crack removed.


> ## 2026-08-11 — THE ANGLE RULE IS REAL AND MEASURED (WELDED 0-90.00 deg, SPLIT 91.10 deg), BUT THE CORNER HE POINTED AT IS A THIRD CLASS: A DOORWAY JAMB
>
> User: *"the original mesh doesn't have a gap, but similar places in the texture
> in other faces does generate gaps, and in the pose I gave you it doesn't -
> prolly due to the angle between the two adjacent faces"*, then *"for the wall it
> flips where the angle between the walls jumps to > 90 degrees (or even more)"*.
> Positive sites he supplied (gap shows, looks right): t=5799, t=5869, t=5929.
> Negative: t=5967, t=5987 (the round-1 poses). All five are one continuous walk.
>
> **HIS RULE IS A REAL PROPERTY OF THIS MESH, AND THE NUMBER IS EXACT.** New
> `--greets_displace_junction_census` walks the ORIGINAL stone at bake time and
> classifies every edge. For `rooms`: **211 WELDED interior edges, dihedral
> 0.00-90.00 deg** (the two faces share vertex INDICES, so the junction displaces
> and the groove carries across) and **12 SPLIT-VERTEX seam edges, dihedral
> 91.10 deg — min = max, nothing in between** (the two faces meet at the same
> POSITION with distinct indices, so BOTH sides present as single-use edges, both
> classify as authored borders, and both pin to exactly zero). The topology flip
> in this mesh sits precisely at >90 deg, which is what his eye read. `floor`:
> 23 welded, 2 split, all at 0 deg. Plus 154 genuinely OPEN borders on `rooms`.
>
> **THE MECHANISM IS THE AUTHORED-BORDER ZERO-PIN, PROVEN BY A/B.** `MeshOps.cpp`
> `isBorderEdge` -> `pinnedZero` on every subdivision vertex along the edge: a
> line held at zero cannot be cut by a mortar groove, so the junction reads as a
> smooth sealed edge. New `--no-greets_displace_border_pin` prices it: at t=5967
> **387 635 px change (18.69%)**, at t=5987 **271 734 px (13.10%)**, concentrated
> in the corner columns, and the jamb silhouette goes from dead straight to
> wandering. Default arm reproduces round 1 byte for byte (t=5967
> `c0beec384141e4f18525a84e6b07a9bc`, t=5987 `4a12c7c358840bb30118518a2454924d`).
>
> **BUT THE CORNER IN HIS TWO POSES IS NOT A >90 deg JUNCTION AT ALL — IT IS A**
> **DOORWAY JAMB.** The census localises it: the wall plane at x=17.898 carries
> vertical OPEN borders at z=-58.014 and z=-62.952 (mid y 2.469, len 4.937) and a
> lintel at (17.898, 4.937, -60.483) len 4.937 — a 4.94-wide, 4.94-high opening,
> and the camera at (18.752, 3.210, -58.851) is standing in it. A jamb has NO
> second target face on the far side, so it is pinned by the same rule for a
> third reason. The 12 split-vertex seams all sit near z=-4.937, nowhere near
> these poses — confirmed by `--greets_displace_seam_weld` (merges them; **byte-
> identical at t=5869/5929/5967/5987**, only t=5799 moves).
>
> **THE FIX, IMPLEMENTED AND MEASURED, DEFAULT OFF — HIS CALL.**
> `--greets_displace_free_edge`: the pin's job is to stop a T-junction opening
> against a neighbour subdivided differently, and that argument needs a
> neighbour. A single-use target edge with NOTHING on its far side (no coincident
> non-displaced edge, no position-coincident target edge) is a FREE SILHOUETTE
> edge and cannot crack against anything, so it displaces. Measured at his five
> poses: **the jamb opens** (`docs/img/fogwt/juncwt_t5967_pin_vs_free.png`,
> `juncwt_t5987_pin_vs_free.png`) and **the good sites are preserved** — the deep
> dark mortar groove at t=5869 is unchanged to the eye
> (`juncwt_t5869_goodsite_preserved.png`).
>
> **TWO HONEST CAVEATS ON THAT FIX.** (1) It is not free: new background (z==0)
> pixels appear — **75 px at t=5967, 41 at t=5799, 7 at t=5869**, zero at the
> other two — so a small crack does open. (2) The silhouette it produces is a
> COARSE WANDER, not the crisp per-stone notch the positive sites show; the jamb
> leans in and out over its height rather than stepping at each mortar row. So it
> answers "why is this corner different" and it does unseal the corner, but
> whether it is the LOOK he wants is his call, not a measurement.
>
> **WHAT THE GAP HE LIKES ACTUALLY IS (measured, t=5869).** Not a hole: the
> `FDS_SNAPSHOT_ZDUMP` across it is continuous (7.651 -> 7.861 world u over 26 px,
> zero background pixels). It is a deep dark mortar groove whose depth-residual
> shows a real recess of about 0.04 u against the local plane
> (`juncwt_t5869_depth_residual.png`, `juncwt_t5869_the_gap_he_likes.png`). The
> welded 27-32 deg junctions of the curved wall at x=5.5..12.7, z=-49..-59 are
> what let it read that deep.
>
> All four new flags are default-off / no-op; the shipping arm is byte-identical.
>
> **THE GPU BULGE IS THE SAME MECHANISM, AND THE CONVENTION HYPOTHESIS IS DEAD.**
> He also reported *"if you look in the gpu renderer - it's actually bulges the
> mesh there"*, and the standing hypothesis was that GpuBench displaces against a
> different reference. MEASURED: the two arms agree term for term — both compute
> `amp*(h-mean)` along the interpolated vertex normal, both at amp 0.300 and mip
> 2, and the mean is the SAME NUMBER (GpuBench reports `rooms` height mean
> **0.5491**; the CPU's mipMean over `greets_wall_h.png` is **0.549053**, and a box
> reduction preserves it exactly at mips 0/1/2/3). Textures are RGBA8Unorm, so no
> sRGB decode either. What GpuBench was missing is the CPU's PIN: it displaced the
> authored patch borders that `DisplaceStoneSubdiv` holds at zero, so at the jamb
> the CPU's value is 0 and the GPU's is up to **+0.035 world units outward**.
> Ported (`--tess_border_ramp`, default 0.15; 196 of 678 patch edges classify as
> borders). Silhouette x per row, same extraction on all three arms — t=5967 rows
> 500-640: GPU no pin span **203 px** std 62.68 median 1463 -> GPU pinned span
> **78 px** std 7.08 median **1520**, against the CPU oracle span 72 px std 16.40
> median **1516**. t=5987: 233/64.12/1228 -> 125/40.33/**1174** against
> 123/21.25/**1172**. Within 4 px and 2 px of the oracle.
> `docs/img/gputess/borderpin_t5967_before_after_cpu.png`,
> `borderpin_t5987_before_after_cpu.png`. (These absolutes are NOT comparable to
> round 1's 238/24 px — the extraction method differs.)

> ## 2026-08-11 — THE SHARDS WERE NOT DIM, THEY WERE EMPTY: A PER-VERTEX CONE CULL DECIDING FACE VISIBILITY
>
> Sent to root-cause the residual the mirror-break commit (`983cdb4`) left
> open — "the offscreen deferred bake at 64² is ~21 luma darker than the
> forward bake, cause unidentified". **The premise was wrong in both direction
> and size.** The shard reflection was not being shaded too dark; **two thirds
> of it was never drawn at all**, in the forward bake and the deferred bake
> alike, and once drawn the offscreen bake reads BRIGHTER than the main pass,
> not darker.
>
> **THE MEASUREMENT** — same bracket the previous commit used
> (`--repro=greets@t=3122 --repro_from=3112 --repro_settle=0`,
> `FDS_GREETS_CAM="28.8,10.8,-62.85,1,0,0"`, square-on to the shatter screen),
> over a 1258×767 panel window derived as the intersection of the
> mirror-on/mirror-off and shards-on/shards-black change masks:
>
> | | panel-window luma |
> |---|--:|
> | pre-break, intact half-silvered mirror | 73.07 |
> | MAIN deferred pass from the shard's own reflected eye (`FDS_GREETS_CAM="68.79,10.8,-62.85,-1,0,0"`) | 73.86 |
> | **break+1, shipped (cull on)** | **24.74** |
> | **break+1, fixed (cull off)** | **86.37** |
>
> The reflection ATLAS itself goes **21.65 → 69.66** mean luma. Look at the
> cells and it is not a brightness story at all: before, each 64² cell is
> black with a few flat untextured quads; after, each cell holds the reflected
> room with its brick texture. Strips:
> `docs/img/fogwt/shardcull_t3122_bracket.png`,
> `shardcull_t3122_zoom.png`, `shardcull_t3122_atlas.png`.
>
> **ROOT CAUSE — `Transform.cpp`'s `g_reflVertCull` block decides FACE
> visibility from VERTEX positions.** Each shard bakes through a very narrow
> off-axis cone: the window is one 1/238th fragment of the panel seen from
> ~20 units, so the half-angle is ~1°. The cull rejected every vertex outside
> that cone, stamping it `TPos=(0,0,1)`, `PX=PY=-1` with all frustum-out bits
> set so its faces would cull. greets's room is wall/floor/ceiling QUADS whose
> corners sit metres off the axis — so a quad whose INTERIOR covered the
> entire shard view had all three corners rejected and vanished. The quads
> that did survive (one corner in, two out) rasterized THROUGH the fake corner
> positions, which is the flat stretched look in the BEFORE zoom. The test is
> only sound when faces are small against the cone, and nothing in greets is.
>
> **FIX: `--shard_cone_cull`, default 0.** The sound cull was already there and
> still runs — the mesh-level off-axis bounding-sphere frustum test inside
> `Transform_Objects` (`g_offAxisFrustumCull`), which rejects whole objects
> conservatively. `1` restores the legacy behaviour exactly (24.74, verified).
>
> **TRIED AND REVERTED — the conservative per-vertex variant.** Widening the
> cone by the mesh's world DIAMETER (no face can reach further from its own
> vertex than that, so no covering face can have all corners rejected) is
> correct about the *culling* and still measured only **58.37**: it does
> nothing about the straddlers, whose rejected corners keep their fake
> positions. Per-vertex marking cannot be made sound here. The correct
> accelerator is a per-FACE test (face bounding sphere vs cone); nobody has
> written it, and this records why it is the shape needed.
>
> **COST OF CORRECTNESS.** `FDS_SHARD_REFL_PROF`, min-of-6 interleaved, run 1
> discarded, load 12.8: the shard bake goes **4.0 ms → 14.5 ms**. The cull's
> speed was the speed of drawing almost nothing. Note this also corrects the
> record in `983cdb4`: its "deferred bake 20.4 ms vs forward 188.3 ms" was
> timed with the cull eating the geometry.
>
> **BLAST RADIUS: THE SHARD BAKE ONLY.** `g_reflVertCull` is set at exactly two
> call sites, both in `MirrorShatter.cpp`; the mirror RTT panels
> (`GreetsMirror.cpp`) set only `g_offAxisFrustumCull` and never took this
> path. Measured, not just read: a NON-shatter greets frame is **byte-identical**
> under `--shard_cone_cull` and the default (`d689b64b…` both ways), and
> `render_gate.sh`'s `mirrortest` — which covers the RTT — PASSes unchanged.
>
> **THE RESIDUAL, ROOT-CAUSED AND NOT FIXED: the offscreen shard bake never
> runs the HDR round-trip, so it is on a different transfer function from the
> frame it must match.** greets defaults `--hdr --hdr_linear`, so the main pass
> writes linear radiance and `Render_TonemapToVPage` applies exposure → ACES →
> sqrt encode. `Hdr_WritableFor` gates every `g_hdrBuf` write on the CURRENT
> dims matching, so at 64² the shard bake silently takes the LDR combine
> instead. The mirror RTT does NOT have this problem — it brackets its bake
> with `Hdr_BeginFramePass(texW,texH)` / `Hdr_ActivateNoFog()` /
> `Render_TonemapToVPage()` (`GreetsMirror.cpp:3273-3286`); `MirrorShatter` has
> no such bracket. Priced: fixed mosaic **86.37** vs the main pass's **73.86**
> from the same eye, i.e. **+12.5 luma, the offscreen bake is BRIGHTER**; under
> `--no-hdr`, where the main pass loses ACES+sqrt and drops to 43.28, the
> mosaic barely moves (79.05) — which is the signature of a pass that is not
> following the frame's transfer function at all. NOT fixed here because
> `g_hdrBuf` is a single global and the shard bake runs N shards concurrently
> across the worker pool, so `Hdr_BeginFramePass` cannot be called per worker;
> the fix is a per-target HDR buffer threaded through `DeferredOverride`.
> Logged in `docs/OPTIMIZATION_BACKLOG.md`.
>
> **NOTE ON `--hdr` AS A NULL RESULT.** `983cdb4` lists `--hdr` among the
> toggles that measured null against the deficit. greets already sets
> `hdr=true` via `setDefault`, so `--hdr` is a no-op there; the toggle that
> moves it is `--no-hdr`.
>
> **WHILE IN HERE: the offscreen G-buffers stopped allocating lightmap planes
> nothing can read** (handoff from the setDefault audit, `0b466b7`). That commit
> established the atlas has ONE reader and TWO gates: `shadow_lightmap()`
> allocates the `lightmapMF`/`ST` planes, and
> `lmKernelEnabled = !shadow_dynamic() || shadow_lm_dynamic()` decides whether a
> pixel ever samples them — greets keeps the second one shut. The MAIN G-buffer
> escapes because `EngineGBuffer_Resize` runs at BOOT, before greets turns
> `shadow_lightmap` on. **The three OFFSCREEN builders do not** — the RTT slot
> (`GreetsMirror.cpp`) and the shard bake's serial + per-worker buffers
> (`MirrorShatter.cpp`) build LAZILY, after `GreetsApplyRunDefaults`, so they
> really were allocating, and Mekalele really was storing into them (`wantLm`
> gates on the plane pointers, `Mekalele.h:1320`). All three now use one shared
> predicate, `DeferredLightmapPlanesReadable()` (`DeferredCommon.h`), so they
> cannot drift from the kernel's gate.
>
> | | |
> |---|--:|
> | RTT slot `s_rttGB` (512² × 6 B) | 1.50 MiB |
> | shard per-worker (12 × 64² × 6 B) | 288 KiB |
> | shard serial `reflGB_` (64² × 6 B) | 24 KiB |
> | **total no longer allocated or written** | **1.80 MiB** |
>
> **Time: NULL, and said so.** Shard bake `FDS_SHARD_REFL_PROF` min-of-6
> interleaved against a control binary built with the old gate, load 18.9:
> planes-on 14.2 ms vs planes-off 14.3 ms — indistinguishable. The win here is
> the allocation, not the per-pixel store.
>
> **NOT ENTANGLED WITH THE DIMMING, and that is measured, not argued.** The
> shatter frame is **byte-identical** across this change (`2e63ef6f…` both
> ways) — removing the planes outright moved zero pixels, which is the direct
> proof that nothing was reading them and that a half-written plane could not
> have been feeding the composite. Positive control the other way: the
> force-open arm `--shadow_lightmap --shadow_lm_dynamic` still gets its planes
> and still diverges from the shipping arm (mean |d| 6.79, 7.6 % of px > 30
> over the panel window), so the gate opens when it should.
>
> **PINS: unchanged and re-verified** — greets `778fa6acd85a69cf241babefcdaf598e`,
> fountain `8db68ccb59416e9a44037e9f387b7bd9`, city `3cbe42b166847e40f7071eedb48d613c`,
> `render_gate.sh` ALL PASS (mirrortest `4ac809e5…`, conetest `b41894f9…`,
> halotest `166fa25a…`). **What that does and does not certify:** no pin recipe
> triggers the shatter, so none of them exercises the changed line. They certify
> no collateral damage; the fix itself is certified by the non-shatter
> byte-null above and by the measurements in this entry.

> ## 2026-08-10 — THE MIRROR-BREAK POP: THE SHARDS SHOW HALF THE REFLECTION THE INTACT SCREEN SHOWED, IN ONE FRAME
>
> His long-standing report ("the look before/after the break start is not
> consistent"). Bracketed on the REAL per-frame path: `--repro=greets@t=3122
> --repro_from=3112 --repro_settle=0` puts the auto-shatter exactly ONE frame
> before the dump, and the paused scrub freezes scene time — so pre-break and
> break+1 are the same scene time, same camera, **zero motion between them**.
> Camera square-on to the shatter screen (`P_TEXT.JPG#6`, area 172.5, plane
> x=48.795, the one `BuildGreetsShatter` picks):
> `FDS_GREETS_CAM="28.8,10.8,-62.85,1,0,0"`.
>
> **THE POP, QUANTIFIED** over the panel region (1289x873 px):
>
> | | mean \|d\| (sum3) | px>30 |
> |---|--:|--:|
> | baseline frame-to-frame motion, pre-break (4 pairs, 10 ticks apart) | 2.37 / 2.98 / 3.45 / 4.72 | 2.0-3.7 % |
> | **the break** (pre -> break+1, same t) | **135.25** | **86.8 %** |
>
> **~40x the scene's own motion**, and panel luma **78.81 -> 34.94 (-56 %) in one
> frame**.
>
> **ROOT CAUSE — `ApplyShardSilverGlaze` halved the reflection unconditionally**
> (`r = sr + (r >> 1)`, `MirrorShatter.cpp:94`). Ground truth: the main deferred
> pass rendered FROM the shard's own reflected eye
> (`FDS_GREETS_CAM="68.79,10.8,-62.85,-1,0,0"`) measures **73.75** luma over the
> panel window; the intact half-silvered panel shows **78.81**, i.e. essentially
> ALL of it; the halved shards showed **34.94 ~= 73.75/2**. The comment justified
> the halving as matching the screen's `litRGB + dst/2` — measurement says the
> screen it replaces does not halve.
>
> **Fix: `--greets_shard_refl_gain` (float, default 1.0; 0.5 = legacy).** The
> halving became a multiply in the same loop. Applied before the text composite,
> so text still rides on top at full strength.
>
> | gain | panel luma | mean \|d\| vs pre-break | px>30 |
> |---|--:|--:|--:|
> | 0.5 (legacy) | 34.94 | 134.40 | 86.9 % |
> | 0.8 | 48.55 | 101.29 | 80.6 % |
> | **1.0 (new default)** | **60.34** | **83.03** | **61.9 %** |
> | 1.2 | 72.20 | 88.33 | 68.2 % |
>
> 1.0 is both the principled value (no attenuation) and the per-pixel optimum —
> 1.2 gets closer on mean luma but WORSE on \|d\|, because the shards' own
> edge-on faces and crack lines cap what the panel can reach. **The pop drops
> 38 %** (135.25 -> 83.03) and the one-frame luma step goes -56 % -> -23 %.
> At the true break instant (shards still at rest, `--snapshot` + `FDS_GREETS_SHATTER=1`)
> the mosaic now reproduces the room: **60.38 vs 76.65**.
> Strips: `docs/img/fogwt/shardpop_t3122_bracket.png`,
> `shardpop_t3122_zoom.png`, `shardpop_t3122_atrest.png`.
>
> **FOUR CANDIDATES MEASURED AND OVERTURNED** (all at break+1, panel luma; the
> brief's leading suspect was the first one):
>
> | term | luma | verdict |
> |---|--:|---|
> | `--greets_shard_res` 64 -> 256 / 512 | 36.00 | **null** (+1.06) — resolution is NOT the pop |
> | `--greets_mirror_tint=0` (silver glaze) | 34.94 | **exactly null** — `sv` only scales the ADDED cast; it never gated the halving, which is why this looked like a dead end |
> | shard reflection camera basis (panel axes vs the shard's own jittered edges) | 35.01 | **null** (+0.07); tried and reverted. The edge basis is non-orthonormal (the two edges are not perpendicular) so the per-shard view matrix is sheared — real, but it moves no pixels here |
> | `--no-greets_displace_flat_mirror` | 77.97 pre-break | **null** — the intact mirror is not reflecting a different (flat) proxy |
>
> **`--no-shard_deferred` IS NOT THE ANSWER, AND ITS FLAG DOC IS BACKWARDS.** It
> does brighten the shards (34.94 -> 55.90) — but MEASURED with
> `FDS_SHARD_REFL_PROF=1`, min-of-27 frames, run 1 discarded, load 4.3-8.6:
> the forward bake costs **188.3 ms** against the deferred bake's **20.4 ms**.
> The flag's own text claims "~20ms vs ~6ms forward"; it is wrong by ~30x in the
> other direction. Left alone.
>
> **RESIDUAL, NOT FIXED.** Even at gain 1.0 the shard bake sits below the intact
> panel. The offscreen deferred bake at 64² is ~21 luma darker than the FORWARD
> bake of the same shards (55.90 vs 34.94 pre-fix), and `--no-shadows` /
> `--no-shadow_lightmap` / `--no-ssao` / `--hdr` / `--no-pbr` / `--no-env_refl`
> are all null against it; `--no-mips` recovers 4.4. Cause unidentified — the
> offscreen deferred path being dimmer than the main deferred pass at the same
> eye is its own bug and wants its own session.
>
> **PINS: unchanged, certified DIFFERENTIALLY** (default vs
> `--greets_shard_refl_gain=0.5`, identical to each other AND to the recorded
> values): greets `778fa6acd85a69cf241babefcdaf598e`, fountain
> `8db68ccb59416e9a44037e9f387b7bd9`, city `3cbe42b166847e40f7071eedb48d613c`,
> `render_gate.sh` ALL PASS (mirrortest `4ac809e5…`, conetest `b41894f9…`,
> halotest `166fa25a…`). **BUT SAY WHAT THAT DOES AND DOES NOT MEAN:** no pin
> recipe ever triggers the shatter, so the glaze never runs in any of them.
> The pins certify NO COLLATERAL DAMAGE; they are blind to the fix itself.
> Cost: min-of-3 shard-pass 21.1/21.9 ms (legacy) vs 21.2/22.9 (new), run 1
> discarded — inside the run-to-run spread at load 5-9.

> ## 2026-08-10 — THE CORNER HE WANTS TO SEE THROUGH HAS NO HOLE IN IT: 723 600 px OF DEPTH SAY THE WALL IS SOLID
>
> Report: at `FDS_GREETS_CAM="18.752037,3.21019745,-58.8513527,-0.892443955,
> -0.0741753578,0.445018977"` t=5967 and `"19.7497902,3.21076035,-59.0800819,
> -0.918940723,-0.0697668344,0.388175935"` t=5987, under `--greets_displace`, a
> gap between two bricks that should be see-through renders closed; "gpu also has
> the same issue, even worse". His F9 dumps: `/tmp/greets_dump_0_t5967.ppm`,
> `/tmp/greets_dump_1_t5987.ppm`. Reproduced at both poses (his dump and the
> `--snapshot` frame are the same picture — left two panels of
> `docs/img/fogwt/gapwt_t5967_corner_strip.png`).
>
> **THE TWO NOMINATED SUSPECTS ARE BOTH BYTE-NULL AT BOTH POSES.** `--no-greets_
> displace_seam_union` and `--no-greets_displace_neighbor_pin`, each alone:
> t=5967 all three md5 `c0beec384141e4f18525a84e6b07a9bc`, t=5987 all three
> `4a12c7c358840bb30118518a2454924d`. Not "small" — **0 of 2 073 600 pixels**.
> The flags DID take: the seam-union arm's `[STONE]` census reads `faces 68149,
> 455 T-junction pins, … (heal-only: 0 splits)` against the default's `68513,
> 214, … (union-welded: 539 splits)`. So no weld is bridging anything here.
>
> **MEASURED — THERE IS NOTHING BEHIND THAT WALL TO SEE.** `FDS_SNAPSHOT_ZDUMP`,
> the region right of the wall-end silhouette (x ≥ 1250, 723 600 px), at BOTH
> poses and on BOTH arms: **zero pixels farther than 4 world units.** Flat-arm
> depth range 1.107–3.170 u (t=5967) and 2.103–2.740 u (t=5987) — one continuous
> solid surface. The far wall behind is at 15.4 u; not one pixel of it shows.
> Picture: `docs/img/fogwt/gapwt_t5967_solidproof.png`.
>
> **WHAT HE IS LOOKING AT IS A CONVEX CORNER OF ONE SOLID WALL, NOT TWO PANELS.**
> `FDS_SNAPSHOT_GBUFDUMP`: every pixel from x=1100 to 1920 is the single material
> `rooms::mirUV`. The two faces meet at an authored edge at x=1520 with
> **continuous depth** in the flat arm (64959 → 64959 at y=400) — no slot, no
> sliver, nothing to weld shut. `--wire_viz=2/3` shows the same: two big quads,
> and the entire brick/mortar pattern is TEXTURE. Cross-section:
> `docs/img/fogwt/gapwt_t5967_corner_crosssection.png`.
>
> **VERDICT: this is an authoring question, not a renderer bug.** The relief is a
> scalar heightfield (`TEXTURES/greets_wall_h.png`, 1024², 8-bit L, min 0 max 172
> mean 140). A heightfield can recess a surface; it cannot open a hole through
> one — no discard, no alpha, no authored void. To see through between two bricks
> there has to BE a hole: either a real opening cut in the LWO, or an alpha-tested
> mortar material. Neither exists today. `docs/img/fogwt/gapwt_heightmap.png`.
>
> **THE GPU IS NOT DOING IT EITHER — ITS "GAP" IS A CRACK.** GpuBench `--tess`
> at the same cam: silhouette x per row wanders over a **238 px** span
> (std 13.54) against CPU `--greets_displace` **24 px** (std 3.90); both flat arms
> agree exactly (GPU 1157–1165, CPU 1157–1164 — a clean cross-validation of the
> two renderers' geometry). The GPU's extra motion is a torn ribbon plus a void
> at the corner (panel 4 of the strip) — and that void sits at x≈1500–1540 where
> the depth scan found nothing beyond 4 u, i.e. it exposes the same wall's own
> interior, not the outside. Tessellation crack, not a revealed opening.
>
> **TWO REAL SHORTFALLS FOUND ON THE WAY — both make the relief read shallower**
> **than the map, and both are separable from the verdict above:**
>
> 1. **`--greets_displace_mip=2` gives the mortar no floor.** The bake census at
>    the shipping default reads `plat/step/floor cells 8236/28168/0` — **ZERO
>    groove-floor cells**: the tessellation cuts a V and never a U, so a mortar
>    joint never reaches a flat bottom. At `--greets_displace_mip=0` the same wall
>    gets **15 438** floor cells and deeper relief (`[-0.153..+0.033]` vs
>    `[-0.131..+0.033]`), at 105 130 faces against 68 513. Visible: panel 6 of the
>    strip, and `docs/img/fogwt/gapwt_t{5967,5987}_cpu_mip0.png`. Cost not measured.
> 2. **The authored patch border is pinned to exactly zero displacement**
>    (`DEMO/MeshOps.cpp:2552`, `:2571`, `:3358` — `pinnedZero`), so the last cell
>    before every border carries no relief. Measured along y=400 approaching the
>    corner edge: Δdepth (displaced − flat) **−0.068 u at x=1440 → −0.053 →
>    −0.025 → −0.010 → +0.000 exactly at x=1520**, a linear ramp to nothing. That
>    is why the corner reads as a smooth sealed edge rather than a toothed one.
>    It is crack safety, so removing it is not free — untested here.
>
> **TOOL TRAP for the next agent: `--displace_viz` is BLIND to the `::mirUV`
> split.** Both modes draw nothing at all over the wall in these poses even though
> that wall *is* displaced (ON/OFF depth differs by up to 0.11 u). `DisplaceViz_
> Record` (`MeshOps.cpp:3538`) keys on ONE `targetMat` pointer, and the
> negative-handedness clone `rooms::mirUV` (`GREETS.CPP:1427`) is created AFTER
> the bake, so those faces stop matching. Do not read an empty overlay as "not
> displaced" — I nearly did.
>
> Images (all 1920×1080 unless noted, `--deferred --profiler=0`):
> `docs/img/fogwt/gapwt_t5967_{cpu_disp,cpu_flat,gpu_tess,gpu_flat,cpu_mip0}.png`,
> `gapwt_t5987_{cpu_disp,cpu_flat,gpu_tess,gpu_flat,cpu_mip0}.png`,
> `gapwt_t{5967,5987}_corner_strip.png`, `gapwt_t5967_solidproof.png`,
> `gapwt_t5967_corner_crosssection.png`, `gapwt_heightmap.png`.
> No code changed; no pin moved.

> ## 2026-08-10 — `--shadow_lm_dynamic` IS A NO-OP, AND OPENING ITS GATE COSTS 1.7 ms FOR NO VISIBLE GAIN
>
> User: *"regarding `--shadow_lm_dynamic` — what would that give us? perf/looks/
> neither? can you show me? and will a longer/more complex bake give better
> results?"* **Answer: neither, because as shipped the flag does nothing at all;
> and when its second gate is forced open the lightmap path is 1.7 ms/frame
> SLOWER with a sub-visible look change that a richer bake cannot improve.**
> All numbers below measured on an isolated worktree built at HEAD `7953bab`
> (`/Users/gil-ad/work/rev-lmdyn`) so concurrent agents' uncommitted
> `FDS/RENDER` work could not contaminate them; run against the main tree's
> `Runtime/`. **No default was changed.**
>
> ### 1. THE FLAG IS INERT — THERE ARE TWO GATES AND IT ONLY OPENS ONE
>
> `--shadow_lm_dynamic` is **byte-identical to the shipping frame at all 18
> poses** (the 16 of `docs/greets_review_poses.txt` + his two new ones,
> `t=5967` / `t=5987`), **0 px, flat arm and `--greets_displace` arm alike**.
> Not "look-neutral" — *inert*. The scene is fully deterministic here (two A-vs-A
> reruns: 0 px), so 0 is a real zero.
>
> **The second gate is the G-buffer plane allocation, and greets closes it.**
> `resolvePixelLightmap` (`DeferredShadowSampling.h:52`) returns null unless
> `gb.lightmapMF` is non-empty, and that plane is allocated **only** by
> `EngineGBuffer_Resize` (`Mekalele.cpp:85`) under `FeatureFlags::shadow_lightmap()`.
> Greets sets `shadow_lightmap` in **`GreetsApplyRunDefaults`** (`GREETS.CPP:1228`),
> which runs at `createGreetsScene` (`:4393`) — *after* every resize call site
> (`Snapshot.cpp:153`, `SDL2.cpp:433` at boot, `ReproHarness.cpp:130`). So the
> planes are never allocated, every pixel's `pl.lm` is null, and no value of
> `shadow_lm_dynamic` can matter. **This is the same defect class as `mirror_rtt`,
> fixed 90 lines away in `7953bab`.**
>
> **MEASURED, the positive control that proves it is the allocation and not
> something else** (t=5743, one binary, only the flag set changes):
>
> | arm | vs shipping default |
> |---|--:|
> | `--shadow_lm_dynamic` | **0 px** |
> | `--shadow_lm_dynamic --shadow_lightmap_texel_density=1` (atlas crippled 4.5x coarser) | **0 px** |
> | `--shadow_lightmap` alone (planes allocated, `lmKernelEnabled` still false) | **0 px** |
> | `--shadow_lightmap --shadow_lm_dynamic` | **868 274 px (41.87 %)** |
>
> A deliberately crippled atlas changing nothing is the proof the atlas is not
> being read. Corroborated on the `--repro` (real per-frame) path: A-vs-B there
> is 1 444 px against that harness's own **1 051 px** A-vs-A noise floor of
> identical signature (all >32/255, mean |Δ| 124) — i.e. indistinguishable —
> while A-vs-C is 870 433 px.
>
> **Consequence: `shadow_lightmap` is read by NOTHING after init.** Its only
> readers are allocation sites (`Mekalele.cpp:85`, `GreetsMirror.cpp:3051`,
> `MirrorShatter.cpp:655/940`) and `LightmapStampOrigBary` / `LightmapBake_Static`
> (both force-enabled for greets). It is **not** a per-pixel sample gate — the
> comment at `GREETS.CPP:1112-1117` justifying its run-phase placement ("it's the
> per-pixel SAMPLE gate the deferred kernel reads for EVERY scene") is factually
> wrong, and is what put it on the wrong side of the resize.
>
> ### 2. WITH THE GATE FORCED OPEN: PERF IS WORSE, NOT BETTER
>
> `--bench=scene@scene=greets,t=T,iters=20 --deferred_prof=1`, **min-of-6
> interleaved**, run 1 after build discarded, load **9.8–11.6** throughout,
> 1920×1080, 12 workers. Arm A = shipping default, arm C = `--shadow_lightmap
> --shadow_lm_dynamic`.
>
> | | t=5743 | t=5780 | t=5814 |
> |---|--:|--:|--:|
> | frame ms A → C | 49.90 → **51.70** | 50.05 → **51.80** | 48.49 → **50.14** |
> | **Δ frame** | **+1.80** | **+1.75** | **+1.65** |
> | `gbuffer` wall (raster) | +0.17 | +0.23 | +0.22 |
> | `gbuffer` thrsum (core-ms) | +3.52 | +1.94 | +3.41 |
> | `lighting-w1` wall | +1.19 | +1.85 | +1.15 |
> | `lighting-w1` thrsum (core-ms) | **+25.49** | +13.73 | +6.84 |
> | `lighting-w2` wall | +0.02 | −0.02 | −0.02 |
> | static bake ms | 55.1 / 54.9 | 54.3 / 55.6 | 54.2 / 54.7 |
>
> **Same sign at all three poses, on the two phases the mechanism predicts.**
> The hypothesis under test — *"lm ON means cube taps only test movers
> (dynamic-only tap = 2 cache lines not 4 since `af1f8f8`), so lighting should
> get cheaper"* — **is refuted by measurement.** The saving is real but smaller
> than what replaces it, and there are two costs, both visible in the table:
> 1. **Raster:** the lightmap arm allocates and *writes* two extra G-buffer
>    planes (`lightmapMF` u32 + `lightmapST` u16 = 6 B/px = 12.4 MB at 1080p)
>    in Mekalele's hot loop → `gbuffer` +0.2 ms wall / +2–3.5 core-ms.
> 2. **Lighting:** `sampleBilinearPlanar` costs *more* than the tap it replaces —
>    a world-space projection onto the face's dominant cardinal plane, a bbox
>    map, and a bilinear gather from a 0.09 GB atlas of per-face mini-atlases
>    with far worse locality than the shadow cube — **and it still pays the
>    dynamic-only tap** on every face where `dynBaked` is true.
>
> The flag's own doc already said "MEASURED NEUTRAL-to-NEGATIVE… planar sampler
> +0.8 ms w1"; this is the same sign, roughly double the magnitude, and now with
> the raster half attributed too.
>
> ### 3. LOOKS: SUB-VISIBLE, AND IT IS 95 % A ONE-LSB SHIFT
>
> Contact sheet, all 18 poses, before | after | diff:
> `docs/img/fogwt/lmdyn_contactsheet.png`. Tight 4x crops at the four
> highest-amplitude poses: `lmdyn_t5743_tight.png`, `lmdyn_t5773_tight.png`,
> `lmdyn_t5814_tight.png`, `lmdyn_t5958a_tight.png`; displace arm:
> `lmdyn_disp_t5743_tight.png`, `lmdyn_disp_t5814_tight.png`.
>
> A-vs-C moves 17–55 % of pixels at every pose, which sounds enormous and is
> not. **Delta histogram at t=5743 (868 274 changed px):**
>
> | \|Δ\| | px | % of frame |
> |---|--:|--:|
> | **exactly 1** | **827 485** | **39.91** |
> | 2 | 19 463 | 0.94 |
> | 3–4 | 9 203 | 0.44 |
> | 5–8 | 5 511 | 0.27 |
> | 9–16 | 3 937 | 0.19 |
> | 17–32 | 2 221 | 0.11 |
> | 33–64 | 449 | 0.022 |
> | 65–255 | **5** | 0.000 |
>
> **95.3 % of all changed pixels differ by exactly one LSB**, and the shift is
> directional: **98.1 % of them get BRIGHTER**, signed mean +0.37 B / +0.21 G /
> −0.00 R over the whole frame. The lightmap composite very slightly
> *under*-shadows relative to the per-pixel reference, in the blue-green of the
> corridor lamps. The genuinely visible residual is a few hundred to a few
> thousand pixels on thin geometry silhouettes — the lintel top edge, the far
> lattice, column edges. Side by side the two frames are indistinguishable.
> `--greets_displace` behaves the same (t=5743 45.3 %, t=5814 41.2 %, t=6097
> 26.3 % — mean |Δ| 0.55–0.63, max 138/96/7).
>
> ### 4. "WILL A LONGER / MORE COMPLEX BAKE HELP?" — NO, AND THAT IS MEASURED
>
> The atlas is per-mesh `res = clamp(ceil(sqrt(meanFaceArea) x density), 8, 128)`.
> Swept the **cap and the density together** (so every mesh actually sharpens),
> comparing each against the per-pixel cube tap as reference at three poses:
>
> | | atlas | bake | t=5743 >12 / max | t=5773 >12 / max | t=5814 >12 / max |
> |---|--:|--:|--:|--:|--:|
> | cap 128 / density 14.2 (default) | 0.09 GB | 54 ms | 4 248 / 138 | 3 852 / 70 | 893 / 96 |
> | cap 256 / density 28.4 | 0.32 GB | 191 ms | 3 925 / 138 | 3 304 / 69 | 822 / 96 |
> | cap 512 / density 56.8 | **1.28 GB** | **670 ms** | 3 917 / **138** | 2 835 / **69** | 748 / **96** |
>
> **A 14x atlas and a 12x bake buys an 8–26 % reduction in an already-sub-visible
> population and moves the max channel delta by 0 or 1.** The 40 % one-LSB field
> does not move at all. Picture: `docs/img/fogwt/lmdyn_bakeres_t5773.png`
> (reference | cap 128 | cap 512 | residual — the three renders are
> indistinguishable and the residual is unchanged).
>
> **So visible quality is NOT bake-limited.** The residual is invariant under
> spatial resolution because it is not spatial: **inferred** mechanism is the
> atlas's **8-bit quantisation of the shadow factor** plus **double filtering**
> (a 4-tap bilinear PCF at bake time, quantised to a byte, then bilinearly
> re-interpolated at sample time) against the runtime's single 4-tap PCF
> evaluated at the pixel's own world position. More texels do not add bit depth,
> and no amount of them makes a bake-time evaluation land on the render-time
> sample point. Raising resolution is the one lever that was tested and it is
> the wrong lever.
>
> **What a richer bake could add, with honest estimates — all `inferred`:**
>
> | item | what it buys | effort | verdict |
> |---|---|---|---|
> | higher res where visible | measured above: ~nothing, at 14x the store | done | **no** |
> | more bits per texel (u16 factor) | would remove the 1-LSB field, which is invisible anyway | medium (format + sampler + 2x store) | **no** |
> | baked PCF / soft edges | bake already does a 4-tap PCF; wider kernel = softer than the runtime reference, i.e. a *different* look, not a truer one | small | only as a look choice |
> | baked penumbra (area light) | genuinely impossible at runtime today — real soft shadows. This is the ONLY item that buys something the cube tap cannot | large (multi-sample light, bake time x N) | the only interesting one, and it is a look project, not a perf one |
> | drop the 3 moving-omni slots | `allocate()` takes `numCubeOmnis` = **11** (`LightmapBake.cpp:373`) but the bake `continue`s on any omni without `Omni_StaticShadow` (`:487`) and the kernel's `cubeOmniStatic` gate can never read them — **3/11 = 27 % of the atlas is allocated, touched at 255, never written, never read** | small | free win *if* the path is ever used |
>
> ### 5. VERDICT
>
> **Neither perf nor looks — and as shipped, not even that: nothing.** Ranked:
> 1. **Best value, and it needs no look decision:** greets pays a **54 ms startup
>    bake and 0.09 GB** for an atlas that is provably never read. Skipping
>    `LightmapBake_Static` when the planes will not exist is pure win. Already on
>    the backlog; still not done.
> 2. **If you want the lightmap path evaluated for real**, the `shadow_lightmap`
>    `setDefault` has to move from `GreetsApplyRunDefaults` to
>    `GreetsApplyInitDefaults` (with the leak-onto-other-scenes concern the old
>    comment raised re-checked, since the flag is allocation-scoped, not
>    per-pixel). Until then `--shadow_lm_dynamic` cannot be evaluated by flag
>    alone, and **any past measurement of it that did not also pass
>    `--shadow_lightmap` measured nothing** — see the correction in
>    `docs/OPTIMIZATION_BACKLOG.md`.
> 3. **Do not default `--shadow_lm_dynamic` ON.** Even with the gate opened it is
>    +1.7 ms/frame (+3.5 %) for a change no one can see.
>
> Full 18-pose x 3-arm PPM set and the bench logs are on disk at `/tmp/lmdyn/`
> (untracked, ~2 GB); the committed evidence is the contact sheet + the six crops.

> ## 2026-08-10 — HIS 12-14 FPS: THE WIN IS 17 ms, BUT ONLY ON A LINE THAT OMITS `--deferred`
>
> Follow-up to `f4088a9` (`fds::DeferredPathEnabled()`). Three GREETS.CPP sites
> corrected: the Piramid chunk split (`:2427`) and the forward `Lighting()` gate
> (`:3867`) now ask `DeferredPathEnabled()` instead of `FeatureFlags::deferred()`,
> and `mirror_rtt` / `mirror_rtt_density` move from `GreetsApplyRunDefaults` to
> `GreetsApplyInitDefaults` — GreetsMirror's `wantRtt` (`:1401`) is evaluated
> during `Initialize_Greets`, so a run-phase default arrived **after** the
> decision and was inert (measured: `0 first-order RTT` slots, no `[MIRROR-RTT]
> slot` lines at all).
>
> **MEASURED at HEAD `af1f8f8`, his pose/res** (`t=3122`, 1512×848,
> `--greets_displace --texture_filter=1`, min-of-6 interleaved, run 1 discarded,
> load 7.2–8.3), two binaries from one tree differing only in GREETS.CPP:
>
> | | before | after |
> |---|--:|--:|
> | frame ms | 66.14 | **49.07** (−17.07, −25.8 %) |
> | BAKE | 15.54–16.41 | **3.22–3.61** |
> | LGHT | 6.38–6.45 | **0.91–0.95** |
> | RNDR | ~41.6 | ~41.3 |
>
> **THE FLAG THAT DECIDES THE SIGN.** The same A/B **with `--deferred` passed
> explicitly** measures **45.70 → 49.53 ms, i.e. +3.83 ms SLOWER**: there
> `FeatureFlags::deferred()` was already true, both predicate fixes are no-ops,
> and all that is left is the RTT slot build the `mirror_rtt` move switches on.
> So this change is a large win on **his** line and a small cost on any line that
> spells `--deferred` out — which includes the pin recipe and the render gates.
> A bench that passes `--deferred` cannot see this fix at all; the first batch
> here did exactly that and reported the wrong sign.
>
> **LOOK: this one MOVES, broadly.** 83–99.5 % of pixels change at every one of
> the 16 review poses (1920×1080; note `--repro_xres/--repro_yres` are read only
> by the `--repro` harness, `ReproHarness.cpp:240`, and are INERT on `--snapshot`),
> mean |Δ| 3.6–6.7/255, max ~200 — a broad, essentially zero-mean shift
> (mean luma +0.02 to +0.47), not a darkening. At his own pose it is 38.8 % of
> pixels and it **removes a defect**: near-black pixels (luma < 8) go
> **2 350 → 4** — the black gash on the right wall in
> `docs/img/fogwt/deferredfix_t3122_before.png` is gone in `_after.png`.
> Mechanism: the chunk split was never happening on his line, so the per-cube-face
> bsphere cull had nothing to reject and 59 556 displaced faces never got
> `NoShadowCast`. Contact sheet (all 17 poses, before | after):
> `docs/img/fogwt/deferredfix_contactsheet.png`.
>
> **PINS DO NOT MOVE — and that is a warning, not a comfort.** greets
> `778fa6acd85a69cf241babefcdaf598e` 4/4 on **both** arms, fountain
> `8db68ccb59416e9a44037e9f387b7bd9` 4/4, city `3cbe42b166847e40f7071eedb48d613c`
> 4/4, `render_gate` 3/3. The greets pin is **blind** to this change: its recipe
> passes `--deferred` (so the predicate fixes are inert) and `t=1588` shows no
> RTT panel. A byte gate that spells the flag out cannot certify a fix about the
> flag being absent.
>
> **CORRECTION to the block below (same session, better data).** The lightmap
> density per-frame delta was re-measured at HEAD on a quiet box (load 3.2–7.9,
> min-of-6 interleaved): `t=5743` 49.17 → 49.33 and `t=5780` 48.70 → 48.84 —
> **neutral at both poses, +0.15 ms, inside the run-to-run spread**. The −1.76 ms
> at `t=5780` recorded below was measured at load 11–30 and was noise. The bake
> and the memory reproduce exactly: atlas 5.61 → 0.09 GB, peak footprint
> 7.46 → 1.53 GB, `[GREETS-BAKE] waited` 1050.2 → 53.6 ms.

> ## 2026-08-09 — THE SHIPPING GREETS ARM BAKED A 5.61 GB LIGHTMAP AND NEVER READ IT
>
> Follow-up to the `--greets_displace` 19.4 GB finding below: the user approved
> extending `--shadow_lightmap_texel_density=14.2` to the SHIPPING arm, so the
> `setDefault` moved out of the `if (greets_displace())` branch into the main
> `GreetsApplyInitDefaults` block. `--greets_displace` now advertises **two**
> companions, not three.
>
> **THE WIN, flat arm, greets `t=5743`, same binary, `…density=0` vs default:**
>
> | | legacy | default 14.2 |
> |---|--:|--:|
> | atlas store (`[LM]` line) | 5.61 GB | **0.09 GB** |
> | peak footprint (`/usr/bin/time -l`) | 7.44 GB | **1.50 GB** |
> | static bake, min-of-9 interleaved, load 11–17 | 1104 ms | **54 ms** |
> | greets-entry join wait (`[GREETS-BAKE] waited`), load 31 | 3497 ms | **221 ms** |
> | frame ms `t=5743`, min-of-15 interleaved | 49.39 | 49.47 |
> | frame ms `t=5780`, min-of-15 interleaved | 51.84 | **50.08** |
>
> 347 of the 370 baked meshes fall under the 128 cap (mean face edge 1.303 world
> → res 19); the 23 that keep it are the big authored quads. Per-frame is
> neutral at `t=5743` (+0.08 ms, inside a several-ms run-to-run spread) and
> −1.76 ms at `t=5780`; the bake and the 5.94 GB are the certain wins.
>
> **THE LOOK MOVED NOTHING, and that is measured, not assumed.** Byte-identical
> at all 16 poses of `docs/greets_review_poses.txt` and at the pin pose — so
> **the greets pin `778fa6acd85a69cf241babefcdaf598e` did NOT move (4/4)**, city
> `3cbe42b166847e40f7071eedb48d613c` and fountain `8db68ccb59416e9a44037e9f387b7bd9`
> 4/4 each, `render_gate` 3/3. Images: `docs/img/fogwt/lmdensity_flat_*`.
> Two poses show 2–7 px at ≤15/255 — **that is the scene's own run-to-run
> nondeterminism, not the change**: same-arm reruns of `t=5773` differ by 6 px
> at max 15, i.e. more than the cross-arm diff.
>
> **WHY it is null, and the bigger finding underneath.** The shipping arm never
> SAMPLES the atlas. `DeferredSurfaceKernel.cpp:1619` gates the lightmap path on
> `lmKernelEnabled = !shadow_dynamic() || shadow_lm_dynamic()`; greets defaults
> `shadow_dynamic` ON and `shadow_lm_dynamic` is compile-default 0, so every
> pixel takes the per-pixel cube tap instead. MEASURED, not inferred:
> `--no-shadow_lightmap` renders **byte-identical** frames at `t=5743` and
> `t=6097`, and re-running the whole 16-pose battery under `--shadow_lm_dynamic`
> (atlas live) is byte-identical between the two densities as well. So greets
> spends a 1.1 s startup bake and 5.6 GB producing an array nothing reads. This
> commit makes that 54 ms and 0.09 GB; **skipping the bake outright when
> `shadow_dynamic && !shadow_lm_dynamic` is the real fix and is NOT done here**
> (FDS/RENDER, and the opposite call — defaulting `--shadow_lm_dynamic` ON — is
> a look decision for the user). Recorded in `docs/OPTIMIZATION_BACKLOG.md`.
>
> Revert: `--shadow_lightmap_texel_density=0` (verified — reproduces the pin
> 4/4). Stale comment left behind on purpose (lane discipline, another agent
> owns FDS/RENDER this session): `FDS/RENDER/LightmapBake.cpp:330-336` still
> claims the flat path never enters the density branch.

> ## 2026-08-10 — "I CAN'T SEE THE MECH IN THE UP-LOOKING BAKE": HIS OFFSET HYPOTHESIS IS RIGHT, AND IT IS 8 UNITS
>
> User: *"I can't see the mech in the up-looking dynamic bake, even when the mech
> is directly above the stairs — I think the camera is offset to one of the
> stairs' side."* **Confirmed, measured, and the offset is nearly the whole
> half-extent of the surface.**
>
> **THE NUMBERS.** `materialCentroid` (`FDS/RENDER/EnvBake.cpp`) derives a
> probe's capture point as the mean world position of **every vertex** of every
> face using the material, then — for a multi-instance surface — greedy-clusters
> at an **8-world-unit** radius and re-centroids on the **heaviest** cluster.
> greets `stairs` is **one pair of flights 9.5 u long**, and 9.5 > 8, so each
> flight splinters into a top cluster (n=22) and a bottom cluster (n=8). The
> function's own comment already concedes this — *"the greedy clustering
> splinters a single statue into several"* — but only in the self-exclusion
> logic, not in the probe placement. "Heaviest" then parks the probe on the top
> landing END:
>
> | | value |
> |---|---|
> | capture point (shipped) | **(45.4, 2.3, −54.9)** |
> | owner-faces AABB | [35.9, 0.0, −70.9] .. [49.1, 3.8, −54.8] |
> | footprint centre | (42.5, 1.9, −62.85) |
> | offset from centre | (+2.9, +0.4, **+7.95**) on a 16.2 u Z extent |
>
> The probe sits at **z = −54.9 against a −54.8 boundary** — literally on the
> z-extreme face of its own footprint. The mech ends its walk at
> **(44.4, 4.7, −62.2)**, directly over that footprint centre. From the shipped
> probe its direction is (−1.0, +2.4, −7.3) = **72° off vertical**, so it lands
> in the **−Z** cube face and +Y never sees it. From the footprint centre the
> same mech is **36° off vertical** — inside +Y.
>
> **THE DRAW SET IS NOT THE PROBLEM, and this was checked first.** Both
> mechanisms were tested. The mech IS a mover (`WorldAabb_MeshIsDynamic`), the
> store IS retained, and `[ENVDYN-WHY]` reports `'stairs' (store 1): OK —
> overlaid the mech into 3 touched face(s), **1754 mech texel(s)** composited
> over static`, every frame. The overlay is drawing the mech into this probe
> continuously; it is just drawing it into the wrong faces. `--env_bake_include_animated`
> (static-bake inclusion) is a separate mechanism and is not implicated.
>
> **THE PROOF PAIR** — the live post-overlay +Y face of the same probe, same
> pose (`--repro=greets@t=7100 --env_dynamic`), via the new `--env_dyn_dump`:
> * `docs/img/envmap/stairs_pY_before.png` — empty room, **no mech**
> * `docs/img/envmap/stairs_pY_after.png` — **the mech, dead centre**
> * `docs/img/envmap/stairs_mZ_before.png` — where it actually was: small, low,
>   near the edge of the −Z face, exactly as 72° predicts
> * whole cubes: `docs/img/envmap/stairs_atlas_before_half.png` /
>   `docs/img/envmap/stairs_atlas_after_half.png`
>
> **THE FIX — `--env_probe_center`, and it is general, not a stairs special-case.**
> Two changes inside `materialCentroid`: (1) **AREA weighting** — each face
> contributes its own centroid weighted by its world area, so the point stops
> being a function of tessellation density; (2) **INSTANCE-GROUP UNION** — the
> greedy clustering is left bit-identical (instance *detection* is untouched),
> but the heaviest cluster is then unioned transitively with every cluster
> within the **2× cluster radius the self-exclusion logic already calls
> "fragments of the probed instance"**, and the capture point is the area
> centroid of that union. The change simply makes the placement obey a rule the
> file already states. New stairs capture point: **(42.6, 0.4, −62.1)** — X and
> Z on the footprint centre.
>
> **The separation guard is exercised and it holds — measured on CITY, not
> asserted from the source comment.** greets turned out to be a bad witness for
> it: the only multi-cluster materials there are `stairs` and `stairs::mirUV`,
> and both merge 4-of-4. (The `materialCentroid` comment's example, "the two
> greets mummies share one material", does not match the scene as it stands —
> `momy-1` and `momy-2` are *separate* materials with one cluster each, so they
> never enter this path at all.) City's vehicle glass is the real test, and
> there the union correctly refuses to swallow the siblings: `cokpit` **1 of 4**
> clusters, `car 2 glass` **1 of 8**, `ambulans glass` **2 of 5**,
> `poliece  glass` **2 of 5**, `bike glass` 4 of 4. That the whole city frame
> then moves by **5 pixels** is the evidence that scattered-instance surfaces
> keep their per-instance probes.
>
> An **UP-FACING-FACES-ONLY** centroid was considered and rejected: three of
> greets' five flagged probes (`momy-1`, `momy-2`, `screen emiter`) are vertical
> reflectors with no up-facing faces at all, so the restriction is undefined
> exactly where it would have to be general.
>
> **DEFAULT OFF, AND THE FLIP WANTS HIS EYE.** Certified DIFFERENTIALLY (one
> binary, flag on vs off — the only valid method in a shared tree):
>
> | gate | flag OFF | flag ON |
> |---|---|---|
> | greets (pin recipe, `--no-env_refl`) | `778fa6ac…` ✅ unmoved | `778fa6ac…` **identical** |
> | fountain | `8db68ccb…` ✅ unmoved | `8db68ccb…` **identical** |
> | city | `3cbe42b1…` ✅ unmoved | `3c64e012…` **MOVES** — 5 px, max Δ 4/255 |
> | greets WITH env_refl (t=1588) | `e5f38b40…` | `757cae6d…` **MOVES** — 343 157 px (16.5 %), max Δ 102, but mean Δ-sum 3.3/765 and only 3 715 px > 10 luma |
>
> All four stable 2/2. The recorded greets pin recipe carries `--no-env_refl`,
> so it is blind to this by construction — the `greets WITH env_refl` row is the
> honest measurement and is why the flag ships OFF. Look pairs for his eye:
> `docs/img/envmap/greets_stairs_view_pair.png` (a camera on the stairs with the
> mech above them — the clearest one) and
> `docs/img/envmap/greets_t1588_probecentre_pair.png` (the pin pose).
>
> **BAKE COST: NO INCREASE, MEASURED.** greets bakes **one fewer probe** with the
> flag on (7 → 6): the new `stairs` and `stairs::mirUV` capture points land 2.2 u
> apart and fall inside the existing 4-unit store-sharing radius, so the two
> collapse onto one store — one 512² cube bake saved. Min-of-5 wall on the greets
> snapshot 1 813 ms OFF vs **1 792 ms** ON (load 8.9–13.4; the −21 ms is inside
> the noise of a 1.8 s run, so the claim is *no measured increase*, not a win).
> The derivation itself adds one cross product + sqrt per face *that uses the
> material*, inside a mesh walk that already happens.
>
> **THE AUTHORED OVERRIDE — `Material::EnvBakeOfs`, editor "probe offset X/Y/Z".**
> A derivation over a surface's own geometry cannot know that a probe wants to
> sit clear of a step nose or below a soffit, so the automated point is not the
> last word. Three floats, world units, **added on top of whichever derivation
> ran** (verified live: `'stairs': authored probe offset (+0.00 +3.00 +0.00) —
> capture point (42.6 0.4 −62.1) -> (42.6 3.4 −62.1)`). All zero = unset =
> byte-null. Live-applies — the edit drops just that store
> (`EnvReflection_InvalidateSurface`) so the probe re-bakes from the new point on
> the next frame and can be dialled in by eye.
>
> Persistence follows the §1a extension idiom: LWO **`RVSF` sub-chunk bit
> `0x1000`**, carrying **three floats under ONE bit** (X, Y, Z). **Proven end to
> end, not asserted:** `lwopatch` wrote `envBakeOfs = (0, 2.5, 0)` onto `stairs`
> in a scratch copy of `Authoring/greets/Piramid.lwo`, `lwsread` regenerated the
> FLD **+12 bytes exactly** (233 621 → 233 633), and the engine — run against a
> scratch asset root via `--no-chdir_assets`, so nothing under `Runtime/` or
> `Authoring/` was touched — reported `'stairs': authored probe offset (+0.00
> +2.50 +0.00) — capture point (45.4 2.3 −54.9) -> (45.4 4.8 −54.9)`, and the
> `::mirUV` clone inherited it. Inertness is proven too: with nothing authored,
> the greets regen is byte-identical at the golden `62c68fc9…`, and 300 random
> writer subsets over the 12 legacy RVSF keys reproduce the pre-change bytes
> exactly. That deviates
> from the one-bit-per-scalar convention `tintR/G/B` follows, deliberately: it is
> one semantic vector, and three bits would have left the u16 with a single free
> bit. **0x2000/0x4000/0x8000 remain free.** It is a SURFACE property, not an
> object one, because a probe's identity in `EnvBake` *is* its material
> (`env.byMat`, one store per material-centroid group) — a per-object value would
> have had no probe to attach to, and §1d's `Object_FdsExt` path is unimplemented.
>
> **NEW INSTRUMENT: `--env_dyn_dump=N`** (1-based store index, the `--env_map_probe`
> numbering) writes the **live, post-overlay** mip-0 cube of probe N to
> `/tmp/envdyn_<material>.ppm` as the standard 3×2 atlas. `FDS_ENVBAKE_DUMP` can
> only show the STATIC capture, which by construction contains no mover — so it
> could not have answered this question. Default 0, byte-null.
>
> **PROCESS NOTE, for the record:** the two `FeatureFlags.def` entries for this
> work were swept into commit `5079f6e` (`--shadow_plane_pack`) by a concurrent
> agent holding the shared tree. The content is correct and in HEAD; the
> attribution is wrong. Same hazard class as the 2026-08-09 note below.

> ## 2026-08-09 — HIS 12-14 FPS, EXPLAINED: THE SCENE RENDERS DEFERRED BUT WAS BUILT AS IF IT WOULD NOT
>
> User, interactive, `./DEMO --greets-displace`, window 1512×848, facing the
> mirror wall (`FDS_GREETS_CAM="-8.6249094,2.72651696,-53.2339516,0.210607708,
> 0.0055912463,-0.977554619"`, t=3122): **12-14 fps**, remembered "a lot better"
> (20-30). The bench said 54.7 ms at 1920×1080, which at 0.63× the pixels should
> be comfortably above 18 fps. The gap is real and it is **22.7 ms**, all of it
> from ONE root cause with two heads.
>
> **THE RENDER PATH IS NOT `FeatureFlags::deferred()`.** `RENDER.CPP:356`
> `deferredEnabled()` ORs five flags — `deferred || hdr || deferred_quarter ||
> deferred_checkerboard || shard_deferred` — and greets sets three of them, then
> additionally forces `Render(RenderPath::ForceDeferred)` whenever
> `greets_mirror` is on (`GREETS.CPP:3943`, and `greets_mirror` is
> `setDefault(true)` at `:1088`). So a plain `./DEMO --greets-displace` **renders
> a deferred frame**. But two SCENE-BUILD/TICK decisions ask the bare flag, which
> is still 0, and they get the opposite answer:
>
> | reader | what it does when it wrongly thinks "forward" | measured cost |
> |---|---|--:|
> | `GREETS.CPP:2414` | skips the Piramid chunk split entirely → the 59 556 displaced faces the chunk pass marks `NoShadowCast` stay casters, and the wall stays one room-sized mesh | **15.10 ms** (BAKE 3.9 → 16.0) |
> | `GREETS.CPP:3849` | runs the forward vertex `Lighting(GreetSc)` pass every frame, whose only consumer is the mirror-RTT offscreen pass | **6.49 ms** (LGHT 0.00 → 6.49) |
>
> **MEASURED**, `--bench=scene@scene=greets,t=3122,iters=20,xres=1512,yres=848`,
> min-of-6, load 9-15:
>
> | arm | fmin | BAKE | LGHT |
> |---|--:|--:|--:|
> | A — his line, `--greets_displace` | **68.49** | 16.02 | 6.49 |
> | B — A + `--deferred` | **44.69** | 3.88 | 0.00 |
> | C — B + `--greets_piramid_chunk_grid=0` | 59.79 | 16.27 | — |
> | flat, no `--deferred` | 45.31 | 3.71 | — |
>
> C isolates it: chunk split = B−C = **15.10 ms**, the rest = A−C = 8.70 ms, of
> which `Lighting()` is 6.49. The flat row is the tell — **the penalty needs the
> displaced geometry**; flat without `--deferred` costs the same as with it.
>
> **THE ARITHMETIC TO 12-14 FPS.** `DEMO/SDL2.cpp:622-626` creates the renderer
> with `SDL_RENDERER_PRESENTVSYNC` unless `--no_vsync` (default 0), so present
> quantises to the refresh. At 60 Hz:
>
> | render | intervals | presented | fps |
> |---|--:|--:|--:|
> | his line, 68.5 ms | 5 | 83.3 ms | **12.0** |
> | fixed, 44.7 ms | 3 | 50.0 ms | **20.0** |
>
> 12.0 is exactly what he reports; 20.0 is exactly the bottom of what he
> remembers. The quantisation is why it reads as a cliff rather than a slope —
> 68.5 and 44.7 straddle two whole steps. *Code-verified + arithmetic; the 60 Hz
> refresh is assumed, not measured, and I never opened a window.*
>
> **FIXED HERE (FDS half):** `fds::DeferredPathEnabled()`, declared in
> `RENDER/ChunkOcclusion.h`, defined in `RENDER.CPP` next to `deferredEnabled()`
> — one predicate, one definition, callable at init. Purely additive; nothing
> calls it yet, and all three pins + render_gate are unmoved.
>
> **HANDOFF (DEMO/GREETS.CPP is another agent's lane right now):**
> 1. `:2414` — `&& fds::FeatureFlags::deferred()` → `&& fds::DeferredPathEnabled()`
> 2. `:3849` — `!fds::FeatureFlags::deferred() ||` → `!fds::DeferredPathEnabled() ||`
> 3. move `setDefault(mirror_rtt, true)` and `setDefault(mirror_rtt_density, 1024.0f)`
>    out of `GreetsApplyRunDefaults` into `GreetsApplyInitDefaults`
>    (docs/SETDEFAULT_AUDIT.md §4.1/§4.3, recommended there and still unfixed).
>
> **DO NOT "fix" this with `setDefault(deferred, true)`.**
> `GreetsApplyInitDefaults` runs FIRST in the t1 init chain, so that would force
> city/chase/fountain/crash onto the deferred path — the exact `shard_deferred`
> leak recorded as §5 L1 in the audit.
>
> **The fix is a LOOK change, and needs his eye + a re-pin decision:** at his
> pose, A vs B differs by **557 589 px (26.9 %)**, mean Δ-sum 18.79/765 — a broad
> low-amplitude shading shift on the ceiling and right-hand wall (the chunk split
> moves per-chunk culling and lighting). Nothing is missing or broken in either.
> Strip: `docs/img/fogwt/deferred_flag_look_t3122.png`.
>
> **HYPOTHESES THAT DIED ON MEASUREMENT, with numbers:**
> * *Pose-dependent mirror/RTT cost.* No. The RTT bake DOES apply the flat-proxy
>   substitution — it takes `OffscreenViewScope` (`GreetsMirror.cpp:3067`) →
>   `g_offscreenViewDepth` → `_offscreenPass` (`Transform.cpp:1180`) →
>   `Face_MainOnly` skipped at `:2429`, proxy admitted at `:1432`. With all 7
>   slots live the displaced-vs-flat delta is **+3.05 ms**. Building the slots at
>   all costs +3.67 ms (tess) / +2.11 ms (flat).
> * *Tessellation is expensive at this pose.* The opposite: at t=3122, 1920×1080,
>   flat and tess are **70.93 vs 70.93 ms** — identical, min-of-6.
> * *Hyphen spelling.* `FeatureFlags.cpp:276-284` normalises dash→underscore
>   after the leading `--`. `--greets-displace --mirror-rtt --strict_flags` runs
>   with 0 unknown flags and the `[STONE]` line fires. Nothing to fix.
> * *Resolution scaling anomaly.* None: 1920×1080 → 1512×848 is 70.93 → 45.80
>   (0.646×) against a pixel ratio of 0.63. Pixel-bound, as expected.
>
> **E, the user's counterexample, upheld:** `--mirror-rtt` changes **9 471 px
> (0.457 %), max Δ 175/255** at his pose, because on the default path the RTT
> slots are *never built* — `mirror_rtt`'s setDefault lands in the RUN block,
> after `Initialize_Greets` has already decided (`GreetsMirror.cpp:1401`). A
> default run logs `0 first-order RTT` and zero `[MIRROR-RTT] slot` lines;
> `--mirror_rtt` logs seven. The "0 px on the authored path" generalization is
> retired in `docs/SETDEFAULT_AUDIT.md`.

> ## 2026-08-09 — THE TWO REPORTED `--greets_displace` REGRESSIONS: NEITHER IS ONE, AND THE REAL COST IS 19.4 GB
>
> User: *"tessellation is costing us now half the fps"* and *"tessellation bake
> seems to hang the starting scenes for quite a lot of time — this should be
> done concurrently — what changed?"*, with *"did we change some
> tessellation/vis params?"*.
>
> **PARAMS: NOTHING CHANGED. Not one.** Every flag in the displace family has
> the identical compile-time default at `HEAD` and at `1a91ed5` — `greets_displace`,
> `_amp` 0.3, `_mip` 2, `_adapt` 1.0, `_cpb` 1.0, `_edge`, `_seam_union`,
> `_fold_relax`, `_shadow_planes`, `_line_height`, `_smooth` 80, `_neighbor_pin`,
> `greets_stone_subdiv` 0, `greets_shadow_proxy`, `greets_displace_flat_mirror`,
> `displace_viz`, `chunk_occl_res`, `tile_bbox_cull`. Of the 40 `setDefault` calls
> in `GREETS.CPP`, exactly ONE moved: `greets_omni_default_range` 30.0 was DELETED
> (`00f7820`, ranges now authored per light in the LWS) — and it acts on both arms
> equally. `DisplaceTest.cpp`'s setDefaults are identical. `DisplaceStoneSubdiv`
> itself (`MeshOps.cpp:1970`) is **untouched**: every one of the +1082 lines in
> that file since `1a91ed5` is above line 4154, i.e. `PomShell_*` / prism, and
> `--pom_shell` is still default 0 (no shell/prism log fires in a displace run).
> `pom_shell_weld` 0→1 is real but inert here for the same reason.
>
> **SYMPTOM 1 (per-frame) DOES NOT REPRODUCE.** `--bench=scene@scene=greets,
> t=5743,iters=20`, 5 arms interleaved, min-of-6, load 8.8–13.1:
>
> | arm | fmin min | Δ flat, same tree |
> |---|--:|--:|
> | `1a91ed5` flat | 50.61 | — |
> | `1a91ed5` tess | 57.15 | **+6.54** |
> | HEAD flat | 53.62 | — |
> | HEAD tess (pre-fix) | 55.55 | **+1.93** |
> | HEAD tess (post-fix) | 54.72 | +1.10 |
>
> The tessellation delta did not grow, it **shrank**. What grew is the BASE cost
> of *both* arms: HEAD's flat arm is +3.0 ms over `1a91ed5`'s, which is the nine
> flags defaulted ON in `1782351` + `bd6e806` — they cost on every path and
> therefore cannot move a tess-vs-flat delta. A second batch at load 20–54 put the
> delta at +5.47 (HEAD) vs +5.44 (`1a91ed5`) — again equal. A whole-timeline sweep
> (`t=200..7000`, 137 frames, min-of-4) gives HEAD +3.9 ms on a 63.4 ms mean.
> **The delta measures 2–13 % depending on batch and load. Never 2×.**
>
> **The prime suspect died on measurement.** `704a5a8` does NOT touch
> `GreetsMirror.cpp`, and the flat-mirror clone is intact at HEAD: the displace
> run clones **9 198 / 9 166 faces** per mirror, exactly the documented figure, not
> the 42 870 of the pre-companion arm. The shatter scoping still reads
> 450 / 450 / 2 886 (flat / displace / displace with the scope off), reproducing
> `704a5a8`'s published table byte for byte.
>
> **SYMPTOM 2: THE BAKE IS ALREADY CONCURRENT, AND IT DID NOT GET SLOWER.** New
> `--init_timeline` (default OFF, byte-null) stamps every init milestone. Full
> demo path, dummy drivers:
>
> | mark | flat | `--greets_displace` |
> |---|--:|--:|
> | `Initialize_Greets` | 1 379 ms | 2 469 ms |
> | ├ `DisplaceStoneSubdiv` block | 0 ms | 573 ms |
> | t1 chain done (all five scenes) | 4 672 ms | 5 116 ms |
> | `Run_Glato` ends | 41 878 ms | 43 380 ms |
> | **`t1.join()` returns** | **+0.1 ms** | **+0.0 ms** |
> | City starts | 47 451 ms | 49 298 ms |
>
> The join is instantaneous in both arms — the 42 s intro absorbs the whole init.
> There is no stall on the demo path. What DOES block is the greets-ENTRY path
> (`--scene-greets`, `--snapshot=greets`, `--bench=scene@scene=greets`): those join
> `Greets_JoinBakeThread` immediately after init with nothing in between, so the
> lightmap bake is 100 % blocking wait — and `join_wait_ms == bake_ms` to the
> millisecond, measured. **That was equally true at `1a91ed5`:** bake 10 684 ms
> there vs 10 895 ms at HEAD (flat 1 341 vs 1 365). Nothing regressed.
>
> **WHAT IS ACTUALLY WRONG, and it is big.** `StaticShadowLightmap::data` is
> `numFaces * lmRes² * numOmnis` BYTES and `allocate()` fills it with 255, so every
> byte is touched and resident. greets sets `shadow_lightmap_res = 128`. That is
> calibrated for the authored wall quads and scales with **face count**, so
> tessellation multiplies it directly. `/usr/bin/time -l`, greets t=5743, 64 GB box:
>
> | arm | baked faces | atlas store | peak footprint | max RSS | bake |
> |---|--:|--:|--:|--:|--:|
> | flat | 33 396 | 5.61 GB | 6.93 GB | 7.44 GB | 1.08 s |
> | `--greets_displace` before | 115 346 | **19.36 GB** | **22.97 GB** | 14.05 GB | 6.2–11.7 s |
> | `--greets_displace` after | 115 346 | **0.14 GB** | **2.35 GB** | 2.36 GB | **0.09 s** |
>
> Max RSS *below* peak footprint is the OS already compressing it. A displaced
> cell is ~1/300 the AREA of the quad it replaces, so each was carrying ~300× the
> shadow texels per world unit that the FLAT wall ships with.
>
> **FIXED behind `--shadow_lightmap_texel_density`** (default 0 = OFF = byte-null;
> `--greets_displace` defaults it to 14.2 texels/world-unit as its **third** perf
> companion, named in the `[STONE]` line). Per-mesh
> `res = clamp(ceil(sqrt(meanFaceArea) * density), 8, shadow_lightmap_res)` —
> capped, so it can only reduce; the runtime sampler was already per-mesh
> (`StaticShadowLightmap::lmRes` is a member). **Look cost in the displaced arm:
> byte-identical at t=1588 / 2845 / 4871 / 6097 and 3 px at 1 LSB at t=5743.** The
> 19.2 GB was buying nothing. Per-frame effect at t=5743 is within noise (55.07 →
> 54.82 min-of-6); the certain wins are the bake (114×) and the memory.
>
> **PINS UNMOVED, all three, on this build:** greets `778fa6acd85a69cf241babefcdaf598e`
> (4/4), fountain `8db68ccb59416e9a44037e9f387b7bd9` (3/3), city
> `3cbe42b166847e40f7071eedb48d613c` (3/3). The flat path never enters the branch.
>
> **INFERENCE, stated as inference:** the user's "half the fps" is most consistent
> with the 23 GB footprint meeting a machine that also has other agents on it —
> the arm's cost becomes a function of memory pressure, which is exactly why it
> measured +5.5 ms at load 20–54 and +1.9 ms at load 9–13 in the same session. Not
> proven; the direct A/B on a memory-pressured box was not run.
>
> Evidence: `docs/img/fogwt/lm_atlas_density.png`,
> `docs/img/fogwt/shatter_wall_recheck_t6133.png`,
> `docs/img/fogwt/shatter_matscope_diff_t6133.png`.

> ## 2026-08-09 — THE CHECKERBOARD LATTICE IS A SECOND BRDF, NOT A RECONSTRUCTION BLUR
>
> The user pushed back on "half-rate shading is a third of the CPU's canopy
> detail" — *"this still doesn't make complete sense … could be an issue in the
> checkerboard path?"* He was right. It is a **defect**, and it is not in the
> reconstruction filter at all.
>
> **Mechanism, read from source.** The wave-2 fill refuses to AVERAGE an
> env-reflective pixel (`envForceFull`, `DeferredSurfaceKernel.cpp:5003` — both
> averaging models break on reflections) and instead re-shades it with the
> scalar fallback at `:5254`. **That fallback is a REDUCED kernel.** Against the
> wave-1 scalar kernel it is missing: the `--pbr` Cook-Torrance GGX lobe (it
> runs Blinn-Phong `std::pow(NdotH, gloss)` at `:5420`), **every** shadow term
> (`computeMapShadowAtten`, `resolveCubeAtten`, the static lightmap, the PolyId
> compare, the bias pair), the AO map, the normal-map LOD fade, and
> `--hdr_metal_kill`; and it applies the spot-cone penumbra to SPECULAR where
> wave 1 does not. greets sets `--pbr` and `--shadows` ON. So alternate pixels
> of every reflective surface are shaded **by two different BRDFs**, and the
> phase is `(px ^ py) & 1` with **no frame term** — a fixed lattice that never
> averages out under motion.
>
> **MEASURED** on greets t=4871 at the user's mech pose, over the 33 478-px
> canopy mask, as *mean luma of the wave-2 cells minus the wave-1 cells* (0 if
> the reconstruction were unbiased):
>
> | arm | ODD−EVEN luma |
> |---|--:|
> | shipped | **+6.82** |
> | `--no-shadows` | +5.51 |
> | `--no-pbr` | **+0.90** |
> | `--no-pbr --no-shadows` | **−0.01** |
> | `--deferred_checkerboard=0` (full rate) | +0.04 |
> | standalone Metal arm | −0.05 |
>
> `--pbr` owns ~5.9 luma of it and the shadow terms ~0.9–1.3; with both taken
> out of wave 1 the two kernels agree to a hundredth of a luma. Whole-frame bias
> is only +0.19, because the fallback only fires on reflective materials.
>
> **FIXED behind `--deferred_checker_env_full`** (default OFF, byte-null,
> verified: greets pin and the t=4871 frame both unchanged). It shades
> env-reflective pixels at FULL rate in wave 1 instead of letting the reduced
> fallback do it. Bias +6.82 → **+0.02**; against the full-rate render the
> canopy now agrees to mean |ΔY| **0.99** (was 4.48) with 174 px > 10 luma (was
> 4 349). **Cost: none.** The fill was already full-shading exactly this set, so
> `lighting-w2` FALLS 3.51 → 3.14 ms (3/3 reps) while `lighting-w1` moves within
> noise; `renderFrame` min-of-mins 53.11 vs 53.03 ms. For scale, the "just turn
> the checkerboard off" alternative is **53.1 → 79.3 ms**.
>
> Crop (A shipped / B fixed / C full-rate / D GPU): `/tmp/fogwt/task3_canopy_lattice.png`.
>
> **STILL OPEN, not mine this run:** the same reduced fallback also fires at
> every material/normal/Z EDGE (the `neighborCompatible` miss), where shadows
> matter most. That is a broader instance of the same defect and is unpriced.

> ## 2026-08-09 — E6 / E7 now have CPU-side flags, and E7 is much smaller than §11 implied
>
> `--env_bake_include_animated` (E6) and `--env_mip_chain` (E7), both default
> OFF / byte-null. Full rationale + numbers in `FeatureFlags.def`.
>
> **TRAP RECORDED:** `g_envBakeSkipDynamic` is NOT "skip animated meshes". It is
> read in THREE places in `Transform.cpp` — the animated-mesh skip (`:1274`),
> the legacy whole-mesh exclusion (`:1549`) and **the reflector's own-FACE skip**
> (`:2396`). The first cut of E6 cleared the global and thereby let the cockpit's
> own canopy glass into its own probe: the +Y face went **91 % VOID** and the
> probe mean **100.31 → 49.11**. The shipped flag hooks `:1274` and only that.
> With it scoped correctly: probe mean 100.31 → **89.14**, all faces 100 %
> nonvoid, −Y (toward the mech's own body) 96.22 → 74.55; canopy **2 817 px**
> changed, mean |ΔY| 22.86 on changed, max 102.4; frame-wide 39 473 px (1.90 %).
> The GPU's mirror-image `--env_bake_skip_animated` moves 5 268 px / mean 24.94.
>
> **E7 IS SMALL ON THE CPU, and this corrects the emphasis in §11.** The flag
> works and has full range — `--env_mip_chain=16` drives the select to the
> bottom of the store's chain (32² face) — but a WITHIN-ARM sweep of the isolated
> env term (render minus `--no-env_refl`, 7×7 high-pass RMS on the canopy) moves
> only **24.68 → 24.32 (chain 9) → 24.05 (chain 8 + `--env_bake_res=128`, the
> exact GPU emulation) → 23.92 (chain 16)**. That is **3 %** across the whole
> dial, against the GPU's own `--env_res` sweep spanning 16.22 → 17.95 (11 %).
> Conclusion: on the CPU the canopy's high-frequency energy is **not** reflected
> detail — it is Fresnel/normal modulation of an already-smooth reflection plus
> the frame ribs and the glass. Matching the lobe width will not make the CPU
> canopy look like the GPU's; what is left is the env term's BRIGHTNESS (CPU
> +131.0 vs GPU +107.6 over the mask, i.e. E0) and probe content.

> ## 2026-08-08 — POM CAMPAIGN RE-BASELINED AFTER THE MIP FLIP (`docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-8)
>
> Every S1d number was measured with `--mips` OFF. Re-measured as OFF/ON **pairs**
> on a private worktree build (so concurrent agents cannot contaminate a figure);
> the arm reproduces its published 10 void / 73 black **to the per-pose digit**,
> and the slip ladder and the silhouette table reproduce **exactly**.
>
> - **Void is mip-INVARIANT** — 10 at both settings, same three poses. Black falls
>   ~25 % (73→52) and is a sampling artefact; do not quote a pre-flip black figure.
> - **The grazing smear does not move** — slip p90 identical to 3 decimals at every
>   cap, and `--texture_filter=2` cannot move it because `slip` is a MARCHED-UV
>   metric and the filter runs downstream of the march. The campaign needs a
>   filter-sensitive motion metric before spending more on the smear.
> - **The silhouette table is byte-for-byte unchanged.** §S1d-6 stands in full.
> - **PERF, two corrections:** `--mips` ON is **not** neutral for the parallax arms
>   (−0.6/−0.7 ms on +POM / +tess / the shell arm, −0.1 on flat), and the
>   **"tessellation and the POM arm cost the same" result does NOT reproduce** —
>   the shell arm is **+1.0 to +1.6 ms MORE expensive** at matched amplitude, at
>   both mip settings.
> - **The quad-diagonal crease is ROOT-CAUSED AND FIXED.** The lid quad really is
>   non-planar (`rooms`: 133 pairs, lid-normal angle max 3.07°, plane gap 0.0878
>   world vs a 0.0900 offset; `floor`, which has no corner verts, measures 0.0000°).
>   **`--pom_shell_lid_planar`** (new, default 0, byte-null) removes the crease at
>   zero measured cost on void/black, the silhouette and perf. It does **not** fix
>   the silhouette — two defects, two fixes.
> - ~~**NOT MINE, FLAGGED: the city pin does not reproduce, stably (2/2)**~~ —
>   **RESOLVED 2026-08-08, and it was never a code drift.** See
>   "the city pin is a function of `cache/city_envmap_cube.bin`" below. Short
>   version: HEAD reproduces **both** recorded pins byte-exactly
>   (`e1221676` default, `37e62845` under the control) when the env cube on disk
>   is the pre-flip bake. The `5476be8c` / `b88ecb7b` pair came from a **fresh
>   worktree with a cold cache**, which re-bakes the cube under the new
>   `--mips` / `--mip_fix` defaults. No unowned commit; nothing to bisect.

> ## 2026-08-08 — THE CITY PIN IS A FUNCTION OF `cache/city_envmap_cube.bin` (the "unowned drift", resolved)
>
> **There is no unowned commit. There was nothing to bisect.** The reported city
> drift is a **stale-cache artifact**, and the reasoning that exonerated the mip
> flip ("both halves moved, so it cannot be the flip") was wrong for a specific,
> reproducible reason recorded below.
>
> **Root cause.** `ComputeCityPanoramaCacheKey` (`DEMO/CityPanoramaCache.cpp:49`)
> keys the 426 MiB cube cache on **CITY.FLD's bytes + the four dims + the format
> salt + the building names** — and on **nothing else**. The bake itself
> (`bakeBuildingCubeFaces`) runs the ordinary software rasterizer, so its output
> depends on the whole shading path *and on FeatureFlags*. **`--mips` and
> `--mip_fix` change the baked cube**, and the key cannot see them. The filename
> is fixed (`cache/city_envmap_cube.bin`), so a differing bake **overwrites** the
> old one rather than landing beside it.
>
> **Measured, the full 2×2** (HEAD `787361a`, clean worktree, dummy drivers). Rows
> = which cube is on disk, columns = the flags the *frame* renders under:
>
> | cube on disk | frame `--no-mips --no-mip_fix` | frame default (mips ON) |
> |---|---|---|
> | **pre-flip bake** (`d1d67f0f…`, what the user's `Runtime/cache/` holds, dated Aug 6 03:40) | `37e62845` ✅ **the recorded prior pin** | `e1221676` ✅ **the recorded current pin** |
> | **cold/current bake** (`63978a18…`, mips ON) | `b88ecb7b` ← the "control failure" | `5476be8c` ← the "drift" |
>
> Every cell is 2/2 stable. **HEAD is byte-faithful to both published pins**; the
> two anomalous hashes are simply the bottom row.
>
> **Why the control could not exonerate the flip.** `--no-mips --no-mip_fix`
> only changes the *frame*. It cannot un-bake a cube that is already on disk,
> because the key ignores flags — so in a fresh worktree the control arm hits the
> mips-ON cube the *preceding default run just wrote* and measures the hybrid
> cell (mips-ON bake + mips-OFF frame), which matches neither pin. That hybrid is
> exactly `b88ecb7b`. **A `--no-mips` control arm on city is only valid against a
> cube baked with mips off** — delete the cube first, or the arm is meaningless.
>
> **Proof the bake, and only the bake, moved:** cold-baking at HEAD with
> `--no-mips --no-mip_fix` reproduces the user's Aug-6 cube **byte-for-byte**
> (`d1d67f0f84fb4af3713e15a64a1b827b`, all 446 694 000 bytes). So across every
> commit from Aug 6 03:40 to HEAD, **no change altered the city env bake** other
> than the mip defaults. Both flags contribute (`--no-mips` alone → `1775b64c…`,
> `--no-mip_fix` alone → `88fec906…`; neither alone is either reference), which
> matches the known split: `--mips` zeroes the LEVEL, `--mip_fix` moves the
> subdivision cut lines.
>
> **Verdict: the new bake is CORRECT, not a regression** — it is the direct,
> intended consequence of the user's own `--mips` default flip finally reaching
> the env-cube bake, which the stale cache had been masking. It is also tiny:
> pinned vs cold-bake frame is 164 536 px changed (7.94 %) but **max channel Δ
> 6/255**, mean Δ-sum 1.63/765 — and the delta is confined to the **glass panes**
> (zero on the adjacent non-reflective wall), which is the expected signature
> since the cube feeds only the env-specular compose. Before/after/|Δ|×32 crop:
> `docs/img/mipsel/city_t1961_envbake_crop.png`. **Not re-pinned yet — the look
> change wants the user's eye first** (see the pin-table row).
>
> **TWO LIVE HAZARDS, both unowned:**
> 1. **The user's `Runtime/` is serving a pre-flip env cube.** His demo renders
>    city reflections baked under the *old* mip defaults, and will keep doing so
>    forever — the key will never invalidate on its own. To adopt the flip
>    properly: `rm Runtime/cache/city_envmap_cube.bin` and re-run.
> 2. **Any run in `Runtime/` by a binary whose bake differs silently overwrites
>    that cube**, permanently moving the main-tree city pin with no commit and no
>    trace. This is a live footgun for every agent.
>
> **The fix** (not applied — `DEMO/CITY.CPP` / `DEMO/CityPanoramaCache.cpp` were
> not mine to change this run): fold the bake-affecting FeatureFlags into the
> key, e.g. mix `mips`/`mip_fix` (and any future bake-affecting flag) into
> `cubeSalt` at the `ComputeCityPanoramaCacheKey` call site in
> `DEMO/CITY.CPP:2581`, **and** put the key in the *filename* the way
> `pom_cone_exact_%016llx.bin` / `pom_horizon_%016llx.bin` already do
> (`DEMO/MeshOps.cpp:775,959`) so variants coexist instead of clobbering. Note
> those two POM caches do **not** have this hole — `ConeExactCacheKey` hashes the
> actual input texels plus every parameter, so it is a real content key.
>
> **Stale analysis this corrects:** the `--mips` re-pin's recorded divergence for
> city ("133 854 px, mean |d| 7.04, max 192, building facades") measured only the
> **frame** half of the flip, because the bake half was masked by the cache. The
> bake half is the additional, much subtler 164 536 px / max 6 above.

> ## 2026-08-08 — MIP SELECTION IS ON BY DEFAULT; ALL SCENE PINS MOVED
>
> **`--mips` default 0 → 1** (user decision). Mip selection had been force-disabled
> since the legacy `NO_MIPMAPS` define: `MiplevelClipper` computed a level and then
> every exit path threw it away. That pinned LEVEL 0 for the albedo **and for the
> normal / roughness / metal / AO chains**, which the deferred kernel indexes by the
> same miplevel — five map sets whose levels 1..N were built, paid for in memory, and
> never read. The flag's old justification ("1998 textures are magnified so mips
> barely engage") argued from NEAR surfaces to justify disabling selection on DISTANT
> ones, and predated the sidecar PBR sets; it is retracted.
>
> **Measured** (greets t=2993, 1080p, `--deferred --texture_filter=1`, new `--mip_stats`
> histogram): OFF = 100 % of draws and 100 % of covered area at level 0. ON = 7.6 % of
> draws / 83.3 % of AREA at level 0, remainder across levels 1-8, **48.8 % of DRAWS at
> level 6**. Branches: 56 115 faces entered, 55 679 face-uniform, **436 (0.78 %) took the
> subdivision path** — rare, but it owns the large near faces.
>
> **Perf is NEUTRAL**: min-of-arm over 5 interleaved 20-iter rounds, greets t=2993 RNDR
> 39.855 ms off vs 39.965 ms on. No measurable texture-cache win, no measurable cost.
> The machine was loaded by concurrent agents (individual rounds 39.8-91.4 ms), so only
> the min is meaningful and nothing under ~0.2 ms is resolvable here.
>
> **ALL SCENE PINS MOVED** (city, fountain, greets, chase ×2) and are re-derived in the
> table below, each with a `--no-mips --no-mip_fix` control proving the move is the
> flip's and not some other drift. `tools/render_gate.sh` baselines did NOT move.
> **Two pre-existing drifts surfaced and are NOT mine: chase t1600 (both default and
> cinematic arms) no longer matches its 2026-07-30 pin even with mips off.**
>
> **`--mip_fix` default 0 → 1** — the split branch's depth ramp coefficient (K=1, not 2;
> texel area per pixel goes as z², independently re-derived). Its earlier "MEASURABLY
> BROKEN" verdict was a **zsh word-splitting artifact** (a `'--mips --mip_fix'` shell
> variable arrived as one argv token and was silently ignored); `--strict_flags` now
> makes that class of error fatal. **Correction: `--mip_fix` is NOT inert when `--mips`
> is off** — the mips gate zeroes the mip LEVEL but not the SUBDIVISION, and this flag
> moves the cut lines, so it changes geometry either way.
>
> **D3 (SHADING_CONTRACT) — the normal-map LOD fade after the flip: MEASURED, NO
> ACTION NEEDED.** The concern was that 48.8 % of draws sit at mip 6, so the flip
> pushes half the frame into the faded/flattened regime in one step. **That is true in
> DRAW count and false in SCREEN AREA — which is the number that matters, and the two
> differ by ~70x here.** Fade is `1-(mip-start+1)*step` with start=2, step=0.33, so
> full bump at mip 0-1, 0.67/0.34/0.01 at mip 2/3/4, fully FLAT from mip 5 up.
> Area-weighted, at six poses (`--mip_stats`):
>
> | pose | full bump | partial | FLAT | bump retained |
> |---|---|---|---|---|
> | greets t=2993 | 88.2 % | 9.1 % | **2.7 %** | 91.4 % |
> | greets t=4200 vista | 85.8 % | 11.0 % | **3.2 %** | 89.7 % |
> | greets t=5958 grazing | 85.9 % | 10.7 % | **3.4 %** | 89.3 % |
> | greets t=5743 review | 85.6 % | 11.2 % | **3.2 %** | 89.6 % |
> | city t=1961 (gate) | 80.3 % | 18.9 % | **0.8 %** | 90.2 % |
> | fountain t=2500 (gate) | 89.7 % | 10.3 % | **0.0 %** | 95.1 % |
>
> The 48.8 % of draws at mip 6 cover **0.7 % of screen area**. Direct check — disabling
> the fade ENTIRELY (`--nmap_lod_fade_start=16`): greets t=4200 changes **0.35 % of
> pixels** (77 px >12/255), city t=1961 changes **ZERO pixels**. Worst-region crop
> `docs/img/mipsel/t4200_nmap_fade_on_vs_off.png` is visually indistinguishable
> (mean \|d\| 0.16). **No "wall goes geometrically flat at distance" is occurring at a
> visible scale, so the threshold does NOT need retuning.**
>
> **Fade vs Toksvig/LEAN — settled by that same measurement: implement NEITHER.** The
> fade is a crude stand-in for proper normal-map mip filtering, and Toksvig would be a
> refinement of it. But the fade's total footprint post-flip is ≤0.35 % of pixels and
> 0 % at the city gate, so roughness coupling would be buying a correction to a term
> that barely fires. Revisit only if content changes push real area past mip 4.
>
> **GPU-PARITY WARNING: the GPU arm has NO normal-map fade at all.** After this flip
> the CPU flattens bump on ~3 % of greets' screen area that the GPU still perturbs, so
> CPU-vs-GPU pairs at distant surfaces now diverge BY CONSTRUCTION. Neither renderer is
> wrong. Do not chase it as a GPU bug.
>
> **Unrelated pre-existing hazard (D6), flagged so it is not misattributed to mips:**
> the CPU's AO is unclamped and can go negative at `ao_strength=2.0`, subtracting
> direct light. If a new artifact appears near AO'd geometry after the flip, check that
> first — the flip changes which AO texels are sampled but did not create the bug.
>
> **Two corrections to my own earlier claims, both measured:**
> 1. `--mip_fix` is **not** inert with `--mips` off (above). The mips gate zeroes the
>    mip LEVEL, not the SUBDIVISION.
> 2. The first cut's lazy-`BaseLod` refactor was **not byte-null**: `_C` was
>    `0.5 * fastLog2(...)` in **double** and the lambda made it float. `_C` positions
>    the subdivision cut lines, so re-associating that arithmetic moves geometry even
>    when the level is forced to 0. Fixed by restoring the legacy expression verbatim
>    on the non-aniso path — **the `0.5 *` there must stay double.**
>
> **Trilinear**: `--texture_filter=2` stops silently degrading to bilinear now that
> `mipFrac` is no longer force-zeroed — 53 888 px (2.60 %) differ at greets t=4200.
> **`mip_bias` 0.5 + truncation = round-to-nearest**, which is correct for point and
> bilinear but WRONG for trilinear: it offsets the inter-level blend by half a level.
> `--mip_bias=0` is the correct pairing with `--texture_filter=2` (derived, not yet
> visually validated).
>
> **Process hazard, recorded because it bit this work twice:** the `--mips` flip and
> then the whole re-pin changeset were both swept into OTHER agents' commits
> (`99c09e7`, `daeb147`) because `FeatureFlags.def` and the git index are shared. A
> commit titled "S1d-6: the shell's silhouette" is what actually flipped a default
> that moved every scene pin.
>
> New: **`--mip_stats`** (per-level draw/area histogram at exit, changes no pixel) and
> **`--mip_aniso`** (max-axis LOD instead of the geometric mean — default OFF, awaiting
> the user's eyes). Crops: `docs/img/mipsel/`. Full write-up in the commit message.


> ## 2026-08-06 — THE MITRE INVERSION IS ROOT-CAUSED; THE WELD IS NOW DEFAULT ON
>
> **`--pom_shell_weld` default 0 → 1** (commit `140b6a0`). Inert unless
> `--pom_shell` selects the lid, and `--pom_shell` is itself default OFF, so no
> shipping render moves — proven, all four gates re-run after the flip and still
> byte-exact. Within a lid arm the unwelded mesh is TORN: **232 612 → 14 163
> void px over the 16 review poses (−93.9 %)**, and the pixels it removes are a
> **full-height black gash between wall panels** at p9 t5958 plus the wall/floor
> wedge at p5 t5963 — the defect the user reported.
> `docs/img/s1d_2f/weld_gash_*.png`.
>
> **The open bug "the true mitre is geometrically correct and measures worse" is
> CLOSED, and the answer is: it optimises the wrong component.** At a fold of
> half-angle `T` the mean-normal weld moves a corner `off·cos T` along each
> incident plane's normal and `off·sin T` **tangentially**; the mitre divides by
> `cos T`, making the normal part exactly `off` and the tangential part
> `off·tan T`. Nothing consumes the normal exactness — `Vertex::ShellH` already
> records the height each corner reached — and the tangential part is what
> slides a patch's BOUNDARY sideways and opens the holes. Cleanest measurement,
> `weld=5` vs `=6` (identical pin set, differing only by the mitre): tangential
> slide 0.0450 → 0.0712 world (×1.58), **void 10 648 → 26 774 (×2.51)**.
> **98–100 % of every mode's extra void carries `--pom_path_viz` code 0 — no
> fragment rasterised at all — so it is geometry and no march-side hypothesis is
> involved.** Do NOT use `--pom_shell_weld=4` or `=6`.
>
> **The formula `off·(1−cos(half-fold))` in S1d-2e.5 and RESEARCH_II §8.5 R2 is
> retracted** — it is `off·sin T`, which is the 0.064 world S1d-2e.5 measured.
> The number was right; the formula was not.
>
> **Two things this changes for the prism (RESEARCH_II §8.6):** precondition 1
> must say "weld, but NOT with a mitre — minimise tangential slide", and there
> is a new precondition **5b, T-JUNCTIONS**: greets carries 140 (edge,T-vertex)
> pairs among the shelled faces alone and they own **70 % of `weld=4`'s void**.
> The mitre's whole difficulty is also specific to the LID-ONLY shell and is an
> argument FOR the prism — adjacent prisms share a side quad, so they stay
> watertight while their lids move apart.
>
> New diagnostic `--pom_shell_slit_census` (default OFF, init-time print, lives
> wholly outside `PomShell_Build`). Full write-up:
> **`docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-2f** (commits `2839c29`, `f2933f7`,
> `dc2e231`, `140b6a0`).

> ## 2026-08-06 — GEOMETRIC TESSELLATION IS BACK ON THE TABLE: +7.3 ms, NOT +54.5
>
> **`--greets_displace` was retired on a number that was wrong by 7.4×.** It is
> now a first-class, working, one-flag option and it is **CHEAPER than the
> recess-shell arm at three of six review poses**. Full tables, look crops, the
> §C4 re-verification and the gates are in
> **`docs/ENVDYN_DISPLACEMENT_PLAN.md` §ADDENDUM 2026-08-06** (commit `1a91ed5`).
>
> **Measured, t=5780, 1080p, 12 threads, iters=20, interleaved, min-of-arm:**
> flat POM **48.5–49.5** · recess shell **56.1** · **tessellation 55.6–55.9**.
> Per pose (min-of-5): tess−flat is +2.4 / +4.0 / +4.1 / +13.8 / +13.9 / +14.1
> and **tess−recess is −4.2 / −2.0 / −1.3** at the corner, grazing-close-up and
> corridor poses. The shell's cost is per-PIXEL and explodes at grazing;
> tessellation's is per-FACE and nearly pose-independent.
>
> **Why the old number was wrong — three landings, none of them tessellation:**
> `9b6d70d --tile_bbox_cull` (default ON) landed **1 h 40 m AFTER** the
> edge-carve commit whose "107.0 ms" the plan quotes, and its own message
> measures the displaced arm 100.0 → 87.0 ms; `a1f89d4 --xfrm_soa_inline` −2.0
> ms; `799c808` removed a faceless mesh that was **84.3 % of that arm's 6.83 M
> shadow verts/frame**. Then this session found the fourth: **the mirror clone
> was re-transforming and re-rasterising the entire tessellated wall** (11.40
> ms/frame vs 3.31 in the flat arm; the clone pushed 42 870 faces while the
> direct view pushed 28 598, because a clone is culled by the frustum and not by
> the mirror WINDOW).
>
> **`--greets_displace` now defaults two perf companions ON** (a `[STONE]` log
> line names them; `--no-<flag>` still wins; both inert without displacement, so
> the shipping flat-POM arm is byte-untouched): `--greets_shadow_proxy` (−5.9 ms;
> **not look-neutral** — byte-identical at 5 of 16 review pairs, worst t=6097
> 58 021 px >12/255 at the corner junction) and the new
> `--greets_displace_flat_mirror` (−5.9 ms; **byte-identical at both mirror
> review poses**, 2 990 px >12/255 at t=5743). One flag = the affordable arm,
> byte-verified identical to spelling all three out.
>
> **Per-face cost, the user's own question, answered:** 92 ns/face threaded ≈
> **0.60 µs/face core**, against the 2–2.8 µs serial the campaign has been
> reasoning with — **3.3–4.7× cheaper**, almost all of it `--tile_bbox_cull`.
> Which is also why the S2/S5 chunk LOD is **not built**: with the companions on,
> the 87 k-face edge carve and the 43 k-face dome path are **0.22 ms apart**, so
> halving the faces buys ≈0.2–3 ms. Ceiling measured, reasoning in §A4.
>
> **What tessellation still cannot do (§C4, re-verified today):** relief lives
> only at the lattice. At t=6097 it writes **0.0023 world = one zEnc code** over
> a 600×400 box that a depth-writing per-pixel arm resolves at 0.0110–0.0233; at
> t=2845 it carries 83 %. **What only it can do:** true silhouettes, real depth
> for every offscreen consumer, and geometry that cannot swim — at t=5958b, the
> grazing pose where the shell smears and slip p99 hits 501, it renders a crisp
> geometric step.
>
> **Still open:** `--greets_displace` at t=6097 is run-to-run nondeterministic
> (6 runs, 6 hashes) while t=5780 is stable 6/6 — not root-caused, and the one
> thing between this arm and full gate-worthiness.

> ## 2026-08-06 — WHERE THE DISPLACEMENT CAMPAIGN ACTUALLY STANDS (the shell half)
>
> **The per-pixel shell can produce protrusion the user likes ("fantastic when
> it works") but not at a setting that is also stable.** That tension is the
> campaign's central measured finding, and it is not a bug in one code path —
> four rounds of hypotheses (cone march, `--pom_normal`, step exhaustion,
> bitangent handedness) were each measured and each REFUTED as the cause.
>
> **The instrument that finally matched the user's eye: SLIP** — texels of
> texture sliding per frame at a fixed point on the stone (`--pom_path_viz`
> mode 2 + the `_uvgeo.bin` camera-free surface coordinate). Arms the user
> calls clean measure p90 0.01–0.12; the arm he called "swimming like a shark"
> measures **p90 15.3 / p99 501** — half a texture tile per frame. Every
> earlier metric (jerk, frame-diff, error-vs-reference) disagreed with his eyes;
> this one agrees.
>
> **Cap ladder** (recess arm, slip p99 / reach p90; clean floor 0.60):
> cap2 0.82/28.6 · cap4 **1.44/53.8** · cap8 3.41/94.5 · cap16 11.4/184 ·
> cap32 37/338 · cap64 **501**/109. Cap 64 has LESS reach than cap 32 — it
> pushes 18.5 % of the wall into the flat clamp. Cap 4 sits at the clean floor
> with ~14× the non-shell arm's reach.
>
> **Why the mechanism, not a bug:** our shell marches the TRUE view ray
> (÷V·N, capped) where classic POM uses the OFFSET-LIMITED form. That was a
> deliberate S1b choice ("grazing lateral travel is exactly what silhouettes
> are made of"). Slip scales with the CAP and NOT with step count (32/128/512
> identical). A hard offset clamp has no usable band (24 texels = no relief;
> 48/64 = polygon artifacts). **Recess-only is clean (0 void, 0 offscreen
> delta) but structurally cannot show a gap between blocks** — the user
> confirmed by eye that cap 4 recess "just gives something equivalent to the
> regular parallax".
>
> **OPEN, and it decides the campaign:** does the LID arm at cap 4/8 show real
> see-through between blocks? If yes, protrusion is viable at a stable setting.
> If no, the swim-free band and the see-through band do not overlap, and
> recess-only is the shippable result with real geometry the only path to
> silhouettes.
>
> **Also measured, do not re-litigate:** `--pom_cone_min_step=1` with only 32
> march steps leaves 9.5k–52k px UNRESOLVED (no parallax shift at all, a hard
> discontinuity clustered at steep block edges); `--parallax_pom=128` drives it
> to 0, and removing the floor makes it 20× worse (209k px — a true per-texel
> cone truncates to a zero step and the march stalls). The march was ALSO
> missing `Material::TbnHandedness` (real bug, fixed `7bfbc87`) — but applying
> it moved slip by nothing, and the per-face variant made the p99 tail WORSE,
> which is evidence the per-face determinant test itself is wrong (it only
> tests 3 verts, only on normal-mapped materials; scene-wide agreement is
> **40.1 %** — 302 of 406 `rooms` faces are negative-determinant but sit on a
> handedness=+1 material). That affects the DEFERRED KERNEL's normal mapping
> too, independent of displacement. Queued in OPTIMIZATION_BACKLOG.
>
> **In flight at write time:** (a) mirrors reflecting the flat proxy while
> still running the parallax march (user-approved; measured that the march DOES
> read in the reflection — 96.3 % of mirror pixels change without it) + a
> shadow-pass geometry decomposition; (b) `docs/DISPLACEMENT_RESEARCH_II.md` —
> a literature re-read against these measurements, whose hinge question is
> whether anyone ever SHIPPED silhouette-correct per-pixel displacement or
> whether shipped POM simply had flat edges; (c) `docs/GPU_BENCHMARK_PLAN.md` —
> a standalone GPU deferred path as a BENCHMARK and ground truth (user: "not as
> a shipping backend"), gated out of the normal build.
>
> **Perf, measured this session:** XFRM main-view 7.9 ms at 958k verts, of
> which the SoA dual-write was 2.40 ms — removed (`--xfrm_soa_inline`, default
> ON, bit-exact, **−25 %**). The transform loop is CACHE-LINE-BOUND (`Vertex`
> is pack(1) 140 B), NOT arithmetic-bound, which is why wider SIMD washed and
> why an approximate reciprocal measured SLOWER. A BVH/hierarchy is refuted
> with numbers (0.45 core-ms ceiling over ~9,150 mesh tests/frame). **The real
> geometry elephant is the SHADOW passes: 33–36 calls/frame, 7.6M verts,
> 340–790 core-ms**, vs main view's 0.96M / 4–7 ms.
>
> **Review poses live in `docs/greets_review_poses.txt`** — every camera the
> user has reported a defect from. Use them; agents kept re-deriving them.
> **F4/F3 scrub the scene clock** at `--scrub_speed`× (default 4).

> **2026-08-05 — GREETS RENDER NONDETERMINISM IS CLOSED. GREETS IS A GATE
> SCENE AGAIN.** Root cause: the opaque deferred kernel read AO maps with the
> WRONG TEXEL WIDTH. `Material::AoMap` arrives from the importer as
> single-channel **8-bit** (`MakeHeight8`, same as height/roughness/metallic),
> but `DeferredSurfaceKernel.cpp` fetched it as `dword` —
> `((const dword*)mip)[swizzledUV]` — so every AO sample sat at byte offset
> `4 × swizzledUV` inside a **1-byte-per-texel** allocation. **Measured** at a
> diverging pixel: `swizzledUV = 995355` in a 1024² (1 MiB) mip → byte offset
> **3,981,420**, i.e. **3.8 MB past the end**. The returned heap bytes differ
> per process; with `ao_map_strength` 2.0 they drove `ao = 1 - 2·(1-aoRaw)`
> down to **-2.22**, the ambient term went **negative**, and `lB<0 → 0` clamped
> it — the long-hunted "diffuse flips 0 ↔ full while specular stays identical".
> Every sibling map fetch (roughness, metallic, xpar-AO) already read bytes AND
> bound-checked `miplevel < numMipmaps`; only this one did not. Fix mirrors the
> transparent kernel's AO fetch. See the Known Issues entry for the full chain.
> - **RESULT [M]:** greets gate recipe **0 flips in 128 sequential runs**, one
>   hash `f5778c7b78a4d70655291363e4119c66` (95 % upper bound on the true rate
>   **0.023**, ~1 in 43). Pre-fix the same recipe flipped **~0.85**. Also 0/16
>   with `--env_refl` ON and 0/16 under `--vanilla` (forward path).
> - **LOOK CHANGE [M]:** greets now shades with the REAL AO map instead of heap
>   garbage — **26.3 % of the frame moves, mean |Δ| 12.4, max |Δ| 98** at the
>   gate pose. Four materials carry separate AO maps (`momy-1`, `amudim`,
>   `stairs`, `rooms`); `--greets-stone-tex` materials use `Mat_AoInAlpha`
>   (albedo alpha) and were never affected. **This wants the user's eye** — it
>   is a bug fix, not a tuning call, but the wall/pillar occlusion look changes.
> - **Gates unchanged:** city `37e62845`, fountain `51fff7cd` byte-exact;
>   render_gate 3/3. The bug only fires on materials with a separate 8-bit
>   `AoMap`, which only greets ships.

> **2026-08-05 — GREETS NOW SHIPS THE PBR STACK BY DEFAULT, and `--vanilla`
> turns everything back off.** Two user-requested changes on fog-wt.
>
> **(A) Greets scene defaults gained five flags** (`GreetsApplyRunDefaults`,
> DEMO/GREETS.CPP — the RUN block, not init, because all five are global render
> flags): `pbr`, `env_brdf_analytic`, `pbr_multiscatter`, `diffuse_energy`,
> `sh_ambient`. That is exactly the set the user typed on every greets launch.
> Applied via `FF::setDefault` behind the existing `GreetsScenePreempted()`
> guard, so an explicit `--no-pbr` still wins and `--scene-mirrortest` /
> `--scene-conetest` never inherit them.
> - Derived from the kernel, not from the flag list: `pbr` is read at
>   DeferredSurfaceKernel.cpp:1429 and drives BOTH the scalar per-light branch
>   (:2400) and the 8-wide vec loop (:2156) — the flag's own help text saying
>   "vec path only" is STALE, greets' normal-mapped pixels take the scalar
>   branch and do get GGX. `env_brdf_analytic` (:1439), `pbr_multiscatter`
>   (:1440) and `diffuse_energy` (:1442) all sit behind `env_refl`, which greets
>   gets for free: its RVSM metallic-map imports (momy / amudim / screen emiter)
>   call `setDefault(env_refl,true)` in MaterialImport at init — **measured in
>   the init log**, which is why those three are not dead defaults.
>   `pbr_multiscatter` is a strict no-op without `env_brdf_analytic` (it reuses
>   its A,B terms), so the pair ships together. `sh_ambient` (:1445 +
>   RENDER.CPP:489) is independent of env_refl.
> - `metal_map` / `roughness_map` / `ao_map` are ALREADY compile-default ON in
>   FeatureFlags.def — verified, nothing added for them.
> - NOT included: `pbr_roughness`, `deferred_vec_force` ([test] knobs), and
>   `xpar_pbr` — which is not a dependent of `pbr` at all (the transparent
>   kernel reads it independently at :2785 and never reads `pbr()`); turning the
>   greets glass PBR is a separate look call nobody has made.
> - **COST [M]** greets bench t=5780, 1920×1080, `FDS_THREADS=1` (one core is
>   the only load-robust arm on a box other agents are rendering on), 6
>   interleaved pairs: **+20.0 ms/frame (+5.3 %)** in the least-loaded pair,
>   +25.9 ms on min-of-arms, ~+39 ms median. The 12-thread A/B could NOT resolve
>   it — 10 interleaved pairs spanned 60.8–199 ms/iter under load 9–47 and min-ON
>   (60.8) came in *under* min-OFF (63.3). Inferred, not measured: at the
>   observed ~6.5× pool speedup that is **~3–6 ms/frame** in a normal run.
>
> **(B) `--vanilla` / `FDS_VANILLA=1`** (FeatureFlags.def + .cpp, category
> engine, default OFF): forces EVERY flag to its compile-time default AND marks
> it explicitly-set, so scene `setDefault` blocks (greets' new PBR set included)
> and `SCRIPTS/*.params` are suppressed too — without the set marks "vanilla"
> would be a lie. **Semantics are pure parse order: put it FIRST.** Proven on
> conetest: `--vanilla` + the render_gate cone recipe = `b41894f9…`, byte-equal
> to the gate baseline; the same recipe with `--vanilla` LAST = `1bc0dc35…`.
> It CLEARS ENV-SET VALUES (the eager FDS_* scan runs before argv, so the CLI
> form wipes it; the env form is applied after the env scan for the same rule),
> works inside `--flags-file` and `FDS_BAKED_ARGS`, and prints a one-line
> `[FLAGS] --vanilla: 424 flags forced…` note so a run is self-identifying. It
> is compile-time DEFAULTS, not all-off: `deferred` defaults off, so a vanilla
> run renders the FORWARD path. Startup-only — the tune console returns 400
> rather than pretending a live mass reset works.
>
> **(A) is REAL and frame-wide, measured against the noise:** three ON runs vs
> three OFF runs of the greets gate pose (`--no-shadows` variant), pairwise —
> within-arm run-to-run noise touches 0.64–14.0 % of the frame, cross-arm ON-vs-OFF
> touches **80.0 / 88.9 / 88.9 %** with max |Δ| 231 and mean |Δ| 4–10 on the changed
> pixels. A broad low-amplitude shift over nearly every lit pixel is exactly the
> signature of swapping the BRDF + the ambient model, and it is an order of
> magnitude outside the noise. `[SHAMB]` appears in every ON run's log and in no
> OFF run's — the SH probe really is baking.
>
> **Gates**: city `37e62845` and fountain `51fff7cd` byte-unchanged;
> render_gate 3/3 (the preempt guard holds); chase byte-identical to the SAME
> binary's pre-change run at all five poses + both cinematic poses. ~~The greets
> pin could not be re-taken — greets is currently 100 % nondeterministic~~
> **SUPERSEDED same day: the nondeterminism was the 8-bit-AO-map-read-as-dword
> bug (see the top block); greets is re-pinned and gate-worthy again.**

> **2026-08-05 — S1d-2d: THE LID ARM'S VOID WAS NEVER THE MARCH. It is the lid
> offset TEARING THE MESH.** Read `docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-2d. Four
> new flags, all default OFF and byte-null: `--pom_shell_weld`,
> `--pom_shell_lid_edge`, `--pom_shell_side_entry`, and mode 3 of
> `--pom_shell_side_faces`.
> - **The discriminator, before any code.** Lid arm void 413 100 px over the 13
>   review poses. With the domain kill AND the base clip both off: 228 411. With
>   `--no-parallax` (no march at all): 198 704. With `--pom_shell_lid_probe`
>   (offset forced to 0, everything else identical): **0**. Against the offset
>   0.02/0.06/0.18/0.36 world: 19 416/58 665/198 131/383 364 — **LINEAR**. The
>   void is a SLIT IN THE GEOMETRY whose width is the lid offset.
> - **Cause:** `PomShell_Build` extrudes along each vertex's OWN normal, and
>   `MakeFacesIndependentByAngle` leaves `rooms` with 588 verts over 196 faces —
>   exactly 3 per face, nothing shared. 155 distinct POSITIONS carry them; 420
>   uses disagree with their position's mean normal by >1°, worst 78.7°.
> - **`--pom_shell_weld=1`** extrudes along the shared (welded) normal, as shell
>   maps do — a mitred corner instead of a wedge. `Vertex::N` untouched, so
>   shading is unchanged; `ShellH` picks up the mitre automatically (min
>   0.955 → 0.598). Void 413 100 → 214 650, and 228 411 → **14 163** with the
>   other two kills off.
> - **`--pom_shell_lid_edge=1`** gives the lid arm the recess arm's CLAMP for a
>   lateral exit only — a non-crossing ray, a side-entry miss and lid overhang
>   still DISCARD, so the see-through survives. That was the other ~152 000 px.
> - **`--pom_shell_side_faces=3`**: the lean must bound the shell only BELOW the
>   authored plane (`dh = max(0, h0−h)`). Above it the neighbour's SHELL bounds
>   this one, and with the weld the two lids already meet at the ridge. Modes 1/2
>   narrow it instead and kill lid rays with real material under them —
>   **67 816 px of pure black**. Byte-identical to mode 1 under recess-only.
> - **`--pom_shell_side_entry=1`** — the restructure the task asked for IS BUILT
>   and is correct: the ray and all four leaning side planes are affine in the
>   slab height, so entry is one convex slab clip and the march starts at the
>   side-face crossing. Nothing serialises (`hStart` was already a `Vec8f`).
>   Depth needed no change (the S1a write is relative to the RASTERED surface).
>   **It is not what was blocking protrusion**, and with mode 3 it is inert by
>   construction — so the two are alternatives, not a stack.
> - **RESULT.** Recommended lid arm = `--pom_shell_weld=1
>   --pom_shell_side_faces=3 --pom_shell_lid_edge=1 --no-pom_shell_base_clip`:
>   **void 413 100 → 14 163 (−96.6 %)**, nine of thirteen poses at 0–159, and the
>   **rust stripe is gone** (crop `docs/img/s1d_entry/p5743_B_...`). Residue is
>   13 986/14 163 GEOMETRIC — the cross-material wall/ceiling junction the
>   per-material weld cannot close.
> - **WHAT IT DOES NOT FIX: offscreen.** Shadow cube vs flat: rec 0, tess 5.32 %,
>   lid 20.68 %, this arm 18.77 %. Non-stone colour >12/255 at p5743: rec 1 411,
>   tess 1 035, lid 9 605, this arm 9 033, and slightly WORSE at both mirror
>   poses. Moving vertices is the lid model's intrinsic cost.
> - **SEE-THROUGH IN THE VALLEYS: still not demonstrated**, and I believe
>   structurally so — greets is a closed room, so a mortar valley has nothing
>   behind it to reveal. Measured: 0–23 px per pose of "a surface >3 world behind
>   the wall wins", even at 3.3× amplitude.
> - **Concave-fold Z-competition hypothesis: FAILS in the recess arm.** 63.4 % of
>   ANGLED_IN clamped pixels (76 765 of 121 014) void under a discard — no second
>   fragment exists, because at an inside corner two walls ABUT on screen rather
>   than overlap.
> - Gates: render_gate 3/3, city `37e62845`, fountain `51fff7cd`, greets recess
>   AND lid arms **depth AND colour** byte-identical at all 13 poses with the
>   flags off — against a binary built from the PARENT COMMIT in a clean
>   worktree, run from the same `Runtime/`. wasm links, 0 bad flags in 578 run
>   logs. Perf: marginal cost ≤ ~0.5 ms/frame as an upper bound from min-of-10 —
>   the machine carried load average 5–15 all session and I could not resolve it
>   better.
> - **greets COLOUR is a usable byte gate again.** After `f4e81e9` (the
>   concurrent AO-width fix) the same recipe gives 3/3 identical colour hashes at
>   p5743/p5958b/p6097. Before it my flags-off pair differed at 13/13. That
>   commit also swapped `Runtime/DEMO` under one of my render batches, so every
>   table in §S1d-2d was re-rendered on the post-fix binary and reproduced to the
>   digit.
> - **Trap recorded:** `--pom_shell_side_edge` must NOT be used with the lid.
>   `PomShell_Build` runs once per material, so `floor`'s seam census sees
>   `rooms` already displaced and mis-labels 19 of 24 sides as free edges.

> **2026-08-05 — S1d-2 CLOSED SHELL (SIDE FACES) IS IN, all flags default OFF.**
> Read `docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-2. Flags: `--pom_shell_side_faces`
> (0/1/2) and `--pom_shell_side_edge` (0/1/2).
> - **PROVENANCE WARNING:** the code, the doc section and `docs/img/s1d_side/`
>   were swept into commits `3712f00` and `2c54ae9` ("editor: displacement
>   panel …") by a concurrent session running `git add -A` in the same worktree
>   while this stage was finishing. The commit titles do not describe the
>   S1d-2 content they carry. Nothing is lost; the log is misleading.
> - **Step 1 (side faces).** At a convex ridge the side face is the neighbour's
>   plane and it LEANS: the solid is the intersection of the half-spaces, so the
>   material reaches cot(fold)·depth past the ridge and the vertical UV box cuts
>   it off there. Four leaning half-planes, baked from S1d-1's topology.
>   Measured over all 13 review poses: pixels the march cannot answer
>   **809 415 → 629 711 (−22 %)**; the subset that goes BLACK
>   **231 073 → 129 579 (−44 %)**; at the user's gash pose t=5743
>   **85 065 → 28 634 (−66 %)**; at the smear pose t=5958b
>   **20 244 → 4 115 (−80 %)**. Void stays at 5 (tess 13, flat POM 5).
> - **LOOK is NOT a clean win.** The gash narrows to a sliver and the t=5958
>   mortar joint tightens toward tess — but at t=5743 the recovered band renders
>   as a **saturated rust stripe**: the lean puts the ray in the right place and
>   then samples patch A's chart EXTRAPOLATED (up to 0.06 UV = 61 texels = 0.36
>   world) where the content belongs to patch B. That is S1d-2c's hand-off,
>   now a measured argument rather than a projected one.
> - **Step 2 (per-class edge policy) is the cheapest win.** Keyed on a per-side
>   TRUE-BOUNDARY SUB-INTERVAL, not the dominant class — a free edge is 0.5 % of
>   `rooms` boundary length but owns 11.9 % of the unanswered pixels, so the
>   dominant-class version fired on **0 pixels**. With the interval:
>   **100 570 px at t=6097 (4.85 % of frame) at ZERO void cost**, and the corner
>   silhouette moves toward the tessellation reference.
> - **PROTRUSION IS NOT RESTORED.** Side faces make the lid arm WORSE: void
>   413 100 → 468 868 (clip kept) / 933 535 (clip replaced). Mechanism: the same
>   lean that widens the shell below the authored plane narrows it above, and my
>   side faces are only a domain TEST — a lid ray they reject is killed instead
>   of ENTERING the shell lower down through the side face. Side-face ENTRY (a
>   per-lane march start height) is the next increment and protrusion needs it.
> - Gates: render_gate 3/3, city `37e62845`, fountain `51fff7cd`, greets recess
>   arm depth byte-identical at all 13 review poses, wasm links, 0 bad flags in
>   299 run logs. Crops: `docs/img/s1d_side/`.
> - **Process note:** `DEMO/CMakeLists.txt` copies the freshly-linked binary into
>   `Runtime/DEMO` on every `cmake --build build`. Never build while a render
>   batch is in flight — it cost me one arm that looked like a regression.

> **2026-08-05 — S1d-1 SEAM CENSUS DONE, and it OVERTURNS the S1d plan's
> premise.** Read `docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-1. Two new flags,
> **both default OFF, byte-null**: `--pom_seam_census` (patch-boundary topology
> + classification, init-time print) and `--pom_seam_viz` (class-coloured
> boundary overlay).
> - Of the **800 513 px** the march cannot answer across all 13 review poses,
>   **31 (0.004 %) sit at a COPLANAR seam.** 72.9 % sit at a CONVEX angled ridge,
>   15.1 % at a concave fold, 11.9 % at a true boundary. Of the 231 064 px that
>   actually go BLACK: 0.013 % coplanar, 66.8 % convex ridge, 33.2 % concave.
> - **Coplanar continuation is already shipping** as `--pom_shell_merge_uv`'s
>   sibling boxes (measured: carries 70 585 px, up to 67.6 % of the would-be
>   population at the mirror poses) and the coplanar UV transform is the
>   **IDENTITY** (worst disagreement 1e-6 UV). So S1d's option (A) as scoped is
>   already done and worth 31 more pixels.
> - **(B) side faces dominate**: 84.9 % of clamped / 66.8 % of void. The user's
>   "full-height black gash on the right wall" at t=5743 is ONE convex ridge
>   (`rooms` g=9, 27° fold) owning 482 171 of the 800 513 px.
> - TRUE boundaries void ZERO under a discard — there the discard is already
>   right and the recess arm's CLAMP is the bug. Cheapest available win.
> - Angled continuation is the expensive path: 27 fold angles, 71 distinct
>   (fold, scale, mirror) transforms, **41.8 % MIRRORED charts**, and 5.4 % of
>   its targets point at the unshelled ceiling. One hop suffices (p99 = 232
>   texels past the boundary).
> - Gates: render_gate 3/3, city `37e62845`, fountain `51fff7cd`, greets
>   shell/tess/flat t=6097 all byte-exact, wasm links. Crops:
>   `docs/img/s1d_seams/`.

> **2026-08-05 — S1 P2-A: `--pom_recess_only` IS IN, default OFF.** Read
> `docs/S1_DISCREPANCY_INVENTORY.md` §10. The user's BLACK HOLES (full-height
> gashes between wall panels, black bar in the mirror) are the LID model's
> mandatory lateral-exit discard firing at internal patch seams; the converged
> reference shares that boundary model, so **every P1 number was scored by a
> yardstick blind to it**. VOID (z==0 px) is now a mandatory column on every arm.
> Void at the user's t=5743 pose: tessellation **3**, lid shell **98 371**,
> recess-only **0** — and 0 at all seven poses measured.
> - Recess-only moves NO vertex; the height field's max sits at the authored
>   plane and all relief carves inward; a ray leaving the patch CLAMPS
>   (`--pom_recess_edge=2` restores the discard as a diagnostic and voids
>   68 k–130 k px on identical geometry, which is how the mechanism was pinned).
> - VERIFIED: shadow cube vs the no-shell arm **0 of 13 533 184 texels** (lid:
>   29.88 %) → C6 zero by construction; **0 px frame-wide drawn nearer than the
>   authored plane** at six poses (lid: 26–74 %) → S1a's ordering hazard retired.
> - COSTS: 0.8–8.5 % of the frame renders FLAT (the clamp) in bands along the
>   seams; the surface recedes half a slab (pair with
>   `--pom_shell_world_amp_set=0.18`); nothing can stand proud of the authored
>   plane ever again. Perf: no measured cost (−0.8 ± 0.9 ms vs the lid arm).
> - It is the cheap correct-by-construction option, NOT the literature one —
>   a CLOSED shell (Hirche'04 side faces + cross-patch march) is S1d.
> - Recipe: `--no-greets_displace --parallax_pom_cone --pom_shell
>   --pom_recess_only --pom_shell_cap=16 --parallax_pom=32 --pom_cone_exact=1
>   --pom_cone_min_step=1 --pom_march_earlyout --pom_shell_world_amp
>   --pom_shell_world_amp_set=0.18 --pom_normal`. Crops: `docs/img/s1_p2a/`.

> **2026-08-04 (session 3) — S1a + S1b + S1c ARE ALL IN, all default OFF, all
> byte-null** (render_gate 3/3, city `37e62845`, fountain `51fff7cd`, wasm
> links). Read `docs/S1_PIXEL_DISPLACEMENT_PLAN.md` for the full record.
> - S1a `--pom_depth_write` (c2616e4), S1b `--pom_shell` (c556148).
> - **Floor void CLOSED** (dfb4272): `--pom_shell_merge_uv` gives each patch a
>   SIBLING BOX LIST (coplanar patches whose UV rects abut), and the domain is
>   the UNION OF THE BOXES — never their bounding box, which was tried first
>   and destroyed the t=6097 corner silhouette. Void at t=5780 **6175 → 404 px**
>   with the corner discard pixel-identical.
> - **t=6097 corner band ADJUDICATED**: the discard is CORRECTING the lid, not
>   eating wall. Of 178 802 discard-affected px: 0 void, 100 % revealing a real
>   surface ~5 world units behind. Reference framing: tess == flat POM exactly;
>   shell-no-discard over-covers by 35 436 px, shell by 12 162 — the discard
>   removes 23 k px of lid inflation. Instrument: `FDS_SNAPSHOT_GBUFDUMP=1`
>   (G-buffer matID plane) + `scratchpad/classify.py`.
> - **S1c `--pom_horizon` LANDED**: 8-azimuth horizon bake (disk-cached, 99–128
>   ms, NOT minutes) + per-light tangent-space elevation-vs-horizon compare.
>   The groove shadow MOVES with the light — the one thing neither the
>   tessellation bake nor the shell march can do (PolyId shadows are
>   identity-only). Path-agnostic: works under `--greets_displace` too.
> - **Perf [M]** greets t=5780, iters=40, 4 interleaved pairs, load 2.3–4.9:
>   flat POM 56.9 · **shell 58.1** · **shell+horizon 61.0** · **tessellation
>   104.7** · tess+horizon 106.8 ms/iter. Horizon = +2.9 ms median for all 7
>   omnis.
> - OPEN: nothing blocking. The user picks the defaults; the three-way crop
>   list is at the end of the plan doc.
>
> **2026-08-04 — ACTIVE CAMPAIGN REDIRECT:** the current campaign is
> **S1 per-pixel shell displacement** — read `docs/S1_PIXEL_DISPLACEMENT_PLAN.md`
> FIRST (mission, stages S1a/S1b/S1c, validation battery, discipline). Research
> basis: `docs/DISPLACEMENT_RESEARCH.md` (07b72c7). In flight at write time:
> (a) S1a `--pom_depth_write` agent in a worktree; (b) groove-line zigzag fix
> agent in the main tree (tessellation path; diagnosis correction: the sawtooth
> verts were NEVER snapped groove-line verts — re-scoping via --displace_viz).
> Recently landed on fog-wt: chunk-occlusion experiment (VISIBILITY_PLAN §7 —
> occlusion refuted, default OFF), displacement fold-relax + parent-plane
> shadow ids + neighbor pin (all default ON; bleed root cause was the PolyId
> single-id collapse, fixed by `greets_displace_shadow_planes`). User note:
> bare `--parallax_pom_lod` in their flag list parses to NOTHING (needs =value).

Read this when resuming. Branch **fog-wt**, nothing pushed. Previous campaign
(structural push): docs/posts/SESSION_STATE_2026-07-04_structural.md.
Range covered here: `1ca269d..7282f7a` (~60 commits, 2026-07-08..11).

## Verification protocol (THE gates — run before/after everything)

> **→ `docs/PERF_LAWS.md` collects every measurement trap below, plus the
> campaign's optimisation laws, in one place with its evidence.** The traps
> in this section are the gate-specific ones; PERF_LAWS has those and the
> instrument/attribution ones together.

All runs headless from Runtime/: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.

> **THREE WAYS TO GET A FALSE RED ON THIS TABLE — all three were hit and
> resolved on 2026-08-29, and none of them was ever a regression. Read this
> before reporting a broken gate.** (Full account: `docs/PERF_STATE.md` §00q.)
>
> **1. THE BINARY CHOOSES THE ASSET TREE, NOT YOUR `cd`.** `main()` calls
> `ChdirToAssetRoot(argv[0])` (`DEMO/REV.CPP:554,1392`): it resolves its own
> executable path with `_NSGetExecutablePath`, then looks for `rev.cfg` in
> `<exedir>`, `<exedir>/../Runtime`, `<exedir>/../../Runtime` and **chdirs to the
> first hit**, silently discarding the working directory you started it in. So
> `cd worktree/Runtime && /other/tree/build/DEMO/DEMO …` renders **the other
> tree's assets and the other tree's `rev.cfg`**, not the ones you cd'd to.
> Gil-Ad's main tree runs `rev.cfg` at **1384×768** and carries untracked asset
> drift, so any binary living under `/Users/gil-ad/work/revival-fog/` mismatches
> this table by construction. **Gate from a stock worktree's own build, or pass
> `FDS_CHDIR_ASSETS=0` / `--no-chdir_assets` to make `cd` authoritative.**
> PROVED both ways 2026-08-29: the two trees' binaries are **byte-identical**
> (`md5` equal, `cmp` reports 0 differing bytes) and the main tree's binary under
> `FDS_CHDIR_ASSETS=0` reproduces the chase, greets and city pins EXACTLY.
> There is no cross-tree build divergence; the build is reproducible.
>
> **2. THE CHASE PINS ARE POSE-SEQUENCE DEPENDENT. `--snapshot=chase@t=800`
> ALONE IS NOT THE PINNED t=800.** The chase rows are pinned from
> `--snapshot=chase@t=100,400,800,1200,1600` — five poses in ONE process — and
> scene state carries across them. Measured: t=100 alone is **byte-identical** to
> t=100 as the first of five, but **t=800 alone differs from t=800 as the third
> of five by 434 591 px (20.96 %), max |Δ| 5, mean 0.62**. Only the FIRST pose of
> a process is sequence-free. **Always run the whole five-pose recipe verbatim.**
> (greets is the mirror-image trap and already documented: ONE POSE PER PROCESS.)
>
> **3. Resolution.** `tools/render_gate.sh` has no override and renders at
> whatever `rev.cfg` says — see the `gate suite` row.
>
> Corollary for merge decisions: every byte-identity claim in this table's
> 2026-08-28/29 rounds was taken from a stock worktree running its own build with
> the verbatim recipes, so they stand. Two alarms raised during that window — "a
> cosmetic string literal moved the pins" and "the incremental `build/` is
> poisoned by LTO" — were **both** cause 2 (a single-pose run compared against a
> five-pose pin) and are **withdrawn**: the same incremental binary reproduces all
> five pins under the canonical recipe.


> **2026-08-26 — the `ssao_hdr_transport` fix (SSAO reaches city/fountain under
> `--hdr` for the first time) moves NOTHING in this table.** All 13 pose hashes
> below reproduce on the fixed binary, plus the city acceptance arm
> `4cb8d2ca…`, plus `render_gate.sh` ALL PASS 4/4 from a stock-`rev.cfg`
> worktree. The reason is structural, not luck: the fix only changes SSAO's
> write target on `Scene::PreferOuterVec` scenes (city / fountain / crash), and
> the only row here that passes `--ssao` at all is `greets (acceptance ×4)` —
> greets runs the SCALAR kernel, keeps the HDR arm, and is byte-identical by
> construction (verified differentially: t=5743 `440aa6bb…` identical with and
> without `--no-ssao_hdr_transport`). The city / fountain rows below carry no
> `--ssao`, so they never enter the changed path. Full account: the 2026-08-26
> entry at the top of this file.

> **2026-08-25 — `ssao_radius_zfloor` default 0 → 48 moves NOTHING in this table.**
> Certified differentially on one binary in one clean worktree, each row's
> recipe VERBATIM (note chase's rows carry no `--profiler=0`; they were run as
> written), arm A appending `--ssao_radius_zfloor=0` (the exact pre-flip arm)
> and arm B taking the new default: **13 of 13 pose hashes identical across the
> arms, and all 13 reproduce the pins recorded below.** No row is re-pinned and
> no value is struck. The mechanism is that `ssao` itself defaults to 0 and no
> scene `setDefault`s it on, so `Render_SSAO` early-returns in every recipe here
> except `greets (acceptance ×4)` — and greets is one of the two scenes
> (with crash) where the floor is INERT by construction, `48 × 0.00252757 =
> 0.121` never reaching `--ssao_radius=4.0`. Full account + the per-scene
> effective-radius table: the 2026-08-25 entry at the top of this file.

| gate | recipe | pin |
|---|---|---|
| city | `FDS_CITY_ENV_PIXEL=1 ./DEMO --snapshot=city@t=1961 --out=<dir> --deferred` | **2026-08-17 — THIS PIN IS STRUCTURALLY BLIND TO `--refl_correct` (and so is the acceptance arm): `RunCitySnapshot` ticks ONCE per timestamp, and city's water carries NO mirrored content on the first tick of a process (measured ladder, same recipe: 1 tick → parent == child byte-identical; 2 ticks → they differ). So the value below is UNMOVED by the commissioned look change, while continuous play moves 277 214 px / 13.4 % at this pose. Judge city's reflection from a warm multi-tick run, never from this pin.** **CURRENT (2026-08-16f, tip `eb5e57d9`): `bd4ffbf87d1492175a9b6c1111fb3f5f`** — 3/3, and now insensitive to `--profiler=0` (the overlay is silenced in `RunCitySnapshot`; before that fix this recipe gave the HUD-bearing `4031ceec1a1090372575c4f9c39e2839`, which an explicit `--profiler` still reproduces exactly). **His acceptance arm** `./DEMO --snapshot=city@t=1961 --out=<dir> --env_live_water --deferred --city_env_pixel` → **`4cb8d2ca68b72f8a24627f42077eef25`** (t=2400 `f473fe2b2658fa0c1c290e1acf8265b9`, t=400 `d3374de6a0840a6927e00eb54b48b359`; one pose per process — a multi-pose sweep has its own temporal history and hashes differently). Cubes on disk for these values: `city_envmap_cube_c0c60c19.bin` md5 `adbac29c55fccc6919c04008ecff374a`, `city_envmap_cube_c0c60ff9.bin` md5 `a896a47c144fc23cff0e85e5c389d84b` (copy them into a fresh worktree or it cold-bakes). Everything below this sentence is history. — **⚠ THIS PIN IS CONDITIONAL ON THE ENV CUBE ON DISK — check `md5 Runtime/cache/city_envmap_cube.bin` BEFORE calling a mismatch a regression.** The cache key ignores FeatureFlags, so the cube is a hidden input the recipe does not state (full analysis + 2×2 matrix in the dated note above). `d1d67f0f84fb4af3713e15a64a1b827b` = pre-flip bake → the pins below hold. `63978a18ed31837348598014716f9932` = cold/current bake (mips ON) → **`5476be8c43864c761b94e2dd83f86aa8`** default and **`b88ecb7bbd0340145e35a80bc7a82f6b`** under the control; both are correct-for-that-cube, NOT drift. A **fresh worktree always cold-bakes**, so it lands in the second column unless you copy the cube in. Also: `DEMO` chdirs to its OWN directory (`ChdirToAssetRoot`, `DEMO/REV.CPP:503`) — launching a worktree binary from the main `Runtime/` does **not** render the main tree's assets or its cube. **Pending decision:** adopting the flip properly means `rm Runtime/cache/city_envmap_cube.bin` and re-pinning to `5476be8c…`; held for the user's eye on `docs/img/mipsel/city_t1961_envbake_crop.png` (max Δ 6/255, glass only). — **RE-PINNED 2026-08-08 (`--mips` default 0→1): `e1221676372e0bba6f65343f6d85b8e7`** (stable 2/2, pre-flip cube). Prior pin `37e62845c4d30eefa321730c5bb7e0b8` reproduces EXACTLY under `--no-mips --no-mip_fix` **on the pre-flip cube** (on a cold-baked cube that control arm is invalid — it measures a mips-ON bake under a mips-OFF frame). Divergence: 133 854 px changed (6.46 %), mean \|d\| 7.04 on changed, 24 761 px >12/255, max 192 — building facades, see `docs/img/mipsel/city_t1961_worst_crop.png`. |
| greets | `FDS_GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551" ./DEMO --snapshot=greets@t=1588 --out=<dir> --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --no-env_refl` | **RE-PINNED 2026-08-14 (cone campaign round 6, the SAME change that moved chase — greets' disco beams are segmented cones and now take the 8-wide solve): `570a7b443f768393dc6647044a9e67b3`** (2/2 stable, differential against the parent binary in one worktree). **381 px of 2 073 600 (0.0184 %) at max |Δ| 1/255** — one LSB, nowhere near a look change. Bought greets cones 7.29 → 6.58 ms at this pose (−9.7 % wall, −9.6 % cycles); the user's t=3122 pose −1.5 % cycles. docs/HW_PROFILING.md §14. Prior value `778fa6acd85a69cf241babefcdaf598e` reproduces 2/2 on the parent commit `43ac3456`; its adjudication history follows and still stands for everything before this change. — **PREVIOUS, RE-ADJUDICATED 2026-08-12 at origin tip `3b00bbc7`: `778fa6acd85a69cf241babefcdaf598e` — 16 runs across FOUR content/code configurations (committed `GREETS.FLD`; the user's dirty `GREETS.FLD` under the same binary; an independent worktree+build with his content; and a build carrying the parent-commit control revert of `0b466b77`), ONE VALUE EVERY TIME. The pin is INVARIANT to his uncommitted authoring files at this pose. The rival value `2e96e91d9ce0188981cd71c3fdebb954` is this exact recipe run WITHOUT the `FDS_GREETS_CAM=` prefix (reproduced first try) — a recipe transcription error, not tree drift. Full adjudication in the 2026-08-12 block above.** Previously measured on the settled tree at `7b5f1f8`+ as the same value. Verified 4/4 before the `--shadow_lightmap_texel_density` flat-arm default, 4/4 after it, and 4/4 with the revert flag `--shadow_lightmap_texel_density=0` — **12 runs, one value; that change does not move this pin** (it is look-null at all 16 review poses too, see the dated block at the top). fountain `8db68ccb59416e9a44037e9f387b7bd9` 4/4 and city `3cbe42b166847e40f7071eedb48d613c` 4/4 alongside it, `render_gate` 3/3. NOTE for whoever reads the history below: the hashes in the older entries (`9eeaf860…`, `6ed5462e…`, `91ec081a…`) do **not** reproduce at HEAD — they were taken while other agents held uncommitted work in the shared tree, exactly the hazard the `2026-08-09c` note warns about. Trust the settled-tree value above. — history: **RE-PINNED 2026-08-09 (`hull`/`cockpit` removed from the Sobel normal-map name gate, `DEMO/GREETS.CPP:1951`; docs/SHADING_CONTRACT.md §11 row E8): `9eeaf860cb5a7f124884a89e0fc3ff5b`** (stable 3/3, across two binary revisions). REASON: `BakeNormalMapFromDiffuse` was Sobelling MECH_HUL.JPG / MECH_COK.JPG — camouflage PAINT — into geometric relief; the user compared the mech against the standalone Metal arm (which bakes no such map) and preferred the GPU's. Only four materials ever hit the gate (`!M->NormalMap` guard); `hull`, `hull not smooth` and `cockpit` are gone, `siling` remains. **AT THIS PIN POSE THE CHANGE IS 1 PIXEL AT 1 LSB** (702,172) — t=1588 barely shows the mech, so the pin move is not the measurement. The measurement is at the §11 mech pose (t=4871): **179 829 px (8.67 %), max channel Δ 164, 11 677 px > 10 luma**, hull pixel (767,723) Y **131.2 → 44.9** against the GPU's 41.0, canopy pixel (760,620) 146.4 → 157.4 against 161.4. Crop: `/tmp/fogwt/task1_mech_strip.png`. city `e1221676…` and fountain `8db68ccb…` do NOT move (greets-only, guarded on `M->RelScene != GreetSc`); fountain re-verified. Prior pin `6780642b30430efa4fd2f87810b2dfdb` reproduces by re-adding the two `strstr` terms. Preceding that: **RE-VERIFIED 2026-08-09c, 3/3 EACH, on a settled tree at HEAD `4f60493`** — these supersede every pin value recorded earlier today, several of which were taken while other agents held uncommitted work in the shared tree and are therefore not reproducible:
> * greets   `778fa6acd85a69cf241babefcdaf598e`
> * fountain `8db68ccb59416e9a44037e9f387b7bd9`  (the ONLY pin that held all day)
> * city     `3cbe42b166847e40f7071eedb48d613c`
>
> Two hazards that produced the bad values, both worth remembering: a shared tree with concurrent uncommitted edits makes an ABSOLUTE pin meaningless — certify byte-nullity DIFFERENTIALLY (two binaries from one tree, one with the diff reverted) instead; and the FIRST run after a rebuild can write a cache the later runs read, so discard it.

**RE-PINNED 2026-08-09b (five more flags defaulted ON at the user's instruction: `metal_spec_f0`, `env_mip_chain=9`, `env_bake_linear`, `sh_bake_linear`, `env_bake_sh_first`): greets `6ed5462e38ced22ecc98b39730d2e915` (2/2), city `3cbe42b166847e40f7071eedb48d613c` (2/2), fountain `8db68ccb59416e9a44037e9f387b7bd9` UNCHANGED (stable 4/4 — but the FIRST run after a rebuild returned a different hash, a cold-bake artifact of the same class as the city env cube; always discard run 1). Preceding it: **RE-PINNED 2026-08-09 (the Sobel name gate DELETED + four flags defaulted ON: `deferred_checker_env_full`, `env_bake_include_animated`, `env_metal_tint_linear`, `shadow_noncaster_depth`): `91ec081a4211554de8f36975fe1ac171`** (stable 2/2). city `5476be8c` and fountain `8db68ccb` did NOT move. Preceding it: **`9eeaf860cb5a7f124884a89e0fc3ff5b`** (gate removal for hull/cockpit only), and before that **RE-PINNED 2026-08-08 (`--hdr_metal_kill` default 0→2, the conductor diffuse kill): `6780642b30430efa4fd2f87810b2dfdb`** (stable 2/2). city `e1221676…` and fountain `8db68ccb…` did NOT move — neither scene has a metallic-mapped material, so the fix is greets-only. Prior pin `adfba8ba3a1971a7c9cac0da689581b1` reproduces under `--hdr_metal_kill=0`. Preceding that: **RE-PINNED 2026-08-08 (`--mips` default 0→1): `adfba8ba3a1971a7c9cac0da689581b1`** (stable 2/2). Prior pin `f1297141611c484bac7cc10a8bdcf630` reproduces EXACTLY under `--no-mips --no-mip_fix` — note BOTH flags are required, because `--mip_fix` moves the subdivision cut lines and the `--mips` gate zeroes only the mip LEVEL, not the geometry. Superseded pin history follows: **RE-PINNED 2026-08-06: `f1297141611c484bac7cc10a8bdcf630`** (3/3 identical runs). Two intended overlay removals moved it in sequence, both pure screen text: `f5778c7b` → `06e1d4d1` (earlier work) → `ae358a6a` (the "Shadow: Depth\|PolyId [F3]" indicator deleted, commit `6b5556d`) → `f1297141` (the always-on centre-pixel `[MAT@…]` material probe moved behind `--mat_probe`, default off, commit `35ec295`; re-running that arm WITH `--mat_probe` reproduces `ae358a6a` byte-exact, which is what proves nothing else moved). Prior pin, for the record: **`f5778c7b78a4d70655291363e4119c66`** — taken over **128 sequential runs, 0 flips** (95 % UB on the flip rate 0.023) after the 8-bit-AO-map fix closed the nondeterminism. This supersedes both `de3e9a5fb3aa39e008ef41b83f2b8d1b` (pre-PBR-defaults) and the "NO VALID PIN" state. Includes the PBR scene defaults AND the user's uncommitted GREETS.FLD / momy textures / Piramid.lwo — a clean checkout hashes differently. Verify with `tools/flip_rate.sh -n 24` if a mismatch appears; a single differing run is now a real regression, not noise. |
| greets (acceptance ×4) | ONE POSE PER PROCESS, from `Runtime/`, `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, **stock committed `rev.cfg` (1920×1080, HiDPI 0)** or `--force_xres=1920 --force_yres=1080` (proved byte-null against the stock cfg), **no `FDS_GREETS_CAM`**, no other env:<br>`./DEMO --snapshot=greets@t=<T> --out=<dir> --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0` | **RE-PINNED 2026-08-30 — THE GTAO DEFAULT FLIP, ON GIL-AD'S RULING. He flew the guarded geometric arm with `--ssao_gtao_bias=0.2` and ruled, verbatim, "all scenes" (ledger decision `f2f179f017df`). `--ssao_gtao_step_dist` 0→1, `--ssao_gtao_srad_max` 256→1024, the AUTO self-occlusion bias 0.05→**0.2** (his number, a look judgement — his words: "I tried increasing values until I got something that makes the shade there go away - so it's kind of a look judge, no hard data"), `--ssao_gtao_round_fix` already ON: t=5743 `08f0741fb8c5d324a2303fb03ed23437`, t=2845 `84e18552fa079409a3da12c4b922bf77`, t=6097 `0cc05784dd3e9e448e3ad06df77180bc`, t=6133 `456eac7dc6d6b2135f0e975e11b3a4fe`** — **3/3 stable each**, and these four are the ONLY rows of the 14 that moved (the other ten, plus the three city acceptance arms, plus `render_gate.sh` 4/4, plus six of the seven warm rows, are byte-identical on the flipped binary; `greets-warm` moved and is re-baselined in `tools/warm_gate.sh`). **THE FLIP IS PURE DEFAULT MOTION AND IT IS PROVED ON THE WHOLE BATTERY**: the same binary under `--ssao_gtao_step_dist=0 --ssao_gtao_srad_max=256 --no-ssao_gtao_round_fix` reproduces the four struck values below EXACTLY, and greets cam A t=5965 under that arm gives `dbe2d4c2716e2d762b366d5a45c151a6` — the value already on file at `docs/DISPLACEMENT_RESEARCH_II.md:1519`, taken months before any of this work. Drop only the `--no-ssao_gtao_round_fix` term and you get the never-baselined intermediate set (round_fix ON, uniform ladder) ~~t=5743 `a7fa9214105fe7501d2d927427c0690e`, t=2845 `8593c7128d3915c2221b4a5123bb665e`, t=6097 `4ea379d0daa9593779fb06a3d4c8ee00`, t=6133 `03160c827e4946d0ecebf5ba1ff8d5d9`~~ — that set was what the tip actually rendered between the round-fix commit and this one, and it was deliberately never pinned. Both guards stay AUTO (`-1`), i.e. on exactly when the ladder is front-loaded, so ONE flag switches the whole pre-flip march back. THE BIAS DEFAULT IS STILL SPELLED `-1` IN `FeatureFlags.def` AND THAT IS DELIBERATE: a literal `0.2` would apply the guard to the uniform ladder too, and that is **not** byte-null there (measured: t=5743 goes `a7fa9214…` → `aa08f5b1f5594250407ddbb3f14cbd20` under `--ssao_gtao_bias=0.2` with `--ssao_gtao_step_dist=0`), which would have cost the exact revert arm. AUTO now resolves to 0.2 with a front-loaded ladder and 0 with the uniform one. — **SUPERSEDED (struck, and reachable as the revert arm above): ~~t=5743 `440aa6bbb350ae95fbacf339dd2ad957`, t=2845 `00d17bc5610624bd1fea698c4b096979`, t=6097 `29c1e7fbd30e9ef811588a63d0778b7b`, t=6133 `bc1b0a8a703d6d6f6b3953eafc864d48`~~** — the 2026-08-16y countersigned set. History follows. — **RE-PINNED 2026-08-16y — `ssao_downscale` default 1→2, countersigned by Gil-Ad 2026-08-16: t=5743 `440aa6bbb350ae95fbacf339dd2ad957`, t=2845 `00d17bc5610624bd1fea698c4b096979`, t=6097 `29c1e7fbd30e9ef811588a63d0778b7b`, t=6133 `bc1b0a8a703d6d6f6b3953eafc864d48`** — **t=6097/t=6133 RE-PINNED 2026-08-17 (jamb-cushion fix: `--greets_displace_block_level` + `--greets_displace_geom_bisector` join the umbrella; 4.50 %/5.63 % px at max 130/146, background unchanged; t=5743/t=2845 byte-identical; ~~t=6097 `135ea9dd4513bad466ec69129730eca4`, t=6133 `aaeb89b6f95e826eea361fe9ed6e6787`~~ reproduce with `--no-greets_displace_block_level --no-greets_displace_geom_bisector`)** — 3/3 each, differential (both binaries built in ONE worktree, ONE asset tree). **THE FLIP IS PURE DEFAULT MOTION AND IT IS PROVED BOTH WAYS**: the child binary under an explicit `--ssao_downscale=1` reproduces the four struck values below EXACTLY 3/3, and the parent binary under `--ssao_downscale=2` reproduces the four values above EXACTLY 3/3 — a bijection between the two arms, so nothing but the default moved. 1512×848 sensitivity row re-taken alongside (below). Realized price of the flip on his acceptance arm, min-of-11 interleaved at 1512×848: frame **49.25 → 39.62 ms** at t=5743 and **41.39 → 31.86 ms** at t=6097 (`ssao` 14.55 → 4.96 / 14.23 → 4.93, `renderFrame` Ginstr/f 4.686 → 3.639 / 4.191 → 3.143); image cost at t=5743 **52.5 % of pixels moved, mean |Δ| 0.869/255 over channels on the moved, max 74** — reproducing the 2026-08-16 dial round's measurement (44–63 % moved, mean 0.53–0.87, max 74 at this pose) to the digit. Crop at the dial round's own region: `docs/img/ssaoperf/default_flip_t5743_crop_d1_d2.png`, full frame `default_flip_t5743_full_d2.jpg`. — **SUPERSEDED (struck, kept for the `--ssao_downscale=1` control arm): ~~t=5743 `26ad272aaa6cc9050c66e84cdaaf5436`, t=2845 `10adec3ab6a4443a1fe93da6fda32b3d`, t=6097 `418fc1fa2ac717aeffefecadf08e40a0`, t=6133 `6d02f31b0e9030a7a2d031d714f51768`~~** — these four are the FULL-RES arm and reproduce today by appending `--ssao_downscale=1` to the recipe. History follows. — **RECIPE RECOVERED AND RE-VERIFIED 2026-08-16x — the four hashes are NOT orphaned** — reproduced **3/3 on two binaries** (tip `773fc45c` md5 `c9fb4f20…`, and the `--needle_cull` build md5 `79a11fba…`), first try, in a fresh clean worktree that cold-baked its own caches. The 2026-08-16w "DOC GAP / eight arms tried, none reproduces them" note was wrong about the pins, right about `docs/`: the recipe existed only in **`scratchpad/xform_pins.sh`**, the 2026-08-16r round's untracked battery script, which every later worktree carries but no committed file mentions. **THE THREE THINGS THAT MOVE IT** (measured here, so the next round does not have to re-derive them): (1) **`FDS_GREETS_CAM` must NOT be set** — the review-poses file `docs/greets_review_poses.txt` lists a camera for all four t values and prefixing it gives t=5743 `19d94f489542283d1bf2c562668e7a38`; this is the exact inverse of the greets t=1588 pin above, where the prefix IS required, and it is the likeliest thing the eight failed candidates did. (2) **profiler state**: `--profiler=0` and no profiler token at all are IDENTICAL (`26ad272a…` both), but an explicit `--profiler` gives `cb7f4a516a0b2c76821b47df6264052d` (the HUD). (3) **resolution**: at 1512×848 (his window, the bench harness's default, `rev.cfg` edited) the same four arms give a different self-consistent set. **RE-TAKEN 2026-08-16y under the `ssao_downscale`=2 default, 3/3 — t=5743 `4667aa83f06af64c5e1f07f9b41e9937`, t=2845 `c250427622e42c42e4f1ae110b790564`, t=6097 `85bbb55579730ac298724e3a40e8b76d`, t=6133 `15de194a54a1d30ef2e09c76a1a1922a`.** The prior full-res set is ~~t=5743 `78d47fbb5f9a8ab68f48bae67475b422`, t=2845 `00fb8bc10d618deae5a17ce81ec7980f`, t=6097 `51c3de03dcf60131977ca6160bbf9fbb`, t=6133 `df2ae34e199630470ec3e3349cae88af`~~ — struck as the default, but **re-verified 3/3 on the flipped binary today** (it was recorded 2/2) by appending `--ssao_downscale=1`, i.e. this row moved for exactly the same reason the 1920×1080 row did and for no other. The four poses and their cameras are the 2026-08-04/-05 review poses (`docs/greets_review_poses.txt`); the pin renders them from the SCENE's own camera, not from those. |
| fountain | `./DEMO --snapshot=fountain@t=2500 --out=<dir> --deferred --hdr --glass-refract=1 --glass-test --profiler=0` | **RE-PINNED 2026-08-08 (`--mips` default 0→1): `8db68ccb59416e9a44037e9f387b7bd9`** (stable 2/2). Prior pin `51fff7cd38767d619280afe0498a6f24` reproduces EXACTLY under `--no-mips --no-mip_fix`. Divergence: 266 063 px changed (12.83 %), mean \|d\| 9.27 on changed, 53 238 px >12/255, max 254. |
| chase (default) | `./DEMO --snapshot=chase@t=100,400,800,1200,1600 --out=<dir> --deferred` | **RE-PINNED 2026-08-28 — PERF CHANGE, `frame_tile_y` default 5→20 IN CHASE ONLY (`createChaseScene`, PERF_STATE §00m): t100 `b67b47f0de8b41365f96fff68e50d367` t400 `5bc199d4949a6212b4b7cb1004ab0e3a` t800 `d1284b5a727bb6c5924b6ba3012f89ae` t1200 `9c0f7c2fac7b8a1408f62110bb70d12f` t1600 `9cdf5603f231392e64000ed2b850877a`** — 3/3 stable across three binary revisions in one worktree (the flag flip on the parent, the `setDefault` child, and the child again with §00m's PassTag instrument added). **THE CONTROL IS EXACT: the SAME child binary under `--frame_tile_y=5` reproduces the five PREVIOUS values byte-identically**, so the grid flag is the whole cause and the revert is one flag. THE MOVE, on this recipe: 57 / 563 / 327 / 1 738 / 59 px of 2 073 600 (0.003–0.084 %), max |Δ| 71/73/33/190/52, mean |Δ| on moved 2.9/4.1/2.4/0.8/1.9; at t=1200, 1 674 of the 1 738 moved pixels are exactly 1 LSB. **BUT THIS RECIPE IS NOT HIS ARM AND UNDERSTATES THE CHANGE BY TWO ORDERS OF MAGNITUDE** — under `--hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao` the same flip moves 23 866 px (1.15 %) at t=800 and 87 121 px (4.20 %) at t=1105, because the per-tile mip hysteresis sees a different tile partition; the amplitude stays tiny (mean |Δ| 0.7 / 1.6, 199 px above 16/255 in 2 M) and the amplifier is MEASURED to be `--texture-filter=2` alone, not `--ssao` and not `--hdr` (discriminator ladder in §00m). Images `docs/img/tilegrid/{gate_t*,hisarm_t800,hisarm_t1105,cine_t*}_{before,after,diff16x,where}.png`. PREVIOUS (= `--frame_tile_y=5` today, reproduced byte-exactly on the child): t100 `f16bedd0a76092dd711b528106b57f28` t400 `fcc9d5610778b6315fd2bc551a77dcd6` t800 `397b878dc36b722ec9d7ed4b7085b016` t1200 `3539492d32571da5017b1d437a1365bd` t1600 `0622d56e943a59ed899f1c7eda282c75`. History below.<br>**RE-PINNED 2026-08-17 — LOOK CHANGE COMMISSIONED BY GIL-AD 2026-08-16 (`--refl_correct`, default ON): t100 `f16bedd0a76092dd711b528106b57f28` t400 `fcc9d5610778b6315fd2bc551a77dcd6` t800 `397b878dc36b722ec9d7ed4b7085b016` t1200 `3539492d32571da5017b1d437a1365bd` t1600 `0622d56e943a59ed899f1c7eda282c75`** — 2/3 runs stable each (run 1 discarded), differential against parent `6acb2ebf…` built in the same worktree on `cb6aad4c`. chase's reflected pass now writes TN/TTangent in all six vertex loops AND takes lights mirrored about the water plane; 8.5–27.0 % of the frame moves at these poses, max |Δ| 73. **The control is exact: the SAME child binary under `--no-refl_correct` reproduces the five values below byte-identically**, so this row's previous set is the flag-off set, not history. Full account + per-pose eyeball notes + images (`docs/img/reflmir/`): docs/OPTIMIZATION_BACKLOG.md **2026-08-17**. PREVIOUS (= `--no-refl_correct` today): t100 `3bfd424458a74b7892821de04ab69ca9` t400 `42d79fadd825a329b36143efe052edfb` t800 `622b96a214404a0abec1d21aae47a478` t1200 `31aa52039f9b228fa6307c12e14811eb` t1600 `ca07a81450afc8f1594d32d5e62c10cb`, all five reproduced 3/3 on the parent this round. NOTE the 2026-08-14 values quoted further down this cell (t100 `7678a6bc…`, t800 `b29c73f1…`, t1600 `1544b0e7…`) have NOT reproduced since that round; the `3bfd4244…` set is what the tip actually gives. History below.<br>**RE-PINNED 2026-08-14 (cone campaign round 6 — `--vol_cone_solve_vec` un-gated for SEGMENTED cones + `FDS_CONE_QUADEARLYOUT`; chase is 32 narrow beams of 34 spots, so 91 % of its cone work moved from the scalar solve to the 8-wide one): t100 `7678a6bc6ea964b3b859ecb11c0673c3` t400 `42d79fadd825a329b36143efe052edfb` t800 `b29c73f1c54f42a02e0dc2484780cc03` t1200 `31aa52039f9b228fa6307c12e14811eb` (UNMOVED) t1600 `1544b0e775900b099ac9e38d42fd750d` (UNMOVED)** — 2/2 stable each, differential (both binaries built in one worktree, one asset tree). THE MOVE IS 20/27/40 PIXELS of 2 073 600 at **max |Δ| 2/255**, nothing above 4/255; it is round 5's VP/DV fold (`fl(Y·Py + Pz)` fused where the scalar arm rounds `Y·Py` first) now reaching the narrow beams. Bought −18 to −21 % of chase's cone pass at t=100/400/800 and −9.4 % at t=1200. Images `docs/img/chasecone/chase_t{100,400,800}_{where,sbs}.png`, full write-up docs/HW_PROFILING.md §14. Prior (2026-08-08, `--mips` default 0→1): t100 `76e7cf68714666bda278f094be4f2c72` t400 `d458e82bf4514c4ff2850468aab5743c` t800 `c145c7a5861fba81d56746f7c10764ee` t1200 `31aa52039f9b228fa6307c12e14811eb` t1600 `1544b0e775900b099ac9e38d42fd750d`; those five reproduce 3/3 on the parent commit `43ac3456` in a clean worktree, which is how the move above was certified. History below.<br>per-frame color-PPM md5, re-pinned 2026-07-30 (cone-tile sky-clip fix — see below; 3-run stable, byte==spot_cone_cull=0 ground truth):<br>**RE-PINNED 2026-08-08 (`--mips` default 0→1):** t100 `76e7cf68714666bda278f094be4f2c72` t400 `d458e82bf4514c4ff2850468aab5743c` t800 `c145c7a5861fba81d56746f7c10764ee` t1200 `31aa52039f9b228fa6307c12e14811eb` t1600 `1544b0e775900b099ac9e38d42fd750d`.<br>Control under `--no-mips --no-mip_fix` reproduces the 2026-07-30 pins EXACTLY for t100/t400/t800/t1200 — **but t1600 gives `c8c93b886dd31fcc01363c806d7626de`, NOT the recorded `7265d7855bdaae74e39f3c21d4f7e612`. chase t1600 had ALREADY drifted before the mip work; cause unidentified, needs its own bisect.** Prior (2026-07-30): t100 `f1a567133a3d20e6f3702c5c560a1299` t400 `2adfb0e8f783c01ec0714b9b396c82f0` t800 `0e2a8804f4feef1bf56f6ee9102a11b9` t1200 `7cefbdb062517865ba29ca88965e999f` t1600 `7265d7855bdaae74e39f3c21d4f7e612` |
| chase (cinematic) | `./DEMO --cinematic --deferred --snapshot=chase@t=800,1600 --out=<dir>` | **RE-PINNED 2026-08-28 (same `frame_tile_y` 5→20 chase default as the row above): t800 `d50a32d33f23a6de505257b663dbdc62` t1600 `92ffa25d675a716c6809a7db133c3961`** — 2/2 stable. **Control exact: `--frame_tile_y=5` on the same child gives `b61b33970977d31b0d8ff50788365d49` / `4d70fdbdfb2596f1ec4ba30e278eee82`, the previous pins byte-identically.** The move is **145 px** at t=800 (max |Δ| 111, three pixels of it, all at (707..709, 615)) and **50 px** at t=1600 (max |Δ| 25) — the smallest of the seven chase rows. PREVIOUS: t800 `b61b33970977d31b0d8ff50788365d49` t1600 `4d70fdbdfb2596f1ec4ba30e278eee82`. History below.<br>**RE-PINNED 2026-08-17 (`--refl_correct`, default ON — same commission as the row above): t800 `b61b33970977d31b0d8ff50788365d49` t1600 `4d70fdbdfb2596f1ec4ba30e278eee82`** — 2/3 stable, differential. Control `--no-refl_correct` on the child gives t800 `c218533003bbfb9fc4f3dcacafc7db11` t1600 `3fc9686cceafa39b68ef5138f84809b7`, byte-identical to the parent. **PRE-EXISTING DOC DRIFT, recorded rather than papered over: the values this row carried before today (t800 `857d899d…`, t1600 `567e6153…`) do NOT reproduce on the unmodified parent — they were left stale by some round between 2026-08-08 and now, and `c2185330…`/`3fc9686c…` are what the parent gives 3/3.** History below.<br>re-pinned 2026-07-30 (cone-tile sky-clip fix; 3-run stable, byte==cull-off): **RE-PINNED 2026-08-08 (`--mips` default 0→1):** t800 `857d899d48ca55a6ae67f03e30b9bf02` t1600 `567e61532fb075b6e590b53a26cea2b6`.<br>Control under `--no-mips --no-mip_fix`: t800 `28e5a2a78d64ae98a1fcc4b739991be2` matches the 2026-07-30 pin, **t1600 gives `debdb1f435a14949b2e05be0bb53b1e7`, NOT the recorded `1cbde501c26d231a4295632dfbebd34b` — same pre-existing t1600 drift as the default arm.** |
| gate suite | `./tools/render_gate.sh` (repo root, dummy drivers) | **FIXED 2026-08-30 — RUN IT ANYWHERE, INCLUDING THE MAIN TREE.** The baselines are 1920x1080 hashes and the script now PASSES THAT RESOLUTION ITSELF (`--force_xres=1920 --force_yres=1080` on every row, `tools/warm_gate.sh` too); it never reads `Runtime/rev.cfg`. Proved by running it in a worktree whose `rev.cfg` was edited to his 1384x768: ALL PASS at all four recorded values, byte-identical to the stock-cfg run. THE HISTORY, kept because the symptom is worth recognising: until 2026-08-30 the script rendered at whatever `rev.cfg` said and could not pass in the main tree. At 1384x768 all four failed with `mirrortest 3e85645e`, `rttslot 45d8f216`, `conetest 11877fc6`, `halotest f6ad21fe` — four md5s unrelated to anything on file, which is what a resolution mismatch looks like, not a regression. **The trap is only narrowed, not gone**: every OTHER recipe still takes its resolution from `rev.cfg` (`tools/flip_rate.sh`, `tools/refrender_battery.sh`, `tools/ssao_gtao_battery.sh`, any hand-typed `./DEMO --snapshot=…`) — add `--force_xres=1920 --force_yres=1080` there rather than editing his cfg or copying binaries between worktrees. Single-row form for a ledger recheck: `tools/render_gate.sh --row <name> --print-md5`. ALL PASS — **baselines UNCHANGED by the 2026-08-08 mips flip** (mirrortest `4ac809e5…`, conetest `b41894f9…`, halotest `166fa25a…` all byte-identical with mips on and off; those test scenes carry no minified textured geometry). |
| wasm | `make wasm` | links |
| **warm gate (NEW 2026-08-29)** | `./tools/warm_gate.sh` (fast, ~1 min) / `--full` (7 rows, **18 s**) — repo root, dummy drivers, stock-`rev.cfg` worktree | **THIS ROW EXISTS BECAUSE EVERY OTHER ROW IN THIS TABLE IS A ONE-TICK `--snapshot`, AND A PATH THAT ONLY BECOMES REACHABLE ON THE SECOND TICK OF A PROCESS EXECUTES ZERO TIMES IN THE WHOLE SUITE.** Demonstrated, not asserted: a binary that simply DOES NOT COMPUTE city's water-reflection fog (`-DFDS_FOG_PUNT_PROBE=1`) passes **12/12 of the rows above** and fails **5 of 7** warm rows — and in every failing row the FIRST frame's hash matches and only ticks 2+ diverge, which is the mechanism itself. Rows and what each covers that a snapshot row cannot: `city-warm` (the froxel composite's water-reflection leg — `gFrReflZ` is null until `FastFog_SetReflectionZ` has mirrored content, i.e. never on tick 1 — plus the froxel temporal EMA's blend arm, `gFrHistValid=false` on frame 1, plus the dispMap wobble); `greets-warm` (greets' iterative code-screen smear, a function of how many times `Render()` has run, not of `t`); `chase-warm`; `city-warm-plain` / `-hdr` / `-dim` / `-noal8` (the last reaching the SCALAR composite tile path). **Run the fast pair on every gate check; run `--full` before merging anything touching the composite, the froxel volume, or reflections — it costs 18 seconds.** `WARM_GATE_BIN=/path/to/DEMO` points it at another build. Same resolution trap as `render_gate.sh`: 1920x1080 baselines, cannot pass in the main tree. Recorded at `bc36387b`; **`greets-warm` RE-BASELINED 2026-08-30 by the GTAO default flip** — it is the only row of the seven carrying `--ssao --ssao-gtao`, it moved, the new values (`1035af88… b3d01586… 93d7aca0… 01c0322a… 8c1c456d…`) are 3/3 stable, and the previous set (`2d652283… d3279eb5… b065d0fa… fd82a58d… c0266682…`) reproduces EXACTLY on the flipped binary under `--ssao_gtao_step_dist=0 --ssao_gtao_srad_max=256 --no-ssao_gtao_round_fix`. The other six rows are byte-identical across the flip. Otherwise **unchanged since — they have already survived one real regression.** On 2026-08-29 city-warm and city-warm-noal8 went red at ticks 4–5 (ticks 1–3 and all 13 one-tick pins byte-identical). It was **one pixel, max |Δ| 1–2, deterministic 3/3** — indistinguishable by eye from the accepted-class LSB drift this tree re-pins for constantly, and it was re-baselined here for about ten minutes. **It was not drift**: it was a broken per-mesh `bsWorld` cache in another agent's city glass fan-out, whose owner found and removed it, and the fix restored these exact values. **A warm-gate red from someone else's change is not yours to re-baseline** — report it to the owner. "One pixel, 1 LSB, deterministic" is not evidence of harmlessness; a stale cached bounding sphere presents exactly that way. |

Traps:
- **WARM-GATE FALSE REDS: how to tell a real one from a fake one, BEFORE you
  bisect.** Measured 2026-08-29 while three agents were merging into `fog-wt`
  at once, after two of them hit warm-gate reds. There are exactly three
  outcomes and they look alike in a careless read:
  1. **`ERROR <row> ran but produced N/M frames` — NOT a pixel regression.**
     The row failed to emit frames at all. `tools/warm_gate.sh` now reports
     this distinctly (it used to print an empty `got` under a `FAIL` header,
     which reads exactly like a pixel change and sends you hunting one — that
     mis-report cost an hour on 2026-08-29). Observed 2 of ~7 suite runs, only
     while OTHER agents' `--bench` processes were on the box, never in 8/8
     standalone repeats. Treat it as machine contention: re-run on a quiet box
     (`scratchpad/quiet.sh`) before believing it.
  2. **A row that fails with the SAME hashes every run is REAL.** Bisect it
     with a parent worktree built at your pre-merge commit; that is how the
     2026-08-29 city rows were settled in ten minutes.
  3. **A row that flips between runs is the machine, not the code** — every
     row in the suite is deterministic standalone (greets-warm 8/8,
     city-warm 3/3 measured).
  **AND THE CONTAMINATION THEORY IS REFUTED, so do not spend time on it:**
  suite rows are SEPARATE PROCESSES (no in-process carryover), and the `--hdr`
  city row does NOT rewrite the env cube — all three
  `Runtime/cache/city_envmap_cube_*.bin` are byte-identical (`adbac29c…`,
  `bc17b83d…`, `a896a47c…`) before and after it. **A row's result does not
  depend on what ran before it in the same suite invocation.**
- **A GREEN 13/13 DOES NOT MEAN A PATH WAS EXERCISED. Check whether the path is
  even REACHABLE at tick 1 before claiming byte-nullity from these rows.**
  Every pinned row above is a single-tick `--snapshot`, and several live,
  default-ON engine paths only switch on from the SECOND tick of a process.
  Case study (2026-08-29): the froxel composite's water-reflection punt — 27.6 %
  of city's 8-lane groups, half of `fog-composite` — was rewritten and all 13
  pins passed before AND after, because `FastFog_SetReflectionZ` has nothing to
  hand over until city's water carries mirrored content, which it does not on
  tick 1. The same census also explains why the backlog's own estimate for that
  item was **7x low**: it was taken cold, at 1512x768, where the punted pixels
  it was pricing largely did not exist. Use `tools/warm_gate.sh`; the full
  candidate list of tick-1-cold paths is in `docs/OPTIMIZATION_BACKLOG.md`
  2026-08-29c.
- **`tools/render_gate.sh` IS RESOLUTION-SENSITIVE, and the main tree cannot
  pass it while `Runtime/rev.cfg` carries the user's window size.** The four
  baselines are 1920x1080 DUMMY-mode hashes; his uncommitted `rev.cfg`
  (1384x768 as of 2026-08-25) makes ALL FOUR rows FAIL with perfectly
  deterministic wrong hashes — `mirrortest 3e85645e…`, `rttslot 45d8f216…`,
  `conetest 11877fc6…`, `halotest f6ad21fe…`. **That is not a regression and
  must not be bisected.** Two ways to tell in under a minute, both used on
  2026-08-25: run the gate from a worktree with the stock committed `rev.cfg`
  (ALL PASS 4/4), or point the MAIN tree's own binary at stock assets —
  `cd <worktree>/Runtime && FDS_MIRRORTEST_MULTI_DUMP=1 <main>/build/DEMO/DEMO
  --no-chdir_assets --scene-mirrortest` reproduces `4ac809e5…` exactly. Never
  edit his `rev.cfg` to make the gate pass; gate elsewhere.
- **`--ssao_dump` is not a free observer — it inflates the pass it measures.**
  The dump forces the SCALAR apply loop, taking the `[ssao]` pass from ~4.2 ms
  to **13.7–16.0 ms** (~3.5×) at chase t=1105, 1920×1080, `--ssao_downscale=2`.
  **Never quote an `[ssao]` timing from a run carrying `--ssao_dump`** — it is a
  correctness instrument, not a perf one. This already cost one wrong cost
  figure. Related: when comparing two SSAO arms, INTERLEAVE the runs — the
  zfloor flip's original +0.17 ms was 6 un-interleaved runs per arm and
  evaporated at 14×14 interleaved (2026-08-25).
- **`DEMO` ignores your shell's CWD.** `ChdirToAssetRoot` (`DEMO/REV.CPP:503`)
  chdirs to the *binary's own* directory (first of `<bindir>`,
  `<bindir>/../Runtime`, `<bindir>/../../Runtime` holding a `rev.cfg`). The build
  copies the binary to `<tree>/Runtime/DEMO`, so **a worktree build always
  renders the worktree's assets and writes the worktree's `cache/`**, no matter
  where you `cd` first. `cd Runtime && /path/to/worktree/Runtime/DEMO` does NOT
  do what it reads like. To gate against the user's uncommitted authoring
  assets you must put the binary in a directory whose asset root *is* that tree.
- **The city env cube is a hidden input to the city pin, and it is not keyed on
  flags.** A cold cache re-bakes it and the pin legitimately moves; a run whose
  bake differs silently *overwrites* `Runtime/cache/city_envmap_cube.bin` and
  moves the main-tree pin with no commit. Always `md5` the cube before calling a
  city mismatch a regression. This cost one full "unowned drift" bisect on
  2026-08-08 — see the dated note at the top.
- **greets IS deterministic and IS gate-worthy — FIXED 2026-08-05 (det-hunt
  round 3).** The whole "~1-in-12 flip", then "100 % nondeterministic", then
  "not a bake and not a race" chain resolved to ONE defect: the opaque deferred
  kernel read 8-bit AO maps as `dword`, indexing 4× past the mip allocation
  (root-cause detail in Known Issues). **Post-fix: 0 flips in 128 sequential
  runs of the gate recipe** (one hash, 95 % UB on the rate 0.023), 0/16 with
  `--env_refl` on, 0/16 under `--vanilla`. Treat a greets mismatch as a real
  regression again. Confirm with `tools/flip_rate.sh -n 24` before calling it.
  The instrument stays: **`tools/flip_rate.sh`** — N sequential runs, distinct-
  hash histogram, flip rate vs the modal hash, Wilson 95 % CI, zero-event upper
  bound. A 3-run arm proves nothing at any nonzero rate; that is how rounds 1–2
  lost a day to a "shadow/lightmap bake" bisect that was pure binomial noise
  (at p≈0.85 a 3-run arm shows 2-of-3 with P≈0.32).
  These instrument traps cost real time in rounds 1–3 and still apply:
  **TRAP: the in-process repeat is NOT a valid determinism instrument for
  greets.** The code-screen texture is an ITERATIVE SMEAR
  (`OldBuf → GridRendererT → ScaledBuf → OldBuf`), so it is a function of how
  many times `Render()` has run, not of `t`. Repeating a timestamp in one
  process legitimately changes it. Compare separate processes.
  **TRAP: hash textures at `SizeX*SizeY*(BPP/8)`.** `Texture::BPP` is in BITS.
  Hashing `SizeX*SizeY*BPP` over-reads 8× and manufactures a convincing "these
  8 PBR maps mutate run-to-run" result. Same over-read family as the bug that
  turned out to BE the root cause — when a per-texel width is in play, check it
  first, in both the instrument and the code under test.
  **TRAP: one greets frame runs `renderFrame` SEVEN+ times, at three
  resolutions.** Six 512×512 offscreen passes (shard reflection / mirror RTT)
  and six 32×32 `sh_ambient` probe cube faces run the SAME `renderFrame`
  before the 1920×1080 main pass. Consequences:
  (a) a stage-trace filter that caches "the main width" on its FIRST call
      captures 512, not 1920, and silently hides the main frame;
  (b) the 32² probe faces are the CHEAPEST place to reproduce a shading
      divergence — 1024 pixels, ~2–8 of them differing, versus 2 M at 1080p.
      Round 3's whole diagnosis ran there.
  Always print the pass resolution on every trace line.
  **NOTE on `ctx.gb`:** round 2 recorded "hashing the global `g_gbuffer` is
  wrong for nested passes". In fact `EngineGBuffer_Resize` installs ONE global
  buffer and the offscreen passes address it at their own (smaller) stride, so
  `ctx.gb == g_gbuffer` for the probe passes — the real requirement is to hash
  only the first `xres*yres` entries of each plane, and to hash ALL ELEVEN
  planes (normal, tangent, txtr, albedo, lightmapMF, lightmapST, shadowMatID,
  faceId, mirrorId, mirrorMask, mirrorMaskZ), several of which are empty by
  default. Shard/mirror bakes with their own `DeferredOverride::gb` are the
  genuine exception.
- **city cache**: `cache/city_envmap_cube.bin` is keyed on CITY.FLD bytes.
  After ANY CITY.FLD install, discard the first run (cache rebuild), then hash.
- Greets pin includes the USER'S UNCOMMITTED files (GREETS.FLD/MAT, momy
  textures, Piramid.lwo, Hull.lwo) — a clean checkout hashes differently.
  Those files are his: never stage, never overwrite, never `git add -A`.
- Editor page freshness: build tag in the panel header (currently b60/b61);
  bump it with every shell.html change or staleness is undiagnosable.
- **chase**: no bakes, no known nondeterminism (pinned srand, fine clock off
  in snapshots). Both pins above confirmed byte-identical over 3 runs each
  (2026-07-12, C0). **RECIPE-FRAGILE**: chase accumulates snapshot state across
  the timestamp-list loop, so a given t's hash depends on the WHOLE list —
  the pins are ONLY valid for the exact recipe `t=100,400,800,1200,1600` (and
  the cinematic `t=800,1600`). Running a subset/superset gives different (still
  deterministic) hashes — NOT a regression. Always gate with the exact list.
  **STALE AT t=1600 (measured 2026-08-05):** with the user's uncommitted
  `Runtime/SCENES/CHASE.FLD` + `Authoring/chase/*.lwo` in the tree, default
  t1600 is `c8c93b886dd31fcc01363c806d7626de` and cinematic t1600 is
  `debdb1f435a14949b2e05be0bb53b1e7`; t100/400/800/1200 and cinematic t800 all
  still match the pins above. Those two are the mountain edits, not a code
  regression — same binary, same values before and after that day's flag work.
  Re-pin them when he commits the FLD.
  Valid snapshot range **t=0..1698** (past 1699 the harness re-dumps the last
  rendered VPage). Regen from `Authoring/chase/` via `pin_scene.py
  --legacy-vlum` is byte-identical to the shipping FLD (delta=0, 747,511 B) —
  the pre-edit baseline for later authored chase stages.
- **Chase cone-tile "missing light on the rect" — FIXED (2026-07-30).** User
  saw rectangular seams in the lighthouse beams during chase (~t=211,
  cinematic). Root cause: the volumetric cone-pass tile cull
  (`Render_VolumetricCones`, `--spot_cone_cull`) computed each tile's far
  bound `zHiT` from `tileLights.zMax` = the farthest **opaque surface** only.
  `computeTileDepthBounds` excludes sky/untouched pixels from `zMax`, so a tile
  that MIXES surface + sky under-estimated its volumetric depth: a beam glowing
  in the tile's sky portion (rays that run to the fog cutoff) got clipped away
  there but kept in the adjacent pure-sky tile → a rectangular per-tile seam.
  Fix: `TileLights.hasSky` (set in `computeTileDepthBounds` when any pixel
  `zEnc==0`); the cone cull extends the far bound to the fog cutoff for
  has-sky tiles (tight opaque `zMax` retained for fully-covered tiles, so
  covered scenes keep the cull's perf). Result byte-== `--spot_cone_cull=0`
  ground truth at every pose; the chase pin move above IS this fix. Cone-pass
  cost +~1–2 ms at t≈211/700 (the previously-dropped correct beam work); still
  ~6–10 ms cheaper than no cull. city/fountain pins byte-unchanged (no
  mixed-sky cone tiles); render_gate 3/3 (conetest byte-identical).

## The big architecture decision (2026-07-11, user-set direction)

> **✅ DONE 2026-07-31 — the sidecar-elimination campaign is COMPLETE.** The
> `.MAT` reader (`MaterialImport_ApplySidecar` + `_ApplySceneDefaults` + helpers)
> is DELETED; every scene now calls `MaterialImport_ApplyRevMaps` (LWO RVSM) in
> its place. `Runtime/SCENES/GREETS.MAT` (last sidecar, data-empty) is DELETED —
> no scene ships sidecar data. The 7 (+1) `#k` split-collapse sites in
> `tools/editor_server.py` are DELETED; splits bake real surfaces via
> `payload.splits` geometric centroids. Save-completeness proven headlessly
> (byte-identical FLD idempotent regen + combined RVSF/RVSM/SMAN gain + split
> without `#k`). Gates: render_gate 3/3, city `37e62845`, fountain `51fff7cd`,
> momy close-cam `7d05a1be` byte-equal. Leftover WRITE-only, not-yet-FLD-backed
> (editor writes a `.MAT` nothing loads, warned): `obj:scale` (§1d FdsObjectScale
> unimplemented) and `normalFlip` (§1e RVSM write-back unimplemented). See
> docs/SIDECAR_MIGRATION_PLAN.md. Original direction preserved below.

**Sidecars are being eliminated.** Persistence belongs in the authoring
sources: per-surface → custom LWO SURF sub-chunks; per-light / per-object /
scene-level → LWS keywords; everything flows through tools/lwsread into the
FLD via **flag-bit + conditional payload** records (the proven extension
idiom — see next section). Crash (no sources yet) falls back to fldpatch
writing the same extended records. Sequencing constraint: writers first, user
re-saves greets once (his GREETS.MAT is the only record of the momy map
assignments), THEN the sidecar reader dies. In-flight work (see bottom)
already follows this; a full migration campaign (all SURF_SIDECAR_KEYS +
light:/obj:/scene: keys, editor Save rewrite, reader retirement) is the next
major batch.

## The FLD/LWS extension mechanism (use this for every new authored property)

Proven end-to-end by the volumetric-beam work (9172c5d):
1. LWS text keyword(s) per light/object/scene (e.g. `VolumetricLight 1`,
   `VolumetricLightIntensity 3.0`) parsed in tools/lwsread/LWSREAD.CPP
   (BOTH build variants: lwsread + lwsread_legacy — same source).
2. FLDSAVE.CPP writes a NEW flag bit + conditional payload after the record
   (bit-gated fields are the FLD's native extension shape; FLDs without the
   bit stay byte-identical — prove with a regen diff).
   **TRAP: bits 256/512/1024 of the light flags are OR-contaminated by
   ReadEndBehavior — bit 512 was NOT free. Headlight beams use bit 2048.
   Always check what ORs into a flag word before claiming a bit.**
3. Engine FLD loader (FDS/FLD/FLD_CONV.CPP) reads the conditional payload
   into an Omni/Material/Scene field (0-sentinel = unset → legacy default;
   GreetsMirror clones inherit via memcpy — sane by construction).
4. Editor write-back: tools/editor_server.py patches the LWS/LWO, regen via
   the scene's lwsread variant (legacy for chase/fountain/city — VLUM×100
   era), installs the FLD (backup to Runtime/SCENES/.backups/ first).

## What landed (grouped; commit msgs carry the detail)

### Authoring recovery — city is a full authoring scene
- Sources found IN-REPO (Original/dos-rev/.../CITY/): CITY1.LWS identified by
  light-set fingerprint; 17/20 objects byte-exact. b1/b3/b6 shipped higher-
  poly than any surviving LWO → recovered FROM the shipping FLD via
  tools/fld2lwo/ (byte-parity regen: CITY1.LWS → shipping CITY.FLD exact).
  Authoring/city/README.md has provenance + regen commands. (d60f5ab,
  cc6244e, 4f943a1)
- **Search lessons** (for the crash hunt + future archaeology): match by
  embedded SRFS surface-name sets, not filenames (the b3 slot holds a "b7"
  building); list ARJ archives (first sweep missed them); lwsread maps LWO
  points 1:1 to FLD verts — NO seam splitting, count-matching is valid.
- 46 authored headlight spotlights baked into CITY1.LWS (two per vehicle,
  parented, warm 255/235/185, 15°/30°), engine gained LightType-2 spot
  conversion + parented aim + flare-stamp skip. Code headlight schemes
  retired (default off, kept for A/B). (48d57e5, e4e34cf)
- Authored volumetric beams: per-light `VolumetricLightIntensity` gain
  (gain 3.0 shipped); retune = tools/add_city_beam_flags.py + regen. (9172c5d,
  1275dea)

### Determinism
- `srand(time(NULL))` → pinned seed (GENERAL.H). The Omni_Rand flare twinkle
  made every run unique — bakes-on frames all-distinct, bakes-off glass-band
  flips. (5f325d4)
- TBR transparent order: facing rank precomputed at insert (torn reads in the
  concurrent sort flipped front/back). (1e91306, fb3a302)
- Glass band scheduler: B1/B2 back/front sub-phases fixed the deterministic
  greets "face pop". (8539e8f → 8539e8)
- Scene clock sawtooth (user-visible "city camera jumps back"): rate was an
  EMA of instantaneous dTimer/dWall (Jensen-biased ~10% high) + hard snaps on
  hitches. Now ratio-of-EMAs + hitch-hold + continuous anchor. (2541c32)

### Rendering features (all default-off unless noted)
- Screen-space glass refraction stack (Mat_Refractive opt-in, per-material
  IOR, band scheduler w/ barriers) — editor ON by default. (f4d470a..)
- HDR: 250 lit-cap now HDR-gated in vec+transparent kernels too → luminosity
  >1 blooms (was scalar-only; editor edits were silently capped). LDR keeps
  the cap. NOTE: OuterVec still stores 8-bit — radiance >255 needs the scalar
  kernel on PreferOuterVec scenes. (f6ec404)
- Cone turbulence/swirl: world-space value noise + helical swirl in ALL
  three cone integration paths, SIMD (+3.2ms greets); user's tuned values in
  Runtime/PRESETS/greets-beams.flags. TRAP: reshaping the fmadd chain moves
  hashes by ulps even at neutral values — off path keeps the exact legacy
  expression. (ab9a9c1, 189eeec)
- Env live water (city): probe bake hides the water mesh, re-shades plane
  texels with the main-view procedural formula (Schlick + caustic cells);
  sample-time WaveSlope perturb animates. Bypasses the pristine cache when
  on (~4s init, 0 per-frame). Glints not baked (view-dependent). (5d28db7,
  0162d3b)
- fastfog dist-dim slice 4: sky dims at horizon + forward pixels dim
  (inert at default 0). (8acf8cd)

### Flags / presets
- `--flags-file=<path>` (nestable, comments, CLI-after-file wins) + unknown
  `--flag` WARNING (was silent — a typo'd `--fast-fog-blob` ran slab fog for
  weeks). Runtime/PRESETS/city-noir.flags = the user's city look, cinematic-
  based, measured byte-equivalent to his old 40-token line + blobs fix.
  (2482013)
- Per-surface migrations of former globals: waterProcedural (tri-state),
  envRefl (tri-state), envBakeRes (pow2, largest-wins on shared probes),
  RefractIor; scene-level sidecar defaults for boltFlash/fastFog bounds —
  NOTE: these sidecar forms are transitional; the LWS/LWO migration
  supersedes them. Precedence everywhere: per-surface > CLI/env > scene
  default > compile default. `SCRIPTS/<scene>.params` lines still override
  scene defaults (per-frame scripts yield only to explicit-set marks).

### Editor (browser, `make editor` → :8099/DEMO.html?editor&scene=<name>)
- Objects: FLD-tree hierarchy in all scenes (chunk-collapse `:cN`, engine
  helpers pooled in hidden "(engine)"; NAMED engine meshes with faces get
  visible entries — that's how the disco ball became reachable, plus its
  material needed MatLib linkage). Per-object scale knob (EditorScale on the
  Scale spline — pivots correctly, all instances; Tri_Possessed meshes are
  honestly inert). Focus = nearest-instance, in-context (2.5× radius).
- Lights: grouped by parent object, multi-select (ctrl/shift), group edit,
  click-to-select in viewport; authored city headlights appear grouped per
  vehicle.
- Surfaces: split w/ mirrors + #1/#2 naming (persistence via source-bake in
  flight — see below), map reset ✕ (restores authored incl. tangents), pack
  picker with FreePBR preview renders (98.7% coverage), procedural
  displacement generator (FBM), map-viz overlay, live smoothing, xpar PBR.
- Editor camera: instant (no momentum) in editor mode only.
- Boot race fixed: objects list retries (city published CurScene mid-init);
  console.warn when objects empty while surfaces exist.

## Known issues / deferred (honest list)

- **Greets mirror: cones leak through wall + doubled screen text
  (2026-07-30, user-reported, NOT yet investigated).** Repro:
  `FDS_GREETS_CAM="-6.75174379,3.12747574,-51.7348709,-0.0600466765,-0.148574546,-0.987076521"`
  t=3430, looking at a text-screen mirror panel. Two symptoms in one frame:
  (a) volumetric cone shafts/blooms visible INSIDE the mirror view where a
  wall should occlude them — suspicion set: the eb36c1f hasSky far-bound
  extension interacting with the mirror RTT bake's G-buffer, or the RTT
  bake's cone pass integrating behind its near plane; (b) the greets text
  ("kombat") rendered TWICE — one crisp, one ghosted/offset below — likely
  the half-silvered composite (text + reflection) meeting a second text
  source (base panel texture vs RTT/recursion path; the recursion-composite
  interaction is a known open item from MIRROR_RECURSION_PLAN slice 3).
  User's exact launch flags (2026-07-30): `FDS_POM_CONE=1 FDS_TEXTURE_FILTER=1
  FDS_POM_SPIKE=8 FDS_PARALLAX_STRENGTH=3 ./DEMO --shadows
  --greets-omni-shadows --greets-omni-default-range=30
  --greets-omni-shadow-res=256 --shadow-skip-animated --greets-spots
  --shadow-dynamic --shadow-lightmap-planar --shadow-lightmap-res=64
  --shadow-lightmap --greets-mirror --mirror-rtt --greets-mirror
  --mirror-rtt-density=1024 --cone-strength=5 --bloom --disco-bloom=0
  --shard-deferred --greets-shard-fall-speed=1 --greets-shard-randomness=0.8
  --hdr-linear --greets-shard-res=64 --bloom-intensity=1.5 --hdr-refl-gain=4
  --cone-fine-tiles --anamorphic --anamorphic_intensity=1.5
  --anamorphic_vert=0 --anamorphic_decay=0.3 --anamorphic_passes=2
  --lens_ghosts --lens_ghost_intensity=0.05 --lens_ghost_count=0
  --lens_ghost_dispersal=0.01 --lens_ghost_halo=0.01 --chromatic
  --chromatic_amount=3 --vignette --vignette_strength=1 --dof --dof_range=20
  --dof_max=4 --greets-stone-tex --ssao-downscale=2 --ssao-gtao
  --ao_map_strength=1 --parallax_strength=0.1 --parallax --nmap_16bit --hdr
  --ssao --shadow_bake_time --aa --pbr --shadow_cube_face_cull
  --deferred-quarter --ssao_temporal --parallax --parallax_pom_lod
  --glass-refract=1 --glass-test --xpar-peel-passes=4 --cone-turbulence=3.5
  --cone-swirl=0.7 --env-brdf-analytic --sh-ambient --diffuse_energy
  --pbr_multiscatter` — note NO --mirror-recurse-depth (order-1/2 RTT path,
  not the recursion), and --deferred-quarter + --hdr are in play (the known
  wave-2/HDR checkerboard interaction family). **Does NOT repro on bare
  ./DEMO** (user-confirmed) — flag-gated; first bisect candidates when
  picked up: --shard-deferred, --hdr/--hdr-linear/--hdr-refl-gain=4,
  --deferred-quarter, --cone-fine-tiles. Parked deliberately
  ("finish the other threads first").

- **Greets render nondeterminism — CLOSED (2026-08-05, det-hunt rounds 1–3).
  TWO root causes, both proven, both fixed.** The old "~1-in-12 flip / subtle
  pano slivers / deterministic with bakes off" description was wrong on every
  count. Harness: **`tools/flip_rate.sh`** — N sequential runs of a scene's
  gate recipe, distinct-hash histogram, flip rate vs the modal hash, Wilson
  95 % CI, and a zero-event upper bound. Use it; a 3-run arm proves nothing at
  p≈0.85 (that is how rounds 1–2 chased a bake/race that never existed).
  - **FIXED (proven, this commit): `GreetsGenerator::Init()` read
    uninitialized heap as the greets code-screen SMEAR SEED.** `OldBuf` /
    `ScaledBuf` / `CodeBuf` were `_aligned_malloc`'d and never zeroed, and
    `OldBuf` is the feedback source that `Render()` resamples into the screen
    texture (`Txtr->Mipmap[0] == OutBuf`) every frame. Causal chain measured
    per pixel, not inferred: at px (1113,376) / material `screen2` every term
    matched across runs (matID, zEnc, refracted background, blend alpha,
    tile-light count) EXCEPT the sampled texel — `9bd0204f` vs `5ecf175c`;
    after the memset it is stable. Stage trace: the divergence entered at
    `TBR_Render` round 2 phase B1, with `beginframe`/`lighting`/`ssao`/
    `hdr-activate`/`pre-tbr` all byte-identical.
  - **Measured effect of the fix (N=48 per arm, same box, same load):** flip
    RATE essentially unchanged — pre 40/48 = 0.833 [0.704, 0.913], post 43/48
    = 0.896 [0.778, 0.955]. What moved is the SIZE of the divergence, over 6
    run-pairs each: differing pixels median **18.2 % → 14.2 %** and max
    per-channel |Δ| **251 → 95**. So the whole-object black-vs-lit flips are
    gone; a smaller, low-amplitude residual remains. Landing it anyway: it is
    a proven read of uninitialized memory into rendered output.
  - **RESIDUAL — CLOSED, ROUND 3 (2026-08-05). ROOT CAUSE: the opaque
    deferred kernel read 8-bit AO maps as `dword`.**
    `Material::AoMap` comes out of the importer as SINGLE-CHANNEL 8-BIT
    (`loadRoleMapCached` → `MakeHeight8`, same as height / roughness /
    metallic). `DeferredSurfaceKernel.cpp`'s ambient block fetched it as
    `((const dword*)aoTex->Mipmap[miplevel])[swizzledUV]`, so every AO sample
    landed at byte offset `4 × swizzledUV` inside a 1-byte-per-texel
    allocation, and it never bound-checked `miplevel < numMipmaps`. Every
    sibling fetch — roughness (:1125), metallic (:2576), the whole transparent
    kernel (:3156) — already read BYTES and checked the mip bound. This one
    site did not.
    **MEASURED per pixel, not inferred** (32² probe face 0, px (27,19),
    matID 11): `aoBPP = 8`, mip 0 = 1024² = 1,048,576 bytes,
    `swizzledUV = 995355` → dword read at byte **3,981,420..3,981,423**, i.e.
    **3.8 MB past the end of the allocation**. Across four runs everything
    else in the per-pixel record was byte-identical (matID, pmid, zEnc, x/y/z,
    normal, mip, swizzledUV, per-light `intensity`/`k`/reject-stage for every
    light in the tile) — only `aoRaw` moved: 0.489 / 0.615 / 0 / 0.051.
    THE CHAIN: `ao = 1 - ao_map_strength(2.0) × Mat->AoStrength × (1 - aoRaw)`
    → `aoRaw = 0` gives **ao = -2.22**, so the ambient seed went NEGATIVE
    (32 → -71.04) and `if (lB < 0) lB = 0` clamped it to zero. That is exactly
    round 2's "diffuse `lB/lG/lR` flips between 0 and a full value while
    SPECULAR is byte-identical" — AO multiplies the ambient (diffuse) term and
    never touches specular, which is why every light-loop hypothesis missed.
    It also explains `lB == lG == lR` at the flipping pixels: greets' ambient
    is grey (32/32/32), so the ambient seed is achromatic by construction.
    Round 3's hypothesis (a) — a stale tail lane in the 8-wide light batch —
    is **DEAD and should not be re-tried**: `zeroTileLightPadding`
    (DeferredLightLists.cpp) explicitly zeroes count..paddedCount and stamps
    `mirrorId = 0xffffffff` so padded lanes can never pass the mask, and the
    per-light dump showed every lane's `intensity`/`k`/stage identical at the
    diverging pixels.
    **THE DISCRIMINATOR THAT CRACKED IT was one run of `--no-ao_map`**: 4/4
    byte-identical frames and 0/1024 diverging probe pixels on all six faces,
    before any code was written. Cheapest-discriminator-first, again.
    **FIX (this commit):** read the mip as `byte*`, branch on `BPP == 8`, and
    bound-check `miplevel < numMipmaps` (mirrors the transparent kernel). The
    32-bit branch stays for the `ao_from_diffuse` dev fallback.
    **POST-FIX [M]:** 0 flips in **128 sequential runs**, one hash
    `f5778c7b78a4d70655291363e4119c66` (95 % UB 0.023); 0/16 with `--env_refl`;
    0/16 under `--vanilla`. Probe faces 0/1024 differing over 6 runs.
    **LOOK CHANGE [M]:** greets now shades with the real AO map — 26.3 % of
    the gate frame moves, mean |Δ| 12.4, max 98. Four materials carry separate
    AO maps (`momy-1`, `amudim`, `stairs`, `rooms`). Wants the user's eye.
  - Gates after both fixes: city `37e62845`, fountain `51fff7cd` byte-exact;
    mirrortest/conetest/halotest all PASS. Greets-only effect (no other scene
    ships a separate 8-bit AoMap).
- ~~Env-bake content varies run-to-run~~ **RESOLVED 2026-08-05 by the AO fix**:
  the env panorama bakes render through the same opaque deferred kernel, so
  they inherited the same out-of-bounds AO read. Measured after the fix: the
  greets gate recipe with `--env_refl` ON is **0 flips in 16 runs**
  (`33c73ac43520a8ff5be262a99fc61f98`). Re-measure with `tools/flip_rate.sh`
  if it ever looks unstable again.
- The user's GREETS.MAT `momy#2|*` lines are DROPPED at load until he
  re-splits + re-saves in the editor (split-bake landed 6c6c972 — re-save now
  bakes momy2 into the LWO as a real surface; accepted, he regenerates).
- volumetric_unified (default-off Beer-Lambert pass) ignores per-light cone
  gain + turbulence.
- Mirror clones don't reflect a live object re-scale; tram return-leg beams
  face backwards (real shuttle behavior); fast_fog_blob_overlap clamps at
  1.5 (3×3×3 neighborhood); police strobe not in the authored lights.
- Legacy equirect env path (--no-env_cube) keeps static bake water.
- CITY.CPP line ~1575 unused `using std::min` (clang-tidy noise, off-limits
  era leftover — fine to fix opportunistically).

## Recently landed (was in-flight — verified + committed)

1. **Split persistence via SOURCE BAKE** (6c6c972): editor Save bakes runtime
   instance-splits into the LWO sources (lwopatch.split_surface reassigns
   non-primary polygon clusters to new real surfaces; bake_splits in
   editor_server.py matches live cluster centroids to source polygons). After
   reload the #k names are real authored surfaces. Crash/no-source scenes
   stay live-only. Recipe: re-split momy → Save → reload → momy/momy2 real
   surfaces with maps. Editor-flow verified by inspection (pieces + pins);
   NOT yet driven through a live browser split-save round trip.
2. **Scene-wide env defaults as LWS keywords** (6c6c972): FdsSceneEnvRefl /
   FdsSceneEnvBakeRes → bit-2048 conditional FLD payload → Scene fields →
   FramePrep. VERIFIED end-to-end (round-trip +8B; live read-back envRefl=1/
   res=512 → 133 probes at 512). Editor 'scene env defaults' row, tag b61.
3. **Crash sources + registry flip DONE** (470d7f1 + 6c6c972): vintage "END"
   laptop scene, lt_scr FLD-recovered via fld2lwo_crash.py, byte-parity regen
   (md5 4f8aac84…). crash promoted to authoring. EVERY scene is now
   source-authored — the fldpatch fallback in the sidecar-elimination plan is
   dead.
4. **Chase upgrade plan** (docs/CHASE_UPGRADE_PLAN.md): 612-line staged plan
   (blasters, hit particles, camera, movement, lighting + more) — planning
   only, awaiting user stage-selection before any implementation.

## Queued next (user-requested, 2026-07-11)

- **CHASE WATER DARK BAND — DONE (604fd43).** Ported soa-vertex 9902349; chase
  water now bright/uniform, no band (t1600 verified); chase pins re-baselined
  (table above); city/fountain byte-identical; render_gate PASS; wasm links.
  Original note kept for context:
  The "lower missing water layer" (horizontal seam, dark band below)
  is the documented `chase water dark band` bug, FIXED on ~/work/revival
  `feature/soa-vertex` commit **9902349**. Two defects: (1) InsertTransparentToTBR
  (FDS/FILLERS/FILLERS.CPP ~1796) computed the strip span from projected PY,
  garbage for the camera-STRADDLING water quad → water inserted only above the
  horizon, vanished below (the band = mirrored-mountain underlay). Fix: verts
  in front of near plane → insert into EVERY strip, sort by FAR surface.
  (2) water_procedural kernel composite darkens vs chase's black night-sky
  reflection → new flag `water_fresnel_composite` (default ON=city), chase
  factory sets it OFF, city re-pins ON. fog-wt CONFIRMED at the exact
  before-state (FILLERS.CPP:1807/1816 old PY code, DeferredSurfaceKernel.cpp:2525
  waterProcOn, no flag yet, CHASE.CPP:996-999 factory). Port all 5 files
  (FILLERS.CPP, DeferredSurfaceKernel.cpp, FeatureFlags.def, CHASE.CPP, CITY.CPP),
  verify chase water bright/no-band + city/fountain byte-identical (fixes city's
  bottom-strip band too, brighten-only).
- **CHASE SPOTS realign — NEEDS REDO** (user "not seeing the spotlights").
  CONFIRMED: L2.3 canyon spots don't visibly light the mountains with the new
  trail-follow camera (verified at t=1200 — moonlit grey, no warm/cool). A
  realign agent (aa4f40da) DIED at the session limit mid-work; its uncommitted
  variant-a checkpoint (surface-wash) did NOT make them visible (still grey) and
  was DISCARDED (reverted to HEAD). Redo needs: re-aim at the mountains the new
  camera frames (t≈1100-1300), BOOST intensity/contrast so warm/cool reads
  against the moon, and try visible VolumetricLight beams (bit-2048; L2.3 dropped
  them as near-invisible — tighter cones + higher gain, esp. under cinematic
  fog). Deliver beams-vs-no-beams A/B. Chase-only (CHASE.LWS + regen).
- **CHASE COMBAT — LANDED, all default-OFF, gated, deterministic, inert**
  (blasters agent a3471e22, resumed repeatedly): 22963db denser barrage
  t≈340-1700 · ff821a2 B2 impact-spark particles · 1e55078 C1 `chase_cam_fx`
  camera shake + FOV punch on hits · ecc3359 chase-scale bolt-light reach
  (blaster_light_range 90, intensity 260, via setDefault in createChaseScene) ·
  2d33373 B2 near-miss water-splash columns (a `water` aim mode in the fire
  table — tracers punch the sea, vertical spray). Combat CODE side is now
  feature-complete.
  All pure-t (snapshot-safe), OFF byte-identical (chase pins unchanged
  9cc80e9e…), city/fountain unmoved. Flags: chase_blasters, chase_spark_size
  (0.00005), chase_spark_bright (255), chase_cam_fx, chase_cam_shake_gain
  (0.04), chase_cam_fov_kick (5). Awaiting user look-approval before default-on.
  **KEY readability finding (agent):** combat reads subtle because ships are
  small in frame + Ship1's oversized L1 engine flare washes nearby sparks —
  the real levers are the FLARE TAME + closer combat FRAMING, not the bolts.
  Deferred combat follow-ups: water-splash columns (need bolt↔water-plane
  intersect), act-3 return-fire + venting hit, FdsMuzzle keyword, bloom-threshold
  tune for bolt cores. NOTE: this agent kept getting resumed and committing
  autonomously — verify+reconcile each time; consider routing chase work through
  one path.
- **EDITOR STABILITY — DONE** (30c2931 texture dedup by path + 339c65a wasm
  INITIAL_MEMORY 128→512MB). Fixes material-reuse + the unaligned-atomic import
  crash. Native pins byte-identical; user confirms in-browser (make editor →
  import same map on 2 surfaces = 1 decode + [reuse]; roughness import = no crash).
- **EDITOR STABILITY (2 issues, user 2026-07-14; blocked on build = blasters
  agent finishing):**
  (a) `unaligned memory access` crash on texture import (e.g. roughness on
  R_leg1.lwo::hull) — same trap class as the audio crash: a wasm ATOMIC op on
  a misaligned address, under the editor's `-pthread + ALLOW_MEMORY_GROWTH`
  build when an import allocation triggers a HEAP GROW while threadpool workers
  are live (emscripten#17816/#23806, already noted in DEMO/CMakeLists.txt).
  INITIAL_MEMORY=128MB → grows on every import. Mitigations: raise
  INITIAL_MEMORY so typical imports don't grow; and/or run the material apply
  single-threaded so no worker is mid-atomic during a grow.
  (b) **Material/texture NOT reused across objects** — same material re-decoded
  + re-allocated per object instead of sharing one loaded Texture. Wasteful AND
  a direct cause of (a): N copies = N allocs = more grow events = more crashes.
  Fix: a texture dedup cache keyed by source path in the import path (the code
  `new Texture` + re-decodes each ApplyMapFile). Do this FIRST — highest value,
  verifiable, and it cuts the crash rate. Distinct from the metallic-import OOM
  (8d936e0, already fixed).

- **Editor UX batch** — DONE (2026-07-12, d5a6ae9, tag b66): solid
  metallic/roughness generators (the mech-metallic recipe), Save "what
  changed" receipt, persistent status bar, canvas-fits-beside-panel (letterbox
  via CSS; fill=engine-resize deferred), settings find/category-groups/
  changed-only. shell.html-only; native/pins untouched.
- **Chase upgrade** — plan in docs/CHASE_UPGRADE_PLAN.md. Provenance: chase is
  a scene BUILT-BUT-CUT in 1998 (lack of tuning time), hand-corrected in the
  revival — **NO sacred 1998 baseline; free to retune for look** (user,
  2026-07-12). **C0 + S0 + L1 landed (2026-07-12, fog-wt).**
  L1 (4a54af5/3bb68ea/4cb7513): flare sanity + SceneCorrections retirement +
  sky gradient. New identifiers: LWS `FdsFlareScale` → light-bit **4096**
  (Light_FlareScale) → Omni::FlareScale; LWS `ZenithColor/SkyColor/
  GroundColor/NadirColor` → scene-header bit **4096** (Scene_SkyColors, on
  AmbientIntensity EndBehavior — distinct word from the light bit) →
  Scene::Sky*; flags `chase_legacy_omni_hack` (default OFF, retired hack
  escape-hatch), `sky_gradient` (default OFF — the CANDIDATE, generic: would
  paint city/crash void sky too). Chase default pin RE-BASELINED (table above).
  Sky is opt-in pending look-approval + a default flip / chase preset (can't
  use SCRIPTS/chase.params — protected). Moon light is degenerate (IRange=0,
  no contribution) — preserved from the hack, future tuning target.
  Authoring/chase/README.md now STALE ("byte-parity 1998" no longer true) —
  small doc-pass TODO. C0 (b72e7a9): chase gate pins (RECIPE-FRAGILE) +
  stale-comment fix. S0 (30a9c2e):
  `tools/build_beatmap.py` + `Authoring/chase/chase.beatmap` (placement-
  agnostic — chase has NO track slot yet, arbitrary song+start-order
  scaffolding), `DEMO/ChaseEvents.{h,cpp}` (beat-map + event-table loader +
  pure-`t` `Events_ActiveAt` — the §8.B contract), flag `chase_event_test`
  (default off) + RunChaseSnapshot determinism proof.
- **`Modplayer_GetPosition` — LANDED on master** (2026-07-12): decision (b),
  decoupled from the refactor migration. Parent ce615c2 bumps submodule
  e6429cf → **9d2a1ca** + adds the header decl. Pure lock-free FFI accessor
  over the EXISTING display triple-buffer (`SongState::get_position` reads the
  `TripleBuffer<PlayData>` the display path already publishes — NO new atomics,
  per the user's correction). Fields: order/row/tickInRow + songTick =
  **milliseconds-since-start** (PlayData has no u64 tick; ms is the monotonic
  clock; add total_samples to PlayData if sample-exact ever needed). **Getter
  requires `Modplayer_SetDisplay(handle,true)`** — the demo currently sets it
  FALSE (REV.CPP:1077/1619) for perf, so a sync consumer must re-enable it.
  Verified monotonic under playback; dead-stripped from DEMO (0 refs → pins
  unchanged); both builds link.
  **ACTION NEEDED FROM USER: merge modplayer PR #20**
  (github.com/Gil-AdB/modplayer/pull/20 — direct push to origin/master was
  branch-protected, so 9d2a1ca is on branch `feat/modplayer-getposition`). If
  it SQUASH-merges to a new SHA, re-bump the parent pointer to the merged
  commit (noted in ce615c2's message).
- **DEFERRED: modplayer `feat/s3m-refactor` adoption** — a more accurate S3M
  player, but it PREDATES the embedder FFI (verified: has Create/Start/
  SetOrder; MISSING SetDisplay/FillBuffer/FillBufferPlanar + the external-audio
  cargo feature). Adopting = a full re-port of the embedder audio layer onto
  the refactored core, its own focused task. Pick up when S3M playback
  accuracy becomes the priority.
- **Sidecar-elimination migration** (the big one): now that all scenes are
  source-authored and the scene-env keywords proved the pattern, migrate the
  remaining SURF_SIDECAR_KEYS + light:/obj:/scene: keys to LWO SURF
  sub-chunks / LWS keywords → FLD payloads, rewrite editor Save, retire the
  sidecar reader (writers first; user re-saves greets once; then reader dies).
  **INCLUDES: rip out the `#k` split-marker scaffolding** (user-confirmed
  2026-07-11). `#k` is vestigial — it exists only because the OLD sidecar/
  live-only-split path couldn't bake, so a `momy#2` edit had to COLLAPSE back
  to the real `momy` surface (the 7 `re.sub(r"(#\d+)+$","")` sites in
  editor_server.py: lines ~146/271/287/389/424/761/932/958). Two facts make
  it dead once sidecars go: (1) splits now BAKE into the LWO → parts are real
  surfaces, nothing transient to collapse; (2) the shell sends explicit
  `payload.splits` with per-part world centroids and bake_splits matches
  parts GEOMETRICALLY, never by parsing `#k` from the name. So in the
  migration those collapse sites get DELETED (not guarded), and a split
  becomes "make a real surface, reassign polygons, any plain name". Do NOT
  add existence-aware-collapse or build further on `#k`. Post-migration the
  only transient label needed is cosmetic (the live window between "split"
  and "Save") — nothing functional keys off it. That also dissolves the
  momy#1/#2-vs-momy2 naming question (currently unresolved, left as-is on
  purpose): naming becomes a free cosmetic choice, not a load-bearing
  convention. Engine side is already clean — Editor_BaseSurfName strips only
  ::mirUV, never `#`.

## Where the rest of the knowledge lives

- Cross-session traps + pins history: memory `measurement-tool-traps`
  (~/.claude/.../memory/) — race-hunt methodology, instrument pitfalls,
  binomial rule, pin re-pin log with justifications.
- Authoring provenance: Authoring/city/README.md (parity math, regen).
- Pipeline wiring: docs/GRAPHICS_PIPELINE.md, docs/ENGINE.md (pre-campaign
  but still accurate for the core).
- Old shipping FLDs: Runtime/SCENES/.backups/ + git history (last commit
  carrying each noted in the promotion commit messages).
