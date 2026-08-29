# OPTIMIZATION BACKLOG

Tracked list of deferred optimizations + measured-quality upgrades so they
don't get lost. Rule for this CPU software renderer (measured): it is
**gather-bound**, not FLOP-bound — but "texture reads are expensive" is NOT a
safe assumption (the env-reflection tap measured ~free). So: **measure each
change** (bench `--snapshot=<scene>@t=<T>@iters=<N>` → `mean ms/iter`,
interleave flag off/on ≥6×, take mins vs the noise floor). Everything here is
behind a default-off flag until measured + look-approved.

Status keys: TODO · IN-PROGRESS · DONE · PARKED (measured not-worth / blocked).

## 2026-08-29f — `TBR-render`'s interior is attributed: 72.5 % transparent deferred lighting, 24.7 % raster, and the three "dead" children were instrumenting the LEGACY path

Full account: `docs/PERF_STATE.md` §00u. **Instrument only, byte-null.**
14/14 pins + `render_gate` 4/4 + `warm_gate.sh --full` 7/7.

**Why the three declared children read 0.000:** `s_xparClearMs/RasterMs/CompMs`
are incremented only inside `RENDER.CPP`'s `unifiedTbr=false` LEGACY block
(`:1207/1218/1252`). city runs the UNIFIED path, so they are never written there
and `addMs` books three honest zeroes. The names were right, the wiring pointed
at the other implementation, and the unified interior (`FILLERS.CPP:2346`) had no
scopes at all.

**The split** (city t=1961; core-ms, thread-summed; `tbr-core` 85.683 against a
9.315 ms wall → **effPar 9.20 of 12**):

| scope | core-ms | % |
|---|--:|--:|
| `tbr-xparflush` | 83.762 | **97.76** |
| — `xflush-composite` | **62.142** | **72.53** |
| — `xflush-raster` | 21.146 | 24.68 |
| — `xflush-clear` | 0.334 | 0.39 |
| `tbr-collect` / `tbr-sprite` / `tbr-walk` | 1.921 | 2.24 |

**Confirmed independently by ablation** (one-line `return` before the composite,
`--hw_prof`): composite is **5.632 ms wall (73.4 %), 0.626 Ginstr (75.8 %),
0.181 Gcyc (76.1 %), IPC 3.459**. Two methods sharing no machinery, agreeing to
three points. Attribution closes to **99.8 %**.

**`hwRead()` is `proc_pid_rusage(getpid())` — PROCESS-WIDE**, so nested
per-worker counters are meaningless inside a strip wave; per-block Ginstr/Gcyc
must come from ablation differencing. Recorded because it constrains every future
sub-scope split in a threaded pass.

### OPEN

1. **The largest block is NOT obviously leverable, and I did not force one.**
   `Render_DeferredTransparentLighting_Tile` runs at **IPC 3.459** (vs
   `renderFrame`'s 3.625) — issue-bound on real work, not stalled. Its pixel
   count is already near-minimal (§00t: the extent bound is 91.58 % live), and
   city runs ONE peel pass, so there is no depth lever. What is left is the
   kernel's own arithmetic, which lives in the greets round's file:
   **measured and handed over, not edited.** The number to hand over:
   **62.1 core-ms / 0.626 Ginstr per frame for 1.578 M live transparent px
   ≈ 36 ns and ~400 instructions per pixel.**
2. **`xflush-raster` (24.7 %) is the second block and it IS mine** (clipper +
   `MekaleleTransparent*`). 21.1 core-ms at **effPar 2.27 — the lowest in the
   split.** Whether that is real serialisation or a short block thinly spread
   across the wave is NOT established; that is the honest next question.
3. **The dead legacy children should be guarded or renamed.** They will keep
   printing 0.000 on the unified path and reading as "measured, empty" when they
   mean "not wired here". Cheap fix; not taken tonight because a table change is
   itself a thing to gate.

## 2026-08-29e — city's `TBR-render` is CLOSED: the census says the bound is already 91.58 % live, so the lever the map named is worth ~0.13 ms

Full account: `docs/PERF_STATE.md` §00t. **No code written; the row is closed
with numbers, which is the outcome the census was there to produce.**

§00l item 8 carried `TBR-render` (6.51 ms) as the last unopened row, noting that
one `--xpar_extent_census` run would decide it. It did:

```
px full=2.43M  bound=1.72M (71.01%)  live=1.58M (91.58% OF THE BOUND)
```

`xpar_strip_extent` **already** cut the scan to 71 % of full width, and what it
scans is **91.58 % live**. The dead scan — the entire addressable prize — is
0.145 M px, **8.42 % of the bound**. Against a measured row of 6.930 ms
(city t=1961, 68.85 ms tick), the ceiling on any tightening is 0.583 ms if a dead
pixel cost the same as a live one (absurd), and **≈0.06–0.19 ms on realistic
assumptions — under 0.3 % of the tick.** The 37 empty flushes (23.4 % of 158) are
already inside that dead total and are worth tens of microseconds.

**STRIKE §00l item 8.** The row is irreducible by its own lever *because the
lever already worked*, and that also explains why the fountain fix measured NULL
here: there was nothing to find.

### FOR WHOEVER OPENS THE ROW ANYWAY

`TBR-render` reads 6.930 ms while its three declared children — `xpar-clear`,
`xpar-raster`, `xpar-composite` — all read **0.000**. The interior instrument
does not cover the deferred/unified-TBR path, so the 1.578 M live pixels (the
only real mass) are unattributed. A sub-scope split is step one for anyone who
wants that mass; the extent bound is not the way in.

## 2026-08-29d — the water scan's analytic reject is clipped away: byte-null, and 26 % off the row at the pose where the pass was 102 % scan

Full account: `docs/PERF_STATE.md` §00s (renumbered from §00r at the merge — the greets round took that letter the same day). **LANDED, byte-null** (14/14 pins +
`render_gate` 4/4 from a stock worktree).

`rowXClip()` in `DEMO/ProceduralWater.cpp`. `D` is affine in x and the keep-test
`sd = K/D in (1,fzp)` collapses to one interval on `D`, so the surviving x-range
is solvable per row in O(1). Applied to both varied glint loops (default and
`--water_glints_batch`). **Byte-null by construction**: bounds widened 2 px, all
per-pixel tests retained, degenerate `fzp<=0` fails OPEN.

Validated two ways: the pins (both arms byte-exact) and a renderer-independent
falsifier — 4 000 random camera configurations, **4 801 816 px, ZERO live pixels
wrongly skipped**, 51 % skipped on average.

Measured, min-of-9, same-binary control at −0.10/+1.00/−0.69 %: t=800 −0.072 ms
(−0.9 %), **t=1105 −0.104 ms (−26.1 %)**, t=1600 −0.195 ms (−1.4 %); batched arm
−0.141 / −0.121 ms. **Predicted ~0.15 ms, measured 0.07–0.20 ms.** A peer's
battery contaminated `__tick` (BOX-AFTER busy=7, load 20.9) so tick is NOT quoted
from this run; the row survived, per the control.

Honest size: 0.2–0.4 % of tick. It lands because it is byte-null and never a
regression, not because the millisecond is impressive.

### OPEN

1. **The same clip fits `RenderGlints` (city/fountain) and was NOT applied** —
   that function is byte-gated by their pins and this file's inlining trap makes
   any edit there a gate risk for a sub-ms return. Mechanism is written; someone
   with a quiet box can take it.
2. **The occlusion reject is the remaining half and needs the Z-buffer** — at
   t=1105 it is 1 132 109 px of the 2 073 600. A per-tile Z-max (hierarchical Z)
   is the only cheap way at it; the water pass has no such structure today.
3. **TOOLING, shared: `pgrep -f` self-matches peer wait-loops.**
   `scratchpad/quietrun.sh`'s box check matched a PEER AGENT'S OWN quiet-wait
   loop (its command line contains the literals `cmake|ninja`), so two waiters
   waited on each other and **neither ever started** — a battery stalled
   silently, looking exactly like a slow run. Both wrappers now use
   `ps -Ao comm=`, which matches the executable NAME and cannot false-match a
   shell. Same class as the documented `pgrep -fl DEMO` self-match, one level up.

## 2026-08-29g — **THE WINDOW LEDGER, and a MERGED REGRESSION found and fixed inside it.** `e017d611` → `78c0a752`: city `renderFrame` −11.6 %, chase −17.7 / −6.6 / −13.1 %, greets −2.0 / −2.9 %

Full tables, method, dup-arm contamination detector and the default-ON vs
flag-gated split: **`docs/PERF_STATE.md` §00v**. Two things belong here.

### The regression — a fan-out was fine, the cache riding with it was not

Round 5's `--city_glass_pool` shipped **two** changes under one flag: a chunked
fan-out of `cityMirrorGlassForward` and a per-mesh `bsWorld` cache. The peer's
`tools/warm_gate.sh`, written hours earlier for exactly this hole, failed
`city-warm` at **tick 4**. Bisected inside ONE suite invocation (so the cross-row
contamination a peer is chasing cannot explain it): tip **FAIL**, cache-only
(flag off) **FAIL**, fan-out-without-cache **PASS**, pre-round-5 **PASS**.
Deterministic — two runs, byte-identical wrong hashes.

**Two lessons, both mine:**

1. **24 repetitions of a blind test is still a blind test.** Round 5 certified
   this "byte-null over 24 consecutive runs, one hash". True, and worthless: all
   24 were ONE-TICK `--snapshot` runs and the defect starts at tick 4. The pins
   have the same blindness by construction, which is why warm_gate exists.
2. **A flag that does not gate the whole change is not a revert arm.** The flag
   controlled the fan-out while the cache ran on *both* paths, so my first
   "flags off still fails" reading pointed away from my own code and nearly
   exonerated it. When shipping two mechanisms, gate them separately or ship one.

The cache is **removed, not repaired** — its failure mechanism is still
unexplained, and the argument for it ("`bsWorld` is a pure function of the mesh")
is one I still cannot fault on paper, which is precisely why it must not come
back without warm_gate green. The site and the flag text both say so.
**The win is fully retained and slightly better: `Tick-ReflGlass` 0.925 → 0.144 ms
(−84.4 %) without the cache, against 0.161 with it.**

`--mirror_rtt_pool` was checked for the same exposure and is **clean** —
greets-warm passes with it ON and OFF, all five warm-tick hashes byte-identical.
It moved only *dispatch* of a pass whose tiles write disjoint regions and added
no cross-item state. **That is the line: re-dispatching disjoint work is safe;
caching across items is what needs the warm gate.**

### The ledger, in one line each

city `renderFrame` **54.264 → 47.952 (−11.6 %)**, tick −9.3 %; chase
`renderFrame` **−17.7 / −6.6 / −13.1 %** at t=800/1105/1600; greets −2.0 / −2.9 %,
tick −2.3 / −3.9 %. Biggest movers: city `lighting-w1` −26.5 %, chase `gbuffer`
−36.9 % (t=800) and `ssao` −26.7 to −32.8 %, greets `RTT` −27.9 % and `ssao`
−11.7 %. **Reported honestly against it:** greets `lighting-w1` **+1.8 %** and
`lighting-w2` **+2.9 %** — real (dup-arm ±0.1 %), unexplained, most likely code
layout, and on the SCALAR kernel no round touched.

## 2026-08-29c — the cross-tree divergence was never a divergence: byte-identical binaries, a self-locating asset root, and pose-sequence-dependent chase pins

Full account: `docs/PERF_STATE.md` §00q. **Zero regressions found; three false
reds explained; the previous round's "treat the installed binary as UNGATED"
recommendation is WITHDRAWN.**

**The build is byte-reproducible across trees** — same commit, same cache, `md5`
equal and `cmp` reports **0 differing bytes**. That one command killed the whole
build-side hypothesis list (`__FILE__`, baked absolute paths, stale objects,
ccache, `-frandom-seed`, submodule skew, `libmodplayer.a`).

**Cause 1 — `ChdirToAssetRoot` (`DEMO/REV.CPP:554`, called at `:1392`).** The
binary resolves its OWN executable path and chdirs to the first `rev.cfg` it
finds in `<exedir>{,/../Runtime,/../../Runtime}`, **silently discarding the
caller's working directory**. So a binary's *file location* selects the assets
and the `rev.cfg`. Gil-Ad's main tree is 1384×768 plus untracked `Runtime/`
drift, so any binary living there mismatches the pins by construction. Proved
both ways: under `FDS_CHDIR_ASSETS=0` the main tree's binary reproduces the
chase, greets and city pins exactly.

**Cause 2 — chase's pins are POSE-SEQUENCE dependent** and it was never written
down. They are pinned from five poses in ONE process; state carries across them.
t=100 alone is byte-identical to t=100 first-of-five, but **t=800 alone differs
from t=800 third-of-five by 434 591 px (20.96 %), max |Δ| 5**. Only the first
pose of a process is sequence-free. This retro-explains and **withdraws two more
alarms I raised the same night** — "a cosmetic string moved the pins" and "the
incremental build is LTO-poisoned" — both were single-pose spot-checks against a
five-pose pin. The supposedly poisoned binary reproduces all five pins under the
canonical recipe.

**Consequence: every byte-identity claim of the 2026-08-28/29 window stands.**
All were taken in a stock worktree from its own build with verbatim recipes.
Re-gated at the merged tip (after `ac34f170`): 14/14 pins + `render_gate` 4/4.
Both causes are now in the `SESSION_STATE.md` gates-table preamble.

### OPEN ITEM

**`ChdirToAssetRoot` should say which root it chose.** It is a feature, not a
bug, and it already has two overrides (`--no-chdir_assets`, `FDS_CHDIR_ASSETS=0`)
— the defect is that it is SILENT, so a gate mismatch caused by it looks like a
render regression and cost this campaign an investigation. One line of provenance
on stderr under an existing diagnostic flag would make it self-diagnosing. Not
taken tonight: a gate-visible print is itself a thing to gate.


## 2026-08-29c — **THE GATE SUITE HAS A STRUCTURAL HOLE, AND IT IS NOW CLOSED**: every pinned row is a ONE-TICK snapshot, so any path that switches on at tick 2 is untested. A binary that deletes city's water fog entirely passes **12/12 pins** and fails **5/7 warm rows**. `tools/warm_gate.sh` DONE

### THE TRAP, STATED ONCE

**A green 13/13 does not mean a path was exercised. Check whether the path is
even reachable at tick 1 before claiming byte-nullity from those rows.**

### DEMONSTRATED, NOT ASSERTED

`-DFDS_FOG_PUNT_PROBE=1` builds a binary that simply does not compute city's
water-reflection fog — the exact shape a botched vectorisation of that leg would
take. Against the canonical suite it is **invisible**:

| suite | result on the deliberately-broken binary |
|---|---|
| the 13 pinned poses | **12/12 PASS** |
| `tools/render_gate.sh` | (unaffected — no water) |
| `tools/warm_gate.sh --full` | **5 of 7 FAIL** |

And the failure signature is the mechanism itself: in *every* failing row the
**first frame's hash matches** and only ticks 2+ diverge.

### THE CENSUS — what tick 1 does not execute

Two distinct classes. **(A) genuinely unreachable** (needs a previous frame's
output), and **(B) deliberately neutered by the snapshot harness**.

| # | path | why it is cold at tick 1 | class | status |
|---|---|---|---|---|
| 1 | city water-reflection leg — `gFrReflZ`, `Froxel_ReflBranch`, `--fog_refl_vec` | `FastFog_SetReflectionZ` is consume-once and only fires from city's reflection pass; city's water carries no mirrored content on tick 1 | A | **MEASURED** — `PUNTED 0` at tick 1, 27.6 % of groups from tick 2 on |
| 2 | froxel temporal EMA **blend arm** — `temporal = gFrTemporal && gFrHistValid` | `gFrHistValid = false` initially (also reset on scene change, grid change, near/far change) | A | **MEASURED, PARTIAL** — the *flag* IS visible at tick 1 because it also enables jitter, so a flag flip would be caught; a change confined to the **blend expression** would not |
| 3 | chunk occlusion cull — `--chunk_occlusion` | doubly cold: `REV.CPP:1629` forces `g_occlSnapshotInert = true` under `--snapshot`, AND its own "first frame of a scene: no capture yet → cull inert" | A+B | **MEASURED that snapshots force it inert.** Pixel effect unconfirmed and *not confirmable by image*: a correct cull is byte-null by design. Needs `--chunk_occl_verify`. Default OFF today → no live exposure |
| 4 | mip hysteresis — `Face::LastMip` | `if (mipHyst > 0 && F->LastMip != 0xFF)`; `LastMip` is unset on frame 1 | A | candidate. `mip_hysteresis` defaults **0.0f** → no live exposure today, but it is a trap the moment anyone dials it |
| 5 | greets code-screen smear — `OldBuf` feedback | iterative smear; a function of how many times `Render()` has run, not of `t` | A | **known** (already a SESSION_STATE trap); now covered by `greets-warm` |
| 6 | xpar peel / strip slices | "whatever the previous frame / the legacy peel left in the xpar layer slices is unknown to the per-strip column tracker" | A | candidate, **low risk** — `XparStripSlices_MarkAllDirty()` runs every frame, so it is self-healing by construction |
| 7 | city env probe cube | disk cache keyed on the FLD, **not** on FeatureFlags; a cold worktree re-bakes | B-ish | **known/documented** in the gates table already |
| 8 | MirrorShatter surface + clone arrays | "kept warm across frames" | A | candidate, **low risk** — allocation lifetime, not content |
| 9 | cross-SCENE state (the fountain→greets peel-floor leak) | a single-scene snapshot never crosses a scene boundary | A | **known** from the prior campaign; still uncovered — no warm row crosses scenes |

Items 1–3 are the ones that have actually cost something. Item 9 is the one
still open: **no gate in this repo runs two scenes in one process.**

### THE KNOWLEDGE EXISTED; THE GATE COVERAGE DID NOT

`./DEMO --help` has said this all along, and it is worth quoting because it
means nobody needs convincing of the mechanism — only of the gap:

> **REPRO** (headless INTERACTIVE harness — use when `--snapshot` CANNOT
> reproduce a defect the user sees in his live run) … Runs the REAL scene
> driver through the REAL per-frame path, scrubbing to t the way F1/F2 does,
> so defects that need **accumulated per-frame state** appear. **`--snapshot`
> renders ONE cold tick with the fine scene clock and the chunk-occlusion cull
> pinned, and is blind to those by construction.**

So `--repro` (and `docs/INTERACTIVE_REPRO.md`) is the sanctioned instrument for
exactly this class, and it names the chunk-occlusion pinning explicitly. **What
was missing was not the tool or the knowledge — it was that not one row of the
gate suite used either.** `tools/warm_gate.sh` closes that with multi-pose
`--snapshot`, which accumulates state across poses within one process (proven:
the punt census reads 0 on pose 1 and 27.6 % from pose 2). A `--repro`-based
row would exercise the real per-frame driver rather than the snapshot driver
and is the natural next extension; I could not get `--repro=city@t=…` to emit
PPMs in the time available, so it is left as a NOTED gap rather than a
half-verified gate row.

### THE COVERAGE THAT NOW EXISTS

`tools/warm_gate.sh` — fast pair (`city-warm`, `greets-warm`) for every gate
check; `--full` is 7 rows in **18 seconds**, so there is no excuse not to run it
before touching the composite, the froxel volume or reflections.
`WARM_GATE_BIN=/path/to/DEMO` retargets it. Baselines recorded at `bc36387b`,
1920×1080, stock `rev.cfg` — same resolution trap as `render_gate.sh`.
Documented as a row in `docs/SESSION_STATE.md`'s gates table with the reason.

### THE SECOND-ORDER COST OF THE HOLE

The same blind spot mis-priced the work. This item was recorded at **0.030 Gi/f
/ 0.72 % of `renderFrame`** from a census taken **cold, at 1512×848**. Warm at
1920×1080 a punted pixel costs ~640 instructions and the item is **5.15 %** —
**7× low**. A cold census does not just fail to catch regressions; it sizes the
backlog wrong.

### NEXT LEVER ON THAT ROW (noted, NOT built)

Vectorising the reflection leg's own arithmetic — everything except
`fastExpNeg` (8-bit LUT) and `fogAntiderivG` (3-way branch). Ceiling
**~0.12 Gi/f**, the residue after 2026-08-29b took 57 % of the punt's cost.
**This one does need the §00k2 contraction rules**, unlike 2026-08-29b which
avoided them by sharing source: `gY = w10*Xc + w11*Yc + w12` and
`uV = Xc*Xc + Yc*Yc + 1` are both exactly rule 2's "A+B+C of products" shape,
which chains from the SECOND term.

## 2026-08-29b — **THE FROXEL COMPOSITE'S SCALAR HALF IS GONE**: `--fog_refl_vec`, 572 648 punted px/frame go 8-wide, `fog-composite` **−27.2 %** and `renderFrame` **−1.40 ms**, BIT-EXACT. And the item was **invisible to all 13 pins**. DONE, merged

`Froxel_CompositeTileVec8` punted a WHOLE 8-lane group to scalar for any
water-reflection lane. Warm at 1920×1080: **71 581 of 259 200 groups (27.6 %) =
572 648 px/frame**, 95.1 % of those lanes genuinely reflective, 64 337 groups
with all eight. Lane-level punting stays refuted (2026-08-16z); the fix was to
stop punting.

### THE TRAP THAT HID IT FOR SEVEN ROUNDS

**The punt never fires on tick 1 of a process.** `--snapshot=city@t=1961`
reports `PUNTED 0`; the census switches on from the second tick, because city's
water has no mirrored content until then. **All 13 pinned poses are one-tick
snapshots, so none of them executes a single line of this code.** They pass
before and after and prove nothing about it. Gate this path on
`--snapshot=city@t=1958,…,1962` and compare the later frames — same family as
the gates table's own "judge city's reflection from a warm multi-tick run".

### PRICED BEFORE BUILT, AND THE BACKLOG'S OWN ESTIMATE WAS 7× LOW

`-DFDS_FOG_PUNT_PROBE=1` (committed, never shipped) skips the punt and runs the
vec path anyway — wrong pixels, but it measures the ceiling: `fog-composite`
0.577 → 0.291 Gi/f, `renderFrame` −2.39 ms. This item was recorded at
0.030 Gi/f / **0.72 %** of renderFrame (1512×848, "92 instr/px"). Warm at
1920×1080 a punted pixel costs **~640 instructions** and the item is
**5.15 %** of renderFrame.

### RESULT — same binary, flag flipped, min-of-11, quiet box, three agreeing batteries

| row | punt | reflvec | delta |
|---|--:|--:|--:|
| `fog-composite` Ginstr/f | 0.5990 | 0.4360 | **−27.21 %** |
| `fog-composite` Gcyc/f | 0.1570 | 0.1160 | **−26.11 %** |
| `fog-composite` wall | 4.874 | 3.482 ms | **−1.39 ms** |
| `fastfog` wall | 9.258 | 7.857 ms | **−1.40 ms (−15.1 %)** |
| `renderFrame` Ginstr/f | 5.5910 | 5.4280 | −2.92 % |
| `renderFrame` wall | 49.357 | 47.956 ms | **−1.40 ms (−2.84 %)** |
| `cones-call` / `fog-columns` | — | — | **0.00 % (controls)** |

**57 % of the probe's ceiling.** The residue is the scalar leg, ~215 instr per
reflective pixel.

### HOW BIT-EXACTNESS WAS WON — by sharing source, not by transcribing

The leg is lifted verbatim into `Froxel_ReflBranch` and called by BOTH paths, so
there is no second spelling and nothing is re-associated — no contraction
archaeology was needed. Three things stay scalar-per-lane, each with precedent
in this same function: the leg (`fastExpNeg` is an 8-bit LUT, `fogAntiderivG` a
3-way branch), the LOG slice index, and the `reflAmt2` fold (done in the spilled
domain so the expression is literally the same one). What went 8-wide is what
always cost: the four bilinear froxel gathers and the Beer-Lambert tail.

**One hoist worth its own line:** `hb` was calling `fastExpNeg` once per
reflective PIXEL for a per-frame value. `FroxelReflConst` lifts it plus
`sigma*meanD` (tau's FIRST product, so nothing re-associates) — 1.7 points of
`fog-composite` by itself.

**28 warm frames byte-identical** against a HEAD-built parent across six arms
(his arm ×2, plain `--deferred`, `--hdr --hdr-linear`, `--fast_fog_dist_dim`,
and `--no-fog_composite_tile_align8` for the scalar-tile path), plus 13/13 pins
and `render_gate.sh` 4/4.

### NEXT ON THIS ROW

Vectorising the leg's own arithmetic (everything but `fastExpNeg` /
`fogAntiderivG`) would attack the remaining ~215 instr/px. **That one DOES need
the §00k2 contraction rules** — `gY = w10*Xc + w11*Yc + w12` and
`uV = Xc*Xc + Yc*Yc + 1` are both "A+B+C of products", the exact shape rule 2
covers. Ceiling on it is ~0.12 Gi/f.
## 2026-08-29b — chase round 2: water-glints decomposed and ported, and chase's sky turns out to be painted by the reflection pass

Full account: `docs/PERF_STATE.md` §00o (renumbered from §00n at the merge -- the SSAO round took that letter the same day).

**LANDED, byte-null (all 14 pins + render_gate 4/4).** `--water_glints_batch`,
**DEFAULT 0**. Batches chase's varied glint pass the way `RenderGlints` has been
batched since the city round. Same-binary flag flip, min-of-9, quiet box:
`water-glints` **−30 % at every watered pose** (t=400 −2.397, t=800 −2.401,
t=1600 −4.186 ms), tick **−4.2 % / −5.6 % / −11.1 %**. t=1105 is +0.022 ms — 691
live water pixels, nothing to batch. **OFF by default because it is not
byte-null: 450/304/241/76/2934 px move at the five pins, every one by max |Δ| 1.**

### OPEN ITEMS

1. **`water-glints` at chase t=1600 is 14.296 ms — 33.6 % of that pose's whole
   tick**, nearly double t=800's 8.089. §00m sampled t=800/t=1105 only and
   under-described the row by 2×. Any future chase map must include t=1600.

2. **The batched port's last LSB.** 241 px at t=800, bisected to the slope
   kernel (not the caustic tap, not the specular tail). Five pinning attempts
   logged in §00o with their pixel counts. It cannot be closed by copying
   `waterWaveSlope8`'s spelling because `waterWaveSlopeVaried` gets a different
   fmadd chain from clang. **Next attempt starts with that function's
   disassembly, not another guess.** Closing it flips the flag's default.

3. **The scan pre-reject.** The pass ray-casts all 2 073 600 pixels every frame:
   **0.461 ms at t=800 (5.7 % of the row) and 0.400 ms at t=1105, where it is
   102 % of the row** — the pass spends its entire time finding 691 pixels.
   A conservative per-tile bound (§00f's shadow-reject shape) is byte-null by
   construction and worth ~0.4 ms at water-poor poses.

4. **What is left in the row is a LOOK call, not a perf item.** After the port,
   t=800's shading is caustics **30.2 %** + four libm `cos`/`sin` **24.4 %** =
   54.6 % of the row, and neither can be reduced without changing how the water
   looks. Parallel efficiency is already 10.0 of 12; this is not a threading row.

5. **CORRECTION to §00m, and it matters: chase's SKY is painted by the
   reflection pass.** §00m blamed the sky movement under `--refl_skip_cones` on
   additive HDR accumulation. That is refuted — `Hdr_BeginFramePass` zeroes
   `g_hdrBuf` every pass and the tonemap writes every pixel ungated. The truth is
   structural: both `Render()` calls target the same VPage, and the engine
   classifies `zEnc == 0` as "sky OR the water's reflection underlay"
   (`DeferredFastFog.cpp:2220`) — **one pixel class** — which reach `g_hdrBuf`
   only via the VPage lift. So the main pass lifts the reflection's sky and ships
   it. `--refl_skip_post`, a reflection-only flag, moves **713 917 of 768 000 sky
   pixels (93.0 %)**, and the red stops exactly at the terrain silhouette:
   `docs/img/chaserefl/skyleak_post_t000800_where.png`. **Not a lifetime bug — a
   classifier defect**; the correct predicate (ray-cast to the water plane)
   already exists in `ProceduralWater.cpp`. Visible harm today is small (mirrored
   beams wash the sky at ≤4/255, zero pixels reach 8/255); the hazard is that any
   change to the reflection pass silently repaints the sky. Not fixed: a fix
   changes where the sky comes from, so it cannot be byte-null.


## 2026-08-29 — **THE MOVEMASK SWEEP (only the cone kernel converts) and `lightSphereScreenRect` PROVEN TO DROP REAL LIGHT (max 5/255, 6 of 39 poses)**. Sweep DONE / bug REPORTED, not fixed

### 1. `anyLane_x8` engine-wide — bit-exact, and a refutation of my own pick

`simdAnyLane_ps8` / `simdAnyByte_epi8` / `simdAllBytes_epi8` now live in
`DeferredCommon.h`. Eight further sites converted (`DeferredFastFog.cpp`
:1424/:1473/:1502; `DeferredSurfaceKernel.cpp` :3512/:6333/:6335/:6946/:7074);
`DeferredSurfaceKernel.cpp:2029` left alone (it needs the BITS, and is compiled
out of shipping); `DeferredSSAO.cpp` left to its owner.
`-DFDS_SIMD_ANYLANE=0` is the exact pre-sweep arm.

**A second, previously unrecorded lowering.** `_mm256_movemask_epi8` is NOT the
25-instruction `ushl.4s` sequence — it is `cmlt.16b` + `and.16b` against a
bit-weight table + `tbl.16b` + `addv.8h` per half, ~12–13 instructions.
Opcode diff of `Render_DeferredLighting_Tile_OuterVec`: `addv.8h` 8→0,
`tbl.16b` 8→0, `cmlt.16b` 6→0, `bics` 4→0, `mov.16b` 90→74, vs `uminv.16b`
0→4; total 4287→4237.

**Measured, city t=1961, two independent interleaved min-of-11 batteries:**

| row | Ginstr/f | Gcyc/f | wall |
|---|--:|--:|--:|
| `cones-call` | −4.42 % / −4.47 % | −5.95 % / −1.70 % | **−5.12 % / −5.21 %** |
| `lighting-w1` | −1.93 % / −2.02 % | **+0.71 % / +1.43 %** | +0.14 % / −0.51 % |
| `fastfog` | −0.28 % / −0.37 % | −0.34 % / +1.02 % | +0.11 % / −0.14 % |

**Only the cone kernel converts.** `lighting-w1` reproducibly sheds ~2 % of its
instructions for zero cycles — IPC 4.08 → 3.94. Those slots were not the
constraint, and `uminv.16b` has much the same latency as the `tbl`/`addv` chain
it replaces, so the critical path is unchanged. `HW_PROFILING.md:1011-1014`
landing a second time. The fog sites are cold (per-column-block, not per-lane).
Kept anyway — bit-exact, free, one spelling engine-wide — but the header
comment carries this table so nobody sizes a plan on it.

**NOT swept, on purpose:** `_mm256_testz_si256` / `_mm256_testz_ps` is the same
family (simde → `vandq_s64` + two `vgetq_lane_s64` per half, ~10–12 instr, four
vector→GPR moves, against three for `vmaxvq_u32(vandq(…)) == 0`). Ten live
sites, all `DeferredSurfaceKernel.cpp` (:6691 :6739 :6758 :6796 :6886 :6948
:6950 :6951 :7078), and :6758 is on the per-(group × light) hot path. Plus
VCL's `horizontal_or`/`horizontal_and` in `TheOtherBarry.h`, `ShadowMap.cpp`,
`Mekalele.h`, which compile to exactly this. **Left because the batteries above
measured this kernel's response to removing this exact class of instruction and
it was zero.** Also spotted: `:6948`'s `!testz(m,m) && simdAllBytes_epi8(m)` is
redundant in its first term (all ⇒ nonzero) and could be deleted — also for no
measurable time.

### 3. C6 (midpoint closed-form W²/D·W) — BUILT, MEASURED, REJECTED

`FDS_CONE_MID_CLOSEDFORM`, compiled out in place as the record. The premise
was sound and the conclusion still wrong. B11 refuted this form at the
8-segment site because that loop runs on 8.1 % of chase's pairs; the midpoint
block runs on every ALIVE pair (75.4 % of city's, post-`--cone_hull_rect`), so
the fire-rate objection genuinely does not transfer.

**It is a LOSS on every column** — city t=1961, two binaries in one worktree,
interleaved min-of-11, quiet box, within-arm spread 0.05–1.8 % (cleanest
battery of the round):

| row | explicit W | closed form | delta |
|---|--:|--:|--:|
| `cones-call` Ginstr/f | 1.838 | 1.864 | **+1.41 %** |
| `cones-call` Gcyc/f | 0.461 | 0.486 | **+5.42 %** |
| `cones-call` wall ms | 13.336 | 13.999 | **+4.97 %** |

**The disassembly gives the mechanism, and the arithmetic saving is real —
it is just bought with spills.** In `Render_VolumetricCones_Tile`:
`fmla.4s` 114→110, `fmul.4s` 198→196, `fsub.4s` 64→62, `fneg.4s` 7→5 = **−10
vector-ALU ops, exactly as predicted**; against `ldr` 925→940 and `str`
606→620 = **+29 stack accesses**, total 4665→4682. Holding `vUv_v` / `vVP_v` /
`vPP_v` plus the three helpers live across the whole atan+integral block down
to the midpoint costs more registers than the block saves ops, and on arm64 an
`__m256` is TWO of the 32 v-registers. **B8's lesson again: in this kernel
register pressure beats op count.** It is also a re-association (18 px at city
t=1961, 45 at t=400, max |Δ| 2/255; chase and greets byte-null) — but the
pixels never became the question.

**Consequence for the ranked list:** with C1/C2/C3/C4/C5/C6 all resolved, the
only unbuilt candidate left from `PERF_CONES_ANALYSIS.md` is C8 (depth-sliced
tile-vs-cone cull), whose prize overlaps almost entirely with C5 — and C5 has
now taken roughly half of §3.1's 12 % perfect-cull ceiling on its own. **The
cone pass should be considered closed at the bit-exact level.** What remains
is the two look flags, which are Gil-Ad's call, not an engineering question.

### 2. `lightSphereScreenRect` — the bug is real, and it is 5/255

`--light_rect_exact` (default OFF) + `FDS_LIGHTRECT_AUDIT=1`. The shipping
small-angle rect drops (light × tile) pairs in **every scene**: city t=400
189 pairs / 5.2 % over 25 lights, greets t=5743 264 / 2.9 % over 31, fountain
21 / 2.6 %, chase 28 / 0.9 %, city t=1961 14 / 0.35 %.

Worst light any pixel of a dropped tile can be missing — derived from the
kernel's exact `falloff = 1 − dist/range` and each tile's nearest pixel to the
light's projected centre — is **1.1 % to 12.7 % of that light's full
brightness**. (The edge-of-rect form reads up to 23 % and is the wrong bound to
quote: a dropped tile lies wholly beyond that edge.)

**39 poses swept, 6 differ**, all strictly brighter under the exact bound:
city t=700 (161 px, max 2), t=1000 (259, 2), t=200 plain `--deferred` (428, 3),
t=500 (60, 2), fountain t=1500 (**359, max 5**), t=3500 (629, 3). All 13 pinned
gate poses are byte-identical, which is why seven rounds never saw it.

At fountain t=1500 the delta is **362 brighter pixels and 0 darker**, in a band
`x[755..878] y[945..949]` — 124 px wide, **5 rows tall**. A tile-boundary
sliver: the "missing light on the rect" shape, same family as `eb36c1fd`, at
2–5/255. `docs/img/conesimpl/lightrect_fountain_t1500_*` (24× amplified; at 1×
there is nothing to see).

**Verdict: reported, not fixed.** Real defect, real light on the floor, but
bounded at 5/255 on 0.03 % of pixels, and flipping it is still a look change on
pinned scenes. The flag ships OFF; what actually got corrected is the SOURCE
COMMENT, which asserted "slightly over-estimates … no correctness impact" and
now says the opposite with the reproduction recipe.
**Gil-Ad's call whether 5/255 of restored light is worth a re-pin.**
## 2026-08-29 — chase round: the tile grid lands, the reflection pass is priced, and two things turn up that are not perf

Full account: `docs/PERF_STATE.md` §00m. Branch `rev-chaseperf`, merged tip.

**LANDED.** `frame_tile_y` 5→20 in chase only (`createChaseScene`, restored in
`cleanup()`): `gbuffer` −3.11 ms (−36 %), tick −3.17 ms; the revert arm measures
+3.51 ms (+6.8 %) at t=800 on the merged tip. Moves pixels (23 866 px / 1.15 % at
t=800 on his arm, 199 px above 16/255 in 2 M). **Revert is one flag,
`--frame_tile_y=5`, and it is exact byte-for-byte.**

**RETURNED AS A MENU, NOT MERGED** — five `--refl_skip_*` flags, all default 0,
byte-null at their defaults. The reflection pass is **17.38 ms of a 49.96 ms
tick** at t=800 (§00l's estimate of ≈11.7 ms was 56 % LOW). Best offer on the
menu is `--refl_skip_cones`: −6.41 ms (12.8 % of tick) for a max change of
**6/255**. His call, uncommissioned.

### OPEN ITEMS THIS ROUND CREATED

1. **`water-glints` is chase's next target — 7.999 ms, 16.0 % of the t=800 tick**,
   `DEMO/ProceduralWater.cpp:785,884`, and it sits OUTSIDE `renderFrame` (it runs
   in the scene's own tick, so no PassTag row covers it). It swings **20×** across
   poses — 7.999 ms at t=800 vs 0.397 ms at t=1105 — which points at work scaling
   with visible glint area. Never examined. After the reflection pass it is the
   largest single row in the scene.

2. **CORRECTNESS, small: the reflection pass's additive cone radiance leaks into
   the main view.** At chase t=800, `--refl_skip_cones` moves 224 466 pixels in
   the SKY (71.6 % of all its moved pixels), above the horizon where no water is
   drawn, at max 4/255. The signed delta is **positive everywhere and negative
   nowhere** over those pixels — removed additive light, not a reordered sum. The
   control is `--refl_skip_ssao`, which moves **0 px** above the horizon, so
   `ReflUnderlayScope` is sound and the leak is specific to the additive
   cone/halo composite into the shared HDR buffer. Not fixed here (amplitude is
   ≤4/255 and the cone files were being worked by another branch this round).

3. **Latent, guarded, currently harmless: screen-space rain would run in the
   reflection pass.** `RENDER.CPP:1406` documents that rain must never run there
   ("would rain upward in the water") and implements the guard for
   `skipVolumetric` — which chase does not pass. Chase therefore runs it. Measured
   0.0000 ms and byte-identical at both poses because chase has no rain armed, so
   it costs nothing today; it becomes a visible bug the moment rain is armed in a
   two-pass scene. `--refl_skip_rain` is the guard.

## 2026-08-28 — **THE CONE PASS, ROUND 8**: city's `cones-call` **15.32 → 13.38 ms BIT-EXACT** and **→ 3.09 ms (−79.8 %) with the two look flags on**. Two landings, one refutation, and two bound defects found in code nobody was auditing. DONE / his call

Implementation round against `docs/PERF_CONES_ANALYSIS.md` (branch
`rev-conesimpl`). Arm throughout: city t=1961, `--env_live_water --deferred
--city_env_pixel`, 1920×1080, `--profiler=0 --deferred_prof=1`, interleaved
min-of-11, **zero other DEMO processes on the box before and after every
quoted battery**. `Ginstr/f` is quoted only as the deterministic column
(within-arm spread 0.05–0.32 %); cycles and wall lead.

| lever | status | `cones-call` Gcyc/f | wall ms | Ginstr/f | pixels |
|---|---|--:|--:|--:|---|
| round baseline | — | 0.525 | 15.32 | 2.081 | — |
| **C4** `anyLane_x8` | **LANDED, bit-exact** | 0.475 (−6.1 %) | 14.26 | 1.971 (−5.3 %) | none |
| **C5** `--cone_hull_rect` | **LANDED, default ON, bit-exact** | 0.424 (−8.2 % more) | 13.38 | 1.838 | none |
| **C2** `--cone_half_y_wide` | built, **default OFF — his call** | 0.229 (−50.4 %) | 6.92 | 0.946 | 0.37 % of px, max 5/255 |
| **C1** `--cone_range_cull=0.5` | built, **default OFF — his call** | 0.099 (−78.6 %) | 3.09 | 0.412 | 19.3 % of px, max 4/255 |
| C3 headlight-pair merge | **REFUTED, not built** | — | — | — | — |
| C7 drop the Newton step | not built (precision call, his stack) | — | — | — | — |

`renderFrame` (×2) at 1920×1080: 53.29 → 52.24 ms bit-exact, → 42.02 ms
(−21.1 %) with both look flags.

### The two bound defects, which are worth more than the milliseconds

* **`lightSphereScreenRect` (`DeferredCommon.h`) is NOT conservative**, and
  its own comment claims the opposite ("slightly over-estimates near the FOV
  edges"). The small-angle form `rx = r·fovX/vz` under-estimates an off-axis
  sphere's silhouette. Measured: at city **t=2400 the exact tangent bound
  admits 3 013 alive (lane × spot) pairs the shipping sphere rect was
  dropping.** It is byte-null at the city pins only because those pairs
  contribute sub-LSB. **This helper is also used by `buildTileLightLists` and
  `buildStripLightLists`** — the same under-estimate is live in the tiled
  LIGHTING cull, where the contributions are not sub-LSB. NOT audited this
  round. **TODO, and it is a correctness item, not a perf item.**
* **A cone whose apex is behind the near plane has no finite screen rect.**
  Perspective projection maps a 3-D convex hull to the hull of the projections
  only for sets strictly in front of the eye. The first `--cone_hull_rect`
  build fell back to the base ball's rect there, and chase caught it: 4 of 5
  pinned poses moved, 62 589 / 24 249 / 27 387 / 228 919 px, **and every
  changed pixel got DARKER, 0 brighter anywhere** — the signature of a leaking
  cull, and the reason the delta's SIGN is worth measuring, not just its size.

### C3 — refuted by its own falsifier before a line was written

`FDS_CONE_PAIRCENSUS=1` (env-gated, kept). The analysis assumed **f = 0.7** of
the 23 L/R pairs merge at K = 1 px. Measured f at K = 1: **0.087 / 0.304 /
0.000 / 0.087 / 0.000** at t = 1961 / 400 / 2400 / 1000 / 3000 — mean 0.096,
**zero at two of five poses**. Worse, the pairs that DO merge are the distant
vehicles, whose beams are cheap per spot; the near cars that dominate the cost
have apexes tens of pixels apart. K = 4 px only reaches f = 0.165, by which
point the apex is displaced 2 px and the "structurally invisible" argument has
failed. Nothing here.

### Corrections to `docs/PERF_CONES_ANALYSIS.md`, for whoever reads it next

1. **C1's "sub-LSB is free" argument is wrong as stated.** `VolCompositeAdd`
   truncates ONCE, on `accB/G/R[lane]` — the sum over every spot on the pixel,
   not per spot. One removed sub-LSB term is enough to flip the floor. C1 is
   therefore never byte-null below k = 1, which is exactly what the sweep
   found (2.79 % of pixels move at k = 0.9).
2. **C1's k² tile-entry falsifier is refuted; the lever survives anyway.**
   Tile-entries fall as **k^0.79** (1187 → 981 → 830 → 687 → 558 at k = 1.0 →
   0.4), because a spot's footprint cannot shrink below one 160×135 tile. The
   win comes from lanes dying earlier INSIDE retained tiles: dead pairs 39.9 %
   → 64.3 % and alive lane×spot −65.9 % at k = 0.5.
3. **C5 was under-called at 2.4–3.2 %; it measured −7.7 % cycles.** The
   analysis capped it at §3.1's 12 % "perfect geometric cull" bound; the hull
   reaches half of that alone (dead pairs 1 279 130 → 628 432).
4. **C4 was under-called at 1.5–4 %; it measured −6.1 % cycles.** Its "25
   instructions per site" reading of the disassembly is CONFIRMED (opcode
   diff: `ushl.4s` 2→0, `ext.16b` 4→0, `orr.8b` 2→0, `mov.s` 34→28, `orr`
   25→15, `ldr` 943→918).

### Carried forward, not done this round

* **The simde `movemask` lowering defect outside the cone pass.** Round 7's
  B12 audit never covered it. The pattern is live elsewhere in the tree —
  `Render_DeferredFastFog`'s lambda carries 2 `ushl.4s`, the `meka` and
  `barry` tile rasterizers carry 26 and 2–6 each, `computeMirrorPresenceGrid`
  27, `bilinear_sample_x8` 4. Not all of those are `movemask` (some are real
  shifts) and none were touched. **Worth one sweep: `anyLane_x8` is a
  10-line, bit-exact, control-flow-only change and it paid 6 % on the first
  loop it was tried in.**
* **C6** (closed-form W²/D·W in the midpoint block) — 1–3 %, re-association,
  moves bytes. Unbuilt; still the best remaining candidate on the ranked list
  now that C1/C2/C3/C4/C5 are resolved.
* **C8** (depth-sliced tile-vs-cone cull) — overlaps almost entirely with C5,
  which has now taken half the available cull prize. Marginal value ≤ 2 %.
## 2026-08-28 — **CITY'S OUTER-VEC WAVE-1 KERNEL, THE FIRST ROUND EVER RUN ON IT: `lighting-w1` −24.2 % instructions, `renderFrame` −5.7 %, and the biggest candidate on the list is KILLED BY ITS OWN CENSUS.** DONE (six levers, all byte-null, four dialable)

**THE TARGET.** `Render_DeferredLighting_Tile_OuterVec`
(`FDS/RENDER/DeferredSurfaceKernel.cpp`) — the kernel city, fountain and crash
select through `Scene::PreferOuterVec`. It is vectorised over PIXELS (8 px × 1
light), takes **no shadow tap of any kind**, has no GGX lobe (so `--pbr` is
structurally inert in it), and had **no instrument at all**: `FDS_OMNI_ABLATE`,
`FDS_PIX_ABLATE`, `FDS_W2_ABLATE` and `FDS_W1LDR_ABLATE` all live inside
`TileT` / `TileFill`, which this kernel never enters. Read-only analysis:
`docs/PERF_LIGHTINGW1_ANALYSIS.md` (branch `rev-w1analysis`); implementation:
branch `rev-w1impl`.

### The instrument, built first

`-DFDS_OVEC_ABLATE=n` (11 per-group stages), `-DFDS_OVEC_OMNI_ABLATE=n` (5
per-light stages), `-DFDS_OVEC_CENSUS=ON` (runtime `--omni_census`). All
compile-time with `if constexpr` and a cumulative sink, for the reason 16l
documented: a *runtime* predicate in this body cost **+4.3 % with the flag OFF**.
The shipping build emits none of it. `--deferred_gloss_stats` also gained a map
census (materials / normalMap / roughMap / metalMap / Reflection>0) rather than
a new flag.

### The ladder — city t=1961, 1512×848, `--env_live_water --deferred --city_env_pixel`

Row = **0.955 Gi/f** on the C1+C8 build (the analysis said 0.957 — it reproduces).

| stage adds | Gi/f | Δ | % of row |
|---|--:|--:|--:|
| z + alive/in-range masks | 0.016 | 0.016 | 1.7 |
| + mat32 / matID / bounds | 0.016 | 0.000 | 0.0 |
| **+ the per-lane material gather** | 0.108 | **0.092** | **9.6** |
| + 8-wide octahedral normal decode | 0.122 | 0.014 | 1.5 |
| + nmap / TBN lane loop + SH | 0.142 | 0.020 | 2.1 |
| + view-space position reconstruct | 0.149 | 0.007 | 0.7 |
| + texel/ambient loads, masks, anyVecLane | 0.174 | 0.025 | 2.6 |
| **+ THE OMNI LOOP** | 0.650 | **0.476** | **49.8** |
| + 250 saturate + tex·lit/256 | 0.673 | 0.023 | 2.4 |
| + needsScalar + 11 scratch store-outs | 0.681 | 0.008 | 0.8 |
| + 8-wide env front-end | 0.718 | 0.037 | 3.9 |
| **+ the per-lane PACK loop (full)** | 0.955 | **0.237** | **24.8** |

The analysis's static estimate of ~47 % for the omni loop was right. **Its ~5 %
for the pack was not** — the pack is a QUARTER of the row, because 36 % of
city's alive lanes carry an env store and take the scalar env compose
(`EnvCubeFetchBil` ×1–2 + face pick + live-water tilt) inside it. That is the
biggest un-attacked block left in this kernel and it belongs to the next round.

### The census — three poses, and it settles the ranking

`--omni_census` on a `-DFDS_OVEC_CENSUS=ON` build, city, his arm:

| number | t=400 | t=1961 | t=2400 | decides |
|---|--:|--:|--:|---|
| material-**uniform** groups | 94.9 % | 95.0 % | 95.3 % | **C3** |
| … uniform AND all 8 lanes alive | 91.0 % | 90.6 % | 87.8 % | C3's actual path |
| `testz(omni_lane)` — light reaches NO lane | **39.6 %** | **37.4 %** | **28.5 %** | **C4** |
| **f** = lanes wanting the scalar redo | **0.91 %** | **3.25 %** | **1.06 %** | **C6 — KILLED** |
| … of those, lanes that DUPLICATE a vec diffuse | 0.06 % | 0.30 % | 0.01 % | C6 again |
| all-plain groups (no scalar, no env lane) | 61.0 % | 57.7 % | 63.1 % | **C7** |
| alive lanes carrying an env store | 35.3 % | 36.2 % | 33.0 % | the next round |
| lights / tile · spots / tile | 15.2 · 4.0 | 23.8 · 6.6 | 11.8 · 1.2 | C5 |

**C6 IS CLOSED BY CENSUS, and with it the C6a look call.** The analysis set its
own kill line at *f* < 0.08 and predicted ~18 % of the row at *f* = 0.30;
measured *f* is **0.0091 to 0.0325**, 2.5–9× under the line. Worse for the
candidate: the redo only *duplicates* a vec diffuse in a group that also has a
vec lane, and that is **0.01–0.30 % of alive lanes** — the kernel's own
`anyVecLane` early-out (`needVec = ~needScalar & alive`, `omniLoopN = 0` when
empty) already collects the all-spec case. **C6a — unifying the vec
`_mm256_rsqrt_ps` with the scalar `fast_rsqrt`, which would move every city
pixel — is therefore NOT WORTH PROPOSING as a perf item.** It remains a
standalone correctness/look question for Gil-Ad and `SHADING_CONTRACT.md`: the
two paths still disagree by up to ±0.3 % with LUT-cell stepping. Not built, not
landed.

Also settled: the `×2` on `PERF_STATE.md`'s `lighting-w1` row is **two
main-view-sized dispatches per frame** (city's mirrored water pass, then the
main view). The census prints one block per dispatch; the mirrored one carries
**zero** env lanes and a much higher `testz` rate (56.2 % at t=1961).

### What landed

Six levers. **All six are byte-null**; C1 and C8 ship flagless (16h/16m/16o
precedent: a dial costs more than the mechanism), the four in the hot loop carry
dials so a future round can A/B them on ONE binary.

| # | lever | flag | mechanism |
|---|---|---|---|
| C1 | publish `sEnvVecDiagOff` + hoist 4 per-group flag reads + `testz`-gate the env uniformity scan | *flagless* | 16m's defect verbatim: a function-local `static` with a dynamic initializer **inside the per-8-pixel-group loop** forces `bl __cxa_guard_acquire` into the function |
| C8 | skip the per-lane nmap/TBN loop and its store/reload when the scene has no normal map | *flagless* | `ctx.anyNormalMap`, one table scan per frame beside `shadowSkipMask`. **City: 138 materials, ZERO normal maps** (also zero rough, zero metal, 78 Reflection>0, 14 Specular>0 all at gloss 64) |
| C4 | `testz(omni_lane) → continue` | `--deferred_ovec_light_skip` | 28–40 % of (group×light) pairs reach no lane; the three rejects are spatially correlated across 8 adjacent pixels |
| C3 | ONE `Material` walk per uniform group + vec texel unpack | `--deferred_ovec_mat_uniform` | 95 % of groups uniform; uniformity tested on `m >> 20` (matID **and** mip) |
| C2 | template the dead mirror compare out of the light loop | `--deferred_ovec_nomirror` | both operands provably constant zero; predicate asserted BOTH ways, dispatched by generic-lambda tag so it is outside the loop |
| C7 | one 256-bit store for an all-plain group | `--deferred_ovec_vec_pack` | 58–63 % of groups; clamp in FLOAT before `cvttps` |

### Measured — predicted vs measured, min-of-5 interleaved, Ginstr floor ±0.14 %

Per-lever, **ONE binary, flag flips**, city t=1961; ALL-OFF arm = 0.983 Gi/f:

| lever | predicted (analysis) | **measured, % of row** | % of `renderFrame` |
|---|--:|--:|--:|
| C4 | 2.4–10 % | **−9.2 %** (0.983→0.893) | −1.9 % |
| C3 | 4.8 % | **−8.0 %** (0.983→0.904) | −1.7 % |
| C2 | 4.1 % | **−4.8 %** (0.983→0.936) | −1.2 % |
| C7 | 1.9 % | **−2.7 %** (0.983→0.956) | −0.6 % |
| all four | — | **−24.6 %** (0.983→0.741) | −5.8 % |

Nearly perfectly additive (singles sum 0.243, combined 0.242). **Every one beat
its prediction**, and C4/C3 beat theirs because the ladder shows the omni loop
and the gather are both bigger than the static estimate assumed.

End-to-end against the **fog-wt parent `e017d611`**, both binaries built in this
one worktree, interleaved, min-of-5:

| pose | `lighting-w1` Gi/f | | `Gcyc/f` | | IPC | `renderFrame` Gi/f | |
|---|--:|--:|--:|--:|--:|--:|--:|
| city t=1961 | 0.978 → **0.741** | **−24.2 %** | 0.244 → 0.187 | −23.4 % | 4.01 → 3.96 | 4.183 → **3.945** | **−5.7 %** |
| city t=2400 | 0.390 → **0.328** | **−15.9 %** | 0.104 → 0.080 | −23.1 % | 3.75 → 4.10 | 2.255 → **2.191** | **−2.8 %** |
| city t=400 | 0.734 → **0.538** | **−26.7 %** | 0.185 → 0.141 | −23.8 % | 3.97 → 3.81 | 3.175 → **2.978** | **−6.2 %** |
| fountain t=2500 | 0.105 → **0.088** | **−16.2 %** | 0.029 → 0.025 | −13.8 % | 3.62 → 3.52 | 1.060 → 1.043 | −1.6 % |
| **greets t=5743 (CONTROL)** | 1.477 → **1.477** | **0.00 %** | 0.382 → 0.379 | −0.8 % | 3.87 → 3.90 | 3.643 → 3.642 | −0.03 % |

**Cycles agree with instructions and IPC barely moves** — this is not the cube-
prepass trap where instructions and cycles told different stories. greets is
*exactly* flat, which is the control that proves the change is confined to the
OuterVec kernel and does not touch the scalar wave-1 kernel greets and chase run.

### Refuted / not landed, and why

* **C6 (specular-only redo lane) + C6a (rsqrt unification)** — killed by census,
  see above. *f* = 0.91–3.25 % against its own 8 % kill line.
* **C1+C8 in isolation — MEASURED ON ITS OWN BINARY, and 16m's mechanism
  reproduces.** A dedicated build of `187355b5` against the parent, interleaved,
  min-of-5: city t=1961 `lighting-w1` **0.977 → 0.953 Gi/f (−2.5 %)** but
  **0.243 → 0.231 Gcyc/f (−4.9 %)**; t=400 **0.733 → 0.709 (−3.3 %) Gi** and
  **0.186 → 0.180 (−3.2 %) Gcyc**. Predicted 1.5–4 % combined; measured 2.5–4.9 %.
  **The cycle win exceeding the instruction win at t=1961 is exactly 16m's
  signature** (16m measured Gi −0.96 to 0.00 % against Gcyc −1.17 to −2.85 %) —
  this is a register/frame effect, not an instruction-count one, and the
  disassembly says so directly:

  | binary | instrs in the symbol | `cxa_guard` refs | `bl` calls | callee-save `stp` pairs |
  |---|--:|--:|--:|--:|
  | parent `e017d611` | 3533 | **2** | **13** | **10** |
  | C1+C8 `187355b5` | 3490 | **0** | **6** | **9** |
  | full round | 3677 | 0 | 6 | 11 |

  Publishing the lazy static removed both guard references, took the call count
  from 13 to 6 and freed a callee-save pair — the same structural table 16m used.
  An earlier draft of this entry called C1+C8 "a wash"; that reading came from
  comparing the parent against the FULL child with its four dials forced OFF,
  which carries their OFF-arm cost (see the next bullet) and is not the C1+C8 arm.
* **The OFF-arm of the four dials reads 2.9 % above the C1+C8 build** (0.955 →
  0.983), which is why the per-lever column above is quoted against the ALL-OFF
  arm and the headline against the parent. **THIS ENTRY ORIGINALLY CALLED THAT
  2.9 % RECOVERABLE BY GOING FLAGLESS. THAT WAS WRONG AND ROUND 2 MEASURED IT:
  collapsing C3/C4/C7 to compile-time `true` is worth −0.19 to −0.27 % of the
  row**, i.e. at the ±0.14 % Ginstr floor. Clang had already hoisted the
  loop-invariant bools; the disassembly shows exactly two `adrp` disappearing
  (the two flag-array address materialisations) and one callee-save pair freed,
  and retired instructions barely move. The 2.9 % was the OFF arm *executing the
  slow paths*, plus LTO layout — not the dial reads. The collapse is kept anyway
  (byte-null, simpler hot body, campaign precedent) but it is a **refutation, not
  a win**, and nobody should go looking for that 2.9 % again.
* **C5 (hoist the spot cone's `_mm256_div_ps` to list-build time) — NOT BUILT.**
  C4 already skips 28–40 % of pairs before the cone block ever runs, and the
  census puts spots at 1.2–6.6 of 11.8–23.8 tile lights. The analysis predicted
  ~0.3 % of the row on instructions and "larger on cycles"; with C4 in front of
  it the instruction half is smaller still. Left on the backlog as the cheapest
  remaining byte-null item.

### The next round's target, named by this round's ladder

**The pack loop, 24.8 % of the row, of which the env-lane scalar compose is the
bulk: 33–36 % of city's alive lanes carry an env store** and each pays a face
pick, a live-water weight, a tilt + re-projection, and one or two
`EnvCubeFetchBil`. `EnvComposeCityVec8` already vectorises the *front end* of
this (reflect dir, Fresnel, mip) and arms on 33–37 % of groups; the *fetch* is
still eight scalar bilinear taps. That is the biggest single block left, and it
is a different shape from anything the campaign has attacked.

## 2026-08-29c — **greets' PASSENGER ROWS: `RTT` was running 96 lighting tiles INLINE on the tick thread — pooling them takes the row 1.77 → 1.28 ms byte-null.** The other two rows are healthy, and `Tick-ReflXfrm`'s attribution is corrected by 30×

Branch `rev-ssao`. The map (§00l item 9) named four unexamined rows. Three are
now measured and one is fixed.

### The diagnosis that mattered: cores, not instructions

| row | wall | Gi/f | Gcyc/f | **cores** | IPC |
|---|--:|--:|--:|--:|--:|
| `shadow-bake` | 1.918 | 0.143 | 0.044 | **7.2** | 3.25 |
| `bloom-chain` | 1.904 | 0.218 | 0.050 | **8.2** | 4.36 |
| **`RTT`** | 1.742 | 0.020 | **0.009** | **1.6** | 2.22 |
| `Tick-SkyCube` (city) | 0.793 | 0.068 | 0.020 | **7.9** | 3.41 |

`cores = Gcyc / clock / wall` is the whole diagnostic. `shadow-bake`,
`bloom-chain` and `Tick-SkyCube` are already parallel and high-IPC — **no
structural headroom, closed**. `RTT` at 1.6 cores was the outlier.

### RTT — the map's premise was wrong, and so was mine at first

§00l called it "0.020 Ginstr/f producing 1.69 ms at IPC 2.38 — not a compute
row", implying a stall or a mislabelled scope. It is neither. Decomposed:
`rtt-pick` 0.002, `rtt-prep` 0.003, **`rtt-bakejob` 1.765 (2 calls/f)** — two
genuine 512×512 offscreen scene renders, of which `rttj-xform` 0.321,
`rttj-raster` 0.305, **`rttj-light` 0.837**. And greets turns the feature on
itself (`FF::setDefault(BoolId::mirror_rtt, true)` in GREETS.CPP), which is why
the `.def` default of 0 is not what runs — worth knowing before reading any
mirror flag's default as its value.

**The defect: `ov.inlineDispatch = true` in `bakeJob`** — all 96 lighting tiles
serially on the tick thread. Inline was correct where it was introduced (the
mirror-SHARD bake: 96 tiles × 238 shards = 22 848 semaphore round trips a
shatter frame at 3.4–4.0 µs of core time each); the RTT amortises one round trip
over 262 144 pixels. The pool is idle here by construction (after Animate,
before the main Transform; `--bake_tick_overlap` dispatches later).

**`--deferred_ovec`… no — `--mirror_rtt_pool`, default ON.** Predicted 0.2–0.35 ms
for the pooled lighting; measured **0.339**. `rttj-light` 0.837 → 0.339
(−59.5 %); **RTT 1.684 → 1.279 ms (−24.1 %)**, interleaved min-of-7. Byte-null,
proved differentially on ONE binary at greets t=5743 / t=2845 / t=6097.

**The same change on the RTT's CONE pass is a regression and is not taken:**
0.064 → 0.153 ms (+139 %). That pass is ~64 µs, so the round trip costs more
than the fan-out saves. The flag covers the lighting only and the cone call keeps
inline dispatch with the number written beside it. *The fan-out pays when the
work per dispatch is large enough; 64 µs is not — the shard bake's lesson, one
level down.*

**Not taken:** `rttj-raster` (0.305 ms) is `MekaleleFillRegionInline` and the
tree has no parallel region-fill; writing one is new code whose z-buffer
tie-breaks are order-dependent — a byte risk for 0.3 ms.

### `Tick-ReflXfrm` — a 30× attribution error, corrected

City's largest tick row outside `renderFrame` (1.883 ms) was two calls under one
scope. Split: **`Tick-ReflGlass` 0.899 ms** (IPC 1.94, ~1.04 cores) and
**`Tick-ReflXfrmOnly` 0.981 ms** (IPC 3.46, ~1.27 cores).

The row has been carried since 2026-08-17 as the `--refl_correct` commission's
per-vertex normal work "priced at ~2 ms/frame". **Measured by flag flip on one
binary: 1.952 ms with, 1.888 ms without — the commission costs 0.064 ms, 3.3 % of
the row.** The rest predates it. Anyone budgeting against that look feature has
been reading a number ~30× too large.

### city's glass forward stamp — LANDED, `--city_glass_pool`, −82.7 %

`cityMirrorGlassForward` is the other half of `Tick-ReflXfrm` and ran serially:
0.899 ms at ~1.04 cores of 12, IPC 1.94. **Census first: 14 784 entries, 14 784
DISTINCT Face pointers, ZERO duplicates, 71 meshes.** Zero duplicates is the
load-bearing number — every iteration writes only its own `F->ReflectionTexture`
/ `F->EU*/EV*` / `F->Flags` and reads only per-mesh and camera state, so there is
no shared destination and no ordering hazard. That is exactly what separates it
from its sibling `Reflected_Transform`.

Two changes: a chunked fan-out (512 faces/chunk; the chunk COUNT is fixed by the
face count so the partition is identical every frame regardless of scheduling),
and `bsWorld` hoisted out of the per-face loop — `MatrixXVector(T->RotMat,
&T->BSphereCtr, …)` depends only on the MESH and was recomputed once per FACE,
**208× per mesh on average**.

| | before | after | |
|---|--:|--:|--:|
| `Tick-ReflGlass` | 0.931 | **0.161 ms** | **−82.7 %** |
| `Tick-ReflXfrm` | 1.951 | **1.282 ms** | −34.3 % |

Predicted ~0.20 ms; measured 0.161. **Byte-null, and for a threading change one
gate run is not evidence: 24 consecutive runs of the city acceptance pose give
ONE hash** (`4cb8d2ca…`, the pin), plus the flag flips to identical hashes at
t=1961 / t=2400 / t=400 on one binary.

### REFUTED — parallelising the mirror-mask clears is +33 % SLOWER

The cores sweep flagged `StampMasks` at 0.478 ms / ~1.3 cores, and it is
dominated by serial `std::memset` of ~8–10 MB a frame (mask 2 MB, mask-Z 4 MB,
up to three ownership planes). `parallel_memset` exists, `gbuf-clear` next door
uses it at ~7 cores, and the change is byte-null by construction. Measured, four
interleaved rounds: **serial 0.478–0.481 ms, pooled 0.617–0.639 ms.**

These clears are **DRAM-bandwidth-bound**, so more workers cannot beat the memory
system and the join is pure cost; `gbuf-clear` profits only because its buffers
are several times larger and amortise it. Kept as `--mirror_mask_pool_clear`,
**default OFF**, numbers in the flag text.

**FOURTH SIGHTING OF ONE LAW, and the clearest statement yet: a fan-out pays only
above a work-per-dispatch threshold.** The RTT cone pass (+139 % at 64 µs), the
SSAO depth gather (+2.5 % cycles), the OuterVec dial predicates, and now a
bandwidth-bound clear — against the RTT lighting's −24 % at 262 144 px per
dispatch. **`cores` tells you where to look; it does not tell you the fan-out
will pay.**

### The cores sweep, and what is left

`cores = Gcyc / clock / wall` applied to every row ≥0.20 ms in greets and city.
Everything large is healthy — `gbuffer` 6.7, `shadow-bake` 7.2, `cones` 8.2,
`Tick-SkyCube` 7.9. What remains serial, after this round's two landings:

| cores | arm | row | wall | verdict |
|--:|---|---|--:|---|
| 1.02 | greets | `rttj-raster` | 0.307 | no parallel region-fill exists; z-order byte risk |
| 1.17 | greets | `hdr-begin` | 0.266 | small, IPC 1.14 |
| 1.22 | city | `Tick-ReflXfrmOnly` | 1.024 | **the handoff below** |
| 1.12 | chase | `Tick-Radix` | 0.279 | small |

### Handoff — the route is already in the tree


Both halves run at ~1 core. `Reflected_Transform` is a demo-side serial copy of a
pass FDS parallelises; the blocker is its in-order append to the shared `FList`,
which feeds `Radix_Sort`, so naive fan-out can reorder equal-key faces and move
pixels. **`Transform.cpp` has already solved exactly this**: a planning pass
reserves each shard's output offset, then execution order floats —
*"execution order free, output order pinned"*. Porting that is the lever, worth
~0.8 ms; it was NOT attempted this round, deliberately, and here is the sizing so
the next round does not have to re-derive it: the vertex loop is a `goto`-based
legacy state machine (`Regular:` / `Ahead:` / `OUT:`) duplicated in CITY.CPP and
CHASE.CPP, the face counts are not known before culling so the reservation has to
over-allocate and compact (which is what `XfrmShard::out` is for), and any error
is a DRAW-ORDER change that moves pixels only where faces share a depth key. A
truncated attempt is worse than none.
`cityMirrorGlassForward` carries two visible redundancies for whoever takes it:
`bsWorld` recomputed PER FACE from per-MESH inputs, and a `powf` per face.

## 2026-08-29b — **THE SSAO ROW IS CLOSED AT THE BIT-EXACT LEVEL.** The depth gather is REFUTED (−9.8 % instructions, +2.5 % cycles) and the `gtaoAcos` sqrt look call is priced at NOTHING — it comes off the decision stack

Continuation of **2026-08-29**. Same branch, same arms. Nothing landed this
round that changes a pixel or a cycle; what landed is three instruments and two
refutations, and the conclusion that the row has no bit-exact work left in it.

### The row as it stands (five scopes, greets t=5743 / chase t=1105)

| scope | greets ms | % | IPC | effPar | chase@main | chase@refl |
|---|--:|--:|--:|--:|--:|--:|
| `ssao-march` | 4.751 | **69.6** | 3.27 | 11.2 | 4.561 | 2.058 |
| `ssao-apply` | 1.762 | 25.8 | **5.29** | 11.0 | 1.687 | 0.504 |
| `ssao-blur` | 0.272 | 4.0 | 3.66 | 9.7 | 0.269 | 0.265 |
| `ssao` | 6.827 | | 3.80 | | 6.552 | 2.877 |

(chase now splits `@refl` / `@main` — the cone round's `TailProf::PassTag`
composing with these scopes.)

### REFUTED — vectorising the march's depth gather

Priced first: `-DFDS_SSAO_DIAG=4` removes the whole gather, **loads included**, and
it is **0.105 Gi/f = 19.8 % of the march**, with cycles moving proportionally
(IPC 2.777 → 2.757) — i.e. **instruction-bound, not stalled on the scattered u16
loads**, which are only ~8 % of its instructions. That reading is what justified
building it. Built bit-exact; three interleaved rounds:

| arm | Gi/f | Gcyc/f | IPC |
|---|--:|--:|--:|
| scalar (shipping) | 0.529 | 0.159–0.164 | **3.29** |
| vector, narrow stores | 0.479 | 0.168–0.170 | 2.83 |
| vector, lane inserts | 0.477 | 0.163–0.166 | 2.90 |

**−9.8 % instructions for +2.5 % cycles.** Within-arm spreads are 0.001–0.003
Gcyc, so the direction is not noise. The scalar loop's eight iterations are
INDEPENDENT and the out-of-order engine was already overlapping their loads with
the surrounding vector arithmetic; the vector form replaces that with one
dependency chain (index → spill → eight dependent loads → widen → mask →
convert). The first variant additionally put a narrow-store-to-wide-load
forwarding stall on that chain; `vld1q_lane_u16` inserts remove that half, and
the chain half does not go away. Reverted; kept as **`-DFDS_SSAO_VECGATHER=ON`**
because on a target with a real hardware gather the balance could invert.

**THIRD OCCURRENCE OF ONE LAW, now stated as one:** cone round C6 ("register
pressure beats op count"), the engine-wide movemask sweep (bit-exact,
instruction-cheaper, cycle-NEUTRAL in the lighting kernel, IPC 4.08 → 3.94), and
this. **An 8-iteration independent scalar loop in these kernels is not
automatically improved by vectorising it — price it in CYCLES before believing
the instruction count.**

### REFUTED — and this one closes an item in Gil-Ad's stack

`gtaoAcos_x8`'s two `_mm256_sqrt_ps` were declined by a prior round as "not
byte-safe, flips ~0.3 % of samples" — declined on look grounds, never priced.
Priced now (`-DFDS_SSAO_DIAG=5`), three interleaved rounds: the rsqrt
substitution is **+1.9 % instructions, +3.1 % cycles, no better on wall**.
`fsqrt.4s` on this core beats rsqrt + multiply + the max-guard the substitution
needs to stay finite. **There is no look call to make: the faster-looking option
is not faster.** Closed, not deferred.

### Two instrument defects of mine, both found by disbelieving a good number

1. Ladder stages 1–4 are CUMULATIVE (`>= n`) and stage 5 read as `>= 4`, so the
   first DIAG=5 run also deleted the gather and reported the substitution as
   worth −19.5 % of the row and moving **99.13 % of pixels**. Both were the
   gather. Stage 5 is now `== 5`; the header states which stages are cumulative.
2. S1 (2026-08-29) rewrote the slice setup and deleted DIAG stages 1–3 with it,
   so a DIAG=2 run now measures NOTHING — and my reading of "0.530 vs 0.529, the
   slice setup is free post-S1" was a dead instrument. **WITHDRAWN.** What is
   established about S1 is its own A/B: −0.092 Gi/f of the 0.140 priced.

### Why the row is closed at the bit-exact level

* the per-lane slice setup — **taken** (2026-08-29, −16.5 %/−22.0 % of the march)
* the depth gather, 19.8 % — **refuted above**
* `gtaoAcos_x8`'s sqrts — **refuted above**, and the look call with them
* the vec loop's bare `_mm256_rsqrt_ps` — still a genuine look/precision item in
  his stack (2026-08-17a), untouched, and not a perf lever either way
* the reconstruct, the bitmask build and the popcount — **already vector**, the
  popcount by clang itself (2× `cnt.16b`)
* `ssao-apply` at **IPC 5.29** is at the core ceiling; `ssao-blur` is 4 % of the row

The next ms in this pass has to come from doing less work — fewer slices, fewer
steps, a coarser grid — and every one of those is a quality dial, i.e. his call,
not a lever. `--ssao_gtao_slices=1` and `--ssao_gtao_steps=2` are already
measured (−31 % / −23 % of the row, `PERF_STATE` §00l.4) and remain the honest
menu if he wants the time back.

## 2026-08-29 — **`Render_SSAO` DECOMPOSED, and the split's target taken: the march's per-lane slice setup goes 4-wide BIT-EXACT — `ssao` −9.3 % (greets) / −16.4 % (chase), march −16.5 % / −22.0 %.** DONE (S1; two candidates refuted by census before coding)

`Render_SSAO` was **11.13 ms at chase t=1105 = 26.3 % of `renderFrame`** and
16.6–18.1 % of greets'. `PERF_STATE.md` §00l called the decomposition "step one"
and priced the interior by arithmetic on two `--ssao_downscale` points. Full
account, tables and the reusable contraction rules: **`docs/PERF_STATE.md`
§00n**. Branch `rev-ssao`.

### Step zero — the scopes, and the inference CONFIRMED

The pass dispatched with `dispatchIndexed(..., nullptr, ...)` and joined on a
bare `tileDone.acquire()` loop, so it never used the `Stamp`/`drain` pairing and
had no `effPar` at all. Five wave scopes now exist. **The stamp must be taken
before the dispatch** — a first attempt with the drain inside the lambda printed
`0.00 calls/f`.

| scope | measured | inferred | effPar | Gi/f | IPC |
|---|--:|--:|--:|--:|--:|
| march | **5.712 ms** | ≈5.8 | 11.0 | 0.621 | **3.26** |
| apply | **1.756 ms** | ≈1.9 | 10.6 | 0.310 | **5.30** |
| blur | **0.272 ms** | ≈0.33 | 9.0 | 0.032 | 3.79 |

**The `--ssao_downscale`-slope inference was right** (1.5 % / 7.6 % / 17.6 %),
and the scopes cover 99.3 % of the row — no hidden block. The new information is
`effPar` (9–11 of 12, no serial bottleneck) and **IPC: the apply is at 5.30, near
the core ceiling; the march is at 3.26.** The march is 64.5 % of the instructions
and 73.3 % of the time — it is the one that stalls, and the one to attack.

### Refuted BEFORE coding — add both to the do-not-repropose list

* **The cone round's arm64 `_mm256_movemask_ps` defect does NOT exist in SSAO.**
  Zero movemask sites in `DeferredSSAO.cpp`, and the 32-sector bitmask's eight
  scalar `__builtin_popcount` calls are **already vectorised by clang** into
  2× `cnt.16b`. Checked in the disassembly before a line was written.
* **Sky/background early-out is worth nothing in the arm that matters**
  (`-DFDS_SSAO_CENSUS`): ALL-SKY 8-cell groups **0.00 % in greets**, 5.08 % in
  chase; valid lanes 99.96 % / 94.40 %; scalar-tail cells **0**.

### What landed — S1

The per-lane slice setup, priced by the new `-DFDS_SSAO_DIAG` ladder at **22.5 %
of the march** (atan2 alone 7.6 %) = **135 instructions per (lane × slice), 1.04 M
a frame**, now runs 4 lanes at a time in **plain NEON** (not simde, so
`fast_rsqrt`'s `vrsqrte`+1-Newton is exact). Predicted −0.09 to −0.105 Gi/f;
**measured −0.092.** greets march −16.5 % ms / −14.8 % Gi, `ssao` −9.3 % / −9.6 %,
`renderFrame` −1.65 % Gi. chase march −22.0 % / −18.6 %, `ssao` **−16.4 %**.
`ssao-apply` and `ssao-blur` are unchanged to the digit — the control.

### THE DURABLE HALF: three contraction rules, established by reading the assembly

The first build failed one pin, so `-DFDS_SSAO_VERIFY` ran the scalar behind the
vector counting mismatches **per term**. Each fault was then settled by compiling
the scalar expression standalone and reading its assembly — not by guessing:

1. **`a*b - c*d` contracts to ONE `fnmsub`**, not two muls and a sub.
2. **For `A + B + C` all products, clang chains from the SECOND term:**
   `fma(C, fma(A, mul(B)))`. Starting at A moved **26 % of lanes**.
3. **A trailing `x * poly` that feeds an add/sub is never materialised alone** —
   `halfPi - a*poly` is one `fmsub`, `a*poly + (±π)` is one `fmadd`. Rounding it
   separately cost **32 196 lanes**.

Final: **0 mismatches in 1 036 800 lanes at two poses, on every term.** Anyone
vectorising a scalar float expression in this tree should start from these three
rules and the verify harness, not from the intrinsics.

### Not taken

`atan_approx_x8` already exists and uses `_mm256_rcp_ps` where the scalar
divides. It is faster and it **moves AO values** — a look call in the same family
as the 8-wide GTAO rsqrt item already in Gil-Ad's stack (2026-08-17a), not a perf
lever. Going the other way (removing precision) is equally his call.

### Next

The march is now 4.827 ms of a 7.280 ms greets row (66 %). What remains inside it
is the sample loop: the scalar depth gather (8 loads + bounds + convert per
sample batch, 1.04 M batches/frame), the reconstruct, two `_mm256_rsqrt_ps`, two
`gtaoAcos_x8` and the bitmask build. The gather's address math and its
`u16 → float` convert are vectorisable the same way this round's setup was; the
two `sqrt`s inside `gtaoAcos_x8` are **not** byte-safe to replace (prior round:
flips ~0.3 % of samples). The apply at IPC 5.30 is close to the ceiling and
should be left alone.

## 2026-08-28b — **ROUND 2 ON THE SAME KERNEL: the pack loop's env fetch goes 8-wide and city's `lighting-w1` reaches −27.5 % against the pre-campaign parent.** DONE (C9, C10; the flag collapse is a REFUTATION)

Continuation of **2026-08-28**. Same target, same arm, same worktree
(`rev-w1impl`), parent for all end-to-end numbers is still `fog-wt` `e017d611`.

### 1. The flag collapse — REFUTED at −0.2 %

Round 1 said "if a future round wants the last 2.9 %, collapse the four dials to
flagless". Measured: **−0.19 to −0.27 % of the row**, which is the ±0.14 %
Ginstr floor. Clang had already hoisted the loop-invariant bools out of both
loops; the disassembly shows only **two `adrp` gone** (the flag-array address
materialisations) and one callee-save pair freed, with static size actually
*rising* 3677 → 4051 as the constant predicates let it specialise more. The
2.9 % was the OFF arm running the slow paths plus LTO layout — never the dial.

Kept anyway (byte-null, simpler body, 16h/16m/16o precedent), now as
`-DFDS_OVEC_HATCH=ON` rebuild arms in the shape `--deferred_fill_oct_pair`
already uses. Keep/drop, one line each:

* `--deferred_ovec_light_skip` (C4) — **dropped**; its `&&` was evaluated per
  (group × light), ~2.3 M times a frame.
* `--deferred_ovec_mat_uniform` (C3) — **dropped**; per group, behind a movemask.
* `--deferred_ovec_vec_pack` (C7) — **dropped**; per group, same shape.
* `--deferred_ovec_nomirror` (C2) — **KEPT LIVE**; read once per TILE, and it is
  the only way to force the `kMirror == true` instantiation on a scene that
  allocates no mirror plane, i.e. the only way to byte-compare the two arms of
  the templated loop from a shipping binary.

### 2. The refreshed ladder — the pack loop GREW to 29.9 % of the row

Round 1 shrank everything around it. City t=1961, 1512×848, row now 0.739 Gi/f:

| block | Gi/f | % of row | was (round 1) |
|---|--:|--:|--:|
| **the omni loop** | 0.336 | **45.5** | 49.8 |
| **the pack loop** | 0.221 | **29.9** | 24.8 |
| everything through the material gather | 0.067 | 9.1 | 9.6 |
| normal decode + view pos + texel/masks | 0.054 | 7.3 | — |
| env front-end (`EnvComposeCityVec8`) | 0.042 | 5.7 | 3.9 |
| saturate + compose + store-outs | 0.019 | 2.6 | 3.2 |

A new **`-DFDS_OVEC_ENVDIAG=n`** ladder (byte-changing, cost instrument only,
same status as `FDS_W1LDR_ABLATE`) splits the pack:

| piece | Gi/f | % of row |
|---|--:|--:|
| 2× `EnvCubeFetchBil` — **the fetch** | 0.038 | **5.1** |
| live-water weight + tilt + re-projection | 0.025 | 3.4 |
| `EnvCube_DirToFaceUV` face pick | 0.014 | 1.9 |
| everything else in the lane loop | 0.144 | 19.5 |

### 3. The same-face census — measured BEFORE choosing the shape

`--omni_census`, city, of the groups carrying a vec-env lane:

| number | t=400 | t=1961 | t=2400 |
|---|--:|--:|--:|
| **all env lanes on ONE cube face** (after the live-water tilt) | **95.9 %** | **96.2 %** | **90.2 %** |
| same face **and** same mip level | 95.9 % | 96.2 % | 90.2 % |
| vec-env lanes per such group | 7.43 | 7.59 | 7.37 |
| lanes needing BOTH mip levels | **100 %** | **100 %** | **100 %** |
| live-water tilt fired | 42.4 % | 37.9 % | 26.1 % |
| `EnvCubeFetchBil` calls / frame | 488 k | 498 k | 287 k |

Same-face implies same-mip 100 % of the time — gloss is per-material and 95 % of
groups are material-uniform. The shape was justified before a line was written.

### 4. What landed

**C9 — `EnvCubeFetchBil8`.** The whole bilinear for eight lanes sharing one face
and one level. The face pick and live-water tilt move out of the lane loop into
a pre-pass (same work, same order, hoisted only so the uniformity test can see
all eight answers); a mixed-face group falls back to the scalar fetch reading the
face/uv the pre-pass already computed, so it is never worse.

**C10 — 8-wide pack for an ENV group.** C7 only ever fired for the 58–63 % of
groups with NO env lane; C9 leaves the env texel in arrays, so the other third
packs 8-wide too. It reproduces the scalar's **order of rounding**, which is not
C7's: the scalar truncates each term with `int()` and clamps the INTEGER SUM, so
this is two `cvttps` + int add + int `min`/`max`, deliberately different from
C7's float-clamp-before-convert (right for a single term, wrong for two).

**Byte-exactness held on the FIRST try, no tuning**, on three details worth
keeping: `u*fr - 0.5f` is a *contracted* fmsub under `-ffp-contract=fast`;
`if (px < 0) px = 0` must be `_mm256_max_ps(zero, px)` and **not**
`max_ps(px, zero)`, because maxps returns its second operand when unordered and
the scalar leaves a NaN alone; and the inter-level lerp keeps `lf` **per lane**,
because `lvlF` can differ inside one integer level even when `lvl0` agrees.

### 5. Predicted vs measured

| lever | predicted (% of row) | measured (% of row) | % of `renderFrame` |
|---|--:|--:|--:|
| flag collapse | "up to 2.9" | **−0.19 to −0.27** | ~0 — **REFUTED** |
| C9 8-wide fetch | 3.4 | **−2.1 to −3.2** | −0.32 to −0.60 |
| C10 8-wide env pack | 1.5 | **−1.4 to −2.2** | −0.24 to −0.37 |
| C9 + C10 | 4.9 | **−4.1 to −5.4** | **−0.64 to −0.97** |

**End-to-end, round 1 + round 2 against `e017d611`**, both binaries in one
worktree, interleaved, min-of-5, Ginstr floor ±0.14 %:

| pose | `lighting-w1` Gi/f | | `Gcyc/f` | | `renderFrame` Gi/f | |
|---|--:|--:|--:|--:|--:|--:|
| city t=1961 | 0.978 → **0.709** | **−27.5 %** | 0.245 → 0.180 | −26.5 % | 4.184 → **3.915** | **−6.4 %** |
| city t=400 | 0.733 → **0.508** | **−30.7 %** | 0.185 → 0.127 | −31.4 % | 3.174 → **2.949** | **−7.1 %** |
| city t=2400 | 0.390 → **0.315** | **−19.2 %** | 0.103 → 0.083 | −19.4 % | 2.255 → **2.180** | **−3.3 %** |
| fountain t=2500 | 0.105 → **0.090** | **−14.3 %** | 0.029 → 0.025 | −13.8 % | 1.061 → 1.045 | −1.5 % |
| **greets t=5743 (control)** | 1.478 → **1.476** | **−0.14 %** | 0.379 → 0.383 | +1.1 % | 3.644 → 3.640 | −0.11 % |

greets sits at the instruction floor, which is the control that keeps proving the
work is confined to the OuterVec kernel.

### 6. The next block, and it is now a different one

The pack loop's **"everything else", 0.144 Gi/f / 19.5 % of the row**, is the
largest piece this round did not price further, and C10 has just taken a bite out
of it that the next ladder run should re-measure. After that the ranking is:

1. **The omni loop, still 45.5 %.** C2 and C4 took ~29 % out of it; what remains
   is ~64 NEON of genuinely per-(pixel × light) arithmetic on the 60–72 % of
   pairs that survive C4. The only structural idea left is transposing it to
   8 lights × 8 pixels, which is a different kernel, not a lever.
2. **The live-water tilt, 3.4 %, and the face pick, 1.9 %** — both now sit in the
   C9 pre-pass as per-lane scalar loops, both 8-wide-able. `EnvCube_DirToFaceUV`
   is a max-abs select plus two divides; byte-exact vectorisation is plausible
   but the face output feeds C9's own uniformity test, so it must stay per-lane
   int. Worth ~1.5–3 % of the row if it works.
3. **C5 (the spot-cone reciprocal) stays not-built**, for the round-1 reason:
   C4 skips 28–40 % of pairs before the cone block runs at all.

## 2026-08-26 — **2026-08-25b IS FIXED: the mechanism is `Scene::PreferOuterVec`, not the tonemap — the outer-vec lighting kernel writes NO HDR radiance, so SSAO was multiplying a cleared buffer.** DONE (`--ssao_hdr_transport`, default 1)

**THE NAMED MECHANISM.** `Render_DeferredLighting_Tile_OuterVec` — the kernel city,
fountain and crash select via `Scene::PreferOuterVec = 1` — **stores 8-bit VPage only
and deliberately leaves the HDR coverage lane `h[3]` at 0**: its pack *is* the HDR
transport, lifted afterwards by the froxel composite (`h[3] > 0 ? h : VPage`) or by
`Hdr_ActivateNoFog`. The SCALAR wave-1 kernel that greets and chase run (PreferOuterVec
0) does write `g_hdrBuf` and stamp coverage. `Render_SSAO` chose its target on
`hdr() && Hdr_WritableFor(W,H)` — "is the buffer **sized** for this view" — which is
true in **both** cases. So on city/fountain the AO multiply landed on a **sized, cleared,
all-zero** buffer, and the lift then seeded that buffer from the **un-occluded** VPage.
The pass ran, cost its milliseconds, and produced nothing.

**THE ONE-RENDER PROOF** (`FDS_HDR_SCAN=1`, extended to print an FNV of `g_hdrBuf`, the
coverage population, the ZPage coverage and a VPage FNV per pipeline tag). Same arm,
`--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao`:

| scene | tag `kernel` | tag `ssao-post` |
|---|---|---|
| fountain t=2500 | `cov=0 maxFinite=0 zcov=396687` | hash **UNCHANGED** |
| city t=1961 (main pass) | `cov=0 maxFinite=0 zcov=1116488` | hash **UNCHANGED** |
| greets t=5743 | `cov=2072779 maxFinite=617` | hash **CHANGES** |
| chase t=1105 | `cov=1957160 maxFinite=24256` | hash **CHANGES** |

`zcov` proves the frames have opaque coverage; `cov=0` proves the kernel put none of it
in `g_hdrBuf`. The next tag then shows **who overwrites**, and it is a DIFFERENT stage in
each scene — city: `fog-post` (the froxel composite, `act=0 → 1`, `maxFinite 0 → 2478`);
fountain: `activate` (`Hdr_ActivateNoFog`, `maxFinite 0 → 255`). **That is why the hunt's
`--no-fast_fog` control "exonerated" the fog and found nothing** — dropping the froxel
composite just hands the same VPage lift to `Hdr_ActivateNoFog`. Both are downstream
symptoms of one upstream fact.

**THE FIX** (`FDS/RENDER/DeferredSSAO.cpp`, one predicate): ask
`Deferred_KernelWritesHdrRadiance()` (`= !deferredLightingOuterVecEnabled()`, exported in
`DeferredCommon.h`) instead of assuming `Hdr_WritableFor`. When the kernel wrote no HDR,
SSAO takes the **VPage arm** — the same 8-bit buffer the whole frame is shaded into —
and the lift carries the occlusion into the radiance. Same class as, and one call site
downstream of, the wave-2 fill kernel's existing "⚠ WAVE-1 TRANSPORT MUST MATCH"
warning. `--no-ssao_hdr_transport` restores the old predicate **exactly** (verified:
byte-identical to the pre-fix hashes on city and fountain).

**ACCEPTANCE, measured.** city t=1961 `--city_env_pixel`: no-ssao `b3372d0f…` (the
hash the defect report recorded), ssao **`6964d6e8…`**, `--ssao_strength=8`
**`3256a72f…`**, and the OFF arm back to `b3372d0f…`. fountain t=2500: no-ssao
`32ff5896…`, ssao **`4e831ea0…`**, strength=8 **`d0a95fe9…`**, OFF arm back to
`32ff5896…`. Look: city 44.95 % px moved, mean |Δ| 4.77 on the moved, max 145;
fountain 15.33 %, mean 5.30, max 107. Crops `docs/img/hdrssao/`.

**THE `ssao_radius_zfloor=48` PIN IS NOW A VISIBLE FACT IN THESE SCENES, and its
bijection re-verifies AS AN IMAGE.** city default == `--ssao_radius_zfloor=0
--ssao_radius=6.0661764` **byte-identical**, and `zfloor=0` alone (effective 4.0) is a
different image (22.65 % px, mean 1.54, 412 px > 12/255). fountain default ==
`zfloor=0 --ssao_radius=4.0441175` byte-identical, `zfloor=0` alone differs (1.61 % px,
mean 1.18, max 8). The 2026-08-25 table's per-scene effective radii were derived from an
arm where the AO never reached the frame; they now reproduce on the frame itself.

**COST, honest, min-of-11 interleaved arms, `--bench=scene`, no `--ssao_dump`.**
fountain t=2500: `[ssao]` pass **4.12 min / 4.16 median** (fix) vs **4.07 / 4.11**
(pre-fix) → **+0.05 ms**; the apply stage alone 0.56 vs 0.52 → **+0.04 ms**. city
t=1961: pass **5.50 / 5.58** vs **5.48 / 5.59** → **+0.02 ms min, −0.01 median**; apply
1.10 vs 1.06 → **+0.04 ms**. The +0.04 ms on the apply is the real, reproducible term
(the 8-bit RMW with three int converts + pack, against the f16 RMW) and it is the whole
price. Everything else in the ~4–5.5 ms pass is unchanged — and it was previously being
paid for **nothing**.

**GATES: 13/13 pinned poses + the city acceptance arm reproduce, `render_gate.sh` 4/4.**
Byte-null on greets and chase by construction (scalar kernel) and verified
differentially on one binary: chase t=1105 his arm `63d1613e…` and greets t=5743
`440aa6bb…` are identical with and without `--no-ssao_hdr_transport`.

## 2026-08-25b — **SSAO IS SILENTLY DISCARDED IN CITY AND FOUNTAIN UNDER `--hdr`**: the pass runs, costs 5.6 ms, and its output never reaches the frame. ~~REPORTED, NOT FIXED — TODO~~ **FIXED 2026-08-26 — see the entry above.** The hypothesis this entry names ("`g_hdrBuf` merely sized-but-not-yet-active when SSAO runs, and `Hdr_ResolveActivate` overwrites the AO") is HALF right: the overwrite is real, but the reason SSAO's write was lost is that the OUTER-VEC kernel never put radiance in the buffer at all — `Hdr_ActivateNoFog` skips covered pixels (`h[3] != 0`), so on greets/chase the AO survives it. The report below stands as written; the mechanism is in the entry above.

Found while landing the `ssao_radius_zfloor` default flip (SESSION_STATE
2026-08-25). Not a regression from that flip — it predates it and the flip is
merely the thing that made it visible, because the flip's city/fountain look-delta
came back *byte-null under `--hdr`* and that was too clean to be true.

**THE SYMPTOM, MEASURED.** city `t=1961`, `FDS_CITY_ENV_PIXEL=1 ... --city_env_pixel`,
one pose per process, dummy drivers. All three of these are the SAME hash
`b3372d0f6ef2a6dba78a7095ef5d36cd`:

* `--deferred --hdr --hdr-linear --texture-filter=2` (no SSAO at all)
* `... --ssao --ssao-gtao --ssao_strength=8 --ssao_power=4`
* `... --ssao --ssao-gtao --ssao_radius_zfloor=0 --ssao_radius=200`

A **50× radius and an 8× strength change nothing**. fountain `t=2500` behaves
identically (`8db68ccb…` with and without `--ssao --ssao-gtao --ssao_strength=8`).

**SSAO IS DEFINITELY RUNNING.** The pass reports itself:
`[ssao] 1920x1080 /2 (960x540), GTAO+bitmask, HDR g_hdrBuf: 5.61 ms` — full main
resolution, the GTAO producer, and it believes it is writing the HDR buffer. And
`--ssao_debug` (which paints the AO term to VPage and returns before the tonemap)
produces a correct, different frame `e5048e48…`. So the AO field is computed and
then lost between `Render_SSAO` and the dumped image. **city and fountain have
been rendering with no ambient occlusion at all in every `--hdr` arm.**

**WHAT IS RULED OUT.** The froxel/fog composite is **EXONERATED**: `--deferred
--hdr --no-fast_fog` is byte-identical with and without
`--ssao --ssao-gtao --ssao_strength=8` (`928c7791…` both). So it is not the fog
pass rebuilding `g_hdrBuf` over the AO multiply.

**WHAT IS NOT RULED OUT / WHERE TO LOOK.** chase keeps its AO under the same
`--hdr --hdr-linear` (measured: `0f581ab7…` no-ssao vs `63d1613e…` ssao at
t=1105), and greets keeps it too, so this is scene-conditional. Prime suspect is
the ordering around HDR *activation*: `Render_SSAO` picks its write target from
`useHdr = hdr() && Hdr_WritableFor(...)`, and `Hdr_WritableFor` tests that the
buffer is **sized for this view**, NOT that `g_hdrActive` is set. If `g_hdrBuf` is
merely sized-but-not-yet-active when SSAO runs, SSAO takes the HDR arm, and the
later `Hdr_ResolveActivate` (RENDER.CPP, "Fog-off HDR activation", which resolves
VPage → `g_hdrBuf` when HDR is not yet active) would overwrite the AO wholesale.
That is a hypothesis, not a measurement — the discriminator is to log
`g_hdrActive` alongside the existing `[ssao]` line in all four scenes and see
which ones have it false at SSAO time.

**WHY IT MATTERS.** SSAO is ~39 % of his greets acceptance frame; in city and
fountain he is paying 5.6 ms for a pass whose output is thrown away, and getting
none of the look. Either fix the target selection or skip the pass — both are
wins, and they are opposite wins, so this needs the measurement above before any
patch.

## 2026-08-25a — MEASUREMENT TRAP: `--ssao_dump` inflates the `[ssao]` pass ~3.5×

`--ssao_dump` materialises the applied full-res AO plane, and doing so **forces
the scalar apply loop**. Measured at chase `t=1105`, 1920×1080,
`--ssao_downscale=2`: the `[ssao]` pass reports **13.7 / 16.0 / 15.4 ms** with the
dump against **~4.1–4.4 ms** without it. **Timings taken from a dumping run are
not pass timings** — the dump is a correctness instrument only.

This trap already produced one wrong number that shipped in a flag description:
the `ssao_radius_zfloor` cost was recorded as **+0.17 ms min / +0.05 ms median**
(6 runs per arm, un-interleaved). Re-measured at the default flip with **14 runs
per arm interleaved**, the arms are indistinguishable — k=0 min 4.15 / median 4.25
/ mean 4.264 ms vs k=48 min 4.07 / median 4.26 / mean 4.238, i.e. delta min −0.08,
median +0.01, mean −0.03, **sign flipping between statistics**. The +0.17 figure is
retracted as run-to-run noise. Standing rule for SSAO A/Bs: interleave the arms,
report min AND median AND mean, and never dump while timing.

## 2026-08-17a — THE 8-WIDE GTAO's TWO RECIPROCALS: the shipped SSAO path disagrees with **its own scalar reference** on up to 24 % of pixels, systematically darker. REPORTED, NOT FIXED

Found while porting GTAO into the GPU arm (`docs/SHADING_CONTRACT.md` §13); the
GPU is only the instrument that made it visible — the finding is CPU-internal and
needs no GPU to reproduce.

`Render_SSAO`'s 8-wide GTAO (`FDS/RENDER/DeferredSSAO.cpp:358-489`) computes the
two horizon cosines with

```cpp
const __m256 dinv = _mm256_rsqrt_ps(dl2);
const __m256 binv = _mm256_rsqrt_ps(bx*bx + by*by + bz*bz + 1e-12f);
```

— **no Newton-Raphson**. The scalar reference 80 lines above it uses `fast_rsqrt`,
which has the NR step (~16 bits). On arm64 simde lowers a bare `_mm256_rsqrt_ps`
to `vrsqrteq_f32`, an **~8-bit** estimate; on x86 it is the native 12-bit
`rsqrtps`. GTAO then feeds those cosines into a **hard 32-sector visibility
bitmask**, which has no tolerance for an 8-bit reciprocal: a horizon that lands
within the estimate's error of a sector boundary sets a different bit.

**MEASURED**, greets 1920×1080, `--ssao --ssao-gtao` at defaults, the shipped
binary against itself with `FDS_SSAO_NOSIMD=1` (the file's own escape hatch), per
pixel against the contract's `|Δ| ≤ 0.005·max + 1e-4`:

| pose | pixels agreeing | AO mean, 8-wide vs scalar |
|---|--:|---|
| t=4871 | 75.85 % | 0.85431 vs 0.85601 (**darker**) |
| t=5743 | 88.29 % | 0.82507 vs 0.82556 |
| t=2845 | 98.00 % | 0.94410 vs 0.94468 |
| t=6097 | 99.83 % | 0.91863 vs 0.91870 |

With the denoise off so each cell is a raw bitmask, the difference is **exactly
integral in sector bits**: 82.60 % of pixels bit-identical, 4.94 % off by −1 bit,
11.79 % by +1, 0.65 % by ±2, **0.0037 % not an integer number of bits**. The +1
side outnumbers −1 by 2.4:1, which is the systematic darkening.

The **hemisphere** path has the same defect twice (`_mm256_rcp_ps` for `invSZ` and
for `radius/(dz+eps)`) and **no escape hatch** — `FDS_SSAO_NOSIMD` guards only the
GTAO branch. Same-inputs CPU-vs-GPU there is 98.22 %.

**This is §12.4's `--deferred_vec` GGX finding in a second kernel**, and the same
fix: one NR step on each reciprocal. Not taken here because `DeferredSSAO.cpp` is
contended and SSAO is ~40 % of his acceptance frame — the price of the refinement
is a perf decision, not a correctness one. The scalar reference is available for
A/B at zero cost (`FDS_SSAO_NOSIMD=1`), and the deltas above are what it buys.

---
## 2026-08-17 — THE COMMISSION LANDS: `--refl_correct`, default ON. chase's reflected pass gets a normal AND a mirrored light for the first time; **city's pins could never have seen it** (its water is empty on tick 1), and the guard that matters most is one nobody asked for — the nested probe bake

**Commissioned by Gil-Ad 2026-08-16** ("for chase — commission the correct
look. there is no '98 look to compare to anyway"). 16w characterised the defect
and refused to land it because the direction was a look call; this round lands
it behind `--refl_correct` (`FeatureFlags.def:182`, **default 1**), re-pins the
seven chase hashes, and leaves `--no-refl_correct` as an exact escape hatch —
**12/12 pin recipes byte-identical to the parent binary with the flag off.**

Parent `DEMO_par` md5 `6acb2ebf54eb2cabd37006dfcb656670`, child `DEMO_new2`
md5 `411af80098affd91e89d34f7c8acd182`, both built in one worktree on
`cb6aad4c`, one asset tree, warm cube cache.

### WHAT LANDED — two halves, and they are in different files

1. **NORMALS + TANGENTS in `Reflected_Transform`.** 16w's two-line arm 1,
   promoted to a runtime flag, in **all SIX vertex loops of each scene** — the
   three non-Phong loops 16w patched *and the three `Tri_Phong` else-branches it
   flagged and did not* (`CHASE.CPP` 459/483/510/552/576/605, `CITY.CPP`
   641/665/692/732/756/785). `IM` is the pass's model→reflected-view rotation,
   copied **before** the FOV row-scaling, so it is the unscaled rotation, staged
   exactly as `Transform.cpp` stages the main pass's. The predicate is hoisted
   once per call, so it is loop-invariant, not a per-vertex branch.
2. **MIRRORED LIGHTS** (`FDS/RENDER/ReflMirror.cpp`, new TU). The scene arms the
   state around its reflected `Render()`; `Render_DeferredLighting` calls
   `ReflMirror_MirrorLights` right after the per-omni SoA build, which rewrites
   each entry's view-space position, world position and spot axis from the light
   **reflected about the same plane the geometry is mirrored about** — `P −
   2(P·N)N` through the world ORIGIN, `d = 0`, which is what `Reflected_Transform`
   actually does (`CHASE.CPP:341`, `CITY.CPP:527`), **not** city's `RflSurfOfs`
   (that is the FastFog plane). Nothing is appended: light count, tile binning and
   every per-pixel loop are untouched. `noinline`, own TU, so it cannot
   re-schedule `Render_DeferredLighting`'s FP.

### THE GUARD NOBODY ASKED FOR, AND IT WAS NOT HYPOTHETICAL

`renderFrame`'s prologue runs `EnvReflection_FramePrep` gated on
`g_offscreenViewDepth == 0` (`RENDER.CPP:520`) — and city's reflected `Render()`
**is** a depth-0 `renderFrame`. So the env-probe bake fires INSIDE the armed
mirrored pass, and its six cube-face renders reach `Render_DeferredLighting`
again. Without a guard those faces are lit by MIRRORED lights and then written
to the **on-disk** cube cache — damage that outlives the process and that no pin
catches, because pins run one tick against an already-warm cache.

`ReflMirror_MirrorLights` now returns early when `g_offscreenViewDepth != 0`.
The gate is exact, not conservative: the reflected pass IS depth 0 and stays
mirrored; every nested bake is depth > 0 and stays main-space.

**It fires.** Measured, not argued: the unguarded child moved city's
acceptance pin at t=2400 to `9a13d69f…` (22 px, max |Δ| 2); with the guard the
same recipe returns **exactly the parent's `f473fe2b…`**. Those 22 pixels were
mirrored lights leaking out of a probe. The two pinned cubes on disk are intact
(`adbac29c…`, `a896a47c…`); one cube baked during this round's unguarded runs
was deleted rather than trusted.

Related, and NOT `env_refl()` being dormant: greets' PBR metallic import calls
`setDefault(env_refl, true)` **process-globally** (`MaterialImport.cpp:865`),
greets inits FIRST, and `setDefault` is one-way — so **a full demo run has
`--env_refl` ON in city and chase**, contrary to what the flag text claimed
before this round. `--snapshot=<scene>` does not reproduce that state (only the
requested scene inits), so every pin in this campaign measures a configuration
the shipping demo does not have.

### THE PIXELS — chase moves at every pose, city moves only when it is allowed to

`--snapshot=chase@t=100,400,800,1000,1300,1600 --deferred`, one 6-pose sweep per
binary (the PERF_STATE chase arm), 1920×1080:

| pose | changed px | % | max \|Δ\| | mean on changed |
|---|--:|--:|--:|--:|
| chase t=100 | 176 184 | 8.50 % | 51 | 7.67 |
| chase t=400 | 184 900 | 8.92 % | 36 | 3.10 |
| chase t=800 | **559 567** | **26.99 %** | 38 | 2.34 |
| chase t=1000 | 270 805 | 13.06 % | 40 | 5.61 |
| chase t=1300 | 173 217 | 8.35 % | 73 | 4.28 |
| chase t=1600 | 9 079 | 0.44 % | 30 | 6.20 |

city, **his acceptance arm** `--env_live_water --deferred --city_env_pixel`,
five CONSECUTIVE ticks ending on the pose:

| pose | changed px | % | max \|Δ\| | mean on changed |
|---|--:|--:|--:|--:|
| city t=1961 | 277 214 | 13.37 % | 85 | 4.82 |
| city t=2400 | 149 074 | 7.19 % | 44 | 3.16 |

### **CITY'S PINS ARE STRUCTURALLY BLIND TO THIS** — the trap of the round

city t=1961 and t=2400 are **byte-identical** under the shipping flag when run
the way the pin recipes run them, and that is not evidence of a null change:

* `RunCitySnapshot` ticks **once per timestamp**. Measured ladder at t=1961,
  same recipe, deeper history: **1 tick → parent == child byte-identical;
  2 ticks → they differ; 3 ticks → they differ.**
* The cause is visible in the frames: on the FIRST tick of a process city's
  water carries **no mirrored content at all** (`docs/img/reflmir/` — the
  tick-1 frame's water is a smooth caramel sheet; by tick 2 the mech's
  reflection and the reflected facades are in it). chase composites its
  reflection immediately, which is why chase's pins move and city's do not.
* So: **judge city from a warm multi-tick run, never from its pins**, and do not
  read "city pin unmoved" as "look unchanged in city" — continuous play moves
  13.4 % of the frame at t=1961.

### EYEBALLED, MY OWN WORDS, PER POSE (images `docs/img/reflmir/`)

Every pose below: `*_before.png` / `*_after.png` (full frame), `*_where.png`
(magenta = changed), `*_crop.png` (before | after at full resolution on the
hottest reflection window).

* **chase t=100** — the pale, flat, milky wedges hanging under each island
  vanish. Before, the water below the islands reads like frosted glass laid on
  the surface; after, it is dark water with a legible reflection.
* **chase t=400** — the whitish veil across the mid-water (the reflected
  island sheet) thins out; the reflected lighthouse at the left edge deepens
  from washed pink to its actual red banding.
* **chase t=800** — the biggest mover (27 % of the frame) and the one to look
  at first. The reflected lighthouse in the water gains saturation and
  contrast: its red/white stripes read as stripes instead of a fogged ghost.
* **chase t=1000** — the pale reflected-terrain sheet across the mid-water goes
  dark; the hero ship reads with more contrast *because the water behind it
  stopped being washed out*. **The ship's own pixels are untouched** — verified
  on the where-map; there is no main-pass contamination at this pose.
* **chase t=1300** — the clearest "reflections gain identity" pose: the
  foreground island's reflection stops being a milky white-blue wedge and picks
  up the island's own rock tone, and the distant lighthouse's reflection
  resolves into readable red/white bands instead of a pale smear.
* **chase t=1600** — the smallest change (0.44 %, and 16w measured ZERO
  zero-TN triangles here): the reflected ship is slightly better defined and
  the pale ghost sheets dim a little. Subtle.
* **city t=1961** — the mech's reflection in the wet street gains its cockpit
  tint, individually legible limbs and hotter, more saturated engine glows;
  the reflected facade behind it recovers its window grid.
* **city t=2400** — reflected building facades stop being pale smears: the
  striped block's banding is legible in the water and the green accent lights
  come through the reflection.

### WHAT READS **WORSE** — flagged, not buried

**chase's lighthouse light shafts get thinner, and one disappears.**
`Render_DeferredVolumetric` reads **only** `ctx.lights` (zero `OmniHead`/`IPos`
reads in `DeferredVolumetric.cpp` / `DeferredFastFog.cpp`), so cones, halos and
froxel glow **inherit the mirror for free** — correctly, in the sense that a
mirrored world should carry mirrored shafts. chase does **not** pass
`skipVolumetric` (city does, `CITY.CPP:3880`), so its reflected pass was
painting a SECOND, unmirrored shaft from each lighthouse **across the sky above
the horizon**. Those ghost shafts now follow the mirrored light and leave the
sky: at t=800 the left lighthouse's beam reads visibly thinner and the right
one's sky shaft is gone entirely (`chase_t000800_before.png` vs `_after.png`).

More correct — a reflection has no business above the waterline — but it is a
visible dimming of a signature 1998 element, and it is the user's call, not
mine. `--no-refl_correct` restores it exactly.

### NOT MIRRORED, DELIBERATELY — each with its consequence, not a claim of correctness

| consumer | space | status |
|---|---|---|
| specular eye vector | pixel's own view space | **correct, no action** — V is derived from the pixel's own view-space position and the mirrored geometry went through the same `View->Mat` |
| forward per-vertex `Lighting()` | true world | **correct by mirror-equivalence** — this is why the flag is deferred-only |
| cones / halos / froxel | the SoA | **mirrored for free**, visible consequence above |
| shadow tap (`srcShadowMapIdx`/`mirN*`/`mirD`) | reflected receiver | wired; **unreachable in chase** (it never calls `ShadowMaps_Rebuild` — zero `shadow` lines in `CHASE.CPP`), double-gated off in city (`city_test_spots` + `shadows`, both 0) |
| **env-cube tap + parallax** | mirrored-world via unmirrored `viewToWorld` | **LIVE**, not inert (see `env_refl` above). city routes its `city_env_pixel` glass through the forward filler for this reason; that helper uses the REAL camera eye, an approximation of size `2·dist(camera, water)` |
| city DEFAULT-arm mirrored glass | forward, UVs from **last frame's** `Transform_Objects` | pre-existing stale-state defect one field over; **not fixed here** |
| `--sh_ambient` | same unmirrored `viewToWorld` | inert (default 0, nothing in `DEMO/` sets it) |
| SSR history | screen | inert (`env_ssr` 0); chase's capture keys on `skipVolumetric`, which chase never passes — would misbehave if enabled |
| city per-frame shadow bakes (`CITY.CPP:3856/3861`) | unmirrored world | remainder, gated off by default |
| reflected FLARE loops | hardcoded `y = -y` | **outside this flag** — `--no-refl_correct` still gives mirrored flares over unmirrored lighting. There are FIVE spellings of the mirror in the tree; they agree only while `RflSurfNorm` stays axis-aligned through the origin |
| froxel fog temporal history | reprojected | chase defeats city's `skipVolumetric` carve-out; live under `--cinematic`/`--fast_fog`, pre-existing |

### GATES

* **Escape hatch exact:** child + `--no-refl_correct` reproduces the parent on
  **all 12 recipes, byte-identical** (chase ×2 arms, city ×3, fountain, greets
  ×5).
* **greets and fountain do not move — proven by pin, not by argument:** greets
  t=1588 `570a7b44…`, greets acceptance ×4 `440aa6bb…` / `00d17bc5…` /
  `135ea9dd…` / `aaeb89b6…`, fountain `8db68ccb…` — all identical parent vs
  child. Neither scene has a `Reflected_Transform`.
* **city pins do not move either** (see the blindness note): city plain
  `bd4ffbf8…`, acceptance t=1961 `4cb8d2ca…`, t=2400 `f473fe2b…`.
* **chase re-pinned, 7 hashes** — see `docs/SESSION_STATE.md`'s gates table.
* Every row 2/3 runs stable (run 1 discarded), one pose per process where the
  recipe says so, stock 1920×1080 `rev.cfg`, warm cube cache.
* `--shadow_plane_hash` unchanged (`51344bf5f3816c23`): the bake is main-space
  and the mirrored-shadow path is unreachable in both scenes.
* **`tools/render_gate.sh` has ZERO coverage of this change** — its four scenes
  have no `Reflected_Transform`. Stated plainly rather than run for a green
  tick that would mean nothing.

### PRICE

See `docs/PERF_STATE.md` §00j. Measured `on` vs `off` on the SAME binary (LTO
layout held fixed), min-of-rounds.

### HANDED ON

* The **look call on chase's lighthouse shafts** (above) — the one thing that
  arguably reads worse.
* **city's default-arm mirrored glass rasterizes with last frame's env UVs** —
  the same class of defect this round fixed for normals, one field over,
  untouched.
* **chase never passes `skipVolumetric`**, so its reflected pass re-runs SSAO,
  bloom, tonemap, rain, DoF and the froxel populate a second time. That is both
  a correctness hazard (fog temporal history) and, on its face, the largest
  single perf item left in chase.

## 2026-08-16z — 16b's LAST THREE CITY ITEMS, PRICED IN ONE ROUND: two are **below bar and closed with numbers**, and the census that refuted one of them found the item that pays — 61 056 pixels a pass taking the scalar composite because 1512/12 is not a multiple of 8

**Result on `8cc5e5e7` in `/Users/gil-ad/work/rev-fogprice`, city under his arm
(`--env_live_water --deferred --city_env_pixel`) at t=1961 / 2400 / 400,
1512×848: ONE landing, BYTE-NULL at 12 pins; TWO refutations, both with the
ladder that refutes them committed. Every instrument here is compile-time and
proven null — the shipping `DEMO` md5s `44be69e4…` with all four of them
compiled out, identical to its parent.**

Method throughout: `--bench=scene@scene=city,t=<T>,iters=30,xres=1512,yres=848
--profiler=1 --deferred_prof=5 --hw_prof`, `SDL_VIDEODRIVER=dummy`, 12 pool
workers, every arm run once per round with the **arm order rotated by round**,
round 0 dropped. Loads ran 7–18 across the session, so **`Ginstr/f` is the
column that decides** and every verdict below is quoted from it.

### ITEM 1 — `Froxel_GlowTile`'s per-(column × light × slice) `atanf`: BELOW BAR, CLOSED

§00b row 10 priced the SYMBOL at 0.3 % of self time and never counted the
calls. `-DFDS_FOG_ATAN_CENSUS=1` (new, committed, never shipped) counts them
and classifies every slice the glow integral walks:

| pose | coarse cols | (col,light) pairs | slice iters | skipped `b<=a` | **ATANS** | wasted after the atan | contributing |
|---|--:|--:|--:|--:|--:|--:|--:|
| t=1961 | 576 | 18 553 | 610 366 | 19 705 | **609 214** | 2 522 (0.4 %) | 588 139 (**96.5 %**) |
| t=2400 | 576 | 11 421 | 440 556 | 10 175 | 441 802 | 447 (0.1 %) | 429 934 (97.3 %) |
| t=400  | 576 | 11 009 | 317 033 | 12 589 | 315 453 | 1 089 (0.3 %) | 303 355 (96.2 %) |

**Both playbook questions answered by measurement.** *Low-cardinality /
tableable?* **No** — 609 214 arguments a frame, all distinct: the argument is
`(2αz+β)/√disc` with α, β and `disc` varying per (column, light) and `z` per
slice. *Hoistable per column?* **No** — nothing in it is invariant at any loop
level; `twoA`, `beta` and `invD` are functions of the column AND the light.

**And the SSAO playbook's third move — push the cheap tests above the expensive
one — is refuted by the census, without a build.** Only **0.4 %** of atans are
computed for a slice a later test then discards, and deferring the atan costs
an EXTRA one at every contiguous run of contributing slices (the loop carries
`aPrev` across slice boundaries; recomputing it at a run start is bit-exact,
but at 96.5 % contribution the runs are long and the extra dominates).

**A second finding the census hands over: the OTHER atan site is dead in city.**
`Froxel_ColumnTile`'s pass-2 glow loop made **zero** calls at all three poses —
`pass2` needs a shadow-casting or flash light and the coarse glow grid covers
every one of city's. So 100 % of the `atanf` self time §00b saw is the glow
grid's, and `fog-columns`' copy of the loop cannot be a target here.

**THE LADDER** (`-DFDS_GLOW_ATAN=n`, committed; **stage 0 builds a binary
byte-identical to its parent**, md5 `44be69e4…`). 1 = a branch-light minimax
polynomial in place of libm; 2 = the atan deleted outright (the ceiling).
Min-of-7 over 8 interleaved rounds, three binaries in one worktree:

| | t=1961 par → poly → **deleted** | t=400 par → poly → **deleted** |
|---|---|---|
| `fog-glow` Ginstr/f | 0.0920 → 0.0820 (−10.9 %) → **0.0610 (−33.7 %)** | 0.0480 → 0.0430 → **0.0320 (−33.3 %)** |
| `fog-glow` wall ms | 0.719 → 0.653 → 0.517 | 0.401 → 0.370 → 0.293 |
| `fastfog` Ginstr/f | 0.868 → 0.858 → 0.836 (−3.7 %) | 0.542 → 0.537 → 0.527 (−2.8 %) |
| **`renderFrame` Ginstr/f** | 4.176 → 4.166 (**−0.24 %**) → 4.143 (**−0.79 %**) | 3.182 → 3.175 (**−0.22 %**) → 3.167 (**−0.47 %**) |
| `renderFrame` wall ms | 38.15 → 37.81 → 37.47 | 27.66 → 27.65 → **27.69** |

0.031 Gi over 609 214 calls = **51 instructions per `atanf`**, call included —
and `otool` confirms the float overload is what is linked (`bl _atanf`, 24
sites; no double conversion to recover).

**VERDICT: BELOW BAR.** Deleting the atan *entirely* is 0.79 % of frame
instructions at t=1961 and 0.47 % at t=400, and at t=400 the frame WALL does
not resolve it at all (+0.12 %). The one attack the census leaves standing — a
polynomial atan — collects **0.24 %**, under the campaign's 0.3 % bar, and it
is not free: it is a numerics judge call on a *difference* of atans, where the
approximation's absolute error is divided by a per-slice `dAtan` that is small
by construction, so its relative error is amplified by the cancellation. Priced
and not landed; the ladder is committed so nobody re-derives it.

### ITEM 2 — THE `fastfog` PER-SYMBOL FOLLOW-UP: the punt is refuted, its census found the real item

§00b row 6 left `Froxel_CompositePixel` at 3.4 % of self time with the note that
`Froxel_CompositeTileVec8` "punts a whole 8-lane group to the scalar path for
any group containing a water-reflection lane — most of city's lower half". The
obvious follow-up is *punt only the LANES*. `-DFDS_FOG_PUNT_CENSUS=1` (new,
committed, never shipped) prices it before it is built:

| pose | groups | **punted** | refl lanes / punted-group lanes | groups with all 8 lanes reflective |
|---|--:|--:|--:|--:|
| t=1961 | 152 640 | 41 898 (27.4 %) | 315 647 / 335 184 = **94.2 %** | 36 753 (**87.7 %**) |
| t=2400 | 152 640 | 22 846 (15.0 %) | **93.9 %** | 20 115 (88.0 %) |
| t=400  | 152 640 | 31 625 (20.7 %) | **93.4 %** | 27 169 (85.9 %) |

**REFUTED, no build.** A punted group is not a boundary artefact, it is the
water region: 94 % of its lanes genuinely need the scalar path, so lane-level
punting recovers at most 6 % of the punted work — and it would have to run the
scalar lanes FIRST and mask the vector store, because the HDR path reads
`h[3]` and then `h[0..2]`, which the vector pass would already have written.

**BUT THE SAME CENSUS COUNTED SOMETHING NOBODY WAS LOOKING FOR: 61 056 TAIL
PIXELS PER COMPOSITE PASS**, 4.76 % of the frame, going scalar for no reason
but arithmetic. `runTiles` splits X into `tsx = ceil(XRes/12)`, which at 1512 is
**126 = 15 groups + 6 leftover pixels**, and `Froxel_CompositeTileVec8`'s tail
loop hands each leftover to `Froxel_CompositePixel`: 6 px × 106 rows × 96 tiles
= 61 056, per pass, in BOTH composite passes (the reflection underlay punts
nothing at all — `gFrReflZ` is only set for the main pass — so its 61 056 are
pure loss).

**`--fog_composite_tile_align8` (default ON, byte-null): round the composite's
per-tile X span up to a multiple of 8.** At 1512 that makes eleven tiles 128
wide and the last `1512 − 11·128 = 104 = 13 groups`, so the tail loop runs
**zero** times. `12·roundup8(ceil(w/12)) ≥ w` always, so no column is ever
dropped, and a tile that starts past `w` gets `x1 > x2` and both pixel loops
no-op. **Bit-exact by construction and confirmed by pin**: which of the two
composite implementations a pixel goes through cannot change its value (they
are pinned identical and no pixel reads another's output).

**READ THE RESOLUTION BEFORE QUOTING THIS ONE.** `tsx = ceil(XRes/12)` is
*already* a multiple of 8 at **1920** (160), so at his stock `rev.cfg`
resolution the flag is a measured NO-OP. It pays at 1512 (6 tail px/row), 1024
(6), 640 (6), 800 (3), 1280 (3), 1366 (2), 2560 (6) — i.e. at the campaign's
own 1512×848 measurement resolution and most windowed sizes, and not at the
one the demo ships in.

### ITEM 3 — the `--env_live_water` tilt's function POINTER: the call is NOT the cost

§00b's "WHAT IS STILL SCALAR" left the tilt reaching the wave field through
`fds::g_envLiveWater.slopeFn`, an indirect call inside
`Render_DeferredLighting_Tile_OuterVec`'s per-lane loop, and the open question
was whether the INDIRECTION or the ARITHMETIC is what the 0.041 Gi/f buys.
`-DFDS_LWTILT_CENSUS=1` counts the calls and `-DFDS_LWTILT_ABLATE=n` splits the
cost (both new, committed, never shipped; **stage 0 byte-identical to parent**).

`EnvLiveWater_TiltDir` calls per frame: **94 483** at t=1961, 37 429 at t=2400,
**103 538** at t=400 — **all of them in the MAIN pass**; the reflection underlay
pass makes zero, which is why t=400 (more visible glass) costs more than t=1961.

Stage 1 replaces the pointer with a DIRECT call to `pwater::WaveSlope` (a
deliberate layering violation the ladder makes and the shipping tree must not —
FDS never names a DEMO symbol; LTO is then free to inline it too, so this is the
*upper* bound on devirtualization). Stage 2 zeroes the slope — the ceiling for
any restructure. Min-of-6/7 over 8 interleaved rounds:

| | t=1961 par → **direct** → **slope=0** | t=400 par → **direct** → **slope=0** |
|---|---|---|
| `lighting-w1` Ginstr/f | 0.9560 → 0.9520 (**−0.42 %**) → 0.9410 (**−1.57 %**) | 0.7260 → 0.7240 (**−0.28 %**) → 0.7100 (**−2.20 %**) |
| `DeferredLighting-call` Gi | 0.9610 → 0.9580 → 0.9470 (−1.46 %) | 0.7320 → 0.7290 → 0.7150 (−2.32 %) |
| **`renderFrame` Ginstr/f** | 4.1760 → 4.1720 (**−0.10 %**) → 4.1600 (**−0.38 %**) | 3.1800 → 3.1780 (**−0.06 %**) → 3.1620 (**−0.57 %**) |
| `lighting-w1` wall ms | 7.257 → 7.268 → 7.108 | 5.685 → 5.666 → 5.340 |

**THE ANSWER IS "THE ARITHMETIC", AND THE WHOLE ROW IS SMALL.** The indirect
call is **0.0040 Gi/f = 27 % of the tilt's slope evaluation and 0.10 % of the
frame** at t=1961 (0.06 % at t=400) — below bar on its own, and the only way to
collect it is the layering violation above. The arithmetic is the other 73 %.

**AND THE RESTRUCTURE IS REFUTED BEFORE BUILDING, WHICH IS WHAT THE PREDICTION
WAS FOR.** The ceiling for *any* rewrite of the lane walk is stage 2:
**0.38 % / 0.57 %** of frame instructions. 2026-08-16e measured this exact
function's honest vector headroom at **3.6×** (clang had already SLP'd the
scalar 2-wide), so an 8-wide batched form collects at most
`0.015 × (1 − 1/3.6) ≈ 0.011 Gi/f = 0.26 %` at t=1961 — before paying for the
lane-walk restructure of `Render_DeferredLighting_Tile_OuterVec`, whose three
prior attempts in this kernel (2026-08-16h's scanline carry) came back **+8 %
to +23 %** on register pressure and netted zero every time. Predicted net:
negative. **Not built.**

**A CORRECTION TO §00b WHILE WE ARE HERE.** "The cost is the wave-slope call,
not the mask read" is only 37 % right at t=1961: of `--env_live_water`'s
+0.041 Gi/f on `lighting-w1`, the slope evaluation is **0.015** and the mask
read + weight + the tilt's own plane-hit + the re-projection is **0.026**.

### A BATTERY TRAP, REPRODUCED — the plain-city pin needs a WARM env cache

`bd4ffbf8…` (`--snapshot=city@t=1961 --deferred --profiler=0`,
`FDS_CITY_ENV_PIXEL=1`) does **not** reproduce on the first run in a fresh
worktree. With `Runtime/cache/` removed the same binary and recipe gives
**`31035019890c02083af0fb70c3384ed2`**, and the very next run — cache now
written — gives `bd4ffbf8…`. Measured both ways, back to back, on `DEMO_lw0`.
This is the pin the tracked battery runs FIRST, so a fresh worktree reads it as
a one-row failure and the natural next move (blaming the `FDS_CITY_ENV_PIXEL=1`
prefix, which is written as a `VAR=x shellfunc` form that *looks* wrong) is a
dead end — bash does export that form, verified separately. The rule is the
campaign's own "discard run 1", and it applies to the PINS, not only the bench.

### GATES

* **12 pin recipes 3/3 at their recorded values** on the default-ON binary and
  on its parent, byte-identical arm to arm (city `bd4ffbf8` / `4cb8d2ca` /
  `f473fe2b` / `d3374de6`, chase `3bfd4244` / `42d79fad` / `622b96a2` /
  `31aa5203` / `ca07a814`, fountain `8db68ccb`, greets t=1588 `570a7b44`,
  greets acceptance `26ad272a` / `10adec3a` / `418fc1fa` / `6d02f31b`).
* `render_gate.sh` **4/4 PASS** (`conetest` IS the fog path);
  `--shadow_plane_hash` **`51344bf5f3816c23`** 2/2 on each binary.
* The three ladders' stage 0 each build a binary byte-identical to the parent
  (`44be69e4…`), and **the two refutation commits leave the shipping binary at
  exactly that md5** — they are byte-null to the executable, not merely to the
  pixels.
* **PIN-VALUE NOTE AFTER THE REBASE:** these gates were run against parent
  `8cc5e5e7`. This round was then rebased onto `7763281d`, which flips
  `ssao_downscale` 1 → 2 and therefore MOVES the four greets acceptance pins.
  The values quoted above are `8cc5e5e7`'s; the byte verdict for THIS round is
  a differential against its own parent and is unaffected — **re-run pairwise on
  the rebased base, all 11 recipes byte-identical between `7763281d` and this
  tip, 0 mismatches**. For the next round's benefit, the four greets acceptance
  pins **under `ssao_downscale=2`** are t=5743 **`440aa6bb`**, t=2845
  **`00d17bc5`**, t=6097 **`135ea9dd`**, t=6133 **`aaeb89b6`** (same recipe,
  stock 1920×1080, no `FDS_GREETS_CAM`); every other recipe still reads its
  recorded value.

### THE PERF TABLE FOR THE ONE LANDING

`--fog_composite_tile_align8`, three arms in ONE worktree (parent binary /
new binary with the flag OFF / new binary default ON), **min-of-11 over 12
interleaved rounds, round 0 dropped, arm order rotated per round**, `iters=30`,
1512×848. The par-vs-OFF column is the FLOOR and it behaves like one.

| | t=1961 par → off → **ON** | t=2400 | t=400 |
|---|---|---|---|
| `fog-composite` Ginstr/f | 0.3760 → 0.3760 → **0.3650 (−2.93 %)** | 0.3010 → 0.3010 → **0.2890 (−3.99 %)** | 0.3340 → 0.3340 → **0.3230 (−3.29 %)** |
| `fastfog` Ginstr/f | 0.8680 → 0.8670 → **0.8570 (−1.27 %)** | 0.4980 → 0.4980 → **0.4860 (−2.41 %)** | 0.5420 → 0.5420 → **0.5300 (−2.21 %)** |
| **`renderFrame` Ginstr/f** | 4.1740 → 4.1730 → **4.1630 (−0.26 %)** | 2.2660 → 2.2660 → **2.2540 (−0.53 %)** | 3.1810 → 3.1800 → **3.1680 (−0.41 %)** |
| FLOOR (par vs off), `renderFrame` Gi | −0.02 % | **0.00 %** | −0.03 % |
| FLOOR (par vs off), `fog-composite` Gi | **0.00 %** | **0.00 %** | **0.00 %** |
| `renderFrame` wall ms | 37.95 → 37.87 → 38.00 | 23.78 → 23.57 → 23.48 | 27.92 → 27.77 → 28.07 |

**SAY THE WALL PART OUT LOUD: the frame does not resolve this.** Loads ran
10–21 through the session and `renderFrame` wall moves +0.13 / −1.27 / +0.54 %
— noise in both directions. The INSTRUCTION column resolves it at every pose,
reproducibly, against a floor that is **exactly 0.00 %** on the row that
carries the work. 0.0110–0.0120 Gi/f for 122 112 pixels moved off the scalar
path = **≈92 instructions per pixel**, which is the honest unit price of the
per-pixel composite against the 8-wide one.

### WHAT IS LEFT IN THE CITY ARM AFTER THIS ROUND, RANKED

1. **`Render_VolumetricCones_Tile` — `cones-call` 1.288 Gi/f, 30.9 % of
   `renderFrame`.** Untouched by this round and still the largest single row;
   §13's dependency chain, and "fewer (px × spot) pairs" is still the only
   lever named.
2. **`Render_DeferredLighting_Tile_OuterVec` — `lighting-w1` 0.956 Gi/f,
   22.9 %.** Item 3 above prices its `--env_live_water` share at 0.015 and
   refuses the restructure; the rest of the row is greets' 16g/16l territory
   applied to city, which no round has done.
3. **The composite's REMAINING scalar half — 335 184 punted px/frame at
   t=1961, ≈0.030 Gi/f (0.72 %) at this round's measured 92 instr/px.** This
   is the item the punt census sized. It needs the water-reflection branch
   (`sqrt`, `fastExpNeg`'s table, `fogAntiderivG`) written 8-wide, with the
   scalar lanes run FIRST and the vector store masked so the HDR read/write
   order survives. Biggest remaining fastfog item by a wide margin.
4. **`fog-columns` 0.400 Gi/f (9.6 %)** — §00 row 8's "the noise is the cost"
   (`vFogNoise` + `vBlobNoise`) reproduces here; no new lever found.
5. **`gbuffer` 0.443, `TBR-render` 0.531** — both already through their rounds.
6. **CLOSED BELOW BAR by this round, do not reopen without new evidence:** the
   glow `atanf` (ceiling 0.79 %, realistic 0.24 %), the live-water slope
   indirection (0.10 %), lane-level composite punting (≤6 % of the punt).

## 2026-08-16x — THE COLLINEAR-NEEDLE CULL, BUILT: the population two rounds priced **is already free**, and the cull that pays is a different one — the SCREEN determinant, at the push. Byte-null by construction, **and the frame does not resolve it**

16w parked *"a load-time collinearity classification, culled at the FList level;
chase is the scene, 2 302 degenerate rejects a frame."* Built the census first,
and it refuted the premise before the cull was worth writing.

Status: **LANDED, default ON** (`--needle_cull`, `FDS/Base/FaceNeedle.h`) —
byte-null on 12 pin recipes × 3 on three binaries, `render_gate` 4/4, the shadow
plane stream unmoved — **but priced honestly it is a ROW win, not a frame win**,
and the load-time item it came from is closed as REFUTED rather than below-bar.

### THE PREMISE IS WRONG: NOT ONE COLLINEAR FACE REACHES THE FLIST IN chase OR city

`-DFDS_NEEDLE_CENSUS=1` (new compile switch, same shape and the same reason as
`FDS_REFLTN_CENSUS` — atomics on the FList-build path; the shipping `DEMO` md5s
IDENTICALLY with the census macros compiled out) classifies every face the
transform WALKS in **object space** — `Compute_Face_Normals`' own
`|(B-A)×(C-A)| < 1e-6` test, plus 16v's needle test (longest edge == the sum of
the other two, relatively) — and again at the push.

| pass | faces walked | 3-D degenerate | pushed | culled by the screen test | of THOSE, 3-D degenerate |
|---|--:|--:|--:|--:|--:|
| chase t=100 main | 42 932 | **0** | 20 092 | 1 183 | 0 |
| chase t=100 mirror | 42 622 | **0** | 19 967 | 1 104 | 0 |
| chase t=800 main | 54 962 | **0** | 25 839 | 768 | 0 |
| chase t=800 mirror | 54 948 | **0** | 24 989 | 755 | 0 |
| city t=1961 main | 52 979 | 182 | 20 420 | 222 | **0** |
| city t=1961 mirror | 45 840 | 203 | 20 657 | 252 | **0** |
| city arm offscreen | 2 835 114 | 42 140 | 717 354 | 8 854 | **0** |
| city arm mirror | 2 790 318 | 43 195 | 743 643 | 12 490 | **0** |
| fountain t=2500 main | 38 016 | 0 | 19 519 | 54 | 0 |
| greets t=5743 main | 135 120 | 288 | 72 730 | 400 | 0 |
| greets t=5743 shadow | 485 382 | 1 708 | 72 438 | 5 408 | **724** |
| greets t=5743 offscreen | 736 725 | 2 552 | 63 493 | 12 984 | 0 |

Read the last two columns together:

* **chase has no collinear faces at all** — 0 of 42 932 walked, at either pose.
  Its 2 302 rejects a frame are **100 % pose-dependent**: edge-on quads and
  sub-pixel slivers. A load-time scan of `CHASE`'s meshes would have found
  nothing, and 16w's "chase is the scene to build it for" was right about where
  the waste is and wrong about what it is made of.
* **city HAS them** (182 / 203 a pass — 16v's `bilding type 1 windows` family)
  **and not one is ever pushed.** The reason is the chain 16u→16v already
  established, one link further on: a zero-area face keeps the **un-normalized
  zero** `N` that `Compute_Face_Normals` deliberately leaves it, so its backface
  test is `AP·N < NormProd` = `0 < 0` = **false**, and a face that is not
  two-sided never enters the FList. They cost one dot product per pass and
  nothing downstream. **The 525 needles were never paying transform + clip +
  sort; that reading of the 818 rejects was wrong.**
* The one place they DO cost something is **greets' shadow bake**, where
  backface culling is off by design (`shadowNoBackface` — single-sided walls
  must still cast): **724 of the 5 408 faces culled there are 3-D degenerate.**
  That is the entire prize a load-time classification could ever have won, in
  one pass of one scene.

### WHAT DOES PAY: the rasterizers' own test, one stage earlier

`--needle_cull` (default ON) computes, at the FList push, the **same screen
determinant the fan loop computes** and drops the face when
`fabs(det) <= 0.01f` — verbatim the value `Mekalele.h`, `TheOtherBarry.h` and
`ShadowMap.cpp` all reject at. Live at all three builders: `Transform_Objects`
(main, shadow, every offscreen/bake pass) and the two hand-written mirror
transforms (`CHASE.CPP` / `CITY.CPP` `Reflected_Transform` — half of chase's
rejects live there, per 16w). A culled face costs no FList slot, no sort key, no
per-tile walk entry and no `FrustumClipper::Render` (which copies 3 × 140 B per
entry — ~1 MB a frame at chase's cull rate).

### BYTE-NULL BY CONSTRUCTION — AND THE CONSTRUCTION IS VERIFIED IN CODE, NOT ASSUMED

The clip and the mip subdivision build every new vertex through
`FrustumClipper::FInterpolator`, whose **first line** is
`lerp2(&V->PX, &_IA->PX, &_IB->PX, t)` — PX/PY interpolated **linearly in screen
space** between two vertices of the polygon, i.e. a point ON the projected edge.
So every polygon either stage emits lies inside the projected triangle's convex
hull, and every fan triangle has `|det|` no larger than the face's own. Two
guards make that argument legal: **all three verts must be in FRONT of the
pass's near plane** (behind-near PX/PY are stale — this is exactly why the
near-clip case must be excluded, not merely why it is conservative), and
**sprite/flare faces (A == B) are never tested** (C is a float there).

Measured, not just argued — the `REFLTN` census counts at the rasterizer:

| pose / pass | degenerate rejects off → on | faces culled | ACCEPTED triangles off → on |
|---|--:|--:|--:|
| chase t=100 main | 1 191 → **8** | 1 183 | 19 420 → **19 420** |
| chase t=100 reflected | 1 111 → **7** | 1 104 | 19 092 → **19 092** |
| chase t=800 main | 791 → 23 | 768 | 28 355 → **28 355** |
| chase t=800 reflected | 824 → 69 | 755 | 25 220 → **25 220** |
| city t=1961 main | 361 → 135 | 222 | 22 498 → **22 498** |
| city t=1961 reflected | 319 → 61 | 252 | 19 749 → **19 749** |

Rejects fall by exactly the number of faces culled and **not one accepted
triangle disappears**. The residue (7, 8, 23, 69) is the near-plane straddlers
the guard keeps plus slivers that only become degenerate after clipping.

### THE PRICE — 1512×848, three arms (off / on / floor), interleaved and rotated, min AND median

`floor` is a second copy of `off`: the noise bar the `on−off` delta has to
clear. chase pools 16 rounds/arm over two independent ladders; city and greets
7 rounds. Instructions are the trustworthy column (the floor arm reproduces to
±0.00 % on it every time); wall time at this scale is not.

| pose | column | off | on | floor | on−off | floor−off |
|---|---|--:|--:|--:|--:|--:|
| chase t=100 | gbuffer Ginstr | 0.188 | **0.185** | 0.188 | **−1.60 %** | +0.00 % |
| chase t=100 | gbuffer ms (min) | 8.113 | **7.718** | 8.093 | **−4.87 %** | −0.25 % |
| chase t=100 | gbuffer ms (med) | 8.268 | **7.963** | 8.308 | **−3.68 %** | +0.49 % |
| chase t=100 | renderFrame Ginstr | 3.704 | 3.699 | 3.703 | −0.13 % | −0.03 % |
| chase t=100 | renderFrame ms (min/med) | 34.73 / 36.02 | 34.82 / 35.70 | 34.76 / 36.08 | **+0.26 / −0.89 %** | +0.06 / +0.18 % |
| chase t=800 | gbuffer Ginstr | 0.333 | 0.331 | 0.333 | −0.60 % | +0.00 % |
| chase t=800 | renderFrame ms (min/med) | 33.22 / 33.96 | 32.36 / 33.53 | 32.92 / 33.99 | −2.59 / −1.27 % | −0.92 / +0.09 % |
| city t=1961 (his arm) | renderFrame Ginstr | 4.176 | 4.173 | 4.175 | −0.07 % | −0.02 % |
| city t=1961 | frame min ms | 45.95 | 46.30 | 46.04 | +0.76 % | +0.20 % |
| greets t=5743 (acceptance) | renderFrame Ginstr | 4.685 | 4.685 | 4.685 | **+0.00 %** | +0.00 % |
| greets t=5743 | RNDR ms | 41.71 | 41.44 | 41.41 | −0.64 % | −0.72 % |

**The honest reading.** The row that carries the work moves and keeps moving:
chase t=100's gbuffer row is **−1.60 % instructions exactly, every round, on
both ladders**, and **−0.30 ms** (−3.7 % median / −4.9 % min) against floors of
+0.5 % / −0.3 %. **The frame does not resolve it**: `renderFrame`'s wall delta
at that pose is −2.54 % in one ladder and **+1.12 % in the other**, and its
instruction delta is −0.13 % — 0.3 ms of a 36 ms frame is inside the ±1 % wall
noise. city and greets show nothing either way (greets' frame instructions are
identical to four decimals). **So: a row win in one scene, frame-neutral
everywhere, and no scene loses.** It is landed default ON because it is a strict
work-remover that is byte-null by construction and costs ~6 flops on values the
push has already loaded — not because the frame got faster by a number worth
quoting.

### GATES

* **12 pin recipes at their recorded values on THREE binaries** — 3/3 flag-OFF
  and 3/3 flag-ON on the same binary (`79a11fba…`, byte-identical arm to arm),
  and 1/1 on the default-ON build (`44be69e4…`): city `bd4ffbf8` (with
  `FDS_CITY_ENV_PIXEL=1`, 4/4 both arms) / `4cb8d2ca` / `f473fe2b` / `d3374de6`,
  chase `3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain
  `8db68ccb`, greets t=1588 `570a7b44`, greets acceptance `26ad272a` /
  `10adec3a` / `418fc1fa` / `6d02f31b`.
* `render_gate.sh` **4/4 PASS** on the OFF binary, under `FDS_NEEDLE_CULL=1`,
  and on the default-ON binary (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
* `--shadow_plane_hash` **`03587397…`, the recorded value**, 2/2 on each arm and
  on the final binary — the gate that matters most here, because the shadow pass
  is where the most faces are culled (5 408 at greets t=5743) and a wrong reject
  there shows up over TIME, not at one pose.
* **crash**, which no pin covers, byte-identical with and without the cull.
* The census build's own nullity: adding its (no-op) macros left the shipping
  `DEMO` md5 unchanged at `79a11fba…`.

### HANDED ON

* **The load-time item is CLOSED, refuted.** There is nothing at load time worth
  culling: the collinear faces exist only in city and greets, and only greets'
  shadow bake ever pushes them (724). Anyone tempted to re-open it should read
  the census table above first.
* **The two populations should not be conflated again.** "Degenerate rasterizer
  reject" is a per-POSE property (edge-on, sub-pixel) and dominates; "collinear
  authored needle" is a per-MESH property and is already free. 16v/16w's 818 and
  2 302 were the former; the 525 were the latter.
* **A frame-level win, if one is wanted here, is upstream of this.** The rejects
  cost the clipper entry, and the clipper entry is a 3 × 140 B vertex copy
  (16p). Cutting the COPY is the row's real ceiling; this cull only removes 6 %
  of the entries that pay it.

## 2026-08-16w — 16v's 19 092: **chase's reflected pass has never transformed a normal.** `Reflected_Transform` writes position, projection and flags into the shared AoS and no `TN` — the count is 100 % of that pass, exactly explained; the correction is a LOOK CALL and is NOT landed

16v handed on *"chase rasterizes 19 092 of 38 512 triangles with all three
corner `TN == (0,0,0)` while having ZERO vertices with `|N| == 0`; the
transform's counters account for all 23 229 of its TN writes and none of its
four skip sites fire."* Both halves of that were true, and together they were
the clue: **the zero does not come from `Transform_Objects` at all.**

Status: **CHARACTERISED, NOT FIXED.** One census instrument + two counterfactual
arms land, all compile-time-gated and *byte-null to the binary itself* — the
shipping `DEMO` md5s identically before and after,
`45e9aa346dd26fb8d9f72a1f5d6fbcfd` (the one landed change that DOES alter
codegen, the `[MESH]` load warning, is separate and carries the normal
12-pin bar).
The correction itself does NOT land — it moves 0.44 – 13.0 % of the frame at
chase's poses and the direction is a judgement about the look, not about
correctness. Pairs are below, for the user's eye.

### THE MECHANISM — `DEMO/CHASE.CPP:260` `Reflected_Transform`

chase draws its water reflection by **mirroring the geometry, not the camera**:
per frame the tick calls `Reflected_Transform(ChaseSc)` → `Radix_Sort` →
`Render()`, then `Transform_Objects(ChaseSc, …)` → `Radix_Sort` → `Render()`
(`CHASE.CPP:1447` / `1470`). `Reflected_Transform` is a **second, demo-side
transform** — a 1998 copy of `Transform_Objects`' vertex loops — and its three
non-Phong loops (`CHASE.CPP:441`, `465`, `492` — the sites the counterfactual patches) write

```
Vtx->TPos_AOS.{x,y,z}   Vtx->RZ   Vtx->PX   Vtx->PY   Vtx->Flags
```

**and nothing else.** There is no `TN` write and no `TTangent` write anywhere in
the function. It transforms the *same* `T->Verts[]` storage the main pass uses,
so the reflected world is rasterized with whatever the last `Transform_Objects`
left in those fields.

That is why 16v's audit came up empty: it counted `Transform_Objects`' own TN
writes and its four skip sites, and every one of those numbers was right. The
reflected pass is not one of its call sites, so no counter in `Transform.cpp`
can ever see it. **The zero enters in a function `Transform.cpp` does not know
exists.** (Same shape as 16c's `Reflected_Transform` finding, one field over.)

### THE COUNTER — 19 092 is 100 % of the reflected pass, and 19 092 + 19 420 = 38 512

`-DFDS_REFLTN_CENSUS=1` (new, compile-time, never shipped) tallies at the exact
triangle the tiled rasterizer accepts — `Mekalele.h:4179`, immediately after the
`|det| <= 0.01` area reject, so `tris` is the set that goes on to shade — how
many arrive with all three corner `TN` at zero, and on which material. The
scene calls a `noinline` reporter after each `Render()`, which is what separates
the two passes.

chase, `--snapshot=chase@t=100,400,800,1200,1600 --deferred`:

| t | REFLECTED tris | all-TN-zero | MAIN tris | all-TN-zero |
|---|--:|--:|--:|--:|
| **100** | **19 092** | **19 092 (100 %)** | **19 420** | **0** |
| 400 | 22 947 | 2 286 | 22 285 | 0 |
| 800 | 25 220 | 5 793 | 28 355 | 0 |
| 1200 | 6 495 | 3 679 | 24 605 | 0 |
| 1600 | 16 258 | 99 | 16 519 | 0 |

**19 092 + 19 420 = 38 512** — 16v's two numbers, and the split is one pass
each. Not one triangle in the main pass is affected, at any pose. `someTNzero`
is 0 everywhere, which is the whole-primitive signature 16v saw.

**Why t=100 is 100 % and the others are not.** `RunChaseSnapshot`
(`Snapshot.cpp:1173`) calls `driver->tick()` **once per timestamp**, so t=100 is
the first tick of the process and *nothing* has written `TN` yet — the AoS is
still load-zeroed. At the later timestamps the residue is the PREVIOUS
timestamp's main-pass `TN`, and the leftover zeros are exactly the meshes that
pass skipped: 2 286 / 5 793 / 3 064 of them are `'moutines surface'`, chase's
terrain, which scrolls in and out of `Tri_Invisible` between poses. That also
closes 16v's last loose end — *"the meshes whose live `Verts[].TN` is zero are
flagged `Tri_Invisible`, a different set from the one being drawn"*: they are
the same meshes, one pass apart.

In **continuous play** the zero form is a first-frame-only event —
`--snapshot=chase@t=98,99,100,101,102` gives 19 078 all-zero at t=98 and **0 at
every consecutive tick after it**. What survives every frame is the *stale* form:
the reflected pass shading with last frame's MAIN-view normals.

### WHY CITY DOES NOT SHOW IT — measured, with a call counter

`CITY.CPP:439`'s `Reflected_Transform` is the same function with the same
omission, and 16v's probe found city identical at 9 poses. The reason is not
that city is fine; it is that **71 `Transform_Objects` calls have already run
before city's first reflected pass** (its init bakes: env cubes, panoramas,
shadow maps), against **3** for chase — and chase's 3 are shadow-pass calls,
which skip the TN write *by design*. A counter on `Transform_Objects`' entry
prints it in the census build:

| scene | xformCalls before the 1st REFLECTED Render | reflected all-TN-zero |
|---|--:|--:|
| city t=1961 | **71** | 0 of 31 329 |
| chase t=100 | **3** (all shadow-pass) | **19 092 of 19 092** |

So city carries the STALE-normal form of the identical defect, permanently, and
never the zero form. **The missing `TN` write is structural and shared; only its
zero-valued symptom is chase's.**

### THE COUNTERFACTUAL — two arms, because the delta is not what the zero count suggests

`-DFDS_REFLTN_FIX=1` (compile-time, default 0, **not a FeatureFlag, not
shipped**) adds two lines to each of the three loops:

```c
MatrixXVector(IM, &Vtx->N,       &Vtx->TN);
MatrixXVector(IM, &Vtx->Tangent, &Vtx->TTangent);
```

`IM` is this pass's model→reflected-view rotation, staged exactly as
`Transform.cpp:1901` stages the main pass's (`MatrixXMatrix(View->Mat, ReflMat,
M); Matrix_Copy(IM, M)`), with `ReflMat` in place of `T->RotMat`. A reflection
is orthogonal, so its inverse-transpose is itself and the normal takes the
position matrix unchanged. **It drives all-TN-zero to 0 at all five poses with
the triangle totals unmoved** (19 092 / 22 947 / 25 220 / 6 495 / 16 258
identical) — the count is fully explained and fully removed.

But the pixels say the zero was the small half of it:

| pose | changed px | % | max \|Δ\| | mean on changed | 16v probe's zero-lane px |
|---|--:|--:|--:|--:|--:|
| chase t=100 | 94 804 | 4.57 | 53 | 13.9 | 93 426 |
| chase t=400 | 86 996 | 4.20 | 51 | 7.4 | 252 |
| chase t=800 | 181 591 | 8.76 | 67 | 11.8 | 15 870 |
| chase t=1000 | **269 669** | **13.00** | 58 | 8.6 | — |
| chase t=1200 | 27 515 | 1.33 | 107 | 18.3 | 3 868 |
| chase t=1300 | 137 473 | 6.63 | **125** | 15.6 | — |
| chase t=1600 | 9 206 | 0.44 | 70 | 30.0 | **0** |

**t=1600 has ZERO zero-TN triangles and still moves 9 206 px; t=400 had 252
probe pixels and moves 86 996.** The 19 092 is the visible tip: the reflected
pass has never had a *correct* normal at any pose, only an occasionally
non-zero one.

### AND A SECOND DEFECT UNDER IT — the reflection mirrors geometry but not lights

`-DFDS_REFLTN_FIX=2` writes the UNMIRRORED rotation instead (`View->Mat *
T->RotMat` — literally the normal the main pass writes, i.e. today's stale value
made deterministic). Arm 2 against shipping at t=800: **147 841 px but mean 3.6,
max 37**; arm 1 against arm 2: **179 334 px, mean 11.3, max 65**. The
mirrored-vs-unmirrored BASIS is the dominant term, not the zero.

That matters because the deferred light list is built in **MAIN view space** —
`DeferredSurfaceKernel.cpp:7686`, `MatrixXVector(View->Mat, &u, &w)` off
`View->ISource` — and is *not* mirrored with the geometry. In a real planar
reflection both the surface normal and the light flip, and `N·L` is preserved;
here only the normal flips, so arm 1 lights the mirrored world with unmirrored
lights. **Visible, and in both directions**: the reflected ships get brighter
(their hulls become legible instead of black silhouettes), the reflected island
skirts get darker (the pale milky wedges under the waterline mostly vanish).
Neither is obviously right, and the normal alone cannot make it right — **a
correct reflected pass needs the lights mirrored too**, which is a bigger change
than this round is scoped for.

### EYEBALLED — what actually looks different, at full resolution

* **`docs/img/refltn/chase_t000800_threeway.png`** (shipping | arm 1 mirrored |
  arm 2 unmirrored, crop on the hero ship's reflection) is the single most
  legible image. Shipping: a dark blue-grey silhouette, wings unlit, only the
  engine glows read. Arm 1: a fully shaded ship — pale-green wings with visible
  panel texture, lit hull, cockpit detail. Arm 2: indistinguishable from
  shipping.
* **`chase_t000100_sbs.png`**, **`chase_t001300_sbs.png`** — the other
  direction. The shipping frame has a pale, flat, faceted wedge below the
  waterline where an island reflects, reading almost like frosted glass over the
  water; the corrected frame makes it dark and the water reads clean. Whether
  that is a fixed artefact or a lost reflection is the call.
* **`chase_t001000_{sbs}.png`** — the mountains-dominate pose (13 % of pixels
  change, `'moutines surface'` is 9 519 of the reflected pass's triangles) and
  the two FULL frames still read the same at a glance; the change is in the
  water's mid-field.
* Full frames + extent overlay: `chase_t000{100,800}_{shipping,corrected}.png`,
  `chase_t000100_where.png` (magenta = changed).

### WHAT LANDED (all byte-null, none of it changes a default)

1. `-DFDS_REFLTN_CENSUS=1` — per-pass rasterizer census (`Mekalele.h`, reporters
   in `CHASE.CPP` / `CITY.CPP`, `Transform_Objects` call counter). Compile
   switch, not a FeatureFlag: the counters are atomics in the hottest setup
   path, and the question is about a build, not a frame.
2. `-DFDS_REFLTN_FIX=1|2` — the two counterfactual arms, so the pairs above can
   be regenerated. Default 0 expands to `((void)0)`.
3. **`[MESH]` load-time zero-normal warning** (16v's smaller remainder, 16u item
   4). `Compute_Vertex_Normals` (`PREPROC.CPP:220`) has always been able to hand
   out a directionless `N` and has never said so — three rounds were spent
   finding its consequences from the far end. One unconditional stderr line at
   the site that creates the value, self-limiting at 10, naming the material of
   the first face that TOUCHES a zero-normal vertex (16v measured that the
   mesh's first face lies). Fires **once in the whole demo**: greets,
   `66 of 3704 verts … 'momy-2'`. city / chase / fountain silent.

**For items 1 and 2, byte-nullity is stronger than pin equality: the shipping
binary is the same file.** `md5 DEMO` before `45e9aa346dd26fb8d9f72a1f5d6fbcfd`,
after `45e9aa346dd26fb8d9f72a1f5d6fbcfd` — no perf arm is meaningful against an
identical binary. **Item 3 (the `[MESH]` warn) does change codegen** — one
integer increment in `Compute_Vertex_Normals`' final per-vertex loop, plus a
cold branch — and is byte-null the ordinary way instead: `a4db1cd0…` reproduces
all 12 pins 3/3, render_gate 4/4 and the shadow-plane stream. It is a
LOAD-TIME function (`Process_TriMesh` / `Scene_Computations`, both gated by
`Tri_Processed`), not a per-frame one — measured by the warning firing exactly
ONCE per greets process, not once per tick.

### THE CITY NEEDLE CULL — PRICED, AND THE PRICE SAYS NO (status: **PARKED** → **CLOSED 2026-08-16x, REFUTED**: the 525 collinear faces are never PUSHED at all — a zero-area face has a zero normal, so its backface test is `0 < 0` — and chase has ZERO collinear faces; its 2 302 rejects are pose-dependent slivers. A screen-determinant pre-reject at the push was built instead. See the 2026-08-16x block at the top.)

16v's other remainder: *"525 collinear triangles of clipper/transform work per
city frame that no rasterizer can ever fill."* Priced with a degenerate-reject
counter at the same rasterizer site (`degenReject` = triangles killed by
`|det| <= 0.01`):

| scene / pose | REFLECTED reject | MAIN reject | accepted tris | reject share |
|---|--:|--:|--:|--:|
| city t=1961 | 454 | 364 | 55 929 | **1.46 %** |
| chase t=100 | 1 111 | 1 191 | 38 512 | **5.98 %** |
| chase t=800 | 824 | 791 | 53 575 | 3.01 % |

818 setups a frame in city, ~1.3 % of the 63 418 clipper entries 16q measured
there — for a load-time geometry edit that changes the FList's contents and
therefore risks the sort. **Not worth it**, and the 525 needles are not even all
of the 818 (edge-on quads and sub-pixel triangles are in there too). The one new
fact: **chase rejects ~2.8× more than city** (2 302/frame at t=100, 6.0 % of its
triangle setups), so if a degenerate cull is ever built, chase is the scene to
build it for — and it should key on the runtime reject, not on a load-time
collinearity scan of one `.FLD`.

### GATES

* **12 pin recipes 3/3 at their recorded values** on the final tree: city
  `bd4ffbf8`, city acceptance `4cb8d2ca`, chase `3bfd4244` / `42d79fad` /
  `622b96a2` / `31aa5203` / `ca07a814`, fountain `8db68ccb`, greets t=1588
  `570a7b44` — plus a four-pose greets displaced-stone arm
  (`--deferred --greets_displace` at t=5743 / 2845 / 6097 / 6133,
  `c96db000` / `beaa14ef` / `5b5c0063` / `fffeaab0`) run **differentially**
  against the parent binary, 3/3 each.
  **DOC GAP:** the flag list behind 16r–16v's recorded greets acceptance pins
  (`26ad272a` / `10adec3a` / `418fc1fa` / `6d02f31b`) is not written down
  anywhere in `docs/`; eight candidate arms were tried and none reproduces them.
  Worth recording next to those hashes before the next round needs them.
  > **2026-08-16x — CLOSED. The pins were never orphaned; the recipe was in
  > `scratchpad/xform_pins.sh`, 16r's own untracked battery script, and it
  > reproduces all four 3/3 on two binaries first try:**
  > `./DEMO --snapshot=greets@t=<T> --out=<dir> --deferred --hdr --hdr-linear
  > --texture-filter=2 --ssao --ssao-gtao --greets-displace --profiler=0`,
  > one pose per process, **no `FDS_GREETS_CAM`**, stock 1920×1080 `rev.cfg`.
  > The likeliest thing the eight candidates did is set `FDS_GREETS_CAM` —
  > `docs/greets_review_poses.txt` carries a camera for all four t values and
  > the t=1588 pin next door REQUIRES the prefix, but here it moves the hash
  > (t=5743 → `19d94f48…`). Now in the SESSION_STATE gates table as its own
  > row, with the profiler and resolution sensitivities beside it.
* `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` /
  `166fa25a`).
* `--shadow_plane_hash` **identical parent vs final, 2/2 stable each** —
  stream md5 `03587397…`, matching the recorded value.
* Census build proven pixel-null on its own before it was trusted: all five
  chase pins byte-identical with `-DFDS_REFLTN_CENSUS=1` compiled in.

### HANDED ON

* **The look call.** Arm 1 vs shipping, at seven poses, images above. If the
  brighter reflected ships are wanted, the landing shape is the two-line write
  plus a decision about the lights (below); if the pale island wedges are
  wanted, nothing should change and this entry is the reason why.
* **The reflected pass mirrors geometry but not lights** — the deeper defect
  arm 1 exposes. Mirroring the light list for the reflected `Render()` (chase
  and city both) is the change that would make a corrected normal *look* right
  rather than merely be self-consistent. Not attempted here.
* **A third normal-less transform, currently unreachable.** The `Tri_Phong`
  `else` branches — `CHASE.CPP:532`, `CITY.CPP:684`, and `Transform_Objects`'
  own at `Transform.cpp:2420`'s `else` — write no `TN` either (their normal
  rotation is the env-mapping code commented out in 2002). `Tri_Phong` is
  **TESTED in five places and SET in none** (`FDS_DEFS.H:161` defines it;
  nothing assigns it), so no mesh has ever taken those loops. If anything ever
  sets that flag it inherits this same bug. Dead code worth deleting.

## 2026-08-16v — 16u's THREE LOOSE ENDS: city's zero normals are **collinear authored triangles, 1575/1575, not cancellation**; and the normal plane's missing mask is **not latent — it fires 137 207 times a frame in chase**, where today's stored value is decided by the host ISA's NaN→int rule

16u handed on three items. All three are closed here, and the middle one turned
out to be the opposite of what "unreachable hardening" expected.
Status: **DONE** · one instrument (`--zero_normal_census`, default 0) · three
guards (`MakeFacesIndependent`'s degenerate-face clone, the G-buffer normal
plane, the POM march's TBN normalizes) · **0 pixels change at every pin and
acceptance pose**, and the reason is different for each one.

### THE NORMAL PLANE HAS NO `tValid` (16u hand-off #1) — AND THE CASE IS LIVE

*(taken first, because what it found renames the other two)*

`Mekalele.h:3159` normalizes the interpolated view normal with
`approx_rsqrt(n2)` and stores the oct code unmasked. What that produces for a
zero normal is **not** a NaN in the G-buffer — it is a platform decision nobody
made:

| step | value |
|---|---|
| `approx_rsqrt(0)` | `+inf` (measured, this build's simde/NEON) |
| `0 * inf` | `NaN` |
| `_mm256_cvtps_epi32(NaN)` | **0 on arm64/NEON** (measured), `0x80000000` on x86 (Intel SDM: integer indefinite — *documented, not measured here*) |
| packed oct code | **`0x00000000`** → decodes to view-space `(0,0,1)` · x86: `0x80008000` → `≈(0,0,-1)` |

So the same frame is shaded with a camera-FACING normal on this machine and a
camera-AVERTED one on an x86 build, at every pixel whose interpolated normal
vanishes. The guard (`nValid = n2 > 1e-12`, stored word ANDed with it — the
tangent plane's test and its masking shape, verbatim) makes that value a
decision: code 0 *is* `oct_encode(0,0,1)`, so on arm64 it is byte-identical to
what the accident produced, and every other target now agrees with it.

**Byte-nullity therefore proves nothing about reachability here** — the masked
value and the accidental value coincide. The control is a probe build
(`-DFDS_ZERO_NORMAL_PROBE=1`) that stores `0x40004000` (≈`(0.707,0.707,0)`) in
exactly the lanes the guard masks:

| arm | probe vs shipping |
|---|---|
| city 9-pose sweep + acceptance arm, fountain, crash ×2, greets ×4 acceptance | **identical** |
| **chase t=100** | **93 426 px (4.51 %)**, max \|Δ\| 27 |
| **chase t=800** | 15 870 px (0.77 %), max 29 |
| **chase t=1200** | 3 868 px (0.19 %), max 37 |
| chase t=400 | 252 px (0.012 %), max 24 |
| chase t=1600 | 0 px |

**EYEBALLED, and it does not look like 4.5 % of a frame.** The affected surface
is the SUBMERGED part of the islands, seen through the water — dark, low
contrast, and at max \|Δ\| 27/255 the shipping and probe frames read the same at
a glance (`chase_t000100_shipping.png` vs `chase_t000100_probe.png`; the `_where`
overlay is what makes the extent legible). That is the honest size of it: a
large, ISA-dependent region that nobody would catch by looking, which is exactly
why it survived — and why the guard is worth having even though no reviewed pose
moves.

Attribution, from the probe build's per-material counter at t=100:
**137 207 degenerate-normal lane stores**, of which **131 804 (96 %) on matID 13
`'moutines surface'`** — the island skirts where the mountains meet the sea —
plus `'tower'` (lighthouse) 3 206 and the ship hull/engine materials in the
hundreds. Images: `docs/img/zeronorm/chase_t000100_{shipping,probe,probe_diff,where}.png`
and the t=000800 set; `_where` paints the affected pixels magenta over the
shipped frame.

**It is not an authoring defect and not the 16u family.** chase has **zero**
vertices with `|N| == 0` (census below). The zero is in the VIEW-space `TN`, and
it arrives whole-primitive: of the 38 512 triangles the single 1920-wide pass
rasterized, **19 092 have all three corner `TN` at zero and not one has one or
two**. See the hand-off at the bottom — that is its own round.

### CITY'S 'bilding type 1 windows' VERTS (16u hand-off #3): **ALL-DEGENERATE 1575 / 1575, CANCEL 0, ORPHAN 0**

`--zero_normal_census` (new, default 0, byte-null) classifies every vertex with
`|N| <= EPSILON` at both normal-build sites by WHY it is zero: ORPHAN (no
incident face), ALL-DEGENERATE (every incident face zero-area, i.e.
`Compute_Face_Normals`' deliberate zero cross), or CANCEL (real-area faces whose
area-weighted normals sum to zero — the only class that corners fillable
geometry).

City, `t=1961`: **1575 zero-normal verts over 35 meshes** — 21 × 27 (meshes whose
first face is `'bilding type 1 windows'`), 11 × 63 (`'b7 windows'`), 3 × 105
(`'b6 windows'`). Every one of them is **ALL-DEGENERATE**, with exactly ONE
incident face, and that face is **collinear**:

```
face area=0          |F->N|=0 edges=(25,25,50)                height=0
   A=(-40.00000,1001.61292,50.00000) B=(…,25.00000) C=(…,0.00000)
face area=6.1e-05    |F->N|=0 edges=(51.3223,51.3223,102.645) height=1.18925e-06
   A=(-40.00000,1001.61292,-50.00000) B=(5.00000,976.93549,-50.00000)
   C=(50.00000,952.25806,-50.00000)
```

**The hand-off's "a face of REAL area 1.22e-4" is the float residue of a
102-unit-long, 1.2e-6-thick needle** (longest edge = the sum of the other two, to
the printed digit). It is authored data — three collinear points, the signature
of an n-gon strip triangulated through a straight run of vertices — and the
faces belong to the HULL/ROOF surfaces (`'bilding type 1 hull'`,
`'bilding type 1 hull side 1'`, `'b7 roof'`), not to the window material the mesh
is named after.

**The stage is the same one greets used, one step later.** They do not exist at
load (`vnormals` reports 0); they appear at `vtangents`, i.e. after
`MakeFacesIndependentByAngle`. `computeSmoothedNormal` (`MeshOps.cpp:171`) sees
`face->N == 0` for the degenerate clone, so its gate `Dot(face->N, adj->N) >=
cos30` is `0 >= 0.866` for **every** neighbour *including the face itself*, the
accumulator stays empty, and it returns `face->N` — zero. That is 16u's chain
verbatim, with `MakeFacesIndependent` in place of `Compute_Vertex_Tangents`.

**So it is the fixable-computation branch, and the fix is the guard family's:**
when the accumulator is empty AND `face->N` has no direction, inherit
`origVtx->N` — the normal this vertex HAD one stage earlier, computed by
`Compute_Vertex_Normals` from the faces that do have area (the degenerate one
contributed nothing to it). Nothing is invented; if the original is zero too
(greets' Piramid corners, where every incident face is degenerate) the zero
stands and PREPROC's tangent guard owns it, exactly as in 16u.

| scene | zero-normal verts before | after |
|---|--:|--:|
| city (t=1961) | 1575 | **0** |
| crash (t=200, `'screen'`) | 6 | **0** |
| greets (acceptance t=5743) | 1074 | 1058 (216 → **200** on the displaced-stone chunk; the rest are momy-2 needles) |
| chase | 0 | 0 |

**Look delta: 0 px.** 12 pin recipes 3/3 at their recorded values, plus an
18-arm differential (9-pose city sweep, city acceptance arm, fountain, crash ×2,
four greets acceptance poses, chase 5-pose) — **byte-identical everywhere**.

And the hand-off's "inert only because that material carries no NormalMap" is
now counted on the FACE rather than the mesh: **0 of the 1575 sit on a face
whose material has a NormalMap or a HeightMap**, so `Mekalele.h:3816`'s
`writeTangent` is false for all of them.

### THE TWO TANGENT READERS UPSTREAM OF THE MASK (16u hand-off #2)

Both live in `if (ctx.heightData && wantTangent)`: the POM march
(`Mekalele.h:1705`) and `--pom_normal` (`2438`, default 0, and it builds its
bumped normal out of the same N/T/B). `heightData` needs `--parallax` — which is
**default 1** — plus the FACE's material carrying a HeightMap, so the march is
live in the shipping arms, not gated behind an opt-in.

Counted per incident FACE (not per mesh — the mesh's first face lies):

| scene | zero-normal verts on a NormalMap face | on a HeightMap face |
|---|--:|--:|
| city / fountain / crash / chase | 0 | **0** |
| greets, his acceptance arm | 803 | **23** |

So in greets a degenerate vertex IS on height-mapped geometry, and pre-16u those
23 carried the NaN tangent into the march unmasked. Post-16u the vertex value is
finite (+X), but the march's own two normalizes — of the interpolated NORMAL and
the interpolated TANGENT — were still unguarded, and they run BEFORE the plane
masks, so the march is the first consumer of the frame, not the last.

Both are now length-guarded (`> 1e-12`) with the identity as the degenerate
answer: `T` and `B` collapse to zero, `VtT`/`VtB` are zero, the march applies no
UV shift and the pixel keeps the geometric UV it would have had without a height
map. **Byte-null: 12 pins 3/3 at their recorded values with both this and the
normal-plane mask in one binary.**

### PERF — the normal-plane mask is in the hottest per-pixel store path

Both guards are in per-pixel code: the normal-plane mask is one compare + one
8-lane store + one AND per 8 pixels in `apply_exact`'s G-buffer body (the
hottest store path in the renderer, run for every covered fragment of every
opaque face), and the march guard is two compares + two selects per 8 pixels of
every height-mapped face. Priced against the parent binary, **11 interleaved
rounds, `--bench=scene` iters=20, min-of-arm, plus a FLOOR arm that is a
byte-identical copy of the parent**:

| pose | base (min ms/iter) | guard (min) | Δ | floor (base vs its own copy) |
|---|--:|--:|--:|--:|
| greets t=5743, his acceptance arm | 74.410 | 74.463 | **+0.053 (+0.07 %)** | −0.075 (−0.10 %) |
| city t=1961, `--deferred` | 63.025 | 62.598 | **−0.427 (−0.68 %)** | +0.170 (+0.27 %) |

**Not resolvable at this instrument's floor, and the two columns disagree in
sign** — greets is +0.07 % against a floor that itself moves −0.10 %, city is
−0.68 % against a floor of +0.27 %. Read that as "no measurable cost", not as a
city speedup: the same-binary control moves a quarter of a percent on its own.
What is certain is the negative claim the bar asks for: there is no measurable
SLOWDOWN in either column, so nothing was traded for the hardening.

### GATES

* **12 pin recipes 3/3 at their recorded values on FOUR binaries** — the census
  instrument, the city fix, the normal-plane guard, and guard+march-guard: city
  `bd4ffbf8` / `4cb8d2ca`, chase `3bfd4244` / `42d79fad` / `622b96a2` /
  `31aa5203` / `ca07a814`, fountain `8db68ccb`, greets t=1588 `570a7b44`, the
  four greets acceptance poses `26ad272a` / `10adec3a` / `418fc1fa` / `6d02f31b`.
* `render_gate.sh` **RENDER_GATE_PLACEHOLDER**.
* `--shadow_plane_hash` SPH_PLACEHOLDER.
* `--zero_normal_census`: city **1575 → 0**, crash **6 → 0**, greets
  **1074 → 1058**, chase 0 → 0.
* The instrument build reproduces every pin (16u's trap, honoured: a flag-gated
  census is not automatically byte-null, so it was gated *and then proven*).

### HANDED ON — THE BIG ONE: **CHASE RASTERIZES HALF ITS TRIANGLES WITH NO VIEW NORMAL**

Found by the probe, not looked for. At chase t=100, **19 092 of the 38 512
triangles** the main 1920-wide pass rasterizes arrive with all three corner
`TN == (0,0,0)`; none arrives with one or two, so it is whole primitives, not
interpolation. `'moutines surface'` (the island skirts) is 96 % of the resulting
degenerate lanes. What is known:

* chase has **0** vertices with `|N| == 0` — the authored normals are fine.
* The transform's own counters say every TN write it performed went to the AoS
  `Vertex` (23 229 verts at t=100) and **none** of the four sites that skip the
  write fired (shadow pass 0, SoA `mvOut` 0, cube-cull loop 0, refl-cull loop 0).
* The rasterized zero-TN vertices are the clipper's `C_Verts` copies (stack
  addresses), and `FrustumClipper::Render` whole-copies its source vertices while
  `FInterpolator` does carry TN — so the zero comes from the SOURCE vertex.
* The meshes whose LIVE `Verts[].TN` is zero after a tick (17 216 verts:
  `m1..m5/mm7/big_m/lighthouse.lwo`) are all flagged `Tri_Invisible` with `RZ <= 0`,
  i.e. a different set from the one being drawn.

That is where the trail was left. The consequence is a real look question, not a
hardening one: those pixels are currently shaded from a fabricated
camera-facing normal, and giving them their true surface normal WILL move the
frame — so it is a look call, with the guard already in place to make whatever
lands deterministic.

Two smaller ones:

* `Compute_Vertex_Normals` still hands out a zero `N` with no diagnostic (16u's
  item 4). `--zero_normal_census` is now that diagnostic, but it is opt-in; a
  one-line scene-load warning would have surfaced all of this years ago.
* The city needles themselves are still in `CITY.FLD` (the fix makes their
  normals well-defined, it does not remove the geometry). 1575 verts and 525
  collinear triangles of clipper/transform work per city frame that no rasterizer
  can ever fill.

## 2026-08-16u — THE 216 NaN TANGENTS: **ONE cause, and it is not the displacement bake and not greets-only.** Every one of them is a zero-area authored triangle normalized without a guard; the fix is byte-null at every pose measured, and the reason is a rasterizer reject, not luck

16t's loose end: *"216 vertices of greets' displaced `Piramid` chunks carry a NaN
`Vertex::Tangent`. Own round."* Instrumented, classified, fixed, and quantified.
Status: **DONE** · one guard in `Compute_Vertex_Tangents`, its twin in
`DisplaceStoneSmoothNormals` (latent) · `--tangent_nan_census` (instrument,
default 0) · **0 pixels change anywhere measured**.

### THE CAUSE — one, 216 / 216, and every hypothesis in the hand-off was wrong

`--tangent_nan_census` walks the scene at four displacement-bake stage
boundaries plus a final sweep, and prints the vertex normal beside every
non-finite `Tangent`. **All 216 have `|N| == 0`.** The chain, all three links
measured:

1. `Compute_Face_Normals` (`PREPROC.CPP:22-40`) **deliberately** leaves a
   degenerate face's `N` as the un-normalized zero cross — `if
   (Vector_Length(&F->N) < 0.000001) { /* nothing */ } else Vector_Norm(...)`.
   That is the codebase's existing convention for degenerate geometry.
2. `MakeFacesIndependent`'s `computeSmoothedNormal` (`MeshOps.cpp:172-221`)
   inherits it verbatim: with `F->N == 0` the angle gate `Dot(F.N, adj.N) >=
   cos30` rejects every neighbour *including the face itself*, the accumulator
   can't beat `EPSILON`, and it `return face->N` — zero.
3. `Compute_Vertex_Tangents`' perpendicular-axis fallback then computed
   `Cross_Product(&Vtx->N, &ref, &Tangent)` — zero, because `N` is — and called
   `Vector_Norm` on it. `Vector_Norm` is `Vector_Scale(V, 1.0/Vector_Length(V),
   V)` (`MATH.CPP:100`), so a zero vector gives `0 * inf` = **NaN in all three
   lanes**.

**Not the displacement bake.** Identical counts with and without
`--greets_displace` — 66 at load, 216 after `MakeFacesIndependentByAngle`
(72 degenerate faces × 3 per-face clones), unchanged by
`DisplaceStoneSmoothNormals`, 1080 at `Initialize_Greets END` (the same 216 in
five copies: the retired `Piramid`, its four chunks, the mirror clones). No
weld, mitre, corner-treatment or border-densification stage touches them.

**Not the displaced stone either** — the materials are `amudim` 192 (the black
marble pillars), `momy-1` 12 and `momy-2` 12 (the mummies). `rooms`/`floor`
contribute **zero**; `DisplaceStoneSmoothNormals`' own twin fallback is taken
by 0 corners.

**And not greets-only.** The same fallback fires at `Initialize_City` (**1386**
vertex-hits over 35 mesh-passes) and in crash (6); chase is clean. The clone
census found greets' because greets is what it walked.

### WHY NOTHING EVER LOOKED WRONG — a rasterizer reject, stated with the line

All 216 have **exactly one incident face**, and the **largest incident-face area
over all of them is 1.54e-10** (census, post-`MakeFacesIndependentByAngle`).
Every one of the three rasterizers rejects a fan triangle whose screen
determinant is ≈0 *before any setup*:
`Mekalele.h:4012` / `TheOtherBarry.h:1098` / `ShadowMap.cpp:1156`, all
`if (fabs(det) <= 0.01f) continue;`. A needle of world area 1.5e-10 projects to
a determinant twelve orders under that threshold. **Zero fragments, in the
deferred raster, the forward raster and the shadow raster alike.**

And had one reached the tangent lanes it would still not have blackened a pixel:
`Mekalele.h:3189` masks the lane on `tLen2 > vEps`, which is **false for NaN**,
so the G-buffer tangent is written as literal 0; `DeferredSurfaceKernel.cpp:2719`
then fails `packedT != 0` and takes the Mikkelsen ⟂N fallback (`2734-2744`).
Two independent reasons the defect was invisible — which is exactly why it
survived to be found by a byte-compare census instead of by an eye.

### THE FIX — guard the normalize, the way the two normalizes next to it are guarded

```c
Cross_Product(&Vtx->N, &ref, &Vtx->Tangent);
if (Vector_Length(&Vtx->Tangent) > EPSILON) Vector_Norm(&Vtx->Tangent);
else                                        Vector_Form(&Vtx->Tangent, 1, 0, 0);
```

The rule is the surrounding code's own: the branch three lines above is
`if (Vector_Length(&Vtx->Tangent) > EPSILON) Vector_Norm(...)` and
`Compute_Face_Normals` is `if (Vector_Length(&F->N) < 0.000001) {} else
Vector_Norm(...)`. **Length-guard the normalize; do not invent a normal.** The
only new decision is the value, and a zero `N` admits no perpendicular to
derive one from — so it takes the axis the fallback's own `ref` choice leaves
free (`ref` is `(0,1,0)` for every `|N.y| < 0.9`, which a zero `N` always
satisfies, and `+X ⟂ (0,1,0)`). Finite, unit, deterministic: the three
properties the other two branches already guarantee.

Twin fix in `DisplaceStoneSmoothNormals` (`MeshOps.cpp:5751-5779`), whose
comment already says *"matches PREPROC's fallback"* — same hazard, 0 hits
today, kept in lockstep so the two don't drift.

### THE LOOK DELTA — **0 px, and the value is unobservable, not merely equal**

Differential, both binaries built in one worktree from one tree, run 1 discarded:

| arm | result |
|---|---|
| greets acceptance t=5743 / 2845 / 6097 / 6133 | **IDENTICAL**, at `26ad272a` / `10adec3a` / `418fc1fa` / `6d02f31b` |
| greets t=1588 pin | **IDENTICAL**, `570a7b44` |
| greets, camera parked on the `amudim` pillars (they fill the frame) | **IDENTICAL** |
| greets, two cameras parked on `momy-1` / `momy-2` | **IDENTICAL** |
| city 9-pose sweep + both pin arms (colour **and** z) | **IDENTICAL**, `bd4ffbf8` / `4cb8d2ca` |
| chase ×5, fountain, crash ×2 | **IDENTICAL** |

**0 pixels changed, max \|Δ\| 0, at every pose.** And the poses are not blind:
the post-render half of the census projects the degenerate-normal verts through
the frame just rendered — at the t=5743 acceptance pose **744 of the 1080 copies
land INSIDE the 1920×1080 viewport**, at 40 distinct pixel positions, screen
bbox x[200..1833] y[221..479], scattered over lit un-occluded wall.

The strong control: a third binary whose fallback emits **+Z instead of +X** —
a completely different basis — is **byte-identical to the +X one** at every arm
above. So this is not "NaN and the fix happened to agree"; the tangent at those
verts is **not read by anything that reaches a pixel**.

Images (before / after / diff / where):
`docs/img/nantan/greets_t5743_nantan_{before,after,diff,where,crop_strip}.png`,
`docs/img/nantan/greets_amudim_nantan_{before,after,diff}.png`.
`_where` marks the 40 on-screen vert positions and their bbox on the shipped
frame; `_diff` is black because the delta is exactly zero.

### GATES

* **12 pin recipes, base-vs-fix identical AND at their recorded 16f/16t values**:
  city `bd4ffbf8` / `4cb8d2ca`, chase `3bfd4244` / `42d79fad` / `622b96a2` /
  `31aa5203` / `ca07a814`, fountain `8db68ccb`, greets t=1588 `570a7b44`, and
  the four greets acceptance poses.
* **`--tangent_nan_census` 216 → 0** at all five greets stage boundaries.
* `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
* `--shadow_plane_hash` **identical base-vs-fix, 2/2 stable on each** —
  `h=7f0f7d68…`, `cum=6aa86b38…`, the 16t values.
* Perf: the guard is one `Vector_Length` compare on a branch already only
  reached by verts with no usable UV gradient, at **scene-init time only**.
  Nothing per-frame.

### A TRAP THE INSTRUMENT WALKED INTO, WORTH THE NEXT PERSON'S TIME

The first census kept its counters and a `std::vector<float>` as **locals of
`Compute_Vertex_Tangents` itself**, all behind `if (tangent_nan_census())` and
therefore never executed with the flag off. **That alone moved the greets t=1588
pin, `570a7b44` -> `a045c99b`, 3/3 stable on each binary** — while the four
acceptance poses, city, chase and fountain did not budge. Under
`-ffp-contract=fast` + thin LTO, extra live state across that function's two hot
loops is enough to change how the tangent solve fuses, and t=1588 is the recipe
sensitive enough to show it. Moving every counter into a `noinline` reporter
that re-derives its numbers from the mesh afterwards restored `570a7b44` exactly.
**A flag-gated instrument is not automatically byte-null; gate it, then prove
it.** (And: an unexplained pin move is worth chasing to its cause before
attributing it to the change under test — here the change under test was
innocent.)

### LOOSE ENDS THIS ROUND FOUND AND DID NOT TAKE

1. **The normal plane has no `tValid` equivalent.** `Mekalele.h:3159-3163`
   calls `approx_rsqrt(n2)` on the interpolated view normal with **no zero
   guard** — `0 * inf = NaN` — and stores it unmasked. The same degenerate verts
   carry `TN == (0,0,0)`, so a zero *normal* reaching a rasterized triangle is
   the more dangerous of the two. Unreachable today for the same reason the
   tangent was, but the tangent had a mask and the normal does not.
2. **Two tangent readers sit UPSTREAM of the mask.** The POM march
   (`Mekalele.h:1705-1718`, needs `ctx.heightData`) and `--pom_normal`
   (`Mekalele.h:2438-2440`, default 0) consume the interpolated tangent before
   `tLen2` is tested. Moot for NaN now, but neither path is guarded.
3. **City's 21 `bilding type 1 windows` meshes have 9 verts each whose
   `|N| == 0` while cornering a face of REAL area (1.22e-4).** Those are not
   needles — the normal cancels for some other reason — so they are reachable
   geometry. Inert today only because that material carries no `NormalMap` and
   no `HeightMap`, so `Mekalele.h:3816`'s `writeTangent` is false and the lane is
   never produced. That is a material-authoring accident, not a guarantee: give
   city buildings a normal map and those triangles start consuming a tangent
   that (pre-fix) was NaN. Worth a census of *why* those normals cancel.
4. `Compute_Vertex_Normals` will hand out a zero `N` to anything that asks;
   nothing in the tree treats that as an error. A scene-load-time warning would
   have surfaced all of this years ago.

## 2026-08-16t — THE NEVER-INVALIDATED CLONE, ANSWERED BY COUNTING INSTEAD OF ARGUING: **`Pos` is stale ZERO times in 856 M compares**, the two fields that DO go stale are both recomputed downstream, and the real bug next door is a SIZE mismatch, not a value one

**16s handed on "`PerTriMeshClone` is never invalidated, five files write
`Vertex::Pos`, nothing enforces it — worth its own round."** Built the census,
counted, and then made the divergence go away to see whether it had ever
mattered. Status: **DONE** · two switches landed, one latent trap closed ·
`--clone_stale_census` (instrument), `--clone_refresh_inputs` (armed fix),
size-drift invalidation in `cloneOf` (default ON, byte-null).

### WHAT IS ACTUALLY STALE — `--clone_stale_census`, census build

The clone snapshots the whole `Vertex` and the whole `Face` on first use.
`Transform_Objects` rewrites only the projection OUTPUTS (`Vertex` offsets
`[0,52)`: PX, PY, Flags, TPos_AOS, RZ, TN, TTangent), so **everything from
offset 52 on plus the entire cloned `Face[]` is frozen at first use forever.**
The census compares a REUSED clone against the live mesh, field by field, and
counts. Hardest arm — greets, his acceptance flags, 13-pose timeline sweep with
`FDS_GREETS_SHATTER=1` so the shatter's 238-shard / 12-worker reflection bake
(the SECOND clone-backed pass, `MirrorShatter.cpp:1375`) is live:

| | compares | diverged |
|---|--:|--:|
| clone reuses | 630 622 | 330 401 |
| **`Pos`** | 856 176 679 | **0** |
| **`N`** | 856 176 679 | **0** |
| **`Tangent`** | 856 176 679 | **0** |
| tail (`BGRA` … `ShellH`) | 856 176 679 | 355 630 633 — **all `BGRA`** |
| `Face` inputs | 285 626 101 | 644 742 — **all `EU1..EV3`** |
| clone size vs live `VIndex`/`FIndex` | 630 622 | **0** |

Zero in `UZ/VZ`, `EUZ/EVZ`, `U/V`, `EU/EV`, `i`, `OrigBary`, `ShellH`,
`N`/`NormProd`, `U1..V3`, `LwDU/DV`, `Filler`/`Txtr`/`ReflectionTexture`, ids.

**The two that do move are both fields something downstream rewrites**: `BGRA`
is the per-vertex lit colour, rewritten on the LIVE mesh every frame by
`Lighting(Scene*)` (`Lighting.cpp:416`); `EU1..EV3` are the env-map coords the
transform's own face loop stamps per pass, and every one of the 644 742 is on
`__discoBall`. Neither is an input the clone-backed pass consumes.

**A trap for the next person who writes this census: compare BYTES.** The first
version used `!=` on floats and reported a permanent `Tangent` divergence on
four displaced-stone chunks. It was NaN — 216 verts of greets' displaced
`Piramid` chunks carry a NaN `Tangent`, and NaN is `!=` itself, so a frozen,
byte-identical clone reads as diverged forever. (**The NaN itself is real and
unrelated — see the loose end at the bottom.**)

### DOES ANY OF IT REACH PIXELS — `--clone_refresh_inputs`, SHIPPING-shaped binary

Level 1 re-copies the 88-byte vertex input block from the live mesh on every
clone reuse; level 2 also re-copies the `Face[]`. One binary, three arms, eight
greets configurations: the four acceptance poses, the t=1588 pin recipe, the
13-pose sweep, the 13-pose sweep under `FDS_GREETS_SHATTER=1`, and the shatter
at t=5743 — **44 md5s per level, and levels 0 / 1 / 2 are byte-identical
everywhere.** The staleness that exists is unreachable.

### THE VERDICT, AND WHY THE INVARIANT HOLDS

Not luck, and not "no one writes `Pos`" — five files do. It holds because the
only per-frame writer of a live mesh's `Verts[].Pos` is `UpdateMirror`
(`GreetsMirror.cpp:2134`), and its targets — the `__mirrorClone_*` meshes —
carry `Tri_NoShadowCast`, which `Transform.cpp:1567` honours ~245 lines BEFORE
`cloneOf` is ever reached. Same for the disco ball's per-tick `LR/LG/LB`
(`GreetsDisco.cpp:753`). Every other `Pos` writer is scene-load-time, and rigid
animation moves `IPos`/`RotMat`, which the transform reads off the `TriMesh`,
never off the clone.

**One flag was missing.** `BuildCompoundMirrors` (`GreetsMirror.cpp:1884`) built
its clone with `HTrack_Visible | Tri_Noshading` and NOT `Tri_NoShadowCast`,
unlike its base-mirror twin — a mesh re-mirrored every frame by the same
`UpdateMirror` that the shadow bake would have cloned once and read forever.
Inert today (the function has no caller anywhere in the tree) and **the fix is
binary-identical after LTO**; landed so the trap is closed before it is wired up.

### THE BUG THE ROUND WAS AIMED AT WAS NOT A VALUE BUG — IT IS A SIZE BUG

`Transform_Objects` walks the clone to the **LIVE** bound (`VEnd = tVerts +
T->VIndex`, face loop likewise to `T->FIndex`) while the storage is whatever
FIRST use sized it to. A mesh that grows after being cloned is read AND WRITTEN
past its allocation. Two editor paths do exactly that to a live mesh —
`MeshOps_ResmoothSurface` grows `VIndex` to `FIndex*3` (`MeshOps.cpp:249`) and
`DisplaceRebuild_Apply` re-runs the whole subdivision bake (`DisplaceRebuild.cpp:240`)
— both reachable from the material editor mid-session, both on meshes
(`rooms`, `floor`, the `Piramid` chunks) that ARE shadow casters and ARE cloned.
**Not reproduced at runtime** (both are init-time-only headless; the editor
trigger is interactive), so this is a code-level finding, not a caught crash.

Fixed in `cloneOf`: a reused clone whose `verts.size()`/`faces.size()` no longer
match `T->VIndex`/`T->FIndex` is rebuilt instead of returned. **Two integer
compares on the map-MISS path** — the one-pointer-compare fast path is
untouched — and the condition fired **0 times in 630 622 reuses**, so it is
free and byte-null by construction.

### GATES

* **11 pin recipes 3/3, parent-identical, all ten at their recorded 16f/16r
  values** (city `bd4ffbf8` / `4cb8d2ca` / `f473fe2b` / `d3374de6`, chase
  `3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain
  `8db68ccb`, greets t=1588 `570a7b44`) plus the four greets acceptance poses
  (t=5743 `26ad272a`, t=2845 `10adec3a`, t=6097 `418fc1fa`, t=6133 `6d02f31b`).
* `render_gate.sh` **4/4 PASS on BOTH binaries** (`4ac809e5` / `826c09e6` /
  `b41894f9` / `166fa25a`).
* `--shadow_plane_hash` **identical between base and child and 2/2 stable on
  each** — `h=7f0f7d68…`, `cum=6aa86b38…`, all bakes.
* **Perf neutral**, min-of-11 order-rotated, greets t=5743 his arm: `DynOmnis`
  core **10.250 → 10.250** ms (floors 0.78 % / 0.49 %), `DynMeshes` wall
  **0.210 → 0.210**, `DynMeshes` core 0.680 → 0.670; `DynOmnis` wall
  1.190 → 1.200, one printed LSB, and the two columns disagree in direction.
  Mechanism bounds it: the added work is one register test per CLONE-BACKED
  MESH (~2 226 mesh-visits/frame) plus two integer compares on the `cloneOf`
  miss path — nothing in any per-vertex or per-face loop.

### LOOSE END, FOUND ON THE WAY AND NOT THIS ROUND'S ITEM

**216 vertices of greets' displaced `Piramid` chunks (`c149`, `c150`, `c162`,
`c166`) carry a NaN `Vertex::Tangent`** under `--greets-displace` — 1 968–53 388
NaN-vertex hits per census dump depending on how many (map × mesh) pairs are
walked. `Tangent` feeds `TTangent`, which feeds tangent-space normal mapping, so
those verts hand the shading kernel a NaN basis. Nothing in this round's battery
moves because of it (all 44 hashes reproduce their pins), so it is either
absorbed downstream or lands on pixels nobody has looked at. **Own round.**
The census counts them: `--clone_stale_census` prints `NaN-live N` per mesh.
**RESOLVED — see 2026-08-16u above.** One cause, 216/216: every one has
`|N| == 0` (a zero-area authored triangle whose face normal `Compute_Face_Normals`
deliberately leaves un-normalized), and `Compute_Vertex_Tangents`' fallback
normalized that zero cross. Not the displacement bake, not the displaced stone,
and not greets-only. Guarded; **0 pixels move at any pose**, because all three
rasterizers reject a zero-area fan triangle before setup.

### THE OTHER CLONE-LIFETIME BUG, MEASURED: `--bake_tick_overlap` LEAKS **~400 MiB PER FRAME**

Found while establishing where the clones live, and it is the same subject:
clone lifetime is a property of the THREAD. `ShadowScratchTLS` is a
`static thread_local` that is heap-allocated, registered in a process-wide
list and **deliberately never freed** (`Shadows.cpp:97-108`) — a design that is
correct only if the set of threads that bake is bounded. It is not:
`ShadowBake_DispatchGreets` constructs a **brand-new `std::thread` every frame**
under either overlap flag (`Shadows.cpp:1516-1517`), and every one of those
threads allocates a fresh full scratch set on first use.

`--mem_census` at three ticks, greets t=5743, his arm + `--bake_tick_overlap`:

| tick | threads | clones | FList+radix | clone Vertex[] | clone Face[] | clone SoA | **total** |
|--:|--:|--:|--:|--:|--:|--:|--:|
| 2 | 3 | 1 777 | 710.81 MiB | 296.19 MiB | 122.78 MiB | 152.75 MiB | **1.25 GiB** |
| 12 | 13 | 6 857 | 3.09 GiB | 1.13 GiB | 480.91 MiB | 598.97 MiB | **5.27 GiB** |
| 24 | 25 | 12 953 | 5.97 GiB | 2.15 GiB | 910.67 MiB | 1.11 GiB | **10.13 GiB** |

Exactly one thread per frame, **~403 MiB/frame**, unbounded — 24 GiB a minute at
60 fps. Baseline without the flag is flat at 3 rows / 1 269 clones / 209.64 MiB
however long it runs.

**And it silently invalidates any timing taken under those flags**, which is the
part that matters for this campaign: a fresh thread means a fresh clone set, so
every frame pays the full 1 269-clone init copy the design assumes is a
one-time cost. Anyone who has measured bake/tick overlap and concluded it does
not pay may have been measuring cold clone construction.

Both flags default 0 and no scene `setDefault` turns them on, so **nothing
shipping is affected** — which is why this is recorded rather than fixed here.
The fix is one persistent orchestrator thread (the join point already exists:
`ShadowBake_JoinPending`) or moving the scratch off `thread_local` onto a
pool-indexed structure; either is threading work that wants its own round and
its own gates. **Silver lining for the round above: under these flags the clone
is rebuilt every frame, so clone staleness is structurally impossible there —
the census numbers above were correctly taken with the flags OFF, which is the
harder case.**

### ITEM 2 — THE DENSE 32-BYTE OUT RECORD: **CEILING RE-CONFIRMED AT 0.56 %, NOT BUILT**, and two coherency requirements 16s's spec did not name

16s's successor item ("`PerTriMeshClone` gains a dense out record; the shadow
vertex loops write it and read `Pos` from the shared `T->Verts`") was NOT
blocked by anything this round landed — it is made *easier*, since the
"read from the shared array is byte-null" half is now measured at 856 M
compares instead of asserted at two poses, and the size-drift guard enforces
the structural precondition the dense record would also depend on. The ladder
was therefore re-run on the post-fix architecture to check the price had not
moved. **It has not.** greets t=5743, his arm, 1920x1080, `--xfrm_ablate`,
min-of-11 order-rotated, floors quoted:

| arm | DynOmnis wall | floor | DynOmnis core | floor | DynMeshes wall |
|---|--:|--:|--:|--:|--:|
| **32** — ships | 0.870 | 0.00 % | 7.340 | 0.68 % | 0.170 |
| **288** — replica CONTROL | 0.860 | 0.00 % | 7.180 | 2.09 % | 0.170 |
| **1568** — END STATE | **0.610** | 1.64 % | **4.980** | 0.00 % | **0.140** |

**−29.1 % wall / −30.6 % core vs the control**, ΔDynOmnis 0.250 + ΔDynMeshes
0.030 = **0.280 ms/frame = 0.56 % of a 49.6 ms greets frame** — 16s's number to
two decimals, on the new tree.

**Not built this round.** The reason is not the ceiling, it is what the build
has to get exactly right, and this round found two requirements the spec's
"all three loop shapes must keep their own `Flags` semantics" does not cover:

1. **`Vtx_Spike` (0x0040) lives in the same `Vertex::Flags` word** and is
   stamped at scene init (`PREPROC.CPP:213-221`), then read by `RENDER.CPP:1569`
   and `CAMERAS.CPP:344`. The transform's `Flags &= ~Vtx_Visible` (mask 0x003F)
   preserves it *by construction of the mask*. A dense out record with a fresh
   `Flags` field silently drops it.
2. **The `Ahead` loop does not always write `PX`/`PY`/`RZ`.** For a vertex
   behind `nearZ` it writes only `Vtx_VisNear` and leaves the PREVIOUS PASS's
   projection live in the record. So the dense array cannot be
   zero-initialised, and it cannot be per-pass — it has to be per-clone,
   persistent, and SEEDED from the clone `Vertex` at first use.

Add the already-known hazard that the face-loop reader is **not byte-null under
`-ffp-contract=fast`** even for a never-taken branch (`docs/VISIBILITY_PLAN.md`
§8, 216 bytes on city), and the shape is: three loop bodies plus two readers,
every one of which breaks the image *silently* if a bit is dropped, for
**0.56 % of ONE scene's frame** — city, chase, fountain and all four
`render_gate` arms run ZERO clone-backed passes (measured this round with
`--clone_stale_census`: `reuses=0` on all three scenes). Half-done coherency
here is exactly how the next stale-clone bug gets written, so it stays PARKED
with the price and the requirements written down rather than half-built.

## 2026-08-16s — SoA PHASE 5, PRICED BY BUILDING THE LOOP INSTEAD OF THE STRUCT: **0.6 % of a greets frame, not 1.25 %**. The variable is the 209.6 MiB shadow CLONE, not `sizeof(Vertex)` — and neither half of the split pays alone

**16r handed Phase 5 on at "1.25 % of frame, quote this not 0.3 %". That number
is an extrapolation of a byte-slope calibrated by *inflating* `sizeof(Vertex)`
140 → 192, run in the direction it was never measured. Nothing in the tree can
shrink `Vertex` without the refactor, so the PER-VERTEX LOOP was rebuilt as a
ladder and the end state timed. It is 0.56-0.63 %.**
Status: **PARKED / BLOCKED ON SCOPE** · one instrument landed · a better-shaped
successor item specified below. Evidence: `docs/PERF_STATE.md` **00h**,
`docs/SOA_VERTEX_REFACTOR.md` (top section).

### THE LADDER — and every HALF of Phase 5 is neutral or worse

`--xfrm_ablate` bits 256/512/1024/2048 (shadow) and 4096/8192/16384 (main view),
census build only: replicas of the per-vertex loop differing **only in where the
read and the write land** — identical FP sequence in the disassembly, read-source
select unswitched out of the loop. greets t=5743, his arm, 1920×1080, DynOmnis
phase-A `xform`, face loop ablated, min-of-24 × 5 rotated rounds:

| arm | `Pos` read from | outputs written to | wall ms | floor |
|---|---|---|--:|--:|
| **32** — ships | per-light clone `Vertex` (140 B) | the same record | **0.870** | 0.00 % |
| **288** — replica CONTROL | same | same | **0.840** | 1.19 % |
| 544 | clone `Vertex` | dense 32 B/vert | 0.99 **(+16 %)** | 1.01 % |
| 1056 | shared `T->Verts` | clone `Vertex` | 1.13 **(+33 %)** | 1.77 % |
| 2080 | compact shared 12 B/vert `Pos` | clone `Vertex` | 0.87 **(0 %)** | 0.00 % |
| **1568 — END STATE** | **shared `T->Verts`** | **dense 32 B/vert** | **0.600 (−28.6 %)** | 1.67 % |

The verdict rows are **min-of-11, order rotated, floors quoted** (signal-to-floor
17×); the half-arms min-of-5. `DynMeshes` on the same runs 0.170 → 0.130 (−23.5 %).
core-ms column moves the same way: 7.130 / 7.200 / 8.86 / 9.86 / 7.51 / **4.870**.

### THE MECHANISM — `--mem_census` names it

`shadow.scratch/per-light mesh clones (Vertex[])` = **209.64 MiB, 1 269 (map ×
mesh) clones**, of which **68 of every 140 bytes are read-only duplicates**. The
shadow loop is ONE stream over that. Splitting only the write keeps the cold
209.6 MiB and adds a second stream; splitting only the read gives two 140-byte
strides. Doing both collapses the read onto one shared 15 MiB array warm across
all 42 bakes. **The lever is the clone, not the struct** — which is also why the
MAIN VIEW, where no clone exists, measures **±5 % (neutral)** on the same arms.

### PREDICTION vs MEASUREMENT

Predicted 0.62 ms/f = **1.25 %**; measured **0.28 ms/f = 0.56 %** (min-of-11 vs
the replica control; 0.31 / 0.63 % against the shipping arm) — over-predicted
**2.0–2.2×**, outside the ±20 % bar. Reproduces at t=5743 / 2845 / 1588 / 6097 at
**0.61 / 0.61 / 0.66 / 0.68 %**. city and chase bake no per-frame shadow map;
their half is the neutral main-view row.

### BLOCKED ON SCOPE — counted, not estimated

`->FIELD` derefs of the 72 bytes Phase 5 deletes, excluding clipper transients:
`Transform.cpp` 155, **`CITY.CPP` 128**, `FRUSTRUM.CPP` 99, **`CHASE.CPP` 95**,
**`FOUNTAIN.CPP` 51**, `Snapshot.cpp` 17, `Clipper.cpp` 11, `CAMERAS.CPP` 10,
`Raytracer.cpp` 9, `ShadowMap.cpp` 8, `GREETS.CPP` 6, then 1–5 each across
`RENDER.CPP` / `Lighting.cpp` / `RADIO.CPP` / `TheOtherBarry.h` / `SkyCube.cpp` /
`RenderInner.cpp` / `IMGGENR.CPP` / `FaceBBox.h` / `VertexFrame.*` /
`PREPROC.CPP` / `SceneBuilder.cpp`. **274 of them are DEMO scene code**,
including three alternative transform pipelines that must each learn to write
the out array or the image breaks silently — the Phase 6.1/6.2 bug list plus the
one it never found. **0.6 % of one scene's frame does not buy that.**

### THE SUCCESSOR ITEM — shadow-only, and it does NOT touch `Vertex`

Since the prize is entirely in the clone: give `PerTriMeshClone` a dense 32-byte
out record, have the shadow vertex loops write it and read `Pos` from
`T->Verts`, and teach the two clone-backed readers (`Transform_Objects`' face
loop; `FrustumClipper::Render`'s `*A = *F->A` entry) to source from it.
`F->frame` / `F->A_idx` are **already plumbed for clone-backed faces** and
`Shadows.cpp` already reads `F->frame->TPos_z[A_idx]`. No filler, no DEMO scene
file, no layout change. Ceiling **0.6 %**; the risk is that a runtime branch in
that face loop is not byte-null under `-ffp-contract=fast`
(`docs/VISIBILITY_PLAN.md` §8), so it must be built branch-free.

### TWO FACTS WORTH KEEPING

* **The read half is byte-null and worth 0 %.** Sourcing `Pos` from `T->Verts`
  instead of the clone gives identical snapshots (greets t=5743 `818f0336…`,
  t=1588 `756790e4…`, control repeated) and moves nothing (arm 2080). The read
  was already free — the line comes in for the write.
* **`PerTriMeshClone` is NEVER invalidated** (nothing clears
  `VertexScratch::clones` or resets `initialized`), so its `verts` — `Pos`
  included — is a snapshot from that mesh's first shadow bake, while
  `DisplaceRebuild.cpp`, `MeshOps.cpp`, `GreetsDisco.cpp`, `MirrorShatter.cpp`
  and `FOUNTAIN.CPP` all write `Vertex::Pos`. Not stale at the poses measured;
  nothing enforces it. **Own round.**

### ADJACENT, NOT PURSUED — the shadow scratch is ~850 MiB of RESIDENT scratch

`--mem_census` at greets t=5743, his arm, in full — recorded here because the
clone investigation surfaced it and nothing tracks it as an item:

| entry | size | shape |
|---|--:|---|
| `per-light FList + radix scratch` | **447.19 MiB** | 76 map-slots × 2 × Polys=199 700 × `FListEntry`(24) — **actually filled: max 5 617, mean 732 per map = 0.4 % of capacity.** The capacity is the WHOLE SCENE's face count, per light-face |
| `per-light mesh clones (Vertex[])` | **209.64 MiB** | 1 269 clones × VIndex × 140; 68 of every 140 B read-only duplicates |
| `per-light clone VertexFrame SoA` | **108.12 MiB** | same 1 269 × ceil8(VIndex) × 72 B — **all 18 arrays sized, while the shadow path writes 4 and reads 1** (§00g refutation 6) |
| `per-light mesh clones (Face[])` | **86.97 MiB** | 1 269 × FIndex × 170 |

The FList row is a reservation the pass cannot use (a high-water-mark grow would
give back ~445 MiB) and the VertexFrame row sizes 13 arrays no shadow reader
touches (~80 MiB). Both are **memory**, not time — untouched pages cost no
bandwidth — so neither is a perf item; they are a footprint item for whoever
cares about resident set.

### GATES

Instrument is census-only: **the shipping binary is byte-identical to the
parent's** (`md5 f5cc3479…`). 11 pin recipes 3/3 parent-identical, ten at their
recorded 16f/16r values plus the four greets acceptance poses; `render_gate.sh`
4/4; `--shadow_plane_hash` stable 2/2.

## 2026-08-16r — 16q's ROW (`Transform_Objects` at 3.35 %): it is not shared machinery, it is greets' shadow bake — 42 of 45 calls, 81 % of the core time. Six mechanisms refuted, one flag state of dead work landed, and Phase 5's ceiling corrected 4×

> **AMENDED 2026-08-16s (block above): this entry's hand-on — "quote 1.25 %,
> not 0.3 %" — is refuted. Built and timed, Phase 5's end state is 0.56-0.63 %, and
> the variable is the 209.6 MiB per-light clone rather than `sizeof(Vertex)`.
> Everything else here stands.**

**16q handed on "`Transform_Objects` is 3.35 % of DEMO self samples at greets
t=5743, more than double what is left of the clipper". Reproduced to three
decimals (3.363 %) — and at city it is 0.309 % and at chase 0.190 %. The symbol
runs 45×/frame at greets and 42 of those calls are the shadow bake, which city,
chase, fountain and crash do not run at all.** Decomposed by invocation source,
then by ablation, then to the instruction. **Six attacks refuted by measurement,
one landed** — `--greets_displace_offscreen_skip` (default ON, byte-exact):
`Tri_NoShadowCast` spared the shadow bake from 149 fully-displaced greets chunks
and *nothing else*, so the mirror RTT and the env/SH probes were transforming
**54 073 and 151 500 verts a frame** (51 % and 59 % of their totals) and then
dropping every one of those meshes' faces on `Face_MainOnly`.
Status: **DONE** · 16q's row is **CLOSED BELOW BAR** except for one corrected
hand-on · two instruments landed.

Full decomposition, all six refutations and the ranked remainder:
`docs/PERF_STATE.md` **00g**.

### THE SPLIT BY SOURCE, WHICH IS THE WHOLE ANSWER

`--xfrm_pass_prof` (census build; and note it does **not** dump under the
shipping `--xfrm_par` — the main-view call early-returns into the sharded driver
before the accounting and the shards set `_xpassN = 0`. Run `--xfrm_par=0`).
greets t=5743, his acceptance arm, per frame:

| pass | calls/f | core-ms | verts xformed / seen | faces tested | FList pushed |
|---|--:|--:|--:|--:|--:|
| MAIN | 1.00 | 1.93 *(serial)* | 201 751 / 325 474 | 67 560 | 36 413 |
| MIRROR-RTT | 2.00 | 0.83 | 105 914 / 597 552 | 35 648 | 113 |
| **SHADOW** | **42.00** | **11.78** | **596 446** / 4 312 518 | 203 622 | 41 871 |
| OFFSCREEN (probes, intermittent) | 0–2.7 | 0–2.29 | 257 604 / 894 992 | 86 503 | 3 169 |

Wall, shipping build: `DynOmnis` phase A **1.22 ms**, `DynMeshes` **0.22**, main
view **0.48** ⇒ **≈1.9 ms of a 49.5 ms frame (3.8 %)**.

### THE INTERIOR — the ablation ladder, run in the shadow pass for the first time

`--xfrm_ablate` was main-view-only by construction, so it could not see the pass
that dominates. The census build now runs it in every pass and a new bit 128
skips the vertex loops. `DynOmnis` xform ms/frame: baseline **1.22** = vertex
loops **0.83** + per-face visibility/backface test **0.26** + accepted-face work
**0.05** + Face pointer walk **0.01** + residue **0.07** (dispatch + 2.7 MB plane
clear + the 19 782-step object walk + per-mesh setup/culls). The four buckets sum
to the total. The PC-offset histogram puts **31.6 % of the whole symbol on one
instruction** — the loop advance after `tst w11,#0x3f ; b.eq`, i.e.
`VisibilityFlagsAll()`'s three random `Vertex::Flags` derefs; at city, which
bakes no shadow, that same offset is 6.5 %.

### REFUTED, WITH THE MEASUREMENT (not with an argument)

| # | attack | measured | verdict |
|---|---|---|---|
| 1 | SIMD width / arithmetic | whole projection block = 0.01–0.03 of 1.22 ms; `--shadow_cube_vert_cull` moves core 10.67 → **10.77** ms | not ALU-bound |
| 2 | scheduling / `effPar` | 8.4–8.7 of 12 — and this box is **8 P + 4 E cores**; the 28-job wave bound is 9.33 | at the P-core count |
| 3 | `--shadow_cone_cull` (default OFF; 10 of 28 maps are spots) | 10.67 → **10.55** core-ms | inside the floor |
| 4 | `--shadow_cube_face_cull` | OFF: 10.67 → **13.15** (+23 %) | the cull is live, nothing to reclaim |
| 5 | more main-view shards | `--xfrm_par` 26/52/104 → WORK 0.445/0.450/**0.443** | flat |
| 6 | drop the 3 DEAD SoA arrays in the shadow pass (only `TPos_z` has a reader there) | removing **all four** buys 0.10 ms ⇒ three of four ≈ 0.075 = **0.15 % of frame** | refused by arithmetic |

### THE PREDICTION THAT FAILED — and it is the round's best number

The shadow pass touches `[0,28)` (written) and `[52,64)` (Pos, read) of the
140-byte `Vertex` — a 64-byte hot window at a 140-byte stride, straddling two
lines ~98 % of the time. **Predicted: pad to 192 (a multiple of 64) and the lines
per vertex halve.** Measured (`-DFDS_VERTEX_PAD_BYTES`):

| `sizeof(Vertex)` | 140 (ship) | 144 | 160 | **192 (64-aligned)** |
|---|--:|--:|--:|--:|
| `DynOmnis` xform ms | **1.21** | 1.23 | 1.33 | **1.58** |
| xformCore ms | 10.37 | 10.83 | 11.59 | 12.37 |

**Monotone in size; the aligned arm is the worst.** `sizeof(Vertex)` is the only
variable — `docs/SOA_VERTEX_REFACTOR.md` §3's controlled experiment, reproduced
in the shadow pass where it had never been run. Slope **0.0086 ms per byte**
(both bakes).

### THE HAND-ON, AND IT IS A CORRECTION

**SoA Phase 5 (`sizeof(Vertex)` 140 → 68) was CLOSED on 2026-08-09 at "0.24–0.31 %
of frame". That number is main-view-only — `--xfrm_prof`'s buckets are, by
construction.** The shadow pass is 3× the main view's vertex count and 81 % of
this symbol. At the measured slope, 72 bytes is **0.62 ms/frame = 1.25 % of a
49.5 ms greets frame**, four to five times the ceiling the doc closed it on. The
verdict (11 files, two alternative transform pipelines, find every writer or the
image breaks silently) may still stand — the *number* does not. Whoever re-opens
Phase 5 should quote 1.25 %, not 0.3 %.

### WHAT LANDED — `--greets_displace_offscreen_skip` (default ON) + `Tri_AllFacesMainOnly`

`--greets_displace` tags the displaced greets stone `Face_MainOnly` and marks the
**149 chunks that are 100 % displaced** (65 179 faces) `Tri_NoShadowCast`, so the
shadow bake skips them entirely — the flat `Tri_OffscreenProxy` stand-in casts for
them. **`Tri_NoShadowCast` is gated on `g_inShadowPass`, so it spared the shadow
bake and nothing else.** The mirror RTT bakes and the env/SH probes are offscreen
passes too; they transformed those chunks in full and then dropped every one of
their faces on the face loop's first test:

| pass | verts on all-`Face_MainOnly` meshes | of its transformed verts | face-visits dropped |
|---|--:|--:|--:|
| MIRROR-RTT | **54 073/f** | **51.1 %** | 31 894 of 33 866 (**94.2 %**) |
| OFFSCREEN probes | **151 500/f** | **58.8 %** | 73 946 of 86 503 (85.5 %) |

**Byte-exact by construction**: every face of such a mesh carries
`Face_MainOnly`, so in an offscreen pass the face loop `continue`s on all of them
and the mesh emits not one FList entry — the same "no faces ⇒ no output"
invariant the existing `FIndex == 0` skip rests on. The hatch is at **scene
init**, not in the mesh loop: a runtime flag read inside that
`-ffp-contract=fast` function is not byte-null even when never taken
(`docs/VISIBILITY_PLAN.md` §8), which is why the mesh-loop line is flagless and
the FeatureFlag decides only whether the *bit gets stamped*.

Inert without `--greets_displace` (nothing else sets `Face_MainOnly`), and inert
in every other scene.

### INSTRUMENTS LANDED

* **`--shadow-prof` now prints `xformCore` and `effPar`** — the sum of the
  per-light `Transform_Objects` durations against the phase's wall time. Two
  clock reads per light per frame, only under the flag. This phase had never been
  measured by that number, and it is what refutes attack 2 above.
* **Census build (`-DFDS_VIS_CENSUS=ON`) additions**, all textually absent from
  the shipping build: `--xfrm_pass_prof` gains the face-loop split
  (`fTested` / `visRej` / `flagFree` / vertex-loop kind) and the `Face_MainOnly`
  accounting (`mainOnlyFaces`, `allMainOnly meshes/verts`); `--xfrm_ablate` runs
  in **every** pass; new bit **128** = skip the per-vertex loops; bit **4** now
  removes the inline SoA stores rather than only the (already dead) post-pass
  sweep.

### GATES

* **11 pin recipes, 3/3 each, parent (`8dde99fd`) vs child, one worktree, one
  asset tree**: city `bd4ffbf8` / `4cb8d2ca` / `f473fe2b` / `d3374de6`, chase
  `3bfd4244` / `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain
  `8db68ccb` — **all ten at their recorded 16f values** — plus greets t=1588
  `570a7b44` and the four greets acceptance poses (t=5743 `26ad272a`, t=2845
  `10adec3a`, t=6097 `418fc1fa`, t=6133 `6d02f31b`), differential and identical.
  **The acceptance poses are the ones with teeth here** — they carry
  `--greets-displace`, which is the only arm in which this change does anything.
* **`--shadow_plane_hash` identical** parent vs child at greets t=5743 on his arm
  (43 bakes, running digest equal at every `seq`) — the gate 16q built, used
  because this round touches `Shadows.cpp`.
* `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` /
  `166fa25a`).
* **Method note, re-earned:** chase's recorded pins reproduce **without**
  `--profiler=0`; adding it gives a different, self-consistent set of five. 16f's
  "the flag is inert on snapshots again" holds for city / fountain / greets and
  **not** for chase. Run chase's recipe verbatim.

### WHAT IS LEFT, RANKED (greets t=5743, and nothing here clears the bar alone)

1. **shadow per-vertex loops, 0.91 ms (1.8 % of frame)** — memory-bound to the
   byte; the only lever is Phase 5, priced above at 1.25 %.
2. **main-view `Transform_Objects`, 0.48 ms (1.0 %)** — same wall, shard count
   flat 26→104.
3. **per-face `VisibilityFlagsAll` in the shadow pass, 0.26 ms (0.53 %)** — three
   random `Vertex::Flags` derefs, 54.2 % of them buying a reject. A per-mesh
   compact `uint8_t` flags array (1 B/vertex, written by the vertex loop, indexed
   by `F->A - tVerts`) would move only the **rejected** half — the accepted half
   needs the same 64-byte line for the tile-bbox stamp, which reads PX/PY/TPos_z
   from offsets 0..23 of the same vertices. Ceiling ≈0.16 ms = **0.32 % of
   frame**. Below bar, PARKED with the arithmetic.
4. **the 13.2 MB/frame shadow plane clear, 0.51 core-ms** — `sm.dirtyX0..Y1`
   already records the texel rect the previous bake wrote, so clearing only that
   rect is byte-exact and available. ≈0.1 ms wall. Below bar, and it is a
   `Shadows.cpp` row, not this symbol's.
5. **`DynMeshes` phase A `effPar` 3.1–3.4** — 14 jobs, 12 workers, and most of
   its 0.22 ms wall is the clear above, not the transform. Fixing (4) fixes this.

## 2026-08-16q — 16p's ROW: the shadow depth raster had NO pre-reject. It has one now — 231 735 clipper entries/frame → 41 787, and the level is per-TILE, not per-map

**16p found `Shadows.cpp`'s per-(light, tile) face walk handing every survivor of
its four rejects straight to `FrustumClipper::Render` — 231 735 entries a frame
at greets t=5743, 83.4 % of the whole frame's, and 81.8 % of them (189 567)
clipped away to NOTHING. `--shadow_bbox_cull` gives it `RenderInner`'s 4-compare
screen-bbox test, verbatim, against the same rect the clipper was just handed.
Entries 231 735 → 41 787 (−82.0 %), clipped-away-to-nothing 189 567 → 22,
`FrustumClipper::Render` self time 2.265 % → 1.429 % of DEMO self samples, the
`DynMeshes` bake's RASTER half 1.31 → 0.78 ms/frame (−40 %), frame minimum −0.8
to −2.3 % across five greets poses. DEFAULT ON.**
Status: **DONE** · 16p's new row is **CLOSED** · two instruments landed with it
· three follow-ons opened, below.

Full map, both counter columns, the census, the map-rect finding and the
counter-example probe: `docs/PERF_STATE.md` **00f**.

### THE CAVEATS 16p WROTE, ANSWERED IN ITS OWN ORDER

1. **"the per-light clone FList may not stamp bboxes — check before assuming."**
   It stamps them. The shadow FList is built by `Transform_Objects`, whose S2
   stamp is gated only on `--tile_bbox_cull`; its `--xfrm_ablate` escape hatch is
   main-view-only (`xab = (_xablate != 0) && (xresOverride < 0)`, and the shadow
   path always passes `xresOverride = sm.xres`). The PX/PY stamped are
   SHADOW-MAP pixels, because the light's `CameraContext` drives the projection —
   so box and tile rect are already in the same space and `RenderInner`'s test
   transplants with no adaptation at all. Nothing had to be built at FList-build
   time; the data was there and nobody read it.
2. **"the shadow tile is often the WHOLE map — the win may be a map-rect reject."**
   Measured, and it is the OPPOSITE: the map-rect reject is worth exactly zero.
   `FDS_SHADOW_TILE_GRID=1` makes the tile rect BE the map rect, and the
   counter-example probe then finds **0 rejectable faces in 42 bakes** — no face
   in a per-light FList has a box that misses its own map, because the
   frustum-level cull is already done upstream by `Transform_Objects`' spot-cone
   / cube-face bsphere culls plus the walk's all-behind reject. **Every one of
   the 190 365 rejects a frame is a reject INSIDE a map the face overlaps**, and
   the yield is a pure function of `gridFor(res)`: 128² maps are single-tile and
   get nothing, 512² maps are 4×4 and are where the entire win is.
3. **"51 % of the clipper's self time is the near/far Z clip — a pre-reject must
   run BEFORE it and must be conservative."** It runs first, before the `Face`
   deref, and it removes **zero** Z-clip work: `needZ` is **14 514/frame on both
   arms, to the unit**, because a vertex behind the light camera's near plane
   leaves the cover-all sentinel box and can never be rejected. That is also why
   the time saved (−37 % of the symbol) is smaller than the entries removed
   (−82 %) — the cull takes the cheap half and leaves the expensive one. The
   `no-clip` population (the faces that draw with no clipping at all) is
   likewise **31 120/frame on both arms, to the unit**.

### THE CENSUS — `(iters=28 − iters=8)/20`, greets t=5743, his acceptance arm

16p's cumulative TOTAL of 8 499 390 reproduced EXACTLY before anything changed.

| `Shadows.cpp` depth raster, per frame | parent | **child** |
|---|--:|--:|
| entries | 231 735 | **41 787 (−82.0 %)** |
| no-clip | 31 120 | **31 120 — IDENTICAL** |
| emitted | 11 048 | 10 645 |
| **rejected** | **189 567 (81.8 %)** | **22 (0.05 %)** |
| whole-frame clipper entries | 277 777 | **87 829** |
| `needZ`, whole frame | 14 514 | **14 514 — IDENTICAL** |

`emitted` losing 403 is not a loss: it counts "the clip manufactured a vertex",
not "the face drew". A face straddling the tile in X and wholly outside it in Y
manufactures vertices at `Left`/`Right` and is then annihilated by `Up`/`Down`.

### THE PRICE — min-of-11, order ROTATED, one pose per process

`scratchpad/shadowbbox_ladder.sh` (new — `scratchpad/ladder.sh` is BLIND here:
the shadow bake is not inside `renderFrame`, whose `shadow-join` row is 0.002 ms).
Noise floor = max over arms of (2nd-min − min)/min.

| pose | frame min, parent → child | `DynMeshes` RASTER ms | floor (frame) |
|---|--:|--:|--:|
| greets t=5743 (his arm) | 49.92 → **49.51 (−0.82 %)** | 1.31 → **0.78 (−40.0 %)** | 0.57 % |
| greets t=1588 (`--deferred`) | 41.65 → **40.69 (−2.30 %)** | 2.06 → **1.21 (−41.3 %)** | 0.10 % |
| greets t=6097 | 41.76 → **41.30 (−1.10 %)** | 1.17 → 0.75 (−35.9 %) | 0.84 % |
| greets t=6133 | 41.85 → **41.47 (−0.91 %)** | 1.23 → 0.77 (−37.4 %) | 0.14 % |
| greets t=2845 | 50.55 → **49.78 (−1.52 %)** | (bake only, floor 44 %) | 1.67 % |
| city t=1961 (his arm) | 46.14 → 45.89 | **no shadow bake at all** | 0.35 % |
| fountain t=2500 | 36.59 → 36.36 | **no shadow bake at all** | 1.68 % |

`renderFrame` Ginstr/f never moves by more than one printed LSB at any pose —
the correct control, since the bake is outside it. The `DynMeshes` XFORM half is
flat (0.20 → 0.21 ms), so the move is the raster and only the raster.

**WHICH POSES BAKE DYNAMIC SHADOWS — measured, because 16p asked.** Only greets,
and `render_gate.sh`'s `conetest`. city / chase / fountain / crash produce **zero
bake invocations**, with `--shadows` explicitly on as well as off. That is 16p's
"city t=1961 bakes none" generalised: this row cannot help three of the five
scenes, and cannot hurt them either.

### BYTE-EXACTNESS: TWO INSTRUMENTS, NOT AN ARGUMENT

* **`--shadow_plane_hash`** (landed, default OFF, one bool load per bake
  invocation): FNV-1a over the packed plane — depth AND polyId, every byte of
  every texel — of every map a bake wrote, in the tick thread's deterministic
  order; one `[SPH]` line per bake plus a running digest. **43–59 bakes per pose,
  IDENTICAL default vs `--no-shadow_bbox_cull`** at greets t=5743/2845/6097/6133/
  1588/3122/4871, at 640×360, without `--greets-displace`, with
  `--shadow-backface-cull`, with `--shadow-dynamic`, under `--no-tile_bbox_cull`,
  with `FDS_GREETS_SHATTER=1` at t=6293,6294, and on `conetest`.
* **`-DFDS_SHADOW_BBOX_VERIFY=ON`** (CMake option, compile-time — the probe sits
  at the top of a function called ~40 000×/frame): the walk computes the reject
  and does NOT apply it, then counts polygons the raster receives from a face the
  reject would have discarded. **79.7 M rejectable face-visits across ten arms,
  0 polygons.** The one arm that reports 0 rejectable visits is
  `FDS_SHADOW_TILE_GRID=1`, which is finding (2) above.

### GATES

* **11 pin recipes, 3/3, parent-identical** (`DEMO_base` = `5071cc37`): city
  `bd4ffbf8` / `4cb8d2ca` / `f473fe2b` / `d3374de6`, chase `3bfd4244` /
  `42d79fad` / `622b96a2` / `31aa5203` / `ca07a814`, fountain `8db68ccb` — all
  ten at their recorded 16f values — plus greets t=1588 and the four acceptance
  poses, differential.
* `render_gate.sh` **4/4 PASS**, and **`conetest` discriminates**: shadow clipper
  entries **8 448 → 2 496 (−70.5 %)**, reject rate **48.9 % → 6.7 %**, surface
  byte-identical. That is 16p's "your one gate arm with teeth", used.
* Instrument cost (parent → `--no-shadow_bbox_cull`) is null at six poses: frame
  min moves both signs inside the floor, `renderFrame` Ginstr/f ≤1 LSB, and the
  OFF arm reproduces the parent's `--clip_stats` census to the digit.
* One render eyeballed per scene: `docs/img/shadowbbox/`.

### WHAT THIS ROW HANDS ON

1. **`FDS_SHADOW_TILE_GRID` is now the wrong shape and nobody has re-asked it.**
   The 4×4 grid was tuned when every tile re-walked the whole FList for free;
   §00d re-ran exactly that question for the FRAME raster grid and found the
   answer had moved. Here the reject changes the trade in the opposite
   direction — more tiles are now cheap to ADD (a rejected face costs one
   sequential `FListEntry` read) but the 512² maps at grid 4 still leave 16
   whole-list walks per map. A per-light FACE→TILE BIN, i.e. `--face_tile_bin`
   for the shadow pass, would collapse the 237 609 pair-visits a frame to two
   list walks; `FaceTileBin.cpp` already exists and its order-preservation proof
   already covers this shape. Not built — the reject took 80 % of the prize for
   four lines.
2. **The `DynOmnis` bake is now the bigger of the two and is untouched by this
   row** — 1.21 ms raster against `DynMeshes`' 0.78, over 28 maps of which 18 are
   single-tile 128². Its cost is per-map fixed overhead and raster, not clipper
   population; a different row.
3. **`Transform_Objects` is 3.35 % of DEMO self samples at greets t=5743, more
   than double what is left of the clipper.** The shadow pass runs it once per
   light per frame. That is the next number on this path, and this round did not
   touch it.

## 2026-08-16p — 16c's HANDOVER ("cutting it further means cutting the copy"): the copy is 2.6 % of the clipper, the row is CLOSED BELOW BAR, and greets' clipper is 83 % SHADOW

**16c closed 00b row 3 and handed on one sentence about what was left:
"`FrustumClipper::Render`'s residue is the per-(face, tile) clip itself — three
140-byte `Vertex` copies … Cutting it further means cutting the copy, not the
traversal." Priced at the five acceptance poses, that sentence is wrong by a
factor of 10 to 40. The three copies are 2.6 % of the clipper symbol's self time
(0.033 % of frame at city t=1961, 0.35 % at its worst pose); copies +
`MiplevelClipper` — the residue exactly as 16c named it — is 0.25 / 0.25 / 0.48 %
of frame at city t=1961 / chase t=800 / greets t=5743, BELOW the 0.5 % bar at
every pose. All three candidate mechanisms are dead. Landed: the instrument
only, `--clip_stats`. BYTE-NULL — 11 pin recipes identical parent-to-child 3/3,
`render_gate.sh` 4/4.**
Status: **DONE (closed below bar)** · the copy row is **CLOSED, do not reopen** ·
one NEW row opened, below.

Full map, both instruments, both counter columns and the disassembly prediction:
`docs/PERF_STATE.md` **00e**.

### THE PRICE — `sample` self time, share of DEMO self samples, one pose per process

`MiplevelClipper` is inlined into `Render`, so the symbol IS the whole
per-(face, tile) clip. 16c's 1.21 % at city t=1961 reproduces at 1.279 %.

| pose | `FrustumClipper::Render` |
|---|--:|
| city t=1961 (his arm) | 1.279 % |
| city t=2400 | 1.360 % |
| city t=400 | 0.740 % |
| chase t=800 | 1.012 % |
| chase t=1600 | 0.847 % (thin sample) |
| **greets t=5743 (his arm)** | **2.270 %** |

Split by a throwaway `noinline` probe (`YSort`, `MiplevelClipper`, and a
`ProbeCopy3` wrapper each forced to their own symbol) and cross-checked against
a PC-offset histogram taken on the SHIPPING inlined binary — `sample`'s call
tree carries `+offset`, so summing self samples per offset and mapping onto
`otool -tvV` attributes the symbol with no code change at all:

| component | city t=1961 | chase t=800 | greets t=5743 |
|---|--:|--:|--:|
| `Render`, the rest (UV stamp, `Calc_Flags`, Z clip, 2D clip) | 0.726 % | 0.638 % | **1.583 %** |
| `YSort` | 0.299 % | 0.238 % | 0.264 % |
| `MiplevelClipper` | 0.162 % | 0.169 % | 0.133 % |
| **the three `Vertex` copies** | **0.095 %** | **0.077 %** | **0.347 %** |
| `FInterpolator` | 0.045 % | 0.046 % | 0.044 % |
| `fds::stats_tls` (TWO calls per visit) | 0.039 % | 0.100 % | 0.087 % |

The `noinline` column OVER-states the copy (it serialises three scattered vertex
loads the inlined build overlaps with the UV stamp, and adds a call per visit);
the offset histogram on the shipping binary reads 0.033 / 0.066 / 0.009 %. The
noinline figure is the ceiling and it is still below bar.

### WHY (a) COPY ELISION IS IMPOSSIBLE, NOT MERELY UNPROFITABLE

The brief's precondition — "if the clipper only READS the source vertices on the
common no-clip path" — is false. `Render` mutates what it copies, twice, and
both mutations are the reason the copy exists at all:

* **per FACE**: `A->U = F->U1; A->UZ = F->U1 * aRZ` (+ `EU/EV/EUZ/EVZ` when
  reflective). UVs live on the FACE; one mesh vertex is shared by faces with
  different UVs, so stamping into the source corrupts its neighbours.
* **per TILE**: `Calc_Flags` recomputes `Vertex::Flags` against THIS tile's
  rect, and a face is visited by 1.45 tiles on average.

Both writes land 4 instructions after the copy, so there is no
"pointer-until-first-mutation" window — and the tile jobs run on 12 pool workers
at once, so a write to a shared mesh `Vertex` is a data race between tiles as
well as wrong. Census-independent; the census only sizes a population that could
never have been used.

### WHY (b) SHRINK THE PAYLOAD IS REFUTED, WITH THE PREDICTION THAT SAID SO FIRST

A full field audit (the clipper's own `C_Prim`/`C_Scnd`/`_IA`/`_IB`/`newVert`,
`MekaleleImpl<>` ×3, all 15 `TheOtherBarry<>` instantiations,
`MekaleleShadowDepth`, and their internal `vc[]` re-copies) finds the 36-byte
object-space `Pos`/`N`/`Tangent` block at `[52,88)` is never read through a
clipper copy — `Mekalele.h:1016-1019`'s `F->A->Pos` is the ORIGINAL vertex via
`F->A`, a different pointer. So a 104-byte copy is SAFE. Three binaries from one
worktree, min-of-11, order ROTATED per round, one pose per process:

| pose | column | 140 B | 104 B (safe) | 52 B (ceiling, incorrect) | 1 printed LSB |
|---|---|--:|--:|--:|--:|
| city t=1961 | `renderFrame` Ginstr/f | 4.1760 | 4.1740 | 4.1730 | 0.02 % |
| | `renderFrame` Gcyc/f | 1.1070 | 1.1060 | 1.1070 | 0.09 % |
| greets t=5743 | `renderFrame` Ginstr/f | 4.6790 | 4.6800 | 4.6780 | 0.02 % |
| | `renderFrame` Gcyc/f | 1.2720 | 1.2670 | 1.2690 | 0.08 % |
| chase t=800 | `renderFrame` Ginstr/f | 3.4670 | 3.4680 | 3.4660 | 0.03 % |
| | `renderFrame` Gcyc/f | 0.7890 | 0.7840 | 0.7920 | 0.13 % |

Every instruction reading is inside one printed LSB (0.024 % on `renderFrame`
Ginstr/f), and the cycle column's larger moves are in an impossible order — at
chase the 26 % cut reads −0.63 % and the 63 % cut reads +0.38 %. **The
disassembly says it before the stopwatch does:** the copy is 43 arm64
instructions; the 52-byte ceiling removes ~18 of them per face-visit, which at
city's 63 418 visits/frame is 1.14 Minstr against `renderFrame`'s 4.176 G =
**0.027 %**. There is nothing there to measure. **(c) SIMD is the same
arithmetic with a smaller numerator** — clang already emits 128-bit `ldr q` /
`str q` pairs for the `pack(1)` struct assignment, the widest arm64 has.

### THE INSTRUMENT THAT LANDED — `--clip_stats`

Default OFF, changes no pixel, one bool load per face-visit when off. Prints an
atexit per-(face, tile) census SPLIT BY DISPATCHER (`fds::ClipSrc`, stamped once
per tile job via `FrustumClipper::SetClipSource`, never per face): entries,
`no-clip` (wholly inside this tile rect), `emitted` (the clip manufactured a
vertex), `rejected` (clipped away to nothing — the copy and the stamp bought no
pixels), plus `needZ` / `need2D` and the mip branch split. The per-face cost is
unchanged: the flat `clipperEntered++` became an indexed `clipperEntered[src]++`,
the same single increment.

**The split is the whole point of the instrument, and it is what found the next
row.** Per-frame figures are `(iters=28 − iters=8)/20` on the same `--bench`
arm, which subtracts init, the env bake and the shadow prebake exactly.

### THE NEW ROW — greets' clipper is 83 % SHADOW RASTER, and 82 % of THOSE entries are thrown away. **DONE 2026-08-16q** — `--shadow_bbox_cull`, 231 735 entries/frame → 41 787, default ON, byte-exact. The three caveats below are answered one by one in that entry; the short version is that the FList DOES carry bboxes, the reject is per-TILE (a map-rect reject is worth exactly zero), and it removes no `needZ` work at all.

**greets t=5743, his arm — 277 777 clipper entries per frame (4.4x city's):**

| dispatcher | entries/f | no-clip | emitted | **rejected** |
|---|--:|--:|--:|--:|
| **`Shadows.cpp` depth raster** | **231 735 (83.4 %)** | 13.4 % | 4.8 % | **81.8 % = 189 567/f** |
| `RenderInnerMekalele` (gbuffer) | 44 629 | 71.8 % | 26.7 % | 1.5 % |
| xpar strip raster (surface kernel) | 1 300 | 13.8 % | 50.8 % | 35.4 % |
| `MekaleleFillRegionInline` (RTT) | 113 | 21.2 % | 78.8 % | 0.0 % |
| **all** | **277 777** | 22.8 % | 8.5 % | **68.7 %** |

city t=1961 for contrast: **63 418 entries/f and ZERO shadow entries** — it bakes
no deferred shadow map at that pose — split `RenderInnerMekalele` 47 681 (3.3 %
rejected), xpar strip raster 15 442 (2.2 % rejected), forward tile job 295. The
tiled G-buffer pass is healthy in BOTH scenes; the shadow pass is not, and it is
the only reason greets' clipper is 2.27 % where city's is 1.28 %.

`Shadows.cpp:894` walks the whole per-light face list and hands every survivor
of its four rejects (no-`Txtr`, non-occluder material, degenerate, all-behind)
straight to `clipper.Render`. **There is no screen-bbox / tile pre-reject on that
path at all** — the S2/B5 `--tile_bbox_cull` and `--face_tile_bin` that 16c
built serve `RenderInner*` only. Every one of those 189 567 rejected entries per
frame pays three 140-byte copies, the UV stamp and `Calc_Flags` before the 2-D
clip discards the polygon. This is the same SHAPE of defect 16c found in
`Reflected_Transform` (a pass the cull never reached), in a different function,
and it is the reason greets' clipper is 2.27 % where city's is 1.28 %.

Caveats for whoever takes it, stated up front so the round is not lost to them:
the shadow FList is built per LIGHT by a per-light clone transform, so the
`FListEntry` bbox may not be stamped there — check before assuming
`fds::FaceBBox_Stamp` can just be called; and the shadow raster's "tile" is
often the whole map, in which case the win is a per-face reject against the
map rect, not a per-tile one. Byte-exactness should follow from the same S2
argument 16c used (a conservative superset box, and the clipper only shrinks
coverage) but must be re-derived for the light frustum, whose near/far the
geometry straddles — `needZ` is where 51 % of greets' clipper self time sits.

### THE OTHER LEFTOVERS, ALL BELOW BAR ON THEIR OWN — PARKED

* **`YSort`, 0.24–0.30 % of frame** at all three poses. Two variable-trip loops
  rotating ≤7 pointers; `nVerts == 3` dominates and a branchless 3-way
  specialisation is a pure permutation, i.e. trivially bit-exact.
* **two `fds::stats_tls()` calls per visit, 0.04–0.10 % of frame** — `Render`
  captures the per-thread block with `FDS_STATS_SCOPE`, then `MiplevelClipper`
  captures it AGAIN (separate function, opaque cross-TU call even when inlined):
  107 122 calls/frame at city t=1961. Passing the captured reference down is
  bit-exact.
* **the UV stamp's first `Face` load** (`ldr d3, [x23, #0x30]`) is the single
  hottest instruction in the symbol at city — 13.2 % of its self time. It is a
  cache miss on the `Face`, not arithmetic, so it is a layout question, not an
  instruction-selection one.

## 2026-08-16o — 16h's ITEM 1, the three oct decodes: the 2-wide pairing it named is REAL but a quarter of the prize — the win needs the CENTRE in the same vector

**16h left "the three oct normal decodes, ~0.078 Gi/f = 29 % of the fill" as its
item 1 and named a `oct_decode_u32_x2` over the TWO NEIGHBOURS as the bit-exact
candidate. Built both. The 2-wide is worth −2.6 to −3.6 % of `lighting-w2`; a
4-wide that also carries the CENTRE on lane 2 is worth −11.9 to −13.4 %, at
every one of six poses. Shipped FLAGLESS and BIT-EXACT — 0 mismatches in 57.3 M
lanes under a bit-pattern verify counter, plus 26 byte-identical surfaces.**
Status: **DONE** (the 4-wide) · **MEASURED AND SUPERSEDED** (16h's named 2-wide)
· **REFUTED BY CONSTRUCTION, not built** (16h's skip-when-equal fast path).

Commit: this one. `lighting-w2` **0.272 → 0.239 Gi/f at t=5743**, so the row
16g opened at 0.306 and 16h took to 0.272 now stands at **0.239 = 13.8 % of
`DeferredLighting-call`, 5.1 % of `renderFrame`**.

### THE BIT-EXACTNESS ARGUMENT IS THE DISASSEMBLY, AND IT IS READ BEFORE IT IS CLAIMED

`otool -tvV` on the SHIPPING binary's `Render_DeferredLighting_TileFill` (four
`sxth` sites: the centre decode, the checkerboard neighbour decode, the quarter
neighbour decode, and the wave-1 replay's) gives the scalar's exact codegen at
`-O3 -ffp-contract=fast`. Every operation in it is ELEMENT-WISE on AArch64:

```
sxth / asr #16          the two int16 halves                    (integer)
scvtf ; fmul kQ         ox, oy
fabs ; fsub ; fsub      az = (1-|ox|) - |oy|                    (ORDERED)
fcmp az,0 ; b.pl        the fold, as fneg + fcsel               (7 instructions)
fmul  len, ox, ox
fmadd len, oy, oy, len  <- FUSED, and the ORDER matters
fmadd len, az, az, len  <- FUSED
frsqrte.2s / fmul.2s / frsqrts.2s / fmul.2s      fast_rsqrt, ALREADY vector
fmul x3                 nx, ny, nz
fmul  s22, s1, s22      ncY*ny        <- the dot the compiler chose:
fmadd s22, s0, s23, s22   + ncX*nx       fma(ncZ,nz, fma(ncX,nx, ncY*ny))
fmadd s22, s2, s24, s22   + ncZ*nz
fcmp ; b.lt             dot >= --quarter_normal_cos
```

`meka::oct_decode_u32_x4` reproduces that sequence with intrinsics — `vfmaq_f32`
for the two fused adds, in the same order, so no reassociation is introduced;
`vrsqrteq_f32` + `vrsqrtsq_f32` for `fast_rsqrt`, which are architecturally
element-wise (the `.4s` form gives each lane what the `.2s` form gives it); and
`vmulq/vfmaq` for the dot, term for term. **The one shape change is the `az < 0`
fold: the scalar BRANCHES over it and the vector must SELECT, because the lanes
disagree.** Both arms are finite for every input word (`|ox|,|oy| <= 1`), and
`ox` can never be `-0.0f` (it is `scvtf` of an integer times a positive scale),
so the selected bits are the branch's bits. `b.pl` and `b.lt` are the ordered
forms, matching `>=` on NaN — which cannot arise here anyway.

**And it is checked, not just argued.** `-DFDS_W2_OCTPAIR_VERIFY=ON` (committed,
runtime gate `--omni_census`) runs the scalar decode behind every lane and
compares BIT PATTERNS of `nx/ny/nz`, of the dot, and of the verdict:

```
greets, his arm, 1512x848, six poses x 5 frames
centre lanes 19.1 M   neighbour lanes 38.2 M   mismatches 0 / 0 / 0
```

**57.3 M lanes, zero disagreements** — normal bits, dot bits and verdict alike.

### THE FOLD IS THE COMMON CASE, WHICH IS WHY THE SELECT IS CHEAP

The same counter prices the branch the vector gives up: the `az < 0` fold is
taken on **62.8 % (t=5813) to 95.2 % (t=6097)** of lanes — 72.4 % at t=5743.
So the scalar's `b.pl` is a data-dependent branch that *falls through* only
5–37 % of the time, and the seven instructions the wide form always pays are
seven the scalar mostly paid anyway. **This is the opposite of what a
"branchless costs extra work" instinct predicts, and it is why the fold was
never the thing to worry about.**

### MEASURED — six poses, five arms, one worktree, one asset tree

Interleaved, order-rotated, **min over 11 kept rounds of 12** (round 1 dropped),
one pose per process, 1512x848, `--profiler=0 --deferred_prof=1 --hw_prof`, his
acceptance arm. Load ran **4.9–10.9**. `off` = the HATCHED child with
`--no-deferred_fill_oct_pair`, i.e. the restructure priced by itself.
`x2` = 16h's named two-neighbour pairing, flagless.

| pose | `lighting-w2` Gi/f par | off | **x4 ON** | x2 | Gcyc/f par -> ON |
|---|--:|--:|--:|--:|--:|
| t=2845 | 0.2750 | 0.2780 (+1.09 %) | **0.2400 (−12.73 %)** | 0.2660 (−3.27 %) | 0.0550 -> 0.0490 (−10.91 %) |
| t=3409 | 0.2710 | 0.2740 (+1.11 %) | **0.2360 (−12.92 %)** | 0.2620 (−3.32 %) | 0.0570 -> 0.0510 (−10.53 %) |
| t=5743 | 0.2720 | 0.2740 (+0.74 %) | **0.2390 (−12.13 %)** | 0.2640 (−2.94 %) | 0.0550 -> 0.0510 (−7.27 %) |
| t=5813 | 0.2700 | 0.2730 (+1.11 %) | **0.2380 (−11.85 %)** | 0.2630 (−2.59 %) | 0.0540 -> 0.0490 (−9.26 %) |
| t=6097 | 0.2770 | 0.2790 (+0.72 %) | **0.2400 (−13.36 %)** | 0.2670 (−3.61 %) | 0.0560 -> 0.0500 (−10.71 %) |
| t=3122 | 0.2760 | 0.2790 (+1.09 %) | **0.2400 (−13.04 %)** | 0.2670 (−3.26 %) | 0.0550 -> 0.0500 (−9.09 %) |

| pose | `DeferredLighting-call` Gi/f | `renderFrame` Gi/f | `lighting-w1` Gi/f |
|---|--:|--:|--:|
| t=2845 | 1.6770 -> **1.6420 (−2.09 %)** | 4.7510 -> **4.7160 (−0.74 %)** | 1.3890 -> 1.3890 (0.00 %) |
| t=3409 | 1.8060 -> **1.7710 (−1.94 %)** | 4.9290 -> **4.8930 (−0.73 %)** | 1.5210 -> 1.5210 (0.00 %) |
| t=5743 | 1.7620 -> **1.7290 (−1.87 %)** | 4.7120 -> **4.6800 (−0.68 %)** | 1.4770 -> 1.4770 (0.00 %) |
| t=5813 | 1.7150 -> **1.6830 (−1.87 %)** | 4.5670 -> **4.5350 (−0.70 %)** | 1.4320 -> 1.4310 (−0.07 %) |
| t=6097 | 1.5790 -> **1.5430 (−2.28 %)** | 4.2210 -> **4.1850 (−0.85 %)** | 1.2890 -> 1.2890 (0.00 %) |
| t=3122 | 1.7370 -> **1.7000 (−2.13 %)** | 6.2430 -> **6.2070 (−0.58 %)** | 1.4470 -> 1.4470 (0.00 %) |

**`lighting-w1` is flat to four decimals at all six poses** — the control that
says this touched wave 2 alone. Against its own OFF arm the mechanism is
**−13.5 to −14.0 %**.

**NOISE FLOORS, per column, from the parent's own spread over the 11 kept
rounds (max−min)/min — read them before reading a delta:**

| row | Gi/f | Gcyc/f | wall |
|---|--:|--:|--:|
| `lighting-w2` | **0.00–0.37 %** | 3.5–5.6 % | 3.4–16.6 % |
| `DeferredLighting-call` | 0.06–0.13 % | 0.9–2.3 % | 3.8–8.3 % |
| `renderFrame` | 0.02–0.12 % | 0.9–1.9 % | 3.1–5.6 % |

So **the instruction column is the one that resolves this change** at every
level: −12 % of w2 against a 0.0–0.4 % floor, −2 % of the call against 0.1 %,
−0.7 % of the frame against 0.1 %. The `Gcyc/f` win on w2 (−7 to −13 %) is two
to three times its own floor and is real; **`Gcyc/f` and `wall` at `renderFrame`
are INSIDE their floors and prove nothing here** — the OFF arm, which can only
add work, reads −3.6 % and −6.5 % renderFrame cycles at two poses in this batch,
which is the floor talking. Do not quote a frame-level time number from this
round.

### WHY 16h's 2-WIDE IS ONLY A QUARTER OF IT — the answer is a live range, not a lane count

Instruction counts from the disassembly, per checkerboard cell:

| | parent | x2 child | **x4 child** |
|---|--:|--:|--:|
| centre decode | 33 | 33 | — |
| neighbour decode + dot + compare, x2 | 38 + 39 | 47 (one 2-wide block) | — |
| the wide block (3 decodes, 2 dots, the mask) | — | — | **52** |
| mask tests in the k-loop | — | 2 | 2 |
| **total** | **110** | **82** | **54** |

At t=5743 the fold is taken on 72.4 % of lanes, so the parent's effective count
is ~105. **x4 predicts 105 − 54 = 51 instructions/cell saved; the clock says
0.0330 Gi/f over 636 349 cells = 51.9. Ladder and disassembly agree to 2 %.**

x2 does NOT deliver its arithmetic: it predicts 105 − 82 = 23/cell and measures
**0.0080 Gi/f = 12.6/cell — 55 % of what the static count promises.** The
difference between the two forms is not the vector width (a `.2s` op and a `.4s`
op cost the same on this core). **It is that x4 also DELETES the centre normal's
live range.** In the parent `ncX/ncY/ncZ` are decoded at the top of the cell body
and stay live across the whole neighbour loop; x2 leaves them exactly there and
merely rearranges the neighbour work, so it pays 16h's tax — *"in this kernel a
flag-guarded predicate or eight extra live values in the pixel body cost about
what any of these mechanisms save"*. x4 consumes the centre inside the block (as
lane 2, by-element: `fmul.2s v0, v0, v0[2]`) and the three scalars die
immediately, so the allocator gets registers back instead of paying for them.
**16h's hypothesis that "this one removes registers rather than adding them" was
right about the mechanism and wrong about which version has it.**

Two small byte-null bonuses fall out of the same move: the centre decode no
longer runs on the 0.06–0.70 % of cells that are env-force-full (their fallback
never reads it), and the neighbour decodes stop being gated behind the matID
test — which costs a wasted lane on the ~1 % of pairs matID rejects and is what
makes the pairing possible at all.

### FLAGLESS, AND THE REASON IS MEASURED — the hatch is FREE here, and that is not 16h's story

16h's material hoist shipped flagless because *the flag ate the win* (par 0.293 /
off 0.300 / **on 0.292**). That is NOT what happens here: `h4on` and `f4` — the
same mechanism with and without a runtime predicate — measure **identical Gi/f
to four decimals at all six poses** (0.2400 / 0.2360 / 0.2390 / 0.2380 / 0.2400 /
0.2400 both). The predicate is loop-invariant and hoists out of the pixel body;
16h's did not, because it sat inside the neighbour loop. **Report the mechanism,
not the slogan: a flag in this kernel is expensive when it is inside the k-loop
and free when it is above it.**

So the decision is made on other grounds, all three pointing the same way: the
hatch buys **no speed**; the change is **bit-exact**, so there is no look to
dial; and the only thing the dial can do is **cost +0.7 to +1.1 % of the fill**.
Shipped **FLAGLESS**, with the three arms preserved as CMake switches
(`-DFDS_W2_OCTPAIR_MODE=0|2|4`, `-DFDS_W2_OCTPAIR_HATCH=ON`) so any future round
can rebuild par / off / on without re-deriving them. `--deferred_fill_oct_pair`
stays in `FeatureFlags.def` but is **INERT in a default build** — the same shape
as `--omni_census`, and its description says so.

### BYTES — 26 surfaces, and the gate is instrumented rather than trusted

* **Five acceptance poses under HIS OWN ARM, at BOTH resolutions**: 1920x1080
  (the snapshot's own) and **1512x848 (the measurement resolution)** —
  identical parent-to-child, and identical for the x2 and hatched builds too.
* **t=3122 under the same arm** — `4b59d3d0…` on all four binaries.
* **The nine 16f pins at their RECORDED values, 3/3 on the child**: greets
  t=1588 `570a7b44`, city `bd4ffbf8` / `4cb8d2ca`, chase `3bfd4244` / `42d79fad`
  / `622b96a2` / `31aa5203` / `ca07a814`, fountain `8db68ccb`.
* **The city checkerboard control** (t=1961, `--env_live_water --deferred --hdr
  --city_env_pixel --deferred_checkerboard` — the fill on a non-tonemapped
  transport, off greets): `7eb0f8c4…` on both binaries.
* **`--omni_census` census rows IDENTICAL parent-to-child** at t=5743 / 2845 /
  6097, every row including `[13] neighbour normal checks` (1 264 816 /
  1 272 044 / 1 277 453), whose semantics were deliberately preserved by
  counting where the verdict is CONSUMED, not where the decode now happens.
  The parent also reproduces 16h's recorded census exactly (641 088 cells,
  99.14 % averaged, 768 full-shade, 1.980 compatible neighbours).
* **`render_gate.sh` 4/4 PASS** — and **INSTRUMENTED, not assumed: all four of
  its arms take ZERO cells through the wave-2 fill.** A census build with
  `--omni_census` added to each of `mirrortest`, `rttslot`, `conetest`,
  `halotest` prints no `[W2-CENSUS]` line at all; `--deferred_checkerboard` is a
  greets `setDefault` and no gate row is a greets row. **The greets t=1588 pin
  DOES exercise it — 1 036 800 cells at 1920x1080 — so that pin plus the
  six-pose arm diff is the real coverage, not the gate.**
* **Eyeballed at 1:1 and at 3x nearest-neighbour** on t=5743 (mech legs over the
  cracked stone floor, the classic speckle surface): no parity pattern. The
  by-parity census the black-checkerboard round (16j) used as its instrument
  agrees — wave-1 parity mean luma 100.178 against wave-2's 100.556 at t=5743
  (+0.378), 108.583/109.079 at t=2845, 89.934/90.275 at t=6097. That small
  positive bias is the fill's own and is on BOTH binaries, which are
  byte-identical; 16j's defect signature was 175.36 against ~0.
* **The fountain t=2500 flip appeared once in five PARENT runs** and never in
  five child runs. Pre-existing, recorded in 16i and 16l, running total now
  ~2 %. **A battery that reads that as a regression will burn a session.**

Renders: `docs/img/w2oct/w2oct_t{2845,5743,6097}_after.png` (byte-identical to
the parent).

### REFUTED BY CONSTRUCTION, and this is the honest form of "don't conflate"

16h left a counter for *"skip the neighbour oct decode when its PACKED normal
equals the centre's"* — **15.80 % hit rate per PAIR**, worth ~0.009 Gi/f gross.
**The 4-wide makes that fast path strictly worse and it was not built.** There
are no longer three decodes to skip individually: there is one block, and a lane
cannot be skipped out of it. To skip the block you would need *both* neighbours
to match the centre — if the two events were independent that is ~2.5 % of
cells, and it would buy 52 instructions on those while costing a compare and a
branch on the other 97.5 %. The premise the counter was measured under
(per-decode skipping) no longer exists. **Not "measured and lost" — dissolved.**

### WHAT IS LEFT IN w2 (now 0.239 Gi/f = 13.8 % of the call)

1. **The 3-channel scalar arithmetic, est. ~0.03 Gi/f** — 16h's item 2, now the
   largest remaining item and **the only one big enough to matter**. Per
   neighbour: three `fmax` + three `fdiv` + three compares for the texel ratio,
   three fp16 loads, three `fmla`, the texel byte unpack; per cell the HDR
   store. All of it is (B,G,R,·) and fits one NEON vector. **Read 16h's warning
   before starting: clang already SLP-vectorises parts of this 2-wide
   (`fmul.2s` x50, `fmla.2s` x22 in the shipping function), so a 4-wide rewrite
   is NOT bit-exact by construction the way this round's was — it must be
   byte-gated, and the `fdiv` reassociation is where it will break.**
2. **`hsB/hsG/hsR` are dead whenever `nsharp > 0`** — ~0.004 Gi/f. Below this
   campaign's 0.01 bar on its own; only worth doing inside item 1.
3. **Nothing else in this row is worth a round.** After items 1 and 2 the fill
   is ~0.20 Gi/f = 11 % of the call, and `lighting-w1` at 1.43–1.52 is where
   84 % of the call still lives.

## 2026-08-16n — THE 2-D SPOT TAP: no leaf is available, and it did not need one — the win is the 17-ARGUMENT CALL, and half of it is not the frame

**16m parked "the 2-D spot tap `computeMapShadowAtten` is still not a leaf:
505 instructions, one `bl` left, nine callee-save pairs, 160-byte frame — same
method, find the `bl`". Found it. It is `CubeShadow_Sample` on the `srcCube`
arm — a REAL callee that a per-call census says 83–99.7 % of surviving calls
take — so 16m's publish-to-global does not apply and the leaf is not
reachable: force-inlining the callee still leaves 8 pairs and a 128-byte
frame. What IS deletable is the call itself, from the caller's side. Inlining
the tap at its scalar call site is `lighting-w1` Gi/f −2.00 % at greets t=3409
and −4.27 % at t=3122, Gcyc/f −2.61 % / −5.01 %, and ONE least-significant
digit (+0.001 Gi/f) at the three acceptance poses where the tap barely runs.**
Shipped FLAGLESS, BIT-EXACT, one statement attribute. Status: **DONE**
(scalar-site inline) · **REFUTED, measured** (inline the callee) · **REFUTED
BY ARITHMETIC, not built** (outline the `srcCube` arm).

### FIRST, THE PRICE — and it is not one number, it is a 400x range across poses

`-DFDS_SPOTCALL_CENSUS=1` counts ACTUAL calls (inside the function, so it sees
both call sites and everything the scalar site's 3-plane guard lets through),
per main-view frame, 1512x848, his greets arm:

| pose | calls / frame | smIdx | srcSm | **srcCube** | vec-site calls |
|---|--:|--:|--:|--:|--:|
| t=6097 | **0** | — | — | — | 0 |
| t=5813 | 3 984 | 100.00 % | 0 % | **0 %** | 0 |
| t=5743 | 14 742 | 16.21 % | 0.47 % | **83.32 %** | 0 |
| t=2845 | 49 368 | 80.99 % | 0.01 % | **19.01 %** | 0 |
| t=3409 | **828 452** | 0.01 % | 0.28 % | **99.71 %** | 0 |
| t=3122 (16k's cam) | **1 771 205** | 0.04 % | 6.61 % | **93.35 %** | 0 |
| t=1588 (the 16f pin, its own arm) | 151 411 | 38.77 % | 9.58 % | **51.64 %** | 0 |

Two facts fall out of that table before any optimisation:

* **The three arms are mutually exclusive in practice** — the shares sum to
  100.0 % at every pose.
* **The VEC call site makes ZERO calls at every pose**, and **city (both
  acceptance arms), chase (all five pin poses) and fountain make zero calls of
  any kind**. The spot tap's home is greets and only greets, exactly as the
  cube tap's is. So "measure it where it runs" means t=3409 and t=3122; the
  other four acceptance poses are controls, and t=6097 is a perfect one.

Priced as the task asks: the 160-byte frame is 22 instructions (11 prologue,
11 epilogue), so the FRAME alone is 0.00008 / 0.0003 / 0.001 Gi/f at t=5813 /
5743 / 2845 — **10x to 100x below the 0.01 Gi/f bar, close it there** — and
0.018 / 0.039 Gi/f at t=3409 / t=3122, which is above it. One lever, two
verdicts, decided per pose.

### THE `bl`, AND WHY 16m's FIX DOES NOT TRANSPLANT

```
00000001001f20d4   bl   __Z17CubeShadow_Sampleiffffffiiib   <- the only call in 504
```

Not a guard. Not cold. It is the mirror-clone SOURCE-CUBE arm, taken by
99.71 % of the calls at t=3409. There is no lazy `static` to publish and no
never-taken branch to delete.

### FOUR SHAPES, PRICED BEFORE BUILT, THREE OF THEM REFUSED OR REFUTED

| shape | verdict |
|---|---|
| publish-to-global (the 16m fix) | **N/A** — the `bl` is a real callee |
| outline the `srcCube` arm (16l's shape, transplanted) | **REFUTED BY ARITHMETIC, not built** |
| **`inl`** — `always_inline` the CALLEE inside the tap | **BUILT, MEASURED, LOSES to `kin` on cycles at 5/5** |
| **`kin`** — `always_inline` the TAP at its scalar call site | **SHIPPED** |

**The outline is refused by its own census.** It buys a leaf for the calls that
do NOT take the cube arm — 0.29 % of them at t=3409, 6.65 % at t=3122 — and
adds a real call to the other 99.7 % / 93.3 %. At t=3409 that is ~30
instructions x 826 050 = 0.0248 Gi/f = **+1.60 % predicted**, against a
20-instruction frame removed from 2 402 calls (0.003 %). It cannot win, and
that is the same arithmetic that killed 16l's `spl` — the shape is
structurally identical, only the denominator moved.

**The leaf is not available at all**, and the disassembly says so in one line.
`otool -tvV`:

| binary | `computeMapShadowAtten` | `bl` | callee-save pairs | frame | kernel `TileT<1>` |
|---|--:|--:|--:|--:|--:|
| **par** | 504 | 1 | **9** | 0xa0 (160 B) | 4827 |
| `inl` | 825 | **0** | **8** | 0x80 (128 B) | 4827 |
| **`kin`** | 504 (vec site only) | 1 | 9 | 0xa0 | **5357** (+530) |

`inl` deletes the `bl` and is STILL NOT A LEAF: it keeps eight pairs and a
128-byte frame, because the state live across the cube tap — x/y/z, wx/wy/wz,
lenInv, nGeo\*, the bias ints, all needed by the `smIdx` block AFTER it — is
real, not hypothetical. **That is the whole difference from 16m**: there the
call was never taken, so deleting it deleted the need; here the call runs, so
inlining it only moves the spills.

### THE MECHANISM THAT DOES PAY — and it is not the frame

`kin` removes, per call: an 11-instruction prologue, an 11-instruction
epilogue, the `bl`, the **12 argument-shuffle `mov`s at the callee's entry**,
and ~11 marshalling instructions at the call site. `computeMapShadowAtten`
takes **SEVENTEEN arguments, ten of them floats** — two more floats than the
ABI has registers for, so two travel by STACK (`stp s10, s11, [sp]` sits in
the parent's call sequence). Total ~**46 instructions of ABI per call**.

**Predicted before measuring, then measured:**

| pose | calls/f | predicted (46/call) | measured `kin` | implied instr/call |
|---|--:|--:|--:|--:|
| t=3409 | 828 452 | 0.0381 Gi/f = **−2.45 %** | **−2.00 %** (0.031 Gi/f) | 37.4 |
| t=3122 | 1 771 205 | 0.0815 Gi/f = **−4.58 %** | **−4.27 %** (0.076 Gi/f) | 42.9 |

High by 18 % and 7 %, same sign, right magnitude, at call counts **2.14x
apart** — which is the point: the model is per-call, and it holds across the
range. **The frame alone (22 instructions) predicts only −1.17 % / −2.19 %,
about HALF the measured win. On this tap the argument list costs as much as
the frame does**, and no amount of leaf-hunting would have found that.

### MEASURED — min over 11 order-rotated interleaved rounds, one pose per process

1512x848, `--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao
--greets-displace`, `--deferred_prof=1 --hw_prof --profiler=0`, 10 iters.
**Both counter columns quoted, per 16l's caveat.**

| pose | calls/f | row | par | `inl` | **`kin`** |
|---|--:|---|--:|--:|--:|
| **3409** | 828 452 | `lighting-w1` Gi/f | 1.552 | 1.528 (−1.55 %) | **1.521 (−2.00 %)** |
| | | .. Gcyc/f | 0.422 | 0.415 (−1.66 %) | **0.411 (−2.61 %)** |
| | | .. wall | 12.801 | 12.688 (−0.88 %) | **12.388 (−3.23 %)** |
| | | `renderFrame` Gi/f | 4.959 | 4.934 (−0.50 %) | **4.927 (−0.65 %)** |
| | | `renderFrame` Gcyc/f | 1.312 | 1.302 (−0.76 %) | **1.297 (−1.14 %)** |
| | | `renderFrame` wall | 42.746 | 42.731 (−0.04 %) | **42.456 (−0.68 %)** |
| **2845** | 49 368 | `lighting-w1` Gi/f | 1.391 | 1.390 (−0.07 %) | **1.389 (−0.14 %)** |
| | | .. Gcyc/f | 0.360 | 0.365 (+1.39 %) | 0.363 (+0.83 %) |
| | | `renderFrame` wall | 41.578 | 41.753 (+0.42 %) | **41.457 (−0.29 %)** |
| **5743** | 14 742 | `lighting-w1` Gi/f | 1.475 | 1.475 (+0.00 %) | 1.476 (+0.07 %) |
| | | .. Gcyc/f | 0.376 | 0.378 (+0.53 %) | **0.376 (+0.00 %)** |
| | | `renderFrame` wall | 41.606 | 41.451 (−0.37 %) | **41.228 (−0.91 %)** |
| **5813** | 3 984 | `lighting-w1` Gi/f | 1.430 | 1.429 (−0.07 %) | 1.431 (+0.07 %) |
| | | .. Gcyc/f | 0.369 | 0.371 (+0.54 %) | 0.370 (+0.27 %) |
| | | `renderFrame` wall | 39.588 | 39.848 (+0.66 %) | 39.790 (+0.51 %) |
| **6097** | **0** | `lighting-w1` Gi/f | 1.288 | 1.288 (+0.00 %) | 1.289 (+0.08 %) |
| | | .. Gcyc/f | 0.346 | 0.348 (+0.58 %) | **0.345 (−0.29 %)** |
| | | `renderFrame` wall | 37.053 | 37.232 (+0.48 %) | **37.022 (−0.08 %)** |
| **3122** | 1 771 205 | `lighting-w1` Gi/f | 1.781 | 1.731 (−2.81 %) | **1.705 (−4.27 %)** |
| | | .. Gcyc/f | 0.479 | 0.480 (+0.21 %) | **0.455 (−5.01 %)** |
| | | .. wall | 14.260 | 14.173 (−0.61 %) | **13.813 (−3.13 %)** |
| | | `renderFrame` Gi/f | 6.583 | 6.533 (−0.76 %) | **6.507 (−1.15 %)** |
| | | `renderFrame` Gcyc/f | 1.640 | 1.647 (+0.43 %) | **1.580 (−3.66 %)** |
| | | `renderFrame` wall | 52.184 | 52.282 (+0.19 %) | **51.900 (−0.54 %)** |

`lighting-w2` Gi/f is flat to four decimals everywhere except one LSB at
t=3122 — the control that says this touched wave 1 alone.

**THE NOISE FLOOR, stated so the small numbers are not over-read.** Spread of
the 11 rounds within each arm: **Gi/f 0.00–0.14 %** (the counter's own quantum
is 0.001 Gi/f = ±0.07 % here, so **every "+0.07 / +0.08 %" above is literally
one least-significant digit**), **Gcyc/f 1.37–4.62 %** — so on the cycle
column only t=3409 (−2.61 %) and t=3122 (−5.01 %) are resolvable at all, and
the +0.83 % at t=2845 / +0.27 % at t=5813 are inside the spread and must not be
read as losses. **`kin` beats `inl` on cycles at 5 of 5 acceptance poses** and
by 5.2 points at t=3122, which is what settles the choice between them.

### THE TAX, PRICED WHERE THE TAP CANNOT PAY

+530 instructions in the kernel is not free, so it was measured where the
change can do nothing at all:

* **greets t=6097 — ZERO calls, same scene, same arm**: Gi/f **+0.08 %** (one
  LSB), Gcyc/f −0.29 %, `renderFrame` wall −0.08 %.
* **city t=1961, his `--env_live_water --deferred --city_env_pixel` arm, 11
  rounds** — a scene with zero spot taps AND zero cube taps: `lighting-w1`
  Gi/f **+0.00 %**, Gcyc/f −0.84 %; `renderFrame` Gi/f +0.02 %, Gcyc/f
  +0.54 %, wall +0.60 %.

So the code growth costs at most one least-significant digit of instructions
anywhere it cannot earn, and nothing measurable on the other scenes' kernels.

### THE HATCH — refused structurally, not timed

`always_inline` is a compile-time decision with no runtime dial. A `bool` flag
would have to keep BOTH bodies live and the OFF arm would still make the call —
the exact shape 16m timed as `hon` at +1.36 to +1.82 %. A TEMPLATE hatch (the
`--deferred_cube_prepass` trick) would duplicate a **4827-instruction kernel**
per arm to gate a one-line placement change. **Shipped FLAGLESS**, third time
running, same reason.

### BYTE VERDICT

* **14 surfaces — the five acceptance poses + the nine 16f pins — bit-identical
  parent-to-child on the FIRST pass, for all three built arms** (`par`, `inl`,
  `kin`) and again for the shipped binary. One worktree, one asset tree. Pins
  at their RECORDED values, not merely parent-matched: city `bd4ffbf8`,
  city-his-arm `4cb8d2ca`, fountain `8db68ccb`, greets t=1588 `570a7b44`,
  chase `622b96a2` / `31aa5203` / `ca07a814`.
* **THE FOUNTAIN t=2500 FLIP DID NOT APPEAR THIS ROUND** — 4 clean passes,
  `8db68ccb` every time. 16i / 16l / 16m each saw it; this round did not. That
  is one more data point that it is intermittent and pre-existing, not that it
  is gone.
* **`--shadow_tap_census` spot-pyramid rows — the committed instrument that
  actually reads this tap's interior — are IDENTICAL parent-to-child at all
  five poses, to every digit**: t=2845 reached 0.040 M/f, uniform-lit 33.9 %,
  uniform-occ 48.7 %, mixed 17.4 %; t=5743 0.002 M/f, 17.9 % / 79.6 % / 2.5 %;
  t=5813 0.004 M/f, 97.4 % / 0.2 % / 2.3 %; t=3409 and t=6097 zero (their calls
  take the `srcCube` arm, which is not the PolyId pyramid's path).
* `--deferred_cube_prepass_verify`: **0 mismatches in 76.8 M taps** over the
  five poses (16.0 / 14.0 / 16.9 / 16.6 / 13.3 M). **Stated honestly: this
  counter does NOT cover the `srcCube` arm** — it verifies the light's OWN cube
  against the prepass. The `srcCube` arm's coverage is the t=3409 frame itself,
  which is 99.71 % `srcCube`-driven and byte-identical.
* `render_gate.sh` **4/4 on both binaries**. **What it can discriminate here:
  nothing.** 16m instrumented its four arms at zero cube taps; this round's
  census adds that those arms and city / chase / fountain make **zero
  `computeMapShadowAtten` calls**, so no gate arm executes one line of the
  changed path. It is a regression net for the rest of the kernel, not evidence
  for this change. The five poses, t=1588 and t=3122 are the coverage.
* Instrument builds still compile: `-DFDS_SHADOW_TAP_CENSUS=ON`.
* Eyeballed: `docs/img/spotleaf/inline_t3409.png` (greets t=3409, his arm — the
  "adept" portal frame, which is exactly the mirror-clone content that makes
  this pose 99.7 % `srcCube`).

### WHAT IS LEFT ON THE SHADOW TAPS

1. **THE CALL FRAME IS NOW CLOSED ON BOTH TAPS.** The cube tap is a leaf with a
   zero-byte frame (16m). The spot tap's frame is gone from the only call site
   that makes calls; the out-of-line body keeps its 160-byte frame but now
   serves only the VEC site, which is measured at **zero calls in every scene**.
   There is no third tap and no third frame.
2. **The next thing on this tap is ARITHMETIC, not placement.** At t=3409 /
   t=3122 the `srcCube` arm recomputes the full mirror reflection per (pixel x
   light) — world position from the view matrix, the plane reflection, the
   world→view rotation back, ~40 float ops — 828 k to 1.77 M times a frame,
   and the SAME reflection is computed a second time by the `srcSm` arm above
   it when both are live. That is a real lever and a different round; it will
   need a bit-exactness argument that this one did not.
3. **Any future spot-tap work must quote t=3409 / t=3122, never the five-pose
   summary.** The call count spans **0 to 1.77 M per frame** across six greets
   poses of the same scene. A lever measured only at t=5743 or t=6097 is being
   measured where the function does not run.
4. ~~**`--ssao_downscale=2` is still the single largest lever on this arm**, and
   still a look change nobody has approved.~~ **SPENT 2026-08-16y — countersigned
   and landed as the DEFAULT** (realized −9.6 ms of frame; see the 2026-08-16y
   entry at the top). Read this item as closed wherever it appears.

### METHOD NOTE

16m's note said "count the `bl`s in the hot function first". That was right and
it was not enough. **Also count the ARGUMENTS.** A 17-argument call with ten
floats moves as much state as a nine-pair frame saves, and neither
instruction-counting nor leaf-hunting sees it — only the call sequence in
`otool -tvV` does. And **count the CALLS before either**: a per-call census
(30 lines, one `#define`) turned "the tap is not a leaf" from a one-line
grievance into a table that decided three of the four shapes without building
them.

## 2026-08-16m — THE CUBE TAP'S CALL FRAME: 16l's fix is REFUTED, and the frame was never the PCF's fault — it was ONE cold call to a lazy `static`

**16l parked "splitting the tail's RARE half into its own `noinline` function
would make the common path a leaf — cheapest remaining item by far". Built and
measured: it makes the leaf and it LOSES at every pose. The frame it was aimed
at is real, but its cause is a `bl __cxa_guard_acquire` behind
`ShadowSwzGetShape()`, and deleting THAT buys the same leaf for nothing —
`lighting-w1` Gcyc/f −1.2 to −2.9 % at all five acceptance poses, zero new
calls, 30 lines, BIT-EXACT.** Shipped FLAGLESS. Status: **DONE** (hoist) ·
**PARKED, measured negative** (the split).

### FIRST, 16l's PREMISE IS STALE AND THE NUMBER IS 0.43 %

16l item 1 reads "`CubeShadow_Sample` still spills nine callee-save pairs into a
144-byte frame, and 81 % of taps never touch the state that forces them". Two
corrections, both measured on the tip rather than read off the old round:

* **It is EIGHT pairs, not nine** — `stp` of d13/d12, d11/d10, d9/d8, x26/x25,
  x24/x23, x22/x21, x20/x19, x29/x30, plus `sub sp, sp, #0x90` (`otool -tvV`).
* **That frame is no longer on the hot path at all.** 16l's own
  `--deferred_cube_prepass` (default ON) routes greets through
  `CubeShadow_SampleCached`, which is INLINED into the tile kernel. A per-call
  counter in each entry (`-DFDS_TAPPATH_CENSUS`) at greets t=5743, his arm:

  | path | taps / frame | share |
  |---|--:|--:|
  | `CubeShadow_SampleCached` (prepass, inlined in the kernel) | 2 943 120 | **99.57 %** |
  | `CubeShadow_Sample` (the out-of-line scalar tap, the one with the frame) | 12 593 | **0.43 %** |
  | ... of all taps, reaching the 2x2 PCF (`CubeShadow_Tail`'s mixed arm) | 577 296 | 19.53 % |

  So a round that optimises the 144-byte frame is optimising 0.43 % of taps.
  What is actually hot is the SAME BODY inlined into
  `Render_DeferredLighting_TileT<1>` — and there the identical wart costs
  register pressure and I-cache in the largest function in the tree.

### THE DISASSEMBLY, WHICH IS THE WHOLE FINDING

`CubeShadow_Sample` contains **exactly one `bl`**, and it is not in the PCF:

```
00000001001f4a48   bl   __Z17ShadowSwzGetShapev     <- the only call in 410 instructions
```

`ShadowSwzGetShape()` returns a function-local `static const ShadowSwzShape`
whose initialiser runs `getenv` + `sscanf` + `fprintf`, so clang must emit a
thread-safe guard: a load, a test, and on the cold arm
`bl __cxa_guard_acquire`. **A potential call is a call to the register
allocator.** Every value live across it has to sit in a callee-save register,
so the function opens a 144-byte frame and stores eight pairs — and it does
that BEFORE the `dynBaked`, `lz <= 0.05`, two face-frustum and iX/iY rejects,
i.e. 80 % of taps save and restore sixteen registers for a branch that
`--shadow_swizzle` (default OFF) can never take.

Only the `cubeIdx` bounds guard is shrink-wrapped past the prologue; every
other reject branches to the full eight-pair epilogue.

### WHAT WAS BUILT — four shapes, one worktree, one asset tree

| shape | what it does |
|---|---|
| **`spl`** | 16l's literal ask: the 2x2 PCF moved to `noinline CubeShadow_TapMixed`, tail-called |
| **`hoi`** | the shape published to a plain global; hot taps read the global, nothing outlined |
| **`both`** | `spl` + `hoi` |
| **`hon`** | `spl` behind a FeatureFlags hatch, flag ON |

`otool -tvV`, and this table alone decides it:

| binary | kernel `TileT<1>` | `CubeShadow_Sample` | `bl` | callee-save pairs | frame | spot tap pairs |
|---|--:|--:|--:|--:|--:|--:|
| **par** | 4762 | 410 | 1 | **8** | 144 B | 10 |
| `spl` | 4535 | 168 | 0 | **0** | none | 10 |
| **`hoi`** | 4828 | **387** | **0** | **0** | **none** | **9** |
| `both` | 4697 | 168 | 0 | 0 | none | 9 |
| `hon` | 4745 | 421 | 5 | **6** | yes | 10 |

`spl` reaches the leaf by EXPORTING the rare half — `CubeShadow_TapMixed` is
285 instructions with **nine** callee-save pairs and a 160-byte frame of its
own, now paid on the 19.6 % of taps that reach it. `hoi` reaches the same leaf
by DELETING the call, keeps the PCF inlined, and fixes the 2-D spot tap
(`computeMapShadowAtten`, 515 → 505, 2 `bl` → 1, 10 → 9 pairs, 0xc0 → 0xa0)
for free, because it had the identical wart.

### MEASURED — min over 11 order-rotated interleaved rounds, one pose per process

1512x848, `--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao
--greets-displace`, `--deferred_prof=1 --hw_prof --profiler=0`, 10 iters.
**Both columns quoted, per 16l's caveat.**

| pose | row | par | `spl` | **`hoi`** | `both` | `hon` |
|---|---|--:|--:|--:|--:|--:|
| **5743** | `lighting-w1` Gi/f | 1.479 | 1.500 (+1.42 %) | **1.475 (−0.27 %)** | 1.485 (+0.41 %) | 1.503 (+1.62 %) |
| | .. Gcyc/f | 0.385 | 0.387 (+0.52 %) | **0.378 (−1.82 %)** | 0.390 (+1.30 %) | 0.389 (+1.04 %) |
| | .. core-ms | 134.49 | 135.25 (+0.57 %) | **132.23 (−1.68 %)** | 136.54 (+1.53 %) | 137.03 (+1.89 %) |
| | `renderFrame` Gcyc/f | 1.285 | 1.289 (+0.31 %) | **1.275 (−0.78 %)** | 1.293 (+0.62 %) | 1.291 (+0.47 %) |
| | `renderFrame` wall | 41.93 | 41.77 (−0.37 %) | **41.46 (−1.12 %)** | 41.93 (0.00 %) | 41.82 (−0.27 %) |
| **2845** | `lighting-w1` Gi/f | 1.398 | 1.415 (+1.22 %) | **1.391 (−0.50 %)** | 1.398 (0.00 %) | 1.417 (+1.36 %) |
| | .. Gcyc/f | 0.372 | 0.376 (+1.08 %) | **0.366 (−1.61 %)** | 0.376 (+1.08 %) | 0.376 (+1.08 %) |
| | `renderFrame` wall | 42.16 | 42.00 (−0.37 %) | **41.52 (−1.50 %)** | 41.95 (−0.48 %) | 42.16 (+0.01 %) |
| **6097** | `lighting-w1` Gi/f | 1.290 | 1.307 (+1.32 %) | **1.288 (−0.16 %)** | 1.295 (+0.39 %) | 1.309 (+1.47 %) |
| | .. Gcyc/f | 0.351 | 0.356 (+1.42 %) | **0.341 (−2.85 %)** | 0.355 (+1.14 %) | 0.355 (+1.14 %) |
| | `renderFrame` Gcyc/f | 1.168 | 1.173 (+0.43 %) | **1.140 (−2.40 %)** | 1.167 (−0.09 %) | 1.168 (0.00 %) |
| | `renderFrame` wall | 37.28 | 37.35 (+0.18 %) | 37.55 (+0.72 %) | 37.53 (+0.66 %) | 37.69 (+1.09 %) |
| **3409** | `lighting-w1` Gi/f | 1.568 | 1.576 (+0.51 %) | **1.553 (−0.96 %)** | 1.565 (−0.19 %) | 1.591 (+1.47 %) |
| | .. Gcyc/f | 0.428 | 0.429 (+0.23 %) | **0.423 (−1.17 %)** | 0.433 (+1.17 %) | 0.439 (+2.57 %) |
| | `renderFrame` wall | 43.11 | 43.34 (+0.53 %) | **42.72 (−0.90 %)** | 43.40 (+0.67 %) | 43.43 (+0.74 %) |
| **5813** | `lighting-w1` Gi/f | 1.430 | 1.452 (+1.54 %) | **1.430 (0.00 %)** | 1.440 (+0.70 %) | 1.456 (+1.82 %) |
| | .. Gcyc/f | 0.374 | 0.375 (+0.27 %) | **0.368 (−1.60 %)** | 0.378 (+1.07 %) | 0.379 (+1.34 %) |
| | `renderFrame` wall | 40.66 | 40.15 (−1.26 %) | **40.14 (−1.26 %)** | 40.86 (+0.50 %) | 40.18 (−1.17 %) |

`lighting-w2` Gi/f is **flat to four decimals at every pose on every arm** —
the control that says this touched wave 1 alone.

**`hoi` wins `Gcyc/f` at 5 of 5 poses (−1.17 to −2.85 %), `Ginstr/f` at 4 of 5
(flat at the fifth), and wall at 4 of 5.** The one wall miss is **t=6097
(+0.72 %)**, and at that pose the two counter columns move the other way
hardest (`lighting-w1` Gcyc −2.85 %, `renderFrame` Gcyc −2.40 %) — read it as
load-dirt, not as a regression, and note that this is the OPPOSITE direction of
16l's t=3409 disagreement, so quote the pose, not the summary.

**`spl` loses on BOTH columns at all five poses** (`lighting-w1` Gi +0.51 to
+1.54 %, Gcyc +0.23 to +1.42 %), so no column-picking rescues it.

### WHY `spl` LOSES, TO THE INSTRUCTION

The mixed arm runs 577 296 times a frame at t=5743. `spl`'s
`CubeShadow_TapMixed` opens a 160-byte frame and stores nine pairs, so the
per-call overhead is ~34 instructions:

  577 296 × 34 = **0.0196 Gi/f = +1.33 % of `lighting-w1`** — against a
  measured **+1.42 %**.

`both` shrinks `TapMixed` to two pairs and no `bl` (~18 instructions of
overhead): predicted **+0.70 %**, measured **+0.41 %**. The arithmetic and the
counter agree twice, which is what makes this a refutation rather than a null
result: **the split's cost IS the call it introduces, and there is no version
of the split that does not introduce it.**

### THE HATCH — refuted in the disassembly BEFORE it was timed

A flag here has to keep the inlined body in the kernel, because that is what
the OFF arm switches to. `hon`'s kernel is 4745 instructions with all **7**
`ShadowSwzGetShape` references still present, against `spl`'s 4535 and 0 — the
ON arm structurally cannot collect the win. Timed anyway: `hon` is **+1.36 to
+1.82 % of `lighting-w1` instructions at every pose**, the worst arm in the
table. Its OFF arm reproduced the parent to −0.14 % (3 rounds), so the build is
a valid control; it is the ON arm that cannot pay.

**Shipped FLAGLESS**, same call as the material hoist: a behaviour-identical
code-shape change whose flag would cost more than the change saves.

### BYTE VERDICT

* **Five acceptance poses + the nine 16f pins = 14 surfaces. 13 bit-identical
  parent-to-child on the first pass**, one worktree, one asset tree; the 14th is
  the fountain flip below. And the pins are at their RECORDED values, not merely
  parent-matched: city `bd4ffbf8`, city-his-arm `4cb8d2ca`, fountain `8db68ccb`,
  greets t=1588 `570a7b44`, chase `622b96a2` / `31aa5203` / `ca07a814`.
* **`--deferred_cube_prepass_verify`: 0 mismatches in 76.8 M taps** over the
  five poses (16.9 / 16.0 / 13.3 / 14.0 / 16.6 M).
* `render_gate.sh` **4/4**. Same warning as 16l, re-earned: it **cannot
  discriminate this change** — none of its four arms is a greets frame, and
  16l instrumented city / chase / fountain / all four gate arms at **zero**
  taps through the prepass. The five poses and greets t=1588 are the coverage.
* **THE FOUNTAIN FLIP APPEARED A THIRD TIME**, again on the **PARENT**:
  `b91cb2ba…` instead of `8db68ccb…` in the first gate pass. Re-gated **8/8
  `8db68ccb` on BOTH binaries**. 16i predicted it, 16l saw it, this round saw
  it. **It is pre-existing and it is not yours** — do not spend a battery.
* Instrument builds still compile: `-DFDS_CUBE_ABLATE=10` and
  `-DFDS_SHADOW_TAP_CENSUS=ON`.
* Eyeballed: `docs/img/tapleaf/hoist_t5743.png`.

### WHAT IS LEFT ON THE TAP

1. **The 2-D SPOT tap, `computeMapShadowAtten`, is still not a leaf**: 505
   instructions, **one `bl` left**, nine callee-save pairs, 160-byte frame. The
   hoist took one of its two calls; the other was not chased. Same method
   (find the `bl`, ask what it is doing in a hot tap) — unpriced, and the tap
   is smaller than the cube tap's.
2. **The call frame as a lever is now CLOSED for the cube tap.** It is a leaf
   with a zero-byte frame; there is nothing left to remove.
3. **The eager/lazy ratio and the uniformity pyramid** — unchanged from 16l
   items 2 and 3, including "the one safe reject was tried and refuted, do not
   re-derive".
4. ~~**`--ssao_downscale=2` is still the single largest lever on this arm**, and
   still a look change nobody has approved.~~ **SPENT 2026-08-16y — countersigned
   and landed as the DEFAULT** (realized −9.6 ms of frame; see the 2026-08-16y
   entry at the top). Read this item as closed wherever it appears.

### METHOD NOTE, because it generalises

Three rounds running (16h's hatch, 16l's four shapes, this one) the answer has
been *the register allocator, not the arithmetic*. The instrument that settles
it in one build is `otool -tvV`: **count the `bl`s in the hot function first.**
A cold call in a hot leaf is worth more than any amount of instruction
counting, because its cost is not in the branch — it is in the sixteen
registers the branch forces everyone else to spill.

## 2026-08-16l — THE CUBE TAP (33 % of the call): its interior is 61 % projection prologue, 8-wide over PIXELS is BIT-EXACT, and the instruction count is the wrong metric for it

**Result at greets t=5743 / 2845 / 6097 / 3409 / 5813, 1512x848, his acceptance
arm: `lighting-w1` CORE TIME −6.0 to −14.6 % and cycles −4.9 to −14.3 % at
EVERY pose; instructions −4.9 to −0.8 % at four poses and **+1.5 % at t=3409**.
`renderFrame` wall −1.3 to −4.2 % at every pose, instructions −1.6 to −0.2 % at
four and +0.44 % at t=3409. BIT-EXACT, and by a counter rather than an
argument: 0 mismatches in 47 M taps, five acceptance poses identical
parent-to-child, nine 2026-08-16f pins at their recorded values.
Flag `--deferred_cube_prepass`.**

Commits `8cef558f` (the interior ladder + the tap's prologue/tail split)
· `a1ce4b30` (the mechanism).

### FIRST, THE INTERIOR — 16g called it "closed" from the outside

`-DFDS_CUBE_ABLATE=n` (committed) is twelve staged early returns inside
`CubeShadow_Sample`, each returning the constant 1.0f through a compare against
a value no tap can produce, so the work above the cut stays live. Stage 0
builds a `cmp`-identical binary. Driven with `-DFDS_OMNI_ABLATE=9` so the omni
loop `continue`s immediately after the tap and the constant answer has nowhere
downstream to go. At t=5743, over 2.943 M taps (`--omni_census`), the tap is
0.599 Gi/f = **204 instructions**:

| stage | Gi/f | instr/tap | % of tap |
|---|--:|--:|--:|
| call frame + cubeIdx guard | 0.069 | 23.4 | 11.5 % |
| lightISource + 3 world subs | 0.040 | 13.6 | 6.7 % |
| `CubeShadow_SelectFace` | 0.061 | 20.7 | 10.2 % |
| face map resolve | 0.026 | 8.8 | 4.3 % |
| `viewToLight` 3x3 matmul | 0.091 | 30.9 | 15.2 % |
| lz near reject | 0.016 | 5.4 | 2.7 % |
| 2 face-frustum rejects | 0.066 | 22.4 | 11.0 % |
| 1/lz + smX/smY projection | 0.015 | 5.1 | 2.5 % |
| int trunc + iX/iY bounds reject | 0.053 | 18.0 | 8.8 % |
| **= the PROLOGUE (the eight rows above)** | **0.368** | **125** | **61.4 %** |
| uniformity pyramid | 0.056 | 19.0 | 9.3 % |
| bilinear weights | 0.012 | 4.1 | 2.0 % |
| 2x2 tap addressing | 0.020 | 6.8 | 3.3 % |
| 4 packed taps + accumulate | 0.074 | 25.1 | 12.4 % |

`otool -tvV` counts exactly **125 instructions** between the same two points in
the shipping binary — ladder and disassembly agree to the instruction. t=2845
reproduces the shape on a 0.479 Gi/f tap. And `--shadow_tap_census` says
**81.1 % of taps never read a texel** (26.1 % uniform-lit, 55.0 %
uniform-occluding), so the branchy memory-bound half is the RARE half: the
common cube tap is "project the pixel into a face and read one pyramid word".

**Two stages of the first ladder lied, and the mechanism generalises.** A cut
whose keep-expression is an INTEGER (`face`, `iX + iY`) folds — clang proves
`float(int) > 1e30f` false, the cut collapses to `return 1.0f`, and since the
only other exit above it is also `return 1.0f`, everything above dies. Both
measured BELOW the no-tap baseline. Third variant of this after 16g's
dead-strip and 16i's sink: sum integers into a float the compiler cannot bound.

### THE MECHANISM — why the axis has to flip

The loop is PIXEL-major. For a fixed pixel the eight lights carry eight cubes,
eight faces and eight 3x3 matrices, so going wide over LIGHTS would GATHER
twelve floats per lane, which is exactly what the scalar's twelve loads already
cost. Fixed LIGHT x eight PIXELS makes the matrix a BROADCAST. So the prologue
moves to a row pass: per tile row, reconstruct the row's view and world
positions, then one 8-wide sweep per cube-carrying light into a lane-major slot
plane; the omni loop's tap reads its 16-byte slot and runs `CubeShadow_Tail` —
the SAME body `CubeShadow_Sample` reaches, split out of it `always_inline` so
the scalar function's codegen stayed byte-identical.

**Nothing about the accumulation changes.** The pixel loop still visits lights
in the same order and adds in the same order; only WHERE the projection is
computed moves. That is what makes bit-exactness reachable at all.

The sweep is UNMASKED. The omni loop kills 45 % of pairs before the tap
(mirrorId 4.4 %, N·L 20.8 %, range 8.7 %, cone 11.1 % — `--omni_census`), and
three of those four are cheap to replicate; none is. The vector body runs all
eight lanes either way, so a mask only pays when it empties a whole group, and
the reject that would do that most often (N·L) needs the normal-mapped shading
normal — 0.100 Gi/f of pixel-body work the row pass would have to duplicate.

### THE HATCH HAD TO LEAVE THE LOOP — 16h's finding, paid for again

As a runtime bool the hatch is one pointer test per pixel and one per tap,
about eight instructions of the ~2 000 a pixel runs. It cost **`lighting-w1`
+0.065 Gi/f (+4.3 %) with the flag OFF** — half the win, on a branch never
taken. It is not the tests: a build with the gate `constexpr false` reproduces
the parent EXACTLY (1.543), so all of it is the register allocator in the
largest function in the tree reacting to two extra live pointers. Four shapes
were tried and none moved the OFF arm:

| shape | OFF Gi/f | ON Gi/f |
|---|--:|--:|
| light-major slot plane + AoS positions read back by the body | 1.657 | 1.501 |
| lane-major plane, six hoisted plane pointers, read-back | 1.605 | 1.464 |
| lane-major, one row pointer, positions recomputed | 1.609 | 1.477 |
| the per-tile setup outlined `noinline` | 1.612 | 1.481 |
| **the tile kernel TEMPLATED on the hatch** | **1.548** | **1.480** |

`Render_DeferredLighting_TileT<bool>` is instantiated twice and dispatched once
per tile. The copies are never hot in the same frame, so the second costs
I-cache footprint nobody walks and buys an OFF arm that is the parent's codegen
to +0.3 %.

### MEASURED — parent / OFF / ON, one worktree, one asset tree

Order-rotated interleaved, min over 11 rounds, one pose per process, 1512x848,
`--profiler=0 --deferred_prof=1 --hw_prof`, 10 iters. Load ran 4.8–13.9.

| pose | row | par | off | **ON** | vs par | vs off |
|---|---|--:|--:|--:|--:|--:|
| **5743** | `lighting-w1` Gi/f | 1.542 | 1.547 (+0.32 %) | **1.479** | **-4.09 %** | -4.40 % |
|  | .. Gcyc/f | 0.442 | 0.449 (+1.58 %) | **0.381** | **-13.80 %** | -15.14 % |
|  | .. core-ms | 155.086 | 156.554 (+0.95 %) | **133.447** | **-13.95 %** | -14.76 % |
|  | .. wall ms | 13.007 | 13.192 (+1.42 %) | **11.324** | **-12.94 %** | -14.16 % |
|  | `lighting-w2` Gi/f | 0.272 | 0.272 (+0.00 %) | **0.272** | **+0.00 %** | +0.00 % |
|  | `DeferredLighting-call` Gi/f | 1.827 | 1.832 (+0.27 %) | **1.764** | **-3.45 %** | -3.71 % |
|  | `renderFrame` Gi/f | 4.777 | 4.782 (+0.10 %) | **4.714** | **-1.32 %** | -1.42 % |
|  | `renderFrame` wall ms | 43.335 | 43.546 (+0.49 %) | **41.541** | **-4.14 %** | -4.60 % |
| **2845** | `lighting-w1` Gi/f | 1.430 | 1.436 (+0.42 %) | **1.398** | **-2.24 %** | -2.65 % |
|  | .. Gcyc/f | 0.412 | 0.416 (+0.97 %) | **0.371** | **-9.95 %** | -10.82 % |
|  | .. core-ms | 142.692 | 143.770 (+0.76 %) | **130.046** | **-8.86 %** | -9.55 % |
|  | .. wall ms | 12.140 | 12.298 (+1.30 %) | **10.909** | **-10.14 %** | -11.29 % |
|  | `lighting-w2` Gi/f | 0.275 | 0.275 (+0.00 %) | **0.275** | **+0.00 %** | +0.00 % |
|  | `DeferredLighting-call` Gi/f | 1.718 | 1.725 (+0.41 %) | **1.687** | **-1.80 %** | -2.20 % |
|  | `renderFrame` Gi/f | 4.791 | 4.798 (+0.15 %) | **4.760** | **-0.65 %** | -0.79 % |
|  | `renderFrame` wall ms | 43.051 | 43.042 (-0.02 %) | **41.718** | **-3.10 %** | -3.08 % |
| **6097** | `lighting-w1` Gi/f | 1.300 | 1.304 (+0.31 %) | **1.290** | **-0.77 %** | -1.07 % |
|  | .. Gcyc/f | 0.385 | 0.388 (+0.78 %) | **0.347** | **-9.87 %** | -10.57 % |
|  | .. core-ms | 132.882 | 136.443 (+2.68 %) | **120.260** | **-9.50 %** | -11.86 % |
|  | .. wall ms | 11.569 | 11.707 (+1.19 %) | **10.263** | **-11.29 %** | -12.33 % |
|  | `lighting-w2` Gi/f | 0.277 | 0.277 (+0.00 %) | **0.277** | **+0.00 %** | +0.00 % |
|  | `DeferredLighting-call` Gi/f | 1.590 | 1.594 (+0.25 %) | **1.580** | **-0.63 %** | -0.88 % |
|  | `renderFrame` Gi/f | 4.231 | 4.236 (+0.12 %) | **4.221** | **-0.24 %** | -0.35 % |
|  | `renderFrame` wall ms | 38.679 | 38.517 (-0.42 %) | **37.366** | **-3.39 %** | -2.99 % |
| **3409** | `lighting-w1` Gi/f | 1.545 | 1.553 (+0.52 %) | **1.568** | **+1.49 %** | +0.97 % |
|  | .. Gcyc/f | 0.451 | 0.457 (+1.33 %) | **0.429** | **-4.88 %** | -6.13 % |
|  | .. core-ms | 157.871 | 158.790 (+0.58 %) | **148.410** | **-5.99 %** | -6.54 % |
|  | .. wall ms | 13.571 | 13.876 (+2.25 %) | **12.983** | **-4.33 %** | -6.44 % |
|  | `lighting-w2` Gi/f | 0.271 | 0.271 (+0.00 %) | **0.271** | **+0.00 %** | +0.00 % |
|  | `DeferredLighting-call` Gi/f | 1.829 | 1.837 (+0.44 %) | **1.853** | **+1.31 %** | +0.87 % |
|  | `renderFrame` Gi/f | 4.952 | 4.959 (+0.14 %) | **4.974** | **+0.44 %** | +0.30 % |
|  | `renderFrame` wall ms | 43.278 | 43.555 (+0.64 %) | **42.720** | **-1.29 %** | -1.92 % |
| **5813** | `lighting-w1` Gi/f | 1.503 | 1.508 (+0.33 %) | **1.430** | **-4.86 %** | -5.17 % |
|  | .. Gcyc/f | 0.435 | 0.440 (+1.15 %) | **0.373** | **-14.25 %** | -15.23 % |
|  | .. core-ms | 150.767 | 152.825 (+1.37 %) | **128.691** | **-14.64 %** | -15.79 % |
|  | .. wall ms | 12.814 | 12.929 (+0.90 %) | **10.979** | **-14.32 %** | -15.08 % |
|  | `lighting-w2` Gi/f | 0.270 | 0.270 (+0.00 %) | **0.270** | **+0.00 %** | +0.00 % |
|  | `DeferredLighting-call` Gi/f | 1.787 | 1.792 (+0.28 %) | **1.713** | **-4.14 %** | -4.41 % |
|  | `renderFrame` Gi/f | 4.638 | 4.643 (+0.11 %) | **4.564** | **-1.60 %** | -1.70 % |
|  | `renderFrame` wall ms | 42.025 | 42.080 (+0.13 %) | **40.255** | **-4.21 %** | -4.34 % |

### THE INSTRUCTION COUNT AND THE CLOCK DISAGREE, AND THAT IS THE FINDING

At t=3409 — the pose 16g already flagged as having the lowest cube-tap share —
the mechanism retires **1.5 % MORE instructions** and takes **4.9 % fewer
cycles and 6.0 % less core time**. It is not a load artifact: `Gcyc/f` (a PMU
counter), `core-ms` (Σ tile-task time) and `wall` all move the same way at all
five poses, and the OFF arm sits at parent ±1.4 % at every pose as the control.
IPC goes 3.42 → 3.86. The change converts a scalar dependent chain — twelve
dependent loads and an 18-flop matmul per (pixel × light) — into wide
independent work, so it trades retired instructions for issue rate.

**This campaign quotes `Ginstr/f` because WALL is load-dirty, not because
instructions are the goal.** When the proxy and the goal disagree this cleanly,
the entry has to say so rather than pick the flattering column.

Confirmed on a quiet machine (load 6.9–7.4, three interleaved rounds, the two
poses that bracket the effect), because "wall is load-dirty" is exactly the
objection this claim invites:

| | `lighting-w1` Gi | Gcyc | core-ms | IPC | `renderFrame` wall |
|---|--:|--:|--:|--:|--:|
| t=3409 par | 1.545 | 0.450 | 157.6 | 3.44 | 43.83 |
| t=3409 **ON** | **1.569 (+1.6 %)** | **0.431 (−4.2 %)** | **148.9 (−5.5 %)** | **3.64** | **43.53** |
| t=5743 par | 1.542 | 0.446 | 155.8 | 3.46 | 43.17 |
| t=5743 **ON** | **1.480 (−4.0 %)** | **0.385 (−13.7 %)** | **135.6 (−13.0 %)** | **3.84** | **41.57 (−3.7 %)** |

Every column reproduces to ±0.001 Gi and ±0.005 Gcyc across the three rounds.
**Say the weak pose plainly: at t=3409 the FRAME is a wash** — 43.95/43.53 ON
against 43.95/43.83 parent, inside the run-to-run spread — not a win. The
lighting phase there is −5.5 % of core time and the frame does not feel it,
because at that pose lighting is a smaller share. On the goal (frame time) this
is a win at four poses and neutral at the fifth; on the proxy (instructions) it
is a win at four and a −1.6 % loss at the fifth. Both are above; neither was
picked.

### THE PRICE, STATED HONESTLY

The ladder puts the prologue at 0.368 Gi/f and the mechanism nets 0.068 of
instructions, so the row pass costs about **0.30 Gi/f to fill ~5.4 M lane-slots
— ~55 instructions a slot**, four times what counting the vector body predicts.
The eager/lazy ratio is the visible half (5.4 M slots filled for 2.943 M taps
taken at t=5743; 4.9 M for 1.600 M at t=3409, and 4.7 M for 2.871 M at t=5813 —
`--omni_census`, and that ordering is exactly the ordering of the result).
Where the other 40 go was not isolated: four shapes of the inner loop were
measured and the spread between best and worst is 2 %, so it is not the face
scan and not the store.

| inner-loop shape (all bit-exact) | ON Gi/f |
|---|--:|
| face scan + per-lane predicate store | **1.480** |
| face scan + verdict folded into a packed slot | 1.508 |
| all-same-face fast path + packed slot | 1.484 |
| all-same-face fast path + per-lane store | 1.490 |

The reduce that tests "all eight lanes on one face" costs more than the
eight-iteration scan it skips, and folding the reject verdict into `mapIdx`
with a vector select costs more than the per-lane branch it removes — that
branch is perfectly predicted, because a rejected lane is rare and clustered.

**~390 lines and a per-worker scratch buffer for −1.3 to −4.2 % of frame.**
That ratio is the judge call and it is stated here, not buried.

### THE BYTE VERDICT IS A COUNTER, NOT AN ARGUMENT

`--deferred_cube_prepass_verify` re-runs the WHOLE SCALAR TAP behind every
cached one — with the pixel BODY's own view and world positions, not the row
pass's — and compares the two answers as bit patterns. That is deliberately
wider than "is the vector maths right": it also catches the row pass's
duplicate reconstruction of those six floats drifting a ULP from the body's,
which is the other way this design could move a pixel.

**0 mismatches in 47 M taps**: 23.9 M in the bench loop, 16.9 M at
`--snapshot=greets@t=5743` under his arm, 6.3 M at the greets t=1588 pin.

The vector maths is pinned to the scalar's own codegen, read off `otool -tvV`
and not guessed: the matmul's MIDDLE product is a plain `fmul` with the first
and third contracted into it and the offset add NOT contracted; `smY` is an
`fmsub`. Written as clang `ext_vector_type` operators under `FP_CONTRACT_OFF`
with `__builtin_elementwise_fma` at those sites — simde's spelling is not an
fma on arm64 and a call-site pragma cannot reach it (2026-08-16e).

### WHAT THE GATES ARE WORTH — the same warning as 16i, re-earned

`render_gate.sh` is **4/4 with the flag on and off, and cannot discriminate it.**
Instrumented, `mirrortest` / `conetest` / `halotest` / `rttslot` check **0 taps**
through the prepass, and so do **city, chase and fountain** — the gate needs
`lmKernelEnabled == false`, i.e. `--shadow_dynamic` without
`--shadow_lm_dynamic`, plus ShadowMode::PolyId, and greets is the only scene
that sets it. **The blast radius of this flag is greets.** The real coverage is
the five acceptance poses and the greets t=1588 pin (6.3 M taps through the
flag, `570a7b44` unmoved), all identical parent-to-child.

**And the fountain flip appeared again, exactly as 16i predicted.** The first
byte-gate run of the PARENT binary produced `b91cb2ba…` at fountain t=2500
instead of `8db68ccb…` — the same alternate hash 16i recorded, on the parent,
in this session's first three runs. Re-gated 6/6 `8db68ccb` on BOTH binaries.
Without 16i's note this round would have spent a battery on it.

### WHAT IS LEFT ON THE TAP

1. **The call frame, 0.069 Gi/f (23.4/tap, 11.5 %).** `CubeShadow_Sample` still
   spills nine callee-save pairs into a 144-byte frame, and 81 % of taps never
   touch the state that forces them. Splitting the tail's RARE half (the 2x2
   PCF, 18.9 % of taps) into its own `noinline` function would make the common
   path a leaf. Not tried. Cheapest remaining item by far.
2. **The eager/lazy ratio — and the one reject that IS safe was TRIED AND
   REFUTED.** The sweep fills 5.4 M slots for 2.943 M taps at t=5743 and
   4.9 M for **1.600 M** at t=3409, which is exactly why t=3409 is the weak
   pose: it is the MIRROR pose, `--omni_census` puts 28.1 % of its pairs on the
   mirrorId reject and another 9.4 % on the 2-D map shadow, so the row pass was
   filling three slots per tap there instead of 1.8. The mirrorId test is the
   only reject the row pass can replicate SAFELY — it is an INTEGER compare, so
   its copy agrees with the loop's at every input, where `len2 > r2`, N·L and
   the cone are float compares whose one-ULP disagreement would make the sweep
   skip a group whose slot the loop then reads. Implemented as a group-granular
   `any lane matches` skip (mirror pixels are the mirror's screen rect, so
   groups go the same way): **it made things worse.** `lighting-w1` at t=5743
   1.479 → 1.523 and at t=5813 1.430 → 1.474, against 1.569 → 1.564 at the pose
   it was written for. The per-pixel `pmid` plane and the per (group × light)
   test cost about ten times what the arithmetic says they should — the same
   discrepancy as the ~55-instructions-a-slot above, and the fifth shape in this
   round to lose to it. Reverted; do not re-derive.

   What is left of the idea: a range/N·L skip needs a MISS sentinel the consumer
   falls back on, which puts a second test back in the omni loop — the thing
   16h says costs what it saves. Unpriced, and now unpromising.
3. **The uniformity pyramid, 0.056 Gi/f** — a dependent load per tap; the 4x4
   pyramid was priced and refused in 16g.
4. ~~**`--ssao_downscale=2` is still the single largest lever on this arm**, and
   still a look change nobody has approved.~~ **SPENT 2026-08-16y — countersigned
   and landed as the DEFAULT** (realized −9.6 ms of frame; see the 2026-08-16y
   entry at the top). Read this item as closed wherever it appears.

## 2026-08-16k — THE SHATTER NONDETERMINISM, CLOSED: one `static` scratch array shared by 12 shard workers. 15 flips in 49 → 0 in 48

**16j handed on "the greets mirror shatter is nondeterministic, ~24.5 %, it lives
in the 12-worker fan-out". It does, and it is not in `MirrorShatter.cpp` at all —
it is one word in the tile-light builder the fan-out calls.**

### THE WRITE

`FDS/RENDER/DeferredLightLists.cpp:247`

```c
static TileChunkSphere chunk[DEFERRED_NUM_TILES];   // <- the bug
```

Written for every tile of THIS call's grid at `:252`, read back across the whole
light loop at `:328-333` (the `sphereCull` branch) and `:340` (`.valid`, cone
cull). `static`, not `thread_local`, not per-call — so it is **shared mutable
state between every concurrent caller of `buildTileLightLists`**.

The mirror-shard bake is exactly such a caller, ×12:
`MirrorShatter::renderReflectionCameras` (`MirrorShatter.cpp:1073`) fans 238
shards over 12 pool workers; each worker's `renderShardIntoCell`
(`:1218`) calls `Render_DeferredLighting(dctx, &ov)` (`:1436`), which reaches
`buildTileLightLists` at `DeferredSurfaceKernel.cpp:7380`. Every per-worker
buffer that pass owns was correctly made per-worker — `ov.gb`, `ov.lights`,
`ov.tileLights`, `ov.vpage`, the cull cone globals are `thread_local` — and this
one array, two frames deeper in the call chain, was not. At
`--deferred_offscreen_tile_px=32` a 64² cell grids **2×2**, so all twelve workers
write `chunk[0..3]`: the same four structs, the same cache line. Whoever wrote
last decided the other eleven workers' cull, and the surviving (tile × light) set
— hence the shaded reflection — depended on thread interleaving.

**FIX: drop the `static`.** `TileChunkSphere chunk[DEFERRED_NUM_TILES];`, 96 × 20 B
= 1.9 KB of stack, written before read inside the guarded branch and never read
outside it. Nothing else changes.

### THE EVIDENCE CHAIN — every step run, none read off the source

| # | arm (greets `t=6293,6294`, his acceptance arm, `FDS_GREETS_SHATTER=1`) | runs | result |
|---|---|--:|---|
| 1 | parent `6c3d38d8`, baseline | **49** | **15 flips, 13 distinct** — 30.6 %, reproduces 16j's 24.5 % |
| 2 | parent + `--no-spot_cone_cull --no-deferred_tile_sphere_cull` | 24 | **0 flips** |
| 3 | parent + `--no-deferred_tile_sphere_cull` ONLY | 24 | **0 flips, and BYTE-IDENTICAL to (2)** |
| 4 | **fixed** binary, same arm as (1) | **48** | **0 flips** |
| 5 | fixed, `FDS_SHARD_REFL_SERIAL=1` | 24 | 0 flips |

**(3) is the localization, and it is exact.** The sphere cull is the *only*
consumer of the corrupted array's DATA (the cone cull reads only `.valid`).
Turning it alone off kills every flip and lands on the same hash as turning both
off — so the cone cull's `.valid` read never decided anything here, and the race's
entire effect goes through `chunk[idx].{cx,cy,cz,R}` at `:328-333`. Nothing was
changed in `MirrorShatter.cpp` to get (2)/(3): they are pure flag arms on the
parent binary.

**(4) is the confirmation, and it is stronger than "0 flips".** The fixed binary
is stable at **`852aabe6a4106182` / `f3c3a2018edfa609`** — *exactly the parent's
modal hashes*, and exactly the parallel modal 16j recorded (`852aabe6…`). The
modal was always the self-consistent answer (every worker reading its own write);
the fix just makes that outcome the only one. (5) sits at **`0ff07c7305582656` /
`467625dfdf34a4f7`** — 16j's recorded serial values, unmoved.

**Size of the defect, imaged:** a flipped parent frame against the modal is
**2 145 px of 2 073 600 (0.103 %), max Δ 1/255**. It is an LSB-level wobble in the
shard reflections — small to the eye, fatal to byte-gating the arm.

### GATES — parent-vs-fixed on one tree, and the recorded values

| pin | parent | fixed | |
|---|---|---|---|
| city t=1961 `--deferred`, `FDS_CITY_ENV_PIXEL=1` | `bd4ffbf8` | `bd4ffbf8` | **recorded value** |
| city t=1961 acceptance arm | `4cb8d2ca` | `4cb8d2ca` | **recorded value** |
| city t=2400 / t=400 acceptance arm | `f473fe2b` / `d3374de6` | same | **recorded values** |
| fountain t=2500 | `8db68ccb` | `8db68ccb` | **recorded value** |
| greets t=1588 | `570a7b44` | `570a7b44` | identical (clean-worktree value) |
| chase t=100/400/800/1200/1600 | 5 values | same 5 | identical 5/5 |

`render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`).
Say what that gate is worth, again: **it cannot discriminate this fix** — no row
runs a mirror shatter, and the change is inert on any single-threaded caller. The
coverage that carries weight is the 49-vs-48 flip battery and the parent-identical
pins.

### PERF — no measurable cost, and the mechanism says there cannot be one

`[SHARD-REFL]` wall, `FDS_SHARD_REFL_PROF=1`, split by call ordinal (call 1 = cold
bake, call 2 = warm — pooling them is what made the first battery read
"+10 %" nonsense), order-rotated A/B, run 1 discarded:

| | parent | fixed | min Δ | median Δ |
|---|--:|--:|--:|--:|
| call 1, min-of-30 | 45.90 ms | 46.60 ms | +1.53 % | +0.10 % |
| call 2, min-of-30 | 18.50 ms | 18.40 ms | −0.54 % | +1.01 % |
| call 1, min-of-16 (independent) | 46.60 ms | 46.60 ms | +0.00 % | +0.10 % |
| call 2, min-of-16 (independent) | 18.40 ms | 18.70 ms | +1.63 % | +0.00 % |

**The sign flips across repetitions and across statistics, so this is noise, not a
delta.** Paired per-round (n=30): call 1 **−0.33 ms** [95 % CI −2.31, +1.64], call
2 **−0.82 ms** [−3.00, +1.36] — point estimates favour the FIXED binary, CI
straddles zero. The instrument cannot bound this at 1 %; the mechanism can. The
change removes a BSS array twelve threads were writing to the same cache line and
replaces it with a stack-pointer adjustment; only 4 of the 96 entries are ever
touched on this pass, and none is initialised. There is no work added.

### THE SECOND DEFECT IN THE SAME FUNCTION — MEASURED, **NOT** LANDED

`tileChunkSphere` (`DeferredCommon.h:496`) reads the **engine globals**
`FOVX / FOVY / CntrEX / CntrEY`. It takes no projection parameters.
`buildTileLightLists` shadows those four names with the caller's camera at
`DeferredLightLists.cpp:211-212` — which reads as if the helper picks them up. It
does not: it is a free function in a header and sees only the globals. So:

* **SERIAL shard bake** — `renderReflectionCamerasSerial` assigns the globals per
  shard (`MirrorShatter.cpp:833-838`), so the chunk spheres are correct.
* **PARALLEL shard bake** — nothing assigns them; they hold the **main camera's**
  1920×1080 projection while the tile rect is a 64² cell. The spheres are garbage.

**Controlled, not argued.** On the serial arm, `--no-spot_cone_cull
--no-deferred_tile_sphere_cull` is **byte-identical** to the culls on (6/6,
`0ff07c73…`) — with the right projection the sphere cull rejects nothing, exactly
as the flag's "BYTE-NULL" claim says. On the parallel arm the same flag **changes
the frame**: `852aabe6…` → `63671ae3…`, **25 567 px (1.23 %), max Δ 1/255**. That
difference is the sphere cull dropping (tile × light) pairs that do reach pixels,
because it is testing against the wrong frustum.

Not landed because it is a LOOK change in a scene under active tuning, and it is
his call. The patch shape is mechanical: give `tileChunkSphere` four projection
parameters and pass `cam.fovX/fovY/cntrEX/cntrEY` at
`DeferredLightLists.cpp:252` and `:347`, and the in-scope projection at
`DeferredVolumetric.cpp:3039`. **It does not close the serial-vs-parallel gap on
its own** — parallel-with-culls-off `63671ae3…` still differs from serial
`0ff07c73…`, because the two paths are different functions (global-swap
`Render(ForceDeferred, skipVolumetric=true)` + `reflGB_` vs per-worker
`Render_DeferredLighting` + `Render_VolumetricCones` + `w.gb`), exactly as 16j
warned. Byte identity between those two arms is not available and never was.

### METHOD NOTES THAT GENERALISE

1. **The audit class is "function-local `static` scratch", and it is invisible to
   a caller audit.** Everything `renderShardIntoCell` owns was already per-worker.
   The race was two frames down the stack in a file whose author never imagined a
   concurrent caller. Grep for `static` in every callee of a fan-out, not just the
   fan-out.
2. **A wall-clock A/B must be split by call ordinal.** The shatter's first bake is
   cold (46 ms) and the second warm (18 ms); pooling them gave a median "+10.3 %"
   that meant nothing.
3. **`--repro` still is not a determinism instrument** (16j). Every number above
   is `--snapshot`.
4. Remaining open from 16j and untouched here: the `--hdr` cone/halo gate row that
   `render_gate.sh` still lacks, which is what blocks landing 16j (b)'s
   `VolCompositeAdd` predicate.

## 2026-08-16j — 16i's THREE HANDOVERS, RUN: one is REAL and fixed, one CANNOT FIRE, and the battery found a DIFFERENT live nondeterminism

**16i (4) handed on two bugs "found by code reading, NOT verified by running
anything" plus one recorded ~2 % pin flip. All three were run. Verdicts:**

| item | verdict |
|---|---|
| (a) OuterVec + `--hdr` + checkerboard black checkerboard | **CONFIRMED by rendering, FIXED** (`DeferredSurfaceKernel.cpp:6309`) |
| (b) `VolCompositeAdd` racing on `g_hdrActive` / `g_hdrBuf` from shard workers | **REFUTED as written** — the precondition is real and measured, the write is never issued |
| (c) fountain t=2500 1-in-43 flip | **0 flips in 49** on the parent at tip `aa60d0ce` — stays an open singleton |
| — | **NEW: the greets mirror SHATTER is nondeterministic, 12 flips in 49 (24.5 %), and it is not (b)** |

### (a) THE BLACK CHECKERBOARD IS REAL, AND IT IS TOTAL — city AND crash

16i predicted it from the source and could not find a shipping config that
reaches it. Reached with two flags, and it is not subtle. **city t=1961,
1920×1080, `--deferred --hdr --deferred_checkerboard --profiler=0`**, censused
by the wave parity `(x^y)&1` (wave 1 shades parity 0, the fill covers parity 1):

| arm | mean luma | wave-1 half | wave-2 half | Δ | px < luma 4 on wave-2 half |
|---|--:|--:|--:|--:|--:|
| `--hdr` (full rate, control) | 175.36 | 175.36 | 175.35 | **0.01** | 723 (of 1 036 800) |
| `--deferred_checkerboard` (no HDR, control) | 89.61 | 89.64 | 89.57 | **0.06** | 134 |
| **`--hdr --deferred_checkerboard`** | 166.85 | **175.36** | **158.33** | **17.03** | **91 764** |
| **`--hdr --hdr_linear --deferred_checkerboard`** | 197.06 | 206.79 | 187.32 | **19.46** | **80 973 (100 % of them on wave-2 parity)** |

The wave-1 half is **bit-identical to the full-rate arm** (175.36 both), which is
the control that says this is the FILL and nothing else. **crash f120** (a dark
scene, so read the ratio not the absolute): wave-1 half 4.47, wave-2 half
**0.14**, and 1 036 081 of 1 036 800 wave-2 pixels below luma 4 — the whole half
of the frame is black. Pictures, before/after:

* `docs/img/ovchk/city_t1961_chk_crop8x_before.png` / `..._after.png` — 64×64 at
  (1376,128), nearest-8× so the lattice is a lattice and not a grey.
* `docs/img/ovchk/city_t1961_hdrchk_before.png` / `..._after.png` (full frame),
  `..._hdr_fullrate.png` (the control), `..._hdrlin_chk_before/after.png`,
  `docs/img/ovchk/crash_f120_chk_before/after.png`.

**THE MECHANISM, AND WHY THE FIX IS NOT AT THE KERNEL 16i WARNED ABOUT.**
`Render_DeferredLighting_Tile_OuterVec` writes **no** `ctx.hdrBuf` — its 8-bit
pack IS the HDR transport, and `Hdr_ActivateNoFog` lifts it precisely because it
leaves `h[3]` at 0 (16i put the warning comment at that kernel's head; it is
right and it stays). The wave-2 fill did not know that: it averaged neighbour
RADIANCE out of `ctx.hdrBuf` — all zero, because OuterVec never wrote it — and
then stamped `h[3] = 1.0f` (`DeferredSurfaceKernel.cpp:6588 / :6678 / :6980`).
That stamp is what does the damage: `Hdr_ActivateNoFog`'s `if (h[x*4+3] != 0.0f)
continue` skips the pixel, so the lift never reaches it and the tonemap prints a
cleared buffer.

**FIX** (`DeferredSurfaceKernel.cpp:6309`): one term on the fill's `hdrWrite`,
`&& !outerVecG`. Wave 2 then takes the SAME transport wave 1 took — 8-bit only,
`h[3]` left 0, `Hdr_ActivateNoFog` lifts both halves identically. It also clears
`fillLdrSkip` for free (that is `&& hdrWrite`), which is *required*: with no HDR
write the VPage average IS the product on this path. After: city parity Δ
**17.03 → 0.05** and **91 764 → 733** dark wave-2 pixels (against 722 legitimately
dark ones on the wave-1 half); under `--hdr_linear` Δ **19.46 → 0.03** and
**80 973 → 0**; crash f120 4.47 vs 4.47 both halves.

**REACHABILITY — it is two flags, not exotic.** `PreferOuterVec = 1` is set by
**three** scenes, not the two 16i named: city (`CITY.CPP:2537`), crash
(`CRASH.CPP:25`) and **fountain (`FOUNTAIN.CPP:1029`)**. None of them
`setDefault`s checkerboard or quarter, so the shipping demo never hits it — but
`--cinematic` turns HDR on per scene, so `--cinematic --deferred_checkerboard`
(or `--deferred_quarter`) on any of those three was a black-latticed frame.
greets is the scene that DOES `setDefault(deferred_checkerboard, true)` and it is
not a `PreferOuterVec` scene, which is why this never showed up in the arm
everything is benched on.

**BYTE-NULL OFF ITS PREDICATE, and this was run, not argued:** city `--hdr`
alone `4b0e31bf…` and city `--deferred_checkerboard` alone `58644ea7…` are
identical parent-to-fixed; four 16f city pins at their recorded values
(`bd4ffbf8` / `4cb8d2ca` / `f473fe2b` / `d3374de6`); fountain t=2500 `8db68ccb`;
chase t=100/400/800/1200/1600 identical parent-to-fixed 5/5; greets t=1588
identical parent-to-fixed; `render_gate.sh` **4/4 PASS** (`4ac809e5` / `826c09e6`
/ `b41894f9` / `166fa25a`).

> **Note on quoting that gate, in 16i's own spirit.** `render_gate.sh` **cannot
> discriminate this flag** either: no arm passes `--deferred_checkerboard`. The
> real coverage is the city/crash census above plus the parent-identical control
> arms. And the chase / greets pin values in a clean worktree are NOT the
> recorded ones (greets' pin keys on uncommitted authoring files, and the chase
> figures in 16f were taken one-pose-per-process); **parent-vs-fixed identity on
> the same tree** is the control that carries the weight there, and it holds.

### (b) `VolCompositeAdd`: THE PRECONDITION IS REAL. THE WRITE IS NEVER ISSUED.

16i's reading was half right, and the half it got right is worth keeping:
**`g_hdrActive` really is stale-`true` while the shard workers run.**
Instrumented and printed from inside the bake, greets under his acceptance arm,
`FDS_GREETS_SHATTER=1`, two ticks:

```
[VOLCONES] inline=1 64x64 spots=50 hdrActive=0   <- tick 1 (nothing has activated yet)
[VOLCONES] inline=1 64x64 spots=50 hdrActive=1   <- tick 2, x238 shards, 12 workers
```

`Hdr_BeginFrame` clears the flag only inside `renderFrame` (`RENDER.CPP:698`),
and `renderReflectionCameras` runs in the greets TICK, before it — so from frame
2 on, every shard worker sees the previous frame's `true`. If `VolCompositeAdd`
were reached there it would do exactly what 16i predicted: take the HDR arm and
add into `fds::g_hdrBuf` — the MAIN frame's 1920×1080 buffer — at cell-local
indices 0..4095, from 12 threads at once.

**It is never reached.** A counter on both arms of `VolCompositeAdd`, over
238 shards × 2 frames: **0 HDR-arm calls and 0 LDR-arm calls**, at the scripted
shatter camera, at the scene camera, with `--draw_cones`, and with
`--fast_fog --fast_fog_density=3 --draw_cones`. The instrument is not broken —
the same counter reads **4 381 429 … 5 055 038 LDR-arm calls per frame** on
`--snapshot=conetest`. The reason is upstream of the composite: every call site
is guarded by `if (accB <= 0 && accG <= 0 && accR <= 0) continue`
(`DeferredVolumetric.cpp:2436 / :2759 / :3418 / :3765`), and in greets the cone
integration accumulates **zero everywhere — in the shard cells AND in the
1920×1080 main frame**, with 50–62 spots in the prefilter. So the shard bake's
`Render_VolumetricCones` (`MirrorShatter.cpp:1457`) is a pass that runs and
composites nothing.

**NOT FIXED, deliberately.** The correct predicate is known and is the one this
codebase already adopted for the kernels — "am I the pass that owns the global
buffer?", i.e. `fds::g_hdrActive && !g_hdrBuf.empty() && ctx.hdrBuf ==
g_hdrBuf.data()` instead of the bare `g_hdrActive` at
`DeferredVolumetric.cpp:327`. `ctx.hdrBuf` is already correct at all three
callers (main frame → `g_hdrBuf.data()`; mirror RTT → `g_hdrBuf.data()`, sized to
the slot by its own `Hdr_BeginFramePass`; shard bake → `ov.hdr`, i.e. the
per-worker buffer or null). **What blocks landing it is that nothing can gate
it:** `render_gate.sh`'s conetest and halotest rows both run WITHOUT `--hdr`
(4.4 M calls, all on the LDR arm), so a mistake in the HDR arm passes 4/4
silently. **Prerequisite for the next round: an `--hdr` cone/halo gate row.**
Until then this is a dormant misroute behind a pass that composites nothing, and
`GreetsMirror.cpp:3648` already carries the sibling mitigation with the comment
that names this exact hazard ("clear the active flag so a later pass (e.g.
parallel shards) can't accumulate into this RTT-sized buffer at its own dims").

### (c) FOUNTAIN t=2500: 0 FLIPS IN 49 — the 1-in-43 stays a singleton

`./DEMO --snapshot=fountain@t=2500 --out=D --deferred --hdr --glass-refract=1
--glass-test --profiler=0`, parent binary at tip `aa60d0ce`, 49 launches:
**49/49 `8db68ccb59416e9a44037e9f387b7bd9`**. Recorded so 16i (3)'s 1-in-43
`b91cb2ba…` stays a documented singleton rather than becoming a ghost. Running
total on that pin across rounds: 1 flip in 43 + 24 + 24 + 49 = **1 in 140**.

### THE FIND THE BATTERY ACTUALLY MADE: THE SHATTER IS NONDETERMINISTIC

Hunting (b) turned up something (b) does not explain. **greets, his acceptance
arm, `--snapshot=greets@t=6293,6294` (two ticks, one process), parent binary:**

| arm | runs | result |
|---|--:|---|
| no shatter | 25 | **25/25 identical**, both poses (`9f5e2400…` / `a32db3f3…`) |
| no shatter, single pose t=6293 | 48 | **48/48 identical** (`9f5e2400…`) |
| **`FDS_GREETS_SHATTER=1`** | **49** | **37 modal + 12 flips over 10 distinct values — 24.5 %** |
| `FDS_GREETS_SHATTER=1`, per pose | 25 | t=6293 **16/25 modal**, t=6294 **16/25 modal** (9 flips each, 8 distinct values each) |
| **`FDS_GREETS_SHATTER=1 FDS_SHARD_REFL_SERIAL=1`** | **25** | **25/25 identical, BOTH poses** (`0ff07c73…` / `467625df…`) |

Same binary, same tree, same two poses, one environment variable apart. So greets
is deterministic and **the mirror-shard bake is not** — a live nondeterminism in
the arm he actually runs, whenever the shatter has fired.

**TWO THINGS ARE ALREADY LOCALIZED, BOTH BY MEASUREMENT.**

1. **It is not (b), and not anything HDR.** The per-pose split shows **tick 1
   flips at the same rate as tick 2** (16/25 modal each) — and on tick 1
   `g_hdrActive` is `false` during the bake (printed above). A mechanism that
   needs the stale `true` cannot produce a flip on the frame that does not have
   it. Every piece of shard state that looked like a candidate is already
   `thread_local` anyway (`FrameState.cpp:36-60`: `g_offAxisFrustumCull`,
   `g_reflVertCull`, the cone / census / phase accumulators).
2. **It is in the FAN-OUT, not in the per-shard math.**
   `FDS_SHARD_REFL_SERIAL=1` is **25/25 identical on both poses**. Read that with
   one caveat stated rather than buried: the serial arm is a different function
   (`renderReflectionCamerasSerial`, the global-swap deferred bake) and its
   hashes differ from the parallel modal ones (`0ff07c73…` vs `852aabe6…`), so
   this is "the serial implementation is stable", not a same-code A/B. It is
   still the right first bisect: whatever is unstable is reached only when 12
   workers run `renderShardIntoCell` concurrently.

**Unproven candidates, listed as hypotheses and NOT as findings:** the per-worker
`Transform_Objects(sc, w.camCtx, w.faces, …, &w.scratch)` at
`MirrorShatter.cpp:1374` walks the SHARED scene meshes from 12 threads with 12
different cameras — any mesh-level cache it touches (`T->worldVerts` is read at
`Transform.cpp:2021`) would be shared storage; and the shards' `mesh->Flags`
visibility toggle brackets the whole pass. Neither has been measured. Do not
quote them until they are.

**NOT the `--repro` harness's own nondeterminism, and worth stating because the
first battery of this round was thrown away on it:** `--repro=greets@t=3122
--repro_from=3112 --repro_settle=0` gave **48 DISTINCT hashes in 48 launches**,
but that harness deliberately leaves `g_fineSceneClock` at its interactive
default (`ReproHarness.cpp` says so in as many words), so it is wall-clock
dependent BY DESIGN and cannot be used as a determinism instrument. The
`--snapshot` path pins that clock, which is why the 25/25 control above is worth
something.

**NEXT ROUND — TODO, and this is the one to take.** Localize the shard-bake
nondeterminism with the RTT-dither stage-digest method: digest each worker's cell
(`w.surf.Data`) at each phase boundary in `renderShardIntoCell`
(`MirrorShatter.cpp` — setup / `Transform_Objects` / `MekaleleFillRegionInline` /
`Render_DeferredLighting` / cones / glaze / text), keyed by shard index so the
work-stealing `cursor` order does not itself perturb the digest, dump post-hoc,
and diff a flipping launch against a modal one to find the first diverging stage.
The free first bisect is already spent (serial is 25/25), so start from the
fan-out: digest per shard index, then bisect the phases inside
`renderShardIntoCell`. A second cheap discriminator before that: dump the atlas
(`FDS_SHARD_ATLAS_DUMP`) across launches to establish whether the flip is already
present IN the atlas (the bake) or only in the frame that samples it.

## 2026-08-16i — WAVE 1's dead LDR chain: the argument reaches it, the chain is 0.033 Gi/f, and the predicate needs one more term than wave 2's

**Result at greets t=5743 / 2845 / 6097 / 3409 / 5813, 1512x848, his acceptance
arm: `lighting-w1` −1.72 to −1.96 % instructions at EVERY pose (1.570 → 1.542 at
t=5743), `DeferredLighting-call` −1.45 to −1.61 %, `renderFrame` −0.54 to
−0.63 %. `lighting-w2` flat to four decimals at every pose. BYTE-NULL: five
poses under his own arm identical parent-to-child 3/3, nine 2026-08-16f pins at
their recorded values, `render_gate.sh` 4/4, the city underlay control
`56f6aff0` on both binaries.**

Commits `c0ccb40d` (instrument) · `2259c2f2` (`--deferred_shade_ldr_skip`,
default ON) · `aee1e8d7` (the stale `GreetsMirror.cpp` lift comment,
adjudicated).

### FIRST, THE SIZE — because 16h's handoff invites the wrong reading

16h left this as "**THE SAME LDR ARGUMENT APPLIES TO WAVE 1, WHICH IS
1.570 Gi/f** … the single largest untried item the round leaves behind". The
argument does apply, in full. The chain it reaches is **0.033 Gi/f**, not 1.570.

`-DFDS_W1LDR_ABLATE=1` (committed; stage 0 builds a binary `cmp`-identical to
the parent, which is what makes it quotable) removes exactly the chain:

| | `lighting-w1` Gi/f | call | `renderFrame` |
|---|--:|--:|--:|
| t=5743 stage 0 (= parent) | 1.570 | 1.855 | 4.805 |
| t=5743 stage 1 | **1.537 (−2.10 %)** | 1.822 | 4.772 (−0.69 %) |
| t=2845 stage 0 | 1.457 | 1.746 | 4.819 |
| t=2845 stage 1 | **1.425 (−2.20 %)** | 1.713 | 4.787 (−0.66 %) |

**2.1 % of `lighting-w1`, 1.8 % of the call.** Consistent with 16g's pixel
ladder, which priced "compose (after the loop)" at 0.060: 0.033 of that is the
LDR half, the rest is the HDR store, which stays. A stage 2 (tail only,
arithmetic held alive by a sink) read *above* stage 0 — its four sink `fadd`s
cost more than the tail it removed — so the 0.033 does not split and nothing
should be quoted from it. Same method note as 16g/16h; it is now three for
three.

### THE PREDICATE — `ctx.ldrDiscarded` IS NECESSARY BUT NOT SUFFICIENT

Wave 2's flag is gated on `ctx.ldrDiscarded` alone. Wave 1 needs **one more
term, `--hdr_linear`**, and the reason is not about readers at all:

* Under `--hdr_linear` the shipped radiance is `rlB = aB²·dlB + sB`, built from
  `dlB` — the **raw light accumulator** — and never from `fdB`. The whole
  fd → out → `out[i]` chain feeds the VPage alone, and `Render_TonemapToVPage`
  rewrites every byte of it (its tile body has no coverage test anywhere).
* Under plain `--hdr` (B1 gamma) the shipped radiance **is** `hB = fdB + sB`.
  The chain is LIVE. A `ldrDiscarded`-only gate would have blanked every
  gamma-HDR frame. **The fill never had this hazard** — its HDR average is
  built from the neighbours' `hdrBuf` on both arms — which is precisely why
  16h's predicate could not be copied across.

Plus a viz escape: the five debug-viz stomps write `out*` and nothing else, so
any of them forces the chain back on (`constexpr false` unless `FDS_DEV`).

**The reader audit that produced this was run blind and converged on the same
predicate.** What it found, for wave 1's specific outputs (wave 2's audit only
covered the fill's):

| reader | verdict |
|---|---|
| the wave-2 FILL's neighbour gather (`out[nidx[k]]`) | reads wave-1 VPage — but only when `!fillLdrSkip`, i.e. under the same `ctx.ldrDiscarded`. Even with `--no-deferred_fill_ldr_skip` the average it builds is itself overwritten by the tonemap |
| xpar / glass composite (`out[i]`, `sampleBgLdr`) | the xpar span starts at `RENDER.CPP:828`, **after** `hdr-activate` (:793), so `g_hdrActive` is true and the composite takes the `g_hdrBuf` arm; the LDR arm and `sampleBgLdr` are unreachable there |
| the glass-refraction LDR snapshot (`RENDER.CPP:816`, `FILLERS.CPP:2099`) | copied unconditionally, but its ONLY reader is `sampleBgLdr` — dead, not gated |
| `Hdr_ActivateNoFog` (`Hdr.cpp:91`) and its inline twin (:825) | `if (h[x*4+3] != 0.0f) continue` — lifts ONLY uncovered pixels, never one wave 1 wrote |
| SSAO (`DeferredSSAO.cpp:969`) | the VPage arm is the `!useHdr` one; `useHdr = hdr() && Hdr_WritableFor` is implied by our predicate. `--ssao_debug` writes VPage and returns before the tonemap — excluded by `ldrDiscarded`'s third gate, and that gate is doing real work: the viz only writes where `zEnc != 0`, so without it the AO frame would show black sky |
| cones/halos, froxels, rain, sprites, bloom, the LDR post stack | shut off by `g_hdrActive`/`Hdr_WritableFor`, or read only where coverage is 0 — re-verified from source, not from 16h's text |
| all 22 `write_ppm` sites, `V_Flip`, the repro/mirrortest/cloak/displace harnesses | every one is `skipVolumetric=false` → tonemap runs first |
| `EnvBake.cpp:490`, `GreetsMirror.cpp:3821`, `MirrorShatter.cpp:1468` | 8-bit readbacks that ARE the product — all have `ldrDiscarded=false` |
| **`MirrorShatter.cpp:915`** | the serial reflection goes through `renderFrame` itself with `skipVolumetric=true` and reads its surface back with **no tonemap on that path**. Only the `!skipVolumetric` term protects it — an "is this an override bake?" test would have missed it |
| `--env_ssr`'s prologue capture (`RENDER.CPP:579`) | reads the PREVIOUS frame's post-tonemap VPage |

Three things hold today by code placement rather than by predicate and must
stay that way: the skip must never reach `Render_DeferredLighting_Tile_OuterVec`
(a warning comment now sits at its head — see below), the mip-viz store must
stay above the skipped block, and an HDR-redirect of the legacy fog
(`DeferredVolumetric.cpp:701`) or the unified volumetric would turn a currently
dead wave-1 reader live.

### MEASURED — parent / OFF / ON, one worktree, one asset tree

Order-rotated interleaved, min over 11 rounds, one pose per process, 1512x848,
`--profiler=0 --deferred_prof=1 --hw_prof`, 10 iters. Load ran 6.0–8.8, so
`Gi/f` decides; `Gcyc/f` is quoted where it agrees and is visibly load-dirty at
t=6097.

| pose | row | par | off | **ON** |
|---|---|--:|--:|--:|
| **t=5743** | `lighting-w1` Gi/f | 1.570 | 1.580 (+0.64 %) | **1.542 (−1.78 %)** |
| | .. Gcyc/f | 0.448 | 0.458 | **0.442 (−1.34 %)** |
| | .. wall | 13.23 | 13.49 | **12.99 (−1.87 %)** |
| | `lighting-w2` Gi/f | 0.272 | 0.272 | **0.272 (0.00 %)** |
| | `DeferredLighting-call` Gi/f | 1.855 | 1.865 | **1.827 (−1.51 %)** |
| | `renderFrame` Gi/f / wall | 4.804 / 43.55 | 4.815 / 43.51 | **4.777 (−0.56 %) / 43.23** |
| **t=2845** | `lighting-w1` Gi/f | 1.457 | 1.467 (+0.69 %) | **1.430 (−1.85 %)** |
| | `renderFrame` Gi/f | 4.818 | 4.829 | **4.791 (−0.56 %)** |
| **t=6097** | `lighting-w1` Gi/f | 1.326 | 1.337 (+0.83 %) | **1.300 (−1.96 %)** |
| | `renderFrame` Gi/f | 4.257 | 4.269 | **4.230 (−0.63 %)** |
| **t=3409** | `lighting-w1` Gi/f | 1.571 | 1.583 (+0.76 %) | **1.544 (−1.72 %)** |
| | `renderFrame` Gi/f | 4.977 | 4.989 | **4.950 (−0.54 %)** |
| **t=5813** | `lighting-w1` Gi/f | 1.531 | 1.541 (+0.65 %) | **1.503 (−1.83 %)** |
| | `renderFrame` Gi/f | 4.665 | 4.676 | **4.637 (−0.60 %)** |

**The OFF column earns its place a third round running: the restructure costs
+0.64 to +0.83 % of w1 on its own**, so the mechanism is worth −2.5 % against
its own OFF arm and −1.8 % against the parent. `lighting-w2` unmoved to four
decimals at all five poses is the control that says this touched wave 1 and
wave 1 only.

### THE RESTRUCTURE, AND WHY IT IS A FLOAT IDENTITY

`fd` used to be computed BEFORE the metal / roughness / metal-tint / env /
SpecMul blocks and scaled in place by two of them. It now lives after them,
inside the skipped block, with the two scalings applied **in the same order**.
`EnvSpecComposeScalar` takes the texels and `metalM` **by value** and returns
through `sB/sG/sR`; nothing between the old site and the new one touches
`texB/lB/metalM/fresEC`. So it is a reordering of *statements*, not of
*operations*.

Shape matters and was measured: the `--hdr_linear` (B2) store moved **above**
the block so nothing is live across the branch — worth ~0.3 % of w1 over the
first shape, which zero-initialised `out*/h*` outside it — and the gamma (B1)
store moved **inside** it, because `hB` is exactly what that arm ships.

### WHAT THE GATE IS ACTUALLY WORTH — say it out loud

`render_gate.sh` is **4/4 and cannot discriminate this flag**. Three of its four
arms pass no `--hdr` at all and the fourth hashes an RTT surface from an
override bake, so `ldrDiscarded && hdr_linear` is false in every one. The real
coverage is the **greets t=1588 pin** (greets `setDefault`s `hdr_linear`, so the
flag IS live there) and the **five-pose acceptance arm**, both parent-identical
3/3. Quoting 4/4 as evidence for an LDR-skip is quoting a gate that could not
have failed.

### DO NOT PORT THIS TO `Render_DeferredLighting_Tile_OuterVec`

A warning comment now sits at its head. That kernel writes **no** HDR at all —
its 8-bit pack IS the HDR transport, because `Hdr_ActivateNoFog` lifts it (it
leaves `h[3]` at 0). Skipping the pack would tonemap every `PreferOuterVec`
scene (city, crash) from a cleared buffer. The saving looks identical in the
source and is not the same code.

### PRICED AT ZERO, NOT LANDED — the override-bake exclusion IS over-broad

16h excluded every override/offscreen bake from `ldrDiscarded` outright. That is
over-broad by exactly **two** passes, and the correct predicates are:

* **greets mirror RTT** (`GreetsMirror.cpp:3582-3600`): `rttHdr && !rttTrace`.
  `rttHdr` ⟹ `Hdr_BeginFramePass` ran ⟹ `Hdr_ActivateNoFog` (:3629) cannot
  early-return ⟹ `g_hdrActive` ⟹ `Render_TonemapToVPage` (:3632) fires and
  rewrites every pixel. Nothing between :3600 and :3632 reads a covered VPage
  pixel. The `!rttTrace` term is real: :3606/:3616 digest and dump
  `s_rttSurf.Data` **before** the tonemap, so the skip would silently change
  what `--mirror_rtt_trace`'s LIGHTING stage measures.
* **mirror shard bake** (`MirrorShatter.cpp:1415`): `hdrBake`, whose
  `Render_TonemapToVPageInline` (:1447) has no `g_hdrActive` guard at all.
  Academic today — `--shard_hdr` defaults 0, so `hdrWrite` is already false.

**Not landed, because it is worth ~zero on this arm and the rule is measure
each change.** greets' whole `RTT` phase is 0.025 Gi/f of a 4.80 Gi/f frame; 2 %
of the lighting inside it is below the instrument's resolution. Recorded here so
the next round doesn't re-derive it, and so nobody "extends the win" to the RTT
without noticing the trace term.

### WHAT IS LEFT

1. **The gamma-HDR (`--hdr` without `--hdr_linear`) half of the same chain.**
   `out*`, the clamps and the `out[i]` store are dead there too — only `fd` and
   `hB` are live. That is a WEAKER skip needing its own predicate and a second
   branch in the pixel body, for a smaller win on an arm nobody benches. 16h's
   durable finding (a flag-guarded predicate in this kernel costs about what
   these mechanisms save) says price the branch before writing it.
2. **`lighting-w1` is still 1.542 Gi/f = 84 % of the call**, and 16g's split
   still stands: the omni loop is 1.25 of it and **the cube tap is 0.747, 36.6 %
   of the whole call**. Everything else in wave 1 is rounding error against
   that. The next real lever on this arm is the tap or `--ssao_downscale=2`,
   not another compose. (**The dial half of that sentence was spent 2026-08-16y**
   — `ssao_downscale=2` is now the default; the tap is what is left.)
3. **The fountain t=2500 pin is not 3/3-reliable.** One run in 43 on the
   **PARENT** binary produced `b91cb2ba…` instead of `8db68ccb…`; a 24-run gate
   on each binary then gave 24/24 `8db68ccb` on both. Pre-existing
   non-determinism, ~2 %, unrelated to this round — but a battery that calls a
   1-in-43 flip a regression will burn a session, so it is written down here.
4. **Two bugs found by code reading during the audit, NOT verified by running
   anything, handed on rather than sat on.** (a) `Render_DeferredLighting_Tile_OuterVec`
   writes no `ctx.hdrBuf`, so on a `PreferOuterVec` scene (city `CITY.CPP:2537`,
   crash `CRASH.CPP:25`) with `--hdr` **and** `--deferred_checkerboard`/`quarter`,
   the fill would average all-zero neighbour radiance and then stamp `h[3]=1`,
   which BLOCKS the `Hdr_ActivateNoFog` lift — predicted result, a black
   checkerboard. Not reachable in any shipping config found. (b)
   `VolCompositeAdd` (`DeferredVolumetric.cpp:327`) consults the GLOBAL
   `g_hdrActive` and writes the GLOBAL `g_hdrBuf` while the shard bake's cone
   pass runs on N workers against a 64² cell, and `renderReflectionCameras` is
   called from the tick before `Render()`, when `g_hdrActive` still holds the
   previous frame's `true`. Both need a run to confirm or kill.

## 2026-08-16h — `lighting-w2` (the checkerboard fill, 16 % of the call): the edge fallback is 0.12 % of it, and the win was a write nobody reads

**Result at greets t=5743 / 2845 / 6097 / 3409 / 5813, 1512x848, his acceptance
arm: `lighting-w2` −7.0 to −7.4 % instructions at EVERY pose (0.293 → 0.272 at
t=5743), `DeferredLighting-call` −1.1 to −1.3 %, `renderFrame` −0.4 to −0.5 %.
`lighting-w1` flat to three decimals. BYTE-NULL: five poses under his own arm
identical to the parent binary, nine pins 3/3 at their recorded values,
`render_gate.sh` 4/4, plus a city control for the arm the gate excludes.**

Commits `a51534dd` (instrument) · `66444117` (material hoist, no flag) ·
`f11f3e0c` (`--deferred_fill_ldr_skip`, default ON).

### THE MEASUREMENT 16g ASKED FOR, AND IT REFUTES ITS OWN HYPOTHESIS

16g's item 2 was "unmeasured: how much of w2 is the full-shade fallback —
`--deferred_checker_edge_full` is default OFF, so every material/normal/depth
edge is re-shaded by a reduced kernel, light loop and all". Two instruments now
answer it, `-DFDS_W2_CENSUS=ON` (cell census, shares the `--omni_census` runtime
gate) and `-DFDS_W2_ABLATE=n` (a nine-stage `continue` ladder with a cumulative
sink). Both committed, both byte-null at stage 0.

```
greets t=5743, 1512x848, his arm
cells 641 088   AVERAGED 635 581 (99.14 %)   FULL-SHADE FALLBACK 768 (0.12 %)
env-reflective drop 4 472 (0.70 %)   z=0 0.04 %   sentinel 0.00 %   border 0.00 %
haveOwn 99.26 %   compatible neighbours 1.980 / averaged   sharp 1.980 / averaged
```

**768 cells.** The fallback is not a cost centre and no attack on the edge
classification is justified on cost grounds. The corollary matters too: with
1.980 of 2 possible neighbours compatible and nsharp == n to three decimals, the
fill has essentially no rejects — **99 % of neighbour pairs go all the way
through**, so every per-pair instruction is paid ~2× per cell.

### THE LADDER — the two-neighbour loop is 54 % of the fill

| stage | | w2 Gi/f | delta |
|---|---|--:|--:|
| 1 | parity test only (the loop skeleton) | 0.011 | |
| 2 | + z alive test | 0.012 | +0.001 |
| 3 | + mirrorId, mat32, sentinel, matIDc | 0.021 | +0.009 |
| 4 | + envForceFull / `checker_env_full` drop | 0.026 | +0.005 |
| 5 | **+ centre oct normal decode** | 0.052 | **+0.026** |
| 6 | + neighbour index setup | 0.060 | +0.008 |
| 7 | + centre `fetchTexel` | 0.079 | +0.019 |
| 8 | **+ neighbour compat + accumulate loop** | 0.238 | **+0.159** |
| 0 | FULL (write-out + the fallback) | 0.294 | +0.056 |

**ONE oct normal decode is 0.026 Gi/f** and the fill does three per cell (centre
+ two neighbours). Stage 9 (0.308) reads ABOVE stage 0 — the top-of-body cut
forces materialisation the shipping allocator spreads out, exactly 16g's method
note. The fallback's cost is bounded by that artefact, not measured by it; the
census is what settles it.

### WHAT SHIPPED

**The material hoist (16g's item 1), NO FLAG.** `fetchTexel` re-derived the
neighbour's matID, bounds-checked it, loaded the table entry and null-tested the
Material and its Texture — for a material `neighborCompatible`'s FIRST term had
already proved equal to the centre's, and which the centre's own fetch had
resolved. `fetchTexelNb` keeps that Texture. Bit-exact by construction.
**−1.0 % of w2 at every pose.**

**`--deferred_fill_ldr_skip` (default ON) — the mover, −6.2 % more.** The fill's
8-bit VPage average is written into a buffer `Render_TonemapToVPage` then
overwrites unconditionally (it never consults the coverage lane) — the "#1 HDR
gotcha" in `docs/GRAPHICS_PIPELINE.md`, never applied here. 16g's
`--deferred_fill_hdr_skip` had already killed the LDR *sharp* accumulators on
the neighbouring argument and left the plain average alone because it IS
written; the write is the part that does not matter. Drops the neighbour colour
load + 3 extracts + 3 adds ×1.980, and the compose + store per cell.

### THE PREDICATE IS THE COMMIT — `hdrWrite` WOULD HAVE BEEN A BUG

Every VPage reader between the lighting call and the tonemap is either shut off
by `g_hdrActive`/`Hdr_WritableFor` (SSAO `DeferredSSAO.cpp:973`, the xpar
composite `DeferredSurfaceKernel.cpp:4568`, cones/halos
`DeferredVolumetric.cpp:339`, rain `DeferredFastFog.cpp:3192`, the sprite
fillers) or reads only where the coverage lane is ZERO (`Hdr_ActivateNoFog`
`Hdr.cpp:91`, the froxel composite `DeferredFastFog.cpp:2220/2364/2592`) — and
the fill sets `h[3] = 1.0f` on every arm that also writes VPage. So the write is
dead **iff the tonemap fires**, and that is strictly stronger than `hdrWrite`:

* **`DEMO/CITY.CPP:3823`'s water-reflection underlay** renders with
  `skipVolumetric=true` into the **MAIN** VPage at the **MAIN** resolution, so
  `hdrBuf` is live — but `renderFrame`'s tonemap sits inside `if
  (!skipVolumetric)`, and `CITY.CPP:3848` reads that VPage straight back to
  displace the reflection. **Its VPage IS the product.**
* `--ssao --ssao_debug` returns before the tonemap with `hdrWrite` true.
* `g_hdrDeferTonemap` (the fountain) re-runs the chain under a different
  predicate.
* Offscreen/override bakes are excluded outright — the shard bake and the greets
  mirror RTT tonemap inline, but `GreetsMirror.cpp:3621` still carries a comment
  claiming wave-2 fill pixels reach `g_hdrBuf` only via the VPage lift, and this
  flag is not the place to adjudicate that. **Open item: that comment looks
  stale against the current fill (all three HDR arms set `h[3]=1`) — somebody
  should confirm and delete it.**

So the kernel reads `ctx.ldrDiscarded`, a promise `renderFrame` sets from the
same three gates the tonemap sits behind. **Control:** city t=1961 with
`--env_live_water --deferred --hdr --city_env_pixel --deferred_checkerboard`
(the underlay pass, checkerboard forced) is byte-identical parent-to-child,
`56f6aff07f5981244463a5a7b23c2539` both.

### REFUTED — and the refutation generalises

**A scanline carry for the repeated neighbour gather. TRIED TWICE, BOTH NET
ZERO.** The cell at px reads px−1 and px+1, the cell at px+2 reads px+1 and
px+3, so every even pixel is gathered TWICE and everything gathered from it (its
G-buffer word, its Z, its decoded normal, its texel) is a pure function of that
pixel. Carrying it one step is bit-exact and should have removed one oct decode
(0.026) and one texel fetch per cell. Measured, min-of-11, three arms:

| form | w2 Gi/f, child OFF | child ON | parent |
|---|--:|--:|--:|
| `NbGather nbg[2]`, runtime `nc` | 0.361 (+23 %) | 0.312 | 0.293 |
| same, trip count 2 + `unroll(full)` | 0.351 (+20 %) | 0.309 | 0.293 |
| two one-slot index-keyed caches | 0.317 (+8.2 %) | 0.293 | 0.293 |

The first two are SROA giving up (an indexed 2-element struct array lands on the
stack); the third is clean code and still nets zero. **The lesson is the OFF
column's, and it is the durable finding of this round: in this kernel a
flag-guarded predicate or eight extra live values in the pixel body cost about
what any of these mechanisms save.** That is also why the material hoist ships
WITHOUT a flag — its first cut carried `--deferred_fill_mat_hoist` and read
par 0.293 / off 0.300 / on 0.292, i.e. the flag ate the win.

**Skipping the neighbour oct decode when its PACKED normal equals the centre's
(dot would be ≥ 0.9999 ≥ `--quarter_normal_cos`, so the branch outcome is
provably unchanged). REFUTED ON HIT RATE: 199 862 of 1 264 816 = 15.80 %.**
Worth ≈0.009 Gi/f gross before the compare costs on the 84 % that miss. Counter
kept in the census.

### WHAT IS LEFT IN w2 (now 0.272 Gi/f = 15 % of the call)

1. **The three oct normal decodes, ~0.078 Gi/f = 29 % of the fill.** The two
   NEIGHBOUR decodes are independent and `fast_rsqrt` is `vrsqrte` + one Newton
   step on a `float32x2_t` (`FDS/FILLERS/SimdHelpers.h:16`) — **lane-independent,
   so a 2-wide `oct_decode_u32_x2` is bit-exact, not a judge call.** Needs the
   two neighbours decoded together, which is the restructure that failed above;
   the difference is that this one removes registers rather than adding them.
2. **The 3-channel scalar arithmetic, est. ~0.03 Gi/f.** Per neighbour: three
   `fmax` + three `fdiv` + three compares for the texel ratio, three fp16 loads,
   three `fmla`, the texel byte unpack; per cell the HDR store. All of it is
   (B,G,R,·) and fits one NEON vector, and 4-wide reduces live registers.
   **Contraction hazard: clang already SLP-vectorises parts of this 2-wide
   (`fmul.2s` ×50, `fmla.2s` ×22 in the shipping function), so a 4-wide rewrite
   must be byte-gated, not assumed.**
3. **`hsB/hsG/hsR` are dead whenever `nsharp > 0`**, which the census says is
   ~always. Three `fadd` per neighbour ≈ 0.004 Gi/f. Needs a 2-bit "which
   neighbours passed" mask to recompute in the rare `nsharp == 0` case.
4. **THE SAME LDR ARGUMENT APPLIES TO WAVE 1, WHICH IS 1.570 Gi/f.** 16g's pixel
   ladder priced "compose (after the loop)" at 0.060 Gi/f and that includes an
   8-bit VPage combine + clamp + store per SHADED pixel, on the same
   `ctx.ldrDiscarded` reasoning and with `ctx.ldrDiscarded` already plumbed.
   **Not attempted here — out of this brief's scope — and it is the single
   largest untried item the round leaves behind.**

Renders (byte-identical to the parent, eyeballed at 1:1 for checker speckle):
`docs/img/w2fill/w2_t{2845,3409,5743,5813,6097}_after.png`.

## 2026-08-16g — 00a ROW 1 (`DeferredLighting-call`, 41 %): the tap's WRAPPER was 7 % of the call, and the floor had never been measured

**Result at greets t=5743 / 2845 / 6097 / 3409 / 5813, 1512x848, his acceptance
arm: `DeferredLighting-call` −6.8 to −7.9 % instructions and −7.2 to −9.1 %
cycles at every pose, `renderFrame` −2.7 to −3.2 % instructions. BYTE-NULL:
nine pins unmoved 3/3, `render_gate.sh` 4/4, and five poses diffed under HIS
OWN ARM (which the pin recipe does not exercise) identical parent-to-child.**

### THE DECOMPOSITION, RE-MEASURED — do not quote the 2026-08-15 split on this arm

Two new instruments, both committed and both byte-null at stage 0:
`-DFDS_PIX_ABLATE=n` (ten staged `continue`s in the PIXEL body, cumulative
sink) alongside the existing `-DFDS_OMNI_ABLATE=n`. Drivers
`scratchpad/dfl_ladder.sh`, `scratchpad/dfl_pixladder.sh`, reports
`scratchpad/dfl_ladder_report.py`, `scratchpad/dfl_pixreport.py`.

`DeferredLighting-call` = **2.040 Gi/f = 41 % of `renderFrame`'s 4.991** at
t=5743. The split, with the omni ladder's stage totals in the loop half and the
pixel ladder's in the floor half:

| item | Gi/f | % of the call |
|---|--:|--:|
| **`lighting-w1`** | **1.720** | **84.3 %** |
| — omni loop | 1.247 | 61.1 % |
|   — **cube tap** | **0.747** | **36.6 %** |
|   — specular lobe (GGX) | 0.161 | 7.9 % |
|   — `computeMapShadowAtten` | 0.079 | 3.9 % |
|   — light vector + N·L reject | 0.078 | 3.8 % |
|   — bounce portal | 0.037 | 1.8 % |
|   — diffuse accumulate | 0.035 | 1.7 % |
|   — len² + range reject | 0.034 | 1.7 % |
|   — rsqrt / dist / attenuation k | 0.032 | 1.6 % |
|   — mirrorId test | 0.026 | 1.3 % |
|   — relief-horizon in-loop test | 0.025 | 1.2 % |
|   — spot cone | ~0.000 | — |
| — **per-pixel floor** | **0.473** | **23.2 %** |
|   — view pos + SH ambient | 0.104 | 5.1 % |
|   — normal decode + TBN | 0.100 | 4.9 % |
|   — compose (after the loop) | 0.060 | 2.9 % |
|   — view dir + PBR per-pixel consts | 0.038 | 1.9 % |
|   — AO fetch | 0.036 | 1.8 % |
|   — POM horizon record | 0.034 | 1.7 % |
|   — mat32 decode + shadowMatID | 0.029 | 1.4 % |
|   — pixel-loop floor (z alive test) | 0.027 | 1.3 % |
|   — albedo fetch + tint | 0.020 | 1.0 % |
|   — sample world position | 0.017 | 0.8 % |
|   — lightmap ADDRESS resolve | 0.008 | 0.4 % |
| **`lighting-w2`** (checkerboard fill) | **0.306** | **15.0 %** |
| setup (light-list, depth-bounds, tile-cull, mirror-grid) | 0.013 | 0.6 % |

t=2845 reproduces the shape: loop 1.108, floor 0.474, w2 0.310, cube tap 0.599.

**Three things this re-measurement contradicts in the older write-ups, all
because greets' flags moved under them:**

1. **`--pbr` is ON at greets** (`GreetsApplyRunDefaults`, `GREETS.CPP:1321`).
   The specular lobe is scalar Cook-Torrance GGX, not `pow_glossClass` — so
   the 2026-08-15 note "no transcendentals to remove: `--pbr` is off at
   greets" is stale.
2. **`--shadow_dynamic` is ON at greets** (`GREETS.CPP:1281`) and
   `--shadow_lm_dynamic` is not, so `lmKernelEnabled` is **false**: the
   static-shadow lightmap is bypassed and every static-omni pixel takes a full
   cube tap. The 2026-08-15 note "`--shadow_dynamic` defaults 0 and greets
   never sets it" is stale, and its refutation (1) was reasoning about a plane
   that is not empty here.
3. **`lighting-w2` exists and is 15 % of the call.** `--deferred_checkerboard`
   is a greets setDefault; no prior round profiled the fill wave at all.

### MEASURED — parent / OFF / ON, one worktree, one asset tree

Interleaved, order-rotated, min over 11 rounds with round 1 dropped, one pose
per process, 1512x848, `--profiler=0`, `--deferred_prof=1 --hw_prof`. Load ran
6-13, so `Ginstr/f` and `Gcyc/f` are the columns that decide; `wall` is quoted
because it agrees. `off` = the CHILD binary with all three flags off — the arm
that prices the restructure by itself.

| pose | row | par | off | **ON** |
|---|---|--:|--:|--:|
| **t=5743** | `DeferredLighting-call` Gi/f | 2.039 | 2.057 (+0.9 %) | **1.877 (−7.9 %)** |
| | .. Gcyc/f | 0.558 | 0.565 (+1.3 %) | **0.516 (−7.5 %)** |
| | .. wall | 17.59 | 17.78 (+1.1 %) | **16.31 (−7.3 %)** |
| | `renderFrame` Gi/f | 4.989 | 5.007 | **4.827 (−3.2 %)** |
| | `renderFrame` wall | 45.02 | 44.34 | **43.71 (−2.9 %)** |
| **t=2845** | `DeferredLighting-call` Gi/f | 1.904 | 1.919 | **1.767 (−7.2 %)** |
| | `renderFrame` Gi/f / wall | 4.977 / 44.13 | 4.993 / 44.73 | **4.840 (−2.8 %) / 43.13 (−2.3 %)** |
| **t=6097** | `DeferredLighting-call` Gi/f | 1.759 | 1.773 | **1.638 (−6.9 %)** |
| | `renderFrame` Gi/f / wall | 4.400 / 39.48 | 4.414 / 40.05 | **4.279 (−2.8 %) / 38.90 (−1.5 %)** |
| **t=3409** | `DeferredLighting-call` Gi/f | 1.972 | 1.984 | **1.877 (−4.8 %)** |
| | `renderFrame` Gi/f / wall | 5.094 / 44.62 | 5.106 / 45.06 | **4.999 (−1.9 %) / 44.08 (−1.2 %)** |
| **t=5813** | `DeferredLighting-call` Gi/f | 1.995 | 2.012 | **1.836 (−8.0 %)** |
| | `renderFrame` Gi/f / wall | 4.846 / 43.03 | 4.863 / 43.42 | **4.687 (−3.3 %) / 42.29 (−1.7 %)** |

**The OFF column earns its place again: the restructure costs +0.2 to +0.9 %
on its own**, so the mechanism is worth −8.7 % against its own OFF arm at
t=5743 and −7.9 % against the parent. t=3409 is the weak pose (−4.8 %) — its
cube-tap share is the lowest of the five.

Per-flag, each alone against the same OFF baseline (t=5743, min-of-4):
`cube_direct` 1.577 (**it is the whole w1 win**), `fill_hdr_skip` w2 0.293
(**the whole w2 win**), `lm_addr_skip` −0.2 %.

### WHAT SHIPPED

**`--deferred_cube_direct` (default ON) — the mover.** `resolveCubeAtten` is
NOT inlined: `-S` on the shipping TU shows a real `bl` to it from the omni
loop, and `sample(1)` gives it 903 of the lighting stage's 15 653 steady-state
samples in its own frame on top of `CubeShadow_Sample`'s 5 377. It takes
twenty arguments and carries four arms — the static-lightmap sample, two
lightmap debug-recompute arms, and Depth mode's slope-bias arithmetic — every
one of which is unreachable when `lmKernelEnabled` is false and `g_shadowMode`
is PolyId, i.e. greets' shipping configuration. Calling `CubeShadow_Sample`
directly in that case passes 8 arguments instead of 20 and drops
wx/wy/wz, `lenInv`, the geometric normal, the `PixelLightmap` and the
`CubeAttenFlags` reference out of the innermost body's live set — which is the
point, in a loop whose disassembly is full of `ldr sN, [sp, #…]` folded
reloads. Predicate hoisted per TILE (the flags) × per PIXEL
(`surfaceShadowId >= 0`); anything it does not cover still takes the wrapper.
**BIT-EXACT by construction: same callee, same arguments, in the arm it
replaces.**

**`--deferred_fill_hdr_skip` (default ON).** Wave 2's `slB/slG/slR` are read
only by its `haveOwn && nsharp > 0 && !hdrWrite` arm, so under `--hdr` they
are three DIVISIONS per compatible neighbour — two neighbours per filled pixel
in checkerboard — computing a value nothing reads. Also hoists
`--quarter_tex_sharp`, which was a `FeatureFlags` read once per FILLED PIXEL.

**`--deferred_lm_addr_skip` (default ON).** Resolve the per-pixel lightmap
ADDRESS only when the lightmap kernel is live. **Measured −0.2 %: real,
reproducible, inside the noise.** Kept because it removes provably dead work,
not because it moved the number.

**The relief-horizon record behind one pointer (`HzFrame`, no flag).** See its
own commit: the ladder priced it at 0.034 Gi/f and it recovers 0.005.

### REFUTED / PRICED, NOT LANDED

* **Hoisting the GGX lobe's per-PIXEL `Gv` and `4·NdotV` out of the per-light
  loop — REFUTED BEFORE IT WAS WRITTEN.** `Gv = NdotV/(NdotV·(1−k)+k)` is a
  DIVISION recomputed per accumulated pair in the source, but `-S` on the
  shipping TU shows exactly ONE `X/(X·omk+k)` fdiv and one reload of a
  precomputed `4·NdotV` stack slot inside the depth-3 loop. **Clang's LICM
  already hoists both.** Read the source, then read the assembly.
* **A finer PolyId uniformity pyramid (`kShadowUniShift` 3 → 2, i.e. 4×4
  blocks with a 5×5 apron) — PRICED, NOT LANDED.** `DeferredLighting-call`
  1.877 → 1.858 (**−1.0 %**), `lighting-w1` 1.570 → 1.551 (−1.2 %), but
  `shadow-bake` 0.216 → 0.224 (**+3.7 %**) and the pyramid's memory goes ×4
  (≈1.9 MB → 7.4 MB across greets' 58 512² faces, both planes).
  **Net `renderFrame` −0.35 %.** Reproducible to 0.001 Gi over three
  interleaved rounds. Not worth 4× the memory on this evidence; recorded so
  nobody re-derives it. The 8×8 shift is still the right default.
* **A finer LIGHT tile grid (`DEFERRED_NUM_TILES_X/Y` beyond 12×8) — REFUTED
  ANALYTICALLY, no build.** 8.32 lights are entered per pixel to shade 2.06,
  which looks like slack, but the tile cull is conservative: a light a finer
  tile would remove is one the per-pixel `len2 > r2` test already rejects, and
  the census puts that at 8.36 % of pairs. Stages 2–4 cost 25.7 instructions
  per pair, so the whole prize is **≈0.012 Gi/f**. The 21.5 % killed by N·L
  and the 10.7 % killed by the cone are genuinely per-pixel and no tiling
  reaches them.

### WHAT IS LEFT, RANKED, ON THE NEW TIP (`DeferredLighting-call` = 1.877)

1. **The cube tap, ~0.60 Gi/f = 32 % of the call.** The interior is still
   closed — three independent measurements say any new predicate in it costs
   more than it removes — and this round only removed the FRAME around it.
   The remaining lever is still "called less". The 8×8 pyramid is in; 4×4 is
   priced above and refused; what has NOT been tried is making the tap
   8-wide over PIXELS for a fixed light (the projection prologue — 3×3 matmul,
   two frustum rejects, `1/lz`, two muls — is the majority of the tap and is
   pure SIMD), which means restructuring the loop from pixel-major to
   light-major. Large, and the per-pixel early-outs fight it.
2. **`lighting-w2`, 0.293 Gi/f = 16 % of the call, and still barely touched.**
   455 instructions per filled pixel to average two neighbours. Untried and
   bit-exact: `fetchTexel` re-resolves the neighbour's Material and Texture
   although `neighborCompatible` has already proved `mID == matIDc`, i.e. the
   neighbour's material IS the centre's — only `Mipmap[mip]` and the swizzled
   UV differ. Unmeasured: how much of the 0.293 is the **full-shade fallback**
   (`--deferred_checker_edge_full` is default OFF, so every material/normal/
   depth edge is re-shaded by wave 2's reduced kernel, light loop and all).
3. **The specular GGX lobe, 0.161 Gi/f (8.6 %).** The invariants are already
   hoisted by the compiler; what is left is D, Gl, the Schlick quintic and two
   divisions that genuinely vary per light.
4. **View pos + SH ambient, 0.104 Gi/f (5.5 %).** `shEvalIrradiance` is 36
   flops plus a 9-mul/6-add world-normal transform per pixel. Rotating the L2
   SH coefficients into VIEW space once per frame would delete the transform
   (~15 flops/px) — but SH rotation is a re-association, so it moves bytes.
   **JUDGE-CALL, not free.**
5. **Normal decode + TBN, 0.100 Gi/f (5.3 %).** Two `fast_rsqrt`, a
   Gram-Schmidt, a cross and a normalise per normal-mapped pixel. No slack
   found; it is already the cheap form.

### METHOD NOTE THAT GENERALISES — read a PIX_ABLATE delta as an upper bound

The pixel ladder priced the OFF relief-horizon record at 0.034 Gi/f; removing
it recovered 0.005. A staged `continue` prices "everything above the cut", so
for a stage whose outputs are consumed only INSIDE a later loop the cut forces
them to be materialised at one point while the shipping build lets the
register allocator spread them across the loop — sometimes rematerialising a
constant instead of keeping it live. Stages whose outputs are consumed
immediately (albedo, mat32 decode, the pixel-loop floor) do not have this
problem. The FIRST build of this ladder had a worse version of the same
disease — per-stage sinks let the compiler dead-strip earlier stages, and
stages 9/10 measured LESS than stage 8. The shipped ladder uses a cumulative
sink; if a future stage delta looks too good, check which of the two it is.

## 2026-08-16e — 00b ROW 4 (`waterWaveSlope`, ~5.4 %): 8-wide, BYTE-NULL, and three contraction traps on the way

**Result at city t=1961 / 2400 / 400, 1512x848, his acceptance arm: frame min
47.51 -> 46.22 (-2.7 %), 30.73 -> 29.59 (-3.7 %), 35.49 -> 33.66 (-5.2 %);
TOTL -1.76 / -1.14 / -1.80 ms. `water-ripple` Ginstr/f 0.219 -> 0.148 (-32.4 %),
wall 1.851 -> 1.142; `water-glints` 0.229 -> 0.203 (-11.4 %) with Gcyc/f
0.082 -> 0.054 (-34 %), wall 2.423 -> 1.590. `renderFrame` instructions FLAT —
both passes run outside it. BYTE-NULL at every pin, `render_gate.sh` 4/4.
Flag `--water_slope_vec8` (default ON). Full map in `docs/PERF_STATE.md` 00b.**

Measured on `e99f5fed` in `/Users/gil-ad/work/rev-wave8`, three arms in one
worktree (parent binary / `--no-water_slope_vec8` / default), min-of-11 over 12
interleaved rounds with r0 dropped and the arm order rotated per round.

### WHAT SHIPPED

**`waterWaveSlope8` + batched consumers.** Both screen passes now collect their
LIVE pixels — the ones that survive the ray-cast (and, for glints, the opaque-Z
reject): 81.4 % and 44.6 % of the scan respectively, per `[WCENSUS]` — and flush
in eights. `pwater::waterWaveSlope` is **132 instructions for one pixel**;
`waterWaveSlope8` is **293 for eight** (36.6/px). The headroom was never 8x:
clang had already SLP-vectorized the scalar **2-wide** (the two scroll layers in
the two lanes of `.2s` vectors), so 4x was the ceiling and 3.6x is taken. The
129-stride halo from 2026-08-16b is what makes the gather cheap — (i0, i0+1) are
contiguous and the components interleaved, so a row's corner pair is ONE 128-bit
load; the tap is 32 quad-loads + 4 transposes for eight lanes.

### THE BYTE STORY IS THE FINDING, NOT THE SPEEDUP

The round expected the fma hazard `waterWaveSlopeVaried`'s comment warns about.
It found three, none of them in the arithmetic being changed:

1. **simde's `_mm256_fnmadd_ps` is not an fma on arm64** — outside
   `SIMDE_X86_FMA_NATIVE` it is a plain `-(a*b)+c` loop body in a header, fused
   only by the build's `-ffp-contract=fast`. And `#pragma clang fp contract` is
   LEXICALLY scoped, so `FP_CONTRACT_OFF` at the call site reaches neither
   simde's fma nor its mul/add. The first cut wrapped the function in
   `FP_CONTRACT_OFF` and the bilinear still came out `fmla.4s`: **84 % of 2.6 M
   taps disagreed with the scalar, some by 6e-4 — a whole neighbouring texel**,
   because a 1-ULP `fu` crosses a texel boundary. Rewritten with clang
   `ext_vector_type` operators in our own file plus `__builtin_elementwise_fma`
   at the sites the scalar fuses: **0 mismatches in 2 612 962 taps.**
2. **LICM hoisted an invariant square out of the specular tail.** `Vy = ey - waterY`
   is loop-invariant; once the slope moved to a batch flush, clang pre-computed
   `Vy*Vy`, which ROUNDS it where the pre-batch code left it unrounded inside an
   fma. Same algebra, swapped rounding, **143 px at 1 LSB**.
3. **The caustic tap's final lerp re-contracted the other way** in the outlined
   flush. Fixed with `sampleWaterCellGlints`, a pinned copy used only by
   `RenderGlints` so the shared sampler stays exactly as chase and the env-bake
   compiled it.

Carry this: a vectorization on this tree is byte-null when it is written where
the pragma can REACH, and the pins move for optimizer decisions **around** the
edit at least as often as for the edit.

### THE `OFF` COLUMN EARNED ITS PLACE

Batching alone (flag off, eight scalar calls per flush) **costs** instructions —
`water-ripple` 0.219 -> 0.243 Ginstr/f, the batch bookkeeping — while **winning**
wall, 1.851 -> 1.577 ms, because eight independent calls overlap in the
out-of-order window where the pre-batch loop interleaved each with its own
branchy tail. The vector form takes 0.243 -> 0.148 on top. A par-vs-default
comparison alone would have credited the restructure's wall win to the vector
code.

### WHAT IS LEFT HERE

* **The `--env_live_water` tilt is still scalar** — the third consumer of the
  field. It reaches it through `fds::g_envLiveWater.slopeFn`, a function POINTER
  called per glass pixel inside the deferred kernel's per-lane loop, so batching
  it means restructuring `Render_DeferredLighting_Tile_OuterVec`'s lane walk.
  That is the 0.041 Ginstr/f `--env_live_water` adds to `lighting-w1` at t=1961;
  `lighting-w1` is unmoved by this round.
* **`water-glints`' residue is the `powf` lobe and the caustic tap**, not the
  slope: its instruction count fell only 11 % while its cycles fell 34 %.
* **The ripple pass's own ray-cast** is ~30 instructions/px over the full
  1 282 176-pixel scan (0.038 Ginstr/f). Vectorizable in principle, but it feeds
  an `int()` truncation into the dispMap index, so it is a byte-risk item with a
  small prize.

### NOTE ON THE RECORDED CITY PINS — CLOSED 2026-08-16f, THERE WAS NO DRIFT

~~`4031ceec` / `925ecd43` no longer reproduce; the drift is city-specific and
pre-existing; whoever owns the city series should re-pin.~~ **No commit moved
them.** Both reproduce byte-exactly at tip `eb5e57d9`, 3/3 each, under the recipe
they were recorded with — i.e. **without `--profiler=0`**. `bd4ffbf8` /
`4cb8d2ca` is the same binary WITH that flag: `RunCitySnapshot` never silenced
the `ProfilerEnable 1` overlay `rev.cfg` seeds, so the older pins carry 3 718 px
of HUD text (0.18 % of the frame, all of it in a 114×221 corner block, zero scene
pixels). Fixed in `DEMO/Snapshot.cpp`; **current city pins `bd4ffbf8` /
`4cb8d2ca`**, now insensitive to the flag. See the 2026-08-16f block at the top
of `docs/SESSION_STATE.md`.

## 2026-08-16c — 00b ROW 3 (`FrustumClipper::Render`, 6.2 %): the tile cull was INERT on the mirror pass

**Result: `gbuffer` at city t=1961 (his arm) `0.658 -> 0.444 Ginstr/f` (-32.5 %),
thrsum `79.3 -> 43.1` core-ms, wall `8.69 -> 6.86`; `renderFrame`
`4.383 -> 4.174 Ginstr/f` (-4.8 %), `1.196 -> 1.115 Gcyc/f`. With the arm order
ROTATED per round (13 rounds, r0 dropped) the wall reads **frame min
`50.61 -> 47.74` (-5.7 %), frame mean `57.71 -> 54.24` (-6.0 %), `renderFrame`
`40.47 -> 37.96` (-6.2 %), `gbuffer` `8.571 -> 6.023` (-29.7 %)**.
**chase t=800 gains
more than city**: `gbuffer` `0.597 -> 0.334 Ginstr/f` (-44.1 %), thrsum
`60.5 -> 26.1` core-ms (-56.9 %), `renderFrame` -6.8 % instructions / -8.8 %
cycles / -6.1 % wall. greets and fountain have no mirror transform and are
NEUTRAL (they get only the binning half: -1.0 % / -5.9 % of `gbuffer`
instructions, frame-level inside the noise). BYTE-NULL — all nine pins unmoved
2/2, `render_gate.sh` 4/4. Two commits: `c26c1c35` + `d9dfa527`.
Full map in `docs/PERF_STATE.md` 00c.**

### THE ROW ITSELF, SAMPLED

`sample <pid> 25` on the running bench, city t=1961, his arm, shares of DEMO
self samples. The parent column reproduces 00b's 6.2 % at 6.46 %:

| symbol | parent | tip |
|---|--:|--:|
| **`FrustumClipper::Render`** | **6.46 %** | **1.21 %** |
| `RenderInnerMekalele` (the per-tile walk) | 2.53 % | 0.16 % |
| `fds::FaceTileBins_Build` + dispatch | — | 0.10 % |
| **together** | **8.99 %** | **1.47 %** |
| `Render_VolumetricCones_Tile` (control) | 35 850 samples | 36 843 |
| `apply_exact<false>` (control) | 8 749 samples | 8 453 |

### THE ROW'S STATED MECHANISM WAS ONLY HALF RIGHT, AND THAT IS THE FINDING

00b row 3 read "every raster tile re-walks the whole face list: 30 tiles x 2
passes x 10 215 faces". The walk is real — it is the `RenderInnerMekalele`
2.53 % row above, and a probe that ran four EXTRA reject-only walks per tile
priced one whole walk at ~4.5 core-ms/frame — but it is a SEPARATE symbol from
the 6.2 % the row was hung on, and it is 39 % of its size.

What was actually happening is that `--tile_bbox_cull` (default ON since the
S2/B5 work) **never fired on the mirror pass at all**. `Transform_Objects`
stamps each pushed face's screen bbox into its `FListEntry`; the two
hand-written mirror transforms that build FList themselves — `Reflected_Transform`
in `DEMO/CITY.CPP` and `DEMO/CHASE.CPP`, for the water-reflection underlay —
pushed `*Ins++ = { F->SortZ.DW, F };`, a two-field aggregate that leaves the
remaining members on their default member initialisers, i.e. the COVER-ALL
sentinel. Every entry, every frame. A new throwaway `[BBOXCENSUS]` probe at
city t=1961 (1512x848, 6x5 tiles):

| pass of the frame | cover-all | avg tiles/face | (face, tile) pairs |
|---|--:|--:|--:|
| main (`Transform_Objects`) | 1.2 % | 1.45 | 29 671 |
| mirror (`Reflected_Transform`) | **100 %** | **30.00** | **621 180** |

Same geometry, **21x the clipper entries**, and city runs that pass every frame.
That is the 6.2 %.

### WHAT SHIPPED

**1. `c26c1c35` — stamp the bbox on the mirror pushes. BYTE-NULL, the whole
prize.** Body extracted verbatim into `FDS/Base/FaceBBox.h`
(`fds::FaceBBox_Stamp`) and called from both `Reflected_Transform`s.
`Transform.cpp` deliberately keeps its own inline copy — its per-mesh face loop
is the one documented there as pin-fragile under `-ffp-contract=fast` + LTO, and
the perf case is entirely on the mirror side. Byte-null follows from the S2
contract, unchanged: the box is a conservative superset of the un-clipped
triangle and the clipper only SHRINKS coverage, so a box that misses a tile
means zero output there; near-plane straddlers keep the sentinel.

**2. `d9dfa527` — `--face_tile_bin` (default ON). BYTE-NULL, the smaller half.**
`renderFrame` walks FList twice (count, then scatter) and hands each tile a
dense, sequential `Face*` run; no tile walks the list. Order inside a tile is
preserved EXACTLY (contiguous chunk split + chunk-major prefix sum), which
matters because the tile kernels are not order-independent — verified
element-by-element by a throwaway `FDS_BINVERIFY` build across city
t=1961/2400/400, greets t=2845/5743/6097 and fountain t=2500, 0 mismatches.
Skipped below 3 000 faces (greets' offscreen RTT passes). New `face-bin`
`--deferred_prof` row prices the build at **0.200 ms/frame** at city t=1961;
`--mem_census` adds two `raster/` rows, **403 KiB arena + 113 KiB scratch**
there. On its own: `gbuffer` wall -8.1 % city / -6.8 % greets, thrsum -11.7 % /
-15.3 %.

### WHAT IS LEFT HERE

* **The finer raster grid is now UNBLOCKED.** §00 row 9 refuted a 12x10 grid at
  +139 % instructions *because each clipper tile re-walked the whole face list* —
  that traversal is gone. Worth re-running on chase (`gbuffer` `effPar` 5.5 of
  12). Caveat: it moves tile boundaries, so it will NOT be byte-null.
* **~~`FrustumClipper::Render`'s residue is the per-(face, tile) clip itself —
  three 140-byte `Vertex` copies, the UV/UZ stamp, and `MiplevelClipper`'s
  subdivision, redone for each tile a face straddles (now 1.45 on average
  instead of 30). Cutting it further means cutting the copy, not the
  traversal.~~ DONE 2026-08-16p — PRICED AND CLOSED BELOW BAR, and the last
  sentence was wrong by 10-40x.** The copies are **2.6 %** of the clipper
  symbol (0.033 % of frame at city t=1961, 0.35 % worst case at greets t=5743);
  copies + `MiplevelClipper` = **0.25 / 0.25 / 0.48 % of frame** at city t=1961 /
  chase t=800 / greets t=5743. Copy elision is structurally impossible (the
  clipper mutates the copies per-face AND per-tile, from 12 workers at once);
  a 63 %-payload-cut ceiling probe moves `renderFrame` instructions by less than
  one printed LSB, exactly as the disassembly predicts (0.027 %). Instrument
  landed: `--clip_stats`. Full evidence `docs/PERF_STATE.md` **00e**; the row
  that replaces it is the SHADOW raster — backlog **2026-08-16p**.

## 2026-08-16b — CITY UNDER HIS ACCEPTANCE ARM (`--env_live_water --deferred --city-env-pixel`): re-ranked, and the biggest row in it had no instrument

**Result at city t=1961, 1512x848, his exact command: frame min 54.72 -> 49.76 ms
(-4.96, -9.1 %), frame mean 59.62 -> 54.20 ms (-5.42), `renderFrame` instructions
4.409 -> 4.387 Ginstr/f. Four landings, THREE BIT-EXACT; one judge call at 4 px of
2 073 600 at |delta| = 1/255. `render_gate.sh` 4/4 PASS; chase x5, greets t1588,
fountain t2500 unmoved throughout. Full map in `docs/PERF_STATE.md` 00b.**

Measured on `44c8aeed` in `/Users/gil-ad/work/rev-cityarm`, two binaries in one
worktree against one asset tree, `SDL_VIDEODRIVER=dummy`, 12 pool workers,
min-of-10 over 11 interleaved rounds with round 0 dropped, arm order rotating.
Loads 12-39 across the session, so `Ginstr/f` is the column that decides.

### What the arm inverts (both new)

* **`--city_env_pixel` makes city FASTER, not slower.** Clean single runs: frame
  min **77.11 ms** plain `--deferred` vs **59.22 ms** with the flag. It moves the
  glass into the deferred kernel (`lighting-w1` 0.715 -> 0.966 Ginstr/f) and
  deletes a forward per-vertex reflective path that cost more. Any city figure
  quoted against plain `--deferred` is quoting a slower frame than he runs.
* **`--env_live_water` is not free per pixel.** 2026-08-13b's "free at the noise
  floor" was the FORWARD paraboloid path (per vertex). Through the deferred
  compose it is **+0.041 / +0.015 / +0.047 Ginstr/f of `lighting-w1`** at
  t=1961 / 2400 / 400 = +4.4 / +3.8 / +6.3 % of the phase, ~0.74 ms at t=1961,
  and the cost is the wave-slope call, not the mask read.

### WHAT SHIPPED

**1. `LGHT`: the fan's unit was a MESH — BYTE-NULL, -4.9 ms.**
`LGHT` runs outside `renderFrame` so it has no `--deferred_prof` row and no round
had opened it. It reads **5.90 ms p50** at t=1961 for **~9 CORE-ms** of work
(`LightMeshVerts` = 1.67 % of DEMO self time in a frame-dominated profile):
`effPar` ~1.5 of 12, a LOAD-IMBALANCE row, not a compute row. City is 56 visible
meshes of which one is the scene-sized building, and a mesh-granular fan's wall
IS the largest mesh — a work-stealing cursor cannot split a task. `LightMeshVerts`
now takes a vertex RANGE; the unit is a 1 024-vertex chunk, and the per-range
prologue (material, ambient, omni candidate list) is a loop over the scene's
omnis, ~3 k instructions against ~800 per vertex, i.e. under 1 % at that size.

| pose | `LGHT` p50 |
|---|---|
| t=1961 | **5.904 -> 0.974 ms** |
| t=2400 | 0.189 -> 0.164 |
| t=400  | 0.281 -> 0.266 |

t=2400 and t=400 barely move **and that is the check**: those poses do not have
the big mesh flagged visible, so there was no imbalance to fix. Instructions are
unchanged everywhere — this buys wall, not work, which is what a parallelism fix
must look like. Also adds `--prof_no_vertex_light` (default off, byte-null), the
ceiling instrument the row lacked; MEASUREMENT ONLY, it changes pixels, because
the forward `TheOtherBarry` filler reads `Vertex::L{R,G,B}` for transparents,
water, sprites and the forward mirror-pass glass.

**2. The water field's modulo — JUDGE CALL, 4 px at |delta| = 1/255.**
`sampleWaterNrm`'s `(i0 + 1) % WNRM` compiled to add/and/negs/and/csneg — five
instructions, because clang cannot fold a `%` on an `int` it cannot prove
non-negative — **four times per `waterWaveSlope` call** (two axes x two scroll
layers). That function is the 4th-hottest symbol of this frame (6.2 % of DEMO
self time) and is shared by the reflection ripple, the glints AND
`--env_live_water`'s tilt. A 129-stride HALO (last row/column = verbatim copy of
the first, written after the normalize so each halo texel is bit-identical to
its source) removes the modulo and makes the (i0, i0+1) pair contiguous:
**161 -> 132 instructions**.

| pass | t=1961 | t=2400 | t=400 |
|---|---|---|---|
| `water-ripple` Ginstr/f | 0.252 -> 0.221 (-12.3 %) | 0.205 -> 0.181 (-11.7 %) | 0.295 -> 0.259 (-12.2 %) |
| `water-glints` Ginstr/f | 0.248 -> 0.230 (-7.3 %) | 0.196 -> 0.182 (-7.1 %) | 0.258 -> 0.241 (-6.6 %) |

The 4 moved pixels are the `-ffp-contract=fast` + LTO re-contraction hazard
`waterWaveSlopeVaried`'s own comment warns about ("touching that reshapes its
fmadd chain -> city moves"), not an arithmetic change: same fetched bits, same
expression, different fma grouping. Smaller than the caustic-sum-plane judge call
that shipped 2026-08-15e (7 px, also all |delta| = 1). **NEW CITY PINS**:
`--deferred` `3413028bc70b99f4bc3ee9eec9de7c14` -> `4031ceec1a1090372575c4f9c39e2839`;
his arm `1986f2df63a4585c5d05082f36f8722c` -> `925ecd43f45d8f0574acc9c9a5a958a1`.

> **SUPERSEDED 2026-08-16f — these four hashes are all HUD-BEARING.** They were
> taken before `RunCitySnapshot` silenced the `rev.cfg` profiler overlay, so each
> carries the same 3 718 px of corner text. The 4-px A/B above is a *differential*
> between two binaries measured under the same recipe, so the HUD is common-mode
> and the result stands. Current, HUD-free: `--deferred`
> **`bd4ffbf87d1492175a9b6c1111fb3f5f`**, his arm
> **`4cb8d2ca68b72f8a24627f42077eef25`**.

**3. `--env_live_water` projected the same direction twice — BIT-EXACT.**
`EnvLiveWater_Weight` opens with `EnvCube_DirToFaceUV` on the UNPERTURBED
direction (correctly — a perturbed mask read would be circular) and the cube
colour fetch repeats the identical call one line later: two dominant-axis selects
and two divides for one projection, on every glass pixel, in the path his arm
runs. Project once, hand face/u/v to the mask, re-project only when the tilt
fires. `lighting-w1` -0.6 % at t=1961, -1.1 % at t=400. The weight helper is
mirrored into `DeferredSurfaceKernel.cpp` rather than refactored in `EnvBake.h`
because that header's mask machinery is under concurrent change (it tracks
`868ba5d8`'s one-bit `EnvLiveWater_MaskBit` form; keep the two in step).

**4. The froxel composite's per-pixel `logf` — BIT-EXACT.**
`Froxel_CompositeTileVec8` punts a whole 8-lane group to the scalar
`Froxel_CompositePixel` for any group containing a water-reflection lane — most
of city's lower half — and that function recomputed
`1.0f / std::log(gFrFar / gFrNear)` and `1.0f / gFrNear` PER PIXEL. The tile
version hoists them because it is a loop; the per-pixel one cannot, and clang
will not hoist across the caller's loop either (the `dword*` VPage store may
alias a float global). Cached per frame from the same expression:
`fog-composite` -3.8 / -3.2 / -3.5 %, `fastfog` -1.8 / -1.8 / -2.3 %.

### PRICED, NOT LANDED

* **`BACKLOG_PLANS.md` 2e — a salted cache for the live-water bake.**
  `--env_live_water` bypasses `city_envmap_cache` by design (the mask is a
  product of the bake's depth buffer; a colour-only hit leaves the tilt silently
  inert), so his arm cold-bakes on every launch. Whole-process wall, `iters=5`,
  3 runs each, spread < 0.01 s: cache warm **0.98 s**, cold bake **2.18 s**, his
  arm **3.06 s**. So **+2.08 s per launch**, 1.20 s of it the cold bake and
  0.88 s the live-water re-shade. 2d/2e/2f are LOOK items in his own queue —
  priced here, not landed.

### WHAT IS LEFT, RANKED, WITH ITS MECHANISM

1. **`FrustumClipper::Render`, 6.2 % — the largest untouched row.** Every raster
   tile re-walks the whole face list: 30 tiles x 2 `renderFrame` passes x 10 215
   pushed faces a frame. A face->tile binning pass is the shape. §00 row 9's
   refuted "finer grid" measured this same disease from the other side (+139 %
   instructions, because each extra tile re-walked everything).
2. **8-wide `waterWaveSlope`, ~5 % of the frame after (2).** Two bilinear taps
   into a 129^2 map per live pixel in three consumers. Every operation maps 1:1
   to a vector form; the risk is fma contraction — this round's judge call came
   from exactly that for a far smaller edit — so budget a byte battery.
   **DONE 2026-08-16e, and BYTE-NULL: no judge call was needed.** The byte
   battery was the right call — it found THREE contraction traps, none in the
   arithmetic being changed (simde's fma is not an fma on arm64 and the pragma
   cannot reach it; LICM pre-rounding an invariant square; the caustic lerp
   re-contracting). See the 2026-08-16e entry at the top.
3. **`Froxel_GlowTile`'s `atanf`**: one libm call per (coarse column x light x
   slice) for the analytic inscatter integral. 0.3 % of self time, unpriced.
4. **`--env_live_water`'s remaining 0.041 Ginstr/f** IS the wave-slope call —
   but item 2 did **NOT** cover it, and that assumption was wrong. The tilt
   reaches the field through `fds::g_envLiveWater.slopeFn`, a function POINTER
   called per glass pixel inside the deferred kernel's per-lane loop; the 8-wide
   form needs a caller that can batch, which the two screen passes can and this
   lane walk cannot without restructuring `Render_DeferredLighting_Tile_OuterVec`.
   Still open after 2026-08-16e; `lighting-w1` is unmoved by that round.
5. **`gbuffer` `effPar` 8.5-8.7 of 12** under this arm. Better than the 5.5 §00
   recorded on chase, so the "half the pool is idle" framing does not carry to
   city. Not a lever at this size.

## 2026-08-16 — HIS ACCEPTANCE ARM: SSAO was 39 % of the frame and had never been profiled

**The arm:** `--deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace`,
greets, 1512x848. **Nothing in the round-1 map covers it** — no prior greets
round carried `--ssao --ssao_gtao`, and `--ssao_downscale` defaults to **1**, so
GTAO runs FULL RES (1.28 M cells) while every tuning note in
`docs/GRAPHICS_PIPELINE.md` 9 was written at quarter-res.

### The ranked map of this arm, measured (min-of-6 over 7 interleaved rounds, r0 dropped)

| item | ms @ t=5743 | Ginstr/f | share of `renderFrame` |
|---|--:|--:|--:|
| **`ssao`** | **19.4** | **2.143** | **39 %** |
| `DeferredLighting-call` (`w1` 14.4 + `w2` 2.1) | 17.6 | 2.040 | 35 % |
| `gbuffer` | 8.5 | 0.986 | 17 % |
| `shadow-bake` (outside `renderFrame`) | 2.5 | 0.207 | — |
| `RTT` | 2.1 | 0.024 | — |
| `bloom-chain` | 1.19 | 0.136 | 2 % |
| `cones` | 0.73 | 0.063 | 1 % |
| `tonemap-post` | 0.45 | 0.037 | 1 % |
| `mirror-grid` | 0.45 | — | 1 % |

`ssao`'s `Ginstr/f` is **2.14–2.16 at all five poses** — it is a fixed full-res
cost, independent of what the camera sees, which no other phase in this engine
is. Ablation at t=5743, same batch: `--no-ssao` frame 58.26 -> **39.33**;
`--no-ssao_gtao` (hemisphere producer) `ssao` 19.4 -> 12.8; dropping
`--texture_filter=2` `gbuffer` 8.50 -> 7.07 (**tf=2 costs 1.4 ms**, all of it in
the raster); `--no-bloom` 1.19 ms; dropping `--greets_displace` frame -5.1 ms
(his approved look, not a target).

### DONE — four rungs, -23 % instructions / -25 % cycles on the pass

Commits `c78d536f` (slice-trig table + 8-wide denoise, BYTE-NULL),
`f5779342` (vector tail, BYTE-NULL), `289e92c7` (sector units, judge call),
`b0905ee1` (apply `vld4_f16`/`vst4_f16`, BYTE-NULL).

Parent `f25bb992` run with the four corner flags EXPLICIT vs the tip run with
the bare umbrella, both binaries built in one worktree against one asset tree,
interleaved min-of-8, r0 dropped, load 23–37:

| pose | `ssao` Ginstr/f | `ssao` Gcyc/f | `ssao` wall ms | frame min ms |
|---|--:|--:|--:|--:|
| t=2845 | 2.150 -> 1.654 | 0.649 -> 0.488 | 19.55 -> 15.06 | 59.53 -> 59.05 |
| t=3409 | 2.141 -> 1.653 | 0.637 -> 0.487 | 22.27 -> 16.28 | 68.45 -> 65.54 |
| t=5743 | 2.143 -> 1.652 | 0.651 -> 0.492 | 23.55 -> 16.36 | 77.02 -> 71.29 |
| t=5813 | 2.149 -> 1.650 | 0.655 -> 0.483 | 20.18 -> 14.26 | 58.30 -> 52.54 |
| t=6097 | 2.155 -> 1.653 | 0.647 -> 0.487 | 19.81 -> 14.14 | 50.17 -> 44.41 |

Whole-frame: `renderFrame` **Ginstr/f -9.0 to -10.2 %, Gcyc/f -9.3 to -10.5 %**.
Image cost of all four rungs together: **343 px of 24 883 200 (0.0014 %) over 12
deterministic greets poses, every one at max |delta| = 1/255** — all of it from
the sector-units rung.

### THE BIG LEVER LEFT IS A DIAL, NOT A REFACTOR — `--ssao_downscale`

> **RESOLVED 2026-08-16y — COUNTERSIGNED AND LANDED AS THE DEFAULT.** Gil-Ad,
> verbatim: *"ssao downscale 2 is ok (no downscale looks much better, but too
> slow)"* — full-res preferred aesthetically, rejected on cost.
> `FDS/Base/FeatureFlags.def` `ssao_downscale` **1 → 2**; `--ssao_downscale=1`
> still restores full-res EXACTLY (proved both directions: the flipped binary
> under `=1` reproduces the four old greets acceptance hashes 3/3, and the
> parent under `=2` reproduces the four new ones 3/3 — a bijection, so nothing
> but the default moved). **Realized, min-of-11 interleaved, 1512×848, his
> acceptance arm: frame 49.25 → 39.62 ms at t=5743 and 41.39 → 31.86 ms at
> t=6097 (−9.63 / −9.53 ms), `ssao` 14.55 → 4.96 and 14.23 → 4.93 ms, `ssao`
> Ginstr/f 1.650 → 0.603, `renderFrame` Ginstr/f −22.3 % / −25.0 %** — i.e. the
> −9.1 ms projected in the table below was slightly conservative. Image cost
> re-measured on the landing binary at t=5743: 52.5 % of pixels moved, mean |Δ|
> 0.869/255 over channels on the moved, max 74 — reproducing the numbers below
> to the digit. New pins, gates and look evidence
> (`docs/img/ssaoperf/default_flip_t5743_crop_d1_d2.png`):
> `docs/SESSION_STATE.md` 2026-08-16y.
>
> **What this closes and what it does not.** It closes the "biggest lever on
> this arm" line that three consecutive rounds handed forward. It does NOT make
> the three remaining SSAO items below more valuable — the opposite: the compute
> and the denoise both shrink quadratically with the dial, so the slice-setup
> and `sqrt` items are now worth ~1/4 of what this round priced them at, and
> `DeferredLighting-call` (1.73 Ginstr/f against `ssao`'s 0.603) is row 1 of the
> arm again by a wide margin.

Measured on the tip, same protocol, load 15–29. This was recorded as a **LOOK
change** and an OPTION when written; it has since been countersigned and is the
default (note above):

| arm | `ssao` ms t=5743 / t=6097 | `ssao` Ginstr/f | frame min t=5743 / t=6097 |
|---|--:|--:|--:|
| `--ssao_downscale=1` (~~default, what he runs~~ **NOW THE OPT-IN CONTROL ARM**) | 14.33 / 14.61 | 1.651 | 53.92 / 44.93 |
| **`--ssao_downscale=2`** (**THE DEFAULT since 2026-08-16y**) | **4.95 / 4.97** | **0.604** | **44.81 / 35.08** |
| `--ssao_downscale=4` | 2.19 / 2.17 | 0.291 | 42.15 / 32.22 |
| `--ssao_gtao_steps=2` | 10.37 / 10.26 | 1.137 | 50.74 / 40.67 |
| `--ssao_gtao_slices=1` | 8.89 / 8.77 | 1.016 | 48.80 / 39.34 |

**`=2` is -9.4 ms of `ssao` and -9.1 ms of the FRAME** — larger than everything
the four rungs above took together. What it costs, measured against `d=1` over
four poses: 44–63 % of pixels move, but **mean |delta| on the moved pixels is
0.53–0.87 / 255** and the peaks are local to contact creases (max 20 at t=2845,
74 at t=5743; `d=4` reaches 102–109). Side-by-side crops:
`docs/img/ssaoperf/dial_t{5743,6097,2845,6001}_crop_d1_d2_d4.png`, full frames
`dial_t*_full_d1_d2_d4.jpg`. ~~**Needs his eye before it goes near a default.**~~
**IT GOT HIS EYE — countersigned 2026-08-16, landed as the default the same day
(note at the top of this section).**

### What is left inside SSAO, ranked, after the four rungs

* **The per-lane scalar slice setup** — `gtaoRow8` still runs the cross
  products, `fast_rsqrt` and `atan2_approx` **per lane per slice** (8 x slices
  scalar setups per vector group). Estimated ~20 % of the compute. Vectorising
  it is bit-exact only if `atan2_approx`'s branches become selects over the same
  polynomial; not attempted.
* **The two `_mm256_sqrt_ps` inside `gtaoAcos_x8`** are now the only
  non-pipelined FP ops left in the sample chain. Replacing them with
  `rsqrt`-and-multiply is NOT byte-safe (the Eberly fit's output is quantised to
  1/32 sectors; a 12-bit reciprocal there flips boundaries at ~0.3 % of samples,
  three orders of magnitude more than the sector-units rung moved).
* **The denoise at `down==1` is a 4x4 box over the FULL-RES plane** — 16 taps a
  pixel. Under `--ssao_downscale>1` it shrinks quadratically along with
  everything else, which is another reason the dial is the lever.

## 2026-08-15 — round-1 item #1, the deferred OMNI LOOP: the shadow chain is 74 % of it, not 24.5 %, and 99.5 % of the 2-D-shadow calls compute the constant 1.0f

**Result: greets t=5743 `lighting-w1` 26.00 -> 24.26 ms (-1.75 ms), `renderFrame` 41.87 -> 40.11 ms, frame min 48.33 -> 46.55 ms; instructions -9.0 % of `lighting-w1`, -6.0 % of `renderFrame`. chase t=800 `lighting-w1` -22.5 % instructions / -1.05 ms. BIT-EXACT: all eight pins unmoved 3/3, `render_gate` 4/4 PASS. No flag — there is nothing to A/B, the skipped calls returned 1.0f.**

### The instrument: a committed staged-continue ladder for the omni loop

`-DFDS_OMNI_ABLATE=n` in `DeferredSurfaceKernel.cpp` — the `FDS_CONE_ABLATE`
shape aimed at the other ~100 %-self monolith. Twelve stages, each `continue`ing
at a natural statement group of the per-light body, each sinking what it retains
into a per-tile accumulator that drains to one volatile store per tile call.
Driver `scratchpad/omni_ablate.sh`, report `scratchpad/omni_ladder.py`.
**Two independent build+measure sessions agree to <= 0.10 % on every row at both
poses.** The shipping build (stage 0, census compiled out) measures
`lighting-w1` 3.247 Gi/f against the parent's 3.247 at t=5743 and 1.916 vs 1.917
at t=3122 — the scaffolding costs nothing.

Stage 1 (the loop deleted entirely) is the kernel's non-light remainder and
independently reproduces round 1's `--prof_no_lights` figure: 0.761 Gi/f vs the
recorded 0.801. **The omni loop proper is full minus stage 1 = 2.486 Gi/f at
t=5743**, against the 2.490 the round-1 map carries. The instrument agrees with
the map it was built to subdivide, to 0.16 %.

### The split (Ginstr/f, session A; session B in docs/SESSION_STATE.md)

| stage | t=5743 dGi | % of loop | t=3122 (his) dGi | % of loop |
|---|--:|--:|--:|--:|
| mirrorId test | 0.043 | 1.7 % | 0.039 | 2.7 % |
| w, N.L dot, dot<0 reject | 0.112 | 4.5 % | 0.045 | 3.1 % |
| len2 + range reject | 0.060 | 2.4 % | 0.030 | 2.1 % |
| bounce-window portal | 0.030 | 1.2 % | 0.025 | 1.7 % |
| rsqrt / dist / attenuation k | 0.051 | 2.1 % | 0.027 | 1.9 % |
| spot cone + smoothstep | 0.006 | 0.2 % | 0.002 | 0.1 % |
| **computeMapShadowAtten** | **0.405** | **16.3 %** | **0.706** | **48.9 %** |
| **resolveCubeAtten (cube tap)** | **1.422** | **57.2 %** | **0.272** | **18.8 %** |
| relief horizon | 0.038 | 1.5 % | 0.025 | 1.7 % |
| diffuse accumulate | 0.055 | 2.2 % | 0.044 | 3.0 % |
| **specular lobe** | **0.264** | **10.6 %** | **0.229** | **15.9 %** |
| loop total | 2.486 | 100 % | 1.444 | 100 % |

**The shadow chain is 73.5 % of the omni loop at t=5743 and 67.7 % at his
pose.** The round-1 map's "the tap chain is 24.5 % of the lighting stage" is
superseded: against the same `lighting-w1` denominator the cube tap alone is
43.8 % at t=5743. The old figure came from `--prof_no_cube_tap`, which
short-circuits `resolveCubeAtten` to **1.0f = fully lit** — so every pair whose
tap would have returned 0 stops taking the `continue` and pays diffuse **and**
specular instead. `--omni_census` prices that compensation exactly: 54.6 % of
the 4.74 M taps come back fully shadowed, i.e. 2.59 M pairs a frame that the
no-tap arm shades and the real kernel does not, at ~148 instructions each =
0.385 Gi of the 1.42 Gi gap. An ablation that makes a light BRIGHTER cannot
price the thing it removed.

### The other new instrument: `--omni_census` (per PIXEL x light, `-DFDS_OMNI_CENSUS=ON`)

The tap census (43ac3456) counts per (tile x light) and structurally cannot say
how many lights a PIXEL accumulates. This does. Deterministic frame to frame.

| | t=5743 @1920x1080 | his pose t=3122 @1512x848 |
|---|--:|--:|
| shaded px | 1.044 M | 0.643 M |
| (px x light) pairs entered | 8.687 M | 7.164 M |
| lights/px entered | 8.32 | 11.14 |
| killed: mirrorId | 4.79 % | **44.28 %** |
| killed: N.L < 0 | 21.49 % | 10.32 % |
| killed: range | 8.36 % | 3.58 % |
| killed: spot cone | 10.68 % | 5.32 % |
| killed: 2-D map shadow | 0.12 % | 7.98 % |
| killed: **cube tap** | **29.77 %** | 0.02 % |
| **reach the accumulate** | **24.79 %** | **28.50 %** |
| **live lights per shaded px** | **2.06** | **3.17** |

Live-lights-per-pixel histogram, t=5743: 0 lights 6.71 %, 1 20.47 %, 2 39.50 %,
3 26.63 %, 4 6.55 %, >=5 0.15 %. His pose: 0 1.50 %, 1 5.97 %, 2 21.00 %,
3 17.31 %, **4 53.68 %**, >=5 0.54 %. **A pixel is shaded by two to four lights
and the loop walks eight to eleven to find them.** The two poses fail in
different places — his dies on mirrorId (the mirror clones), t=5743 dies on the
cube tap — so neither pose alone names the lever.

### WHAT SHIPPED: the 2-D-shadow call, skipped when it can only return 1.0f

`computeMapShadowAtten` reads exactly three index planes — the light's own 2-D
spot map, a mirror clone's SOURCE map, a mirror clone's SOURCE cube — and each
of its three bodies is guarded on that index being `>= 0`. With all three
negative it can only return its `1.0f` initialiser. It is nonetheless an
**out-of-line function with a 176-byte frame and ten callee-save `stp`/`ldp`
pairs**, invoked once per (pixel x light) that clears the cone test.

The census says **99.50 % of its 4.749 M calls a frame at t=5743 carry none of
the three** (his pose: 32.43 % of 2.615 M). That whole 0.405 Gi/f stage is a
constant, computed four and three-quarter million times a frame.

The guard is the function's own three `if`s hoisted to the call site, spelled as
one AND: `(smIdx & srcSm & srcCube) >= 0` is false iff every index is negative
(absent = -1, so bit 31 survives the AND only when all three have it). Bit-exact
by construction — it can only skip calls returning 1.0f, and 1.0f fails the
`<= 0.0f` test either way. The vec path needs no equivalent; it already tests the
same planes 8-wide (`anyShadow`) before its lane loop.

Measured (3 arms, one worktree, one asset tree, interleaved min over 7 rounds,
round 0 discarded, load 4.8-7.7):

| | greets t=5743 | greets t=3122 (his) | chase t=800 | city t=1961 | fountain t=1200 |
|---|--:|--:|--:|--:|--:|
| `lighting-w1` Gi/f | 3.246 -> **2.953** (-9.0 %) | 1.916 -> **1.872** (-2.3 %) | 0.738 -> **0.572** (-22.5 %) | 1.146 -> 1.145 | 0.335 -> 0.335 |
| `lighting-w1` ms | 26.00 -> **24.26** | 15.73 -> 15.53 | 5.07 -> **4.01** | 8.84 -> 8.73 | 2.39 -> 2.40 |
| `renderFrame` Gi/f | 4.858 -> **4.566** (-6.0 %) | 4.945 -> **4.901** (-0.9 %) | 4.087 -> **3.921** (-4.1 %) | 6.232 -> 6.230 | 1.580 -> 1.580 |
| `renderFrame` ms | 41.87 -> **40.11** | 37.80 -> **37.41** | 36.73 -> **35.35** | 53.98 -> 54.26 | 13.22 -> 13.13 |
| frame min ms | 48.33 -> **46.55** | 44.37 -> **43.82** | (snapshot) | 76.15 -> 76.59 | 15.42 -> 15.31 |

The frame-saving-equals-pass-saving check passes at t=5743: `lighting-w1` gives
back 1.75 ms, `renderFrame` 1.75 ms, the frame 1.78 ms. city and fountain are
neutral to 0.1 % on instructions (city runs the OuterVec kernel; fountain's whole
omni loop is 0.1 Gi). chase was not expected to move and moves the most in
relative terms — its lights carry cubes, not 2-D maps.

### REFUTED, WITH NUMBERS

**(1) Skip the dynamic shadow plane when it was never baked.** `--shadow_dynamic`
defaults 0 and greets never sets it, so `packDyn` is provably all-zero
(`assign(n, 0)` at rebuild; written only by the `DynamicMeshesPerFrame` bake,
reached only from the two `if (FeatureFlags::shadow_dynamic())` sites). With the
plane zero, PolyId's `closestPacked` collapses **algebraically** to
`ShadowTexId(psB[o])` and Depth's `closest` to `ShadowTexZ(psB[o])` — four loads,
four id extracts and eight compares per tap, deletable bit-exactly. Built it
(one `dynPlaneEmpty` bool through `resolveCubeAtten` into `CubeShadow_Sample`).
**It costs +12.4 % of `lighting-w1`'s instructions at t=5743 and +10.3 % at his
pose — worse than the parent** (3.318 vs 3.246 Gi/f, with lever A's -0.293
already inside it, so the bool alone is +0.365 Gi). Reverted. This is the THIRD
independent measurement of the same mechanism — the cube tap is at its
register-allocation limit and *any* new runtime predicate in its innermost body
costs more than the work it removes (cf. the tap census's +2.0 % hooks,
d9248f6d's `FDS_DEV` abort branch). **Treat "add a cheap test to the tap" as
refuted-by-default; the tap only gets cheaper by being CALLED LESS.**

**(2) Pass the pixel's world position into `computeMapShadowAtten`.** Both
mirror-clone branches recompute `wp = viewToWorld * (x,y,z) + cameraWorld` — 9
muls + 9 adds, per (pixel x light), for a per-PIXEL quantity the caller already
hoists as `sampleWorldX/Y/Z` and hands `resolveCubeAtten` one line later. 67.5 %
of the surviving calls at his pose take one of those branches, so the predicted
saving was ~0.027 Gi. **Measured 0.010 Gi — `lighting-w1` 1.872 -> 1.862 at his
pose (-0.53 %) and 2.954 -> 2.954 at t=5743, cycles flat-to-up.** The premise was
two-thirds wrong: the compiler was already CSE-ing most of it. Not worth a
signature change with a contraction risk against the pins. Reverted.

### WHAT REMAINS, AND ITS REGIME

After the change the omni loop is ~2.19 Gi/f at t=5743 and ~1.40 at his pose.

1. **The cube tap, 1.42 Gi/f = 65 % of what is left at t=5743.** 4.725 M taps at
   ~300 instructions each. `CubeShadow_Sample` is 368 static instructions with a
   144-byte frame and eight callee-save pairs; `resolveCubeAtten` wraps it in 332
   more with a 208-byte frame and eight more. Refutation (1) says the interior is
   closed to cheap edits. **The only lever left is fewer taps**, which is
   byte-moving — the parked 8x8 PolyId id-uniformity pyramid (43ac3456) is the
   specified form, and the census now gives it a denominator: 76.1 % of taps sit
   in an 8x8-uniform block.
2. **`computeMapShadowAtten` at HIS pose, ~0.66 Gi/f.** 1.77 M surviving calls,
   65 % of them the mirror-clone `srcCube` branch: world pos, mirror reflect,
   world->view round trip, then a full cube sample. The round trip exists only
   because `CubeShadow_Sample` wants view space while the reflection happens in
   world space; a world->light matrix would delete ~15 flops per call but is a
   re-association, not a hoist. **JUDGE-CALL, not free.**
   **UPDATE 2026-08-15c — the stage is now fully accounted, and the `srcCube`
   round trip is the ONLY thing left in it.** Of the 1.767 M surviving calls,
   1.703 M (96.4 %) are `srcCube` and already take the cube pyramid's 81.6 %
   skip; 0.063 M (3.6 %) are `srcSm`, a single NON-PCF *depth* compare that an
   *id* pyramid structurally cannot answer; and the light's own 2-D spot map is
   live in **0.02 %** of the stage's calls — about 520 a frame. Wiring the
   pyramid into that spot tap (done, byte-null, free — the spot pyramids were
   already being built) is worth **-0.003 Gi/f at t=1588 and 0.000 at his pose
   and t=5743**. Read the "48.9 % of the omni loop" row above as the STAGE, not
   the spot tap. Same reason `volSpotShadow` (`DeferredFastFog.cpp:344`), the
   volumetric reader of these same 2-D maps, is out of the pyramid's reach: it
   is a depth compare too.
3. **The specular lobe, 0.264 / 0.229 Gi/f (10.6 % / 15.9 %).** No
   transcendentals to remove: `--pbr` is off at greets, so the lobe is
   `pow_glossClass` = a bit-exact squaring chain for gloss in {48, 64}, ~6 fmuls.
   The cost is the half-vector, the `rsqrt`, and that it runs for every
   ACCUMULATED pair. **Nothing cheap here; it is already the cheap form.**
4. **The reject chain itself, ~0.3 Gi/f.** 8.32 lights entered per pixel to
   shade 2.06. Halving the list would save ~0.15 Gi — but the tile-sphere cull
   already took the geometric slack, and the remaining rejects are N.L (21.5 %)
   and cone (10.7 %), both genuinely per-pixel.

## 2026-08-14b — fountain's 77 % frame item, closed: the two-layer transparent kernel was scanning 198 M px a frame to shade 0.97 M, and 3 of its 4 peel passes rendered nothing

**Result: fountain t=1200 27.46 -> 15.47 ms frame min (-11.99 ms, -43.7 %), `TBR-render` 20.17 -> 8.17 ms (-59.5 %), instructions -55.0 % of `renderFrame`. Every gate byte-identical. Two flags, both default ON, both byte-null by construction.**

### What the profile called "two layers" is not two depth layers

`Render_DeferredTransparentLighting_Tile<0>` / `<1>` are `XparLayer::Front` /
`XparLayer::Back` — the front- and back-FACING transparent layers, not a depth
peel. The depth peel is a separate axis: `FOUNTAIN.CPP:1083` sets
`FntSc->XparPeelPasses = 4`, so each (clump, side) rasters and composites FOUR
times. Anyone attacking "one of the two layers" would have been attacking the
wrong structure.

### The census — `--xpar_extent_census`, fountain t=1200, 1920x1080

`RenderXparClumpInStrip` composites a *clump*: a run of consecutive
same-(mesh, frontFacing) transparent faces in one 8-row strip. It flushes on
every mesh/side change **and on every interleaved sprite**, and the fountain
spray is 33 358 sprites a frame. So:

| | value |
|---|--:|
| clump flushes / frame | **3 221** |
| composite invocations (flushes x 4 peel passes) | **12 884** |
| px a full-strip-width scan covers | **197.90 M** |
| px carrying a live transparent fragment | **0.97 M — 0.491 %** |

Each of those 12 884 invocations cleared and re-scanned all 1920x8 px of its
strip for a clump that typically spans a handful of 8-px tile columns. **The
per-strip dispatch bounds Y and only Y** — `Render_DeferredTransparentLighting_
Tile<Layer>(stripCtx, stripIdx, 0, strip_y, XRes, strip_y + strip_h)`. Nothing
in the pipeline bounded X, and nothing skipped an empty clump.

Per side and per peel pass, same pose:

| layer | calls | empty | live px | live / scanned |
|---|--:|--:|--:|--:|
| front peel0 | 1 665 | 12.3 % | 0.517 M | 60.3 % |
| front peel1 | 1 665 | **100.0 %** | 0.000 M | 0 % |
| front peel2 | 1 665 | **100.0 %** | 0.000 M | 0 % |
| front peel3 | 1 665 | **100.0 %** | 0.000 M | 0 % |
| back peel0 | 1 556 | 14.5 % | 0.454 M | 56.2 % |
| back peel1 | 1 556 | **99.9 %** | 0.000 M | 0 % |
| back peel2 | 1 556 | **100.0 %** | 0.000 M | 0 % |
| back peel3 | 1 556 | **100.0 %** | 0.000 M | 0 % |

**9 663 of 12 884 passes a frame produce nothing.** `--xpar-peel-passes` is a
per-SCENE constant, but a clump is a sprite-delimited run of faces, and those
almost never stack in depth. Exactly ONE clump in the frame has a real second
depth layer.

### Lever 1 — `--xpar_strip_extent` (default ON, byte-null by construction)

The rasterizer records the tile-column extent it touched (`meka::g_rasterXExtent`,
updated inside the `g_rasterStripClamp.tileYMax < INT32_MAX` branch that only
the TBR xpar strip path sets, so every opaque raster emits the code it emitted
before). The clump then clears and composites only those columns.

Byte-null argument, not measurement: outside the tracked range a strip's slice
is in its cleared state, so (a) the clear has nothing to clear there and (b) the
composite kernel's first per-pixel test is `mat32 == 0xFFFFFFFF -> continue`.
The extent is the union of per-triangle tile bounding boxes — a superset of what
was written, which is the direction that keeps it safe. Both TBR schedulers call
`XparStripSlices_MarkAllDirty()` before dispatch so the previous frame, the
legacy peel and resolution changes are all covered.

**Scanned px 197.90 M -> 6.66 M (3.36 %). Live fragment count IDENTICAL (0.97 M)
in both arms — the bound loses nothing.**

### Lever 2 — `--xpar_peel_early_out` (default ON, byte-null by construction)

Reverse-peel accept mask (`Mekalele.h:1455`):
`zmask = (z_candidate < z_existing) & (z_candidate > peelFloor)`, with
`z_existing` pre-cleared to `0xFFFF` and `peelFloor` = the previous pass's layer
Z. If pass N-1 committed nothing, its extent is still all `0xFFFF`, so pass N
needs `z < 0xFFFF && z > 0xFFFF` — empty. And pass N then leaves an untouched
layer in turn, so every later pass is empty too. A committed fragment can never
store `0xFFFF` (pass 0's own mask already requires `z_candidate < 0xFFFF`), so
"all 0xFFFF over the clump's columns" is an EXACT "nothing here", not a
heuristic. Passes 12 884 -> 6 014; live count unchanged.

### THREE-ARM COST — parent binary / OFF / ON, one asset tree, interleaved, min-of-6 over 7 rounds (r0 dropped)

Load 15.95 -> 10.90 across the batch, so read `Ginstr/f` where a wall figure
surprises. `parent` = `43ac3456` built in this worktree; `off` = HEAD with both
flags off; `ext` / `peel` = one lever each.

**fountain t=1200, `--deferred`, 1920x1080**

| | parent | off | ext only | peel only | **ON** |
|---|--:|--:|--:|--:|--:|
| frame min | 27.46 | 27.56 | 17.14 | 22.20 | **15.47** |
| `renderFrame` | 25.26 | 25.31 | 14.98 | 19.95 | **13.27** |
| `TBR-render` | 20.17 | 20.17 | 9.93 | 14.75 | **8.17** |
| `renderFrame` Ginstr/f | 3.514 | 3.515 | 1.840 | 2.672 | **1.583** |
| `TBR-render` Ginstr/f | 3.011 | 3.012 | 1.338 | 2.170 | **1.081** |

**The frame-saving-equals-pass-saving check closes to 0.1 %:** `renderFrame`
falls 11.992 ms, `TBR-render` falls 12.003 ms — 100.1 % of the frame saving is
in the phase that was attacked, and `FRAME_MIN` moves the same 11.99 ms.

| pose | parent frame | ON frame | delta | parent TBR | ON TBR | TBR Ginstr/f |
|---|--:|--:|--:|--:|--:|---|
| fountain t=1200 | 27.46 | **15.47** | **-11.99 (-43.7 %)** | 20.17 | 8.17 | 3.011 -> 1.081 |
| fountain t=2500 | 22.73 | **13.88** | **-8.85 (-38.9 %)** | 14.54 | 6.06 | 2.211 -> 0.766 |
| fountain t=600 | 19.96 | **12.28** | **-7.68 (-38.5 %)** | 11.99 | 4.29 | 1.821 -> 0.538 |
| city t=1961 | 76.34 | 76.30 | 0.00 | 7.00 | 6.77 | 0.846 -> 0.841 |
| greets t=3122 (his pose) | 47.04 | 46.96 | -0.08 | 6.13 | 6.06 | 1.178 -> 1.162 |
| chase t=1600 | — | — | — | 4.61 | 4.62 | 0.853 -> 0.853 |

`parent` vs `off` agree on instructions to 3-4 decimals at **every** pose
(3.514/3.515, 2.549/2.551, 2.314/2.315, 6.217/6.220, 5.016/5.016, 1.899/1.899),
so the flags carry no dark cost in their OFF arm. The other three scenes are
NULL, as expected — they do not run thousands of sprite-delimited clumps a
frame. This is a fountain fix.

### GATES — every arm byte-identical, three flag configurations

Differential (same binary, flags off / peel-only / both on), so the claim is
"this moved nothing", not "a hash matched":

| gate | OFF | peel only | **ON** |
|---|---|---|---|
| fountain t=2500 `8db68ccb59416e9a44037e9f387b7bd9` | 3/3 | 3/3 | 3/3 |
| fountain t=1200 glass `40ce5f1e9847ce50add24e68f482fda8` | 3/3 | 3/3 | 3/3 |
| fountain t=1200 plain deferred `3417643da0dfaf57f52be489e1356fce` | 2/2 | 2/2 | 2/2 |
| greets t=1588 `778fa6acd85a69cf241babefcdaf598e` | 2/2 | 2/2 | 2/2 |
| city t=1961 `3f8948232c192a979ffe7f76c4b387ab` | 2/2 | 2/2 | 2/2 |
| chase t=100/400/800/1200/1600 (all five pins) | ✓ | ✓ | ✓ |
| `render_gate.sh` | — | — | ALL FOUR PASS (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`) |

Both TBR schedulers are covered on purpose: the glass rows go through
`TBR_Render_GlassLayered` (barrier-per-band), the plain-deferred row through the
plain `TBR_Render` strip walk.

**Temporal / animated-spray evidence:** `--snapshot=fountain@t=100,200,...,3000`
— **30 animated frames, cat-md5 `14a2ba04f4174453f89eb5701b0028f7` in both
arms, and all 30 per-frame md5s match pairwise.** The additive spray is where a
composite-order or peel-order error would show as flicker; it is byte-identical
frame by frame across the pose sweep, so there is no look call to make here.

### WHAT IS LEFT IN `TBR-render` (8.17 ms at t=1200)

Not the composite any more. The bound leaves 6.66 M px scanned for 0.97 M live,
and the early-out removes 3/4 of the passes; what remains is the per-pass
`FrustumClipper` construct + `InitViewport` + `clipper.Render` over the clump's
faces (6 014 times a frame), the 33 358 `SpriterRT<32>` sprite blits, and the
now-narrow memsets. **Next lever, unpriced:** `peel1` still runs 2 792 times a
frame to find nothing — you cannot know a pass is empty without running it, but
you *could* know it from pass 0's own fragment count if the filler reported
"committed exactly one layer per pixel". That is a real, byte-null-shaped idea
and it is NOT measured.

## 2026-08-14 — the parked per-tile shadow early-out, MEASURED: the byte-null half ships, the byte-affecting half is refuted at the tile and re-specified at 8x8

The shadow-diet round parked *"per-tile light/shadow early-outs — a tile fully
outside a light's range or fully occluded/unoccluded can skip taps"* because it
changes shadow bytes. Both halves are now measured rather than argued, with a
new instrument (`--shadow_tap_census`, and `--shadow_tap_census_block=B` for the
granularity question). One half shipped; the other is refuted **at the tile**
and comes back with a specification.

### First, the denominator — it was wrong by 2x

The parked note carried "shadow sampling is 48.8 % of the lighting stage". The
real number, by ablation (`--prof_no_cube_tap`, one binary, interleaved min-of-4,
greets t=5743 @1920x1080):

| arm | lighting-w1 | renderFrame |
|---|--:|--:|
| shipping | 29.710 ms | 50.054 ms |
| cube tap short-circuited to 1.0 | 22.441 ms | 42.133 ms |
| **the whole cube tap** | **7.269 ms (24.5 %)** | **7.921 ms (15.8 %)** |

Every ceiling below is a fraction of **7.27 ms**, not of ~14.5.

### The census, and the two poses that disagree

`--shadow_tap_census` counts, per (tile x light-slot): pixels of the tile for
which the light survived the per-pixel mirror/dot/range gate, and how each cube
tap resolved. Main-view tiles only.

| | greets t=5743, 1920x1080 | his pose t=3122, 1512x848 |
|---|--:|--:|
| shaded px/frame | 1 043 940 | 643 389 |
| tile-light entries/frame | 837 (8.72 lights/tile) | 1 453 (15.14 lights/tile) |
| **DEAD entries** (zero in-range px) | **80 = 9.6 %** (74 carry a cube) | **763 = 52.5 %** (433 carry a cube) |
| dead loop-prologue px/frame | 0.87 M | **5.17 M** |
| cube taps/frame | 4.725 M | 0.848 M |
| tap result | lit 40.8 %, shadowed 54.7 %, partial 4.5 % | lit 98.8 %, shadowed 0.2 % |
| **tile-uniform taps** | **16.8 %** all-lit, 0.0 % all-shadowed | 91.6 % all-lit |

The two poses are complementary and neither alone would have told the truth:
t=5743 is tap-heavy and light-list-clean; **his pose is the opposite — half of
every tile's light list cannot light one pixel of that tile.**

### DONE, shipped default ON, byte-null: `--deferred_tile_sphere_cull`

The tile light list culled a light against the tile's screen rect **and**, 
separately, against its z-extent. A conjunction of two separable projections of
a sphere is strictly weaker than the sphere test: a light off the **diagonal**
corner of a tile's frustum chunk passes both and reaches no pixel. Fixed by
testing the light's range sphere against the tile's chunk bounding sphere —
the sphere `tileChunkSphere()` already builds for the spot-cone cull.

Byte-null by construction: it can only drop a pair whose every pixel is farther
than the light's cull range, i.e. every pixel that would have failed the
per-pixel `len2 > range2` test anyway, and it reads the **same** `range2` that
test compares against so a `--deferred_max_range` clamp cannot desynchronise
them. **The census proves it structurally, not just by hash:** the cull removes
397 entries per frame at his pose and the cube-tap count does not move by one
(0.848 M both ways).

| | entries/frame | dead entries | dead prologue px | cube taps |
|---|--:|--:|--:|--:|
| t=5743 off / **on** | 837 / **793** | 80 / **36** | 0.87 M / **0.39 M** | 4.725 M / **4.725 M** |
| his pose off / **on** | 1453 / **1056** | 763 / **366** | 5.17 M / **2.50 M** | 0.848 M / **0.848 M** |

**Cost, three-arm interleaved (parent binary / feature OFF / feature ON), one
asset tree, min over rounds.** The box carried load 10-13 from other agents, so
`Ginstr/f` is the column to read — it reproduces to 0.3 % and a descheduled
worker retires no instructions:

| | parent | OFF | **ON** | ON vs OFF |
|---|--:|--:|--:|--:|
| **his pose** lighting-w1 Ginstr/f | 1.998 | 2.001 | **1.935** | **-0.066 (-3.3 %)** |
| his pose renderFrame Ginstr/f | 5.163 | 5.168 | **5.097** | -0.071 (-1.4 %) |
| his pose lighting-w1 wall_min | 19.275 | 19.545 | 18.979 | -0.57 ms |
| **t=5743** lighting-w1 Ginstr/f | 3.296 | 3.296 | **3.278** | **-0.018 (-0.55 %)** |
| t=5743 renderFrame Ginstr/f | 5.114 | 5.114 | **5.091** | -0.023 (-0.45 %) |

**Stated honestly: the wall column does not separate the arms at t=5743** — the
per-round spread there is +/-1 ms against a 0.5 % instruction delta. The
instruction column is monotone in the right direction at both poses and the
mechanism (2.67 M fewer prologue iterations a frame at his pose, ~18
instructions each) predicts the size it measures.

GATES, cull ON *and* OFF on the same binary — differential, so the claim is
"this change moved nothing", not "the hash happens to match":

| pin | ON | OFF |
|---|---|---|
| greets `778fa6acd85a69cf241babefcdaf598e` | 3/3 | 2/2 |
| fountain `8db68ccb59416e9a44037e9f387b7bd9` | 2/2 (run 1 cold-bake discarded) | 3/3 |
| city `3f8948232c192a979ffe7f76c4b387ab` | 2/2 | 2/2 |
| `render_gate.sh` | ALL FOUR PASS (`4ac809e5` / `826c09e6` / `b41894f9` / `166fa25a`) | — |

**Residual, named rather than hidden:** the chunk sphere spans the tile's
*opaque* depth bounds, so a transparent pixel in front of `zMin` is outside it.
That exposure is not new — the shipped `deferred_zcull` already rejects on the
same `zMin` — and it does not fire on anything we ship: the greets pin runs
`--glass-refract=1 --glass-test --xpar-peel-passes=4` and the fountain pin is
glass-heavy, and both are byte-identical either way. `DeferredVolumetric` is
structurally immune: it reads only `tileLights[].zMax` / `.hasSky` and iterates
the frame-global `ViewLightsSoA` for the integration itself, never the per-tile
entries — which the passing `conetest` / `halotest` rows confirm.

### REFUTED at the tile: uniform shadow classification. And the reason is GRANULARITY, not coherence

A perfect oracle — free classification, zero error — replacing every tap in a
tile-uniform (tile x light) entry with one answer would remove **16.8 %** of the
taps at t=5743. Against the measured 7.27 ms that is a **1.22 ms** ceiling, for
a change that moves bytes, whose real corner-sampling classifier captures only
part of it, and whose errors land on **160x135 px** blocks — the most visible
seam size in the frame. Not worth building; not built.

But the parked idea was right that the shadow field is coherent. It was wrong
about where. `--shadow_tap_census_block=B` asks the same question per BxB block
(greets t=5743, uniform share of all 4.725 M taps):

| granularity | uniform taps |
|---|--:|
| tile (160x135) | 16.8 % |
| 32x32 | 35.8 % |
| 16x16 | 50.9 % |
| 8x8 | **76.1 %** |
| 4x4 | 89.5 % |

**SHIPPED 2026-08-15 (docs/SESSION_STATE.md 2026-08-15b), and two of the three
requirements below were wrong.** `ShadowMap::uniSD` / `uniDyn`, one u32 per 8x8
block holding the id of the block's **9x9 apron** (the apron removes the
straddle case instead of handling it). (a) per-8x8 DEPTH BOUNDS is a depth-mode
concept — PolyId compares no depths; (c) the frustum-corner classification was
an artefact of classifying in SCREEN space — the pyramid is indexed in
SHADOW-MAP space by the tap's own iX/iY, which has already selected its cube
face. **80.3 % of taps skip at t=5743**, `lighting-w1` -0.192 Gi/f (-6.5 %),
frame -1.01 ms; his pose -0.125 Gi/f, -0.33 ms. All eight pins unmoved,
`render_gate` 4/4, 9 poses x 4 configs identical. The ceiling below is NOT
"76 % of 7.27 ms": a skipped tap saves ~50 instructions (loads + compares +
addressing); the face select, the 3x3 matmul and the projection are untouched
and are the majority of the tap. Original specification follows.

**TODO, re-specified as a BYTE-NULL design at 8x8.** PolyId mode makes an exact
test available that a corner sample never was: over a block's footprint on the
cube face, if every texel carries the same id `c`, the 2x2 PCF is exactly
`occ = (c != 0 && c != receiverId) ? 1 : 0` — one compare per pixel, no taps, no
error. What it needs: (a) per-8x8 depth bounds (a min/max reduction over
ZPage16, the shape `computeTileDepthBounds` already has), (b) an id-uniformity
pyramid per cube face, built once per bake, (c) a per-(block x light)
classification projecting the block's 8 frustum-cell corners and bailing when
they straddle two faces. Ceiling: 76 % of 7.27 ms minus the classification, and
unlike the tile variant it costs no pixel its exactness.

### Dead hypotheses, with their numbers

* **"Cube taps still run for lights the pixel is out of range of."** FALSE in the
  shipping kernel. The scalar light loop's order is mirror id -> `dot < 0` ->
  `len2 > r2` -> bounce portal -> cone -> map shadows -> cube tap, so no tap can
  belong to an out-of-range pair. The census closes it: the sphere cull deletes
  397 (tile x light) pairs a frame at his pose and the tap count is unchanged.
* **...except in the 8-wide `--deferred_vec` kernel, where it is REAL.** A lane
  failing `mask_range` (or the mirror-id match) gets `safe_len2 = 1`, hence
  `lenInv = 1`, `dist = 1`, `k = dot * (1 - 1/Range)` — which stays **positive**
  for any front-facing lane, so it passes the `kArr[lane] <= 0` guard and calls
  `resolveCubeAtten`, whose result the `mask` blend then throws away. It costs
  this box nothing (`FDS_DEFERRED_VEC_DEFAULT` is 0 on arm64) and is left alone
  deliberately: no pin covers that path, so a "free" fix there would be an
  unverifiable one. On an x86 build, where the default is 1, it is live waste.
* **"The tile light lists do no range culling."** They do — screen rect
  (`lightSphereScreenRect`), z-extent, spot cone, mirror-footprint presence. The
  gap was only that rect AND z-extent approximates a sphere separably.

### The instrument had to be compiled out, and that is its own measurement

The census hooks sit in the two innermost bodies of the hottest loop in the
engine. Never taken, they still cost **+0.040 Ginstr/f (+2.0 %) on lighting-w1**
as first written; moving the block index out of the per-pixel body and into the
tap site took that to +0.018 G (+0.9 %); folding four counter arrays into one
did not help. It is register pressure in the light loop, the same mechanism
`d9248f6d` measured for the cube tap's `FDS_DEV` abort branch. So the hooks are
behind a CMake option (`-DFDS_SHADOW_TAP_CENSUS=ON`), default OFF, and the
shipping kernel measures **+0.003 G (+0.15 %)** against its parent — inside the
0.3 % reproducibility floor. The flag stays registered and prints the rebuild
line when it cannot do anything.

## 2026-08-12 — TODO: the cone INTEGRATION BODY is now the majority of the cone pass

> **UPDATE 2026-08-15 (round 7, docs/HW_PROFILING.md §15).** Two of the things
> this entry points at have since been settled, so read them before starting:
> the ablation split it asks for **was re-run on the vectorised arm** — on
> *chase*, the scene that turned out to own the biggest cone bill (§14.2) — and
> the `!segPath` gate quoted in the first bullet **is gone** (§14.3: it was set
> on a round-1 greets measurement that the kernel has since invalidated; greets
> now improves without it). The prologue this entry dismisses as not-the-lever
> was in fact worth **−9.1 % of chase's pass and −8.1 % of city's**, bit-exact,
> because the spot loop is the innermost of three (§15). What is genuinely left
> of the body: the per-segment shadow tap on greets (`shadowed=51` of 51 spots,
> unpriced), and the `rsqrt_nr_x8` question in this entry's second bullet, still
> open. The 8-segment `W²`/`D·W` closed form is **REFUTED** — built and measured
> at +0.1..+0.7 % instructions, because that loop runs only on ALIVE pairs
> (8.1 % of chase's at t=800); see §15.5 rather than rebuilding it.


The per-lane quadratic solve is **DONE** (`--vol_cone_solve_vec`, default ON,
bit-exact, −9.4 ms/frame on city t=1961; docs/HW_PROFILING.md §9). That moves
the cone pass from 4.09 → 2.87 Ginstr/f, and it is *still* the biggest single
item in the frame (2.87 of 7.07, against DeferredLighting 1.25, fastfog 1.09,
gbuffer 0.89, TBR-render 0.85) — but its composition has flipped. The untouched
SIMD body + shadow taps + accumulate is now **~1.52 of 2.87 G, i.e. the
majority**, so the next lever inside this pass is the integrand, not the
prologue.

**That ~1.52 G is INFERRED**, by holding a16567b's 63.6/36.4 ablation split and
assuming the body is unchanged (its code is). It has not been re-measured on the
new arm. **First step for whoever takes this: re-run the a16567b ablation
against the vectorized arm** (keep the prologue, `continue` before the
integration, sink the result, diff `Ginstr/f`) and get the real split before
believing anything downstream of it.

Two things already known about that body, from this work:

* It is the `useAnalytic` closed form for wide cones (city) and the 8-segment
  hybrid for narrow/turbulent ones (greets). Those are different cost shapes and
  will need separate numbers — the solve port itself won on one and lost on the
  other, which is why it is gated on `!segPath`.
* Its `rsqrt_nr_x8` / `_mm256_rcp_ps`+NR chains are *not* obviously safe to
  cheapen. The approximation family measured a dead loss in the SOLVE (raw
  estimates: +1.6 % instructions, −1.0 % wall — NEON estimates are 8-bit, so
  usable accuracy costs more than the divide), but the body's arithmetic mix is
  different and the question is open there. Measure raw-vs-NR first; it closes
  the family in one build.
## 2026-08-12 — PARKED: the mirror-shard PER-FACE cone cull. It works, it is byte-identical, and it recovers none of the 10.5 ms — the bake is 78 % deferred lighting

`ddb1d15` closed with a recorded next step: *"The right accelerator is a
per-FACE test (face bounding sphere vs cone); nobody has written it."* It is
written now (`FDS/RENDER/ReflFaceCull.cpp`, `--shard_cone_cull=2`, compile-time
gated behind `-DFDS_SHARD_BAKE_LAB=ON`). **The test is correct and the premise
is wrong.**

**IT IS CORRECT — byte-identical, not "close".** At the shatter bracket
(`--repro=greets@t=3122 --repro_from=3112 --repro_settle=0`,
`FDS_GREETS_SHATTER=1`, `FDS_GREETS_CAM="28.8,10.8,-62.85,1,0,0"`), the whole
1024² reflection atlas with the cull on is **0 of 1 048 576 pixels different**
from the cull off, with **84.3 % of face tests rejected** and the face list
going **9 921 → 8 678** entries. The invariant that buys that: a face whose
bounding sphere reaches the cone survives WHOLE, and a face that survives is
transformed and rasterized with its vertices UNTOUCHED — no stamped fake
positions, which is the entire failure mode of the per-vertex form.

**THE PREMISE IS WRONG: THE SHARD BAKE IS NOT GEOMETRY-BOUND.** New
`[SHARD-PHASE]` attribution (on `FDS_SHARD_REFL_PROF`), core-ms summed over 12
workers, min-of-6 interleaved, same bracket:

| phase | cull off | per-face cull | legacy per-vertex (broken) |
|---|--:|--:|--:|
| `Transform_Objects` | 6.5 | 7.5 | 6.4 |
| G-buffer fill (raster) | 27.4 | 29.7 | 7.6 |
| **`Render_DeferredLighting`** | **124.4** | **129.1** | **46.2** |
| volumetric cones | 0.2 | 0.3 | 0.1 |
| **wall ms** | **14.1** | **14.7** | **6.8** |

The geometry front-end is **4 %** of the pass and the deferred shading of the
reflection's own pixels is **78 %**. So the 4.0 → 14.5 ms `ddb1d15` priced is
not culling that was lost, it is **the reflection appearing** — every one of
those pixels is wanted, and no cull, however conservative or however tight, can
take them back. The per-face cull's 12.5 % fewer face-list entries are faces
that rasterize zero pixels; dropping them is free and buys nothing.

**AND IT COSTS ALL THREE SCENE PINS TO CARRY.** Merely having the call inside
`Transform_Objects`' per-mesh body moves greets `778fa6ac→7a6370a1`, fountain
`8db68ccb→eebf68e6` and city `3cbe42b1→80583b85` under `-ffp-contract=fast`, on
snapshots that never shatter a mirror — the same hazard
`docs/VISIBILITY_PLAN.md §8a` records for `--xfrm_pass_prof`. Bisected: the
branch it adds to the face loop is byte-null; the CALL is not, at either call
site tried (before the vertex loops, and after them). Hence the compile-time
gate rather than a re-pin: **0.1 ms is not worth three pins.**

**WHERE THE NEXT ms ACTUALLY IS, then, and it is a big one:**
`Render_DeferredLighting` runs **238 times per shatter frame on a 64² target**
and is 78 % of the pass. Its per-invocation fixed work — light binning into
tiles, the tile-light lists, the shadow-atlas setup — is being paid 238 times
for 4 096 pixels each. STATUS: **DONE 2026-08-12, and the diagnosis above named
the wrong suspects** — see the dated block at the top of `docs/SESSION_STATE.md`.
Measured, the orchestrator prologue this paragraph blames is **9–10 %** of the
pass. The fixed work is per **TILE**, not per invocation: the tile grid was
engine-global at 12×8, so a 64² cell walked 96 tiles (32 of them entirely off
the right edge, shading nothing) = 22 848 tile invocations per shatter frame,
each paying a kernel prologue **and** a `renderns::tileDone` release+acquire on
a semaphore all 12 workers share — **3.4–4.0 µs of core time per pair contended,
against 34.5 ns uncontended**. Two flags, both default-on, both byte-null:
`--deferred_inline_tile_sem` (inline dispatch posts no permit) and
`--deferred_offscreen_tile_px` (offscreen targets size the grid to themselves).
`Render_DeferredLighting` 121.7 → 93.6 core-ms, bake wall 13.6 → 11.6 ms, fixed-
work share 32 % → 3 %, break frame 46.25 → 43.88 ms. Atlas, break+1 frame, all
three pins and `render_gate` byte-identical. **What is left is not overhead:**
97 % of the pass is now the per-pixel shading of 974 848 pixels, and the only
remaining levers are rate/resolution reductions (offscreen checkerboard or
quarter-rate — the wave-2 `TileFill` machinery is already per-target selectable
— or a smaller `texRes_`), every one of which is a LOOK change on a surface the
user gates by eye. Note the per-target-HDR item below still touches this call.

**STATUS of the cull itself: PARKED — built, measured byte-identical, measured
not-worth.** Reproduce with `cmake -S . -B build-lab -G Ninja
-DFDS_SHARD_BAKE_LAB=ON` and `--shard_cone_cull=0|1|2`.

## 2026-08-11 — TODO: a PER-TARGET HDR buffer, so an offscreen bake tonemaps like the frame it feeds

**The defect.** `g_hdrBuf` is a single global sized by `Hdr_BeginFrame()` to the
MAIN view, and every HDR write in the deferred kernel gates on
`Hdr_WritableFor(ctx.xres, ctx.yres)` — the CURRENT pass's dims. So an offscreen
bake at any other resolution silently falls through to the LDR combine
(`texel*light/256 + spec`) while the main frame renders linear radiance through
exposure → ACES → sqrt. The two are different transfer functions, so an
offscreen bake cannot match the frame it is composited into, at any gain.

**Who is affected.** The **mirror-shard bake** (`MirrorShatter.cpp`) — measured
2026-08-11 at the greets shatter screen: with the coverage bug fixed the shard
mosaic reads **86.37** panel-window luma against the **73.86** the main deferred
pass renders from the same reflected eye (**+12.5, brighter**), and under
`--no-hdr` — where the main pass loses ACES+sqrt and falls to 43.28 — the mosaic
barely moves (79.05), which is the signature of a pass ignoring the frame's
transfer function. The **mirror RTT is NOT affected**: it already brackets its
bake with `Hdr_BeginFramePass(texW,texH)` / `Hdr_ActivateNoFog()` /
`Render_TonemapToVPage()` (`GreetsMirror.cpp:3273-3286`), which is exactly the
right shape — and is the reference implementation for this item.

**Why the RTT's fix does not port as-is.** The RTT bake is serial, so it can own
the global for the duration. The shard bake fans N shards across the worker pool
concurrently (`renderShardIntoCell`), so `Hdr_BeginFramePass` cannot be called
per worker without racing. The fix is to thread an HDR target through
`DeferredOverride` (alongside `gb` / `vpage` / `zpage16`) so each worker
accumulates into its own float buffer and tonemaps it, instead of every pass
reaching for one global. That also removes the `Hdr_WritableFor` dims check as
the de-facto "am I the main pass?" test, which is what makes the failure silent.

**Priced before starting:** the shard bake pass is 14.5 ms (min-of-6, load 12.8);
a per-worker `texRes²×4` float buffer is 64 KB at res 64 — the cost is the extra
tonemap sweep, not the memory. STATUS: **BUILT 2026-08-12, MEASURED, AND
OVERTURNED. The diagnosis above is right; the remedy is wrong.**

`--shard_hdr` (default **0**) is that fix, exactly as designed: a per-worker HDR
buffer through `DeferredOverride`, `Hdr_ActivateNoFogInline` +
`Render_TonemapToVPageInline` after the kernel. It makes the residual **four
times worse.** Panel-window luma at the shatter bracket, against the MAIN
deferred pass from the shard's own reflected eye as ground truth:

| | luma | vs reference |
|---|--:|--:|
| reference (main pass, reflected eye) | 74.78 | — |
| shipping, `--no-shard_hdr` | 87.31 | **+12.53** (reproduces `ddb1d15`'s +12.5) |
| `--shard_hdr` | 129.79 | **+55.01** |

**Why: the shard atlas is an ALBEDO TEXTURE, not a finished image.** The shards
are ordinary opaque scene geometry, so the MAIN frame's deferred kernel samples
the atlas as a texel, lights it, writes linear radiance to `g_hdrBuf` and
tonemaps it with everything else. One A/B proves it: with the flag OFF, sweeping
the **frame's** `--hdr_exposure` 1.0 → 2.0 moves the mosaic **87.31 → 130.78**.
The frame's tonemap already owns those pixels; tonemapping the cell as well
applies the transfer function twice — and `--shard_hdr` at exposure 1.0 (129.79)
landing on legacy-at-exposure-2.0 (130.78) is that doubling's signature.

**The mirror RTT is not a counterexample, it is the clue.** It keeps the FLOAT
radiance in the material's `hdrRefl` and hands *that* to the frame
(`GreetsMirror.cpp`); its `Render_TonemapToVPage` onto the 8-bit RTT surface is
only the LDR fallback. So the real remedy for the +12.5 is an **HDR atlas** —
shard cells carrying linear radiance into the frame through an `hdrRefl`-shaped
path — not a tonemapped 8-bit cell. **STATUS of that: TODO, not started.**

**What DID ship from this, unconditionally and byte-null:** the plumbing.
`DeferredLightingCtx::hdrBuf` now carries each pass's own HDR target, replacing
the kernels' `Hdr_WritableFor(ctx.xres, ctx.yres)` test — "the global happens to
be sized like me" standing in for "am I the main pass?", which is what made the
failure silent in the first place. Main frame identical (all three pins,
`render_gate` 3/3); it is the prerequisite the HDR-atlas fix will need.

**AND IT SHIPPED A REGRESSION FOR 15 MINUTES — the plumbing's first form,
`ctx.hdrBuf = ov ? ov->hdr : …`, silently dropped the mirror RTT (the *other*
`DeferredOverride` user, which brings no `ov->hdr` because it borrows the global
through `Hdr_BeginFramePass`) off the HDR path.** Fixed in `283b46ca` by making
the override's buffer an override rather than a mode switch, and verified
2026-08-13 by **byte-identity with the pre-restructure `5adcae12` binary** —
frame `4abe5214…`, RTT slot `5199d3d1…`, against the broken tip's `c7ef96f6…` /
`ab17ac64…`. Cost while broken: 98.9 % of the RTT slot's pixels, −21.0 mean
luma on the slot, −12.3 on the panel as it appears in the frame. Full analysis
in `docs/SESSION_STATE.md` (2026-08-13). **The residual +12.5 above is
untouched by any of this and remains the open item.**

~~**GATE GAP, STILL OPEN AND WORTH ITS OWN LINE:** `render_gate`'s `mirrortest` is
cited throughout this campaign as the thing that "covers the mirror RTT". It does
not. `--scene-mirrortest` never enables `mirror_rtt` (default 0; only
`GREETS.CPP`'s `setDefault` and the editor turn it on), and measurement confirms
it: `mirrortest` is byte-identical on the baseline, the broken and the fixed
binaries — **with `--hdr` as well as without**. The order-2 RTT path has no
standing gate. A cheap one exists and is not wired: greets t=3122
`--hdr --deferred` with `FDS_MIRROR_RTT_DUMP=1`, hashing `/tmp/rtt_*.ppm`.~~
**CLOSED 2026-08-13 — `render_gate`'s fourth row, `rttslot`
`826c09e63217e778cfcef70fe0167279`.** Built on `mirrortest`, not on greets: the
same scene already prepares two order-2 slots the moment `mirror_rtt` is on
(its two mirrors face each other, `MirrorTestDriver.cpp:269` calls
`PrepareSecondOrderMirrorRtt`), and unlike a greets row it does not key on the
user's uncommitted authoring files — which is exactly why `render_gate`'s own
header keeps greets out. Recipe `--scene-mirrortest --mirror_rtt
--shard_deferred --hdr` with `FDS_MIRROR_RTT_DUMP=1`, gated surface the 4
`/tmp/rtt_*.ppm` slot dumps. Proved in **both** directions: PASS 3/3 on
`6656300b`, **FAIL on `00d28a8b`** (`2ecd5e81…`) while the other three rows pass
there unchanged. All three flags shown load-bearing by controls that do not
discriminate the binaries. Details in `docs/SESSION_STATE.md` (2026-08-13b).

## 2026-08-10 — MEMORY-SIZE SWEEP: `--mem_census`, and the per-shadow-map FList (403 MiB at 0.5 % fill)

Asked as *"the lightmap defect and the shadow-plane defect had shapes — sweep
the engine for more of both"*. The two shapes:

1. **Wrong-variable scaling** (`943d644`) — an allocation whose size is a
   product of counts, where one of the counts is the wrong thing to scale with.
   The static lightmap was `numFaces × lmRes² × numOmnis` bytes fully touched at
   init: it scaled with FACE COUNT at a flat 128²/face instead of with SURFACE
   AREA, so tessellation multiplied it ~300× for nothing (19.4 GB displaced).
2. **Scatter** (`af1f8f8`) — one logical datum split across parallel arrays that
   a hot loop always reads together. Four u16 shadow planes 512 KB apart =
   8 cache lines per tap; packing to one u32 plane bought −1.0 ms, byte-null.

**The instrument shipped first, and it is the durable part of this work:
`--mem_census`** (`FDS/Base/MemCensus.h`, default off, byte-null — it reads
sizes and writes stderr). It walks every large allocation and prints resident
bytes, whether every byte is TOUCHED, and **the formula it scales with**. That
last column is the whole point: `0.5 GB` is not actionable, `0.5 GB BECAUSE it
scales with the whole scene's face count, per light-face` is. It also prints the
process `phys_footprint` and the UNCENSUSED RESIDUAL, so a subsystem nobody has
taught it about shows up as a gap rather than as silence. **Adding a subsystem
is one function plus one line** (`FDS_MEMCENSUS_REPORTER`). Cross-checked against
`/usr/bin/time -l`: census-reported footprint 2.17 GiB vs `peak memory footprint`
2 334 362 264 B on the same run.

### Per-scene totals (1920×1080, dummy drivers, `--mem_census`, end of tick 1)

| scene / arm | censused | of which TOUCHED | `phys_footprint` | uncensused residual |
|---|--:|--:|--:|--:|
| greets (pin recipe) | 738.68 MiB | 726.51 | 1.36 GiB | 655 MiB |
| greets `--greets_displace` | **1.46 GiB** | 1.43 GiB | **2.17 GiB** | 734 MiB |
| city (pin recipe) | 760.44 MiB | 744.87 | 1.19 GiB | 459 MiB |
| fountain (pin recipe) | 249.33 MiB | 237.92 | 1.07 GiB | 847 MiB |
| chase t=800 | 125.20 MiB | 110.06 | 197.02 MiB | 72 MiB |

Caveat when reading the `texture/*` rows: `MatLib` is process-global and every
scene is initialised at startup, so those rows are the WHOLE DEMO's texture set,
not the current scene's. The `geometry/*` and `shadow*` rows are `CurScene`-only.
That is also most of why fountain shows an 847 MiB residual on a 249 MiB census.

### THE RANKED TABLE

Sizes are measured (census + `time -l`). Every *win* is labelled measured or
inferred; none of the unlanded items has been built, so none has a measured win.

| # | finding | measured size | formula | should scale with | waste | fix shape | expected win |
|---|---|--:|---|---|--:|---|---|
| **1** | **per-shadow-map FList + radix scratch** `FDS/RENDER/Shadows.cpp` (`perLightFaces[i].resize(Polys)`) | greets **83.27 MiB**, displaced **403.10 MiB** | `numShadowMaps × 2 × Polys × sizeof(FListEntry)=24`; numShadowMaps = 6×cubeOmnis + spots = **76** at greets | the faces ONE LIGHT-FACE actually sees | **97.7 % / 99.5 %** — measured fill mean **856 of 37 173** (flat) and **881 of 182 350** (displaced), max 5 560 / 5 564 | high-water-mark sizing: the `resize` already runs on the tick thread before the Phase-A dispatch, so growth there is safe; keep a per-map high-water + slack, grow-only, with an overflow guard in the fill | **inferred** −81 MiB greets / **−395 MiB** displaced RSS + the first-touch cost at init. No frame-time claim. |
| **2** | **city env paraboloid hemi sheets** `DEMO/CITY.CPP:2724` (`Materialize(sheet, kSheetRes, kSheetRes)`) | **355.00 MiB** (355 sheets) | `6 sheets × kSheetRes(512)² × 4 B × numBuildings` | the building's reflective footprint / screen size | fixed 512² for every building regardless of size | per-building sheet res from bounding radius or authored importance; or 256² globally | **inferred** −266 MiB at 256². **LOOK CHANGE — needs his eye before anything is built.** |
| **3** | **per-shadow-map mesh clones** `FDS/Base/VertexScratch.*` | greets **178.16 MiB** (93.43 Vertex + 36.04 Face + 48.69 VertexFrame), displaced **440.86 MiB** | `Σ over (shadow map × mesh it rasterised) of VIndex×140 + FIndex×162 + ceil8(VIndex)×72`; **2 445 clones** flat, 1 360 displaced | concurrent light passes — but see below | the VertexFrame slab clones all **18** SoA fields when the depth-only shadow pass needs a handful | (a) a shadow-only VertexFrame with the fields the depth pass reads; (b) merge Phase A/B per light so a scratch can be recycled | **inferred** up to −48.69 MiB greets / −119 MiB displaced from (a) alone |
| **4** | **env probe cube stores** `FDS/RENDER/EnvBake.cpp` | city **155.39 MiB** | `stores(78) × 6 faces × res(256)² × 4 B × mipchain(1.328)` | probe count × res², both authored | 78 probes at a uniform res | dedup near-coincident probes; per-probe `storeRes` from footprint | **inferred**, unquantified |
| **5** | **static shadow lightmap atlas** (the `943d644` item, capped but not closed) | greets **88.51 MiB**, displaced **147.38 MiB** | `Σ per mesh of faces × lmRes² × omnis × 1 B`; lmRes 8..128, omnis 11, 115 346 faces displaced | surface area (now partly does, via `--shadow_lightmap_texel_density`) | still linear in FACE COUNT below the density cap | lower the `shadow_lightmap_res` cap, or make the density the only knob | **inferred**, small next to 1–3 |
| **6** | **`ShadowMap::packDyn`** `FDS/FILLERS/ShadowMap.cpp:411` | greets **51.62 MiB** (= packSD exactly) | `res² × 4 B × (6×cubeOmnis + spots)` | the maps a dynamic mesh actually reaches | allocated for EVERY map; `hasDynMeshVisible` typically true for 1–2 of 6 cube faces | allocate on first dynamic hit and keep (the per-frame bit churns, so don't key on it) | **inferred** −30..45 MiB greets |
| **7** | **transparent G-buffer planes, unconditional** `FDS/FILLERS/Mekalele.cpp:125-141` | **45.6 MiB** on EVERY scene (front 16.6 + back 16.6 + z.front/z.back 8.3 + peelFloor 4.15) | `W*H × (4+4+4+4+2+2+2)` | whether the scene has transparent geometry at all | chase and city pay the back layer and the K>1 peel floor for nothing | allocate on first transparent commit | **inferred** −20 MiB on xpar-free scenes. NOT byte-null-trivial: the rasterizer reads the raw pointers. |
| **8** | **`g_stripLights[512]`** `FDS/RENDER/DeferredLightLists.cpp:397` | **8.27 MiB** BSS | `DEFERRED_MAX_STRIPS(512) × sizeof(TileLights)=16 928` | `YRes/8` — **135** strips at 1080p | sized by the CAP; ~74 % never written, so address space rather than RSS | size at resize like every other screen-derived array | low; named because the header claimed "96 KiB" (see below) |
| **9** | **DoF full-res source** `FDS/RENDER/Hdr.cpp` (`g_dofSrcF32`) | **33.2 MiB** when `--dof` runs full-res (0 in the pin arms) | `W*H × 4 × sizeof(float)` — an f16→f32 EXPANSION, i.e. 2× `g_hdrBuf` | the buffer it copies | the copy widens 2 B → 4 B for no stated reason | keep it `hdrf`, or require `--dof_downscale>1` (which skips it entirely) | **inferred** −16.6 MiB |
| **10** | **`WaterBuf[65536*4]`** CITY + CHASE | 1 MiB each, **¾ never touched** | a BYTE count used as an ELEMENT count | 65536 DWords (256×256) | 4× | one-token fix | **LANDED** (see below) |

**Two stale comments were load-bearing** — they are why items 1 and 8 were
invisible, and both are corrected in this changeset:
`DeferredCommon.h` claimed TileLights was *"24 tiles × 8 arrays × 128 floats =
96 KiB total"* (actual: 96 tiles × 33 arrays, `sizeof` 16 928 → 1.55 MiB for
`s_tileLights` and 8.27 MiB for `g_stripLights`), and `FaceListContext.h`
claimed *"contiguous 16-byte slots"* (actual `sizeof(FListEntry)` is 24 —
sortKey(4) + 4 B PADDING + face(8) + bbox(8); reordering does not recover the
padding, only a 32-bit face index would). Two independent audits mis-priced
those lists straight from the stale figures.

### The signature to remember, from item 1

Between the flat and displaced greets arms `Polys` went **37 173 → 182 350**
(4.9×) and the FList capacity went with it, **83 → 403 MiB**. The number of
faces a light-face actually filled went **5 560 → 5 564**. The capacity tracked
tessellation; the demand did not — because the tessellated geometry sits behind
`--greets_shadow_proxy` and never enters a shadow FList at all. That is the
wrong-variable shape in its purest form, and it is exactly what the census's
formula column is for.

### Shape 2 (scatter): the surviving candidates, ranked by base pointers × hotness

`docs/OPTIMIZATION_BACKLOG.md` already inventoried the opaque G-buffer sweep
(§"17 B/px unconditional from 6 separate arrays") and scored it *sequential*.
That judgement priced the BANDWIDTH, not the lines / TLB entries / prefetch
streams per access — which is the axis the `af1f8f8` fix actually moved. Re-open
with that in mind:

* **The wave-1 deferred kernel** (`DeferredSurfaceKernel.cpp:1666`) reads **8
  distinct base pointers for the same pixel index `i`**: `ZPage16`,
  `gb.mirrorId`, `gb.txtr`, `gb.shadowMatID`, `gb.albedo`, `gb.normal`,
  `gb.tangent`, plus `lightmapMF`+`lightmapST`. Planes are 2–8 MB apart, each
  its own `malloc`. Same shape at `:4326` (OuterVec, 6 bases), `:3226`
  (transparent, 6), `:5180` (wave-2 TileFill, 3 bases × 5 pixels).
* **The cleanest pair in the tree**: `lightmapMF` (u32) + `lightmapST` (u16) —
  one logical datum (`meshLMId | faceIdx | s | t` = 6 B that fits one u64), read
  by `resolvePixelLightmap` (`DeferredShadowSampling.h:47`) as two planes 4 MB
  apart, every pixel, whenever `--shadow_lightmap` is on. **This is the closest
  structural analogue to the shadow-plane fix that is still open.**
* **The second-cleanest**: `mirrorMask` (u8) + `mirrorMaskZ` (u16) — allocated
  together (`GreetsMirror.cpp:1171`), written together, read together twice per
  rasterizer row (`Mekalele.h:1456` and `:1510`), and the code already asserts
  they are inseparable (`if (zplane.size() < plane.size()) return; // sized
  together`). 3 B across two allocations 2 MB apart.
* **Half-res fog** (`DeferredFastFog.cpp:902`): `gFogAmt/gFogZ/gFogGR/gFogGG/
  gFogGB` — five parallel `vector<float>`, all five read at the same index, 4
  taps → **20 addresses from 5 bases per pixel** in the bilateral compositor
  (`:1002`). One logical record = 20 B that fits one 32-B struct.
* **SSAO** (`DeferredSSAO.cpp:145`): `aoZ` then `aoRaw` at the same index, 16 box
  taps in the blur and 4 in the apply. 2 bases, always together.
* **EdgeAA** (`DeferredEdgeAA.cpp:99`): `ZPage16` + `gb.normal`, 5-tap stencil
  over 3 rows = ~6 lines from 2 far-apart bases per pixel.
* **`TileLights` scalar per-light loop** (`DeferredSurfaceKernel.cpp:2504`):
  ~22–25 base pointers per light per pixel, consecutive arrays 512 B apart. This
  is the path every normal-mapped pixel takes. **Caveat for judgement:** 16.9 KB
  per tile is L1/L2-resident, so this is a latency/µop item, not a DRAM one.
* **NEGATIVE RESULT, reported not buried:** the froxel accumulation planes
  (`gFrAccR/G/B/T` + `gFrSct`, 5 bases × 8 gathers per pixel) look like a prime
  candidate, but `DeferredFastFog.cpp:2528` records that **an AoS repack of the
  froxel arrays moved nothing**. Read that before spending time there. It may
  have been measured against the uniform-group fast path at `:2486` that already
  collapses the common case — worth confirming, not worth assuming.

### Landed in this changeset (byte-null, gated)

* `--mem_census` + `--mem_census_frame` and reporters for: the three G-buffers
  and their optional planes, the xpar Z/peel planes, cube+2D shadow maps and
  their swizzle copies, the per-shadow-map FList and mesh clones, the main face
  list, the static lightmap atlases, the HDR buffer and post chain, the
  glass-refraction snapshots, froxel + half-res fog, SSAO, the strip light
  lists, scene geometry (with mirror/proxy clones broken out), texture pixel
  data + mip chains by role, env probe stores, and the framebuffer + Z-buffer.
* `WaterBuf[65536*4]` → `[65536]` in CITY and CHASE (item 10). Write-only in
  live code; the `memcpy` of `65536*4` BYTES now exactly fills it.
* The two stale size comments above.

**Gate: differential, on ONE tree snapshot** (other agents held uncommitted work
in the shared tree, which makes an absolute pin meaningless — see the
2026-08-09c hazard note in `docs/SESSION_STATE.md`). Two binaries, `DEMO_memA`
(with this diff) and `DEMO_memB` (my hunks reverted, everyone else's kept),
3 runs each after discarding run 1: **identical, and equal to the recorded pins**
— greets `778fa6acd85a69cf241babefcdaf598e`, fountain
`8db68ccb59416e9a44037e9f387b7bd9`, city `3cbe42b166847e40f7071eedb48d613c`,
chase t100/400/800/1200/1600 `76e7cf68…` `d458e82b…` `c145c7a5…` `31aa5203…`
`1544b0e7…`. 24 runs, one value per cell.

### Not re-litigated

`Vertex` 140→68 and `Face` layout are refuted in
`docs/SOA_VERTEX_REFACTOR.md` (2026-08-09) and §2 below; the census reports
their bytes but this sweep does not reopen them.

## 2026-08-09 — HOT-STRUCT SWEEP: the front end is closed, the SHADOW-TAP planes are the item

Asked as *"trimming hot-struct bloat paid off once — is there more, here or in
other structs?"*. Three structs were audited against their consumer loops and
measured. **The winner is not `Vertex` and not `Face`; it is the four shadow-map
planes, and the mechanism is the same one (`cache LINES touched per access`).**

### 1. `Vertex` 140 → 68 (SoA Phase 5) — **PARKED, re-refuted with fresh numbers**

Full working in docs/SOA_VERTEX_REFACTOR.md (2026-08-09 section). Short version:
`VERT` is the only bucket it can touch and it is **0.345 ms at greets / 0.611 ms
p50 at city / 0.114–0.158 ms at chase**; the best case is ~40 % of that =
**0.24–0.31 % of frame**, for an 11-file refactor across two alternative transform
pipelines. chase is FACE-dominated (FACE 2.9–3.3× VERT), so it is the wrong lever
there by shape as well. Not attempted; do it for the one-writer contract if ever,
not for perf.

### 2. `Face` (162 B, pack(1)) — audited field-by-field; **layout work NOT indicated**

Full consumer audit done (every field × every compiled consumer, `FL.CPP` and
`3DS/WORLD.CPP` excluded as dead). Two structural facts worth keeping:

* **`alignof(Face) == 1` and the stride is 162, so `gcd(162,64) == 2` — a `Face`
  in `TriMesh::Faces[]` has 32 distinct cache-line PHASES.** Faces do not start on
  line boundaries; each spans 3 or 4 lines depending on index. **Any "put the hot
  fields in line 0" plan is defeated by the phase rotation** unless the struct is
  also padded to 128/192 or split into parallel arrays. This is the thing that
  makes `Face` unlike `Vertex`, and it should be checked before anyone proposes a
  regroup.
* **24 bytes of the struct are cold and sit in the middle of it.** `EU1..EV3`
  (offset 72) has exactly one active reader in the built tree —
  `FRUSTRUM.CPP:1065-1070`, *inside* `if (F->Flags & Face_Reflective)` — and
  downstream only `TheOtherBarry<…, TEXTURETEXTURE>` consumes it. `ReflectionTexture`
  (offset 112) is the same reflective-only gate. Together 32 B of a 162 B stride
  that the FList-build loop and Mekalele's per-face prologue both walk over for
  nothing. Evicting them to a side table indexed for reflective faces would take
  `Face` to 130 B.

**But the prize is bounded and was measured at ~zero.** The FACE bucket is only
0.254 ms (greets) / 0.539 ms (city), and §2 of the SoA doc already measured that
reducing lines-per-deref does not move it (the accesses are cache-resident). The
one live cross-core effect found in the audit was tested directly and **also
measured neutral** — see item 4. **Recommendation: do not spend effort on `Face`
layout.** The audit is recorded so the next person does not re-derive it.

### 3. Cube-shadow tap: the packed plane is now the SOURCE OF TRUTH — **DONE, shipped ON, byte-null**

**This was the item, and it landed.** At greets t=5743 the cube tap was **10.28 ms
of a 30.5 ms `lighting-w1`** (`--prof_no_cube_tap`, min-of-3: 30.518 → 20.238;
`--prof_no_lights` → 11.543 for scale). A PolyId tap needs a texel's `polyId` AND
its `depth`, for the static pair and the dynamic pair — and those were **four
separate `std::vector<uint16_t>` that at greets' 512² sit 512 KB apart.** 32 bytes
of useful data, gathered from 4 base pointers over 2 PCF rows = **up to 8 cache
lines (~512 B of line traffic) per tap**; the static-lightmap composite path's
dynamic-only tap still cost 4.

`ShadowMap` now holds **two `std::vector<uint32_t>`** — `packSD` and `packDyn`,
each texel `z | (ShadowMatID << 16)` — and they are the ONLY representation.
A texel's id+z is ONE 32-bit load: **8 lines → 4, and 4 → 2.** Bytes resident are
unchanged (4 × u16 either way); only the grouping changed. `ShadowTexZ` /
`ShadowTexId` / `ShadowTexPack` (FDS/FILLERS/ShadowMap.h) are the accessors.

The probe flag `--shadow_plane_pack` and `CubeShadow_SamplePacked` are **deleted**:
with the packed plane as the source of truth there is nothing left to A/B.

Measured, greets t=5743, 1920×1080, dummy drivers, `--deferred_prof=1`, per-frame
`wall_min`, **min over 11 interleaved rounds** (batch 1 = 6 rounds, load 6.2–8.9;
batch 2 = 5 rounds, load 9.5–12.3, arm order alternated). Matched pair from ONE
tree snapshot at `63bdc85`, base = the four-u16 tree, so the only difference is
this diff. The first round after each rebuild was discarded.

| arm | `lighting-w1` | `renderFrame` | `shadow-bake` |
|---|--:|--:|--:|
| shipping, four u16 planes | 27.620 | 44.730 | 2.474 |
| shipping, **packed** | **26.423** (−1.20) | **43.720** (−1.01) | **2.307** (−0.17) |
| `--no-shadow_lightmap`, four u16 planes | 27.696 | 44.832 | 2.488 |
| `--no-shadow_lightmap`, **packed** | **26.439** (−1.26) | **43.840** (−0.99) | **2.295** (−0.19) |

**Both arms beat the derived-copy probe's numbers, which is the prediction.** The
probe measured −0.42 shipping / −1.05 full-tap on `lighting-w1` while *also* paying
a per-bake rebuild of 144 MB; removing the rebuild and keeping the layout gives
−1.20 / −1.26. The probe's numbers were correctly called a lower bound.

**The `shadow-bake` win is the cleanest signal in the table**: 11 of the 12 packed
bake samples sit below the *minimum* of the 12 base samples (packed 2.295–2.495,
base 2.474–2.671). Mechanism, and it is not the tap: the per-frame dynamic bake
clears ONE plane instead of two arrays over the same bytes, and `ShadowBarry`'s
masked store writes z+id in one 32-bit `select` instead of a `blendv` on the z
array plus a per-lane scalar scatter into the id array.

**A caveat measured and reported, not hidden:** `--no-shadow_lightmap` no longer
doubles the win the way it did for the probe (−1.26 vs −1.20, not 2×). The
lines-touched model predicts a bigger spread; the honest reading is that at
min-of-11 both arms are near the same floor and the extra leverage is being
absorbed elsewhere in `lighting-w1`. The per-round record is what carries the full
tap: with the arm order alternated, the `--no-shadow_lightmap` arm favours packed
**5/5**, and the shipping arm 3/5. In the fixed-order batch the last arm in each
round absorbed the round's load drift — a positional bias worth remembering when
reading any interleaved A/B here.

**Cold readers pay nothing, and that was checked.** The static-lightmap bake and
the fog/volumetric spot taps read only ONE half of a texel, so they now pull a 4 B
word where they used to pull 2 B. That is 2× the bytes but the SAME number of cache
lines per tap, and it measures nil: `[LM] LightmapBake_Static` 53.7 ms base vs
54.0 ms packed (min-of-11), inside the spread.

**Byte-null, certified differentially** — matched base-vs-packed pair from one tree
snapshot, 3 runs each, interleaved so the tree's concurrent authoring edits land on
both arms: greets `778fa6ac…`, city `3cbe42b1…`, fountain `8db68ccb…`, chase
t100 `76e7cf68…` t400 `d458e82b…` t800 `c145c7a5…` t1200 `31aa5203…` t1600
`1544b0e7…`. **48 hashes, 24 matched pairs, zero differences**, and every one also
equals the recorded absolute pin.

**Memory: neutral, which is the point.** The probe form cost **+144 MB** of derived
copy (greets carries 11 cube maps × 6 faces at 512² plus 10 spot maps at 256²;
66 × 512² × 4 B × 2 + 10 × 256² × 4 B × 2 = 143.7 MB). Source-of-truth costs zero:
peak footprint min-of-3 **1420.0 MB base vs 1415.6 MB packed**, i.e. equal within a
±5 MB run-to-run spread. There is no rebuild pass left to time.

Orthogonal to `--shadow_swizzle`, which attacks the ROW-STRADDLE axis and measured
NEGATIVE in all 15 shapes (docs/ARCHITECTURE_NEXT.md). That experiment never
touched the parallel-plane axis; it survives, retargeted at the two packed planes
(`packSDSw` / `packDynSw`, two derived copies instead of four).

### 4. `Face::LastMip` — a dead store from 12 workers into a shared line. **Removed; measured NEUTRAL.**

Found by the `Face` audit: `MiplevelClipper` wrote `F->LastMip` once per face
**per tile** from every tile worker, gated only on `g_mipLastMipWrite` (true), while
**both readers are gated on `mip_hysteresis > 0`, which is DEFAULT 0.** So at
shipping defaults it was a dead store — into byte 136 of a 162-byte `Face`, a line
shared with `ownerMirrorId` / `behindMirrorMask` / `A_idx..C_idx` / `frame`, all
read by other hot paths. Now gated on the same `mipHyst > 0` the readers use
(FRUSTRUM.CPP:784/867). Byte-null by construction.

**Measured NEUTRAL** — matched A/B pair built from ONE tree snapshot (the only
difference is this diff), greets t=5743, min-of-6, quiet box: `gbuffer` 5.286 →
5.201 (−0.085), `renderFrame` 43.630 → 43.440 (−0.19), `shadow-bake` 2.480 → 2.477.
All inside the run-to-run spread.

**That neutral result is itself a finding: it prices Face-tail cross-core line
contention at ~0 and is the direct evidence behind item 2's "do not do `Face`
layout work".** Kept because a provably dead store should not be executed, not
because it bought time.

### Also inventoried, not acted on (ranked by streamed bytes/frame)

| item | traffic/frame | residency | verdict |
|---|--:|---|---|
| cube-shadow taps (2 packed planes × 2×2 PCF + header) | ~0.5 GB of line traffic | 12.6 MB/cube omni ≫ LLC → DRAM | **item 3 above — DONE, halved** |
| `Material` pointer chase | ~796 MB of accesses (6 lines/px: 1 matTable + 5 `Material`) | table ≤ 114 KB → **L2, not DRAM** | `sizeof(Material)` = **467 B** (was 455 before `EnvBakeOfs[3]` landed in 63bdc85), pack(1), no `static_assert`. Per-pixel fields are spread over lines 0/1/3/4/6 and the "hot fields on line 0" comment is **stale** — `TintR/G/B`, `SpecMul`, `AoStrength`, `Roughness/MetallicMap`, `Reflection`, `TbnHandedness` all drifted off it. No `matID` memo anywhere (every kernel re-chases per pixel). A 64-byte hot-fields record per matID (16 KB, L1-resident) collapses 5 lines → 1. **PARKED — now MEASURED, and refuted as a latency item: see §"2026-08-10 — hardware counters" item 2. Bound on the win is ≲0.1 ms/frame.** |
| texture + aux-map point fetches | 130–660 MB | mip chains → DRAM | already measured: the albedo gather is worth only 0.71 ms (PERF_STATE) |
| `g_hdrBuf` | 166–232 MB (10–14 sweeps × 16.59 MB) | > LLC | `hdrf` is already `__fp16` on arm64 (8 B/px, not the 16 B PERF_STATE still quotes) |
| opaque G-buffer sweep | 35–44 MB read | sequential | 17 B/px unconditional from **6 separate arrays** = 9 concurrent streams/worker |
| `Omni` | 60 KB | trivial | `sizeof(Omni)` = **515 B**, not the "[256 Bytes]" its comment claims, and **302 B of it is a `Vertex` + a `Face`** the lighting path never reads. Pure list-walk pollution; harmless at 117 lights. |
| `TileLights` | 1.55 MiB resident | L1 per tile | `sizeof` = 16 928 B × 96 tiles. Its own comment says "96 KiB total" — **stale by 17×** (8 arrays grew to 33). Not a bandwidth item: ~1.3–3.8 KB touched per tile. |
| `FListEntry` | — | — | 24 B, not the 16 B its comment claims (bbox fields were added) — 2.67 per line, not 4. |

## PBR quality series (incremental, one at a time, user reviews each)
Discussion: engine PBR compositing vs canonical (SESSION_STATE / chat 2026-07-13).
The engine is standard in the big decisions (additive diffuse+specular, GGX,
Schlick, metalness→F0, roughness-mip reflection); it simplifies in 3 places —
each is a candidate below.

**SERIES COMPLETE (2026-07-14):** all four increments (#1 analytic split-sum
env-BRDF, #2 SH irradiance ambient, #3 (1−F) diffuse energy, #4 multi-scatter
compensation) have LANDED on fog-wt, each default-OFF and each measured
effectively free (within the frame noise floor). All four await user
look-approval before any default flip. The env-BRDF LUT and diffuse-cubemap
items below stay PARKED.

- **#1 analytic split-sum env-BRDF** — DONE (e0640fe, flag `env_brdf_analytic`,
  default OFF). Measured +0.72 ms on greets = within noise → effectively free.
  Retires the `f90=1-rough` hack. Awaiting user look-approval to default ON.
- **#2 SH irradiance ambient** — DONE (d29302a, flag `sh_ambient`, default
  OFF). Replaces the flat `Sc->Ambient` constant with 9-coeff L2 RGB SH
  irradiance evaluated per-pixel along the (post normal-map) shading normal
  (~9 FMA/channel, no gather; clamped >=0). `SHAmbient_EnsureBaked`
  (EnvBake.cpp) projects a scene-center 32²×6 env cube — rendered through the
  same deferred cube-face path the env-reflection probes use — into 27 floats,
  A_l/π folded in so a uniform env evaluates back to its own colour
  (magnitude-comparable to the flat ambient). Injected at all three opaque
  kernel ambient sites (scalar wave-1 = greets; OuterVec vec-fill = city,
  covers its scalar fallback via `lane_ambB`; TileFill quarter/checker).
  Transparent kernel keeps flat ambient (out of scope). **Measured (arm64,
  1920×1080, whole-frame, 8 interleaved OFF/ON rounds, min-of-each):**
  greets scalar Δ = **+0.84 ms** (min 73.53→74.38; noise floor 6.3 ms) →
  within noise = **effectively free**; city OuterVec Δ = **+3.47 ms** (min
  103.26→106.73; noise floor 9.8 ms; city renders two deferred passes and the
  OuterVec SH is a per-lane *scalar* loop, hence the larger — still
  sub-noise-floor — delta). Flag-OFF byte-identical: city `37e62845…`,
  fountain `51fff7cd…`. A/B: matte pillars/walls gain directional 3D form
  (flat dead silhouette OFF → shaped fill ON); heavily-lit/emissive regions
  barely move (ambient is a small fraction there, HDR-compressed). Awaiting
  user look-approval to default ON. NB the one-shot bake renders the scene →
  nondeterministic on greets, so md5 pins must gate with the flag OFF.
- **#3 (1−F) diffuse energy conservation** — DONE (ccc0229, flag
  `diffuse_energy`, default OFF). Scales the deferred DIFFUSE accumulator by
  `(1-fres)` at the combine, where `fres` is the SAME per-pixel Schlick Fresnel
  the env-specular reflection already computes. Light reflected specularly (F)
  can't also diffuse; the engine scaled diffuse by `(1-metalness)` but skipped
  `(1-F)`, double-counting at grazing (full diffuse AND a strong Fresnel
  reflection). `EnvSpecComposeScalar` + `EnvComposeCityVec8` now expose `fres`
  via an optional out-param; wired at ALL opaque env-compose sites — scalar
  wave-1 (greets/fountain), TileFill, and OuterVec (both scalar-fallback lanes
  multiply the float diffuse; the vec fast-path uses an additive INTEGER
  correction `int(vf*(1-fres)) - int(vf)` so the flag-OFF path is byte-for-byte
  untouched). Transparent kernel carries no env term → out of scope (keeps full
  diffuse), same as #2. Only pixels with a reflection (Reflection>0 / metal)
  pay. **Measured (arm64, 1920×1080, city OuterVec, --snapshot=city@t=1961,
  iters=60, 8 interleaved OFF/ON rounds, min-of-each):** OFF min 100.344 ms →
  ON min 100.333 ms → Δ = **−0.011 ms** (ON marginally faster = noise; OFF
  noise floor min→max = 8.4 ms) → **within noise = effectively free** (it's ~4
  ALU ops per reflective pixel). Flag-OFF byte-identical: city
  `37e62845…`, fountain `51fff7cd…`. A/B (city glass, deterministic): reflective
  facades darken at grazing angles (blue windowed building + red facade go
  noticeably darker ON; 19.3% of pixels change, 100% darkened, mean luma −30.6,
  no pixel brightens), while the matte concrete pillar + emissive window-lights
  (no env term) stay byte-identical. Effect is PRONOUNCED on city because its
  glass has a high authored F0 (`city_env_f0=60`); on true dielectrics
  (F0≈0.04) it's the subtle grazing-only darkening the canonical BRDF intends.
  NB fountain shows zero change at its pin (its glass spheres are TRANSPARENT =
  no env term). Awaiting user look-approval to default ON.
- **#4 multi-scatter compensation** (Fdez-Agüera) — DONE (2718046, flag
  `pbr_multiscatter`, default OFF). The split-sum env-BRDF (#1) is single-scatter
  only — it drops the energy returned by repeated microfacet bounces, so rough
  metals read too dark. Adds it back from the SAME A,B `env_brdf_analytic`
  already computes in `EnvSpecComposeScalar` (a few ALU ops on reflective pixels
  only, NO new gather): `Ess=A+B` (single-scatter energy), `Favg=F0+(1-F0)/21`
  (avg Fresnel), `Fms=Favg*Ess/(1-Favg*(1-Ess))`, then `ek *= 1+Fms*(1-Ess)/Ess`
  (scales the SPECULAR energy only; the single-scatter Fresnel handed to #3 is
  left untouched). **DEPENDS ON `--env_brdf_analytic`** — it needs the A,B terms,
  so it's a NO-OP with that flag off (lives inside the analytic branch; the
  ad-hoc Schlick else-branch computes no A,B). `--env_brdf_analytic` already
  routes city glass off the OuterVec fast path to the scalar compose, so only
  `EnvSpecComposeScalar` needed wiring (4 call sites, 3 flag decls; no
  `EnvComposeCityVec8` touch). **Measured (arm64, 1920×1080, city@t=1961,
  iters=60, 10 interleaved OFF/ON rounds, `--env_brdf_analytic` as the baseline
  in BOTH to isolate #4 from #1, min-of-each):** OFF min 102.569 ms → ON min
  102.271 ms → Δ = **−0.298 ms** (ON marginally faster = noise/thermal, OFF ran
  first each round; noise floor max−min = 14.1 ms) → **within noise = effectively
  free**. Flag-OFF byte-identical: city `37e62845…`, fountain `51fff7cd…`
  (verified default AND `--pbr_multiscatter`-alone with `env_brdf_analytic` off).
  A/B (city glass, deterministic, `--env_brdf_analytic` baseline): reflective
  facades brighten — 19.2% of pixels change, **100% brighten / 0% darken**, mean
  +2.08 luma at the authored `city_env_gloss=24` (rough≈0.28, F0=0.6 → +10%
  specular), rising to +8.66 luma at a rougher `city_env_gloss=6` (the effect
  scales with roughness exactly as the compensation intends — characterized:
  F0=0.04 dielectric barely moves 1.00–1.05×, a true rough metal rough=0.8/F0=1.0
  → 1.79×, rough=1.0 → 2.22×). Non-reflective surfaces (concrete pillar,
  emissive window-lights) unchanged. NB city glass is a moderately-rough high-F0
  DIELECTRIC (metalM=0), not a true rough metal, so its brightening is modest;
  the effect is authored-material-dependent and pronounced only on rough metals.
  Awaiting user look-approval to default ON.
- **env-BRDF LUT (texture) vs analytic** — PARKED. The analytic (#1) is free +
  needs no memory; a real (F0·A+B) LUT is 1 cached tap. Only revisit if the
  analytic precision ever proves insufficient.
- **Diffuse irradiance as a cubemap** — PARKED (avoid). That's a per-pixel
  gather every lit pixel; #2 (SH) gets the same result as pure ALU.

## Micro-optimizations (only if a profile flags them)
- **env-BRDF `exp2`** — TODO-if-needed. `std::exp2(-9.28*ndv)` at
  DeferredSurfaceKernel.cpp ~1239 is scalar libm, and its result is usually
  CLAMPED away inside `std::min(rx*rx, exp2(...))` so full precision is wasted.
  The file already has a fast LUT-based log2/exp2 (used for `pow` ~2065) —
  reuse it (near-free), and it would vectorize into the `__m256` block so the
  city-glass AVX2 fast path (disabled today when the flag is on) could stay on.
  Measured free at greets scale, so LOW priority — do only if a profile shows it.

- **B5 per-face screen-bbox tile-walk pre-reject — DONE (2026-08-02, S2,
  commit 9b6d70d, `--tile_bbox_cull` default ON).** Each face's projected
  screen bbox (int16, floor/ceil+1px, from the face's own A/B/C PX/PY) is
  stamped into its `FListEntry` at FList-build time (Transform.cpp push); the
  tile walk (`RenderInnerMekalele`/`RenderInner`) 4-compare-rejects a face whose
  bbox misses the tile rect BEFORE the Face deref — a rejected face costs only
  the sequential FListEntry read (skips the 3 scattered Vertex flag loads + the
  clipper entry). PURE reject (the clipper already clips to the tile →
  byte-identical); near-plane-straddling faces (any vert behind nearZ) keep a
  cover-all sentinel and are never rejected. **Measured (t=5780, 1080p, threaded,
  40-iter p50, cull OFF→ON):** edge-displaced greets (86.6k faces) frame
  99.3→86.85 (−12.5), RNDR 63.66→51.63 (−12.0); t=2145 108.8→100.0 (−8.8);
  flags-off greets 47.5→47.0 (−0.5); city 95.1→94.1 (−1.0). Byte-null: city
  37e62845 + fountain 51fff7cd exact, render_gate 3/3, displaced-greets
  stable-pixel 1px/2.07M (greets race), wasm links. TRAP found: `frame->PX` is
  NOT populated by `VertexFrame_DumpFromAoS` (only PY), and conetest's giant
  quad has unpopulated `*_idx` — hence the fill reads AoS `F->A->PX` directly.
- **B5 shadow-bake face cull — SUPERSEDED by S1 offscreen proxy** (commit
  376f826, `--greets_shadow_proxy`, opt-in). Instead of per-face rejecting
  displaced faces in the shadow raster, the whole displaced detail is
  main-camera-only (Face_MainOnly) and a FLAT ~226-face proxy casts/reflects in
  every offscreen pass. **Measured: BAKE 27.3→21.5 (−5.8), frame 92.8→88.1
  (−4.7) at t=5780.** The win is BOUNDED — the shadow cube-face cull already
  limited per-frame wall rastering, and mixed chunks (rooms+siling) still pay
  Phase-A transform of ~22k displaced verts (only 151 pure-displaced chunks /
  59k faces are fully Tri_NoShadowCast'd). Default OFF: the flat caster's
  shadows differ from the displaced walls' on the looked-at wall (~1% px,
  maxD 142 — NOT invisible, a look call) and a flat caster carries no relief so
  it does NOT fix the reported light-bleeding.

- **S3 mesh-level AABB-vs-frustum cull — MEASURED, NOT WORTH IT (2026-08-02).**
  XFRM (the per-vertex transform, where a pre-transform AABB reject would save
  work) is 0.70 ms flags-off greets / 2.31 ms city / 7.8 ms edge-displaced
  greets — all tiny vs the RNDR (50-80 ms) + BAKE (20-30 ms) elephants. The
  existing bsphere cull + the greets Piramid chunk split already reject most
  off-screen geometry (XFRM would be an order of magnitude higher if they
  weren't firing — they are). An AABB is a tighter bound but its ceiling is a
  fraction of XFRM (sub-ms). At the heavy displaced pose the walls are ON-screen
  (that's why they're displaced) so an AABB wouldn't cull them either.
  Foundation-F AABBs stay consumed only by the env overlay; skip.

- **S4(a) fan↔edge seam holes — SPEC (not landed; risk vs budget).** The
  cross-patch heal (`MeshOps.cpp:2446-2490`) only REPOSITIONS the finer side's
  verts onto the coarser (anchor) polyline; it never INSERTS a vert on the finer
  side at an anchor kink it lacks. Fan(i/2^L params)↔edge(groove params)
  junctions have no subset relation → the fan chord bypasses the edge's groove
  kink → hairline hole. FIX = union the two sides' param lists at TRIANGULATION
  time (both sides emit `edgeVert` at every union param → welded, heal becomes a
  no-op) — a two-phase change to the tessellator (register-all-params, then
  re-emit), too invasive to retrofit safely late. First step: a diagnostic
  counting sides where neither param list ⊆ the other (the true un-healable
  count) next to the `%d T-junction pins` log.

- **S4(b) stone light-bleeding — SPEC (AO-on-direct; S1 does NOT fix it).**
  Bleeding = DIRECT disco light on mortar that the single-shadow-id collapse
  (acne fix) left un-self-shadowed. A FLAT proxy has no relief so it cannot
  restore per-block mortar self-shadow (the coordinator's S1-fixes-bleeding
  premise is geometrically wrong — verified). The map's grooves ARE the missing
  occlusion. AO already exists (rooms/floor carry it in albedo-alpha,
  `Mat_AoInAlpha`) but is applied to AMBIENT ONLY
  (`DeferredSurfaceKernel.cpp:1856-1874`). FIX = behind a default-off greets flag,
  for `Mat_AoInAlpha` displaced mats, move the AO multiply from ambient-only to
  the FINAL (ambient+direct) color after the light loop → static, acne-free
  groove darkening under direct light. Hot-kernel change (scalar path covers
  greets); flag-gated for byte-null; strength is a user look call. Tuning-only
  fallback: raise `ao_map_strength` (deepens ambient grooves, won't fully stop
  the direct leak).

- **S5 chunk-level LOD (flat+POM near/edge-displaced far) — DESIGN NOTE.**
  Largely SUBSUMED by S1: the offscreen proxy already IS the flat LOD for every
  non-main view. A camera-distance main-pass LOD would bake both meshes per
  chunk and swap by distance via the existing chunk machinery — but the S1
  Face_MainOnly/Tri_OffscreenProxy split + the proxy mesh are exactly the dual
  representation an LOD needs; extending it to swap in the MAIN pass by distance
  is the remaining step. Low priority: RNDR is pixel-bound at these poses (the
  displaced faces add little coverage — B2's ~2-2.8 µs/face is fixed cost, which
  S2 already reclaimed), so a main-pass geometric LOD buys little beyond S2+S1.
  **CEILING MEASURED 2026-08-06 and it confirms "low priority" with numbers**
  (docs/ENVDYN_DISPLACEMENT_PLAN.md §A4). Face-count ladder at t=5780 under
  `--greets_displace`: 6 522 faces (uniform L1) → 87 256 (edge carve) moves RNDR
  46.04 → 53.46 = **92 ns/face threaded ≈ 0.60 µs/face core**, i.e. **3.3–4.7×
  cheaper than B2's 2–2.8 µs serial** — `--tile_bbox_cull` took most of it.
  Halving the face count is worth −6.13 ms with the two new companions OFF, but
  with them ON the 87 k edge carve and the 43 k dome path measure **55.86 vs
  55.64, 0.22 ms apart**: the clone half of that work is already gone. So a
  per-chunk screen-space-error LOD is worth **≈0.2–3 ms** against a bake-time-only
  ladder (the bake is 2–6 s; no per-frame re-tessellation), 25–40 MB resident,
  and DMM's per-edge min-level rule to keep boundaries watertight when neighbours
  choose levels independently (today's border pins only close cracks for a FIXED
  level assignment). NOT built, deliberately; reproducible in three bench runs.

## Geometry front-end (XFRM) — measured 2026-08-05, docs/SOA_VERTEX_REFACTOR.md

> **RE-OPENED 2026-08-06 (b) with a CONTROLLED experiment — the mechanism is now
> quantified, and two items below are REPRICED.** `-DFDS_VERTEX_PAD_BYTES=N`
> adds dead tail padding to `Vertex`: not one instruction in any loop changes,
> only `sizeof`. greets t=5780 `--greets_displace`, 253 280 verts, per-frame min
> over 24, `pad=0` run first and last (drift 0.005 ms):
> **sizeof 140 → VERT 1.118/1.123 ms; 204 → 1.203 (+7.6 %); 268 → 2.315
> (+107 %).** Per-vertex time is a steep, super-linear function of the struct
> stride, with a cliff past ~256 B (stride prefetcher gives up).
> Bandwidth: ~284 B of streamed traffic per vertex at 4.41 ns = **64 GB/s on ONE
> core**, and this doc's own 958 k-vert numbers land at 64.3 GB/s (VERT) and
> 62.3 GB/s (the zero-arithmetic SoA sweep). Three unrelated loops at 62–64 GB/s
> is a single-core streaming ceiling — the one explanation for every wash on
> record (Vec8f, reciprocal estimate, single-precision divide, dropping 2 of 3
> mat-vecs, and the new field reorder).
> **Repricing:**
> - **Phase 5 / the interleaved 64-byte output array is CONFIRMED and worth MORE
>   than the −26 % below** — traffic ~284 → ~132 B/vert, i.e. on the order of
>   **2× VERT** against the measured slope. Blocker unchanged: `Vertex` must
>   split into mesh storage vs the clipper's transient type (every `RasterFunc`
>   takes `Vertex**`).
> - **"Read the per-face `Flags`/`PX`/`PY` from the SoA arrays" is REFUTED.** A
>   field reorder that puts PX/PY/Flags/TPos_AOS.z in 24 contiguous bytes cuts
>   the predicted lines per random 3-vertex deref 2.88 → 1.56 (−46 %) and moved
>   FACE by **0.6 %** (0.583/0.604 → 0.588/0.606). The A/B/C derefs are already
>   cache-resident. The 73 % is the branch chain + the per-face `F->Txtr->Flags`
>   chase + the Face stream, not the vertex reads. No `A/B/C_idx` invariant
>   needed, and no win there to collect.
> - **DONE 2026-08-06 — `--xfrm_par`, DEFAULT ON.** The main-view
>   `Transform_Objects` now runs on the pool: the object list is cut into
>   contiguous mesh-index BLOCKS (2 per worker) that are work-stolen off a shared
>   cursor, each appending into an FList segment **reserved in mesh order** (the
>   prefix sum of per-mesh `FIndex`), then compacted in block order — so
>   execution order is free while output order is pinned, and the result is
>   bit-identical to serial whatever order the workers finish in. Measured,
>   1920x1080, per-frame min over 24, min-of-arm over 3 interleaved reps, load
>   14-23: **displaced t=5780 1.546 -> 0.449 ms (-1.10, 3.4x); shipping t=5743
>   0.423 -> 0.261 (-0.16, 1.6x)**. Machinery overhead measured with 1 block
>   (inline, no dispatch): 10-35 us. Gates: render_gate 3/3 in both arms, city
>   `37e62845` and fountain `51fff7cd` PIN EXACT, chase 5-pose + cinematic and
>   greets t=1588 / t=5780 off==on, and **24 sequential runs of the greets pin
>   recipe with the parallel path on = 1 hash, 0 flips**.
>   **It also corrected the premise it was proposed on.** "The chip's aggregate
>   bandwidth is several times 64 GB/s" is FALSE for this access pattern: the
>   displaced arm streams 53.7 MB in 0.411 ms = **~131 GB/s, ~2x the single-core
>   figure**, and finer blocks do not move it — so that arm is now bandwidth-
>   bound at the SOCKET. The shipping arm is bound by something else entirely:
>   one mirror clone is 55 % of its main-view verts AND 55 % of its faces, so it
>   sits at the per-mesh LPT bound (predicted 0.221, measured 0.246). Full
>   working in docs/SOA_VERTEX_REFACTOR.md 2026-08-06 (c).
>   **Consequence for what is left:** more threads cannot help the displaced arm
>   (bytes/vertex can — Phase 5 / the interleaved 64-byte output array is now the
>   ONLY lever on it), and the shipping arm's lever is shrinking that mirror
>   clone (docs/VISIBILITY_PLAN.md 8e), which would help the serial path too.
> - **LANDED, byte-null, neutral:** the transform's `UZ`/`VZ` stores were dead
>   (the clipper overwrites them at entry) — 10 sites deleted in `fdc7a07`.

> **RE-MEASURED 2026-08-06 — THIS SECTION IS CLOSED AS A PERF TARGET.** With
> tessellation retired and `9d`'s faceless-mesh cull landed, the WHOLE geometry
> front end is **≈1.2–2.6 ms of a ~79 ms greets frame (1.5–3.3 %)**: main-view
> `Transform_Objects` **0.449–0.468 ms** min (SETUP 0.004 / VERT 0.244–0.257 /
> SOA 0.001 / FACE 0.176–0.185) at 49 447 verts, SHADOW phase A 0.74–2.07 ms
> wall across both bake calls, OFFSCREEN ~1.18 core-ms. The 7.92 ms baseline
> quoted below is `--greets_displace`, which is retired geometry.
> **One thing did land from this pass:** the per-light shadow depth/polyId
> `std::fill` ran SERIALLY on the tick thread inside the window `--shadow_prof`
> calls `xform=` — 10.50 MB / 0.19–0.40 core-ms per DynMeshes bake, i.e. MOST of
> that bucket was memset, not transform. Now cleared on the owning phase-A
> worker (byte-null at every gate). Census: `-DFDS_SHADOW_CLEAR_CENSUS`
> → `[SHADOW-CLEAR]`.
> **Phase 5's ceiling is also half what the doc claims** (−26 %, not −45 %, once
> the 13 extra SoA write streams are counted) unless the outputs are laid out as
> ONE interleaved 64-byte-per-vertex array instead of 13 SoA arrays — see the
> 2026-08-06 section of docs/SOA_VERTEX_REFACTOR.md for the corrected design,
> the verified `sizeof(Vertex)`=140 → 68 target, and the full consumer inventory
> (the migration surface is 11 files, incl. `Reflected_Transform` and FOUNTAIN).
> Prize for the whole refactor: ~0.15–0.4 ms (0.2–0.5 % of frame). Do it for
> cleanliness if at all, not for perf.

Instrument: `--xfrm_prof=N` + `--xfrm_ablate=<mask>` (both default OFF, byte-null).
Baseline at greets t=5780 `--greets_displace`, per-frame min: main-view
`Transform_Objects` 7.92 ms = VERT 4.01 + SOA 2.40 + FACE 1.45.

- **DONE — the Phase-1 AoS→SoA dual-write sweep, 2.40 ms.** `--xfrm_soa_inline`
  (now default ON) moves the SoA store into the per-vertex loops. Measured
  7.93 → 5.96 ms (−1.97, −25 %) at t=5780; bit-exact (city/fountain pins exact,
  chase 7 poses + greets t=1588/t=5780 byte-identical, `--soa-verify` clean).
- **The 958 k verts are mostly MIRROR CLONES, not the wall.** The displaced
  Piramid is 261,768 verts; the main view transforms 958,204, because
  `GreetsMirror` clones the whole scene per mirror (534,356 verts each for
  'teleporter' and 'P_TEXT.JPG#6') as ordinary meshes gated only by
  `HTrack_Visible`. **This is now the single biggest front-end lever** — bigger
  than anything left inside `Transform_Objects` — and it lives in
  `FDS/RENDER/GreetsMirror.cpp`: per-clone frustum/visible-panel culling, or a
  decimated clone of the displaced stone (the reflection does not need
  block-level relief). **CEILINGS MEASURED 2026-08-05** — see
  `docs/VISIBILITY_PLAN.md` §8, instruments `--mirror_cull_census[_cell]` +
  `--xfrm_pass_prof` (all default OFF). Exactly ONE clone is active at the wall
  poses = 56 % of main-view transformed verts; mirror RTT passes DO NOT run
  (`--mirror_rtt` defaults 0) and hide clones anyway, so this is purely a
  main-view cost. A per-source-mesh split saturates at ~40 %; a SPATIAL split
  of the clone at ~8 world units (103 sub-meshes) culls **63 % at wall poses /
  89 % at a panel pose** — but only against the MIRROR WINDOW (0.04–3.9 % of
  screen), not the frustum (2–20 %). Estimated ~2 ms off a 5.9 ms main-view
  `Transform_Objects`. NOT built: a spatial split breaks `UpdateMirror`'s
  contiguous-`ClonedMeshRange` invariant and every `m.cloneMesh` consumer
  (RTT/shatter/editor/envbake) — see §8e. **Cheaper first move: 49 % of the
  clone is the displaced Piramid; clone the S1 flat `stone_shadow_proxy`
  instead (~226 faces vs 87,256).** Look call on reflection fidelity.
- **A visibility HIERARCHY (BVH/octree) is REFUTED with numbers (§8c).** ~9,150
  mesh-level tests/frame across ~40 passes, ~7,690 of them rejections, at a
  measured ~55–61 ns per rejected sweep = **0.45 core-ms/frame** total prize,
  ~90 % of it inside 12-way-threaded shadow passes. A hierarchy also cannot
  reduce transformed verts at mesh-sized leaves, and finer leaves measured a net
  LOSS (verts −3.3 %, XFRM +1.08 ms).
- **Phase 5 of the SoA refactor is a PERF item now, not just cleanliness.**
  `Vertex` is pack(1) 140 B and the per-frame loop touches fields spanning
  offsets 4..123 — every cache line. Ablating 2 of the 3 per-vertex mat-vecs
  (34 % of the struct) buys only 8.7 % of VERT: the loop is line-bound, not
  arithmetic-bound. Moving the per-frame-written outputs out of the AoS struct
  shrinks the stride the transform walks; ceiling ~45 % of VERT. (Corollary:
  Phase 2 / Vec8f stays parked — widening lanes cannot help a stride problem.)
- **Per-face visibility test = ~73 % of the FACE bucket** (t=6097: 0.322 of
  0.395 ms). `Face::VisibilityFlagsAll()` is `A->Flags & B->Flags & C->Flags` —
  three chases into 140-byte `Vertex` structs; the tile-bbox stamp chases the
  same three again. Reading `Flags`/`PX`/`PY` from the VertexFrame SoA arrays
  (4-byte stride) is the obvious fix, BUT it requires `F->A/B/C_idx` to be
  trustworthy on every mesh and the tile-bbox comment in Transform.cpp records
  meshes where they are not (the conetest quad) — a wrong bbox DROPS a face
  where a wrong SortZ was harmless. Needs a per-mesh "indices stamped" invariant
  first.
- **Per-chunk normal cones** (bulk-accept / bulk-reject a chunk's faces without
  the per-face dot) — ~~still unmeasured~~ **REFUTED BY MECHANISM 2026-08-06, do
  not build.** A normal cone can only remove the backface term
  `AP·F->N < F->NormProd`. Two facts kill it:
  1. That term is the CHEAP half of the per-face cull. The 73 % figure above is
     `--xfrm_ablate=8`, which runs `VisibilityFlagsAll()` **and** the dot; the
     expense is the three random derefs into 140-byte `Vertex` structs for
     `Flags`, while `F->N`/`F->NormProd` are Face-local and sequential. A cone
     cannot answer the frustum test, so it leaves the expensive part in place.
  2. It cannot help the SHADOW passes **at all** — `shadowNoBackface`
     (`Transform.cpp`, `g_inShadowPass && !shadow_backface_cull()`) sits before
     the dot in the `||` chain, so the dot never runs there. That is 60–80 % of
     the front-end ms.
  Its whole ceiling is the backface dot in MAIN + OFFSCREEN, inside a 0.18 ms
  main-view FACE bucket.

## Perf (measured bottlenecks — from docs/PERF_STATE.md + the 15fps analysis)
The greets frame is ~2.5–3× a "generic deferred" frame; the fat is shadowing,
not shading. Biggest levers, in order:
- **Per-pixel CUBE-SHADOW taps ~32 ms** — the #1 cost (1.44M taps @ ~22 ns).
  `shadow_polyid_no_pcf` (single tap vs 4-tap PCF) saves ~9 ms already; fewer
  shadow-casting omnis; a cheaper cube-face select / better cache layout of the
  4 buffer streams. This is where the real fps is.
- **Dynamic-omni shadow re-bake ~12.5 ms** — re-rasterizing moving geometry to
  depth maps every frame. Cache/skip static portions.
- **Mirror RTT (teleporter, 2nd-order recursive)** — a full second scene
  render. Density/recursion knobs (`mirror_rtt_density`) are the lever.
- **Vectorize the general env-specular compose** (2026-07-31, user-queued).
  `EnvSpecComposeScalar` runs scalar per pixel for everything except the
  city-glass shape (`EnvComposeCityVec8` engages only for: uniform cube
  store across the 8-lane group + noParallax + cv-pull + no rough/metal
  maps + no env diagnostics + none of sphere-parallax/SSR/analytic-BRDF).
  Greets env surfaces are always scalar (per-material probes break the
  uniformity gate; AABB parallax; rough/metal maps). The math chain is the
  same one CityVec8 already vectorizes — per-lane stores become
  mask-selected, parallax correction is a few more vector ops; the
  gathers (face fetch) don't vectorize away regardless (ENV_NOFETCH
  attribution). MEASURE FIRST: only worth it if the env compose is a
  material slice of a greets frame vs the cube-shadow/cone elephants
  above.

## Correctness/determinism (blocks the gate, not just perf)
- **Greets env-bake non-determinism** — TODO, now URGENT-ish. Greets renders
  run-to-run distinct (4/4 by 2026-07-14, up from the old "~1-in-12 flip") —
  amplified by the user's new metallic materials (more env probes; env-bakes
  vary). This BREAKS the greets md5 gate (gate on city/fountain/pbrtest until
  fixed). Root-cause the bake ordering/threading nondeterminism. Related:
  measurement-tool-traps memory.
- **Editor metallic-import OOM** — IN-PROGRESS (targeted per-surface re-bake +
  capped editor bake res). Same env-probe subsystem as the determinism issue.
- **`--greets_displace` at t=6097 is run-to-run NONDETERMINISTIC** (measured
  2026-08-05): 6 sequential runs → 6 distinct color-PPM hashes, with every new
  flag OFF, while t=5780 in the same runs was byte-stable 6/6. So the "greets is
  deterministic again" re-pin (f4e81e9) is scoped to the NON-displaced pin
  recipe; the displaced path has its own defect at that pose. t=6097 cannot be
  used as a byte gate until this is root-caused. Not investigated — found while
  gating the XFRM work.

## Architecture cleanup — retire the ::mirUV material split (QUEUED 2026-08-05)

**What.** Replace the per-MATERIAL UV-handedness clone with a per-FACE
handedness bit, and compute `B = handedness·(N×T)` from it everywhere.

**Why now.** `GreetsFixBitangentHandedness` (DEMO/GREETS.CPP:1254) splits every
face with a negative UV determinant onto a `<name>::mirUV` clone carrying
`TbnHandedness = -1`, because the DEFERRED kernel is per-pixel and the G-buffer
has no channel for handedness — a material clone was the only path available.
Handedness is a property of a TRIANGLE's UV winding, and the determinant is
already computed at the split site; it is simply thrown into a material clone
instead of being stored. The split has caused real damage all session:

- **The parallax march never applied it** — `Mekalele.h:1462` builds
  `B = N×T` unconditionally while `DeferredSurfaceKernel.cpp` applies the sign
  in 5 places and `LightmapBake.cpp:793` applies it. With 41.8% of greets charts
  mirrored (S1d-1 census), the march walked the height field in the OPPOSITE V
  direction on those faces — the user's "texture edge moving half the face"
  swim. Found 2026-08-05 only because the user observed that deeper faces at
  STEEPER grazing angles did NOT swim, which falsified the angle-based theory.
- **`floor` has ZERO faces under its own name post-init** (they all moved to
  `floor::mirUV`), which silently gave the floor no shell at all and cost an
  agent hours.
- **`::mirUV` clones ALIAS their base material's shell tables** — naive frees
  are a use-after-free (hit during the editor rebuild work).
- It inflates material counts through the seam census and amplitude audits.

**Where.** Store the sign in the `Face_*` flag bits (a free bit, not a new
field). Do NOT put it per-vertex: `Vertex` is pack(1) 140 B and the XFRM profile
measured the transform loop CACHE-LINE-BOUND, so widening it costs ms. Stamp at
the determinant computation in `GreetsFixBitangentHandedness`; consumers are
`Mekalele.h` (march), `DeferredSurfaceKernel.cpp` (×5), `LightmapBake.cpp`.
The deferred kernel needs the bit reaching per-pixel — that is the hard part and
the reason the split exists; check whether a G-buffer bit is available or
whether the material lookup can carry it without a clone.

**Sequencing.** AFTER the immediate march fix (thread `Material::TbnHandedness`
into the raster context) — that unblocks the swim and is small. This entry is
the follow-up that fixes the CLASS rather than the instance.

**Expected benefit.** Removes a whole defect class; no measured perf claim.

## Offscreen geometry — what is LEFT after the faceless-mesh skip (2026-08-06)

Context: `docs/VISIBILITY_PLAN.md` §9. The faceless retired-Piramid skip
(`799c808`) took SHADOW 540 706 → 175 594 verts/frame at t=5743 and is DONE.
Ranked by share of what remains, measured with `--xfrm_pass_mesh_prof`:

- **`Hull.lwo` (the robot) — 82 800 verts/frame, 47.2 % of the post-fix shadow
  front end.** TODO. 2 400 faces / 7 200 verts, transformed in ~11.5 of 29
  shadow calls per frame. Levers: a shadow-caster LOD, or tightening the
  per-light mesh cull so fewer of the 29 calls keep it. Both are look-neutral
  inside a shadow map (silhouette-only consumer), so this is a perf call, not a
  look call — unlike the wall proxy below. **Measure before building:** the
  whole SHADOW front end is ~10.7 core-ms/frame post-fix, so the ceiling here is
  ~5 core-ms, i.e. sub-ms of wall clock at the pool's speedup.
- **`Piramid.lwo:cNN` chunks — 80 635 verts/frame, 45.9 %.** PARKED. Already
  chunked; VISIBILITY_PLAN §7b measured finer chunking a net LOSS.
- **robot legs — 11 403 verts/frame, 6.5 %.** TODO-if-#1-pays (same lever).
- **~~wall casters (flat proxy for `rooms`/`floor`/`siling`)~~ — PARKED, MEASURED
  NOT WORTH IT.** `rooms` + `rooms::mirUV` are 2 506 of 540 706 shadow verts
  (0.46 %); with `floor*` and `siling` ~4 000 (0.74 %), = 2.3 % post-fix.
  `--greets_shadow_proxy` was sized against ~81 k **displaced** faces; with
  tessellation retired the shadow bake already rasterises the ~226-face flat
  surface the proxy would substitute, so that win is already banked. Do not
  re-propose without a new measurement.

**Separate LOOK call, not actioned:** mirror clones are hidden inside the
mirror RTT scope but **not** inside the env/SH probe bakes. Re-measured
2026-08-06 after the orphan-clone-vertex compaction (`964bf1d`,
VISIBILITY_PLAN §10): at t=5743 the four clones are **49 390 of 200 464
OFFSCREEN verts/frame (24.6 %)** in the shipping arm (was 68 540 of 219 614 =
31.2 % before the compaction) and **48 804 of 2 407 892 (2.0 %)** under
`--greets_displace`. Whether a probe should see a reflection at all is the
user's call; the machinery already exists — `g_envBakeSkipMirrorClones` /
`EnvBake_IsMirrorCloneObj` at `Transform.cpp:1481` structurally excludes
clones from probe bakes, but it is gated on **`--env_bake_fix`, which defaults
0**, so today's shipping probes DO see the reflections. Turning that on is the
switch; the saving is the 49 390 verts/frame above.

## Mirror clone — what is left after the orphan compaction (2026-08-06)

Context: `docs/VISIBILITY_PLAN.md` §10. `964bf1d` clones only the vertices a
surviving clone face references: displaced-arm MAIN 545 339 → 299 449
verts/frame (−45.1 %), shipping-arm mirror-panel poses 54 272 → 28 782
(−47.0 %, XFRM 0.304 → 0.189 ms), byte-identical at 21 gates in both arms.
Ranked leftovers:

- **~~`--mirror_clone_tight_bsphere` → default ON~~ — PARKED, MEASURED ZERO
  WIN.** Landed default-OFF in `964bf1d`. It is the *correct* sphere (a clone
  cannot draw a vertex it does not carry) and it measured byte-identical at all
  21 gates in both arms — but byte-identical **because it culls nothing**:
  main-view transformed verts are the same with it ON and OFF at all six
  (arm × pose) pairs measured. Even the correct tight sphere over a whole
  compacted clone is 0.0 % frustum-cullable at every pose. Keep the flag for
  whoever revisits a split; do not flip it.
- **Bound the OPAQUE clone raster by the mirror window.** TODO, and the
  remaining half of the user's original question. The TRANSPARENT path already
  does exactly this (`RENDER.CPP` ~936–990: a clone batch's bound is its
  mirror's stamped `gb.mirrorId` window, not the clone geometry's projection);
  the opaque path rasterises clone faces over their full projection and
  rejects them per pixel. RNDR was 7.19 of the clone's 11.40 ms in the
  pre-`1a91ed5` displaced arm. Needs no clone split. NOT measured post-`964bf1d`.
- **~~Spatial split of the clone (VISIBILITY_PLAN §8e)~~ — DE-SCOPED, MEASURED
  NOT WORTH IT.** Post-compaction the clone is ~27 k verts at the wall pose and
  **1 926** at the mirror-panel poses, against a 0.19–0.44 ms main-view pass, so
  §8b's "~2 ms" is void. Re-measured ceiling: at the panel poses a cell = 8
  split culls 98–100 % **of 1 926 verts** (~0.01 ms); at the wall pose the best
  granularity is per-source-mesh at 10 260 verts (~0.05 ms) and the spatial
  cells §8e specced are the *worse* of the two there (25.7 %). The margin cells
  used to hold was the orphan block, twice removed (`799c808`, `964bf1d`). Do
  not re-propose without a new measurement.

## Three architecture candidates, decided on measurement (2026-08-08)

Measured with `--deferred_prof` (`docs/PERF_STATE.md` §0). Read that section for the
tables; this is the disposition.

- **(C) make the filtered-albedo plane (`--texture_filter>0`) the default — KILLED.**
  The premise was that the G-buffer stores a texel ADDRESS, so the kernel pays a
  dependent random gather per pixel that a linear filtered plane would remove.
  Measured at 5 poses × 3 interleaved reps: the kernel is **not faster at any pose**
  (`lighting-w1` 27.32→27.29→27.53 at greets 5743; same flat result at greets 2000 /
  4200, city, fountain), while the raster pass pays a consistent **+1.2–1.4 ms** for
  the extra plane. Frame cost +0.9…+4.0 ms. Proven directly by `--prof_no_tex`:
  deleting the albedo gather outright changes the kernel by +0.9 % — there is no
  gather cost to recover, because the normal / metal / roughness / AO / horizon maps
  are still fetched at the same `Mipmap[mip][swizzledUV]` address. `--texture_filter`
  stays a quality flag. **Do not re-propose as a perf lever without new evidence
  against the `--prof_no_tex` row.**
- **(B) clustered / finer light assignment — NOT SUPPORTED as stated, but the light
  loop IS the elephant.** The omni loop is **20.9 ms = 45 % of the greets frame**
  (`--prof_no_lights`), the single largest slice. But finer binning attacks light
  COUNT, and count is not what sets the cost: the per-tile census (12×8 grid) gives
  greets **6.9 lights/tile → 20.9 ms** and city **26.1 lights/tile → 3.4 ms**. 3.8×
  more lights for 1/6 the cost. The difference is per-light WORK (greets' pixels are
  normal-mapped → scalar kernel path with cube-shadow taps; city's take the 8-wide vec
  path). Corroborating: `--cone_fine_tiles` on city — the same "finer tiles" idea on
  the pass that owns 44 % of that frame — measured **no gain** (30.87 vs 30.97 ms,
  3 reps each). Attack per-light cost, not lights-per-tile.
- **(A) material binning inside the tile — the honest ceiling is ~8.6 ms.** Binning a
  tile's pixels by matID would hoist the `matID → Material*` resolve and the
  has-normal-map / has-AO / has-roughness branches out of the inner loop. That work is
  the NON-light part of the shading wave, measured at **8.61 ms** (`--prof_no_lights`,
  greets 5743) out of a 46.4 ms frame — and binning would recover a fraction of it,
  not all of it. Not refuted, but it is an 18 %-of-frame target where (B)'s territory
  is 45 %, and the (C) result says the coherent-gather half of the argument is worth
  ~0.

**What the numbers actually point at, in order:** the per-light cube-shadow/PBR loop
on greets (20.6 ms, split 10.8 ms shadow sampling / 9.8 ms per-light shading math),
the volumetric cone pass on city (30.7 ms = 44 %, and not a tile-balance problem), and
`TBR_Render` on fountain (14.8 ms = 73 %). None of the three candidates on the table
addresses any of those three directly.

**The one measurement that would settle (B), and does not exist:** a per-PIXEL count
of lights that survive the range/cone test inside the kernel loop. The census counts
per TILE. If most of a tile's 6.9 lights are rejected per pixel, finer binning removes
the rejects and (B) is worth its 20.6 ms target; if most survive, finer binning
removes nothing and the lever is per-light cost (cheaper shadow tap, fewer shadowed
omnis). Cheap to add next to `FDS_TILE_LIGHT_PROF` in `DeferredSurfaceKernel.cpp`.

Also newly visible and previously unattributed (all `docs/PERF_STATE.md` §0):
- **city `cones-call` 30.7 ms = 44 % of the city frame** — the largest single phase
  in any scene measured, and nobody was looking at it. `--cone_fine_tiles` does not
  help (measured). Untouched question: what the cone pass actually costs per spot.
- **fountain `TBR_Render` 14.8 ms = 73 %** — the fountain is a TBR-bound frame, not a
  lighting-bound one (its whole deferred lighting is 1.7 ms).
- **the bloom chain (DoF + bright pass + anamorphic + bloom + lens ghosts) is
  ~1.8 ms/frame on greets** and sat OUTSIDE every existing timer until now.
- **`mirror-grid` 0.68 ms/frame** — a full-res scalar scan of the mirrorMask plane in
  the lighting setup, every frame, on every scene that has a mirror mask.
- **the mirror clone costs ~3.4 ms of G-buffer raster** on greets t=5743 (5.69 ms with
  `--greets_mirror`, 2.30 without).

## Static shadow lightmap: the atlas is sized by FACE COUNT, not area (2026-08-09)

`StaticShadowLightmap::data` is `numFaces * lmRes² * numOmnis` **bytes**, and
`allocate()` fills it with 255 — so every byte is touched, resident and counted.
greets sets `shadow_lightmap_res = 128` (`GREETS.CPP` `GreetsApplyInitDefaults`),
which is calibrated for its authored wall quads and scales with the wrong
quantity the moment anything is tessellated.

**MEASURED** (greets `t=5743`, `/usr/bin/time -l` peak footprint, 64 GB box):

| arm | baked faces | atlas store | peak footprint | bake |
|---|--:|--:|--:|--:|
| flat | 33 396 | 5.61 GB | 6.93 GB | 1.08 s |
| `--greets_displace`, before | 115 346 | 19.36 GB | 22.97 GB | 6.2–11.7 s |
| `--greets_displace`, after | 115 346 | **0.14 GB** | **2.35 GB** | **0.09 s** |

- **DONE for the displaced arm** — `--shadow_lightmap_texel_density` (default 0 =
  OFF = byte-null), defaulted to 14.2 texels/world-unit by `--greets_displace` as
  its third perf companion. Per-mesh res = `clamp(ceil(sqrt(meanFaceArea) *
  density), 8, shadow_lightmap_res)`; capped, so it can only reduce. Look cost in
  the displaced arm: **byte-identical at t=1588 / 2845 / 4871 / 6097, and 3 px at
  1 LSB at t=5743** — the 19.2 GB it removes was buying nothing.
- **DONE for the FLAT (shipping) arm too — 2026-08-09, at the user's
  instruction.** `setDefault(shadow_lightmap_texel_density, 14.2)` moved out of
  the `--greets_displace` branch and into the main `GreetsApplyInitDefaults`
  block, so both arms get it. **347 of the 370 baked meshes** fall under the 128
  cap (mean face edge 1.303 world → res 19); the 23 that keep the cap are the
  big authored quads. MEASURED on the flat arm at `t=5743`:

  | | legacy (`…density=0`) | default 14.2 |
  |---|--:|--:|
  | atlas store | 5.61 GB | **0.09 GB** |
  | peak footprint (`/usr/bin/time -l`) | 7.44 GB | **1.50 GB** |
  | static bake (min-of-9 interleaved, load 11–17) | 1104 ms | **54 ms** |
  | greets-entry join wait (load 31) | 3497 ms | **221 ms** |
  | frame ms `t=5743` / `t=5780` (min-of-6, HEAD `af1f8f8`, load 3.2–7.9) | 49.17 / 48.70 | 49.33 / 48.84 |

  Per-frame is **neutral** (+0.15 ms both poses, inside the spread). An earlier
  batch at load 11–30 read −1.76 ms at `t=5780`; that was noise — the quiet-box
  re-measure above supersedes it. The bake and the memory are the real wins.

  **LOOK: NULL, and measured as such** — byte-identical at all 16 poses of
  `docs/greets_review_poses.txt` and at the pin pose, so the greets pin
  `778fa6ac…` did **not** move (4/4); city `3cbe42b1…` and fountain `8db68ccb…`
  4/4 each; `render_gate` 3/3. Revert flag `--shadow_lightmap_texel_density=0`
  reproduces the pin 4/4.
- **NEW, and bigger than the above: the shipping greets arm BAKES the atlas AND
  NEVER READS IT.** `DeferredSurfaceKernel.cpp:1619`
  `lmKernelEnabled = !shadow_dynamic() || shadow_lm_dynamic()`; greets defaults
  `shadow_dynamic` ON and `shadow_lm_dynamic` is compile-default 0, so every
  pixel takes the cube tap. MEASURED: `--no-shadow_lightmap` renders
  **byte-identical** frames at t=5743 and t=6097, and forcing the atlas live
  with `--shadow_lm_dynamic` is byte-identical to the shipping frame as well.
  So the remaining 0.09 GB + 54 ms bake is *still* pure waste on the default
  path — the whole `LightmapBake_Static` call could be skipped when
  `shadow_dynamic && !shadow_lm_dynamic`. Not done here: it is an FDS/RENDER
  change, and someone may want `--shadow_lm_dynamic` to become the default
  instead (which is the opposite fix, and a look call).
- **CORRECTION, 2026-08-10 — "forcing the atlas live with `--shadow_lm_dynamic`"
  ABOVE DID NOT FORCE ANYTHING LIVE.** That sentence read a null result as
  "the lightmap gives the same answer as the cube tap". It does not: the flag
  is **inert on greets**, because there is a SECOND gate it does not touch.
  `resolvePixelLightmap` (`DeferredShadowSampling.h:52`) needs `gb.lightmapMF`,
  which only `EngineGBuffer_Resize` (`Mekalele.cpp:85`) allocates, under
  `shadow_lightmap()` — and greets sets that flag in `GreetsApplyRunDefaults`
  (`GREETS.CPP:1228`, at `createGreetsScene` `:4393`), i.e. **after** every
  resize call site (`Snapshot.cpp:153`, `SDL2.cpp:433`, `ReproHarness.cpp:130`).
  Planes never exist → `pl.lm` is null on every pixel → `shadow_lm_dynamic`
  cannot matter. Same defect class as `mirror_rtt`, fixed in `7953bab`.
  **Positive control (t=5743, one binary):** `--shadow_lm_dynamic` **0 px**;
  `--shadow_lm_dynamic --shadow_lightmap_texel_density=1` (atlas crippled)
  **0 px** — a crippled atlas changing nothing is the proof it is unread;
  `--shadow_lightmap --shadow_lm_dynamic` **868 274 px (41.87 %)**.
  So the bake-skip above is even more clearly right, and it is now known what
  the opposite fix would cost: with both gates open, **+1.7 ms/frame**
  (min-of-6 interleaved at t=5743/5780/5814, load 9.8–11.6) for a change that
  is **95 % one-LSB** and invisible. Also note `shadow_lightmap` is read by
  nothing after init — it is an allocation-time flag, not the per-pixel sample
  gate `GREETS.CPP:1112-1117` claims it is. Full write-up + evidence:
  `docs/SESSION_STATE.md` 2026-08-10 and `docs/img/fogwt/lmdyn_*`.
- **Bake-resolution question, settled by measurement (2026-08-10):** a richer
  bake does **not** improve the lightmap path. Cap+density swept together to
  256/28.4 (0.32 GB, 191 ms bake) and 512/56.8 (**1.28 GB, 670 ms**): the
  >12/255 population falls only 8–26 % and the max channel delta moves by 0–1
  (138→138, 70→69, 96→96). The residual is not spatial — **inferred**: 8-bit
  quantisation of the shadow factor plus double filtering (4-tap PCF at bake,
  quantised, then bilinear at sample) vs the runtime's PCF at the pixel's own
  world position. Picture: `docs/img/fogwt/lmdyn_bakeres_t5773.png`.
- **DONE 2026-08-10 — the bake is now SKIPPED when the atlas has no reader.**
  `Initialize_Greets` evaluates `FeatureFlags::shadow_lightmap()` at the bake
  spawn point (where only CLI/env can have set it, since `GreetsApplyRunDefaults`
  has not run — so it is exactly the value `EngineGBuffer_Resize` already saw at
  boot) and, when it is off, skips `LightmapBake_Static` **and** the atlas
  allocation entirely. Keyed on the plane-allocation gate alone, deliberately:
  it is the conservative half, so `--shadow_lightmap` still bakes and the
  `--shadow_lightmap --shadow_lm_dynamic` arm is bit-for-bit unchanged (verified,
  matched pair: `fe61ae5c673fb56907eb79d071a7bfb6` both arms).
  **MEASURED:** static bake 55.8–78.3 ms → 0.1 ms (the stamp below); greets-entry
  join wait → gone with the thread; `--mem_census` loses the two lightmap rows,
  **−89.21 MiB** of allocated-and-TOUCHED store (censused total 750.94 → 661.73
  MiB, 41 → 39 buffers); process peak `phys_footprint` −20 to −24 MB (smaller
  than the census because `allocate()`'s uniform 255 fill compresses — *inferred*).
  Frame ms neutral (min-of-6 interleaved, load 7.6–12.9: t=5743 51.89 → 51.47,
  t=5780 50.55 → 50.88, both inside a ~7 ms spread).
  Pins unmoved 3/3 each: greets `778fa6ac…`, fountain `8db68ccb…`, city
  `3cbe42b1…`, `render_gate` 3/3 on both arms.
  **THE SIDE EFFECT THAT HAD TO BE KEPT, and it is not hypothetical:**
  `LightmapBake_Static` also stamps `Face::MeshFaceIdx`, whose *second* consumer
  is `tbrXparOrderLess` (`FILLERS.CPP:1876`) — the camera-independent tie-break
  of the per-strip transparent sort, live every frame and nothing to do with
  lightmaps. Split out as `LightmapStampFaceIndices` and still called on the skip
  path. **Proof it is load-bearing:** a control binary with only that call removed
  moves the greets pin to `76ff35370495cbd67c221fb899a7833b` (stable 2/2) — 1 px,
  max channel Δ 24/255, the exact coplanar-tie signature. Every *other* side
  effect (`staticLMTable`, `staticLMMeshId`, `staticShadowLM`, `omniSceneIdx`,
  `coverageBits`, `planarBases`) is reachable only through `resolvePixelLightmap`
  → `resolveCubeAtten`'s `useLightmap && pl.lm` branch, plus two diagnostics
  (`LightmapViz_Available`, the `--mem_census` row) — all dead on the shipping arm.
- **Cheap unrelated win found while there:** `lm.allocate(faces, numCubeOmnis, res)`
  (`LightmapBake.cpp:373`) sizes K from **all 11** cube omnis, but the bake
  `continue`s on any omni lacking `Omni_StaticShadow` (`:487`) and the kernel's
  `cubeOmniStatic` gate can never read those slots. greets has 8 static + 3
  moving → **3/11 = 27 % of the atlas is allocated, touched at 255, never
  written and never read.** Only worth doing if the lightmap path is ever used.
- Stale comment left behind, worth a one-line fix by whoever next touches the
  file: `FDS/RENDER/LightmapBake.cpp:330-336` still says greets turns the
  density on "only under `--greets_displace`… so the byte-pinned FLAT path never
  takes this branch at all". Both halves are now false.
- Same for city/fountain if they ever raise `shadow_lightmap_res` off its
  default of 16.
- Related: the atlas is sampled per pixel by the deferred kernel
  (`DeferredShadowSampling.h` `resolvePixelLightmap` → `sampleBilinear`), so its
  size is a per-frame cache/TLB question as well as a startup one. On a 64 GB box
  with the array resident the per-frame difference at t=5743 measured **within
  noise** (min-of-6: 55.07 ms before vs 54.82 after); the cost that IS certain is
  the startup bake and the 20 GB of resident pages every other process has to
  live around.

## 2026-08-10 — HARDWARE COUNTERS: THE LIGHTING STAGE IS COMPUTE-BOUND, NOT MEMORY-BOUND

Instrument: `--hw_prof` (adds task-wide retired-instruction / core-cycle / IPC
columns to the `--deferred_prof` table). Method, boundaries and the IPC
calibration ladder for this box: **docs/HW_PROFILING.md**. Cache-miss counters
are unreachable without root on Apple silicon, so IPC is the stall proxy
throughout — sound for before/after on one kernel, not an absolute score across
different code.

Reference anchors measured on this M2 Max: compute-bound (no memory traffic)
IPC 2.42; L1-latency chase 0.96; L2 0.14; DRAM 0.01.

### 1. The packed shadow plane (af1f8f8) — **CONFIRMED, and the mechanism is now measured**

`af1f8f8` claimed −1.0/−1.2 ms/frame from collapsing 4 u16 shadow planes into 2
u32 (8 cache lines per PolyId tap → 4). It shipped on a wall-clock argument. The
counters were re-run against it: matched pair, both arms built from ONE tree with
the same instrumentation applied, run from ONE `Runtime/` (af1f8f8 touches no
asset), greets t=5743, 1920×1080, dummy drivers, iters=20, **8 interleaved ABBA
rounds at load 13.7–18.8**, wall = min over rounds, counters = median.

| phase | wall A→B | Ginstr/f | Gcyc/f | IPC |
|---|--:|--:|--:|--:|
| **lighting-w1** | 27.72 → **26.53 ms (−4.3 %)** | 3.4965 → 3.4360 (**−1.7 %**) | 0.9875 → 0.9125 (**−7.6 %**) | 3.543 → 3.768 (**+6.3 %**) |
| DeferredLighting-call | 32.71 → 31.52 (−3.6 %) | −1.5 % | −7.6 % | +6.5 % |
| renderFrame | 45.35 → 44.19 (−2.6 %) | −1.1 % | −6.5 % | +5.8 % |
| shadow-bake | 2.41 → 2.35 (−2.7 %) | −0.5 % | −1.5 % | +0.7 % |
| gbuffer *(control)* | 6.70 → 6.77 (+1.1 %) | **±0.0 %** | −0.5 % | +0.5 % |
| lighting-w2 / cones / bloom / tonemap / TBR *(controls)* | — | ≤ +0.8 % | ≤ +3.6 % | \|Δ\| ≤ 0.7 % |

The −1.19 ms lands exactly on the claim. **The counters add what wall clock could
not: cycles fell 4.5× harder than instructions did** (−7.6 % vs −1.7 %). The work
did not change; the machine stopped waiting for it. That is a memory-side win by
construction, and it is confined to the two stages that tap the shadow map —
every control phase moved under 1 % in IPC, and `gbuffer` retired a byte-identical
3 significant figures of instructions in both arms. IPC sample sets barely
overlap (A 3.164–3.691, B 3.354–3.806).

### 2. The `Material` hot-record — **PARKED, refuted with numbers (bound ≲0.1 ms)**

The parked item above proposed a 64-byte L1-resident hot record per matID to
collapse the deferred kernel's 5-lines-per-pixel `Material` walk, and flagged
itself "latency not bandwidth, likely less than byte count suggests". Measured at
greets t=5743, lighting-w1 (post-pack arm):

* **1 657 retired instructions per pixel**, **440 core-cycles per pixel**
  (3.436 G instr and 0.9125 G cycles over 1920×1080), **IPC 3.77**.
* An IPC of 3.77 is incompatible with domination by serialised loads — the L2
  chase anchor on this box is IPC 0.14. The ROB is not starved.
* Arithmetic bound: the `Material` table is ≤ 114 KB, so a miss is an L2 hit at
  **30.6 ns ≈ 87 cycles** (measured). Six serialised L2 round-trips per pixel
  would be **~525 cycles/px — more than the entire measured 440-cycle pixel
  budget**. The walk therefore cannot be missing; those lines are being hit and
  overlapped.
* Upside ceiling: the record removes ~5 loads of the 1 657 instructions/px =
  **0.30 % of instructions**, so at unchanged IPC **≲0.1 ms/frame** of the
  26.5 ms wave.

**Verdict: not worth the invasiveness.** Caveat, stated plainly: this is a bound
from aggregate counters plus a latency anchor, *not* an isolation experiment — no
build was made with the walk removed. It rules out the large win the byte count
suggested; it does not measure the small one.

### 3. Where the greets lighting stage actually goes — **compute-bound; the lever is instructions/pixel**

lighting-w1 IPC **3.77**, against a compute anchor of 2.42 and an L1-latency
anchor of 0.96. The stage is issue/throughput-limited, not stalled. `gbuf-clear`
in the same table reads IPC **0.555** on both greets and city — the one phase
that genuinely is bandwidth-bound, which is a useful check that the instrument
discriminates rather than reporting ~3.5 everywhere.

`sample(1)` self-time over the same workload, DEMO symbols only (~50 090 samples
total; ~38 788 land in the lighting stage):

| symbol | samples | share of lighting stage |
|---|--:|--:|
| `Render_DeferredLighting_Tile` | 18 493 | 48 % |
| `CubeShadow_Sample` | 8 695 | **22 %** |
| `Render_DeferredLighting_TileFill` | 3 547 | 9 % |
| `resolveCubeAtten` | 2 898 | 7 % |
| `computeMapShadowAtten` | 2 518 | 6 % |
| `Shadow_MaterialSkipsCasting` | 1 479 | 4 % |

**Shadow sampling is ~40 % of the lighting stage** (15 590 samples across the
four shadow symbols) — the largest single sub-cost, and the same place af1f8f8
already took 7.6 % of cycles out of. That is where the remaining headroom is, and
it should be attacked as *instruction count* (fewer taps, cheaper tap, wider
SIMD), not as cache layout: at IPC 3.77 there are no stalls left to reclaim.

**TODO — new, actionable:** `Shadow_MaterialSkipsCasting` (Shadows.cpp:186) is
called **per pixel** from DeferredSurfaceKernel.cpp:1775 under
`--shadow_noncaster_depth`, and costs 4 % of the lighting stage. Its result
depends only on `Mat` — a material property, constant for every pixel of a given
matID — yet each call hashes the pointer and does a relaxed atomic load on a
shared 256-entry table. Hoist it to a per-tile lookup keyed by matID (the tile
kernel already hoists ~50 FeatureFlags reads out of the pixel loop for exactly
this reason). Expected ~0.5–1 ms/frame at greets t=5743. Not yet attempted.

### 4. city t=1961 — **the frame is volumetric cones, not lighting**

Same instrument, city t=1961, load 21.7 (ms inflated by load; the counter columns
and the *shape* are what to read). `renderFrame` runs **2×/frame** on city.

| phase | wall_min | Ginstr/f | IPC |
|---|--:|--:|--:|
| renderFrame (×2) | 70.47 | 8.344 | 3.72 |
| **cones-call** (×1) | **30.50 (43 % of frame)** | **4.150 (50 % of ALL instructions)** | 3.95 |
| gbuffer | 10.31 | 0.891 | 3.04 |
| DeferredLighting-call | 10.43 | 1.243 | 4.12 |
| fastfog | 9.25 | 1.093 | 3.62 |
| TBR-render | 7.11 | 0.847 | 3.54 |

**Anomaly worth flagging:** city's profile is nothing like greets'. Half of every
instruction the city frame retires is spent in the volumetric cone pass, which
runs once while the rest of the frame runs twice. It is compute-bound (IPC 3.95),
so the lever is again instruction count — cone count, march steps, or early-out —
not memory. Any city optimisation aimed at the lighting kernel is aimed at 15 %
of the frame.

### 5. Allocation audit — two leads, neither yet triaged

`scratchpad/hwprof_alloc.sh` (the CLT stand-in for Instruments' Allocations
template) on greets t=5743, live snapshot 14 s in, load 5.5. Physical footprint
**1.4 GB**; peak RSS 1.51 GB; MALLOC_LARGE 956 MB + MALLOC_SMALL 515 MB.
Biggest attributed live stacks:

| bytes | calls | stack |
|--:|--:|---|
| 117.5 MB | 5 | `Initialize_Greets` → `MaterialImport_ApplyRevMaps` → `loadRoleMapCached` → `Load_Texture` → `LoadPNG` |
| 99.4 MB | 370 | `Initialize_Greets::$_7` thread → `LightmapBake_Static` |

**LEAD, not a claim:** `leaks` reports **9 383 leaks / 387.9 MB** unreferenced at
the snapshot. C++ interior pointers and pointer-tagging (e.g. the packed
`Material*|skip-bit` cache in `Shadow_MaterialSkipsCasting`) are classic
false-positive sources for `leaks`, so this needs triage before it means
anything — but 388 MB against a 1.4 GB footprint is too large to leave
unexamined. Cross-read it with `--mem_census`'s UNCENSUSED RESIDUAL line: if the
census accounts for everything and `leaks` still says 388 MB, it is false
positives; if the residual is the same order, it is real.

### Method note that generalises

During this work the box went from load 13 to load 33 mid-session. Across that,
greets `renderFrame` **wall_min inflated 45.3 → 96.4 ms (+113 %)** while
**Ginstr/f moved 5.251 → 5.257 (+0.1 %)**. Retired-instruction count is very
nearly load-invariant, and IPC nearly so. On a shared machine, prefer them; quote
ms only with the load that produced it.

## How this list is maintained
Add an entry the moment an optimization is deferred (with: what, why deferred,
where in code, expected cost/benefit). Mark DONE with the commit + measured
result. SESSION_STATE "Queued next" points here; the memory
`optimization-backlog` points here too.
