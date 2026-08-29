# `FeatureFlags.def` — candidates for deletion

**Tree:** branch `fog-wt`, HEAD `0223d14b` (2026-08-29). Working tree dirty in
`Runtime/` and `docs/img/`; nothing this audit read is uncommitted.
**Scope:** read-only. No edit was made to any tracked file; the two files this
audit produced are this document and `tools/flag_audit.py`.
**Table audited:** `FDS/Base/FeatureFlags.def` — 780 lines, 543 128 B,
**606 flags** (324 `BOOL`, 172 `FLOAT`, 110 `INT`), of which 13 are behind
`#if FDS_DEV`.

**Regenerate everything below:**

```sh
python3 tools/flag_audit.py --json build/flag_audit.json --md build/flag_audit.md
# ~13 s. Every number in this document comes out of that one run.
# FLAG_AUDIT_DATE / FLAG_AUDIT_LANDED_CUTOFF override the "2 weeks" cut-off.
```

---

## 0. Summary

| bucket | flags | help-text bytes | in-loop reads | flags with an in-loop read |
|---|--:|--:|--:|--:|
| DELETE-UNWIRED | 2 | 324 | 0 | 0 |
| DELETE-REFUTED | 3 | 5 397 | 0 | 0 |
| DELETE-LANDED-AB | 23 | 39 321 | 8 | 6 |
| DECIDE-LOOK | 10 | 27 595 | 4 | 3 |
| UNSURE | 25 | 39 377 | 8 | 5 |
| KEEP-DEBUG | 76 | 56 177 | 29 | 18 |
| KEEP-TUNABLE | 467 | 291 444 | 107 | 76 |
| **total** | **606** | **459 635** | **156** | **108** |

**28 flags are proposed for deletion** (2 UNWIRED + 3 REFUTED + 23 LANDED-AB),
carrying **45 042 B** of help text — 9.8 % of the table's prose and 8.3 % of the
file. **10 more are DECIDE-LOOK**: they are not deletions, they are decisions
you owe, and each one is one sentence long below.

Three facts that frame the rest:

* **Help text is 84.6 % of the file** (459 635 B of 543 128 B). Cutting flags is
  how you cut the file; the median flag carries 414 B, the top row 7 208 B.
* **Two categories are 43 % of the prose.** `parallax` (122 flags, 147 213 B) and
  `greets` (87 flags, 86 290 B) are the live displacement/S1d research surface.
  I did **not** propose them for deletion — they are the campaign's working set —
  but that is where the mass is, and it is worth knowing.
* **The per-frame cost of a flag read is small and already handled.** Only 108
  of 606 flags are read anywhere inside a `for`/`while` body, 62 of those in
  `FDS/RENDER|FILLERS|FRUSTRUM`, and **zero** of the 183 flag reads in
  `DeferredSurfaceKernel.cpp` or the 5 in `Mekalele.cpp` are in a loop — the hot
  kernels hoist every flag to a `const bool` at function entry. Deleting flags
  buys binary size and reading time, essentially not frame time. The one row
  that says otherwise is `--greets_displace_offscreen_skip`, whose own help
  records that *"a runtime flag read inside that `-ffp-contract=fast` function
  is not byte-null even when never taken"* (docs/VISIBILITY_PLAN.md §8).

---

## 1. Method, and what it cannot see

`tools/flag_audit.py`, one file, five passes:

1. **Parse.** Every `FDS_FLAG_{BOOL,FLOAT,INT}` row → name, env var, default,
   category, help text, byte length, `#if FDS_DEV` state, and which of
   `REFUTED / refuted / superseded / instrument / A/B only / his eye /
   default OFF / CANDIDATE / byte-* / DEAD / no longer` the text contains.
2. **Reference count**, outside the `.def`, over `DEMO/ FDS/ tools/`, in four
   spellings — because the tree uses four:
   `FeatureFlags::<name>(` / `FF::<name>(` (the generated accessor),
   `(Bool|Float|Int)Id::<name>` (`get`/`isSet`/`setDefault`),
   `"<name>"` as a C string (`findBoolByCliName`, `setParamFromText`), and
   `"--<name>"` in a raw-argv scan (`REV.CPP`'s pre-parse `chdir` check).
   Comments and block comments are blanked before matching, so a mention in
   prose is never counted as a use. **A first pass that grepped only
   `FeatureFlags::<name>(` reported 50 unwired flags; 48 of them were false.
   The number below is 2.**
3. **Git.** One `git log -p --unified=0 -- FeatureFlags.def` over the 388
   non-merge commits that touched the file (410 including merges), from which each flag's row gets: introduction,
   last touch, every commit that changed its DEFAULT, and the full commit
   message (scanned for `byte-exact|byte-identical|byte-null|md5|bit-exact`).
4. **Loop depth.** A brace-depth walk with comments and string literals blanked,
   giving the number of enclosing `for`/`while`/`do` bodies at each reference.
   Validated against a synthetic fixture. **Known under-count:** a brace-less
   single-statement loop body (`for (...) f(FF::x());`) reads as depth 0. It
   never over-counts.
5. **Ledger.** `groundwork export --json` (118 records, `GROUNDWORK_WRITER=
   subagent:flagaudit`, read-only), matched on flag names in subject + claim.
   12 flags carry a ledger record.

**Hand adjudication.** Seven rows defeat the mechanical rule because the
byte-equality phrase in their help is not a claim about *their* two arms. They
are listed with the sentence that forced the call, in `MANUAL_OVERRIDES` at the
top of the script, and every one of them moved **out** of a DELETE bucket, never
into one.

**What the audit cannot see:** whether a scene *needs* a flag's OFF arm for a
future investigation, whether a look the owner has accepted might be reverted,
and anything about `.claude/worktrees/*` or `worktree-mech-spots/` (excluded).

---

## 2. DELETE-UNWIRED — 2 flags, zero code references

Both were orphaned when the code they gated was replaced; both still advertise
behaviour they no longer have.

| flag | default | .def line | last touch | what happened |
|---|---|--:|---|---|
| `vol_rect_cull` | `1` | 298 | 2026-05-19 `e26b0142` | Its help promises *"Default on; `--no-vol_rect_cull` disables for A/B"*. There is no reader. `FDS/RENDER/DeferredVolumetric.cpp:1155` says why: *"(per-batch rect-cull experiment was reverted —"*. The surviving screen-rect cull is `--cone_hull_rect` (`DeferredVolumetric.cpp:3107`), a different flag. The only tree-wide mentions are `docs/PERF_STATE.md:5760` and the same line in `worktree-mech-spots/`. |
| `water_ripple_scale` | `1.0f` | 420 | row added 2026-06-19 `3280f7b3` | Introduced with `--water_ripple` in `3280f7b3`; its reader was deleted the next day by `58e3b610` ("city: coherent water — one shared wave field"), which did **not** touch the `.def` — the row has been orphaned ever since. Its three siblings `water_ripple`, `water_ripple_amp`, `water_ripple_speed` are all still read (`CITY.CPP:3813/3590/3591`, `ProceduralWater.cpp:836/1000`); only `_scale` is not. Zero mentions anywhere in the tree outside the `.def`. |

**Recipe.** Delete the two `.def` lines (298, 420). Nothing else references
either name in code. Update `docs/PERF_STATE.md:5760` (a row in a flag table)
for `vol_rect_cull`. No call site to touch, no doc to touch for
`water_ripple_scale`. **This is byte-null by construction: neither flag is
read.**

---

## 3. DELETE-REFUTED — 3 flags, default OFF, refuted in the ledger and in the text

The tree already does this: `greets_displace_crease_normals`,
`greets_displace_joint_snap`, `greets_displace_joint_split`,
`greets_displace_env_clamp`, `greets_displace_edge_vert_merge`,
`greets_displace_tsplit_shared` and `greets_displace_tsplit_all_faces` are all
named by ledger refutations and are all **already gone** from the `.def`. These
three are the same class, still present.

| flag | .def line | reader | ledger | the refutation | instrument value it still claims |
|---|--:|---|---|---|---|
| `refl_skip_rain` | 190 | `FDS/RENDER/RENDER.CPP:1412` | `da3b5399de80` | *"rain@refl and rain@main are 0.0000 ms at chase t=800 AND t=1105, and the flag is BYTE-IDENTICAL to base at both poses — chase has no rain armed, so this saves zero milliseconds and moves zero pixels."* | **None.** It is inert at every pose it was built for. |
| `refl_skip_post` | 189 | `FDS/RENDER/RENDER.CPP:465` | `7db03051cf38` | *"the reflection pass's WHOLE post chain is … 1.27 ms, the flag recovers only 0.66–0.94 ms of tick, and it costs 88.4 % of the frame at max 181/255"* — worst saving-to-damage ratio on the ladder by two orders of magnitude. Its own text ends *"REFUTED AS A PERF ITEM by 00m and it should not be on the menu"*. | **None**, and `--refl_skip_vol`'s help says it *"supersedes the three flags above"* anyway. |
| `mirror_mask_pool_clear` | 507 | `FDS/RENDER/GreetsMirror.cpp:4236` | `5e72dd2e4e6c` + `2dab7c032afc` | *"serial 0.478–0.481 ms, pooled 0.617–0.639 ms — +33 % WORSE, reproducibly"*, four interleaved rounds. | It claims one: its help opens *"[REFUTED, DEFAULT OFF — kept as the arm that proves it.]"*. **That value is now redundant** — the measurement is in the ledger twice and the mechanism is `perf.law.L1_fanout_threshold` (`d5f079501e3c`), which cites this round as one of its four sightings. |

**Recipe.** For each: remove the `.def` line; at the single call site, delete the
`if (…)` arm the flag guards and keep the unflagged path (all three default OFF,
so the kept path is today's shipping path). Then:

* `refl_skip_rain` / `refl_skip_post` — `RENDER.CPP:1412` / `:465`. Update
  `docs/OPTIMIZATION_BACKLOG.md` and `docs/PERF_STATE.md` (§00m carries the
  ladder; leave the *measurements*, drop the flag names from the "available
  flags" rows). The remaining ladder is `refl_skip_ssao`, `refl_skip_cones`,
  `refl_skip_vol` — all three DECIDE-LOOK below.
* `mirror_mask_pool_clear` — `GreetsMirror.cpp:4236`; keep the serial
  `std::memset`. Update `docs/OPTIMIZATION_BACKLOG.md`, `docs/PERF_STATE.md`.

**Not in this bucket, and why.** `greets_displace_free_edge` matches a ledger
refutation (`10994f6ef014`) but that record refutes the *vertex-coincidence
test* inside the flag — a sub-method that was then replaced ("the veto now
probes a face soup within 0.05 u"). The flag itself is live, has 5 references
and a `setDefault`, and carries the owner's own open note. It is **DECIDE-LOOK**.
`greets_displace_cpb` matches `558b45ea139b`, which refutes it *as a wall
lever* only; it is a live density tunable (`KEEP-TUNABLE`).

---

## 4. DELETE-LANDED-AB — 23 flags, default ON ≥ 2 weeks, `--no-` arm proven

Two sub-classes, and **they are not the same decision.**

### 4A — arms are byte-equal (8 flags): the OFF arm is provably dead weight

For these, ON and OFF produce identical pixels. The `--no-` arm buys exactly one
thing: the ability to bisect a future regression to this lever. Nothing else.

| flag | .def line | ON since | call sites | the proof, quoted from its own row |
|---|--:|---|---|---|
| `cone_fine_tiles` | 30 | 2026-06-18 `d9073e71` | `DeferredVolumetric.cpp:3057`, plus a redundant `setDefault(true)` at `GREETS.CPP:1347` | *"Same per-pixel work (byte-identical output — verified on conetest)"* |
| `vertex_light_parallel` | 81 | 2026-07-03 `d2c43056` | `Lighting.cpp:439` | *"writes are per-mesh Verts → disjoint; math untouched → byte-identical to serial"* |
| `tile_bbox_cull` | 105 | 2026-08-02 `9b6d70de` | `CITY.CPP:487`, `CHASE.CPP:294`, `RENDER.CPP:664`, `Shadows.cpp:919`, `Transform.cpp:2805`, `RenderInner.cpp:104`, `:260` | *"PURE reject (the clipper already clips to the tile → byte-identical output; near-plane-straddling faces keep the cover-all sentinel and are never rejected)"* |
| `xfrm_soa_inline` | 144 | 2026-08-05 `a1f89d43` | `Transform.cpp:1559` | *"BIT-EXACT BY CONSTRUCTION … city pin 37e62845 exact, fountain pin 51fff7cd exact, greets t=1588 and chase 5+2 poses byte-identical off-vs-on, --soa-verify clean on all four scenes"* |
| `vol_cone_lane_vec` | 766 | 2026-08-13 `03ef0ff0` | `DeferredVolumetric.cpp:897` | *"IT IS BIT-EXACT … OFF restores both scalar loops verbatim and is byte-identical"* |
| `xpar_strip_extent` | 115 | 2026-08-14 `b502c394` | `DeferredSurfaceKernel.cpp:5587` | *"BYTE-NULL by construction: the clear happens over exactly the columns the previous raster dirtied"* |
| `xpar_peel_early_out` | 116 | 2026-08-14 `b502c394` | `DeferredSurfaceKernel.cpp:5633` | *"BYTE-NULL by construction: … the two halves of that conjunction cannot both hold"* |
| `deferred_tile_sphere_cull` | 31 | 2026-08-14 `43ac3456` | `DeferredLightLists.cpp:359` | *"BYTE-NULL: it can only drop a (tile × light) pair for which every pixel of the tile is farther from the light than its cull range"* |

`tile_bbox_cull` is the only one with in-loop reads (3: `CITY.CPP:487`,
`CHASE.CPP:294`, `Transform.cpp:2805`), and all three are the documented
read-once hoist at the top of a per-object loop, not a per-pixel read.

**Recipe, per flag.** (a) Delete the `.def` line. (b) At each call site, inline
the ON branch: `const bool x = FeatureFlags::foo();` → delete the declaration and
constant-fold `x` to `true` (for `cone_fine_tiles`, `numTilesX/Y` become
`DEFERRED_NUM_TILES_X/Y`; for `vertex_light_parallel`, drop the `if` and its
`else` serial walk; for `xpar_strip_extent`, `extentOn` becomes the bounds test
alone). (c) `cone_fine_tiles` additionally needs `GREETS.CPP:1347`'s
`setDefault(FF::BoolId::cone_fine_tiles, true)` deleted — it is already
redundant. (d) Docs naming these by flag name, to update: `docs/PERF_STATE.md`,
`docs/OPTIMIZATION_BACKLOG.md`, `docs/SESSION_STATE.md` (all eight),
`docs/SOA_VERTEX_REFACTOR.md` + `docs/VISIBILITY_PLAN.md` (`xfrm_soa_inline`,
`tile_bbox_cull`), `docs/FRAME_PIPELINE_PLAN.md` (`vertex_light_parallel`),
`docs/HW_PROFILING.md` (`vol_cone_lane_vec`). (e) Gate: the change is a
compile-time constant fold of a proven-equal predicate, so the standing pin
battery must come back byte-identical; if it does not, the premise was wrong and
the flag stays.

### 4B — landed LOOK fix (15 flags): OFF is the LEGACY path, not a byte-equal twin

These satisfy the same rule — default ON for ≥ 2 weeks, the `--no-` arm proven
byte-null **against the pre-flag baseline** — but ON and OFF do *not* render the
same frame. The OFF arm is a revert hatch to the old look. **Deleting these is a
different call and it is yours, not mine.** I list them because they are the
single largest block of "flag whose decision is already made", 27 KB of text.

Nine of them carry **stale help text**: they still say *"MEASUREMENT/FIX
CANDIDATE, default OFF = byte-null … left OFF pending the user's own review"*
while their `.def` default is `1`. The flip is not accidental — `bd6e8060`
(2026-08-09) opens *"The user overrode my recommendations to leave these off and
asked for all of them defaulted"*, and `17823518` the same day landed four more
as "measured correctness fixes". **Whatever is decided about deletion, that
sentence is now false in nine rows and should go.**

| flag | .def line | ON since | reader | note |
|---|--:|---|---|---|
| `env_cube` | 177 | 2026-07-04 `75f51f04` | `CITY.CPP:2777`, `Transform.cpp:2944`, `EnvBake.cpp:909`, `:1398` | OFF restores the equirect panorama path bit-identically. `docs/ENV_CUBEMAP_PLAN.md`, `docs/GRAPHICS_PIPELINE.md` |
| `greets_displace_seam_union` | 482 | 2026-08-02 `c256c038` | `MeshOps.cpp:3798` (+2 name-strings in `DisplaceTest.cpp`) | OFF *"reproduces the pre-fix holey heal for A/B"* |
| `mip_fix` | 599 | 2026-08-08 `b8319e10` | `CITY.CPP:2854`, `FRUSTRUM.CPP:794` | header still says *"default 0 = OFF, byte-null"* — **stale**. `docs/SIDECAR_MIGRATION_PLAN.md` |
| `env_bake_linear` | 633 | 2026-08-09 `bd6e8060` | `CITY.CPP:2857`, `EnvBake.cpp:569/987/2424` | stale "pending review". `docs/SHADING_CONTRACT.md` §8.1, `docs/GPU_BENCHMARK_PLAN.md` |
| `sh_bake_linear` | 650 | 2026-08-09 `bd6e8060` | `EnvBake.cpp:3877` | stale. `docs/SHADING_CONTRACT.md` |
| `metal_spec_f0` | 646 | 2026-08-09 `bd6e8060` | `DeferredSurfaceKernel.cpp:2571` | stale. `docs/SHADING_CONTRACT.md` |
| `env_bake_sh_first` | 656 | 2026-08-09 `bd6e8060` | `EnvBake.cpp:1626` | stale. `docs/SHADING_CONTRACT.md` |
| `greets_shatter_screen_mat_only` | 671 | 2026-08-09 `bd6e8060` | `GREETS.CPP:915`, `:944` | *"Default ON = the repair"* |
| `env_metal_tint_linear` | 649 | 2026-08-09 `17823518` | `DeferredSurfaceKernel.cpp:1153` | stale. `docs/SHADING_CONTRACT.md` §8.2 row S-d |
| `shadow_noncaster_depth` | 651 | 2026-08-09 `17823518` | `DeferredSurfaceKernel.cpp:2588` | stale. `docs/SHADING_CONTRACT.md` |
| `deferred_checker_env_full` | 661 | 2026-08-09 `17823518` | `DeferredSurfaceKernel.cpp:2453/6205/7462` | *"odd-even bias +6.82 → +0.02 luma"*. `docs/SHADING_CONTRACT.md` |
| `env_bake_include_animated` | 659 | 2026-08-09 `17823518` | `Transform.cpp:1441` | `docs/SHADING_CONTRACT.md` §11 row E6 — **and `ece0dc27` documents a live interaction with `--env_probe_follow_owner`**; read that before removing the OFF arm |
| `mirror_flare_bbox` | 674 | 2026-08-09 `704a5a89` | `FILLERS.CPP:1493` | fixes flares popping at mirror edges; text records one part as *"NOT ADDRESSED (measured, stated, unfixed)"* |
| `env_dyn_static_exclude` | 720 | 2026-08-12 `b4d9670c` | `EnvBake.cpp:1596` | *"A/B KNOB, default 1 = the fix"*; only bites under `--env_dynamic` (greets) |
| `greets_displace_groove_shade` | 780 | 2026-08-14 `d9cc81d9` | `MeshOps.cpp:5731`, `:6887` | *"Default ON (the carved look is wrong without it)"* |

**Recipe (if you take them).** Same three steps as 4A, but the gate is different:
inlining the ON branch changes **no pixel** (ON is already the default), so the
pin battery must be byte-identical — that part is mechanical. What you lose is
the revert, and **`docs/SHADING_CONTRACT.md`'s ladder cites eight of these rows
by flag name**; that document has to be rewritten to say "shipped, no dial"
rather than pointing at a flag. My recommendation: **do the stale-text fix now
(9 rows), take the deletions separately.**

---

## 5. DECIDE-LOOK — 10 flags, default OFF, priced, waiting on your eye

Not deletions. One line each, numbers quoted from the flag's own text or from
the ledger record named.

| flag | what turning it on buys | what it costs, measured |
|---|---|---|
| `cone_half_y_wide` | −6.5 ms, 46.7 % of the cone pass (ledger `f8af05b52f8a`) | 0.37 % of pixels move, max **5/255**. Fires on every city tile (46 wide headlights, 0 narrow cones); chase/greets keep full rate. |
| `water_glints_batch` | up to **−11 % of a chase tick** (ledger `1c066fdb911e`) | pixels move by **1 LSB**; residue localised to the slope kernel (53 px), not the caustic tap. |
| `refl_skip_cones` | **−6.41 ms** at chase t=800 (12.8 % of the tick), −4.26 ms at t=1105 (9.3 %) | 15.1 % of px move, mean 0.91, **max 6/255**; 71.6 % of the movement is in the SKY — not reflection-only. Its own text: *"Judge from the crops, never from the ms."* |
| `refl_skip_ssao` | −3.67 ms (ledger `a3c301c57c90`) | **max 119/255**, but **0 px above the horizon** at chase t=800. The ms column and the eye rank this and `refl_skip_cones` in *opposite* orders — that is the whole open question. |
| `refl_skip_vol` | the wholesale switch (`skipVolumetric=true`, what city already does) | *"the top of the ladder and the biggest look change in it"* — supersedes `refl_skip_ssao/cones/post`. |
| `light_rect_exact` | fixes `bug.lightSphereScreenRect.drops_light` (ledger `942388b53d1c`) — the small-angle screen rect is not conservative and drops lights | 6 of 39 swept poses change, always brighter, **max 5/255** in tile-boundary slivers. Correctness switch, priced, awaiting a call. |
| `mip_aniso` | max-axis mip metric; consecutive-frame mean\|d\| **14.048 → 13.749** over a 13-step dolly (−2.1 %), bit-identical on walls it does not target | +0.12 % renderFrame instructions; distant-surface softening with no aniso-tap filler to win the detail back — *"a look decision"*. Evidence pairs in `docs/img/mipaniso/`. |
| `greets_shadow_proxy` | frame **67.95 → 62.05 ms**, BAKE 9.18 → 3.45 ms at t=5780 | byte-identical at 5 of 16 review poses; elsewhere 1 282–110 668 px > 4/255, worst t=6097 (58 021 px > 12/255, max 144, at the corner junction). ~1 % of px shift by the displacement offset. |
| `greets_displace_free_edge` | relief on free silhouette edges (doorway jambs, arch reveals) that the border pin currently flattens | your own 2026-08-12 note: *"makes most of the sites better, but for the specific pose I sent you, it adds a bulge similar to the gpu one"*. Ledger `10994f6ef014` refutes only the vertex-coincidence test inside it, which was replaced. |
| `greets_displace_border_v2` | the TESSELLATION half of the stone border rework; **148 px** residue with `--greets_displace_profile_agree` mode 1 | 901 px without profile_agree — *"neither is worth having alone"*, and profile_agree *"is still an unjudged look call"*. |

---

## 6. UNSURE — 25 flags, and the reason for each

**(a) Age hold — 18 flags.** Byte-equal A/B hatches whose default went ON less
than two weeks ago. Same class as §4A, wrong side of the clock:

* **13 days** (landed 2026-08-16, clears the rule on 2026-08-30):
  `deferred_lm_addr_skip`, `deferred_cube_direct`, `deferred_cube_prepass`,
  `deferred_fill_hdr_skip`, `deferred_fill_ldr_skip`, `deferred_fill_oct_pair`,
  `deferred_shade_ldr_skip`, `needle_cull`, `face_tile_bin`,
  `fog_composite_tile_align8`, `water_slope_vec8`, `shadow_bbox_cull`,
  `greets_displace_offscreen_skip`. All thirteen carry an explicit
  byte-null/bit-exact-by-construction argument. `deferred_fill_oct_pair` is the
  strongest of them: its own text says *"The SHIPPING form is FLAGLESS … the
  hatch buys no speed, the change is bit-exact so it needs no look dial, and the
  only thing the dial can do is cost +0.7 to +1.1 % of the fill."*
* **12 days:** `refl_correct` (2026-08-17). Also the largest row in the table
  (7 208 B) and the one the ledger has both a measurement and an open proposal
  against (`plan.city.refl_correct_cost_cut`) — hold.
* **2 days:** `ssao_hdr_transport` (2026-08-27).
* **1 day:** `deferred_ovec_light_skip`, `deferred_ovec_nomirror`,
  `deferred_ovec_mat_uniform`, `deferred_ovec_vec_pack` (2026-08-28).
* **0 days:** `fog_refl_vec`, `city_glass_pool`, `mirror_rtt_pool` (today).

**(b) Hand-adjudicated out of a DELETE bucket — 3 flags.**

* `pom_shell_base_clip` — its byte claim is *"Inert without `--pom_shell`, so
  flags-off is byte-identical"*: a statement about the **parent gate**
  (`--pom_shell`, default 0, never set by any scene), not about its own arms.
  Live A/B in `docs/S1D_CLOSED_SHELL_PLAN.md`.
* `pom_prism_flat` — same shape, parent `--pom_prism` (default 0).
* `greets_displace_border_pin` — its own text: *"it PRICES the crack safety, it
  is not a fix"*. An explicit pricing knob for live research.

**(c) Everything else — 4 flags.** `pom_shell_side_entry`, `pom_prism_march`,
`greets_displace_plane_normal` were flagged by the "superseded / no longer"
heuristic; reading them shows the supersede language names an earlier *variant*
inside the same research thread, so they were hand-moved to KEEP-TUNABLE.
`viz_legend` matched the landed-AB rule on *"Byte-null with every viz off"* — a
statement about the vizzes, not about its arms — and was hand-moved to
KEEP-DEBUG.

---

## 7. KEEP — 543 flags

**KEEP-DEBUG, 76 flags, 56 177 B.** Instruments: rows whose help opens with a
bracketed `[INSTRUMENT] / [VIZ] / [DEBUG] / [CENSUS]` tag, or whose name carries
`viz|dump|census|stats|verify|hash|log|prof|trace|report|assert`. Only four of
the 76 are driven by a `tools/` script today, and those four are the ones a
deletion would actually break: `--ssao_dump` (`tools/ssao_ao_compare.py`,
`tools/ssao_gtao_battery.sh`), `--hw_prof` (`tools/ovec/ab.sh`, `ab2.sh`,
`ab3.sh`, `gi.sh`, `ssab.sh`), `--deferred_prof` (`tools/ovec/ab{,2,3}.sh`) and
`--profiler` (`tools/flip_rate.sh`, `tools/ovec/gates.sh` — classified
KEEP-TUNABLE, it is seeded from `rev.cfg`). The gate scripts
`tools/render_gate.sh` and `tools/warm_gate.sh` drive scene/look flags rather
than instruments (`--draw_cones`, `--mirror_rtt`, `--shard_deferred`,
`--fast_fog*`, `--fog_refl_vec`, `--city_env_pixel`, `--env_live_water`), and
`tools/editor_server.py` drives `--surf_smoothing_authored`, `--nmap_16bit`,
`--water_procedural` — all KEEP-TUNABLE. Two rows worth noting rather than
cutting: `--ssao_dump` carries a ledger trap
(`perf.trap.T7_ssao_dump_inflates` — it inflates the pass it measures ~3.5×, a
correctness instrument and never a perf one), and `--shadow_tap_census` is
labelled `DEAD` in the keyword scan only because its *column heading* is
"DEAD entries"; it is a live ceiling measurement for two parked early-outs.

**KEEP-TUNABLE, 467 flags, 291 444 B.** Live numeric tunables, scene controls
and A/B dials that are read. This is where the bulk sits, and where the honest
answer to "cut some out" is *not by this audit's rules*: `parallax` (122) and
`greets` (87) are the displacement/S1d working set. If you want a second cull
pass, the mechanical handle that would find them is **"gated behind a parent
whose default is 0 and which no scene ever sets"** — `--pom_shell`,
`--pom_prism`, `--pom_ref_march`, `--env_dynamic` and `--greets_displace` are
all operator-only today (verified: none appears in any of the 115 `setDefault`
call sites). That is a research-surface question, not a dead-code one, so I did
not act on it.

---

## 8. Appendix — every flag, one row each

`bucket` carries the `-A` / `-B` sub-class for DELETE-LANDED-AB. `in-loop` is
the count of code references inside a `for`/`while` body, with up to two sites
as `file:line(Ldepth)`. `refs` counts code references outside the `.def` in all
four spellings. Regenerate with `--md`.


| flag | type | default | bucket | refs | in-loop | last touch | evidence |
|---|---|---|---|--:|--:|---|---|
| `vol_rect_cull` | BOOL | `1` | DELETE-UNWIRED | 0 | — | 2026-05-19 e26b0142 | 0 code refs in DEMO/ FDS/ tools/ (accessor, *Id::, name-string, argv-string) |
| `water_ripple_scale` | FLOAT | `1.0f` | DELETE-UNWIRED | 0 | — | 2026-06-19 3280f7b3 | 0 code refs in DEMO/ FDS/ tools/ (accessor, *Id::, name-string, argv-string) |
| `refl_skip_post` | BOOL | `0` | DELETE-REFUTED | 1 | — | 2026-08-29 1770bf51 | help text says REFUTED · ledger 7db03051cf38 (flag.refl_skip_post) · default is the no-op value |
| `refl_skip_rain` | BOOL | `0` | DELETE-REFUTED | 1 | — | 2026-08-29 1770bf51 | help text says REFUTED · ledger da3b5399de80 (flag.refl_skip_rain) · default is the no-op value |
| `mirror_mask_pool_clear` | BOOL | `0` | DELETE-REFUTED | 1 | — | 2026-08-29 9dcee857 | help text says REFUTED · ledger 5e72dd2e4e6c (flag.mirror_mask_pool_clear) · default is the no-op value |
| `cone_fine_tiles` | BOOL | `1` | DELETE-LANDED-AB-A | 2 | 1 FDS/RENDER/DeferredVolumetric.cpp:3057(L1) | 2026-06-18 d9073e71 | default ON since 2026-06-18 (d9073e71) · help asserts the arms are byte/bit-equal: tiles) instead of the coarse 6x4 (24). Same per-pixel work (byte-identical output — verified on conetest), but the cone-heavy tiles (disco/beam region) spread across 4x more work-stealing tasks  |
| `deferred_tile_sphere_cull` | BOOL | `1` | DELETE-LANDED-AB-A | 1 | — | 2026-08-14 43ac3456 | default ON since 2026-08-14 (43ac3456) · help asserts the arms are byte/bit-equal: ists, on top of the separable screen-rect + z-extent tests. BYTE-NULL: it can only drop a (tile x light) pair for which every pixel of the tile is farther from the light than its cull range |
| `vertex_light_parallel` | BOOL | `1` | DELETE-LANDED-AB-A | 1 | — | 2026-07-03 d2c43056 | default ON since 2026-07-03 (d2c43056) · help asserts the arms are byte/bit-equal: h (writes are per-mesh Verts -> disjoint; math untouched -> byte-identical to serial). The pool is parked during the scene tick, so this is free parallelism (~1 ms serial on greets). --no-vertex |
| `tile_bbox_cull` | BOOL | `1` | DELETE-LANDED-AB-A | 7 | 3 DEMO/CITY.CPP:487(L1) DEMO/CHASE.CPP:294(L1) | 2026-08-02 9b6d70de | default ON since 2026-08-02 (9b6d70de) · help asserts the arms are byte/bit-equal: ntry). PURE reject (the clipper already clips to the tile → byte-identical output; near-plane-straddling faces keep the cover-all sentinel and are never rejected). Default ON; --no-tile_bbox_cul |
| `xpar_strip_extent` | BOOL | `1` | DELETE-LANDED-AB-A | 1 | — | 2026-08-14 b502c394 | default ON since 2026-08-14 (b502c394) · help asserts the arms are byte/bit-equal: lump's own raster touched, instead of the full strip width. BYTE-NULL by construction: the clear happens over exactly the columns the previous raster dirtied (every other column is already  |
| `xpar_peel_early_out` | BOOL | `1` | DELETE-LANDED-AB-A | 1 | — | 2026-08-14 b502c394 | default ON since 2026-08-14 (b502c394) · help asserts the arms are byte/bit-equal:  depth peel as soon as a pass leaves its layer Z untouched. BYTE-NULL by construction: pass N's accept mask is `(z_candidate < z_existing) & (z_candidate > peelFloor)` with z_existing pre-c |
| `xfrm_soa_inline` | BOOL | `1` | DELETE-LANDED-AB-A | 1 | — | 2026-08-05 6470fd14 | default ON since 2026-08-05 (a1f89d43) · help asserts the arms are byte/bit-equal:  a SECOND time and re-reads 16 of its 136 bytes per vertex. BIT-EXACT BY CONSTRUCTION - same values from the same source, stored one loop earlier; the VertexFrame's contents are identical,  |
| `env_cube` | BOOL | `1` | DELETE-LANDED-AB-B | 4 | 1 FDS/RENDER/Transform.cpp:2944(L2) | 2026-07-04 75f51f04 | default ON since 2026-07-04 (75f51f04) · help asserts the arms are byte/bit-equal:  Default ON since slice D (validated: gates + flag-off pins byte-identical, no seams, bench <= equirect); --no-env_cube restores the equirect path bit-identical. Set FDS_ENV_CUBE_SELFTEST=1 to r |
| `greets_displace_seam_union` | BOOL | `1` | DELETE-LANDED-AB-B | 3 | 1 DEMO/MeshOps.cpp:3798(L1) | 2026-08-02 c256c038 | default ON since 2026-08-02 (c256c038) · help asserts the arms are byte/bit-equal: oundaries) are untouched (adaptive-level tessellation stays byte-identical). Default ON; --no-greets_displace_seam_union reproduces the pre-fix holey heal for A/B. Only read when --greets_displa |
| `mip_fix` | BOOL | `1` | DELETE-LANDED-AB-B | 2 | — | 2026-08-08 daeb147c | default ON since 2026-08-08 (b8319e10) · help asserts the arms are byte/bit-equal: plevelClipper (FDS/FRUSTRUM/FRUSTRUM.CPP), default 0 = OFF, byte-null. The clipper picks a mip two different ways and THE TWO DISAGREE. Small faces (screen area < XRes*YRes*0.02) take a who |
| `env_bake_linear` | BOOL | `1` | DELETE-LANDED-AB-B | 4 | — | 2026-08-09 bd6e8060 | default flipped ON 2026-08-09 (bd6e8060) — landed LOOK fix · help still says the flag is parked OFF: STALE TEXT |
| `metal_spec_f0` | BOOL | `1` | DELETE-LANDED-AB-B | 1 | — | 2026-08-09 bd6e8060 | default flipped ON 2026-08-09 (bd6e8060) — landed LOOK fix · help still says the flag is parked OFF: STALE TEXT |
| `env_metal_tint_linear` | BOOL | `1` | DELETE-LANDED-AB-B | 1 | — | 2026-08-09 17823518 | default flipped ON 2026-08-09 (17823518) — landed LOOK fix · help still says the flag is parked OFF: STALE TEXT |
| `sh_bake_linear` | BOOL | `1` | DELETE-LANDED-AB-B | 1 | — | 2026-08-09 bd6e8060 | default flipped ON 2026-08-09 (bd6e8060) — landed LOOK fix · help still says the flag is parked OFF: STALE TEXT |
| `shadow_noncaster_depth` | BOOL | `1` | DELETE-LANDED-AB-B | 1 | — | 2026-08-09 17823518 | default flipped ON 2026-08-09 (17823518) — landed LOOK fix · help still says the flag is parked OFF: STALE TEXT |
| `env_bake_sh_first` | BOOL | `1` | DELETE-LANDED-AB-B | 1 | — | 2026-08-09 bd6e8060 | default ON since 2026-08-09 (bd6e8060) · help asserts the arms are byte/bit-equal: >Ambient constant. MEASUREMENT/FIX CANDIDATE, default OFF = byte-null; inert unless BOTH --sh_ambient and --env_refl are on. THE DEFECT, and it is an ORDERING one: renderFrame triggers EnvR |
| `env_bake_include_animated` | BOOL | `1` | DELETE-LANDED-AB-B | 1 | — | 2026-08-11 ece0dc27 | default ON since 2026-08-09 (17823518) · help asserts the arms are byte/bit-equal: tion probe bakes. MEASUREMENT/LOOK CANDIDATE, default OFF = byte-null. THE DIVERGENCE (docs/SHADING_CONTRACT.md 11 row E6): EnvBake.cpp's renderSixFaces sets g_envBakeSkipDynamic for every  |
| `deferred_checker_env_full` | BOOL | `1` | DELETE-LANDED-AB-B | 3 | — | 2026-08-09 17823518 | default ON since 2026-08-09 (17823518) · help asserts the arms are byte/bit-equal:  the wave-2 fill's full-shade fallback do it. Default OFF = byte-null. THE DEFECT: the fill already refuses to AVERAGE an env-reflective pixel (`envForceFull`, DeferredSurfaceKernel.cpp:500 |
| `greets_shatter_screen_mat_only` | BOOL | `1` | DELETE-LANDED-AB-B | 2 | 1 DEMO/GREETS.CPP:915(L2) | 2026-08-09 bd6e8060 | default ON since 2026-08-09 (bd6e8060) · help asserts the arms are byte/bit-equal: 50 of them matched by rule (B)'s material test), so this is byte-null everywhere the shatter is not triggered and byte-null in the flat arm even when it is. Default ON = the repair; --no-gr |
| `mirror_flare_bbox` | BOOL | `1` | DELETE-LANDED-AB-B | 1 | — | 2026-08-09 704a5a89 | default ON since 2026-08-09 (704a5a89) · help asserts the arms are byte/bit-equal: ical Spriter instantiation with a null mask pointer and are byte-identical; so is every scene with no mirrors. NOT ADDRESSED (measured, stated, unfixed): Transform.cpp's omni loop still admits a |
| `env_dyn_static_exclude` | BOOL | `1` | DELETE-LANDED-AB-B | 1 | — | 2026-08-12 b4d9670c | default ON since 2026-08-12 (b4d9670c) · help asserts the arms are byte/bit-equal: they are slabs) — that one holds even with the overlay off. BYTE-NULL where it cannot bite: ANDed with the store's own EnvDynamic retention flag AND with --env_dynamic, which is compile-def |
| `vol_cone_lane_vec` | BOOL | `1` | DELETE-LANDED-AB-A | 1 | — | 2026-08-13 03ef0ff0 | default ON since 2026-08-13 (03ef0ff0) · help asserts the arms are byte/bit-equal: R once per BATCH, so the composite loop is untouched. IT IS BIT-EXACT, and unlike the solve it cost nothing to make so: neither loop contains an a*b+c that -ffp-contract=fast could fuse amb |
| `greets_displace_groove_shade` | BOOL | `1` | DELETE-LANDED-AB-B | 2 | 1 DEMO/MeshOps.cpp:5731(L1) | 2026-08-14 d9cc81d9 | default ON since 2026-08-14 (d9cc81d9) · help asserts the arms are byte/bit-equal: ace (no bake, no records); flag-off-displaced and flat arms byte-identical. |
| `refl_skip_ssao` | BOOL | `0` | DECIDE-LOOK | 1 | — | 2026-08-29 1770bf51 | PERF LADDER for the MIRRORED WATER-REFLECTION UNDERLAY pass, chase only today (DEFAULT 0 = OFF = byte-identical; the whole ladder is a MENU for the owner's eye, not a shipped change). chase calls a bare Render() for its reflection pass (DEMO/CHASE.CPP), i.e. skipVolumetric=false, where CITY.CPP passes skipVolumetric=t |
| `refl_skip_cones` | BOOL | `0` | DECIDE-LOOK | 1 | — | 2026-08-29 1770bf51 | PERF LADDER for the MIRRORED WATER-REFLECTION UNDERLAY pass, chase only today (DEFAULT 0 = OFF = byte-identical; the whole ladder is a MENU for the owner's eye, not a shipped change). chase calls a bare Render() for its reflection pass (DEMO/CHASE.CPP), i.e. skipVolumetric=false, where CITY.CPP passes skipVolumetric=t · ledger a3c301c57c90 menu.refl_skip_cones_or_ssao: skipping cones (-6.41 ms, ma |
| `refl_skip_vol` | BOOL | `0` | DECIDE-LOOK | 1 | — | 2026-08-29 1770bf51 | PERF LADDER for the MIRRORED WATER-REFLECTION UNDERLAY pass, chase only today (DEFAULT 0 = OFF = byte-identical; the whole ladder is a MENU for the owner's eye, not a shipped change). chase calls a bare Render() for its reflection pass (DEMO/CHASE.CPP), i.e. skipVolumetric=false, where CITY.CPP passes skipVolumetric=t |
| `cone_half_y_wide` | BOOL | `0` | DECIDE-LOOK | 1 | — | 2026-08-28 005a0823 | ledger f8af05b52f8a menu.cone_half_y_wide: half-Y stepping for wide-cone tiles: -6.5 ms (46.7 % of the cone pass) for 0.37 % of pixels at max 5/255 - default OFF, his eye decides. [seeded 2026-08-29 by the coordinator from commit messages / PE |
| `light_rect_exact` | BOOL | `0` | DECIDE-LOOK | 2 | 1 FDS/RENDER/DeferredLightLists.cpp:594(L1) | 2026-08-29 0653da84 | ledger 942388b53d1c menu.light_rect_exact: exact light screen rect (fixes bug.lightSphereScreenRect.drops_light at a cost) - default OFF, priced, awaiting a call. [seeded 2026-08-29 by the coordinator from commit messages / PERF_STATE] |
| `mip_aniso` | BOOL | `0` | DECIDE-LOOK | 2 | — | 2026-08-13 39f1356c | ax-axis is the correct metric for a point sampler and every objective score in the defect region agrees, but there is no aniso-tap filler to win the detail back, so the distant-surface softening is a look decision. Evidence pairs in docs/img/mipaniso/, full write-up in docs/SESSION_STATE.md. Measure with --mip_aniso_stats. |
| `water_glints_batch` | BOOL | `0` | DECIDE-LOOK | 1 | — | 2026-08-29 230d7910 | ledger 1c066fdb911e menu.water_glints_batch: batched water glints: up to -11 % of a chase tick, pixels move by 1 LSB - default OFF, his eye decides. [seeded 2026-08-29 by the coordinator from commit messages / PERF_STATE] |
| `greets_shadow_proxy` | BOOL | `0` | DECIDE-LOOK | 4 | — | 2026-08-06 897e6f57 | invisible), and a flat caster carries no mortar relief so it does NOT restore per-block groove self-shadow. DEFAULT 0 so --greets_displace alone keeps its reviewed look; set 1 to opt into the perf (a look call). Only read when --greets_displace is on. RE-MEASURED 2026-08-06 on the post-799c808 tree (t=5780 wall bench, 1080p, 12 threads, iters=20, 4 interleaved rounds |
| `greets_displace_free_edge` | BOOL | `0` | DECIDE-LOOK | 5 | 1 DEMO/MeshOps.cpp:2831(L1) | 2026-08-13 705b70da | HAND-ADJUDICATED: the ledger refutation 10994f6ef014 is scoped to the VERTEX-COINCIDENCE test inside the flag ('the veto now probes a face soup within 0.05 u'), i.e. a superseded sub-method, NOT the flag; the owner's own 2026-08-12 note ('makes most of the sites better, but ... adds a bulge') is an open look call. |
| `greets_displace_border_v2` | BOOL | `0` | DECIDE-LOOK | 2 | 2 DEMO/MeshOps.cpp:3949(L1) DEMO/MeshOps.cpp:5744(L1) | 2026-08-18 efd0844d | e). v2 WITHOUT profile_agree is 901 px: this flag is the TESSELLATION half and mode 1 is the PROFILE half; neither is worth having alone. NOT DEFAULTED ON: --greets_displace_profile_agree is still an unjudged look call. WHAT IT DOES NOT DO, measured, so nobody re-derives it: the fold relax is still load-bearing -- v2 + mode 1 with --no-greets_displace_fold_relax is 3 878 |
| `deferred_ovec_light_skip` | BOOL | `1` | UNSURE | 1 | — | 2026-08-28 88b4552b | byte-equal A/B hatch but default ON only since 2026-08-28 (< 2 weeks at 2026-08-29) — hold |
| `deferred_ovec_nomirror` | BOOL | `1` | UNSURE | 1 | — | 2026-08-28 a468a279 | byte-equal A/B hatch but default ON only since 2026-08-28 (< 2 weeks at 2026-08-29) — hold |
| `deferred_ovec_mat_uniform` | BOOL | `1` | UNSURE | 1 | — | 2026-08-28 88b4552b | byte-equal A/B hatch but default ON only since 2026-08-28 (< 2 weeks at 2026-08-29) — hold |
| `deferred_ovec_vec_pack` | BOOL | `1` | UNSURE | 1 | — | 2026-08-28 88b4552b | byte-equal A/B hatch but default ON only since 2026-08-28 (< 2 weeks at 2026-08-29) — hold |
| `deferred_lm_addr_skip` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 7cf23fb5 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `deferred_cube_direct` | BOOL | `1` | UNSURE | 2 | — | 2026-08-16 7cf23fb5 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `deferred_cube_prepass` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 66166a58 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `deferred_fill_hdr_skip` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 7cf23fb5 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `deferred_fill_ldr_skip` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 f11f3e0c | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `deferred_fill_oct_pair` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 dc752523 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `deferred_shade_ldr_skip` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 2259c2f2 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `needle_cull` | BOOL | `1` | UNSURE | 3 | 3 DEMO/CITY.CPP:491(L1) DEMO/CHASE.CPP:298(L1) | 2026-08-16 8cc5e5e7 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `face_tile_bin` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 d9dfa527 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `refl_correct` | BOOL | `1` | UNSURE | 3 | 2 DEMO/CITY.CPP:493(L1) DEMO/CHASE.CPP:300(L1) | 2026-08-17 9b3b89db | byte-equal A/B hatch but default ON only since 2026-08-17 (< 2 weeks at 2026-08-29) — hold |
| `pom_shell_base_clip` | BOOL | `1` | UNSURE | 1 | — | 2026-08-04 9d248a13 | HAND-ADJUDICATED: the byte claim is 'Inert without --pom_shell, so flags-off is byte-identical' -- about the PARENT gate (--pom_shell, default 0), not about this flag's arms; live A/B in the S1d closed-shell research. |
| `fog_refl_vec` | BOOL | `1` | UNSURE | 1 | 1 FDS/RENDER/DeferredFastFog.cpp:2618(L1) | 2026-08-29 dce8719f | byte-equal A/B hatch but default ON only since 2026-08-29 (< 2 weeks at 2026-08-29) — hold |
| `fog_composite_tile_align8` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 cb6aad4c | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `water_slope_vec8` | BOOL | `1` | UNSURE | 2 | — | 2026-08-16 eb5e57d9 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `greets_displace_offscreen_skip` | BOOL | `1` | UNSURE | 1 | 1 DEMO/GREETS.CPP:2789(L1) | 2026-08-16 c8cd9310 | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `shadow_bbox_cull` | BOOL | `1` | UNSURE | 1 | — | 2026-08-16 8dde99fd | byte-equal A/B hatch but default ON only since 2026-08-16 (< 2 weeks at 2026-08-29) — hold |
| `city_glass_pool` | BOOL | `1` | UNSURE | 1 | — | 2026-08-29 c15a9cb1 | byte-equal A/B hatch but default ON only since 2026-08-29 (< 2 weeks at 2026-08-29) — hold |
| `mirror_rtt_pool` | BOOL | `1` | UNSURE | 1 | — | 2026-08-29 50325bfb | byte-equal A/B hatch but default ON only since 2026-08-29 (< 2 weeks at 2026-08-29) — hold |
| `ssao_hdr_transport` | BOOL | `1` | UNSURE | 1 | — | 2026-08-27 e017d611 | byte-equal A/B hatch but default ON only since 2026-08-27 (< 2 weeks at 2026-08-29) — hold |
| `pom_prism_flat` | BOOL | `1` | UNSURE | 1 | — | 2026-08-07 c5bf8ec4 | HAND-ADJUDICATED: the byte claim is 'inert without --pom_prism (itself default 0), so shipping stays byte-null' -- about the PARENT gate, not the arms. |
| `greets_displace_border_pin` | BOOL | `1` | UNSURE | 1 | 1 DEMO/MeshOps.cpp:2802(L1) | 2026-08-11 2b61b85f | HAND-ADJUDICATED: its own text: 'it PRICES the crack safety, it is not a fix' -- an explicit pricing knob for the live displacement research, not a landed lever with a dead arm. |
| `deferred_cube_prepass_verify` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-08-16 7d2ae5d2 | instrument tag / name |
| `deferred_gloss_stats` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-14 eda00563 | instrument tag / name |
| `deferred_tile_stats` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-14 eda00563 | instrument tag / name |
| `shadow_tap_census` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-08-14 43ac3456 | instrument tag / name |
| `shadow_tap_census_block` | INT | `0` | KEEP-DEBUG | 1 | — | 2026-08-14 43ac3456 | instrument tag / name |
| `omni_census` | BOOL | `0` | KEEP-DEBUG | 14 | — | 2026-08-15 0b85e5df | instrument tag / name |
| `shadow_prof` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-14 eda00563 | instrument tag / name |
| `shadow_prof_cache` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-05-15 76bc1da0 | instrument tag / name |
| `dump_shadowmap` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-20 c40c5f1a | instrument tag / name |
| `shadow_lightmap_viz` | INT | `0` | KEEP-DEBUG | 7 | — | 2026-05-25 1e711f88 | instrument tag / name |
| `soa_verify` | BOOL | `0` | KEEP-DEBUG | 2 | 2 FDS/RENDER/Transform.cpp:2741(L1) FDS/RENDER/Transform.cpp:2757(L1) | 2026-05-30 44343e1b | instrument tag / name |
| `xpar_extent_census` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-08-15 da286d8b | instrument tag / name |
| `prof_no_tex` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-14 eda00563 | instrument tag / name |
| `prof_no_lights` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-05-14 eda00563 | instrument tag / name |
| `prof_no_spec` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-14 eda00563 | instrument tag / name |
| `prof_no_fog` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-14 eda00563 | instrument tag / name |
| `prof_no_vertex_light` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-16 900bfdc9 | instrument tag / name |
| `prof_no_cube_tap` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-05-29 205b119d | instrument tag / name |
| `xfrm_prof` | INT | `0` | KEEP-DEBUG | 2 | — | 2026-08-05 a1f89d43 | instrument tag / name |
| `xfrm_pass_prof` | INT | `0` | KEEP-DEBUG | 1 | — | 2026-08-05 6ec3da82 | instrument tag / name |
| `mirror_cull_census` | INT | `0` | KEEP-DEBUG | 3 | 1 FDS/RENDER/GreetsMirror.cpp:2483(L1) | 2026-08-05 6ec3da82 | instrument tag / name |
| `mirror_cull_census_cell` | FLOAT | `0.0f` | KEEP-DEBUG | 2 | — | 2026-08-05 6ec3da82 | instrument tag / name |
| `xfrm_pass_mesh_prof` | INT | `0` | KEEP-DEBUG | 1 | — | 2026-08-06 57778d4a | instrument tag / name |
| `nmap_viz` | INT | `0` | KEEP-DEBUG | 7 | — | 2026-07-03 46810cb9 | instrument tag / name |
| `env_refl_viz` | INT | `0` | KEEP-DEBUG | 5 | — | 2026-07-04 35f2afc6 | instrument tag / name |
| `pom_viz` | BOOL | `0` | KEEP-DEBUG | 3 | — | 2026-07-31 15cb7ca5 | instrument tag / name |
| `pom_shell_stats` | BOOL | `0` | KEEP-DEBUG | 1 | 1 FDS/FILLERS/Mekalele.h:4340(L1) | 2026-08-04 c556148a | instrument tag / name |
| `poly_viz` | BOOL | `0` | KEEP-DEBUG | 6 | 1 FDS/FILLERS/Mekalele.h:4273(L1) | 2026-08-04 9d248a13 | instrument tag / name |
| `pom_horizon_viz` | BOOL | `0` | KEEP-DEBUG | 3 | — | 2026-08-04 c86b5bc0 | instrument tag / name |
| `pom_shell_patch_dump` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-04 c86b5bc0 | instrument tag / name |
| `pom_seam_census` | BOOL | `0` | KEEP-DEBUG | 4 | 1 DEMO/MeshOps.cpp:8986(L3) | 2026-08-05 34b5b5c8 | instrument tag / name |
| `pom_seam_viz` | INT | `0` | KEEP-DEBUG | 8 | 1 DEMO/MeshOps.cpp:8997(L3) | 2026-08-05 34b5b5c8 | instrument tag / name |
| `pom_shell_census` | BOOL | `0` | KEEP-DEBUG | 3 | 1 DEMO/MeshOps.cpp:6446(L1) | 2026-08-04 90768df6 | instrument tag / name |
| `face_id_dump` | BOOL | `0` | KEEP-DEBUG | 2 | 1 FDS/FILLERS/Mekalele.h:4298(L1) | 2026-08-04 5c72bb48 | instrument tag / name |
| `pom_mip_viz` | BOOL | `0` | KEEP-DEBUG | 3 | — | 2026-07-31 4633aeb7 | instrument tag / name |
| `pom_path_viz` | INT | `0` | KEEP-DEBUG | 5 | 2 DEMO/Snapshot.cpp:697(L1) DEMO/Snapshot.cpp:894(L1) | 2026-08-05 6470fd14 | instrument tag / name |
| `viz_tangent` | BOOL | `0` | KEEP-DEBUG | 2 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:2893(L2) | 2026-05-20 c40c5f1a | instrument tag / name |
| `viz_normal` | BOOL | `0` | KEEP-DEBUG | 2 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:2894(L2) | 2026-06-28 6888c431 | instrument tag / name |
| `viz_geonormal` | BOOL | `0` | KEEP-DEBUG | 2 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:2895(L2) | 2026-06-23 4b64fecf | instrument tag / name |
| `viz_matid` | BOOL | `0` | KEEP-DEBUG | 2 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:2896(L2) | 2026-06-03 ce4e269c | instrument tag / name |
| `viz_pmid` | BOOL | `0` | KEEP-DEBUG | 2 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:2897(L2) | 2026-06-03 6f1fa123 | instrument tag / name |
| `aa_viz` | BOOL | `0` | KEEP-DEBUG | 3 | — | 2026-06-29 77051cca | instrument tag / name |
| `vol_prof` | BOOL | `0` | KEEP-DEBUG | 3 | 1 FDS/RENDER/DeferredVolumetric.cpp:2921(L1) | 2026-05-19 a29dc263 | instrument tag / name |
| `mip_stats` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-08 b8319e10 | instrument tag / name |
| `clip_stats` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-08-16 5071cc37 | instrument tag / name |
| `water_census` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-15 ebb03fc9 | instrument tag / name |
| `dump_panorama` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-20 c40c5f1a | instrument tag / name |
| `chunk_occl_verify` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-03 5bcd6cca | instrument tag / name |
| `vis_stats` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-08-03 5bcd6cca | instrument tag / name |
| `zero_normal_census` | BOOL | `0` | KEEP-DEBUG | 3 | — | 2026-08-16 58ab589f | instrument tag / name |
| `tangent_nan_census` | BOOL | `0` | KEEP-DEBUG | 4 | 1 DEMO/MeshOps.cpp:6946(L1) | 2026-08-16 1f640716 | instrument tag / name |
| `displace_viz` | INT | `0` | KEEP-DEBUG | 17 | 1 DEMO/MeshOps.cpp:6356(L1) | 2026-08-02 a12351c0 | instrument tag / name |
| `shadow_plane_hash` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-16 8dde99fd | instrument tag / name |
| `clone_stale_census` | INT | `0` | KEEP-DEBUG | 2 | — | 2026-08-16 1d800add | instrument tag / name |
| `mirror_rtt_trace` | BOOL | `0` | KEEP-DEBUG | 2 | — | 2026-08-16 0610ddbb | instrument tag / name |
| `mirror_prof` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-06-10 0d07a49e | instrument tag / name |
| `chase_cam_dump` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-07-30 c957d7b7 | instrument tag / name |
| `ssao_dump` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-17 bf300ea0 | instrument tag / name · used by tools/ssao_ao_compare.py, tools/ssao_gtao_battery.sh |
| `mirrortest_multi_dump` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-06-03 11fadd81 | instrument tag / name |
| `dump_mats` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-05-14 eda00563 | instrument tag / name |
| `wire_viz` | INT | `0` | KEEP-DEBUG | 9 | — | 2026-08-06 897e6f57 | instrument tag / name |
| `wire_viz_dim` | FLOAT | `0.25f` | KEEP-DEBUG | 2 | — | 2026-08-06 897e6f57 | instrument tag / name |
| `viz_legend` | BOOL | `1` | KEEP-DEBUG | 1 | — | 2026-08-06 247c62c1 | HAND-ADJUDICATED: gates the on-screen legend for the debug vizzes (--pom_path_viz, --wire_viz, --displace_viz, --pom_seam_viz); 'byte-null with every viz off' is a statement about the vizzes, not an arms equality. |
| `viz_arm` | BOOL | `0` | KEEP-DEBUG | 3 | — | 2026-08-06 897e6f57 | instrument tag / name |
| `pom_shell_slit_census` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-06 2839c29b | instrument tag / name |
| `deferred_prof` | INT | `0` | KEEP-DEBUG | 2 | — | 2026-08-08 a4defa94 | instrument tag / name · used by tools/ovec/ab.sh, tools/ovec/ab2.sh, tools/ovec/ab3.sh |
| `env_dyn_stats` | INT | `0` | KEEP-DEBUG | 1 | — | 2026-08-08 3f1c31c1 | instrument tag / name |
| `env_map_viz` | INT | `0` | KEEP-DEBUG | 8 | — | 2026-08-09 bd6e8060 | instrument tag / name |
| `env_dyn_dump` | INT | `0` | KEEP-DEBUG | 1 | — | 2026-08-09 5079f6eb | instrument tag / name |
| `mem_census` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-10 239a9d7d | instrument tag / name |
| `mem_census_frame` | INT | `1` | KEEP-DEBUG | 1 | — | 2026-08-10 239a9d7d | instrument tag / name |
| `hw_prof` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-12 e544ff3c | instrument tag / name · used by tools/ovec/ab.sh, tools/ovec/ab2.sh, tools/ovec/ab3.sh, tools/ovec/gi.sh, tools/ovec/ssab.sh |
| `greets_displace_junction_census` | BOOL | `0` | KEEP-DEBUG | 10 | 10 DEMO/MeshOps.cpp:2908(L1) DEMO/MeshOps.cpp:5149(L1) | 2026-08-11 2b61b85f | instrument tag / name |
| `env_dyn_dump_seq` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-12 51d1d15a | instrument tag / name |
| `mip_aniso_stats` | BOOL | `0` | KEEP-DEBUG | 1 | — | 2026-08-13 39f1356c | instrument tag / name |
| `env_water_region_viz` | INT | `0` | KEEP-DEBUG | 1 | — | 2026-08-16 868ba5d8 | instrument tag / name |
| `param_scripts` | BOOL | `1` | KEEP-TUNABLE | 2 | — | 2026-06-10 95c0ca34 | live tunable / scene control, 2 code refs |
| `vanilla` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-08-05 3712f00e | live tunable / scene control, 4 code refs |
| `scrub_speed` | INT | `4` | KEEP-TUNABLE | 1 | — | 2026-08-06 6b5556db | live tunable / scene control, 1 code refs |
| `warn_unknown_flags` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-07-11 24820138 | live tunable / scene control, 1 code refs |
| `profiler` | BOOL | `0` | KEEP-TUNABLE | 9 | — | 2026-06-12 1b59233f | live tunable / scene control, 9 code refs |
| `tune_server` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-12 6ce2443f | live tunable / scene control, 1 code refs |
| `tune_port` | INT | `8666` | KEEP-TUNABLE | 1 | — | 2026-06-12 6ce2443f | live tunable / scene control, 1 code refs |
| `deferred` | BOOL | `FDS_DEFERRED_DEFAULT_ON` | KEEP-TUNABLE | 12 | — | 2026-05-14 eda00563 | live tunable / scene control, 12 code refs |
| `spot_cone_cull` | BOOL | `1` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/DeferredVolumetric.cpp:3180(L2) | 2026-06-12 14ac3da1 | live tunable / scene control, 2 code refs |
| `deferred_zcull` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `deferred_vec` | BOOL | `FDS_DEFERRED_VEC_DEFAULT` | KEEP-TUNABLE | 1 | — | 2026-06-29 544dcee7 | live tunable / scene control, 1 code refs |
| `deferred_vec_force` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-29 b60d9160 | live tunable / scene control, 1 code refs |
| `deferred_outer_vec` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-14 eda00563 | live tunable / scene control, 2 code refs |
| `deferred_checkerboard` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-05-14 eda00563 | live tunable / scene control, 4 code refs |
| `deferred_quarter` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-05-14 eda00563 | live tunable / scene control, 4 code refs |
| `deferred_no_spec` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `quarter_tex_sharp` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-07-02 9f5cdd75 | live tunable / scene control, 1 code refs |
| `pbr` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-06-29 a288b121 | live tunable / scene control, 4 code refs |
| `pbr_roughness` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-06-29 a288b121 | live tunable / scene control, 1 code refs |
| `deferred_unified_tbr` | BOOL | `1` | KEEP-TUNABLE | 4 | — | 2026-05-30 af3a2821 | live tunable / scene control, 4 code refs |
| `xpar_peel_passes` | INT | `1` | KEEP-TUNABLE | 3 | — | 2026-06-18 1c993326 | live tunable / scene control, 3 code refs |
| `deferred_max_range` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `env_brdf_analytic` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-07-14 e0640fe8 | live tunable / scene control, 4 code refs |
| `sh_ambient` | BOOL | `0` | KEEP-TUNABLE | 6 | — | 2026-07-14 d29302ac | live tunable / scene control, 6 code refs |
| `diffuse_energy` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-07-14 ccc02296 | live tunable / scene control, 4 code refs |
| `pbr_multiscatter` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-07-14 2718046c | live tunable / scene control, 4 code refs |
| `shadows` | BOOL | `FDS_SHADOWS_DEFAULT_ON` | KEEP-TUNABLE | 10 | 3 FDS/RENDER/DeferredSurfaceKernel.cpp:8505(L1) FDS/RENDER/DeferredSurfaceKernel.cpp:8531(L1) | 2026-05-14 eda00563 | live tunable / scene control, 10 code refs |
| `shadow_polyid` | BOOL | `FDS_SHADOW_POLYID_DEFAULT_ON` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `shadow_polyid_no_pcf` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-29 205b119d | live tunable / scene control, 2 code refs |
| `shadow_backface_cull` | BOOL | `0` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/Transform.cpp:2794(L1) | 2026-05-14 eda00563 | live tunable / scene control, 2 code refs |
| `shadow_validate` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `shadow_bake_time` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-28 d61228dd | live tunable / scene control, 1 code refs |
| `shadow_cone_cull` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-15 76bc1da0 | live tunable / scene control, 2 code refs |
| `shadow_skip_animated` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-20 bed9b292 | live tunable / scene control, 2 code refs |
| `shadow_dynamic` | BOOL | `0` | KEEP-TUNABLE | 8 | — | 2026-05-20 2eaa2579 | live tunable / scene control, 8 code refs |
| `shadow_fzp_mult` | FLOAT | `3.0f` | KEEP-TUNABLE | 2 | — | 2026-05-14 eda00563 | live tunable / scene control, 2 code refs |
| `shadow_gbuffer_overlap` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-06-18 d14e373c | live tunable / scene control, 4 code refs |
| `bake_tick_overlap` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-07-03 d2c43056 | live tunable / scene control, 4 code refs |
| `shadow_bias` | INT | `512` | KEEP-TUNABLE | 2 | — | 2026-05-14 eda00563 | live tunable / scene control, 2 code refs |
| `shadow_slope_bias` | INT | `1024` | KEEP-TUNABLE | 2 | — | 2026-05-14 eda00563 | live tunable / scene control, 2 code refs |
| `shadow_lightmap` | BOOL | `0` | KEEP-TUNABLE | 8 | — | 2026-08-11 2b61b85f | live tunable / scene control, 8 code refs |
| `shadow_lightmap_res` | INT | `16` | KEEP-TUNABLE | 2 | — | 2026-05-22 7f37e599 | live tunable / scene control, 2 code refs |
| `shadow_lightmap_recompute_bake` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-25 d135bffc | live tunable / scene control, 1 code refs |
| `shadow_lightmap_recompute_at_bary` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-25 d135bffc | live tunable / scene control, 1 code refs |
| `shadow_lightmap_nearest` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-26 d84364e9 | live tunable / scene control, 1 code refs |
| `shadow_lightmap_planar` | BOOL | `0` | KEEP-TUNABLE | 3 | 1 FDS/RENDER/LightmapBake.cpp:451(L1) | 2026-05-29 205b119d | live tunable / scene control, 3 code refs |
| `rast_full_store` | BOOL | `1` | KEEP-TUNABLE | 2 | — | 2026-05-30 af3a2821 | live tunable / scene control, 2 code refs |
| `rast_inside_template` | BOOL | `1` | KEEP-TUNABLE | 1 | 1 FDS/FILLERS/Mekalele.h:3610(L2) | 2026-05-30 af3a2821 | live tunable / scene control, 1 code refs |
| `no_sort` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-16 f6ff365e | live tunable / scene control, 1 code refs |
| `no_quad_sort` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `xpar_force_twosided` | BOOL | `0` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/Transform.cpp:2782(L1) | 2026-05-14 eda00563 | live tunable / scene control, 2 code refs |
| `no_xpar_frontback` | BOOL | `0` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/Transform.cpp:2786(L1) | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `no_xpar_objgroup` | BOOL | `0` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/Transform.cpp:2787(L1) | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `xpar_light_all` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `xpar_tile_lights` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-08-15 da286d8b | live tunable / scene control, 1 code refs |
| `glass_refract` | FLOAT | `0.0f` | KEEP-TUNABLE | 4 | — | 2026-07-09 f4d470a6 | live tunable / scene control, 4 code refs |
| `glass_refract_ior` | FLOAT | `1.5f` | KEEP-TUNABLE | 1 | — | 2026-07-09 f4d470a6 | live tunable / scene control, 1 code refs |
| `glass_refract_max` | FLOAT | `48.0f` | KEEP-TUNABLE | 1 | — | 2026-07-09 f4d470a6 | live tunable / scene control, 1 code refs |
| `glass_refract_rough_blur` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-07-09 f4d470a6 | live tunable / scene control, 1 code refs |
| `glass_test` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-09 f4d470a6 | live tunable / scene control, 1 code refs |
| `xpar_pbr` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-10 d7f2d499 | live tunable / scene control, 1 code refs |
| `debug_xpar_ids` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-20 c40c5f1a | live tunable / scene control, 1 code refs |
| `debug_xpar_min_area` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-05-20 c40c5f1a | live tunable / scene control, 1 code refs |
| `xfrm_rcp` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-06 fdc7a077 | live tunable / scene control, 1 code refs |
| `xfrm_ablate` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-08-16 5c26162c | live tunable / scene control, 2 code refs |
| `quarter_normal_cos` | FLOAT | `0.95f` | KEEP-TUNABLE | 2 | — | 2026-05-29 205b119d | live tunable / scene control, 2 code refs |
| `quarter_z_jump` | FLOAT | `0.04f` | KEEP-TUNABLE | 2 | — | 2026-05-29 dd57a37f | live tunable / scene control, 2 code refs |
| `nmap_lod_fade_start` | INT | `2` | KEEP-TUNABLE | 2 | — | 2026-05-29 4c43629f | live tunable / scene control, 2 code refs |
| `nmap_lod_fade_step` | FLOAT | `0.33f` | KEEP-TUNABLE | 2 | — | 2026-05-29 dd57a37f | live tunable / scene control, 2 code refs |
| `frame_tile_x` | INT | `6` | KEEP-TUNABLE | 1 | — | 2026-08-16 e965dc26 | live tunable / scene control, 1 code refs |
| `frame_tile_y` | INT | `5` | KEEP-TUNABLE | 4 | — | 2026-08-16 e965dc26 | live tunable / scene control, 4 code refs |
| `surf_smoothing_authored` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-07-09 11a36b0c | live tunable / scene control, 2 code refs |
| `texture_filter` | INT | `0` | KEEP-TUNABLE | 8 | — | 2026-07-06 e9e60a3e | live tunable / scene control, 8 code refs |
| `no_nmap` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-14 eda00563 | live tunable / scene control, 2 code refs |
| `nmap_strength` | FLOAT | `1.5f` | KEEP-TUNABLE | 1 | — | 2026-05-16 991414b6 | live tunable / scene control, 1 code refs |
| `nmap_blur` | INT | `4` | KEEP-TUNABLE | 1 | — | 2026-05-16 991414b6 | live tunable / scene control, 1 code refs |
| `ao_map` | BOOL | `1` | KEEP-TUNABLE | 3 | — | 2026-06-26 524d60c2 | live tunable / scene control, 3 code refs |
| `ao_map_strength` | FLOAT | `2.0f` | KEEP-TUNABLE | 2 | — | 2026-07-03 46810cb9 | live tunable / scene control, 2 code refs |
| `ao_direct` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-02 b3dbb51b | live tunable / scene control, 1 code refs |
| `ao_direct_strength` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-08-02 b3dbb51b | live tunable / scene control, 1 code refs |
| `env_refl` | BOOL | `0` | KEEP-TUNABLE | 17 | 2 DEMO/MaterialImport.cpp:493(L1) DEMO/MaterialImport.cpp:488(L1) | 2026-07-03 a0a55638 | live tunable / scene control, 17 code refs |
| `env_refl_gain` | FLOAT | `1.0f` | KEEP-TUNABLE | 3 | — | 2026-07-03 a0a55638 | live tunable / scene control, 3 code refs |
| `metal_map` | BOOL | `1` | KEEP-TUNABLE | 5 | — | 2026-07-03 df1f7858 | live tunable / scene control, 5 code refs |
| `env_refl_res` | INT | `512` | KEEP-TUNABLE | 1 | — | 2026-07-10 eb739417 | live tunable / scene control, 1 code refs |
| `env_bake_res` | INT | `256` | KEEP-TUNABLE | 2 | — | 2026-07-10 eb739417 | live tunable / scene control, 2 code refs |
| `env_refl_scene_mode` | INT | `0` | KEEP-TUNABLE | 4 | — | 2026-07-11 6c6c972a | live tunable / scene control, 4 code refs |
| `env_bake_res_scene` | INT | `0` | KEEP-TUNABLE | 4 | — | 2026-07-11 6c6c972a | live tunable / scene control, 4 code refs |
| `env_bake_res_cap` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-07-14 8d936e01 | live tunable / scene control, 2 code refs |
| `env_bake_fix` | BOOL | `0` | KEEP-TUNABLE | 12 | 3 DEMO/MaterialImport.cpp:494(L1) DEMO/MaterialImport.cpp:489(L1) | 2026-07-09 a7355824 | live tunable / scene control, 12 code refs |
| `city_env_pixel` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-07-04 27be9c6f | live tunable / scene control, 2 code refs |
| `env_sphere_parallax` | FLOAT | `0.0f` | KEEP-TUNABLE | 2 | — | 2026-07-06 76c228cd | live tunable / scene control, 2 code refs |
| `env_ssr` | INT | `0` | KEEP-TUNABLE | 3 | — | 2026-07-06 90385501 | live tunable / scene control, 3 code refs |
| `env_ssr_stride` | FLOAT | `4.0f` | KEEP-TUNABLE | 1 | — | 2026-07-06 90385501 | live tunable / scene control, 1 code refs |
| `env_ssr_thick` | FLOAT | `40.0f` | KEEP-TUNABLE | 1 | — | 2026-07-06 90385501 | live tunable / scene control, 1 code refs |
| `city_env_f0` | FLOAT | `60.0f` | KEEP-TUNABLE | 1 | 1 DEMO/CITY.CPP:3235(L2) | 2026-07-04 dd90c6fe | live tunable / scene control, 1 code refs |
| `city_env_lum` | FLOAT | `0.45f` | KEEP-TUNABLE | 1 | 1 DEMO/CITY.CPP:3246(L2) | 2026-07-04 231acbb0 | live tunable / scene control, 1 code refs |
| `city_env_gloss` | FLOAT | `24.0f` | KEEP-TUNABLE | 1 | 1 DEMO/CITY.CPP:3236(L2) | 2026-07-04 bec32e70 | live tunable / scene control, 1 code refs |
| `env_live_water` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-08-16 868ba5d8 | live tunable / scene control, 3 code refs |
| `env_live_water_amp` | FLOAT | `0.10f` | KEEP-TUNABLE | 1 | — | 2026-07-11 5d28db79 | live tunable / scene control, 1 code refs |
| `env_live_water_shade` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-07-11 0162d3b0 | live tunable / scene control, 1 code refs |
| `env_dynamic` | BOOL | `0` | KEEP-TUNABLE | 6 | 1 FDS/RENDER/EnvBake.cpp:1664(L1) | 2026-07-30 33ba8790 | live tunable / scene control, 6 code refs |
| `env_dynamic_budget` | INT | `2` | KEEP-TUNABLE | 1 | — | 2026-07-30 33ba8790 | live tunable / scene control, 1 code refs |
| `draw_aabbs` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-07-30 0d8f2174 | live tunable / scene control, 2 code refs |
| `parallax` | BOOL | `1` | KEEP-TUNABLE | 7 | 2 DEMO/GREETS.CPP:1797(L1) DEMO/MaterialImport.cpp:471(L1) | 2026-07-06 5ed5e284 | live tunable / scene control, 7 code refs |
| `nmap_16bit` | BOOL | `1` | KEEP-TUNABLE | 3 | 1 DEMO/GREETS.CPP:1846(L1) | 2026-06-27 ef43ea23 | live tunable / scene control, 3 code refs |
| `roughness_map` | BOOL | `1` | KEEP-TUNABLE | 5 | — | 2026-06-27 19aa611a | live tunable / scene control, 5 code refs |
| `roughness_strength` | FLOAT | `1.0f` | KEEP-TUNABLE | 4 | — | 2026-06-27 19aa611a | live tunable / scene control, 4 code refs |
| `parallax_strength` | FLOAT | `0.3f` | KEEP-TUNABLE | 11 | 6 DEMO/DisplaceTest.cpp:1427(L1) DEMO/GREETS.CPP:1838(L1) | 2026-07-07 01029cb8 | live tunable / scene control, 11 code refs |
| `parallax_pom` | INT | `8` | KEEP-TUNABLE | 4 | — | 2026-07-07 bcf45f5a | live tunable / scene control, 4 code refs |
| `parallax_pom_quarter` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-07-07 0092481b | live tunable / scene control, 1 code refs |
| `parallax_pom_lod` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-07-07 0092481b | live tunable / scene control, 1 code refs |
| `parallax_pom_cone` | BOOL | `0` | KEEP-TUNABLE | 7 | 3 DEMO/DisplaceTest.cpp:1431(L1) DEMO/GREETS.CPP:1809(L1) | 2026-07-07 bcf45f5a | live tunable / scene control, 7 code refs |
| `parallax_pom_refine` | INT | `6` | KEEP-TUNABLE | 1 | — | 2026-07-07 bcf45f5a | live tunable / scene control, 1 code refs |
| `parallax_pom_relax` | FLOAT | `4.0f` | KEEP-TUNABLE | 1 | — | 2026-07-07 bcf45f5a | live tunable / scene control, 1 code refs |
| `pom_height_mip` | INT | `-1` | KEEP-TUNABLE | 1 | — | 2026-07-31 4633aeb7 | live tunable / scene control, 1 code refs |
| `parallax_max_offset` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-07-31 4633aeb7 | live tunable / scene control, 1 code refs |
| `pom_depth_write` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-04 c2616e43 | live tunable / scene control, 1 code refs |
| `pom_normal` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-08-04 bae41aad | live tunable / scene control, 3 code refs |
| `pom_normal_strength` | FLOAT | `1.0f` | KEEP-TUNABLE | 3 | — | 2026-08-04 bae41aad | live tunable / scene control, 3 code refs |
| `pom_shell` | BOOL | `0` | KEEP-TUNABLE | 14 | — | 2026-08-04 c556148a | live tunable / scene control, 14 code refs |
| `pom_shell_cap` | FLOAT | `8.0f` | KEEP-TUNABLE | 7 | — | 2026-08-04 c556148a | live tunable / scene control, 7 code refs |
| `pom_tbn_face_sign` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-05 7bfbc87e | live tunable / scene control, 1 code refs |
| `pom_shell_cap_fade` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-08-05 6470fd14 | live tunable / scene control, 1 code refs |
| `pom_shell_pin` | BOOL | `0` | KEEP-TUNABLE | 2 | 1 DEMO/DisplaceRebuild.cpp:378(L1) | 2026-08-04 c556148a | live tunable / scene control, 2 code refs |
| `pom_shell_domain` | BOOL | `1` | KEEP-TUNABLE | 2 | — | 2026-08-04 c556148a | live tunable / scene control, 2 code refs |
| `pom_shell_base_clip_raw` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-04 9d248a13 | live tunable / scene control, 1 code refs |
| `pom_shell_merge_uv` | FLOAT | `0.05f` | KEEP-TUNABLE | 1 | — | 2026-08-04 9d248a13 | live tunable / scene control, 1 code refs |
| `pom_horizon` | BOOL | `0` | KEEP-TUNABLE | 7 | 2 DEMO/GREETS.CPP:1835(L1) DEMO/DisplaceRebuild.cpp:285(L1) | 2026-08-04 c86b5bc0 | live tunable / scene control, 7 code refs |
| `pom_horizon_soft` | FLOAT | `0.15f` | KEEP-TUNABLE | 1 | — | 2026-08-04 c86b5bc0 | live tunable / scene control, 1 code refs |
| `pom_horizon_strength` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-08-04 c86b5bc0 | live tunable / scene control, 1 code refs |
| `pom_horizon_radius` | INT | `128` | KEEP-TUNABLE | 3 | 3 DEMO/GREETS.CPP:1839(L1) DEMO/DisplaceRebuild.cpp:200(L1) | 2026-08-04 c86b5bc0 | live tunable / scene control, 3 code refs |
| `pom_shell_side_faces` | INT | `0` | KEEP-TUNABLE | 5 | — | 2026-08-05 59567fc0 | live tunable / scene control, 5 code refs |
| `pom_shell_side_edge` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-08-05 3712f00e | live tunable / scene control, 2 code refs |
| `pom_shell_weld` | INT | `1` | KEEP-TUNABLE | 4 | — | 2026-08-06 140b6a07 | live tunable / scene control, 4 code refs |
| `pom_shell_lid_edge` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-07 c5bf8ec4 | live tunable / scene control, 1 code refs |
| `pom_shell_keep_uv` | FLOAT | `0.125f` | KEEP-TUNABLE | 1 | — | 2026-08-07 c5bf8ec4 | live tunable / scene control, 1 code refs |
| `pom_shell_entry_flat` | BOOL | `0` | KEEP-TUNABLE | 1 | 1 DEMO/MeshOps.cpp:8170(L1) | 2026-08-08 690b3ae2 | live tunable / scene control, 1 code refs |
| `pom_shell_lid_true_edge` | INT | `0` | KEEP-TUNABLE | 3 | — | 2026-08-08 b32e214d | live tunable / scene control, 3 code refs |
| `pom_shell_keep_uv_overhang` | FLOAT | `-1.0f` | KEEP-TUNABLE | 1 | — | 2026-08-08 99c09e77 | live tunable / scene control, 1 code refs |
| `pom_shell_side_entry` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-05 59567fc0 | HAND-ADJUDICATED: the supersede/no-longer language names an earlier VARIANT inside the same S1d thread, not this flag; active A/B in docs/S1D_CLOSED_SHELL_PLAN.md. |
| `pom_shell_world_amp` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-08-05 59567fc0 | live tunable / scene control, 3 code refs |
| `pom_shell_world_amp_set` | FLOAT | `0.0f` | KEEP-TUNABLE | 4 | — | 2026-08-04 90768df6 | live tunable / scene control, 4 code refs |
| `snapshot_ss` | INT | `1` | KEEP-TUNABLE | 1 | — | 2026-08-04 5c72bb48 | live tunable / scene control, 1 code refs |
| `pom_ref_march` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-04 5c72bb48 | live tunable / scene control, 1 code refs |
| `pom_ref_steps` | INT | `512` | KEEP-TUNABLE | 1 | — | 2026-08-04 5c72bb48 | live tunable / scene control, 1 code refs |
| `pom_shell_lid_probe` | BOOL | `0` | KEEP-TUNABLE | 3 | 1 DEMO/MeshOps.cpp:8255(L2) | 2026-08-04 5c72bb48 | live tunable / scene control, 3 code refs |
| `pom_recess_only` | BOOL | `0` | KEEP-TUNABLE | 12 | — | 2026-08-05 7bc3d3cc | live tunable / scene control, 12 code refs |
| `pom_recess_edge` | INT | `0` | KEEP-TUNABLE | 3 | — | 2026-08-05 7bc3d3cc | live tunable / scene control, 3 code refs |
| `pom_march_earlyout` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-08-04 774a9cb4 | live tunable / scene control, 3 code refs |
| `pom_cone_exact` | INT | `0` | KEEP-TUNABLE | 7 | 4 DEMO/GREETS.CPP:1814(L1) DEMO/GREETS.CPP:2034(L1) | 2026-08-04 774a9cb4 | live tunable / scene control, 7 code refs |
| `pom_cone_min_step` | FLOAT | `0.0f` | KEEP-TUNABLE | 3 | — | 2026-08-04 774a9cb4 | live tunable / scene control, 3 code refs |
| `pom_march_steps_auto` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-08-04 8ba2b8b2 | live tunable / scene control, 1 code refs |
| `pom_rebuild` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-08-05 3712f00e | live tunable / scene control, 2 code refs |
| `pom_rebuild_test` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-08-05 3712f00e | live tunable / scene control, 2 code refs |
| `ao_from_diffuse` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-26 524d60c2 | live tunable / scene control, 1 code refs |
| `nmap_from_diffuse` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-20 c40c5f1a | live tunable / scene control, 2 code refs |
| `nmap_as_diffuse` | BOOL | `0` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:2898(L2) | 2026-05-20 c40c5f1a | live tunable / scene control, 1 code refs |
| `aa` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-06-29 3145e663 | live tunable / scene control, 3 code refs |
| `aa_strength` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-06-29 3145e663 | live tunable / scene control, 1 code refs |
| `no_vsync` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-17 ff9417c5 | live tunable / scene control, 1 code refs |
| `draw_cones` | BOOL | `0` | KEEP-TUNABLE | 3 | 2 FDS/RENDER/DeferredFastFog.cpp:4355(L1) FDS/RENDER/DeferredVolumetric.cpp:2957(L1) | 2026-05-17 cc17d656 | live tunable / scene control, 3 code refs |
| `city_test_spots` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-17 cc17d656 | live tunable / scene control, 1 code refs |
| `cone_strength` | FLOAT | `0.05f` | KEEP-TUNABLE | 5 | 1 FDS/RENDER/DeferredVolumetric.cpp:2964(L1) | 2026-05-17 be13e178 | live tunable / scene control, 5 code refs |
| `volumetric_unified` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-19 2323670b | live tunable / scene control, 2 code refs |
| `fog_sigma_mult` | FLOAT | `3.0f` | KEEP-TUNABLE | 1 | — | 2026-05-19 2323670b | live tunable / scene control, 1 code refs |
| `omni_halo_strength` | FLOAT | `0.0f` | KEEP-TUNABLE | 4 | 2 FDS/RENDER/DeferredVolumetric.cpp:4017(L1) FDS/RENDER/DeferredVolumetric.cpp:4021(L1) | 2026-05-19 e26b0142 | live tunable / scene control, 4 code refs |
| `omni_halo_range_mult` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:8594(L1) | 2026-05-20 74a4133b | live tunable / scene control, 1 code refs |
| `omni_halo_force_range` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:8588(L1) | 2026-05-20 74a4133b | live tunable / scene control, 1 code refs |
| `vol_n_samples` | INT | `4` | KEEP-TUNABLE | 3 | 1 FDS/RENDER/DeferredVolumetric.cpp:3417(L1) | 2026-05-19 548b70d0 | live tunable / scene control, 3 code refs |
| `vol_cone_half_y` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-12 47878521 | live tunable / scene control, 1 code refs |
| `vol_vec` | BOOL | `1` | KEEP-TUNABLE | 4 | 2 FDS/RENDER/DeferredVolumetric.cpp:3419(L1) FDS/RENDER/DeferredVolumetric.cpp:4039(L1) | 2026-05-19 7938ae7d | live tunable / scene control, 4 code refs |
| `cone_range_cull` | FLOAT | `1.0f` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/DeferredVolumetric.cpp:3106(L1) | 2026-08-28 21624788 | live tunable / scene control, 2 code refs |
| `cone_hull_rect` | BOOL | `1` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredVolumetric.cpp:3107(L1) | 2026-08-28 0d16f6cd | live tunable / scene control, 1 code refs |
| `vol_halo_analytic` | BOOL | `1` | KEEP-TUNABLE | 2 | 2 FDS/RENDER/DeferredVolumetric.cpp:3420(L1) FDS/RENDER/DeferredVolumetric.cpp:4039(L1) | 2026-05-19 e26b0142 | live tunable / scene control, 2 code refs |
| `vol_cone_analytic` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-08-12 e544ff3c | live tunable / scene control, 1 code refs |
| `vol_analytic_noise` | FLOAT | `0.08f` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/DeferredVolumetric.cpp:3421(L1) | 2026-05-20 74a4133b | live tunable / scene control, 2 code refs |
| `cone_turbulence` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredVolumetric.cpp:2987(L1) | 2026-07-11 ab9a9c16 | live tunable / scene control, 1 code refs |
| `cone_turb_scale` | FLOAT | `3.0f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredVolumetric.cpp:2994(L1) | 2026-07-11 ab9a9c16 | live tunable / scene control, 1 code refs |
| `cone_turb_speed` | FLOAT | `0.5f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredVolumetric.cpp:2997(L1) | 2026-07-11 ab9a9c16 | live tunable / scene control, 1 code refs |
| `cone_swirl` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredVolumetric.cpp:3003(L1) | 2026-07-11 ab9a9c16 | live tunable / scene control, 1 code refs |
| `cone_turb_octaves` | INT | `2` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/DeferredVolumetric.cpp:3009(L1) | 2026-07-11 ab9a9c16 | live tunable / scene control, 1 code refs |
| `deferred_skybox` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-20 d25f3d3b | live tunable / scene control, 2 code refs |
| `fast_fog` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-06-04 6187e3cb | live tunable / scene control, 3 code refs |
| `fast_fog_density` | FLOAT | `3.0f` | KEEP-TUNABLE | 3 | — | 2026-06-04 6187e3cb | live tunable / scene control, 3 code refs |
| `fast_fog_height` | FLOAT | `0.0f` | KEEP-TUNABLE | 2 | — | 2026-06-04 6187e3cb | live tunable / scene control, 2 code refs |
| `fast_fog_bottom` | FLOAT | `-1.0e9f` | KEEP-TUNABLE | 3 | — | 2026-06-04 6187e3cb | live tunable / scene control, 3 code refs |
| `fast_fog_top` | FLOAT | `1.0e9f` | KEEP-TUNABLE | 3 | — | 2026-06-04 6187e3cb | live tunable / scene control, 3 code refs |
| `fast_fog_feather` | FLOAT | `0.0f` | KEEP-TUNABLE | 2 | — | 2026-06-04 2f26ada7 | live tunable / scene control, 2 code refs |
| `fast_fog_dist_dim` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-07-06 97a28ef0 | live tunable / scene control, 1 code refs |
| `fast_fog_dist_dim_far` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-07-06 be3dfc5c | live tunable / scene control, 1 code refs |
| `mips` | BOOL | `1` | KEEP-TUNABLE | 6 | 1 FDS/FRUSTRUM/FRUSTRUM.CPP:1166(L1) | 2026-08-08 99c09e77 | live tunable / scene control, 6 code refs |
| `mip_bias` | FLOAT | `0.5f` | KEEP-TUNABLE | 3 | — | 2026-07-06 d1c6ec4d | live tunable / scene control, 3 code refs |
| `mip_hysteresis` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-07-06 d1c6ec4d | live tunable / scene control, 1 code refs |
| `fast_fog_blobs` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-06-04 6187e3cb | live tunable / scene control, 2 code refs |
| `fast_fog_cell` | FLOAT | `400.0f` | KEEP-TUNABLE | 2 | — | 2026-06-04 6187e3cb | live tunable / scene control, 2 code refs |
| `fast_fog_blob_jitter` | FLOAT | `0.35f` | KEEP-TUNABLE | 1 | — | 2026-06-04 6187e3cb | live tunable / scene control, 1 code refs |
| `fast_fog_worley` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-06-09 b8e0920f | live tunable / scene control, 2 code refs |
| `fast_fog_worley_thresh` | FLOAT | `0.5f` | KEEP-TUNABLE | 3 | — | 2026-06-09 b8e0920f | live tunable / scene control, 3 code refs |
| `fast_fog_blob_overlap` | FLOAT | `0.0f` | KEEP-TUNABLE | 2 | — | 2026-06-10 e8e5843a | live tunable / scene control, 2 code refs |
| `fast_fog_falloff` | FLOAT | `0.0f` | KEEP-TUNABLE | 2 | — | 2026-06-04 6187e3cb | live tunable / scene control, 2 code refs |
| `fast_fog_froxel` | BOOL | `1` | KEEP-TUNABLE | 3 | — | 2026-06-10 8f177998 | live tunable / scene control, 3 code refs |
| `fast_fog_froxel_x` | INT | `256` | KEEP-TUNABLE | 1 | — | 2026-06-09 2d2b942a | live tunable / scene control, 1 code refs |
| `fast_fog_froxel_y` | INT | `144` | KEEP-TUNABLE | 1 | — | 2026-06-09 2d2b942a | live tunable / scene control, 1 code refs |
| `fast_fog_froxel_z` | INT | `64` | KEEP-TUNABLE | 1 | — | 2026-06-09 bfdbb7cc | live tunable / scene control, 1 code refs |
| `fast_fog_froxel_temporal` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-09 3bc688d2 | live tunable / scene control, 1 code refs |
| `rain` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-06-12 a7146837 | live tunable / scene control, 4 code refs |
| `rain_intensity` | FLOAT | `1.0f` | KEEP-TUNABLE | 2 | — | 2026-06-12 a7146837 | live tunable / scene control, 2 code refs |
| `rain_speed` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-06-12 a7146837 | live tunable / scene control, 1 code refs |
| `fast_fog_refl_depth` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-12 983145e4 | live tunable / scene control, 1 code refs |
| `bolt_width` | FLOAT | `0.85f` | KEEP-TUNABLE | 3 | — | 2026-06-14 dcd7970c | live tunable / scene control, 3 code refs |
| `bolt_kink` | FLOAT | `0.30f` | KEEP-TUNABLE | 1 | — | 2026-06-14 1dae85b7 | live tunable / scene control, 1 code refs |
| `bolt_seg` | FLOAT | `1.0f` | KEEP-TUNABLE | 2 | — | 2026-06-14 1dae85b7 | live tunable / scene control, 2 code refs |
| `bolt_geom` | BOOL | `1` | KEEP-TUNABLE | 3 | 1 DEMO/FOUNTAIN.CPP:2311(L1) | 2026-06-16 d4e3b035 | live tunable / scene control, 3 code refs |
| `bolt_interval` | FLOAT | `220.0f` | KEEP-TUNABLE | 1 | — | 2026-06-16 d4e3b035 | live tunable / scene control, 1 code refs |
| `bolt_flash_peak` | FLOAT | `500.0f` | KEEP-TUNABLE | 2 | — | 2026-06-16 d4e3b035 | live tunable / scene control, 2 code refs |
| `bolt_flash_range` | FLOAT | `500.0f` | KEEP-TUNABLE | 3 | — | 2026-06-16 d4e3b035 | live tunable / scene control, 3 code refs |
| `bolt_flash_decay` | FLOAT | `0.10f` | KEEP-TUNABLE | 1 | — | 2026-06-16 d4e3b035 | live tunable / scene control, 1 code refs |
| `rain_lens` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-12 a303afc8 | live tunable / scene control, 1 code refs |
| `city_rain` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-06-12 683fd073 | live tunable / scene control, 2 code refs |
| `city_headlights` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-07-11 48d57e51 | live tunable / scene control, 2 code refs |
| `city_headlights_front` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-07-11 48d57e51 | live tunable / scene control, 3 code refs |
| `city_headlight_intensity` | FLOAT | `1.2f` | KEEP-TUNABLE | 3 | 2 DEMO/CITY.CPP:2453(L1) DEMO/CITY.CPP:2498(L2) | 2026-07-10 e4e34cfd | live tunable / scene control, 3 code refs |
| `city_headlight_range` | FLOAT | `1600.0f` | KEEP-TUNABLE | 1 | 1 DEMO/CITY.CPP:2490(L1) | 2026-07-10 e4e34cfd | live tunable / scene control, 1 code refs |
| `fast_fog_froxel_taps` | INT | `1` | KEEP-TUNABLE | 1 | — | 2026-06-11 44aa74be | live tunable / scene control, 1 code refs |
| `fast_fog_froxel_blend` | FLOAT | `0.9f` | KEEP-TUNABLE | 1 | — | 2026-06-11 ac8b8568 | live tunable / scene control, 1 code refs |
| `fast_fog_xpar` | BOOL | `1` | KEEP-TUNABLE | 3 | — | 2026-06-10 12e926d2 | live tunable / scene control, 3 code refs |
| `fast_fog_glow_grid_div` | INT | `4` | KEEP-TUNABLE | 1 | — | 2026-06-10 42336579 | live tunable / scene control, 1 code refs |
| `fast_fog_glow_max` | FLOAT | `0.0f` | KEEP-TUNABLE | 2 | — | 2026-06-10 4f7e5d92 | live tunable / scene control, 2 code refs |
| `fast_fog_halfres` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-04 6187e3cb | live tunable / scene control, 1 code refs |
| `fast_fog_adaptive` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-04 9b18621a | live tunable / scene control, 1 code refs |
| `fast_fog_adaptive_step` | INT | `4` | KEEP-TUNABLE | 1 | — | 2026-06-04 9b18621a | live tunable / scene control, 1 code refs |
| `fast_fog_adaptive_thresh` | FLOAT | `0.06f` | KEEP-TUNABLE | 1 | — | 2026-06-04 9b18621a | live tunable / scene control, 1 code refs |
| `fast_fog_inscatter` | FLOAT | `0.0f` | KEEP-TUNABLE | 2 | — | 2026-06-07 5231673a | live tunable / scene control, 2 code refs |
| `fast_fog_inscatter_analytic` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-07 5231673a | live tunable / scene control, 1 code refs |
| `fast_fog_inscatter_samples` | INT | `6` | KEEP-TUNABLE | 1 | — | 2026-06-07 5231673a | live tunable / scene control, 1 code refs |
| `fast_fog_inscatter_jitter` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-08 a5bf647e | live tunable / scene control, 1 code refs |
| `fast_fog_shadow_analytic` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-09 98069e32 | live tunable / scene control, 1 code refs |
| `fast_fog_shadow_earlyout` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-08 3ff6070a | live tunable / scene control, 1 code refs |
| `fast_fog_shadow_pcf` | INT | `1` | KEEP-TUNABLE | 1 | — | 2026-06-08 a5bf647e | live tunable / scene control, 1 code refs |
| `fast_fog_dither` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-06-04 9c9f8d5b | live tunable / scene control, 1 code refs |
| `cinematic` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-06-20 3359b92e | live tunable / scene control, 2 code refs |
| `hdr` | BOOL | `0` | KEEP-TUNABLE | 33 | 1 FDS/RENDER/DeferredVolumetric.cpp:2967(L1) | 2026-06-17 711db6d0 | live tunable / scene control, 33 code refs |
| `hdr_exposure` | FLOAT | `1.0f` | KEEP-TUNABLE | 4 | — | 2026-06-17 711db6d0 | live tunable / scene control, 4 code refs |
| `hdr_white` | FLOAT | `1.0f` | KEEP-TUNABLE | 2 | — | 2026-06-17 0df8c61c | live tunable / scene control, 2 code refs |
| `hdr_glow_scale` | FLOAT | `0.25f` | KEEP-TUNABLE | 7 | 1 FDS/RENDER/DeferredVolumetric.cpp:2968(L1) | 2026-06-17 3213e35a | live tunable / scene control, 7 code refs |
| `hdr_glow_softknee` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-21 64041b54 | live tunable / scene control, 1 code refs |
| `hdr_cone_softknee` | BOOL | `0` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/DeferredVolumetric.cpp:2967(L1) | 2026-06-21 64041b54 | live tunable / scene control, 2 code refs |
| `cone_glow_max` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-06-21 64041b54 | live tunable / scene control, 1 code refs |
| `hdr_linear` | BOOL | `0` | KEEP-TUNABLE | 11 | — | 2026-06-17 7b5ac709 | live tunable / scene control, 11 code refs |
| `bloom` | BOOL | `0` | KEEP-TUNABLE | 6 | — | 2026-06-18 66b9a672 | live tunable / scene control, 6 code refs |
| `bloom_threshold` | FLOAT | `200.0f` | KEEP-TUNABLE | 5 | — | 2026-06-18 66b9a672 | live tunable / scene control, 5 code refs |
| `bloom_intensity` | FLOAT | `0.6f` | KEEP-TUNABLE | 3 | — | 2026-06-18 66b9a672 | live tunable / scene control, 3 code refs |
| `hdr_refl_gain` | FLOAT | `2.5f` | KEEP-TUNABLE | 2 | — | 2026-06-18 12e8c8b1 | live tunable / scene control, 2 code refs |
| `anamorphic` | BOOL | `0` | KEEP-TUNABLE | 5 | — | 2026-06-18 51692d76 | live tunable / scene control, 5 code refs |
| `anamorphic_intensity` | FLOAT | `0.5f` | KEEP-TUNABLE | 2 | — | 2026-06-18 51692d76 | live tunable / scene control, 2 code refs |
| `anamorphic_passes` | INT | `6` | KEEP-TUNABLE | 2 | — | 2026-06-18 51692d76 | live tunable / scene control, 2 code refs |
| `anamorphic_decay` | FLOAT | `0.85f` | KEEP-TUNABLE | 2 | — | 2026-06-18 51692d76 | live tunable / scene control, 2 code refs |
| `anamorphic_vert` | FLOAT | `0.25f` | KEEP-TUNABLE | 2 | — | 2026-06-18 51692d76 | live tunable / scene control, 2 code refs |
| `lens_ghosts` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 3 code refs |
| `lens_ghost_intensity` | FLOAT | `0.4f` | KEEP-TUNABLE | 1 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 1 code refs |
| `lens_ghost_count` | INT | `4` | KEEP-TUNABLE | 1 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 1 code refs |
| `lens_ghost_dispersal` | FLOAT | `0.32f` | KEEP-TUNABLE | 1 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 1 code refs |
| `lens_ghost_halo` | FLOAT | `0.4f` | KEEP-TUNABLE | 1 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 1 code refs |
| `chromatic` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 4 code refs |
| `chromatic_amount` | FLOAT | `2.5f` | KEEP-TUNABLE | 2 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 2 code refs |
| `vignette` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 4 code refs |
| `vignette_strength` | FLOAT | `0.4f` | KEEP-TUNABLE | 2 | — | 2026-06-19 8bb9bb10 | live tunable / scene control, 2 code refs |
| `dof` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-06-19 64f55805 | live tunable / scene control, 2 code refs |
| `dof_focus` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-06-19 64f55805 | live tunable / scene control, 1 code refs |
| `dof_range` | FLOAT | `0.06f` | KEEP-TUNABLE | 1 | — | 2026-06-19 64f55805 | live tunable / scene control, 1 code refs |
| `dof_max` | FLOAT | `10.0f` | KEEP-TUNABLE | 1 | — | 2026-06-19 f707930e | live tunable / scene control, 1 code refs |
| `dof_downscale` | INT | `2` | KEEP-TUNABLE | 1 | — | 2026-07-03 6b1c3503 | live tunable / scene control, 1 code refs |
| `grade` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-06-19 fb3d881e | live tunable / scene control, 3 code refs |
| `grade_teal_orange` | FLOAT | `0.3f` | KEEP-TUNABLE | 1 | — | 2026-06-19 fb3d881e | live tunable / scene control, 1 code refs |
| `grade_contrast` | FLOAT | `1.05f` | KEEP-TUNABLE | 1 | — | 2026-06-19 fb3d881e | live tunable / scene control, 1 code refs |
| `grade_saturation` | FLOAT | `1.1f` | KEEP-TUNABLE | 1 | — | 2026-06-19 fb3d881e | live tunable / scene control, 1 code refs |
| `grade_temp` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-06-19 fb3d881e | live tunable / scene control, 1 code refs |
| `grain` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-06-19 fb3d881e | live tunable / scene control, 3 code refs |
| `grain_strength` | FLOAT | `6.0f` | KEEP-TUNABLE | 1 | — | 2026-06-19 fb3d881e | live tunable / scene control, 1 code refs |
| `skip_cubemap` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `city_envmap_cache` | BOOL | `1` | KEEP-TUNABLE | 2 | — | 2026-05-31 7d3019b0 | live tunable / scene control, 2 code refs |
| `water_ripple` | BOOL | `1` | KEEP-TUNABLE | 2 | — | 2026-06-19 3280f7b3 | live tunable / scene control, 2 code refs |
| `water_ripple_amp` | FLOAT | `30.0f` | KEEP-TUNABLE | 1 | — | 2026-06-19 4b265caf | live tunable / scene control, 1 code refs |
| `water_ripple_speed` | FLOAT | `1.0f` | KEEP-TUNABLE | 5 | — | 2026-06-19 3280f7b3 | live tunable / scene control, 5 code refs |
| `water_scroll_speed` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-06-19 fcb38a57 | live tunable / scene control, 1 code refs |
| `water_bump` | BOOL | `1` | KEEP-TUNABLE | 2 | — | 2026-06-19 9e226008 | live tunable / scene control, 2 code refs |
| `water_bump_strength` | FLOAT | `0.6f` | KEEP-TUNABLE | 3 | — | 2026-06-19 48590198 | live tunable / scene control, 3 code refs |
| `water_bump_shininess` | FLOAT | `14.0f` | KEEP-TUNABLE | 3 | — | 2026-06-19 1088b52b | live tunable / scene control, 3 code refs |
| `water_bump_scale` | FLOAT | `6.0f` | KEEP-TUNABLE | 6 | — | 2026-06-19 1088b52b | live tunable / scene control, 6 code refs |
| `water_detile` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-20 2e9ef244 | live tunable / scene control, 1 code refs |
| `water_detile_amp` | FLOAT | `0.6f` | KEEP-TUNABLE | 1 | — | 2026-06-20 2e9ef244 | live tunable / scene control, 1 code refs |
| `water_detile_scale` | FLOAT | `3.5f` | KEEP-TUNABLE | 1 | — | 2026-06-20 2e9ef244 | live tunable / scene control, 1 code refs |
| `water_procedural` | BOOL | `0` | KEEP-TUNABLE | 7 | — | 2026-06-20 20ad6f6f | live tunable / scene control, 7 code refs |
| `water_deep_b` | FLOAT | `58.0f` | KEEP-TUNABLE | 2 | — | 2026-06-20 20ad6f6f | live tunable / scene control, 2 code refs |
| `water_deep_g` | FLOAT | `44.0f` | KEEP-TUNABLE | 2 | — | 2026-06-20 20ad6f6f | live tunable / scene control, 2 code refs |
| `water_deep_r` | FLOAT | `26.0f` | KEEP-TUNABLE | 2 | — | 2026-06-20 20ad6f6f | live tunable / scene control, 2 code refs |
| `water_reflectivity` | FLOAT | `0.9f` | KEEP-TUNABLE | 2 | — | 2026-06-20 20ad6f6f | live tunable / scene control, 2 code refs |
| `water_fresnel_base` | FLOAT | `0.65f` | KEEP-TUNABLE | 2 | — | 2026-06-20 de931a53 | live tunable / scene control, 2 code refs |
| `water_fresnel_composite` | BOOL | `1` | KEEP-TUNABLE | 3 | — | 2026-07-14 604fd43c | live tunable / scene control, 3 code refs |
| `water_albedo_mix` | FLOAT | `0.45f` | KEEP-TUNABLE | 4 | — | 2026-06-20 d0ca54ba | live tunable / scene control, 4 code refs |
| `water_tex_scale` | FLOAT | `0.06f` | KEEP-TUNABLE | 4 | — | 2026-06-20 d0ca54ba | live tunable / scene control, 4 code refs |
| `water_tex_warp` | FLOAT | `5.0f` | KEEP-TUNABLE | 4 | — | 2026-06-20 bcf6a760 | live tunable / scene control, 4 code refs |
| `water_tex_flow` | FLOAT | `1.0f` | KEEP-TUNABLE | 3 | — | 2026-06-20 bcf6a760 | live tunable / scene control, 3 code refs |
| `water_variation` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-07-14 de185db0 | live tunable / scene control, 2 code refs |
| `debug_panorama` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-20 c40c5f1a | live tunable / scene control, 1 code refs |
| `greets_mat_labels` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-01 7cb0e95e | live tunable / scene control, 1 code refs |
| `greets_stone_tex` | BOOL | `1` | KEEP-TUNABLE | 2 | — | 2026-07-06 5ed5e284 | live tunable / scene control, 2 code refs |
| `greets_nmap_flip_g` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-22 8aa62964 | live tunable / scene control, 1 code refs |
| `greets_tbn_fix` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-22 2b63a78c | live tunable / scene control, 1 code refs |
| `greets_floor_uv_scale` | FLOAT | `1.5f` | KEEP-TUNABLE | 1 | — | 2026-06-22 560bfd13 | live tunable / scene control, 1 code refs |
| `greets_floor_detile` | FLOAT | `0.06f` | KEEP-TUNABLE | 1 | — | 2026-06-22 560bfd13 | live tunable / scene control, 1 code refs |
| `greets_mirror` | BOOL | `0` | KEEP-TUNABLE | 7 | — | 2026-06-01 51863afe | live tunable / scene control, 7 code refs |
| `greets_mirror_debug_mask` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-01 d7fc4fb1 | live tunable / scene control, 1 code refs |
| `greets_mirror_cam` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-03 ddb1bff3 | live tunable / scene control, 1 code refs |
| `mirrortest_skip_mirror` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-02 8e6f37b8 | live tunable / scene control, 1 code refs |
| `greets_spot_height` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `greets_blasters` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-06-24 58fde936 | live tunable / scene control, 3 code refs |
| `blaster_light` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-26 958571ae | live tunable / scene control, 1 code refs |
| `blaster_light_intensity` | FLOAT | `160.0f` | KEEP-TUNABLE | 2 | — | 2026-06-26 958571ae | live tunable / scene control, 2 code refs |
| `blaster_light_range` | FLOAT | `14.0f` | KEEP-TUNABLE | 2 | — | 2026-06-26 958571ae | live tunable / scene control, 2 code refs |
| `no_greets_spots` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-20 3aa290ca | live tunable / scene control, 2 code refs |
| `greets_omni_shadows` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-05-20 3aa290ca | live tunable / scene control, 2 code refs |
| `greets_omni_shadow_res` | INT | `256` | KEEP-TUNABLE | 2 | — | 2026-05-20 3e021620 | live tunable / scene control, 2 code refs |
| `greets_moving_omni_shadow_res` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-05-29 205b119d | live tunable / scene control, 2 code refs |
| `greets_piramid_chunk_grid` | INT | `8` | KEEP-TUNABLE | 1 | — | 2026-08-02 1739e952 | live tunable / scene control, 1 code refs |
| `greets_chunk_size` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-08-02 1739e952 | live tunable / scene control, 1 code refs |
| `chunk_occlusion` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-08-03 5bcd6cca | live tunable / scene control, 2 code refs |
| `chunk_occl_bias` | FLOAT | `0.5f` | KEEP-TUNABLE | 1 | — | 2026-08-03 5bcd6cca | live tunable / scene control, 1 code refs |
| `chunk_occl_res` | INT | `256` | KEEP-TUNABLE | 1 | — | 2026-08-03 5bcd6cca | live tunable / scene control, 1 code refs |
| `chunk_occl_snapshot_force` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-03 5bcd6cca | live tunable / scene control, 1 code refs |
| `greets_stone_subdiv` | INT | `0` | KEEP-TUNABLE | 6 | — | 2026-07-31 9973799e | live tunable / scene control, 6 code refs |
| `greets_displace` | BOOL | `0` | KEEP-TUNABLE | 10 | — | 2026-07-31 9973799e | live tunable / scene control, 10 code refs |
| `greets_displace_amp` | FLOAT | `0.3f` | KEEP-TUNABLE | 12 | — | 2026-07-31 9973799e | live tunable / scene control, 12 code refs |
| `greets_displace_mip` | INT | `2` | KEEP-TUNABLE | 7 | — | 2026-08-01 8474205e | live tunable / scene control, 7 code refs |
| `greets_displace_adapt` | FLOAT | `1.0f` | KEEP-TUNABLE | 6 | — | 2026-08-02 a12351c0 | live tunable / scene control, 6 code refs |
| `greets_displace_cpb` | FLOAT | `1.0f` | KEEP-TUNABLE | 6 | — | 2026-08-02 a12351c0 | live tunable / scene control, 6 code refs |
| `greets_displace_edge` | BOOL | `1` | KEEP-TUNABLE | 1 | 1 DEMO/MeshOps.cpp:2428(L1) | 2026-08-02 39d2f88d | live tunable / scene control, 1 code refs |
| `greets_displace_fold_relax` | BOOL | `1` | KEEP-TUNABLE | 3 | 1 DEMO/MeshOps.cpp:6216(L1) | 2026-08-03 135ffeab | live tunable / scene control, 3 code refs |
| `greets_displace_shadow_planes` | BOOL | `1` | KEEP-TUNABLE | 2 | — | 2026-08-03 135ffeab | live tunable / scene control, 2 code refs |
| `greets_displace_neighbor_pin` | BOOL | `1` | KEEP-TUNABLE | 3 | — | 2026-08-03 135ffeab | live tunable / scene control, 3 code refs |
| `greets_displace_line_height` | BOOL | `1` | KEEP-TUNABLE | 1 | 1 DEMO/MeshOps.cpp:2458(L1) | 2026-08-04 78ccfec5 | live tunable / scene control, 1 code refs |
| `greets_displace_flat_mirror` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-08-06 897e6f57 | live tunable / scene control, 4 code refs |
| `greets_displace_smooth` | FLOAT | `80.0f` | KEEP-TUNABLE | 2 | — | 2026-08-01 25e29692 | live tunable / scene control, 2 code refs |
| `shadow_cube_face_cull` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-05-29 205b119d | live tunable / scene control, 1 code refs |
| `clone_refresh_inputs` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-16 1d800add | live tunable / scene control, 1 code refs |
| `shadow_cube_vert_cull` | BOOL | `0` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/Transform.cpp:2154(L1) | 2026-05-29 205b119d | live tunable / scene control, 1 code refs |
| `shadow_swizzle` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-07-01 ffcaaf81 | live tunable / scene control, 4 code refs |
| `shadow_lm_dynamic` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-07-02 8ad15ad9 | live tunable / scene control, 3 code refs |
| `greets_omni_default_range` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-08-06 00f78200 | live tunable / scene control, 1 code refs |
| `greets_mirror_min_area` | FLOAT | `2.0f` | KEEP-TUNABLE | 1 | — | 2026-06-27 7e346ded | live tunable / scene control, 1 code refs |
| `greets_mirror_rtt_min_area` | FLOAT | `1.5f` | KEEP-TUNABLE | 1 | — | 2026-06-16 92fae9a4 | live tunable / scene control, 1 code refs |
| `mirror_rtt_probe` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-10 c9b8c6d7 | live tunable / scene control, 1 code refs |
| `mirror_rtt` | BOOL | `0` | KEEP-TUNABLE | 6 | — | 2026-06-10 09a6fc8b | live tunable / scene control, 6 code refs |
| `mirror_recurse_depth` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-07-04 b5d81e25 | live tunable / scene control, 2 code refs |
| `mirror_rtt_alpha` | FLOAT | `0.75f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/GreetsMirror.cpp:1640(L1) | 2026-06-11 fcfe04ba | live tunable / scene control, 1 code refs |
| `mirror_rtt_gain` | FLOAT | `0.55f` | KEEP-TUNABLE | 1 | — | 2026-06-11 c90db8e0 | live tunable / scene control, 1 code refs |
| `greets_disco` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-11 c5af3f19 | live tunable / scene control, 1 code refs |
| `shard_deferred` | BOOL | `0` | KEEP-TUNABLE | 4 | — | 2026-06-16 5bad86a4 | live tunable / scene control, 4 code refs |
| `mirror_rtt_density` | FLOAT | `256.0f` | KEEP-TUNABLE | 3 | 2 FDS/RENDER/GreetsMirror.cpp:1594(L1) FDS/RENDER/GreetsMirror.cpp:2767(L3) | 2026-06-11 534ab445 | live tunable / scene control, 3 code refs |
| `mirror_rtt_adaptive` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-18 06c225df | live tunable / scene control, 1 code refs |
| `mirror_rtt_adaptive_scale` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-06-18 06c225df | live tunable / scene control, 1 code refs |
| `greets_mirror_tint` | FLOAT | `0.0f` | KEEP-TUNABLE | 3 | 1 FDS/RENDER/MirrorShatter.cpp:949(L2) | 2026-06-17 bf0b6b52 | live tunable / scene control, 3 code refs |
| `greets_shard_randomness` | FLOAT | `1.0f` | KEEP-TUNABLE | 2 | — | 2026-06-17 a9fcc951 | live tunable / scene control, 2 code refs |
| `greets_shard_fall_speed` | FLOAT | `1.5f` | KEEP-TUNABLE | 2 | — | 2026-06-17 a9fcc951 | live tunable / scene control, 2 code refs |
| `greets_shard_lay_flat` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-17 a9fcc951 | live tunable / scene control, 1 code refs |
| `greets_shard_res` | INT | `64` | KEEP-TUNABLE | 2 | — | 2026-06-18 f2290443 | live tunable / scene control, 2 code refs |
| `greets_shard_refl_gain` | FLOAT | `1.0f` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/MirrorShatter.cpp:950(L2) | 2026-08-10 983cdb45 | live tunable / scene control, 2 code refs |
| `disco_bloom` | FLOAT | `1.5f` | KEEP-TUNABLE | 2 | — | 2026-06-11 fce46793 | live tunable / scene control, 2 code refs |
| `mirror_bounce` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-06-12 1a7465d5 | live tunable / scene control, 1 code refs |
| `mirror_bounce_gain` | FLOAT | `0.55f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/GreetsMirror.cpp:2391(L2) | 2026-06-12 1a7465d5 | live tunable / scene control, 1 code refs |
| `mirror_bounce_range` | FLOAT | `1.6f` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/GreetsMirror.cpp:2395(L2) | 2026-06-12 1a7465d5 | live tunable / scene control, 1 code refs |
| `fountain_particle_omni_pct` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `chase_legacy_omni_hack` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-12 3bb68ea4 | live tunable / scene control, 1 code refs |
| `sky_gradient` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-12 4cb75133 | live tunable / scene control, 1 code refs |
| `chase_engine_mod` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-12 8110345a | live tunable / scene control, 1 code refs |
| `chase_engine_mod_gain` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-07-12 8110345a | live tunable / scene control, 1 code refs |
| `chase_blasters` | BOOL | `0` | KEEP-TUNABLE | 6 | 1 DEMO/CHASE.CPP:1099(L1) | 2026-07-14 c22689d9 | live tunable / scene control, 6 code refs |
| `blaster_flash_peak` | FLOAT | `950.0f` | KEEP-TUNABLE | 1 | — | 2026-07-14 43cdc077 | live tunable / scene control, 1 code refs |
| `blaster_flash_range` | FLOAT | `420.0f` | KEEP-TUNABLE | 1 | — | 2026-07-14 c22689d9 | live tunable / scene control, 1 code refs |
| `blaster_flash_decay` | FLOAT | `0.12f` | KEEP-TUNABLE | 1 | — | 2026-07-14 c22689d9 | live tunable / scene control, 1 code refs |
| `chase_spark_size` | FLOAT | `0.00011f` | KEEP-TUNABLE | 1 | — | 2026-07-14 43cdc077 | live tunable / scene control, 1 code refs |
| `chase_spark_bright` | FLOAT | `255.0f` | KEEP-TUNABLE | 1 | — | 2026-07-14 ff821a22 | live tunable / scene control, 1 code refs |
| `chase_cam_fx` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-14 1e550784 | live tunable / scene control, 1 code refs |
| `chase_cam_shake_gain` | FLOAT | `0.04f` | KEEP-TUNABLE | 1 | — | 2026-07-14 1e550784 | live tunable / scene control, 1 code refs |
| `chase_cam_fov_kick` | FLOAT | `5.0f` | KEEP-TUNABLE | 1 | — | 2026-07-14 1e550784 | live tunable / scene control, 1 code refs |
| `ssao` | BOOL | `0` | KEEP-TUNABLE | 8 | — | 2026-06-23 fd0568da | live tunable / scene control, 8 code refs |
| `ssao_downscale` | INT | `2` | KEEP-TUNABLE | 1 | — | 2026-08-16 7763281d | live tunable / scene control, 1 code refs |
| `ssao_samples` | INT | `16` | KEEP-TUNABLE | 1 | — | 2026-06-24 9451b82a | live tunable / scene control, 1 code refs |
| `ssao_radius` | FLOAT | `4.0f` | KEEP-TUNABLE | 1 | — | 2026-06-23 fd0568da | live tunable / scene control, 1 code refs |
| `ssao_strength` | FLOAT | `1.5f` | KEEP-TUNABLE | 1 | — | 2026-06-23 fd0568da | live tunable / scene control, 1 code refs |
| `ssao_bias` | FLOAT | `0.1f` | KEEP-TUNABLE | 1 | — | 2026-06-23 fd0568da | live tunable / scene control, 1 code refs |
| `ssao_power` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-06-23 fd0568da | live tunable / scene control, 1 code refs |
| `ssao_blur` | INT | `2` | KEEP-TUNABLE | 1 | — | 2026-06-23 fd0568da | live tunable / scene control, 1 code refs |
| `ssao_debug` | BOOL | `0` | KEEP-TUNABLE | 6 | — | 2026-06-23 fd0568da | live tunable / scene control, 6 code refs |
| `ssao_gtao` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-24 52af1b44 | live tunable / scene control, 1 code refs |
| `ssao_gtao_slices` | INT | `2` | KEEP-TUNABLE | 1 | — | 2026-06-24 52af1b44 | live tunable / scene control, 1 code refs |
| `ssao_gtao_steps` | INT | `4` | KEEP-TUNABLE | 1 | — | 2026-06-24 52af1b44 | live tunable / scene control, 1 code refs |
| `ssao_gtao_thickness` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-06-24 52af1b44 | live tunable / scene control, 1 code refs |
| `ssao_temporal` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-07-03 871c1d84 | live tunable / scene control, 2 code refs |
| `ssao_temporal_blend` | FLOAT | `0.85f` | KEEP-TUNABLE | 1 | — | 2026-07-03 871c1d84 | live tunable / scene control, 1 code refs |
| `ssao_radius_zfloor` | FLOAT | `48.0f` | KEEP-TUNABLE | 1 | — | 2026-08-25 07151781 | live tunable / scene control, 1 code refs |
| `ssao_zprec_probe` | FLOAT | `1.0f` | KEEP-TUNABLE | 1 | — | 2026-08-25 b6505105 | live tunable / scene control, 1 code refs |
| `skip_to_fountain` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-05-14 eda00563 | live tunable / scene control, 1 code refs |
| `scene_mirrortest` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-06-02 d5592612 | live tunable / scene control, 2 code refs |
| `scene_cloaktest` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-04 9056af06 | live tunable / scene control, 1 code refs |
| `scene_greets` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-06-03 11fadd81 | live tunable / scene control, 1 code refs |
| `scene_conetest` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-06-04 6187e3cb | live tunable / scene control, 2 code refs |
| `scene_displacetest` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-02 24c36140 | live tunable / scene control, 1 code refs |
| `scene_chase` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-29 7f4e3f5e | live tunable / scene control, 1 code refs |
| `chase_start` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-07-29 7f4e3f5e | live tunable / scene control, 2 code refs |
| `chase_event_test` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-07-12 30a9c2e5 | live tunable / scene control, 1 code refs |
| `mat_probe` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-06 247c62c1 | live tunable / scene control, 1 code refs |
| `xfrm_par` | INT | `-1` | KEEP-TUNABLE | 1 | — | 2026-08-06 33b044f0 | live tunable / scene control, 1 code refs |
| `mirror_clone_tight_bsphere` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-06 964bf1d1 | live tunable / scene control, 1 code refs |
| `pom_prism` | INT | `0` | KEEP-TUNABLE | 3 | — | 2026-08-06 e144205c | live tunable / scene control, 3 code refs |
| `pom_prism_free` | INT | `0` | KEEP-TUNABLE | 1 | 1 DEMO/MeshOps.cpp:9595(L3) | 2026-08-07 c5bf8ec4 | live tunable / scene control, 1 code refs |
| `pom_prism_march` | INT | `0` | KEEP-TUNABLE | 3 | — | 2026-08-07 eabb28ed | HAND-ADJUDICATED: same: an active S1d-5 research A/B (docs/S1D_CLOSED_SHELL_PLAN.md), default 0 and behind --pom_shell + --pom_prism. |
| `repro_from` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-08 cf122881 | live tunable / scene control, 1 code refs |
| `repro_settle` | INT | `8` | KEEP-TUNABLE | 1 | — | 2026-08-08 cf122881 | live tunable / scene control, 1 code refs |
| `repro_seq` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-08 cf122881 | live tunable / scene control, 1 code refs |
| `repro_play` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-08 cf122881 | live tunable / scene control, 1 code refs |
| `repro_max_frames` | INT | `20000` | KEEP-TUNABLE | 1 | — | 2026-08-08 cf122881 | live tunable / scene control, 1 code refs |
| `repro_xres` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-08-08 cf122881 | live tunable / scene control, 2 code refs |
| `repro_yres` | INT | `0` | KEEP-TUNABLE | 2 | — | 2026-08-08 cf122881 | live tunable / scene control, 2 code refs |
| `repro_late_cam` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-15 da286d8b | live tunable / scene control, 1 code refs |
| `repro_run_fountain` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-16 44c8aeed | live tunable / scene control, 1 code refs |
| `repro_prescenes` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-08-15 da286d8b | live tunable / scene control, 2 code refs |
| `strict_flags` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-08-08 e31d3122 | HAND-ADJUDICATED: the 'byte-identical' phrase is a HISTORICAL ANECDOTE about a 2026-08-08 incident, not a claim about this flag's arms; --no-strict_flags is a documented escape hatch for scripts that pass foreign argv words. |
| `hdr_metal_kill` | INT | `2` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/DeferredSurfaceKernel.cpp:4179(L2) | 2026-08-08 4d5caba4 | live tunable / scene control, 2 code refs |
| `pom_shell_lid_planar` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-08 f358bed6 | live tunable / scene control, 1 code refs |
| `env_dyn_sched` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-08 3f1c31c1 | live tunable / scene control, 1 code refs |
| `env_dyn_face_budget` | INT | `6` | KEEP-TUNABLE | 1 | — | 2026-08-08 3f1c31c1 | live tunable / scene control, 1 code refs |
| `env_dyn_max_stall` | INT | `3` | KEEP-TUNABLE | 1 | — | 2026-08-08 3f1c31c1 | live tunable / scene control, 1 code refs |
| `env_dyn_offscreen_period` | INT | `30` | KEEP-TUNABLE | 1 | — | 2026-08-08 3f1c31c1 | live tunable / scene control, 1 code refs |
| `chdir_assets` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-08-08 980ca51a | live tunable / scene control, 1 code refs |
| `pom_shell_sil_slack` | FLOAT | `0.0f` | KEEP-TUNABLE | 1 | — | 2026-08-08 b32e214d | live tunable / scene control, 1 code refs |
| `env_mip_chain` | INT | `9` | KEEP-TUNABLE | 2 | — | 2026-08-09 bd6e8060 | live tunable / scene control, 2 code refs |
| `deferred_checker_edge_full` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-08-09 bd6e8060 | live tunable / scene control, 3 code refs |
| `env_map_probe` | INT | `0` | KEEP-TUNABLE | 3 | — | 2026-08-09 bd6e8060 | live tunable / scene control, 3 code refs |
| `env_dyn_face_cull` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-09 88e97b3f | live tunable / scene control, 1 code refs |
| `env_dyn_face_cull_slack` | FLOAT | `15.0f` | KEEP-TUNABLE | 2 | — | 2026-08-09 88e97b3f | live tunable / scene control, 2 code refs |
| `vec_ggx_refine` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-08-09 4f60493e | live tunable / scene control, 1 code refs |
| `init_timeline` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-09 e4b59e30 | live tunable / scene control, 1 code refs |
| `shadow_lightmap_texel_density` | FLOAT | `0.0f` | KEEP-TUNABLE | 3 | — | 2026-08-09 943d6446 | live tunable / scene control, 3 code refs |
| `env_probe_center` | BOOL | `0` | KEEP-TUNABLE | 2 | — | 2026-08-09 5079f6eb | live tunable / scene control, 2 code refs |
| `env_probe_follow_owner` | BOOL | `0` | KEEP-TUNABLE | 3 | — | 2026-08-11 ece0dc27 | live tunable / scene control, 3 code refs |
| `env_probe_follow_budget` | INT | `1` | KEEP-TUNABLE | 1 | — | 2026-08-10 19b14c9b | live tunable / scene control, 1 code refs |
| `env_probe_follow_eps` | FLOAT | `1.0f` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/EnvBake.cpp:1822(L1) | 2026-08-10 19b14c9b | live tunable / scene control, 2 code refs |
| `shard_cone_cull` | INT | `0` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/MirrorShatter.cpp:894(L2) | 2026-08-12 5adcae12 | live tunable / scene control, 2 code refs |
| `greets_displace_seam_weld` | BOOL | `0` | KEEP-TUNABLE | 4 | 1 DEMO/MeshOps.cpp:2713(L1) | 2026-08-11 2b61b85f | live tunable / scene control, 4 code refs |
| `pbrtest_studio` | INT | `0` | KEEP-TUNABLE | 1 | — | 2026-08-11 e3c38276 | live tunable / scene control, 1 code refs |
| `greets_displace_plane_normal` | BOOL | `0` | KEEP-TUNABLE | 5 | 1 DEMO/MeshOps.cpp:4978(L1) | 2026-08-12 7685cc58 | HAND-ADJUDICATED: active round-3 displacement research A/B (doorway-jamb ride direction); the supersede language is about round 2's de-slide. |
| `greets_displace_block_level` | BOOL | `0` | KEEP-TUNABLE | 3 | 1 DEMO/MeshOps.cpp:5639(L2) | 2026-08-17 66438550 | live tunable / scene control, 3 code refs |
| `greets_displace_geom_bisector` | BOOL | `0` | KEEP-TUNABLE | 3 | 1 DEMO/MeshOps.cpp:5418(L2) | 2026-08-17 66438550 | live tunable / scene control, 3 code refs |
| `greets_displace_profile_agree` | INT | `0` | KEEP-TUNABLE | 3 | 2 DEMO/MeshOps.cpp:5573(L2) DEMO/MeshOps.cpp:5574(L2) | 2026-08-18 a384fb7b | live tunable / scene control, 3 code refs |
| `greets_displace_band_ladder` | BOOL | `0` | KEEP-TUNABLE | 3 | 3 DEMO/MeshOps.cpp:4270(L1) DEMO/MeshOps.cpp:4414(L4) | 2026-08-18 4424d888 | live tunable / scene control, 3 code refs |
| `greets_displace_border_mean` | INT | `0` | KEEP-TUNABLE | 3 | 1 DEMO/MeshOps.cpp:4979(L1) | 2026-08-13 705b70da | live tunable / scene control, 3 code refs |
| `greets_displace_border_mean_scale` | FLOAT | `0.40f` | KEEP-TUNABLE | 1 | 1 DEMO/MeshOps.cpp:4980(L1) | 2026-08-12 7685cc58 | live tunable / scene control, 1 code refs |
| `env_dyn_fade` | INT | `16` | KEEP-TUNABLE | 3 | 2 FDS/RENDER/EnvBake.cpp:1820(L1) FDS/RENDER/EnvBake.cpp:2297(L1) | 2026-08-12 51d1d15a | live tunable / scene control, 3 code refs |
| `env_dyn_fade_mode` | INT | `1` | KEEP-TUNABLE | 4 | 3 FDS/RENDER/EnvBake.cpp:1841(L2) FDS/RENDER/EnvBake.cpp:1842(L2) | 2026-08-12 51d1d15a | live tunable / scene control, 4 code refs |
| `vol_cone_solve_vec` | BOOL | `1` | KEEP-TUNABLE | 1 | — | 2026-08-14 f1ffc925 | live tunable / scene control, 1 code refs |
| `shard_cone_cull_margin` | FLOAT | `1.3f` | KEEP-TUNABLE | 2 | 1 FDS/RENDER/MirrorShatter.cpp:885(L2) | 2026-08-12 5adcae12 | live tunable / scene control, 2 code refs |
| `deferred_inline_tile_sem` | BOOL | `0` | KEEP-TUNABLE | 1 | — | 2026-08-12 4d4e1e6c | live tunable / scene control, 1 code refs |
| `deferred_offscreen_tile_px` | INT | `32` | KEEP-TUNABLE | 1 | — | 2026-08-12 4d4e1e6c | live tunable / scene control, 1 code refs |
| `shard_hdr` | BOOL | `0` | KEEP-TUNABLE | 1 | 1 FDS/RENDER/MirrorShatter.cpp:1067(L1) | 2026-08-12 00d28a8b | live tunable / scene control, 1 code refs |
| `env_live_water_mask_bias` | FLOAT | `0.5f` | KEEP-TUNABLE | 1 | — | 2026-08-16 868ba5d8 | live tunable / scene control, 1 code refs |
| `greets_displace_mitre` | BOOL | `1` | KEEP-TUNABLE | 1 | 1 DEMO/MeshOps.cpp:5249(L1) | 2026-08-14 7d0d7a01 | live tunable / scene control, 1 code refs |
